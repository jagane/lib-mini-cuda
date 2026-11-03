/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc.h — minimal CUDA-runtime-like library over raw NVIDIA driver
 * ioctls.  Does one thing: synchronous Host⇄Device byte copies through
 * a single CE channel, with no CUDA runtime and no libcuda involved.
 *
 * Invariants the library enforces for you:
 *   - Paper F1:  for any allocation with a CPU alias (i.e. anything returned
 *                by mc_malloc_host), the pointer is BOTH the CPU VA and the
 *                GPU VA.  Pass the same pointer to memcpy on the host side
 *                and as source/destination to mc_memcpy.
 *   - mc_memcpy is synchronous: returns only after the semaphore fires, or
 *     MC_ETIMEOUT after TIMEOUT_MS (2 s).
 *   - Single-threaded.  Calling from multiple threads is undefined.
 *
 * Pointers returned by mc_malloc_device are NOT CPU-dereferenceable.  Reading
 * or writing them from CPU code is undefined behaviour; treat them as opaque
 * device handles and move data in/out via mc_memcpy.
 */
#ifndef MC_H
#define MC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Export marker for the public ABI.  The library is compiled with
 * -fvisibility=hidden, so every symbol is library-private unless it carries
 * MC_API.  A version script (mc/libmc.map) enforces the same set at link
 * time; the two must agree.  Consumers compiling against this header get a
 * plain declaration.
 *
 * Everything declared under mc/internal/ is deliberately NOT exported: those
 * are cross-TU helpers whose names (rm_*, uvm_*, write_gp_entry, ...) are
 * generic enough to collide once multiple architecture backends are linked
 * into the same object. */
#if defined(MC_BUILDING_LIBMC) && defined(__GNUC__)
#  define MC_API __attribute__((visibility("default")))
#else
#  define MC_API
#endif

typedef struct mc_ctx mc_ctx_t;

typedef enum {
    MC_OK         = 0,
    MC_ETIMEOUT   = 1,  /* semaphore never fired within TIMEOUT_MS */
    MC_EALLOC     = 2,  /* allocation table full / OOM / vidmem refused */
    MC_EIOCTL     = 3,  /* driver ioctl failed */
    MC_EINVAL     = 4,  /* bad argument (size == 0, unknown pointer, ...) */
    MC_EINTERNAL  = 5,  /* library invariant violated (should not happen) */
    MC_EHANG      = 6,  /* compute kernel exited but the SM-authored CE
                         * submission on the victim channel did not commit
                         * (PBDMA wedged, malformed pushbuffer, etc.) */
    MC_EVERSION   = 7,  /* loaded NVIDIA driver does not exactly match the
                         * private RM/UVM ABI targeted by this build */
    MC_EARCH      = 8,  /* the GPU's architecture has no backend in this
                         * build, or the backend's classes are not present
                         * on this device.  Reported before any
                         * architecture-specific object is allocated. */
    MC_EUNSUPPORTED = 9,/* the operation needs a feature this GPU or this
                         * build does not provide.  Distinct from MC_EINVAL:
                         * the arguments are fine, the capability is absent.
                         * Query with mc_feature_supported() to find out in
                         * advance rather than by failing. */
} mc_status_t;

/* Which agent performs an mc_memcpy transfer.  Both arms produce
 * bit-identical results; they differ only in who authors the
 * submission to PBDMA.
 *
 *   MC_XFER_HOST — the host writes the channel's pushbuffer methods,
 *                  advances USERD GPPut, and rings the BAR1 doorbell.
 *                  This is the canonical CE-copy path; works for any
 *                  matching-VAS pointer pair.
 *   MC_XFER_SM   — a single SM thread of the sm_owner kernel writes
 *                  the entire submission (pushbuffer + GPFIFO entry +
 *                  USERD GPPut + BAR1 doorbell) from device code.
 *                  PBDMA on the victim DMA channel wakes and runs the
 *                  CE-authored copy.  Both pointers MUST live in a
 *                  non-UVM carrier VAS (either MC_VAS_SYSMEM_CARRIER
 *                  or MC_VAS_FB_CARRIER) — the sm_owner kernel only
 *                  addresses carrier VAS GPU VAs; else MC_EINVAL.
 *                  The FB-carrier variant keeps PB / GPFIFO / USERD in
 *                  HBM, so the SM's submission writes never cross PCIe;
 *                  the only MMIO is the BAR1 doorbell at the end.  The
 *                  release semaphore stays in sysmem either way — it is
 *                  the cell the host polls. */
typedef enum {
    MC_XFER_HOST = 0,
    MC_XFER_SM   = 1,
} mc_xfer_t;

