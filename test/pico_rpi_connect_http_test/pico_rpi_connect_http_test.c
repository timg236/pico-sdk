/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Tests for the pico_rpi_connect_http client over lwIP with the
// threadsafe-background async context: GET, POST, request and response
// headers, HTTP and transport error handling, and a soak loop.
//
// Requires an access point and the HTTP test server from host/:
//   host/wifi_ap_mode.sh start
//   host/http_test_server.py
// The defaults below match those scripts; override with -DWIFI_SSID=...,
// -DWIFI_PASSWORD=..., -DHTTP_TEST_SERVER=... at configure time.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/sem.h"
#include "pico/test.h"

#include "http_client.h"

PICOTEST_MODULE_NAME("http", "pico_rpi_connect_http test");

#ifndef WIFI_SSID
#define WIFI_SSID "pico-http-test"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "pico-http-test-pw"
#endif
#ifndef HTTP_TEST_SERVER
#define HTTP_TEST_SERVER "10.42.0.1"
#endif
#ifndef HTTP_TEST_PORT
#define HTTP_TEST_PORT 8080
#endif
// Nothing listens here: connections should be refused.
#ifndef HTTP_TEST_CLOSED_PORT
#define HTTP_TEST_CLOSED_PORT 8099
#endif
#ifndef HTTP_TEST_SOAK_ITERATIONS
#define HTTP_TEST_SOAK_ITERATIONS 50
#endif
#ifndef HTTP_TEST_REQUEST_TIMEOUT_MS
#define HTTP_TEST_REQUEST_TIMEOUT_MS 15000
#endif

#define HTTP_TEST_BODY_CAPACITY 2048
#define HTTP_TEST_HDR_CAPACITY 1024
#define HTTP_TEST_CONTENT_LEN_UNKNOWN 0xffffffffu

typedef struct {
    httpc_connection_t settings;
    httpc_state_t *state;
    semaphore_t done_sem;
    volatile bool done;
    httpc_result_t result;
    uint32_t status;            // HTTP status code from the response
    uint32_t rx_len;            // body bytes reported by the result callback
    uint32_t hdr_content_len;   // Content-Length from the headers callback
    uint16_t hdr_len;
    uint32_t body_len;
    bool overflow;
    char hdr[HTTP_TEST_HDR_CAPACITY];
    char body[HTTP_TEST_BODY_CAPACITY];
} http_test_req_t;

static err_t test_headers_fn(__unused httpc_state_t *connection, void *arg, struct pbuf *hdr,
                             u16_t hdr_len, u32_t content_len) {
    http_test_req_t *req = (http_test_req_t *)arg;
    req->hdr_content_len = content_len;
    req->hdr_len = MIN(hdr_len, sizeof(req->hdr) - 1);
    pbuf_copy_partial(hdr, req->hdr, req->hdr_len, 0);
    req->hdr[req->hdr_len] = '\0';
    return ERR_OK;
}

