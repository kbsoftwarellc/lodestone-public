#!/usr/bin/env python3
"""Generate cairn_data.hpp from the extracted location JSONs.

Single source of truth for layers (SPEC 7.1): this registry drives the C++
renderer and, later, the panel builder script.

LAYERS is APPEND-ONLY. panel_items() in dllmain.cpp indexes kLayers by hardcoded
position (12..15 are Chest/Junk/Outpost/FruitTree), so inserting a layer mid-list
silently repoints those rows at the wrong data. Add at the end, always.

Inputs that used to come from an uncommitted $SCRATCH dir now live in
data/codegen/, recovered from the shipped header by freeze_inputs.py. See that
script for why.
"""
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(REPO, "data/locations")
OUT = os.path.join(REPO, "cpp-mod/mods/CairnMap/src/cairn_data.hpp")
FROZEN = os.path.join(REPO, "data/codegen")

# Icon names are short object names; dllmain derives the package path from the
# name prefix, because the two icon families live in different directories:
#   T_itemicon_*  full-colour item art  -> /Game/Others/InventoryItemIcon/Texture/
#   T_icon_*      monochrome UI glyph   -> /Game/Pal/Texture/UI/IngameMenu/
# A glyph is tinted with the layer colour; item art is drawn untinted.
# key, json file, RGBA color, default enabled, in-game icon texture (or None)
LAYERS = [
    ("Coal",       "CoalLocations.json",         (0x20, 0x20, 0x20, 0xFF), True, "T_itemicon_Material_Coal"),
    ("Copper",     "CopperLocations.json",       (0xD9, 0x77, 0x2B, 0xFF), False, "T_itemicon_Material_CopperOre"),
    ("Quartz",     "QuartzLocations.json",       (0xE8, 0xE8, 0xF8, 0xFF), True, "T_itemicon_Material_Quartz"),
    ("Sulfur",     "SulfurLocations.json",       (0xD9, 0xC8, 0x2B, 0xFF), True, "T_itemicon_Material_Sulfur"),
    ("Hexolite",   "HexoliteLocations.json",     (0x39, 0xD1, 0xD1, 0xFF), True, "T_itemicon_Material_Sapphire"),
    ("Oil",        "OilLocations.json",          (0x14, 0x0A, 0x0A, 0xFF), True, "T_itemicon_Material_CrudeOil"),
    ("SkyOre",     "SkyIslandOreLocations.json", (0x40, 0xD9, 0xFF, 0xFF), True, "T_itemicon_Material_SkyIslandOre"),
    ("TreeOre",    "WorldTreeOreLocations.json", (0x4D, 0xFF, 0x66, 0xFF), True, "T_itemicon_Material_WorldTreeOre"),
    ("Magma",      "MagmaRockLocations.json",    (0xFF, 0x59, 0x1A, 0xFF), True, "T_itemicon_Material_Lava_Ancient"),
    ("NightStone", "NightStoneLocations.json",   (0xA6, 0x66, 0xFF, 0xFF), True, "T_itemicon_Material_NightStone"),
    ("DogCoin",    "DogCoinLocations.json",      (0xFF, 0xD9, 0x33, 0xD9), False, "T_itemicon_Material_DogCoin"),
    ("Lotus",      None,                         (0xFF, 0x73, 0xCC, 0xD9), False, "T_itemicon_Food_Lotus_hp_01"),
    # Appended 2026-07-13 upstream but never added here, so running this script
    # dropped them and left panel_items() indexing kLayers[12..15] out of bounds.
    # Colors/icons transcribed verbatim from the shipped header.
    # The icon is vanilla's own treasure glyph, not the KEY item art upstream used:
    # a key is what opens a chest, not what the layer marks. T_icon_compass_Search_*
    # is the pair of glyphs the search skills use for their two target kinds
    # (Treasure / Junk), so they depict the targets. Being T_icon_* it is a glyph and
    # gets tinted with the layer colour; see Style::icon_is_glyph.
    ("Chest",      "TreasureChestLocations.json", (0xFF, 0xC8, 0x3A, 0xFF), False, "T_icon_compass_Search_Treasure"),
    # Was T_itemicon_Food_BaconEggs -- upstream placeholder, and on screen it is
    # unmistakably a plate of bacon and eggs sitting in the legend next to "Junk".
    # T_icon_compass_Search_Junk is vanilla's own junk glyph, the exact sibling of
    # the Search_Treasure one the Chest row now uses.
    ("Junk",       "JunkLocations.json",         (0x9A, 0x8C, 0x6E, 0xFF), False, "T_icon_compass_Search_Junk"),
    ("Outpost",    "OutpostLocations.json",      (0xFF, 0x3C, 0x3C, 0xFF), True, "T_icon_camp"),
    ("FruitTree",  "FruitTreeLocations.json",    (0x5A, 0xDC, 0x5A, 0xFF), True, "T_itemicon_Consume_AffectionFruit_01"),
    # Appended 2026-07-17. Already extracted (upstream's gen_data.py writes it) but
    # never wired into a layer -- the JSON sat unused in data/locations/. Icon is
    # vanilla's raw-mushroom item art (verified present in the pak). Cave mushrooms
    # spawn underground, so default OFF like the other niche layers.
    ("CaveMushroom", "CaveMushroomLocations.json", (0xC8, 0x8A, 0x5A, 0xFF), False, "T_itemicon_Food_Mushroom"),
    # Appended 2026-07-17. Field bosses from DT_BossSpawnerLoactionData (not the
    # cells -- see tools/extract-locations/extract_bosses.py). 154 rows collapse to
    # ~122 pins after co-located tier variants dedupe. Both alpha Pals and humanoid
    # raid bosses; levels exist in the table but the Point schema is pins-only for
    # now. Icon is vanilla's compass boss glyph (T_icon_compass_*, so UI/InGame --
    # Style::icon_dir special-cases it); being a glyph it takes the layer colour.
    # Default off: specialised, and dense enough to clutter the default view.
    ("Boss",       "BossLocations.json",         (0xFF, 0x40, 0x30, 0xFF), False, "T_icon_compass_boss"),
    # Appended 2026-07-17. Materials re-extracted from the CURRENT pak (see below),
    # not frozen upstream data. The extractor was validated first: a fresh cell walk
    # reproduces 90-97% of every shipped ore layer's points at exact coords and adds
    # more (the game grew since CairnMap froze its data), so first_triple picks the
    # right FVector and these new-class coords are trustworthy by the same mechanism.
    #   Paldium   = BP_PalMapObjectSpawner_PalCrystal{,_Small}  (the "PalCrystal" node
    #               that drops Paldium Fragments; icon is the only PalCrystal item art).
    #   Red Berry = BP_PalMapObjectSpawner_RedBerry.
    #   Mushroom  = BP_PalMapObjectSpawner_{Mushroom,YakushimaMushroom_01,_02} (cave
    #               mushrooms are already their own layer above).
    #   Skill Fruit = every BP_PalMapObjectSpawner_SkillFruits_<Biome>; the item you
    #               harvest is a Skill Card, so that is the icon (no generic fruit art).
    # Iron is intentionally absent: BP_PalMapObjectSpawner_RockIron is only 8 nodes and
    # is not the generic Ore->Ingot source players mean by "iron"; that deposit class is
    # still unidentified. All default OFF -- dense (Paldium 2k, Berry ~2k) and niche.
    ("Paldium",    "PaldiumLocations.json",      (0x5A, 0x8C, 0xF0, 0xFF), False, "T_itemicon_Material_PalCrystal_Ex"),
    ("RedBerry",   "RedBerryLocations.json",     (0xE2, 0x3A, 0x3A, 0xFF), False, "T_itemicon_Food_Berries"),
    ("Mushroom",   "MushroomLocations.json",     (0xD0, 0x5A, 0x4A, 0xFF), False, "T_itemicon_Food_Mushroom"),
    ("SkillFruit", "SkillFruitLocations.json",   (0xB0, 0x60, 0xE0, 0xFF), False, "T_itemicon_Consume_SkillCard_Neutral"),
]
EFFIGY_ICON = "T_itemicon_Relic"
NOTE_ICON = "T_itemicon_Consume_TechnologyBook_G1"
EGG_ICON = "T_itemicon_Material_PalEgg"
BOSSES = [(-266563, 174506), (-361695, -112009), (81363, 90183), (29975, 413325),
          (-321596, 209085), (-778216, -36026), (-889805, -435828), (-29428, -115900)]


