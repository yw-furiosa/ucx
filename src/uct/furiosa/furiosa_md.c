/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "furiosa_md.h"

#include <ucs/memory/memtype_cache.h>
#include <uct/furiosa/base/furiosa_base.h>
#include <ucs/sys/module.h>
#include <furiosa_mem.h>

#include <inttypes.h>
#include <fcntl.h>


static ucs_config_field_t uct_furiosa_md_config_table[] = {
    {"", "", NULL, ucs_offsetof(uct_furiosa_md_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

    {"DEVICE_ID", "0", "Index of the Furiosa NPU device.",
     ucs_offsetof(uct_furiosa_md_config_t, device_id), UCS_CONFIG_TYPE_INT},

    {NULL}
};

static ucs_status_t uct_furiosa_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    uct_md_base_md_query(attr);
    attr->detect_mem_types = UCS_BIT(UCS_MEMORY_TYPE_RDMA);
    attr->dmabuf_mem_types = UCS_BIT(UCS_MEMORY_TYPE_RDMA);
    return UCS_OK;
}

static void uct_furiosa_md_close(uct_md_h uct_md)
{
    uct_furiosa_md_t *md = ucs_derived_of(uct_md, uct_furiosa_md_t);

    furiosa_mem_fini(md->device_id);
    ucs_free(md);
}

static ucs_status_t
uct_furiosa_md_query_attributes(uct_md_h md, const void *addr, size_t length,
                                ucs_memory_info_t *mem_info, int *dmabuf_fd)
{
    uct_furiosa_md_t *fmd = ucs_derived_of(md, uct_furiosa_md_t);
    int dev_id;
    unsigned resolved_id;
    void *base;
    size_t size;

    if (!furiosa_mem_contains(addr)) {
        return UCS_ERR_OUT_OF_RANGE;
    }

    dev_id      = furiosa_mem_get_device_id(addr);
    resolved_id = dev_id >= 0 ? (unsigned)dev_id : fmd->device_id;
    base        = furiosa_mem_get_base(resolved_id);
    size        = furiosa_mem_get_size(resolved_id);

    *dmabuf_fd             = furiosa_mem_get_dmabuf_fd(resolved_id);
    mem_info->type         = UCS_MEMORY_TYPE_RDMA;
    mem_info->base_address = base;
    mem_info->alloc_length = size;
    mem_info->sys_dev      = UCS_SYS_DEVICE_ID_UNKNOWN;
    return UCS_OK;
}

static ucs_status_t uct_furiosa_md_mem_query(uct_md_h md, const void *addr,
                                              const size_t length,
                                              uct_md_mem_attr_t *mem_attr_p)
{
    int dmabuf_fd = UCT_DMABUF_FD_INVALID;
    ucs_status_t status;
    ucs_memory_info_t mem_info;
    int dup_fd;

    status = uct_furiosa_md_query_attributes(md, addr, length, &mem_info,
                                             &dmabuf_fd);
    if (status != UCS_OK) {
        return status;
    }

    ucs_memtype_cache_update(mem_info.base_address, mem_info.alloc_length,
                             mem_info.type, mem_info.sys_dev);

    if (mem_attr_p->field_mask & UCT_MD_MEM_ATTR_FIELD_MEM_TYPE) {
        mem_attr_p->mem_type = mem_info.type;
    }

    if (mem_attr_p->field_mask & UCT_MD_MEM_ATTR_FIELD_SYS_DEV) {
        mem_attr_p->sys_dev = mem_info.sys_dev;
    }

    if (mem_attr_p->field_mask & UCT_MD_MEM_ATTR_FIELD_BASE_ADDRESS) {
        mem_attr_p->base_address = mem_info.base_address;
    }

    if (mem_attr_p->field_mask & UCT_MD_MEM_ATTR_FIELD_ALLOC_LENGTH) {
        mem_attr_p->alloc_length = mem_info.alloc_length;
    }

    if (mem_attr_p->field_mask & UCT_MD_MEM_ATTR_FIELD_DMABUF_FD) {
        dup_fd = dup(dmabuf_fd);
        if (dup_fd < 0) {
            return UCS_ERR_IO_ERROR;
        }
        mem_attr_p->dmabuf_fd = dup_fd;
    }

    if (mem_attr_p->field_mask & UCT_MD_MEM_ATTR_FIELD_DMABUF_OFFSET) {
        mem_attr_p->dmabuf_offset = furiosa_mem_to_offset(addr);
    }
    return UCS_OK;
}

static ucs_status_t
uct_furiosa_md_detect_memory_type(uct_md_h md, const void *addr, size_t length,
                                  ucs_memory_type_t *mem_type_p)
{
    uct_md_mem_attr_t mem_attr;
    ucs_status_t status;

    mem_attr.field_mask = UCT_MD_MEM_ATTR_FIELD_MEM_TYPE;
    status              = uct_furiosa_md_mem_query(md, addr, length, &mem_attr);
    if (status != UCS_OK) {
        return status;
    }

    *mem_type_p = mem_attr.mem_type;
    return UCS_OK;
}

static uct_md_ops_t uct_furiosa_md_ops = {
    .close              = uct_furiosa_md_close,
    .query              = uct_furiosa_md_query,
    .mem_alloc          = (uct_md_mem_alloc_func_t)ucs_empty_function_return_unsupported,
    .mem_free           = (uct_md_mem_free_func_t)ucs_empty_function_return_unsupported,
    .mem_advise         = (uct_md_mem_advise_func_t)ucs_empty_function_return_unsupported,
    .mem_reg            = (uct_md_mem_reg_func_t)ucs_empty_function_return_unsupported,
    .mem_dereg          = (uct_md_mem_dereg_func_t)ucs_empty_function_return_unsupported,
    .mem_query          = uct_furiosa_md_mem_query,
    .mkey_pack          = (uct_md_mkey_pack_func_t)ucs_empty_function_return_unsupported,
    .mem_attach         = (uct_md_mem_attach_func_t)ucs_empty_function_return_unsupported,
    .detect_memory_type = uct_furiosa_md_detect_memory_type,
};

static ucs_status_t
uct_furiosa_md_open(uct_component_h component, const char *md_name,
                    const uct_md_config_t *md_config, uct_md_h *md_p)
{
    uct_furiosa_md_config_t *config = ucs_derived_of(md_config,
                                                     uct_furiosa_md_config_t);
    uct_furiosa_md_t *md;
    int ret;

    md = ucs_malloc(sizeof(uct_furiosa_md_t), "uct_furiosa_md_t");
    if (md == NULL) {
        ucs_error("failed to allocate memory for uct_furiosa_md_t");
        return UCS_ERR_NO_MEMORY;
    }

    ret = furiosa_mem_init(config->device_id);
    if (ret < 0) {
        ucs_error("furiosa_mem_init(npu%d) failed: %d", config->device_id, ret);
        ucs_free(md);
        return UCS_ERR_NO_DEVICE;
    }

    md->device_id       = config->device_id;
    md->super.ops       = &uct_furiosa_md_ops;
    md->super.component = &uct_furiosa_component;

    *md_p = (uct_md_h)md;
    ucs_debug("opened furiosa MD: npu%u base=%p size=%zu (via libfuriosa_mem)",
              md->device_id,
              furiosa_mem_get_base(md->device_id),
              furiosa_mem_get_size(md->device_id));
    return UCS_OK;
}

static ucs_status_t
uct_furiosa_query_md_resources(uct_component_h component,
                               uct_md_resource_desc_t **resources_p,
                               unsigned *num_resources_p)
{
    ucs_status_t status;

    status = uct_furiosa_base_discover_devices();
    if (status != UCS_OK) {
        ucs_debug("furiosa device discovery failed, no devices available");
        return uct_md_query_empty_md_resource(resources_p, num_resources_p);
    }

    return uct_md_query_single_md_resource(component, resources_p,
                                           num_resources_p);
}

uct_component_t uct_furiosa_component = {
    .query_md_resources = uct_furiosa_query_md_resources,
    .md_open            = uct_furiosa_md_open,
    .cm_open            = (uct_component_cm_open_func_t)
                          ucs_empty_function_return_unsupported,
    .rkey_unpack        = uct_md_stub_rkey_unpack,
    .rkey_ptr           = (uct_component_rkey_ptr_func_t)
                          ucs_empty_function_return_unsupported,
    .rkey_release       = (uct_component_rkey_release_func_t)
                          ucs_empty_function_return_success,
    .rkey_compare       = uct_base_rkey_compare,
    .name               = "furiosa",
    .md_config          = {
        .name           = "Furiosa NPU memory domain",
        .prefix         = "FURIOSA_",
        .table          = uct_furiosa_md_config_table,
        .size           = sizeof(uct_furiosa_md_config_t),
    },
    .cm_config          = UCS_CONFIG_EMPTY_GLOBAL_LIST_ENTRY,
    .tl_list            = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_furiosa_component),
    .flags              = 0,
    .md_vfs_init        = (uct_component_md_vfs_init_func_t)ucs_empty_function
};
UCT_COMPONENT_REGISTER(&uct_furiosa_component);
