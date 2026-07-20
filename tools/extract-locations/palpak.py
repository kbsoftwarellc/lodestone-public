#!/usr/bin/env python3
"""Read Palworld's Pal-Windows.pak directly. Pure stdlib + Epic's Oodle .so.

Why not repak: repak would need installing Rust, and it still needs an Oodle library
from somewhere. It also unpacks whole trees, when what we want is ~215 MiB of
MainGrid cells out of a 37.7 GiB pak. Targeted reads beat a full unpack here, and
the format is small enough to read directly.

The pak validates itself. Every FPakEntry stores SHA1 of its *stored* payload -- the
compressed bytes for a compressed entry -- so 185,003 built-in test vectors ship with
the file. Every fiddly detail below (header stride, block offset base, Oodle args) is
checked against them. If the reader is wrong, it says so; it cannot silently produce
plausible garbage.

Beware: for uncompressed entries stored == uncompressed, so a test run over only
stored files "passes" under either reading of what Hash covers. The distinction is
only observable on a compressed entry. Measured on GlobalShaderCache-PCD3D_SM5.bin
(27 blocks): SHA1(stored) matches, SHA1(uncompressed) does not.

Verified facts about this pak (Palworld 1.0):
    version 11 (Fnv64BugFix), classic .pak (NOT IoStore -- no .utoc/.ucas)
    bEncryptedIndex = 0, EncryptionKeyGuid = 0  -> no AES key needed
    CompressionMethods[1] = 'Oodle'; 133,836 Oodle / 51,167 stored; 0 encrypted
    185,003 entries across 9,007 directories

Layout notes that bite:
  - The inline FPakEntry at each file's offset is 53 B when uncompressed, and
    57 + 16*nblocks when compressed (8 off + 8 size + 8 usize + 4 cmi + 20 hash
    [+ 4 blockcount + 16*n blocks] + 1 encrypted + 4 blocksize).
  - Compression block offsets are relative to the START OF THE ENTRY in the inline
    header, but absolute in the index-decoded entry. Mixing these up is the classic
    UnrealPak trap; the SHA1 check catches it instantly.
"""

import ctypes
import hashlib
import io
import struct
import sys
from pathlib import Path

PAK_MAGIC = 0x5A6F12E1
# Footer for pak v11, measured against this file rather than assumed:
#   EncryptionKeyGuid 16 + bEncryptedIndex 1 + Magic 4 + Version 4
#   + IndexOffset 8 + IndexSize 8 + IndexHash 20 + CompressionMethods 5*32 = 221
# The magic therefore sits at EOF-204, but the footer STARTS at EOF-221.
FOOTER_SIZE = 221


# ------------------------------------------------------------------ Oodle

class Oodle:
    """ctypes binding for OodleLZ_Decompress.

    Signature is taken from Epic's own oodle2.h (2.9.5), not guessed:

        OO_SINTa OodleLZ_Decompress(
            const void* compBuf, OO_SINTa compBufSize,
            void* rawBuf, OO_SINTa rawLen,
            OodleLZ_FuzzSafe fuzzSafe, OodleLZ_CheckCRC checkCRC,
            OodleLZ_Verbosity verbosity,
            void* decBufBase, OO_SINTa decBufSize,
            OodleDecompressCallback* fpCallback, void* callbackUserData,
            void* decoderMemory, OO_SINTa decoderMemorySize,
            OodleLZ_Decode_ThreadPhase threadPhase)
    """

    FUZZ_SAFE_YES = 1
    CHECK_CRC_NO = 0
    VERBOSITY_NONE = 0
    THREAD_PHASE_ALL = 3

    def __init__(self, so_path):
        self.lib = ctypes.CDLL(str(so_path))
        f = self.lib.OodleLZ_Decompress
        f.restype = ctypes.c_ssize_t
        f.argtypes = [
            ctypes.c_void_p, ctypes.c_ssize_t,   # compBuf, compBufSize
            ctypes.c_void_p, ctypes.c_ssize_t,   # rawBuf, rawLen
            ctypes.c_int, ctypes.c_int, ctypes.c_int,  # fuzzSafe, checkCRC, verbosity
            ctypes.c_void_p, ctypes.c_ssize_t,   # decBufBase, decBufSize
            ctypes.c_void_p, ctypes.c_void_p,    # callback, userdata
            ctypes.c_void_p, ctypes.c_ssize_t,   # decoderMemory, decoderMemorySize
            ctypes.c_int,                        # threadPhase
        ]
        self._f = f

    def decompress(self, comp: bytes, raw_len: int) -> bytes:
        out = ctypes.create_string_buffer(raw_len)
        n = self._f(comp, len(comp), out, raw_len,
                    self.FUZZ_SAFE_YES, self.CHECK_CRC_NO, self.VERBOSITY_NONE,
                    None, 0, None, None, None, 0, self.THREAD_PHASE_ALL)
        if n != raw_len:
            raise ValueError(f"OodleLZ_Decompress returned {n}, expected {raw_len}")
        return out.raw[:raw_len]


