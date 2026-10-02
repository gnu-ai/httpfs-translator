/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * httpfs.h — Node model of the httpfs filesystem (Phase 2).
 *
 * NAVIGATION PRINCIPLE
 * --------------------
 * The translator is mounted on a base URL, for instance:
 *
 *      settrans -a /web httpfs https://example.org
 *
 * Every POSIX directory in the tree corresponds to a URL path, and
 * every directory — at any level — exposes three virtual files
 * describing ITS OWN resource:
 *
 *      /web/.../content   the raw body received from the server
 *      /web/.../headers   the raw HTTP headers of the response
 *      /web/.../status    the HTTP status code (e.g. "200\n")
 *
 * PHASE 2 ADDITIONS (implemented in netfs.c)
 * -----------------------------------------
 *   1. Node cache — every looked-up child is remembered by its
 *      parent (per-name child list) and kept alive by a global MRU
 *      cache of bounded size (HTTPFS_NODE_CACHE_MAX).  Re-opening
 *      a path reuses the node and its downloaded resource instead
 *      of starting from scratch.
 *
 *   2. Range streaming — the resource of a directory is now
 *      described by a small state machine:
 *
 *        UNKNOWN --(HEAD)--> probed metadata {status, size, mtime}
 *          |
 *          +--(first read: GET bytes=0-65535)
 *                206 --> BLOCKS mode: reads fetch only the 64 KiB
 *                                windows they touch (bounded cache)
 *                200 --> FULLBODY mode: the server ignores Range,
 *                                the whole body is kept in memory
 *                                (the Phase 1 behavior)
 *
 *      As a result, stat'ing a view no longer downloads anything
 *      when the server announces the size (HEAD + Content-Length).
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
 * Constants
 * ------------------------------------------------------------------ */

/* Size of one block in BLOCKS mode, and the per-node budget of the
   block cache.  A read fetches only the blocks it touches. */
constexpr size_t HTTPFS_BLOCK_SIZE       = 64 * 1024;
constexpr size_t HTTPFS_BLOCK_CACHE_BYTES = 4 * 1024 * 1024;

/* Maximum number of nodes kept alive by the global MRU node cache
   (each cached node holds one hard reference). */
constexpr int HTTPFS_NODE_CACHE_MAX = 64;

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
 * Resource state machine (directory nodes)
 * ------------------------------------------------------------------ */

enum httpfs_res_state
{
    RES_UNKNOWN = 0,  /* metadata not probed yet                        */
    RES_PROBED,      /* HEAD done; content mode still undetermined      */
    RES_FULLBODY,    /* whole body in res.full (server ignores Range)   */
    RES_BLOCKS,      /* windowed access through the block cache        */
    RES_ERROR,       /* transport-level failure (sticky)               */
};

/* One cached window of a resource in RES_BLOCKS mode. */
struct httpfs_block
{
    struct httpfs_block *older;  /* FIFO ring links (older/newer)       */
    struct httpfs_block *newer;
    uint64_t idx;               /* block number                        */
    size_t len;                 /* useful bytes in data                 */
    char *data;                 /* the window's bytes                   */
};

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

    /* --- Per-parent child list (directories) -------------------- */
    /* All live children of this node.  Guarded by the PARENT's
       child_lock.  The membership lock is always taken BEFORE any
       child node's libnetfs lock is touched, never the other way
       round (see docs/architecture.md §4). */
    struct netnode *children;      /* head of the child list         */
    struct netnode *sibling_prev;
    struct netnode *sibling_next;
    pthread_mutex_t child_lock;

    /* --- Global node-cache links --------------------------------- */
    /* Managed by cache_node() in netfs.c; guarded by node_cache_lock.
       A cached node holds one extra hard reference. */
    struct node *cache_prev;
    struct node *cache_next;

    /* --- Resource of a directory node --------------------------- */
    /* res_lock protects every field of this section. */
    pthread_mutex_t res_lock;

    bool meta_done;            /* HEAD probe done?                    */
    int meta_err;              /* errno of the HEAD probe              */
    struct http_response probe;/* the probe: headers + status + size   */

    enum httpfs_res_state state;
    int content_err;           /* sticky errno of the content engine   */

    struct http_response full; /* RES_FULLBODY: the whole body         */
    long long size;            /* whole-resource size, -1 unknown     */

    /* RES_BLOCKS: window cache, keyed by block number. */
    hurd_ihash_t blocks;
    struct httpfs_block *block_oldest;  /* FIFO ring, oldest first    */
    struct httpfs_block *block_newest;
    size_t block_bytes;        /* bytes held by the block cache        */
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
