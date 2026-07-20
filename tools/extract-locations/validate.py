import json, math, os
MOD='/home/lfievet/dev/MapCollectablesMod-Rework/Content/Mods/MapCollectablesMod/Data'
raw=json.load(open(os.environ['SCRATCH']+'/locations-raw.json'))
def nn(modfile, cls):
    mod=[(p['x'],p['y'],p['z']) for p in json.load(open(f'{MOD}/{modfile}'))['Locations']]
    mine=[(e[0],e[1],e[2]) for e in raw.get(cls,[])]
    if not mine: return f"{modfile:26s} -> {cls}: NO positions extracted"
    dists=[]
    for m in mod:
        best=min(math.dist(m,x) for x in mine)
        dists.append(best)
    dists.sort()
    med=dists[len(dists)//2]
    within100=sum(1 for d in dists if d<100)/len(dists)*100
    return f"{modfile:26s} ({len(mod):4d} pts) vs {cls} ({len(mine):4d}): median={med:8.1f}u  <100u: {within100:5.1f}%  max={dists[-1]:9.1f}"
print(nn('CoalLocations.json','BP_PalMapObjectSpawner_RockCoal'))
print(nn('CopperLocations.json','BP_PalMapObjectSpawner_RockCopper'))
print(nn('QuartzLocations.json','BP_PalMapObjectSpawner_RockQuartz'))
print(nn('SulfurLocations.json','BP_PalMapObjectSpawner_Sulfur'))
print(nn('HexoliteLocations.json','BP_PalMapObjectSpawner_Crystal'))
print(nn('OilLocations.json','BP_LevelObject_OilField'))
