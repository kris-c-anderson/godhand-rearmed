"""Lists the draws and transfers in a PS2X_GIF_DUMP file (the GIF stream sent to PCSX2's GS).

usage: python -I tools/gs/gif_dump.py <dump> [--field N] [--rect x0,y0,x1,y1] [--tbp HEX] [--vram out.bin] [--transfers]

One line per draw (a run of vertices with the same PRIM and state): field, index, primitive, context, texture
(TBP/TBW/PSM/size, CLUT), frame buffer, ALPHA/TEST, vertex count, screen bounding box (XYOFFSET removed), colors and
texture coordinates. --rect keeps draws touching a screen rectangle, --tbp draws using one texture base pointer.
--vram writes the GS local memory captured at the start of the dump (4 MB) for tools/gs/gs_vram.py.
"""
import argparse
import struct

PRIM_NAMES = ["point", "line", "lstrip", "tri", "tstrip", "tfan", "sprite", "?"]
PSM_NAMES = {0x00: "CT32", 0x01: "CT24", 0x02: "CT16", 0x0A: "CT16S", 0x13: "T8", 0x14: "T4", 0x1B: "T8H",
             0x24: "T4HL", 0x2C: "T4HH", 0x30: "Z32", 0x31: "Z24", 0x32: "Z16", 0x3A: "Z16S"}
# A+D register numbers that change how a draw looks (a new draw record starts after one of them).
STATE_REGS = {0x06: "TEX0_1", 0x07: "TEX0_2", 0x08: "CLAMP_1", 0x09: "CLAMP_2", 0x14: "TEX1_1", 0x15: "TEX1_2",
              0x16: "TEX2_1", 0x17: "TEX2_2", 0x18: "XYOFFSET_1", 0x19: "XYOFFSET_2", 0x1A: "PRMODECONT",
              0x1B: "PRMODE", 0x1C: "TEXCLUT", 0x3B: "TEXA", 0x3D: "FOGCOL", 0x40: "SCISSOR_1", 0x41: "SCISSOR_2",
              0x42: "ALPHA_1", 0x43: "ALPHA_2", 0x44: "DIMX", 0x45: "DTHE", 0x46: "COLCLAMP", 0x47: "TEST_1",
              0x48: "TEST_2", 0x49: "PABE", 0x4A: "FBA_1", 0x4B: "FBA_2", 0x4C: "FRAME_1", 0x4D: "FRAME_2",
              0x4E: "ZBUF_1", 0x4F: "ZBUF_2"}


def records(path):
    data = open(path, "rb").read()
    pos = 0
    while pos + 5 <= len(data):
        kind = chr(data[pos])
        size = struct.unpack_from("<I", data, pos + 1)[0]
        yield kind, data[pos + 5:pos + 5 + size]
        pos += 5 + size


