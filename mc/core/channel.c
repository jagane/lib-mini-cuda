/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * core/channel.c — per-kind channel bring-up and teardown.
 *
 * Three shapes share the core resource plumbing (pushbuffer, GPFIFO,
 * USERD, semaphore) and differ in how those resources are made visible to
 * the GPU: the UVM channel registers vidmem-backed resources with UVM,
 * while the carrier-bound DMA and compute channels map sysmem or
 * BAR1-aliased FB through per-resource NV50_MEMORY_VIRTUAL carriers.
 *
 * The selected copy engine flows unchanged through TSG, channel and CE
 * object — RM cross-checks the engine type at each allocation.
 *
 * Declared in internal/channel.h; driven by core/context.c.
 */

#define _GNU_SOURCE
#include <emmintrin.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nvtypes.h"
#include "class/cla06fsubch.h"          /* NVA06F_SUBCHANNEL_COPY_ENGINE / _COMPUTE */
#include "ctrl/ctrlc36f.h"              /* NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN */
#include "arch/hopper/qmd_v04.h"

#include "internal/types.h"
#include "internal/log.h"
#include "internal/rm.h"
#include "internal/uvm.h"
#include "internal/submit.h"
#include "internal/vaspace.h"
#include "internal/channel.h"

/* ── Channel alloc/free core helpers ─────────────────────────────────
 *
 * Both mc_dma_channel_init and mc_compute_channel_init share the same
 * skeleton: allocate three sysmem buffers (gpfifo+USERD, pushbuffer,
 * semaphore), DMA-map each into the carrier VAS, allocate TSG + channel
 * + engine-class object, fetch the work-submit-token, and schedule.
 * mc_channel_alloc_via_carrier encapsulates the shared sequence; each
 * core buffer gets its own NV50_MEMORY_VIRTUAL carrier via
 * mc_va_space_dma_map_resource — libcuda's recipe (one carrier per
 * source hMemory, RM picks the GPU VA).
 */

/*
 * mc_channel_alloc_via_carrier — allocate the gpfifo/pb/sema buffers,
 * DMA-map them into the carrier VAS, allocate TSG + channel +
 * engine-class object, fetch the work-submit-token, schedule.  The
 * engine-class branch (CE vs compute) is determined by ch->type.
 *
 * Caller must have already populated ch->type, ch->vas_id,
 * ch->subchannel, and ch->engine_type.
 *
 * Returns 0 on success, -1 on failure.  Partial state is freed by
 * mc_channel_free_core.
 */
/* Allocate one channel-resource buffer (PB / GPFIFO+USERD / sema)
 * with a host-visible CPU pointer.  Aperture is picked by the carrier
 * kind:
 *
 *   MC_VAS_KIND_CARRIER     → sysmem allocation; CPU pointer is the
 *                             real backing memory.  (today's path)
 *   MC_VAS_KIND_CARRIER_FB  → vidmem allocation; CPU pointer is a
 *                             BAR1-aliased view of the FB pages
 *                             (REFLECTED mapping).  PBDMA / SM read
 *                             FB directly through the GPU MMU; the
 *                             host alias is for one-time SET_OBJECT
 *                             pushbuffer writes and diagnostics.
 *
 * `is_userd` marks the one buffer that carries USERD.  On an
 * architecture whose runlist entry cannot hold an arbitrary host
 * physical address (cap.userd_requires_vidmem — Ampere), that buffer
 * is forced to vidmem even in a sysmem carrier, because RM rejects the
 * channel outright otherwise.  See the field comment in hal/arch.h.
 * This also moves the sysmem carrier onto the split core/vaspace.c
 * already documents as libcuda's recipe: GPFIFO + USERD in FB,
 * pushbuffer and semaphore in sysmem.
 *
 * `*out_h_mem` and `*out_cpu_ptr` are populated on success.  Returns
 * 0 on success, -1 on failure.
 */
static int mc_channel_alloc_resource(mc_ctx_t *ctx, mc_va_space_kind_t kind,
                                     NvU64 size, bool is_userd,
                                     NvHandle *out_h_mem, void **out_cpu_ptr)
{
  bool want_vidmem = (kind == MC_VAS_KIND_CARRIER_FB)
                     || (is_userd && ctx->arch->cap.userd_requires_vidmem);

  *out_cpu_ptr = NULL;
  if (kind != MC_VAS_KIND_CARRIER && kind != MC_VAS_KIND_CARRIER_FB)
  {
    return -1;
  }

  if (!want_vidmem)
  {
    *out_h_mem = rm_alloc_sysmem_wc_at(ctx->ctl_fd, ctx->dev_path, ctx->h_client,
                                    ctx->h_device, size, NULL, out_cpu_ptr);
  }
  else
  {
    /* No want_va: FB BAR1 aliases for channel resources don't need
     * the Paper-F1 VA pool — they have no GPU VA == CPU VA invariant
     * to maintain (the GPU sees them via the carrier's bump-allocated
     * GPU VA, which is unrelated to where the host BAR1 mmap lands).
     * This is the same shape the UVM channel already uses for its own
     * GPFIFO+USERD, which is the placement measured working on GA100. */
    *out_h_mem = rm_alloc_vidmem_bar1_at(ctx->ctl_fd, ctx->dev_path,
                                         ctx->h_client, ctx->h_device,
                                         size, NULL, out_cpu_ptr);
  }
  return (*out_h_mem == 0 || *out_cpu_ptr == NULL) ? -1 : 0;
}

