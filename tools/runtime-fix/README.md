# MapCollectablesFix — UE4SS runtime fix for Palworld 1.0

Standalone UE4SS Lua mod that makes Map Collectables Helper (the 5.1 cooked pak)
fully work on Palworld 1.0, **fully automatically**: icons repair themselves
whenever the map opens or a filter checkbox changes. No keys needed.

Root cause of the 1.0 breakage: the mod's Blueprint looks up the map's icon
canvas by child index, and 1.0 inserted new children into `WBP_Map_Body`
(`WBP_SkyIslandCloud`, `Image_MapMask`, `Canvas_ForIcon_NoMask`,
`Canvas_ForIcon_Priority`), so the mod's icon widgets were created but never
added to the visual tree.

## Install

1. Map Collectables Helper pak in `Pal/Content/Paks/LogicMods/` (Workshop item
   3704720562 or Nexus), with RE-UE4SS experimental-palworld (1.0 build) and
   `BPModLoaderMod : 1`.
2. Copy this folder to `<UE4SS>/Mods/MapCollectablesFix/` (so the script is at
   `Mods/MapCollectablesFix/scripts/main.lua`).
3. Add `MapCollectablesFix : 1` to `<UE4SS>/Mods/mods.txt`.

## What it does

- **Auto-repair**: a 0.8 s watcher detects map opens (the mod recreates its
  widgets each time) and checkbox changes, then re-attaches every icon to
  `Canvas_ForIcon_Mask`. All widget work runs on the game thread
  (`ExecuteInGameThread`): LoopAsync callbacks are off-thread and racing the
  engine crashes intermittently. Repairs are debounced until the mod has
  finished populating its arrays, and every attached icon is detached again
  the moment the map closes so the game always gets its vanilla canvas back.
- **Exact projection**, cached after first calibration: seeded by matching the
  8 boss towers (world positions embedded, extracted from the 1.0 pak) to the 8
  `WBP_Map_IconTower_C` pins (farthest-pair, 4 orientation hypotheses, 0 px
  residual), refined by one matched least-squares pass over the ~150
  `WBP_Map_IconFTTower_C` statue pins (0.3 px). Axis form:
  slotX = a·worldY + b, slotY = c·worldX + d.
- **Fresh 1.0 data**: stale pre-1.0 resource entries snap to embedded
  surface-only 1.0 node positions via a spatial hash (10 km buckets); nodes
  removed by the 1.0 update are hidden.
- **Live collected state**: effigies/notes read `bPickedInClient` from their
  actor, so collected ones (including from old saves) disappear on refresh,
  honoring the "include collected" checkboxes.

**New 1.0 layers**: the fix also renders flat tinted dots (one shared texture,
negligible cost) for content the original mod predates: Sky Island ore (cyan),
World Tree ore (green), magma rock (orange), NightStone (violet), and the
underground collectables DogCoin (gold) and Lotus flowers (pink), drawn at
their surface X/Y to mark the cave to enter. Layer toggles are real checkbox rows injected at the bottom of the mod's own
map panel (label + color dot + checkbox). The checkbox style is copied from an
existing row: the default UMG style ships with empty brushes (a 0-px,
unclickable widget). States persist to `state.lua` next to the script.
Positions come from the offline pak extraction.

Key: **F7** force refresh + recalibrate.

## Known limits / notes

- Cave interiors sit on an instanced grid (deep Z): underground nodes
  (all Lotus flowers, DogCoins, most cave mushrooms, ~half the ore) are
  excluded from embedded data; showing them at cave entrances is future work.
- The pak leaks ~3.6k orphan widgets per map open (pre-existing mod behavior);
  harmless short-term, fixed properly by the upcoming Blueprint port.
- Hot-reloading UE4SS Lua (Ctrl+R) while widgets are being torn down can crash
  the game: restart instead after editing the script.
