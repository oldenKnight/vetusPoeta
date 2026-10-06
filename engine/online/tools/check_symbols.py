#!/usr/bin/env python3
"""DESIGN 1.3 guard: network code lives in engine/online only.

Usage: check_symbols.py <cmake build dir>
Runs `nm` over every static library built from engine/ (libvp_*.a, except libvp_online.a) and fails if one of
them references a socket, resolver, WinHTTP/WinINet or libcurl symbol, or starts processes (the dev transport's
way to the network). Then checks that libvp_online.a is where those references live (sanity: the scan works).
Python 3 stdlib only. Prints "SKIP" and exits 0 when nm is missing.
"""
import os
import re
import shutil
import subprocess
import sys

NETWORK = re.compile(r"^(__imp_)?_*(socket|connect|getaddrinfo|GetAddrInfoW?|gethostbyname|gethostbyname_r|WSAStartup|"
                     r"WSASocket[AW]?|curl_\w+|WinHttp\w+|Internet(Open|Connect)\w*|URLDownloadToFile[AW]?|"
                     r"posix_spawnp?|execvp?e?|popen|fork|vfork|CreateProcess[AW]?)(@\S*)?$")


def undefined(nm, lib):
    try:
        out = subprocess.run([nm, "--undefined-only", lib], capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if parts and NETWORK.match(parts[-1]):
            syms.add(parts[-1])
    return syms


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    nm = shutil.which("nm")
    if not nm:
        print("SKIP (nm not found)")
        return 0
    root = os.path.join(os.path.abspath(sys.argv[1]), "engine")
    libs = []
    for d, _, files in os.walk(root):
        libs += [os.path.join(d, f) for f in files if f.startswith("libvp_") and f.endswith(".a")]
    if not libs:
        print("FAIL: no engine libraries under " + root)
        return 1
    bad = 0
    online = None
    for lib in sorted(libs):
        syms = undefined(nm, lib)
        if syms is None:
            print("FAIL: nm could not read " + lib)
            bad += 1
            continue
        name = os.path.basename(lib)
        if name == "libvp_online.a":
            online = syms
            continue
        if syms:
            print("FAIL: %s references %s" % (name, ", ".join(sorted(syms))))
            bad += 1
        else:
            print("ok   %s: no network or process symbols" % name)
    if online is None:
        print("FAIL: libvp_online.a not found")
        bad += 1
    else:
        print("info libvp_online.a references %s" % (", ".join(sorted(online)) or "nothing"))
        if not online:
            print("FAIL: the scan found nothing in libvp_online.a either (scan broken?)")
            bad += 1
    print("PASS" if not bad else "FAILED (%d)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
