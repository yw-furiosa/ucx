/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 *
 * Phase 4 standalone test — remote key pack / unpack round-trip.
 *
 * Build:
 *   gcc -o test_furiosa_rkey test_furiosa_rkey.c \
 *       -I src -I install/include -I . \
 *       -L install/lib -luct -lucs -Wl,-rpath,$PWD/install/lib
 *
 * Run:
 *   ./test_furiosa_rkey
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include <uct/api/uct.h>
#include <uct/api/v2/uct_v2.h>
#include <ucs/type/status.h>

/* Pull in the Furiosa-specific types for rkey struct verification */
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

/* ------------------------------------------------------------------ */
/* Test 1: md_query reports rkey_packed_size == sizeof(uct_furiosa_rkey_t) */
/* ------------------------------------------------------------------ */
static void test_rkey_packed_size(uct_md_h md)
{
    uct_md_attr_v2_t attr;
    ucs_status_t status;

    printf("\n[test_rkey_packed_size]\n");

    memset(&attr, 0, sizeof(attr));
    attr.field_mask = UINT64_MAX;
    status = uct_md_query_v2(md, &attr);
    CHECK(status == UCS_OK, "uct_md_query_v2 returned %d", status);
    CHECK(attr.rkey_packed_size == sizeof(uct_furiosa_rkey_t),
          "rkey_packed_size=%zu expected=%zu",
          attr.rkey_packed_size, sizeof(uct_furiosa_rkey_t));
    CHECK((attr.flags & UCT_MD_FLAG_NEED_RKEY) != 0,
          "UCT_MD_FLAG_NEED_RKEY is set (flags=0x%" PRIx64 ")",
          attr.flags);
}

/* ------------------------------------------------------------------ */
/* Test 2: mkey_pack round-trip via rkey_unpack                        */
/* ------------------------------------------------------------------ */
static void test_mkey_pack_unpack(uct_md_h md, uct_component_h component,
                                  void *npu_ptr, size_t npu_len)
{
    ucs_status_t status;
    uct_mem_h memh = NULL;
    uct_md_attr_v2_t attr;
    void *mkey_buf = NULL;
    uct_rkey_t rkey;
    void *rkey_handle = NULL;
    uct_furiosa_rkey_t *unpacked;
    uct_md_mem_reg_params_t reg_params;
    uct_md_mem_dereg_params_t dereg_params;
    uct_md_mkey_pack_params_t pack_params;
    uct_rkey_unpack_params_t unpack_params;

    printf("\n[test_mkey_pack_unpack]\n");

    /* Register NPU memory */
    memset(&reg_params, 0, sizeof(reg_params));
    reg_params.field_mask = 0;
    status = uct_md_mem_reg_v2(md, npu_ptr, npu_len, &reg_params, &memh);
    CHECK(status == UCS_OK, "mem_reg returned %d", status);
    if (status != UCS_OK) {
        return;
    }

    /* Query rkey size and allocate buffer */
    memset(&attr, 0, sizeof(attr));
    attr.field_mask = UINT64_MAX;
    status = uct_md_query_v2(md, &attr);
    CHECK(status == UCS_OK && attr.rkey_packed_size > 0,
          "rkey_packed_size=%zu", attr.rkey_packed_size);

    mkey_buf = calloc(1, attr.rkey_packed_size);
    assert(mkey_buf != NULL);

    /* Pack */
    memset(&pack_params, 0, sizeof(pack_params));
    pack_params.field_mask = 0;
    status = uct_md_mkey_pack_v2(md, memh, npu_ptr, npu_len,
                                 &pack_params, mkey_buf);
    CHECK(status == UCS_OK, "mkey_pack returned %d", status);

    /* Verify packed buffer contents directly */
    {
        uct_furiosa_rkey_t *packed = (uct_furiosa_rkey_t *)mkey_buf;

        CHECK(packed->bar_phys_addr != 0,
              "packed bar_phys_addr=0x%" PRIx64, packed->bar_phys_addr);
        CHECK(packed->bar_offset != 0,
              "packed bar_offset=0x%" PRIx64, packed->bar_offset);
        CHECK(packed->length == npu_len,
              "packed length=%zu expected=%zu", packed->length, npu_len);
        CHECK(packed->device_id == 6,
              "packed device_id=%u", packed->device_id);
    }

    /* Unpack */
    memset(&unpack_params, 0, sizeof(unpack_params));
    unpack_params.field_mask = 0;
    status = component->rkey_unpack(component, mkey_buf, &unpack_params,
                                    &rkey, &rkey_handle);
    CHECK(status == UCS_OK, "rkey_unpack returned %d", status);
    CHECK(rkey_handle == NULL, "rkey_handle is NULL");

    /* Verify unpacked key matches packed */
    unpacked = (uct_furiosa_rkey_t *)(uintptr_t)rkey;
    {
        uct_furiosa_rkey_t *packed = (uct_furiosa_rkey_t *)mkey_buf;

        CHECK(unpacked->bar_phys_addr == packed->bar_phys_addr,
              "round-trip bar_phys_addr: 0x%" PRIx64 " == 0x%" PRIx64,
              unpacked->bar_phys_addr, packed->bar_phys_addr);
        CHECK(unpacked->bar_offset == packed->bar_offset,
              "round-trip bar_offset: 0x%" PRIx64 " == 0x%" PRIx64,
              unpacked->bar_offset, packed->bar_offset);
        CHECK(unpacked->length == packed->length,
              "round-trip length: %zu == %zu",
              unpacked->length, packed->length);
        CHECK(unpacked->device_id == packed->device_id,
              "round-trip device_id: %u == %u",
              unpacked->device_id, packed->device_id);
    }

    /* Release */
    status = component->rkey_release(component, rkey, rkey_handle);
    CHECK(status == UCS_OK, "rkey_release returned %d", status);

    free(mkey_buf);

    /* Deregister */
    memset(&dereg_params, 0, sizeof(dereg_params));
    dereg_params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
    dereg_params.memh       = memh;
    dereg_params.flags      = 0;
    status = uct_md_mem_dereg_v2(md, &dereg_params);
    CHECK(status == UCS_OK, "mem_dereg returned %d", status);
}

