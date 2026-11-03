/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * core/alloc.c — the allocation table and the public allocator surface.
 *
 * Owns mc_malloc_device / mc_malloc_host / mc_malloc_host_wc,
 * mc_host_register / mc_host_unregister, mc_free and mc_gpu_va, plus the
 * fixed-size table that lets mc_free recover an allocation's RM handle,
 * GPU VA, size and VAS from the opaque pointer the user holds.
 *
 * Paper-F1 invariant: every allocation with a CPU alias is anchored into
 * the VA pool so CPU VA == GPU VA.  Enforced by routing mc_malloc_host
 * through rm_alloc_sysmem_at + uvm_map_buffer_at, both with a
 * caller-supplied want_va inside the pool.  mc_malloc_device has no CPU
 * alias and therefore does not use the pool.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nvtypes.h"

#include "internal/types.h"
#include "internal/log.h"
#include "internal/rm.h"
#include "internal/uvm.h"
#include "internal/vaspace.h"
#include "internal/alloc.h"
#include "internal/compute.h"

/* ── Allocation table helpers ──────────────────────────────────────────────
 *
 * The allocation table is a fixed-size flat array (MC_ALLOC_TABLE_SLOTS
 * = 256) in mc_ctx_t that tracks every user buffer returned by
 * mc_malloc_{device,host}.  It exists so mc_free can take the opaque
 * pointer the user got back and recover the associated RM handle, GPU
 * VA, size, and is_device bit.
 *
 * A flat linear scan is fast enough at 256 entries, and sidesteps
 * allocator-inside-allocator concerns (a hash table would need its own
 * memory management that could interact badly with the library's VA
 * pool invariants).  A free slot is one with ptr == NULL; mc_free
 * clears the whole struct on release.
 */

static mc_alloc_t *alloc_table_find_free(mc_ctx_t *ctx)
{
  int i;
  for (i = 0; i < MC_ALLOC_TABLE_SLOTS; i++)
    if (ctx->allocs[i].ptr == NULL)
      return &ctx->allocs[i];
  return NULL;
}

mc_alloc_t *alloc_table_lookup(mc_ctx_t *ctx, const void *ptr)
{
  int i;
  if (ptr == NULL)
    return NULL;
  for (i = 0; i < MC_ALLOC_TABLE_SLOTS; i++)
    if (ctx->allocs[i].ptr == ptr)
      return &ctx->allocs[i];
  return NULL;
}

typedef struct {
  NvU64 base;
  NvU64 length;
  NvU64 offset;
} mc_uvm_external_segment_t;

static NvU64 mc_host_page_size(void)
{
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    return 4096;
  return (NvU64)page_size;
}

static int mc_build_host_register_segments(const void *ptr, size_t n,
                                           mc_uvm_external_segment_t segs[MC_UVM_EXTERNAL_SEGMENTS_MAX],
                                           int *out_count,
                                           NvU64 *out_map_base,
                                           NvU64 *out_map_size)
{
  uintptr_t user_va;
  uintptr_t user_end;
  uintptr_t map_base;
  uintptr_t map_end;
  uintptr_t page_size;
  uintptr_t page_mask;
  NvU64     map_size;
  int       count = 0;

  if (ptr == NULL || n == 0 || segs == NULL || out_count == NULL
      || out_map_base == NULL || out_map_size == NULL)
    return -1;

  page_size = (uintptr_t)mc_host_page_size();
  if (page_size == 0 || (page_size & (page_size - 1)) != 0)
    return -1;
  page_mask = page_size - 1;

  user_va = (uintptr_t)ptr;
  if ((uintptr_t)n > UINTPTR_MAX - user_va)
    return -1;
  user_end = user_va + (uintptr_t)n;
  if (user_end > UINTPTR_MAX - page_mask)
    return -1;

  map_base = user_va & ~page_mask;
  map_end  = (user_end + page_mask) & ~page_mask;
  if (map_end <= map_base)
    return -1;

  map_size = (NvU64)(map_end - map_base);

  if (((user_va & page_mask) == 0 && (((uintptr_t)n) & page_mask) == 0)
      || map_size <= page_size)
  {
    segs[count++] = (mc_uvm_external_segment_t) {
      .base   = (NvU64)map_base,
      .length = map_size,
      .offset = 0,
    };
  }
  else
  {
    segs[count++] = (mc_uvm_external_segment_t) {
      .base   = (NvU64)map_base,
      .length = (NvU64)page_size,
      .offset = 0,
    };

    if (map_size > 2 * (NvU64)page_size)
    {
      segs[count++] = (mc_uvm_external_segment_t) {
        .base   = (NvU64)(map_base + page_size),
        .length = map_size - 2 * (NvU64)page_size,
        .offset = (NvU64)page_size,
      };
    }

    segs[count++] = (mc_uvm_external_segment_t) {
      .base   = (NvU64)(map_end - page_size),
      .length = (NvU64)page_size,
      .offset = map_size - (NvU64)page_size,
    };
  }

  *out_count    = count;
  *out_map_base = (NvU64)map_base;
  *out_map_size = map_size;
  return 0;
}

