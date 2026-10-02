/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* test_ranged_reads — validation of the Phase 2 transport primitives.
 *
 * Drives http_head, http_fetch and http_fetch_range against the
 * embedded loopback server (both the range-capable and the
 * Range-ignoring flavor):
 *
 *   - HEAD answers 200 and reveals the size (Content-Length);
 *   - a bounded window answers 206 with exactly the requested slice
 *     and the whole size in Content-Range;
 *   - an open-ended window answers 206 with the tail of the body;
 *   - a window beyond the resource answers 416;
 *   - the plain server answers 200 with the whole body: the caller
 *     must detect the fallback.
 *
 * Loopback only (127.0.0.1); fully deterministic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "test_helpers.h"

#define TEST_PORT_RANGES 42833
#define TEST_PORT_PLAIN 42834

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (! (cond)) { fprintf (stderr, "test_ranged_reads: FAIL: %s\n", msg); \
                    failures++; } \
} while (0)

int main (void)
{
    struct test_server *srv = nullptr;
    struct test_server *plain = nullptr;
    char url[64];
    /* The transport API destroys the response first, so it must be
       handed a pristine (all-zero) structure. */
    struct http_response resp = { nullptr, 0, nullptr, 0, -1, -1 };
    int err;

    if (test_server_start (TEST_PORT_RANGES, &srv) != 0)
    {
        fprintf (stderr, "test_ranged_reads: could not start server\n");
        return 77;
    }
    if (test_server_start_plain (TEST_PORT_PLAIN, &plain) != 0)
    {
        fprintf (stderr, "test_ranged_reads: could not start plain server\n");
        test_server_stop (srv);
        return 77;
    }

    snprintf (url, sizeof url, "http://127.0.0.1:%u/file", TEST_PORT_RANGES);

    /* --- HEAD: metadata without a body --------------------------- */
    err = http_head (url, &resp);
    CHECK (err == 0, "http_head succeeds");
    CHECK (resp.status == 200, "HEAD status 200");
    CHECK (resp.body_len == 0, "HEAD body is empty");
    CHECK (resp.size_total == (long long) TEST_BODY_SIZE,
           "HEAD reveals the size");
    http_response_release (&resp);

    /* --- bounded window: 206 + the exact slice -------------------- */
    err = http_fetch_range (url, 100, 199, &resp);
    CHECK (err == 0, "bounded range succeeds");
    CHECK (resp.status == 206, "bounded range answered 206");
    CHECK (resp.body_len == 100, "bounded range length");
    CHECK (memcmp (resp.body, test_body + 100, 100) == 0,
           "bounded range bytes");
    CHECK (resp.size_total == (long long) TEST_BODY_SIZE,
           "Content-Range reports the whole size");
    http_response_release (&resp);

    /* --- open-ended window: 206 + the tail ------------------------ */
    err = http_fetch_range (url, TEST_BODY_SIZE - 500, TEST_BODY_SIZE, &resp);
    CHECK (err == 0 && resp.status == 206 && resp.body_len == 500,
           "open-ended tail: 206, 500 bytes");
    CHECK (memcmp (resp.body, test_body + TEST_BODY_SIZE - 500, 500) == 0,
           "tail bytes");
    http_response_release (&resp);

    /* --- window beyond the resource: 416 -------------------------- */
    err = http_fetch_range (url, TEST_BODY_SIZE + 10, TEST_BODY_SIZE + 99,
                            &resp);
    CHECK (err == 0 && resp.status == 416, "beyond the end: 416");
    http_response_release (&resp);

    /* --- plain server: Range ignored, 200 + whole body ------------ */
    snprintf (url, sizeof url, "http://127.0.0.1:%u/file", TEST_PORT_PLAIN);
    err = http_fetch_range (url, 100, 199, &resp);
    CHECK (err == 0, "plain server request succeeds");
    CHECK (resp.status == 200, "plain server answers 200 (fallback)");
    CHECK (resp.body_len == TEST_BODY_SIZE, "plain server sends whole body");
    CHECK (memcmp (resp.body, test_body, TEST_BODY_SIZE) == 0,
           "whole body bytes");
    http_response_release (&resp);

    /* --- full GET sanity (Phase 1 behavior) ---------------------- */
    err = http_fetch (url, &resp);
    CHECK (err == 0 && resp.status == 200
           && resp.body_len == TEST_BODY_SIZE, "full GET still works");
    http_response_release (&resp);

    test_server_stop (plain);
    test_server_stop (srv);

    if (failures == 0)
    {
        printf ("test_ranged_reads: OK\n");
        return 0;
    }
    printf ("test_ranged_reads: %d failure(s)\n", failures);
    return 1;
}
