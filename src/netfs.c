/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * netfs.c — The heart of the httpfs translator: libnetfs callbacks.
 *
 * NAVIGATION MODEL
 * ----------------
 *   POSIX directory            <->  (virtual, unbounded) URL path
 *   /.../content               <->  raw body of the response
 *   /.../headers               <->  raw HTTP headers (of the probe)
 *   /.../status                <->  HTTP code ("200\n")
 *
 * PHASE 2
 * -------
 *   1. Node cache.  Every child created by a lookup is remembered
 *      in its parent's child list and kept alive by a global MRU
 *      cache of HTTPFS_NODE_CACHE_MAX entries (the ftpfs pattern:
 *      the cache holds hard references, eviction releases them).
 *      Re-opening a path finds the same node again and reuses its
 *      downloaded resource.
 *
 *   2. Range streaming.  A directory's resource is a small state
 *      machine (see httpfs.h):
 *
 *        ensure_metadata : one HEAD -> status, size, Last-Modified
 *        ensure_content  : one ranged GET on first read
 *                            206 -> RES_BLOCKS: reads fetch only the
 *                                    64 KiB windows they touch
 *                            200 -> RES_FULLBODY: server ignores
 *                                    Range; whole body in memory
 *
 * LOCKS AND REFERENCES
 * --------------------
 *   np->lock         libnetfs node lock (callbacks see it locked)
 *   nn->res_lock     protects the resource state machine
 *   nn->child_lock   protects the parent's child list
 *   node_cache_lock  protects the global MRU cache
 *
 *   Locking order:  node np->lock  ->  child_lock / res_lock /
 *   node_cache_lock.  Nothing ever takes an np->lock while holding
 *   a child_lock, and the node cache is only touched while the
 *   *newly created* node is locked (never the LRU victim), so the
 *   eviction cascade can never deadlock on a lock we hold.
 *
 *   Every child holds one hard reference on its parent node (taken
 *   in netfs_attempt_lookup, returned in netfs_node_norefs); the
 *   traversal reference held by libnetfs guarantees that the
 *   eviction cascade can never free the directory being looked
 *   through.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <hurd/netfs.h>
#include <hurd/iohelp.h>

#include "httpfs.h"

/* ------------------------------------------------------------------
 * Local constants
 * ------------------------------------------------------------------ */

/* Directory entry construction (same convention as the other Hurd
   translators, cf. ftpfs): the size of an entry is the fixed part
   + the name + "\0", rounded up to a multiple of 4. */
#define DIRENT_ALIGN     4
#define DIRENT_NAME_OFFS offsetof (struct dirent, d_name)
#define DIRENT_LEN(namelen)                                                \
    ((DIRENT_NAME_OFFS + (namelen) + 1 + (DIRENT_ALIGN - 1))               \
     & ~((size_t) (DIRENT_ALIGN - 1)))

/* Size of the memory region handed back to libnetfs.  Careful:
   netfs_S_dir_readdir sets data_dealloc = 1, so this buffer MUST
   come from mmap (VM memory), never from malloc. */
#define DIRENTS_SIZE     (8 * 1024)

/* POSIX spells the anonymous mapping MAP_ANONYMOUS; some systems
   (older Hurd headers) only know the historical MAP_ANON alias. */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* ------------------------------------------------------------------
 * Utilities
 * ------------------------------------------------------------------ */

/* fnv1a — 64-bit FNV-1a hash of a string.  Used to derive inode
   numbers that are stable and reproducible from the URLs. */
static uint64_t fnv1a (const char *s)
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *) s; *p != '\0'; p++)
    {
        h ^= (uint64_t) *p;
        h *= 1099511628211ULL;
    }
    return h;
}

/* ino_of — inode number of a node, derived from its URL and its
   kind (the content/headers/status views of one directory thus get
   distinct inode numbers). */
static ino_t ino_of (const struct netnode *nn)
{
    uint64_t h = fnv1a (nn->kind == HTTPFS_DIR ? nn->url : nn->parent->url);
    return (ino_t) (h ^ ((uint64_t) nn->kind << 56));
}

/* reserved_name — find the kind of a reserved name.
   Returns true when NAME is one of the virtual files. */
static bool reserved_name (const char *name, enum httpfs_kind *kind)
{
    if (strcmp (name, HTTPFS_NAME_CONTENT) == 0)
        *kind = HTTPFS_CONTENT;
    else if (strcmp (name, HTTPFS_NAME_HEADERS) == 0)
        *kind = HTTPFS_HEADERS;
    else if (strcmp (name, HTTPFS_NAME_STATUS) == 0)
        *kind = HTTPFS_STATUS;
    else
        return false;
    return true;
}

/* ------------------------------------------------------------------
 * The resource engine (directory nodes)
 * ------------------------------------------------------------------ */

