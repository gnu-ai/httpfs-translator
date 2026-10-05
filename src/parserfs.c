/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * parserfs.c — Implementation of the shared libnetfs server.
 *
 * MAPPING A STATIC TREE ONTO libnetfs
 * -----------------------------------
 * libnetfs hands us callbacks with its own node type (struct
 * node).  The document tree is the durable object, so the mapping
 * is deliberately simple:
 *
 *      doc_node --(first lookup)--> struct node   (in doc->priv)
 *                 --(last reference gone)--> freed
 *
 * Each libnetfs node is a tiny shell (struct netnode) holding
 * only a pointer to its doc_node; all the data — names, bytes,
 * tree links — lives in the doc tree.  Nothing is cached beyond
 * that: a re-lookup after the shell died simply builds a new one.
 *
 * The locking discipline is the one of httpfs's netfs.c (see
 * docs/architecture.md §4): the contract of netfs_attempt_lookup
 * gives us DIR locked and must return *NP locked; every other
 * callback unlocks its node before returning.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include "mach-shim.h"
#include <hurd.h>
#include <hurd/iohelp.h>
#include <hurd/netfs.h>

#include "parserfs.h"

/* ------------------------------------------------------------------ */
/* libnetfs globals (the identity of the server is per-translator)
 * ------------------------------------------------------------------ */

static const char *server_name = "parserfs";
static const char *server_version = "0.4.0";

char *netfs_server_name;       /* set by pfs_serve   */
char *netfs_server_version;    /* set by pfs_serve   */
int netfs_maxsymlinks = 8;

/* The one tree this server exposes. */
static struct doc_node *the_tree;

/* ------------------------------------------------------------------ */
/* The node shell
 * ------------------------------------------------------------------ */

struct netnode
{
    struct doc_node *doc;      /* the real content            */
};

/* is_dir — a doc node without data is a directory by construction. */
static bool is_dir (const struct doc_node *d)
{
    return d->data == NULL;
}

/* make_shell — build the libnetfs node for a doc node, once.

   The shell carries one PERMANENT reference of its own, so a
   client dropping its last reference never races a concurrent
   lookup: the shell lives as long as the (static) tree, which it
   can never outgrow.  netfs_node_norefs therefore only runs when
   the whole translator goes away. */
static struct node *make_shell (struct doc_node *d)
{
    struct netnode *nn = malloc (sizeof *nn);
    if (nn == NULL)
        return NULL;
    nn->doc = d;

    struct node *np = netfs_make_node (nn);
    if (np == NULL)
        {
            free (nn);
            return NULL;
        }

    netfs_nref (np);                 /* our permanent reference */
    d->priv = np;
    return np;
}

/* ------------------------------------------------------------------ */
/* Directory entries (the mmap discipline of httpfs's netfs.c)
 * ------------------------------------------------------------------ */

/* One served entry is at most a page-sized dirent header plus a
   name; the buffer below is a single VM page rounded up. */
#define DIRENT_NAME_OFFS ((offsetof (struct dirent, d_name) + 3) & ~3)
#define DIRENT_LEN(name_len) \
    ((DIRENT_NAME_OFFS + (name_len) + 1 + 7) & ~7)
#define DIRENTS_SIZE 65536

/* fnv1a — the non-cryptographic hash used for inode numbers. */
static uint64_t fnv1a (const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *) s; *p; p++)
        {
            h ^= *p;
            h *= 1099511628211ULL;
        }
    return h;
}

/* ino_of — a stable inode for a doc node: its name path hashed. */
static ino_t ino_of (const struct doc_node *d)
{
    /* Hash the accumulated path on the stack of the tree: node
       names are short, so this stays cheap and path-unique. */
    char path[512];
    size_t len = 0;
    const struct doc_node *chain[64];
    int depth = 0;

    for (const struct doc_node *n = d; n != NULL && depth < 64;
         n = n->parent)
        chain[depth++] = n;

    for (int i = depth - 1; i >= 0; i--)
        {
            size_t nl = strlen (chain[i]->name);
            if (len + nl + 1 >= sizeof path)
                break;
            if (len > 0)
                path[len++] = '/';
            memcpy (path + len, chain[i]->name, nl);
            len += nl;
        }
    path[len] = '\0';

    return (ino_t) (fnv1a (path) ^ (uint64_t) (uintptr_t) the_tree);
}

/* ------------------------------------------------------------------ */
/* libnetfs callbacks — lookups and listings
 * ------------------------------------------------------------------ */

/* netfs_attempt_lookup — resolve NAME inside DIR.
   Contract: on success *NP is locked and referenced; DIR is
   unlocked no matter what (unless *NP == DIR). */
