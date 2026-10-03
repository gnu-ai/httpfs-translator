/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * parserfs.h — Shared libnetfs server for the Phase 3 parser
 * translators (htmlfs, jsonfs, csvfs, tsvfs).
 *
 * The three translators expose the very same kind of content — a
 * static document tree (doc.h) built once from the source file —
 * so the whole libnetfs plumbing is written ONCE, here:
 *
 *      parser reads the source  ->  struct doc_node tree
 *      pfs_serve (tree, ...)    ->  a mounted, read-only filesystem
 *
 * A parser translator therefore shrinks to a main() that parses
 * its arguments, builds the tree, and calls pfs_serve().
 *
 * STACKING (the "parser chain" of PLAN.md Phase 3)
 * -----------------------------------------------
 *      settrans -a /web httpfs https://example.org/api/x
 *      settrans -a /api jsonfs  /web/content
 *      cat /api/version          # a JSON member, as a file
 *
 * The source path is opened BEFORE the translator handshake, so
 * mounting /api triggers the httpfs download through normal
 * POSIX open() semantics — translators compose by paths.
 */

#ifndef HTTPFS_PARSERFS_H
#define HTTPFS_PARSERFS_H

#include "doc.h"

/*
 * pfs_serve — become a read-only translator serving TREE.
 *
 * INPUT :  tree   the document tree (owned by the server from
 *                  now on; it is never released);
 *          name   the server name reported to the filesystem
 *                 (netfs_server_name, e.g. "jsonfs");
 *          version  the version string.
 * OUTPUT:  this function only returns on a fatal error; it runs
 *          the RPC loop for the life of the translator.
 *
 * The caller must NOT have called netfs_init() or friends.
 */
void pfs_serve (struct doc_node *tree, const char *name,
                const char *version);

/*
 * pfs_read_file — read a whole file into memory before serving it
 * to a parser.
 *
 * INPUT :  path   the file to open (it may well be the "content"
 *                 view of an httpfs translator);
 *          out    *out receives a malloc'd buffer (not
 *                 NUL-terminated);
 *          outlen its length.
 * OUTPUT:  0, or an errno code.
 */
int pfs_read_file (const char *path, char **out, size_t *outlen);

/* Refuse absurd inputs before they reach a parser: 256 MiB is
   already a huge document for a filesystem tree. */
constexpr size_t PFS_MAX_SOURCE_BYTES = 256 * 1024 * 1024;

#endif /* HTTPFS_PARSERFS_H */
