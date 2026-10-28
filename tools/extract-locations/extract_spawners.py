#!/usr/bin/env python3
"""Write a Locations JSON for placed spawner/actor classes, read straight from the pak.

Pak-native version of extract_locations.py: no repak unpack, no extracted tree on
disk. Walks every MainWorld_5 World Partition cell
(`Pal/Content/Pal/Maps/MainWorld_5/PL_MainWorld5/_Generated_/*.umap`, ~10k cells),
finds the actor exports whose class import matches one of the requested classes,
and takes the actor position from its root component in the cell's .uexp -- the
same rule extract_locations.py uses (DefaultSceneRoot child first, first plausible
FVector of its body). The .uexp is only read for cells that contain a match.

    python3 extract_spawners.py OUT.json CLASS [CLASS ...] [--raw RAW.json]

CLASS is the placed actor class, e.g. `BP_PalMapObjectSpawner_Sulfur`; the `_C`
suffix is optional and fnmatch wildcards work (`'BP_PalMapObjectSpawner_Lotus_*'`).
Directly placed nodes (no spawner) are matched the same way, e.g.
`BP_MapObject_DamagableRock0018`. Points from all classes are merged, rounded to
2 decimals, exact duplicates dropped, sorted -- the same output as gen_data.py.
`--raw` additionally writes the per-class points with their cell
(`{class: [[x, y, z, cell], ...]}`, the locations-raw.json shape).

`--min-z -15000` drops cave/dungeon interiors: they are instanced rooms at deep Z
(about -40k..-80k) and would draw as surface pins where nothing is. The surface ore
layers use exactly this cut (docs/SPEC-2.0.md §2.1); `--raw` keeps everything.

The pak and Oodle come from `--pak`/`--oodle` or the PAK/OODLE env vars (same as
extract_bosses.py); fetch the library with fetch_oodle.py. The pak is only read.
"""
import argparse
import collections
import fnmatch
import json
import os
import sys
import time
from pathlib import Path

import palpak
import uecell

CELLS = "Pal/Content/Pal/Maps/MainWorld_5/PL_MainWorld5/_Generated_/"
TYPE = "/Game/Mods/MapCollectablesMod/Data/PreloadedLocationsDataAssetType"
DEFAULT_PAK = os.path.expanduser(
    "~/.local/share/Steam/steamapps/common/Palworld/Pal/Content/Paks/Pal-Windows.pak")


def walk(pak, patterns):
    """{class: [(x, y, z, cell), ...]} for every placed actor matching patterns."""
    files = pak.directory_index().get(CELLS)
    if not files:
        sys.exit(f"error: no cells under {CELLS} -- wrong pak?")
    out = collections.defaultdict(list)
    stats = collections.Counter()
    for fn in sorted(files):
        if not fn.endswith(".umap"):
            continue
        stats["cells"] += 1
        d, header, imps, exps = uecell.parse(pak.read_payload(files[fn]))
        targets = {}
        for i, e in enumerate(exps):
            c = e["cls"]
            if c >= 0:
                continue
            cname = imps[-c - 1][1]
            if cname.endswith("_C") and any(fnmatch.fnmatchcase(cname[:-2], p) for p in patterns):
                targets[i + 1] = cname[:-2]
        if not targets:
            continue
        uexp = pak.read_payload(files[fn[:-len(".umap")] + ".uexp"])
        children = collections.defaultdict(list)
        for e in exps:
            if e["outer"] in targets:
                children[e["outer"]].append(e)
        for idx, cls in targets.items():
            pos = None
            for k in sorted(children.get(idx, []), key=lambda e: e["name"] != "DefaultSceneRoot"):
                pos = uecell.first_triple(uexp[k["off"] - header: k["off"] - header + k["size"]])
                if pos:
                    break
            if pos:
                out[cls].append((round(pos[0], 2), round(pos[1], 2), round(pos[2], 2), fn))
            else:
                stats["no_position"] += 1
    return out, stats


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", type=Path, help="Locations JSON to write")
    ap.add_argument("classes", nargs="+", help="placed actor class names (_C optional, wildcards ok)")
    ap.add_argument("--raw", type=Path, help="also write per-class points with their cell")
    ap.add_argument("--min-z", type=float, help="drop points below this Z (-15000 = surface only)")
    ap.add_argument("--pak", default=os.environ.get("PAK", DEFAULT_PAK))
    ap.add_argument("--oodle", default=os.environ.get("OODLE"),
                    help="liboo2corelinux64.so.9 (cells are Oodle-compressed)")
    args = ap.parse_args()
    if not args.oodle:
        sys.exit("error: need --oodle or OODLE=... (python3 fetch_oodle.py --out DIR)")

    patterns = [c[:-2] if c.endswith("_C") else c for c in args.classes]
    t = time.time()
    pak = palpak.Pak(args.pak, oodle=palpak.Oodle(args.oodle))
    raw, stats = walk(pak, patterns)
    print(f"{stats['cells']} cells walked in {time.time() - t:.1f}s")

    for p in patterns:
        if not any(fnmatch.fnmatchcase(c, p) for c in raw):
            print(f"warning: no placed actors match {p}")
    for cls in sorted(raw, key=lambda c: -len(raw[c])):
        print(f"{len(raw[cls]):6d}  {cls}")
    if stats["no_position"]:
        print(f"warning: {stats['no_position']} matching actors had no readable position")

    kept = [p[:3] for v in raw.values() for p in v if args.min_z is None or p[2] >= args.min_z]
    total = sum(len(v) for v in raw.values())
    if args.min_z is not None:
        print(f"{total - len(kept)} points below z {args.min_z:.0f} dropped (cave/dungeon interiors)")
    pts = sorted(set(kept))
    data = {"$type": TYPE, "Locations": [{"x": x, "y": y, "z": z} for x, y, z in pts]}
    args.out.write_text(json.dumps(data, indent=2))
    print(f"{len(pts)} unique points ({len(kept) - len(pts)} exact duplicates dropped) -> {args.out}")
    if pts:
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        print(f"bounds x {min(xs):.0f}..{max(xs):.0f}  y {min(ys):.0f}..{max(ys):.0f}")
    if args.raw:
        args.raw.write_text(json.dumps({c: [list(p) for p in v] for c, v in raw.items()}))
        print(f"raw -> {args.raw}")


if __name__ == "__main__":
    main()
