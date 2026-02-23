/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 *
 * Phase 8 standalone test -- put_zcopy / get_zcopy via BAR4 memcpy.
 *
 * Build:
 *   gcc -DNVALGRIND -o test_furiosa_zcopy test_furiosa_zcopy.c \
 *       -I src -I install/include -I . \
 *       -L install/lib -luct -lucs -Wl,-rpath,$PWD/install/lib
 *
 * Run:
 *   ./test_furiosa_zcopy
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include <uct/api/uct.h>
#include <ucs/async/async.h>
#include <uct/furiosa/furiosa_md.h>

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

/**
 * Helper: set up MD + worker + iface + ep.
 * Returns 0 on success, -1 on failure.
 */
static int setup_ep(uct_component_h comp,
                    uct_md_h *md_out,
                    ucs_async_context_t **async_out,
                    uct_worker_h *worker_out,
                    uct_iface_h *iface_out,
                    uct_ep_h *ep_out)
{
    uct_md_config_t *md_config = NULL;
    uct_iface_config_t *iface_config = NULL;
    uct_iface_params_t iface_params;
    uct_ep_params_t ep_params;
    ucs_status_t status;

    status = uct_md_config_read(comp, NULL, NULL, &md_config);
    assert(status == UCS_OK);
    status = uct_md_open(comp, "furiosa/0", md_config, md_out);
    uct_config_release(md_config);
    if (status != UCS_OK) {
        printf("  FATAL: uct_md_open failed: %d\n", status);
        return -1;
    }

    status = ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK,
                                      async_out);
    assert(status == UCS_OK);
    status = uct_worker_create(*async_out, UCS_THREAD_MODE_SINGLE, worker_out);
    assert(status == UCS_OK);

    status = uct_md_iface_config_read(*md_out, "furiosa", NULL, NULL,
                                      &iface_config);
    assert(status == UCS_OK);

    memset(&iface_params, 0, sizeof(iface_params));
    iface_params.field_mask = UCT_IFACE_PARAM_FIELD_OPEN_MODE |
                              UCT_IFACE_PARAM_FIELD_DEVICE;
    iface_params.open_mode = UCT_IFACE_OPEN_MODE_DEVICE;
    iface_params.mode.device.tl_name  = "furiosa";
    iface_params.mode.device.dev_name = "furiosa";

    status = uct_iface_open(*md_out, *worker_out, &iface_params,
                            iface_config, iface_out);
    uct_config_release(iface_config);
    if (status != UCS_OK) {
        printf("  FATAL: uct_iface_open failed: %d\n", status);
        return -1;
    }

    memset(&ep_params, 0, sizeof(ep_params));
    ep_params.field_mask = UCT_EP_PARAM_FIELD_IFACE;
    ep_params.iface      = *iface_out;

    status = uct_ep_create(&ep_params, ep_out);
    if (status != UCS_OK) {
        printf("  FATAL: uct_ep_create failed: %d\n", status);
        return -1;
    }

    return 0;
}

static void teardown_ep(uct_md_h md, ucs_async_context_t *async,
                        uct_worker_h worker, uct_iface_h iface,
                        uct_ep_h ep)
{
    uct_ep_destroy(ep);
    uct_iface_close(iface);
    uct_worker_destroy(worker);
    ucs_async_context_destroy(async);
    uct_md_close(md);
}

