"""Extract God Hand's files from your own disc image (for modding work; the output is game data: keep it private).

    python tools/disc_extract.py <God Hand.iso> <output folder> [name filter ...]

Writes every file of _DATA.AFS under its GODHAND.FST path (e.g. <out>/pl/pl00.dat), plus the ISO root files, and
<out>/files.csv (path, AFS index, size). With filters, only paths containing one of them (case-insensitive).
Standard library only.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "snr2"))
from snr2disc import Disc  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 2
    iso, out = argv[1], argv[2]
    filters = [f.lower() for f in argv[3:]]
    disc = Disc(iso)
    os.makedirs(out, exist_ok=True)
    rows = []
    for path, index in disc.fst_files():
        if filters and not any(f in path.lower() for f in filters):
            continue
        data = disc.read_afs(index)
        target = os.path.join(out, *path.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as f:
            f.write(data)
        rows.append((path, index, len(data)))
    if not filters:
        for name, (lba, size) in disc.root.items():
            if name == "_DATA.AFS" or size > 64 * 1024 * 1024:
                continue
            with open(os.path.join(out, name), "wb") as f:
                f.write(disc.read_root_file(name))
    with open(os.path.join(out, "files.csv"), "w", encoding="utf-8") as f:
        f.write("path,afs_index,size\n")
        for path, index, size in rows:
            f.write("%s,%d,%d\n" % (path, index, size))
    print("%d files written to %s" % (len(rows), out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