/* Which VA space an allocation lives in.  Selected at mc_malloc_* time
 * and recoverable via the allocation table for any pointer the library
 * returned.
 *
 *   MC_VAS_UVM             — UVM-managed VAS, IS_EXTERNALLY_OWNED, the
 *                            only VAS reachable from the UVM CE channel
 *                            (mc_memcpy/_d2h's UVM path).  For host
 *                            allocs the CPU and GPU VAs are equal
 *                            (Paper F1 anchored into the VA pool).
 *   MC_VAS_SYSMEM_CARRIER  — Non-UVM FERMI_VASPACE_A wrapping a
 *                            per-resource NV50_MEMORY_VIRTUAL carriers,
 *                            channel resources (PB/GPFIFO/USERD/sema)
 *                            in sysmem.  Reachable from the carrier's
 *                            DMA + compute channels.  For host allocs
 *                            the CPU VA is kernel-chosen and NOT equal
 *                            to the GPU VA — use mc_gpu_va() to recover
 *                            the GPU VA when an external primitive
 *                            needs the raw GPU address (mc_memcpy
 *                            itself does this translation internally
 *                            via the alloc table).
 *   MC_VAS_FB_CARRIER      — Sibling carrier whose own channel
 *                            resources (PB/GPFIFO/USERD/sema) live in
 *                            FB vidmem with BAR1-aliased CPU mappings
 *                            for host bring-up + diagnostics.  Holds
 *                            its own BAR1 doorbell PTE and three
 *                            channels (HOST_DMA, SM_VICTIM_DMA,
 *                            COMPUTE).  Operational on H100 PCIe:
 *                            mc_memcpy(MC_XFER_SM) on this VAS
 *                            keeps every byte of the submission
 *                            protocol as FB↔GPU-L2 traffic with only
 *                            the doorbell ring as a PCIe MWr.  The
 *                            release semaphore stays sysmem-resident
 *                            even here — the victim DMA channel owns
 *                            a dedicated sysmem sema cell that the
 *                            host polls directly, because spinning on
 *                            an FB cell would make every poll a PCIe
 *                            read competing with the transfer it is
 *                            waiting on.  Same CPU/GPU-VA contract as
 *                            MC_VAS_SYSMEM_CARRIER for user buffers.
 *
 * This is the canonical VAS identifier; the internal headers use it directly
 * (no internal alias type). */
typedef enum {
    MC_VAS_UVM             = 0,
    MC_VAS_SYSMEM_CARRIER  = 1,
    MC_VAS_FB_CARRIER      = 2,
} mc_vas_t;

/* Opens /dev/nvidiactl and first verifies that the loaded driver exactly
 * matches the private RM/UVM ABI targeted by this build.  MC_EVERSION is
 * returned before any RM object is allocated if it does not.  On a match,
 * opens the selected /dev/nvidia<N> (nvidia0 for plain mc_init; see
 * mc_init_params_t.device) and /dev/nvidia-uvm, reserves the VA pool, and brings up
 * the UVM, DMA, and compute channels.  Returns MC_OK on success and stores an
 * opaque context pointer in *out_ctx.  On failure returns a status code
 * (details go to stderr) and *out_ctx is left NULL. */

/* ── Optional capabilities ────────────────────────────────────────────────
 *
 * Not every GPU can do everything libmc implements, and not every build
 * includes everything.  Ampere and Ada, for instance, have no BAR1 usermode
 * aperture at all, so there is nowhere to install a doorbell PTE -- which
 * means no FB carrier and nothing SM-authored, on any driver, forever.  That
 * is a property of the silicon, not a bug to work around.
 *
 * So features are queried rather than assumed.  A caller that needs one asks
 * first; a caller that does not care ignores this entirely and gets the
 * stable copy path, which every supported GPU provides.
 */
typedef enum {
    MC_FEAT_XFER_HOST = 0,      /* host-authored CE copy; always available   */
    MC_FEAT_HOST_REGISTER,      /* mc_host_register on caller-owned memory   */
    MC_FEAT_CARRIER_SYSMEM,     /* the sysmem carrier VA space               */
    MC_FEAT_CARRIER_FB,         /* the FB carrier VA space (needs BAR1)      */
    MC_FEAT_COMPUTE_LAUNCH,     /* launching a compute kernel                */
    MC_FEAT_XFER_SM,            /* mc_memcpy(..., MC_XFER_SM)                */
    MC_FEAT_CE_DOORBELL_RING,   /* GPU-issued doorbell writes (needs BAR1)   */
    MC_FEAT_COUNT
} mc_feature_t;

#define MC_FEAT_BIT(f) (1ULL << (unsigned)(f))

/* Request every feature the GPU and build can provide. */
#define MC_FEATURES_ALL  (~0ULL)

/* The features a caller wanting only synchronous host-authored copies needs.
 * Asking for just these skips bringing up the carrier VA spaces and their
 * channels entirely, which is most of what mc_init does. */
