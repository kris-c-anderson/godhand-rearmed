"""Paints a character's fur onto the game's arm-skin textures where the God Hand arm leaves the character's stump, so the
arm grows out of the fur the way Xavier's snake arm does in his own texture (build_characters.py, trim "paint").

paint_textures(game_texs, node, names, fur_rgba, axis, start, length) -> {name: new tim3 bytes, ...}
    game_texs: [(name, tim3 bytes)] of the game's texture container; node: the (trimmed) node dict (vbs with pos/uv);
    names: the textures to paint; fur_rgba: HxWx4 uint8 fur image (tiled over the texture); the weight of the fur at a
    vertex is 1 where axis.p <= start (inside the stump and at its end) and fades to 0 over 'length' beyond it. The
    painted 16-colour texture keeps the game's size and layout.
weight_map(node, w, h, axis, start, length) -> HxW float32 weights (0..1), rasterized from the node's triangles in the
    texture's UV space, for the full-resolution replacement as well.
"""
import math
import struct

import numpy as np
from PIL import Image

from ghmodel import strip_tris


def node_tris(node):
    """[(pos3, uv2 texel-space as fractions s,t), ...] per triangle of a node dict."""
    out = []
    for vb in node['vbs']:
        for a, b, c in strip_tris(vb['flags']):
            tri = []
            for i in (a, b, c):
                u, v = vb['uv'][i]
                tri.append((vb['pos'][i], (u / 4096.0, 1.0 + v / 4096.0)))  # t from the top row
            out.append(tri)
    return out


def weight_map(node, w, h, axis, start, length):
    W = np.zeros((h, w), np.float32)
    count = np.zeros((h, w), np.float32)
    ys, xs = np.mgrid[0:h, 0:w]
    px = xs + 0.5
    py = ys + 0.5
    for tri in node_tris(node):
        pts = []
        for pos, (s, t) in tri:
            d = pos[0] * axis[0] + pos[1] * axis[1] + pos[2] * axis[2]
            wt = 1.0 if d <= start else max(0.0, 1.0 - (d - start) / length) if length > 0 else 0.0
            pts.append(((s % 1.0) * w, (t % 1.0) * h, wt))
        (x0, y0, w0), (x1, y1, w1), (x2, y2, w2) = pts
        det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(det) < 1e-9:
            continue
        bx0, bx1 = int(max(0, math.floor(min(x0, x1, x2)) - 1)), int(min(w, math.ceil(max(x0, x1, x2)) + 1))
        by0, by1 = int(max(0, math.floor(min(y0, y1, y2)) - 1)), int(min(h, math.ceil(max(y0, y1, y2)) + 1))
        if bx1 <= bx0 or by1 <= by0:
            continue
        X = px[by0:by1, bx0:bx1]
        Y = py[by0:by1, bx0:bx1]
        l1 = ((x1 - x0) * (Y - y0) - (X - x0) * (y1 - y0)) / det  # weight of vertex 2
        l2 = ((X - x0) * (y2 - y0) - (x2 - x0) * (Y - y0)) / det  # weight of vertex 1
        l0 = 1.0 - l1 - l2
        inside = (l0 >= -0.02) & (l1 >= -0.02) & (l2 >= -0.02)
        val = np.clip(l0 * w0 + l2 * w1 + l1 * w2, 0.0, 1.0)
        W[by0:by1, bx0:bx1] = np.where(inside, np.maximum(W[by0:by1, bx0:bx1], val), W[by0:by1, bx0:bx1])
        count[by0:by1, bx0:bx1] += inside
    # grow the painted area by a texel so bilinear sampling at the island edges does not show skin
    grown = W.copy()
    for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        grown = np.maximum(grown, np.roll(np.roll(W, dy, 0), dx, 1) * 0.999)
    return np.where(count > 0, W, grown).astype(np.float32)


def composite(skin_rgba, fur_rgba, weights):
    """skin and fur HxWx4 (fur tiled to the skin's size), weights HxW: lerp of the colours; alpha stays the skin's."""
    h, w = skin_rgba.shape[:2]
    fh, fw = fur_rgba.shape[:2]
    tiled = fur_rgba[np.arange(h) % fh][:, np.arange(w) % fw]
    out = skin_rgba.astype(np.float32)
    wt = weights[..., None]
    out[..., :3] = out[..., :3] * (1.0 - wt) + tiled[..., :3].astype(np.float32) * wt
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def quantize16(rgba):
    """16-colour indices and palette (RGBA, alpha 255) of an opaque image."""
    h, w = rgba.shape[:2]
    q = Image.fromarray(np.ascontiguousarray(rgba[..., :3]), 'RGB').quantize(16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal3 = np.array(q.getpalette()[:48], np.uint8).reshape(-1, 3)
    while len(pal3) < 16:
        pal3 = np.vstack([pal3, pal3[-1:]])
    pal = np.hstack([pal3[:16], np.full((16, 1), 255, np.uint8)])
    d = ((rgba[:, :, None, :3].astype(int) - pal[None, None, :, :3].astype(int)) ** 2).sum(-1)
    return np.argmin(d, -1).astype(np.uint8), pal


def tim3_encode(idx, pal, w, h):
    """A 4-bit TIM3 in the game's layout (as encode_xavier.py writes them)."""
    from ghtex import swizzle4
    idx = np.asarray(idx, np.uint8)
    if w >= 128 and h >= 128:
        data = swizzle4(idx, w, h)
        fid = 6
    else:
        f = idx.ravel()
        data = bytes((f[0::2] | (f[1::2] << 4)).astype(np.uint8))
        fid = 0
    clut = bytearray()
    for r, g, b, a in pal:
        clut += bytes((int(r), int(g), int(b), int(round(int(a) * 128 / 255))))
    tbw = max(2, w // 64)
    tw = int(math.log2(w))
    th = int(math.log2(h))
    tex0 = (tbw << 14) | (20 << 20) | (tw << 26) | (th << 30)
    pic = struct.pack('<3I2H4B2H2Q2I', 48 + len(clut) + len(data), len(clut), len(data), 48, 16, 0, 1, 3, 4, w, h, tex0, 0x260, 0, 0)
    return b'TIM3' + bytes((4, fid, 1, 0)) + bytes(8) + pic + data + bytes(clut)


def paint_textures(game_texs, node, names, fur_rgba, axis, start, length):
    from ghtex import tim3_decode
    out = {}
    for name, t in game_texs:
        if name not in names:
            continue
        rgba, info = tim3_decode(t)
        w, h = info['w'], info['h']
        W = weight_map(node, w, h, axis, start, length)
        painted = composite(rgba, fur_rgba, W)
        idx, pal = quantize16(painted)
        out[name] = tim3_encode(idx, pal, w, h)
    return out