static int mc_channel_alloc_via_carrier(mc_ctx_t *ctx, mc_channel_t *ch)
{
  NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS token_params = {};
  mc_va_space_t *vas = &ctx->vas[ch->vas_id];
  void *gpfifo_cpu_ptr = NULL;
  void *pb_cpu_ptr     = NULL;
  void *sema_cpu_ptr   = NULL;

  if (!mc_va_space_kind_is_carrier(vas->kind)) return -1;

  /* Three core allocations.  Aperture chosen by the VAS kind:
   * sysmem-carrier gets sysmem allocs, FB-carrier gets vidmem +
   * BAR1-aliased CPU mappings.  Either way the CPU pointer is
   * read/writeable for bring-up + diagnostics.  The first buffer
   * carries USERD, so it may additionally be forced to vidmem by the
   * architecture — see mc_channel_alloc_resource. */
  if (mc_channel_alloc_resource(ctx, vas->kind, MC_GPFIFO_USERD_SIZE,
                                /*is_userd=*/true,
                                &ch->h_gpfifo_mem, &gpfifo_cpu_ptr) != 0)
    return -1;
  ch->gpfifo_cpu  = gpfifo_cpu_ptr;
  ch->gpfifo_ring = (volatile uint32_t *)gpfifo_cpu_ptr;
  ch->userd_cpu   = (char *)gpfifo_cpu_ptr + MC_USERD_OFFSET;

  if (mc_channel_alloc_resource(ctx, vas->kind, MC_GPFIFO_USERD_SIZE,
                                /*is_userd=*/false,
                                &ch->h_pb_mem, &pb_cpu_ptr) != 0)
    return -1;
  ch->pb_cpu = (uint32_t *)pb_cpu_ptr;

  if (mc_channel_alloc_resource(ctx, vas->kind, MC_GPFIFO_USERD_SIZE,
                                /*is_userd=*/false,
                                &ch->h_sema_mem, &sema_cpu_ptr) != 0)
    return -1;
  ch->sema_ptr = (volatile uint32_t *)sema_cpu_ptr;
  *ch->sema_ptr = 0;

  /* DMA-map each core buffer through libcuda's per-resource carrier
   * shape (one NV50_MEMORY_VIRTUAL per source, dmaOffset=0, RM picks
   * GPU VA).  See mc/README.md, "VA spaces". */
  ch->gpfifo_gpu_va = mc_va_space_dma_map_resource(
      ctx, vas, ch->h_gpfifo_mem, MC_GPFIFO_USERD_SIZE);
  if (!ch->gpfifo_gpu_va) return -1;

  ch->pb_gpu_va = mc_va_space_dma_map_resource(
      ctx, vas, ch->h_pb_mem, MC_GPFIFO_USERD_SIZE);
  if (!ch->pb_gpu_va) return -1;

  ch->sema_gpu_va = mc_va_space_dma_map_resource(
      ctx, vas, ch->h_sema_mem, MC_GPFIFO_USERD_SIZE);
  if (!ch->sema_gpu_va) return -1;

  /* FB-carrier victim DMA channel: allocate a sysmem release-sema
   * cell that the SM-author path uses as the active sema for both
   * H2D and D2H.  PCIe producer-side ordering of posted writes from
   * one Requester ID into one address space (sysmem) then guarantees
   * the sema MWr cannot land at host DRAM ahead of any earlier data
   * MWr from the same channel — so the host polling this cell is a
   * valid completion observable.  The HBM cell allocated above
   * remains for diagnostic dumps but isn't on the success path.
   * See experimental/sm_owner.c::mc_sm_owner_submit for the unified routing. */
  if (vas->kind == MC_VAS_KIND_CARRIER_FB
      && ch->role == MC_ROLE_SM_VICTIM_DMA)
  {
    void *sema_sys_cpu = NULL;
    ch->sema_sysmem_gpu_va = mc_va_space_alloc_scratch_wc(
        ctx, vas, MC_SEMA_SIZE, /*align=*/0,
        &ch->h_sema_sysmem_mem, &sema_sys_cpu);
    if (!ch->sema_sysmem_gpu_va) return -1;
    ch->sema_sysmem_ptr = (volatile uint32_t *)sema_sys_cpu;
    *ch->sema_sysmem_ptr = 0;
  }

  /* TSG + channel + engine-class object on the carrier's VAS. */
  ch->h_tsg = rm_alloc_tsg(ctx->ctl_fd, ctx->arch, ctx->h_client, ctx->h_device,
                           ch->engine_type, vas->h_vaspace);
  if (!ch->h_tsg) return -1;

  ch->h_channel = rm_alloc_channel(
      ctx->ctl_fd, ctx->arch, ctx->h_client, ch->h_tsg, ch->gpfifo_gpu_va,
      MC_GPFIFO_ENTRIES, ch->engine_type, ch->h_gpfifo_mem,
      MC_USERD_OFFSET);
  if (!ch->h_channel) return -1;

  /* Dispatch on the channel's kind rather than inferring it from its engine
   * type.  The old test was `engine_type == NV2080_ENGINE_TYPE_GR0`, which
   * only worked because compute happened to be the only role on GR0 -- an
   * architecture that runs compute on a different runlist, or a CE role that
   * lands on GR0, would silently allocate the wrong engine class. */
  if (ch->type == MC_CH_TYPE_COMPUTE)
    ch->h_engine = rm_alloc_compute(ctx->ctl_fd, ctx->arch, ctx->h_client,
                                    ch->h_channel);
  else
    ch->h_engine = rm_alloc_ce(ctx->ctl_fd, ctx->arch, ctx->h_client,
                               ch->h_channel, ch->engine_type);
  if (!ch->h_engine) return -1;

  /* Work-submit-token + schedule. */
  if (rm_control(ctx->ctl_fd, ctx->h_client, ch->h_channel,
                 NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN, &token_params,
                 sizeof(token_params)) != 0)
    return -1;
  ch->work_submit_token = token_params.workSubmitToken;

  if (rm_gpfifo_schedule(ctx->ctl_fd, ctx->h_client, ch->h_channel) != 0)
    return -1;

  ch->gp_put       = 0;
  ch->sema_payload = 0;
  return 0;
}