/* ------------------------------------------------------------------ */
/* Test 3: Multiple regions pack to distinct rkeys                     */
/* ------------------------------------------------------------------ */
static void test_two_regions(uct_md_h md, uct_component_h component,
                             void *npu_ptr, size_t npu_len)
{
    ucs_status_t status;
    uct_mem_h memh1 = NULL;
    uct_mem_h memh2 = NULL;
    uct_furiosa_rkey_t buf1;
    uct_furiosa_rkey_t buf2;
    uct_md_mem_reg_params_t reg_params;
    uct_md_mem_dereg_params_t dereg_params;
    uct_md_mkey_pack_params_t pack_params;
    size_t half;
    void *ptr2;

    printf("\n[test_two_regions]\n");

    /* Split the NPU memory in half */
    half = npu_len / 2;
    ptr2 = (uint8_t *)npu_ptr + half;

    memset(&reg_params, 0, sizeof(reg_params));
    reg_params.field_mask = 0;

    status = uct_md_mem_reg_v2(md, npu_ptr, half, &reg_params, &memh1);
    CHECK(status == UCS_OK, "reg region1 returned %d", status);

    status = uct_md_mem_reg_v2(md, ptr2, half, &reg_params, &memh2);
    CHECK(status == UCS_OK, "reg region2 returned %d", status);

    memset(&pack_params, 0, sizeof(pack_params));
    pack_params.field_mask = 0;

    memset(&buf1, 0, sizeof(buf1));
    status = uct_md_mkey_pack_v2(md, memh1, npu_ptr, half,
                                 &pack_params, &buf1);
    CHECK(status == UCS_OK, "pack region1 returned %d", status);

    memset(&buf2, 0, sizeof(buf2));
    status = uct_md_mkey_pack_v2(md, memh2, ptr2, half,
                                 &pack_params, &buf2);
    CHECK(status == UCS_OK, "pack region2 returned %d", status);

    /* Same device, same BAR phys, but different offsets */
    CHECK(buf1.bar_phys_addr == buf2.bar_phys_addr,
          "same bar_phys_addr for both regions");
    CHECK(buf1.bar_offset != buf2.bar_offset,
          "different offsets: 0x%" PRIx64 " vs 0x%" PRIx64,
          buf1.bar_offset, buf2.bar_offset);
    CHECK(buf2.bar_offset - buf1.bar_offset == half,
          "offset delta == half: %" PRIu64 " == %zu",
          buf2.bar_offset - buf1.bar_offset, half);

    /* Cleanup */
    memset(&dereg_params, 0, sizeof(dereg_params));
    dereg_params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
    dereg_params.flags      = 0;

    dereg_params.memh = memh2;
    uct_md_mem_dereg_v2(md, &dereg_params);
    dereg_params.memh = memh1;
    uct_md_mem_dereg_v2(md, &dereg_params);
}

/* ================================================================== */

int main(void)
{
    ucs_status_t status;
    uct_component_h *components = NULL;
    unsigned num_components = 0;
    uct_component_h furiosa_comp = NULL;
    uct_md_h md = NULL;
    uct_md_config_t *md_config = NULL;
    uct_md_resource_desc_t *md_resources = NULL;
    unsigned num_md_resources = 0;
    uct_md_attr_v2_t attr;
    void *npu_ptr;
    unsigned i;

    printf("=== Phase 4: Remote key pack/unpack test ===\n");

    /* Init UCS/UCT */
    status = uct_query_components(&components, &num_components);
    assert(status == UCS_OK);

    for (i = 0; i < num_components; i++) {
        if (strcmp(components[i]->name, "furiosa") == 0) {
            furiosa_comp = components[i];
            break;
        }
    }
    if (furiosa_comp == NULL) {
        printf("SKIP: furiosa component not found\n");
        uct_release_component_list(components);
        return 77;
    }

    /* Open MD */
    status = uct_md_config_read(furiosa_comp, NULL, NULL, &md_config);
    assert(status == UCS_OK);

    status = furiosa_comp->query_md_resources(furiosa_comp,
                                               &md_resources,
                                               &num_md_resources);
    assert(status == UCS_OK && num_md_resources > 0);

    status = furiosa_comp->md_open(furiosa_comp,
                                    md_resources[0].md_name,
                                    md_config, &md);
    assert(status == UCS_OK);
    uct_config_release(md_config);
    ucs_free(md_resources);

    /* Get NPU mmap'd region for testing */
    memset(&attr, 0, sizeof(attr));
    attr.field_mask = UINT64_MAX;
    status = uct_md_query_v2(md, &attr);
    assert(status == UCS_OK);

    /* The dmabuf_addr is the mmap'd BAR4 region — use it as our NPU pointer.
     * We read it from the MD struct directly since there's no public API. */
    {
        uct_furiosa_md_t *fmd = (uct_furiosa_md_t *)md;

        npu_ptr = fmd->dmabuf_addr;
        printf("NPU mmap region: %p, size: 0x%" PRIx64 "\n",
               npu_ptr, fmd->dmabuf_size);

        test_rkey_packed_size(md);
        test_mkey_pack_unpack(md, furiosa_comp, npu_ptr, 4096);
        test_two_regions(md, furiosa_comp, npu_ptr, 8192);
    }

    /* Cleanup */
    uct_md_close(md);
    uct_release_component_list(components);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
