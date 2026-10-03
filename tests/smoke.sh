#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# End-to-end smoke test of the httpfs translator on a live GNU/Hurd
# system: mount a local loopback HTTP server through settrans and
# validate the three views (content, status, headers), a deep URL
# path and a 404.  Loopback only; fully deterministic.
#
# Usage: tests/smoke.sh /path/to/httpfs

set -e
BINARY="$1"
PORT=42999
FAIL=0

say() { printf '%s\n' "$*"; }
fail() { say "smoke: FAIL: $*"; FAIL=1; }

[ -x "$BINARY" ] || { say "smoke: $BINARY not executable"; exit 2; }

mkdir -p /tmp/httpfs-smoke
printf 'Bonjour, monde !' > /tmp/httpfs-smoke/hello.txt

( cd /tmp/httpfs-smoke && python3 -m http.server "$PORT" --bind 127.0.0.1 ) &
SERVER=$!
sleep 2

mkdir -p /web
settrans -a /web "$BINARY" "http://127.0.0.1:$PORT"

[ "$(cat /web/status 2>/dev/null)" = "200" ] \
    || fail "status view should report 200"
# NB: /web/content is the server's directory listing (the body of
# the root URL); the exact bytes of a file live under its own path.
[ "$(cat /web/hello.txt/content 2>/dev/null)" = "Bonjour, monde !" ] \
    || fail "content view should return the exact body"
[ "$(stat -c %s /web/hello.txt/content 2>/dev/null)" = "16" ] \
    || fail "stat should know the size without a download"
[ "$(cat /web/hello.txt/status 2>/dev/null)" = "200" ] \
    || fail "deep URL path should work"
[ "$(cat /web/missing.txt/status 2>/dev/null)" = "404" ] \
    || fail "a missing resource should report 404"
[ "$(head -c 40 /web/headers 2>/dev/null)" != "" ] \
    || fail "headers view should be readable"

# ------------------------------------------------------------------
# Phase 3: the parser chain, stacked on the transport above.
# The same loopback server serves a JSON and a CSV document; the
# parsers mount /web/content-style views of them.
# ------------------------------------------------------------------

SRCDIR=$(dirname "$BINARY")

printf '{"version": "1.2", "stable": true, "mirrors": ["a", "b"]}' \
    > /tmp/httpfs-smoke/api.json
printf 'name,year\nGNU,1983\n' > /tmp/httpfs-smoke/table.csv
printf '<html><head><title>Smoke &amp; Test</title></head><body><h1>Hi</h1><a href="/x">link</a></body></html>' \
    > /tmp/httpfs-smoke/page.html

# The parser mount points live on the real filesystem; the SOURCE
# paths (/web/api.json/content, ...) need no mkdir — httpfs
# synthesizes URL directories, and the tree is read-only anyway.
mkdir -p /api /table /page

settrans -a /api "$SRCDIR/jsonfs" /web/api.json/content
[ "$(cat /api/version 2>/dev/null)" = "1.2" ] \
    || fail "jsonfs should expose the version member"
[ "$(cat /api/stable 2>/dev/null)" = "true" ] \
    || fail "jsonfs should expose the boolean member"
[ "$(cat /api/mirrors/1 2>/dev/null)" = "b" ] \
    || fail "jsonfs should expose array entries"
settrans -D /api

settrans -a /table "$SRCDIR/csvfs" /web/table.csv/content
[ "$(cat /table/count 2>/dev/null)" = "1" ] \
    || fail "csvfs should count the data rows"
[ "$(cat /table/rows/0/year 2>/dev/null)" = "1983" ] \
    || fail "csvfs row view should expose the year cell"
[ "$(cat /table/columns/name 2>/dev/null)" = "GNU" ] \
    || fail "csvfs column view should expose the name values"
settrans -D /table

settrans -a /page "$SRCDIR/htmlfs" /web/page.html/content
[ "$(cat /page/title 2>/dev/null)" = "Smoke & Test" ] \
    || fail "htmlfs should decode entities in the title"
[ "$(cat /page/headings/0 2>/dev/null)" = "1	Hi
" ] \
    || fail "htmlfs should expose the heading"
[ "$(cat /page/links/0/url 2>/dev/null)" = "/x
" ] \
    || fail "htmlfs should expose the link target"
settrans -D /page

settrans -D /web
kill "$SERVER" 2>/dev/null || true
rm -rf /tmp/httpfs-smoke

if [ "$FAIL" = 0 ]; then
    say "smoke: OK"
    exit 0
fi
exit 1
