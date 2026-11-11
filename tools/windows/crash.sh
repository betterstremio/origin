#!/bin/bash
# Names the functions in a Windows crash line ("[crash] exception c0000005 thread 123 at Origin.exe+0x3fc2b8 ...", from
# crash.log or a shared applog): each Origin.exe address becomes the function it falls in, from the symbols of
# Origin.dbg.exe (the build that made the exe). WSL:  tools/windows/crash.sh "<the line>" [Origin.dbg.exe]
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../paths.sh"
LINE="${1:?the [crash] line}"; DBG="${2:-$BUILD_ROOT/windows/out/Origin.dbg.exe}"
x86_64-w64-mingw32-nm -n --defined-only "$DBG" | python3 -I -c '
import sys, bisect, re
syms = []
for l in sys.stdin:
    p = l.split()
    if len(p) >= 3 and p[1].lower() in "tw": syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]; base = 0x140000000
for m in re.finditer(r"(\S+)\+0x([0-9a-f]+)", sys.argv[1]):
    mod, off = m.group(1), int(m.group(2), 16)
    if mod.lower() != "origin.exe": print("  %-28s (another module)" % m.group(0)); continue
    i = bisect.bisect_right(addrs, base + off) - 1
    print("  %-28s %s+0x%x" % (m.group(0), syms[i][1] if i >= 0 else "?", base + off - addrs[i] if i >= 0 else 0))
' "$LINE"