void mc_free_registered_uvm_ranges(mc_ctx_t *ctx, const mc_alloc_t *slot)
{
  int i;

  if (ctx == NULL || slot == NULL || ctx->uvm_fd < 0)
    return;

  for (i = slot->uvm_segment_count; i > 0; i--)
    uvm_free_range(ctx->uvm_fd, slot->uvm_segment_base[i - 1],
                   "h_registered");
}

/* ── mc_malloc_device / mc_malloc_host / mc_free ─────────────────────────
 *
 * Both allocators record their result in the ctx-local allocation table
 * so mc_free can find them later.  The difference is whether the
 * allocation has a CPU alias:
 *   - mc_malloc_device: NV01_MEMORY_LOCAL_USER + plain uvm_map_buffer
 *     (kernel-chosen PROT_NONE VA).  Returned pointer IS the GPU VA;
 *     CPU dereference is UB.  Use for the source of H2D and destination
 *     of D2H when the CPU won't touch it.
 *   - mc_malloc_host: NV01_MEMORY_SYSTEM + VA-pool-anchored MAP_FIXED +
 *     uvm_map_buffer_at.  CPU VA == GPU VA (Paper F1), both valid, same
 *     pointer.  Use for anything the CPU needs to read/write.
 */

/*
 * Allocate device-only HBM into the requested VAS.  No CPU alias; the
 * returned void* is a GPU VA cast from uintptr_t.  Two VAS arms:
 *
 *   MC_VAS_UVM     — rm_alloc_vidmem + uvm_map_buffer (kernel-chosen
 *                    GPU VA).  Reachable from the UVM channel.
 *   MC_VAS_SYSMEM_CARRIER — mc_va_space_alloc_vidmem (rm_alloc_vidmem +
 *                    rm_map_memory_dma into the carrier).  Reachable
 *                    from the DMA + compute channels.  The 64-KiB
 *                    GPU-VA alignment requirement for vidmem PTEs is
 *                    handled inside the helper.
 *
 * On failure rolls back partial state and returns NULL.
 */