/*
 * ensure_metadata — run the HEAD probe of NN's resource, once.
 *
 * The result (status, size, filetime, raw headers) is sticky: it is
 * remembered for the whole life of the node.  Transport-level
 * failures are recorded in meta_err; HTTP statuses, whatever they
 * are, are information, not errors.
 */
static int ensure_metadata (struct netnode *nn)
{
    int err;

    pthread_mutex_lock (&nn->res_lock);

    if (! nn->meta_done)
    {
        err = http_head (nn->url, &nn->probe);
        nn->meta_err = err;
        nn->meta_done = true;
        if (err == 0)
        {
            nn->size = nn->probe.size_total;
            nn->state = RES_PROBED;   /* content mode now decidable */
        }
    }
    else
        err = nn->meta_err;

    pthread_mutex_unlock (&nn->res_lock);
    return err;
}

/*
 * install_block — put a fetched window into the block cache,
 * evicting the oldest blocks when the per-node budget is exceeded.
   NN's res_lock must be held; BLK ownership is transferred to the
   cache on success.  Returns 0, or ENOMEM after freeing BLK.
 */
static int install_block (struct netnode *nn, struct httpfs_block *blk)
{
    if (nn->blocks == nullptr
        && hurd_ihash_create (&nn->blocks, HURD_IHASH_NO_LOCP) != 0)
    {
        free (blk->data);
        free (blk);
        return ENOMEM;
    }

    hurd_ihash_add (nn->blocks, (hurd_ihash_key_t) blk->idx, blk);
    blk->older = nn->block_newest;
    blk->newer = nullptr;
    if (nn->block_newest != nullptr)
        nn->block_newest->newer = blk;
    else
        nn->block_oldest = blk;
    nn->block_newest = blk;
    nn->block_bytes += blk->len;

    /* Evict from the FIFO front while the budget is exceeded.  The
       block just installed is at the back, so it survives as long
       as the budget covers at least one block — which it does. */
    while (nn->block_bytes > HTTPFS_BLOCK_CACHE_BYTES
           && nn->block_oldest != nullptr)
    {
        struct httpfs_block *victim = nn->block_oldest;

        hurd_ihash_remove (nn->blocks, (hurd_ihash_key_t) victim->idx);
        nn->block_oldest = victim->newer;
        if (nn->block_oldest != nullptr)
            nn->block_oldest->older = nullptr;
        else
            nn->block_newest = nullptr;
        nn->block_bytes -= victim->len;

        free (victim->data);
        free (victim);
    }

    return 0;
}

/*
 * fetch_block — obtain the contents of block IDX of NN's resource.
   NN's res_lock must be held.  Returns 0 with *BLK pointing to the
   cached block (owned by the cache), or an errno.
 *
 * A 416 answer means the window lies beyond the resource: an empty
 * block is served, which read_blocks interprets as end of file.
 */
static int fetch_block (struct netnode *nn, uint64_t idx,
                        struct httpfs_block **blk)
{
    struct httpfs_block *b = hurd_ihash_find (nn->blocks,
                                              (hurd_ihash_key_t) idx);
    if (b != nullptr)
    {
        *blk = b;
        return 0;                   /* cache hit */
    }

    /* The window of block IDX: [idx*BS, min(size, (idx+1)*BS)).
       When the total size is unknown, ask for a full block and let
       a short answer mark the end of the resource. */
    size_t start = (size_t) (idx * HTTPFS_BLOCK_SIZE);
    size_t end = start + HTTPFS_BLOCK_SIZE - 1;
    if (nn->size >= 0 && (long long) end >= nn->size)
        end = (size_t) nn->size - 1;

    struct http_response resp = { nullptr, 0, nullptr, 0, -1, -1 };
    int err = http_fetch_range (nn->url, start, end, &resp);
    if (err != 0)
        return err;                 /* transport failure */

    if (resp.status == 416)
    {
        /* Beyond the resource: the caller learns the true size. */
        http_response_release (&resp);
        b = malloc (sizeof *b);
        if (b == nullptr)
            return ENOMEM;
        b->idx = idx;
        b->len = 0;
        b->data = nullptr;
        b->older = b->newer = nullptr;
        err = install_block (nn, b);
        if (err != 0)
            return err;
        *blk = b;
        return 0;
    }

    if (resp.status != 206)
    {
        /* The server changed its mind mid-stream (it answered 206
           for the very first probe, but not now).  This is not
           something a read can recover from: report an error. */
        http_response_release (&resp);
        return EIO;
    }

    b = malloc (sizeof *b);
    if (b == nullptr)
    {
        http_response_release (&resp);
        return ENOMEM;
    }
    b->idx = idx;
    b->len = resp.body_len;
    b->data = resp.body;           /* ownership moves to the cache  */
    b->older = b->newer = nullptr;
    resp.body = nullptr;
    http_response_release (&resp);  /* frees only the headers now */

    err = install_block (nn, b);
    if (err != 0)
        return err;

    *blk = b;
    return 0;
}

