/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "furiosa_iface.h"
#include "furiosa_md.h"

#include <uct/furiosa/base/furiosa_base.h>

#include <string.h>
#include <limits.h>
#include <inttypes.h>
#include <uct/base/uct_iov.inl>

static ucs_status_t
uct_furiosa_ep_put_short(uct_ep_h tl_ep, const void *buffer,
                         unsigned length, uint64_t remote_addr,
                         uct_rkey_t rkey)
{
    if (ucs_likely(length != 0)) {
        memcpy((void *)(uintptr_t)remote_addr, buffer, length);
    }

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), PUT, SHORT,
                      length);
    return UCS_OK;
}

static ucs_status_t
uct_furiosa_ep_get_short(uct_ep_h tl_ep, void *buffer, unsigned length,
                         uint64_t remote_addr, uct_rkey_t rkey)
{
    if (ucs_likely(length != 0)) {
        memcpy(buffer, (void *)(uintptr_t)remote_addr, length);
    }

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), GET, SHORT,
                      length);
    return UCS_OK;
}

static ucs_status_t
uct_furiosa_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                         size_t iovcnt, uint64_t remote_addr,
                         uct_rkey_t rkey, uct_completion_t *comp)
{
    size_t size = uct_iov_get_length(iov);

    if (ucs_likely(size != 0)) {
        memcpy((void *)(uintptr_t)remote_addr, iov->buffer, size);
    }

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), PUT, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));
    return UCS_OK;
}

static ucs_status_t
uct_furiosa_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                         size_t iovcnt, uint64_t remote_addr,
                         uct_rkey_t rkey, uct_completion_t *comp)
{
    size_t size = uct_iov_get_length(iov);

    if (ucs_likely(size != 0)) {
        memcpy(iov->buffer, (void *)(uintptr_t)remote_addr, size);
    }

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), GET, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));
    return UCS_OK;
}

static ucs_status_t
uct_furiosa_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_furiosa_iface_t *iface = ucs_derived_of(tl_iface,
                                                 uct_furiosa_iface_t);

    uct_base_iface_query(&iface->super, iface_attr);
    iface_attr->iface_addr_len          = sizeof(uct_furiosa_iface_addr_t);
    iface_attr->device_addr_len         = 0;
    iface_attr->ep_addr_len             = 0;
    iface_attr->cap.flags              = UCT_IFACE_FLAG_PUT_SHORT |
                                         UCT_IFACE_FLAG_GET_SHORT |
                                         UCT_IFACE_FLAG_PUT_ZCOPY |
                                         UCT_IFACE_FLAG_GET_ZCOPY |
                                         UCT_IFACE_FLAG_CONNECT_TO_IFACE;
    iface_attr->cap.put.max_short       = UINT_MAX;
    iface_attr->cap.put.min_zcopy       = 0;
    iface_attr->cap.put.max_zcopy       = SIZE_MAX;
    iface_attr->cap.put.opt_zcopy_align = 1;
    iface_attr->cap.put.align_mtu       = iface_attr->cap.put.opt_zcopy_align;
    iface_attr->cap.put.max_iov         = 1;
    iface_attr->cap.get.max_short       = UINT_MAX;
    iface_attr->cap.get.min_zcopy       = 0;
    iface_attr->cap.get.max_zcopy       = SIZE_MAX;
    iface_attr->cap.get.opt_zcopy_align = 1;
    iface_attr->cap.get.align_mtu       = iface_attr->cap.get.opt_zcopy_align;
    iface_attr->cap.get.max_iov         = 1;
    iface_attr->latency                 = ucs_linear_func_make(1e-9, 0);
    iface_attr->bandwidth.dedicated     = 0.0001;
    iface_attr->bandwidth.shared        = 0;
    iface_attr->overhead                = 0;
    iface_attr->priority                = 0;
    iface_attr->max_num_eps             = SIZE_MAX;

    return UCS_OK;
}

static UCS_CLASS_INIT_FUNC(uct_furiosa_ep_t, const uct_ep_params_t *params)
{
    uct_furiosa_iface_t *iface = ucs_derived_of(params->iface,
                                                 uct_furiosa_iface_t);

    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_furiosa_ep_t)
{
}

UCS_CLASS_DEFINE(uct_furiosa_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_furiosa_ep_t, uct_ep_t,
                          const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_furiosa_ep_t, uct_ep_t);

static ucs_status_t
uct_furiosa_iface_get_address(uct_iface_h tl_iface,
                              uct_iface_addr_t *iface_addr)
{
    uct_furiosa_iface_t *iface = ucs_derived_of(tl_iface,
                                                 uct_furiosa_iface_t);

    *(uct_furiosa_iface_addr_t *)iface_addr = iface->id;
    return UCS_OK;
}
static int
uct_furiosa_iface_is_reachable_v2(
        const uct_iface_h tl_iface,
        const uct_iface_is_reachable_params_t *params)
{
    uct_furiosa_iface_t *iface = ucs_derived_of(tl_iface,
                                                 uct_furiosa_iface_t);
    uct_furiosa_iface_addr_t *addr;

    if (!uct_iface_is_reachable_params_addrs_valid(params)) {
        return 0;
    }

    addr = (uct_furiosa_iface_addr_t *)params->iface_addr;
    if (addr == NULL) {
        uct_iface_fill_info_str_buf(params, "no iface address");
        return 0;
    }

    if (iface->id != *addr) {
        uct_iface_fill_info_str_buf(
                params, "different iface id %" PRIx64 " vs %" PRIx64,
                iface->id, *addr);
        return 0;
    }

    return uct_iface_scope_is_reachable(tl_iface, params);
}


