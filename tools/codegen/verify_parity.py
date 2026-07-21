#!/usr/bin/env python3
"""Prove a regenerated cairn_data.hpp is semantically identical to the committed one.

The header is 53 lines but 145 KB: each array is one enormous single line, so a
line diff reports "everything changed" for a one-point edit and reports nothing
useful for a reorder. Text diffing this file cannot answer the only question that
matters -- did any actual data change?

So parse both sides into structures and compare those: array contents (order-
sensitive within an array, since kLayers points at them by index), the layer table
(name, colour, count, default_on, icon, and which array each row references), and
the emitted symbols. Array *emission order* in the file is deliberately ignored --
it's a C++ declaration order with no semantic weight.

    python3 tools/codegen/verify_parity.py <committed.hpp> <regenerated.hpp>
    python3 tools/codegen/verify_parity.py --regen     # regenerate to a temp file and compare

Exit 0 only if the two are semantically equal.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "cpp-mod/mods/CairnMap/src/cairn_data.hpp"
GEN = REPO / "tools/codegen/gen_cairn_data.py"

ARRAY_RE = re.compile(
    r"inline\s+constexpr\s+(?P<type>Point|GuidPoint|NotePoint)\s+(?P<name>k\w+)\s*\[\]\s*=\s*\{(?P<body>.*?)\};",
    re.S,
)
LAYER_ROW_RE = re.compile(
    r'\{\s*L"(?P<key>[^"]+)"\s*,\s*L"(?P<display>[^"]*)"\s*,\s*(?P<r>\d+)\s*,\s*(?P<g>\d+)\s*,\s*(?P<b>\d+)\s*,\s*(?P<a>\d+)\s*,'
    r'\s*(?P<arr>k\w+)\s*,\s*(?P<count>\d+)\s*,\s*(?P<on>true|false)\s*,\s*(?P<icon>nullptr|L"[^"]*")\s*\}'
)
SYMBOL_RE = re.compile(r'inline\s+constexpr\s+const\s+wchar_t\*\s+(k\w+)\s*=\s*(L"[^"]*")\s*;')
STRUCT_RE = re.compile(r"struct\s+(\w+)\s*\{[^}]*\};")


def parse(path):
    text = Path(path).read_text(encoding="utf-8")
    arrays = {}
    for m in ARRAY_RE.finditer(text):
        # Normalize whitespace only; keep element order (kLayers indexes into these).
        body = re.sub(r"\s+", "", m["body"])
        arrays[m["name"]] = (m["type"], body)

    layers_block = re.search(r"inline\s+constexpr\s+Layer\s+kLayers\[\]\s*=\s*\{(.*?)\};", text, re.S)
    layers = []
    if layers_block:
        for m in LAYER_ROW_RE.finditer(layers_block[1]):
            layers.append((m["key"], m["display"], (m["r"], m["g"], m["b"], m["a"]), m["arr"],
                           int(m["count"]), m["on"], m["icon"]))

    return {
        "arrays": arrays,
        "layers": layers,
        "symbols": dict(SYMBOL_RE.findall(text)),
        "structs": sorted(STRUCT_RE.findall(text)),
    }


def compare(a, b):
    ok = True

    # ---- structs
    if a["structs"] != b["structs"]:
        print(f"  FAIL structs: {a['structs']} != {b['structs']}")
        ok = False
    else:
        print(f"  ok   structs: {', '.join(a['structs'])}")

    # ---- arrays
    an, bn = set(a["arrays"]), set(b["arrays"])
    if an - bn:
        print(f"  FAIL arrays dropped by regen: {sorted(an - bn)}")
        ok = False
    if bn - an:
        print(f"  FAIL arrays added by regen: {sorted(bn - an)}")
        ok = False
    for name in sorted(an & bn):
        ta, ba = a["arrays"][name]
        tb, bb = b["arrays"][name]
        if ta != tb:
            print(f"  FAIL {name}: type {ta} != {tb}")
            ok = False
        elif ba != bb:
            print(f"  FAIL {name}: contents differ ({len(ba)} vs {len(bb)} chars)")
            ok = False
    if ok:
        print(f"  ok   arrays: {len(an)} arrays, contents identical")

    # ---- layer table (order IS semantic: panel_items() hardcodes indices)
    if a["layers"] != b["layers"]:
        print("  FAIL kLayers differs:")
        for i, (x, y) in enumerate(zip(a["layers"], b["layers"])):
            if x != y:
                print(f"       [{i}] {x}\n         -> {y}")
        if len(a["layers"]) != len(b["layers"]):
            print(f"       length {len(a['layers'])} != {len(b['layers'])}")
        ok = False
    else:
        print(f"  ok   kLayers: {len(a['layers'])} rows identical, order preserved")

    # ---- loose symbols
    if a["symbols"] != b["symbols"]:
        print(f"  FAIL symbols: {a['symbols']} != {b['symbols']}")
        ok = False
    else:
        print(f"  ok   symbols: {', '.join(sorted(a['symbols']))}")

    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("committed", nargs="?", default=str(HEADER))
    ap.add_argument("regenerated", nargs="?")
    ap.add_argument("--regen", action="store_true",
                    help="run the generator into a temp copy and compare against the committed header")
    args = ap.parse_args()

    if args.regen:
        # Regenerate in place, but restore the committed file no matter what.
        original = HEADER.read_bytes()
        try:
            subprocess.run([sys.executable, str(GEN)], check=True,
                           stdout=subprocess.DEVNULL, cwd=REPO)
            produced = HEADER.read_bytes()
        finally:
            HEADER.write_bytes(original)
        tmp = Path(tempfile.mkdtemp()) / "regen.hpp"
        tmp.write_bytes(produced)
        left, right = HEADER, tmp
    else:
        if not args.regenerated:
            sys.exit("need two files, or --regen")
        left, right = Path(args.committed), Path(args.regenerated)

    print(f"committed:   {left}")
    print(f"regenerated: {right}")
    a, b = parse(left), parse(right)

    identical = left.read_bytes() == right.read_bytes()
    print(f"\nbyte-identical: {identical}")
    if not identical:
        print("(expected while array emission order is being normalized; "
              "semantic equality is the gate)\n")

    ok = compare(a, b)
    print(f"\n{'PASS — semantically identical' if ok else 'FAIL — real data changed'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