/*
 * ensure_content — determine how the resource must be read.
   NN's res_lock must be held.
 *
 *   RES_PROBED + first read:
 *     ranged GET of the first block:
 *       206 -> RES_BLOCKS (and the block is already cached);
 *       200 -> RES_FULLBODY: the server ignores Range, and the
 *              body just received IS the whole document;
 *       >=400 -> RES_FULLBODY with the error page as the body;
 *       transport error -> RES_ERROR (sticky).
 */
static int ensure_content (struct netnode *nn)
{
    int err;

    if (nn->state == RES_PROBED)
    {
        if (nn->size == 0)
        {
            /* Empty resource: nothing to fetch, serve zeroes... we
               simply complete with an empty full-body response. */
            nn->full = (struct http_response) { nullptr, 0, nullptr,
                                                nn->probe.status, 0, -1 };
            nn->state = RES_FULLBODY;
            return 0;
        }

        struct http_response resp = { nullptr, 0, nullptr, 0, -1, -1 };
        err = http_fetch_range (nn->url, 0, HTTPFS_BLOCK_SIZE - 1, &resp);
        if (err != 0)
        {
            nn->content_err = err;
            nn->state = RES_ERROR;
            return err;
        }

        if (resp.status == 206)
        {
            /* Windowed access from now on. */
            long long total = resp.size_total;
            struct httpfs_block *b = malloc (sizeof *b);
            if (b == nullptr)
            {
                http_response_release (&resp);
                nn->content_err = ENOMEM;
                nn->state = RES_ERROR;
                return ENOMEM;
            }
            b->idx = 0;
            b->len = resp.body_len;
            b->data = resp.body;
            b->older = b->newer = nullptr;
            resp.body = nullptr;
            http_response_release (&resp);

            nn->state = RES_BLOCKS;
            if (nn->size < 0 && total > 0)
                nn->size = total;
            install_block (nn, b);
            return 0;
        }

        /* 200 (server ignored the Range header) or any other
           status: keep the whole body and read from it forever. */
        nn->full = resp;
        if (nn->size < 0)
            nn->size = resp.size_total > 0 ? resp.size_total
                                           : (long long) resp.body_len;
        nn->state = RES_FULLBODY;
        return 0;
    }

    return nn->state == RES_ERROR ? nn->content_err : 0;
}

/*
 * resource_size — the size of NN's readable content, in bytes.
   Triggers ensure_metadata and, when the size cannot be known
   without it, ensure_content.
 */
static long long resource_size (struct netnode *nn)
{
    int err = ensure_metadata (nn);
    if (err != 0)
        return -1;

    pthread_mutex_lock (&nn->res_lock);

    long long size = nn->size;
    if (size < 0 || nn->probe.status >= 400)
    {
        /* Unknown size, or an error page whose length only a GET
           reveals: run the content engine once. */
        if (nn->state == RES_PROBED)
            err = ensure_content (nn);
        else if (nn->state == RES_ERROR)
            err = nn->content_err;

        if (nn->state == RES_FULLBODY)
            size = (long long) nn->full.body_len;
        else if (nn->state == RES_BLOCKS)
            size = nn->size;
        else
            size = err == 0 ? 0 : -1;
    }

    pthread_mutex_unlock (&nn->res_lock);
    return size;
}

/*
 * read_blocks — copy the window [OFFSET, OFFSET+LEN) of NN's
   resource into BUF, fetching only the blocks it touches.
   Returns 0 and sets *LEN to the number of bytes copied (0 = EOF).
 */
static int read_blocks (struct netnode *nn, uint64_t offset, size_t *len,
                        char *buf)
{
    uint64_t remaining = *len;
    uint64_t done = 0;

    /* Clip to the end of the resource when its size is known. */
    if (nn->size >= 0)
    {
        uint64_t size = (uint64_t) nn->size;
        if (offset >= size)
        {
            *len = 0;
            return 0;               /* EOF */
        }
        if (offset + remaining > size)
            remaining = size - offset;
    }

    while (remaining > 0)
    {
        uint64_t idx = (offset + done) / HTTPFS_BLOCK_SIZE;
        size_t skip = (size_t) ((offset + done) % HTTPFS_BLOCK_SIZE);
        struct httpfs_block *blk = nullptr;
        int err = fetch_block (nn, idx, &blk);
        if (err != 0)
        {
            nn->content_err = err;
            nn->state = RES_ERROR;
            if (done == 0)
            {
                *len = 0;
                return err;
            }
            break;                  /* serve what we already have */
        }

        if (blk->len <= skip)
        {
            /* The server returned a short block: end of resource. */
            if (nn->size < 0)
                nn->size = (long long) (idx * HTTPFS_BLOCK_SIZE
                                        + blk->len);
            break;
        }

        size_t n = blk->len - skip;
        if (n > remaining)
            n = (size_t) remaining;
        memcpy (buf + done, blk->data + skip, n);
        done += n;
        remaining -= n;

        /* A short block is the last one: stop here.  (When the size
           was known the read has been clipped already, so this only
           saves a useless request when it was not.) */
        if (blk->len < HTTPFS_BLOCK_SIZE)
        {
            if (nn->size < 0)
                nn->size = (long long) (idx * HTTPFS_BLOCK_SIZE + blk->len);
            break;
        }
    }

    *len = (size_t) done;
    return 0;
}