error_t netfs_attempt_lookup (struct iouser *user, struct node *dir,
                              const char *name, struct node **np)
{
    struct netnode *dn = dir->nn;

    (void) user;               /* a read-only, world-readable tree */
    *np = nullptr;

    if (!is_dir (dn->doc))
        {
            pthread_mutex_unlock (&dir->lock);
            return ENOTDIR;
        }

    if (strcmp (name, ".") == 0)
        {
            netfs_nref (dir);
            *np = dir;
            return 0;
        }

    if (strcmp (name, "..") == 0)
        {
            struct doc_node *pd = dn->doc->parent != NULL
                                  ? dn->doc->parent : dn->doc;
            struct node *pnp = pd->priv != NULL ? pd->priv : make_shell (pd);

            if (pnp == NULL)
                {
                    pthread_mutex_unlock (&dir->lock);
                    return ENOMEM;
                }
            pthread_mutex_lock (&pnp->lock);
            netfs_nref (pnp);
            *np = pnp;
            pthread_mutex_unlock (&dir->lock);
            return 0;
        }

    struct doc_node *child = doc_find (dn->doc, name);
    if (child == NULL)
        {
            pthread_mutex_unlock (&dir->lock);
            return ENOENT;
        }

    /* Existing shell, or a fresh one. */
    struct node *cnp = child->priv != NULL ? child->priv
                        : make_shell (child);
    if (cnp == NULL)
        {
            pthread_mutex_unlock (&dir->lock);
            return ENOMEM;
        }

    netfs_nref (cnp);
    pthread_mutex_unlock (&dir->lock);
    pthread_mutex_lock (&cnp->lock);
    *np = cnp;
    return 0;
}

/* netfs_get_dirents — list the children of a directory.
   ENTRY/NENTRIES drive the pagination, exactly as in the
   httpfs server: see the DIRENTS_SIZE comment there. */
