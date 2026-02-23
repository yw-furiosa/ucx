/**
 * Phase 3 test: Memory Registration (mem_reg / mem_dereg)
 *
 * Verifies that:
 *   1. Registration of NPU memory within BAR4 mmap range succeeds
 *   2. The returned memh contains correct bar_offset, address, length, device_id
 *   3. Registration of host memory (outside BAR4) is rejected
 *   4. Registration of address past BAR4 end is rejected
 *   5. Deregistration succeeds and frees the handle
 *   6. MD query advertises UCT_MD_FLAG_REG
 *
 * Build:
 *   gcc -o test_furiosa_mem_reg test_furiosa_mem_reg.c \
 *       -I src -I install/include -I . -L install/lib -luct -lucs -Wl,-rpath,$PWD/install/lib
 *
 * Run:
 *   UCX_LOG_LEVEL=warn ./test_furiosa_mem_reg
 */

#include <uct/api/uct.h>
#include <uct/api/v2/uct_v2.h>
#include <ucs/type/status.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include "src/uct/furiosa/furiosa_md.h"

#define NPU_BAR4_RESERVED_SIZE 0x10000000ULL

#define TEST_PASS(msg) printf("  PASS: %s\n", (msg))
#define TEST_FAIL(msg) do { printf("  FAIL: %s\n", (msg)); failures++; } while(0)

static uct_component_h find_furiosa_component(void)
{
    uct_component_h *components;
    uct_component_h result = NULL;
    uct_component_attr_t attr;
    unsigned num_components;
    unsigned i;
    ucs_status_t status;

    status = uct_query_components(&components, &num_components);
    if (status != UCS_OK) {
        return NULL;
    }

    for (i = 0; i < num_components; i++) {
        attr.field_mask = UCT_COMPONENT_ATTR_FIELD_NAME;
        status = uct_component_query(components[i], &attr);
        if (status != UCS_OK) {
            continue;
        }
        if (strcmp(attr.name, "furiosa") == 0) {
            result = components[i];
            break;
        }
    }

    uct_release_component_list(components);
    return result;
}

