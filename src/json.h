/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * json.h — JSON parser of the Phase 3 jsonfs translator.
 *
 * jsonfs maps a JSON document onto a tree of POSIX files:
 *
 *      object  ->  directory, one child per member
 *      array   ->  directory, children "0", "1", "2", ...
 *      string  ->  file holding the DECODED text
 *      number  ->  file holding the raw JSON literal ("3.14", "1e9")
 *      true / false / null
 *              ->  files holding the literal as-is
 *
 * Member names become file names, sanitized and uniquified by the
 * document tree (see doc.h): {"a/b": 1, "a_b": 2} exposes "a_b"
 * and "a_b_2".  An LLM browsing the filesystem therefore reads a
 * JSON API answer the way it reads any directory.
 */

#ifndef HTTPFS_JSON_H
#define HTTPFS_JSON_H

#include <stddef.h>

#include "doc.h"

/* ------------------------------------------------------------------ */
/* Parsing limits — a hostile document must not exhaust the host.
 * ------------------------------------------------------------------ */

constexpr int JSON_MAX_DEPTH = 128;          /* nesting levels       */
constexpr size_t JSON_MAX_NODES = 1000000;   /* total tree nodes      */

/* ------------------------------------------------------------------ */
/* Entry point
 * ------------------------------------------------------------------ */

/*
 * json_to_doc — parse a JSON document into a doc tree.
 *
 * INPUT :  buf    the document bytes (need not be NUL-terminated);
 *          len    number of bytes;
 *          err    if non-NULL and parsing fails, a malloc'd
 *                 human-readable message ("truncated string at
 *                 byte 42") is stored there for the caller to
 *                 print (and free).
 * OUTPUT:  the root of the tree, or NULL on a parse error.
 *
 * The input is treated as UTF-8 and passed through verbatim: only
 * the JSON escapes (\" \\ \/ \b \f \n \r \t \uXXXX) are decoded.
 * The whole tree is built in memory; the caller owns it.
 */
struct doc_node *json_to_doc (const char *buf, size_t len, char **err);

#endif /* HTTPFS_JSON_H */
