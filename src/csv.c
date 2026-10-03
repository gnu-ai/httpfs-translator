/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * csv.c — Implementation of the CSV/TSV parser (csv.h).
 *
 * THE STATE MACHINE
 * -----------------
 * One record is a list of fields; one field is a run of bytes in
 * one of two modes, decided by its first character:
 *
 *      unquoted:   bytes up to the delimiter or the record end;
 *      quoted:     bytes up to the closing quote, where a
 *                  doubled quote ("") is an escaped quote.
 *
 * A quoted field may span several lines — the cursor only leaves
 * it at a real closing quote.  This is RFC 4180 verbatim, and
 * the same table every spreadsheet exports.
 *
 *      a,b,"c""d",e
 *       \ \  \   \ \____ field "e"
 *        \ \  \_________ field 'c"d'   (quoted: "" -> ")
 *         \ \___________ field "b"
 *          \____________ field "a"
 *
 * THE TREE
 * --------
 * The first record is the header; its cells name the columns
 * (sanitized and uniquified by the document tree).  Every later
 * record becomes /rows/<n>, holding one file per present cell.
 * The /columns/<name> files are filled in a second pass over an
 * in-memory index of cells — bounded by CSV_MAX_CELLS.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csv.h"

/* ------------------------------------------------------------------ */
/* The in-memory table
 * ------------------------------------------------------------------ */

/* The parser first materializes the table as a plain array of
   rows — simpler to reason about than emitting tree nodes while
   a quoted field is still open. */

struct csv_field
{
    char *text;                 /* decoded bytes, NUL-terminated */
    size_t len;
};

struct csv_row
{
    struct csv_field *cells;
    size_t n;
    size_t cap;
};

static void row_init (struct csv_row *r)
{
    r->cap = 8;
    r->n = 0;
    r->cells = malloc (r->cap * sizeof *r->cells);
    if (r->cells == NULL)
        {
            fprintf (stderr, "csv: out of memory\n");
            abort ();
        }
}

static void row_push (struct csv_row *r, char *text, size_t len)
{
    if (r->n == r->cap)
        {
            r->cap *= 2;
            r->cells = realloc (r->cells, r->cap * sizeof *r->cells);
            if (r->cells == NULL)
                {
                    fprintf (stderr, "csv: out of memory\n");
                    abort ();
                }
        }
    r->cells[r->n].text = text;
    r->cells[r->n].len = len;
    r->n++;
}

static void row_free (struct csv_row *r)
{
    for (size_t i = 0; i < r->n; i++)
        free (r->cells[i].text);
    free (r->cells);
}

/* ------------------------------------------------------------------ */
/* Delimiter sniffing
 * ------------------------------------------------------------------ */

char csv_sniff (const char *buf, size_t len)
{
    static const char candidates[] = { '\t', ';', ',', '|' };
    constexpr int N_CANDIDATES = (int) sizeof candidates;

    size_t eol = 0;
    while (eol < len && buf[eol] != '\n' && buf[eol] != '\r')
        eol++;

    /* Default: the comma (candidates[2]) when the line carries no
       candidate at all — single-column data. */
    int best = 2;
    size_t best_count = 0;

    for (int c = 0; c < N_CANDIDATES; c++)
        {
            size_t count = 0;
            for (size_t i = 0; i < eol; i++)
                if (buf[i] == candidates[c])
                    count++;

            /* Strictly greater: the earlier candidate (the tab)
               wins ties, as documented. */
            if (count > best_count)
                {
                    best = c;
                    best_count = count;
                }
        }

    return candidates[best];
}

/* ------------------------------------------------------------------ */
/* The cursor
 * ------------------------------------------------------------------ */

/*
 * read_field — consume one field at *POS and return its decoded
 * bytes (malloc'd, length in *LEN).  The delimiter and record
 * terminators are consumed as required.
 *
 * RFC 4180 detail: a line break inside a QUOTED field belongs to
 * the field; the record only ends after the closing quote.
 */
static char *read_field (const char *buf, size_t len, size_t *pos,
                         char delim, size_t *flen, int *record_end)
{
    size_t cap = 32, n = 0;
    char *out = malloc (cap);
    if (out == NULL)
        {
            fprintf (stderr, "csv: out of memory\n");
            abort ();
        }

    *record_end = 0;

    if (*pos < len && buf[*pos] == '"')
        {
            (*pos)++;
            for (;;)
                {
                    if (*pos >= len)
                        break;                /* unterminated: tolerant */

                    char c = buf[(*pos)++];

                    if (c == '"')
                        {
                            /* "" is an escaped quote; a single
                               quote ends the field. */
                            if (*pos < len && buf[*pos] == '"')
                                {
                                    if (n + 1 >= cap)
                                        {
                                            cap *= 2;
                                            out = realloc (out, cap);
                                            if (out == NULL)
                                                abort ();
                                        }
                                    out[n++] = '"';
                                    (*pos)++;
                                    continue;
                                }
                            break;
                        }

                    if (n + 1 >= cap)
                        {
                            cap *= 2;
                            out = realloc (out, cap);
                            if (out == NULL)
                                {
                                    fprintf (stderr, "csv: out of memory\n");
                                    abort ();
                                }
                        }
                    out[n++] = c;
                }

            /* Skip up to the delimiter or the record end: some
               producers append garbage after the closing quote. */
            while (*pos < len && buf[*pos] != delim
                   && buf[*pos] != '\n' && buf[*pos] != '\r')
                (*pos)++;
        }
    else
        {
            while (*pos < len && buf[*pos] != delim
                   && buf[*pos] != '\n' && buf[*pos] != '\r')
                {
                    if (n + 1 >= cap)
                        {
                            cap *= 2;
                            out = realloc (out, cap);
                            if (out == NULL)
                                {
                                    fprintf (stderr, "csv: out of memory\n");
                                    abort ();
                                }
                        }
                    out[n++] = buf[(*pos)++];
                }
        }

    /* Consume the delimiter, or the record terminator(s). */
    if (*pos < len && buf[*pos] == delim)
        (*pos)++;
    else
        {
            if (*pos < len && buf[*pos] == '\r')
                (*pos)++;
            if (*pos < len && buf[*pos] == '\n')
                (*pos)++;
            *record_end = 1;
        }

    out[n] = '\0';
    *flen = n;
    return out;
}

/* ------------------------------------------------------------------ */
/* The tree builder
 * ------------------------------------------------------------------ */

struct doc_node *csv_to_doc (const char *buf, size_t len, char delim,
                            char **err)
{
    (void) err;

    struct doc_node *root = doc_new_root ();
    struct csv_row *rows = NULL;
    size_t n_rows = 0, cap_rows = 64;
    size_t cells = 0;
    size_t pos = 0;

    /* ---- Pass 1: split the document into rows ------------------ */
    rows = malloc (cap_rows * sizeof *rows);
    if (rows == NULL)
        {
            fprintf (stderr, "csv: out of memory\n");
            abort ();
        }

    while (pos < len)
        {
            /* Skip completely empty lines (a trailing newline
               would otherwise create a phantom last row). */
            if (buf[pos] == '\n' || buf[pos] == '\r')
                {
                    if (buf[pos] == '\r')
                        pos++;
                    if (pos < len && buf[pos] == '\n')
                        pos++;
                    continue;
                }

            if (n_rows == cap_rows)
                {
                    cap_rows *= 2;
                    rows = realloc (rows, cap_rows * sizeof *rows);
                    if (rows == NULL)
                        {
                            fprintf (stderr, "csv: out of memory\n");
                            abort ();
                        }
                }
            row_init (&rows[n_rows]);

            for (;;)
                {
                    size_t flen;
                    int record_end;
                    char *field = read_field (buf, len, &pos, delim,
                                              &flen, &record_end);
                    row_push (&rows[n_rows], field, flen);
                    if (++cells > CSV_MAX_CELLS)
                        break;

                    if (record_end)
                        break;
                }
            n_rows++;

            if (cells > CSV_MAX_CELLS)
                break;
        }

    if (n_rows == 0)
        {
            free (rows);
            doc_filef (root, "count", "0\n");
            return root;
        }

    /* ---- The header names the columns -------------------------- */
    struct csv_row *header = &rows[0];

    /* One usable name per column: sanitized, unique, and never
       empty ("col<N>" as a fallback). */
    char **colname = malloc (header->n * sizeof *colname);
    if (colname == NULL)
        abort ();

    struct doc_node *dir_header = doc_dir (root, "header");
    struct doc_node *dir_rows = doc_dir (root, "rows");
    struct doc_node *dir_cols = doc_dir (root, "columns");

    for (size_t c = 0; c < header->n; c++)
        {
            char fallback[32];
            const char *base;
            size_t base_len;

            if (header->cells[c].len > 0)
                {
                    base = header->cells[c].text;
                    base_len = header->cells[c].len;
                }
            else
                {
                    snprintf (fallback, sizeof fallback, "col%zu", (size_t) c);
                    base = fallback;
                    base_len = strlen (fallback);
                }

            char *fname = doc_unique_name (dir_cols, base, base_len);
            colname[c] = fname;               /* owned by /columns */

            char idx[32];
            snprintf (idx, sizeof idx, "%zu", c);
            doc_filef (dir_header, idx, "%s\n", fname);
        }

    /* ---- Pass 2: the row view ---------------------------------- */
    for (size_t r = 1; r < n_rows; r++)
        {
            char rname[24];
            snprintf (rname, sizeof rname, "%zu", r - 1);
            struct doc_node *dir_row = doc_dir (dir_rows, rname);

            for (size_t c = 0; c < rows[r].n; c++)
                {
                    if (c < header->n)
                        doc_file (dir_row, colname[c],
                                  strdup (rows[r].cells[c].text),
                                  rows[r].cells[c].len);
                    else
                        {
                            /* A cell beyond the header: it still
                               deserves a name. */
                            char extra[24];
                            snprintf (extra, sizeof extra, "col%zu", c);
                            char *fname = doc_unique_name (dir_row, extra,
                                                           strlen (extra));
                            doc_file (dir_row, fname,
                                      strdup (rows[r].cells[c].text),
                                      rows[r].cells[c].len);
                            free (fname);
                        }
                }
        }

    /* ---- Pass 3: the column view ---------------------------------
       Every value of a column, one per line — the orientation an
       agent prefers ("give me every release year"). */
    for (size_t c = 0; c < header->n; c++)
        {
            struct sview
            {
                char *b;
                size_t len, cap;
            } v = { NULL, 0, 0 };

            v.cap = 64;
            v.b = malloc (v.cap);
            if (v.b == NULL)
                abort ();

            for (size_t r = 1; r < n_rows; r++)
                {
                    if (c < rows[r].n)
                        {
                            size_t need = v.len + rows[r].cells[c].len + 2;
                            if (need > v.cap)
                                {
                                    while (v.cap < need)
                                        v.cap *= 2;
                                    v.b = realloc (v.b, v.cap);
                                    if (v.b == NULL)
                                        abort ();
                                }
                            memcpy (v.b + v.len, rows[r].cells[c].text,
                                    rows[r].cells[c].len);
                            v.len += rows[r].cells[c].len;
                            v.b[v.len++] = '\n';
                        }
                }
            v.b[v.len] = '\0';
            doc_file (dir_cols, colname[c], v.b, v.len);
        }

    doc_filef (root, "count", "%zu\n", n_rows - 1);

    /* ---- Release the materialized table --------------------------
       colname goes first: it still reads HEADER->n, and HEADER
       points INTO rows — which is about to be freed. */
    for (size_t c = 0; c < header->n; c++)
        free (colname[c]);
    free (colname);

    for (size_t r = 0; r < n_rows; r++)
        row_free (&rows[r]);
    free (rows);

    return root;
}
