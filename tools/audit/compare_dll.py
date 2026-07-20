#!/usr/bin/env python3
"""Verify a built main.dll actually contains the data its source header declares.

Two DLLs compiled from identical source never byte-match: the PE header carries a
timestamp, the debug directory carries a fresh RSDS GUID, and the toolchain on a
hosted runner drifts. So "diff the DLLs" answers nothing. What *is* invariant is the
data: every `inline constexpr` point array in cairn_data.hpp lands in .rdata as a
contiguous run of packed little-endian int32s. We can regenerate those bytes from the
source and find them in the binary.

That direction matters. Comparing a DLL against another DLL only tells you two blobs
agree. Comparing a DLL against the *source header* tells you the binary carries the
data the source says it does -- which is the actual question when you're deciding
whether to run someone's compiled artifact inside your game.

Usage:
    compare_dll.py <dll> [--header <cairn_data.hpp>]     verify DLL against source
    compare_dll.py <dll> --against <other.dll>           compare two builds' data
    compare_dll.py <dll> --header <hpp> --against <other.dll>    both

Exit status is 0 only if every check passes.

Not covered, deliberately: kNotes and kLayers embed `const wchar_t*` pointers, whose
values depend on image layout and relocations, so their bytes are not comparable this
way. They are reported as skipped rather than silently counted as passing.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------- source header

ARRAY_RE = re.compile(
    r"inline\s+constexpr\s+(?P<type>Point|GuidPoint)\s+(?P<name>k\w+)\s*\[\]\s*=\s*\{(?P<body>.*?)\};",
    re.S,
)
POINT_RE = re.compile(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}")
# GUIDs are emitted as hex literals (0xA9589829); coords stay decimal and signed.
_HEX = r"(?:0[xX][0-9a-fA-F]+|\d+)"
GUIDPOINT_RE = re.compile(
    rf"\{{\s*\{{\s*({_HEX})\s*,\s*({_HEX})\s*,\s*({_HEX})\s*,\s*({_HEX})\s*\}}\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}}"
)


def parse_header(path):
    """cairn_data.hpp -> {name: (type, packed_bytes, count)}"""
    text = path.read_text(encoding="utf-8", errors="replace")
    out = {}
    for m in ARRAY_RE.finditer(text):
        name, typ, body = m["name"], m["type"], m["body"]
        if typ == "Point":
            pts = [(int(a), int(b)) for a, b in POINT_RE.findall(body)]
            blob = b"".join(struct.pack("<ii", x, y) for x, y in pts)
        else:  # GuidPoint { uint32 guid[4]; int32 x, y; }
            rows = GUIDPOINT_RE.findall(body)
            pts = [tuple(int(v, 0) for v in r) for r in rows]
            blob = b"".join(struct.pack("<IIIIii", *r) for r in pts)
        out[name] = (typ, blob, len(pts))
    return out


# ---------------------------------------------------------------- minimal PE

def pe_sections(data):
    """[(name, file_off, raw_size, va)] -- enough to attribute a hit to a section."""
    if data[:2] != b"MZ":
        raise ValueError("not a PE image (no MZ)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image (no PE signature)")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    tbl = pe + 24 + opt_size
    secs = []
    for i in range(nsec):
        off = tbl + i * 40
        name = data[off : off + 8].rstrip(b"\0").decode("ascii", "replace")
        # IMAGE_SECTION_HEADER: +8 VirtualSize, +12 VirtualAddress,
        #                       +16 SizeOfRawData, +20 PointerToRawData
        _vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        secs.append((name, raw_ptr, raw_size, va))
    return secs


def which_section(secs, off):
    for name, ptr, size, _ in secs:
        if ptr <= off < ptr + size:
            return name
    return "?"


def pe_imports_exports(data):
    """(sorted imported DLL names, sorted exported symbol names) -- best effort."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)  # PE32+ vs PE32
    secs = pe_sections(data)

    def rva2off(rva):
        for _, ptr, size, va in secs:
            if va <= rva < va + max(size, 1):
                return ptr + (rva - va)
        return None

    def cstr(off):
        end = data.index(b"\0", off)
        return data[off:end].decode("ascii", "replace")

    imports = []
    imp_rva = struct.unpack_from("<I", data, dd + 8)[0]  # entry 1 = import
    off = rva2off(imp_rva) if imp_rva else None
    if off:
        while True:
            name_rva = struct.unpack_from("<I", data, off + 12)[0]
            if not name_rva:
                break
            n = rva2off(name_rva)
            if n is None:
                break
            imports.append(cstr(n))
            off += 20

    exports = []
    exp_rva = struct.unpack_from("<I", data, dd)[0]  # entry 0 = export
    off = rva2off(exp_rva) if exp_rva else None
    if off:
        cnt, names_rva = struct.unpack_from("<I", data, off + 24)[0], struct.unpack_from("<I", data, off + 32)[0]
        t = rva2off(names_rva)
        if t:
            for i in range(cnt):
                nr = struct.unpack_from("<I", data, t + i * 4)[0]
                n = rva2off(nr)
                if n:
                    exports.append(cstr(n))
    return sorted(set(imports)), sorted(set(exports))


