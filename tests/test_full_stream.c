/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* Fetch the whole body served by the test server and validate it byte
   by byte against the deterministic pattern. */

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PORT 42831

int main(void)
{
    struct test_server *server;
    char url[64];
    char *body = NULL;
    size_t len = 0;
    long status = 0;
    int rc = 1;

    if (test_server_start(TEST_PORT, &server) != 0) {
        fprintf(stderr, "test_full_stream: could not start test server\n");
        return 77;
    }

    snprintf(url, sizeof(url), "http://127.0.0.1:%u/file", TEST_PORT);

    if (test_fetch(url, NULL, &body, &len, &status) != 0) {
        fprintf(stderr, "test_full_stream: request failed\n");
        goto out;
    }

    if (status != 200) {
        fprintf(stderr, "test_full_stream: expected status 200, got %ld\n",
                status);
        goto out;
    }

    if (len != TEST_BODY_SIZE) {
        fprintf(stderr, "test_full_stream: expected %zu bytes, got %zu\n",
                (size_t) TEST_BODY_SIZE, len);
        goto out;
    }

    if (memcmp(body, test_body, TEST_BODY_SIZE) != 0) {
        fprintf(stderr, "test_full_stream: body does not match\n");
        goto out;
    }

    rc = 0;

out:
    free(body);
    test_server_stop(server);
    if (rc == 0)
        printf("test_full_stream: OK\n");
    return rc;
}