#define MC_FEATURES_BASIC                                                     \
    (MC_FEAT_BIT(MC_FEAT_XFER_HOST) | MC_FEAT_BIT(MC_FEAT_HOST_REGISTER))

typedef struct {
    /* Bitmask of MC_FEAT_BIT(...) values to bring up.  Features that are
     * requested but unavailable on this GPU are skipped, not an error --
     * query with mc_feature_supported() afterwards.  Zero means
     * MC_FEATURES_ALL, so a zeroed struct behaves like mc_init. */
    uint64_t features;

    /* Which GPU to drive, as the "/dev/nvidia<device>" minor index.  0 is
     * the first card, so a zeroed struct behaves like mc_init and targets
     * /dev/nvidia0.  On a host with several cards, pass the index of the one
     * you want; mc_init_ex binds the whole context (RM device/subdevice, UVM
     * registration, and every allocation) to that single card. */
    int device;
} mc_init_params_t;

/* Whether `feature` is available on this context.  False either because the
 * GPU cannot do it, the build excluded it, or it was not requested at init. */
MC_API bool mc_feature_supported(const mc_ctx_t *ctx, mc_feature_t feature);

/* Human-readable feature name, for diagnostics.  Never NULL. */
MC_API const char *mc_feature_name(mc_feature_t feature);

MC_API mc_status_t mc_init(mc_ctx_t **out_ctx);

/* mc_init with an explicit feature request.  mc_init(&ctx) is exactly
 * mc_init_ex(&ctx, NULL), which requests everything available.
 *
 * Bringing up only what is asked for matters: the full set costs two extra
 * VA spaces, five extra channels and four kernel-module stagings, all of
 * which a caller doing plain host-authored copies never touches. */
MC_API mc_status_t mc_init_ex(mc_ctx_t **out_ctx,
                              const mc_init_params_t *params);

/* Strict-LIFO teardown: drain channel, unregister from UVM, munmap, rm_free
 * all handles, close fds, free mc_ctx_t.  Safe against NULL. */
MC_API void mc_fini(mc_ctx_t *ctx);

/* Allocate HBM (vidmem) with NO CPU alias, mapped into `vas`.  Returned
 * pointer is a GPU VA cast to void*; do NOT dereference from the CPU.
 * Returns NULL on failure.
 *
 *   MC_VAS_UVM     — uvm_map_buffer with kernel-chosen GPU VA.
 *   MC_VAS_SYSMEM_CARRIER, MC_VAS_FB_CARRIER —
 *                    each per-resource NV50_MEMORY_VIRTUAL carrier
 *                    DMA-maps the source `hMemory` with
 *                    `dmaOffset = 0`; RM picks the GPU VA. */
MC_API void *mc_malloc_device(mc_ctx_t *ctx, size_t n, mc_vas_t vas);

/* Allocate host memory mapped into `vas`.  Returned pointer is the CPU
 * VA, usable for direct CPU dereference.  Returns NULL on failure.
 *
 *   MC_VAS_UVM     — Paper F1 anchored: returned CPU VA is also the GPU VA.
 *                    Pass it directly to mc_memcpy/_d2h.
 *   MC_VAS_SYSMEM_CARRIER, MC_VAS_FB_CARRIER —
 *                    kernel-chosen CPU VA (sysmem mapping; FB
 *                    carriers expose user host allocs through the
 *                    sysmem-style path too, with the FB-residency
 *                    only affecting the channel's own resources, not
 *                    user buffers).  GPU VA recoverable via
 *                    mc_gpu_va().  Pass the CPU VA to
 *                    mc_memcpy/_d2h (it dispatches by VAS via
 *                    alloc-table lookup); pass mc_gpu_va(p) when an
 *                    external primitive needs the raw GPU VA.
 *
 * Cacheability: host allocations are write-back cacheable, in every VA
 * space.  This is deliberate and it matters — the same buffer allocated
 * write-combined reads back at roughly 30 MB/s instead of ~11 GB/s,
 * because write-combined memory is uncached on the load path.  mc still
 * uses write-combining internally for its control plane (pushbuffers,
 * GPFIFO, USERD, semaphores), which the host writes and the GPU reads;
 * that is not what this function hands you.
 *
 * Coherency contract for carrier-VAS host allocs (both
 * MC_VAS_SYSMEM_CARRIER and MC_VAS_FB_CARRIER — user host allocs are
 * sysmem either way; only the FB carrier's *channel* resources live
 * in FB): CPU writes you
 * make before handing the buffer to mc_memcpy(..., MC_XFER_HOST)
 * will be ordered correctly by the library's own fence discipline.
 * The same holds for mc_memcpy(..., MC_XFER_SM): the caller's buffer is
 * only ever read by the Copy Engine, never by the SM kernel, and no
 * caller-side flushing is needed either way — for a cached buffer
 * because CE reads of sysmem are cache-coherent DMA on x86 (dirty
 * lines are snooped), and for an mc_malloc_host_wc buffer because the
 * library's own SFENCE in the submit path drains the write-combining
 * buffers before the doorbell.  The channel state the SM kernel
 * does read (pushbuffer, GPFIFO, USERD, semaphores, CB0) is library-
 * owned write-combined memory whose visibility the library's own
 * SFENCE discipline guarantees; callers never touch it. */