/*
 * mc_channel_free_core — drain + disable the channel, then DMA-unmap
 * and free the core (gpfifo/pb/sema) plumbing.  Best-effort: every
 * step checks its own precondition so partial-init states are safe.
 *
 * Callers run this FIRST in their kind-specific *_channel_fini, then
 * unmap and free their kind-specific extras (qmd buffers, BAR1
 * doorbell mapping, VAS, etc.).  This ordering matters: the channel
 * may have outstanding GPU work referencing those PTEs; drain +
 * disable here ensures no SM/CE/PBDMA touches them after we return.
 */
static void mc_channel_free_core(mc_ctx_t *ctx, mc_channel_t *ch)
{
  /* Drain (best-effort). */
  if (ch->userd_cpu != NULL)
    drain_channel(ch, MC_TIMEOUT_MS);

  /* Disable the channel before unmapping anything it might still
   * dereference. */
  if (ch->h_channel && ctx->ctl_fd >= 0)
    rm_channel_disable(ctx->ctl_fd, ctx->h_client, ch->h_channel);

  /* DMA-unmap each per-resource carrier and free the carrier handle.
   * The carrier was allocated by mc_va_space_dma_map_resource and
   * recorded in vas->carriers[]; we take ownership at channel free
   * time so the h_mem can be safely freed below.
   *
   * Order matches alloc order: gpfifo, pb, sema (oldest carriers
   * first).  We pull each entry out of the table by handle match
   * and shift survivors down. */
  mc_va_space_release_carrier(ctx, &ctx->vas[ch->vas_id], ch->h_gpfifo_mem);
  mc_va_space_release_carrier(ctx, &ctx->vas[ch->vas_id], ch->h_pb_mem);
  mc_va_space_release_carrier(ctx, &ctx->vas[ch->vas_id], ch->h_sema_mem);
  /* No-op when h_sema_sysmem_mem is 0 (non-FB-victim channels). */
  mc_va_space_release_carrier(ctx, &ctx->vas[ch->vas_id],
                              ch->h_sema_sysmem_mem);

  /* CPU-side mappings. */
  if (ch->gpfifo_cpu)       munmap(ch->gpfifo_cpu, MC_GPFIFO_USERD_SIZE);
  if (ch->pb_cpu)           munmap(ch->pb_cpu,     MC_GPFIFO_USERD_SIZE);
  if (ch->sema_ptr)         munmap((void *)ch->sema_ptr, MC_GPFIFO_USERD_SIZE);
  if (ch->sema_sysmem_ptr)  munmap((void *)ch->sema_sysmem_ptr, MC_SEMA_SIZE);

  /* RM handles in reverse alloc order. */
  if (ctx->ctl_fd >= 0 && ctx->h_client)
  {
    if (ch->h_engine)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ch->h_channel,
                     ch->h_engine, "h_engine");
    if (ch->h_channel)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ch->h_tsg,
                     ch->h_channel, "h_channel");
    if (ch->h_tsg)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ch->h_tsg, "h_tsg");
    if (ch->h_sema_sysmem_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ch->h_sema_sysmem_mem, "h_sema_sysmem_mem");
    if (ch->h_sema_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ch->h_sema_mem, "h_sema_mem");
    if (ch->h_pb_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ch->h_pb_mem, "h_pb_mem");
    if (ch->h_gpfifo_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ch->h_gpfifo_mem, "h_gpfifo_mem");
  }
}

/* ── DMA-channel bring-up ─────────────────────────────────────────────
 *
 * Allocates a CE channel on a carrier VAS (sysmem-carrier or
 * FB-carrier).  Channel core (PB/GPFIFO/USERD/sema) follows the
 * VAS's aperture rule via mc_channel_alloc_resource.  The kind-
 * specific extras are the token cell — a small sysmem scratch
 * DMA-mapped into the carrier VAS, used as the CE source for
 * doorbell-write demos.
 *
 * `slot` selects which mc_ctx.vas[] entry to attach to; `role`
 * tags the resulting channel (HOST_DMA or SM_VICTIM_DMA).  The
 * caller's mc_vas_add_channel CHECK enforces role uniqueness within
 * the VAS.  Both DMA roles use the same bring-up code; only the role
 * tag and intended dispatch differ.
 */
