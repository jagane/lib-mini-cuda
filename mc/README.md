# libmc

A small CUDA-runtime-like library over the raw NVIDIA driver ioctl
surface — no `libcuda` or `cudart`. Normal builds use the committed
sm_owner kernel bytes and do not require `nvcc`; `nvcc` is needed only
to regenerate that embedded kernel.

The library wraps the raw ioctl-and-submit machinery behind a small
public API.  It supports:

- Synchronous **Host ⇄ Device byte copies** through a CE engine, using
  either a host-authored or SM-authored submission strategy.
- Allocation in either the **UVM-managed VA space** (Paper-F1
  anchored, single-pool ergonomics) or the **carrier VA space** (the
  non-UVM FERMI_VASPACE_A shared by the DMA + compute channels).
- A **GPU-resident submission** path: an SM thread authors the entire
  CE-channel submission (pushbuffer, GPFIFO entry, USERD GP_PUT, BAR1
  doorbell) from device code.  Needs a BAR1 usermode aperture, so it is
  Hopper-only today — see *Optional features* below.

## API (`mc.h`)

```c
/* Lifecycle. */
mc_status_t  mc_init(mc_ctx_t **out_ctx);
void         mc_fini(mc_ctx_t *ctx);

/* Allocator: both functions take a VA-space selector.
 *   MC_VAS_UVM            — UVM-managed VAS (Paper-F1 anchored for host allocs).
 *   MC_VAS_SYSMEM_CARRIER — carrier VAS with channel resources (PB/GPFIFO/
 *                           USERD/sema) in sysmem.  Reachable by the
 *                           SM-owner kernel.
 *   MC_VAS_FB_CARRIER     — sibling carrier with channel resources in FB
 *                           (HBM).  No PCIe round-trip on the SM's
 *                           critical path; the doorbell ring is the only
 *                           MMIO write per SM-authored submission.
 */
typedef enum {
    MC_VAS_UVM            = 0,
    MC_VAS_SYSMEM_CARRIER = 1,
    MC_VAS_FB_CARRIER     = 2,
} mc_vas_t;

void        *mc_malloc_device(mc_ctx_t *ctx, size_t n, mc_vas_t vas);
void        *mc_malloc_host  (mc_ctx_t *ctx, size_t n, mc_vas_t vas);
/* Write-combined variant — cudaHostAllocWriteCombined's analogue.  For
 * H2D staging buffers the host never reads back: unsnooped DMA can be
 * faster (measured gains varied by session; NVIDIA documents up to
 * 40%), but CPU reads run ~370x slower.  See mc.h for numbers. */
void        *mc_malloc_host_wc(mc_ctx_t *ctx, size_t n, mc_vas_t vas);
/* Register memory the caller already owns, rather than allocating it —
 * cudaHostRegister's analogue.  The pointer keeps its address; mc adds
 * the GPU mapping.  mc_host_unregister removes it again. */
mc_status_t  mc_host_register  (mc_ctx_t *ctx, void *ptr, size_t n, mc_vas_t vas);
mc_status_t  mc_host_unregister(mc_ctx_t *ctx, void *ptr);
void         mc_free         (mc_ctx_t *ctx, void *p);
uint64_t     mc_gpu_va       (mc_ctx_t *ctx, const void *user_ptr);

/* Copy: both functions take an agent selector.
 *   MC_XFER_HOST — host writes the channel's pushbuffer + rings the
 *                  doorbell; CE engine performs the copy.
 *   MC_XFER_SM   — a single SM thread of the sm_owner kernel writes
 *                  the entire submission (PB + GPFIFO + USERD GPPut +
 *                  BAR1 doorbell) from device code.  Both pointers
 *                  must live in a carrier VAS (MC_VAS_SYSMEM_CARRIER
 *                  or MC_VAS_FB_CARRIER).
 */
typedef enum { MC_XFER_HOST = 0, MC_XFER_SM = 1 } mc_xfer_t;

mc_status_t  mc_memcpy(mc_ctx_t *ctx, const void *dst_dev, const void *src_host,
                       size_t n, mc_xfer_t agent);
```

One entry point covers both directions.  `mc_memcpy` looks each pointer up
in the allocation table, so it already knows which side is device and which
is host, and builds the method stream accordingly.  The parameter names
predate that unification and read more narrowly than the function behaves.

