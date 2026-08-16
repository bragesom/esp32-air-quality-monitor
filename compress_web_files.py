#!/usr/bin/env python3
"""
Gzip the web assets in data/www for SPIFFS upload.

Runs both standalone (`python compress_web_files.py`) and as the PlatformIO
`pre:` hook declared in platformio.ini.

IMPORTANT: main() is called unconditionally. PlatformIO executes extra scripts
through SCons, which supplies globals where __name__ == "SCons.Script" -- so a
`if __name__ == "__main__"` guard would silently never run, leaving the device
serving stale or uncompressed assets. Do not reintroduce that guard.

Orphan .gz files (whose source was removed or renamed) are deleted, because
serveStatic PREFERS the .gz variant: a leftover .gz would keep shipping old
JavaScript alongside fresh sources.
"""

import os
import gzip
import shutil

# Resolve relative to this file so the hook works regardless of CWD.
try:
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))
except NameError:          # SCons may exec without __file__
    BASE_DIR = os.getcwd()

DATA_DIR = os.path.join(BASE_DIR, "data", "www")

FILES_TO_COMPRESS = [
    "index.html",
    "chart.html",
    "files.html",
    "clock.html",
    "style.css",
    "common.js",
    "chart-lite.js",
    "dashboard.js",
    "charts.js",
    "files.js",
]


def compress_file(source_path, dest_path):
    """Gzip source_path -> dest_path and report the ratio."""
    with open(source_path, "rb") as f_in:
        with gzip.open(dest_path, "wb") as f_out:
            shutil.copyfileobj(f_in, f_out)

    original = os.path.getsize(source_path)
    compressed = os.path.getsize(dest_path)
    ratio = ((original - compressed) / original * 100) if original else 0.0
    print(f"  gzip {os.path.basename(source_path)}: "
          f"{original} -> {compressed} bytes ({ratio:.1f}% smaller)")


def main():
    if not os.path.isdir(DATA_DIR):
        print(f"compress_web_files: {DATA_DIR} not found, nothing to do")
        return

    print("compress_web_files: compressing web assets...")
    expected = set()

    for name in FILES_TO_COMPRESS:
        source = os.path.join(DATA_DIR, name)
        dest = source + ".gz"
        if os.path.exists(source):
            compress_file(source, dest)
            expected.add(name + ".gz")
        else:
            print(f"  WARNING: {name} listed but not found in data/www")

    # Drop stale .gz so an old asset can never be uploaded/served.
    for entry in os.listdir(DATA_DIR):
        if entry.endswith(".gz") and entry not in expected:
            os.remove(os.path.join(DATA_DIR, entry))
            print(f"  removed orphan {entry}")

    print(f"compress_web_files: {len(expected)} file(s) ready")


# Called unconditionally on purpose -- see module docstring.
main()
