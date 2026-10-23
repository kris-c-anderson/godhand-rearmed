"""Build the playable character mods from your own disc: assets/characters/<id>/ -> <out>/mods/characters/<id>/.

    python -I tools/models/build_characters.py --iso <God Hand.iso> --out <folder holding godhand.exe> [--texture-pack <dir>] [id ...]

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
model node of that file drawn as nothing; a "paint": {"textures": [...], "fur": "<mod texture>", "start": s, "length": l}
on a trim paints the mod's fur over those game textures where the kept node lies within l beyond axis.p = s (the arm
skin grows out of the fur), and writes the painted textures' full-resolution replacements (from the pack's, when
--texture-pack holds one) into the character's textures folder. Painting needs numpy and Pillow. A "textures" folder in the
character's assets (PCSX2 replacement images named by the in-game textures' hashes, made by the character's encoder) is
copied to <out>/mods/characters/<id>/textures: the runtime adds it to the replacement folders, so the character's
textures draw at full resolution. The output is built
from your disc, so like the rest of the build it stays private. Standard library only, except painting.
"""
import json
import os
import shutil
import struct
import sys

texture_pack = None  # --texture-pack <folder holding replacements/>: the HD pack the painted textures start from

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "xavier"))
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
                trim, paint = {}, {}
                for name, t in how.get("trim", {}).items():
                    tint, inset, pt = t.get("tint"), t.get("inset"), t.get("paint")
                    trim[name] = (tuple(t["axis"]), t["min"],
                                  (tint["start"], tint["length"], tuple(tint["rgb"])) if tint else None,
                                  (inset["start"], inset["factor"], tuple(inset["centre"])) if inset else None)
                    if pt:
                        paint[name] = (tuple(pt["textures"]), pt["fur"], pt["start"], pt["length"])
                data = ghmodel.merge_model(game, nodes, texs, tuple(how.get("keep", ghmodel.KEEP)),
                                           how.get("append_textures", True), trim, paint)
                if paint:
                    try:
                        painted_hd(folder, dest, game, data, texs, paint, trim, texture_pack)
                    except ImportError as e:
                        print("  warning: no full-resolution painted textures (%s)" % e)
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


def painted_hd(folder, dest, game, merged, mod_tm3, paint, trim, texture_pack):
    """Full-resolution replacements for the painted textures: the pack's replacement of the game's texture (an
    uncompressed BGRA .dds, as God Hand packs ship) or the game's own texture scaled up, with the character's
    full-resolution fur painted on with the same weights, saved under the painted texture's replacement name."""
    import numpy as np
    from PIL import Image
    import paint as painter
    from ghtex import tim3_decode
    from pcsx2tex import replacement_name
    game_tex = {nm: tb for t, d, o in ghmodel.dat_read(game) if t == "TM3" and d is not None for nm, tb, _ in ghmodel.tm3_read(d)}
    new_tex = {nm: tb for t, d, o in ghmodel.dat_read(merged) if t == "TM3" and d is not None for nm, tb, _ in ghmodel.tm3_read(d)}
    mod_tex = {nm: tb for nm, tb, _ in ghmodel.tm3_read(mod_tm3)}
    md = [d for t, d, o in ghmodel.dat_read(game) if t == "MD" and d is not None and d[:3] == b"scr"][0]
    kind, nodes = ghmodel.scr_read(md)
    hd_dir = os.path.join(dest, "textures")
    os.makedirs(hd_dir, exist_ok=True)
    for node_name, (names, fur_name, start, length) in paint.items():
        nd = [n for n in nodes if n["name"] == node_name][0]
        mdb = ghmodel.mdb_read(md, nd["mdb_off"])
        if node_name in trim:
            mdb = ghmodel.trim_mdb(mdb, *trim[node_name])
        axis = trim[node_name][0] if node_name in trim else (-1, 0, 0)
        # the fur at full resolution: the character's own replacement of its fur texture
        fur_png = os.path.join(folder, "textures", replacement_name(mod_tex[fur_name]) + ".png")
        fur_hd = np.asarray(Image.open(fur_png).convert("RGBA")) if os.path.isfile(fur_png) else None
        for name in names:
            if name not in game_tex or name not in new_tex:
                continue
            rgba, info = tim3_decode(game_tex[name])
            w, h = info["w"], info["h"]
            base = None
            if texture_pack:
                dds = os.path.join(texture_pack, "replacements", replacement_name(game_tex[name]) + ".dds")
                if os.path.isfile(dds):
                    b = open(dds, "rb").read()
                    dh, dw = struct.unpack_from("<II", b, 12)
                    bpp = struct.unpack_from("<I", b, 88)[0]
                    if bpp == 32 and len(b) >= 128 + dw * dh * 4:
                        px = np.frombuffer(b[128:128 + dw * dh * 4], np.uint8).reshape(dh, dw, 4)
                        base = np.ascontiguousarray(px[..., [2, 1, 0, 3]])
            scale = 1
            if base is None:
                scale = 4
                base = np.asarray(Image.fromarray(rgba, "RGBA").resize((w * scale, h * scale), Image.LANCZOS))
            else:
                scale = base.shape[1] // w
            fur = fur_hd if fur_hd is not None else np.asarray(Image.fromarray(tim3_decode(mod_tex[fur_name])[0], "RGBA").resize((scale * 256, scale * 512), Image.LANCZOS))
            W = painter.weight_map(mdb, base.shape[1], base.shape[0], axis, start, length)
            out = painter.composite(base, fur, W)
            Image.fromarray(out[..., :3], "RGB").save(os.path.join(hd_dir, replacement_name(new_tex[name]) + ".png"), optimize=True)
            print("  %s: painted %s (%dx%d%s)" % (os.path.basename(dest), name, base.shape[1], base.shape[0], ", from the pack" if texture_pack and scale > 4 else ""))


def main(argv):
    args = list(argv[1:])
    def opt(name):
        if name in args:
            i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
        return None
    iso, out, pack = opt("--iso"), opt("--out"), opt("--texture-pack")
    if not iso or not out:
        print(__doc__)
        return 2
    global texture_pack
    texture_pack = pack
    return build(iso, out, tuple(args))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