error_t netfs_get_dirents (struct iouser *cred, struct node *dir, int entry,
                           int nentries, char **data,
                           mach_msg_type_number_t *datacnt, vm_size_t bufsize,
                           int *amt)
{
    struct netnode *dn = dir->nn;

    (void) cred;
    (void) bufsize;

    *amt = 0;
    *datacnt = 0;

    if (!is_dir (dn->doc))
        {
            pthread_mutex_unlock (&dir->lock);
            return ENOTDIR;
        }

    /* Count the children first: pagination needs the total. */
    size_t total = doc_child_count (dn->doc);

    if ((size_t) entry >= total || nentries == 0)
        {
            pthread_mutex_unlock (&dir->lock);
            return 0;
        }

    size_t nb = total - (size_t) entry;
    if (nentries != -1 && (size_t) nentries < nb)
        nb = (size_t) nentries;

    char *start = mmap (nullptr, DIRENTS_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (start == MAP_FAILED)
        {
            pthread_mutex_unlock (&dir->lock);
            return ENOMEM;
        }

    /* Walk to the ENTRY-th child, then copy NB names. */
    char *p = start;
    size_t seen = 0, emitted = 0;
    struct doc_node *c;

    for (c = dn->doc->first_child;
         c != NULL && emitted < nb; c = c->next_sibling, seen++)
        {
            if ((size_t) entry > 0 && seen < (size_t) entry)
                continue;

            size_t name_len = strlen (c->name);
            size_t reclen = DIRENT_LEN (name_len);

            struct dirent hdr;
            memset (&hdr, 0, sizeof hdr);
            hdr.d_namlen = name_len;
            hdr.d_fileno = ino_of (c);
            hdr.d_reclen = reclen;
            hdr.d_type = is_dir (c) ? DT_DIR : DT_REG;

            memcpy (p, &hdr, DIRENT_NAME_OFFS);
            strcpy (p + DIRENT_NAME_OFFS, c->name);
            p += reclen;
            emitted++;
        }

    /* Trim the unused tail of the mapping at a page boundary: the
       RPC will vm_deallocate the whole region (data_dealloc = 1). */
    long page = sysconf (_SC_PAGESIZE);
    uintptr_t end = ((uintptr_t) p + (uintptr_t) page - 1)
                    & ~((uintptr_t) page - 1);
    if (end < (uintptr_t) (start + DIRENTS_SIZE))
        munmap ((void *) end,
                (size_t) ((uintptr_t) (start + DIRENTS_SIZE) - end));

    *data = start;
    *amt = (int) emitted;
    *datacnt = (mach_msg_type_number_t) (p - start);
    pthread_mutex_unlock (&dir->lock);
    return 0;
}

/* netfs_validate_stat — fill in np->nn_stat from the doc node. */
error_t netfs_validate_stat (struct node *np, struct iouser *cred)
{
    struct netnode *nn = np->nn;
    struct doc_node *d = nn->doc;
    (void) cred;

    mode_t mode = is_dir (d) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    off_t size = is_dir (d) ? (off_t) doc_child_count (d)
                            : (off_t) d->data_len;

    memset (&np->nn_stat, 0, sizeof np->nn_stat);
    np->nn_stat.st_ino = ino_of (d);
    np->nn_stat.st_mode = mode;
    np->nn_stat.st_nlink = 1;
    np->nn_stat.st_uid = 0;
    np->nn_stat.st_gid = 0;
    np->nn_stat.st_size = size;
    np->nn_stat.st_blksize = 4096;
    np->nn_stat.st_blocks = (size + 511) / 512;

    np->nn_translated = mode;
    return 0;
}

/* netfs_attempt_read — the whole point of the exercise: bytes. */
error_t netfs_attempt_read (struct iouser *cred, struct node *np,
                            loff_t offset, size_t *len, void *data)
{
    struct netnode *nn = np->nn;
    (void) cred;

    if (is_dir (nn->doc))
        return EISDIR;

    size_t n_bytes = nn->doc->data_len;

    if (offset < 0 || (uint64_t) offset >= (uint64_t) n_bytes)
        {
            *len = 0;
            return 0;
        }

    size_t n = *len;
    if (n > n_bytes - (size_t) offset)
        n = n_bytes - (size_t) offset;

    memcpy (data, nn->doc->data + offset, n);
    *len = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* libnetfs callbacks — nodes and users
 * ------------------------------------------------------------------ */

/* netfs_node_norefs — the shell lost its last reference.
   The doc node survives (it belongs to the tree); only the
   shell goes, and priv is cleared so a later lookup rebuilds
   it. */
void netfs_node_norefs (struct node *np)
{
    struct netnode *nn = np->nn;

    if (nn != nullptr)
        {
            if (nn->doc != nullptr)
                nn->doc->priv = nullptr;
            free (nn);
        }

    free (np);
}

void netfs_try_dropping_softrefs (struct node *np)
{
    (void) np;
}

error_t netfs_check_open_permissions (struct iouser *user, struct node *np,
                                      int flags, int newnode)
{
    (void) user; (void) np; (void) flags; (void) newnode;
    return 0;
}

error_t netfs_report_access (struct iouser *cred, struct node *np, int *types)
{
    (void) cred; (void) np;
    *types = O_READ;
    return 0;
}

/* netfs_make_user — build a credential from the RPC's id arrays.
   libiohelp's API takes an out-parameter and raw arrays
   (iohelp_create_complex_iouser), not idvecs built by the
   caller. */
struct iouser *netfs_make_user (uid_t *uids, int nuids, uid_t *gids, int ngids)
{
    struct iouser *user = nullptr;

    if (iohelp_create_complex_iouser (&user, uids, nuids, gids, ngids) != 0)
        return nullptr;
    return user;
}

/* ------------------------------------------------------------------ */
/* libnetfs callbacks — a read-only filesystem: every modification
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

/* ------------------------------------------------------------------ */
/* Reading the source file
 * ------------------------------------------------------------------ */

int pfs_read_file (const char *path, char **out, size_t *outlen)
{
    int fd = open (path, O_RDONLY);
    if (fd < 0)
        return errno;

    struct stat st;
    if (fstat (fd, &st) != 0)
        {
            int err = errno;
            close (fd);
            return err;
        }

    if (!S_ISREG (st.st_mode))
        {
            close (fd);
            return EINVAL;
        }

    if (st.st_size > (off_t) PFS_MAX_SOURCE_BYTES)
        {
            close (fd);
            return EFBIG;
        }

    char *buf = malloc (st.st_size != 0 ? (size_t) st.st_size : 1);
    if (buf == NULL)
        {
            close (fd);
            return ENOMEM;
        }

    size_t total = 0;
    while (total < (size_t) st.st_size)
        {
            ssize_t n = read (fd, buf + total, (size_t) st.st_size - total);
            if (n < 0)
                {
                    int err = errno;
                    free (buf);
                    close (fd);
                    return err;
                }
            if (n == 0)
                break;                    /* the file shrank: tolerant */
            total += (size_t) n;
        }

    close (fd);
    *out = buf;
    *outlen = total;
    return 0;
}

/* ------------------------------------------------------------------ */
/* The server itself
 * ------------------------------------------------------------------ */

void pfs_serve (struct doc_node *tree, const char *name,
                const char *version)
{
    mach_port_t bootstrap;

    the_tree = tree;
    server_name = name;
    server_version = version;

    /* The strings must outlive us: libnetfs keeps the pointers. */
    netfs_server_name = strdup (server_name);
    netfs_server_version = strdup (server_version);

    netfs_init ();

    /* The root shell exists from the very first RPC. */
    netfs_root_node = make_shell (tree);
    if (netfs_root_node == nullptr)
        error (1, ENOMEM, "cannot create the root node.");

    netfs_root_node->nn_stat.st_mode = S_IFDIR | 0555;
    netfs_root_node->nn_stat.st_nlink = 1;
    netfs_root_node->nn_translated = S_IFDIR | 0555;

    /* The bootstrap port connects the translator to the translated
       node; settrans provided it. */
    task_get_bootstrap_port (mach_task_self (), &bootstrap);
    if (bootstrap == MACH_PORT_NULL)
        error (2, 0, "must be started as a translator (settrans).");

    netfs_startup (bootstrap, 0);

    /* RPC service loop: never returns. */
    netfs_server_loop ();
}
