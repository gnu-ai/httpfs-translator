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

## 5. Locking protocol

Two mutexes coexist:

| Mutex            | Owner        | Scope                                      |
|------------------|--------------|--------------------------------------------|
| `np->lock`       | libnetfs     | serialized access to the node; callbacks see the node locked |
| `nn->res_lock`   | httpfs       | serializes the download and protects `res`, `res_err`, `fetched` |

`ensure_resource(nn)`:

```c
lock (nn->res_lock);
if (!nn->fetched) { nn->res_err = http_fetch (nn->url, &nn->res);
                    nn->fetched = true; }
err = nn->res_err;
unlock (nn->res_lock);
return err;
```

Concurrent readers of the same directory trigger exactly one GET; the
others wait on `res_lock` and then observe `fetched == true`.

Note: `netfs_validate_stat` and `netfs_attempt_read` run with the
libnetfs node lock held, so a first access blocks the RPC thread for
the duration of the HTTP request (bounded by the 60 s transfer
timeout). This is the documented v1 behavior.

## 6. Resource fetching (`src/http.c`)

`http_fetch(url, out)` performs one GET with:

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
| `stat` (view) | `netfs_validate_stat` | **triggers the parent's GET**; mode `0444`; `st_size` = view size; `st_mtime` from `Last-Modified`; `st_ino` = FNV-1a(URL) xor kind |
| `read`/`pread` (view) | `netfs_attempt_read` | slices the cached resource; offset beyond the end → `*len = 0` (EOF) |
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
harness used during development); on the Hurd, end-to-end behavior is
additionally covered by `settrans` + ordinary tools.

## 11. Known limitations (v0.2)

- One GET per node, full body cached in memory: a large resource
  costs RAM proportional to its size, and re-opening a path
  re-downloads it (no shared cache yet — the `ihash` field is the
  Phase 2 hook).
- No HTTP `Range` streaming: `pread` is served from the cached copy
  (Phase 2 plans direct range requests).
- `stat`ing a view triggers the download; a `ls -l` on a directory
  downloads that directory's resource (not the children's).
- Reserved names shadow remote path components named `content`,
  `headers` or `status`.
- No cache invalidation: a node keeps the first response it saw for
  its whole (short) life.
- Query strings and fragments cannot be expressed in the path
  (path components only).
