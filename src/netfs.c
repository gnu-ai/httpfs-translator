/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * netfs.c — The heart of the httpfs translator: libnetfs callbacks.
 *
 * NAVIGATION MODEL
 * ----------------
 *   POSIX directory            <->  (virtual, unbounded) URL path
 *   /.../content               <->  raw body of the response
 *   /.../headers               <->  raw HTTP headers
 *   /.../status                <->  HTTP code ("200\n")
 *
 * Every path component creates a child directory whose URL is
 * "parent URL / name".  The resource of a directory is downloaded
 * once (a single GET) and then served from memory: subsequent
 * reads (pread, mmap, ...) are purely local.
 *
 * LOCKS
 * -----
 *   np->lock        owned by libnetfs (callbacks see a locked node)
 *   nn->res_lock    protects the download and the node cache
 *
 * REFERENCES
 * ----------
 * A child holds one hard reference on its parent's node, taken in
 * netfs_attempt_lookup and returned in netfs_node_norefs: the
 * parent can never disappear before its last child.
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
 * Managing a directory's resource
 * ------------------------------------------------------------------ */

/* ensure_resource — download (exactly once) the resource of the
   directory NN.  Returns 0 or an errno code.  The outcome — even a
   network error — is remembered for the whole life of the node. */
static int ensure_resource (struct netnode *nn)
{
    int err;

    pthread_mutex_lock (&nn->res_lock);

    if (! nn->fetched)
    {
        err = http_fetch (nn->url, &nn->res);
        nn->res_err = err;
        nn->fetched = true;
    }
    else
        err = nn->res_err;

    pthread_mutex_unlock (&nn->res_lock);
    return err;
}

/* view_size — size in bytes of the virtual file represented by NN
   (the resource must already be downloaded). */
static size_t view_size (const struct netnode *nn)
{
    switch (nn->kind)
    {
    case HTTPFS_CONTENT:
        return nn->parent->res.body_len;
    case HTTPFS_HEADERS:
        return nn->parent->res.headers != nullptr
               ? strlen (nn->parent->res.headers) : 0;
    case HTTPFS_STATUS:
        {
            char tmp[24];
            int n = snprintf (tmp, sizeof tmp, "%ld\n",
                              nn->parent->res.status);
            return n > 0 ? (size_t) n : 0;
        }
    default:
        return 0;               /* a directory is not a view */
    }
}

/* ------------------------------------------------------------------
 * Node creation
 * ------------------------------------------------------------------ */

/* create_child — create inside DIR (locked) a node NAME of kind
   KIND.  On output: *NP designates the new node, LOCKED and holding
   one reference.  Returns 0 or an errno code. */
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
    nn->fetched = false;
    nn->res_err = 0;
    nn->res = (struct http_response) { nullptr, 0, nullptr, 0, -1 };
    nn->ihash_table = nullptr;      /* child cache: Phase 2 */
    pthread_mutex_init (&nn->res_lock, nullptr);
    pthread_mutex_init (&nn->ihash_lock, nullptr);

    /* A URL is only defined for directories; the views refer to
       their parent's resource. */
    nn->url = (kind == HTTPFS_DIR) ? http_url_join (dn->url, name) : nullptr;

    if (nn->name == nullptr || (kind == HTTPFS_DIR && nn->url == nullptr))
    {
        free (nn->name);
        free (nn->url);
        pthread_mutex_destroy (&nn->res_lock);
        pthread_mutex_destroy (&nn->ihash_lock);
        free (nn);
        return ENOMEM;
    }

    /* The child keeps its parent alive. */
    netfs_nref (dir);

    *np = netfs_make_node (nn);
    if (*np == nullptr)
    {
        netfs_nrele (dir);
        free (nn->name);
        free (nn->url);
        pthread_mutex_destroy (&nn->res_lock);
        pthread_mutex_destroy (&nn->ihash_lock);
        free (nn);
        return ENOMEM;
    }
    nn->node = *np;

    /* libnetfs contract: the found node must be locked. */
    pthread_mutex_lock (&(*np)->lock);
    return 0;
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
    root->fetched = false;
    root->res_err = 0;
    root->res = (struct http_response) { nullptr, 0, nullptr, 0, -1 };
    root->ihash_table = nullptr;

    if (root->name == nullptr || root->url == nullptr)
    {
        free (root->name);
        free (root->url);
        curl_global_cleanup ();
        return ENOMEM;
    }

    if (pthread_mutex_init (&root->res_lock, nullptr) != 0
        || pthread_mutex_init (&root->ihash_lock, nullptr) != 0)
    {
        pthread_mutex_destroy (&root->res_lock);
        curl_global_cleanup ();
        free (root->name);
        free (root->url);
        return ENOMEM;
    }

    /* Per-child cache, reserved for Phase 2 (no location pointers
       stored inside the values: HURD_IHASH_NO_LOCP). */
    error_t err = hurd_ihash_create (&root->ihash_table, HURD_IHASH_NO_LOCP);
    if (err != 0)
    {
        pthread_mutex_destroy (&root->res_lock);
        pthread_mutex_destroy (&root->ihash_lock);
        curl_global_cleanup ();
        free (root->name);
        free (root->url);
        return err;
    }

    return 0;
}

