/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc_carrier_host_demo.c — host-authored transfers over a carrier VA space.
 *
 * Why this exists separately from mc_carrier_demo
 * -----------------------------------------------
 * mc_carrier_demo's subject is the SM-authored submission path: its
 * MC_XFER_HOST calls only seed and verify, and without MC_FEAT_XFER_SM there
 * is nothing left of it to run.  That feature needs a BAR1 usermode aperture,
 * which Ampere and Ada do not have on any driver.  The consequence was a gap:
 * those architectures brought up a sysmem carrier successfully and no test
 * ever moved a byte through it.
 *
 * That gap mattered more than it looked.  The carrier channel is exactly the
 * one whose USERD placement was wrong on Ampere -- the runlist entry holds
 * only 8 bits of the USERD physical address high half on GA100 against 20 on
 * GH100, so a sysmem USERD landed above 2^40 and RM rejected the channel with
 * NV_ERR_INVALID_ADDRESS.  The fix moves USERD to vidmem there.  Until this
 * demo existed, the evidence for that fix was "mc_init stops failing", which
 * shows a channel can be *created*, not that it can carry traffic.
 *
 * So: allocate both buffers in a carrier VA space and round-trip real data
 * through its host-authored CE channel.  MC_XFER_HOST only, no compute, no
 * doorbell -- so it runs anywhere a carrier exists, which is every supported
 * architecture.
 *
 * Exercises:
 *   - mc_malloc_device(ctx, n, carrier)   HBM inside the carrier VAS
 *   - mc_malloc_host  (ctx, n, carrier)   sysmem inside the carrier VAS
 *   - mc_memcpy(MC_XFER_HOST) dispatched by VAS to the carrier's DMA channel,
 *     in both directions, over the channel whose GPFIFO+USERD placement is
 *     architecture-dependent
 *   - mc_gpu_va recovering a carrier GPU VA from a CPU pointer
 *   - mc_free / mc_fini teardown of carrier-VAS allocations
 *
 * `--fb` selects MC_VAS_FB_CARRIER instead, which needs MC_FEAT_CARRIER_FB and
 * so is Hopper-only today; the demo declines cleanly elsewhere.
 *
 * Result interpretation:
 *
 *   PASS    -> the pattern survived host -> HBM -> host through the carrier
 *              channel.  Both directions of the VAS-dispatched mc_memcpy work
 *              and teardown is clean.
 *   TIMEOUT -> the CE submission was published but never retired.  On a new
 *              architecture suspect channel-resource placement before
 *              suspecting the method stream.
 *   VERIFY  -> bytes moved but are wrong: usually GPU-VA bookkeeping in the
 *              carrier's per-resource mapping.
 *
 * Build: `make mc_carrier_host_demo` from the repository root.
 * Run:   sudo ./bin/mc_carrier_host_demo [--size N] [--iters M] [--h2d]
 *                                        [--sysmem | --fb]
 *
 * Exit codes: 0 = PASS or feature absent, 1 = init/alloc failure,
 *             2 = mc_memcpy failure, 4 = verify mismatch.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mc/mc.h>
#include "mc_test_helpers.h"

#define DEFAULT_SIZE (1ULL * 1024ULL * 1024ULL) /* 1 MiB */
#define FILL_PATTERN 0x5EEDBEEFu