Plus several research-oriented helper entry points: `mc_dbell_demo_ring`,
`mc_memcpy_gpu_doorbell_ce`, `mc_memcpy_gpu_doorbell_sm`,
`mc_compute_doorbell_kernel`, `mc_compute_get_scratch`.  See `mc.h` for
contracts; each is a chain-of-evidence demo for a particular path the
library can drive (host-doorbell, CE-doorbell, SM-doorbell, etc.).

## VA spaces

Three VAS arms reachable from the public allocators:

- **`MC_VAS_UVM`** — UVM-managed `FERMI_VASPACE_A` allocated with
  `IS_EXTERNALLY_OWNED`.  For host allocs, the returned CPU pointer
  is **also the GPU VA** (Paper Finding 1: anchored into a 4-GiB
  pre-reserved VA pool at `0x200000000`).  Used by `mc_memcpy`
  through the *primary UVM channel* — same as today's vanilla CUDA
  H2D/D2H path.

- **`MC_VAS_SYSMEM_CARRIER`** — non-UVM `FERMI_VASPACE_A` whose channel
  resources (PB / GPFIFO / USERD / sema) live in sysmem.  Each
  NVOS46 mapping into the VAS allocates its own
  `NV50_MEMORY_VIRTUAL` carrier sized to the source `hMemory`, with
  `dmaOffset = 0` (RM picks the GPU VA).  This mirrors libcuda's
  per-resource shape; the older single-bump-allocator design is
  gone.  Holds the BAR1 doorbell PTE and is shared by the **DMA
  channel** (a second carrier-bound CE) and the **compute
  channel**.  For host allocs, CPU and GPU VAs are **distinct**
  (kernel-chosen CPU mapping; RM-chosen GPU VA); recover the GPU VA
  via `mc_gpu_va(ctx, ptr)`.  `mc_memcpy` on a carrier pair routes
  through the DMA channel; with `MC_XFER_SM`, the sm_owner kernel
  runs on the compute channel and authors the DMA channel's
  submission from device code.

- **`MC_VAS_FB_CARRIER`** — sibling carrier whose channel resources
  (PB / GPFIFO / USERD) live in **FB (HBM)**, not sysmem.  Same
  per-resource carrier shape as `MC_VAS_SYSMEM_CARRIER`, but three
  channels (HOST_DMA + SM_VICTIM_DMA + COMPUTE) where the sysmem
  carrier has two and reuses HOST_DMA as its SM victim.  The
  point is the SM-authored hot path: with everything in FB, an SM
  thread's pushbuffer / GPFIFO entry / USERD GPPut writes never
  cross PCIe — only the BAR1 doorbell ring at the end is an MMIO
  write.  The release semaphore for the SM-authored copy is
  sysmem-resident (allocated alongside the FB victim DMA channel)
  and the host polls it after the compute kernel returns:  PCIe
  producer-side ordering of MWrs from one Requester ID into
  sysmem (D2H) and `FLUSH_ENABLE` on the LAUNCH_DMA (H2D) make
  the sema observation a valid completion gate without an
  on-GPU spin-wait.

`mc_init` brings up the channels the requested features need: up to six
(UVM, sysmem-DMA, sysmem-compute, FB-HOST_DMA, FB-SM_VICTIM_DMA, FB-compute),
but fewer where a feature is unavailable or unrequested.  On Ampere, Ada and
GA10x the three FB-carrier channels are never created at all, and
`mc_init_ex(MC_FEATURES_BASIC)` creates neither carrier.  Pointers from `mc_malloc_*(ctx, n, MC_VAS_UVM)`
are not addressable from carrier-bound channels, and vice versa;
`mc_memcpy` validates that both arguments share a VAS and returns
`MC_EINVAL` otherwise.

### Paper-F1 invariant

`mc_malloc_host(ctx, n, MC_VAS_UVM)` returns a pointer that is **both
the CPU VA and the GPU VA**.  This is a hard invariant baked into UVM's
external-range registration — violating it produces intermittent
GPU-MMU corruptions on H100 PCIe (25–34 % failure rate before this was
understood; see [the Paper-F1 evidence][paper-f1]).  The library anchors every
UVM-host allocation into the VA pool, so users don't think about it.

`mc_malloc_host(ctx, n, MC_VAS_SYSMEM_CARRIER)` and the
`MC_VAS_FB_CARRIER` variant are **not** F1-anchored: the returned
pointer is the CPU VA only.  The GPU VA is RM-chosen inside the
carrier and recoverable via `mc_gpu_va(ctx, ptr)`.

`mc_malloc_device` always returns a GPU-only VA cast to `void *`; not
CPU-dereferenceable in either VAS.

