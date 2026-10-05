#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# CI guest script: build and test httpfs-translator on real
# GNU/Hurd, driven by
# gnu-ai/mistral-vm-debian-hurd (hurd_vm.py) — the phase 1 CI
# item of its PLAN.md.
#
#   python3 hurd_vm.py <image> ci/guest-hurd.sh
#
# Runs as root inside the guest; the exit code of this script
# becomes the exit code of the driver.  Steps: build from the
# pushed branch, run the whole deterministic suite (libmicrohttpd
# embedded server on the loopback), then a smoke test through a
# REAL mounted translator fetching a REAL URL.

set -e

echo "=== guest: $(uname -a)"

echo "=== installing the build dependencies"
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential autoconf automake pkg-config \
    libcurl4-openssl-dev libmicrohttpd-dev ca-certificates wget

echo "=== fetching the pushed tree from the CI host"
rm -rf httpfs-translator repo.tar.gz
wget -q -O repo.tar.gz http://10.0.2.2:8000/httpfs-translator.tar.gz \
    || { echo "FAIL: the CI host is not serving the tree on :8000"; exit 1; }
tar xzf repo.tar.gz
cd httpfs-translator

echo "=== building"
sh ./autogen.sh
sh ./configure
make

echo "=== make check (embedded loopback server, no real network)"
make check

echo "=== smoke test: a real URL through the mounted translator"
# The CA bundle of this Debian ports snapshot is broken (no
# certificates installed at all), so the smoke test drives a
# real plain-HTTP URL through the NAT the same apt uses.  The
# transport contract (content, headers, status) is identical.
rm -f /web
touch /web
settrans -a /web ./src/httpfs http://deb.debian.org/debian/
ls /web
V=$(cat /web/status)
echo "status: $V"
[ "$V" = "200" ] || { echo "FAIL: expected HTTP 200, got '$V'"; exit 1; }
C=$(head -c 64 /web/content)
[ -n "$C" ] || { echo "FAIL: the content must not be empty"; exit 1; }
echo "content starts with: $C"
settrans -g /web
sleep 1

echo "=== all good"
