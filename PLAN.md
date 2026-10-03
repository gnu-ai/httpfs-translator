# Development Plan & Roadmap

## Phase 1: Foundation & Deterministic Testing Environment
- [ ] Prepare GitHub Actions workflow to run Debian GNU/Hurd inside QEMU.
- [ ] Write the core C test suite using **`libmicrohttpd` v1** to simulate a loopback (`127.0.0.1`) embedded HTTP server.
- [ ] Implement specific test endpoints: `Range` header support, truncated responses, HTTP error codes, static raw files for post-read SHA256 checksum validation.
- [ ] Ensure tests model atomic POSIX calls and remain completely environment-agnostic.

## Phase 2: Refactoring Core `httpfs` (Raw Transport)
- [x] Strip all HTML parsing code from the current `httpfs` implementation.
- [x] Optimize `libnetfs` callbacks to map POSIX calls cleanly and performantly to the network.
- [x] Implement a robust seek mechanism using HTTP `Range` headers to faithfully map `lseek`/`pread`.
- [x] Validate `httpfs` against the Phase 1 test suite (must reliably yield 100% raw bytes while handling EOF, timeouts, and disconnects).

## Phase 3: Content-Type Translators Architecture (The Parser Chain)
- [x] **`htmlfs`:** Develop the standalone DOM/HTML parsing translator and interface it to be stacked via `settrans` on top of `httpfs` (tolerant single-pass extractor: title, visible text, meta, headings, links).
- [x] **`jsonfs`:** Design and build the JSON translator to map JSON trees into Mach file system nodes (objects → directories, arrays → numbered entries, scalars → files).
- [x] **`csvfs` / `tsvfs`:** Develop interfaces to dynamically interpret structured text tables (RFC 4180 dialect, delimiter sniffing, row and column views; `tsvfs` is `csvfs` under its tab default).
- [x] Write dedicated test suites to verify correct I/O interactions between the transport translator (`httpfs`) and parser translators (`test_json`, `test_html`, `test_csv` unit tests plus the stacking section of `tests/smoke.sh`).

## Phase 4: Evolution & Hardening
- [ ] Controlled migration of embedded tests from `libmicrohttpd` v1 to **`libmicrohttpd` v2**, leveraging new APIs for increased I/O determinism (e.g., complex fault-injection scenarios).
- [ ] Multi-task and multi-user operation: `httpfs` and the parser chain (`htmlfs`, `jsonfs`, `csvfs`/`tsvfs`) must serve concurrent readers and several users at once — one seek/Range cursor per open file per reader, no shared mutable state without locking, no user-serialized global state.
- [ ] Optimize Mach IPC and minimize buffer-copy overhead (memory-to-memory I/O) between `httpfs` and upstream content translators.
