"""Build the playable character mods from your own disc: assets/characters/<id>/ -> <out>/mods/characters/<id>/.

    python -I tools/models/build_characters.py --iso <God Hand.iso> --out <folder holding godhand.exe> [id ...]

Each assets/characters/<id>/character.json lists the disc files it replaces:
    {"files": {"pl/pl00.dat": {"nodes": "xavier_nodes.scr", "textures": "xavier_textures.tm3"},
               "pl/pl01.dat": {"nodes": "xavier_coat.scr", "textures": "xavier_textures.tm3", "keep": [],
                               "append_textures": false}}}
"nodes"/"textures": the mod's own model nodes and textures merged into the game's file (its skeleton, motions and the
"keep" nodes, by default the God Hand arm, stay the game's; textures replace the game's of the same name, new names
are appended unless "append_textures" is false); "trim": {"GH_00": {"axis": [-1, 0, 0], "min": 0.27}} cuts a kept
node at the plane axis.p = min (rest pose, model space), keeping the far side and clipping the triangles that cross it
(e.g. the God Hand arm where it enters the character's own upper arm); a "tint": {"start": s, "length": l, "rgb": [r,
g, b]} on a trim multiplies the node's vertex colours by rgb/128 up to axis.p = s, fading to unchanged over l; an
"inset": {"start": s, "factor": f, "centre": [cy, cz]} pulls the vertices between the cut and axis.p = s towards the
bone line (by f at the cut, unchanged at s), so the part inside the character's own stump stays hidden; "empty": every
model node of that file drawn as nothing. A "textures" folder in the
character's assets (PCSX2 replacement images named by the in-game textures' hashes, made by the character's encoder) is
copied to <out>/mods/characters/<id>/textures: the runtime adds it to the replacement folders, so the character's
textures draw at full resolution. The output is built
from your disc, so like the rest of the build it stays private. Standard library only.
"""
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "snr2"))
import ghmodel  # noqa: E402
from snr2disc import Disc  # noqa: E402


def build(iso, out, only=()):
    disc = Disc(iso)
    paths = {p.lower(): i for p, i in disc.fst_files()}
    base = os.path.join(ROOT, "assets", "characters")
    built = 0
    for cid in sorted(os.listdir(base)):
        folder = os.path.join(base, cid)
        spec_path = os.path.join(folder, "character.json")
        if (only and cid not in only) or not os.path.isfile(spec_path):
            continue
        spec = json.load(open(spec_path, encoding="utf-8"))
        dest = os.path.join(out, "mods", "characters", cid)
        os.makedirs(dest, exist_ok=True)
        for rel, how in spec["files"].items():
            if rel.lower() not in paths:
                raise SystemExit("%s: %s is not on the disc" % (cid, rel))
            game = disc.read_afs(paths[rel.lower()])
            if how.get("empty"):
                data = ghmodel.empty_model(game)
            else:
                nodes = open(os.path.join(folder, how["nodes"]), "rb").read()
                texs = open(os.path.join(folder, how["textures"]), "rb").read()
                trim = {}
                for name, t in how.get("trim", {}).items():
                    tint, inset = t.get("tint"), t.get("inset")
                    trim[name] = (tuple(t["axis"]), t["min"],
                                  (tint["start"], tint["length"], tuple(tint["rgb"])) if tint else None,
                                  (inset["start"], inset["factor"], tuple(inset["centre"])) if inset else None)
                data = ghmodel.merge_model(game, nodes, texs, tuple(how.get("keep", ghmodel.KEEP)),
                                           how.get("append_textures", True), trim)
            if len(data) > len(game):
                print("  warning: %s/%s is %d bytes, the game's %d (it must fit the game's place on the disc)" % (cid, rel, len(data), len(game)))
            target = os.path.join(dest, *rel.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(data)
            print("  %s: %s %d -> %d bytes" % (cid, rel, len(game), len(data)))
        hd = os.path.join(folder, "textures")
        if os.path.isdir(hd):
            target = os.path.join(dest, "textures")
            if os.path.isdir(target):
                shutil.rmtree(target)
            shutil.copytree(hd, target)
            print("  %s: %d full-resolution textures" % (cid, len(os.listdir(target))))
        if os.path.isfile(os.path.join(folder, "character.ini")):
            shutil.copyfile(os.path.join(folder, "character.ini"), os.path.join(dest, "character.ini"))
        built += 1
    disc.close()
    print("%d character(s) built into %s" % (built, os.path.join(out, "mods", "characters")))
    return 0


def main(argv):
    args = list(argv[1:])
    def opt(name):
        if name in args:
            i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
        return None
    iso, out = opt("--iso"), opt("--out")
    if not iso or not out:
        print(__doc__)
        return 2
    return build(iso, out, tuple(args))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
