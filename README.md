# Lodestone

Resources, collectables and points of interest on the **Palworld 1.0+** world map, as
toggleable layers. Windows and Linux/Proton.

By **tehAon**. Built on [CairnMap](https://github.com/Pixnop/CairnMap) by Pixnop (MIT) —
see [NOTICE](NOTICE) for exactly what was imported and why.

## Why

Two reasons, both about trust:

1. **Every binary is built by CI, from source, in the open.** A mod DLL runs inside your
   game with your privileges. Here GitHub Actions builds the *complete* installable folder
   — DLL, `Info.json`, thumbnail — and a release is that artifact, unmodified. Nothing is
   assembled off-machine, so every shipped byte traces to a commit.
2. **The build is hermetic.** Every input is pinned to an immutable commit SHA, and the one
   binary the build downloads is SHA256-asserted. Nothing can move under you between builds.

Plus more layers, and the map is readable out of the box — see [Roadmap](#roadmap).

## Data provenance

All coordinates are datamined from Palworld's own cooked level data, read straight out of
`Pal-Windows.pak`. **Nothing is scraped from community map sites.** Those sites' curated
datasets are theirs; copying them would be substantial extraction regardless of what a
robots.txt does or doesn't say. The game files are the source those sites derive from
anyway — and they hold more.

## Install

Requires [UE4SS (Okaetsu `experimental-palworld` fork)](https://github.com/Okaetsu/RE-UE4SS/releases/tag/experimental-palworld).

1. Download `Lodestone-vX.Y.Z.zip` from Releases (or a CI run's artifacts).
2. Extract the `Lodestone/` folder into `Pal/Binaries/Win64/ue4ss/Mods/`.
3. Add `Lodestone : 1` to `Pal/Binaries/Win64/ue4ss/Mods/mods.txt`, above the `Keybinds`
   line (its comment asks to stay last).
4. Launch, open the world map — the layer panel appears.

Your layer toggles and dot sizing are saved to `Mods/Lodestone/settings.txt` and restored
next launch. Delete that file to restore defaults.

### Linux / Proton

UE4SS needs this launch option, set via the Steam UI (editing `localconfig.vdf` while Steam
is running gets overwritten):

```
WINEDLLOVERRIDES="dwmapi=n,b" %command%
```

Verify it loaded in `Pal/Binaries/Win64/ue4ss/UE4SS.log`:

```
[Lodestone] loaded (P1)
[Lodestone] Unreal initialized
[Lodestone] calibrated: seed ...px refine ...px (N anchors)
[Lodestone] N dots placed, M collected hidden (pool P) in Tms
```

## Verifying a release yourself

You don't have to take our word for any of it:

```bash
gh run download <run-id> -R kbsoftwarellc/lodestone -n Lodestone-dist
python3 tools/audit/compare_dll.py <downloaded>/dlls/main.dll \
  --header cpp-mod/mods/CairnMap/src/cairn_data.hpp
```

Two DLLs built from identical source never byte-match — PE timestamps, RSDS GUIDs, runner
toolchain drift — so diffing binaries proves nothing. `compare_dll.py` checks what actually
matters: it regenerates every embedded coordinate array from the source header, locates
those exact bytes in `.rdata`, and compares the import and export tables. Matching data +
imports limited to UE4SS/CRT/kernel32 + exports of exactly `start_mod`/`uninstall_mod`
means the binary is what the source says it is. CI runs this on every build.

The source audit is one grep. No networking, no registry, no process spawning:

```bash
grep -rEi "socket|WinHttp|WinInet|curl|https?://|CreateProcess|ShellExecute|RegOpenKey|LoadLibrary|GetProcAddress|WriteProcessMemory" cpp-mod/mods/
```

It touches the disk in exactly one place: `Mods/Lodestone/settings.txt`, to remember your
layer toggles and dot sizing. That's the only file it opens and the only reason it calls
`kernel32` (`GetModuleFileNameW`, to locate its own mod folder).

## Build

CI does it; no Windows toolchain needed:

```bash
gh workflow run build-cpp-mod -R kbsoftwarellc/lodestone
```

The engine-free projection core builds and tests natively:

```bash
g++ -std=c++20 -O2 -I cpp-mod/mods/CairnMap/src cpp-mod/tests/test_project.cpp -o /tmp/tp && /tmp/tp
```

Regenerating the embedded data (`data/locations/*.json` → `cairn_data.hpp`):

```bash
python3 tools/codegen/gen_cairn_data.py
python3 tools/codegen/verify_parity.py --regen   # must be byte-identical
```

`LAYERS` in the codegen is **append-only** — `panel_items()` indexes `kLayers` by hardcoded
position, so inserting mid-list silently repoints rows at the wrong data.

## Layers

On by default (2,525 dots — sparse and readable). Dense layers ship off; toggle them on as
needed and it'll remember.

| Group | On by default | Off by default |
|---|---|---|
| Collectables | Effigies (407), Notes (64), Eggs (live, nearby) | — |
| Ores | Coal (496), Quartz (523), Sulfur (279), Hexolite (349), Sky Ore (208), World Tree Ore (80), Magma (10), Night Stone (271) | Copper (1,396) |
| Resources | Oil (185), Fruit Tree (65) | Lotus (700), Dog Coin (128) |
| Points of Interest | Outposts (59) | Chests (1,559), Junk (670) |

## Roadmap

- **Hardwood / gathering trees** — the question that started this. Hardwood drops from
  specific tree meshes, not resource nodes, so no existing mod maps it.
- **POIs** — Fast Travel, Dungeons, Alphas, Sealed Realms, Towers, Bounties, live-queried
  via `UPalLocationManager` (confirmed present in-game), needing no embedded data.
- **Pal spawns + level ranges** — positions embedded, Pal IDs and levels read live so they
  survive balance patches.
- **Iron, Paldium, Red Berries, NPCs/Merchants.**
- **Perf** — per-layer dot ranges and lazy creation, before any mass layer lands.

## License

MIT — see [LICENSE](LICENSE) (Pixnop's, retained verbatim as MIT requires) and [NOTICE](NOTICE).

By **tehAon**. MIT licensed.
