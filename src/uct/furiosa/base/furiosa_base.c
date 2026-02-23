/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "furiosa_base.h"

#include <ucs/arch/atomic.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/string.h>
#include <ucs/sys/module.h>
#include <ucs/sys/topo/base/topo.h>
#include <uct/furiosa/furiosa_md.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>


int uct_furiosa_base_open_bar4(unsigned device_id)
{
    char path[64];
    int fd;
    ucs_snprintf_safe(path, sizeof(path), UCT_FURIOSA_DEV_DIR "/"
                      UCT_FURIOSA_BAR4_PATTERN, device_id);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ucs_debug("failed to open %s: %m", path);
        return -errno;
    }
    ucs_debug("opened furiosa BAR4 device %s (fd=%d)", path, fd);
    return fd;
}

void uct_furiosa_base_close_fd(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

ucs_status_t uct_furiosa_base_get_bar_info(int bar4_fd,
                                            struct npu_bar_info *info)
{
    int ret;

    memset(info, 0, sizeof(*info));
    ret = ioctl(bar4_fd, NPU_BAR_GET_INFO, info);
    if (ret < 0) {
        ucs_error("ioctl(NPU_BAR_GET_INFO) failed: %m");
        return UCS_ERR_IO_ERROR;
    }

    ucs_debug("BAR4 info: phys=0x%" PRIx64 " size=0x%" PRIx64,
              info->bar_phy_addr, info->bar_size);
    return UCS_OK;
}

ucs_status_t uct_furiosa_base_export_dmabuf(int bar4_fd, uint64_t offset,
                                             uint64_t size, int *dmabuf_fd)
{
    struct npu_dmabuf_region region;
    int ret;

    region.offset = offset;
    region.size   = size;
    region.fd     = -1;

    ret = ioctl(bar4_fd, NPU_BAR_EXPORT_DMABUF, &region);
    if (ret < 0) {
        ucs_error("ioctl(NPU_BAR_EXPORT_DMABUF) failed: offset=0x%" PRIx64
                  " size=0x%" PRIx64 ": %m", offset, size);
        return UCS_ERR_IO_ERROR;
    }

    *dmabuf_fd = region.fd;
    ucs_debug("exported dmabuf: offset=0x%" PRIx64 " size=0x%" PRIx64
              " fd=%d", offset, size, region.fd);
    return UCS_OK;
}

void uct_furiosa_base_close_dmabuf_fd(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

ucs_status_t uct_furiosa_base_mmap_dmabuf(int dmabuf_fd, uint64_t size,
                                           void **addr)
{
    void *ptr;

    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (ptr == MAP_FAILED) {
        ucs_error("mmap(dmabuf_fd=%d, size=0x%" PRIx64 ") failed: %m",
                  dmabuf_fd, size);
        return UCS_ERR_IO_ERROR;
    }

    *addr = ptr;
    ucs_debug("mmaped dmabuf: fd=%d size=0x%" PRIx64 " addr=%p",
              dmabuf_fd, size, ptr);
    return UCS_OK;
}


ucs_status_t uct_furiosa_base_mmap_bar4(int bar4_fd, uint64_t offset,
                                         uint64_t size, void **addr)
{
    void *ptr;

    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, bar4_fd,
              (off_t)offset);
    if (ptr == MAP_FAILED) {
        ucs_error("mmap(bar4_fd=%d, offset=0x%" PRIx64 ", size=0x%" PRIx64
                  ") failed: %m", bar4_fd, offset, size);
        return UCS_ERR_IO_ERROR;
    }

    *addr = ptr;
    ucs_debug("mmaped BAR4 direct: fd=%d offset=0x%" PRIx64 " size=0x%" PRIx64
              " addr=%p", bar4_fd, offset, size, ptr);
    return UCS_OK;
}

void uct_furiosa_base_munmap(void *addr, uint64_t size)
{
    if (addr != NULL && addr != MAP_FAILED) {
        munmap(addr, size);
    }
}

/**
 * Scan /dev/rngd/ for npu*bar4 device files to discover available NPUs.
 */
ucs_status_t uct_furiosa_base_discover_devices(void)
{
    static pthread_mutex_t discovery_mutex = PTHREAD_MUTEX_INITIALIZER;
    static uint32_t discovery_done         = 0;

    ucs_status_t status = UCS_OK;
    unsigned discovered  = 0;
    unsigned dev_id;
    struct npu_bar_info info;
    int fd;

    if (ucs_atomic_fadd32(&discovery_done, 0)) {
        return UCS_OK;
    }

    pthread_mutex_lock(&discovery_mutex);

    if (ucs_atomic_fadd32(&discovery_done, 0)) {
        goto out;
    }

    ucs_debug("starting furiosa NPU device discovery");

    for (dev_id = 0; dev_id < UCT_FURIOSA_MAX_DEVICES; dev_id++) {
        fd = uct_furiosa_base_open_bar4(dev_id);
        if (fd < 0) {
            continue;
        }

        status = uct_furiosa_base_get_bar_info(fd, &info);
        close(fd);

        if (status != UCS_OK) {
            ucs_debug("furiosa npu%u: BAR4 query failed, skipping", dev_id);
            continue;
        }

        ucs_debug("furiosa npu%u: BAR4 phys=0x%" PRIx64 " size=0x%" PRIx64,
                  dev_id, info.bar_phy_addr, info.bar_size);
        discovered++;
    }

    if (discovered > 0) {
        ucs_debug("discovered %u furiosa NPU device(s)", discovered);
        status = UCS_OK;
        ucs_atomic_add32(&discovery_done, 1);
    } else {
        ucs_debug("no furiosa NPU devices found");
        status = UCS_ERR_NO_DEVICE;
    }

out:
    pthread_mutex_unlock(&discovery_mutex);
    return status;
}

ucs_status_t
uct_furiosa_base_query_devices(uct_md_h md,
                                uct_tl_device_resource_t **tl_devices_p,
                                unsigned *num_tl_devices_p)
{
    return uct_single_device_resource(md, md->component->name,
                                      UCT_DEVICE_TYPE_ACC,
                                      UCS_SYS_DEVICE_ID_UNKNOWN,
                                      tl_devices_p, num_tl_devices_p);
}

UCS_MODULE_INIT()
{
    return UCS_OK;
}
