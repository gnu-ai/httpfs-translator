/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * json.c — Implementation of the JSON parser (RFC 8259 subset).
 *
 * METHOD
 * ------
 * A classic recursive-descent parser over a cursor:
 *
 *      struct jscan  — where we are (buffer, position, depth)
 *      jvalue (s)    — one JSON value: object, array or scalar
 *      jstring (s)   — a quoted string, escapes decoded
 *      jnumber (s)   — the grammar [minus] int frac exp
 *
 * Each production consumes its bytes and returns; there is no
 * backtracking, so the parser runs in a single O(n) pass — the
 * cost of building a file is dominated by the tree allocations.
 *
 * WHY NOT JSON-C?
 * ---------------
 * The project deliberately keeps a zero-dependency stance for
 * parsers (CONTEXT.md): every dependency must be justified by a
 * feature the C23 standard library cannot deliver.  A complete
 * JSON reader is 250 lines — and being able to bound the memory
 * (JSON_MAX_NODES) is worth writing them ourselves.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/* ------------------------------------------------------------------ */
/* The cursor
 * ------------------------------------------------------------------ */

struct jscan
{
    const char *buf;        /* the whole document          */
    size_t len;             /* its length                  */
    size_t pos;             /* the current byte            */
    int depth;              /* current nesting depth       */
    size_t nodes;           /* nodes created so far       */
    char *err;              /* parse error, or NULL        */
};

/* fail — record the FIRST error (later ones are usually noise
   cascading from it) and return a distinctive status. */
enum { J_OK = 0, J_ERR = -1, J_END = -2 };

static int jfail (struct jscan *s, const char *msg)
{
    if (s->err == NULL)
        {
            /* 64 bytes: the message, a position, and a wide margin
               — without resorting to the non-POSIX asprintf. */
            s->err = malloc (64);
            if (s->err != NULL)
                snprintf (s->err, 64, "%s at byte %zu", msg, s->pos);
        }
    return J_ERR;
}

/* peek — the current byte, or -1 at the end of the document. */
static int peek (const struct jscan *s)
{
    return s->pos < s->len ? (unsigned char) s->buf[s->pos] : -1;
}

/* skip_ws — jump over the four JSON white spaces (and only them:
   a raw newline inside a number is an error, not whitespace). */