### Coherency note

Host allocs are cacheable (write-back) on the CPU side; the library
handles ordering in both transfer arms.  Under `MC_XFER_SM` the
caller's buffer is only ever read by the Copy Engine, and CE reads of
sysmem are cache-coherent DMA on x86 — dirty CPU cache lines are
snooped, so no caller-side flushing is needed.  The channel state the
SM kernel itself reads (pushbuffer, GPFIFO, USERD, semaphores, CB0) is
library-owned write-combined memory made visible by the library's own
SFENCE discipline; callers never touch it.  See the `mc_malloc_host`
doc-comment in `mc.h`.

## Minimal user program

```c
#include <mc/mc.h>
#include <string.h>

int main(void) {
    mc_ctx_t *ctx;
    mc_init(&ctx);

    size_t    n = 64 * 1024 * 1024;
    uint32_t *h = mc_malloc_host  (ctx, n, MC_VAS_UVM);
    uint32_t *d = mc_malloc_device(ctx, n, MC_VAS_UVM);

    memset(h, 0xAB, n);
    mc_memcpy(ctx, d, h, n, MC_XFER_HOST);
    memset(h, 0, n);
    mc_memcpy(ctx, h, d, n, MC_XFER_HOST);
    /* h now holds 0xAB bytes again. */

    mc_free(ctx, d);
    mc_free(ctx, h);
    mc_fini(ctx);
}
```

Build into `bin/` so the same relative runtime path as the repository's test
binaries applies:

```sh
cc -Imc/include myapp.c -Llib -lmc -Wl,-rpath,'$ORIGIN/../lib' -o bin/myapp
```
Initialize the pinned official driver-source submodule, then from the repository
root run `make libmc` or `make mc_demo` (see
`tests/mc/mc_demo.c` for the full-featured demo).  `NVIDIA_SRC` may point at an
existing checkout of the verified driver release, or of another release with
`MC_ALLOW_UNTESTED_DRIVER=1` (see the top-level README).

For the SM-authored path, allocate carrier-VAS buffers (sysmem or
FB) and pass `MC_XFER_SM`:

```c
mc_vas_t carrier = MC_VAS_SYSMEM_CARRIER;   /* or MC_VAS_FB_CARRIER */
void *d = mc_malloc_device(ctx, n, carrier);
void *h = mc_malloc_host  (ctx, n, carrier);
mc_memcpy(ctx, d, h, n, MC_XFER_HOST);   /* seed HBM */
mc_memcpy(ctx, h, d, n, MC_XFER_SM);     /* SM authors the D2H */
```

See `tests/mc/mc_carrier_demo.c` (use `--fb` to exercise the
FB-carrier path) and `tests/mc/mc_sm_owner_demo.c`.

## Scope / restrictions

- **Exact driver ABI.**  This release is verified against NVIDIA 610.43.02.
  A build targets exactly one release (the one in `NVIDIA_SRC`), and `mc_init`
  returns `MC_EVERSION` before allocating RM objects when the loaded driver
  differs; private RM/UVM interfaces are not assumed compatible across
  releases.
- **Single-threaded.**  Calling any API from more than one thread at a
  time is undefined.  One `mc_ctx_t` per process; `mc_init` brings up
  the UVM channel plus three channels per carrier (HOST_DMA / one CE,
  SM_VICTIM_DMA / a second CE on the FB carrier, COMPUTE).
- **Synchronous.**  `mc_memcpy` returns only after the relevant
  semaphore fires or `MC_ETIMEOUT` (2 s) elapses.  No streams, no
  events.
- **One context per process.** `mc_init` reserves a process-global VA pool;
  creating a second context in the same process is unsupported and does not
  create an independent pool.
- **Size bounds.** Per allocation and per transfer: 1 ≤ n ≤ 4 GiB − 1.
  CE's `LINE_LENGTH_IN` is a 32-bit method. Internal mappings round and align
  ranges as required. The demonstration CLI uses dword-aligned sizes because
  its pattern generator operates on `uint32_t`; that is not a library limit.
- **Environment.**  Set `MC_VERBOSE=1` for debug-level stderr output.
  Requires `sudo` for `/dev/nvidia0`, `/dev/nvidiactl`,
  `/dev/nvidia-uvm`.
- **`MC_XFER_SM` and research instrumentation.** Matching official 610.43.02
  kernel modules need no doorbell setting. If the instrumented modules from
  `libcuda-research` are loaded, set `nv_dbell_disable_intercept=1`; otherwise
  their doorbell-watchpoint shadow page absorbs the SM-issued write.

