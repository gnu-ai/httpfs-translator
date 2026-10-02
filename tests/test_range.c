/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* Validate single-range requests against the deterministic body:
   a bounded range, an open-ended range, and an out-of-bounds start. */

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PORT 42832

static int check_range(const char *range, long expect_status,
                       size_t expect_off, size_t expect_len)
{
    char url[64];
    char *body = NULL;
    size_t len = 0;
    long status = 0;
    int rc = 1;

    snprintf(url, sizeof(url), "http://127.0.0.1:%u/file", TEST_PORT);

    if (test_fetch(url, range, &body, &len, &status) != 0) {
        fprintf(stderr, "test_range: request 'bytes=%s' failed\n", range);
        goto out;
    }

    if (status != expect_status) {
        fprintf(stderr, "test_range: 'bytes=%s': expected status %ld, got %ld\n",
                range, expect_status, status);
        goto out;
    }

    if (status == 206) {
        if (len != expect_len) {
            fprintf(stderr, "test_range: 'bytes=%s': expected %zu bytes, got %zu\n",
                    range, expect_len, len);
            goto out;
        }
        if (memcmp(body, test_body + expect_off, expect_len) != 0) {
            fprintf(stderr, "test_range: 'bytes=%s': body does not match\n",
                    range);
            goto out;
        }
    }

    rc = 0;

out:
    free(body);
    return rc;
}

int main(void)
{
    struct test_server *server;
    int rc = 77;

    if (test_server_start(TEST_PORT, &server) != 0) {
        fprintf(stderr, "test_range: could not start test server\n");
        return 77;
    }

    rc = 0;
    if (check_range("100-199", 206, 100, 100) != 0)
        rc = 1;
    if (check_range("60000-", 206, 60000, TEST_BODY_SIZE - 60000) != 0)
        rc = 1;
    if (check_range("99999-", 416, 0, 0) != 0)
        rc = 1;

    test_server_stop(server);
    if (rc == 0)
        printf("test_range: OK\n");
    return rc;
}