class Gs:
    def __init__(self, out):
        self.out = out
        self.regs = {}
        self.prim = 0
        self.rgbaq = (0x80, 0x80, 0x80, 0x80)
        self.q = 1.0
        self.st = (0.0, 0.0)
        self.uv = (0, 0)
        self.draw = None
        self.field = 0
        self.index = 0
        self.bitblt = self.trxpos = self.trxreg = 0
        self.pos = 0
        self.canvas = None
        self.canvas_dbp = None
        self.trx = None

    # --- draws ---
    def flush(self):
        if self.draw and self.draw["verts"]:
            self.index += 1
            self.out(self.field, self.index, self.draw)
        self.draw = None

    def state_changed(self):
        self.flush()

    def vertex(self, x, y, z):
        if self.draw is None:
            ctxt = (self.prim >> 9) & 1
            self.draw = {"prim": self.prim, "tex0": self.regs.get(0x06 + ctxt, 0), "frame": self.regs.get(0x4C + ctxt, 0),
                         "alpha": self.regs.get(0x42 + ctxt, 0), "test": self.regs.get(0x47 + ctxt, 0),
                         "ofs": self.regs.get(0x18 + ctxt, 0), "scissor": self.regs.get(0x40 + ctxt, 0),
                         "tex1": self.regs.get(0x14 + ctxt, 0), "texa": self.regs.get(0x3B, 0), "verts": [],
                         "offset": self.pos}
        self.draw["verts"].append((x / 16.0, y / 16.0, z, self.rgbaq, self.uv, self.st, self.q))

    # --- registers ---
    def write(self, reg, value):
        if reg == 0x00:
            self.flush()
            self.prim = value & 0x7FF
        elif reg == 0x01:
            self.rgbaq = (value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, (value >> 24) & 0xFF)
            self.q = struct.unpack("<f", struct.pack("<I", value >> 32))[0]
        elif reg == 0x02:
            self.st = (struct.unpack("<f", struct.pack("<I", value & 0xFFFFFFFF))[0],
                       struct.unpack("<f", struct.pack("<I", value >> 32))[0])
        elif reg == 0x03:
            self.uv = ((value & 0x3FFF) / 16.0, ((value >> 16) & 0x3FFF) / 16.0)
        elif reg in (0x04, 0x05, 0x0C, 0x0D):
            x, y = value & 0xFFFF, (value >> 16) & 0xFFFF
            z = (value >> 32) & (0xFFFFFF if reg in (0x04, 0x0C) else 0xFFFFFFFF)
            self.vertex(x, y, z)
        elif reg == 0x50:
            self.bitblt = value
        elif reg == 0x51:
            self.trxpos = value
        elif reg == 0x52:
            self.trxreg = value
        elif reg == 0x53:
            self.flush()
            self.out_transfer(value)
        else:
            if reg in STATE_REGS and self.regs.get(reg) != value:
                self.state_changed()
            self.regs[reg] = value

    def image(self, data):
        """IMAGE data of a host->local CT32 transfer: kept per destination (--frames)."""
        if self.canvas is None or not self.trx:
            return
        dbp, dx, dy, w, h = self.trx
        if self.canvas_dbp is not None and dbp != self.canvas_dbp:
            return
        import struct as _s
        n = min(len(data) // 4, w * h)
        for i in range(n):
            x, y = dx + i % w, dy + i // w
            if x < self.canvas.width and y < self.canvas.height:
                r, g, b, _ = data[i * 4:i * 4 + 4]
                self.canvas.putpixel((x, y), (r, g, b))
        self.trx = None

    def out_transfer(self, trxdir):
        b, p, r = self.bitblt, self.trxpos, self.trxreg
        if (trxdir & 3) == 0 and ((b >> 56) & 0x3F) == 0:
            self.trx = ((b >> 32) & 0x3FFF, (p >> 32) & 0x7FF, (p >> 48) & 0x7FF, r & 0xFFF, (r >> 32) & 0xFFF)
        self.index += 1
        self.out(self.field, self.index, {"transfer": True, "dir": trxdir & 3, "dbp": (b >> 32) & 0x3FFF,
                                          "dbw": (b >> 48) & 0x3F, "dpsm": (b >> 56) & 0x3F, "sbp": b & 0x3FFF,
                                          "dx": (p >> 32) & 0x7FF, "dy": (p >> 48) & 0x7FF,
                                          "w": r & 0xFFF, "h": (r >> 32) & 0xFFF})

    def packed(self, desc, lo, hi):
        if desc == 0x0:
            self.write(0x00, lo & 0x7FF)
        elif desc == 0x1:
            r, g, b, a = lo & 0xFF, (lo >> 32) & 0xFF, hi & 0xFF, (hi >> 32) & 0xFF
            self.rgbaq = (r, g, b, a)
        elif desc == 0x2:
            self.st = (struct.unpack("<f", struct.pack("<I", lo & 0xFFFFFFFF))[0],
                       struct.unpack("<f", struct.pack("<I", lo >> 32))[0])
            self.q = struct.unpack("<f", struct.pack("<I", hi & 0xFFFFFFFF))[0]
        elif desc == 0x3:
            self.uv = ((lo & 0x3FFF) / 16.0, ((lo >> 32) & 0x3FFF) / 16.0)
        elif desc == 0x4:
            self.vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, (hi >> 4) & 0xFFFFFF)
        elif desc == 0x5:
            self.vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, hi & 0xFFFFFFFF)
        elif desc in (0x6, 0x7, 0x8, 0x9):
            self.write(desc, lo)
        elif desc == 0xA:
            pass
        elif desc in (0xC, 0xD):
            self.vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, hi & 0xFFFFFFFF)
        elif desc == 0xE:
            self.write(hi & 0xFF, lo)


def parse_gif(gs, stream):
    """Walks a GIF stream (concatenated 'G' records of one field)."""
    q = 0
    total = len(stream) // 16
    while q < total:
        lo, hi = struct.unpack_from("<QQ", stream, q * 16)
        q += 1
        nloop = lo & 0x7FFF
        pre = (lo >> 46) & 1
        prim = (lo >> 47) & 0x7FF
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF or 16
        regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
        if pre and flg == 0:
            gs.write(0x00, prim)
        if flg == 0:
            for _ in range(nloop):
                for desc in regs:
                    if q >= total:
                        return
                    dlo, dhi = struct.unpack_from("<QQ", stream, q * 16)
                    gs.pos = q * 16
                    q += 1
                    gs.packed(desc, dlo, dhi)
        elif flg == 1:
            words = nloop * nreg
            for i in range(words):
                gs.pos = q * 16
                value = struct.unpack_from("<Q", stream, q * 16 + (i % 2) * 8)[0]
                desc = regs[i % nreg]
                if desc not in (0xE, 0xF):
                    gs.write(desc, value)
                if i % 2 == 1:
                    q += 1
            if words % 2:
                q += 1
        else:
            gs.image(stream[q * 16:(q + nloop) * 16])
            q += nloop


