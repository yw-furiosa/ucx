/**
 * Phase 2 test: Memory Type Detection
 *
 * Verifies that detect_memory_type correctly identifies:
 *   - Host memory → NOT detected as NPU (returns error)
 *   - Address within BAR4 mmap range → detected as UCS_MEMORY_TYPE_RDMA
 *
 * Build:
 *   gcc -o test_furiosa_mem_detect test_furiosa_mem_detect.c \
 *       -I install/include -L install/lib -luct -lucs -Wl,-rpath,$PWD/install/lib
 *
 * Run:
 *   UCX_LOG_LEVEL=warn ./test_furiosa_mem_detect
 */

#include <uct/api/uct.h>
#include <ucs/type/status.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Internal header — PoC test needs access to md struct for dmabuf_addr */
#include "src/uct/furiosa/furiosa_md.h"

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
    uct_md_mem_attr_t mem_attr;
    void *host_ptr;
    void *npu_ptr;
    int failures = 0;

    printf("=== Phase 2: Memory Type Detection Test ===\n\n");

    /* Find furiosa component */
    component = find_furiosa_component();
    if (component == NULL) {
        printf("SKIP: furiosa component not found\n");
        return 77; /* autotools skip code */
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

    /* Access internal struct to get mmap'd address */
    fmd = ucs_derived_of(md, uct_furiosa_md_t);
    if (fmd->dmabuf_addr == NULL) {
        printf("SKIP: BAR4 mmap not available\n");
        uct_md_close(md);
        return 77;
    }
    printf("  INFO: BAR4 mmap addr=%p size=0x%lx\n",
           fmd->dmabuf_addr, (unsigned long)fmd->dmabuf_size);
    TEST_PASS("BAR4 mmap region available");

    printf("\n--- Test 1: Host memory should NOT be detected as NPU ---\n");
    host_ptr = malloc(4096);
    assert(host_ptr != NULL);

    mem_attr.field_mask = UCT_MD_MEM_ATTR_FIELD_MEM_TYPE;
    status = uct_md_mem_query(md, host_ptr, 4096, &mem_attr);
    if (status != UCS_OK) {
        /* Expected: furiosa MD returns error for non-NPU memory */
        TEST_PASS("host memory correctly rejected (status != UCS_OK)");
    } else if (mem_attr.mem_type != UCS_MEMORY_TYPE_RDMA) {
        TEST_PASS("host memory not detected as RDMA type");
    } else {
        TEST_FAIL("host memory incorrectly detected as NPU/RDMA memory!");
    }
    free(host_ptr);

    printf("\n--- Test 2: NPU memory (BAR4 mapped) should be detected ---\n");
    npu_ptr = fmd->dmabuf_addr;

    mem_attr.field_mask = UCT_MD_MEM_ATTR_FIELD_MEM_TYPE |
                          UCT_MD_MEM_ATTR_FIELD_BASE_ADDRESS |
                          UCT_MD_MEM_ATTR_FIELD_ALLOC_LENGTH;
    status = uct_md_mem_query(md, npu_ptr, 4096, &mem_attr);
    if (status == UCS_OK && mem_attr.mem_type == UCS_MEMORY_TYPE_RDMA) {
        TEST_PASS("NPU memory detected as UCS_MEMORY_TYPE_RDMA");
    } else {
        printf("  status=%d mem_type=%d\n", status,
               (status == UCS_OK) ? mem_attr.mem_type : -1);
        TEST_FAIL("NPU memory NOT detected as RDMA type");
    }

    if (status == UCS_OK) {
        if (mem_attr.base_address == fmd->dmabuf_addr) {
            TEST_PASS("base_address matches BAR4 mmap addr");
        } else {
            TEST_FAIL("base_address mismatch");
        }
        if (mem_attr.alloc_length == (size_t)fmd->dmabuf_size) {
            TEST_PASS("alloc_length matches BAR4 mmap size");
        } else {
            TEST_FAIL("alloc_length mismatch");
        }
    }

    printf("\n--- Test 3: Address at end of BAR4 range (boundary) ---\n");
    npu_ptr = (uint8_t *)fmd->dmabuf_addr + fmd->dmabuf_size - 1;

    mem_attr.field_mask = UCT_MD_MEM_ATTR_FIELD_MEM_TYPE;
    status = uct_md_mem_query(md, npu_ptr, 1, &mem_attr);
    if (status == UCS_OK && mem_attr.mem_type == UCS_MEMORY_TYPE_RDMA) {
        TEST_PASS("last byte of BAR4 range detected as NPU");
    } else {
        TEST_FAIL("last byte of BAR4 range NOT detected");
    }

    printf("\n--- Test 4: Address just past BAR4 range (out of bounds) ---\n");
    npu_ptr = (uint8_t *)fmd->dmabuf_addr + fmd->dmabuf_size;

    mem_attr.field_mask = UCT_MD_MEM_ATTR_FIELD_MEM_TYPE;
    status = uct_md_mem_query(md, npu_ptr, 1, &mem_attr);
    if (status != UCS_OK) {
        TEST_PASS("address past BAR4 range correctly rejected");
    } else if (mem_attr.mem_type != UCS_MEMORY_TYPE_RDMA) {
        TEST_PASS("address past BAR4 range not detected as RDMA");
    } else {
        TEST_FAIL("address past BAR4 range incorrectly detected as NPU!");
    }

    /* Cleanup */
    uct_md_close(md);

    printf("\n=== Result: %s (%d failure%s) ===\n",
           failures == 0 ? "ALL PASSED" : "FAILED",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
