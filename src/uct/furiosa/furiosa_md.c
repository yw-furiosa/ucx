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

#include <inttypes.h>
#include <fcntl.h>
#include <pthread.h>


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

    if (md->dmabuf_addr != NULL) {
        uct_furiosa_base_munmap(md->dmabuf_addr, md->dmabuf_size);
    }
    uct_furiosa_base_close_dmabuf_fd(md->dmabuf_fd);
    if (md->bar4_fd_owned) {
        uct_furiosa_base_close_fd(md->bar4_fd);
    }
    ucs_free(md);
}

static ucs_status_t
uct_furiosa_md_query_attributes(uct_md_h md, const void *addr, size_t length,
                                ucs_memory_info_t *mem_info, int *dmabuf_fd)
{
    uct_furiosa_md_t *fmd = ucs_derived_of(md, uct_furiosa_md_t);
    void *begin;
    void *end;

    /*
     * FIXME: This mmap-based address range check is a PoC workaround.
     * We mmap BAR4 in md_open solely to obtain a host VA range, then detect
     * NPU memory by checking if a pointer falls within that range.
     * In production, we should use device-runtime's allocator interface to
     * determine whether a given address belongs to the NPU.
     */
    if (fmd->dmabuf_addr == NULL) {
        return UCS_ERR_UNSUPPORTED;
    }

    begin = fmd->dmabuf_addr;
    end   = (uint8_t *)begin + fmd->dmabuf_size;

    if ((addr < begin) || (addr >= end)) {
        mem_info->type = UCS_MEMORY_TYPE_LAST;
        return UCS_ERR_OUT_OF_RANGE;
    }

    *dmabuf_fd             = fmd->dmabuf_fd;
    mem_info->type         = UCS_MEMORY_TYPE_RDMA;
    mem_info->base_address = fmd->dmabuf_addr;
    mem_info->alloc_length = (size_t)fmd->dmabuf_size;
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
        mem_attr_p->dmabuf_offset = UCS_PTR_BYTE_DIFF(mem_info.base_address,
                                                       addr);
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
    ucs_status_t status;
    struct npu_bar_info bar_info;
    int bar4_fd;
    int dmabuf_fd;
    void *dmabuf_addr;
    md = ucs_malloc(sizeof(uct_furiosa_md_t), "uct_furiosa_md_t");
    if (md == NULL) {
        ucs_error("failed to allocate memory for uct_furiosa_md_t");
        return UCS_ERR_NO_MEMORY;
    }
    bar4_fd = uct_furiosa_base_open_bar4(config->device_id);
    if (bar4_fd < 0) {
        ucs_error("failed to open furiosa npu%d BAR4", config->device_id);
        status = UCS_ERR_NO_DEVICE;
        goto err_free_md;
    }
    status = uct_furiosa_base_get_bar_info(bar4_fd, &bar_info);
    if (status != UCS_OK) {
        goto err_close_bar4;
    }

    /* Export dmabuf for future RDMA registration (ibv_reg_dmabuf_mr).
     * This is non-fatal: we keep the fd but don't mmap through it. */
    dmabuf_fd = -1;
    status = uct_furiosa_base_export_dmabuf(bar4_fd, NPU_BAR4_RESERVED_SIZE,
                                             UCT_FURIOSA_POC_MAP_SIZE,
                                             &dmabuf_fd);
    if (status != UCS_OK) {
        ucs_warn("furiosa: dmabuf export failed (non-fatal)");
        dmabuf_fd = -1;
    }

    /* FIXME: This direct BAR4 mmap is a PoC workaround to get a host VA
     * range for memory type detection. In production, the Furiosa
     * device-runtime allocator would provide mapped regions, and the MD
     * would query that allocator instead of mapping BAR4 itself. */
    dmabuf_addr = NULL;
    status = uct_furiosa_base_mmap_bar4(bar4_fd, NPU_BAR4_RESERVED_SIZE,
                                         UCT_FURIOSA_POC_MAP_SIZE,
                                         &dmabuf_addr);
    if (status != UCS_OK) {
        goto err_close_dmabuf;
    }

    md->bar4_fd        = bar4_fd;
    md->bar4_fd_owned  = true;
    md->device_id      = config->device_id;
    md->bar_phys_addr  = bar_info.bar_phy_addr;
    md->bar_size       = bar_info.bar_size;
    md->dmabuf_fd      = dmabuf_fd;
    md->dmabuf_addr    = dmabuf_addr;
    md->dmabuf_size    = UCT_FURIOSA_POC_MAP_SIZE;
    md->super.ops      = &uct_furiosa_md_ops;
    md->super.component = &uct_furiosa_component;

    *md_p = (uct_md_h)md;
    ucs_debug("opened furiosa MD: npu%u bar_phys=0x%" PRIx64
              " dmabuf_addr=%p dmabuf_size=0x%" PRIx64,
              md->device_id, md->bar_phys_addr,
              md->dmabuf_addr, md->dmabuf_size);
    return UCS_OK;

err_close_dmabuf:
    uct_furiosa_base_close_dmabuf_fd(dmabuf_fd);
err_close_bar4:
    uct_furiosa_base_close_fd(bar4_fd);
err_free_md:
    ucs_free(md);
    return status;
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
