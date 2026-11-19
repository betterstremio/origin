#!/usr/bin/env python3
"""Validate the Tizen shared libraries against the port's ARM ABI and glibc baseline:
- the libraries it needs: only the C library family and GLES (SDL and zlib are linked in; EGL and the TV's player,
  window and settings libraries are opened by name at run time, never needed up front: a TV without one of those
  file names still loads the library and says so);
- no C library symbol newer than Tizen 4's (glibc 2.24);
- both symbol hash tables (old and GNU), whichever loader reads it;
- no thread-local storage segment (the TVs' loaders of 4.0-5.5 and our own in-memory loader do not set one up);
- ARM, 32-bit, the soft-float calling convention of the TVs' system libraries.
Usage: check-elf.py [--engine] <lib.so>   (exit 1 with the reasons when it fails)"""
import argparse
import struct
import sys

ALLOWED = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "ld-linux.so.3",
           "libGLESv2.so.2"}
GLIBC_MAX = (2, 24)
PT_DYNAMIC, PT_TLS = 2, 7
DT_NEEDED, DT_HASH, DT_STRTAB, DT_STRSZ, DT_GNU_HASH, DT_VERNEED, DT_VERNEEDNUM = 1, 4, 5, 10, 0x6FFFFEF5, 0x6FFFFFFE, 0x6FFFFFFF
EF_ARM_ABI_FLOAT_HARD = 0x400


def check(path, engine):
    with open(path, "rb") as library:
        d = library.read()
    bad = []
    if len(d) < 52 or d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        return ["not a 32-bit little-endian ELF file"]
    e_machine, = struct.unpack_from("<H", d, 18)
    e_flags, = struct.unpack_from("<I", d, 36)
    e_phoff, = struct.unpack_from("<I", d, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", d, 42)
    if e_machine != 40:
        bad.append("not ARM (machine %d)" % e_machine)
    if e_flags & EF_ARM_ABI_FLOAT_HARD:
        bad.append("hard-float calling convention (the TVs use soft-float)")
    phs = [struct.unpack_from("<IIIIIIII", d, e_phoff + i * e_phentsize) for i in range(e_phnum)]
    loads = [p for p in phs if p[0] == 1]

    def off(vaddr):  # a virtual address to a file offset
        for p in loads:
            if p[2] <= vaddr < p[2] + p[4]:
                return vaddr - p[2] + p[1]
        raise ValueError("address 0x%x outside the file" % vaddr)

    if not engine and any(p[0] == PT_TLS for p in phs):
        bad.append("a thread-local storage segment (use pthread keys for the native app)")
    dyn = [p for p in phs if p[0] == PT_DYNAMIC]
    if not dyn:
        return bad + ["no dynamic section"]
    tags = []
    o = dyn[0][1]
    while True:
        tag, val = struct.unpack_from("<iI", d, o)
        o += 8
        if tag == 0:
            break
        tags.append((tag & 0xFFFFFFFF, val))
    get = lambda t: [v for k, v in tags if k == t]
    strtab = off(get(DT_STRTAB)[0])
    cstr = lambda i: d[strtab + i:d.index(b"\0", strtab + i)].decode()
    needed = [cstr(v) for v in get(DT_NEEDED)]
    for n in needed:
        if n not in ALLOWED:
            bad.append("needs %s from the TV (link it in, or open it by name at run time)" % n)
    if not get(DT_HASH):
        bad.append("no old-style hash table (-Wl,--hash-style=both)")
    if not get(DT_GNU_HASH):
        bad.append("no GNU hash table (-Wl,--hash-style=both)")
    newest = (0, 0)
    if get(DT_VERNEED):
        o = off(get(DT_VERNEED)[0])
        for _ in range(get(DT_VERNEEDNUM)[0]):
            vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from("<HHIII", d, o)
            a = o + vn_aux
            for _ in range(vn_cnt):
                vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from("<IHHII", d, a)
                name = cstr(vna_name)
                if name.startswith("GLIBC_") and name[6:7].isdigit():
                    v = tuple(int(x) for x in name[6:].split(".")[:2])
                    newest = max(newest, v)
                    if v > GLIBC_MAX:
                        bad.append("%s (from %s) is newer than Tizen 4's C library (GLIBC_%d.%d)" % (name, cstr(vn_file), *GLIBC_MAX))
                a += vna_next
                if not vna_next:
                    break
            o += vn_next
            if not vn_next:
                break
    print("%s: needs %s; newest C library symbol GLIBC_%d.%d" % (path, ", ".join(needed), *newest))
    return bad


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="store_true", help="allow engine TLS handled by the native loader")
    parser.add_argument("library")
    args = parser.parse_args()
    reasons = check(args.library, args.engine)
    for r in reasons:
        print("  FAIL: " + r, file=sys.stderr)
    sys.exit(1 if reasons else 0)