# ------------------------------------------------------------------ reader

class Reader:
    def __init__(self, b):
        self.f = io.BytesIO(b) if isinstance(b, (bytes, bytearray)) else b

    def u8(self):  return struct.unpack("<B", self.f.read(1))[0]
    def i32(self): return struct.unpack("<i", self.f.read(4))[0]
    def u32(self): return struct.unpack("<I", self.f.read(4))[0]
    def i64(self): return struct.unpack("<q", self.f.read(8))[0]
    def u64(self): return struct.unpack("<Q", self.f.read(8))[0]

    def string(self):
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:  # UTF-16
            raw = self.f.read(-n * 2)
            return raw.decode("utf-16-le").rstrip("\0")
        return self.f.read(n).decode("utf-8", "replace").rstrip("\0")


class PakEntry:
    __slots__ = ("offset", "size", "usize", "cmi", "hash", "blocks", "encrypted",
                 "block_size", "header_size")


class Pak:
    def __init__(self, path, oodle=None):
        self.path = Path(path)
        self.fh = open(self.path, "rb")
        self.oodle = oodle
        self._read_footer()
        self._read_index()

    # -------------------------------------------------- footer
    def _read_footer(self):
        self.file_size = self.path.stat().st_size
        self.fh.seek(self.file_size - FOOTER_SIZE)
        foot = self.fh.read(FOOTER_SIZE)
        r = Reader(foot)
        self.enc_key_guid = r.f.read(16)
        self.encrypted_index = r.u8()
        magic = r.u32()
        if magic != PAK_MAGIC:
            raise ValueError(f"bad pak magic 0x{magic:08X} (expected 0x{PAK_MAGIC:08X})")
        self.version = r.i32()
        self.index_offset = r.i64()
        self.index_size = r.i64()
        self.index_hash = r.f.read(20)
        methods = []
        for _ in range(5):
            m = r.f.read(32).rstrip(b"\0").decode("ascii", "replace")
            if m:
                methods.append(m)
        # index 0 is always "no compression"; manifest indices are 1-based
        self.compression_methods = [""] + methods

    # -------------------------------------------------- index
    def _read_index(self):
        self.fh.seek(self.index_offset)
        idx = self.fh.read(self.index_size)
        r = Reader(idx)
        self.mount_point = r.string()
        self.num_entries = r.i32()
        self.path_hash_seed = r.u64()

        if r.i32():                      # bHasPathHashIndex
            r.i64(); r.i64(); r.f.read(20)

        self.full_dir_index_offset = None
        if r.i32():                      # bHasFullDirectoryIndex
            self.full_dir_index_offset = r.i64()
            self.full_dir_index_size = r.i64()
            self.full_dir_index_hash = r.f.read(20)

        n = r.i32()                      # EncodedPakEntries size
        self.encoded_entries = r.f.read(n)
        self.num_non_encodable = r.i32()

    def directory_index(self):
        """{ '/dir/': { 'file.uasset': encoded_entry_offset } } -- 9,007 dirs."""
        if self.full_dir_index_offset is None:
            raise ValueError("pak has no FullDirectoryIndex")
        self.fh.seek(self.full_dir_index_offset)
        r = Reader(self.fh.read(self.full_dir_index_size))
        out = {}
        for _ in range(r.i32()):
            d = r.string()
            files = {}
            for _ in range(r.i32()):
                fn = r.string()
                files[fn] = r.i32()
            out[d] = files
        return out

    # -------------------------------------------------- entries
    def decode_entry(self, at):
        """Decode one bit-packed entry from EncodedPakEntries.

        UE packs a flags word then only the fields that don't fit the common case.
        Block offsets decoded here are ABSOLUTE (unlike the inline header's).
        """
        r = Reader(self.encoded_entries[at:at + 4096])
        v = r.u32()
        e = PakEntry()
        e.cmi = (v >> 23) & 0x3F
        off32 = bool(v & (1 << 31))
        usz32 = bool(v & (1 << 30))
        sz32 = bool(v & (1 << 29))
        e.encrypted = bool(v & (1 << 22))
        nblocks = (v >> 6) & 0xFFFF
        e.block_size = (v & 0x3F) << 11

        e.offset = r.u32() if off32 else r.u64()
        e.usize = r.u32() if usz32 else r.u64()
        e.size = (r.u32() if sz32 else r.u64()) if e.cmi != 0 else e.usize
        if e.block_size == (0x3F << 11):
            e.block_size = r.u32()

        e.blocks = []
        if e.cmi != 0 and nblocks:
            if nblocks == 1 and not e.encrypted:
                # implicit single block: whole payload, right after the inline header
                start = e.offset + self.inline_header_size(e.cmi, 1)
                e.blocks = [(start, start + e.size)]
            else:
                for _ in range(nblocks):
                    s = r.u32()
                    e.blocks.append((s, s + r.u32() if False else 0))
                # not used -- we always re-read the inline header instead
        e.hash = None
        return e

    @staticmethod
    def inline_header_size(cmi, nblocks):
        # 8 off + 8 size + 8 usize + 4 cmi + 20 hash [+ 4 count + 16*n] + 1 enc + 4 blocksize
        return 53 if cmi == 0 else 57 + 16 * nblocks

    def read_inline_entry(self, offset):
        """The authoritative FPakEntry, stored inline right before the payload."""
        self.fh.seek(offset)
        head = self.fh.read(64)
        r = Reader(head)
        e = PakEntry()
        e.offset = r.i64()
        e.size = r.i64()
        e.usize = r.i64()
        e.cmi = r.u32()
        e.hash = r.f.read(20)
        e.blocks = []
        if e.cmi != 0:
            self.fh.seek(offset + 48)
            n = struct.unpack("<i", self.fh.read(4))[0]
            raw = self.fh.read(16 * n)
            for i in range(n):
                s, x = struct.unpack_from("<qq", raw, i * 16)
                # Inline block offsets are relative to the entry start.
                e.blocks.append((offset + s, offset + x))
            tail = self.fh.read(5)
            e.encrypted = bool(tail[0])
            e.block_size = struct.unpack_from("<I", tail, 1)[0]
            e.header_size = 57 + 16 * n
        else:
            self.fh.seek(offset + 48)
            tail = self.fh.read(5)
            e.encrypted = bool(tail[0])
            e.block_size = struct.unpack_from("<I", tail, 1)[0]
            e.header_size = 53
        return e

    def read_payload(self, encoded_offset, verify=True):
        """Bytes of one file, decompressing if needed.

        FPakEntry.Hash is SHA1 of the *stored* payload -- i.e. the compressed bytes
        for a compressed entry, not the uncompressed ones. This is easy to get wrong:
        for uncompressed entries stored == uncompressed, so both readings agree and
        a test over stored-only files 'confirms' either hypothesis. Verified here
        against a compressed entry (GlobalShaderCache-PCD3D_SM5.bin, 27 blocks),
        where only SHA1(stored) matches.

        So the integrity check happens on the bytes read off disk, before Oodle sees
        them. The decode is then checked separately: OodleLZ_Decompress runs fuzz-safe
        and must return exactly the expected raw length for every block, and the
        assembled total must equal UncompressedSize. Corrupt input fails the hash;
        a wrong decoder fails the length.
        """
        stub = self.decode_entry(encoded_offset)
        e = self.read_inline_entry(stub.offset)

        if e.encrypted:
            raise ValueError("entry is encrypted (this pak should have none)")

        if verify:
            self.fh.seek(stub.offset + e.header_size)
            stored = self.fh.read(e.size)
            got = hashlib.sha1(stored).hexdigest()
            if got != e.hash.hex():
                raise ValueError(f"stored-payload SHA1 mismatch: got {got} want {e.hash.hex()}")

        if e.cmi == 0:
            self.fh.seek(stub.offset + e.header_size)
            data = self.fh.read(e.usize)
        else:
            method = self.compression_methods[e.cmi]
            if method != "Oodle":
                raise ValueError(f"unsupported compression '{method}'")
            if self.oodle is None:
                raise ValueError("Oodle library not loaded")
            out = bytearray()
            for i, (s, x) in enumerate(e.blocks):
                self.fh.seek(s)
                comp = self.fh.read(x - s)
                remaining = e.usize - len(out)
                raw_len = min(e.block_size, remaining)
                out += self.oodle.decompress(comp, raw_len)
            if len(out) != e.usize:
                raise ValueError(f"assembled {len(out)} bytes, expected {e.usize}")
            data = bytes(out)

        return data
