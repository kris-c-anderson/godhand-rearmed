#include "ps2_vu1_impl.inl"
#include "ps2_runtime.h"

// The runtime library references the generated EE function table (register_functions.cpp of a game); this offline
// tool runs no EE code, so it gets an empty one.
const uint32_t g_ps2RecompiledFunctionTableBase = 0u;
const uint32_t g_ps2RecompiledFunctionTableEnd = 0u;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

#include <cstdlib>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using A = VU1InterpreterAccess;

    constexpr uint32_t kCodeSize = 16u * 1024u;
    constexpr uint32_t kDefaultBlockPairs = 128u;
    constexpr uint32_t kUploadGridPairs = 32u; // block grid for uploads (compileUnit)
    constexpr uint32_t kMaxVfLatency = 4u;
    constexpr uint32_t kMaxViLatency = 4u;
    constexpr uint32_t kMaxAccLatency = 1u;
    constexpr uint32_t kFmacLatency = 4u;

    // Stage (c) of the microVU-style work (docs/PROGRESS.md §45):
    // - gFastFmac: FMACs whose flags are folded in a dead-flag segment use fmacFastV (the plain SSE operation, no
    //   range checks; VU1_RECOMP_EXACT_FMAC=1 keeps fmacV);
    // - gRawLoads (now OFF by default; VU1_RECOMP_RAW_LOADS=1 turns it on): LQ/LQI/LQD keep the loaded bits as the
    //   normalized value, relying on DAZ for denormal operands. Urban Reign verified it (PS2X_VU1_VERIFY=obs, 0
    //   mismatches), but that check restores the interpreter's normalization at every call. God Hand (2026-10-17,
    //   round 9) loads integer data whose exponent field is 255 (negative ints such as 0xFFFFFF8F) into VF registers
    //   that a later program reads with an FMAC: the character program 4dfe leaves vf31 = (0xFFFFFF8F, 0xFFFFFFC8, 0,
    //   0xFFFFF334) and the stage program cd8f runs MUL vf31, vf31, vf0x at 0x368. The interpreter (and the VU) read
    //   those lanes as finite numbers (vnorm: +-0x7F7FFFFF), so the product is -0; the raw bits reach SSE as NaN, the
    //   normalized-register shadow (vu1NormShadow) carries them across calls, vf31 stays NaN and the stage program's
    //   clipping path emits no triangle fan in that frame (floor/walls near the camera missing, clear colour visible;
    //   port_prep/round9, chained replay of rec/r9g/vu1trig_7322.bin). Normalized loads keep the shadow equal to
    //   vnorm(raw), as every other write does.
    const bool gFastFmac = [] { const char *v = std::getenv("VU1_RECOMP_EXACT_FMAC"); return !(v && *v == '1'); }();
    const bool gRawLoads = [] { const char *v = std::getenv("VU1_RECOMP_RAW_LOADS"); return v && *v == '1'; }();
    // - gSticky: keep accumulating the sticky zero/sign status bits in fast FMACs (VU1_RECOMP_STICKY=1). By default an
    //   image without any status reader (FSEQ/FSSET/FSAND/FSOR) uses fmacFastN; the runtime logs any access to VU1's
    //   register window from VU0 (noteVu0DataAddress), the only way to read VU1's status from outside.
    const bool gSticky = [] { const char *v = std::getenv("VU1_RECOMP_STICKY"); return v && *v == '1'; }();
    // - gVu0Fast: images that fit in VU0's 4 KB (VU0 programs) also get the fast FMACs. Their flags are only visible to
    //   the program itself (handled by the dead-flag analysis) and to the EE through CFC2 vi16/vi17, which Urban Reign
    //   never executes (it reads only the clip flags, which fast FMACs do not touch; docs/PROGRESS.md §62). Such an
    //   image is emitted twice: fast under its hash, exact under hash ^ kVu0ExactTag, which the runtime runs with
    //   PS2X_VU0_FAST=0. VU1_RECOMP_VU0_EXACT=1: only the exact one, under the plain hash (the previous output).
    const bool gVu0Fast = [] { const char *v = std::getenv("VU1_RECOMP_VU0_EXACT"); return !(v && *v == '1'); }();
    // - gAllReady: at the end of a segment store the ready cycle of every register it wrote (VU1_RECOMP_ALL_READY=1, the
    //   previous output). By default only writes in the last kMax*Latency positions: an earlier one is ready by the
    //   segment's end, and so is the older value the store would have replaced (written before the segment began, at
    //   least kMax*Latency cycles earlier), so every later stall check reads the same as with the store.
    const bool gAllReady = [] { const char *v = std::getenv("VU1_RECOMP_ALL_READY"); return v && *v == '1'; }();
    constexpr uint64_t kVu0ExactTag = 0x5655304558414354ull; // "VU0EXACT"; same constant in ps2_vu1_core.cpp
    constexpr uint32_t kSegmentJoinLimit = 24u;
    constexpr size_t kPartBytes = 400u * 1024u;

    uint64_t fnv1a64(const uint8_t *data, size_t size)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    std::string hex64(uint64_t value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%016" PRIX64 "ull", value);
        return text;
    }

    std::string hx(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%Xu", value);
        return text;
    }

    std::string hexPcId(uint32_t index)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%04X", index * 8u);
        return text;
    }

    std::string label(uint32_t pc)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "pc_%04X", pc);
        return text;
    }

    std::string num(int64_t value)
    {
        return std::to_string(value);
    }

    uint32_t laneBit(uint32_t component)
    {
        return 1u << (3u - component);
    }

    uint32_t nextPc(uint32_t pc)
    {
        return pc + 8u >= kCodeSize ? 0u : pc + 8u;
    }

    uint8_t fDest(uint32_t i) { return static_cast<uint8_t>((i >> 21) & 0xFu); }
    uint8_t fFt(uint32_t i) { return static_cast<uint8_t>((i >> 16) & 0x1Fu); }
    uint8_t fFs(uint32_t i) { return static_cast<uint8_t>((i >> 11) & 0x1Fu); }
    uint8_t fFd(uint32_t i) { return static_cast<uint8_t>((i >> 6) & 0x1Fu); }
    uint8_t fVit(uint32_t i) { return static_cast<uint8_t>((i >> 16) & 0xFu); }
    uint8_t fVis(uint32_t i) { return static_cast<uint8_t>((i >> 11) & 0xFu); }
    uint8_t fVid(uint32_t i) { return static_cast<uint8_t>((i >> 6) & 0xFu); }
    int32_t fImm11(uint32_t i) { return static_cast<int32_t>(i << 21) >> 21; }

    std::string vf(uint32_t reg) { return "vu.m_state.vf[" + num(reg) + "]"; }

    std::string vfMem(uint32_t reg) { return reg == 32u ? std::string("vu.m_state.acc") : vf(reg); }

    struct RegView
    {
        bool local = false;
        bool record = false;
        std::set<uint32_t> vfUsed;
        std::set<uint32_t> vfWritten;
        std::set<uint32_t> viUsed;
        std::set<uint32_t> viWritten;
        std::set<uint32_t> vfClean;
        std::set<uint32_t> vfRawStored;
    };

    RegView gView;

    struct RegViewProbe
    {
        RegView saved = gView;
        ~RegViewProbe() { gView = saved; }
    };

    std::string vi(uint32_t reg)
    {
        if (!gView.local)
            return "vu.m_state.vi[" + num(reg) + "]";
        if (reg == 0u)
            return "0";
        if (gView.record)
            gView.viUsed.insert(reg);
        return "vi" + num(reg);
    }

    std::string viW(uint32_t reg)
    {
        if (gView.local && gView.record && reg != 0u)
            gView.viWritten.insert(reg);
        return vi(reg);
    }

    void noteVf(uint32_t reg, bool write)
    {
        if (!gView.record || reg == 0u)
            return;
        gView.vfUsed.insert(reg);
        if (write)
            gView.vfWritten.insert(reg);
    }

    std::string rawV(uint32_t reg)
    {
        if (!gView.local)
            return reg == 32u ? std::string("_mm_loadu_ps(vu.m_state.acc)") : "_mm_loadu_ps(" + vf(reg) + ")";
        if (reg == 0u)
            return "kVf0()";
        noteVf(reg, false);
        if (gView.vfRawStored.count(reg))
            return "_mm_loadu_ps(" + vfMem(reg) + ")";
        return (gView.vfClean.count(reg) ? "vn" : "vr") + num(reg);
    }

    std::string normV(uint32_t reg)
    {
        if (!gView.local)
            return reg == 32u ? std::string("nacc(ctx)") : "nv(ctx, " + num(reg) + "u)";
        if (reg == 0u)
            return "kVf0()";
        noteVf(reg, false);
        return "vn" + num(reg);
    }

    std::string normBc(uint32_t reg, uint32_t bc)
    {
        if (!gView.local)
            return "nbc(ctx, " + num(reg) + "u, " + num(bc) + "u)";
        return "bcast<" + num(bc) + "u>(" + normV(reg) + ")";
    }

    std::string laneBits(uint32_t reg, uint32_t component)
    {
        if (!gView.local)
            return "std::bit_cast<uint32_t>(" + vf(reg) + "[" + num(component) + "])";
        return "static_cast<uint32_t>(_mm_extract_ps(" + rawV(reg) + ", " + num(component) + "))";
    }

    std::string laneFloat(uint32_t reg, uint32_t component)
    {
        if (!gView.local)
            return vf(reg) + "[" + num(component) + "]";
        return "laneF<" + num(component) + "u>(" + rawV(reg) + ")";
    }

    std::string writeV(uint32_t reg, const std::string &raw, const std::string &norm, const std::string &dest)
    {
        if (!gView.local)
            return "writeVf(vu, ctx, " + num(reg) + "u, " + raw + ", " + norm + ", " + dest + ");";
        if (reg == 0u)
            return "";
        noteVf(reg, true);
        const std::string r = "vr" + num(reg);
        const std::string n = "vn" + num(reg);
        const bool full = dest == hx(0xFu);
        const bool clean = gView.vfClean.count(reg) != 0u;
        const bool stored = gView.vfRawStored.count(reg) != 0u;
        if (raw == norm && (full || clean))
        {
            gView.vfClean.insert(reg);
            gView.vfRawStored.erase(reg);
            return n + " = " + (full ? raw : "blendDest(" + n + ", " + raw + ", " + dest + ")") + ";";
        }
        gView.vfClean.erase(reg);
        if (full)
        {
            gView.vfRawStored.insert(reg);
            return "_mm_storeu_ps(" + vfMem(reg) + ", " + raw + "); " + n + " = " + norm + ";";
        }
        gView.vfRawStored.erase(reg);
        const std::string oldRaw = clean ? n : (stored ? "_mm_loadu_ps(" + vfMem(reg) + ")" : r);
        return r + " = blendDest(" + oldRaw + ", " + raw + ", " + dest + "); " + n + " = blendDest(" + n + ", " + norm + ", " + dest + ");";
    }

    std::string writeFullV(uint32_t reg, const std::string &raw, const std::string &norm)
    {
        if (!gView.local)
            return "writeVfFull(vu, ctx, " + num(reg) + "u, " + raw + ", " + norm + ");";
        if (reg == 0u)
            return "";
        noteVf(reg, true);
        if (raw == norm)
        {
            gView.vfClean.insert(reg);
            gView.vfRawStored.erase(reg);
            return "vn" + num(reg) + " = " + raw + ";";
        }
        gView.vfClean.erase(reg);
        gView.vfRawStored.erase(reg);
        return "vr" + num(reg) + " = " + raw + "; vn" + num(reg) + " = " + norm + ";";
    }

    struct UpperEmit
    {
        bool nop = true;
        bool generic = false;
        std::string value;
        std::string target;
        std::string statement;
        uint32_t dest = 0u;
        int writeReg = -1;
        bool readsQ = false;
        bool isFmac = false;
        bool normalize = false;
        uint32_t reg = 0u;
        std::string fmacCall;
    };

    UpperEmit upperEmit(uint32_t upper)
    {
        UpperEmit e;
        const uint32_t op = upper & 0x3Fu;
        const uint32_t dest = fDest(upper);
        const uint32_t ft = fFt(upper);
        const uint32_t fs = fFs(upper);
        const uint32_t fd = fFd(upper);
        const std::string s = normV(fs);
        const auto bcExpr = [&](uint32_t bc) { return normBc(ft, bc); };
        const std::string tVec = normV(ft);
        const std::string zero = "_mm_setzero_ps()";

        const auto fmac = [&](const char *kind, const std::string &t, bool usesAcc, bool toAcc, bool readsQ)
        {
            if (dest == 0u)
                return;
            e.nop = false;
            e.dest = dest;
            e.readsQ = readsQ;
            e.isFmac = true;
            e.fmacCall = std::string("<FK::") + kind + ">(vu, ctx, cyc, " + hx(dest) + ", " + s + ", " + t + ", " +
                         (usesAcc ? normV(32u) : zero) + ", un";
            e.value = "fmac" + e.fmacCall + ")";
            if (toAcc)
            {
                e.reg = 32u;
                e.writeReg = -2;
            }
            else
            {
                e.reg = fd;
                e.writeReg = static_cast<int>(fd);
            }
        };
        const auto unary = [&](const std::string &value, uint32_t reg, bool normalize = false)
        {
            if (dest == 0u)
                return;
            e.nop = false;
            e.dest = dest;
            e.value = value;
            e.reg = reg;
            e.normalize = normalize;
            e.writeReg = static_cast<int>(reg);
        };

        if (op < 0x3Cu)
        {
            if (op <= 0x1Bu)
            {
                const uint32_t bc = op & 3u;
                switch (op >> 2)
                {
                case 0: fmac("Add", bcExpr(bc), false, false, false); break;
                case 1: fmac("Sub", bcExpr(bc), false, false, false); break;
                case 2: fmac("Madd", bcExpr(bc), true, false, false); break;
                case 3: fmac("Msub", bcExpr(bc), true, false, false); break;
                case 4: unary("_mm_max_ps(" + s + ", " + bcExpr(bc) + ")", fd); break;
                case 5: unary("_mm_min_ps(" + s + ", " + bcExpr(bc) + ")", fd); break;
                case 6: fmac("Mul", bcExpr(bc), false, false, false); break;
                }
                return e;
            }
            switch (op)
            {
            case 0x1C: fmac("Mul", "qN(vu)", false, false, true); break;
            case 0x1D: unary("_mm_max_ps(" + s + ", iN(vu))", fd); break;
            case 0x1E: fmac("Mul", "iN(vu)", false, false, false); break;
            case 0x1F: unary("_mm_min_ps(" + s + ", iN(vu))", fd); break;
            case 0x20: fmac("Add", "qN(vu)", false, false, true); break;
            case 0x21: fmac("Madd", "qN(vu)", true, false, true); break;
            case 0x22: fmac("Add", "iN(vu)", false, false, false); break;
            case 0x23: fmac("Madd", "iN(vu)", true, false, false); break;
            case 0x24: fmac("Sub", "qN(vu)", false, false, true); break;
            case 0x25: fmac("Msub", "qN(vu)", true, false, true); break;
            case 0x26: fmac("Sub", "iN(vu)", false, false, false); break;
            case 0x27: fmac("Msub", "iN(vu)", true, false, false); break;
            case 0x28: fmac("Add", tVec, false, false, false); break;
            case 0x29: fmac("Madd", tVec, true, false, false); break;
            case 0x2A: fmac("Mul", tVec, false, false, false); break;
            case 0x2B: unary("_mm_max_ps(" + s + ", " + tVec + ")", fd); break;
            case 0x2C: fmac("Sub", tVec, false, false, false); break;
            case 0x2D: fmac("Msub", tVec, true, false, false); break;
            case 0x2E: fmac("Opmsub", tVec, true, false, false); break;
            case 0x2F: unary("_mm_min_ps(" + s + ", " + tVec + ")", fd); break;
            default:
                e.nop = false;
                e.generic = true;
                break;
            }
            return e;
        }

        const uint32_t special = (upper & 3u) | ((upper >> 4) & 0x7Cu);
        if (special <= 0x0Fu || (special >= 0x18u && special <= 0x1Bu))
        {
            const uint32_t bc = special & 3u;
            switch (special >> 2)
            {
            case 0: fmac("Add", bcExpr(bc), false, true, false); break;
            case 1: fmac("Sub", bcExpr(bc), false, true, false); break;
            case 2: fmac("Madd", bcExpr(bc), true, true, false); break;
            case 3: fmac("Msub", bcExpr(bc), true, true, false); break;
            case 6: fmac("Mul", bcExpr(bc), false, true, false); break;
            }
            return e;
        }
        static constexpr uint32_t kShift[4] = {0u, 4u, 12u, 15u};
        switch (special)
        {
        case 0x10: case 0x11: case 0x12: case 0x13:
            unary("itof<" + num(kShift[special & 3u]) + "u>(" + rawV(fs) + ")", ft);
            break;
        case 0x14: case 0x15: case 0x16: case 0x17:
            unary("ftoi<" + num(kShift[special & 3u]) + "u>(" + s + ")", ft, true);
            break;
        case 0x1C: fmac("Mul", "qN(vu)", false, true, true); break;
        case 0x1D: unary("absN(" + s + ")", ft); break;
        case 0x1E: fmac("Mul", "iN(vu)", false, true, false); break;
        case 0x1F:
            e.nop = false;
            e.statement = "clipOp(vu, ctx, cyc, " + rawV(fs) + ", " + rawV(ft) + ");";
            break;
        case 0x20: fmac("Add", "qN(vu)", false, true, true); break;
        case 0x21: fmac("Madd", "qN(vu)", true, true, true); break;
        case 0x22: fmac("Add", "iN(vu)", false, true, false); break;
        case 0x23: fmac("Madd", "iN(vu)", true, true, false); break;
        case 0x24: fmac("Sub", "qN(vu)", false, true, true); break;
        case 0x25: fmac("Msub", "qN(vu)", true, true, true); break;
        case 0x26: fmac("Sub", "iN(vu)", false, true, false); break;
        case 0x27: fmac("Msub", "iN(vu)", true, true, false); break;
        case 0x28: fmac("Add", tVec, false, true, false); break;
        case 0x29: fmac("Madd", tVec, true, true, false); break;
        case 0x2A: fmac("Mul", tVec, false, true, false); break;
        case 0x2C: fmac("Sub", tVec, false, true, false); break;
        case 0x2D: fmac("Msub", tVec, true, true, false); break;
        case 0x2E: fmac("Opmula", tVec, false, true, false); break;
        case 0x2F:
        case 0x30:
            break;
        default:
            e.nop = false;
            e.generic = true;
            break;
        }
        return e;
    }

    struct LowerEmit
    {
        std::string code;
        bool writesVf0 = false;
        bool generic = false;
    };

    uint32_t firstComponent(uint32_t dest)
    {
        if (dest & 0x8u)
            return 0u;
        if (dest & 0x4u)
            return 1u;
        if (dest & 0x2u)
            return 2u;
        return 3u;
    }

    using BranchVi = std::function<std::string(uint32_t)>;

    LowerEmit lowerEmit(uint32_t pc, uint32_t lower, uint32_t upper, const BranchVi *branchVi = nullptr)
    {
        LowerEmit e;
        const auto br = [&](uint32_t reg) { return branchVi ? (*branchVi)(reg) : "vu.readBranchVi(" + num(reg) + "u)"; };
        if (lower == 0u || lower == 0x8000033Cu)
            return e;
        const uint32_t opHi = (lower >> 25) & 0x7Fu;
        const uint32_t dest = fDest(lower);
        const uint32_t ft = fFt(lower);
        const uint32_t fs = fFs(lower);
        const uint32_t it = fVit(lower);
        const uint32_t is = fVis(lower);
        const uint32_t id = fVid(lower);
        const int32_t imm = fImm11(lower);
        const std::string dst = hx(dest);
        const auto generic = [&]()
        {
            e.generic = true;
            e.writesVf0 = true;
            e.code = "genericLower(vu, ctx, cyc, " + hx(lower) + ", " + hx(upper) + "); syncAllNorm(vu, ctx);";
        };
        const auto viSet = [&](uint32_t reg, const std::string &value)
        {
            if (reg != 0u)
                e.code += viW(reg) + " = static_cast<int16_t>(" + value + ");";
        };
        const auto loadVf = [&](uint32_t reg, const std::string &address)
        {
            if (dest == 0u)
                return;
            e.code += "{ const __m128 lv = ldq(mem, " + address + "); " + writeV(reg, "lv", gRawLoads ? "lv" : "vnorm(lv)", dst) + " }";
            e.writesVf0 = e.writesVf0 || reg == 0u;
        };
        const auto storeVf = [&](uint32_t reg, const std::string &address)
        {
            if (dest != 0u)
                e.code += "stq(vu, ctx, cyc, " + address + ", " + rawV(reg) + ", " + dst + ");";
        };
        const uint32_t imm12 = (((lower >> 21) & 1u) << 11) | (lower & 0x7FFu);
        const uint32_t imm24 = lower & 0xFFFFFFu;
        const std::string status = "(vu.m_state.status & 0xFFFu)";

        switch (opHi)
        {
        case 0x00:
            loadVf(ft, "qaddr(" + vi(is) + ", " + num(imm) + ")");
            return e;
        case 0x01:
            storeVf(fs, "qaddr(" + vi(it) + ", " + num(imm) + ")");
            return e;
        case 0x04:
            if (it != 0u)
                viSet(it, "ldw(mem, qaddr(" + vi(is) + ", " + num(imm) + ") + " + num(firstComponent(dest) * 4u) + "u) & 0xFFFFu");
            return e;
        case 0x05:
            if (dest != 0u)
                e.code = "stw(vu, ctx, cyc, qaddr(" + vi(is) + ", " + num(imm) + "), " + vi(it) + ", " + dst + ");";
            return e;
        case 0x08:
        case 0x09:
        {
            const int16_t imm15 = static_cast<int16_t>(static_cast<uint32_t>(static_cast<int16_t>(lower & 0x7FFu)) | ((lower >> 10) & 0x7800u));
            viSet(it, vi(is) + (opHi == 0x08u ? " + " : " - ") + num(imm15));
            return e;
        }
        case 0x10:
            e.code = "commitClip(vu, ctx, cyc); " + viW(1) + " = (vu.m_state.clip & 0xFFFFFFu) == " + hx(imm24) + " ? 1 : 0;";
            return e;
        case 0x11:
            e.code = "queueFcsetAt(vu, ctx, cyc, " + hx(imm24) + ");";
            return e;
        case 0x12:
            e.code = "commitClip(vu, ctx, cyc); " + viW(1) + " = (vu.m_state.clip & " + hx(imm24) + ") != 0u ? 1 : 0;";
            return e;
        case 0x13:
            e.code = "commitClip(vu, ctx, cyc); " + viW(1) + " = (vu.m_state.clip | " + hx(imm24) + ") == 0xFFFFFFu ? 1 : 0;";
            return e;
        case 0x14:
            if (it != 0u)
                e.code = "commitStatus(vu, ctx, cyc); " + viW(it) + " = " + status + " == " + hx(imm12) + " ? 1 : 0;";
            return e;
        case 0x15:
            e.code = "queueFssetAt(vu, ctx, cyc, " + hx(imm12) + ");";
            return e;
        case 0x16:
            if (it != 0u)
            {
                e.code = "commitStatus(vu, ctx, cyc); ";
                viSet(it, status + " & " + hx(imm12));
            }
            return e;
        case 0x17:
            if (it != 0u)
            {
                e.code = "commitStatus(vu, ctx, cyc); ";
                viSet(it, status + " | " + hx(imm12));
            }
            return e;
        case 0x18:
            if (it != 0u)
                e.code = "commitStatus(vu, ctx, cyc); " + viW(it) + " = (vu.m_state.mac & 0xFFFFu) == static_cast<uint32_t>(static_cast<uint16_t>(" + vi(is) + ")) ? 1 : 0;";
            return e;
        case 0x1A:
        case 0x1B:
            if (it != 0u)
            {
                e.code = "commitStatus(vu, ctx, cyc); ";
                viSet(it, std::string("vu.m_state.mac ") + (opHi == 0x1Au ? "&" : "|") + " static_cast<uint32_t>(static_cast<uint16_t>(" + vi(is) + "))");
            }
            return e;
        case 0x1C:
            if (it != 0u)
            {
                e.code = "commitClip(vu, ctx, cyc); ";
                viSet(it, "vu.m_state.clip & 0x0FFFu");
            }
            return e;
        case 0x20:
            return e;
        case 0x21:
            viSet(it, num((pc + 16u) / 8u));
            return e;
        case 0x24:
            e.code = "jt = (static_cast<uint32_t>(static_cast<uint16_t>(" + br(is) + ")) * 8u) & 0x3FFFu;";
            return e;
        case 0x25:
            e.code = "jt = (static_cast<uint32_t>(static_cast<uint16_t>(" + br(is) + ")) * 8u) & 0x3FFFu;";
            viSet(it, num((pc + 16u) / 8u));
            return e;
        case 0x28:
        case 0x29:
            e.code = "tk = static_cast<int16_t>(" + br(is) + ") " + (opHi == 0x28u ? "==" : "!=") +
                     " static_cast<int16_t>(" + br(it) + ");";
            return e;
        case 0x2C:
        case 0x2D:
        case 0x2E:
        case 0x2F:
        {
            static const char *kCompare[4] = {"< 0", "> 0", "<= 0", ">= 0"};
            e.code = "tk = static_cast<int16_t>(" + br(is) + ") " + kCompare[opHi - 0x2Cu] + ";";
            return e;
        }
        case 0x40:
            break;
        default:
            generic();
            return e;
        }

        const uint32_t funct = lower & 0x3Fu;
        switch (funct)
        {
        case 0x30:
            viSet(id, vi(is) + " + " + vi(it));
            return e;
        case 0x31:
            viSet(id, vi(is) + " - " + vi(it));
            return e;
        case 0x32:
        {
            const int32_t imm5 = static_cast<int32_t>(((lower >> 6) & 0x1Fu) << 27) >> 27;
            viSet(it, vi(is) + " + " + num(imm5));
            return e;
        }
        case 0x34:
            viSet(id, vi(is) + " & " + vi(it));
            return e;
        case 0x35:
            viSet(id, vi(is) + " | " + vi(it));
            return e;
        default:
            break;
        }
        if (funct < 0x3Cu)
        {
            generic();
            return e;
        }

        const uint32_t special = (lower & 3u) | ((lower >> 4) & 0x7Cu);
        const uint32_t fsf = (lower >> 21) & 3u;
        const uint32_t ftf = (lower >> 23) & 3u;
        switch (special)
        {
        case 0x30:
            if (dest != 0u)
            {
                e.code = writeV(ft, rawV(fs), normV(fs), dst);
                e.writesVf0 = ft == 0u;
            }
            return e;
        case 0x31:
            if (dest != 0u)
            {
                const std::string source = rawV(fs);
                const std::string sourceNorm = normV(fs);
                const std::string write =
                    writeV(ft, "_mm_shuffle_ps(mr, mr, _MM_SHUFFLE(0, 3, 2, 1))", "_mm_shuffle_ps(mn, mn, _MM_SHUFFLE(0, 3, 2, 1))", dst);
                e.code = "{ const __m128 mr = " + source + "; const __m128 mn = " + sourceNorm + "; " + write + " }";
                e.writesVf0 = ft == 0u;
            }
            return e;
        case 0x34:
            loadVf(ft, "qaddr(" + vi(is) + ", 0)");
            viSet(is, vi(is) + " + 1");
            return e;
        case 0x35:
            storeVf(fs, "qaddr(" + vi(it) + ", 0)");
            viSet(it, vi(it) + " + 1");
            return e;
        case 0x36:
            viSet(is, vi(is) + " - 1");
            loadVf(ft, "qaddr(" + vi(is) + ", 0)");
            return e;
        case 0x37:
            viSet(it, vi(it) + " - 1");
            storeVf(fs, "qaddr(" + vi(it) + ", 0)");
            return e;
        case 0x38:
            e.code = "divOp(vu, ctx, cyc, " + laneFloat(fs, fsf) + ", " + laneFloat(ft, ftf) + ");";
            return e;
        case 0x39:
            e.code = "sqrtOp(vu, ctx, cyc, " + laneFloat(ft, ftf) + ");";
            return e;
        case 0x3A:
            e.code = "rsqrtOp(vu, ctx, cyc, " + laneFloat(fs, fsf) + ", " + laneFloat(ft, ftf) + ");";
            return e;
        case 0x3B:
            return e;
        case 0x3C:
            if (it != 0u)
                viSet(it, laneBits(fs, fsf) + " & 0xFFFFu");
            return e;
        case 0x3D:
            if (dest != 0u)
            {
                e.code = "{ const __m128 mv = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int16_t>(" + vi(is) +
                         " & 0xFFFF))); " + writeV(ft, "mv", "vnorm(mv)", dst) + " }";
                e.writesVf0 = ft == 0u;
            }
            return e;
        case 0x3E:
            if (it != 0u)
                viSet(it, "ldw(mem, qaddr(" + vi(is) + ", 0) + " + num(firstComponent(dest) * 4u) + "u) & 0xFFFFu");
            return e;
        case 0x3F:
            if (dest != 0u)
                e.code = "stw(vu, ctx, cyc, qaddr(" + vi(is) + ", 0), " + vi(it) + ", " + dst + ");";
            return e;
        case 0x64:
            if (dest != 0u)
            {
                e.code = "commitEfu(vu, cyc); { const __m128 pv = _mm_set1_ps(vu.m_state.p); " + writeV(ft, "pv", "vnorm(pv)", dst) + " }";
                e.writesVf0 = ft == 0u;
            }
            return e;
        case 0x68:
            viSet(it, "vu.m_state.top & 0x3FFu");
            return e;
        case 0x69:
            viSet(it, "vu.m_state.itop & 0x3FFu");
            return e;
        case 0x6C:
            e.code = "xgkickStart(vu, ctx, cyc, " + vi(is) + ");";
            return e;
        case 0x7B:
            return e;
        default:
            generic();
            return e;
        }
    }

    struct Pair
    {
        uint32_t pc = 0;
        A::Decoded d{};
        bool reserved = false;
        bool branch = false;
        bool dynamic = false;
        bool conditional = false;
        uint32_t target = 0;
        bool fallback = false;
    };

    struct Known
    {
        uint32_t pos = 0;
        uint64_t bound = 0;
        std::string id;
        std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> vf;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> vi;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> acc;
        bool div = false;
        uint32_t divPos = 0;
        uint32_t divLat = 0;
        std::map<uint32_t, std::vector<std::string>> checks;
        std::set<std::string> checked;
        bool fdivChecked = false;
        uint32_t fdivK = 0;
        std::vector<uint32_t> off;
        std::vector<uint32_t> fmacPos;
        bool readsStatus = false;
        std::set<uint32_t> collapsed;
        uint32_t lastCollapsed = ~0u;
        bool flagsDead = false; // see Emitter::flagLive
        int backupReg = -1;
        std::string backupVar;
    };

    std::string segLabel(uint32_t pc)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "seg_%04X", pc);
        return text;
    }

    struct Emitter
    {
        std::vector<Pair> pairs;
        uint32_t first = 0;
        uint32_t last = 0;
        bool usesTk = false;
        bool usesJt = false;
        std::set<std::string> exits;
        std::vector<bool> isTarget;
        std::set<uint32_t> segs;
        bool aggregateFlags = true;
        bool cacheRegisters = true;
        // flagLive[i]: an instruction that reads (or sets) the MAC/status flags (FMEQ/FMAND/FMOR, FSEQ/FSAND/FSOR/FSSET)
        // may be reached from pair i, or a dynamic jump (JR/JALR) whose target is unknown. Where it is false, only the
        // final MAC/status values can ever be observed (at the end of the program or at a budget exit), so every FMAC
        // of a segment can fold its flags into the one record the segment's last FMAC pushes, including the last
        // kFmacLatency positions that otherwise push their own record for the next segment's readers.
        // VU1_RECOMP_NO_DEAD_FLAGS=1 turns this off.
        std::vector<bool> flagLive;
        // gFastFmac for this image. An image whose code fits in VU0's 4 KB micro memory may be a VU0 program, whose
        // status flags the EE reads (CFC2 vi16); Urban Reign's VU0 skinning program underflows for real, and the plain
        // SSE operation cannot tell a flushed tiny result from an exact zero, so such images keep the exact FMACs.
        bool fastFmac = false;
        bool noSticky = false; // fmacFastN: fastFmac, no status reader in the image and not VU1_RECOMP_STICKY
        bool deadFlags = [] { const char *v = std::getenv("VU1_RECOMP_NO_DEAD_FLAGS"); return !(v && *v == '1'); }();
        // VU1_RECOMP_NO_SELF_LOOPS=1: re-enter self-looping segments through their label as before.
        bool disableSelfLoops = [] { const char *v = std::getenv("VU1_RECOMP_NO_SELF_LOOPS"); return v && *v == '1'; }();
        uint32_t selfLoops = 0u;

        bool inBlock(uint32_t pc) const
        {
            const uint32_t index = pc / 8u;
            return index >= first && index < last;
        }

        std::string jumpTo(uint32_t pc)
        {
            if (inBlock(pc))
                return "goto " + (segs.count(pc / 8u) ? segLabel(pc) : label(pc)) + ";";
            exits.insert("exit_continue");
            return "{ epc = " + hx(pc) + "; goto exit_continue; }";
        }

        bool heavy(const Pair &p) const
        {
            if (p.fallback || p.reserved)
                return true;
            if (!p.d.iBit)
            {
                const unsigned pipe = A::pipeline(p.d.lowerUsage);
                if (pipe == A::kPipeXgkick || pipe == A::kPipeEfu || p.d.lowerUsage.waitP)
                    return true;
                const RegViewProbe probe;
                if (lowerEmit(p.pc, p.d.lower, p.d.upper).generic)
                    return true;
            }
            const RegViewProbe probe;
            return upperEmit(p.d.upper).generic;
        }

        bool breaker(uint32_t index) const
        {
            const Pair &p = pairs[index];
            if (heavy(p))
                return true;
            if (p.branch || p.d.eBit)
                return heavy(pairs[nextPc(p.pc) / 8u]);
            return false;
        }

        bool isStart(uint32_t index) const
        {
            if (index == first || isTarget[index])
                return true;
            if (index >= 1u && (breaker(index - 1u) || pairs[index - 1u].branch || pairs[index - 1u].d.eBit))
                return true;
            return index >= 2u && pairs[index - 2u].branch;
        }

        uint32_t segmentPositions(uint32_t start) const
        {
            if (breaker(start))
                return 0u;
            uint32_t positions = 0u;
            for (uint32_t index = start;;)
            {
                if (index >= last || (index != start && isStart(index)) || breaker(index))
                    return positions;
                const Pair &p = pairs[index];
                if (p.branch || p.d.eBit)
                    return positions + 2u;
                ++positions;
                const uint32_t next = nextPc(p.pc) / 8u;
                if (next != index + 1u)
                    return positions;
                index = next;
            }
        }

        void planSegments()
        {
            segs.clear();
            for (uint32_t index = first; index < last; ++index)
                if (isStart(index) && segmentPositions(index) >= 1u)
                    segs.insert(index);
        }

        std::string knownStall(const Pair &p, Known &k)
        {
            const auto &d = p.d;
            std::vector<std::string> terms;
            uint32_t extra = 0u;
            const auto internal = [&](uint32_t writerPos, uint32_t latency)
            {
                const uint32_t distance = k.pos - writerPos;
                if (distance >= latency)
                    return;
                terms.push_back("s" + k.id + "_" + num(writerPos) + " + " + num(latency) + "u");
                extra = std::max(extra, latency - distance);
            };
            const auto external = [&](const std::string &expr, uint32_t maxLatency)
            {
                if (k.pos + 1u >= maxLatency)
                    return;
                terms.push_back(expr);
            };
            std::set<std::pair<uint32_t, uint32_t>> lanes;
            for (const auto *usage : {&d.upperUsage, &d.lowerUsage})
                for (uint32_t index = 0; index < usage->vfReadCount; ++index)
                {
                    const auto access = usage->vfRead[index];
                    if (access.reg == 0u)
                        continue;
                    for (uint32_t component = 0; component < 4u; ++component)
                        if (access.lanes & laneBit(component))
                            lanes.insert({access.reg, component});
                }
            for (const auto &lane : lanes)
            {
                const auto it = k.vf.find(lane);
                if (it != k.vf.end())
                    internal(it->second.first, it->second.second);
                else
                    external("vu.m_vfReady[" + num(lane.first) + "][" + num(lane.second) + "]", kMaxVfLatency);
            }
            const uint32_t viReads = (d.upperUsage.viRead | d.lowerUsage.viRead) & 0xFFFEu;
            for (uint32_t reg = 1; reg < 16u; ++reg)
            {
                if (!(viReads & (1u << reg)))
                    continue;
                const auto it = k.vi.find(reg);
                if (it != k.vi.end())
                    internal(it->second.first, it->second.second);
                else
                    external("vu.m_viReady[" + num(reg) + "]", kMaxViLatency);
            }
            const uint32_t accReads = d.upperUsage.accRead | d.lowerUsage.accRead;
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if (!(accReads & laneBit(component)))
                    continue;
                const auto it = k.acc.find(component);
                if (it != k.acc.end())
                    internal(it->second.first, it->second.second);
                else
                    external("vu.m_accReady[" + num(component) + "]", kMaxAccLatency);
            }
            if (!d.iBit && (A::pipeline(d.lowerUsage) == A::kPipeFdiv || d.lowerUsage.waitQ))
            {
                if (k.div)
                    internal(k.divPos, k.divLat);
                else if (!k.fdivChecked)
                {
                    k.fdivChecked = true;
                    k.fdivK = k.pos;
                }
            }
            k.bound += 1u + extra;
            k.off.push_back(static_cast<uint32_t>(k.bound - 1u));
            if (terms.empty())
                return "";
            std::string out = "        { uint64_t r = cyc;";
            for (const std::string &term : terms)
                out += " r = rmax(r, " + term + ");";
            out += " cyc = r; }\n";
            return out;
        }

        void recordWrites(const Pair &p, Known &k)
        {
            const auto &d = p.d;
            const auto lw = d.lowerUsage.vfWrite;
            if (lw.reg != 0u && d.suppressedLowerVf != lw.reg)
                for (uint32_t component = 0; component < 4u; ++component)
                    if (lw.lanes & laneBit(component))
                        k.vf[{lw.reg, component}] = {k.pos, d.lowerUsage.vfLatency != 0u ? d.lowerUsage.vfLatency : d.lowerUsage.latency};
            const auto uw = d.upperUsage.vfWrite;
            if (uw.reg != 0u)
                for (uint32_t component = 0; component < 4u; ++component)
                    if (uw.lanes & laneBit(component))
                        k.vf[{uw.reg, component}] = {k.pos, d.upperUsage.vfLatency != 0u ? d.upperUsage.vfLatency : d.upperUsage.latency};
            const uint32_t viWrites = d.lowerUsage.viWrite & 0xFFFEu;
            for (uint32_t reg = 1; reg < 16u; ++reg)
                if (viWrites & (1u << reg))
                    k.vi[reg] = {k.pos, d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency};
            for (uint32_t component = 0; component < 4u; ++component)
                if (d.upperUsage.accWrite & laneBit(component))
                    k.acc[component] = {k.pos, 1u};
            if (!d.iBit && A::pipeline(d.lowerUsage) == A::kPipeFdiv && !d.lowerUsage.waitQ && d.lowerUsage.latency != 0u)
            {
                k.div = true;
                k.divPos = k.pos;
                k.divLat = d.lowerUsage.latency;
            }
            ++k.pos;
        }

        std::string stall(const Pair &p, const std::string &stub)
        {
            const auto &d = p.d;
            std::vector<std::pair<uint32_t, uint32_t>> vfLanes;
            const auto addReads = [&](const auto &usage)
            {
                for (uint32_t index = 0; index < usage.vfReadCount; ++index)
                {
                    const auto access = usage.vfRead[index];
                    if (access.reg == 0u)
                        continue;
                    for (uint32_t component = 0; component < 4u; ++component)
                    {
                        const auto item = std::make_pair(static_cast<uint32_t>(access.reg), component);
                        if ((access.lanes & laneBit(component)) != 0u &&
                            std::find(vfLanes.begin(), vfLanes.end(), item) == vfLanes.end())
                            vfLanes.push_back(item);
                    }
                }
            };
            addReads(d.upperUsage);
            addReads(d.lowerUsage);
            const uint32_t viReads = (d.upperUsage.viRead | d.lowerUsage.viRead) & 0xFFFEu;
            const uint32_t accReads = 0u;
            const unsigned lowerPipe = A::pipeline(d.lowerUsage);
            const bool fdiv = lowerPipe == A::kPipeFdiv || d.lowerUsage.waitQ;
            const bool efu = lowerPipe == A::kPipeEfu;
            const bool waitP = d.lowerUsage.waitP;
            const bool xgkick = lowerPipe == A::kPipeXgkick;
            if (vfLanes.empty() && viReads == 0u && accReads == 0u && !fdiv && !efu && !waitP && !xgkick)
                return "";
            std::string out = "        { uint64_t r = cyc;";
            for (const auto &[reg, component] : vfLanes)
                out += " r = rmax(r, vu.m_vfReady[" + num(reg) + "][" + num(component) + "]);";
            for (uint32_t reg = 1; reg < 16u; ++reg)
                if (viReads & (1u << reg))
                    out += " r = rmax(r, vu.m_viReady[" + num(reg) + "]);";
            if (fdiv)
                out += " if (vu.m_fdiv.valid) r = rmax(r, vu.m_fdiv.readyCycle);";
            if (efu)
                out += " r = rmax(r, vu.m_efuResourceReady);";
            if (waitP)
                out += " if (vu.m_efu[0].valid) r = rmax(r, vu.m_efu[0].readyCycle); if (vu.m_efu[1].valid) r = rmax(r, vu.m_efu[1].readyCycle);";
            if (xgkick)
                out += " if (!xgkickStall(vu, ctx, cyc, r, budget)) " + stub + " }\n";
            else
                out += " if (r > cyc) { if (r >= budget) { cyc = budget; " + stub + " } cyc = r; } }\n";
            return out;
        }

        std::string body(const Pair &p, const std::string &stub, Known *known = nullptr)
        {
            const auto &d = p.d;
            std::string out;
            const uint32_t viWrites = d.lowerUsage.viWrite & 0xFFFEu;
            const uint32_t writtenVi = viWrites != 0u ? static_cast<uint32_t>(std::countr_zero(viWrites)) : 0u;
            const bool recordBackup = writtenVi != 0u && d.lowerUsage.delaysNextBranchRead;
            std::string backupVar = "oldVi";
            gView.record = known != nullptr;
            BranchVi branchVi;
            const BranchVi *branchViPtr = nullptr;
            if (known)
            {
                out += knownStall(p, *known);
                out += "        s" + known->id + "_" + num(known->pos) + " = cyc;\n";
                if (recordBackup)
                {
                    backupVar = "bk" + known->id + "_" + num(known->pos);
                    out += "        const int32_t " + backupVar + " = " + vi(writtenVi) + ";\n";
                }
                if (known->backupReg >= 0)
                {
                    const int reg = known->backupReg;
                    const std::string var = known->backupVar;
                    branchVi = [reg, var](uint32_t r)
                    {
                        if (r == 0u)
                            return std::string("0");
                        if (static_cast<int>(r) == reg)
                            return var;
                        return vi(r);
                    };
                    branchViPtr = &branchVi;
                }
                if (!d.iBit)
                {
                    const uint32_t opHi = (d.lower >> 25) & 0x7Fu;
                    if ((opHi >= 0x14u && opHi <= 0x18u) || opHi == 0x1Au || opHi == 0x1Bu)
                        known->readsStatus = true;
                }
            }
            else
            {
                out += "        if (cyc >= budget) " + stub + "\n";
                out += stall(p, stub);
            }
            out += "        {\n";
            if (recordBackup && !known)
                out += "            const int32_t oldVi = " + vi(writtenVi) + ";\n";
            const UpperEmit up = upperEmit(d.upper);
            std::string fmacValue = up.value;
            std::string fmacDecl = "__m128 un;";
            std::string fmacAfter;
            if (known && up.isFmac)
            {
                known->fmacPos.push_back(known->pos);
                if (known->pos == known->lastCollapsed)
                {
                    fmacValue = "fmacF" + up.fmacCall + ", fl)";
                    fmacDecl = "__m128 un; FmacFlags fl;";
                    fmacAfter = " pushFmacFlags(vu, ctx, cyc, fl.mac, fl.status, fl.status | fl.extra | fAccScalar | stickyOf(fAccZ, fAccS));";
                }
                else if (known->collapsed.count(known->pos))
                {
                    if (known->flagsDead && fastFmac && noSticky)
                        fmacValue = "fmacFastN" + up.fmacCall + ")";
                    else
                        fmacValue = std::string(known->flagsDead && fastFmac ? "fmacFastV" : "fmacV") + up.fmacCall +
                                    ", fAccZ, fAccS, fAccScalar)";
                }
            }
            if (up.readsQ || up.generic)
                out += "            syncQ(vu, ctx, cyc);\n";
            const auto upperStore = [&]()
            {
                if (up.nop)
                    return std::string();
                if (up.generic)
                    return "            genericUpper(vu, ctx, cyc, " + hx(d.upper) + "); syncAllNorm(vu, ctx);\n";
                if (!up.statement.empty())
                    return "            " + up.statement + "\n";
                if (up.isFmac)
                    return "            { " + fmacDecl + " const __m128 uv = " + fmacValue + ";" + fmacAfter + " " + writeV(up.reg, "uv", "uv", hx(up.dest)) +
                           " }\n";
                return "            { const __m128 uv = " + up.value + "; " +
                       writeV(up.reg, "uv", up.normalize ? std::string("vnorm(uv)") : std::string("uv"), hx(up.dest)) + " }\n";
            };
            const bool upperFirst = !d.iBit && d.upperVfShadowReg == 0u;
            const std::string earlyUpper = upperFirst ? upperStore() : std::string();
            const LowerEmit low = d.iBit ? LowerEmit{} : lowerEmit(p.pc, d.lower, d.upper, branchViPtr);
            std::string lowerCode = low.code.empty() ? "" : "            " + low.code + "\n";
            if (d.iBit)
            {
                uint32_t bits = d.lower;
                const uint32_t exponent = (bits >> 23) & 0xFFu;
                if (exponent == 0u)
                    bits &= 0x80000000u;
                else if (exponent == 0xFFu)
                    bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
                out += upperStore();
                out += "            vu.m_state.i = std::bit_cast<float>(" + hx(bits) + ");\n";
            }
            else if (d.upperVfShadowReg != 0u)
            {
                const std::string reg = vf(d.upperVfShadowReg);
                if (up.generic)
                {
                    out += "            float shadowOld[4]; std::memcpy(shadowOld, " + reg + ", 16u);\n";
                    out += "            genericUpper(vu, ctx, cyc, " + hx(d.upper) + ");\n";
                    out += "            float shadowNew[4]; std::memcpy(shadowNew, " + reg + ", 16u); std::memcpy(" + reg + ", shadowOld, 16u);\n";
                    out += lowerCode;
                    out += "            std::memcpy(" + reg + ", shadowNew, 16u); syncAllNorm(vu, ctx);\n";
                }
                else
                {
                    out += "            const __m128 shadowOld = " + rawV(d.upperVfShadowReg) + ";\n";
                    out += "            const __m128 shadowOldN = " + normV(d.upperVfShadowReg) + ";\n";
                    if (up.isFmac)
                        out += "            " + fmacDecl + " const __m128 uv = " + fmacValue + ";" + fmacAfter + "\n";
                    else
                        out += "            const __m128 uv = " + up.value + "; const __m128 un = " +
                               (up.normalize ? std::string("vnorm(uv)") : std::string("uv")) + ";\n";
                    out += lowerCode;
                    out += "            " + writeFullV(d.upperVfShadowReg, "blendDest(shadowOld, uv, " + hx(up.dest) + ")",
                                                  "blendDest(shadowOldN, un, " + hx(up.dest) + ")") + "\n";
                }
            }
            else
            {
                out += earlyUpper;
                out += lowerCode;
            }
            if (!known)
            {
                out += "            vu.m_viBranchBackupValid = false;\n";
                const auto markVf = [&](uint32_t reg, uint32_t lanes, uint32_t latency)
                {
                    for (uint32_t component = 0; component < 4u; ++component)
                        if (lanes & laneBit(component))
                            out += "            vu.m_vfReady[" + num(reg) + "][" + num(component) + "] = cyc + " + num(latency) + "u;\n";
                };
                const auto lw = d.lowerUsage.vfWrite;
                if (lw.reg != 0u && d.suppressedLowerVf != lw.reg)
                    markVf(lw.reg, lw.lanes, d.lowerUsage.vfLatency != 0u ? d.lowerUsage.vfLatency : d.lowerUsage.latency);
                const auto uw = d.upperUsage.vfWrite;
                if (uw.reg != 0u)
                    markVf(uw.reg, uw.lanes, d.upperUsage.vfLatency != 0u ? d.upperUsage.vfLatency : d.upperUsage.latency);
                for (uint32_t reg = 1; reg < 16u; ++reg)
                    if (viWrites & (1u << reg))
                        out += "            vu.m_viReady[" + num(reg) + "] = cyc + " +
                               num(d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency) + "u;\n";
                for (uint32_t component = 0; component < 4u; ++component)
                    if (d.upperUsage.accWrite & laneBit(component))
                        out += "            vu.m_accReady[" + num(component) + "] = cyc + 1u;\n";
                if (recordBackup)
                    out += "            vu.m_viBranchBackupValue = oldVi; vu.m_viBranchBackupReg = " + num(writtenVi) +
                           "u; vu.m_viBranchBackupValid = true;\n";
            }
            if ((up.writeReg == 0 || up.generic || low.writesVf0) && !gView.local)
                out += "            resetVf0(vu, ctx);\n";
            out += "        }\n";
            out += "        ++cyc;\n";
            gView.record = false;
            if (known)
            {
                known->backupReg = recordBackup ? static_cast<int>(writtenVi) : 0;
                known->backupVar = recordBackup ? backupVar : std::string();
                recordWrites(p, *known);
            }
            return out;
        }

        std::string resolution(const Pair &p)
        {
            const uint32_t next = nextPc(p.pc);
            const uint32_t after = nextPc(next);
            if (p.dynamic)
            {
                exits.insert("exit_continue");
                return "        { epc = jt; goto exit_continue; }\n";
            }
            if (p.conditional)
                return "        if (tk) " + jumpTo(p.target) + "\n        " + jumpTo(after) + "\n";
            return "        " + jumpTo(p.target) + "\n";
        }

        std::pair<std::string, std::string> walkSegment(uint32_t start, Known &k)
        {
            std::string code;
            std::string tail;
            for (uint32_t index = start;;)
            {
                if (index >= last)
                {
                    tail = "        " + jumpTo(index * 8u) + "\n";
                    break;
                }
                if ((index != start && isStart(index) && k.pos >= kSegmentJoinLimit) || breaker(index))
                {
                    tail = "        " + jumpTo(index * 8u) + "\n";
                    if (breaker(index))
                        tail = "        goto " + label(index * 8u) + ";\n";
                    break;
                }
                const Pair &p = pairs[index];
                code += "        // " + label(p.pc) + "\n";
                code += body(p, "", &k);
                const uint32_t next = nextPc(p.pc);
                if (p.branch)
                {
                    if (p.conditional)
                        usesTk = true;
                    if (p.dynamic)
                        usesJt = true;
                    code += body(pairs[next / 8u], "", &k);
                    tail = resolution(p);
                    break;
                }
                if (p.d.eBit)
                {
                    code += body(pairs[next / 8u], "", &k);
                    exits.insert("exit_ended");
                    tail = "        { epc = " + hx(nextPc(next)) + "; goto exit_ended; }\n";
                    break;
                }
                if (next / 8u != index + 1u)
                {
                    tail = "        " + jumpTo(next) + "\n";
                    break;
                }
                index = next / 8u;
            }
            return {code, tail};
        }

        std::string readyStores(const Known &k)
        {
            std::string out;
            const auto at = [&](const std::pair<uint32_t, uint32_t> &write)
            {
                return "s" + k.id + "_" + num(write.first) + " + " + num(write.second) + "u";
            };
            const auto live = [&](const std::pair<uint32_t, uint32_t> &write, uint32_t maxLatency)
            {
                return gAllReady || k.pos - write.first < maxLatency;
            };
            for (uint32_t reg = 1; reg < 32u; ++reg)
            {
                std::vector<std::pair<uint32_t, uint32_t>> lanes;
                for (uint32_t component = 0; component < 4u; ++component)
                {
                    const auto it = k.vf.find({reg, component});
                    if (it != k.vf.end() && live(it->second, kMaxVfLatency))
                        lanes.push_back(it->second);
                }
                if (lanes.empty())
                    continue;
                if (lanes.size() == 4u && std::all_of(lanes.begin(), lanes.end(), [&](const auto &w) { return w == lanes[0]; }))
                {
                    out += "        setReady4(vu.m_vfReady[" + num(reg) + "].data(), " + at(lanes[0]) + ");\n";
                    continue;
                }
                for (uint32_t component = 0; component < 4u; ++component)
                {
                    const auto it = k.vf.find({reg, component});
                    if (it != k.vf.end() && live(it->second, kMaxVfLatency))
                        out += "        vu.m_vfReady[" + num(reg) + "][" + num(component) + "] = " + at(it->second) + ";\n";
                }
            }
            for (const auto &[reg, write] : k.vi)
                if (live(write, kMaxViLatency))
                    out += "        vu.m_viReady[" + num(reg) + "] = " + at(write) + ";\n";
            const bool accLive = std::all_of(k.acc.begin(), k.acc.end(), [&](const auto &w) { return live(w.second, kMaxAccLatency); });
            if (k.acc.size() == 4u && accLive &&
                std::all_of(k.acc.begin(), k.acc.end(), [&](const auto &w) { return w.second == k.acc.begin()->second; }))
                out += "        setReady4(vu.m_accReady.data(), " + at(k.acc.begin()->second) + ");\n";
            else
                for (const auto &[component, write] : k.acc)
                    if (live(write, kMaxAccLatency))
                        out += "        vu.m_accReady[" + num(component) + "] = " + at(write) + ";\n";
            if (k.backupReg > 0)
                out += "        vu.m_viBranchBackupValue = " + k.backupVar + "; vu.m_viBranchBackupReg = " + num(k.backupReg) +
                       "u; vu.m_viBranchBackupValid = true;\n";
            else if (k.backupReg == 0)
                out += "        vu.m_viBranchBackupValid = false;\n";
            return out;
        }

        std::string emitSegment(uint32_t start)
        {
            gView = RegView{};
            gView.local = cacheRegisters;
            Known plan;
            plan.id = hexPcId(start);
            walkSegment(start, plan);
            gView.vfUsed.clear();
            gView.vfWritten.clear();
            gView.viUsed.clear();
            gView.viWritten.clear();
            gView.vfClean.clear();
            gView.vfRawStored.clear();
            Known k;
            k.id = plan.id;
            if (!plan.readsStatus && aggregateFlags)
            {
                const bool flagsDead = deadFlags && start < flagLive.size() && !flagLive[start];
                k.flagsDead = flagsDead;
                for (const uint32_t position : plan.fmacPos)
                    if (flagsDead || position + kFmacLatency <= plan.pos)
                        k.collapsed.insert(position);
                if (k.collapsed.size() < 2u)
                    k.collapsed.clear();
                else
                    k.lastCollapsed = *k.collapsed.rbegin();
            }
            const auto [code, tail] = walkSegment(start, k);
            const RegView view = gView;
            gView = RegView{};
            std::string out = segLabel(start * 8u) + ":\n";
            const std::string fallback = " goto " + label(start * 8u) + ";\n";
            // The entry checks (cycle budget, stalls on registers written before the segment, pending DIV). A
            // segment that loops to itself repeats them before each further iteration.
            std::string checks = "        if (cyc + " + num(k.bound + kMaxVfLatency - 1u) + "u > budget)" + fallback;
            for (const auto &[position, exprs] : k.checks)
            {
                std::string max = exprs[0];
                for (size_t i = 1; i < exprs.size(); ++i)
                    max = "rmax(" + max + ", " + exprs[i] + ")";
                checks += "        if (" + max + " > cyc + " + num(position) + "u)" + fallback;
            }
            if (k.fdivChecked)
                checks += "        if (vu.m_fdiv.valid && vu.m_fdiv.readyCycle > cyc + " + num(k.fdivK) + "u)" + fallback;
            out += checks;
            out += "    {\n        uint64_t ";
            for (uint32_t i = 0; i < k.pos; ++i)
                out += (i ? ", s" : "s") + k.id + "_" + num(i) + " = 0u";
            out += ";\n";
            if (!k.collapsed.empty())
                out += "        __m128 fAccZ = _mm_setzero_ps(), fAccS = _mm_setzero_ps(); uint32_t fAccScalar = 0u;\n";
            const auto memRaw = [](uint32_t reg) { return reg == 32u ? std::string("vu.m_state.acc") : vf(reg); };
            for (const uint32_t reg : view.vfUsed)
                out += "        __m128 vr" + num(reg) + " = _mm_loadu_ps(" + memRaw(reg) + "); __m128 vn" + num(reg) + " = _mm_load_ps(ctx.norm[" +
                       num(reg) + "]);\n";
            for (const uint32_t reg : view.viUsed)
                out += "        int32_t vi" + num(reg) + " = vu.m_state.vi[" + num(reg) + "];\n";
            // Self loop (the segment branches back to its own start, e.g. a vertex loop): after the usual write-back
            // memory and locals agree, so instead of re-entering at the segment label and reloading every cached
            // register, re-run the entry checks and continue at the body. Only the locals the write-back does not
            // cover are refreshed: raw values the segment stored straight to memory (vfRawStored) and registers whose
            // raw value is the normalized one (vfClean; the reload would read that back into vr). The collapsed
            // flag accumulators start over as on a fresh entry. A failed check falls back to the per-pair path,
            // which reads the (written-back) memory.
            const std::string selfJump = "goto " + segLabel(start * 8u) + ";";
            std::string loopTail = tail;
            const size_t selfAt = disableSelfLoops ? std::string::npos : loopTail.find(selfJump);
            if (selfAt != std::string::npos)
            {
                const std::string body = "segb_" + hexPcId(start);
                std::string refresh;
                for (const uint32_t reg : view.vfWritten)
                {
                    if (view.vfRawStored.count(reg))
                        refresh += "        vr" + num(reg) + " = _mm_loadu_ps(" + memRaw(reg) + ");\n";
                    else if (view.vfClean.count(reg))
                        refresh += "        vr" + num(reg) + " = vn" + num(reg) + ";\n";
                }
                if (!k.collapsed.empty())
                    refresh += "        fAccZ = _mm_setzero_ps(); fAccS = _mm_setzero_ps(); fAccScalar = 0u;\n";
                loopTail.replace(selfAt, selfJump.size(), "{\n" + checks + refresh + "        goto " + body + ";\n        }");
                out += body + ":\n";
                ++selfLoops;
            }
            out += code;
            for (const uint32_t reg : view.vfWritten)
            {
                if (!view.vfRawStored.count(reg))
                    out += "        _mm_storeu_ps(" + memRaw(reg) + ", " + (view.vfClean.count(reg) ? "vn" : "vr") + num(reg) + ");";
                out += " _mm_store_ps(ctx.norm[" + num(reg) + "], vn" + num(reg) + ");\n";
            }
            for (const uint32_t reg : view.viWritten)
                out += "        vu.m_state.vi[" + num(reg) + "] = vi" + num(reg) + ";\n";
            out += readyStores(k);
            out += loopTail;
            out += "    }\n";
            return out;
        }

        std::string emitPair(uint32_t index)
        {
            const Pair &p = pairs[index];
            std::string out = label(p.pc) + ":\n";
            if (p.fallback)
            {
                exits.insert("exit_fallback");
                out += "        { epc = " + hx(p.pc) + "; goto exit_fallback; }\n";
                return out;
            }
            exits.insert("exit_budget");
            const std::string stub = "{ epc = " + hx(p.pc) + "; goto exit_budget; }";
            out += body(p, stub);
            const uint32_t next = nextPc(p.pc);
            if (p.branch)
            {
                const Pair &slot = pairs[next / 8u];
                std::string state = "{ vu.m_state.branchPending = true; vu.m_state.branchTarget = " +
                                    (p.dynamic ? std::string("jt") : hx(p.target)) + "; vu.m_state.branchDelay = 0u; }";
                if (p.conditional)
                {
                    usesTk = true;
                    state = "if (tk) " + state;
                }
                if (p.dynamic)
                    usesJt = true;
                const std::string slotStub = "{ epc = " + hx(next) + "; " + state + " goto exit_budget; }";
                out += "        // delay slot\n";
                out += body(slot, slotStub);
                out += resolution(p);
                return out;
            }
            if (p.d.eBit)
            {
                const Pair &slot = pairs[next / 8u];
                const std::string slotStub = "{ epc = " + hx(next) + "; vu.m_state.ebit = true; goto exit_budget; }";
                out += "        // end delay slot\n";
                out += body(slot, slotStub);
                exits.insert("exit_ended");
                out += "        { epc = " + hx(nextPc(next)) + "; goto exit_ended; }\n";
                return out;
            }
            if (next / 8u != index + 1u || index + 1u >= last || segs.count(index + 1u))
                out += "        " + jumpTo(next) + "\n";
            return out;
        }
    };

    bool plainPair(const Pair &p)
    {
        return !p.reserved && !p.branch && !p.d.eBit && !p.d.dBit && !p.d.tBit;
    }

    // One compile unit: a whole captured image (vu1_<hash>.bin, 16 KB, every pair known) or a microcode upload found on
    // the disc (vup_<pair address>_<tag>.bin: the bytes of one MPG upload, or a chain of them, at that address; every
    // other pair is unknown). Units are cut into blocks as before; each block is emitted once under the hash of its
    // generated code (fastBlock<code hash, 0>), so a block shared by several images or uploads is compiled once.
    struct UnitBlock
    {
        uint32_t first = 0;     // pair range the block runs
        uint32_t last = 0;
        uint32_t checkStart = 0; // the block's code depends on pairs [checkStart, checkEnd): its own, the delay slot
        uint32_t checkEnd = 0;   // and the pair after it, and those its dead-flag analysis looked through
        bool noSticky = false;  // compiled for code memory without status flag readers
        uint64_t key = 0;       // fastBlock<key, 0u>
        // Bit k: the block may also be entered for pairs [first + k, last) alone, when only [first + k, checkEnd)
        // match (no branch from there back before first + k; set for blocks whose code needs nothing outside).
        uint64_t suffixSafe[2] = {0u, 0u};
    };
    struct UnitOut
    {
        std::string name;
        uint32_t start = 0;     // known pairs [start, end)
        uint32_t end = 0;
        std::vector<uint8_t> bytes; // the known pairs
        std::vector<UnitBlock> blocks;
        bool image = false;
    };
    struct EmitState
    {
        std::set<uint64_t> emitted;                 // block keys written to a part file
        std::vector<std::string> pending;           // block texts not yet written
        size_t pendingBytes = 0;
        uint32_t part = 0;
    };

    bool flushParts(const std::filesystem::path &outDir, EmitState &state, bool force)
    {
        while (!state.pending.empty() && (force || state.pendingBytes >= kPartBytes))
        {
            std::string text = "#include \"ps2_vu1_impl.inl\"\n\n";
            size_t size = 0u;
            size_t taken = 0u;
            while (taken < state.pending.size() && (size == 0u || size + state.pending[taken].size() <= kPartBytes))
            {
                size += state.pending[taken].size();
                text += state.pending[taken++];
            }
            state.pending.erase(state.pending.begin(), state.pending.begin() + static_cast<std::ptrdiff_t>(taken));
            state.pendingBytes -= size;
            char partName[64];
            std::snprintf(partName, sizeof(partName), "vu1_part_%04u.cpp", state.part++);
            std::ofstream partFile(outDir / partName, std::ios::binary);
            partFile << text;
            if (!partFile)
                return false;
        }
        return true;
    }

    // code: the 16 KB micro memory as this unit knows it. Pairs outside [knownStart, knownEnd) are unknown: they are
    // treated as reserved (never compiled, never looked through), so no block depends on them.
    // exactVu0: an image that may be a VU0 program keeps the exact FMACs (the PS2X_VU0_FAST=0 variant).
    // mayBeVu0Out: whether it fits in VU0's micro memory (images only; uploads are compiled for VU1).
    bool compileUnit(EmitState &state, const std::filesystem::path &outDir, const std::vector<uint8_t> &code,
                     uint32_t knownStart, uint32_t knownEnd, bool isImage, uint32_t maxBlockPairs,
                     std::map<std::string, uint32_t> &stats, bool exactVu0, bool *mayBeVu0Out, UnitOut &unit)
    {
        VU1Interpreter vu(VU1Interpreter::Unit::VU1);
        const uint32_t total = kCodeSize / 8u;
        uint32_t used = 0u;
        for (uint32_t pc = knownStart * 8u; pc + 8u <= knownEnd * 8u; pc += 8u)
            if (std::any_of(code.begin() + pc, code.begin() + pc + 8u, [](uint8_t b) { return b != 0u; }))
                used = pc / 8u + 1u;
        const uint32_t count = isImage ? std::min(total, used + 2u) : knownEnd;
        const auto known = [&](uint32_t index) { return index >= knownStart && index < knownEnd; };

        Emitter emitter;
        const bool mayBeVu0 = isImage && used * 8u <= 4096u;
        if (mayBeVu0Out)
            *mayBeVu0Out = mayBeVu0;
        emitter.fastFmac = gFastFmac && (!mayBeVu0 || (gVu0Fast && !exactVu0));
        bool readsStatus = false;
        for (uint32_t pc = knownStart * 8u; pc + 8u <= knownEnd * 8u; pc += 8u)
        {
            uint32_t lower, upper;
            std::memcpy(&lower, code.data() + pc, 4u);
            std::memcpy(&upper, code.data() + pc + 4u, 4u);
            const uint32_t opHi = (lower >> 25) & 0x7Fu;
            if (!(upper & 0x80000000u) && opHi >= 0x14u && opHi <= 0x17u)
                readsStatus = true;
        }
        emitter.noSticky = emitter.fastFmac && !readsStatus && !gSticky;
        stats[emitter.noSticky ? "units without sticky accumulation" : "units keeping sticky accumulation"] += 1u;
        emitter.pairs.resize(total);
        uint32_t reserved = 0u;
        for (uint32_t index = 0; index < total; ++index)
        {
            Pair &p = emitter.pairs[index];
            p.pc = index * 8u;
            if (!known(index))
            {
                p.reserved = true; // unknown: the interpreter runs it, nothing is compiled across it
                continue;
            }
            p.d = A::decodePair(vu, code.data(), p.pc);
            p.reserved = p.d.upperUsage.reserved || p.d.lowerUsage.reserved;
            if (p.reserved)
                ++reserved;
            p.branch = !p.d.iBit && A::pipeline(p.d.lowerUsage) == A::kPipeBranch;
            if (p.branch)
            {
                const uint32_t opHi = (p.d.lower >> 25) & 0x7Fu;
                p.dynamic = opHi == 0x24u || opHi == 0x25u;
                p.conditional = opHi >= 0x28u;
                p.target = (p.pc + 8u + static_cast<uint32_t>(fImm11(p.d.lower) * 8)) & 0x3FFFu;
            }
        }
        // An upload whose bytes do not decode as VU code (a false MPG match in the scanned files) is skipped.
        if (!isImage && reserved * 16u > knownEnd - knownStart)
        {
            ++stats["uploads skipped (not VU code)"];
            return true;
        }
        for (uint32_t index = knownStart; index < count; ++index)
        {
            Pair &p = emitter.pairs[index];
            const Pair &next = emitter.pairs[nextPc(p.pc) / 8u];
            p.fallback = p.reserved || p.d.dBit || p.d.tBit;
            if ((p.branch || p.d.eBit) && !plainPair(next))
                p.fallback = true;
            if (p.branch && p.d.eBit)
                p.fallback = true;
            const uint32_t op = p.d.upper & 0x3Fu;
            const uint32_t special = (p.d.upper & 3u) | ((p.d.upper >> 4) & 0x7Cu);
            if ((op == 0x2Eu || (op >= 0x3Cu && special == 0x2Eu)) && (fDest(p.d.upper) & 0xEu) != 0xEu && fDest(p.d.upper) != 0u)
                p.fallback = true;
            if (p.fallback)
                ++stats["fallback pairs"];
            if (!p.d.iBit && !p.reserved && lowerEmit(p.pc, p.d.lower, p.d.upper).generic)
                ++stats["generic lower"];
            if (!p.reserved && upperEmit(p.d.upper).generic)
                ++stats["generic upper"];
        }

        // Flag readers reachable from each pair (see Emitter::flagLive): backwards fixed point over the static control
        // flow of this unit (fall-through, branch targets, delay slots; an E bit ends the program after its delay slot).
        // Unknown pairs count as readers. A block's dead-flag decisions depend on every pair the search went through
        // from it, so those pairs are part of what the block is checked against at run time (blockReach below).
        const auto readsFlags = [](const Pair &p)
        {
            if (p.d.iBit || p.reserved)
                return false;
            const uint32_t opHi = (p.d.lower >> 25) & 0x7Fu;
            return (opHi >= 0x14u && opHi <= 0x18u) || opHi == 0x1Au || opHi == 0x1Bu;
        };
        std::vector<bool> target(total, false);
        for (uint32_t index = 0; index < total; ++index)
            if (emitter.pairs[index].branch && !emitter.pairs[index].dynamic)
                target[emitter.pairs[index].target / 8u] = true;
        std::vector<std::vector<uint32_t>> successors(total);
        std::vector<bool> seed(total, false);
        for (uint32_t index = 0; index < total; ++index)
        {
            const Pair &p = emitter.pairs[index];
            const uint32_t slot = nextPc(p.pc) / 8u;
            if (readsFlags(p) || (p.branch && p.dynamic) || p.fallback || p.reserved)
                seed[index] = true; // an actual reader, or a continuation this analysis cannot follow
            const bool slotOfEnd = index > 0u && emitter.pairs[index - 1u].d.eBit;
            if (p.branch)
            {
                successors[index].push_back(slot);
                if (!p.dynamic)
                    successors[slot].push_back(p.target / 8u);
            }
            // Fall-through: always, except after an E-bit delay slot that nothing jumps to (the program ends).
            if (!(slotOfEnd && !target[index]))
                successors[index].push_back(slot);
        }
        std::vector<bool> live = seed;
        for (bool changed = true; changed;)
        {
            changed = false;
            for (uint32_t index = total; index-- > 0u;)
            {
                if (live[index])
                    continue;
                for (const uint32_t next : successors[index])
                    if (next < total && live[next])
                    {
                        live[index] = true;
                        changed = true;
                        break;
                    }
            }
        }
        emitter.flagLive = live;
        // The pairs the liveness of [first, last) was decided from: forward from the block, not past a reader.
        const auto blockReach = [&](uint32_t first, uint32_t last)
        {
            uint32_t low = first, high = last;
            std::vector<bool> seen(total, false);
            std::vector<uint32_t> work;
            for (uint32_t index = first; index < last; ++index)
            {
                seen[index] = true;
                work.push_back(index);
            }
            while (!work.empty())
            {
                const uint32_t index = work.back();
                work.pop_back();
                low = std::min(low, index);
                high = std::max(high, index + 1u);
                if (seed[index])
                    continue;
                for (const uint32_t next : successors[index])
                    if (next < total && !seen[next])
                    {
                        seen[next] = true;
                        work.push_back(next);
                    }
            }
            return std::make_pair(low, high);
        };

        emitter.isTarget.assign(total + 1u, false);
        for (uint32_t index = knownStart; index < count; ++index)
        {
            const Pair &p = emitter.pairs[index];
            if (p.branch && !p.dynamic)
                emitter.isTarget[p.target / 8u] = true;
        }

        std::vector<bool> spanned(count + 1u, false);
        for (uint32_t index = knownStart; index < count; ++index)
        {
            const Pair &p = emitter.pairs[index];
            if (!p.branch || p.dynamic)
                continue;
            const uint32_t target = p.target / 8u;
            if (target <= index && target >= knownStart)
                for (uint32_t cut = target + 1u; cut <= std::min(count, index + 1u); ++cut)
                    spanned[cut] = true;
        }
        std::vector<std::pair<uint32_t, uint32_t>> blocks;
        for (uint32_t start = knownStart; start < count;)
        {
            uint32_t cut;
            if (isImage)
            {
                const uint32_t end = std::min(count, start + maxBlockPairs);
                cut = end;
                while (cut < count && cut > start + maxBlockPairs / 4u && spanned[cut])
                    --cut;
                if (cut < count && spanned[cut])
                    cut = end;
            }
            else
            {
                // Uploads: blocks end on a fixed grid of micro memory addresses. When a later upload overwrites the
                // start of this one, the blocks past its end still match, so only the pairs up to the next grid line
                // are left to the interpreter. A loop that spans a grid line keeps its block going (up to
                // maxBlockPairs).
                cut = std::min(count, (start / kUploadGridPairs + 1u) * kUploadGridPairs);
                while (cut < count && spanned[cut] && cut - start < maxBlockPairs)
                    cut = std::min(count, cut + kUploadGridPairs);
            }
            blocks.emplace_back(start, cut);
            start = cut;
        }

        unit.image = isImage;
        unit.start = knownStart;
        // An image's blocks were compiled seeing the zero pairs after its code: those are part of what they depend on.
        unit.end = isImage ? std::min(total, count + 2u) : knownEnd;
        unit.bytes.assign(code.begin() + unit.start * 8u, code.begin() + unit.end * 8u);
        // One block: emitted with the given flag liveness, checked at run time against pairs [checkStart, checkEnd).
        const auto emitBlock = [&](uint32_t start, uint32_t end, const std::vector<bool> &live, uint32_t checkStart,
                                   uint32_t checkEnd, bool suffixes) -> bool
        {
            emitter.first = start;
            emitter.last = end;
            emitter.usesTk = false;
            emitter.usesJt = false;
            emitter.exits.clear();
            emitter.flagLive = live;
            emitter.planSegments();
            std::string body;
            for (uint32_t index = start; index < end; ++index)
                body += emitter.emitPair(index);
            emitter.selfLoops = 0u;
            for (const uint32_t segment : emitter.segs)
                body += emitter.emitSegment(segment);
            stats["known segments"] += static_cast<uint32_t>(emitter.segs.size());
            stats["self-looping segments"] += emitter.selfLoops;
            std::ostringstream out;
            out << "    uint64_t cyc = ctx.cycle;\n    const uint64_t budget = ctx.budget;\n    const uint8_t *const mem = ctx.mem;\n";
            out << "    uint32_t epc = 0u;\n";
            if (emitter.usesTk)
                out << "    bool tk = false;\n";
            if (emitter.usesJt)
                out << "    uint32_t jt = 0u;\n";
            out << "    (void)mem;\n";
            out << "    switch (vu.m_state.pc)\n    {\n";
            for (uint32_t index = start; index < end; ++index)
                out << "    case " << hx(index * 8u) << ": goto " << (emitter.segs.count(index) ? segLabel(index * 8u) : label(index * 8u)) << ";\n";
            out << "    default: return StepResult::Fallback;\n    }\n";
            out << body;
            const std::pair<const char *, const char *> exitKinds[] = {
                {"exit_budget", "Budget"}, {"exit_continue", "Continue"}, {"exit_ended", "Ended"}, {"exit_fallback", "Fallback"}};
            for (const auto &[name, result] : exitKinds)
                if (emitter.exits.count(name))
                    out << name << ":\n    ctx.cycle = cyc;\n    vu.m_state.pc = epc;\n    return StepResult::" << result << ";\n";
            out << "}\n\n";
            std::string text = out.str();
            if (mayBeVu0)
            {
                // VU0 programs check their data addresses for VU1's register window (noteVu0DataAddress).
                for (size_t at = 0; (at = text.find("qaddr(", at)) != std::string::npos; at += 8u)
                    text.replace(at, 6u, "qaddrV0(");
            }
            const uint64_t key = fnv1a64(reinterpret_cast<const uint8_t *>(text.data()), text.size());
            UnitBlock unitBlock;
            unitBlock.first = start;
            unitBlock.last = end;
            unitBlock.checkStart = checkStart;
            unitBlock.checkEnd = checkEnd;
            unitBlock.noSticky = emitter.noSticky;
            unitBlock.key = key;
            if (suffixes && end - start <= 128u)
            {
                // Entering at p runs per-pair code from p on and jumps only to pairs at or after p, unless some branch
                // at or after p goes back before it.
                for (uint32_t p = start + 1u; p < end; ++p)
                {
                    bool safe = true;
                    for (uint32_t index = p; index < end && safe; ++index)
                    {
                        const Pair &pair = emitter.pairs[index];
                        if (pair.branch && !pair.dynamic && pair.target / 8u >= start && pair.target / 8u < p)
                            safe = false;
                    }
                    if (safe)
                        unitBlock.suffixSafe[(p - start) / 64u] |= 1ull << ((p - start) % 64u);
                }
            }
            unit.blocks.push_back(unitBlock);
            ++stats["blocks"];
            if (!state.emitted.insert(key).second)
            {
                ++stats["blocks shared with an earlier unit"];
                return true;
            }
            std::string full = "template <>\nVU1InterpreterAccess::StepResult VU1InterpreterAccess::fastBlock<" + hex64(key) +
                               ", 0u>(VU1Interpreter &vu, Vu1FastCtx &ctx)\n{\n" + text;
            state.pendingBytes += full.size();
            state.pending.push_back(std::move(full));
            return flushParts(outDir, state, false);
        };
        for (const auto &[start, end] : blocks)
        {
            for (uint32_t index = start; index < end; ++index)
                if (!emitter.flagLive[index])
                    ++stats["pairs with dead MAC/status flags"];
            const auto [reachLow, reachHigh] = blockReach(start, end);
            const uint32_t localEnd = std::min(end + 2u, unit.end);
            const uint32_t checkStart = std::max(unit.start, std::min(start, reachLow));
            const uint32_t checkEnd = std::min(std::max(end + 2u, reachHigh), unit.end);
            const std::vector<bool> unitLive = emitter.flagLive;
            if (!emitBlock(start, end, unitLive, checkStart, checkEnd, false))
                return false;
            emitter.flagLive = unitLive;
            // An upload's block whose dead-flag analysis looked outside it also gets a variant that assumes a flag
            // reader past every exit: it needs only its own pairs, so it still serves when a later upload has replaced
            // the code around it. The assembly tries the precise one first.
            if (!isImage && (checkStart != start || checkEnd != localEnd))
            {
                std::vector<bool> local(total, true);
                for (uint32_t index = start; index < end; ++index)
                    local[index] = seed[index];
                for (bool changed = true; changed;)
                {
                    changed = false;
                    for (uint32_t index = end; index-- > start;)
                    {
                        if (local[index])
                            continue;
                        for (const uint32_t next : successors[index])
                            if (next < start || next >= end || local[next])
                            {
                                local[index] = true;
                                changed = true;
                                break;
                            }
                    }
                }
                ++stats["upload blocks with a local variant"];
                if (!emitBlock(start, end, local, start, localEnd, true))
                    return false;
                emitter.flagLive = unitLive;
            }
        }
        std::cout << unit.name << ": pairs " << hx(unit.start * 8u) << ".." << hx(unit.end * 8u) << ", " << blocks.size() << " blocks\n";
        return true;
    }
}

