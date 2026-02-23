/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_FURIOSA_MD_H
#define UCT_FURIOSA_MD_H

#include <stdbool.h>
#include <stdint.h>
#include <uct/base/uct_md.h>
#include <ucs/config/types.h>

extern uct_component_t uct_furiosa_component;


typedef struct uct_furiosa_md {
    uct_md_t         super;
    int              bar4_fd;
    bool             bar4_fd_owned;
    unsigned         device_id;
    uint64_t         bar_phys_addr;
    uint64_t         bar_size;
    int              dmabuf_fd;
    void            *dmabuf_addr;
    uint64_t         dmabuf_size;
} uct_furiosa_md_t;

/**
 * Memory handle for a registered NPU memory region.
 * For PoC, registration simply validates the address falls within the
 * mmap'd BAR4 range and records the offset/size for later mkey_pack.
 */
typedef struct uct_furiosa_mem {
    uint64_t         bar_offset;    /**< offset from BAR4 base (phys) */
    void            *address;       /**< registered host VA */
    size_t           length;        /**< registered length */
    unsigned         device_id;     /**< NPU device index */
} uct_furiosa_mem_t;

typedef struct uct_furiosa_md_config {
    uct_md_config_t  super;
    int              device_id;
} uct_furiosa_md_config_t;

#endif