def fmt_draw(field, index, d):
    if d.get("transfer"):
        return (f"f{field} #{index} TRANSFER dir={d['dir']} dbp={d['dbp']:x} dbw={d['dbw']} psm={PSM_NAMES.get(d['dpsm'], hex(d['dpsm']))}"
                f" at={d['dx']},{d['dy']} size={d['w']}x{d['h']}")
    prim = d["prim"]
    tex0, frame, ofs = d["tex0"], d["frame"], d["ofs"]
    ofx, ofy = (ofs & 0xFFFF) / 16.0, ((ofs >> 32) & 0xFFFF) / 16.0
    xs = [v[0] - ofx for v in d["verts"]]
    ys = [v[1] - ofy for v in d["verts"]]
    tme, abe, fst, ctxt = (prim >> 4) & 1, (prim >> 6) & 1, (prim >> 8) & 1, (prim >> 9) & 1
    tex = ""
    if tme:
        tex = (f" tex={tex0 & 0x3FFF:x}/{(tex0 >> 14) & 0x3F}/{PSM_NAMES.get((tex0 >> 20) & 0x3F, hex((tex0 >> 20) & 0x3F))}"
               f"/{1 << ((tex0 >> 26) & 0xF)}x{1 << ((tex0 >> 30) & 0xF)} tcc={(tex0 >> 34) & 1} tfx={(tex0 >> 35) & 3}"
               f" cbp={(tex0 >> 37) & 0x3FFF:x}/{PSM_NAMES.get((tex0 >> 51) & 0xF, '?')}")
        if fst:
            us = [v[4][0] for v in d["verts"]]
            vs = [v[4][1] for v in d["verts"]]
            tex += f" uv={min(us):.0f}-{max(us):.0f},{min(vs):.0f}-{max(vs):.0f}"
        else:
            ss = [v[5][0] / v[6] if v[6] else 0 for v in d["verts"]]
            ts = [v[5][1] / v[6] if v[6] else 0 for v in d["verts"]]
            tex += f" st/q={min(ss):.3f}-{max(ss):.3f},{min(ts):.3f}-{max(ts):.3f}"
    colors = sorted({"%02x%02x%02x%02x" % v[3] for v in d["verts"]})
    return (f"f{field} #{index} {PRIM_NAMES[prim & 7]} ctxt={ctxt} tme={tme} abe={abe} fb={frame & 0x1FF:x}/{(frame >> 16) & 0x3F}"
            f" alpha={d['alpha']:x} test={d['test']:x} n={len(d['verts'])}"
            f" x={min(xs):.1f}-{max(xs):.1f} y={min(ys):.1f}-{max(ys):.1f}{tex}"
            f" rgba={','.join(colors[:4])}{'...' if len(colors) > 4 else ''} @{d['offset']:x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--field", type=int)
    ap.add_argument("--rect")
    ap.add_argument("--tbp")
    ap.add_argument("--vram")
    ap.add_argument("--transfers", action="store_true", help="also list image transfers")
    ap.add_argument("--frames", help="DIR:DBP: rebuild the CT32 image uploaded to DBP (hex) each field as DIR/upload_<field>.png")
    ap.add_argument("--bytes", help="OFFSET:LEN: hex of the field's GIF stream there (the @offset of a draw)")
    a = ap.parse_args()
    rect = [float(v) for v in a.rect.split(",")] if a.rect else None
    tbp = int(a.tbp, 16) if a.tbp else None

    def out(field, index, d):
        if a.field is not None and field != a.field:
            return
        if d.get("transfer"):
            if a.transfers:
                print(fmt_draw(field, index, d))
            return
        if tbp is not None and (not (d["prim"] >> 4) & 1 or d["tex0"] & 0x3FFF != tbp):
            return
        if rect:
            ofs = d["ofs"]
            xs = [v[0] - (ofs & 0xFFFF) / 16.0 for v in d["verts"]]
            ys = [v[1] - ((ofs >> 32) & 0xFFFF) / 16.0 for v in d["verts"]]
            if max(xs) < rect[0] or min(xs) > rect[2] or max(ys) < rect[1] or min(ys) > rect[3]:
                return
        print(fmt_draw(field, index, d))

    gs = Gs(out)
    if a.frames:
        from PIL import Image
        frames_dir, frames_dbp = a.frames.rsplit(":", 1)
        gs.canvas = Image.new("RGB", (640, 448), (255, 0, 255))
        gs.canvas_dbp = int(frames_dbp, 16)
    stream = bytearray()
    for kind, payload in records(a.dump):
        if kind == "M" and a.vram:
            open(a.vram, "wb").write(payload)
        elif kind == "G":
            stream += payload
        elif kind == "V":
            if a.bytes and (a.field is None or gs.field == a.field):
                off, length = (int(v, 16) for v in a.bytes.split(":"))
                print(bytes(stream[off:off + length]).hex())
            parse_gif(gs, bytes(stream))
            gs.flush()
            if a.frames:
                from pathlib import Path as _P
                gs.canvas.save(_P(frames_dir) / f"upload_{struct.unpack_from('<Q', payload, 0)[0]}.png")
            stream.clear()
            gs.field = struct.unpack_from("<Q", payload, 0)[0]
            gs.index = 0
            print(f"--- vsync, field {gs.field}") if a.field is None else None


if __name__ == "__main__":
    main()
