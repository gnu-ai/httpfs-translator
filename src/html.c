/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * html.c — Implementation of the tolerant HTML extractor (html.h).
 *
 * THE TOKENIZER
 * -------------
 * A small state machine walks the bytes exactly once:
 *
 *      <TAG ATTR="v" ...>      an opening (or self-closing) tag
 *      </TAG>                 a closing tag
 *      <!-- ... -->           a comment (dropped)
 *      anything else          character data (the visible text)
 *
 * Only three constructs influence the machine's state:
 *
 *      <script>, <style>  their raw content is invisible — the
 *                         cursor jumps to the matching close tag;
 *      <a href=...>       starts a link: the character data up to
 *                         </a> becomes the anchor text;
 *      <h1>..<h6>         starts a heading, emitted at the close
 *                         tag as "level<TAB>text".
 *
 * Everything else is context-free: <meta> just carries its
 * attributes, and an unknown tag is simply skipped.  There is no
 * stack of open elements and no error-recovery logic — that is
 * what makes the parser tolerant by construction: real-world
 * pages are not valid HTML, and a browser never fails on them.
 *
 * ATTRIBUTE VALUES
 * ----------------
 * href="x", href='x' and href=x are all accepted; entities are
 * decoded in values and character data alike (decode_entities).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "html.h"

/* ------------------------------------------------------------------ */
/* A growable byte buffer (the classic "string builder")
 * ------------------------------------------------------------------ */

struct sbuf
{
    char *b;
    size_t len;
    size_t cap;
};

static void sbuf_init (struct sbuf *s)
{
    s->cap = 64;
    s->len = 0;
    s->b = malloc (s->cap);
    if (s->b == NULL)
        {
            fprintf (stderr, "html: out of memory\n");
            abort ();
        }
    s->b[0] = '\0';             /* a valid empty string from day one */
}

static void sbuf_reset (struct sbuf *s)
{
    s->len = 0;
    s->b[0] = '\0';
}

static void sbuf_push (struct sbuf *s, char c)
{
    if (s->len + 1 >= s->cap)
        {
            s->cap *= 2;
            s->b = realloc (s->b, s->cap);
            if (s->b == NULL)
                {
                    fprintf (stderr, "html: out of memory\n");
                    abort ();
                }
        }
    s->b[s->len++] = c;
    s->b[s->len] = '\0';
}

