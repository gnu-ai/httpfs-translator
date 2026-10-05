# Building and installing httpfs

Validated end to end on Debian GNU/Hurd (hurd-amd64):
build, 4/4 tests, and mounting through `settrans` against live
servers.

## Prerequisites

- GNU/Hurd (for the translator itself).  On Debian GNU/Hurd, all the
  Hurd headers and translator libraries (`hurd/netfs.h`,
  `hurd/ihash.h`, `libnetfs`, `libihash`, ...) come from the single
  `hurd-dev` package — there is no `libnetfs-dev` nor `libihash`
  package.  The runtime libraries are pulled in as `hurd-libs0.3`.
- **libcurl >= 7.0.0** (`libcurl4-openssl-dev` on Debian)
- A **C23 compiler**: GCC 13 or newer (`-std=c23`), or a compiler
  accepting `-std=c2x`
- `autoconf`, `automake`, `pkg-config` (to regenerate the build
  system via `autogen.sh`)

The translator links, explicitly, against `libnetfs`, `libihash` and
`libiohelp` (all from `hurd-dev`) and `libmachuser` (from
`libc0.3-dev`, pulled in by `build-essential`): on Debian GNU/Hurd
every Hurd library is a separate shared object and the linker does
not search the dependencies of the libraries you name.
- *Optional*: `libmicrohttpd` (`libmicrohttpd-dev`) — only
  needed for `make check`; without it the two network tests are
  skipped and a warning is printed. Both the classic 0.9.x API and
  the 1.x API (Debian trixie ships 1.0.1, where callbacks return
  `enum MHD_Result`) are supported

## Building from a release tarball

```console
$ ./configure
$ make
```

## Building from the repository

```console
$ ./autogen.sh          # autoreconf --install --force
$ ./configure
$ make
```

Out-of-tree builds are supported:

```console
$ mkdir build && cd build && ../configure && make
```

`configure` fails with `a C23 compiler is required (GCC 13 or newer)`
if the toolchain cannot compile the C23 probe.

## Installing

```console
$ make install          # default: /usr/local/bin/httpfs
$ make install prefix=/opt/hurd-ai
```

## Running

httpfs is a *translator*: it must be started by `settrans`, which
provides the bootstrap port and the mount point.

```console
$ settrans -a /web httpfs https://example.org     # active translator
$ cat /web/content                                 # use it
$ settrans -g /web                                # detach/stop
```

Without a base URL, or started outside `settrans`, the program exits
with:

```
httpfs: usage: settrans -a <mount point> httpfs <URL>
httpfs: must be started as a translator (settrans).
```

## Running the test suite

```console
$ make check
```

- `test_url` runs everywhere (no network).
- `test_full_stream` and `test_range` require `libmicrohttpd` and
  perform loopback-only HTTP on `127.0.0.1` (ports 42831/42832).

## Troubleshooting

| Symptom | Cause / remedy |
|---------|----------------|
| `configure: error: a C23 compiler is required` | install GCC >= 13, or point `CC=` to it |
| `configure: WARNING: libmicrohttpd not found` | harmless: the network tests will be skipped |
| reads block for ~60 s then `ETIMEDOUT` | the remote host is unreachable; check the URL scheme and DNS |
| `cat /web/status` prints `404` | the transport worked; the remote path does not exist — this is the designed way to expose missing resources |
| `settrans: httpfs: No such file or directory` | the translator is not installed: run `make install`, or give `settrans` the full path to `src/httpfs` |