MC_API void *mc_malloc_host(mc_ctx_t *ctx, size_t n, mc_vas_t vas);

/* Write-combined variant of mc_malloc_host — the analogue of
 * cudaHostAllocWriteCombined, for buffers the CPU only WRITES (H2D
 * staging).  Same VA-space semantics, same mc_free.
 *
 * The read penalty is the part that never varies, measured on H100 PCIe:
 * a WC buffer reads back at ~32 MB/s versus ~11.8 GB/s cached — about
 * 370x — because WC is uncached on the load path.  The DMA-side gain is
 * real but confined to small transfers: WC memory is not snooped
 * during DMA, which removes slow iterations rather than raising the
 * ceiling.  Measured on H100 PCIe, a 4 MiB H2D runs at the same peak
 * either way (51.6 GB/s) but averages 16% higher write-combined; by
 * 64 MiB the two are indistinguishable (see the "Transfer performance"
 * section of libcuda-research/docs/findings.md).
 * NVIDIA documents up to 40% for the CUDA equivalent.  Use this only
 * when the host never reads the buffer back; a single accidental read
 * loop costs more than the DMA gain saves.  mc_memcpy treats both
 * variants identically. */
MC_API void *mc_malloc_host_wc(mc_ctx_t *ctx, size_t n, mc_vas_t vas);

/* Register an existing malloc/mmap-backed host range into mc's
 * allocation table, analogous to cudaHostRegister(ptr, n, 0).
 *
 * The implementation currently supports MC_VAS_UVM only, matching the
 * cudaHostRegister trace shape:
 *   - RM creates an NV01_MEMORY_SYSTEM_OS_DESCRIPTOR over the page-covered
 *     CPU VA range.  mc does not allocate or mmap the memory.
 *   - UVM creates/maps external ranges at the CPU VAs, so GPU VA == CPU VA.
 *
 * After MC_OK, pass `ptr` to mc_memcpy just like a pointer returned by
 * mc_malloc_host.  The caller still owns the storage and must free(3) it only
 * after mc_host_unregister has returned. */
MC_API mc_status_t mc_host_register(mc_ctx_t *ctx, void *ptr, size_t n, mc_vas_t vas);

/* Unregister a pointer previously passed to mc_host_register.  No-op on NULL;
 * returns MC_EINVAL for unknown pointers or pointers owned by mc_malloc_*.
 * Does not free(3) or munmap(2) the user's storage. */
MC_API mc_status_t mc_host_unregister(mc_ctx_t *ctx, void *ptr);

/* Free a pointer returned by either mc_malloc_*.  No-op on NULL. */
MC_API void mc_free(mc_ctx_t *ctx, void *p);

/* Look up the GPU VA backing a pointer returned by mc_malloc_*.
 *
 * For MC_VAS_UVM host allocs the GPU VA equals the CPU VA (Paper F1)
 * so this is a no-op for them.  For carrier-VAS host allocs (both
 * MC_VAS_SYSMEM_CARRIER and MC_VAS_FB_CARRIER) the GPU VA differs
 * from the CPU VA returned by mc_malloc_host, and this function is
 * the supported way to recover it.  For device allocs (which return
 * their GPU VA cast to void*), the result equals the
 * argument cast to uint64_t.
 *
 * Returns 0 if `user_ptr` was not returned by mc_malloc_* on this
 * context (unknown pointer). */
MC_API uint64_t mc_gpu_va(mc_ctx_t *ctx, const void *user_ptr);

/* Synchronous copy.  Both pointers must have been returned by
 * mc_malloc_* on this context, with matching VAS — otherwise returns
 * MC_EINVAL.  Dispatch is automatic: UVM-VAS pointers route through
 * the UVM channel; CARRIER-VAS pointers route through the DMA
 * channel.  CPU pointers (host allocs) are translated to their GPU
 * VA via the allocation table before the method stream is built.
 *
 * `agent` selects who authors the submission to PBDMA.  See mc_xfer_t
 * for the trade-offs.  MC_XFER_SM additionally requires both pointers
 * in a carrier VAS (MC_VAS_SYSMEM_CARRIER or MC_VAS_FB_CARRIER). */
MC_API mc_status_t mc_memcpy(mc_ctx_t *ctx, const void *dst, const void *src,
                      size_t n, mc_xfer_t agent);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MC_H */
