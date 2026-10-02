/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* test_url — validation of URL construction (http_url_join).
 *
 * A fully deterministic test: no network, no server.  It covers
 * base/child concatenation and the percent-encoding of reserved
 * characters (spaces, "?", "#", "%", UTF-8 bytes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"

static int failures = 0;

/* check — compare http_url_join(base, segment) with the expected
   value, and free the result in every case. */
static void check (const char *base, const char *segment,
                   const char *expected)
{
    char *url = http_url_join (base, segment);

    if (url == nullptr)
    {
        fprintf (stderr, "test_url: allocation failure for \"%s\"\n", base);
        failures++;
        return;
    }

    if (strcmp (url, expected) != 0)
    {
        fprintf (stderr,
                 "test_url: http_url_join(\"%s\", \"%s\")\n"
                 "  got      : %s\n"
                 "  expected : %s\n",
                 base, segment, url, expected);
        failures++;
    }

    free (url);
}

int main (void)
{
    /* Concatenation, with and without a trailing "/". */
    check ("http://a.org", "b", "http://a.org/b");
    check ("http://a.org/", "b", "http://a.org/b");
    check ("https://a.org/d/", "page", "https://a.org/d/page");

    /* Reserved characters -> percent-encoding. */
    check ("https://a.org/d", "x y", "https://a.org/d/x%20y");
    check ("http://a.org", "a?b=c", "http://a.org/a%3Fb%3Dc");
    check ("http://a.org", "d#e", "http://a.org/d%23e");
    check ("http://a.org", "100%", "http://a.org/100%25");
    check ("http://a.org/d/", "a/b", "http://a.org/d/a%2Fb");

    /* UTF-8 byte (é) encoded byte per byte. */
    check ("http://a.org", "caf\xC3\xA9", "http://a.org/caf%C3%A9");

    /* Unreserved characters, copied verbatim. */
    check ("http://a.org/d/", "a.b-c_d~e", "http://a.org/d/a.b-c_d~e");

    if (failures == 0)
    {
        printf ("test_url: OK\n");
        return 0;
    }

    fprintf (stderr, "test_url: %d failure(s)\n", failures);
    return 1;
}
