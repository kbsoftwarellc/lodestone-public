#!/usr/bin/env python3
"""Recover gen_cairn_data.py's missing inputs from the committed header.

gen_cairn_data.py reads three files from a $SCRATCH dir -- towers.json,
relic_guids.json, locations-raw.json -- that were never committed, and no
committed script produces them. The effigy/note GUID extraction ("extraits des
cellules L15") does not exist in the repo at all. So the generator cannot be run,
which is how it silently drifted 12-vs-16 layers behind the header it generates.

Rather than reconstruct an extraction pipeline we don't have, invert the problem:
cairn_data.hpp is the artifact those inputs produced, and it *is* committed. Parse
it back into the inputs. The generator becomes runnable and hermetic, and the data
it emits is by construction the data that's shipping.

This is a one-shot bootstrap, not part of the build. Once data/codegen/*.json are
committed, this script's job is done. When a layer is genuinely re-extracted from
game files (Phase D), its frozen file gets replaced by real pipeline output.

    python3 tools/codegen/freeze_inputs.py [--check]

--check re-derives and compares against the committed frozen inputs without
writing, so CI can prove they still match the header.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "cpp-mod/mods/CairnMap/src/cairn_data.hpp"
OUT_DIR = REPO / "data/codegen"

POINT_ARRAY_RE = re.compile(r"inline\s+constexpr\s+Point\s+(k\w+)\s*\[\]\s*=\s*\{(.*?)\};", re.S)
GUID_ARRAY_RE = re.compile(r"inline\s+constexpr\s+GuidPoint\s+(k\w+)\s*\[\]\s*=\s*\{(.*?)\};", re.S)
NOTE_ARRAY_RE = re.compile(r"inline\s+constexpr\s+NotePoint\s+(k\w+)\s*\[\]\s*=\s*\{(.*?)\};", re.S)

POINT_RE = re.compile(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}")
GUID_ROW_RE = re.compile(
    r"\{\s*\{\s*0[xX]([0-9a-fA-F]{8})\s*,\s*0[xX]([0-9a-fA-F]{8})\s*,"
    r"\s*0[xX]([0-9a-fA-F]{8})\s*,\s*0[xX]([0-9a-fA-F]{8})\s*\}\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}"
)
NOTE_ROW_RE = re.compile(r'\{\s*L"([^"]*)"\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}')


def parse_header(text):
    points = {m[0]: POINT_RE.findall(m[1]) for m in POINT_ARRAY_RE.findall(text)}
    guids = {m[0]: GUID_ROW_RE.findall(m[1]) for m in GUID_ARRAY_RE.findall(text)}
    notes = {m[0]: NOTE_ROW_RE.findall(m[1]) for m in NOTE_ARRAY_RE.findall(text)}
    return points, guids, notes


def build(text):
    points, guids, notes = parse_header(text)

    for need, where in (("kStatues", points), ("kLotus", points),
                        ("kEffigies", guids), ("kNotes", notes)):
        if need not in where or not where[need]:
            sys.exit(f"error: {need} missing or empty in {HEADER} — cannot freeze")

    out = {}

    # towers.json: gen_cairn_data.py reads towers["BP_LevelObject_TowerFastTravelPoint"]
    # and sorts it into kStatues. Round-trips as [x, y] pairs.
    out["towers.json"] = {
        "BP_LevelObject_TowerFastTravelPoint": [[int(x), int(y)] for x, y in points["kStatues"]]
    }

    # relic_guids.json: emit_guid slices a flat 32-char hex string into four
    # uint32s, so store it in exactly that shape. Uppercase to match the header.
    out["relic_guids.json"] = {
        "Relic": [["".join(g).upper(), int(x), int(y)] for *g, x, y in guids["kEffigies"]]
    }

    # notes.json: notes are NOT GuidPoints. The header keys them by row name
    # (L"Day-xx"), which is what the collected-state mask joins on. The old
    # generator emitted them via emit_guid as GuidPoint -- a hard compile error
    # against the header's NotePoint, and the reason parity was never reachable.
    out["notes.json"] = [[row, int(x), int(y)] for row, x, y in notes["kNotes"]]

    # lotus.json: load_lotus() filtered raw spawners to onshore points using
    # locations-raw.json, which isn't committed. Freeze its *output*; the filter
    # stays in the generator as documented dead code for future re-extraction.
    out["lotus.json"] = [[int(x), int(y)] for x, y in points["kLotus"]]

    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="compare against committed frozen inputs instead of writing")
    args = ap.parse_args()

    built = build(HEADER.read_text(encoding="utf-8"))
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    bad = False
    for name, obj in built.items():
        path = OUT_DIR / name
        blob = json.dumps(obj, indent=1) + "\n"
        n = len(obj if isinstance(obj, list) else next(iter(obj.values())))
        if args.check:
            if not path.exists():
                print(f"  MISSING {path}")
                bad = True
            elif path.read_text() != blob:
                print(f"  DRIFT   {path} no longer matches the header")
                bad = True
            else:
                print(f"  ok      {name:18s} {n:5d} rows")
        else:
            path.write_text(blob)
            print(f"  wrote   {name:18s} {n:5d} rows -> {path.relative_to(REPO)}")

    if args.check and bad:
        sys.exit("frozen inputs do not match the committed header")
    return 0


if __name__ == "__main__":
    sys.exit(main())