/*
 * release_resource — free everything NN's resource holds.
   Called from netfs_node_norefs with no lock held.
 */
static void release_resource (struct netnode *nn)
{
    pthread_mutex_lock (&nn->res_lock);

    if (nn->blocks != nullptr)
    {
        struct httpfs_block *b = nn->block_oldest;
        while (b != nullptr)
        {
            struct httpfs_block *next = b->newer;
            free (b->data);
            free (b);
            b = next;
        }
        hurd_ihash_destroy (nn->blocks);
        hurd_ihash_free (nn->blocks);
        nn->blocks = nullptr;
    }
    nn->block_oldest = nn->block_newest = nullptr;
    nn->block_bytes = 0;

    http_response_release (&nn->probe);
    http_response_release (&nn->full);

    pthread_mutex_unlock (&nn->res_lock);
}

/* ------------------------------------------------------------------
 * The global node cache
 * ------------------------------------------------------------------ */

/* One MRU list of nodes, each holding one hard reference.  Only
   cache_node takes node_cache_lock, always with the *new* node
   locked; the eviction path may therefore block on the LRU
   victim's locks without any risk of inversion. */
static pthread_mutex_t node_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct node *cache_mru = nullptr;
static struct node *cache_lru = nullptr;
static int cache_len = 0;

/* cache_node — remember NP (locked, referenced) in the MRU cache
   and evict the oldest entries beyond HTTPFS_NODE_CACHE_MAX. */
static void cache_node (struct node *np)
{
    struct netnode *nn = np->nn;

    pthread_mutex_lock (&node_cache_lock);

    nn->cache_prev = nullptr;
    nn->cache_next = cache_mru;
    if (cache_mru != nullptr)
        cache_mru->nn->cache_prev = np;
    cache_mru = np;
    if (cache_lru == nullptr)
        cache_lru = np;
    netfs_nref (np);               /* the cache's own reference */
    cache_len++;

    /* Evict the oldest entries beyond the maximum. */
    while (cache_len > HTTPFS_NODE_CACHE_MAX)
    {
        struct node *victim = cache_lru;
        struct netnode *vn = victim->nn;

        if (vn->cache_prev != nullptr)
            vn->cache_prev->nn->cache_next = vn->cache_next;
        else
            cache_mru = vn->cache_next;
        if (vn->cache_next != nullptr)
            vn->cache_next->nn->cache_prev = vn->cache_prev;
        else
            cache_lru = vn->cache_prev;
        vn->cache_prev = vn->cache_next = nullptr;
        cache_len--;

        netfs_nrele (victim);     /* may free it, may not */
    }

    pthread_mutex_unlock (&node_cache_lock);
}

/* ------------------------------------------------------------------
 * Node creation and the child list
 * ------------------------------------------------------------------ */

/*
 * create_child — create inside DIR (locked) a node NAME of kind
   KIND.  On output: *NP designates the new node, LOCKED and holding
   one reference.  The child is entered in DIR's child list and in
   the global node cache.  Returns 0 or an errno code.
 */
static error_t create_child (struct node *dir, const char *name,
                             enum httpfs_kind kind, struct node **np)
{
    struct netnode *nn = malloc (sizeof *nn);
    struct netnode *dn = dir->nn;

    *np = nullptr;
    if (nn == nullptr)
        return ENOMEM;

    /* Fields common to every kind. */
    nn->kind = kind;
    nn->name = strdup (name);
    nn->parent = dn;
    nn->node = nullptr;
    nn->children = nullptr;
    nn->sibling_prev = nn->sibling_next = nullptr;
    nn->cache_prev = nn->cache_next = nullptr;
    pthread_mutex_init (&nn->child_lock, nullptr);

    nn->meta_done = false;
    nn->meta_err = 0;
    nn->probe = (struct http_response) { nullptr, 0, nullptr, 0, -1, -1 };
    nn->state = RES_UNKNOWN;
    nn->content_err = 0;
    nn->full = (struct http_response) { nullptr, 0, nullptr, 0, -1, -1 };
    nn->size = -1;
    nn->blocks = nullptr;
    nn->block_oldest = nn->block_newest = nullptr;
    nn->block_bytes = 0;
    pthread_mutex_init (&nn->res_lock, nullptr);

    /* A URL is only defined for directories; the views refer to
       their parent's resource. */
    nn->url = (kind == HTTPFS_DIR) ? http_url_join (dn->url, name) : nullptr;

    if (nn->name == nullptr || (kind == HTTPFS_DIR && nn->url == nullptr))
        goto memerr;

    /* The child keeps its parent alive. */
    netfs_nref (dir);

