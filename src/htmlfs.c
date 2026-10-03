/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * htmlfs.c — Entry point of the htmlfs translator (Phase 3).
 *
 * USAGE
 * ------
 *      settrans -a /page htmlfs /path/to/document.html
 *
 * or, in the parser chain on top of the httpfs transport:
 *
 *      settrans -a /web httpfs https://example.org/doc.html
 *      settrans -a /page htmlfs /web/content
 *      cat /page/title          # the <title>, as a plain file
 *      ls /page/links           # the links, as directories
 *
 * The first non-option argument is the path of the HTML source;
 * it is read ONCE at mount time and extracted into the tree
 * described in html.h (extraction never fails: an unparseable
 * page yields an empty tree, the way a browser renders something
 * anyway).  Re-mount to refresh.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <error.h>
#include <stdio.h>
#include <stdlib.h>

#include "html.h"
#include "options.h"
#include "parserfs.h"

/* Program-specific usage block completed by --help. */
static const char usage_htmlfs[] =
    "Mount an HTML document as a filesystem:\n"
    "  settrans -a <mount point> htmlfs <source file>\n"
    "The page is read once at mount time; re-mount to refresh.\n"
    "\n"
    "Report bugs at <https://github.com/gnu-ai/httpfs-translator/issues>.\n";

int main (int argc, char *argv[])
{
    const char *source = nullptr;
    char *buf = nullptr;
    size_t len = 0;

    /* --- The GNU base commands ----------------------------------
       --version and --help are answered before anything is
       mounted. */
    if (options_handle (argc, argv, "htmlfs", usage_htmlfs))
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
                     "htmlfs <source file>");

    int rc = pfs_read_file (source, &buf, &len);
    if (rc != 0)
        error (1, rc, "cannot read %s", source);

    /* Never returns; the tree belongs to the server from now on. */
    pfs_serve (html_to_doc (buf, len, nullptr), "htmlfs", "0.4.0");
    return 0;
}
