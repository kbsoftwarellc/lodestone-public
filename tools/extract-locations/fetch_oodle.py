#!/usr/bin/env python3
"""Fetch Epic's Oodle Data decompressor from Epic's own CDN.

Palworld's pak is Oodle-compressed (133,836 of 185,003 entries), and nothing in
_Generated_ is stored raw, so reading level data offline requires a decompressor.
There is no way around this.

Provenance matters more than convenience here, because the alternative is running
a native library of unknown origin. Third-party mirrors of the Oodle SDK exist and
are widely used, but a mirror is a stranger's binary: ctypes.CDLL() executes its
init code the instant it loads, long before any hash of the *output* is checked.
A pak-hash oracle proves the decompressor produced correct bytes; it proves nothing
about what the library did while loading.

So take it from Epic:

  - Epic made Oodle Data free for Unreal Engine users (2021). Linking a GitHub
    account to an Epic account grants access to EpicGames/UnrealEngine.
  - Epic's git repo does NOT contain the binaries -- only help/ and include/. The
    libraries are fetched by Setup.sh from Epic's CDN via the dependency manifest
    Engine/Build/Commit.gitdeps.xml. (This is why 'sparse-clone the SDK directory'
    does not work; that path exists only in repackaged mirrors.)
  - This script reads that manifest and downloads exactly one file from Epic's CDN,
    verifying Epic's own SHA1 for it. No mirror, no Setup.sh, no multi-GB sync.

Version note: Palworld is UE 5.1, whose Oodle header reports 2.9.5 -- but 2.9.5
ships no Linux .so, only a static .a. Oodle's bitstream is backward compatible, so
a later decoder reads earlier streams. We take 2.9.8. That assumption is not
load-bearing on trust: pak entries each carry SHA1(uncompressed payload), so a
wrong decoder fails loudly on the first block rather than corrupting silently.

Usage:
    python3 tools/extract-locations/fetch_oodle.py [--out DIR] [--version 2.9.8]

Requires: gh CLI authenticated as an account linked to Epic (EpicGames org member).
"""

import argparse
import gzip
import hashlib
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

MANIFEST_PATH = "Engine/Build/Commit.gitdeps.xml"
REPO = "EpicGames/UnrealEngine"
REF = "5.1"  # Palworld is UE 5.1


def gh_raw(path, ref=REF):
    """Read a file out of the private Epic repo via the authenticated gh CLI."""
    r = subprocess.run(
        ["gh", "api", f"repos/{REPO}/contents/{path}?ref={ref}",
         "-H", "Accept: application/vnd.github.raw"],
        capture_output=True,
    )
    if r.returncode != 0:
        err = r.stderr.decode(errors="replace").strip()
        if "Not Found" in err:
            sys.exit(
                "error: cannot read EpicGames/UnrealEngine.\n"
                "  Link your GitHub account at https://www.unrealengine.com/ue4-on-github\n"
                "  then ACCEPT the EpicGames org invitation (it stays 'pending' otherwise):\n"
                "    gh api --method PATCH user/memberships/orgs/EpicGames -f state=active"
            )
        sys.exit(f"error: gh failed: {err}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("third_party/oodle"))
    ap.add_argument("--version", default="2.9.8")
    ap.add_argument("--manifest", type=Path,
                    help="use a already-downloaded Commit.gitdeps.xml (it is ~25MB)")
    args = ap.parse_args()

    want = f"Engine/Source/Runtime/OodleDataCompression/Sdks/{args.version}/lib/Linux/liboo2corelinux64.so.9"

    if args.manifest and args.manifest.exists():
        blob = args.manifest.read_bytes()
        print(f"  manifest: {args.manifest} ({len(blob):,} B, cached)")
    else:
        print(f"  fetching {MANIFEST_PATH} @ {REF} ...")
        blob = gh_raw(MANIFEST_PATH)
        print(f"  manifest: {len(blob):,} B")

    root = ET.fromstring(blob)
    base = root.get("BaseUrl")
    files = {f.get("Name"): f for f in root.find("Files")}
    blobs = {b.get("Hash"): b for b in root.find("Blobs")}
    packs = {p.get("Hash"): p for p in root.find("Packs")}
    print(f"  BaseUrl: {base}")
    print(f"  entries: {len(files):,} files / {len(blobs):,} blobs / {len(packs):,} packs")

    f = files.get(want)
    if f is None:
        sys.exit(f"error: {want} not in manifest (try --version 2.9.6/2.9.7/2.9.8)")
    fhash = f.get("Hash")
    b = blobs.get(fhash)
    if b is None:
        sys.exit(f"error: no blob for hash {fhash}")
    p = packs.get(b.get("PackHash"))
    if p is None:
        sys.exit(f"error: no pack for hash {b.get('PackHash')}")

    off, size = int(b.get("PackOffset")), int(b.get("Size"))
    url = f"{base}/{p.get('RemotePath')}/{p.get('Hash')}"
    print(f"\n  file : {want}")
    print(f"  sha1 : {fhash}   (Epic's own, from the manifest)")
    print(f"  pack : {url}")
    print(f"  slice: offset {off:,} size {size:,}")

    print("\n  downloading pack from Epic's CDN ...")
    with urllib.request.urlopen(url) as r:
        packed = r.read()
    print(f"  pack: {len(packed):,} B compressed")

    raw = gzip.decompress(packed)
    data = raw[off:off + size]
    print(f"  unpacked: {len(raw):,} B -> slice {len(data):,} B")

    got = hashlib.sha1(data).hexdigest()
    if got != fhash:
        sys.exit(f"error: SHA1 MISMATCH\n  expected {fhash}\n  got      {got}")
    print(f"  SHA1 VERIFIED against Epic's manifest: {got}")

    args.out.mkdir(parents=True, exist_ok=True)
    dest = args.out / "liboo2corelinux64.so.9"
    dest.write_bytes(data)
    dest.chmod(0o755)
    print(f"\n  wrote {dest} ({len(data):,} B)")
    print("  provenance: EpicGames/UnrealEngine@5.1 manifest -> cdn.unrealengine.com -> SHA1 verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