static void test_iface_query_zcopy_caps(uct_component_h comp)
{
    uct_md_h md = NULL;
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_h iface = NULL;
    uct_ep_h ep = NULL;
    uct_iface_attr_t attr;
    ucs_status_t status;

    printf("\n[test_iface_query_zcopy_caps]\n");

    if (setup_ep(comp, &md, &async, &worker, &iface, &ep) != 0) {
        CHECK(0, "setup_ep failed");
        return;
    }

    memset(&attr, 0, sizeof(attr));
    status = uct_iface_query(iface, &attr);
    CHECK(status == UCS_OK, "uct_iface_query returned %d", status);
    CHECK((attr.cap.flags & UCT_IFACE_FLAG_PUT_ZCOPY) != 0,
          "cap.flags has PUT_ZCOPY (flags=0x%" PRIx64 ")", attr.cap.flags);
    CHECK((attr.cap.flags & UCT_IFACE_FLAG_GET_ZCOPY) != 0,
          "cap.flags has GET_ZCOPY (flags=0x%" PRIx64 ")", attr.cap.flags);
    CHECK(attr.cap.put.max_zcopy > 0,
          "cap.put.max_zcopy=%zu (expected > 0)",
          (size_t)attr.cap.put.max_zcopy);
    CHECK(attr.cap.get.max_zcopy > 0,
          "cap.get.max_zcopy=%zu (expected > 0)",
          (size_t)attr.cap.get.max_zcopy);
    CHECK(attr.cap.put.max_iov >= 1,
          "cap.put.max_iov=%zu (expected >= 1)",
          (size_t)attr.cap.put.max_iov);
    CHECK(attr.cap.get.max_iov >= 1,
          "cap.get.max_iov=%zu (expected >= 1)",
          (size_t)attr.cap.get.max_iov);

    teardown_ep(md, async, worker, iface, ep);
}

static void test_put_get_zcopy_1byte(uct_component_h comp)
{
    uct_md_h md = NULL;
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_h iface = NULL;
    uct_ep_h ep = NULL;
    uct_furiosa_md_t *fmd;
    unsigned char *npu_mem;
    unsigned char send_byte;
    unsigned char recv_byte;
    uct_iov_t iov;
    ucs_status_t status;

    printf("\n[test_put_get_zcopy_1byte]\n");

    if (setup_ep(comp, &md, &async, &worker, &iface, &ep) != 0) {
        CHECK(0, "setup_ep failed");
        return;
    }

    fmd = ucs_derived_of(md, uct_furiosa_md_t);
    npu_mem = (unsigned char *)fmd->dmabuf_addr;
    CHECK(npu_mem != NULL, "dmabuf_addr is non-NULL (%p)", (void *)npu_mem);

    /* Clear target byte */
    npu_mem[0] = 0x00;

    /* put_zcopy: write 0xAB via iov */
    send_byte = 0xAB;
    memset(&iov, 0, sizeof(iov));
    iov.buffer = &send_byte;
    iov.length = 1;
    iov.count  = 1;

    status = uct_ep_put_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)&npu_mem[0], 0, NULL);
    CHECK(status == UCS_OK, "put_zcopy(1 byte) returned %d", status);
    CHECK(npu_mem[0] == 0xAB, "NPU byte = 0x%02x (expected 0xAB)",
          npu_mem[0]);

    /* get_zcopy: read it back */
    recv_byte = 0x00;
    memset(&iov, 0, sizeof(iov));
    iov.buffer = &recv_byte;
    iov.length = 1;
    iov.count  = 1;

    status = uct_ep_get_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)&npu_mem[0], 0, NULL);
    CHECK(status == UCS_OK, "get_zcopy(1 byte) returned %d", status);
    CHECK(recv_byte == 0xAB, "recv byte = 0x%02x (expected 0xAB)",
          recv_byte);

    teardown_ep(md, async, worker, iface, ep);
}

static void test_put_get_zcopy_64bytes(uct_component_h comp)
{
    uct_md_h md = NULL;
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_h iface = NULL;
    uct_ep_h ep = NULL;
    uct_furiosa_md_t *fmd;
    unsigned char *npu_mem;
    unsigned char send_buf[64];
    unsigned char recv_buf[64];
    uct_iov_t iov;
    ucs_status_t status;
    unsigned i;

    printf("\n[test_put_get_zcopy_64bytes]\n");

    if (setup_ep(comp, &md, &async, &worker, &iface, &ep) != 0) {
        CHECK(0, "setup_ep failed");
        return;
    }

    fmd = ucs_derived_of(md, uct_furiosa_md_t);
    npu_mem = (unsigned char *)fmd->dmabuf_addr;

    /* Fill send buffer with pattern */
    for (i = 0; i < 64; i++) {
        send_buf[i] = (unsigned char)(i ^ 0xAA);
    }
    memset(npu_mem, 0, 64);

    /* put_zcopy: write 64 bytes */
    memset(&iov, 0, sizeof(iov));
    iov.buffer = send_buf;
    iov.length = 64;
    iov.count  = 1;

    status = uct_ep_put_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)npu_mem, 0, NULL);
    CHECK(status == UCS_OK, "put_zcopy(64 bytes) returned %d", status);
    CHECK(memcmp(npu_mem, send_buf, 64) == 0,
          "NPU memory matches send pattern (64 bytes)");

    /* get_zcopy: read 64 bytes back */
    memset(recv_buf, 0, 64);
    memset(&iov, 0, sizeof(iov));
    iov.buffer = recv_buf;
    iov.length = 64;
    iov.count  = 1;

    status = uct_ep_get_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)npu_mem, 0, NULL);
    CHECK(status == UCS_OK, "get_zcopy(64 bytes) returned %d", status);
    CHECK(memcmp(recv_buf, send_buf, 64) == 0,
          "recv buffer matches send pattern (64 bytes)");

    teardown_ep(md, async, worker, iface, ep);
}

