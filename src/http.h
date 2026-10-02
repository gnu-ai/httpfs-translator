/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * http.h — Interface of the httpfs HTTP transport layer.
 *
 * This module is deliberately independent of the Hurd: it depends
 * only on the standard C library (POSIX) and on libcurl.  It can
 * therefore be compiled and unit-tested in isolation (see
 * tests/test_url.c).
 *
 * Its single responsibility — as mandated by the project
 * architecture (CONTEXT.md) — is to turn a URL into raw bytes:
 *
 *      http_fetch (url, &response)   full GET into memory
 *      http_url_join (base, name)     build "base/name", percent-encoded
 *      http_response_release (&r)     destroy a response
 */

#ifndef HTTPFS_HTTP_H
#define HTTPFS_HTTP_H

#include <stddef.h>
#include <sys/types.h>

/* ------------------------------------------------------------------
 * Data structures
 * ------------------------------------------------------------------ */

/*
 * http_response — the complete result of one download.
 *
 * Every field is allocated by http_fetch() and released by
 * http_response_release().  The body is NOT NUL-terminated (raw
 * binary bytes may flow through it); "headers", on the other hand,
 * is a valid C string.
 */
struct http_response
{
    char *body;        /* response body, raw bytes                    */
    size_t body_len;   /* useful length of the body                   */
    char *headers;     /* raw header block, including status lines    */
    long status;       /* final HTTP status code (after redirects)    */
    long filetime;     /* last-modified time (unix), -1 if unavailable */
};

/* ------------------------------------------------------------------
 * Prototypes
 * ------------------------------------------------------------------ */

/*
 * http_fetch — download a resource with a single GET request.
 *
 * INPUT :  url    complete address (scheme + host + path) ;
 *          out    response to fill in (destroyed first if non-empty).
 * OUTPUT:  0 on success, otherwise an error code from <errno.h>.
 *
 * Behaviour: follows redirects (at most HTTP_MAX_REDIRECTS), decodes
 * transfer encodings automatically (gzip, deflate), disables signal
 * handling in libcurl (CURLOPT_NOSIGNAL) because several threads may
 * call us, and bounds the duration of the operation.
 */
int http_fetch (const char *url, struct http_response *out);

/*
 * http_url_join — build the URL of a child resource.
 *
 * INPUT :  base      URL of the parent directory ;
 *          segment   name of the component to append (no « / »).
 * OUTPUT:  a string allocated with malloc(), to be released with
 *          free(), or NULL if memory could not be allocated.
 *
 * Example:  http_url_join ("http://a.org/d/", "x y") yields
 *           "http://a.org/d/x%20y"
 *
 * Unreserved characters (letters, digits, "-", ".", "_", "~") are
 * copied verbatim; every other byte is percent-encoded.
 */
char *http_url_join (const char *base, const char *segment);

/*
 * http_response_release — destroy a response.
 *
 * The structure is reset to a pristine state, so the function may
 * safely be called several times (it is idempotent).
 */
void http_response_release (struct http_response *r);

#endif /* HTTPFS_HTTP_H */
