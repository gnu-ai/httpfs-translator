/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * httpfs.c — Entry point of the httpfs translator for GNU/Hurd.
 *
 * USAGE
 * -----
 *      settrans -a /web httpfs https://example.org
 *
 * The first non-option argument is the base URL of the "browser";
 * the tree is then explored as described in httpfs.h:
 *
 *      cat /web/content              body of https://example.org
 *      cat /web/docs/x.html/content  body of https://example.org/docs/x.html
 *      cat /web/status               HTTP status code of the root
 *
 * The software agent (LLM) of the GNU AI project can therefore
 * browse the Web by simply reading files.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <error.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "mach-shim.h"
#include <hurd.h>

#include "httpfs.h"
#include "options.h"
#include <hurd/netfs.h>

/* Variables required by libnetfs (defined by the translator). */
char *netfs_server_name = "httpfs";
char *netfs_server_version = "0.4.0";
int netfs_maxsymlinks = 8;

/* Program-specific usage block completed by --help. */
static const char usage_httpfs[] =
    "Mount an HTTP base URL as a filesystem:\n"
    "  settrans -a <mount point> httpfs <URL>\n"
    "Then read the tree: /web/content, /web/status, /web/headers,\n"
    "or a deeper path such as /web/docs/x.html/content.\n"
    "\n"
    "Report bugs at <https://github.com/gnu-ai/httpfs-translator/issues>.\n";

int main (int argc, char *argv[])
{
    error_t err;
    mach_port_t bootstrap;
    struct netnode *nn_root;
    const char *base_url = nullptr;

    /* --- The GNU base commands ----------------------------------
       --version and --help are answered before any Hurd library
       is initialized; the translator stays inspectable like any
       other GNU tool. */
    if (options_handle (argc, argv, "httpfs", usage_httpfs))
        return EXIT_SUCCESS;

    /* --- Retrieve the base URL -----------------------------------
       settrans hands its arguments to the translator; the first
       argument that is not an option is our URL. */
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')
        {
            base_url = argv[i];
            break;
        }

    if (base_url == nullptr)
        error (1, 0, "usage: settrans -a <mount point> httpfs <URL>");

    /* --- Initialize libnetfs ------------------------------------ */
    netfs_init ();

    /* --- Create the root node: the base URL ---------------------- */
    nn_root = malloc (sizeof (struct netnode));
    if (nn_root == nullptr)
        error (1, ENOMEM, "cannot create the root node.");

    err = httpfs_init (nn_root, base_url);
    if (err != 0)
        error (1, err, "cannot initialize the root node.");

    netfs_root_node = netfs_make_node (nn_root);
    if (netfs_root_node == nullptr)
        error (1, ENOMEM, "cannot create the root node.");
    nn_root->node = netfs_root_node;

    /* Minimal stat information for the root; netfs_validate_stat
       recomputes it whenever necessary. */
    memset (&netfs_root_node->nn_stat, 0, sizeof (netfs_root_node->nn_stat));
    netfs_root_node->nn_stat.st_mode = S_IFDIR | 0555;
    netfs_root_node->nn_stat.st_nlink = 1;
    netfs_root_node->nn_translated = S_IFDIR | 0555;

    /* --- Start the server ----------------------------------------
       The bootstrap port connects the translator to the translated
       node; it is provided by the parent process (settrans). */
    task_get_bootstrap_port (mach_task_self (), &bootstrap);
    if (bootstrap == MACH_PORT_NULL)
        error (2, 0, "must be started as a translator (settrans).");

    netfs_startup (bootstrap, 0);

    /* RPC service loop: never returns. */
    netfs_server_loop ();

    return 0;
}
