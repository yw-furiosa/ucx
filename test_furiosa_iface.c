/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 *
 * Phase 6 standalone test -- TL registration, iface open/close, iface query.
 *
 * Build:
 *   gcc -o test_furiosa_iface test_furiosa_iface.c \
 *       -I src -I install/include -I . \
 *       -L install/lib -luct -lucs -Wl,-rpath,$PWD/install/lib
 *
 * Run:
 *   ./test_furiosa_iface
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include <uct/api/uct.h>
#include <ucs/async/async.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, fmt, ...)                                               \
    do {                                                                     \
        if (cond) {                                                          \
            printf("  PASS: " fmt "\n", ##__VA_ARGS__);                     \
            g_pass++;                                                        \
        } else {                                                             \
            printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                     \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

static uct_component_h find_furiosa_component(void)
{
    uct_component_h *components = NULL;
    unsigned num_components = 0;
    uct_component_h found = NULL;
    ucs_status_t status;
    unsigned i;

    status = uct_query_components(&components, &num_components);
    assert(status == UCS_OK);

    for (i = 0; i < num_components; i++) {
        uct_component_attr_t attr;
        memset(&attr, 0, sizeof(attr));
        attr.field_mask = UCT_COMPONENT_ATTR_FIELD_NAME;
        status = uct_component_query(components[i], &attr);
        if (status == UCS_OK && strcmp(attr.name, "furiosa") == 0) {
            found = components[i];
            break;
        }
    }

    uct_release_component_list(components);
    return found;
}

static void test_tl_resource_query(uct_md_h md)
{
    uct_tl_resource_desc_t *resources = NULL;
    unsigned num_resources = 0;
    ucs_status_t status;
    int found_furiosa = 0;
    unsigned i;

    printf("\n[test_tl_resource_query]\n");

    status = uct_md_query_tl_resources(md, &resources, &num_resources);
    CHECK(status == UCS_OK, "uct_md_query_tl_resources returned %d", status);
    CHECK(num_resources > 0, "num_resources=%u (expected > 0)", num_resources);

    for (i = 0; i < num_resources; i++) {
        printf("    resource[%u]: tl=%s dev=%s type=%d\n",
               i, resources[i].tl_name, resources[i].dev_name,
               resources[i].dev_type);
        if (strcmp(resources[i].tl_name, "furiosa") == 0) {
            found_furiosa = 1;
        }
    }

    CHECK(found_furiosa, "found furiosa TL in resource list");

    if (num_resources > 0) {
        CHECK(strcmp(resources[0].tl_name, "furiosa") == 0,
              "tl_name=\"%s\" (expected \"furiosa\")", resources[0].tl_name);
        CHECK(strcmp(resources[0].dev_name, "furiosa") == 0,
              "dev_name=\"%s\" (expected \"furiosa\")", resources[0].dev_name);
        CHECK(resources[0].dev_type == UCT_DEVICE_TYPE_ACC,
              "dev_type=%d (expected UCT_DEVICE_TYPE_ACC=%d)",
              resources[0].dev_type, UCT_DEVICE_TYPE_ACC);
    }

    uct_release_tl_resource_list(resources);
}

static void test_iface_open_close(uct_md_h md)
{
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_config_t *iface_config = NULL;
    uct_iface_h iface = NULL;
    uct_iface_params_t params;
    ucs_status_t status;

    printf("\n[test_iface_open_close]\n");

    status = ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK, &async);
    CHECK(status == UCS_OK, "ucs_async_context_create returned %d", status);
    if (status != UCS_OK) {
        return;
    }

    status = uct_worker_create(async, UCS_THREAD_MODE_SINGLE, &worker);
    CHECK(status == UCS_OK, "uct_worker_create returned %d", status);
    if (status != UCS_OK) {
        goto out_async;
    }

    status = uct_md_iface_config_read(md, "furiosa", NULL, NULL, &iface_config);
    CHECK(status == UCS_OK, "uct_md_iface_config_read returned %d", status);
    if (status != UCS_OK) {
        goto out_worker;
    }

    memset(&params, 0, sizeof(params));
    params.field_mask = UCT_IFACE_PARAM_FIELD_OPEN_MODE |
                        UCT_IFACE_PARAM_FIELD_DEVICE;
    params.open_mode = UCT_IFACE_OPEN_MODE_DEVICE;
    params.mode.device.tl_name  = "furiosa";
    params.mode.device.dev_name = "furiosa";

    status = uct_iface_open(md, worker, &params, iface_config, &iface);
    CHECK(status == UCS_OK, "uct_iface_open returned %d", status);
    if (status != UCS_OK) {
        goto out_config;
    }

    CHECK(iface != NULL, "iface handle is non-NULL");

    uct_iface_close(iface);
    CHECK(1, "uct_iface_close completed without crash");

out_config:
    uct_config_release(iface_config);
out_worker:
    uct_worker_destroy(worker);
out_async:
    ucs_async_context_destroy(async);
}

static void test_iface_query(uct_md_h md)
{
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_config_t *iface_config = NULL;
    uct_iface_h iface = NULL;
    uct_iface_params_t params;
    uct_iface_attr_t attr;
    ucs_status_t status;

    printf("\n[test_iface_query]\n");

    status = ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK, &async);
    assert(status == UCS_OK);
    status = uct_worker_create(async, UCS_THREAD_MODE_SINGLE, &worker);
    assert(status == UCS_OK);
    status = uct_md_iface_config_read(md, "furiosa", NULL, NULL, &iface_config);
    assert(status == UCS_OK);

    memset(&params, 0, sizeof(params));
    params.field_mask = UCT_IFACE_PARAM_FIELD_OPEN_MODE |
                        UCT_IFACE_PARAM_FIELD_DEVICE;
    params.open_mode = UCT_IFACE_OPEN_MODE_DEVICE;
    params.mode.device.tl_name  = "furiosa";
    params.mode.device.dev_name = "furiosa";

    status = uct_iface_open(md, worker, &params, iface_config, &iface);
    assert(status == UCS_OK);

    memset(&attr, 0, sizeof(attr));
    status = uct_iface_query(iface, &attr);
    CHECK(status == UCS_OK, "uct_iface_query returned %d", status);
    CHECK((attr.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE) != 0,
          "cap.flags has CONNECT_TO_IFACE (flags=0x%" PRIx64 ")",
          attr.cap.flags);
    CHECK(attr.bandwidth.dedicated > 0,
          "bandwidth.dedicated=%g (expected > 0)", attr.bandwidth.dedicated);
    CHECK(attr.bandwidth.shared == 0,
          "bandwidth.shared=%g (expected 0)", attr.bandwidth.shared);
    CHECK(attr.max_num_eps == 0,
          "max_num_eps=%zu (expected 0 for skeleton)", attr.max_num_eps);
    CHECK(attr.device_addr_len == 0,
          "device_addr_len=%zu (expected 0)", attr.device_addr_len);
    CHECK(attr.iface_addr_len == 0,
          "iface_addr_len=%zu (expected 0)", attr.iface_addr_len);

    uct_iface_close(iface);
    uct_config_release(iface_config);
    uct_worker_destroy(worker);
    ucs_async_context_destroy(async);
}

static void test_ep_create_unsupported(uct_md_h md)
{
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_config_t *iface_config = NULL;
    uct_iface_h iface = NULL;
    uct_ep_h ep = NULL;
    uct_iface_params_t iface_params;
    uct_ep_params_t ep_params;
    ucs_status_t status;

    printf("\n[test_ep_create_unsupported]\n");

    status = ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK, &async);
    assert(status == UCS_OK);
    status = uct_worker_create(async, UCS_THREAD_MODE_SINGLE, &worker);
    assert(status == UCS_OK);
    status = uct_md_iface_config_read(md, "furiosa", NULL, NULL, &iface_config);
    assert(status == UCS_OK);

    memset(&iface_params, 0, sizeof(iface_params));
    iface_params.field_mask = UCT_IFACE_PARAM_FIELD_OPEN_MODE |
                              UCT_IFACE_PARAM_FIELD_DEVICE;
    iface_params.open_mode = UCT_IFACE_OPEN_MODE_DEVICE;
    iface_params.mode.device.tl_name  = "furiosa";
    iface_params.mode.device.dev_name = "furiosa";

    status = uct_iface_open(md, worker, &iface_params, iface_config, &iface);
    assert(status == UCS_OK);

    memset(&ep_params, 0, sizeof(ep_params));
    ep_params.field_mask = UCT_EP_PARAM_FIELD_IFACE;
    ep_params.iface      = iface;

    status = uct_ep_create(&ep_params, &ep);
    CHECK(status == UCS_ERR_UNSUPPORTED,
          "ep_create returned %d (expected UCS_ERR_UNSUPPORTED=%d)",
          status, UCS_ERR_UNSUPPORTED);

    uct_iface_close(iface);
    uct_config_release(iface_config);
    uct_worker_destroy(worker);
    ucs_async_context_destroy(async);
}

int main(void)
{
    uct_component_h comp;
    uct_md_config_t *md_config = NULL;
    uct_md_h md = NULL;
    ucs_status_t status;

    printf("=== Furiosa Phase 6: TL iface test ===\n");

    comp = find_furiosa_component();
    if (comp == NULL) {
        printf("FATAL: furiosa component not found\n");
        return 1;
    }
    printf("Found furiosa component.\n");

    status = uct_md_config_read(comp, NULL, NULL, &md_config);
    assert(status == UCS_OK);
    status = uct_md_open(comp, "furiosa/0", md_config, &md);
    assert(status == UCS_OK);
    printf("MD opened.\n");

    test_tl_resource_query(md);
    test_iface_open_close(md);
    test_iface_query(md);
    test_ep_create_unsupported(md);

    uct_md_close(md);
    uct_config_release(md_config);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail > 0) ? 1 : 0;
}
