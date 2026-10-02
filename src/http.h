/*
 * http.h — Interface of the httpfs HTTP transport layer.
 *
 * This module is deliberately independent of the Hurd: it depends
 * only on the standard C library (POSIX) and on libcurl.  It can
 * therefore be compiled and unit-tested in isolation (see
 * tests/test_url.c and tests/test_ranged_reads.c).
 *
 * Its single responsibility — as mandated by the project
 * architecture (CONTEXT.md) — is to turn a URL into raw bytes.  The
 * Phase 2 additions give callers fine-grained access:
 *
 *      http_head (url, &resp)              metadata only (no body)
 *      http_fetch (url, &resp)             whole body, one GET
 *      http_fetch_range (url, s, e, &resp) one window, one GET + Range
 *      http_url_join (base, name)          "base/name", percent-encoded
 *      http_response_release (&r)          destroy a response
 *
 * The response of http_fetch_range reports, through its status
 * field, whether the server honored the window (206) or ignored the
 * Range header and sent the whole document (200) — the caller
 * decides which strategy to use from that point on.
 */

#ifndef HTTPFS_HTTP_H
#define HTTPFS_HTTP_H

#include <stddef.h>
#include <sys/types.h>

/* ------------------------------------------------------------------
 * Data structures
 * ------------------------------------------------------------------ */

/*
 * http_response — the complete result of one request.
 *
 * Every field is allocated by the transport functions and released
 * by http_response_release().  The body is NOT NUL-terminated (raw
 * binary bytes may flow through it); "headers", on the other hand,
 * is a valid C string.
 *
 * size_total is the best known size of the whole resource, in
 * bytes, or -1 when no source revealed it:
 *   - Content-Range total ("bytes x-y/TOTAL") — authoritative;
 *   - else Content-Length of the response;
 *   - else the length of the body actually received;
 *   - else -1.
 */
struct http_response
{
    char *body;        /* response body, raw bytes                    */
    size_t body_len;   /* useful length of the body                   */
    char *headers;     /* raw header block, including status lines    */
    long status;       /* final HTTP status code (after redirects)    */
    long long size_total;  /* whole-resource size, -1 if unknown      */
    long filetime;     /* last-modified time (unix), -1 if unavailable */
};

/* ------------------------------------------------------------------
 * Prototypes
 * ------------------------------------------------------------------ */

/*
 * http_head — fetch only the metadata of a resource (HTTP HEAD).
 *
 * INPUT :  url    complete address ;
 *          out    response to fill in (destroyed first if non-empty).
 * OUTPUT:  0 on success, otherwise an error code from <errno.h>.
 * The body is always empty; headers, status, size_total (from
 * Content-Length) and filetime are filled in.
 */
int http_head (const char *url, struct http_response *out);

/*
 * http_fetch — download a whole resource with a single GET.
 *
 * INPUT / OUTPUT: as for http_head, plus the body.  A 4xx or 5xx
 * status is NOT an error: the body (error page, JSON message, ...)
 * remains exploitable.
 *
 * Behaviour: follows redirects (at most HTTP_MAX_REDIRECTS), decodes
 * transfer encodings automatically (gzip, deflate), disables signal
 * handling in libcurl (several threads may call us), and bounds the
 * duration of the operation.
 */
int http_fetch (const char *url, struct http_response *out);

/*
 * http_fetch_range — download one window of a resource.
 *
 * INPUT :  url, start, end — the byte range [start, end], inclusive.
 * OUTPUT:  as for http_fetch; out->status says what happened:
 *            206: the server honored the window (body = the slice);
 *            200: the server ignored Range (body = the whole
 *                 document — callers should fall back to full-body
 *                 mode);
 *            416: the window lies entirely beyond the resource.
 * Any other status is the server's answer to the document itself.
 */
int http_fetch_range (const char *url, size_t start, size_t end,
                      struct http_response *out);

/*
 * http_url_join — build the URL of a child resource.
 *
 * INPUT :  base      URL of the parent directory ;
 *          segment   name of the component to append (no « / »).
 * OUTPUT:  a string allocated with malloc(), to be released with
 *          free(), or NULL if memory could not be allocated.
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