static void test_put_get_zcopy_4096bytes(uct_component_h comp)
{
    uct_md_h md = NULL;
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_h iface = NULL;
    uct_ep_h ep = NULL;
    uct_furiosa_md_t *fmd;
    unsigned char *npu_mem;
    unsigned char *send_buf;
    unsigned char *recv_buf;
    uct_iov_t iov;
    ucs_status_t status;
    unsigned i;

    printf("\n[test_put_get_zcopy_4096bytes]\n");

    if (setup_ep(comp, &md, &async, &worker, &iface, &ep) != 0) {
        CHECK(0, "setup_ep failed");
        return;
    }

    fmd = ucs_derived_of(md, uct_furiosa_md_t);
    npu_mem = (unsigned char *)fmd->dmabuf_addr;

    send_buf = (unsigned char *)malloc(4096);
    recv_buf = (unsigned char *)malloc(4096);
    assert(send_buf != NULL && recv_buf != NULL);

    /* Fill send buffer with pattern */
    for (i = 0; i < 4096; i++) {
        send_buf[i] = (unsigned char)(i & 0xFF);
    }
    memset(npu_mem, 0, 4096);

    /* put_zcopy: write 4096 bytes */
    memset(&iov, 0, sizeof(iov));
    iov.buffer = send_buf;
    iov.length = 4096;
    iov.count  = 1;

    status = uct_ep_put_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)npu_mem, 0, NULL);
    CHECK(status == UCS_OK, "put_zcopy(4096 bytes) returned %d", status);
    CHECK(memcmp(npu_mem, send_buf, 4096) == 0,
          "NPU memory matches send pattern (4096 bytes)");

    /* get_zcopy: read 4096 bytes back */
    memset(recv_buf, 0, 4096);
    memset(&iov, 0, sizeof(iov));
    iov.buffer = recv_buf;
    iov.length = 4096;
    iov.count  = 1;

    status = uct_ep_get_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)npu_mem, 0, NULL);
    CHECK(status == UCS_OK, "get_zcopy(4096 bytes) returned %d", status);
    CHECK(memcmp(recv_buf, send_buf, 4096) == 0,
          "recv buffer matches send pattern (4096 bytes)");

    free(send_buf);
    free(recv_buf);
    teardown_ep(md, async, worker, iface, ep);
}

