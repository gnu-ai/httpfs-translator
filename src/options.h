/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * options.h — The GNU base commands --version and --help.
 *
 * Every binary of the httpfs chain (httpfs, htmlfs, jsonfs, csvfs,
 * tsvfs) answers the two base commands of the GNU toolbox before it
 * touches libnetfs or any Hurd machinery: a translator started by
 * settrans must stay inspectable like any other GNU program, from
 * a shell or from a script (make check, packaging, release notes).
 */

#ifndef OPTIONS_H
#define OPTIONS_H

#include <stdbool.h>

/*
 * Scan the command line for --version / -V and --help / -h.
 *
 * When one of them is found, print the standard GNU answer on
 * stdout and return true; the caller then exits with EXIT_SUCCESS
 * without mounting anything.  Otherwise return false and the
 * normal translator startup proceeds.
 *
 * progname — the program name shown in the answers;
 * usage    — the program-specific usage block, printed by --help
 *            after the one-line synopsis.
 */
bool options_handle (int argc, char *argv[],
                     const char *progname, const char *usage);

#endif /* OPTIONS_H */
