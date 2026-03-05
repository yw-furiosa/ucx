/**
 * Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 *
 * Furiosa NPU remote data transfer test — UCP-based client/server.
 *
 * This test verifies host ↔ remote-NPU data transfer using UCP RMA (put/get).
 * The same test binary works as both server and client.
 *
 * Data flow (Approach A — UCP with TCP + furiosa transports):
 *   Client host_buf  →  TCP  →  Server host staging  →  memcpy  →  Server NPU BAR4
 *
 * When Approach B (native furiosa RDMA) is implemented, UCP will automatically
 * route through it instead.  This test code does NOT need to change.
 *
 * Build:
 *   gcc -DNVALGRIND -o test_furiosa_remote test_furiosa_remote.c \
 *       -I src -I install/include -I . \
 *       -L install/lib -lucp -luct -lucs -Wl,-rpath,$PWD/install/lib
 *
 * Run (two terminals / two nodes):
 *   Server:  UCX_FURIOSA_DEVICE_ID=6 ./test_furiosa_remote
 *   Client:  UCX_FURIOSA_DEVICE_ID=6 ./test_furiosa_remote <server-ip>
 */

#include <ucp/api/ucp.h>
#include <ucs/type/status.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>

/* Internal header — access dmabuf_addr for NPU memory pointer */
#include "src/uct/furiosa/furiosa_md.h"

/* ────────────────── Configuration ────────────────── */

