/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* Shared helpers for the httpfs test suite.
 *
 * A deterministic in-process HTTP server (libmicrohttpd) serves a fixed
 * body on 127.0.0.1, supporting single-range requests, and libcurl is
 * used as the client side.
 */

#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stddef.h>

/* Size of the deterministic body served by the test server. */
#define TEST_BODY_SIZE 65536

/* The deterministic body, filled in by test_server_start(). */
extern unsigned char test_body[TEST_BODY_SIZE];

struct test_server;

/* Start an HTTP server on 127.0.0.1:PORT serving test_body.
   Returns 0 on success, non-zero on failure. */
int test_server_start(unsigned short port, struct test_server **serverp);

/* Stop a server started with test_server_start(). */
void test_server_stop(struct test_server *server);

/* GET the URL.  If RANGE is non-NULL, send "Range: bytes=RANGE".
   On success, return 0, the response body in a malloced buffer *BUFP
   of length *LENP, and the HTTP status code in *STATUSP. */
int test_fetch(const char *url, const char *range,
              char **bufp, size_t *lenp, long *statusp);

#endif /* TEST_HELPERS_H */
