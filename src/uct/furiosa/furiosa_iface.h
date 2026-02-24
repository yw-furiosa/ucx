/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_FURIOSA_IFACE_H
#define UCT_FURIOSA_IFACE_H

#include <uct/base/uct_iface.h>

#define UCT_FURIOSA_TL_NAME "furiosa"
typedef uint64_t uct_furiosa_iface_addr_t;
typedef struct uct_furiosa_iface {
    uct_base_iface_t             super;
    uct_furiosa_iface_addr_t     id;
} uct_furiosa_iface_t;

typedef struct uct_furiosa_ep {
    uct_base_ep_t super;
} uct_furiosa_ep_t;

#endif
