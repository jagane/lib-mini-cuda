/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
 * SPDX-License-Identifier: MIT
 *
 * internal/rm.h — RM ioctl wrappers implemented by driver/rm.c.
 *
 * Split out of the former mc_internal.h.  Library-internal: none of these
 * are exported from libmc.so (see mc/libmc.map).
 */
#ifndef MC_INTERNAL_RM_H
#define MC_INTERNAL_RM_H

#include <stdbool.h>
#include <stddef.h>

#include "nvtypes.h"
#include "hal/arch.h"

typedef enum {
  MC_RM_VERSION_ERROR    = -1,
  MC_RM_VERSION_OK       = 0,
  MC_RM_VERSION_MISMATCH = 1,
} mc_rm_version_result_t;

mc_rm_version_result_t rm_check_driver_version(int ctl_fd,
                                                const char *expected,
                                                char *actual,
                                                size_t actual_size);
void     rm_free_handle(int ctl_fd, NvHandle h_root, NvHandle h_parent,
                        NvHandle h_target, const char *label);
NvHandle rm_alloc(int ctl_fd, NvHandle root, NvHandle parent,
                  NvU32 hclass, void *alloc_params);
NvHandle rm_alloc_vidmem(int ctl_fd, NvHandle root, NvHandle device,
                         NvU64 size, NvU64 *out_phys);
int      rm_register_client_fd(int ctl_fd, int dev_fd);
/* Cached (write-back) host memory — the default, and what every buffer a
 * user program reads should use. */
NvHandle rm_alloc_sysmem_at(int ctl_fd, const char *dev_path, NvHandle root,
                            NvHandle device, NvU64 size, void *want_va,
                            void **out_cpu_va);
/* Write-combined host memory — control plane only (pushbuffer, GPFIFO,
 * USERD, semaphores, QMD/CB0/SASS).  Host-write, GPU-read; do not use for
 * anything the host reads back. */
NvHandle rm_alloc_sysmem_wc_at(int ctl_fd, const char *dev_path, NvHandle root,
                               NvHandle device, NvU64 size, void *want_va,
                               void **out_cpu_va);
NvHandle rm_register_user_memory(int ctl_fd, const char *dev_path, NvHandle root,
                                 NvHandle device, void *page_base,
                                 NvU64 page_covered_size);
/* Allocate a vidmem region and ALSO produce a host-visible BAR1
 * alias for it via NV04_MAP_MEMORY with NVOS33_FLAGS_MAPPING_REFLECTED.
 * Result: PBDMA / SM see the FB allocation through the GPU MMU; the
 * host gets `*out_cpu_va` pointing at the same bytes through BAR1.
 *
 * `want_va`, if non-NULL, anchors the BAR1 mmap into the Paper-F1 VA
 * pool (caller pre-reserves via va_pool_reserve).  Pass NULL to let
 * the kernel choose the CPU VA (rare; FB-carrier callers always
 * anchor).  Returns the new vidmem hMemory on success, 0 on failure
 * (no allocation leaks; the helper rolls back on partial failure). */
NvHandle rm_alloc_vidmem_bar1_at(int ctl_fd, const char *dev_path,
                                 NvHandle client, NvHandle device,
                                 NvU64 size, void *want_va,
                                 void **out_cpu_va);
NvHandle rm_alloc_vaspace(int ctl_fd, const mc_arch_desc_t *arch,
                          NvHandle root, NvHandle device);
NvHandle rm_alloc_vaspace_dma(int ctl_fd, const mc_arch_desc_t *arch,
                              NvHandle root, NvHandle device);
NvHandle rm_alloc_virtual_memory(int ctl_fd, NvHandle root, NvHandle device,
                                 NvHandle h_vaspace, NvU64 size,
                                 NvU64 *out_base);
NvU64    rm_map_memory_dma(int ctl_fd, NvHandle client, NvHandle device,
                           NvHandle h_virt, NvHandle h_memory,
                           NvU64 mem_offset, NvU64 size, NvU64 want_offset);
void     rm_unmap_memory_dma(int ctl_fd, NvHandle client, NvHandle device,
                             NvHandle h_virt, NvHandle h_memory,
                             NvU64 dma_offset);
void    *rm_map_memory_at(int ctl_fd, const char *dev_path, NvHandle client,
                          NvHandle parent, NvHandle h_memory,
                          NvU64 mem_offset, NvU64 size, NvU32 flags,
                          void *want_cpu_va);
void    *rm_map_memory(int ctl_fd, const char *dev_path,
                              NvHandle client, NvHandle parent,
                              NvHandle h_memory, NvU64 mem_offset,
                              NvU64 size, NvU32 flags);
int      rm_control(int ctl_fd, NvHandle client, NvHandle object, NvU32 cmd,
                    void *params, NvU32 params_size);
NvU32    pick_non_grce_lce(int ctl_fd, NvHandle h_client, NvHandle h_subdevice);

/* NV2080_CTRL_CMD_MC_GET_ARCH_INFO on a subdevice.  Returns 0 on success and
 * fills the out-params; non-zero on failure with them left untouched.
 * `implementation` is only meaningful together with `architecture` -- the
 * values are namespaced per architecture. */
int      rm_get_arch_info(int ctl_fd, NvHandle h_client, NvHandle h_subdevice,
                          NvU32 *out_architecture, NvU32 *out_implementation,
                          NvU32 *out_revision);

/* NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2 on a DEVICE (not a subdevice).
 * Writes at most `max_classes` entries and stores the count in *out_count.
 * V2 is used deliberately: the V1 form embeds an NvP64 pointer that would
 * need marshalling, while V2 is a flat struct that round-trips through the
 * ordinary control path. */
int      rm_get_classlist(int ctl_fd, NvHandle h_client, NvHandle h_device,
                          NvU32 *out_classes, NvU32 max_classes,
                          NvU32 *out_count);
void     rm_perf_boost(int ctl_fd, NvHandle client, NvHandle subdevice);

NvHandle rm_alloc_root(int ctl_fd);
NvHandle rm_alloc_device(int ctl_fd, NvHandle root, NvU32 device_id);
NvHandle rm_alloc_subdevice(int ctl_fd, NvHandle root, NvHandle device);
NvHandle rm_alloc_tsg(int ctl_fd, const mc_arch_desc_t *arch,
                      NvHandle root, NvHandle device,
                      NvU32 engine_type, NvHandle vaspace);
NvHandle rm_alloc_channel(int ctl_fd, const mc_arch_desc_t *arch,
                          NvHandle root, NvHandle tsg,
                          NvU64 gp_fifo_offset, NvU32 gp_fifo_entries,
                          NvU32 engine_type, NvHandle h_userd,
                          NvU64 userd_offset);
NvHandle rm_alloc_ce(int ctl_fd, const mc_arch_desc_t *arch,
                     NvHandle root, NvHandle channel, NvU32 engine_type);
NvHandle rm_alloc_compute(int ctl_fd, const mc_arch_desc_t *arch,
                          NvHandle root, NvHandle channel);
int      rm_gpfifo_schedule(int ctl_fd, NvHandle h_client, NvHandle h_channel);
void     rm_channel_disable(int ctl_fd, NvHandle h_client, NvHandle h_channel);

#endif /* MC_INTERNAL_RM_H */
