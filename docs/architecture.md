# httpfs — Architecture reference

This document describes the internals of the `httpfs` translator
exhaustively: the node model, the locking and reference protocols,
the exact behavior of every libnetfs callback, memory ownership rules,
error semantics, and the build system. It is aimed at maintainers and
at anyone stacking a content translator (htmlfs, jsonfs, ...) on top
of httpfs.

```
client process
    │  POSIX calls: open, read, stat, getdents, ...
    ▼
GNU Mach IPC ─► libnetfs (threads, peropens, node locks, RPC dispatch)
                   │
                   ▼
        httpfs callbacks (src/netfs.c)
                   │  ensure_resource(): one GET per directory node
                   ▼
        transport layer (src/http.c) ─► libcurl ─► remote HTTP server
```

---

## 1. Modules

| Module          | Hurd dependency | Responsibility |
|-----------------|-----------------|----------------|
| `src/http.c/h`  | none            | Turn a URL into raw bytes: one GET, redirect following, transfer-encoding decode, timeouts, error translation, URL joining with RFC 3986 percent-encoding |
| `src/httpfs.h`  | libnetfs, libihash, libcurl | The node model: `struct netnode`, view kinds, prototypes of `httpfs_init` / `httpfs_destroy` |
| `src/netfs.c`   | libnetfs, libihash | All libnetfs callbacks; node life cycle; the three views; directory listings |
| `src/httpfs.c`  | libnetfs | `main()`: argument parsing, root node creation, `netfs_startup`, service loop |

The transport layer is deliberately free of Hurd headers, so it is
compilable and unit-testable on any POSIX system (this is what
`tests/test_url.c` exploits).

## 2. Node model

Every libnetfs `struct node` wraps a `struct netnode` defined by
httpfs (`src/httpfs.h`):

```c
enum httpfs_kind : int
{
    HTTPFS_DIR     = 0,   /* directory: a (virtual) URL path        */
    HTTPFS_CONTENT = 1,   /* "content" view: raw body               */
    HTTPFS_HEADERS = 2,   /* "headers" view: raw header block       */
    HTTPFS_STATUS  = 3,   /* "status" view: HTTP code + "\n"       */
};
```

Key fields of `struct netnode`:

- `kind` — one of the four kinds above. Everything else derives from
  it.
- `url` — defined for `HTTPFS_DIR` nodes only. It is the complete URL
  of the resource. Views have `url == NULL` and refer to
  `parent->res`.
- `parent` — the parent netnode; `NULL` for the root. A view never
  has its own resource: it is a *projection* of its parent's.
- `node` — back-pointer to the wrapping libnetfs node, needed to
  resolve `..` and to release the parent reference.
- `res_lock` + `fetched` + `res_err` + `res` — the downloaded
  resource: mutex, one-shot flag, sticky error code, and the
  `struct http_response` (body, headers, status, filetime).
- `ihash_table` — allocated on the root only, **reserved for Phase 2**
  (per-name child cache); currently unused so that every lookup
  creates a fresh node.

### Invariants

1. Only `HTTPFS_DIR` nodes own a `res`. Views read `parent->res`.
2. `fetched` never goes back to false: a node keeps its first result
   (success *or* failure) for its whole life.
3. A child's `parent` pointer is valid as long as the child exists,
   because the child holds a hard reference on the parent *node*
   (see §4).

## 3. URL mapping

`netfs_attempt_lookup(dir, name)`:

1. `name == "."` — return `dir` itself, still locked, with one extra
   hard reference. (The node returned by a lookup must be locked;
   since it is the same node, leaving it locked is the correct way to
   satisfy both sides of the contract.)
2. `name == ".."` — return the parent node (the root case is normally
   intercepted by libnetfs itself; defensively it returns the root).
3. Reserved names (`content`, `headers`, `status`) — create a view
   node whose `parent` is the current directory.
