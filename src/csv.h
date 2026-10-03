/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * csv.h — CSV/TSV parser of the Phase 3 csvfs/tsvfs translator.
 *
 * A delimited table becomes a tree of POSIX files, in two
 * complementary orientations:
 *
 *      /count                      number of data rows
 *      /header/0 ... /header/n     the column names, in order
 *      /rows/<n>/<column>          one file per cell (row view)
 *      /columns/<column>           all values of a column, one
 *                                  per line (column view)
 *
 * The row view mirrors how a human reads a table; the column
 * view is the one a software agent prefers ("give me every
 * release year").  Both come from the same single pass.
 */

#ifndef HTTPFS_CSV_H
#define HTTPFS_CSV_H

#include <stddef.h>

#include "doc.h"

/* Maximum number of cells, so that a hostile file cannot exhaust
   the host (same policy as json.h). */
constexpr size_t CSV_MAX_CELLS = 10000000;

/* ------------------------------------------------------------------ */
/* Entry points
 * ------------------------------------------------------------------ */

/*
 * csv_sniff — guess the field delimiter of a document.
 *
 * Among the four candidates '\t', ';', ',' and '|', the one that
 * appears most often on the first line wins (ties favor the
 * tab).  A first line without any of them means single-column
 * data: the comma is returned as a harmless default.
 */
char csv_sniff (const char *buf, size_t len);

/*
 * csv_to_doc — parse a delimited table into a doc tree.
 *
 * INPUT :  buf / len   the document bytes;
 *          delim       the field delimiter (see csv_sniff);
 *          err         unused (kept for symmetry with the other
 *                      parsers): extraction never fails.
 * OUTPUT:  the root of the tree; the caller owns it.
 *
 * The dialect implemented is RFC 4180: fields may be quoted
 * ("..." with "" as an escaped quote), a record ends at LF or
 * CRLF, and the first record is the header.  A quoted field may
 * contain newlines and delimiters verbatim.
 *
 * Ragged rows are tolerated: a missing cell simply has no file,
 * a surplus cell extends the row directory (named "col<N>" for
 * columns beyond the header).
 */
struct doc_node *csv_to_doc (const char *buf, size_t len, char delim,
                            char **err);

#endif /* HTTPFS_CSV_H */
