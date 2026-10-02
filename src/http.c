/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * http.c — Implementation of the httpfs HTTP transport layer.
 *
 * "Transport layer" means exactly this: turn a URL into raw bytes,
 * and never interpret those bytes.  Content analysis (HTML, JSON,
 * ...) is delegated to the translators stacked on top of httpfs
 * (htmlfs, jsonfs, ...), as described in CONTEXT.md.
 *
 * The code is written in C23 and uses only POSIX interfaces for
 * everything that is not libcurl.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

/* Reasonable limits for a non-interactive browser. */
constexpr long HTTP_CONNECT_TIMEOUT_S = 15;  /* connection setup     */
constexpr long HTTP_TOTAL_TIMEOUT_S   = 60;  /* whole transfer       */
constexpr long HTTP_MAX_REDIRECTS     = 10;

/* User-Agent string presented to servers. */
constexpr char HTTP_USER_AGENT[] = "GNU-AI-httpfs/0.2 (Hurd translator)";

/* ------------------------------------------------------------------
 * Growable buffer — accumulator for received bytes
 * ------------------------------------------------------------------ */

/*
 * http_buf — an in-memory buffer that grows by doubling.
 *
 * The pair (data, len) describes the useful bytes; "cap" is the
 * allocated capacity.  The invariant  len <= cap  holds everywhere.
 */
struct http_buf
{
    char *data;
    size_t len;
    size_t cap;
};

/* buf_append — append n bytes to the buffer, growing it if needed.
   Returns 0 on success, -1 when memory is exhausted. */
static int buf_append (struct http_buf *b, const char *bytes, size_t n)
{
    /* Enough room?  If not, double the capacity (with a 256-byte
       floor) until everything fits. */
    if (b->len + n > b->cap)
    {
        size_t new_cap = b->cap > 0 ? b->cap : 256;
        while (new_cap < b->len + n)
            new_cap *= 2;

        char *grown = realloc (b->data, new_cap);
        if (grown == nullptr)
            return -1;
        b->data = grown;
        b->cap = new_cap;
    }

    memcpy (b->data + b->len, bytes, n);
    b->len += n;
    return 0;
}

/* ------------------------------------------------------------------
 * libcurl write callbacks
 * ------------------------------------------------------------------ */

/* Body reception: every block delivered by libcurl is appended to
   the caller's buffer.  Returning 0 would abort the transfer. */
static size_t cb_body (char *ptr, size_t size, size_t nmemb, void *opaque)
{
    struct http_buf *b = opaque;
    size_t n = size * nmemb;

    if (buf_append (b, ptr, n) != 0)
        return 0;               /* abort: libcurl sees a short write */
    return n;
}

/* Header reception: every raw line (including status lines such as
   "HTTP/1.1 200 OK") is appended to the header block. */
static size_t cb_headers (char *ptr, size_t size, size_t nmemb, void *opaque)
{
    struct http_buf *b = opaque;
    size_t n = size * nmemb;

    if (buf_append (b, ptr, n) != 0)
        return 0;
    return n;
}

/* ------------------------------------------------------------------
 * Public functions
 * ------------------------------------------------------------------ */

void http_response_release (struct http_response *r)
{
    if (r == nullptr)
        return;
    free (r->body);
    free (r->headers);
    r->body = nullptr;
    r->headers = nullptr;
    r->body_len = 0;
    r->status = 0;
    r->filetime = -1;
}