static void sbuf_free (struct sbuf *s)
{
    free (s->b);
    s->b = NULL;
    s->len = s->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Entity decoding — the five named entities every HTML document
 * uses, plus both numeric forms.
 * ------------------------------------------------------------------ */

static int is_ws (char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* utf8_push — append one code point (sibling in json.c). */
static void utf8_push (struct sbuf *s, unsigned long cp)
{
    if (cp < 0x80)
        sbuf_push (s, (char) cp);
    else if (cp < 0x800)
        {
            sbuf_push (s, (char) (0xC0 | (cp >> 6)));
            sbuf_push (s, (char) (0x80 | (cp & 0x3F)));
        }
    else
        {
            sbuf_push (s, (char) (0xE0 | (cp >> 12)));
            sbuf_push (s, (char) (0x80 | ((cp >> 6) & 0x3F)));
            sbuf_push (s, (char) (0x80 | (cp & 0x3F)));
        }
}

/*
 * decode_entities — copy RAW into OUT, resolving &amp; &lt; &gt;
 * &quot; &apos; and the numeric forms &#38; &#x26;.
 *
 * An unknown or truncated entity (say "a &amp b") is copied
 * verbatim: browsers do exactly that.
 */
static void decode_entities (const char *raw, size_t n, struct sbuf *out)
{
    for (size_t i = 0; i < n; i++)
        {
            if (raw[i] != '&')
                {
                    sbuf_push (out, raw[i]);
                    continue;
                }

            /* Look for the terminating ';' within a sane window. */
            size_t limit = n - i < 12 ? n - i : 12;
            size_t semi = 0;
            for (size_t j = 1; j < limit; j++)
                if (raw[i + j] == ';')
                    {
                        semi = j;
                        break;
                    }
            if (semi == 0)
                {
                    sbuf_push (out, raw[i]);
                    continue;
                }

            /* Numeric form? */
            if (semi > 2 && raw[i + 1] == '#')
                {
                    unsigned long cp = 0;
                    int ok = 1;

                    if (raw[i + 2] == 'x' || raw[i + 2] == 'X')
                        {
                            for (size_t j = i + 3; j < i + semi; j++)
                                {
                                    char c = raw[j];
                                    cp <<= 4;
                                    if (c >= '0' && c <= '9')
                                        cp |= (unsigned long) (c - '0');
                                    else if (c >= 'a' && c <= 'f')
                                        cp |= (unsigned long) (c - 'a' + 10);
                                    else if (c >= 'A' && c <= 'F')
                                        cp |= (unsigned long) (c - 'A' + 10);
                                    else
                                        { ok = 0; break; }
                                }
                        }
                    else
                        {
                            for (size_t j = i + 2; j < i + semi; j++)
                                {
                                    char c = raw[j];
                                    if (c < '0' || c > '9')
                                        { ok = 0; break; }
                                    cp = cp * 10 + (unsigned long) (c - '0');
                                }
                        }

                    if (ok && cp != 0)
                        {
                            utf8_push (out, cp);
                            i += semi;
                            continue;
                        }
                    /* Fall through: copy the raw '&'. */
                }
            /* SEMI is the OFFSET of ';' from the '&': 4 for
               "&amp;", 3 for "&lt;", 5 for "&quot;". */
            else if (semi == 4 && memcmp (raw + i + 1, "amp", 3) == 0)
                { sbuf_push (out, '&');  i += 4; continue; }
            else if (semi == 3 && memcmp (raw + i + 1, "lt", 2) == 0)
                { sbuf_push (out, '<');  i += 3; continue; }
            else if (semi == 3 && memcmp (raw + i + 1, "gt", 2) == 0)
                { sbuf_push (out, '>');  i += 3; continue; }
            else if (semi == 5 && memcmp (raw + i + 1, "quot", 4) == 0)
                { sbuf_push (out, '"');  i += 5; continue; }
            else if (semi == 5 && memcmp (raw + i + 1, "apos", 4) == 0)
                { sbuf_push (out, '\''); i += 5; continue; }

            sbuf_push (out, raw[i]);
        }
}

/* ------------------------------------------------------------------ */
/* Tag scanning
 * ------------------------------------------------------------------ */

/*
 * tag_name — read the name of the tag at POS (just after '<' or
 * '</'), lower-cased, into OUT; *POS is advanced past the name.
 */
static void tag_name (const char *buf, size_t len, size_t *pos,
                      char *out, size_t outsz)
{
    size_t n = 0;

    while (*pos < len && (unsigned char) buf[*pos] > ' '
           && buf[*pos] != '>' && buf[*pos] != '/' && n + 1 < outsz)
        {
            char c = buf[*pos];
            out[n++] = (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
            (*pos)++;
        }
    out[n] = '\0';
}

/*
 * find_attr — return the (entity-decoded, malloc'd) value of the
 * attribute NAME inside the tag text spanning [start, end), or
 * NULL when the attribute is absent.
 *
 * All three HTML quoting styles are accepted:
 *      href="x"    href='x'    href=x
 */
static char *find_attr (const char *buf, size_t start, size_t end,
                        const char *name)
{
    size_t nlen = strlen (name);

    for (size_t i = start; i + nlen < end; i++)
        {
            /* The name must begin on a delimiter. */
            if (i > start && !is_ws (buf[i - 1]))
                continue;

            /* Case-insensitive name match. */
            size_t j = 0;
            while (j < nlen)
                {
                    char c = buf[i + j];
                    if (c >= 'A' && c <= 'Z')
                        c = (char) (c - 'A' + 'a');
                    if (c != name[j])
                        break;
                    j++;
                }
            if (j != nlen)
                continue;

            size_t after = i + nlen;
            if (after >= end || buf[after] != '=')
                continue;                /* absent or value-less */

            /* Skip '=' and surrounding whitespace. */
            after++;
            while (after < end && is_ws (buf[after]))
                after++;
            if (after >= end)
                return NULL;

            struct sbuf val;
            sbuf_init (&val);

            if (buf[after] == '"' || buf[after] == '\'')
                {
                    char q = buf[after++];
                    while (after < end && buf[after] != q)
                        sbuf_push (&val, buf[after++]);
                }
            else
                {
                    while (after < end && !is_ws (buf[after])
                           && buf[after] != '>')
                        sbuf_push (&val, buf[after++]);
                }

            struct sbuf decoded;
            sbuf_init (&decoded);
            decode_entities (val.b, val.len, &decoded);
            sbuf_free (&val);
            return decoded.b;
        }

    return NULL;
}

/*
 * skip_until_close — jump the cursor past the next "</name>" (used
 * for <script> and <style>, whose content is invisible).  A
 * missing close tag skips to the end of the document: tolerant.
 */
static void skip_until_close (const char *buf, size_t len, size_t *pos,
                              const char *name)
{
    char pattern[16];
    snprintf (pattern, sizeof pattern, "</%s", name);
    size_t plen = strlen (pattern);

    while (*pos < len)
        {
            if (buf[*pos] == '<' && *pos + plen <= len
                && strncasecmp (buf + *pos, pattern, plen) == 0)
                {
                    while (*pos < len && buf[*pos] != '>')
                        (*pos)++;
                    if (*pos < len)
                        (*pos)++;
                    return;
                }
            (*pos)++;
        }
}

/* ------------------------------------------------------------------ */
/* The extractor
 * ------------------------------------------------------------------ */

struct doc_node *html_to_doc (const char *buf, size_t len, char **err)
{
    (void) err;

    struct doc_node *root = doc_new_root ();
    struct doc_node *dir_meta = doc_dir (root, "meta");
    struct doc_node *dir_head = doc_dir (root, "headings");
    struct doc_node *dir_links = doc_dir (root, "links");

    struct sbuf title;          /* <title> content             */
    struct sbuf text;           /* the visible text            */
    struct sbuf heading;        /* the heading being collected */
    struct sbuf anchor;         /* the anchor being collected  */
    sbuf_init (&title);
    sbuf_init (&text);
    sbuf_init (&heading);
    sbuf_init (&anchor);

    size_t pos = 0;
    size_t items = 0;           /* global budget guard         */
    size_t n_heading = 0;       /* next /headings/<n> index    */
    size_t n_link = 0;          /* next /links/<n> index       */
    int in_title = 0;
    int in_heading = 0;          /* 0, or the heading level 1-6 */
    int in_anchor = 0;
    char *anchor_href = NULL;

    /* emit_link — create /links/<n> with the collected pieces.
       The trailing newline is consistent with every other
       line-oriented file of the tree. */
    #define EMIT_LINK()                                                    \
        do                                                                 \
            {                                                              \
                if (in_anchor && anchor_href != NULL                        \
                    && ++items <= HTML_MAX_ITEMS)                           \
                    {                                                      \
                        char fname[24];                                    \
                        snprintf (fname, sizeof fname, "%zu", n_link++);    \
                        struct doc_node *d = doc_dir (dir_links, fname);   \
                        doc_filef (d, "url", "%s\n", anchor_href);         \
                        doc_filef (d, "text", "%s\n", anchor.b);           \
                    }                                                      \
                free (anchor_href);                                        \
                anchor_href = NULL;                                        \
                in_anchor = 0;                                             \
            }                                                              \
        while (0)

    while (pos < len)
        {
            /* ---- character data -------------------------------- */
            if (buf[pos] != '<')
                {
                    size_t start = pos;
                    while (pos < len && buf[pos] != '<')
                        pos++;

                    if (in_title)
                        decode_entities (buf + start, pos - start, &title);
                    else if (in_heading)
                        decode_entities (buf + start, pos - start, &heading);
                    else if (in_anchor)
                        decode_entities (buf + start, pos - start, &anchor);
                    else
                        decode_entities (buf + start, pos - start, &text);
                    continue;
                }

            /* ---- comment --------------------------------------- */
            if (pos + 4 <= len && memcmp (buf + pos, "<!--", 4) == 0)
                {
                    pos += 4;
                    while (pos + 3 <= len && memcmp (buf + pos, "-->", 3) != 0)
                        pos++;
                    pos = pos + 3 <= len ? pos + 3 : len;
                    continue;
                }

            pos++;                            /* consume '<' */
            char name[16] = { 0 };

            /* ---- closing tag ----------------------------------- */
            if (pos < len && buf[pos] == '/')
                {
                    pos++;
                    tag_name (buf, len, &pos, name, sizeof name);

                    if (strcmp (name, "title") == 0)
                        in_title = 0;
                    else if (in_heading != 0 && name[0] == 'h'
                             && name[1] >= '1' && name[1] <= '6')
                        {
                            if (heading.len > 0 && ++items <= HTML_MAX_ITEMS)
                                {
                                    char fname[24];
                                    snprintf (fname, sizeof fname, "%zu",
                                              n_heading++);
                                    doc_filef (dir_head, fname, "%d\t%s\n",
                                               in_heading, heading.b);
                                }
                            sbuf_reset (&heading);
                            in_heading = 0;
                        }
                    else if (strcmp (name, "a") == 0)
                        EMIT_LINK ();

                    while (pos < len && buf[pos] != '>')
                        pos++;
                    if (pos < len)
                        pos++;
                    continue;
                }

            /* ---- opening tag ------------------------------------ */
            tag_name (buf, len, &pos, name, sizeof name);

            size_t body_end = pos;
            while (body_end < len && buf[body_end] != '>')
                body_end++;                  /* the attributes span */
            size_t after = body_end < len ? body_end + 1 : len;

            if (strcmp (name, "title") == 0)
                in_title = 1;
            else if (strcmp (name, "script") == 0
                     || strcmp (name, "style") == 0)
                {
                    /* Invisible content: jump to the close tag. */
                    pos = after;
                    skip_until_close (buf, len, &pos, name);
                    continue;
                }
            else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6'
                     && name[2] == '\0')
                {
                    in_heading = name[1] - '0';
                    sbuf_reset (&heading);
                }
            else if (strcmp (name, "meta") == 0)
                {
                    char *mname = find_attr (buf, pos, body_end, "name");
                    if (mname == NULL)
                        mname = find_attr (buf, pos, body_end, "property");
                    char *mcontent = find_attr (buf, pos, body_end, "content");

                    if (mname != NULL && mcontent != NULL
                        && ++items <= HTML_MAX_ITEMS)
                        {
                            char *fname = doc_unique_name (dir_meta, mname,
                                                           strlen (mname));
                            doc_filef (dir_meta, fname, "%s\n", mcontent);
                            free (fname);
                        }

                    free (mname);
                    free (mcontent);
                }
            else if (strcmp (name, "a") == 0)
                {
                    char *href = find_attr (buf, pos, body_end, "href");
                    if (href != NULL)
                        {
                            EMIT_LINK ();     /* close any dangling one */
                            ++items;
                            in_anchor = 1;
                            anchor_href = href;
                            sbuf_reset (&anchor);
                        }
                    else
                        free (href);
                }

            pos = after;
        }

    /* ---- Flush what a truncated page left open ---------------- */
    if (in_heading != 0 && heading.len > 0)
        {
            char fname[24];
            snprintf (fname, sizeof fname, "%zu", n_heading++);
            doc_filef (dir_head, fname, "%d\t%s\n", in_heading, heading.b);
        }
    EMIT_LINK ();

    /* ---- The root files --------------------------------------- */
    doc_file_str (root, "title", title.b);
    doc_file_str (root, "text", text.b);

    /* Whitespace normalization of /text happens here rather than
       during the walk: one trim-and-squeeze pass keeps the
       tokenizer simple. */
    {
        struct doc_node *t = doc_find (root, "text");
        size_t r = 0, w = 0;
        int pending_ws = 0;

        while (r < t->data_len)
            {
                if (is_ws (t->data[r]))
                    {
                        pending_ws = 1;
                        r++;
                        continue;
                    }
                if (w > 0 && pending_ws)
                    t->data[w++] = ' ';
                pending_ws = 0;
                t->data[w++] = t->data[r++];
            }
        while (w > 0 && t->data[w - 1] == ' ')
            w--;
        t->data_len = w;
        if (w > 0)
            t->data[w] = '\0';
    }

    sbuf_free (&title);
    sbuf_free (&text);
    sbuf_free (&heading);
    sbuf_free (&anchor);
    free (anchor_href);

    return root;
}
