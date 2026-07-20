import json, os, collections
MOD='/home/lfievet/dev/MapCollectablesMod-Rework/Content/Mods/MapCollectablesMod/Data'
raw=json.load(open(os.environ['SCRATCH']+'/locations-raw.json'))
TYPE="/Game/Mods/MapCollectablesMod/Data/PreloadedLocationsDataAssetType"

def collect(classes):
    pts=set()
    for c in classes:
        for e in raw.get(c,[]): pts.add((e[0],e[1],e[2]))
    return sorted(pts)

def write(fname, classes):
    pts=collect(classes)
    data={"$type":TYPE, "Locations":[{"x":x,"y":y,"z":z} for x,y,z in pts]}
    json.dump(data, open(f'{MOD}/{fname}','w'), indent=2)
    print(f"{fname:32s} {len(pts):5d} positions")

S='BP_PalMapObjectSpawner_'
# Updated (same files as the existing ones)
write('CoalLocations.json',[S+'RockCoal'])
write('CopperLocations.json',[S+'RockCopper'])
write('QuartzLocations.json',[S+'RockQuartz'])
write('SulfurLocations.json',[S+'Sulfur'])
write('HexoliteLocations.json',[S+'Crystal'])
write('OilLocations.json',['BP_LevelObject_OilField'])
# New (1.0 structures)
write('SkyIslandOreLocations.json',[S+'SkyIslandOre'])
write('WorldTreeOreLocations.json',[S+'WorldTreeOre'])
write('MagmaRockLocations.json',[S+'DamagableRock0022'])
write('NightStoneLocations.json',[S+'NightStone'])
write('DogCoinLocations.json',[S+'DogCoin'])
lotus=[c for c in raw if c.startswith(S+'Lotus_')]
for stat in ['HP','Attack','Stamina','Weight','Workspeed']:
    write(f'Lotus{stat}Locations.json',[c for c in lotus if f'_{stat}_' in c])
write('CaveMushroomLocations.json',[S+'CaveMushroom'])
write('JunkLocations.json',[c for c in raw if c.startswith(S+'Junk_')])
write('TreasureChestLocations.json',[c for c in raw if c.startswith(S+'Treasure_')])
