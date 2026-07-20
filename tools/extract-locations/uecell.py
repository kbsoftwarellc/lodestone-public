import struct

def parse_names(d, count, off):
    names, o = [], off
    for _ in range(count):
        l = struct.unpack_from('<i', d, o)[0]; o += 4
        if l >= 0:
            names.append(d[o:o+l-1].decode('utf8','replace')); o += l
        else:
            names.append(d[o:o-l*2-2].decode('utf-16-le','replace')); o += -l*2
        o += 4
    return names

def parse(pumap):
    d = open(pumap, 'rb').read()
    i32 = lambda o: struct.unpack_from('<i', d, o)[0]
    i64 = lambda o: struct.unpack_from('<q', d, o)[0]
    assert struct.unpack_from('<I', d, 0)[0] == 0x9E2A83C1
    cv = i32(24); o = 28 + cv*20
    header = i32(o); o += 4
    l = i32(o); o += 4 + l  # package name
    o += 4  # flags
    namecount, nameoff = i32(o), i32(o+4); o += 8
    o += 8  # soft paths
    o += 8  # gatherable
    expcount, expoff = i32(o), i32(o+4); o += 8
    impcount, impoff = i32(o), i32(o+4)
    names = parse_names(d, namecount, nameoff)
    imports = []
    for i in range(impcount):
        b = impoff + i*32
        imports.append(names[i32(b+8)])  # ClassName ... wait: ClassPackage(0,4) ClassName(8,12) Outer(16) ObjectName(20,24)
    # fix: ObjectName at 20
    impnames = []
    for i in range(impcount):
        b = impoff + i*32
        impnames.append((names[i32(b+8)], names[i32(b+20)]))  # (ClassName, ObjectName)
    exports = []
    for i in range(expcount):
        b = expoff + i*96
        exports.append(dict(cls=i32(b), outer=i32(b+12), name=names[i32(b+16)],
                            num=i32(b+20), size=i64(b+28), off=i64(b+36)))
    return d, header, impnames, exports

def clsname(e, impnames):
    c = e['cls']
    if c < 0: return impnames[-c-1][1]
    return f"export#{c}"
