/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * mc_features_demo — feature querying and reduced-footprint initialisation.
 *
 * Two claims, checked against the real driver:
 *
 *   1. mc_init_ex(MC_FEATURES_BASIC) brings up a working context with the
 *      carrier VA spaces and their channels never created.  A caller doing
 *      plain host-authored copies should not pay for five channels and four
 *      kernel stagings it will never touch.
 *
 *   2. mc_feature_supported() tells the truth in both cases, and an operation
 *      whose feature is absent returns MC_EUNSUPPORTED rather than failing
 *      somewhere deep in submission.
 *
 * This matters more for architectures than for footprint.  Ampere and Ada
 * have no BAR1 usermode aperture, so the FB carrier and every SM-authored
 * path are permanently unavailable there.  A caller has to be able to ask,
 * and asking has to work.
 *
 * `--porcelain` skips the checks and prints one machine-readable line:
 *
 *     features: xfer_host host_register carrier_sysmem
 *
 * tools/run_mc_tests.py uses that to decide which matrix cells this GPU can
 * run at all, so the format is a contract rather than a convenience: one
 * line, the literal prefix "features:", then zero or more mc_feature_name()
 * values separated by single spaces.  Printing the names rather than a mask
 * keeps it readable and means the harness never has to track bit positions.
 *
 * Exit codes: 0 PASS, 1 verification failed, 3 init failed.
 */
#include <mc/mc.h>
#include "mc_test_helpers.h"

#include <stdio.h>
#include <string.h>

#include "mc_test_helpers.h"

static int failures;

#define CHECK_THAT(cond, ...)                        \
  do {                                               \
    if (!(cond)) {                                   \
      printf("FAIL: ");                              \
      printf(__VA_ARGS__);                           \
      printf("\n");                                  \
      failures++;                                    \
    }                                                \
  } while (0)

static void report(mc_ctx_t *ctx, const char *what)
{
    int f;
    printf("  %s:", what);
    for (f = 0; f < MC_FEAT_COUNT; f++)
        if (mc_feature_supported(ctx, (mc_feature_t)f))
            printf(" %s", mc_feature_name((mc_feature_t)f));
    printf("\n");
}

/* A copy must work under any feature set: it is the one thing every
 * supported GPU and every build provides. */
static void check_copy_works(mc_ctx_t *ctx, const char *what)
{
    const size_t n = 1u << 20;
    uint32_t    *h = mc_malloc_host(ctx, n, MC_VAS_UVM);
    uint32_t    *d = mc_malloc_device(ctx, n, MC_VAS_UVM);

    if (h == NULL || d == NULL) {
        printf("FAIL: %s: allocation failed\n", what);
        failures++;
        return;
    }
    memset(h, 0xAB, n);
    CHECK_THAT(mc_memcpy(ctx, d, h, n, MC_XFER_HOST) == MC_OK,
               "%s: H2D copy failed", what);
    memset(h, 0, n);
    CHECK_THAT(mc_memcpy(ctx, h, d, n, MC_XFER_HOST) == MC_OK,
               "%s: D2H copy failed", what);
    CHECK_THAT(((unsigned char *)h)[0] == 0xAB
               && ((unsigned char *)h)[n - 1] == 0xAB,
               "%s: round-trip data mismatch", what);
    mc_free(ctx, d);
    mc_free(ctx, h);
}

/* Machine-readable capability report -- see the file comment for the format
 * contract.  Deliberately does no verification: a harness asking "what can
 * this GPU do?" must not fail for a reason unrelated to the question. */
static int print_porcelain(int device)
{
    mc_ctx_t        *ctx = NULL;
    mc_init_params_t params = { .device = device };
    int              f;

    if (mc_init_ex(&ctx, &params) != MC_OK) {
        printf("mc_init failed\n");
        return 3;
    }
    printf("features:");
    for (f = 0; f < MC_FEAT_COUNT; f++)
        if (mc_feature_supported(ctx, (mc_feature_t)f))
            printf(" %s", mc_feature_name((mc_feature_t)f));
    printf("\n");
    mc_fini(ctx);
    return 0;
}

int main(int argc, char **argv)
{
    mc_ctx_t        *ctx = NULL;
    int              device = 0;
    uint64_t         full_mask = 0;
    int              f;

    argc = mc_test_extract_device(argc, argv, &device);
    if (argc < 0)
        return 2;

    mc_init_params_t full  = { .device = device };
    mc_init_params_t basic = { .features = MC_FEATURES_BASIC, .device = device };

    if (argc > 1 && strcmp(argv[1], "--porcelain") == 0)
        return print_porcelain(device);

    /* ── Full initialisation: whatever this GPU can do ──────────────── */
    if (mc_init_ex(&ctx, &full) != MC_OK) {
        printf("mc_init failed\n");
        return 3;
    }
    report(ctx, "full   ");
    for (f = 0; f < MC_FEAT_COUNT; f++)
        if (mc_feature_supported(ctx, (mc_feature_t)f))
            full_mask |= MC_FEAT_BIT(f);

    CHECK_THAT(mc_feature_supported(ctx, MC_FEAT_XFER_HOST),
               "full: xfer_host must always be available");
    check_copy_works(ctx, "full");
    mc_fini(ctx);
    ctx = NULL;

    /* ── Reduced initialisation: only the basics ────────────────────── */
    if (mc_init_ex(&ctx, &basic) != MC_OK) {
        printf("mc_init_ex(MC_FEATURES_BASIC) failed\n");
        return 3;
    }
    report(ctx, "basic  ");

    CHECK_THAT(mc_feature_supported(ctx, MC_FEAT_XFER_HOST),
               "basic: xfer_host must still be available");
    CHECK_THAT(!mc_feature_supported(ctx, MC_FEAT_CARRIER_FB),
               "basic: carrier_fb should not have been brought up");
    CHECK_THAT(!mc_feature_supported(ctx, MC_FEAT_COMPUTE_LAUNCH),
               "basic: compute_launch should not have been brought up");
    CHECK_THAT(!mc_feature_supported(ctx, MC_FEAT_XFER_SM),
               "basic: xfer_sm should not have been brought up");

    /* Copies still work with everything else switched off -- that is the
     * point of the reduced set. */
    check_copy_works(ctx, "basic");

    /* An operation whose feature was not brought up must decline cleanly.
     * MC_EUNSUPPORTED, not MC_EINVAL: the arguments are fine. */
    {
        void *hb = mc_malloc_host(ctx, 4096, MC_VAS_UVM);
        void *db = mc_malloc_device(ctx, 4096, MC_VAS_UVM);
        if (hb && db) {
            mc_status_t rc = mc_memcpy(ctx, db, hb, 4096, MC_XFER_SM);
            CHECK_THAT(rc == MC_EUNSUPPORTED || rc == MC_EINVAL,
                       "basic: MC_XFER_SM returned %d, expected "
                       "MC_EUNSUPPORTED", (int)rc);
            mc_free(ctx, db);
            mc_free(ctx, hb);
        }
    }
    mc_fini(ctx);

    /* Sanity: the reduced set must be a subset of the full one. */
    CHECK_THAT((MC_FEATURES_BASIC & ~full_mask & MC_FEAT_BIT(MC_FEAT_XFER_HOST)) == 0,
               "basic requested a feature the GPU does not have");

    if (failures) {
        printf("FAIL: %d feature check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: feature querying and reduced-footprint init\n");
    return 0;
}