static err_t test_recv_fn(void *arg, struct altcp_pcb *conn, struct pbuf *p, __unused err_t err) {
    http_test_req_t *req = (http_test_req_t *)arg;
    if (req->body_len + p->tot_len <= sizeof(req->body) - 1) {
        pbuf_copy_partial(p, req->body + req->body_len, p->tot_len, 0);
        req->body_len += p->tot_len;
        req->body[req->body_len] = '\0';
    } else {
        req->overflow = true;
    }
    altcp_recved(conn, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void test_result_fn(void *arg, httpc_result_t httpc_result, u32_t rx_content_len,
                           u32_t srv_res, __unused err_t err) {
    http_test_req_t *req = (http_test_req_t *)arg;
    req->result = httpc_result;
    req->rx_len = rx_content_len;
    req->status = srv_res;
    req->state = NULL; // freed by the client after this callback returns
    req->done = true;
    sem_release(&req->done_sem);
}

static const char *test_extra_headers_fn(void *arg) {
    return (const char *)arg;
}

// Run one request to completion. Returns 0 when the transfer completed
// (successfully or with an error result - check req->result and req->status),
// -1 if the request could not be started or timed out.
static int http_test_request(http_test_req_t *req, const char *host, uint16_t port,
                             const char *uri, const char *post_data, uint16_t post_len,
                             const char *extra_headers) {
    memset(req, 0, sizeof(*req));
    sem_init(&req->done_sem, 0, 1);
    req->settings.result_fn = test_result_fn;
    req->settings.headers_done_fn = test_headers_fn;
    if (extra_headers) {
        req->settings.extra_headers_fn = test_extra_headers_fn;
        req->settings.extra_headers_arg = (void *)extra_headers;
    }
    req->hdr_content_len = HTTP_TEST_CONTENT_LEN_UNKNOWN;

    err_t ret;
    cyw43_arch_lwip_begin();
    if (post_data) {
        ret = httpc_post_file_dns(host, port, uri, &req->settings, test_recv_fn, req,
                                  post_len, post_data, &req->state);
    } else {
        ret = httpc_get_file_dns(host, port, uri, &req->settings, test_recv_fn, req, &req->state);
    }
    cyw43_arch_lwip_end();
    if (ret != ERR_OK) {
        printf("Failed to start request %s (%d)\n", uri, (int)ret);
        return -1;
    }

    if (!sem_acquire_timeout_ms(&req->done_sem, HTTP_TEST_REQUEST_TIMEOUT_MS)) {
        // The result callback may still race the timeout: re-check under the
        // lwIP lock before aborting.
        cyw43_arch_lwip_begin();
        bool timed_out = !req->done;
        if (timed_out && req->state) {
            httpc_abort(req->state);
            req->state = NULL;
        }
        cyw43_arch_lwip_end();
        if (timed_out) {
            printf("Request %s timed out\n", uri);
            return -1;
        }
    }
    return 0;
}

static int http_test_get(http_test_req_t *req, const char *uri) {
    return http_test_request(req, HTTP_TEST_SERVER, HTTP_TEST_PORT, uri, NULL, 0, NULL);
}

static int http_test_post(http_test_req_t *req, const char *uri, const char *data, uint16_t len) {
    return http_test_request(req, HTTP_TEST_SERVER, HTTP_TEST_PORT, uri, data, len, NULL);
}

// Request bodies used by the POST and soak sections.
static const char post_body[] = "hello from pico_rpi_connect_http";

// Body served by /data/<n>: byte i is 'A' + (i % 26).
static int check_data_body(const http_test_req_t *req, uint32_t len) {
    if (req->body_len != len) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (req->body[i] != (char)('A' + (i % 26))) {
            return -1;
        }
    }
    return 0;
}

int main(void) {
    stdio_init_all();

    PICOTEST_START();

    if (cyw43_arch_init()) {
        printf("Failed to initialise cyw43\n");
        return -1;
    }
    cyw43_arch_enable_sta_mode();

    printf("Connecting to '%s'\n", WIFI_SSID);
    int rc = 1;
    for (int i = 0; i < 3 && rc != 0; i++) {
        rc = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                                CYW43_AUTH_WPA2_AES_PSK, 30000);
    }
    PICOTEST_CHECK_AND_ABORT(rc == 0, "Failed to connect to WiFi");
    printf("Connected, server %s:%u\n", HTTP_TEST_SERVER, (unsigned)HTTP_TEST_PORT);

    static http_test_req_t req;

    PICOTEST_START_SECTION("GET");
    rc = http_test_get(&req, "/get");
    PICOTEST_CHECK_AND_ABORT(rc == 0, "GET /get did not complete");
    PICOTEST_CHECK(req.result == HTTPC_RESULT_OK, "GET /get transfer failed");
    PICOTEST_CHECK(req.status == 200, "GET /get status is not 200");
    PICOTEST_CHECK(strcmp(req.body, "pico-http-test-get\n") == 0, "GET /get body mismatch");
    PICOTEST_CHECK(req.hdr_content_len == req.body_len, "Content-Length does not match body");
    PICOTEST_CHECK(req.rx_len == req.body_len, "Result length does not match body");
    PICOTEST_END_SECTION();

    PICOTEST_START_SECTION("Response headers");
    rc = http_test_get(&req, "/get");
    PICOTEST_CHECK_AND_ABORT(rc == 0, "GET /get did not complete");
    PICOTEST_CHECK(strstr(req.hdr, "X-Test-Server: pico-http-test") != NULL,
                   "X-Test-Server header missing from response");
    PICOTEST_CHECK(strstr(req.hdr, "Content-Type: text/plain") != NULL,
                   "Content-Type header missing from response");
    PICOTEST_END_SECTION();

    PICOTEST_START_SECTION("POST");
    rc = http_test_post(&req, "/echo", post_body, sizeof(post_body) - 1);
    PICOTEST_CHECK_AND_ABORT(rc == 0, "POST /echo did not complete");
    PICOTEST_CHECK(req.result == HTTPC_RESULT_OK, "POST /echo transfer failed");
    PICOTEST_CHECK(req.status == 200, "POST /echo status is not 200");
    PICOTEST_CHECK(strcmp(req.body, post_body) == 0, "POST /echo body was not echoed");
    PICOTEST_END_SECTION();

    PICOTEST_START_SECTION("Request headers");
    rc = http_test_request(&req, HTTP_TEST_SERVER, HTTP_TEST_PORT, "/headers",
                           NULL, 0, "X-Test-Id: 42\r\n");
    PICOTEST_CHECK_AND_ABORT(rc == 0, "GET /headers did not complete");
    PICOTEST_CHECK(req.status == 200, "GET /headers status is not 200");
    PICOTEST_CHECK(strstr(req.body, "X-Test-Id: 42") != NULL,
                   "extra request header was not sent");
    PICOTEST_END_SECTION();

    // Transfers that carry an HTTP error complete with HTTPC_RESULT_OK; the
    // status code is reported to the result callback.
    PICOTEST_START_SECTION("HTTP errors");
    rc = http_test_get(&req, "/status/404");
    PICOTEST_CHECK_AND_ABORT(rc == 0, "GET /status/404 did not complete");
    PICOTEST_CHECK(req.status == 404, "GET /status/404 status is not 404");
    rc = http_test_get(&req, "/status/500");
    PICOTEST_CHECK_AND_ABORT(rc == 0, "GET /status/500 did not complete");
    PICOTEST_CHECK(req.status == 500, "GET /status/500 status is not 500");
    rc = http_test_get(&req, "/status/403");
    PICOTEST_CHECK_AND_ABORT(rc == 0, "GET /status/403 did not complete");
    PICOTEST_CHECK(req.status == 403, "GET /status/403 status is not 403");
    // POST to an unknown endpoint: the error status is reported for POSTs too.
    rc = http_test_post(&req, "/nonexistent", post_body, sizeof(post_body) - 1);
    PICOTEST_CHECK_AND_ABORT(rc == 0, "POST /nonexistent did not complete");
    PICOTEST_CHECK(req.status == 404, "POST /nonexistent status is not 404");
    // 204 carries no body and no Content-Length; the request must still
    // complete rather than wait for the connection to close.
    rc = http_test_get(&req, "/status/204");
    PICOTEST_CHECK_AND_ABORT(rc == 0, "GET /status/204 did not complete");
    PICOTEST_CHECK(req.result == HTTPC_RESULT_OK, "GET /status/204 transfer failed");
    PICOTEST_CHECK(req.status == 204, "GET /status/204 status is not 204");
    PICOTEST_CHECK(req.body_len == 0, "GET /status/204 returned a body");
    PICOTEST_END_SECTION();

    // The server holds the connection open after responding, so completion
    // relies on parsing the lowercase content-length header.
    PICOTEST_START_SECTION("Lowercase Content-Length");
    rc = http_test_get(&req, "/lowercase");
    PICOTEST_CHECK_AND_ABORT(rc == 0, "GET /lowercase did not complete");
    PICOTEST_CHECK(req.result == HTTPC_RESULT_OK, "GET /lowercase transfer failed");
    PICOTEST_CHECK(strcmp(req.body, "lowercase-content-length\n") == 0,
                   "GET /lowercase body mismatch");
    PICOTEST_END_SECTION();

    PICOTEST_START_SECTION("Transport errors");
    rc = http_test_request(&req, HTTP_TEST_SERVER, HTTP_TEST_CLOSED_PORT, "/get",
                           NULL, 0, NULL);
    PICOTEST_CHECK_AND_ABORT(rc == 0, "Closed-port request did not complete");
    PICOTEST_CHECK(req.result != HTTPC_RESULT_OK, "Closed-port request did not fail");
    rc = http_test_request(&req, "no-such-host.invalid", HTTP_TEST_PORT, "/get",
                           NULL, 0, NULL);
    PICOTEST_CHECK_AND_ABORT(rc == 0, "Bad-hostname request did not complete");
    PICOTEST_CHECK(req.result == HTTPC_RESULT_ERR_HOSTNAME, "Bad hostname did not fail DNS");
    PICOTEST_END_SECTION();

    PICOTEST_START_SECTION("Soak");
    int failures = 0;
    for (int i = 0; i < HTTP_TEST_SOAK_ITERATIONS; i++) {
        rc = http_test_get(&req, "/data/1024");
        if (rc != 0 || req.result != HTTPC_RESULT_OK || check_data_body(&req, 1024) != 0) {
            printf("Soak: GET /data/1024 failed at iteration %d\n", i);
            failures++;
        }
        rc = http_test_post(&req, "/echo", post_body, sizeof(post_body) - 1);
        if (rc != 0 || req.result != HTTPC_RESULT_OK || strcmp(req.body, post_body) != 0) {
            printf("Soak: POST /echo failed at iteration %d\n", i);
            failures++;
        }
        if ((i + 1) % 10 == 0) {
            printf("Soak: %d/%d iterations\n", i + 1, HTTP_TEST_SOAK_ITERATIONS);
        }
    }
    PICOTEST_CHECK(failures == 0, "Soak test had failures");
    PICOTEST_END_SECTION();

    cyw43_arch_deinit();

    PICOTEST_END_TEST();
}