void *mc_malloc_device(mc_ctx_t *ctx, size_t n, mc_vas_t vas)
{
  mc_alloc_t *slot;
  NvHandle    h_mem  = 0;
  NvU64       gpu_va = 0;

  if (ctx == NULL || n == 0 || n > MC_MAX_TRANSFER_SIZE) return NULL;
  if (vas != MC_VAS_UVM
      && vas != MC_VAS_SYSMEM_CARRIER
      && vas != MC_VAS_FB_CARRIER) return NULL;
  slot = alloc_table_find_free(ctx);
  if (slot == NULL) { ERROR_LOG("mc_malloc_device: table full"); return NULL; }

  if (vas == MC_VAS_UVM)
  {
    h_mem = rm_alloc_vidmem(ctx->ctl_fd, ctx->h_client, ctx->h_device, n, NULL);
    if (!h_mem) return NULL;
    gpu_va = uvm_map_buffer(ctx->uvm_fd, ctx->dev_fd, ctx->h_client,
                            ctx->gpu_inst_uuid, h_mem, n, "d_user");
    if (!gpu_va)
    {
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device, h_mem,
                     "d_user_rollback");
      return NULL;
    }
  }
  else /* either carrier — sysmem-carrier or FB-carrier */
  {
    /* mc_va_space_alloc_vidmem owns both the rm_alloc_vidmem and the
     * carrier DMA-map.  Default 64-KiB align is enforced inside.  The
     * helper's only carrier-kind dependency is the per-resource carrier,
     * which is identical for both carriers; user buffers in
     * FB_CARRIER are allocated the same way as in SYSMEM_CARRIER. */
    gpu_va = mc_va_space_alloc_vidmem(ctx, &ctx->vas[vas], n, 0, &h_mem);
    if (!gpu_va) return NULL;
  }

  slot->ptr       = (void *)(uintptr_t)gpu_va;
  slot->h_mem     = h_mem;
  slot->gpu_va    = gpu_va;
  slot->size      = n;
  slot->is_device = true;
  slot->vas       = vas;
  return slot->ptr;
}

/*
 * Allocate host DRAM mapped into the requested VAS.  Two VAS arms:
 *
 *   MC_VAS_UVM     — Paper F1 anchored: caller's pointer is BOTH the CPU
 *                    VA and the GPU VA.  va_pool_reserve →
 *                    rm_alloc_sysmem_at(MAP_FIXED) → uvm_map_buffer_at.
 *   MC_VAS_SYSMEM_CARRIER — kernel-chosen CPU VA from rm_alloc_sysmem_at;
 *                    GPU VA is bump-allocated inside the carrier and
 *                    DIFFERENT from the CPU VA.  Caller passes the
 *                    returned CPU pointer to memcpy/CPU access; uses
 *                    mc_gpu_va() to recover the GPU VA when calling
 *                    primitives that need it directly.
 *
 * On failure rolls back partial state and returns NULL.
 */
static void *malloc_host_impl(mc_ctx_t *ctx, size_t n, mc_vas_t vas,
                              int want_wc)
{
  mc_alloc_t *slot;
  void       *cpu_va = NULL;
  NvHandle    h_mem  = 0;
  NvU64       gpu_va = 0;

  if (ctx == NULL || n == 0 || n > MC_MAX_TRANSFER_SIZE) return NULL;
  if (vas != MC_VAS_UVM
      && vas != MC_VAS_SYSMEM_CARRIER
      && vas != MC_VAS_FB_CARRIER) return NULL;
  slot = alloc_table_find_free(ctx);
  if (slot == NULL) { ERROR_LOG("mc_malloc_host: table full"); return NULL; }

  if (vas == MC_VAS_UVM)
  {
    cpu_va = va_pool_reserve((NvU64)n, "h_user");
    if (cpu_va == NULL) return NULL;
    h_mem = want_wc
              ? rm_alloc_sysmem_wc_at(ctx->ctl_fd, ctx->dev_path, ctx->h_client,
                                      ctx->h_device, (NvU64)n, cpu_va, &cpu_va)
              : rm_alloc_sysmem_at(ctx->ctl_fd, ctx->dev_path, ctx->h_client,
                                   ctx->h_device, (NvU64)n, cpu_va, &cpu_va);
    if (!h_mem) return NULL;
    gpu_va = uvm_map_buffer_at(ctx->uvm_fd, ctx->dev_fd, ctx->h_client,
                               ctx->gpu_inst_uuid, h_mem, cpu_va, (NvU64)n,
                               "h_user");
    if (!gpu_va)
    {
      munmap(cpu_va, n);
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device, h_mem,
                     "h_user_rollback");
      return NULL;
    }
  }
  else /* either carrier */
  {
    /* No VA-pool anchor: kernel chooses cpu_va, the carrier picks
     * gpu_va independently.  Both are bookkept in mc_alloc_t and
     * mc_gpu_va() recovers gpu_va from the user's CPU pointer.  The
     * sysmem-backed scratch allocator is identical for both carrier
     * kinds (host buffers in the FB carrier are still sysmem; only
     * the FB carrier's *channel resources* — PB/GPFIFO/USERD/sema —
     * live in FB). */
    gpu_va = want_wc
               ? mc_va_space_alloc_scratch_wc(ctx, &ctx->vas[vas],
                                              (NvU64)n, 0, &h_mem, &cpu_va)
               : mc_va_space_alloc_scratch(ctx, &ctx->vas[vas],
                                           (NvU64)n, 0, &h_mem, &cpu_va);
    if (!gpu_va) return NULL;
  }

  slot->ptr       = cpu_va;
  slot->h_mem     = h_mem;
  slot->gpu_va    = gpu_va;
  slot->size      = n;
  slot->is_device = false;
  slot->vas       = vas;
  return cpu_va;
}