4. Any other name — create a directory node whose URL is
   `http_url_join(parent_url, name)`:
   - the base is copied verbatim, adding `/` only if it does not
     already end with one;
   - each byte of the name is copied verbatim if unreserved
     (`A-Za-z0-9-._~`, RFC 3986), percent-encoded as `%XX`
     otherwise.

Lookup is purely constructive: it never contacts the network. A URL
only materializes when one of its views is `stat`ed or read.

## 4. Reference protocol (children pin parents)

When a child is created, `netfs_attempt_lookup` takes **one hard
reference on the parent node** (`netfs_nref(dir)`, which is legal
because `dir` is locked by the caller). The reference is returned in
`netfs_node_norefs`:

```c
if (nn->parent != nullptr)
    netfs_nrele (nn->parent->node);
```

This guarantees that a view's `parent` pointer can never dangle, even
if the client drops its reference on the parent directory while
keeping a file descriptor open on a view.

Per the libnetfs contract, a node returned by a successful lookup is
**locked** and holds one **hard reference**; the lookup itself always
unlocks `dir` (except when `*np == dir`, i.e. the `.` case).

## 5. Locking, reference and cache protocols

Four mutexes coexist (Phase 2):

| Mutex             | Owner   | Scope                                          |
|-------------------|---------|------------------------------------------------|
| `np->lock`        | libnetfs| serialized access to the node; callbacks see it locked |
| `nn->res_lock`    | httpfs  | the whole resource state machine (§5.2)       |
| `nn->child_lock`  | httpfs  | the parent's child list                       |
| `node_cache_lock` | httpfs  | the global MRU node cache                      |

Locking order: `np->lock` → `child_lock` / `res_lock` /
`node_cache_lock`. Nothing ever takes an `np->lock` while holding a
`child_lock`; and the node cache is only entered while the *newly
created* node is locked (never the LRU victim), so the eviction
cascade can never deadlock on a lock we hold. The traversal
reference that libnetfs keeps on the directory being looked through
additionally guarantees that the eviction cascade can never free
that directory.

### 5.1 The node cache (Phase 2)

Every child created by a lookup is entered in its parent's child
list *and* in a global MRU cache of `HTTPFS_NODE_CACHE_MAX` (64)
entries, each holding one hard reference — the ftpfs pattern. The
sequence:

- `create_child` (called with `dir` locked): builds the node, pins
  the parent (`netfs_nref(dir)`), inserts into the child list
  (under `child_lock`), then `cache_node(np)` adds the cache's own
  reference and evicts the oldest entries with `netfs_nrele`.
- `netfs_attempt_lookup` first searches the parent's live children;
  a hit is returned directly (`netfs_nref` under the membership
  lock, then the node is locked after releasing the parent's lock —
  the same discipline as ftpfs).