static void test_put_get_zcopy_zero_length(uct_component_h comp)
{
    uct_md_h md = NULL;
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_h iface = NULL;
    uct_ep_h ep = NULL;
    uct_furiosa_md_t *fmd;
    unsigned char *npu_mem;
    unsigned char sentinel;
    uct_iov_t iov;
    ucs_status_t status;

    printf("\n[test_put_get_zcopy_zero_length]\n");

    if (setup_ep(comp, &md, &async, &worker, &iface, &ep) != 0) {
        CHECK(0, "setup_ep failed");
        return;
    }

    fmd = ucs_derived_of(md, uct_furiosa_md_t);
    npu_mem = (unsigned char *)fmd->dmabuf_addr;

    /* Put sentinel value, then verify zero-length zcopy doesn't overwrite */
    npu_mem[0] = 0xCC;
    sentinel = 0xFF;
    memset(&iov, 0, sizeof(iov));
    iov.buffer = &sentinel;
    iov.length = 0;
    iov.count  = 1;

    status = uct_ep_put_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)npu_mem, 0, NULL);
    CHECK(status == UCS_OK, "put_zcopy(0 bytes) returned %d", status);
    CHECK(npu_mem[0] == 0xCC,
          "NPU byte unchanged after zero-length put_zcopy (0x%02x)",
          npu_mem[0]);

    /* Zero-length get_zcopy should not modify recv buffer */
    sentinel = 0xDD;
    memset(&iov, 0, sizeof(iov));
    iov.buffer = &sentinel;
    iov.length = 0;
    iov.count  = 1;

    status = uct_ep_get_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)npu_mem, 0, NULL);
    CHECK(status == UCS_OK, "get_zcopy(0 bytes) returned %d", status);
    CHECK(sentinel == 0xDD,
          "recv byte unchanged after zero-length get_zcopy (0x%02x)",
          sentinel);

    teardown_ep(md, async, worker, iface, ep);
}

static void test_put_get_zcopy_large(uct_component_h comp)
{
    uct_md_h md = NULL;
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_h iface = NULL;
    uct_ep_h ep = NULL;
    uct_furiosa_md_t *fmd;
    unsigned char *npu_mem;
    unsigned char *send_buf;
    unsigned char *recv_buf;
    uct_iov_t iov;
    ucs_status_t status;
    size_t size;
    unsigned i;

    printf("\n[test_put_get_zcopy_large (1MB)]\n");

    if (setup_ep(comp, &md, &async, &worker, &iface, &ep) != 0) {
        CHECK(0, "setup_ep failed");
        return;
    }

    fmd = ucs_derived_of(md, uct_furiosa_md_t);
    npu_mem = (unsigned char *)fmd->dmabuf_addr;
    size = 1024 * 1024; /* 1MB */

    send_buf = (unsigned char *)malloc(size);
    recv_buf = (unsigned char *)malloc(size);
    assert(send_buf != NULL && recv_buf != NULL);

    /* Fill send buffer with pattern */
    for (i = 0; i < size; i++) {
        send_buf[i] = (unsigned char)((i * 7 + 13) & 0xFF);
    }
    memset(npu_mem, 0, size);

    /* put_zcopy: write 1MB */
    memset(&iov, 0, sizeof(iov));
    iov.buffer = send_buf;
    iov.length = size;
    iov.count  = 1;

    status = uct_ep_put_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)npu_mem, 0, NULL);
    CHECK(status == UCS_OK, "put_zcopy(1MB) returned %d", status);
    CHECK(memcmp(npu_mem, send_buf, size) == 0,
          "NPU memory matches send pattern (1MB)");

    /* get_zcopy: read 1MB back */
    memset(recv_buf, 0, size);
    memset(&iov, 0, sizeof(iov));
    iov.buffer = recv_buf;
    iov.length = size;
    iov.count  = 1;

    status = uct_ep_get_zcopy(ep, &iov, 1,
                              (uint64_t)(uintptr_t)npu_mem, 0, NULL);
    CHECK(status == UCS_OK, "get_zcopy(1MB) returned %d", status);
    CHECK(memcmp(recv_buf, send_buf, size) == 0,
          "recv buffer matches send pattern (1MB)");

    free(send_buf);
    free(recv_buf);
    teardown_ep(md, async, worker, iface, ep);
}

int main(void)
{
    uct_component_h comp;

    printf("=== Furiosa Phase 8: put_zcopy / get_zcopy test ===\n");

    comp = find_furiosa_component();
    if (comp == NULL) {
        printf("FATAL: furiosa component not found\n");
        return 1;
    }
    printf("Found furiosa component.\n");

    test_iface_query_zcopy_caps(comp);
    test_put_get_zcopy_1byte(comp);
    test_put_get_zcopy_64bytes(comp);
    test_put_get_zcopy_4096bytes(comp);
    test_put_get_zcopy_zero_length(comp);
    test_put_get_zcopy_large(comp);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail > 0) ? 1 : 0;
}