void *mc_malloc_host(mc_ctx_t *ctx, size_t n, mc_vas_t vas)
{
  return malloc_host_impl(ctx, n, vas, /*want_wc=*/0);
}

void *mc_malloc_host_wc(mc_ctx_t *ctx, size_t n, mc_vas_t vas)
{
  return malloc_host_impl(ctx, n, vas, /*want_wc=*/1);
}

mc_status_t mc_host_register(mc_ctx_t *ctx, void *ptr, size_t n, mc_vas_t vas)
{
  mc_uvm_external_segment_t segs[MC_UVM_EXTERNAL_SEGMENTS_MAX];
  mc_alloc_t               *slot;
  NvHandle                  h_mem = 0;
  NvU64                     map_base = 0;
  NvU64                     map_size = 0;
  int                       seg_count = 0;
  int                       mapped_count = 0;
  int                       i;

  if (ctx == NULL || ptr == NULL || n == 0 || n > MC_MAX_TRANSFER_SIZE)
    return MC_EINVAL;
  if (vas != MC_VAS_UVM)
  {
    WARN_LOG("mc_host_register: only MC_VAS_UVM is supported today");
    return MC_EINVAL;
  }
  if (ctx->uvm_fd < 0)
    return MC_EINTERNAL;
  if (alloc_table_lookup(ctx, ptr) != NULL)
  {
    WARN_LOG("mc_host_register(%p): pointer already tracked", ptr);
    return MC_EINVAL;
  }

  slot = alloc_table_find_free(ctx);
  if (slot == NULL)
  {
    ERROR_LOG("mc_host_register: table full");
    return MC_EALLOC;
  }

  if (mc_build_host_register_segments(ptr, n, segs, &seg_count,
                                      &map_base, &map_size) != 0)
    return MC_EINVAL;

  h_mem = rm_register_user_memory(ctx->ctl_fd, ctx->dev_path, ctx->h_client,
                                  ctx->h_device,
                                  (void *)(uintptr_t)map_base, map_size);
  if (!h_mem)
    return MC_EIOCTL;

  for (i = 0; i < seg_count; i++)
  {
    if (uvm_map_buffer_range_at(ctx->uvm_fd, ctx->dev_fd, ctx->h_client,
                                ctx->gpu_inst_uuid, h_mem,
                                segs[i].base, segs[i].length,
                                segs[i].offset, "h_registered") != 0)
      goto rollback;
    mapped_count++;
  }

  slot->ptr               = ptr;
  slot->h_mem             = h_mem;
  slot->gpu_va            = (NvU64)(uintptr_t)ptr;
  slot->size              = (NvU64)n;
  slot->is_device         = false;
  slot->is_registered     = true;
  slot->vas               = vas;
  slot->map_base          = map_base;
  slot->map_size          = map_size;
  slot->uvm_segment_count = seg_count;
  for (i = 0; i < seg_count; i++)
    slot->uvm_segment_base[i] = segs[i].base;

  DEBUG_LOG("mc_host_register: ptr=%p size=0x%zx map_base=0x%llx map_size=0x%llx segments=%d h=0x%x",
            ptr, n, (unsigned long long)map_base,
            (unsigned long long)map_size, seg_count, h_mem);
  return MC_OK;

rollback:
  for (i = mapped_count; i > 0; i--)
    uvm_free_range(ctx->uvm_fd, segs[i - 1].base, "h_registered_rollback");
  rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device, h_mem,
                 "h_registered_rollback");
  return MC_EIOCTL;
}

