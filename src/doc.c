/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * doc.c — Implementation of the portable document tree (doc.h).
 *
 * The implementation is a classic "first child / next sibling"
 * representation, the one used by every DOM library since the
 * original SpiderMonkey: a directory keeps a pointer to its first
 * and last child, and every child keeps a pointer to its next
 * sibling.  Appending is O(1) and walking the children is a simple
 * for-loop — no dynamic array bookkeeping involved.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc.h"

/* ------------------------------------------------------------------ */
/* Allocation discipline */
/* ------------------------------------------------------------------ */

/*
 * xmalloc — allocate N bytes or die.
 *
 * A parser that runs out of memory has nothing better to do than
 * fail loudly: partial documents would silently lie to the LLM
 * reading them.  Callers therefore never check for NULL.
 */
static void *xmalloc (size_t n)
{
    void *p = malloc (n != 0 ? n : 1);
    if (p == NULL)
        {
            fprintf (stderr, "doc: out of memory\n");
            abort ();
        }
    return p;
}

static char *xstrndup (const char *s, size_t n)
{
    char *p = xmalloc (n + 1);
    memcpy (p, s, n);
    p[n] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* Construction */
/* ------------------------------------------------------------------ */

/* alloc_node — one node, zeroed, with NAME copied. */
static struct doc_node *alloc_node (const char *name, size_t name_len)
{
    struct doc_node *n = xmalloc (sizeof *n);
    memset (n, 0, sizeof *n);
    n->name = xstrndup (name, name_len);
    return n;
}

struct doc_node *doc_new_root (void)
{
    return alloc_node ("", 0);
}

/* append_child — link CHILD at the end of PARENT's child list. */
static void append_child (struct doc_node *parent, struct doc_node *child)
{
    child->parent = parent;
    child->next_sibling = NULL;

    if (parent->last_child != NULL)
        parent->last_child->next_sibling = child;
    else
        parent->first_child = child;
    parent->last_child = child;
}

struct doc_node *doc_dir (struct doc_node *parent, const char *name)
{
    struct doc_node *d = alloc_node (name, strlen (name));
    append_child (parent, d);
    return d;
}

struct doc_node *doc_file (struct doc_node *parent, const char *name,
                           char *data, size_t len)
{
    struct doc_node *f = alloc_node (name, strlen (name));
    f->data = data;
    f->data_len = len;
    append_child (parent, f);
    return f;
}

struct doc_node *doc_file_str (struct doc_node *parent, const char *name,
                               const char *str)
{
    return doc_file (parent, name, xstrndup (str, strlen (str)),
                     strlen (str));
}

struct doc_node *doc_filef (struct doc_node *parent, const char *name,
                           const char *fmt, ...)
{
    va_list ap;
    char *buf = NULL;
    int n;

    va_start (ap, fmt);
    n = vsnprintf (NULL, 0, fmt, ap);
    va_end (ap);

    if (n < 0)
        return doc_file_str (parent, name, "");

    /* vsnprintf writes one more byte than it reports (the NUL):
       allocate n+1, hand over n bytes of content (data_len), and
       let the trailing NUL ride along — harmless to the readers,
       which all trust data_len. */
    buf = xmalloc ((size_t) n + 1);
    va_start (ap, fmt);
    vsnprintf (buf, (size_t) n + 1, fmt, ap);
    va_end (ap);

    return doc_file (parent, name, buf, (size_t) n);
}

/* ------------------------------------------------------------------ */
/* Naming helpers */
/* ------------------------------------------------------------------ */

char *doc_safe_name (const char *name, size_t len)
{
    char *s = xstrndup (name, len);

    for (size_t i = 0; i < len; i++)
        if (s[i] == '/' || (unsigned char) s[i] < 0x20)
            s[i] = '_';

    if (len == 0)
        {
            free (s);
            return xstrndup ("(empty)", 7);
        }

    /* Names looking like "." or ".." would confuse directory
       walkers; keep them readable but harmless. */
    if (strcmp (s, ".") == 0 || strcmp (s, "..") == 0)
        {
            char *dotted = xmalloc (len + 2);
            dotted[0] = '_';
            memcpy (dotted + 1, s, len + 1);
            free (s);
            return dotted;
        }

    return s;
}

char *doc_unique_name (struct doc_node *parent, const char *name,
                       size_t len)
{
    char *base = doc_safe_name (name, len);
    char *candidate = base;
    unsigned int suffix = 2;

    while (doc_find (parent, candidate) != NULL)
        {
            /* "_<n>" needs at most 11 more bytes (an unsigned int
               plus the NUL); 32 leaves a wide margin. */
            size_t cap = strlen (base) + 32;
            char *next = malloc (cap);

            if (next == NULL)
                {
                    fprintf (stderr, "doc: out of memory\n");
                    abort ();
                }
            snprintf (next, cap, "%s_%u", base, suffix++);

            /* base is only read above, while still alive; the
               previous candidate (but never base itself) can go. */
            if (candidate != base)
                free (candidate);
            candidate = next;
        }

    /* candidate IS base when no collision happened: it is the
       result, not a duplicate to release. */
    if (candidate != base)
        free (base);
    return candidate;
}

/* ------------------------------------------------------------------ */
/* Queries */
/* ------------------------------------------------------------------ */

struct doc_node *doc_find (const struct doc_node *parent, const char *name)
{
    if (parent == NULL)
        return NULL;

    for (struct doc_node *c = parent->first_child; c != NULL;
         c = c->next_sibling)
        if (strcmp (c->name, name) == 0)
            return c;

    return NULL;
}

size_t doc_child_count (const struct doc_node *parent)
{
    size_t n = 0;

    if (parent == NULL)
        return 0;

    for (struct doc_node *c = parent->first_child; c != NULL;
         c = c->next_sibling)
        n++;

    return n;
}

/* ------------------------------------------------------------------ */
/* Destruction */
/* ------------------------------------------------------------------ */

void doc_release (struct doc_node *node)
{
    if (node == NULL)
        return;

    /* Recursion is fine here: parser trees are bounded by the
       document size, and the stack cost is one frame per LEVEL. */
    struct doc_node *c = node->first_child;
    while (c != NULL)
        {
            struct doc_node *next = c->next_sibling;
            doc_release (c);
            c = next;
        }

    free (node->name);
    free (node->data);
    free (node);
}
