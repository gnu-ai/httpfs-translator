/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * jsonfs.c — Entry point of the jsonfs translator (Phase 3).
 *
 * USAGE
 * ------
 *      settrans -a /api jsonfs /path/to/document.json
 *
 * or, in the parser chain on top of the httpfs transport:
 *
 *      settrans -a /web httpfs https://example.org/api/info
 *      settrans -a /api jsonfs /web/content
 *      cat /api/version        # a JSON member, as a plain file
 *
 * The first non-option argument is the path of the JSON source;
 * it is read ONCE at mount time and parsed into the tree described
 * in json.h.  Re-mount to refresh.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>

#include "json.h"
#include "options.h"
#include "parserfs.h"

/* Program-specific usage block completed by --help. */
static const char usage_jsonfs[] =
    "Mount a JSON document as a filesystem:\n"
    "  settrans -a <mount point> jsonfs <source file>\n"
    "The document is read once at mount time; re-mount to refresh.\n"
    "\n"
    "Report bugs at <https://github.com/gnu-ai/httpfs-translator/issues>.\n";

int main (int argc, char *argv[])
{
    const char *source = nullptr;
    char *buf = nullptr;
    size_t len = 0;
    struct doc_node *tree;
    char *err = nullptr;

    /* --- The GNU base commands ----------------------------------
       --version and --help are answered before anything is
       mounted. */
    if (options_handle (argc, argv, "jsonfs", usage_jsonfs))
        return EXIT_SUCCESS;

    /* The first non-option argument is the source document. */
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')
            {
                source = argv[i];
                break;
            }

    if (source == nullptr)
        error (1, 0, "usage: settrans -a <mount point> "
                     "jsonfs <source file>");

    int rc = pfs_read_file (source, &buf, &len);
    if (rc != 0)
        error (1, rc, "cannot read %s", source);

    tree = json_to_doc (buf, len, &err);
    free (buf);

    if (tree == nullptr)
        {
            error (0, 0, "jsonfs: %s: %s", source,
                   err != nullptr ? err : "parse error");
            free (err);
            return 1;
        }

    /* Never returns; the tree belongs to the server from now on. */
    pfs_serve (tree, "jsonfs", "0.4.0");
    return 0;
}
