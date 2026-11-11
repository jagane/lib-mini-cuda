/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 The libmc authors
 * SPDX-License-Identifier: MIT
 *
 * Host-only test of the GPFIFO channel allocation parameters across driver
 * releases.  NV_CHANNEL_ALLOC_PARAMS differs between releases (610.43.02 has
 * hHandleVASpace, 595.99.02 does not); rm_alloc_channel() sets the fields it
 * means to and must leave every other byte zero, in particular both VA-space
 * fields, because RM rejects channel-level VA spaces on TSG-parented channels.
 * NV_ESC_RM_ALLOC is wrapped, so neither a GPU nor RM is required.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "nvos.h"
#include "nv_escape.h"
#include "alloc/alloc_channel.h"

#include "hal/arch.h"
#include "internal/rm.h"

static int failures;
static int ioctl_calls;
static NVOS64_PARAMETERS       seen_alloc;
static NV_CHANNEL_ALLOC_PARAMS seen_params;

#define EXPECT(cond)                                                  \
  do                                                                  \
  {                                                                   \
    if (!(cond))                                                      \
    {                                                                 \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                     \
    }                                                                 \
  } while (0)

/* The probe result must agree with the headers this test compiles against. */
#if MC_NV_HAVE_CHANNEL_HANDLE_VASPACE
_Static_assert(offsetof(NV_CHANNEL_ALLOC_PARAMS, hHandleVASpace) >
               offsetof(NV_CHANNEL_ALLOC_PARAMS, hVASpace),
               "probe says hHandleVASpace exists");
#endif

int __wrap_ioctl(int fd, unsigned long request, ...)
{
  NVOS64_PARAMETERS *p;
  va_list            args;

  (void)fd;
  ioctl_calls++;
  EXPECT(_IOC_NR(request) == (NV_ESC_RM_ALLOC & 0xff));
  EXPECT(_IOC_SIZE(request) == sizeof(*p));

  va_start(args, request);
  p = va_arg(args, NVOS64_PARAMETERS *);
  va_end(args);
  EXPECT(p != NULL);
  if (p == NULL) return -1;

  seen_alloc = *p;
  memcpy(&seen_params, (const void *)(uintptr_t)p->pAllocParms,
         sizeof(seen_params));
  p->status = NV_OK;
  return 0;
}

static void test_channel_params(void)
{
  mc_arch_desc_t          arch = {0};
  NV_CHANNEL_ALLOC_PARAMS expected;
  NvHandle                h;

  arch.cls.channel_gpfifo = 0xc86f;
  h = rm_alloc_channel(7, &arch, 0x1000, 0x2000, 0x7f0000001000ULL, 512,
                       3, 0x3000, 0x8000ULL);
  EXPECT(h != 0);
  EXPECT(ioctl_calls == 1);
  EXPECT(seen_alloc.hRoot == 0x1000);
  EXPECT(seen_alloc.hObjectParent == 0x2000);
  EXPECT(seen_alloc.hClass == 0xc86f);

  memset(&expected, 0, sizeof(expected));
  expected.gpFifoOffset    = 0x7f0000001000ULL;
  expected.gpFifoEntries   = 512;
  expected.engineType      = 3;
  expected.hUserdMemory[0] = 0x3000;
  expected.userdOffset[0]  = 0x8000ULL;

  EXPECT(seen_params.hVASpace == 0);
#if MC_NV_HAVE_CHANNEL_HANDLE_VASPACE
  EXPECT(seen_params.hHandleVASpace == 0);
#endif
  /* Byte-exact: no field this release adds may carry stack garbage. */
  EXPECT(memcmp(&seen_params, &expected, sizeof(expected)) == 0);
  if (!failures) printf("PASS: channel alloc params (NVIDIA %s)\n",
                        MC_NVIDIA_DRIVER_VERSION);
}

int main(void)
{
  test_channel_params();
  return failures ? 1 : 0;
}