# ---------------------------------------------------------------- checks

# kernel32 is now genuinely used (GetModuleHandleExW/GetModuleFileNameW) to locate
# the mod's own directory for settings.txt. Everything else stays forbidden: no
# ws2_32/winhttp/wininet (network), no advapi32 (registry), no shell32 (process).
ALLOWED_IMPORT_PREFIXES = ("ue4ss", "msvcp", "vcruntime", "api-ms-win-crt", "kernel32")
EXPECTED_EXPORTS = {"start_mod", "uninstall_mod"}


def locate(data, blob):
    """All file offsets where blob occurs (expect exactly 1 for a real array)."""
    hits, start = [], 0
    while True:
        i = data.find(blob, start)
        if i < 0:
            return hits
        hits.append(i)
        start = i + 1
        if len(hits) > 4:  # runaway guard; a unique blob won't hit this
            return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dll", type=Path)
    ap.add_argument("--header", type=Path, help="cairn_data.hpp to verify against")
    ap.add_argument("--against", type=Path, help="second DLL to compare data blobs with")
    args = ap.parse_args()

    data = args.dll.read_bytes()
    secs = pe_sections(data)
    print(f"{args.dll}  ({len(data):,} bytes)")
    for name, ptr, size, _ in secs:
        print(f"    {name:<8} {size:>9,} B @ 0x{ptr:06x}")

    ok = True

    # ---- imports / exports
    imports, exports = pe_imports_exports(data)
    print(f"\n  imports: {', '.join(imports) or '(none)'}")
    bad = [i for i in imports if not i.lower().startswith(ALLOWED_IMPORT_PREFIXES)]
    if not imports:
        # An empty list means the parse failed, not that the DLL imports nothing.
        # Never let that read as a pass.
        print("  FAIL: no imports parsed — import directory unreadable, cannot vouch for this binary")
        ok = False
    elif bad:
        print(f"  FAIL: unexpected imports (network/registry/process capability?): {bad}")
        ok = False
    else:
        print("  PASS: imports limited to UE4SS + MSVC CRT + kernel32")

    print(f"  exports: {', '.join(exports) or '(none)'}")
    if set(exports) != EXPECTED_EXPORTS:
        print(f"  FAIL: expected exactly {sorted(EXPECTED_EXPORTS)}")
        ok = False
    else:
        print("  PASS: exports are exactly start_mod/uninstall_mod")

    # ---- data blobs vs source header
    found = {}
    if args.header:
        arrays = parse_header(args.header)
        print(f"\n  {args.header}: {len(arrays)} comparable arrays")
        total = 0
        for name, (typ, blob, count) in sorted(arrays.items()):
            if not blob:
                print(f"    {name:<14} {count:>6} pts  SKIP (empty)")
                continue
            hits = locate(data, blob)
            total += count
            if len(hits) == 1:
                found[name] = blob
                print(f"    {name:<14} {count:>6} pts  found @ 0x{hits[0]:06x} ({which_section(secs, hits[0])})")
            elif not hits:
                print(f"    {name:<14} {count:>6} pts  *** NOT FOUND IN BINARY ***")
                ok = False
            else:
                # Not a failure per se: identical data could legitimately dedupe.
                found[name] = blob
                print(f"    {name:<14} {count:>6} pts  found x{len(hits)} @ {[hex(h) for h in hits]}")
        print(f"  {'PASS' if ok else 'FAIL'}: {total:,} points across {len(found)} arrays located in binary")
        print("  note: kNotes/kLayers embed pointers -> not byte-comparable, not checked here")

    # ---- data blobs vs another DLL
    if args.against:
        other = args.against.read_bytes()
        print(f"\n  vs {args.against} ({len(other):,} bytes)")
        if not found:
            print("    (need --header to know what to compare)")
        else:
            same = 0
            for name, blob in sorted(found.items()):
                if locate(other, blob):
                    same += 1
                else:
                    print(f"    {name:<14} *** DIFFERS / absent in other build ***")
                    ok = False
            print(f"    {same}/{len(found)} arrays byte-identical in both builds")
            if same == len(found):
                print("    PASS: both binaries carry the same data")

    print(f"\n{'PASS — binary matches source' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
