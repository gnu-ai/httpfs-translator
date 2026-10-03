#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

"""Headless GNU/Hurd test driver: build the boot ISO, run QEMU, drive
the serial console, and execute the build + test suite inside the VM.

Everything is driven through the serial line (console=com0), so no
graphics, no SSH and no manual interaction are ever needed:

    qemu -serial <this driver>  <->  GNU/Hurd getty (root, no password)

The steps performed inside the guest mirror the documented manual
validation (INSTALL.md): apt dependencies, autogen, configure, make,
make check, and the settrans smoke test against a loopback server.

Usage:  hurd_vm.py <hurd-image.qcow2/raw> <repo-url> <git-sha> [jobs]

Requires on the host: qemu-system-x86 (KVM if available), grub-mkimage
with the i386-pc modules (Debian: grub-pc-bin + grub-common).
"""

import os
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# Image parameters: discovered automatically from the image's own
# /boot/grub/grub.cfg when run with --probe, otherwise use the values
# of the official Debian GNU/Hurd preinstalled images.
HURD_IMG_URL = ("https://cdimage.debian.org/cdimage/ports/latest/"
                "hurd-amd64/debian-hurd-amd64-20260314.img.tar.xz")
ROOT_PART = "2"                       # partition of the root filesystem
FS_UUID = "41bea907-da2e-4747-b848-8a2af4ce43bf"
KERNEL = "/boot/gnumach-1.8-amd64-up.gz"

GRUB_MODULES = ["serial", "terminal", "biosdisk", "ata", "ext2", "fat",
                "part_msdos", "multiboot", "boot", "echo", "search",
                "gzio", "cat", "ls", "normal", "configfile"]

TIMEOUT_LOGIN = 900                   # seconds to reach the login prompt
TIMEOUT_CMD = 1800                    # seconds per guest command

# ------------------------------------------------------------------


def build_boot_iso(workdir: str) -> str:
    """Create the El Torito ISO with the serial-console GRUB."""
    cfg_in = os.path.join(HERE, "hurd-boot.cfg")
    early = os.path.join(workdir, "hurd-boot.cfg")
    with open(cfg_in) as f:
        cfg = f.read()
    cfg = (cfg.replace("@KERNEL@", KERNEL)
              .replace("@ROOTPART@", ROOT_PART)
              .replace("@UUID@", FS_UUID)
              .replace("@PREFIX@", f"(hd0,msdos{ROOT_PART})/boot/grub"))
    with open(early, "w") as f:
        f.write(cfg)

    eltorito = os.path.join(workdir, "grub.eltorito")
    subprocess.run(
        ["grub-mkimage", "-O", "i386-pc-eltorito", "-o", eltorito,
         "-c", early, "-p", f"(hd0,msdos{ROOT_PART})/boot/grub"]
        + GRUB_MODULES,
        check=True)

    iso = os.path.join(workdir, "httpfs-ci.iso")
    subprocess.run([sys.executable,
                    os.path.join(HERE, "make_iso.py"), eltorito, iso],
                   check=True)
    return iso


# ------------------------------------------------------------------
# Serial console driver: QEMU runs with -serial telnet:..., and we
# speak the Telnet protocol on a local socket (IAC negotiation only).

class SerialVM:
    def __init__(self, iso: str, disk: str, port: int = 44555,
                 memory: str = "2G", machine: str = "pc"):
        self.port = port
        accel = "-enable-kvm" if os.path.exists("/dev/kvm") else "-accel tcg"
        self.cmd = [
            "qemu-system-x86_64", accel, f"-m", memory, "-M", machine,
            "-no-reboot",
            "-cdrom", iso, "-boot", "d",
            "-drive", f"file={disk},cache=writeback",
            "-net", "user", "-net", "nic,model=e1000",
            "-display", "none",
            "-serial", f"telnet:127.0.0.1:{port},server,nowait",
        ]
        self.proc = None
        self.log = b""
        self.sock = None

    def start(self):
        self.proc = subprocess.Popen(self.cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(
                    ("127.0.0.1", self.port), timeout=5)
                self.sock.settimeout(5)
                # refuse the Telnet negotiations QEMU proposes
                self.sock.sendall(b"\xff\xfc\x01\xff\xfc\x03\xff\xfe\x00")
                return
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError("QEMU exited early")
                time.sleep(0.5)
        raise RuntimeError("serial port never came up")

    def read_some(self, wait: float = 5.0) -> bytes:
        self.sock.settimeout(wait)
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return b""
        clean = re.sub(rb"\xff[\xfd\xfc\xfe\xfb].", b"", data)
        self.log += clean
        return clean

    def wait_for(self, pattern: str, timeout: int) -> bool:
        rx = re.compile(pattern.encode())
        deadline = time.time() + timeout
        while time.time() < deadline:
            if rx.search(self.log):
                return True
            self.read_some(5)
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU exited during wait for " + pattern)
        return False

    def send(self, line: str):
        self.sock.sendall(line.encode() + b"\r")

    def stop(self):
        if self.sock:
            self.sock.close()
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# ------------------------------------------------------------------

def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    disk, repo_url, sha = sys.argv[1], sys.argv[2], sys.argv[3]

    iso = build_boot_iso(os.getcwd())
    vm = SerialVM(iso, disk)
    vm.start()
    ok = False
    logf = open("serial.log", "wb")
    old_log = vm.log
    try:
        print("[ci] waiting for the Hurd login prompt "
              f"(up to {TIMEOUT_LOGIN}s)...")
        if not vm.wait_for(r"login:", TIMEOUT_LOGIN):
            print("[ci] FAIL: no login prompt on the serial console")
            print(vm.log.decode("latin-1", "replace")[-4000:])
            return 1
        print("[ci] login prompt reached, logging in as root")
        vm.send("root")
        if not vm.wait_for(r"root@|# |\$ ", 120):
            print("[ci] FAIL: no shell prompt after login")
            return 1

        def run(cmd, timeout=TIMEOUT_CMD):
            print(f"[ci] $ {cmd}")
            vm.log = b""
            vm.send(cmd + "; echo RC=$?")
            if not vm.wait_for(r"RC=(\d+)", timeout):
                print("[ci] FAIL: command did not complete: " + cmd)
                return None
            m = re.search(rb"RC=(\d+)", vm.log)
            print(vm.log.decode("latin-1", "replace")[-2000:])
            return int(m.group(1)) == 0

        steps = [
            "export DEBIAN_FRONTEND=noninteractive",
            "apt-get update",
            ("apt-get install -y --no-install-recommends "
             "build-essential autoconf automake pkg-config "
             "libcurl4-openssl-dev libmicrohttpd-dev hurd-dev git "
             "python3 ca-certificates"),
            f"rm -rf /src && git clone {repo_url} /src",
            f"cd /src && git checkout {sha}",
            "cd /src && ./autogen.sh && ./configure && make",
            "cd /src && make check",
            "cd /src && sh tests/smoke.sh $PWD/src/httpfs",
        ]
        for cmd in steps:
            if run(cmd) is not True:
                print(f"[ci] FAIL at: {cmd}")
                return 1
        print("[ci] ALL PASS")
        ok = True
        run("poweroff", 300)
        return 0
    finally:
        logf.write(vm.log)
        logf.close()
        vm.stop()
        if not ok:
            print("---- serial console tail ----")
            print(vm.log.decode("latin-1", "replace")[-4000:])


if __name__ == "__main__":
    sys.exit(main())
