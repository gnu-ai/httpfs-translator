#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# CLI conformance test for the GNU base commands: every binary of
# the httpfs chain (httpfs, htmlfs, jsonfs, csvfs, tsvfs) must
# answer --version and --help on stdout with exit status 0 — on
# any system, without Hurd libraries, mount points or network.
#
# Usage: tests/test_cli.sh [builddir]   (default: the build tree
# next to this script, i.e. ../src when run from tests/).

set -u

BUILDDIR="${1:-../src}"
FAIL=0

say() { printf '%s\n' "$*"; }
fail() { say "test_cli: FAIL: $*"; FAIL=1; }

for prog in httpfs htmlfs jsonfs csvfs tsvfs; do
    BINARY="$BUILDDIR/$prog"
    if [ ! -x "$BINARY" ]; then
        # The translators need the Hurd libraries to link; outside
        # GNU/Hurd they are simply not built.  Skip, do not fail.
        say "test_cli: SKIP: $BINARY not built (requires GNU/Hurd)"
        exit 77
    fi

    "$BINARY" --version >/dev/null 2>&1 \
        || fail "$prog --version must exit 0"
    "$BINARY" --version 2>/dev/null | grep -q 'GNU AI' \
        || fail "$prog --version must name the project"
    "$BINARY" --version 2>/dev/null | grep -q 'GPLv3' \
        || fail "$prog --version must name the license"
    "$BINARY" -V >/dev/null 2>&1 \
        || fail "$prog -V must exit 0"
    "$BINARY" --help >/dev/null 2>&1 \
        || fail "$prog --help must exit 0"
    "$BINARY" --help 2>/dev/null | grep -q 'Usage:' \
        || fail "$prog --help must show the usage"
    "$BINARY" --help 2>/dev/null | grep -q 'Report bugs' \
        || fail "$prog --help must tell where to report bugs"
    "$BINARY" -h >/dev/null 2>&1 \
        || fail "$prog -h must exit 0"
done

if [ "$FAIL" = 0 ]; then
    say "test_cli: OK"
    exit 0
fi
exit 1
