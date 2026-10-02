/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Upstream's canonical header name is <microhttpd.h> on all versions;
   the "libmicrohttpd.h" alias was dropped by Debian's 1.0.1 package. */
#include <microhttpd.h>
#include <curl/curl.h>

/*
 * libmicrohttpd 1.0 changed the result type of every callback from
 * "int" to the distinct enum type "enum MHD_Result".  Passing an
 * int-returning function where an MHD_Result-returning one is
 * expected is an incompatible-pointer-type error under modern
 * compilers, so the result type is selected per MHD_VERSION.
 */
#if defined(MHD_VERSION) && MHD_VERSION >= 0x01000000
typedef enum MHD_Result mhd_result_t;
#else
typedef int mhd_result_t;
#endif

/*
 * MHD_USE_INTERNAL_POLLING_THREAD replaced the deprecated
 * MHD_USE_SELECT_INTERNALLY (both select the same internal polling
 * thread mode); older 0.9.x headers only know the latter.
 */
#ifndef MHD_USE_INTERNAL_POLLING_THREAD
#define MHD_USE_INTERNAL_POLLING_THREAD MHD_USE_SELECT_INTERNALLY
#endif

unsigned char test_body[TEST_BODY_SIZE];

struct test_server {
    struct MHD_Daemon *daemon;
};

static void test_body_init(void)
{
    unsigned int x = 12345;
    size_t i;

    for (i = 0; i < TEST_BODY_SIZE; i++) {
        x = x * 1103515245u + 12345u;
        test_body[i] = (unsigned char)(x >> 16);
    }
}

static mhd_result_t answer_cb(void *cls, struct MHD_Connection *conn,
                              const char *url, const char *method,
                              const char *version, const char *upload_data,
                              size_t *upload_data_size, void **con_cls)
{
    struct MHD_Response *resp;
    const char *range;
    unsigned long long start, end;
    char content_range[64];
    int nfields;

    (void) cls;
    (void) url;
    (void) version;
    (void) upload_data;
    (void) upload_data_size;
    (void) con_cls;

    /* HEAD is honored: it is the metadata probe used by httpfs
       (Phase 2); MHD suppresses the body by itself. */
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
        return MHD_NO;

    range = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Range");
    if (range == NULL) {
        /* Full body. */
        resp = MHD_create_response_from_buffer(TEST_BODY_SIZE, test_body,
                                                MHD_RESPMEM_MUST_COPY);
        if (resp == NULL)
            return MHD_NO;
        MHD_add_response_header(resp, "Accept-Ranges", "bytes");
        MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return MHD_YES;
    }

    if (strncmp(range, "bytes=", 6) != 0)
        return MHD_NO; /* unsupported range unit */

    range += 6;
    if (strchr(range, ',') != NULL)
        return MHD_NO; /* multi-range requests are not supported */

    nfields = sscanf(range, "%llu-%llu", &start, &end);
    if (nfields < 1)
        return MHD_NO; /* malformed range (no start offset) */

    if (start >= TEST_BODY_SIZE) {
        /* 416 Requested Range Not Satisfiable */
        resp = MHD_create_response_from_buffer(0, (void *) "", MHD_RESPMEM_PERSISTENT);
        if (resp == NULL)
            return MHD_NO;
        if (snprintf(content_range, sizeof(content_range),
                     "bytes */%zu", (size_t) TEST_BODY_SIZE)
            >= (int) sizeof(content_range)) {
            MHD_destroy_response(resp);
            return MHD_NO;
        }
        MHD_add_response_header(resp, "Content-Range", content_range);
        MHD_queue_response(conn, 416, resp);
        MHD_destroy_response(resp);
        return MHD_YES;
    }

    /* An absent end offset means "through the end of the body"; an end
       offset beyond the end of the body is clamped. */
    if (nfields < 2 || end >= TEST_BODY_SIZE || end < start)
        end = TEST_BODY_SIZE - 1;

    resp = MHD_create_response_from_buffer((size_t)(end - start + 1),
                                           test_body + start,
                                           MHD_RESPMEM_MUST_COPY);
    if (resp == NULL)
        return MHD_NO;
    if (snprintf(content_range, sizeof(content_range),
                 "bytes %llu-%llu/%zu", start, end, (size_t) TEST_BODY_SIZE)
        >= (int) sizeof(content_range)) {
        MHD_destroy_response(resp);
        return MHD_NO;
    }
    MHD_add_response_header(resp, "Content-Range", content_range);
    MHD_add_response_header(resp, "Accept-Ranges", "bytes");
    MHD_queue_response(conn, MHD_HTTP_PARTIAL_CONTENT, resp);
    MHD_destroy_response(resp);
    return MHD_YES;
}