error_t httpfs_destroy (struct netnode *root)
{
    http_response_release (&root->res);
    free (root->name);
    free (root->url);
    root->name = nullptr;
    root->url = nullptr;

    if (root->ihash_table != nullptr)
    {
        hurd_ihash_destroy (root->ihash_table);
        hurd_ihash_free (root->ihash_table);
        root->ihash_table = nullptr;
    }

    pthread_mutex_destroy (&root->ihash_lock);
    pthread_mutex_destroy (&root->res_lock);
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

    /* ".." — the parent directory (the root case is handled by
       libnetfs itself; defensively, a ".." at the root returns the
       root). */
    if (strcmp (name, "..") == 0)
    {
        struct node *parent = dn->parent != nullptr ? dn->parent->node : dir;

        pthread_mutex_lock (&parent->lock);
        netfs_nref (parent);
        *np = parent;
        pthread_mutex_unlock (&dir->lock);
        return 0;
    }

    /* Reserved names: the views on this directory's resource. */
    if (reserved_name (name, &kind))
        err = create_child (dir, name, kind, np);
    else
        /* Any other name: one more URL path component. */
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
   Stat'ing a view triggers the download of the directory's
   resource (once per node). */
error_t netfs_validate_stat (struct node *np, struct iouser *cred)
{
    struct netnode *nn = np->nn;
    (void) cred;

    mode_t mode;
    off_t size;

    if (nn->kind == HTTPFS_DIR)
    {
        mode = S_IFDIR | 0555;
        size = 4096;                  /* indicative value */
    }
    else
    {
        int err = ensure_resource (nn->parent);
        if (err != 0)
            return err;

        mode = S_IFREG | 0444;
        size = (off_t) view_size (nn);
    }

    memset (&np->nn_stat, 0, sizeof np->nn_stat);
    np->nn_stat.st_ino = ino_of (nn);
    np->nn_stat.st_mode = mode;
    np->nn_stat.st_nlink = 1;
    np->nn_stat.st_uid = 0;
    np->nn_stat.st_gid = 0;
    np->nn_stat.st_size = size;
    np->nn_stat.st_blksize = 4096;
    np->nn_stat.st_blocks = (size + 511) / 512;

    if (nn->kind != HTTPFS_DIR && nn->parent->res.filetime > 0)
        np->nn_stat.st_mtim.tv_sec = nn->parent->res.filetime;

    np->nn_translated = mode;
    return 0;
}

/* netfs_attempt_read — read up to *LEN bytes at OFFSET from NP.
   The bytes come from the cached resource of the directory. */
error_t netfs_attempt_read (struct iouser *cred, struct node *np,
                            loff_t offset, size_t *len, void *data)
{
    struct netnode *nn = np->nn;
    (void) cred;

    if (nn->kind == HTTPFS_DIR)
        return EISDIR;

    int err = ensure_resource (nn->parent);
    if (err != 0)
        return err;

    /* Select the view. */
    const char *bytes;
    size_t n_bytes;
    char tmp[24];

    switch (nn->kind)
    {
    case HTTPFS_CONTENT:
        bytes = nn->parent->res.body;
        n_bytes = nn->parent->res.body_len;
        break;
    case HTTPFS_HEADERS:
        bytes = nn->parent->res.headers;
        n_bytes = view_size (nn);
        break;
    case HTTPFS_STATUS:
        {
            int n = snprintf (tmp, sizeof tmp, "%ld\n",
                              nn->parent->res.status);
            bytes = tmp;
            n_bytes = (size_t) (n > 0 ? n : 0);
        }
        break;
    default:
        return EISDIR;
    }

    if (bytes == nullptr)
        bytes = "";

    /* Out of bounds: report end of file (EOF). */
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
        if (nn->parent != nullptr)
            netfs_nrele (nn->parent->node);

        http_response_release (&nn->res);
        if (nn->ihash_table != nullptr)
        {
            hurd_ihash_destroy (nn->ihash_table);
            hurd_ihash_free (nn->ihash_table);
        }
        pthread_mutex_destroy (&nn->ihash_lock);
        pthread_mutex_destroy (&nn->res_lock);
        free (nn->name);
        free (nn->url);
        free (nn);
    }

    free (np);
}

void netfs_try_dropping_softrefs (struct node *np)
{
    /* No node is kept in a global cache: nothing to do. */
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