## Reference — source layout

The library is split by layer.  The translation-unit count moves with the
number of backends compiled in, so it is not quoted here; `MC_LIB_SRCS` in the
Makefile is the list:

```
mc/
  include/mc/mc.h   public API — the only header consumers include
  internal/         private headers, one per concern
  core/             architecture-neutral lifecycle and VA-space policy
  driver/           RM and UVM ioctl surfaces
  hal/              method streams and submission primitives
  arch/<name>/      per-architecture code (hopper, ampere = GA100,
                    ampere_ga10x, ada)
  experimental/     research paths
  kernels/          device-side sources
```

There are two include roots: `-Imc/include` exposes only `<mc/mc.h>`, and
`-Imc` is the library-internal root.  A translation unit therefore writes
`#include "internal/types.h"` or `#include "arch/hopper/qmd_v04.h"`, so the
layer it reaches into is visible at the include site.  Consumers are given
the public root alone and never see `internal/`.

**Public surface:**

- `include/mc/mc.h` — the stable API: lifecycle, allocation, copies, and the
  feature queries.  Thirteen functions.
- `include/mc/mc_experimental.h` — the research entrypoints, behind
  `MC_ENABLE_EXPERIMENTAL`.  Not covered by the stability the rest of the API
  implies; a `MC_ENABLE_EXPERIMENTAL=0` build does not contain them.  Five
  functions.  Both headers mark every entry point
  `MC_API`; everything else is hidden by `-fvisibility=hidden` and the
  `mc/libmc.map` version script.  `make check-abi` verifies the headers and
  the exported ABI agree, and `make check-link` that nothing is left
  unresolved.

### Selecting architectures

`MC_ARCHS` names the backends compiled in.  It defaults to every directory
under `mc/arch/`, discovered with a wildcard rather than listed, so adding a
backend really is adding a directory: a hand-maintained list would compile
nothing for a new `mc/arch/<name>/` and the omission would look like the
backend not working.

It drives three things at once — which `mc/arch/<name>/*.c` are compiled, which
`-DMC_HAVE_ARCH_<NAME>` the registry sees, and which cubins are built — so the
source list, the registry table and the embedded kernels cannot disagree.
Trimming it (`make MC_ARCHS=hopper`) gives a leaner library.  An empty
selection fails at compile time rather than producing a library that detects
nothing.

### Optional features

Not every GPU can do everything libmc implements.  Ampere and Ada have no
BAR1 usermode aperture, so there is nowhere to install a doorbell PTE, so the
FB carrier and every SM-authored path are permanently unavailable there — a
property of the silicon rather than a gap to close.

So features are queried, not assumed:

```c
if (mc_feature_supported(ctx, MC_FEAT_XFER_SM))
    mc_memcpy(ctx, dst, src, n, MC_XFER_SM);
```

`mc_init_ex()` takes a requested feature mask; anything not requested is
never brought up, which for `MC_FEATURES_BASIC` skips both carrier VA spaces
and five of the six channels.  Availability is *derived* from the
architecture descriptor rather than declared per backend, so adding a backend
cannot forget to update a feature list — there is no feature list.  An
operation whose feature is absent returns `MC_EUNSUPPORTED`.

**Private headers** (`internal/`) — each TU includes the ones it uses rather
than a single umbrella:

- `types.h` — the data model: `MC_*` constants, `mc_channel_t`,
  `mc_va_space_t`, `mc_alloc_t`, the per-kind extras union, `struct mc_ctx`.
- `log.h` — `DEBUG_LOG` / `WARN_LOG` / `ERROR_LOG` and `CHECK`.
- `rm.h`, `uvm.h` — the driver-wrapper prototypes.
- `submit.h`, `vaspace.h`, `channel.h`, `alloc.h`, `compute.h` — the
  prototypes for `hal/submit.c`, `core/vaspace.c`, `core/channel.c`,
  `core/alloc.c`, and the SM-authored helper.

**Library translation units:**

- `driver/rm.c` — RM ioctl wrappers (root/device/subdevice/usermode/tsg/
  channel/ce/compute object allocators; `rm_alloc_vidmem`,
  `rm_alloc_sysmem_at`, `rm_alloc_vaspace*`, `rm_alloc_virtual_memory`,
  `rm_map_memory*`, `rm_control`, `pick_non_grce_lce`,
  `rm_gpfifo_schedule`, `rm_channel_disable`).  Also hosts
  `mc_debug()`.