- `netfs_node_norefs` unlinks the dying node from its parent's list
  (under `child_lock`, while the node's `np->lock` is held), then
  releases the parent pin; the cascade can walk up the chain
  without ever locking the directory currently being traversed.

### 5.2 The resource engine (Phase 2)

A directory's resource is a small state machine
(`enum httpfs_res_state`, see httpfs.h):

```
UNKNOWN --ensure_metadata--> PROBED --ensure_content--> FULLBODY | BLOCKS
                                                     \--> ERROR (sticky)
```

- `ensure_metadata` — one HEAD, once per node: status, whole size
  (from Content-Range/Content-Length), Last-Modified, raw headers.
  The probe response feeds the `status` and `headers` views forever.
- `ensure_content` — one ranged GET of the first block on the first
  read: 206 → BLOCKS mode (per-block `Range` requests from now on,
  cached in a 64-entry-free FIFO of 64 KiB blocks, bounded at 4 MiB
  per node); 200 → FULLBODY mode (the server ignores Range: the
  whole body received is kept, Phase 1 behavior); transport errors
  are sticky in `content_err`.
- `read_blocks` — serves a read window by touching only the blocks
  it overlaps; a 416 answer installs an empty block (end of
  resource), a short block marks the true size when the server
  never announced it.
- `resource_size` — the size for `stat`: HEAD-only when the server
  announces it; the content engine runs only when nothing else can
  reveal the size (e.g. an error page).

Note: `netfs_validate_stat` and `netfs_attempt_read` run with the
libnetfs node lock held, so the first access to a resource blocks
the RPC thread for the duration of its HTTP request(s) (each bounded
by the 60 s transfer timeout). This is the documented behavior.

## 6. Resource fetching (`src/http.c`)

Three request flavors share one engine (`http_get_impl`):
`http_head` (HEAD, metadata), `http_fetch` (plain GET) and
`http_fetch_range` (GET + `Range: bytes=start-end`). Every response
carries `size_total` — the best known whole-resource size from
Content-Range, Content-Length or the received body — and the raw
header block. `http_fetch` performs one GET with:

| Option                    | Value | Rationale |
|---------------------------|-------|-----------|
| `CURLOPT_WRITEFUNCTION/DATA` | growable buffer | the body may be binary of unknown length |
| `CURLOPT_HEADERFUNCTION/DATA` | same buffer type | the raw header block (status lines included) is captured |
| `CURLOPT_USERAGENT`       | `GNU-AI-httpfs/0.2 (Hurd translator)` | honest identification |
| `CURLOPT_NOSIGNAL`        | 1 | libnetfs is multithreaded; signals are unsafe |
| `CURLOPT_FOLLOWLOCATION`  | 1 (max 10) | the translator behaves like a browser |
| `CURLOPT_CONNECTTIMEOUT`  | 15 s | |
| `CURLOPT_TIMEOUT`         | 60 s | |
| `CURLOPT_FILETIME`        | 1 | ask for `Last-Modified`, exposed as `st_mtime` |
| `CURLOPT_ACCEPT_ENCODING` | gzip, deflate | libcurl decodes; `content` stays the raw payload |

Error translation (network failures only — HTTP statuses are *not*
errors):

| libcurl result | errno |
|----------------|-------|
| `CURLE_OUT_OF_MEMORY` | `ENOMEM` |
| `CURLE_OPERATION_TIMEDOUT` | `ETIMEDOUT` |
| `CURLE_COULDNT_RESOLVE_HOST`, `CURLE_COULDNT_CONNECT` | `EHOSTUNREACH` |
| anything else | `EIO` |

Ownership: on success the body and header buffers move into
`*out`; `http_response_release()` frees them and resets the
structure (idempotent).

## 7. POSIX call mapping (every callback)

| POSIX call | Callback | httpfs behavior |
|------------|----------|-----------------|
| `open`/permissions | `netfs_check_open_permissions` | always 0 (read-only, world-readable) |
| path resolution | `netfs_attempt_lookup` | §3; never touches the network |
| `stat/fstat` (directory) | `netfs_validate_stat` | mode `0555`, size 4096 (indicative), no fetch |
| `stat` (view) | `netfs_validate_stat` | **one HEAD probe** (no document download when the size is announced); mode `0444`; `st_size` = view size; `st_mtime` from `Last-Modified`; `st_ino` = FNV-1a(URL) xor kind |
| `read`/`pread` (view) | `netfs_attempt_read` | BLOCKS mode: fetches only the 64 KiB windows touched; FULLBODY mode: slices the cached body; offset beyond the end → `*len = 0` (EOF) |
| `read` (directory) | `netfs_attempt_read` | `EISDIR` |
| `getdents` | `netfs_get_dirents` | the three views, at every level; entries built like ftpfs (`d_namlen`, `d_reclen`, `d_type = DT_REG`) |
| `write`, `truncate`, `chmod`, `chown`, `mkdir`, `unlink`, … | corresponding `netfs_attempt_*` | `EROFS` (read-only filesystem) |
| `readlink` | `netfs_attempt_readlink` | `EINVAL` (no symlinks exist) |
| `statfs` | `netfs_attempt_statfs` | zeroed structure, success |
| `fsync` | `netfs_attempt_sync`, `netfs_attempt_syncfs` | success (nothing to flush) |
| user creation | `netfs_make_user` | `iohelp_create_iouser` |
| node destruction | `netfs_node_norefs` | releases the pinned parent, the response, the mutexes, the strings, then `free(nn)`, `free(np)` |

### Why dirents live in VM memory

`libnetfs`'s `netfs_S_dir_readdir` sets `data_dealloc = 1`: the RPC
layer deallocates the returned buffer with `vm_deallocate`. The
buffer must therefore come from `mmap`
(`MAP_PRIVATE | MAP_ANONYMOUS`), never from `malloc` — the same
discipline as ftpfs, with the unused tail of the region returned to
the VM at page granularity.

## 8. Root node life cycle

`main()` (src/httpfs.c):

1. take the first non-option argument as the base URL, refuse to run
   without one;
2. `netfs_init()`;
3. allocate and initialize the root netnode (`httpfs_init`: libcurl
   global init, strings, mutexes, reserved ihash);
4. wrap it with `netfs_make_node` — libnetfs takes ownership of the
   netnode pointer;
5. minimal root stat (recomputed later by `netfs_validate_stat`);
6. `netfs_startup(bootstrap, 0)` — the bootstrap port must be valid,
   otherwise the program is not running as a translator;
7. `netfs_server_loop()` — never returns.

`netfs_root_node`, `netfs_server_name`, `netfs_server_version` and
`netfs_maxsymlinks` are defined by the translator; everything else
(protds, peropens, the root node variable itself) belongs to
libnetfs.

## 9. Build system

- `AC_USE_SYSTEM_EXTENSIONS` enables POSIX/XSI interfaces
  (`loff_t`, ...) through `config.h` — included first by every
  source file.
- A `configure` cache check selects the first working of
  `-std=c23` / `-std=c2x` and exports it as `C23_CFLAGS`; if neither
  compiles a small C23 probe (`constexpr`, `nullptr`), configure
  aborts. The code uses: `constexpr`, `nullptr`, `bool`, fixed-type
  enums (`enum httpfs_kind : int`).
- `PKG_CHECK_MODULES(CURL)` — mandatory, transport.
- `PKG_CHECK_MODULES(MHD)` — optional; when absent, the libmicrohttpd
  tests are skipped (`HAVE_MHD` conditional) and the translator
  still builds.
- `-Wall -Wextra` is on for every compilation.

## 10. Testing strategy

Determinism first (see `CONTEXT.md`):

- `test_url.c` — pure logic: base/child concatenation, RFC 3986
  percent-encoding (spaces, `?`, `#`, `%`, UTF-8 bytes, `/` in a
  name). No network, no server, always built.
- `test_helpers.c` — an embedded HTTP server (libmicrohttpd) on
  `127.0.0.1` serving a deterministic 64 KiB body generated with a
  small LCG; supports single-range requests (206 with `Content-Range`,
  416 out of bounds) and a fetch helper built on libcurl.
- `test_full_stream.c` — the whole body, byte for byte, status 200.
- `test_range.c` — bounded range, open-ended range, out-of-bounds
  range (206/206/416), each slice compared to the expected window
  of the pattern.

Integration outside the Hurd is exercised by driving the netfs
callbacks directly against a real HTTP server (the verification
harness used during development).  On the Hurd itself, the full
build and the 4-test suite pass on Debian GNU/Hurd (hurd-amd64) with
the real libnetfs/libihash/libiohelp, and end-to-end behavior is
verified through `settrans` + ordinary tools (stat, dd, tail, sha256)
against both range-capable and Range-ignoring servers.

## 11. Known limitations (v0.3)

- Servers that ignore Range keep the whole body in memory
  (FULLBODY mode): a large resource still costs RAM proportional to
  its size.
- The node cache is per-translator and holds at most 64 nodes; a
  very wide traversal evicts older paths, which will re-probe on
  their next lookup.
- No cache invalidation: a node keeps what it saw for its (short)
  life; there is no TTL, no If-Modified-Since revalidation.
- The `headers` view shows the HEAD probe's headers, not the
  headers of the ranged GETs that later fetched the blocks.
- Reserved names shadow remote path components named `content`,
  `headers` or `status`.
- Query strings and fragments cannot be expressed in the path
  (path components only).
- First access to a resource blocks its RPC thread for the HTTP
  request (no async I/O yet).

## 12. The parser chain (Phase 3, v0.4)

Phase 3 adds four read-only translators that interpret a document
instead of transporting it: `htmlfs`, `jsonfs`, `csvfs` and `tsvfs`.
They stack on `httpfs` through ordinary paths:

```
settrans -a /web httpfs https://example.org/api/x
settrans -a /api jsonfs /web/content
cat /api/version
```

Mounting `/api` opens `/web/content` with plain POSIX semantics,
which triggers the httpfs download — translators compose by
paths, with no new protocol between them.

### 12.1 The portable document tree (doc.c)

All four parsers build the same structure: a `struct doc_node`
tree of directories and files (first child / next sibling
representation).  The tree is deliberately Hurd-free — plain
C23 + POSIX — so the parsers compile and are unit-tested on any
POSIX system (`tests/test_json.c`, `test_html.c`, `test_csv.c`),
exactly like the transport layer (`http.c`).

Names are guaranteed safe and unique by the tree itself
(`doc_safe_name`, `doc_unique_name`): a JSON key `a/b` becomes
`a_b`, a duplicate becomes `a_2`.  A parser can therefore never
emit a name that breaks a directory walk.

### 12.2 One server for four parsers (parserfs.c)

Only `parserfs.c` knows about libnetfs.  It is the same callback
set as the Phase 2 server (§4-§7), minus everything a static tree
does not need:

- `netfs_attempt_lookup` walks the doc tree and lazily wraps a
  doc node in a libnetfs "shell" (`doc_node.priv`);
- each shell holds one **permanent** reference, so a client
  dropping its last reference can never race a concurrent
  lookup — the shell lives exactly as long as the static tree;
- `netfs_get_dirents` paginates the child list with the same
  mmap discipline as §6;
- `netfs_attempt_read` is a plain `memcpy` from the node bytes.

A parser translator therefore reduces to: read the source file
(`pfs_read_file`, bounded at 256 MiB), build the tree, call
`pfs_serve` (tree, name, version).

### 12.3 What each parser exposes

| Translator  | Tree |
|-------------|------|
| `htmlfs`    | `/title`, `/text` (normalized visible text), `/meta/<name>`, `/headings/<n>` (`level<TAB>text`), `/links/<n>/{url,text}` |
| `jsonfs`    | objects → directories, arrays → `0, 1, ...`, scalars → files (strings decoded, numbers verbatim); a container root is re-homed at the top level |
| `csvfs`     | `/count`, `/header/<i>`, `/rows/<n>/<column>` (row view), `/columns/<column>` (column view) |
| `tsvfs`     | same program as `csvfs`, default delimiter `\t` (chosen by `argv[0]`); `-d` overrides; `csvfs` sniffs the delimiter from the header line |

All three parsers are bounded (`JSON_MAX_NODES`,
`HTML_MAX_ITEMS`, `CSV_MAX_CELLS`): a hostile document fails
loudly at parse time instead of exhausting the host.

### 12.4 Tolerance policy

`htmlfs` and `csvfs` never fail: a truncated or hostile document
yields a partial tree, the way a browser still renders broken
pages.  `jsonfs` is the exception — JSON has a grammar, so a
malformed document refuses to mount with a byte-precise error
message, rather than serving an LLM silently-wrong data.