    *np = netfs_make_node (nn);
    if (*np == nullptr)
    {
        netfs_nrele (dir);
        goto memerr;
    }
    nn->node = *np;

    /* libnetfs contract: the found node must be locked.  The node
       is brand new and unreachable, so this cannot block. */
    pthread_mutex_lock (&(*np)->lock);

    /* Enter the child in the parent's list. */
    pthread_mutex_lock (&dn->child_lock);
    nn->sibling_next = dn->children;
    if (dn->children != nullptr)
        dn->children->sibling_prev = nn;
    dn->children = nn;
    pthread_mutex_unlock (&dn->child_lock);

    /* Keep the node alive for future lookups. */
    cache_node (*np);

    return 0;

memerr:
    free (nn->name);
    free (nn->url);
    pthread_mutex_destroy (&nn->res_lock);
    pthread_mutex_destroy (&nn->child_lock);
    free (nn);
    return ENOMEM;
}

/*
 * find_child — return the live child of DN named NAME, or NULL.
   DN's child_lock must NOT be held; it is taken here.
 */
static struct netnode *find_child (struct netnode *dn, const char *name)
{
    struct netnode *cn;

    pthread_mutex_lock (&dn->child_lock);
    for (cn = dn->children; cn != nullptr; cn = cn->sibling_next)
        if (strcmp (cn->name, name) == 0)
            break;
    pthread_mutex_unlock (&dn->child_lock);
    return cn;
}

/*
 * unlink_child — remove NN from its parent's child list.
   Called from netfs_node_norefs with the dying node's libnetfs lock
   held; taking the parent's child_lock is the only lock needed.
 */
static void unlink_child (struct netnode *nn)
{
    struct netnode *parent = nn->parent;

    pthread_mutex_lock (&parent->child_lock);
    if (nn->sibling_prev != nullptr)
        nn->sibling_prev->sibling_next = nn->sibling_next;
    else
        parent->children = nn->sibling_next;
    if (nn->sibling_next != nullptr)
        nn->sibling_next->sibling_prev = nn->sibling_prev;
    nn->sibling_prev = nn->sibling_next = nullptr;
    pthread_mutex_unlock (&parent->child_lock);
}

/* ------------------------------------------------------------------
 * Tree life cycle (httpfs_init / httpfs_destroy)
 * ------------------------------------------------------------------ */

error_t httpfs_init (struct netnode *root, const char *base_url)
{
    CURLcode rc = curl_global_init (CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK)
        return EIO;

    root->kind = HTTPFS_DIR;
    root->name = strdup ("/");
    root->url = strdup (base_url);
    root->parent = nullptr;
    root->node = nullptr;
    root->children = nullptr;
    root->sibling_prev = root->sibling_next = nullptr;
    root->cache_prev = root->cache_next = nullptr;

    root->meta_done = false;
    root->meta_err = 0;
    root->probe = (struct http_response) { nullptr, 0, nullptr, 0, -1, -1 };
    root->state = RES_UNKNOWN;
    root->content_err = 0;
    root->full = (struct http_response) { nullptr, 0, nullptr, 0, -1, -1 };
    root->size = -1;
    root->blocks = nullptr;
    root->block_oldest = root->block_newest = nullptr;
    root->block_bytes = 0;

    if (root->name == nullptr || root->url == nullptr)
    {
        free (root->name);
        free (root->url);
        curl_global_cleanup ();
        return ENOMEM;
    }

    if (pthread_mutex_init (&root->child_lock, nullptr) != 0
        || pthread_mutex_init (&root->res_lock, nullptr) != 0)
    {
        pthread_mutex_destroy (&root->child_lock);
        pthread_mutex_destroy (&root->res_lock);
        curl_global_cleanup ();
        free (root->name);
        free (root->url);
        return ENOMEM;
    }

    return 0;
}

error_t httpfs_destroy (struct netnode *root)
{
    release_resource (root);
    free (root->name);
    free (root->url);
    root->name = nullptr;
    root->url = nullptr;

    pthread_mutex_destroy (&root->res_lock);
    pthread_mutex_destroy (&root->child_lock);
    curl_global_cleanup ();
    return 0;
}

/* ------------------------------------------------------------------
 * libnetfs callbacks — reading the tree
 * ------------------------------------------------------------------ */

/* netfs_attempt_lookup — resolve NAME inside DIR.
   Contract: on success *NP is locked and referenced; DIR is
   unlocked no matter what (unless *NP == DIR). */