- `driver/uvm.c` — UVM ioctl wrappers (`uvm_setup`,
  `uvm_register_channel`, `uvm_map_buffer*`, `uvm_unmap_buffer`,
  `uvm_unregister_*`).
- `hal/methods.c` — every GPU method word libmc emits:
  `mc_write_transfer_methods` (CE copy), `write_gp_entry`,
  `mc_compute_write_setup` and `mc_compute_write_launch`.  Pure encoding —
  no fds, handles or fences.  Keeping this the single source is what makes
  per-architecture work tractable.  Golden-byte tested by
  `tests/unit/mc_methods_test.c`.
- `hal/arch.h` + `arch/<name>/backend.c` — the per-architecture descriptor:
  class ids, layout offsets, alignments and capability bits, as plain data.
  `struct mc_ctx` and `mc_channel_t` each carry a `const mc_arch_desc_t *`,
  and every class id and arch-varying constant is read through it rather
  than named at the call site.  Invariant-checked by
  `tests/unit/mc_arch_desc_test.c`.
- `arch/<name>/ops.c` — the behaviour hooks a descriptor cannot express as
  data: `alloc_usermode` (the parameter struct's *shape* differs — Hopper and
  Blackwell take one, Volta through Ampere are `RS_NONE`), the USERD
  accessors (Blackwell's channel class publishes no `GPGet`), and
  `engine_for_role` (which engine a role runs on is policy, not a constant).
  Four hooks, each justified by a specific divergence in the pinned headers.
  This is also the only place in the library that names a USERD struct.
- `hal/registry.c` + `hal/detect.c` — backend selection.  The registry is the
  explicit table of compiled-in backends plus pure lookups over it; detection
  asks `NV2080_CTRL_CMD_MC_GET_ARCH_INFO` what the chip is, then verifies
  against `NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2` that the device actually
  exposes the classes the chosen backend will allocate.  Unknown silicon is
  `MC_EARCH`, not a guess.  `MC_ARCH_FORCE=<name>` overrides selection for
  bring-up and warns loudly.  Tested by `tests/unit/mc_arch_detect_test.c`.
- `hal/method_abi.h` — a `_Static_assert` bank, built from the SDK `DRF_*`
  macros, proving the encodings above are shared by the Ampere, Ada and
  Blackwell classes rather than being Hopper-specific.  If a class ever
  moves a field, the build fails and names it.
- `hal/submit.c` — submission mechanics: `mc_channel_arm`/`submit`/
  `poll_sema`, `ring_doorbell` and `drain_channel`, reaching USERD only
  through the architecture's `ops->userd_*` hooks -- it names no USERD class
  struct itself, which is what lets one submit path serve every backend.  No fds or RM
  handles opened here.
- `core/vaspace.c` — VA pool (`va_pool_init`, `va_pool_reserve`) +
  carrier VA space helpers (`mc_va_space_init_uvm`,
  `mc_va_space_init_carrier`, `mc_va_space_init_carrier_fb`,
  `mc_va_space_dma_map_resource` (allocates a fresh
  `NV50_MEMORY_VIRTUAL` per source `hMemory`, NV04_MAP_MEMORY_DMA
  with `dmaOffset = 0`, RM picks the GPU VA),
  `mc_va_space_release_carrier` (NVOS47 + free, called by
  channel teardown before freeing the source `hMemory`),
  `mc_va_space_install_doorbell_pte`,
  `mc_va_space_alloc_scratch`, `mc_va_space_alloc_vidmem`,
  `mc_va_space_fini`).
- `core/context.c` — lifecycle: `mc_init` (version check, device open, VA
  pool, RM object tree, engine selection, VA-space and channel bring-up)
  and `mc_fini` (strict-LIFO teardown, best-effort so one failure does not
  strand the rest).
- `core/channel.c` — per-kind channel bring-up and teardown
  (`mc_{dma,compute,uvm}_channel_{init,fini}`), plus the shared core
  resource plumbing.  Declared in `internal/channel.h`.
- `core/alloc.c` — the allocation table and the allocator surface:
  `mc_malloc_*`, `mc_host_register`/`_unregister`, `mc_free`, `mc_gpu_va`.
  Declared in `internal/alloc.h`.
- `core/memcpy.c` — `mc_memcpy` and its VAS + agent dispatch.
- `core/feature.c` — which features this GPU and build provide, derived from
  the architecture descriptor with dependencies closed transitively.
- `arch/hopper/qmd_v04.{c,h}`, `arch/ampere/qmd_v03.{c,h}` — the QMD builders,
  each exposed through its architecture's compute ops as a single `build_qmd`
  operation.  The field setters stay private: they are how a QMD of that
  version is assembled, not what a caller asks for, so a new version
  implements one function rather than nine.  Ada shares Ampere's V03 — it
  brings its own compute *class* but not its own QMD layout.

  V03 is not V04 with fields moved.  It is 256 bytes against 384, sets
  `SM_GLOBAL_CACHING_ENABLE` where V04 does not, stores constant-buffer
  addresses unshifted where V04 stores them `>>6`, names its register-count
  field `REGISTER_COUNT_V`, and has no `QMD_TYPE` field at all.  Neither
  version's header ships in the pinned tree — it carries only Kepler's
  `cla0c0qmd.h` — so both follow Mesa NVK, and `tests/unit/mc_qmd_test.c` pins
  the result with golden dwords rather than trusting the transcription.

  Kernel-argument offsets inside CB0 are a separate axis from the QMD version:
  nvcc places them at `0x210` on sm_90 and `0x160` on sm_80.  They live in the
  compute ops as data, and `hal/cbuf.c` fills CB0 from them — the procedure is
  identical everywhere, only the offsets differ.
- `experimental/doorbell.c` — host-authored CE-doorbell demos
  (`mc_dbell_demo_ring`, `mc_memcpy_gpu_doorbell_ce`): the copy engine
  writes a token to the BAR1 doorbell aperture through a GPU MMU PTE.
- `experimental/compute.c` — HOPPER_COMPUTE_A demos
  (`mc_compute_doorbell_kernel`, `mc_memcpy_gpu_doorbell_sm`,
  `mc_compute_get_scratch`).  Builds the per-channel one-time setup
  pushbuffer (cache invalidates + shader-memory windows).
- `experimental/sm_owner.c` — SM-authored CE submission helper.  Library-internal
  (declared in `internal/compute.h`); reached through the public API via
  `mc_memcpy(..., MC_XFER_SM)`.  Wraps the embedded sm_owner kernel
  in a launch-and-poll path against the compute channel.

**Embedded kernels:**

- `kernels/store_dword.cu` — one thread, one system-scope store to whatever
  address it is given.  Named for what it does, not for one use of it: pointed
  at a BAR1 doorbell it demonstrates an SM store can ring one and wake PBDMA
  (Hopper-only, since that aperture is); pointed at ordinary memory it is the
  minimal proof that a kernel launches at all, which is what makes
  `MC_FEAT_COMPUTE_LAUNCH` meaningful and why Ampere and Ada carry this cubin
  despite having no doorbell to ring.  `volatile` is load-bearing — it is what
  makes the store lower to `STG.E.STRONG.SYS` rather than sitting in L1.
- `kernels/sm_owner.cu` — single-thread kernel that authors an
  entire CE-channel submission from one SM thread.  Compiled only for
  architectures that can ring a doorbell, so Hopper today.  Uses `st.relaxed.sys.global`
  + `membar.sys` for fence discipline; each method-stream dword and
  the GPFIFO entry / USERD GPPut / BAR1 doorbell are emitted as
  system-scope strong-ordered global stores.  Build-time assertion in
  the Makefile validates that the cubin contains ≥ 19 such stores.
- `kernels/sm_owner_args.h` — shared host/device argument-struct layout.
  The kernel takes a single `struct mc_sm_owner_args *` parameter
  (insulates the host glue from NVCC parameter-packing details).
- `arch/<name>/{sm_owner,doorbell}_sass.h` — auto-generated by the Makefile
  from the cubins, one set per architecture: SASS does not port across SM
  versions, so each architecture compiles its own.  The descriptor carries
  pointers to its own blobs, and a backend with none simply cannot offer the
  features that launch them — a normal state for a backend written before its
  cubins exist, not an error.  Committed so a fresh checkout builds without `nvcc`;
  regenerate both with `make kernels`.  The build asserts the SASS mnemonics
  that matter (store scope, ordering) rather than trusting nvcc's lowering.

**Reference / cross-check:**

- [`../docs/mc_architecture.md`](../docs/mc_architecture.md) — concise context,
  VA-space, channel, and submission architecture.
- [`../docs/compute_kernel_launch.md`](../docs/compute_kernel_launch.md) — the
  compute launch (all backends) and the Hopper-only SM-authored copy chain.

[paper-f1]: https://github.com/maxime-peim/libcuda-research/blob/main/docs/findings.md#paper-f1-va-identity
