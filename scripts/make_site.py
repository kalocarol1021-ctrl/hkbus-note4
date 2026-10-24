#!/usr/bin/env python3
"""Assemble the GitHub Pages site: browser installer + firmware + ota.json.

  python3 scripts/make_site.py BUILD_DIR SITE_DIR BASE_URL VERSION [NOTES]

BUILD_DIR must contain zectrix_hkbus_board.bin (application image) and
hkbus-note4-full.bin (merged image for address 0x0). BASE_URL is the public
address of the site, e.g. https://user.github.io/zectrix-note4-hkbus/.
"""
import hashlib
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    build, site, base, version = sys.argv[1:5]
    notes = sys.argv[5] if len(sys.argv) > 5 else ""
    base = base.rstrip("/") + "/"

    os.makedirs(os.path.join(site, "firmware"), exist_ok=True)
    for name in os.listdir(os.path.join(ROOT, "installer")):
        src = os.path.join(ROOT, "installer", name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(site, name))

    app = os.path.join(site, "firmware", "hkbus-note4-app.bin")
    full = os.path.join(site, "firmware", "hkbus-note4-full.bin")
    shutil.copy2(os.path.join(build, "zectrix_hkbus_board.bin"), app)
    shutil.copy2(os.path.join(build, "hkbus-note4-full.bin"), full)

    manifest = {
        "version": version,
        "notes": notes,
        # Read by the device (absolute HTTPS URL required).
        "url": base + "firmware/hkbus-note4-app.bin",
        "size": os.path.getsize(app),
        "sha256": sha256(app),
        # Read by the browser installer (relative to the site root).
        "full_url": "firmware/hkbus-note4-full.bin",
        "full_size": os.path.getsize(full),
        "full_sha256": sha256(full),
    }
    with open(os.path.join(site, "firmware", "ota.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)
    # GitHub Pages: serve files as-is.
    open(os.path.join(site, ".nojekyll"), "w").close()
    print(json.dumps(manifest, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