error_t netfs_attempt_lookup (struct iouser *user, struct node *dir,
                              const char *name, struct node **np)
{
    struct netnode *dn = dir->nn;
    enum httpfs_kind kind;
    error_t err;

    (void) user;               /* no access control for now */
    *np = nullptr;

    if (dn->kind != HTTPFS_DIR)
    {
        pthread_mutex_unlock (&dir->lock);
        return ENOTDIR;
    }

    /* "." — the directory itself: return it as-is, still locked,
       with one extra reference. */
    if (strcmp (name, ".") == 0)
    {
        netfs_nref (dir);
        *np = dir;
        return 0;
    }

    /* ".." — the parent directory (the root case is normally
       intercepted by libnetfs itself; defensively, a ".." at the
       root returns the root). */
    if (strcmp (name, "..") == 0)
    {
        struct node *parent = dn->parent != nullptr ? dn->parent->node : dir;

        pthread_mutex_lock (&parent->lock);
        netfs_nref (parent);
        *np = parent;
        pthread_mutex_unlock (&dir->lock);
        return 0;
    }

    /* Is there already a live child under this name?  The node
       cache keeps it referenced, so taking one more reference
       right here is safe. */
    struct netnode *cn = find_child (dn, name);
    if (cn != nullptr)
    {
        struct node *hit = cn->node;

        netfs_nref (hit);
        pthread_mutex_unlock (&dir->lock);
        pthread_mutex_lock (&hit->lock);
        *np = hit;
        return 0;
    }

    /* Miss: create the child.  Reserved names become the views on
       this directory's resource; anything else extends the URL. */
    if (reserved_name (name, &kind))
        err = create_child (dir, name, kind, np);
    else
        err = create_child (dir, name, HTTPFS_DIR, np);

    pthread_mutex_unlock (&dir->lock);
    return err;
}

/* netfs_get_dirents — list the entries of a directory.
   The listing is identical at every level: the three views. */
