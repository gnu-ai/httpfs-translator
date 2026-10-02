# httpfs — an HTTP translator for GNU/Hurd, built for AI agents

`httpfs` is a complete rewrite of the GNU/Hurd **httpfs** translator: a
filesystem server that turns a remote HTTP resource into a live POSIX
filesystem. It is designed for the **GNU AI** project, so that a large
language model — or any software agent — can *browse the Web by simply
reading files*.

```console
$ settrans -a /web httpfs https://www.gnu.org
$ cat /web/content                    # body of https://www.gnu.org
$ cat /web/software/content           # body of .../software
$ cat /web/software/status            # the HTTP status code, e.g. 200
$ ls /web
content  headers  status
```

No HTML parsing, no JSON handling, no content interpretation of any
kind: `httpfs` is a **pure transport layer**. Semantic interpretation
is delegated to downstream translators stacked on top of it
(`htmlfs`, `jsonfs`, ...) following the Hurd translator philosophy
described in `CONTEXT.md`.

---

## Table of contents

1. [How the Web appears as files](#how-the-web-appears-as-files)
2. [Requirements](#requirements)
3. [Building](#building)
4. [Installing and mounting](#installing-and-mounting)
5. [Using httpfs from an agent](#using-httpfs-from-an-agent)
6. [Behavior in detail](#behavior-in-detail)
7. [Testing](#testing)
8. [Project layout](#project-layout)
9. [Architecture (in depth)](#architecture-in-depth)
10. [Status and roadmap](#status-and-roadmap)
11. [License](#license)

---

## How the Web appears as files

The translator is mounted on a **base URL**. Each POSIX directory in
the mounted tree corresponds to a URL path; each directory — at every
level — exposes exactly three virtual files describing *its own*
resource:

| Virtual file | Content                        | Example                        |
|--------------|--------------------------------|--------------------------------|
| `content`    | The raw response body          | the HTML, JSON, binary payload |
| `headers`    | The raw HTTP header block      | `HTTP/1.1 200 OK\r\nServer:…`  |
| `status`     | The HTTP status code + `\n`    | `200\n`, `404\n`, `503\n`      |

Path components extend the URL one by one:

```
POSIX path                        →  URL fetched
─────────────────────────────────────────────────────────────────
/web                              →  https://example.org
/web/content                      →  body of https://example.org
/web/docs/page.html/content       →  body of https://example.org/docs/page.html
/web/docs/page.html/status        →  status of the same request
```

Because the scheme is uniform at every level, an agent can predict it
after seeing it once. A resource is identified by `…/content`; its
fate by `…/status`; its metadata by `…/headers`.

## Requirements

- GNU/Hurd with `libnetfs` and `libihash` (the translator itself)
- **libcurl >= 7.0.0** (HTTP transport)
- A **C23 compiler** (GCC 13 or newer; the build system also accepts
  the older `-std=c2x` spelling)
- GNU autotools (`autoconf`, `automake`) to regenerate the build
  system
- *Optional*: `libmicrohttpd` — only needed to run the `make check`
  network tests; the translator builds without it. Both the 0.9.x
  and the 1.x API are supported (the callback result type is
  selected at compile time via `MHD_VERSION`)

## Building

```console
$ ./autogen.sh                # runs autoreconf --install --force
$ ./configure
$ make
```

The configure script requires a compiler able to accept C23
(`-std=c23` or `-std=c2x`); it aborts with a clear message otherwise.
System interfaces (POSIX/XSI) are enabled through
`AC_USE_SYSTEM_EXTENSIONS`, so no manual `-D_GNU_SOURCE` is needed.

To build out of tree: `mkdir build && cd build && ../configure && make`.

## Installing and mounting

```console
$ make install                                # /usr/local/bin/httpfs
$ settrans -a /web httpfs https://example.org  # mount
$ settrans -D /web                             # unmount
```

- `-a` starts the translator actively, in the foreground; use
  `settrans` without `-a` for a passive (on-demand) translator.
- The first non-option argument is the base URL. `http://` and
  `https://` are both supported, including redirects (up to 10) and
  compressed transfer encodings (gzip, deflate — decoded by libcurl).
- The mount point is **read-only**: every modification call
  (`write`, `mkdir`, `unlink`, `chmod`, …) returns `EROFS`.
- The translator must be started *as a translator*: outside `settrans`
  it exits with `must be started as a translator (settrans)`.

## Using httpfs from an agent

An LLM agent needs three operations, all plain POSIX:

```sh
# 1. Fetch and read a document
cat /web/docs/report.html/content

# 2. Check whether the resource exists and what the server said
cat /web/docs/report.html/status        # "404\n" → not found

# 3. Inspect metadata (content type, length, server, cookies…)
cat /web/docs/report.html/headers
```

Reading is done through ordinary file operations (`read`, `pread`,
`seek`), so standard tools work too:

```console
$ wc -c /web/content
12734
$ head -c 40 /web/content
$ sha256sum /web/content
$ grep -i "content-type" /web/headers
```

Sequential reads at arbitrary offsets are local: the resource is
downloaded **once** per node, cached in memory, and sliced on demand.

## Behavior in detail

- **One GET per node.** The resource of a directory is downloaded the
  first time one of its views is `stat`ed or read, then cached for the
  life of the node. A second open of the same path re-fetches (a new
  node is created per lookup); plan Phase 2 adds a shared cache.
- **HTTP errors are not I/O errors.** A `404` or `500` response is a
  successful transport: the body (error page, JSON message, …) is
  readable through `content`, and the code through `status`. Only
  network-level failures (DNS, timeout, connection refused) surface
  as POSIX errors (`EHOSTUNREACH`, `ETIMEDOUT`, …).
- **`stat`** reports: `st_size` (view size), `st_mtime` taken from
  `Last-Modified` when the server provides it, stable `st_ino`
  derived from the URL (FNV-1a hash), mode `0555` for directories and
  `0444` for the views.
- **Time limits.** Connection establishment is bounded at 15 s, the
  whole transfer at 60 s; up to 10 redirects are followed.
- **Reserved names.** `content`, `headers` and `status` always resolve
  to the views — a remote path whose last component is literally
  named `content` cannot be reached through those names.
- **Encoding.** Path components are percent-encoded per RFC 3986
  (unreserved characters `A-Za-z0-9-._~` pass through, every other
  byte becomes `%XX`), so names with spaces or UTF-8 work.
- **Concurrency.** libnetfs serves RPCs from several threads; each
  node's download is guarded by its own mutex, and libcurl runs with
  `CURLOPT_NOSIGNAL`.

## Testing

```console
$ make check
```

- `test_url` — always built; deterministic, no network: validates URL
  construction and percent-encoding (`src/http.c`).
- `test_full_stream`, `test_range` — built when `libmicrohttpd` is
  present: an embedded loopback HTTP server serves a deterministic
  64 KiB body; the tests validate a full download byte for byte and
  single-range requests (206/416 handling). Absent libmicrohttpd,
  they are skipped and a warning is printed at configure time.

Everything runs on `127.0.0.1`; no external network is touched.

## Project layout

```
LICENSE                  GPLv3 full text
README.md                 this document
INSTALL.md                build and installation instructions
docs/architecture.md      internals: locks, references, POSIX mapping
CONTEXT.md                project vision (transport-layer purity)
PLAN.md                   development roadmap
src/
  http.c / http.h         transport layer: GET, buffers, URL joining
  httpfs.c                entry point: argument parsing, netfs startup
  httpfs.h                node model: struct netnode, view kinds
  netfs.c                 libnetfs callbacks: lookup, stat, dirents, read
tests/
  test_url.c              URL construction (deterministic)
  test_helpers.c / .h     embedded HTTP server (libmicrohttpd) + fetcher
  test_full_stream.c      whole-body download validation
  test_range.c            single-range request validation
configure.ac              build system (C23 detection, optional MHD)
```

## Architecture (in depth)

The full description — node model, locking protocol, reference
protocol (children pin their parents), the libnetfs callback contract
for each POSIX call, memory ownership and error mapping — is in
[`docs/architecture.md`](docs/architecture.md).

Summary of the flow:

```
client process
    │  POSIX calls (read, stat, getdents, …)
    ▼
GNU Mach IPC → libnetfs (locking, peropens, RPC dispatch)
    ▼
httpfs callbacks (src/netfs.c)
    │  ensure_resource(): one GET per node
    ▼
transport layer (src/http.c) → libcurl → remote HTTP server
```

## Status and roadmap

Implemented and working:
- URL tree navigation with percent-encoding
- `content` / `headers` / `status` views at every level
- One-shot fetch with caching, redirects, gzip, timeouts
- Read-only POSIX semantics, stable inodes, `Last-Modified` → `mtime`
- Deterministic test suite (URL building; HTTP transport with
  libmicrohttpd when available)

Planned (see `PLAN.md`):
- Phase 2: shared node cache (the reserved `ihash` field), HTTP
  `Range`-based streaming instead of full-body caching
- Phase 3: stacked content translators (`htmlfs`, `jsonfs`, ...)
- Phase 4: hardening, libmicrohttpd v2 fault injection, CI on
  Debian GNU/Hurd under QEMU

## License

`httpfs` is free software: you can redistribute it and/or modify it
under the terms of the **GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version**. See [`LICENSE`](LICENSE).

SPDX identifiers used throughout the sources:

```
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
```