int mc_dma_channel_init(mc_ctx_t *ctx, mc_vas_t slot,
                               mc_channel_role_t role,
                               NvU32 lce_engine_type)
{
  mc_va_space_t         *vas = &ctx->vas[slot];
  mc_channel_t          *ch  = mc_vas_add_channel(vas, role);
  if (ch != NULL) ch->arch = ctx->arch;
  struct mc_dma_extras  *ex;
  void                  *cpu = NULL;

  if (ch == NULL) return -1;
  ex              = &ch->x.dma;
  ch->type        = MC_CH_TYPE_DMA;
  ch->vas_id      = slot;
  ch->subchannel  = NVA06F_SUBCHANNEL_COPY_ENGINE;
  ch->engine_type = lce_engine_type;

  if (mc_channel_alloc_via_carrier(ctx, ch) != 0) return -1;

  /* Token cell: always sysmem, regardless of carrier kind.  The
   * doorbell-write demo's CE op reads 4 bytes from this cell and
   * writes them to the BAR1 doorbell page; sysmem keeps the host
   * able to read/write the cell via a normal CPU pointer. */
  ex->token_cell_gpu_va = mc_va_space_alloc_scratch_wc(
      ctx, vas, 4, 4,
      &ex->h_token_mem, &cpu);
  if (!ex->token_cell_gpu_va) return -1;
  ex->token_cell = (volatile uint32_t *)cpu;
  *ex->token_cell = 0;

  DEBUG_LOG("mc_dma_channel ok: vas=%d role=%d ch=0x%x token_gpu_va=0x%llx work_token=0x%x",
            (int)slot, (int)role, ch->h_channel,
            (unsigned long long)ex->token_cell_gpu_va,
            ch->work_submit_token);
  return 0;
}

void mc_dma_channel_fini(mc_ctx_t *ctx, mc_vas_t slot,
                                mc_channel_role_t role)
{
  mc_channel_t         *ch = mc_vas_find_channel(&ctx->vas[slot], role);
  struct mc_dma_extras *ex;
  if (ch == NULL) return;
  ex = &ch->x.dma;

  /* Core first: drain + disable so no in-flight CE op can be
   * referencing the token cell PTE we're about to unmap.  The core
   * also releases the gpfifo/pb/sema per-resource carriers and frees
   * the TSG/channel/engine + hMemory handles. */
  mc_channel_free_core(ctx, ch);

  /* Token cell teardown: release its per-resource carrier, then free
   * the sysmem hMemory.  The carrier VAS + BAR1 doorbell PTE belong
   * to globals, not us. */
  mc_va_space_release_carrier(ctx, &ctx->vas[slot], ex->h_token_mem);
  if (ex->token_cell)
    munmap((void *)ex->token_cell, 4);
  if (ctx->ctl_fd >= 0 && ctx->h_client && ex->h_token_mem)
    rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                   ex->h_token_mem, "dma_ch.token_cell");

  memset(ch, 0, sizeof(*ch));
}

/* ── COMPUTE-channel bring-up (mc_compute_channel_init) ────────────────
 *
 * Allocates a HOPPER_COMPUTE_A channel on the carrier VAS — the SAME
 * VAS the DMA channel uses, so SM threads walk the same PDB and can
 * reach the BAR1 doorbell PTE at ctx->vas[CARRIER].dbell_gpu_va.
 *
 * Differences from mc_dma_channel_init:
 *   - engineType comes from the architecture's engine policy for
 *     MC_ROLE_COMPUTE (GR0 on Hopper) rather than an LCE.
 *   - Engine class = HOPPER_COMPUTE_A (0xCBC0) rather than HOPPER_DMA_COPY_A.
 *   - Loads a single compute module (QMD/CB0/SASS) into the carrier VAS.
 */

/* Allocate the QMD/CB0/SASS triple for one compute kernel and stage
 * its SASS bytes.  Each region lives in the carrier VAS via the
 * scratch allocator — sysmem-backed, GPU MMU PTE per region.  All
 * sysmem (not vidmem): keeps bring-up simple; libcuda would put
 * these in a vidmem code/data pool for performance, but a sysmem
 * alloc with a GPU MMU PTE is functionally identical for one launch. */
