import sys, random, time
sys.path.insert(0, ".")
from palpak import Pak, Oodle

PAK = "/home/tehaon/.local/share/Steam/steamapps/common/Palworld/Pal/Content/Paks/Pal-Windows.pak"
SO  = sys.argv[1]

oodle = Oodle(SO)
print("  Oodle loaded OK")
p = Pak(PAK, oodle=oodle)
print(f"  pak v{p.version}  encrypted_index={p.encrypted_index}  entries={p.num_entries:,}")
print(f"  compression methods: {p.compression_methods}")
print(f"  index @ {p.index_offset:,}  size {p.index_size:,}")

t=time.time()
di = p.directory_index()
nfiles = sum(len(v) for v in di.values())
print(f"  FullDirectoryIndex: {len(di):,} dirs / {nfiles:,} files  ({time.time()-t:.1f}s)")
assert nfiles == p.num_entries, f"file count {nfiles} != {p.num_entries}"
print("  file count matches header -> index parse is correct")

# flatten
flat = [(d+f, off) for d, fs in di.items() for f, off in fs.items()]
random.seed(1234)
sample = random.sample(flat, 1000)

ok = fail = 0
errs = {}
comp = raw = 0
for name, off in sample:
    try:
        e = p.decode_entry(off)
        data = p.read_payload(off)     # verifies SHA1 internally
        if e.cmi: comp += 1
        else: raw += 1
        ok += 1
    except Exception as ex:
        fail += 1
        k = str(ex)[:70]
        errs[k] = errs.get(k, 0) + 1
        if fail <= 3: print(f"    FAIL {name}: {ex}")

print(f"\n  ==> {ok}/{len(sample)} SHA1-verified   ({comp} Oodle, {raw} stored)")
if errs:
    print("  error histogram:")
    for k,v in sorted(errs.items(), key=lambda x:-x[1])[:5]: print(f"    x{v}  {k}")
