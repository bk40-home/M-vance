#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Put packages (.fwsc) in docs/flash/firmware/ with the manifest the firmware page reads (latest.json):
  {name, version, tag, url, published, files: [{file, product, size, sha256}]}
Each package is checked as tools/fm1_install.py would before it is listed (identity, loader, CRCs).
  tools/flash_manifest.py --version 1.0 [--tag v1.0] [--url RELEASE_PAGE] [--published ISO] PACKAGE.fwsc...
The Pages workflow runs it on the latest release's packages; by hand, for a local try of the page."""
import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1_install as I  # noqa: E402

OUT = ROOT / "docs/flash/firmware"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("packages", nargs="+")
    ap.add_argument("--version", required=True)
    ap.add_argument("--tag", default="")
    ap.add_argument("--url", default="")
    ap.add_argument("--published", default="")
    ap.add_argument("--out", default=str(OUT))
    a = ap.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    files = []
    for p in map(Path, a.packages):
        try:
            product, _ = I.load_package(str(p), False)
        except I.InstallError as e:
            raise SystemExit(f"flash_manifest: {e}")
        raw = p.read_bytes()
        if p.resolve() != (out / p.name).resolve():
            shutil.copyfile(p, out / p.name)
        files.append({"file": p.name, "product": product, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
        print(f"{p.name}: {product}, {len(raw)} B")
    files.sort(key=lambda f: (not f["product"].startswith("FM-1_"), f["file"]))   # the FM-1 first
    m = {"name": "M-vance", "version": a.version, "tag": a.tag, "url": a.url, "published": a.published, "files": files}
    (out / "latest.json").write_text(json.dumps(m, indent=1) + "\n")
    print(f"-> {out / 'latest.json'}")


if __name__ == "__main__":
    main()
