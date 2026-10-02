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
 * everything that is not libcurl.  The three request flavors
 * (HEAD, full GET, ranged GET) share one implementation:
 * http_get_impl selects the method and the optional Range header.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "http.h"

#include <ctype.h>
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
constexpr char HTTP_USER_AGENT[] = "GNU-AI-httpfs/0.3 (Hurd translator)";

/* The request flavors shared by the public functions. */
enum http_mode
{
    HTTP_MODE_GET,      /* plain GET, no Range header                */
    HTTP_MODE_HEAD,     /* HEAD: metadata only                        */
    HTTP_MODE_RANGE,    /* GET with a "Range: bytes=start-end" header */
};

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
 * Header parsing
 * ------------------------------------------------------------------ */

/* header_value — find the value of header NAME in the raw header
   block (a C string of "Name: value" lines).  Comparison of the
   field name is case-insensitive.  Returns the value with its
   leading blanks skipped, or NULL when absent. */
static const char *header_value (const char *headers, const char *name)
{
    if (headers == nullptr)
        return nullptr;

    size_t name_len = strlen (name);
    const char *line = headers;

    while ((line = strstr (line, name)) != nullptr)
    {
        /* The occurrence must start a line and be followed by ":" */
        if ((line == headers || line[-1] == '\n')
            && strncasecmp (line, name, name_len) == 0)
        {
            const char *p = line + name_len;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == ':')
            {
                p++;
                while (*p == ' ' || *p == '\t')
                    p++;
                return p;
            }
        }
        line += name_len;
    }
    return nullptr;
}

/* parse_ll — parse a decimal long long, stopping at the first
   character that is not a digit.  Returns true on success. */
static bool parse_ll (const char *s, long long *out)
{
    if (s == nullptr)
        return false;

    long long v = 0;
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10 + (*s - '0');
        s++;
    }
    *out = v;
    return true;
}

/*
 * size_total_of — best known size of the whole resource.
 *
 *   - the total of "Content-Range: bytes x-y/TOTAL" is
 *     authoritative (it describes the whole document, not the
 *     slice); "TOTAL" may be "*" (unknown);
 *   - else the Content-Length of the response;
 *   - else the length actually received;
 *   - else -1.
 */
static long long size_total_of (const struct http_buf *headers,
                                size_t body_len)
{
    /* The header buffer is one byte longer than headers->len because
       http_get_impl appends the terminating NUL. */
    const char *hdrs = headers->data != nullptr ? headers->data : "";
    const char *cr = header_value (hdrs, "Content-Range");
    long long v;

    if (cr != nullptr)
    {
        /* "bytes start-last/TOTAL": jump to the slash. */
        const char *slash = strchr (cr, '/');
        if (slash != nullptr && slash[1] != '*' && parse_ll (slash + 1, &v)
            && v >= 0)
            return v;
        return -1;
    }

    const char *cl = header_value (hdrs, "Content-Length");
    if (cl != nullptr && parse_ll (cl, &v) && v >= 0)
        return v;

    return body_len > 0 ? (long long) body_len : -1;
}

/* ------------------------------------------------------------------
 * Common request engine
 * ------------------------------------------------------------------ */

/*
 * http_get_impl — perform one request of the given flavor.
 *
 * The out-of-range discipline is that of the public functions: a
 * transport-level failure (DNS, timeout, connection refused) is an
 * errno; an HTTP status, whatever it is, is a success.
 */
static int http_get_impl (enum http_mode mode, const char *url,
                          size_t start, size_t end,
                          struct http_response *out)
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
    curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1L);        /* thread-safe  */
    curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);   /* a browser   */
    curl_easy_setopt (curl, CURLOPT_MAXREDIRS, HTTP_MAX_REDIRECTS);
    curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, HTTP_CONNECT_TIMEOUT_S);
    curl_easy_setopt (curl, CURLOPT_TIMEOUT, HTTP_TOTAL_TIMEOUT_S);
    /* Ask the server for Last-Modified so that CURLINFO_FILETIME has
       something to report (otherwise it always returns -1). */
    curl_easy_setopt (curl, CURLOPT_FILETIME, 1L);
    /* Accept transfer encodings; libcurl decodes them, so the body
       we keep is the decoded, raw payload. */
    curl_easy_setopt (curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");

    if (mode == HTTP_MODE_HEAD)
        curl_easy_setopt (curl, CURLOPT_NOBODY, 1L);
    else if (mode == HTTP_MODE_RANGE)
    {
        /* CURLOPT_RANGE takes "X-Y" WITHOUT the "bytes=" prefix:
           libcurl adds "Range: bytes=X-Y" itself. */
        char range[64];
        if (snprintf (range, sizeof range, "%zu-%zu", start, end)
            >= (int) sizeof range)
        {
            curl_easy_cleanup (curl);
            return ERANGE;               /* window too large to express */
        }
        curl_easy_setopt (curl, CURLOPT_RANGE, range);
    }

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
        case CURLE_COULDNT_CONNECT:     err = EHOSTUNREACH; break;
        case CURLE_RANGE_ERROR:         err = ERANGE;      break;
        default:                        err = EIO;         break;
        }
        goto leave;
    }

    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo (curl, CURLINFO_FILETIME, &filetime);

    /* A 4xx or 5xx status is NOT a transport error: the body (an
       error page, a JSON message...) remains exploitable by the
       client.  httpfs therefore exposes the response as-is. */

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
    out->size_total = size_total_of (&headers, body.len);
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
    r->size_total = -1;
    r->filetime = -1;
}

int http_head (const char *url, struct http_response *out)
{
    return http_get_impl (HTTP_MODE_HEAD, url, 0, 0, out);
}

int http_fetch (const char *url, struct http_response *out)
{
    return http_get_impl (HTTP_MODE_GET, url, 0, 0, out);
}

int http_fetch_range (const char *url, size_t start, size_t end,
                      struct http_response *out)
{
    return http_get_impl (HTTP_MODE_RANGE, url, start, end, out);
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