mc_status_t mc_host_unregister(mc_ctx_t *ctx, void *ptr)
{
  mc_alloc_t *slot;
  NvHandle    h_mem;

  if (ctx == NULL) return MC_EINVAL;
  if (ptr == NULL) return MC_OK;

  slot = alloc_table_lookup(ctx, ptr);
  if (slot == NULL)
  {
    WARN_LOG("mc_host_unregister(%p): not found in allocation table", ptr);
    return MC_EINVAL;
  }
  if (!slot->is_registered)
  {
    WARN_LOG("mc_host_unregister(%p): pointer was not registered by mc_host_register",
             ptr);
    return MC_EINVAL;
  }

  mc_free_registered_uvm_ranges(ctx, slot);
  h_mem = slot->h_mem;
  memset(slot, 0, sizeof(*slot));

  if (h_mem)
    rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device, h_mem,
                   "h_registered_mem");

  return MC_OK;
}

/*
 * Free a pointer previously returned by either mc_malloc.  No-op on
 * NULL.  Unknown pointer logs WARN and returns: we'd rather a caller
 * see a noisy log message than have mc_free silently do something
 * surprising with a bogus pointer.
 *
 * Order mirrors mc_fini's user-alloc loop: UVM unmap first (so PTEs
 * are dropped before we free the backing handle), then munmap for
 * host allocs (device allocs have no CPU alias), then RM handle free.
 * The allocation-table slot is zeroed last so it becomes reusable.
 */
void mc_free(mc_ctx_t *ctx, void *p)
{
  mc_alloc_t *slot;

  if (ctx == NULL || p == NULL) return;
  slot = alloc_table_lookup(ctx, p);
  if (slot == NULL)
  {
    WARN_LOG("mc_free(%p): not found in allocation table", p);
    return;
  }
  if (slot->is_registered)
  {
    (void)mc_host_unregister(ctx, p);
    return;
  }
  /* GPU-mapping teardown: dispatch on the slot's VAS.
   *   MC_VAS_UVM     — uvm_unmap_buffer (UVM-managed PTEs).
   *   carrier kinds — release the per-resource NV50 carrier
   *                   tracked in vas->carriers[].
   * Calling uvm_unmap_buffer against a non-UVM gpu_va wedges UVM
   * bookkeeping, hence the gate. */
  if (slot->vas == MC_VAS_UVM)
  {
    uvm_unmap_buffer(ctx->uvm_fd, ctx->gpu_inst_uuid, slot->gpu_va, slot->size,
                     slot->is_device ? "d_user" : "h_user");
  }
  else /* sysmem-carrier or fb-carrier */
  {
    /* Per-resource carrier: release pulls the entry out of
     * vas->carriers[] and frees the carrier handle itself. */
    mc_va_space_release_carrier(ctx, &ctx->vas[slot->vas], slot->h_mem);
  }

  if (!slot->is_device)
    munmap(slot->ptr, slot->size);
  rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device, slot->h_mem,
                 slot->is_device ? "d_user_mem" : "h_user_mem");
  memset(slot, 0, sizeof(*slot));
}

/* Look up the GPU VA backing a pointer returned by mc_malloc_*.  See
 * mc.h for the contract (Paper-F1 round-trip for UVM-host; carrier-
 * host requires this to recover the GPU VA; device just returns its
 * own GPU VA back). */
uint64_t mc_gpu_va(mc_ctx_t *ctx, const void *user_ptr)
{
  const mc_alloc_t *slot;

  if (ctx == NULL || user_ptr == NULL) return 0;
  slot = alloc_table_lookup(ctx, user_ptr);
  if (slot == NULL) return 0;
  return slot->gpu_va;
}