def load_points(fname):
    pts = json.load(open(os.path.join(DATA, fname)))["Locations"]
    return sorted({(round(p["x"]), round(p["y"])) for p in pts})


def load_lotus_frozen():
    """Lotus points as shipped.

    The onshore filter below needs locations-raw.json, which was never committed.
    Worse, the five per-stat Lotus*Locations.json files are all empty stubs: the
    placed classes are biome variants (Lotus_Grass01, _Desert01, ...) and the stat
    is rolled at runtime from DT_MapObjectLotteryDataTable, so per-stat locations
    do not exist statically. Hence one merged Lotus layer, frozen from the header.
    """
    return [tuple(p) for p in json.load(open(os.path.join(FROZEN, "lotus.json")))]


def load_lotus_from_raw(scratch):  # unused; kept for future re-extraction (Phase D)
    raw = json.load(open(os.path.join(scratch, "locations-raw.json")))
    towers = json.load(open(os.path.join(scratch, "towers.json")))
    ref = [(p[0], p[1]) for p in towers["BP_LevelObject_TowerFastTravelPoint"]]
    for f in ("EffigyLocations.json", "CoalLocations.json", "CopperLocations.json"):
        ref += [(p["x"], p["y"]) for p in json.load(open(os.path.join(DATA, f)))["Locations"]]
    cell = 25000
    grid = {}
    for x, y in ref:
        grid.setdefault((int(x // cell), int(y // cell)), []).append((x, y))

    def onshore(x, y):
        ci, cj = int(x // cell), int(y // cell)
        for i in range(ci - 1, ci + 2):
            for j in range(cj - 1, cj + 2):
                for rx, ry in grid.get((i, j), []):
                    if (rx - x) ** 2 + (ry - y) ** 2 < cell * cell:
                        return True
        return False

    pts = {(round(e[0]), round(e[1]))
           for c in raw if c.startswith("BP_PalMapObjectSpawner_Lotus_")
           for e in raw[c]}
    return sorted(p for p in pts if onshore(*p))


def main():
    towers = json.load(open(os.path.join(FROZEN, "towers.json")))
    statues = sorted((round(p[0]), round(p[1]))
                     for p in towers["BP_LevelObject_TowerFastTravelPoint"])

    out = []
    out.append("// Generated by tools/codegen/gen_cairn_data.py - DO NOT EDIT")
    out.append("#pragma once")
    out.append("#include <cstdint>")
    out.append("#include <cstddef>")
    out.append("")
    out.append("namespace CairnMap::Data {")
    out.append("struct Point { int32_t x, y; };")
    out.append("struct Layer { const wchar_t* key; uint8_t r, g, b, a; const Point* points; size_t count; bool default_on; const wchar_t* icon; };")
    out.append("struct GuidPoint { uint32_t guid[4]; int32_t x, y; };")
    out.append("struct NotePoint { const wchar_t* row; int32_t x, y; };")
    out.append("")

    def emit(name, pts):
        rows = ",".join(f"{{{x},{y}}}" for x, y in pts)
        out.append(f"inline constexpr Point k{name}[] = {{{rows}}};")

    emit("BossTowers", BOSSES)
    emit("Statues", statues)
    layer_rows = []
    for key, fname, (r, g, b, a), on, icon in LAYERS:
        pts = load_lotus_frozen() if fname is None else load_points(fname)
        emit(key, pts)
        icon_lit = f'L"{icon}"' if icon else "nullptr"
        layer_rows.append(
            f'    {{L"{key}", {r}, {g}, {b}, {a}, k{key}, {len(pts)}, {"true" if on else "false"}, {icon_lit}}},')
        print(f"{key:12s} {len(pts):5d} points")
    # effigies & notes (GUID + position, extraits des cellules L15)
    guids = json.load(open(os.path.join(FROZEN, "relic_guids.json")))
    def emit_guid(name, rows):
        body = ",".join("{{0x%s,0x%s,0x%s,0x%s},%d,%d}" %
                        (r[0][0:8], r[0][8:16], r[0][16:24], r[0][24:32], r[1], r[2]) for r in rows)
        out.append(f"inline constexpr GuidPoint k{name}[] = {{{body}}};")
    def emit_note(name, rows):
        body = ",".join('{L"%s",%d,%d}' % (r[0], r[1], r[2]) for r in rows)
        out.append(f"inline constexpr NotePoint k{name}[] = {{{body}}};")

    emit_guid("Effigies", guids["Relic"])
    notes = json.load(open(os.path.join(FROZEN, "notes.json")))
    emit_note("Notes", notes)
    out.append(f'inline constexpr const wchar_t* kEffigyIcon = L"{EFFIGY_ICON}";')
    out.append(f'inline constexpr const wchar_t* kNoteIcon = L"{NOTE_ICON}";')
    out.append(f'inline constexpr const wchar_t* kEggIcon = L"{EGG_ICON}";')
    print(f"effigies {len(guids['Relic'])}, notes {len(notes)}")
    out.append("")
    out.append("inline constexpr Layer kLayers[] = {")
    out.extend(layer_rows)
    out.append("};")
    out.append("} // namespace CairnMap::Data")
    open(OUT, "w").write("\n".join(out) + "\n")
    print("->", OUT)


main()
