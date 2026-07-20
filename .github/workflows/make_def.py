"""Parse `dumpbin /exports` output into a module-definition file."""
import re
import sys

lines = open(sys.argv[1], errors="ignore").read().splitlines()
syms, started = [], False
for l in lines:
    if re.match(r"\s+ordinal\s+hint", l):
        started = True
        continue
    if started:
        m = re.match(r"\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)", l)
        if m:
            syms.append(m.group(1))
with open(sys.argv[2], "w") as f:
    f.write("LIBRARY UE4SS.dll\nEXPORTS\n")
    for s in syms:
        f.write(f"    {s}\n")
print("symbols:", len(syms))