error_t netfs_get_dirents (struct iouser *cred, struct node *dir, int entry,
                           int nentries, char **data,
                           mach_msg_type_number_t *datacnt, vm_size_t bufsize,
                           int *amt)
{
    static const char *const names[] =
        { HTTPFS_NAME_CONTENT, HTTPFS_NAME_HEADERS, HTTPFS_NAME_STATUS };
    constexpr int NUM_ENTRIES = 3;

    (void) cred;
    (void) bufsize;

    *amt = 0;
    *datacnt = 0;

    if (dir->nn->kind != HTTPFS_DIR)
    {
        pthread_mutex_unlock (&dir->lock);
        return ENOTDIR;
    }

    if (entry >= NUM_ENTRIES || nentries == 0)
    {
        pthread_mutex_unlock (&dir->lock);
        return 0;
    }

    int nb = NUM_ENTRIES - entry;
    if (nentries != -1 && nentries < nb)
        nb = nentries;

    /* Buffer in VM memory (see the DIRENTS_SIZE comment above).
       MAP_PRIVATE | MAP_ANONYMOUS: a private anonymous mapping,
       valid both under strict POSIX and on the Hurd. */
    char *start = mmap (nullptr, DIRENTS_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (start == MAP_FAILED)
    {
        pthread_mutex_unlock (&dir->lock);
        return ENOMEM;
    }

    char *p = start;
    for (int i = entry; i < entry + nb; i++)
    {
        size_t name_len = strlen (names[i]);
        size_t reclen = DIRENT_LEN (name_len);

        struct dirent hdr;
        memset (&hdr, 0, sizeof hdr);
        hdr.d_namlen = name_len;
        hdr.d_fileno = (ino_t) (fnv1a (dir->nn->url)
                                ^ ((uint64_t) (i + 1) << 56));
        hdr.d_reclen = reclen;
        hdr.d_type = DT_REG;

        memcpy (p, &hdr, DIRENT_NAME_OFFS);
        strcpy (p + DIRENT_NAME_OFFS, names[i]);
        p += reclen;
    }

    /* Return the unused part of the region to the VM, trimmed at
       the page boundary: the same discipline as ftpfs, required
       by the data_dealloc = 1 semantics of the RPC. */
    long page = sysconf (_SC_PAGESIZE);
    uintptr_t end = ((uintptr_t) p + (uintptr_t) page - 1)
                    & ~((uintptr_t) page - 1);
    if (end < (uintptr_t) (start + DIRENTS_SIZE))
        munmap ((void *) end,
                (size_t) ((uintptr_t) (start + DIRENTS_SIZE) - end));

    *data = start;
    *amt = nb;
    *datacnt = (mach_msg_type_number_t) (p - start);
    pthread_mutex_unlock (&dir->lock);
    return 0;
}

/* netfs_validate_stat — fill in np->nn_stat.
   Phase 2: stat'ing a view costs one HEAD (metadata only); the
   document itself is only downloaded when its size cannot be
   known otherwise. */
error_t netfs_validate_stat (struct node *np, struct iouser *cred)
{
    struct netnode *nn = np->nn;
    (void) cred;

    mode_t mode;
    long long size;

    if (nn->kind == HTTPFS_DIR)
    {
        mode = S_IFDIR | 0555;
        size = 4096;                  /* indicative value */
    }
    else
    {
        int err = ensure_metadata (nn->parent);
        if (err != 0)
            return err;

        mode = S_IFREG | 0444;

        switch (nn->kind)
        {
        case HTTPFS_CONTENT:
            size = resource_size (nn->parent);
            if (size < 0)
                size = 0;
            break;

        case HTTPFS_HEADERS:
            size = nn->parent->probe.headers != nullptr
                   ? (long long) strlen (nn->parent->probe.headers) : 0;
            break;

        case HTTPFS_STATUS:
            {
                char tmp[24];
                int n = snprintf (tmp, sizeof tmp, "%ld\n",
                                  nn->parent->probe.status);
                size = n > 0 ? n : 0;
            }
            break;

        default:
            size = 0;
            break;
        }
    }

    memset (&np->nn_stat, 0, sizeof np->nn_stat);
    np->nn_stat.st_ino = ino_of (nn);
    np->nn_stat.st_mode = mode;
    np->nn_stat.st_nlink = 1;
    np->nn_stat.st_uid = 0;
    np->nn_stat.st_gid = 0;
    np->nn_stat.st_size = (off_t) size;
    np->nn_stat.st_blksize = 4096;
    np->nn_stat.st_blocks = (size + 511) / 512;

    if (nn->kind != HTTPFS_DIR && nn->parent->probe.filetime > 0)
        np->nn_stat.st_mtim.tv_sec = nn->parent->probe.filetime;

    np->nn_translated = mode;
    return 0;
}

/* netfs_attempt_read — read up to *LEN bytes at OFFSET from NP. */
error_t netfs_attempt_read (struct iouser *cred, struct node *np,
                            loff_t offset, size_t *len, void *data)
{
    struct netnode *nn = np->nn;
    (void) cred;

    if (nn->kind == HTTPFS_DIR)
        return EISDIR;

    struct netnode *own = nn->parent;    /* the resource's owner */
    int err;

    if (nn->kind == HTTPFS_CONTENT)
    {
        err = ensure_metadata (own);
        if (err != 0)
            return err;

        pthread_mutex_lock (&own->res_lock);

        if (own->state == RES_PROBED)
            err = ensure_content (own);
        else if (own->state == RES_ERROR)
            err = own->content_err;

        if (err != 0)
        {
            pthread_mutex_unlock (&own->res_lock);
            return err;
        }

        if (own->state == RES_FULLBODY)
        {
            /* Serve from the whole-body cache (Phase 1 behavior). */
            const char *bytes = own->full.body != nullptr
                                ? own->full.body : "";
            uint64_t n_bytes = own->full.body_len;

            if (offset < 0 || (uint64_t) offset >= n_bytes)
            {
                pthread_mutex_unlock (&own->res_lock);
                *len = 0;
                return 0;
            }

            size_t n = *len;
            if (n > n_bytes - (uint64_t) offset)
                n = (size_t) (n_bytes - (uint64_t) offset);

            memcpy (data, bytes + offset, n);
            *len = n;
            pthread_mutex_unlock (&own->res_lock);
            return 0;
        }

        /* RES_BLOCKS: fetch only the windows this read touches. */
        if (offset < 0)
        {
            pthread_mutex_unlock (&own->res_lock);
            *len = 0;
            return 0;
        }

        size_t n = *len;
        err = read_blocks (own, (uint64_t) offset, &n, data);
        pthread_mutex_unlock (&own->res_lock);
        *len = n;
        return err;
    }

    /* The headers and status views come from the probe: no
       document download is ever needed. */
    err = ensure_metadata (own);
    if (err != 0)
        return err;

    const char *bytes;
    size_t n_bytes;
    char tmp[24];

    if (nn->kind == HTTPFS_HEADERS)
    {
        bytes = own->probe.headers != nullptr ? own->probe.headers : "";
        n_bytes = strlen (bytes);
    }
    else
    {
        int n = snprintf (tmp, sizeof tmp, "%ld\n", own->probe.status);
        bytes = tmp;
        n_bytes = (size_t) (n > 0 ? n : 0);
    }

    if (offset < 0 || (uint64_t) offset >= (uint64_t) n_bytes)
    {
        *len = 0;
        return 0;
    }

    size_t n = *len;
    if (n > n_bytes - (size_t) offset)
        n = n_bytes - (size_t) offset;

    memcpy (data, bytes + offset, n);
    *len = n;
    return 0;
}

/* ------------------------------------------------------------------
 * libnetfs callbacks — nodes and users
 * ------------------------------------------------------------------ */

/* netfs_node_norefs — the node NP just lost its last reference.
   Release its pinned parent, its resource, then itself. */
void netfs_node_norefs (struct node *np)
{
    struct netnode *nn = np->nn;

    if (nn != nullptr)
    {
        /* Leave the parent's child list first, while the parent is
           still pinned by our reference. */
        if (nn->parent != nullptr)
            unlink_child (nn);

        release_resource (nn);

        if (nn->parent != nullptr)
            netfs_nrele (nn->parent->node);

        pthread_mutex_destroy (&nn->res_lock);
        pthread_mutex_destroy (&nn->child_lock);
        free (nn->name);
        free (nn->url);
        free (nn);
    }

    free (np);
}

void netfs_try_dropping_softrefs (struct node *np)
{
    /* The node cache holds hard references only (the ftpfs
       pattern), so there are no soft references to drop. */
    (void) np;
}

error_t netfs_check_open_permissions (struct iouser *user, struct node *np,
                                      int flags, int newnode)
{
    /* Read-only filesystem, world-readable. */
    (void) user; (void) np; (void) flags; (void) newnode;
    return 0;
}

error_t netfs_report_access (struct iouser *cred, struct node *np, int *types)
{
    (void) cred; (void) np;
    *types = O_READ;
    return 0;
}

struct iouser *netfs_make_user (uid_t *uids, int nuids, uid_t *gids, int ngids)
{
    return iohelp_create_iouser (nullptr, nullptr, uids, nuids, gids, ngids);
}

/* ------------------------------------------------------------------
 * libnetfs callbacks — a read-only filesystem: every modification
 * attempt is refused.
 * ------------------------------------------------------------------ */

error_t netfs_attempt_write (struct iouser *cred, struct node *np,
                             loff_t offset, size_t *len, const void *data)
{ (void) cred; (void) np; (void) offset; (void) len; (void) data;
  return EROFS; }

error_t netfs_attempt_set_size (struct iouser *cred, struct node *np,
                                loff_t size)
{ (void) cred; (void) np; (void) size; return EROFS; }

error_t netfs_attempt_chown (struct iouser *cred, struct node *np,
                             uid_t uid, gid_t gid)
{ (void) cred; (void) np; (void) uid; (void) gid; return EROFS; }

error_t netfs_attempt_chauthor (struct iouser *cred, struct node *np,
                                uid_t author)
{ (void) cred; (void) np; (void) author; return EROFS; }

error_t netfs_attempt_chmod (struct iouser *cred, struct node *np, mode_t mode)
{ (void) cred; (void) np; (void) mode; return EROFS; }

error_t netfs_attempt_chflags (struct iouser *cred, struct node *np, int flags)
{ (void) cred; (void) np; (void) flags; return EROFS; }

error_t netfs_attempt_utimes (struct iouser *cred, struct node *np,
                              struct timespec *atime, struct timespec *mtime)
{ (void) cred; (void) np; (void) atime; (void) mtime; return EROFS; }

error_t netfs_attempt_unlink (struct iouser *user, struct node *dir,
                              const char *name)
{ (void) user; (void) dir; (void) name; return EROFS; }

error_t netfs_attempt_rmdir (struct iouser *user, struct node *dir,
                             const char *name)
{ (void) user; (void) dir; (void) name; return EROFS; }

error_t netfs_attempt_mkdir (struct iouser *user, struct node *dir,
                             const char *name, mode_t mode)
{ (void) user; (void) dir; (void) name; (void) mode; return EROFS; }

error_t netfs_attempt_rename (struct iouser *user, struct node *fromdir,
                              const char *fromname, struct node *todir,
                              const char *toname, int excl)
{ (void) user; (void) fromdir; (void) fromname; (void) todir;
  (void) toname; (void) excl; return EROFS; }

error_t netfs_attempt_link (struct iouser *user, struct node *dir,
                            struct node *file, const char *name, int excl)
{ (void) user; (void) dir; (void) file; (void) name; (void) excl;
  return EROFS; }

error_t netfs_attempt_mkfile (struct iouser *user, struct node *dir,
                              mode_t mode, struct node **np)
{ *np = nullptr; pthread_mutex_unlock (&dir->lock); (void) user; (void) mode;
  return EROFS; }

error_t netfs_attempt_create_file (struct iouser *user, struct node *dir,
                                   const char *name, mode_t mode,
                                   struct node **np)
{ *np = nullptr; pthread_mutex_unlock (&dir->lock);
  (void) user; (void) name; (void) mode; return EROFS; }

error_t netfs_attempt_mksymlink (struct iouser *cred, struct node *np,
                                 const char *name)
{ (void) cred; (void) np; (void) name; return EROFS; }

error_t netfs_attempt_mkdev (struct iouser *cred, struct node *np,
                             mode_t type, dev_t indexes)
{ (void) cred; (void) np; (void) type; (void) indexes; return EROFS; }

error_t netfs_attempt_readlink (struct iouser *user, struct node *np, char *buf)
{ (void) user; (void) np; (void) buf; return EINVAL; }

error_t netfs_attempt_statfs (struct iouser *cred, struct node *np,
                              fsys_statfsbuf_t *st)
{ (void) cred; (void) np; memset (st, 0, sizeof *st); return 0; }

error_t netfs_attempt_sync (struct iouser *cred, struct node *np, int wait)
{ (void) cred; (void) np; (void) wait; return 0; }

error_t netfs_attempt_syncfs (struct iouser *cred, int wait)
{ (void) cred; (void) wait; return 0; }
