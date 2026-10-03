/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * html.h — HTML extractor of the Phase 3 htmlfs translator.
 *
 * htmlfs does NOT try to rebuild a full DOM (libxml would be a
 * heavy dependency for a translator); it extracts the structure a
 * software agent actually browses, in one tolerant pass:
 *
 *      /title              text of <title>
 *      /text               the visible text (scripts, styles and
 *                          comments stripped, entities decoded)
 *      /meta/<name>        content of <meta name=...> (or property=)
 *      /headings/<n>       one file per <h1>..<h6>:
 *                          "level<TAB>text" (e.g. "2\tInstall")
 *      /links/<n>/url      target of the n-th <a href>
 *      /links/<n>/text     anchor text
 *
 * Tolerance is the design driver: real-world pages are not valid
 * HTML, and a browser never fails on them.  The tokenizer below
 * therefore never reports errors — at worst an extraction is
 * empty.
 */

#ifndef HTTPFS_HTML_H
#define HTTPFS_HTML_H

#include <stddef.h>

#include "doc.h"

/* Total cap on the number of extracted items, so that a hostile
   page cannot exhaust the host (same policy as json.h). */
constexpr size_t HTML_MAX_ITEMS = 500000;

/* ------------------------------------------------------------------ */
/* Entry point */
/* ------------------------------------------------------------------ */

/*
 * html_to_doc — extract a tree from an HTML document.
 *
 * INPUT :  buf / len    the document bytes (any encoding is
 *                       accepted; only entities are decoded);
 *          err          unused (kept for symmetry with the other
 *                       parsers): extraction never fails.
 * OUTPUT:  the root of the tree (never NULL); the caller owns it.
 */
struct doc_node *html_to_doc (const char *buf, size_t len, char **err);

#endif /* HTTPFS_HTML_H */