int main(int argc, char **argv)
{
    uct_component_h component;
    uct_md_config_t *md_config;
    uct_md_h md;
    uct_furiosa_md_t *fmd;
    ucs_status_t status;
    uct_md_attr_v2_t md_attr;
    uct_md_mem_reg_params_t reg_params;
    uct_md_mem_dereg_params_t dereg_params;
    uct_mem_h memh;
    uct_furiosa_mem_t *fmem;
    void *npu_ptr;
    void *host_ptr;
    uint64_t expected_offset;
    int failures = 0;

    printf("=== Phase 3: Memory Registration Test ===\n\n");

    /* Find furiosa component */
    component = find_furiosa_component();
    if (component == NULL) {
        printf("SKIP: furiosa component not found\n");
        return 77;
    }
    TEST_PASS("furiosa component found");

    /* Open MD */
    status = uct_md_config_read(component, NULL, NULL, &md_config);
    assert(status == UCS_OK);

    status = uct_md_open(component, "furiosa", md_config, &md);
    uct_config_release(md_config);
    if (status != UCS_OK) {
        printf("SKIP: furiosa MD open failed (no NPU device?)\n");
        return 77;
    }
    TEST_PASS("furiosa MD opened");

    /* Check internal state */
    fmd = ucs_derived_of(md, uct_furiosa_md_t);
    if (fmd->dmabuf_addr == NULL) {
        printf("SKIP: BAR4 mmap not available\n");
        uct_md_close(md);
        return 77;
    }
    printf("  INFO: BAR4 mmap addr=%p size=0x%lx dev=%u\n",
           fmd->dmabuf_addr, (unsigned long)fmd->dmabuf_size, fmd->device_id);
    TEST_PASS("BAR4 mmap region available");

    /* --- Test 1: MD query advertises UCT_MD_FLAG_REG --- */
    printf("\n--- Test 1: MD query advertises registration capability ---\n");
    md_attr.field_mask = UCT_MD_ATTR_FIELD_FLAGS | UCT_MD_ATTR_FIELD_REG_MEM_TYPES;
    status = uct_md_query_v2(md, &md_attr);
    assert(status == UCS_OK);

    if (md_attr.flags & UCT_MD_FLAG_REG) {
        TEST_PASS("UCT_MD_FLAG_REG is set");
    } else {
        TEST_FAIL("UCT_MD_FLAG_REG is NOT set");
    }

    if (md_attr.reg_mem_types & UCS_BIT(UCS_MEMORY_TYPE_RDMA)) {
        TEST_PASS("reg_mem_types includes RDMA");
    } else {
        TEST_FAIL("reg_mem_types does NOT include RDMA");
    }

    /* --- Test 2: Register NPU memory (start of BAR4 range) --- */
    printf("\n--- Test 2: Register NPU memory at start of BAR4 range ---\n");
    npu_ptr = fmd->dmabuf_addr;
    memset(&reg_params, 0, sizeof(reg_params));
    reg_params.field_mask = 0;

    status = uct_md_mem_reg_v2(md, npu_ptr, 4096, &reg_params, &memh);
    if (status == UCS_OK) {
        TEST_PASS("mem_reg succeeded for BAR4 start");

        fmem = (uct_furiosa_mem_t *)memh;
        expected_offset = NPU_BAR4_RESERVED_SIZE; /* 0 offset from mmap start + reserved */

        if (fmem->address == npu_ptr) {
            TEST_PASS("memh->address matches input");
        } else {
            TEST_FAIL("memh->address mismatch");
        }

        if (fmem->length == 4096) {
            TEST_PASS("memh->length is 4096");
        } else {
            printf("  got length=%zu\n", fmem->length);
            TEST_FAIL("memh->length mismatch");
        }

        if (fmem->bar_offset == expected_offset) {
            TEST_PASS("memh->bar_offset matches expected");
        } else {
            printf("  expected=0x%" PRIx64 " got=0x%" PRIx64 "\n",
                   expected_offset, fmem->bar_offset);
            TEST_FAIL("memh->bar_offset mismatch");
        }

        if (fmem->device_id == fmd->device_id) {
            TEST_PASS("memh->device_id matches MD device_id");
        } else {
            TEST_FAIL("memh->device_id mismatch");
        }

        /* Deregister */
        memset(&dereg_params, 0, sizeof(dereg_params));
        dereg_params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
        dereg_params.memh       = memh;
        status = uct_md_mem_dereg_v2(md, &dereg_params);
        if (status == UCS_OK) {
            TEST_PASS("mem_dereg succeeded");
        } else {
            TEST_FAIL("mem_dereg failed");
        }
    } else {
        printf("  status=%d\n", status);
        TEST_FAIL("mem_reg failed for BAR4 start");
    }

    /* --- Test 3: Register NPU memory at an offset within BAR4 --- */
    printf("\n--- Test 3: Register NPU memory at 1MB offset ---\n");
    npu_ptr = (uint8_t *)fmd->dmabuf_addr + (1 << 20); /* +1MB */
    memset(&reg_params, 0, sizeof(reg_params));
    reg_params.field_mask = 0;

    status = uct_md_mem_reg_v2(md, npu_ptr, 4096, &reg_params, &memh);
    if (status == UCS_OK) {
        TEST_PASS("mem_reg at 1MB offset succeeded");

        fmem = (uct_furiosa_mem_t *)memh;
        expected_offset = NPU_BAR4_RESERVED_SIZE + (1 << 20);

        if (fmem->bar_offset == expected_offset) {
            TEST_PASS("bar_offset correct for 1MB offset");
        } else {
            printf("  expected=0x%" PRIx64 " got=0x%" PRIx64 "\n",
                   expected_offset, fmem->bar_offset);
            TEST_FAIL("bar_offset wrong for 1MB offset");
        }

        memset(&dereg_params, 0, sizeof(dereg_params));
        dereg_params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
        dereg_params.memh       = memh;
        uct_md_mem_dereg_v2(md, &dereg_params);
    } else {
        TEST_FAIL("mem_reg at 1MB offset failed");
    }

    /* --- Test 4: Host memory passthrough registration --- */
    printf("\n--- Test 4: Host memory registration (passthrough) ---\n");
    host_ptr = malloc(4096);
    assert(host_ptr != NULL);
    memset(&reg_params, 0, sizeof(reg_params));
    reg_params.field_mask = 0;
    status = uct_md_mem_reg_v2(md, host_ptr, 4096, &reg_params, &memh);
    if (status == UCS_OK) {
        uct_furiosa_mem_t *hmem = (uct_furiosa_mem_t *)memh;
        if (hmem->bar_offset == 0) {
            TEST_PASS("host memory registered with bar_offset=0 (passthrough)");
        } else {
            TEST_FAIL("host memory bar_offset != 0");
        }
        memset(&dereg_params, 0, sizeof(dereg_params));
        dereg_params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
        dereg_params.memh       = memh;
        uct_md_mem_dereg_v2(md, &dereg_params);
    } else {
        TEST_FAIL("host memory registration failed (expected passthrough)");
    }
    free(host_ptr);
    /* --- Test 5: Address past BAR4 end (passthrough) --- */
    printf("\n--- Test 5: Address past BAR4 end (passthrough) ---\n");
    npu_ptr = (uint8_t *)fmd->dmabuf_addr + fmd->dmabuf_size;
    memset(&reg_params, 0, sizeof(reg_params));
    reg_params.field_mask = 0;

    status = uct_md_mem_reg_v2(md, npu_ptr, 4096, &reg_params, &memh);
    if (status == UCS_OK) {
        uct_furiosa_mem_t *hmem = (uct_furiosa_mem_t *)memh;
        if (hmem->bar_offset == 0) {
            TEST_PASS("address past BAR4 registered as passthrough");
        } else {
            TEST_FAIL("address past BAR4 bar_offset != 0");
        }
        memset(&dereg_params, 0, sizeof(dereg_params));
        dereg_params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
        dereg_params.memh       = memh;
        uct_md_mem_dereg_v2(md, &dereg_params);
    } else {
        TEST_FAIL("address past BAR4 registration failed (expected passthrough)");
    }

    /* --- Test 6: Region spanning past BAR4 end (partial overlap → error) --- */
    printf("\n--- Test 6: Region spanning past BAR4 end (partial overlap) ---\n");
    npu_ptr = (uint8_t *)fmd->dmabuf_addr + fmd->dmabuf_size - 2048;
    memset(&reg_params, 0, sizeof(reg_params));
    reg_params.field_mask = 0;

    status = uct_md_mem_reg_v2(md, npu_ptr, 4096, &reg_params, &memh);
    if (status == UCS_ERR_INVALID_PARAM) {
        TEST_PASS("spanning region correctly rejected with UCS_ERR_INVALID_PARAM");
    } else if (status == UCS_OK) {
        TEST_FAIL("spanning region should have been rejected, got UCS_OK");
        memset(&dereg_params, 0, sizeof(dereg_params));
        dereg_params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
        dereg_params.memh       = memh;
        uct_md_mem_dereg_v2(md, &dereg_params);
    } else {
        TEST_FAIL("spanning region returned unexpected status");
    }

    /* Cleanup */
    uct_md_close(md);

    printf("\n=== Result: %s (%d failure%s) ===\n",
           failures == 0 ? "ALL PASSED" : "FAILED",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
