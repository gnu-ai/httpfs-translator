/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * csvfs.c — Entry point of the csvfs/tsvfs translator (Phase 3).
 *
 * USAGE
 * ------
 *      settrans -a /table csvfs /path/to/data.csv
 *      settrans -a /table csvfs -d ';' /path/to/data.csv
 *      settrans -a /table tsvfs /path/to/data.tsv
 *
 * or, in the parser chain on top of the httpfs transport:
 *
 *      settrans -a /web httpfs https://example.org/stats.csv
 *      settrans -a /table csvfs /web/content
 *      cat /table/columns/year     # a whole column, one value
 *                                  # per line
 *
 * The same binary serves as csvfs and tsvfs: when invoked under
 * the tsvfs name the default delimiter is the tab.  Without an
 * explicit -d option csvfs sniffs the delimiter from the header
 * line (csv_sniff).  The source is read ONCE at mount time; the
 * tree it becomes is described in csv.h.  Re-mount to refresh.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <error.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csv.h"
#include "parserfs.h"

int main (int argc, char *argv[])
{
    const char *source = nullptr;
    char *buf = nullptr;
    size_t len = 0;
    char delim = '\0';
    bool as_tsv = false;

    /* --- Parse the command line ------------------------------- */
    for (int i = 1; i < argc; i++)
        {
            if (strcmp (argv[i], "-d") == 0 && i + 1 < argc)
                {
                    delim = argv[++i][0];
                    if (delim == 't')          /* -d '\t' spelled out */
                        delim = '\t';
                    continue;
                }
            if (argv[i][0] != '-')
                {
                    source = argv[i];
                    continue;
                }
        }

    if (source == nullptr)
        error (1, 0, "usage: settrans -a <mount point> "
                     "csvfs [-d <delim>] <source file>");

    /* Invoked under another name?  tsvfs wants tabs by default. */
    {
        char *prog = strdup (argv[0]);
        const char *base = basename (prog);
        as_tsv = strcmp (base, "tsvfs") == 0;
        free (prog);
    }

    int rc = pfs_read_file (source, &buf, &len);
    if (rc != 0)
        error (1, rc, "cannot read %s", source);

    if (delim == '\0')
        delim = as_tsv ? '\t' : csv_sniff (buf, len);

    /* Never returns; the tree belongs to the server from now on. */
    pfs_serve (csv_to_doc (buf, len, delim, nullptr),
               as_tsv ? "tsvfs" : "csvfs", "0.4.0");
    return 0;
}
