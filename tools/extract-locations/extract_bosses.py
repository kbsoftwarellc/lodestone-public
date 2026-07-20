#!/usr/bin/env python3
"""Extract field-boss locations from DT_BossSpawnerLoactionData into a Locations JSON.

Unlike the ore/collectable layers, bosses are not placed as actors in the World
Partition cells -- they live in a single UI DataTable,
`Pal/Content/Pal/DataTable/UI/DT_BossSpawnerLoactionData` (the shipped typo
"Loaction" is real; match it). One row per field boss:
`FPalUIBossSpawnerLoactionData {FName SpawnerID, FName CharacterID, FVector
Location, int32 Level}` (reflection dump, Pal.hpp:8238).

The table is serialized with UNVERSIONED property serialization, so there are no
per-property name tags to walk -- the bytes are just the values in schema order,
behind a small header. Rather than parse the unversioned header (which needs the
.usmap schema), anchor on the one field that is unmistakable: the FVector, three
little-endian doubles in world-coordinate range. The struct layout then fixes the
rest deterministically:

    [ ... ][CharacterID FName 8B][Location 3xf64 = 24B][Level i32 = 4B]

CharacterID is the 8 bytes immediately before the vector; Level the int32
immediately after it. Verified: every vector so found has a boss CharacterID at
-8 and a Level in [1,80] at +24 (two row strides, 54B with the optional
SpawnerID present and 47B without -- CharacterID/Location/Level are always
present, which is why anchoring on them is safe).

Only non-boss rows dropped: three `REGION_Oilrig` markers and two null rows. Both
`BOSS_*` (field alpha Pals + humanoid raid bosses) and the single mixed-case
`Boss_Anubis` are kept -- everything that is a real boss spawn.

Levels are read and reported but not written to the layer JSON: the layer is
pins-only for now (the Point schema carries no level). They are here so a future
level-aware layer does not need to re-derive the parse.

    PAK=/path/to/Pal-Windows.pak OODLE=/path/to/liboo2corelinux64.so.9 \
        python3 extract_bosses.py

Writes ../../data/locations/BossLocations.json relative to this file.
"""
import json
import os
import struct
import sys
from pathlib import Path

import palpak
import uecell

HERE = Path(__file__).resolve().parent
OUT = HERE.parents[1] / "data/locations/BossLocations.json"
TABLE = "Pal/Content/Pal/DataTable/UI/DT_BossSpawnerLoactionData"
TYPE = "/Game/Mods/MapCollectablesMod/Data/PreloadedLocationsDataAssetType"


def load_names(uasset: bytes):
    i32 = lambda o: struct.unpack_from("<i", uasset, o)[0]
    cv = i32(24)
    o = 28 + cv * 20
    o += 4                      # legacy/header int
    length = i32(o)
    o += 4 + length             # package name string
    o += 4                      # flags
    nc, no = i32(o), i32(o + 4)
    return uecell.parse_names(uasset, nc, no), nc


def extract(pak: palpak.Pak):
    di = pak.directory_index()
    flat = {d + f: off for d, files in di.items() for f, off in files.items()}
    ua = pak.read_payload(flat[TABLE + ".uasset"])
    ux = pak.read_payload(flat[TABLE + ".uexp"])
    names, nc = load_names(ua)

    # Locate the DataTable export body inside the .uexp (SerialOffset - header).
    tmp = HERE / "_boss_tmp.uasset"
    tmp.write_bytes(ua)
    try:
        _, header, _, exps = uecell.parse(str(tmp))
    finally:
        tmp.unlink(missing_ok=True)
    e = exps[0]
    b = ux[e["off"] - header: e["off"] - header + e["size"]]

    def fname(off):
        if off < 0 or off + 8 > len(b):
            return None
        idx, num = struct.unpack_from("<i", b, off)[0], struct.unpack_from("<i", b, off + 4)[0]
        return names[idx] if (0 <= idx < nc and 0 <= num < (1 << 20)) else None

    rows, last = [], -100
    for off in range(0, len(b) - 24):
        v = struct.unpack_from("<3d", b, off)
        if all(abs(x) < 2e6 and (x == 0 or abs(x) > 1e-2) for x in v) and any(x != 0 for x in v):
            if off - last < 24:
                continue
            last = off
            char = fname(off - 8)
            level = struct.unpack_from("<i", b, off + 24)[0]
            rows.append((char, v[0], v[1], v[2], level))
    return rows


def main():
    pak_path = os.environ.get(
        "PAK", os.path.expanduser(
            "~/.local/share/Steam/steamapps/common/Palworld/Pal/Content/Paks/Pal-Windows.pak"))
    oodle = os.environ.get("OODLE")
    pak = palpak.Pak(pak_path, oodle=palpak.Oodle(oodle) if oodle else None)
    rows = extract(pak)

    bosses = [r for r in rows if r[0] and not r[0].startswith("REGION_")]
    dropped = [r for r in rows if not (r[0] and not r[0].startswith("REGION_"))]
    bad = [r for r in bosses if not (1 <= r[4] <= 80)]
    if bad:
        sys.exit(f"error: {len(bad)} boss rows have out-of-range levels -- parse is off: {bad[:3]}")

    pts = sorted({(round(x), round(y), round(z)) for _, x, y, z, _ in bosses})
    data = {"$type": TYPE, "Locations": [{"x": x, "y": y, "z": z} for x, y, z in pts]}
    OUT.write_text(json.dumps(data, indent=2))
    print(f"{len(bosses)} bosses ({len(pts)} unique points), dropped {len(dropped)} non-boss rows")
    print(f"level range {min(r[4] for r in bosses)}-{max(r[4] for r in bosses)}")
    print("->", OUT)


if __name__ == "__main__":
    main()
