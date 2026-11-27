# Lodestone changelog

Every build handed to anyone gets a version. It shows in the map panel title
("Lodestone 0.3.0") and in `UE4SS.log` (`[Lodestone] v0.3.0 loaded`). Bump
`LODESTONE_VERSION` in `dllmain.cpp`, `mod/Info.json` and this file together.

## 0.3.0 — 2026-11-27 (commit c26b5f5, DLL sha256 d624b0ac…)
- Track a Pal, live: when a wild one of the tracked species is near, the compass
  points at the real Pal. A marked spawn spot you reach (50 m) with none there is
  hidden for 10 minutes and the next nearest takes over.
- In-world marker: an icon on screen right over the tracked Pal (or its spawn spot)
  and the nearest effigy, height included, with name, distance and "up/down".
  `WorldMarker=0` turns it off; `WorldMarkerRange` (metres, default 300).
- Effigy heights read from the game pak (406 of 407).
- Version number in the panel title and the log.

## 0.2.0 — 2026-11-27 (commit 1733599, DLL sha256 33e682fa…)
Shared as `Lodestone-latest-2026-11-27.zip` before versions were shown in game.
- World Tree: its points draw on the World Tree map, not off the main map's
  top-left edge.
- Track a Pal: marks the spawn areas where it is likeliest (was 0 points for Mimog).
- Pal picker shows real names (Mimog, not MimicDog).
- Boss dungeons, cave sites, treasure spots, map towers and NPC layers; readable
  compass icons; perf and stability work since 0.1.0.

## 0.1.0
First release.
