#!/usr/bin/env python3
"""Prepares the web files for the LittleFS image.

Copies every file from SRC_DIR to OUT_DIR, gzipping text assets to
<name>.gz so the HTTPS server can send them with Content-Encoding: gzip.
Already compressed formats are copied as they are.

usage: gzip_web.py SRC_DIR OUT_DIR STAMP_FILE
"""
import gzip
import shutil
import sys
from pathlib import Path

GZIP_SUFFIXES = {".html", ".htm", ".js", ".mjs", ".css", ".json", ".svg", ".txt", ".xml", ".ico"}


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 1

    src_dir = Path(sys.argv[1]).resolve()
    out_dir = Path(sys.argv[2]).resolve()
    stamp = Path(sys.argv[3])

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    total_in = total_out = 0
    for src in sorted(p for p in src_dir.rglob("*") if p.is_file()):
        rel = src.relative_to(src_dir)
        data = src.read_bytes()

        dst = out_dir / rel
        out = data

        if src.suffix.lower() in GZIP_SUFFIXES:
            # mtime=0 keeps the output reproducible between builds
            compressed = gzip.compress(data, compresslevel=9, mtime=0)
            # tiny files can grow when gzipped, keep the original then
            if len(compressed) < len(data):
                dst = out_dir / rel.with_name(rel.name + ".gz")
                out = compressed

        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(out)

        total_in += len(data)
        total_out += len(out)
        print(f"web: {dst.relative_to(out_dir)} {len(data)} -> {len(out)} bytes")

    print(f"web: total {total_in} -> {total_out} bytes")
    stamp.touch()
    return 0


if __name__ == "__main__":
    sys.exit(main())
