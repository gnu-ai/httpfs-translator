/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * doc.h — The portable document tree of the Phase 3 parsers.
 *
 * PHASE 3 IN ONE PICTURE
 * ----------------------
 * The transport translator (httpfs) downloads raw bytes; the
 * parser translators (htmlfs, jsonfs, csvfs) turn a document into
 * a browsable tree of POSIX files:
 *
 *      settrans -a /web httpfs https://example.org/page.html
 *      settrans -a /page htmlfs /web/content
 *      cat /page/title
 *
 * All three parsers build the very same kind of tree, described
 * here.  The tree itself is deliberately INDEPENDENT of the Hurd:
 * it is plain C23 + POSIX, so the parsers can be compiled and
 * unit-tested on any POSIX system (see tests/test_json.c, ...).
 * Only the thin server layer (parserfs.c) knows about libnetfs.
 *
 * THE MODEL
 * ---------
 * A document is a tree of nodes.  A node is either:
 *
 *   - a DIRECTORY, holding an ordered list of children, or
 *   - a FILE, holding raw bytes (its "content").
 *
 * Children are reached by name, exactly as files in a directory.
 * The parser that builds the tree guarantees unique, safe names
 * (no "/", no empty name); doc_unique_name() helps doing so.
 *
 *                      root (dir)
 *                     /    |     \
 *                "title" "links" "links/2" ...
 *                 (file)  (dir)   (dir)
 */

#ifndef HTTPFS_DOC_H
#define HTTPFS_DOC_H

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* The node
 * ------------------------------------------------------------------ */

struct doc_node
{
    /* Identity inside the parent directory. */
    char *name;                    /* entry name, never NULL, never empty */

    /* Tree links (ownership: the parent owns its children). */
    struct doc_node *parent;
    struct doc_node *first_child;  /* first and last children (dirs)  */
    struct doc_node *last_child;
    struct doc_node *next_sibling; /* links of the child list         */

    /* Content of a file node; both NULL for a directory. */
    char *data;                    /* malloc'd bytes, NOT NUL-terminated */
    size_t data_len;

    /* Opaque slot for the server layer (parserfs.c stores its
       libnetfs node here).  Portable code must not touch it. */
    void *priv;
};

/* ------------------------------------------------------------------ */
/* Construction — all functions abort on memory exhaustion, as a
 * parser running out of memory cannot recover anyway.
 * ------------------------------------------------------------------ */

/* doc_new_root — create the (empty) root directory of a document. */
struct doc_node *doc_new_root (void);

/* doc_dir — append a new child DIRECTORY called NAME to PARENT.
   Returns the new directory. */
struct doc_node *doc_dir (struct doc_node *parent, const char *name);

/* doc_file — append a new child FILE called NAME to PARENT, taking
   OWNERSHIP of DATA (a malloc'd buffer of LEN bytes, possibly
   binary).  Returns the new file. */
struct doc_node *doc_file (struct doc_node *parent, const char *name,
                           char *data, size_t len);

/* doc_file_str — append a file whose content is the NUL-terminated
   string STR (copied; the caller keeps its own copy). */
struct doc_node *doc_file_str (struct doc_node *parent, const char *name,
                               const char *str);

/* doc_filef — append a file whose content is built printf-style.
   The resulting bytes are NOT NUL-terminated: a trailing "\n" must
   be part of the format string when a line is wanted. */
struct doc_node *doc_filef (struct doc_node *parent, const char *name,
                            const char *fmt, ...)
    __attribute__ ((format (printf, 3, 4)));

/* ------------------------------------------------------------------ */
/* Naming helpers
 * ------------------------------------------------------------------ */

/* doc_safe_name — return a malloc'd copy of NAME that can be used
   as a file name: "/" and control bytes become "_", an empty name
   becomes "(empty)".  The caller frees the result. */
char *doc_safe_name (const char *name, size_t len);

/* doc_unique_name — like doc_safe_name, but also guarantees the
   result does not collide with any existing child of PARENT: a
   conflict appends "_2", "_3", ... */
char *doc_unique_name (struct doc_node *parent, const char *name,
                       size_t len);

/* ------------------------------------------------------------------ */
/* Queries
 * ------------------------------------------------------------------ */

/* doc_find — return the child of PARENT called NAME, or NULL.
   Linear scan: parser trees are small, and this is only hit once
   per lookup while the server layer caches its own nodes. */
struct doc_node *doc_find (const struct doc_node *parent, const char *name);

/* doc_child_count — number of children of PARENT. */
size_t doc_child_count (const struct doc_node *parent);

/* ------------------------------------------------------------------ */
/* Destruction
 * ------------------------------------------------------------------ */

/* doc_release — recursively free a whole (sub)tree.  Normally the
   tree lives as long as the translator process; the tests use
   this to check for leaks.  Every libnetfs node still attached
   through priv is NOT touched here (the server layer owns it). */
void doc_release (struct doc_node *node);

#endif /* HTTPFS_DOC_H */