static void skip_ws (struct jscan *s)
{
    while (s->pos < s->len)
        {
            char c = s->buf[s->pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                s->pos++;
            else
                break;
        }
}

/* ------------------------------------------------------------------ */
/* Scalar productions
 * ------------------------------------------------------------------ */

/*
 * utf8_push — append one code point to a growable buffer.
 *
 * The \u escape produces a code POINT; a document may encode it
 * as up to three UTF-8 bytes.  Surrogate pairs (the only way to
 * reach code points above 0xFFFF) are combined on the spot, as
 * required by RFC 8259 §7 ("characters ... encoded as a pair").
 */
static void utf8_push (char **out, size_t *len, size_t *cap,
                       unsigned long cp)
{
    unsigned char tmp[4];
    int n;

    if (cp < 0x80)
        { tmp[0] = (unsigned char) cp; n = 1; }
    else if (cp < 0x800)
        {
            tmp[0] = (unsigned char) (0xC0 | (cp >> 6));
            tmp[1] = (unsigned char) (0x80 | (cp & 0x3F));
            n = 2;
        }
    else
        {
            tmp[0] = (unsigned char) (0xE0 | (cp >> 12));
            tmp[1] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
            tmp[2] = (unsigned char) (0x80 | (cp & 0x3F));
            n = 3;
        }

    if (*len + (size_t) n + 1 > *cap)
        {
            *cap = (*cap * 2) + 16 + (size_t) n;
            *out = realloc (*out, *cap);
            if (*out == NULL)
                {
                    fprintf (stderr, "json: out of memory\n");
                    abort ();
                }
        }

    memcpy (*out + *len, tmp, (size_t) n);
    *len += (size_t) n;
}

/*
 * hex4 — read exactly four hexadecimal digits (the \uXXXX escape).
 */
static int hex4 (struct jscan *s, unsigned long *out)
{
    unsigned long v = 0;

    if (s->pos + 4 > s->len)
        return jfail (s, "truncated \\u escape");

    for (int i = 0; i < 4; i++)
        {
            char c = s->buf[s->pos + (size_t) i];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= (unsigned long) (c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= (unsigned long) (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= (unsigned long) (c - 'A' + 10);
            else
                return jfail (s, "bad \\u escape");
        }

    s->pos += 4;
    *out = v;
    return J_OK;
}

/*
 * jstring — read a quoted string, escapes decoded.
 *
 * OUTPUT:  the decoded bytes (malloc'd, not NUL-terminated) in
 *          *OUT / *OUTLEN.
 */
static int jstring (struct jscan *s, char **out, size_t *outlen)
{
    *out = NULL;
    *outlen = 0;

    if (peek (s) != '"')
        return jfail (s, "expected a string");

    s->pos++;                        /* the opening quote */
    {
        size_t cap = 16, len = 0;
        char *b = malloc (cap);
        if (b == NULL)
            {
                fprintf (stderr, "json: out of memory\n");
                abort ();
            }

        for (;;)
            {
                int c = peek (s);

                if (c < 0)
                    {
                        free (b);
                        return jfail (s, "unterminated string");
                    }
                if (c == '"')
                    {
                        s->pos++;
                        *out = b;
                        *outlen = len;
                        return J_OK;
                    }
                if (c == '\\')
                    {
                        s->pos++;
                        int e = peek (s);
                        s->pos++;

                        switch (e)
                            {
                            case '"':  case '\\': case '/':
                                utf8_push (&b, &len, &cap,
                                           (unsigned long) e);
                                break;
                            case 'b':  utf8_push (&b, &len, &cap, '\b');   break;
                            case 'f':  utf8_push (&b, &len, &cap, '\f');   break;
                            case 'n':  utf8_push (&b, &len, &cap, '\n');   break;
                            case 'r':  utf8_push (&b, &len, &cap, '\r');   break;
                            case 't':  utf8_push (&b, &len, &cap, '\t');   break;
                            case 'u':
                                {
                                    unsigned long cp;
                                    if (hex4 (s, &cp) != J_OK)
                                        { free (b); return J_ERR; }

                                    /* Surrogate pair: D800-DBFF
                                       must be followed by
                                       DC00-DFFF. */
                                    if (cp >= 0xD800 && cp <= 0xDBFF)
                                        {
                                            unsigned long lo;
                                            if (peek (s) == '\\'
                                                && s->pos + 1 < s->len
                                                && s->buf[s->pos + 1] == 'u')
                                                {
                                                    s->pos += 2;
                                                    if (hex4 (s, &lo) != J_OK)
                                                        { free (b); return J_ERR; }
                                                    if (lo < 0xDC00 || lo > 0xDFFF)
                                                        {
                                                            free (b);
                                                            return jfail (s, "lone high surrogate");
                                                        }
                                                    cp = 0x10000
                                                         + ((cp - 0xD800) << 10)
                                                         + (lo - 0xDC00);
                                                }
                                            else
                                                {
                                                    free (b);
                                                    return jfail (s, "lone high surrogate");
                                                }
                                        }

                                    utf8_push (&b, &len, &cap, cp);
                                }
                                break;
                            default:
                                free (b);
                                return jfail (s, "unknown escape");
                            }
                        continue;
                    }

                /* Any other byte passes through verbatim: the
                   document is UTF-8, the tree is UTF-8. */
                utf8_push (&b, &len, &cap, (unsigned long) (unsigned char) c);
                s->pos++;
            }
    }
}

/*
 * jnumber — consume the JSON number grammar and copy the literal
 * verbatim: re-formatting a float would lie about the precision
 * the server sent.
 *
 *      number = [ - ] int [ frac ] [ exp ]
 */
static int jnumber (struct jscan *s, char **out, size_t *outlen)
{
    size_t start = s->pos;

    if (peek (s) == '-')
        s->pos++;

    /* int: "0" alone, or digits without a leading zero */
    if (peek (s) == '0')
        s->pos++;
    else if (peek (s) >= '1' && peek (s) <= '9')
        while (peek (s) >= '0' && peek (s) <= '9')
            s->pos++;
    else
        return jfail (s, "bad number");

    /* frac */
    if (peek (s) == '.')
        {
            s->pos++;
            if (!(peek (s) >= '0' && peek (s) <= '9'))
                return jfail (s, "bad number fraction");
            while (peek (s) >= '0' && peek (s) <= '9')
                s->pos++;
        }

    /* exp */
    if (peek (s) == 'e' || peek (s) == 'E')
        {
            s->pos++;
            if (peek (s) == '+' || peek (s) == '-')
                s->pos++;
            if (!(peek (s) >= '0' && peek (s) <= '9'))
                return jfail (s, "bad number exponent");
            while (peek (s) >= '0' && peek (s) <= '9')
                s->pos++;
        }

    *outlen = s->pos - start;
    *out = malloc (*outlen != 0 ? *outlen : 1);
    if (*out == NULL)
        {
            fprintf (stderr, "json: out of memory\n");
            abort ();
        }
    memcpy (*out, s->buf + start, *outlen);
    return J_OK;
}

/* ------------------------------------------------------------------ */
/* Tree construction */
/* ------------------------------------------------------------------ */

/* budget — count one more node, refusing documents that would
   exhaust the host. */
static int budget (struct jscan *s)
{
    if (++s->nodes > JSON_MAX_NODES)
        return jfail (s, "too many values (limit reached)");
    return J_OK;
}

/*
 * jvalue — parse one value into a fresh child of PARENT.
 *
 * The recursive heart of the parser.  The depth budget forbids
 * the billion-laughs style deep-nesting denial of service.
 */
static int jvalue (struct jscan *s, struct doc_node *parent,
                   const char *name)
{
    if (s->depth >= JSON_MAX_DEPTH)
        return jfail (s, "nesting too deep");

    skip_ws (s);
    if (budget (s) != J_OK)
        return J_ERR;

    int c = peek (s);

    /* --- object --------------------------------------------- */
    if (c == '{')
        {
            s->pos++;
            s->depth++;

            struct doc_node *dir = doc_dir (parent, name);
            skip_ws (s);

            if (peek (s) == '}')
                {
                    s->pos++;
                    s->depth--;
                    return J_OK;
                }

            for (;;)
                {
                    char *key;
                    size_t keylen;

                    skip_ws (s);
                    if (jstring (s, &key, &keylen) != J_OK)
                        return J_ERR;
                    skip_ws (s);

                    if (peek (s) != ':')
                        {
                            free (key);
                            return jfail (s, "expected ':' after a member name");
                        }
                    s->pos++;

                    char *fname = doc_unique_name (dir, key, keylen);
                    int rc = jvalue (s, dir, fname);
                    free (key);
                    free (fname);
                    if (rc != J_OK)
                        return J_ERR;

                    skip_ws (s);
                    if (peek (s) == ',')
                        {
                            s->pos++;
                            continue;
                        }
                    if (peek (s) == '}')
                        {
                            s->pos++;
                            s->depth--;
                            return J_OK;
                        }
                    return jfail (s, "expected ',' or '}' in an object");
                }
        }

    /* --- array ---------------------------------------------- */
    if (c == '[')
        {
            s->pos++;
            s->depth++;

            struct doc_node *dir = doc_dir (parent, name);
            skip_ws (s);

            if (peek (s) == ']')
                {
                    s->pos++;
                    s->depth--;
                    return J_OK;
                }

            size_t index = 0;
            for (;;)
                {
                    char fname[32];
                    snprintf (fname, sizeof fname, "%zu", index++);

                    int rc = jvalue (s, dir, fname);
                    if (rc != J_OK)
                        return J_ERR;

                    skip_ws (s);
                    if (peek (s) == ',')
                        {
                            s->pos++;
                            continue;
                        }
                    if (peek (s) == ']')
                        {
                            s->pos++;
                            s->depth--;
                            return J_OK;
                        }
                    return jfail (s, "expected ',' or ']' in an array");
                }
        }

    /* --- scalars -------------------------------------------- */
    if (c == '"')
        {
            char *bytes;
            size_t nbytes;
            if (jstring (s, &bytes, &nbytes) != J_OK)
                return J_ERR;
            doc_file (parent, name, bytes, nbytes);
            return J_OK;
        }

    if (c == 't' && s->pos + 4 <= s->len
        && memcmp (s->buf + s->pos, "true", 4) == 0)
        {
            s->pos += 4;
            return doc_file_str (parent, name, "true") != NULL ? J_OK : J_ERR;
        }

    if (c == 'f' && s->pos + 5 <= s->len
        && memcmp (s->buf + s->pos, "false", 5) == 0)
        {
            s->pos += 5;
            return doc_file_str (parent, name, "false") != NULL ? J_OK : J_ERR;
        }

    if (c == 'n' && s->pos + 4 <= s->len
        && memcmp (s->buf + s->pos, "null", 4) == 0)
        {
            s->pos += 4;
            return doc_file_str (parent, name, "null") != NULL ? J_OK : J_ERR;
        }

    if (c == '-' || (c >= '0' && c <= '9'))
        {
            char *lit;
            size_t litlen;
            if (jnumber (s, &lit, &litlen) != J_OK)
                return J_ERR;
            doc_file (parent, name, lit, litlen);
            return J_OK;
        }

    return jfail (s, "unexpected character");
}

/* ------------------------------------------------------------------ */
/* Entry point */
/* ------------------------------------------------------------------ */

struct doc_node *json_to_doc (const char *buf, size_t len, char **err)
{
    struct jscan s = { buf, len, 0, 0, 0, NULL };
    struct doc_node *root;

    if (err != NULL)
        *err = NULL;

    root = doc_new_root ();

    /* The root value becomes the tree itself: an object at the
       root exposes its members directly ("cat /api/version").  A
       scalar root (rare but legal) is exposed as "value". */
    if (jvalue (&s, root, "value") != J_OK)
        {
            doc_release (root);
            if (err != NULL && s.err != NULL)
                *err = s.err;
            else
                free (s.err);
            return NULL;
        }

    skip_ws (&s);
    if (s.pos != s.len)
        {
            if (s.err == NULL)
                {
                    s.err = malloc (64);
                    if (s.err != NULL)
                        snprintf (s.err, 64,
                                  "trailing data at byte %zu", s.pos);
                }
            doc_release (root);
            if (err != NULL)
                *err = s.err;
            else
                free (s.err);
            return NULL;
        }

    /* A container root IS the root directory: expose its children
       at the top level by re-homing them. */
    {
        struct doc_node *value = doc_find (root, "value");

        if (value != NULL && value->first_child != NULL
            && value->data == NULL)
            {
                root->first_child = value->first_child;
                root->last_child = value->last_child;

                for (struct doc_node *c = root->first_child; c != NULL;
                     c = c->next_sibling)
                    c->parent = root;

                /* The empty "value" shell disappears; its name
                   buffer is the only thing it owns. */
                free (value->name);
                free (value);
            }
    }

    return root;
}
