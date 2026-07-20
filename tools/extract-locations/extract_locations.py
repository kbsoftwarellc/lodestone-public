import os, sys, json, struct, collections
sys.path.insert(0, os.environ['SCRATCH'])
from uecell import parse

W = os.environ['SCRATCH'] + '/world'
PREFIXES = ('BP_PalMapObjectSpawner_', 'BP_LevelObject_OilField', 'BP_MapObject_DamagableRock')

def first_triple(blob):
    for o in range(0, len(blob)-23):
        v = struct.unpack_from('<3d', blob, o)
        if all(abs(x) < 2e6 and (x == 0 or abs(x) > 1e-3) for x in v) and any(x != 0 for x in v):
            return v
    return None

out = collections.defaultdict(list)
errors = 0
nparsed = 0
for root, _, files in os.walk(W):
    for fn in files:
        if not fn.endswith('.umap'): continue
        p = os.path.join(root, fn)
        try:
            d, header, imps, exps = parse(p)
        except Exception:
            errors += 1; continue
        nparsed += 1
        # target exports
        targets = {}
        for i, e in enumerate(exps):
            c = e['cls']
            cname = imps[-c-1][1] if c < 0 else None
            if cname and cname.endswith('_C') and cname[:-2].startswith(PREFIXES):
                targets[i+1] = cname[:-2]
        if not targets: continue
        uexp = None
        # child components
        children = collections.defaultdict(list)
        for i, e in enumerate(exps):
            if e['outer'] in targets:
                children[e['outer']].append(e)
        for idx, cls in targets.items():
            pos = None
            kids = sorted(children.get(idx, []), key=lambda e: (e['name'] != 'DefaultSceneRoot',))
            for k in kids:
                if uexp is None:
                    uexp = open(p.replace('.umap','.uexp'),'rb').read()
                blob = uexp[k['off']-header : k['off']-header+k['size']]
                pos = first_triple(blob)
                if pos: break
            if pos:
                out[cls].append([round(pos[0],2), round(pos[1],2), round(pos[2],2), os.path.basename(p)])
print(f"{nparsed} cells parsed, {errors} errors")
for cls in sorted(out, key=lambda c: -len(out[c])):
    print(f"{len(out[cls]):6d}  {cls}")
json.dump(out, open(os.environ['SCRATCH']+'/locations-raw.json','w'))
