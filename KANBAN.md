<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

Kanban of the httpfs translator, derived from PLAN.md: one card
per task. Moving a card means moving it between the sections
below. Deliverables and acceptance criteria stay in PLAN.md.
-->

# httpfs translator — Kanban

Derived from [`PLAN.md`](PLAN.md). Cards follow the phases of the
plan; the checked state mirrors PLAN.md and the repository state.

## To do

### Phase 1: Foundation & Deterministic Testing Environment
- [ ] Write the core C test suite using **`libmicrohttpd` v1** to simulate a loopback (`127.0.0.1`) embedded HTTP server
- [ ] Implement specific test endpoints: `Range` header support, truncated responses, HTTP error codes, static raw files for post-read SHA256 checksum validation
- [ ] Ensure tests model atomic POSIX calls and remain completely environment-agnostic

### Phase 3: Content-Type Translators Architecture (The Parser Chain)
- [ ] Cluster operation: `httpfs` and the parser chain usable in a multi-node Hurd cluster — one seek/Range cursor per open file per reader, stacking behaviour independent of single-node topology

### Phase 4: Evolution & Hardening
- [ ] Controlled migration of embedded tests from `libmicrohttpd` v1 to **`libmicrohttpd` v2** (complex fault-injection scenarios)
- [ ] Multi-task and multi-user operation: concurrent readers and users, one seek/Range cursor per open file per reader, no shared mutable state without locking
- [ ] Optimize Mach IPC and minimize buffer-copy overhead (memory-to-memory I/O) between `httpfs` and upstream content translators

## In progress

_(nothing)_

## Done

### Phase 1: Foundation & Deterministic Testing Environment (started)
- [x] GitHub Actions workflow to run Debian GNU/Hurd inside QEMU (`.github/workflows/hurd.yml`)

### Phase 3: Content-Type Translators Architecture (The Parser Chain)
- [x] **`htmlfs`:** standalone DOM/HTML parsing translator, stacked via `settrans` on top of `httpfs` (tolerant single-pass extractor: title, visible text, meta, headings, links)
- [x] **`jsonfs`:** JSON translator mapping JSON trees into Mach file system nodes (objects → directories, arrays → numbered entries, scalars → files)
- [x] **`csvfs` / `tsvfs`:** dynamic interpretation of structured text tables (RFC 4180 dialect, delimiter sniffing, row and column views; `tsvfs` is `csvfs` under its tab default)
- [x] Dedicated test suites for the transport/parser chain I/O (`test_json`, `test_html`, `test_csv` unit tests plus the stacking section of `tests/smoke.sh`)

### Phase 2: Refactoring Core `httpfs` (Raw Transport)
- [x] Strip all HTML parsing code from the current `httpfs` implementation
- [x] Optimize `libnetfs` callbacks to map POSIX calls cleanly and performantly to the network
- [x] Implement a robust seek mechanism using HTTP `Range` headers to faithfully map `lseek`/`pread`
- [x] Validate `httpfs` against the Phase 1 test suite (100% raw bytes, EOF, timeouts, disconnects)