#define OOB_PORT        19876          /* TCP port for out-of-band exchange   */
#define TEST_SIZE_SMALL 64             /* 64-byte transfer test               */
#define TEST_SIZE_LARGE (1024 * 1024)  /* 1 MB transfer test                  */
#define PATTERN_SEED    0xA5           /* fill pattern for verification       */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  PASS: " fmt "\n", ##__VA_ARGS__);                    \
            g_pass++;                                                       \
        } else {                                                            \
            printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                    \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

/* ────────────────── OOB TCP helpers ────────────────── */

/**
 * Send exactly @len bytes over a socket.
 */
static int oob_send(int sock, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    ssize_t n;

    while (len > 0) {
        n = send(sock, p, len, 0);
        if (n <= 0) {
            perror("oob_send");
            return -1;
        }
        p   += n;
        len -= n;
    }
    return 0;
}

/**
 * Receive exactly @len bytes from a socket.
 */
static int oob_recv(int sock, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    ssize_t n;

    while (len > 0) {
        n = recv(sock, p, len, MSG_WAITALL);
        if (n <= 0) {
            perror("oob_recv");
            return -1;
        }
        p   += n;
        len -= n;
    }
    return 0;
}

/**
 * Send a variable-length blob: [uint64_t length][payload]
 */
static int oob_send_blob(int sock, const void *data, uint64_t len)
{
    if (oob_send(sock, &len, sizeof(len)) != 0) return -1;
    if (len > 0 && oob_send(sock, data, len) != 0) return -1;
    return 0;
}

/**
 * Receive a variable-length blob.  Caller must free(*out).
 */
static int oob_recv_blob(int sock, void **out, uint64_t *out_len)
{
    uint64_t len;

    if (oob_recv(sock, &len, sizeof(len)) != 0) return -1;
    *out_len = len;
    if (len == 0) {
        *out = NULL;
        return 0;
    }
    *out = malloc(len);
    if (*out == NULL) return -1;
    if (oob_recv(sock, *out, len) != 0) {
        free(*out);
        *out = NULL;
        return -1;
    }
    return 0;
}

/**
 * Server: listen, accept one client.
 */
static int oob_server_accept(void)
{
    int listen_fd, client_fd, optval = 1;
    struct sockaddr_in addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(OOB_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    listen(listen_fd, 1);
    printf("OOB: listening on port %d ...\n", OOB_PORT);

    client_fd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (client_fd < 0) {
        perror("accept");
        return -1;
    }
    printf("OOB: client connected.\n");
    return client_fd;
}

/**
 * Client: connect to server.
 */
static int oob_client_connect(const char *server_name)
{
    struct addrinfo hints, *res;
    char port_str[16];
    int fd, ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", OOB_PORT);

    ret = getaddrinfo(server_name, port_str, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", server_name, gai_strerror(ret));
        return -1;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        perror("socket");
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    printf("OOB: connected to %s:%d\n", server_name, OOB_PORT);
    return fd;
}

/* ────────────────── UCP helpers ────────────────── */

/**
 * Blocking flush on an endpoint.
 */
static ucs_status_t flush_ep(ucp_worker_h worker, ucp_ep_h ep)
{
    ucp_request_param_t param;
    void *request;
    ucs_status_t status;

    param.op_attr_mask = 0;
    request = ucp_ep_flush_nbx(ep, &param);
    if (request == NULL) {
        return UCS_OK;
    } else if (UCS_PTR_IS_ERR(request)) {
        return UCS_PTR_STATUS(request);
    }

    do {
        ucp_worker_progress(worker);
        status = ucp_request_check_status(request);
    } while (status == UCS_INPROGRESS);

    ucp_request_free(request);
    return status;
}

/**
 * Close endpoint and wait for completion.
 */
static void ep_close(ucp_worker_h worker, ucp_ep_h ep)
{
    ucp_request_param_t param;
    ucs_status_t status;
    void *request;

    param.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
    param.flags        = 0;  /* graceful close */
    request = ucp_ep_close_nbx(ep, &param);
    if (UCS_PTR_IS_PTR(request)) {
        do {
            ucp_worker_progress(worker);
            status = ucp_request_check_status(request);
        } while (status == UCS_INPROGRESS);
        ucp_request_free(request);
    }
}

/* ────────────────── Test logic ────────────────── */

/**
 * Fill buffer with deterministic pattern based on offset.
 */
static void fill_pattern(void *buf, size_t len, uint8_t seed)
{
    uint8_t *p = (uint8_t *)buf;
    size_t i;

    for (i = 0; i < len; i++) {
        p[i] = (uint8_t)((i * 7 + seed) & 0xFF);
    }
}

/**
 * Verify buffer matches expected pattern.
 */
static int verify_pattern(const void *buf, size_t len, uint8_t seed)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t i;

    for (i = 0; i < len; i++) {
        if (p[i] != (uint8_t)((i * 7 + seed) & 0xFF)) {
            fprintf(stderr, "    mismatch at offset %zu: got 0x%02x expected 0x%02x\n",
                    i, p[i], (uint8_t)((i * 7 + seed) & 0xFF));
            return -1;
        }
    }
    return 0;
}

/**
 * Run the test suite.
 *
 * @param is_server   1 = server (owns NPU memory), 0 = client (initiates RMA)
 * @param oob_sock    TCP socket for out-of-band exchange
 */
static int run_test(int is_server, int oob_sock)
{
    ucp_params_t ucp_params;
    ucp_config_t *config;
    ucp_context_h context;
    ucp_worker_params_t worker_params;
    ucp_worker_h worker;
    ucp_worker_attr_t worker_attr;
    ucp_ep_params_t ep_params;
    ucp_ep_h ep;
    ucs_status_t status;
    int ret = -1;

    /* ── Step 1: UCP init ── */
    status = ucp_config_read(NULL, NULL, &config);
    assert(status == UCS_OK);

    memset(&ucp_params, 0, sizeof(ucp_params));
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_NAME;
    ucp_params.features   = UCP_FEATURE_RMA;
    ucp_params.name       = "furiosa_remote_test";

    status = ucp_init(&ucp_params, config, &context);
    ucp_config_release(config);
    if (status != UCS_OK) {
        fprintf(stderr, "ucp_init failed: %s\n", ucs_status_string(status));
        return -1;
    }

    memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    status = ucp_worker_create(context, &worker_params, &worker);
    assert(status == UCS_OK);

    /* ── Step 2: Exchange worker addresses (OOB) ── */
    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    status = ucp_worker_query(worker, &worker_attr);
    assert(status == UCS_OK);

    printf("UCP: local worker address length: %zu\n", worker_attr.address_length);

    /* Send our address, receive peer's address */
    void *peer_addr = NULL;
    uint64_t peer_addr_len = 0;

    if (oob_send_blob(oob_sock, worker_attr.address,
                      worker_attr.address_length) != 0) {
        goto err_worker;
    }
    if (oob_recv_blob(oob_sock, &peer_addr, &peer_addr_len) != 0) {
        goto err_worker;
    }
    printf("UCP: received peer worker address (%"PRIu64" bytes)\n", peer_addr_len);

    /* ── Step 3: Create endpoint to peer ── */
    memset(&ep_params, 0, sizeof(ep_params));
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address    = (ucp_address_t *)peer_addr;

    status = ucp_ep_create(worker, &ep_params, &ep);
    if (status != UCS_OK) {
        fprintf(stderr, "ucp_ep_create failed: %s\n", ucs_status_string(status));
        goto err_peer_addr;
    }
    printf("UCP: endpoint created.\n");

    if (is_server) {
        /* ────────── SERVER: register NPU memory, send rkey ────────── */

        /* Open furiosa MD to get NPU BAR4 mmap address.
         * We use the UCT layer directly just to get the mmap pointer.
         * In production, the NPU runtime allocator would provide this. */
        uct_component_h *components = NULL;
        unsigned num_components = 0;
        uct_component_h furiosa_comp = NULL;
        uct_md_config_t *md_config = NULL;
        uct_md_h uct_md = NULL;
        uct_furiosa_md_t *fmd;
        unsigned i;

        status = uct_query_components(&components, &num_components);
        assert(status == UCS_OK);

        for (i = 0; i < num_components; i++) {
            if (strcmp(components[i]->name, "furiosa") == 0) {
                furiosa_comp = components[i];
                break;
            }
        }
        if (furiosa_comp == NULL) {
            fprintf(stderr, "FATAL: furiosa component not found\n");
            uct_release_component_list(components);
            goto err_ep;
        }

        status = uct_md_config_read(furiosa_comp, NULL, NULL, &md_config);
        assert(status == UCS_OK);
        status = uct_md_open(furiosa_comp, "furiosa", md_config, &uct_md);
        uct_config_release(md_config);
        if (status != UCS_OK) {
            fprintf(stderr, "FATAL: cannot open furiosa MD: %s\n",
                    ucs_status_string(status));
            uct_release_component_list(components);
            goto err_ep;
        }
        uct_release_component_list(components);

        fmd = (uct_furiosa_md_t *)uct_md;
        void *npu_buf   = fmd->dmabuf_addr;
        size_t npu_size = fmd->dmabuf_size;
        printf("SERVER: NPU BAR4 mmap addr=%p size=0x%zx device_id=%u\n",
               npu_buf, npu_size, fmd->device_id);

        if (npu_buf == NULL) {
            fprintf(stderr, "FATAL: NPU BAR4 mmap not available\n");
            uct_md_close(uct_md);
            goto err_ep;
        }

        /* Clear NPU memory */
        memset(npu_buf, 0, TEST_SIZE_LARGE);

        /* Register NPU memory with UCP for remote access */
        ucp_mem_map_params_t mem_params;
        ucp_mem_h memh;
        void *rkey_buf = NULL;
        size_t rkey_buf_size = 0;

        memset(&mem_params, 0, sizeof(mem_params));
        mem_params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                                UCP_MEM_MAP_PARAM_FIELD_LENGTH;
        mem_params.address    = npu_buf;
        mem_params.length     = npu_size;

        status = ucp_mem_map(context, &mem_params, &memh);
        if (status != UCS_OK) {
            fprintf(stderr, "ucp_mem_map failed: %s\n", ucs_status_string(status));
            uct_md_close(uct_md);
            goto err_ep;
        }
        printf("SERVER: NPU memory registered with UCP.\n");

        /* Pack rkey for client */
        status = ucp_rkey_pack(context, memh, &rkey_buf, &rkey_buf_size);
        assert(status == UCS_OK);
        printf("SERVER: rkey packed (%zu bytes)\n", rkey_buf_size);

        /* Send rkey + NPU base address to client via OOB */
        uint64_t remote_addr = (uint64_t)(uintptr_t)npu_buf;
        oob_send_blob(oob_sock, rkey_buf, rkey_buf_size);
        oob_send(oob_sock, &remote_addr, sizeof(remote_addr));
        printf("SERVER: sent rkey and remote NPU address to client.\n");

        ucp_rkey_buffer_release(rkey_buf);

        /* Wait for client to signal test completion.
         * CRITICAL: must keep calling ucp_worker_progress() so UCP can
         * process incoming RMA operations from the client. */
        uint8_t done = 0;
        printf("SERVER: waiting for client to complete tests ...\n");
        {
            int flags = fcntl(oob_sock, F_GETFL, 0);
            fcntl(oob_sock, F_SETFL, flags | O_NONBLOCK);

            while (!done) {
                ucp_worker_progress(worker);

                ssize_t n = recv(oob_sock, &done, sizeof(done), 0);
                if (n == sizeof(done)) {
                    break;
                }
            }

            fcntl(oob_sock, F_SETFL, flags);
        }

        /* ── Verify: check NPU memory was written by client ── */
        printf("\n[server_verify_put_small]\n");
        CHECK(verify_pattern(npu_buf, TEST_SIZE_SMALL, PATTERN_SEED) == 0,
              "NPU memory matches client's %d-byte put pattern", TEST_SIZE_SMALL);

        printf("\n[server_verify_put_large]\n");
        /* Large test writes at offset TEST_SIZE_SMALL */
        void *large_region = (uint8_t *)npu_buf + TEST_SIZE_SMALL;
        CHECK(verify_pattern(large_region, TEST_SIZE_LARGE, PATTERN_SEED + 1) == 0,
              "NPU memory matches client's %d-byte put pattern", TEST_SIZE_LARGE);

        /* Signal server verification done */
        done = 1;
        oob_send(oob_sock, &done, sizeof(done));

        /* Cleanup */
        ucp_mem_unmap(context, memh);
        uct_md_close(uct_md);

    } else {
        /* ────────── CLIENT: receive rkey, do put/get ────────── */

        /* Receive rkey + remote NPU address from server */
        void *rkey_buf = NULL;
        uint64_t rkey_buf_len = 0;
        uint64_t remote_addr = 0;
        ucp_rkey_h rkey;

        oob_recv_blob(oob_sock, &rkey_buf, &rkey_buf_len);
        oob_recv(oob_sock, &remote_addr, sizeof(remote_addr));
        printf("CLIENT: received rkey (%"PRIu64" bytes) remote_addr=0x%"PRIx64"\n",
               rkey_buf_len, remote_addr);

        status = ucp_ep_rkey_unpack(ep, rkey_buf, &rkey);
        free(rkey_buf);
        if (status != UCS_OK) {
            fprintf(stderr, "ucp_ep_rkey_unpack failed: %s\n",
                    ucs_status_string(status));
            goto err_ep;
        }
        printf("CLIENT: rkey unpacked.\n");

        /* ── Test 1: Small put (64 bytes) ── */
        printf("\n[test_put_small]\n");
        {
            void *send_buf = malloc(TEST_SIZE_SMALL);
            assert(send_buf != NULL);
            fill_pattern(send_buf, TEST_SIZE_SMALL, PATTERN_SEED);

            status = ucp_put_nbi(ep, send_buf, TEST_SIZE_SMALL, remote_addr, rkey);
            CHECK(status == UCS_OK || status == UCS_INPROGRESS,
                  "ucp_put_nbi(%d bytes) returned %s",
                  TEST_SIZE_SMALL, ucs_status_string(status));

            status = flush_ep(worker, ep);
            CHECK(status == UCS_OK, "flush after put_small: %s",
                  ucs_status_string(status));

            free(send_buf);
        }

        /* ── Test 2: Small get (64 bytes) — read back what we just put ── */
        printf("\n[test_get_small]\n");
        {
            void *recv_buf = calloc(1, TEST_SIZE_SMALL);
            assert(recv_buf != NULL);

            status = ucp_get_nbi(ep, recv_buf, TEST_SIZE_SMALL, remote_addr, rkey);
            CHECK(status == UCS_OK || status == UCS_INPROGRESS,
                  "ucp_get_nbi(%d bytes) returned %s",
                  TEST_SIZE_SMALL, ucs_status_string(status));

            status = flush_ep(worker, ep);
            CHECK(status == UCS_OK, "flush after get_small: %s",
                  ucs_status_string(status));

            CHECK(verify_pattern(recv_buf, TEST_SIZE_SMALL, PATTERN_SEED) == 0,
                  "get_small data matches put pattern (%d bytes)", TEST_SIZE_SMALL);

            free(recv_buf);
        }

        /* ── Test 3: Large put (1 MB) at offset ── */
        printf("\n[test_put_large]\n");
        {
            uint64_t large_addr = remote_addr + TEST_SIZE_SMALL;
            void *send_buf = malloc(TEST_SIZE_LARGE);
            assert(send_buf != NULL);
            fill_pattern(send_buf, TEST_SIZE_LARGE, PATTERN_SEED + 1);

            status = ucp_put_nbi(ep, send_buf, TEST_SIZE_LARGE, large_addr, rkey);
            CHECK(status == UCS_OK || status == UCS_INPROGRESS,
                  "ucp_put_nbi(%d bytes) returned %s",
                  TEST_SIZE_LARGE, ucs_status_string(status));

            status = flush_ep(worker, ep);
            CHECK(status == UCS_OK, "flush after put_large: %s",
                  ucs_status_string(status));

            free(send_buf);
        }

        /* ── Test 4: Large get (1 MB) — read back ── */
        printf("\n[test_get_large]\n");
        {
            uint64_t large_addr = remote_addr + TEST_SIZE_SMALL;
            void *recv_buf = calloc(1, TEST_SIZE_LARGE);
            assert(recv_buf != NULL);

            status = ucp_get_nbi(ep, recv_buf, TEST_SIZE_LARGE, large_addr, rkey);
            CHECK(status == UCS_OK || status == UCS_INPROGRESS,
                  "ucp_get_nbi(%d bytes) returned %s",
                  TEST_SIZE_LARGE, ucs_status_string(status));

            status = flush_ep(worker, ep);
            CHECK(status == UCS_OK, "flush after get_large: %s",
                  ucs_status_string(status));

            CHECK(verify_pattern(recv_buf, TEST_SIZE_LARGE, PATTERN_SEED + 1) == 0,
                  "get_large data matches put pattern (%d bytes)", TEST_SIZE_LARGE);

            free(recv_buf);
        }

        /* Signal server that tests are done */
        uint8_t done = 1;
        oob_send(oob_sock, &done, sizeof(done));

        /* Wait for server verification */
        oob_recv(oob_sock, &done, sizeof(done));

        ucp_rkey_destroy(rkey);
    }

    ret = 0;

err_ep:
    ep_close(worker, ep);

err_peer_addr:
    free(peer_addr);

err_worker:
    ucp_worker_release_address(worker, worker_attr.address);
    ucp_worker_destroy(worker);
    ucp_cleanup(context);

    return ret;
}

/* ────────────────── Main ────────────────── */

int main(int argc, char **argv)
{
    int is_server = (argc < 2);
    int oob_sock;

    printf("=== Furiosa Remote Data Transfer Test ===\n");
    printf("Mode: %s\n\n", is_server ? "SERVER" : "CLIENT");

    if (is_server) {
        oob_sock = oob_server_accept();
    } else {
        oob_sock = oob_client_connect(argv[1]);
    }

    if (oob_sock < 0) {
        return 1;
    }

    int ret = run_test(is_server, oob_sock);
    close(oob_sock);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (ret != 0 || g_fail > 0) ? 1 : 0;
}
