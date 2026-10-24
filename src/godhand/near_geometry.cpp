// Near geometry: walls, pillars and floors next to the camera are drawn instead of dropped.
//
// The PS2 cannot draw a triangle with a corner outside the GS's 16-bit screen range (0..4096 pixels), nor one reaching
// behind the camera, so God Hand's VU1 renderers drop such triangles whole (the GS's ADC bit on the vertex that
// completes them). The big renderers (skinned characters and the like) cut what crosses the screen edges with a
// clipper of their own; the simple ones that draw most of the scenery do not: any corner outside the clip volume
// drops the triangle, so a wall the camera comes close to vanishes (its corners are far off to the side or just
// behind the camera). Dump gif_7979 (Oct 2026): a red wall right of the camera, every one of its triangles dropped.
//
// Two parts:
// 1. The simple renderers' test "fcand vi01,0x03ffff" (a clip flag of any of the triangle's three corners) becomes
//    "fcand vi01,0x010410" (only the far plane, +z, as in the big renderers): nothing is dropped for the screen edges
//    or the near plane any more. The ADC bit is then the game's own strip stitching, the back-face test, or the far
//    plane. Patched in VU1 micro memory as each upload lands (ps2x::vu1hooks upload hook), and in the uploads the
//    build compiles ahead of time (build.py vu writes patched copies, *_nearclip.bin), so the compiled code matches.
// 2. Every PATH1 packet from those renderers goes through a filter here before the GS (the others, which clip by
//    themselves, are left alone: VU1 says which XGKICK sent each packet). A triangle the GS cannot take
//    (a corner outside the 16-bit range, or behind the camera) gets ADC in the packet and is cut by us instead, from
//    the full positions:
//    the VU writes 32-bit integers into the packet's X, Y and Z words and the GS only keeps 16 (Y, X) or 24 bits (Z),
//    so the packet still has them. In front of the camera, every attribute the GS interpolates (S, T, Q, Z, colour,
//    fog) is affine on screen, so the cut along the screen range is exact. A triangle reaching behind the camera is
//    first cut at the near plane (0.1 m) in clip space (W = 1/Q, X_clip = (x - 2048) W; texture coordinates S/Q,
//    colour and fog interpolated there), its depth then taken from its own Z = A + B Q line. The pieces go out as an
//    extra packet of plain triangles right after the original (same PRIM bits, type 3), and an A+D write puts PRIM
//    back.
//
// Behind the camera, the VU's back-face test is wrong for the triangle (it works on projected positions, mirrored
// for a corner behind the eye: an odd count of such corners flips it), and ADC also carries the game's stitching. The
// stitch flag of each vertex is read from the batch's input in VU1 data memory (bit 15 of the first input qword's W:
// "ilw.w vi15,0(vi04); iand vi14,vi15,vi03"). The input sits K qwords before the output (iaddiu vi05,vi04,K: 0xd0,
// 0xd6, 0x100), one vertex every S qwords after a 2-qword header (iaddiu vi04,vi04,S before the XGKICK: 3, 5, 6),
// both read from the micro memory at each upload; a packet is matched to its input by its GIF tag (the renderer
// copies the input's first qword as the tag) and checked: every stitched vertex must have ADC. Then: stitched = not
// drawn; else ADC = the projected back-face test, which an odd count of corners behind the camera flips. Without
// stitch information (another renderer, a mismatch), only a triangle with no ADC and an even count is drawn.
//
// [Video] NearGeometry (on): read at start.
#include "godhand/near_geometry.h"
#include "godhand/log.h"
#include "runtime/ps2_vu1_hooks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace godhand
{
    namespace
    {
        constexpr uint32_t kClipAny = 0x2403FFFFu;  // fcand vi01,0x03ffff
        constexpr uint32_t kClipFar = 0x24010410u;  // fcand vi01,0x010410
        constexpr uint32_t kXgkickVi05 = 0x80002EFCu;
        constexpr double kNear = 0.1;               // the game's near plane (metres = clip W)
        constexpr double kRangeLo = 8.0, kRangeHi = 4087.0; // the GS's screen range, pixels, with a margin

        std::atomic<uint64_t> g_patched{0}, g_cut{0}, g_cutNear{0}, g_noStitch{0};
        std::atomic<uint64_t> g_packets{0}, g_crossing{0}, g_crossingMatched{0};

        // ---- 1. Microcode ---------------------------------------------------------------------------------------

        bool isClipw(uint32_t upper) { return !(upper >> 31) && (upper & 0x7FFu) == 0x1FFu; }

        // All tables: per instruction pair of VU1 micro memory, used on the VU1 thread only (uploads, their analysis
        // and the PATH1 filter all run there, in order).
        constexpr uint32_t kPairs = 16384 / 8;
        std::array<bool, kPairs> g_patchedPair{};       // its clip test patched here
        std::array<uint32_t, kPairs> g_programLayout{}; // k | s << 16 of the patched renderer it is part of, 0 = none
        std::array<bool, kPairs> g_programPatched{};    // part of a program with a patched clip test
        std::array<uint16_t, kPairs> g_program{};       // the program each pair belongs to (its lowest pair)

        // A program is the code one renderer runs: the stretches between [E] bits (an end bit, then its delay slot)
        // united by the direct branches between them. The [E] bits alone are not a boundary: a renderer's vertex loop
        // has two exits, the plain one and the ADC path (a stitched or clipped last vertex), each with its own XGKICK
        // and [E], so the ADC tail sits after the first [E] (304bb8 at 0x400: kicks at 0x464 and 0x46c, [E] at 0x465
        // and 0x46d) and looked like another, unpatched program: every packet whose last vertex was stitched went to
        // the GS uncut, wrapped triangles across the screen (gif_13533, Oct 2026). The MSCNT continuation after an [E]
        // (`b top`) is a branch into the loop too.
        bool branchTarget(uint32_t lo, uint32_t up, uint32_t i, uint32_t &target)
        {
            if (up >> 31)
                return false; // the lower word is an immediate
            const uint32_t op = lo >> 25;
            if (!(op == 0x20u || op == 0x21u || (op >= 0x28u && op <= 0x29u) || (op >= 0x2Cu && op <= 0x2Fu)))
                return false; // b, bal, ibeq, ibne, ibltz, ibgtz, iblez, ibgez
            int32_t imm = static_cast<int32_t>(lo & 0x7FFu);
            if (imm & 0x400)
                imm -= 0x800;
            target = static_cast<uint32_t>(static_cast<int32_t>(i) + 1 + imm) & (kPairs - 1u);
            return true;
        }

        void findPrograms(const uint8_t *code, uint32_t pairs)
        {
            auto endBit = [code](uint32_t j) {
                uint32_t up;
                std::memcpy(&up, code + j * 8u + 4u, 4u);
                return ((up >> 30) & 1u) != 0u;
            };
            // Stretches between [E]s: each pair's stretch is named by the stretch's first pair.
            static std::array<uint16_t, kPairs> parent;
            uint32_t first = 0;
            for (uint32_t i = 0; i < pairs; ++i)
            {
                parent[i] = static_cast<uint16_t>(first);
                if (endBit(i) && i + 1u < pairs)
                {
                    parent[i + 1u] = static_cast<uint16_t>(first); // the delay slot
                    first = i + 2u;
                    ++i;
                }
            }
            auto find = [&](uint32_t a) {
                while (parent[a] != a)
                {
                    parent[a] = parent[parent[a]];
                    a = parent[a];
                }
                return a;
            };
            for (uint32_t i = 0; i < pairs; ++i)
            {
                uint32_t lo, up, target;
                std::memcpy(&lo, code + i * 8u, 4u);
                std::memcpy(&up, code + i * 8u + 4u, 4u);
                if (!branchTarget(lo, up, i, target) || target >= pairs)
                    continue;
                const uint32_t a = find(i), b = find(target);
                if (a != b)
                    parent[std::max(a, b)] = static_cast<uint16_t>(std::min(a, b));
            }
            for (uint32_t i = 0; i < pairs; ++i)
                g_program[i] = static_cast<uint16_t>(find(i));
        }

        // Only the renderers whose clip test was patched: their packets are matched to their input (stitch flags).
        // The big renderers clip by themselves (the PS2's own near-plane and screen clipper): a triangle they skip
        // (ADC) is drawn by them again as a clipped fan, so it must stay skipped. Dump gif_1652 (Oct 2026): a floor
        // triangle reaching behind the camera, skipped by such a renderer and redrawn by it as a fan, was cut and
        // drawn here too, over the fan: the two fought in depth, flickering darker and lighter.
        void analyse(const uint8_t *code, uint32_t codeSize)
        {
            const uint32_t pairs = std::min<uint32_t>(codeSize / 8u, kPairs);
            g_programLayout.fill(0u);
            g_programPatched.fill(false);
            findPrograms(code, pairs);
            for (uint32_t i = 0; i < pairs; ++i)
                if (g_patchedPair[i])
                    for (uint32_t j = 0; j < pairs; ++j)
                        if (g_program[j] == g_program[i])
                            g_programPatched[j] = true;
            auto pair = [code](uint32_t i, uint32_t &lo, uint32_t &up) {
                std::memcpy(&lo, code + i * 8u, 4u);
                std::memcpy(&up, code + i * 8u + 4u, 4u);
            };
            // The layout of each patched renderer, from the code before its XGKICK (any register) in its own program:
            // the nearest "iaddiu vi05,vi04,K" (output = input + K) and "iaddiu vi04,vi04,S" (one vertex every S
            // qwords). 304bb8's S sits 19 pairs before the kick, so no fixed window: the whole program.
            for (uint32_t i = 0; i < pairs; ++i)
            {
                uint32_t lo, up;
                pair(i, lo, up);
                if ((lo & 0xFFFF07FFu) != 0x800006FCu || (up >> 31)) // xgkick viN
                    continue;
                if (!g_programPatched[i])
                    continue;
                uint32_t s = 0, k = 0;
                for (uint32_t j = i; j-- > 0u && !(s && k);)
                {
                    if (g_program[j] != g_program[i])
                        continue;
                    pair(j, lo, up);
                    if (up >> 31)
                        continue;
                    if (!s && (lo & 0xFFFFF800u) == 0x10042000u && (lo & 0x7FFu) >= 3u && (lo & 0x7FFu) <= 8u)
                        s = lo & 0x7FFu; // iaddiu vi04,vi04,S
                    if (!k && (lo & 0xFFFFF800u) == 0x10052000u)
                        k = lo & 0x7FFu; // iaddiu vi05,vi04,K
                }
                if (!s || !k)
                    continue;
                const uint32_t layout = k | (s << 16);
                for (uint32_t j = 0; j < pairs; ++j)
                    if (g_program[j] == g_program[i])
                    {
                        if (g_programLayout[j] && g_programLayout[j] != layout)
                            GH_WARN(Game, "near geometry: program at pair 0x" << std::hex << g_program[i] << " has kicks with different layouts (0x"
                                                                             << g_programLayout[j] << " and 0x" << layout << ")" << std::dec);
                        g_programLayout[j] = layout;
                    }
            }
        }

        void uploadHook(uint8_t *code, uint32_t codeSize, uint32_t byteAddress, uint32_t size)
        {
            const uint32_t pairs = std::min<uint32_t>(codeSize / 8u, static_cast<uint32_t>(g_patchedPair.size()));
            const uint32_t first = byteAddress / 8u, last = std::min(pairs, (byteAddress + size + 7u) / 8u);
            for (uint32_t i = first; i < last; ++i)
                g_patchedPair[i] = false;
            for (uint32_t i = first; i < last; ++i)
            {
                uint32_t lo, up;
                std::memcpy(&lo, code + i * 8u, 4u);
                std::memcpy(&up, code + i * 8u + 4u, 4u);
                if (lo != kClipAny || (up >> 31))
                    continue;
                bool clip = false;
                for (uint32_t j = i > 64u ? i - 64u : 0u; j < std::min(pairs, i + 64u) && !clip; ++j)
                {
                    uint32_t u2;
                    std::memcpy(&u2, code + j * 8u + 4u, 4u);
                    clip = isClipw(u2);
                }
                if (!clip)
                    continue;
                std::memcpy(code + i * 8u, &kClipFar, 4u);
                g_patchedPair[i] = true;
                if (g_patched.fetch_add(1, std::memory_order_relaxed) < 8u)
                    GH_TRACE(Game, "near geometry: renderer clip test patched at pair 0x" << std::hex << i << std::dec);
            }
            analyse(code, codeSize);
        }

        // ---- 2. The PATH1 filter --------------------------------------------------------------------------------

        struct Vertex
        {
            int64_t x = 0, y = 0;  // 12.4, full
            double z = 0.0;        // full
            float s = 0.0f, t = 0.0f, q = 1.0f;
            bool hasQ = false;
            double r = 0, g = 0, b = 0, a = 0, f = 0, u = 0, v = 0;
            bool adc = false, stitch = false, stitchKnown = false;
            uint32_t xyzOffset = 0; // the vertex's XYZ qword in the packet
        };

        // A vertex as the cut works on it: screen (x, y, z, S, T, Q) or clip space (x, y = X, Y clip; w; S, T = s, t).
        struct Cut
        {
            double x, y, z, w, s, t, q, r, g, b, a, f, u, v;
        };

        Cut lerp(const Cut &p, const Cut &q, double k)
        {
            Cut o;
            o.x = p.x + (q.x - p.x) * k;
            o.y = p.y + (q.y - p.y) * k;
            o.z = p.z + (q.z - p.z) * k;
            o.w = p.w + (q.w - p.w) * k;
            o.s = p.s + (q.s - p.s) * k;
            o.t = p.t + (q.t - p.t) * k;
            o.q = p.q + (q.q - p.q) * k;
            o.r = p.r + (q.r - p.r) * k;
            o.g = p.g + (q.g - p.g) * k;
            o.b = p.b + (q.b - p.b) * k;
            o.a = p.a + (q.a - p.a) * k;
            o.f = p.f + (q.f - p.f) * k;
            o.u = p.u + (q.u - p.u) * k;
            o.v = p.v + (q.v - p.v) * k;
            return o;
        }

        // Sutherland-Hodgman against value(v) >= 0, value affine in the interpolated quantities.
        template <typename F>
        void clipAgainst(std::vector<Cut> &poly, std::vector<Cut> &scratch, F value)
        {
            scratch.clear();
            for (size_t i = 0; i < poly.size(); ++i)
            {
                const Cut &p = poly[i], &q = poly[(i + 1) % poly.size()];
                const double vp = value(p), vq = value(q);
                if (vp >= 0.0)
                    scratch.push_back(p);
                if ((vp >= 0.0) != (vq >= 0.0))
                    scratch.push_back(lerp(p, q, vp / (vp - vq)));
            }
            poly.swap(scratch);
        }

        struct TagInfo
        {
            uint64_t lo, hi;
            uint32_t nreg;
            uint32_t prim; // PRIM for its triangles
        };

        struct Out
        {
            std::vector<uint8_t> &bytes;
            const TagInfo *tag = nullptr;
            size_t tagAt = 0;
            uint32_t vertices = 0; // in the current tag
            uint32_t total = 0;    // in the extra packet
        };

        void put64(std::vector<uint8_t> &out, uint64_t lo, uint64_t hi)
        {
            const size_t at = out.size();
            out.resize(at + 16u);
            std::memcpy(out.data() + at, &lo, 8u);
            std::memcpy(out.data() + at + 8u, &hi, 8u);
        }

        uint64_t floatBits(float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, 4u);
            return bits;
        }

        uint64_t clampInt(double value, double low, double high)
        {
            return static_cast<uint64_t>(std::llround(std::clamp(value, low, high)));
        }

        // One vertex of the extra packet, in the source tag's register order.
        void emitVertex(Out &out, const Cut &c, const Vertex &flatColour, bool flat)
        {
            const TagInfo &tag = *out.tag;
            const double r = flat ? flatColour.r : c.r, g = flat ? flatColour.g : c.g;
            const double b = flat ? flatColour.b : c.b, a = flat ? flatColour.a : c.a;
            for (uint32_t i = 0; i < tag.nreg; ++i)
            {
                const uint32_t desc = static_cast<uint32_t>((tag.hi >> (i * 4u)) & 0xFu);
                uint64_t lo = 0, hi = 0;
                switch (desc)
                {
                case 0x1: // RGBAQ
                    lo = clampInt(r, 0, 255) | (clampInt(g, 0, 255) << 32);
                    hi = clampInt(b, 0, 255) | (clampInt(a, 0, 255) << 32);
                    break;
                case 0x2: // ST (+Q)
                    lo = floatBits(static_cast<float>(c.s)) | (floatBits(static_cast<float>(c.t)) << 32);
                    hi = floatBits(static_cast<float>(c.q));
                    break;
                case 0x3: // UV
                    lo = clampInt(c.u * 16.0, 0, 0x3FFF) | (clampInt(c.v * 16.0, 0, 0x3FFF) << 32);
                    break;
                case 0x4: // XYZF2
                    lo = clampInt(c.x * 16.0, 0, 0xFFFF) | (clampInt(c.y * 16.0, 0, 0xFFFF) << 32);
                    hi = (clampInt(c.z, 0, 0xFFFFFF) << 4) | (clampInt(c.f, 0, 255) << 36);
                    break;
                case 0x5: // XYZ2
                    lo = clampInt(c.x * 16.0, 0, 0xFFFF) | (clampInt(c.y * 16.0, 0, 0xFFFF) << 32);
                    hi = clampInt(c.z, 0, 4294967295.0);
                    break;
                case 0xA: // FOG
                    hi = clampInt(c.f, 0, 255) << 36;
                    break;
                default: // NOP and the like: nothing
                    break;
                }
                put64(out.bytes, lo, hi);
            }
            ++out.vertices;
            ++out.total;
        }

        // Screen-space vertex from a packet vertex (in front of the camera).
        Cut screenOf(const Vertex &v)
        {
            Cut c{};
            c.x = static_cast<double>(v.x) / 16.0;
            c.y = static_cast<double>(v.y) / 16.0;
            c.z = v.z;
            c.s = v.s;
            c.t = v.t;
            c.q = v.q;
            c.w = 1.0;
            c.r = v.r; c.g = v.g; c.b = v.b; c.a = v.a; c.f = v.f; c.u = v.u; c.v = v.v;
            return c;
        }

        void emitPolygon(Out &out, std::vector<Cut> &poly, std::vector<Cut> &scratch, const Vertex &kick, bool flat)
        {
            clipAgainst(poly, scratch, [](const Cut &c) { return c.x - kRangeLo; });
            clipAgainst(poly, scratch, [](const Cut &c) { return kRangeHi - c.x; });
            clipAgainst(poly, scratch, [](const Cut &c) { return c.y - kRangeLo; });
            clipAgainst(poly, scratch, [](const Cut &c) { return kRangeHi - c.y; });
            for (size_t i = 1; i + 1 < poly.size(); ++i)
            {
                emitVertex(out, poly[0], kick, flat);
                emitVertex(out, poly[i], kick, flat);
                emitVertex(out, poly[i + 1], kick, flat);
            }
        }

        // The triangle (a, b, c), c completing it: cut and sent if it can be drawn. Its ADC is set by the caller.
        void cutTriangle(Out &out, const Vertex &a, const Vertex &b, const Vertex &c, bool flat, std::vector<Cut> &poly,
                         std::vector<Cut> &scratch)
        {
            const Vertex *v[3] = {&a, &b, &c};
            for (const Vertex *p : v)
                if (!std::isfinite(p->q) || p->q == 0.0f || !std::isfinite(p->s) || !std::isfinite(p->t) || !std::isfinite(p->z))
                    return; // on the camera plane or garbage: no position to rebuild, left dropped
            int behind = 0;
            bool tooNear = false; // a corner between the camera and the near plane: its depth is past the 16-bit range
            for (const Vertex *p : v)
            {
                behind += (p->q <= 0.0f) ? 1 : 0;
                tooNear = tooNear || (p->hasQ && p->q > 1.0f / static_cast<float>(kNear));
            }
            if (behind == 0 && !tooNear)
            {
                if (c.adc)
                    return; // stitched, a back face, or past the far plane: exact for a triangle in front
                poly.clear();
                for (const Vertex *p : v)
                    poly.push_back(screenOf(*p));
                emitPolygon(out, poly, scratch, c, flat);
                g_cut.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (behind == 3)
                return;
            if (behind == 0)
            {
                // All in front, one corner nearer than the near plane: the VU's tests are right (nothing mirrored).
                if (c.adc)
                    return;
            }
            // Reaching behind the camera.
            else if (c.stitchKnown)
            {
                if (c.stitch)
                    return;
                const bool backFace = c.adc != ((behind & 1) != 0); // the projected test, flipped by an odd count
                if (backFace)
                    return;
            }
            else
            {
                g_noStitch.fetch_add(1, std::memory_order_relaxed);
                if (c.adc || (behind & 1))
                    return;
            }
            // Its depth line Z = A + B Q, from the two corners farthest apart in Q.
            int i0 = 0, i1 = 1;
            double best = -1.0;
            for (int i = 0; i < 3; ++i)
                for (int j = i + 1; j < 3; ++j)
                    if (std::fabs(v[i]->q - v[j]->q) > best)
                    {
                        best = std::fabs(v[i]->q - v[j]->q);
                        i0 = i;
                        i1 = j;
                    }
            if (!(best > 1e-6))
                return;
            const double B = (v[i0]->z - v[i1]->z) / (static_cast<double>(v[i0]->q) - v[i1]->q);
            const double A = v[i0]->z - B * v[i0]->q;
            // Fog of a corner behind the camera is garbage (the VU's fog is computed from its depth, then clamped):
            // it takes the fog of the nearest corner in front instead, so the cut piece does not darken.
            double nearFog = 0.0, nearQ = -1.0;
            for (const Vertex *p : v)
                if (p->q > 0.0f && p->q > nearQ)
                {
                    nearQ = p->q;
                    nearFog = p->f;
                }
            poly.clear();
            for (const Vertex *p : v)
            {
                Cut k{};
                k.w = 1.0 / p->q;
                k.x = (static_cast<double>(p->x) / 16.0 - 2048.0) * k.w;
                k.y = (static_cast<double>(p->y) / 16.0 - 2048.0) * k.w;
                k.s = p->s / p->q; // true texture coordinates
                k.t = p->t / p->q;
                k.r = p->r; k.g = p->g; k.b = p->b; k.a = p->a; k.u = p->u; k.v = p->v;
                k.f = p->q > 0.0f ? p->f : nearFog;
                poly.push_back(k);
            }
            clipAgainst(poly, scratch, [](const Cut &k) { return k.w - kNear; });
            for (Cut &k : poly)
            {
                k.q = 1.0 / k.w;
                k.x = k.x * k.q + 2048.0;
                k.y = k.y * k.q + 2048.0;
                k.s *= k.q;
                k.t *= k.q;
                k.z = A + B * k.q;
            }
            for (const Cut &k : poly)
                if (!std::isfinite(k.x) || !std::isfinite(k.y) || !std::isfinite(k.z) || !std::isfinite(k.s) || !std::isfinite(k.t) ||
                    !std::isfinite(k.q))
                    return;
            emitPolygon(out, poly, scratch, c, flat);
            g_cutNear.fetch_add(1, std::memory_order_relaxed);
        }

        int32_t s32(uint32_t value) { return static_cast<int32_t>(value); }

        // The batch's input for this packet (its stitch flags), when a known renderer layout matches.
        // kickPc: the address of the XGKICK that sent the packet (bytes), inside the program that sent it.
        bool findInput(const uint8_t *packet, uint32_t size, uint32_t sourceAddress, const uint8_t *vuData, uint32_t vuDataSize,
                       uint32_t kickPc, uint32_t &inputQw, uint32_t &stride)
        {
            const uint32_t qwords = vuDataSize / 16u;
            if (qwords == 0u || size < 16u)
                return false;
            const uint32_t value = g_programLayout[(kickPc / 8u) % g_programLayout.size()];
            if (!value)
                return false; // not one of the patched renderers
            const uint32_t out = sourceAddress / 16u;
            const uint32_t k = value & 0xFFFFu, s = value >> 16;
            const uint32_t in = (out + qwords - (k % qwords)) % qwords;
            if (std::memcmp(vuData + in * 16u, packet, 16u) != 0)
                return false;
            inputQw = in;
            stride = s;
            return true;
        }

        thread_local uint32_t t_prim = 0;   // PRIM as the GS has it (this thread's PATH1)
        thread_local bool t_primKnown = false;

        void path1Filter(uint8_t *packet, uint32_t size, uint32_t sourceAddress, const uint8_t *vuData, uint32_t vuDataSize,
                         uint32_t kickPc, std::vector<uint8_t> &extra)
        {
            // The other renderers (the big ones, 2D, effects) are left as the PS2 has them: their clip tests were not
            // changed, and the big ones redraw what they skip as clipped fans of their own. By the XGKICK's own address:
            // the start of the VU1 run is not enough (the renderers end with [E] and a branch back to their top, and
            // the game goes on with MSCNT there, outside the program as the [E] bits split it: the patched renderers'
            // packets went to the GS untouched, wrapped triangles across the screen; Oct 2026, gif_8136).
            if (!g_programPatched[(kickPc / 8u) % g_programPatched.size()])
                return;
            thread_local std::vector<Cut> poly, scratch;
            Out out{extra};
            TagInfo outTag{};
            uint32_t inputQw = 0, stride = 0;
            const bool haveInput = findInput(packet, size, sourceAddress, vuData, vuDataSize, kickPc, inputQw, stride);
            bool crossing = false;
            const uint32_t vuQwords = vuDataSize / 16u;
            bool firstTag = true;
            uint32_t pos = 0;
            std::array<Vertex, 3> queue{};
            uint32_t queued = 0;
            Vertex fan0{};
            while (pos + 16u <= size)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, packet + pos, 8u);
                std::memcpy(&hi, packet + pos + 8u, 8u);
                pos += 16u;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                const bool eop = (lo >> 15) & 1u;
                const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                if ((lo >> 46) & 1u)
                {
                    t_prim = static_cast<uint32_t>((lo >> 47) & 0x7FFu);
                    t_primKnown = true;
                    queued = 0;
                }
                if (flg != 0u)
                {
                    // REGLIST / IMAGE: not vertices we cut; skip over.
                    const uint64_t bytes = flg == 1u ? ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u
                                                     : static_cast<uint64_t>(nloop) * 16u;
                    pos += static_cast<uint32_t>(std::min<uint64_t>(bytes, size - pos));
                    firstTag = false;
                    if (eop)
                        break;
                    continue;
                }
                const uint32_t type = t_prim & 7u;
                const bool triangles = t_primKnown && type >= 3u && type <= 5u;
                // Only a tag of plain vertex registers (and one kicking XYZ) can be sent again as it is laid out.
                bool cuttable = false;
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    const uint32_t desc = static_cast<uint32_t>((hi >> (r * 4u)) & 0xFu);
                    if (desc == 0x4u || desc == 0x5u)
                        cuttable = true;
                    else if (desc != 0x1u && desc != 0x2u && desc != 0x3u && desc != 0xAu && desc != 0xFu)
                    {
                        cuttable = false;
                        break;
                    }
                }
                const bool flat = !((t_prim >> 3) & 1u);
                TagInfo tag{lo, hi, nreg, t_prim};
                Vertex cur{};
                for (uint32_t loop = 0; loop < nloop && pos + nreg * 16u <= size; ++loop)
                {
                    for (uint32_t r = 0; r < nreg; ++r, pos += 16u)
                    {
                        const uint32_t desc = static_cast<uint32_t>((hi >> (r * 4u)) & 0xFu);
                        uint32_t w[4];
                        std::memcpy(w, packet + pos, 16u);
                        switch (desc)
                        {
                        case 0x0: // PRIM
                            t_prim = w[0] & 0x7FFu;
                            t_primKnown = true;
                            queued = 0;
                            break;
                        case 0x1:
                            cur.r = w[0] & 0xFFu; cur.g = w[1] & 0xFFu; cur.b = w[2] & 0xFFu; cur.a = w[3] & 0xFFu;
                            break;
                        case 0x2:
                            std::memcpy(&cur.s, &w[0], 4u);
                            std::memcpy(&cur.t, &w[1], 4u);
                            std::memcpy(&cur.q, &w[2], 4u);
                            cur.hasQ = true;
                            break;
                        case 0x3:
                            cur.u = (w[0] & 0x3FFFu) / 16.0; cur.v = (w[1] & 0x3FFFu) / 16.0;
                            break;
                        case 0xA:
                            cur.f = (w[3] >> 4) & 0xFFu;
                            break;
                        case 0x4: case 0x5: case 0xC: case 0xD:
                        {
                            Vertex v = cur;
                            v.x = s32(w[0]);
                            v.y = s32(w[1]);
                            if (desc == 0x4 || desc == 0xC)
                            {
                                v.z = static_cast<double>(s32(w[2]) >> 4);
                                v.f = (w[3] >> 4) & 0xFFu;
                            }
                            else
                                v.z = static_cast<double>(w[2]);
                            v.adc = desc >= 0xC || ((w[3] >> 15) & 1u);
                            v.xyzOffset = pos;
                            if (firstTag && haveInput && stride)
                            {
                                const uint32_t at = (inputQw + 2u + loop * stride) % vuQwords;
                                uint32_t flags;
                                std::memcpy(&flags, vuData + at * 16u + 12u, 4u);
                                v.stitch = (flags & 0x8000u) != 0u;
                                v.stitchKnown = true;
                            }
                            if (!triangles)
                                break;
                            // The triangle this vertex completes.
                            const Vertex *ta = nullptr, *tb = nullptr;
                            if (type == 4u && queued >= 2u)
                            {
                                ta = &queue[(queued - 2u) % 3u];
                                tb = &queue[(queued - 1u) % 3u];
                            }
                            else if (type == 5u && queued >= 2u)
                            {
                                ta = &fan0;
                                tb = &queue[(queued - 1u) % 3u];
                            }
                            else if (type == 3u && (queued % 3u) == 2u)
                            {
                                ta = &queue[(queued - 2u) % 3u];
                                tb = &queue[(queued - 1u) % 3u];
                            }
                            if (ta)
                            {
                                const Vertex *tri[3] = {ta, tb, &v};
                                bool outside = false, unknown = false;
                                for (const Vertex *p : tri)
                                {
                                    if (p->x < 0 || p->x > 0xFFFF || p->y < 0 || p->y > 0xFFFF ||
                                        (p->hasQ && (p->q <= 0.0f || p->q > 1.0f / static_cast<float>(kNear))))
                                        outside = true;
                                    if (!p->hasQ && p->z <= 0.0)
                                        unknown = true; // behind the camera, no Q to rebuild it from
                                    if (std::llabs(p->x) > 0x7FFFFF00ll || std::llabs(p->y) > 0x7FFFFF00ll)
                                        unknown = true; // saturated: on the camera plane
                                }
                                if (outside || unknown)
                                {
                                    crossing = true;
                                    // The GS must not draw it as it is.
                                    packet[v.xyzOffset + 13u] |= 0x80u; // ADC: bit 15 of the fourth word
                                    if (!unknown && cuttable)
                                    {
                                        if (!out.tag || std::memcmp(&outTag, &tag, sizeof(tag)) != 0)
                                        {
                                            // A new tag in the extra packet for this source tag's layout.
                                            if (out.tag)
                                            {
                                                uint64_t done;
                                                std::memcpy(&done, extra.data() + out.tagAt, 8u);
                                                done |= out.vertices & 0x7FFFu;
                                                std::memcpy(extra.data() + out.tagAt, &done, 8u);
                                            }
                                            outTag = tag;
                                            out.tag = &outTag;
                                            out.tagAt = extra.size();
                                            out.vertices = 0;
                                            const uint64_t prim = (static_cast<uint64_t>(tag.prim) & ~7ull) | 3u;
                                            put64(extra, (1ull << 46) | (prim << 47) | (static_cast<uint64_t>(nreg & 0xFu) << 60), hi);
                                        }
                                        cutTriangle(out, *ta, *tb, v, flat, poly, scratch);
                                    }
                                }
                            }
                            if (type == 5u && queued == 0u)
                                fan0 = v;
                            queue[queued % 3u] = v;
                            ++queued;
                            break;
                        }
                        default:
                            break;
                        }
                    }
                }
                firstTag = false;
                if (eop)
                    break;
            }
            if (crossing)
            {
                g_crossing.fetch_add(1, std::memory_order_relaxed);
                if (haveInput)
                    g_crossingMatched.fetch_add(1, std::memory_order_relaxed);
            }
            if (g_packets.fetch_add(1, std::memory_order_relaxed) % 200000u == 199999u)
                GH_INFO(Game, "near geometry: " << g_patched.load() << " clip tests patched; " << g_crossing.load()
                                                << " packets needed cutting, " << g_crossingMatched.load()
                                                << " of them matched to their input (stitch flags); triangles cut: "
                                                << g_cut.load() << " at the screen's edges, " << g_cutNear.load()
                                                << " at the camera; " << g_noStitch.load()
                                                << " reaching behind it without stitch flags");
            if (!out.tag)
            {
                extra.clear();
                return;
            }
            // Close the last tag, EOP, then PRIM back as the GS had it after the original packet.
            uint64_t done;
            std::memcpy(&done, extra.data() + out.tagAt, 8u);
            done |= out.vertices & 0x7FFFu;
            std::memcpy(extra.data() + out.tagAt, &done, 8u);
            if (out.total == 0u)
            {
                extra.clear();
                return;
            }
            put64(extra, 1u | (1ull << 15) | (1ull << 60), 0xEull); // NLOOP 1, EOP, A+D
            put64(extra, t_prim, 0x00);                             // PRIM
        }
    }

    void installNearGeometry()
    {
        const char *text = std::getenv("GODHAND_NEAR_GEOMETRY");
        if (text && *text == '0')
        {
            GH_INFO(Game, "near geometry: off (walls next to the camera drop out as on the PS2)");
            return;
        }
        ps2x::vu1hooks::setUploadHook(&uploadHook);
        ps2x::vu1hooks::setPath1Filter(&path1Filter);
        GH_INFO(Game, "near geometry: on (triangles at the screen edges and behind the camera are cut and drawn)");
    }

    void nearGeometryProgramAt(uint32_t pair, uint32_t &program, bool &patched, uint32_t &layout)
    {
        pair %= kPairs;
        program = g_program[pair];
        patched = g_programPatched[pair];
        layout = g_programLayout[pair];
    }

    void nearGeometryStats(uint64_t &patched, uint64_t &cut, uint64_t &cutNear, uint64_t &noStitch)
    {
        patched = g_patched.load();
        cut = g_cut.load();
        cutNear = g_cutNear.load();
        noStitch = g_noStitch.load();
    }
}
