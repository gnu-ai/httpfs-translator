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
[ "$(cat /web/content 2>/dev/null)" = "Bonjour, monde !" ] \
    || fail "content view should return the exact body"
[ "$(stat -c %s /web/content 2>/dev/null)" = "16" ] \
    || fail "stat should know the size without a download"
[ "$(cat /web/hello.txt/status 2>/dev/null)" = "200" ] \
    || fail "deep URL path should work"
[ "$(cat /web/missing.txt/status 2>/dev/null)" = "404" ] \
    || fail "a missing resource should report 404"
[ "$(head -c 40 /web/headers 2>/dev/null)" != "" ] \
    || fail "headers view should be readable"

settrans -D /web
kill "$SERVER" 2>/dev/null || true
rm -rf /tmp/httpfs-smoke

if [ "$FAIL" = 0 ]; then
    say "smoke: OK"
    exit 0
fi
exit 1
