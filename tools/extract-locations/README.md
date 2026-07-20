# Offline resource location extraction (Palworld 1.0+)

Regenerates the mod's `Data/*Locations.json` files straight from the game pak,
without any in-game tracking. Validated against miapuffia's hand-tracked data:
median nearest-neighbor distance 0.0 units.

## Procedure

1. List and extract the world cells with [repak](https://github.com/trumank/repak):
   ```
   repak unpack --output world \
     --include 'Pal/Content/Pal/Maps/MainWorld_5/**' \
     ".../Palworld/Pal/Content/Paks/Pal-Windows.pak"
   ```
2. `SCRATCH=$PWD python3 extract_locations.py` (expects `$SCRATCH/world`,
   produces `$SCRATCH/locations-raw.json`): parses every World Partition cell
   (`_Generated_/*.umap`), finds the `BP_PalMapObjectSpawner_*`,
   `BP_LevelObject_OilField`, etc. actor exports, and reads the root component's
   position (3 doubles) from the `.uexp`.
3. `SCRATCH=$PWD python3 gen_data.py`: regenerates the JSONs in
   `Content/Mods/MapCollectablesMod/Data/`.
4. `SCRATCH=$PWD python3 validate.py`: consistency check against the previous
   files (nearest-neighbor distances).

`uecell.py` is the minimal UE package parser (summary/names/imports/exports),
calibrated for Palworld's cooked UE 5.1 legacy packages (non-IoStore).

Useful mappings: `Crystal` spawner = hexolite quartz (rock 0019),
rock 0020 = Sky Island ore, 0021 = World Tree ore, 0022 = magma rock.
Treasure map points (`PalTreasureMapPoint`) are dynamic spawns: no static
locations to extract.
