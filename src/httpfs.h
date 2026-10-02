/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * httpfs.h — Node structure of the httpfs filesystem.
 *
 * NAVIGATION PRINCIPLE
 * --------------------
 * The translator is mounted on a base URL, for instance:
 *
 *      settrans -a /web httpfs https://example.org
 *
 * Every POSIX directory in the tree then corresponds to a URL
 * path, and every directory — at any level — exposes three virtual
 * files describing ITS OWN resource:
 *
 *      /web/.../content   the raw body received from the server
 *      /web/.../headers   the raw HTTP headers of the response
 *      /web/.../status    the HTTP status code (e.g. "200\n")
 *
 * So   cat /web/content                  downloads the root, while
 *      cat /web/docs/page.html/content   downloads the sub-path.
 * The scheme is uniform, hence predictable for a software agent
 * (an LLM) exploring the web through the translator.
 *
 * The resource of a directory is downloaded ONCE, then kept in
 * memory for the whole life of the node (the "fetched" flag below).
 */

#ifndef HTTPFS_HTTPFS_H
#define HTTPFS_HTTPFS_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#include <curl/curl.h>

#include <hurd/netfs.h>
#include <hurd/ihash.h>

#include "http.h"

/* ------------------------------------------------------------------
 * Kind of a node
 * ------------------------------------------------------------------ */

enum httpfs_kind : int
{
    HTTPFS_DIR     = 0,   /* directory: a (virtual) URL path            */
    HTTPFS_CONTENT = 1,   /* "content" file: body of the resource      */
    HTTPFS_HEADERS = 2,   /* "headers" file: raw response headers       */
    HTTPFS_STATUS  = 3,   /* "status" file: HTTP status code            */
};

/* The three reserved names present in every directory. */
constexpr char HTTPFS_NAME_CONTENT[] = "content";
constexpr char HTTPFS_NAME_HEADERS[] = "headers";
constexpr char HTTPFS_NAME_STATUS[]  = "status";

/* ------------------------------------------------------------------
 * The httpfs-private node
 * ------------------------------------------------------------------ */

/*
 * netnode — private data of one node.
 *
 * A "directory" node owns a URL; a "file" node (content, headers,
 * status) is a view over its parent's resource.  The parent is kept
 * alive by a hard reference held for as long as the child exists
 * (see netfs_attempt_lookup and netfs_node_norefs in netfs.c).
 */
struct netnode
{
    enum httpfs_kind kind;     /* nature of the node (see above)     */

    char *name;                /* local name, for display purposes   */
    char *url;                /* complete URL (directories only)    */

    struct netnode *parent;    /* parent, nullptr for the root        */
    struct node *node;         /* back-pointer to the libnetfs node   */

    /* --- Downloaded resource (directories only) ----------------- */
    pthread_mutex_t res_lock;  /* protects the fields below          */
    bool fetched;              /* has the resource been downloaded?   */
    int res_err;               /* errno of the last download          */
    struct http_response res;  /* body and metadata                   */

    /* --- Reserved for Phase 2: per-name child cache ------------- */
    hurd_ihash_t ihash_table;
    pthread_mutex_t ihash_lock;
};

/* ------------------------------------------------------------------
 * Prototypes (implemented in netfs.c)
 * ------------------------------------------------------------------ */

/* httpfs_init — initialize the (already allocated) root node.
   Returns 0 or an errno code. */
error_t httpfs_init (struct netnode *root, const char *base_url);

/* httpfs_destroy — release everything httpfs_init allocated. */
error_t httpfs_destroy (struct netnode *root);

#endif /* HTTPFS_HTTPFS_H */