int main(int argc, char **argv)
{
  mc_test_args_t args = { .n_bytes = DEFAULT_SIZE, .iters = 1 };

  mc_ctx_t   *ctx      = NULL;
  void       *d_buf    = NULL;   /* HBM, in the carrier VAS    */
  void       *h_buf    = NULL;   /* sysmem source, carrier VAS */
  void       *h_verify = NULL;   /* sysmem readback target     */
  uint32_t    n;
  int         it;
  size_t      bad_off = 0;
  int         rc      = 0;

  mc_vas_t     carrier      = MC_VAS_SYSMEM_CARRIER;
  mc_feature_t need         = MC_FEAT_CARRIER_SYSMEM;
  const char  *carrier_name = "sysmem-carrier";
  int          read_i = 1, write_i = 1;
  const char  *dir_label;
  uint64_t     best_ns = 0;

  /* Pre-scan argv for --fb / --sysmem before the generic parser, which does
   * not know them and rejects unknown flags.  Compact argv in place so it
   * sees only what it owns.  Same shape as mc_carrier_demo on purpose: the
   * two demos differ in what they exercise, not in how they are driven. */
  while (read_i < argc)
  {
    if (strcmp(argv[read_i], "--fb") == 0)
    {
      carrier = MC_VAS_FB_CARRIER;  need = MC_FEAT_CARRIER_FB;
      carrier_name = "fb-carrier";  read_i++;
    }
    else if (strcmp(argv[read_i], "--sysmem") == 0)
    {
      carrier = MC_VAS_SYSMEM_CARRIER;  need = MC_FEAT_CARRIER_SYSMEM;
      carrier_name = "sysmem-carrier";  read_i++;
    }
    else
    {
      argv[write_i++] = argv[read_i++];
    }
  }
  argc = write_i;

  switch (mc_test_parse_args(argc, argv, &args))
  {
  case MC_TEST_ARGS_OK:
    break;
  case MC_TEST_ARGS_HELP:
    printf("\n  --sysmem      Use MC_VAS_SYSMEM_CARRIER (default)\n");
    printf("  --fb          Use MC_VAS_FB_CARRIER (needs a BAR1 usermode\n");
    printf("                aperture; Hopper-only today)\n");
    return 0;
  case MC_TEST_ARGS_ERROR:
    return 1;
  }

  if (args.n_bytes > 0x7FFFFFFFu)
  {
    fprintf(stderr, "size too large: %zu\n", args.n_bytes);
    return 1;
  }
  n = (uint32_t)args.n_bytes;
  dir_label = args.h2d ? "H2D" : "D2H";

  mc_init_params_t params = { .device = args.device };
  if (mc_init_ex(&ctx, &params) != MC_OK)
  {
    fprintf(stderr, "mc_init failed\n");
    return 1;
  }

  /* Decline before allocating anything.  The whole demo is about one VA
   * space; if this GPU has no such carrier there is nothing to test, which
   * is not a failure.  The wording is deliberate -- tools/run_mc_tests.py
   * treats "unsupported" anywhere in the output as a whole-cell skip, and
   * that is exactly right here. */
  if (!mc_feature_supported(ctx, need))
  {
    fprintf(stderr, "unsupported: this GPU does not provide %s\n",
            mc_feature_name(need));
    mc_fini(ctx);
    return 0;
  }

  printf("mc_init ok — host-authored transfers over %s "
         "(size=%u, iters=%d, dir=%s)\n",
         carrier_name, n, args.iters, dir_label);

  d_buf    = mc_malloc_device(ctx, n, carrier);
  h_buf    = mc_malloc_host  (ctx, n, carrier);
  h_verify = mc_malloc_host  (ctx, n, carrier);
  if (d_buf == NULL || h_buf == NULL || h_verify == NULL)
  {
    fprintf(stderr, "carrier alloc failed (d=%p h=%p verify=%p)\n",
            d_buf, h_buf, h_verify);
    rc = 1;
    goto out;
  }

  printf("  d_buf (HBM, %s):    gpu_va=0x%llx\n", carrier_name,
         (unsigned long long)mc_gpu_va(ctx, d_buf));
  printf("  h_buf (sysmem, %s): cpu_va=%p gpu_va=0x%llx\n", carrier_name,
         h_buf, (unsigned long long)mc_gpu_va(ctx, h_buf));

  /* Prove the buffers really are carrier-resident before claiming the copy
   * exercised the carrier's channel.  A carrier VAS has RM pick the GPU VA,
   * so it differs from the CPU pointer; MC_VAS_UVM is the opposite -- the
   * Paper-F1 invariant makes them equal.  Without this the demo would still
   * pass if the allocator silently fell back to UVM, and would then be
   * testing the same path mc_demo already covers. */
  {
    uint64_t h_gpu = mc_gpu_va(ctx, h_buf);
    if (h_gpu == 0 || mc_gpu_va(ctx, d_buf) == 0)
    {
      fprintf(stderr, "carrier alloc has no GPU VA\n");
      rc = 1;
      goto out;
    }
    if (h_gpu == (uint64_t)(uintptr_t)h_buf)
    {
      fprintf(stderr,
              "host buffer's GPU VA equals its CPU VA (0x%llx) — that is the "
              "UVM identity mapping, not a carrier.  This demo would not be "
              "testing the carrier channel.\n",
              (unsigned long long)h_gpu);
      rc = 1;
      goto out;
    }
  }

  for (it = 0; it < args.iters; it++)
  {
    uint64_t t0, ns;

    /* Fresh pattern per iteration so a stale buffer cannot pass by luck. */
    mc_test_seed_pattern((uint32_t *)h_buf, n / 4u, FILL_PATTERN + it);
    memset(h_verify, 0, n);

    /* H2D: host sysmem -> HBM, both carrier-resident. */
    t0 = mc_test_now_ns();
    if (mc_memcpy(ctx, d_buf, h_buf, n, MC_XFER_HOST) != MC_OK)
    {
      fprintf(stderr, "iter %d: H2D mc_memcpy failed\n", it + 1);
      rc = 2;
      goto out;
    }
    ns = mc_test_now_ns() - t0;
    if (args.h2d && (best_ns == 0 || ns < best_ns)) best_ns = ns;

    /* D2H: back out of HBM into a different sysmem buffer, so the verify
     * cannot be satisfied by the source still holding the pattern. */
    t0 = mc_test_now_ns();
    if (mc_memcpy(ctx, h_verify, d_buf, n, MC_XFER_HOST) != MC_OK)
    {
      fprintf(stderr, "iter %d: D2H mc_memcpy failed\n", it + 1);
      rc = 2;
      goto out;
    }
    ns = mc_test_now_ns() - t0;
    if (!args.h2d && (best_ns == 0 || ns < best_ns)) best_ns = ns;

    if (!mc_test_verify_pattern((const uint32_t *)h_verify, n / 4u,
                                FILL_PATTERN + it, &bad_off))
    {
      fprintf(stderr,
              "iter %d: verify mismatch at dword %zu: got 0x%08x, "
              "expected 0x%08x\n",
              it + 1, bad_off, ((const uint32_t *)h_verify)[bad_off],
              FILL_PATTERN + it);
      rc = 4;
      goto out;
    }
  }

  printf("  %s peak: %.2f GB/s (%.3f ms)\n", dir_label,
         mc_test_bandwidth_gbps(n, best_ns), (double)best_ns / 1e6);
  printf("PASS: host-authored round trip through the %s survived %d "
         "iteration(s);\n"
         "      data moved over the carrier's CE channel, not the UVM one.\n",
         carrier_name, args.iters);

out:
  if (h_verify) mc_free(ctx, h_verify);
  if (h_buf)    mc_free(ctx, h_buf);
  if (d_buf)    mc_free(ctx, d_buf);
  mc_fini(ctx);
  return rc;
}