static uct_iface_ops_t uct_furiosa_iface_ops = {
    .ep_pending_purge = (uct_ep_pending_purge_func_t)ucs_empty_function,
    .ep_connect    = (uct_ep_connect_func_t)ucs_empty_function_return_success,
    .ep_disconnect = (uct_ep_disconnect_func_t)ucs_empty_function_return_success,
    .cm_ep_conn_notify = (uct_cm_ep_conn_notify_func_t)
            ucs_empty_function_return_unsupported,
    .ep_destroy = UCS_CLASS_DELETE_FUNC_NAME(uct_furiosa_ep_t),
    .ep_put_short = uct_furiosa_ep_put_short,
    .ep_get_short = uct_furiosa_ep_get_short,
    .ep_put_zcopy = uct_furiosa_ep_put_zcopy,
    .ep_get_zcopy = uct_furiosa_ep_get_zcopy,
    .ep_put_bcopy = (uct_ep_put_bcopy_func_t)
            ucs_empty_function_return_unsupported,
    .ep_get_bcopy = (uct_ep_get_bcopy_func_t)
            ucs_empty_function_return_unsupported,
    .ep_am_short = (uct_ep_am_short_func_t)ucs_empty_function_return_unsupported,
    .ep_am_short_iov = (uct_ep_am_short_iov_func_t)
            ucs_empty_function_return_unsupported,
    .ep_am_bcopy = (uct_ep_am_bcopy_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic_cswap64 = (uct_ep_atomic_cswap64_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic64_post  = (uct_ep_atomic64_post_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic64_fetch = (uct_ep_atomic64_fetch_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic_cswap32 = (uct_ep_atomic_cswap32_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic32_post  = (uct_ep_atomic32_post_func_t)
            ucs_empty_function_return_unsupported,
    .ep_atomic32_fetch = (uct_ep_atomic32_fetch_func_t)
            ucs_empty_function_return_unsupported,
    .ep_pending_add    = (uct_ep_pending_add_func_t)
            ucs_empty_function_return_unsupported,
    .ep_flush    = (uct_ep_flush_func_t)ucs_empty_function_return_success,
    .ep_fence    = (uct_ep_fence_func_t)ucs_empty_function_return_success,
    .ep_check    = (uct_ep_check_func_t)ucs_empty_function_return_unsupported,
    .ep_create   = UCS_CLASS_NEW_FUNC_NAME(uct_furiosa_ep_t),
    .iface_flush = (uct_iface_flush_func_t)ucs_empty_function_return_success,
    .iface_fence = (uct_iface_fence_func_t)ucs_empty_function_return_success,
    .iface_progress_enable  = (uct_iface_progress_enable_func_t)
            ucs_empty_function,
    .iface_progress_disable = (uct_iface_progress_disable_func_t)
            ucs_empty_function,
    .iface_progress = (uct_iface_progress_func_t)ucs_empty_function_return_zero,
    .iface_event_fd_get       = (uct_iface_event_fd_get_func_t)
            ucs_empty_function_return_unsupported,
    .iface_event_arm          = (uct_iface_event_arm_func_t)
            ucs_empty_function_return_unsupported,
    .iface_close              = (uct_iface_close_func_t)ucs_empty_function,
    .iface_query              = uct_furiosa_iface_query,
    .iface_get_device_address = (uct_iface_get_device_address_func_t)
            ucs_empty_function_return_success,
    .iface_get_address        = uct_furiosa_iface_get_address,
    .iface_is_reachable       = uct_base_iface_is_reachable
};

static uct_iface_internal_ops_t uct_furiosa_iface_internal_ops = {
    .iface_query_v2      = uct_iface_base_query_v2,
    .iface_estimate_perf = (uct_iface_estimate_perf_func_t)
            ucs_empty_function_return_unsupported,
    .iface_vfs_refresh   = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate         = (uct_ep_invalidate_func_t)
            ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2   = (uct_ep_connect_to_ep_v2_func_t)
            ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2 = uct_furiosa_iface_is_reachable_v2,
    .ep_is_connected       = (uct_ep_is_connected_func_t)
            ucs_empty_function_return_zero_int
};

static UCS_CLASS_INIT_FUNC(uct_furiosa_iface_t, uct_md_h md,
                           uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    UCS_CLASS_CALL_SUPER_INIT(uct_base_iface_t, &uct_furiosa_iface_ops,
                              &uct_furiosa_iface_internal_ops, md, worker,
                              params,
                              tl_config UCS_STATS_ARG(params->stats_root)
                                      UCS_STATS_ARG(UCT_FURIOSA_TL_NAME));

    self->id = (uct_furiosa_iface_addr_t)((uct_furiosa_md_t *)md)->device_id;
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_furiosa_iface_t)
{
}

UCS_CLASS_DEFINE(uct_furiosa_iface_t, uct_base_iface_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_furiosa_iface_t, uct_iface_t, uct_md_h,
                          uct_worker_h, const uct_iface_params_t*,
                          const uct_iface_config_t*);


UCT_TL_DEFINE(&uct_furiosa_component, furiosa, uct_furiosa_base_query_devices,
              uct_furiosa_iface_t, "FURIOSA_", uct_iface_config_table,
              uct_iface_config_t);