static int mc_compute_module_init(mc_ctx_t *ctx, mc_vas_t slot,
                                  struct mc_compute_module *mod,
                                  const uint8_t *sass_bytes,
                                  size_t sass_len)
{
  mc_va_space_t *vas = &ctx->vas[slot];
  void          *qmd_cpu_ptr     = NULL;
  void          *cb0_cpu_ptr     = NULL;
  void          *sass_cpu_ptr    = NULL;
  void          *scratch_cpu_ptr = NULL;
  NvU64          scratch_base_gpu_va;

  mod->qmd_gpu_va = mc_va_space_alloc_scratch_wc(
      ctx, vas, MC_GPFIFO_USERD_SIZE, 0, &mod->h_qmd_mem, &qmd_cpu_ptr);
  if (!mod->qmd_gpu_va) return -1;
  mod->qmd_cpu = (uint8_t *)qmd_cpu_ptr;

  mod->cb0_gpu_va = mc_va_space_alloc_scratch_wc(
      ctx, vas, MC_GPFIFO_USERD_SIZE, 0, &mod->h_cb0_mem, &cb0_cpu_ptr);
  if (!mod->cb0_gpu_va) return -1;
  mod->cb0_cpu = (uint8_t *)cb0_cpu_ptr;

  mod->sass_gpu_va = mc_va_space_alloc_scratch_wc(
      ctx, vas, MC_GPFIFO_USERD_SIZE, 0, &mod->h_sass_mem, &sass_cpu_ptr);
  if (!mod->sass_gpu_va) return -1;
  mod->sass_cpu = (uint8_t *)sass_cpu_ptr;

  /* Dedicated scratch dword exposed via mc_compute_get_scratch.
   *
   * This target proves the SM's global store is visible to the host.
   * Use BAR1-aliased vidmem rather than tiny sysmem here: on H200 with
   * the 610 ABI the old sysmem scratch store is not visible even though
   * the kernel completion semaphore fires, while the same kernel's BAR1
   * doorbell store is the proven working path.
   */
  mod->h_scratch_mem = rm_alloc_vidmem_bar1_at(ctx->ctl_fd, ctx->dev_path,
                                               ctx->h_client, ctx->h_device,
                                               MC_GPFIFO_USERD_SIZE, NULL,
                                               &scratch_cpu_ptr);
  if (!mod->h_scratch_mem || scratch_cpu_ptr == NULL) return -1;

  scratch_base_gpu_va = mc_va_space_dma_map_resource(
      ctx, vas, mod->h_scratch_mem, MC_GPFIFO_USERD_SIZE);
  if (!scratch_base_gpu_va)
  {
    munmap(scratch_cpu_ptr, MC_GPFIFO_USERD_SIZE);
    rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                   mod->h_scratch_mem, "compute_module.h_scratch_mem_rollback");
    mod->h_scratch_mem = 0;
    return -1;
  }
  mod->scratch_cpu    = (volatile uint32_t *)((uint8_t *)scratch_cpu_ptr + 32);
  mod->scratch_gpu_va = scratch_base_gpu_va + 32;

  /* Stage SASS bytes once; per-call code only patches QMD + CB0. */
  memcpy(mod->sass_cpu, sass_bytes, sass_len);
  _mm_sfence();
  return 0;
}

static void mc_compute_module_fini(mc_ctx_t *ctx, mc_vas_t slot,
                                   struct mc_compute_module *mod)
{
  /* Release each per-resource carrier in reverse alloc order (LIFO).
   * mc_va_space_release_carrier no-ops if the handle isn't tracked,
   * so partial-init failures are safe. */
  mc_va_space_release_carrier(ctx, &ctx->vas[slot], mod->h_scratch_mem);
  mc_va_space_release_carrier(ctx, &ctx->vas[slot], mod->h_sass_mem);
  mc_va_space_release_carrier(ctx, &ctx->vas[slot], mod->h_cb0_mem);
  mc_va_space_release_carrier(ctx, &ctx->vas[slot], mod->h_qmd_mem);
  /* munmap each CPU mapping created during mc_compute_module_init.
   * These were leaked previously: every mc_ctx teardown left orphan
   * mappings around.  Sizes mirror the alloc sites above. */
  if (mod->scratch_cpu)
    munmap((uint8_t *)mod->scratch_cpu - 32, MC_GPFIFO_USERD_SIZE);
  if (mod->sass_cpu) munmap(mod->sass_cpu, MC_GPFIFO_USERD_SIZE);
  if (mod->cb0_cpu)  munmap(mod->cb0_cpu,  MC_GPFIFO_USERD_SIZE);
  if (mod->qmd_cpu)  munmap(mod->qmd_cpu,  MC_GPFIFO_USERD_SIZE);

  if (ctx->ctl_fd >= 0 && ctx->h_client)
  {
    if (mod->h_scratch_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     mod->h_scratch_mem, "compute_module.h_scratch_mem");
    if (mod->h_sass_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     mod->h_sass_mem, "compute_module.h_sass_mem");
    if (mod->h_cb0_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     mod->h_cb0_mem, "compute_module.h_cb0_mem");
    if (mod->h_qmd_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     mod->h_qmd_mem, "compute_module.h_qmd_mem");
  }
}

