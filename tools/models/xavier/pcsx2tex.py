"""PCSX2 texture replacement file names for 4-bit TIM3 textures, as the game uploads them (numpy, xxhash).

PCSX2 finds a replacement by <TEX0 hash>-<CLUT hash>-<bits>.png (GSTextureReplacements.cpp): the TEX0 hash is XXH3-64
over the texture's 256-byte GS memory blocks in block order (PSMT4 at TBP 0 with the texture's TBW; blocks of 32x16
texels), the CLUT hash XXH3-64 over the 16 CLUT entries (RGBA, alpha 0-128), bits = PSM | TW << 6 | TH << 10 (| 0x4000,
the old TCC bit, which PCSX2's own dumps of this game carry). Checked against a God Hand pack: Gene's pl0000, pl0001
and pl0009 come out with the names its replacements use.
"""
import struct

import numpy as np
import xxhash

from ghtex import _addr4, unswizzle4


def indices(tim3):
    p = 16
    total, clut, img = struct.unpack_from('<3I', tim3, p)
    hs = struct.unpack_from('<H', tim3, p + 12)[0]
    w, h = struct.unpack_from('<2H', tim3, p + 20)
    tex0 = struct.unpack_from('<Q', tim3, p + 24)[0]
    data = tim3[p + hs:p + hs + img]
    pal = tim3[p + hs + img:p + hs + img + clut]
    if w >= 128 and h >= 128:
        ind = unswizzle4(data, w, h)
    else:
        d = np.frombuffer(data, np.uint8)
        ind = np.zeros(len(d) * 2, np.uint8)
        ind[0::2] = d & 15
        ind[1::2] = d >> 4
        ind = ind.reshape(h, w)
    return ind, pal, tex0, w, h


def replacement_name(tim3, tcc=True):
    ind, pal, tex0, w, h = indices(tim3)
    tbw = (tex0 >> 14) & 63
    a4 = _addr4(w, h, tbw)
    memn = np.zeros(int(a4.max()) + 1 + 512, np.uint8)
    memn[a4.ravel()] = ind.ravel()
    mem = (memn[0::2] | (memn[1::2] << 4)).astype(np.uint8)
    hs = xxhash.xxh3_64()
    for by in range(h // 16):
        for bx in range(w // 32):
            a = int(a4[by * 16, bx * 32]) // 2 // 256 * 256
            hs.update(mem[a:a + 256].tobytes())
    bits = 20 | (int(np.log2(w)) << 6) | (int(np.log2(h)) << 10) | (0x4000 if tcc else 0)
    return '%x-%x-%08x' % (hs.intdigest(), xxhash.xxh3_64_intdigest(pal[:64]), bits)
