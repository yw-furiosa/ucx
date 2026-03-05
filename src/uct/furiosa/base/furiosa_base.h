/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_FURIOSA_BASE_H_
#define UCT_FURIOSA_BASE_H_

#include <uct/base/uct_iface.h>
#include <uct/base/uct_md.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * NPU BAR4 UAPI definitions from renegade_driver.
 * Copied here to avoid build dependency on the kernel driver source tree.
 * Source: renegade_driver/include/npu_bar_uapi.h
 */
#define NPU_BAR_IOC_MAGIC            'N'

#define NPU_BAR4_RESERVED_SIZE       (256ULL * 1024 * 1024) /* 256MB */
#define NPU_BAR4_AVAILABLE_SIZE      (48ULL * 1024 * 1024 * 1024 \
                                      - (256 * 1024 * 1024) \
                                      - NPU_BAR4_RESERVED_SIZE)

/* PoC: map a small region instead of the full 47.5GB available */
#define UCT_FURIOSA_POC_MAP_SIZE     (256ULL * 1024 * 1024) /* 256MB */

struct npu_bar_info {
    uint64_t bar_phy_addr;
    uint64_t bar_size;
} __attribute__((packed));

struct npu_dmabuf_region {
    uint64_t offset;
    uint64_t size;
    int      fd;
} __attribute__((packed));

#define NPU_BAR_GET_INFO      _IOWR(NPU_BAR_IOC_MAGIC, 0x00, struct npu_bar_info)
#define NPU_BAR_EXPORT_DMABUF _IOWR(NPU_BAR_IOC_MAGIC, 0x01, struct npu_dmabuf_region)

/* Device path pattern: /dev/rngd/npu{id}bar4 */
#define UCT_FURIOSA_DEV_DIR       "/dev/rngd"
#define UCT_FURIOSA_BAR4_PATTERN  "npu%ubar4"
#define UCT_FURIOSA_MAX_DEVICES   8


/**
 * Open the BAR4 device file for a given NPU device index.
 *
 * @param device_id  NPU device index (e.g., 0 for /dev/rngd/npu0bar4)
 * @return           File descriptor on success, negative errno on failure.
 */
int uct_furiosa_base_open_bar4(unsigned device_id);

/**
 * Close a BAR4 device file descriptor.
 */
void uct_furiosa_base_close_fd(int fd);

/**
 * Get BAR4 memory information (physical address and size).
 *
 * @param bar4_fd    File descriptor from uct_furiosa_base_open_bar4().
 * @param info       Output: BAR4 physical address and size.
 * @return           UCS_OK on success.
 */
ucs_status_t uct_furiosa_base_get_bar_info(int bar4_fd,
                                            struct npu_bar_info *info);

/**
 * Export a region of BAR4 memory as a dmabuf file descriptor.
 *
 * @param bar4_fd    BAR4 device fd.
 * @param offset     Offset within BAR4 (must be >= NPU_BAR4_RESERVED_SIZE,
 *                   page-aligned).
 * @param size       Size of the region (page-aligned).
 * @param dmabuf_fd  Output: dmabuf file descriptor.
 * @return           UCS_OK on success.
 */
ucs_status_t uct_furiosa_base_export_dmabuf(int bar4_fd, uint64_t offset,
                                             uint64_t size, int *dmabuf_fd);

/**
 * Close a dmabuf file descriptor.
 */
void uct_furiosa_base_close_dmabuf_fd(int fd);

/**
 * Map a dmabuf fd into userspace.
 *
 * @param dmabuf_fd  dmabuf file descriptor.
 * @param size       Size to map.
 * @param addr       Output: mapped userspace address.
 * @return           UCS_OK on success.
 */
ucs_status_t uct_furiosa_base_mmap_dmabuf(int dmabuf_fd, uint64_t size,
                                           void **addr);


/**
 * Map a region of BAR4 directly from the device file descriptor.
 * This bypasses dmabuf and maps BAR4 MMIO space directly into userspace.
 *
 * @param bar4_fd   BAR4 device file descriptor.
 * @param offset    Offset within BAR4 (should be page-aligned).
 * @param size      Size to map (page-aligned).
 * @param addr      Output: mapped userspace address.
 * @return          UCS_OK on success.
 */
ucs_status_t uct_furiosa_base_mmap_bar4(int bar4_fd, uint64_t offset,
                                         uint64_t size, void **addr);

/**
 * Unmap a previously mapped dmabuf region.
 */
void uct_furiosa_base_munmap(void *addr, uint64_t size);

/**
 * Discover available Furiosa NPU devices by scanning /dev/rngd/npu*bar4.
 * Registers found devices with UCX topology.
 *
 * @return UCS_OK if at least one device found.
 */
ucs_status_t uct_furiosa_base_discover_devices(void);

/**
 * Query UCX transport layer devices for this MD.
 */
ucs_status_t
uct_furiosa_base_query_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p);

#endif /* UCT_FURIOSA_BASE_H_ */