static int run(int argc, char **argv);

int main(int argc, char **argv)
{
    // Every line goes out at once: a crash still leaves the last unit compiled in the log.
    std::cout << std::unitbuf;
    try
    {
        return run(argc, argv);
    }
    catch (const std::exception &e)
    {
        std::cerr << "vu1_recomp: " << e.what() << std::endl;
        return 1;
    }
}

static int run(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: vu1_recomp <capture-dir> <out-dir> [max-pairs-per-block]\n"
                     "  capture-dir: vu1_<hash>.bin (16 KB micro memory images, as captured) and vup_<pair address hex>_<tag>.bin\n"
                     "  (microcode uploads found on the disc: the bytes an MPG chain writes at that pair address)\n";
        return 2;
    }
    const std::filesystem::path captureDir = argv[1];
    const std::filesystem::path outDir = argv[2];
    const uint32_t blockPairs = argc > 3 ? static_cast<uint32_t>(std::stoul(argv[3])) : kDefaultBlockPairs;
    std::filesystem::create_directories(outDir);
    for (const auto &entry : std::filesystem::directory_iterator(outDir))
    {
        const std::string name = entry.path().filename().string();
        if (name.rfind("vu1_", 0) == 0 && entry.path().extension() == ".cpp")
            std::filesystem::remove(entry.path());
    }

    std::vector<std::filesystem::path> images;
    std::vector<std::filesystem::path> uploads;
    for (const auto &entry : std::filesystem::directory_iterator(captureDir))
    {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".bin")
            continue;
        if (name.rfind("vu1_", 0) == 0)
            images.push_back(entry.path());
        else if (name.rfind("vup_", 0) == 0)
            uploads.push_back(entry.path());
    }
    std::sort(images.begin(), images.end());
    std::sort(uploads.begin(), uploads.end());

    EmitState state;
    std::map<std::string, uint32_t> stats;
    std::vector<uint64_t> imageHashes;           // runImage<hash>: exact-image fast path, as before
    std::map<uint64_t, std::vector<UnitBlock>> imageBlocks;
    std::vector<UnitOut> units;                  // assembled at run time for any micro memory contents
    std::set<std::string> unitBytesSeen;
    const auto readFile = [](const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    for (const auto &path : images)
    {
        std::vector<uint8_t> code = readFile(path);
        if (code.size() != kCodeSize)
        {
            std::cerr << path << ": expected " << kCodeSize << " bytes\n";
            return 1;
        }
        const uint64_t hash = fnv1a64(code.data(), code.size());
        bool mayBeVu0 = false;
        UnitOut unit;
        char imageName[32];
        std::snprintf(imageName, sizeof(imageName), "vu1_%016" PRIx64, hash);
        unit.name = imageName;
        if (!compileUnit(state, outDir, code, 0u, kCodeSize / 8u, true, blockPairs, stats, !gVu0Fast, &mayBeVu0, unit))
            return 1;
        imageHashes.push_back(hash);
        imageBlocks[hash] = unit.blocks;
        if (!mayBeVu0)
            units.push_back(std::move(unit)); // VU1 code: its blocks also serve other memory contents
        if (mayBeVu0 && gVu0Fast)
        {
            UnitOut exact;
            exact.name = unit.name + " (VU0 exact)";
            if (!compileUnit(state, outDir, code, 0u, kCodeSize / 8u, true, blockPairs, stats, true, nullptr, exact))
                return 1;
            imageHashes.push_back(hash ^ kVu0ExactTag);
            imageBlocks[hash ^ kVu0ExactTag] = exact.blocks;
        }
    }
    for (const auto &path : uploads)
    {
        const std::string name = path.stem().string();
        const size_t underscore = name.find('_', 4u);
        const uint32_t address = static_cast<uint32_t>(std::stoul(name.substr(4u, underscore - 4u), nullptr, 16));
        const std::vector<uint8_t> bytes = readFile(path);
        if (bytes.empty() || bytes.size() % 8u != 0u || address * 8u + bytes.size() > kCodeSize)
        {
            std::cerr << path << ": bad upload (address " << address << ", " << bytes.size() << " bytes)\n";
            return 1;
        }
        std::cout << "compiling " << name << " (" << bytes.size() / 8u << " pairs at " << hx(address * 8u) << ")\n";
        const std::string identity = std::to_string(address) + ":" + std::string(bytes.begin(), bytes.end());
        if (!unitBytesSeen.insert(identity).second)
            continue; // the same upload found twice
        std::vector<uint8_t> code(kCodeSize, 0u);
        std::copy(bytes.begin(), bytes.end(), code.begin() + address * 8u);
        UnitOut unit;
        unit.name = name;
        const size_t before = stats["uploads skipped (not VU code)"];
        if (!compileUnit(state, outDir, code, address, address + static_cast<uint32_t>(bytes.size() / 8u), false, blockPairs, stats,
                         false, nullptr, unit))
            return 1;
        if (stats["uploads skipped (not VU code)"] == before)
            units.push_back(std::move(unit));
    }
    if (!flushParts(outDir, state, true))
        return 1;
    stats["units for run-time assembly"] = static_cast<uint32_t>(units.size());
    for (const auto &[name, value] : stats)
        std::cout << name << ": " << value << "\n";

    std::ostringstream reg;
    reg << "// Generated by vu1_recomp. Do not edit.\n#include \"ps2_vu1_impl.inl\"\n\n";
    for (const uint64_t key : state.emitted)
        reg << "template <>\nVU1InterpreterAccess::StepResult VU1InterpreterAccess::fastBlock<" << hex64(key)
            << ", 0u>(VU1Interpreter &, Vu1FastCtx &);\n";
    reg << "\n";
    for (const uint64_t hash : imageHashes)
    {
        const auto &blocks = imageBlocks[hash];
        reg << "template <>\nVU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<" << hex64(hash)
            << ">(VU1Interpreter &vu, const Vu1RunArgs &args)\n{\n    static constexpr FastBlock table[] = {\n";
        uint32_t entries = 0u;
        for (const UnitBlock &block : blocks)
            for (uint32_t index = block.first; index < block.last; ++index, ++entries)
                reg << "        &fastBlock<" << hex64(block.key) << ", 0u>,\n";
        reg << "    };\n    return runFast(vu, args, table, " << entries << "u);\n}\n\n";
    }
    for (size_t index = 0; index < units.size(); ++index)
    {
        const UnitOut &unit = units[index];
        reg << "// " << unit.name << "\nstatic const uint8_t kUnitBytes" << index << "[] = {";
        for (size_t at = 0; at < unit.bytes.size(); ++at)
            reg << (at % 32u == 0u ? "\n    " : "") << static_cast<unsigned>(unit.bytes[at]) << ",";
        reg << "\n};\nstatic const Vu1CompiledUnitBlock kUnitBlocks" << index << "[] = {\n";
        for (const UnitBlock &block : unit.blocks)
            reg << "    {" << block.first << "u, " << block.last << "u, " << block.checkStart << "u, " << block.checkEnd << "u, {"
                << hex64(block.suffixSafe[0]) << ", " << hex64(block.suffixSafe[1]) << "}, " << (block.noSticky ? "true" : "false")
                << ", &VU1InterpreterAccess::fastBlock<" << hex64(block.key) << ", 0u>},\n";
        reg << "};\n\n";
    }
    reg << "void registerGeneratedVu1Programs()\n{\n";
    for (const uint64_t hash : imageHashes)
        reg << "    registerVu1CompiledProgram(" << hex64(hash) << ", &VU1InterpreterAccess::runImage<" << hex64(hash) << ">);\n";
    for (size_t index = 0; index < units.size(); ++index)
        reg << "    registerVu1CompiledUnit(Vu1CompiledUnit{" << units[index].start << "u, " << units[index].end << "u, kUnitBytes" << index
            << ", kUnitBlocks" << index << ", " << units[index].blocks.size() << "u, " << (units[index].image ? "true" : "false")
            << "});\n";
    reg << "}\n";
    std::ofstream file(outDir / "vu1_programs.cpp", std::ios::binary);
    file << reg.str();
    return file ? 0 : 1;
}