int http_fetch (const char *url, struct http_response *out)
{
    CURL *curl = nullptr;
    struct http_buf body = { nullptr, 0, 0 };
    struct http_buf headers = { nullptr, 0, 0 };
    long status = 0;
    long filetime = -1;
    CURLcode rc;
    int err = 0;

    /* Pre-conditions: the caller supplies a URL and a response
       structure that it accepts to see reset. */
    if (url == nullptr || out == nullptr)
        return EINVAL;

    http_response_release (out);

    curl = curl_easy_init ();
    if (curl == nullptr)
        return ENOMEM;

    /* --- Request configuration ---------------------------------- */
    curl_easy_setopt (curl, CURLOPT_URL, url);
    curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, cb_body);
    curl_easy_setopt (curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt (curl, CURLOPT_HEADERFUNCTION, cb_headers);
    curl_easy_setopt (curl, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt (curl, CURLOPT_USERAGENT, HTTP_USER_AGENT);
    curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1L);       /* thread-safe   */
    curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L); /* a browser    */
    curl_easy_setopt (curl, CURLOPT_MAXREDIRS, HTTP_MAX_REDIRECTS);
    curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, HTTP_CONNECT_TIMEOUT_S);
    curl_easy_setopt (curl, CURLOPT_TIMEOUT, HTTP_TOTAL_TIMEOUT_S);
    /* Ask the server for Last-Modified so that CURLINFO_FILETIME has
       something to report (otherwise it always returns -1). */
    curl_easy_setopt (curl, CURLOPT_FILETIME, 1L);
    /* Accept transfer encodings; libcurl decodes them, so the body
       we keep is the decoded, raw payload. */
    curl_easy_setopt (curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");

    /* --- Transfer ----------------------------------------------- */
    rc = curl_easy_perform (curl);
    if (rc != CURLE_OK)
    {
        /* Translate the main libcurl failures into usual errno codes. */
        switch (rc)
        {
        case CURLE_OUT_OF_MEMORY:       err = ENOMEM;      break;
        case CURLE_OPERATION_TIMEDOUT:  err = ETIMEDOUT;   break;
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:    err = EHOSTUNREACH; break;
        default:                        err = EIO;         break;
        }
        goto leave;
    }

    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo (curl, CURLINFO_FILETIME, &filetime);

    /* A 4xx or 5xx status is NOT a transport error: the body (an
       error page, a JSON message...) remains exploitable by the
       client.  httpfs therefore exposes the response as-is; the
       "status" virtual file lets the visitor tell 200 from 404. */

    /* --- Publish the result -------------------------------------- */
    /* The header block must become a valid C string. */
    if (headers.len == 0 || buf_append (&headers, "\0", 1) != 0)
    {
        err = ENOMEM;
        goto leave;
    }

    out->body = body.data;
    out->body_len = body.len;
    out->headers = headers.data;
    out->status = status;
    out->filetime = filetime;
    body.data = nullptr;      /* ownership transferred to *out */
    headers.data = nullptr;
    err = 0;

leave:
    curl_easy_cleanup (curl);
    free (body.data);
    free (headers.data);
    return err;
}

/* ------------------------------------------------------------------
 * URL construction
 * ------------------------------------------------------------------ */

/* byte_needs_encoding — true when c must be percent-encoded.
   RFC 3986 keeps letters, digits, "-", ".", "_" and "~" verbatim. */
static bool byte_needs_encoding (unsigned char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        return false;
    if (c >= '0' && c <= '9')
        return false;
    return c != '-' && c != '.' && c != '_' && c != '~';
}

char *http_url_join (const char *base, const char *segment)
{
    if (base == nullptr || segment == nullptr)
        return nullptr;

    size_t base_len = strlen (base);
    size_t seg_len = strlen (segment);

    /* Worst encoding case: every byte becomes three ("%XX"), plus
       the possible separator and the final NUL byte. */
    size_t size = base_len + seg_len * 3 + 2;
    char *url = malloc (size);
    if (url == nullptr)
        return nullptr;

    size_t i = 0;

    /* Copy the base; add " / " only if it does not already end
       with one. */
    memcpy (url, base, base_len);
    i = base_len;
    if (base_len == 0 || base[base_len - 1] != '/')
        url[i++] = '/';

    /* Copy the segment, encoding bytes as needed. */
    for (size_t j = 0; j < seg_len; j++)
    {
        unsigned char c = (unsigned char) segment[j];
        if (! byte_needs_encoding (c))
            url[i++] = (char) c;
        else
        {
            static const char hex_digits[] = "0123456789ABCDEF";
            url[i++] = '%';
            url[i++] = hex_digits[c >> 4];
            url[i++] = hex_digits[c & 0x0F];
        }
    }

    url[i] = '\0';
    return url;
}