int mc_compute_channel_init(mc_ctx_t *ctx, mc_vas_t slot)
{
  mc_va_space_t            *vas = &ctx->vas[slot];
  mc_channel_t             *ch  = mc_vas_add_channel(vas, MC_ROLE_COMPUTE);
  if (ch != NULL) ch->arch = ctx->arch;
  struct mc_compute_extras *ex;

  if (ch == NULL) return -1;
  ex              = &ch->x.compute;
  ch->type        = MC_CH_TYPE_COMPUTE;
  ch->vas_id      = slot;
  ch->subchannel  = NVA06F_SUBCHANNEL_COMPUTE;
  ch->engine_type = ctx->arch->ops->engine_for_role(ctx->arch, MC_ROLE_COMPUTE,
                                                    0);

  if (mc_channel_alloc_via_carrier(ctx, ch) != 0) return -1;

  /* Load the embedded compute kernels.  Two modules per channel:
   *   - mc_doorbell_kernel: tiny `*dst = token` kernel used by
   *     mc_compute_doorbell_kernel / mc_memcpy_gpu_doorbell_sm.
   *   - sm_owner_kernel: the SM-authors-channel kernel used by
   *     mc_sm_owner_*.  Authors PB+GPFIFO+USERD+doorbell from a
   *     single SM thread instead of the host doing it.
   * Each module owns its own QMD/CB0/SASS triple in this VAS
   * (sysmem-backed via the scratch allocator regardless of carrier
   * kind — only the channel-core PB/GPFIFO/USERD/sema follow the
   * carrier's aperture rule). */
  if (mc_compute_module_init(ctx, slot, &ex->module,
                             ctx->arch->kernels.store_dword.bytes,
                             ctx->arch->kernels.store_dword.len) != 0)
    return -1;

  /* Only stage sm_owner where the architecture actually has it.  Ampere, Ada
   * and GA10x ship no sm_owner cubin -- that kernel ends in a BAR1 doorbell
   * write they have no aperture for -- so staging it there passed a NULL
   * blob to a memcpy (undefined even for length 0) and allocated a QMD, CB0,
   * SASS page and scratch page for a kernel that can never run.
   *
   * ex->sm_owner_module stays zeroed, which is what mc_compute_module_fini
   * already tolerates.  Nothing reaches it: MC_FEAT_XFER_SM is withheld
   * wherever the blob is absent. */
  if (ctx->arch->kernels.sm_owner.bytes != NULL
      && ctx->arch->kernels.sm_owner.len != 0)
  {
    if (mc_compute_module_init(ctx, slot, &ex->sm_owner_module,
                               ctx->arch->kernels.sm_owner.bytes,
                               ctx->arch->kernels.sm_owner.len) != 0)
      return -1;
  }

  DEBUG_LOG("mc_compute_channel ok: vas=%d tsg=0x%x ch=0x%x compute=0x%x "
            "gpfifo_gpu_va=0x%llx token=0x%x",
            (int)slot, ch->h_tsg, ch->h_channel, ch->h_engine,
            (unsigned long long)ch->gpfifo_gpu_va,
            ch->work_submit_token);
  return 0;
}

/* Symmetric reverse-order teardown for the compute channel.  Best-effort.
 * Does NOT free the carrier VAS (that belongs to globals). */
void mc_compute_channel_fini(mc_ctx_t *ctx, mc_vas_t slot)
{
  mc_channel_t             *ch = mc_vas_find_channel(
                                     &ctx->vas[slot], MC_ROLE_COMPUTE);
  struct mc_compute_extras *ex;
  if (ch == NULL) return;
  ex = &ch->x.compute;

  /* Core first: drain + disable so no in-flight kernel can be
   * dereferencing the QMD/CB0/SASS PTEs we're about to unmap. */
  mc_channel_free_core(ctx, ch);

  /* Tear down the loaded compute modules in reverse-init order. */
  mc_compute_module_fini(ctx, slot, &ex->sm_owner_module);
  mc_compute_module_fini(ctx, slot, &ex->module);

  memset(ch, 0, sizeof(*ch));
}

/* ── UVM-channel bring-up (mc_uvm_channel_init) ────────────────────────
 *
 * The UVM channel is the foundation: it owns the UVM-managed VAS
 * that mc_malloc_host / mc_malloc_device anchor user allocations
 * into, and it's the channel mc_memcpy / mc_memcpy submits
 * to.
 *
 * Unlike the DMA / compute channels, this one does NOT use
 * mc_channel_alloc_via_carrier: its GPFIFO + USERD live in vidmem
 * (BAR1-mapped via rm_map_memory_at) and the channel is registered
 * with UVM via UVM_REGISTER_CHANNEL.  The GPFIFO+USERD region must
 * be UVM-mapped before channel construct (gpFifoOffset is read at
 * construct time); the pushbuffer + semaphore are UVM-mapped after.
 *
 * Returns 0 on success, -1 on failure (mc_fini cleans up partial
 * state).
 */
