/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_FURIOSA_MD_H
#define UCT_FURIOSA_MD_H

#include <uct/base/uct_md.h>
#include <ucs/config/types.h>

extern uct_component_t uct_furiosa_component;

typedef struct uct_furiosa_md {
    uct_md_t         super;
    unsigned         device_id;
} uct_furiosa_md_t;

typedef struct uct_furiosa_md_config {
    uct_md_config_t  super;
    int              device_id;
} uct_furiosa_md_config_t;

#endif