/* Plain mode: always 200 + the whole body; the Range header is
   ignored.  This is the fallback path that the httpfs content
   engine must detect (full-body mode). */
static mhd_result_t answer_plain_cb(void *cls, struct MHD_Connection *conn,
                                   const char *url, const char *method,
                                   const char *version,
                                   const char *upload_data,
                                   size_t *upload_data_size, void **con_cls)
{
    struct MHD_Response *resp;

    (void) cls;
    (void) url;
    (void) version;
    (void) upload_data;
    (void) upload_data_size;
    (void) con_cls;

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
        return MHD_NO;

    resp = MHD_create_response_from_buffer(TEST_BODY_SIZE, test_body,
                                          MHD_RESPMEM_MUST_COPY);
    if (resp == NULL)
        return MHD_NO;
    MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return MHD_YES;
}

/* Common startup code for both server modes. */
static int server_start_common(unsigned short port,
                               struct test_server **serverp,
                               MHD_AccessHandlerCallback cb)
{
    struct test_server *server;

    test_body_init();

    server = malloc(sizeof(*server));
    if (server == NULL)
        return -1;

    server->daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, port,
                                     NULL, NULL, cb, NULL, MHD_OPTION_END);
    if (server->daemon == NULL) {
        free(server);
        return -1;
    }

    *serverp = server;
    return 0;
}

int test_server_start(unsigned short port, struct test_server **serverp)
{
    return server_start_common(port, serverp, &answer_cb);
}

int test_server_start_plain(unsigned short port, struct test_server **serverp)
{
    return server_start_common(port, serverp, &answer_plain_cb);
}

void test_server_stop(struct test_server *server)
{
    if (server == NULL)
        return;
    MHD_stop_daemon(server->daemon);
    free(server);
}

struct write_ctx {
    char *buf;
    size_t len;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct write_ctx *ctx = userdata;
    size_t total = size * nmemb;
    char *grown;

    grown = realloc(ctx->buf, ctx->len + total);
    if (grown == NULL)
        return 0; /* abort the transfer */
    ctx->buf = grown;
    memcpy(ctx->buf + ctx->len, ptr, total);
    ctx->len += total;
    return total;
}

int test_fetch(const char *url, const char *range,
              char **bufp, size_t *lenp, long *statusp)
{
    CURL *curl;
    struct curl_slist *headers = NULL;
    struct write_ctx ctx;
    char header[64];
    long status = 0;
    int rc = -1;

    ctx.buf = NULL;
    ctx.len = 0;

    curl = curl_easy_init();
    if (curl == NULL)
        return -1;

    if (range != NULL) {
        if (snprintf(header, sizeof(header), "Range: bytes=%s", range)
            >= (int) sizeof(header))
            goto out;
        headers = curl_slist_append(NULL, header);
        if (headers == NULL)
            goto out;
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    if (curl_easy_perform(curl) != CURLE_OK)
        goto out;

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    *bufp = ctx.buf;
    *lenp = ctx.len;
    *statusp = status;
    rc = 0;
    ctx.buf = NULL;

out:
    free(ctx.buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc;
}