int mc_uvm_channel_init(mc_ctx_t *ctx, NvU32 lce_engine_type)
{
  NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS token_params = {};
  mc_channel_t         *ch;
  struct mc_uvm_extras *ex;
  void                 *pb_direct   = NULL;
  void                 *sema_direct = NULL;

  ch = mc_vas_add_channel(&ctx->vas[MC_VAS_UVM], MC_ROLE_UVM_CE);
  if (ch != NULL) ch->arch = ctx->arch;
  if (ch == NULL) return -1;

  ch->type        = MC_CH_TYPE_UVM;
  ch->vas_id      = MC_VAS_UVM;
  ch->subchannel  = NVA06F_SUBCHANNEL_COPY_ENGINE;
  ch->engine_type = lce_engine_type;

  ex = &ch->x.uvm;

  /* GPFIFO+USERD vidmem, pushbuffer sysmem, semaphore sysmem — see the
   * comment on the sema allocation below for why it is not in FB. */
  ex->h_gpfifo_userd_mem = rm_alloc_vidmem(ctx->ctl_fd, ctx->h_client,
                                      ctx->h_device, MC_GPFIFO_USERD_SIZE, NULL);
  if (!ex->h_gpfifo_userd_mem) return -1;
  /* The uniform mc_channel_t.h_gpfifo_mem field aliases the same RM
   * handle as ch->x.uvm.h_gpfifo_userd_mem (which the BAR1 mmap also
   * targets); the generic submit primitives use it. */
  ch->h_gpfifo_mem = ex->h_gpfifo_userd_mem;

  pb_direct = va_pool_reserve(MC_PB_SIZE, "pushbuffer");
  if (pb_direct == NULL) return -1;
  ch->h_pb_mem = rm_alloc_sysmem_wc_at(ctx->ctl_fd, ctx->dev_path, ctx->h_client,
                                    ctx->h_device, MC_PB_SIZE, pb_direct,
                                    &pb_direct);
  if (!ch->h_pb_mem) return -1;

  /* The release semaphore lives in sysmem, because that is where libcuda
   * puts it — libcuda's SET_SEMAPHORE_VA resolves to NV01_MEMORY_SYSTEM.
   * The host polls this cell in a tight loop; from an HBM cell that poll
   * is a BAR1 read, so every spin becomes a PCIe transaction competing
   * with the transfer it is waiting on.  Ordering holds either way: the
   * release launch carries FLUSH_ENABLE with FLUSH_TYPE=SYS, and for a
   * sysmem destination PCIe posted-write ordering from a single Requester
   * ID also keeps the release behind the data. */
  sema_direct = va_pool_reserve(MC_SEMA_SIZE, "sema");
  if (sema_direct == NULL) return -1;
  ch->h_sema_mem = rm_alloc_sysmem_at(ctx->ctl_fd, ctx->dev_path, ctx->h_client,
                                      ctx->h_device, MC_SEMA_SIZE, sema_direct,
                                      &sema_direct);
  if (!ch->h_sema_mem) return -1;

  /* UVM-managed VA space + UVM setup.  The GPFIFO+USERD region must
   * be UVM-mapped before channel construct: HOPPER_CHANNEL_GPFIFO_A
   * reads gpFifoOffset at construct time. */
  if (mc_va_space_init_uvm(ctx) != 0) return -1;

  if (uvm_setup(ctx->ctl_fd, ctx->dev_fd, ctx->h_client, ctx->h_subdevice,
                ctx->vas[MC_VAS_UVM].h_vaspace,
                ctx->gpu_inst_uuid, &ctx->uvm_fd) != 0)
    return -1;

  ex->gpfifo_userd_cpu = va_pool_reserve(MC_GPFIFO_USERD_SIZE, "gpfifo_userd");
  if (ex->gpfifo_userd_cpu == NULL) return -1;
  ex->gpfifo_userd_cpu = rm_map_memory_at(ctx->ctl_fd, ctx->dev_path,
                                     ctx->h_client, ctx->h_device,
                                     ex->h_gpfifo_userd_mem, 0,
                                     MC_GPFIFO_USERD_SIZE,
                                     0, ex->gpfifo_userd_cpu);
  if (ex->gpfifo_userd_cpu == NULL) return -1;
  ch->gpfifo_cpu  = ex->gpfifo_userd_cpu;
  ch->gpfifo_ring = (volatile uint32_t *)ex->gpfifo_userd_cpu;
  ch->userd_cpu   = (char *)ex->gpfifo_userd_cpu + MC_USERD_OFFSET;

  ch->gpfifo_gpu_va = uvm_map_buffer_at(ctx->uvm_fd, ctx->dev_fd,
                                        ctx->h_client, ctx->gpu_inst_uuid,
                                        ex->h_gpfifo_userd_mem,
                                        ex->gpfifo_userd_cpu,
                                        MC_GPFIFO_USERD_SIZE, "gpfifo_userd");
  if (!ch->gpfifo_gpu_va) return -1;

  /* TSG + channel + CE engine, all bound to the UVM VAS. */
  ch->h_tsg = rm_alloc_tsg(ctx->ctl_fd, ctx->arch, ctx->h_client, ctx->h_device,
                           lce_engine_type,
                           ctx->vas[MC_VAS_UVM].h_vaspace);
  if (!ch->h_tsg) return -1;

  ch->h_channel = rm_alloc_channel(ctx->ctl_fd, ctx->arch, ctx->h_client, ch->h_tsg,
                                   ch->gpfifo_gpu_va, MC_GPFIFO_ENTRIES,
                                   lce_engine_type, ex->h_gpfifo_userd_mem,
                                   MC_USERD_OFFSET);
  if (!ch->h_channel) return -1;

  ch->h_engine = rm_alloc_ce(ctx->ctl_fd, ctx->arch, ctx->h_client, ch->h_channel,
                             lce_engine_type);
  if (!ch->h_engine) return -1;

  if (rm_control(ctx->ctl_fd, ctx->h_client, ch->h_channel,
                 NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN, &token_params,
                 sizeof(token_params)) != 0)
    return -1;
  ch->work_submit_token = token_params.workSubmitToken;

  /* UVM-map pushbuffer + semaphore.  User buffers added by mc_malloc_*. */
  ch->pb_gpu_va = uvm_map_buffer_at(ctx->uvm_fd, ctx->dev_fd, ctx->h_client,
                                    ctx->gpu_inst_uuid, ch->h_pb_mem,
                                    pb_direct, MC_PB_SIZE, "pushbuffer");
  if (!ch->pb_gpu_va) return -1;
  ch->pb_cpu = (uint32_t *)pb_direct;

  /* sema_direct already points at the sysmem cell allocated above. */
  ch->sema_gpu_va = uvm_map_buffer_at(ctx->uvm_fd, ctx->dev_fd, ctx->h_client,
                                      ctx->gpu_inst_uuid, ch->h_sema_mem,
                                      sema_direct, MC_SEMA_SIZE, "sema");
  if (!ch->sema_gpu_va) return -1;
  ch->sema_ptr = (volatile uint32_t *)sema_direct;

  if (uvm_register_channel(ctx->uvm_fd, ctx->dev_fd, ctx->h_client,
                           ctx->gpu_inst_uuid, ch->h_channel) != 0)
    return -1;

  if (rm_gpfifo_schedule(ctx->ctl_fd, ctx->h_client, ch->h_channel) != 0)
    return -1;

  ch->gp_put       = 0;
  ch->sema_payload = 0;
  return 0;
}

