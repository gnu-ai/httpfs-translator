/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * options.c — The GNU base commands --version and --help.
 *
 * The answers follow the GNU coding standards so that every
 * translator of the chain behaves like the rest of a GNU system:
 *
 *   - --version prints the program name, the project, the version
 *     and the license — nothing else, on stdout;
 *   - --help prints the usage, the options, and where to report
 *     bugs, on stdout;
 *   - both exit with status 0, before any Hurd library is
 *     initialized.
 *
 * The version number comes from configure (config.h), the single
 * source of truth of the release: it is never duplicated here.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "options.h"

/* --- The --version answer ------------------------------------------
   Program name, project, version, license — the layout expected by
   scripts that grep the version of a tool. */

static void
print_version (const char *progname)
{
    printf ("%s (GNU AI) %s\n", progname, VERSION);
    printf ("License GPLv3+: GNU GPL version 3 or later"
            " <https://gnu.org/licenses/gpl.html>.\n");
    printf ("This is free software: you are free to change"
            " and redistribute it.\n");
    printf ("There is NO WARRANTY, to the extent permitted by law.\n");
}

/* --- The --help answer ---------------------------------------------
   The synopsis is shared; each binary adds its own usage block
   (its specific arguments and the settrans invocation). */

static void
print_help (const char *progname, const char *usage)
{
    printf ("Usage: %s [OPTION]...\n", progname);
    printf ("  -h, --help     display this help and exit\n");
    printf ("  -V, --version  output version information and exit\n");
    printf ("\n%s", usage);
}

/* --- The scanner ----------------------------------------------------
   A translator receives its arguments from settrans; anything
   that is not one of the two base commands is left untouched. */

bool
options_handle (int argc, char *argv[],
                const char *progname, const char *usage)
{
    for (int i = 1; i < argc; i++)
        {
            if (strcmp (argv[i], "--version") == 0
                || strcmp (argv[i], "-V") == 0)
                {
                    print_version (progname);
                    return true;
                }
            if (strcmp (argv[i], "--help") == 0
                || strcmp (argv[i], "-h") == 0)
                {
                    print_help (progname, usage);
                    return true;
                }
        }
    return false;
}
