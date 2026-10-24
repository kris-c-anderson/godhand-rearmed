// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1_hooks.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <smmintrin.h>

// vu/ps2_vu1_core.cpp: with PS2X_VU1_CAPTURE set, every VU1 microcode upload is saved for vu1_recomp.
void ps2xNoteVu1Upload(uint32_t byteAddress, const uint8_t *data, uint32_t size);

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

// Defined in gs/gs_frontend.cpp: true unless PS2X_GIF_CONTINUATION=0.
bool ps2xGsGifContinuationEnabled();

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage)
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }

    template <uint32_t Vl, uint32_t Components, bool ZeroExtend, uint32_t Mode>
    void unpackScalarFast(uint8_t *vuData, uint32_t vuAddr, const uint8_t *src, uint32_t count, uint32_t cl, uint32_t wl, uint32_t *row)
    {
        constexpr uint32_t bytesPerComponent = Vl == 0u ? 4u : (Vl == 1u ? 2u : 1u);
        constexpr uint32_t bytesPerVector = bytesPerComponent * Components;
        uint32_t destVec = vuAddr;
        uint32_t cyclePos = 0u;
        for (uint32_t index = 0; index < count; ++index)
        {
            uint8_t *dst = vuData + (destVec & 0x3FFu) * 16u;
            const uint8_t *srcVec = src + index * bytesPerVector;
            uint32_t v[4];
            if constexpr (Components < 4u)
                std::memcpy(v, dst, sizeof(v));
            for (uint32_t c = 0; c < Components; ++c)
            {
                if constexpr (Vl == 0u)
                    std::memcpy(&v[c], srcVec + c * 4u, 4u);
                else if constexpr (Vl == 1u)
                {
                    uint16_t raw;
                    std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                    v[c] = ZeroExtend ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                }
                else
                    v[c] = ZeroExtend ? static_cast<uint32_t>(srcVec[c]) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcVec[c])));
            }
            if constexpr (Components == 1u)
                v[1] = v[2] = v[3] = v[0];
            if constexpr (Components == 2u)
            {
                v[2] = v[0]; // V2 writes XYXY on hardware (as PCSX2; fix from the God of War recomp)
                v[3] = v[1];
            }
            if constexpr (Mode != 0u)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    v[c] += row[c];
                    if constexpr (Mode == 2u)
                        row[c] = v[c];
                }
            }
            std::memcpy(dst, v, sizeof(v));
            ++destVec;
            if (++cyclePos == wl)
            {
                cyclePos = 0u;
                destVec += cl - wl;
            }
        }
    }

    bool simdUnpackEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_VIF_SIMD_UNPACK");
            return !value || *value != '0';
        }();
        return enabled;
    }

    template <uint32_t Vl, uint32_t Components, bool ZeroExtend, uint32_t Mode>
    void unpackFast(uint8_t *vuData, uint32_t vuAddr, const uint8_t *src, uint32_t count, uint32_t cl, uint32_t wl, uint32_t *row)
    {
        if (!simdUnpackEnabled())
        {
            unpackScalarFast<Vl, Components, ZeroExtend, Mode>(vuData, vuAddr, src, count, cl, wl, row);
            return;
        }
        constexpr uint32_t bytes = (Vl == 0u ? 4u : Vl == 1u ? 2u : 1u) * Components;
        const __m128i mask = _mm_setr_epi32(-1, Components > 1u ? -1 : 0, Components > 2u ? -1 : 0, Components > 3u ? -1 : 0);
        __m128i rowValue = _mm_loadu_si128(reinterpret_cast<const __m128i *>(row));
        uint32_t destVec = vuAddr;
        uint32_t cyclePos = 0u;
        for (uint32_t index = 0; index < count; ++index)
        {
            const uint8_t *source = src + index * bytes;
            __m128i value;
            if constexpr (Vl == 0u)
            {
                if constexpr (Components == 4u)
                    value = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source));
                else
                {
                    alignas(16) uint32_t lanes[4]{};
                    std::memcpy(lanes, source, bytes);
                    value = _mm_load_si128(reinterpret_cast<const __m128i *>(lanes));
                }
            }
            else
            {
                uint64_t packed = 0u;
                std::memcpy(&packed, source, bytes);
                const __m128i raw = _mm_cvtsi64_si128(static_cast<int64_t>(packed));
                if constexpr (Vl == 1u)
                    value = ZeroExtend ? _mm_cvtepu16_epi32(raw) : _mm_cvtepi16_epi32(raw);
                else
                    value = ZeroExtend ? _mm_cvtepu8_epi32(raw) : _mm_cvtepi8_epi32(raw);
            }
            auto *destination = reinterpret_cast<__m128i *>(vuData + (destVec & 0x3FFu) * 16u);
            if constexpr (Components == 1u)
                value = _mm_shuffle_epi32(value, 0u);
            else if constexpr (Components == 2u)
                value = _mm_shuffle_epi32(value, 0x44); // V2 writes XYXY on hardware
            else if constexpr (Components < 4u)
                value = _mm_or_si128(_mm_and_si128(mask, value), _mm_andnot_si128(mask, _mm_loadu_si128(destination)));
            if constexpr (Mode != 0u)
            {
                value = _mm_add_epi32(value, rowValue);
                if constexpr (Mode == 2u)
                    rowValue = value;
            }
            _mm_storeu_si128(destination, value);
            ++destVec;
            if (++cyclePos == wl)
            {
                cyclePos = 0u;
                destVec += cl - wl;
            }
        }
        if constexpr (Mode == 2u)
            _mm_storeu_si128(reinterpret_cast<__m128i *>(row), rowValue);
    }

    template <uint32_t Vl, uint32_t Components>
    void unpackFast(uint8_t *vuData, uint32_t vuAddr, const uint8_t *src, uint32_t count, bool zeroExtend, uint32_t mode, uint32_t cl,
                    uint32_t wl, uint32_t *row)
    {
        if (zeroExtend)
        {
            if (mode == 1u)
                unpackFast<Vl, Components, true, 1u>(vuData, vuAddr, src, count, cl, wl, row);
            else if (mode == 2u)
                unpackFast<Vl, Components, true, 2u>(vuData, vuAddr, src, count, cl, wl, row);
            else
                unpackFast<Vl, Components, true, 0u>(vuData, vuAddr, src, count, cl, wl, row);
        }
        else
        {
            if (mode == 1u)
                unpackFast<Vl, Components, false, 1u>(vuData, vuAddr, src, count, cl, wl, row);
            else if (mode == 2u)
                unpackFast<Vl, Components, false, 2u>(vuData, vuAddr, src, count, cl, wl, row);
            else
                unpackFast<Vl, Components, false, 0u>(vuData, vuAddr, src, count, cl, wl, row);
        }
    }

    bool tryUnpackFast(uint8_t *vuData, uint32_t vuAddr, const uint8_t *src, uint32_t count, uint32_t vl, uint32_t components, bool zeroExtend,
                       uint32_t mode, uint32_t cl, uint32_t wl, uint32_t *row)
    {
        switch (vl * 4u + components - 1u)
        {
        case 0u: unpackFast<0u, 1u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 1u: unpackFast<0u, 2u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 2u: unpackFast<0u, 3u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 3u: unpackFast<0u, 4u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 4u: unpackFast<1u, 1u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 5u: unpackFast<1u, 2u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 6u: unpackFast<1u, 3u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 7u: unpackFast<1u, 4u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 8u: unpackFast<2u, 1u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 9u: unpackFast<2u, 2u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 10u: unpackFast<2u, 3u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        case 11u: unpackFast<2u, 4u>(vuData, vuAddr, src, count, zeroExtend, mode, cl, wl, row); return true;
        default: return false;
        }
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    // The game's own patches of what was just uploaded (ps2_vu1_hooks.h); the capture keeps the
                    // upload as the disc has it.
                    ps2x::vu1hooks::runUploadHook(m_vu1Code, PS2_VU1_CODE_SIZE, destAddr, copyBytes);
                    markVU1CodeModified();
                    ps2xNoteVu1Upload(destAddr, data + pos, copyBytes);
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                uint32_t imageQw = 0u;
                if (m_vif1PendingPath2ImageQwc != 0u)
                {
                    imageQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, qwCount);
                    std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(imageQw) * 16u, 0u);
                    const uint64_t imageTag =
                        static_cast<uint64_t>(imageQw & 0x7FFFu) |
                        ((m_vif1PendingPath2ImageQwc == imageQw) ? (1ull << 15) : 0ull) |
                        (static_cast<uint64_t>(kGifFmtImage) << 58);
                    std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
                    std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(imageQw) * 16u);
                    submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, m_vif1PendingPath2DirectHl);
                    m_vif1PendingPath2ImageQwc -= imageQw;
                    if (m_vif1PendingPath2ImageQwc == 0u)
                        m_vif1PendingPath2DirectHl = false;
                }

                if (imageQw < qwCount)
                {
                    const uint8_t *packets = data + pos + imageQw * 16u;
                    const uint32_t packetBytes = (qwCount - imageQw) * 16u;
                    submitGifPacket(GifPathId::Path2, packets, packetBytes, true, directHl);

                    // An IMAGE tag cut at the end of this DIRECT is continued by exactly one layer: the GS frontend's
                    // GIF continuation when it is on (default; it resumes the real tag with the next packet), else the
                    // synthesized IMAGE tag above. Doing both makes the frontend read the synthesized tag as a pixel
                    // qword and the upload's last qword as a new GIFtag (God Hand title: CLUT tail -> PACKED nloop
                    // 30975 that swallowed every later packet, FBP 0 never written, black screen).
                    const uint32_t pendingImageQw = ps2xGsGifContinuationEnabled() ? 0u : pendingGifImageQwc(packets, packetBytes);
                    if (pendingImageQw != 0u)
                    {
                        m_vif1PendingPath2ImageQwc = pendingImageQw;
                        m_vif1PendingPath2DirectHl = directHl;
                    }
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes && !maskEnable && cl >= wl && (vl != 3u || vn == 3u))
            {
                const uint8_t *srcBase = data + pos;
                const uint32_t mode = vif1_regs.mode & 3u;
                const bool add = (vl != 3u || vn != 3u) && (mode == 1u || mode == 2u);
                const uint32_t limit = components > 4 ? 4u : static_cast<uint32_t>(components);
                uint32_t firstWrite = 0u;
                if (vl == 0u && components == 4 && !add && cl == wl)
                {
                    const uint32_t first = std::min(writeVectorCount, 0x400u - vuAddr);
                    std::memcpy(m_vu1Data + vuAddr * 16u, srcBase, first * 16u);
                    if (first < writeVectorCount)
                        std::memcpy(m_vu1Data, srcBase + first * 16u, (writeVectorCount - first) * 16u);
                    firstWrite = writeVectorCount;
                }
                else if (vl != 3u && tryUnpackFast(m_vu1Data, vuAddr, srcBase, writeVectorCount, vl, static_cast<uint32_t>(components), zeroExtend,
                                                   add ? mode : 0u, cl, wl, vif1_regs.row))
                {
                    firstWrite = writeVectorCount;
                }
                for (uint32_t writeIndex = firstWrite; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t destVec = (vuAddr + (writeIndex / wl) * cl + writeIndex % wl) & 0x3FFu;
                    uint8_t *dst = m_vu1Data + destVec * 16u;
                    const uint8_t *srcVec = srcBase + writeIndex * bytesPerVector;
                    uint32_t v[4];
                    if (vl == 0u && components == 4)
                        std::memcpy(v, srcVec, sizeof(v));
                    else
                    {
                        std::memcpy(v, dst, sizeof(v));
                        if (vl == 0u)
                        {
                            if (components == 1)
                            {
                                uint32_t scalar;
                                std::memcpy(&scalar, srcVec, sizeof(scalar));
                                v[0] = v[1] = v[2] = v[3] = scalar;
                            }
                            else
                                std::memcpy(v, srcVec, limit * 4u);
                        }
                        else if (vl == 1u)
                        {
                            for (uint32_t c = 0; c < (components == 1 ? 1u : limit); ++c)
                            {
                                uint16_t raw;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                v[c] = zeroExtend ? static_cast<uint32_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                            }
                            if (components == 1)
                                v[1] = v[2] = v[3] = v[0];
                        }
                        else if (vl == 2u)
                        {
                            for (uint32_t c = 0; c < (components == 1 ? 1u : limit); ++c)
                                v[c] = zeroExtend ? static_cast<uint32_t>(srcVec[c]) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(srcVec[c])));
                            if (components == 1)
                                v[1] = v[2] = v[3] = v[0];
                        }
                        else
                        {
                            uint16_t packed;
                            std::memcpy(&packed, srcVec, sizeof(packed));
                            // V4-5 expands RGB to bits 3..7 and alpha to bit 7 (fix from the God of War recomp).
                            v[0] = (packed & 0x001Fu) << 3u;
                            v[1] = (packed & 0x03E0u) >> 2u;
                            v[2] = (packed & 0x7C00u) >> 7u;
                            v[3] = (packed & 0x8000u) >> 8u;
                        }
                    }
                    if (components == 2 && vl != 3u)
                    {
                        v[2] = v[0]; // V2 writes XYXY on hardware
                        v[3] = v[1];
                    }
                    if (add)
                    {
                        for (uint32_t c = 0; c < 4u; ++c)
                        {
                            v[c] += vif1_regs.row[c];
                            if (mode == 2u)
                                vif1_regs.row[c] = v[c];
                        }
                    }
                    std::memcpy(dst, v, sizeof(v));
                }
            }
            else if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5 expands RGB to bits 3..7 and alpha to bit 7 (fix from the God of War recomp).
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = (packed & 0x001Fu) << 3u;
                        decompressed[1] = (packed & 0x03E0u) >> 2u;
                        decompressed[2] = (packed & 0x7C00u) >> 7u;
                        decompressed[3] = (packed & 0x8000u) >> 8u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // V2 writes XYXY on hardware, before masks and STMOD apply (fix from the God of War recomp).
                    if (handledFormat && components == 2)
                    {
                        decompressed[2] = decompressed[0];
                        decompressed[3] = decompressed[1];
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
}