/* Symmetric reverse-order teardown for the UVM channel.  Best-effort:
 * every step checks its own precondition so partial-init states from
 * mc_uvm_channel_init failures are safe to pass in.  Mirrors the
 * mc_dma_channel_fini / mc_compute_channel_fini structure (drain +
 * disable first, then unmap + free).  Also unregisters the GPU + VA
 * space from UVM here — symmetric reverse of uvm_setup() + UVM_REGISTER_CHANNEL.
 * The DMA / compute channels never talk to UVM, so this state only
 * exists when the UVM channel does. */
void mc_uvm_channel_fini(mc_ctx_t *ctx)
{
  mc_channel_t         *ch = mc_vas_find_channel(&ctx->vas[MC_VAS_UVM],
                                                 MC_ROLE_UVM_CE);
  struct mc_uvm_extras *ex;
  if (ch == NULL) return;
  ex = &ch->x.uvm;

  /* Drain + disable first, while UVM mappings + RM handles are still
   * intact.  No other GPU work can wake this channel afterwards. */
  if (ch->userd_cpu != NULL)
    drain_channel(ch, MC_TIMEOUT_MS);

  if (ch->h_channel && ctx->ctl_fd >= 0)
    rm_channel_disable(ctx->ctl_fd, ctx->h_client, ch->h_channel);
  if (ch->h_channel && ctx->uvm_fd >= 0)
    uvm_unregister_channel(ctx->uvm_fd, ctx->h_client, ch->h_channel);

  /* CPU-side mappings (BAR1 for gpfifo_userd; sysmem for pushbuffer and sema). */
  if (ch->gpfifo_ring) munmap((void *)ch->gpfifo_ring, MC_GPFIFO_USERD_SIZE);
  if (ch->sema_ptr)    munmap((void *)ch->sema_ptr,    MC_SEMA_SIZE);
  if (ch->pb_cpu)      munmap(ch->pb_cpu,              MC_PB_SIZE);

  /* UVM unmaps for gpfifo_userd / pb / sema, then unregister VA space + GPU. */
  if (ctx->uvm_fd >= 0)
  {
    if (ch->pb_gpu_va)
      uvm_unmap_buffer(ctx->uvm_fd, ctx->gpu_inst_uuid, ch->pb_gpu_va,
                       MC_PB_SIZE, "pb");
    if (ch->gpfifo_gpu_va)
      uvm_unmap_buffer(ctx->uvm_fd, ctx->gpu_inst_uuid, ch->gpfifo_gpu_va,
                       MC_GPFIFO_USERD_SIZE, "gpfifo_userd");
    if (ch->sema_gpu_va)
      uvm_unmap_buffer(ctx->uvm_fd, ctx->gpu_inst_uuid, ch->sema_gpu_va,
                       MC_SEMA_SIZE, "sema");
    uvm_unregister_gpu_vaspace(ctx->uvm_fd, ctx->gpu_inst_uuid);
    uvm_unregister_gpu(ctx->uvm_fd, ctx->gpu_inst_uuid);
  }

  /* RM handle frees (reverse of allocation). */
  if (ctx->ctl_fd >= 0 && ctx->h_client)
  {
    if (ch->h_engine)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ch->h_channel,
                     ch->h_engine, "h_ce");
    if (ch->h_channel)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ch->h_tsg,
                     ch->h_channel, "h_channel");
    if (ch->h_tsg)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ch->h_tsg, "h_tsg");
    if (ch->h_sema_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ch->h_sema_mem, "h_sema_mem");
    if (ex->h_gpfifo_userd_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ex->h_gpfifo_userd_mem, "h_gpfifo_userd_mem");
    if (ch->h_pb_mem)
      rm_free_handle(ctx->ctl_fd, ctx->h_client, ctx->h_device,
                     ch->h_pb_mem, "h_pb_mem");
  }

  memset(ch, 0, sizeof(*ch));
}
