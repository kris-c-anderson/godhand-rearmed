#pragma once

#include "ps2x/state_io.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_hooks.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <emmintrin.h>
#include <limits>
#include <string>
#include <vector>
#include <cstdlib>

static inline bool vu1JrUsesDelayedVi()
{
    static const bool delayed = [] { const char *v = std::getenv("PS2X_VU1_JR_DELAYED_VI"); return v && *v == '1'; }();
    return delayed;
}

#ifndef VU1_FORCE_INLINE
#if defined(_MSC_VER)
#define VU1_FORCE_INLINE __forceinline
#else
#define VU1_FORCE_INLINE inline __attribute__((always_inline))
#endif
#endif

#if defined(_MSC_VER)
#define VU1_NOINLINE __declspec(noinline)
#else
#define VU1_NOINLINE __attribute__((noinline))
#endif

namespace vu1_impl
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    inline int freeSlot(uint32_t validMask, uint32_t capacity)
    {
        const uint32_t freeMask = ~validMask & ((1u << capacity) - 1u);
        return freeMask == 0u ? -1 : std::countr_zero(freeMask);
    }

    struct Vu1PacketSink
    {
        std::vector<std::vector<uint8_t>> packets;
        bool forward = true;
    };

    inline thread_local Vu1PacketSink *packetSink = nullptr;
}

using namespace vu1_impl;

// On hardware, VU0 data addresses 0x4000-0x43FF (qwords 0x400-0x43F) reach VU1's registers: VF at 0x400-0x41F, VI at
// 0x420-0x42F, then the control registers (status at 0x430, MAC 0x431, clip 0x432...). That window is the only way
// anything outside VU1 can read VU1's status flag. The runtime does not map it, and vu1_recomp stops accumulating
// VU1 sticky status (docs/PROGRESS.md §45), so an access is logged (PS2X_VU1_VERIFY counts it as an error).
void ps2xVu1RegisterWindowAccess(uint32_t qword) noexcept; // ps2_vu1_core.cpp

static VU1_FORCE_INLINE void noteVu0DataAddress(bool vu0, uint32_t qword)
{
    if (vu0 && (qword & 0xFFC0u) == 0x400u)
        ps2xVu1RegisterWindowAccess(qword);
}

VU1_FORCE_INLINE float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

VU1_FORCE_INLINE float VU1Interpreter::normalizeOperand(float value) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0xFFu)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

VU1_FORCE_INLINE float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

VU1_FORCE_INLINE uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

VU1_FORCE_INLINE int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

VU1_FORCE_INLINE void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

VU1_FORCE_INLINE void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

VU1_FORCE_INLINE void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

VU1_FORCE_INLINE void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        long double exactResult = 0.0L;
        if (calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

inline bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const long double q = operand(m_state.q);
    const long double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

VU1_FORCE_INLINE uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

inline uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        float product = left * right;
        const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

VU1_FORCE_INLINE void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    const int slot = freeSlot(m_flagValid, kMaxFlagEntries);
    if (slot < 0)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    FlagPipelineEntry *entry = &m_flagPipeline[static_cast<size_t>(slot)];
    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
    m_flagValid |= 1u << slot;
    noteReadyCycle(entry->readyCycle);
}

VU1_FORCE_INLINE void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

VU1_FORCE_INLINE void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

VU1_FORCE_INLINE void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (uint32_t mask = m_flagValid; mask != 0u; mask &= mask - 1u)
    {
        FlagPipelineEntry &entry = m_flagPipeline[static_cast<size_t>(std::countr_zero(mask))];
        if (entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    const int slot = freeSlot(m_flagValid, kMaxFlagEntries);
    if (slot < 0)
    {
        reportReservedInstruction(false, 0xFFFFFFFEu);
        return;
    }
    FlagPipelineEntry &entry = m_flagPipeline[static_cast<size_t>(slot)];
    entry = {};
    entry.valid = true;
    entry.issueCycle = m_cycle;
    entry.readyCycle = m_cycle + kFmacLatency;
    entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
    entry.writesSticky = true;
    m_flagValid |= 1u << slot;
    noteReadyCycle(entry.readyCycle);
}

VU1_FORCE_INLINE void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    const int slot = freeSlot(m_flagValid, kMaxFlagEntries);
    if (slot < 0)
    {
        reportReservedInstruction(true, 0xFFFFFFFDu);
        return;
    }
    FlagPipelineEntry &entry = m_flagPipeline[static_cast<size_t>(slot)];
    entry = {};
    entry.valid = true;
    entry.issueCycle = m_cycle;
    entry.readyCycle = m_cycle + kFmacLatency;
    entry.clip = m_workingClip;
    entry.writesClip = true;
    m_flagValid |= 1u << slot;
    noteReadyCycle(entry.readyCycle);
}

VU1_FORCE_INLINE void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (uint32_t mask = m_flagValid; mask != 0u; mask &= mask - 1u)
    {
        FlagPipelineEntry &entry = m_flagPipeline[static_cast<size_t>(std::countr_zero(mask))];
        if (entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    const int slot = freeSlot(m_flagValid, kMaxFlagEntries);
    if (slot < 0)
    {
        reportReservedInstruction(false, 0xFFFFFFFAu);
        return;
    }
    FlagPipelineEntry &entry = m_flagPipeline[static_cast<size_t>(slot)];
    entry = {};
    entry.valid = true;
    entry.issueCycle = m_cycle;
    entry.readyCycle = m_cycle + kFmacLatency;
    entry.clip = m_workingClip;
    entry.writesClip = true;
    m_flagValid |= 1u << slot;
    noteReadyCycle(entry.readyCycle);
}

VU1_FORCE_INLINE void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
    noteReadyCycle(m_fdiv.readyCycle);
}

VU1_FORCE_INLINE void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            noteReadyCycle(entry.readyCycle);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

VU1_FORCE_INLINE void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    const int slot = freeSlot(m_storeValid, kMaxPendingStores);
    if (slot < 0)
    {
        reportReservedInstruction(false, 0xFFFFFFFCu);
        return;
    }
    PendingStore &store = m_storePipeline[static_cast<size_t>(slot)];
    store.valid = true;
    store.readyCycle = m_cycle + 1u;
    store.address = address;
    store.laneMask = laneMask;
    std::copy(words, words + 4, store.words.begin());
    m_storeValid |= 1u << slot;
    noteReadyCycle(store.readyCycle);
}

VU1_FORCE_INLINE void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    const int slot = freeSlot(m_vfWriteValid, kMaxPendingVfWrites);
    if (slot < 0)
    {
        reportReservedInstruction(false, 0xFFFFFFF7u);
        return;
    }
    PendingVfWrite &write = m_vfWritePipeline[static_cast<size_t>(slot)];
    write = {};
    write.valid = true;
    write.readyCycle = m_cycle + latency;
    write.sequence = ++m_nextWriteSequence;
    write.reg = reg;
    write.laneMask = laneMask;
    std::copy(value, value + 4, write.value.begin());
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((laneMask & laneForComponent(component)) != 0u)
            m_vfLatestWrite[reg][component] = write.sequence;
    }
    m_vfWriteValid |= 1u << slot;
    noteReadyCycle(write.readyCycle);
}

VU1_FORCE_INLINE void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    const int slot = freeSlot(m_viWriteValid, kMaxPendingViWrites);
    if (slot < 0)
    {
        reportReservedInstruction(false, 0xFFFFFFF6u);
        return;
    }
    PendingViWrite &write = m_viWritePipeline[static_cast<size_t>(slot)];
    write = {};
    write.valid = true;
    write.readyCycle = m_cycle + latency;
    write.sequence = ++m_nextWriteSequence;
    write.reg = reg;
    write.value = value;
    m_viLatestWrite[reg] = write.sequence;
    m_viWriteValid |= 1u << slot;
    noteReadyCycle(write.readyCycle);
}

VU1_FORCE_INLINE void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    const int slot = freeSlot(m_accWriteValid, kMaxPendingAccWrites);
    if (slot < 0)
    {
        reportReservedInstruction(true, 0xFFFFFFF5u);
        return;
    }
    PendingAccWrite &write = m_accWritePipeline[static_cast<size_t>(slot)];
    write = {};
    write.valid = true;
    write.readyCycle = m_cycle + latency;
    write.sequence = ++m_nextWriteSequence;
    write.laneMask = laneMask;
    std::copy(value, value + 4, write.value.begin());
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((laneMask & laneForComponent(component)) != 0u)
            m_accLatestWrite[component] = write.sequence;
    }
    m_accWriteValid |= 1u << slot;
    noteReadyCycle(write.readyCycle);
}

inline void VU1Interpreter::commitReadyPipelines()
{
    if (m_cycle < m_nextCommitCycle)
        return;

    uint64_t next = std::numeric_limits<uint64_t>::max();

    for (uint32_t mask = m_flagValid; mask != 0u; mask &= mask - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
        FlagPipelineEntry &entry = m_flagPipeline[index];
        if (entry.readyCycle > m_cycle)
        {
            next = std::min(next, entry.readyCycle);
            continue;
        }

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
        m_flagValid &= ~(1u << index);
    }

    if (m_fdiv.valid)
    {
        if (m_fdiv.readyCycle <= m_cycle)
        {
            m_state.q = m_fdiv.value;
            const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
            m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            m_fdiv = {};
        }
        else
        {
            next = std::min(next, m_fdiv.readyCycle);
        }
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
            continue;
        if (entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
        }
        else
        {
            next = std::min(next, entry.readyCycle);
        }
    }

    for (uint32_t mask = m_storeValid; mask != 0u; mask &= mask - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
        PendingStore &store = m_storePipeline[index];
        if (store.readyCycle > m_cycle)
        {
            next = std::min(next, store.readyCycle);
            continue;
        }
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store = {};
        m_storeValid &= ~(1u << index);
    }

    for (uint32_t mask = m_vfWriteValid; mask != 0u; mask &= mask - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
        PendingVfWrite &write = m_vfWritePipeline[index];
        if (write.readyCycle > m_cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write.valid = false;
        m_vfWriteValid &= ~(1u << index);
    }

    for (uint32_t mask = m_viWriteValid; mask != 0u; mask &= mask - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
        PendingViWrite &write = m_viWritePipeline[index];
        if (write.readyCycle > m_cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write.valid = false;
        m_viWriteValid &= ~(1u << index);
    }

    for (uint32_t mask = m_accWriteValid; mask != 0u; mask &= mask - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
        PendingAccWrite &write = m_accWritePipeline[index];
        if (write.readyCycle > m_cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write.valid = false;
        m_accWriteValid &= ~(1u << index);
    }

    m_nextCommitCycle = next;
}

inline void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        const uint32_t firstSource = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        if (firstSource + 16u <= m_activeVuDataSize)
        {
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes, m_activeVuData + firstSource, 16u);
        }
        else
        {
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
                m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
            }
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

inline void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    if (Vu1PacketSink *sink = packetSink)
    {
        sink->packets.emplace_back(m_xgkick.packet.data(), m_xgkick.packet.data() + m_xgkick.totalBytes);
        if (!sink->forward)
        {
            m_xgkick.active = false;
            return;
        }
    }
    // The game's PATH1 filter (ps2_vu1_hooks.h): may change the packet in place and add packets after it.
    thread_local std::vector<uint8_t> extra;
    extra.clear();
    if (const ps2x::vu1hooks::Path1Filter filter = ps2x::vu1hooks::path1Filter(); filter && m_activeVuData)
        filter(m_xgkick.packet.data(), m_xgkick.totalBytes, m_xgkick.sourceAddress, m_activeVuData, m_activeVuDataSize,
               m_xgkick.kickPc, extra);
    if (m_activeMemory)
    {
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
        if (!extra.empty())
            m_activeMemory->submitGifPacket(GifPathId::Path1, extra.data(), static_cast<uint32_t>(extra.size()));
    }
    else if (m_activeGs)
    {
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
        if (!extra.empty())
            m_activeGs->processGIFPacket(extra.data(), static_cast<uint32_t>(extra.size()));
    }
    m_xgkick.active = false;
}

inline void VU1Interpreter::resetXgkick()
{
    m_xgkick.sourceAddress = 0u;
    m_xgkick.totalBytes = 0u;
    m_xgkick.copiedBytes = 0u;
    m_xgkick.currentTagEnd = 0u;
    m_xgkick.cycleCredit = 0u;
    m_xgkick.issueCycle = 0u;
    m_xgkick.active = false;
    m_xgkick.currentTagEop = false;
}

inline void VU1Interpreter::startXgkick(uint32_t qwordAddress, uint32_t kickPc)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    resetXgkick();
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.kickPc = kickPc;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

VU1_FORCE_INLINE void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

VU1_FORCE_INLINE void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
    {
        if (!m_xgkick.active && m_nextCommitCycle > m_cycle + 1u)
        {
            const uint64_t quietUntil = std::min(targetCycle, m_nextCommitCycle) - 1u;
            if (quietUntil > m_cycle)
                m_cycle = quietUntil;
        }
        advanceOneCycle();
    }
}

VU1_FORCE_INLINE uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((access.lanes & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_vfReady[access.reg][component]);
            }
        }
        for (uint32_t mask = usage->viRead & 0xFFFEu; mask != 0u; mask &= mask - 1u)
            ready = std::max(ready, m_viReady[static_cast<size_t>(std::countr_zero(mask))]);
        if (usage->accRead != 0u)
        {
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((usage->accRead & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_accReady[component]);
            }
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

VU1_FORCE_INLINE void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    for (uint32_t mask = decoded.lowerUsage.viWrite & 0xFFFEu; mask != 0u; mask &= mask - 1u)
        m_viReady[static_cast<size_t>(std::countr_zero(mask))] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    if (decoded.upperUsage.accWrite != 0u)
    {
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
                m_accReady[component] = m_cycle + kAccForwardLatency;
        }
    }
}


namespace vu1_impl
{
    inline int32_t vuFloatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }
}

// ============================================================================
// Upper instructions (FMAC pipeline)
// ============================================================================
inline void VU1Interpreter::execUpper(uint32_t instr)
{
    m_currentUpperInstruction = instr;
    uint8_t dest = DEST(instr);
    uint8_t ft = FT(instr);
    uint8_t fs = FS(instr);
    uint8_t fd = FD(instr);
    uint8_t op = instr & 0x3F;

    float *vd = m_state.vf[fd];
    float normalizedVs[4];
    float normalizedVt[4];
    float normalizedAcc[4];
    for (uint32_t component = 0; component < 4u; ++component)
    {
        normalizedVs[component] = normalizeOperand(m_state.vf[fs][component]);
        normalizedVt[component] = normalizeOperand(m_state.vf[ft][component]);
        normalizedAcc[component] = normalizeOperand(m_state.acc[component]);
    }
    const float *vs = normalizedVs;
    const float *vt = normalizedVt;
    const float *acc = normalizedAcc;
    const float q = normalizeOperand(m_state.q);
    const float i = normalizeOperand(m_state.i);
    float result[4];

    // Upper opcode decoding (bits 5:0 of upper word)
    switch (op)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03: // ADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07: // SUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B: // MADDbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: // MSUBbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: // MAXbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17: // MINIbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B: // MULbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x1C: // MULq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x1D: // MAXi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
    case 0x1E: // MULi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x1F: // MINIi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
    case 0x20: // ADDq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x21: // MADDq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x22: // ADDi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x23: // MADDi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x24: // SUBq
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x25: // MSUBq
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    case 0x26: // SUBi
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x27: // MSUBi
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    case 0x28: // ADD
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x29: // MADD
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2A: // MUL
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2B: // MAX
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;
    case 0x2C: // SUB
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2D: // MSUB
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    case 0x2E: // OPMSUB
        result[0] = acc[0] - vs[1] * vt[2];
        result[1] = acc[1] - vs[2] * vt[0];
        result[2] = acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
        applyFmacDest(vd, result, dest);
        return;
    case 0x2F: // MINI
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;

    // Upper special group (low op 0x3C..0x3F).
    // Like lower1 special, the real selector is not just bits 5:0.  Dobie decodes:
    //   op = (instr & 0x3) | ((instr >> 4) & 0x7C)
    // Several instructions in this group also use FT as the destination, not FD.
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    {
        const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        float *vtDest = m_state.vf[ft];

        switch (specialOp)
        {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03: // ADDAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07: // SUBAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B: // MADDAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F: // MSUBAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x10: // ITOF0
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x11: // ITOF4
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 16.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x12: // ITOF12
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 4096.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x13: // ITOF15
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 32768.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x14: // FTOI0
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 1.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x15: // FTOI4
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 16.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x16: // FTOI12
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 4096.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x17: // FTOI15
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 32768.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B: // MULAbc
        {
            float bc = broadcast(vt, specialOp & 3);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x1C: // MULAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x1D: // ABS
            for (int c = 0; c < 4; c++)
                result[c] = std::fabs(vs[c]);
            applyDest(vtDest, result, dest);
            return;
        case 0x1E: // MULAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x1F: // CLIP
        {
            uint32_t wBits = 0u;
            std::memcpy(&wBits, &m_state.vf[ft][3], sizeof(wBits));
            const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;

            const auto exceedsClipPlane = [limit](float value, uint32_t signMask)
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, &value, sizeof(bits));
                bits ^= signMask;
                int32_t orderedBits = 0;
                std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
                return orderedBits > limit;
            };

            uint32_t flags = 0u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x00000000u))
                flags |= 0x01u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x80000000u))
                flags |= 0x02u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x00000000u))
                flags |= 0x04u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x80000000u))
                flags |= 0x08u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x00000000u))
                flags |= 0x10u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x80000000u))
                flags |= 0x20u;
            queueClip(flags);
            return;
        }
        case 0x20: // ADDAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x21: // MADDAq
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x22: // ADDAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x23: // MADDAi
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x24: // SUBAq
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x25: // MSUBAq
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        case 0x26: // SUBAi
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x27: // MSUBAi
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        case 0x28: // ADDA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x29: // MADDA
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2A: // MULA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2C: // SUBA
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2D: // MSUBA
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        case 0x2E: // OPMULA
            result[0] = vs[1] * vt[2];
            result[1] = vs[2] * vt[0];
            result[2] = vs[0] * vt[1];
            result[3] = 0.0f;
            applyFmacDestAcc(result, dest);
            return;
        case 0x2F:
        case 0x30: // NOP
            return;
        default:
            reportReservedInstruction(true, instr);
            return;
        }
    }

    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    default:
        reportReservedInstruction(true, instr);
        return;
    }
}

namespace vu1_impl
{
    inline float vuEatan(float value)
    {
        constexpr float coefficients[] = {
            0.999999344348907f,
            -0.333298563957214f,
            0.199465364217758f,
            -0.139085337519646f, // PCSX2 value (was -0.130853..., a typo; fix from the God of War recomp)
            0.096420042216778f,
            -0.055909886956215f,
            0.021861229091883f,
            -0.004054057877511f};
        constexpr float quarterPi = 0.785398185253143f;

        const float squared = value * value;
        float polynomial = coefficients[7];
        for (int index = 6; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return quarterPi + value * polynomial;
    }

    inline float vuEsin(float value)
    {
        constexpr float coefficients[] = {
            1.0f,
            -0.166666567325592f,
            0.008333025500178f,
            -0.000198074136279f,
            0.000002601886990f};

        const float squared = value * value;
        float polynomial = coefficients[4];
        for (int index = 3; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return value * polynomial;
    }

    inline float vuEexp(float value)
    {
        constexpr float coefficients[] = {
            0.249998688697815f,
            0.031257584691048f,
            0.002591371303424f,
            0.000171562001924f,
            0.000005430199963f,
            0.000000690600018f};

        float polynomial = coefficients[5];
        for (int index = 4; index >= 0; --index)
            polynomial = coefficients[index] + value * polynomial;
        polynomial = 1.0f + value * polynomial;
        polynomial *= polynomial;
        polynomial *= polynomial;
        return polynomial != 0.0f ? 1.0f / polynomial : std::numeric_limits<float>::max();
    }
}

// ============================================================================
// Lower instructions
// ============================================================================
inline void VU1Interpreter::execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr)
{
    (void)upperInstr;
    if (instr == 0x00000000 || instr == 0x8000033C) // NOP
        return;

    uint8_t opHi = (instr >> 25) & 0x7F;
    const uint32_t pcMask = microAddressMask();

    // The lower instruction encoding uses bits 31:25 for the primary opcode
    switch (opHi)
    {
    case 0x00: // LQ (Load Quadword from VU data memory)
    {
        uint8_t it = FT(instr);  // VF destination
        uint8_t is = VIS(instr); // VI base
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u;
        noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            applyDest(m_state.vf[it], tmp, dest);
        }
        return;
    }
    case 0x01: // SQ (Store Quadword to VU data memory)
    {
        uint8_t is = FS(instr);  // VF source
        uint8_t it = VIT(instr); // VI base
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[it] + imm)) * 16u;
        noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            uint32_t words[4]{};
            std::memcpy(words, m_state.vf[is], sizeof(words));
            queueStore(addr, words, dest);
        }
        return;
    }
    case 0x04: // ILW (Integer Load Word from VU data memory)
    {
        uint8_t it = VIT(instr); // VI destination
        uint8_t is = VIS(instr); // VI base
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u;
        noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            int comp = 0;
            if (dest & 0x8)
                comp = 0;
            else if (dest & 0x4)
                comp = 1;
            else if (dest & 0x2)
                comp = 2;
            else
                comp = 3;
            uint32_t v;
            std::memcpy(&v, vuData + addr + comp * 4, 4);
            if (it != 0)
                m_state.vi[it] = (int32_t)(int16_t)(v & 0xFFFF);
        }
        return;
    }
    case 0x05: // ISW (Integer Store Word to VU data memory)
    {
        uint8_t it = VIT(instr); // VI source
        uint8_t is = VIS(instr); // VI base
        uint8_t dest = (instr >> 21) & 0xF;
        int16_t imm = IMM11(instr);
        uint32_t addr = ((uint32_t)(int32_t)(m_state.vi[is] + imm)) * 16u;
        noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
        addr &= (dataSize - 1);
        if (addr + 16 <= dataSize)
        {
            const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[it] & 0xFFFF));
            const uint32_t words[4] = {val, val, val, val};
            queueStore(addr, words, dest);
        }
        return;
    }
    case 0x08: // IADDIU
    {
        uint8_t it = VIT(instr);
        uint8_t is = VIS(instr);
        int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it] = (int16_t)(m_state.vi[is] + imm);
        return;
    }
    case 0x09: // ISUBIU
    {
        uint8_t it = VIT(instr);
        uint8_t is = VIS(instr);
        int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
        if (it != 0)
            m_state.vi[it] = (int16_t)(m_state.vi[is] - imm);
        return;
    }
    case 0x10: // FCEQ
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        if (1 != 0)
            m_state.vi[1] = ((m_state.clip & 0xFFFFFF) == imm24) ? 1 : 0;
        return;
    }
    case 0x11: // FCSET
    {
        queueFcset(instr & 0xFFFFFFu);
        return;
    }
    case 0x12: // FCAND
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        if (1 != 0)
            m_state.vi[1] = ((m_state.clip & imm24) != 0) ? 1 : 0;
        return;
    }
    case 0x13: // FCOR
    {
        uint32_t imm24 = instr & 0xFFFFFF;
        if (1 != 0)
            m_state.vi[1] = ((m_state.clip | imm24) == 0xFFFFFF) ? 1 : 0;
        return;
    }
    case 0x14: // FSEQ
    {
        const uint8_t it = VIT(instr);
        const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
        if (it != 0)
            m_state.vi[it] = ((m_state.status & 0xFFFu) == imm12) ? 1 : 0;
        return;
    }
    case 0x15: // FSSET
    {
        const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
        queueFsset(imm12);
        return;
    }
    case 0x16: // FSAND
    {
        const uint8_t it = VIT(instr);
        const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
        if (it != 0)
            m_state.vi[it] = static_cast<int32_t>((m_state.status & 0xFFFu) & imm12);
        return;
    }
    case 0x17: // FSOR
    {
        const uint8_t it = VIT(instr);
        const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
        if (it != 0)
            m_state.vi[it] = static_cast<int32_t>((m_state.status & 0xFFFu) | imm12);
        return;
    }
    case 0x18: // FMEQ
    {
        uint8_t it = VIT(instr);
        uint8_t is = VIS(instr);
        if (it != 0)
            m_state.vi[it] = ((m_state.mac & 0xFFFF) == (uint32_t)(uint16_t)m_state.vi[is]) ? 1 : 0;
        return;
    }
    case 0x1A: // FMAND
    {
        uint8_t it = VIT(instr);
        uint8_t is = VIS(instr);
        if (it != 0)
            m_state.vi[it] = (int32_t)(m_state.mac & (uint32_t)(uint16_t)m_state.vi[is]);
        return;
    }
    case 0x1B: // FMOR
    {
        uint8_t it = VIT(instr);
        uint8_t is = VIS(instr);
        if (it != 0)
            m_state.vi[it] = (int32_t)(m_state.mac | (uint32_t)(uint16_t)m_state.vi[is]);
        return;
    }
    case 0x1C: // FCGET
    {
        const uint8_t it = VIT(instr);
        if (it != 0)
            m_state.vi[it] = static_cast<int32_t>(m_state.clip & 0x0FFFu);
        return;
    }
    case 0x20: // B (unconditional branch)
    {
        int16_t imm = IMM11(instr);
        uint32_t target = (m_state.pc + 8 + imm * 8) & pcMask;
        m_state.branchPending = true;
        m_state.branchTarget = target;
        m_state.branchDelay = 1;
        return;
    }
    case 0x21: // BAL (Branch and link)
    {
        uint8_t it = VIT(instr);
        int16_t imm = IMM11(instr);
        uint32_t target = (m_state.pc + 8 + imm * 8) & pcMask;
        if (it != 0)
            m_state.vi[it] = (int32_t)((m_state.pc + 16) / 8);
        m_state.branchPending = true;
        m_state.branchTarget = target;
        m_state.branchDelay = 1;
        return;
    }
    case 0x24: // JR
    {
        uint8_t is = VIS(instr);
        // JR/JALR take the current VI (PCSX2 microVU, ported from the GoW recomp); the one-instruction delayed VI
        // only applies to conditional-branch comparisons. PS2X_VU1_JR_DELAYED_VI=1 restores the old behaviour.
        uint32_t target = ((uint32_t)(uint16_t)(vu1JrUsesDelayedVi() ? readBranchVi(is) : m_state.vi[is]) * 8u) & pcMask;
        m_state.branchPending = true;
        m_state.branchTarget = target;
        m_state.branchDelay = 1;
        return;
    }
    case 0x25: // JALR
    {
        uint8_t it = VIT(instr);
        uint8_t is = VIS(instr);
        // JR/JALR take the current VI (PCSX2 microVU, ported from the GoW recomp); the one-instruction delayed VI
        // only applies to conditional-branch comparisons. PS2X_VU1_JR_DELAYED_VI=1 restores the old behaviour.
        uint32_t target = ((uint32_t)(uint16_t)(vu1JrUsesDelayedVi() ? readBranchVi(is) : m_state.vi[is]) * 8u) & pcMask;
        if (it != 0)
            m_state.vi[it] = (int32_t)((m_state.pc + 16) / 8);
        m_state.branchPending = true;
        m_state.branchTarget = target;
        m_state.branchDelay = 1;
        return;
    }
    case 0x28: // IBEQ
    {
        uint8_t it = VIT(instr);
        uint8_t is = VIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)readBranchVi(is) == (int16_t)readBranchVi(it))
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & pcMask;
            m_state.branchPending = true;
            m_state.branchTarget = target;
            m_state.branchDelay = 1;
        }
        return;
    }
    case 0x29: // IBNE
    {
        uint8_t it = VIT(instr);
        uint8_t is = VIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)readBranchVi(is) != (int16_t)readBranchVi(it))
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & pcMask;
            m_state.branchPending = true;
            m_state.branchTarget = target;
            m_state.branchDelay = 1;
        }
        return;
    }
    case 0x2C: // IBLTZ
    {
        uint8_t is = VIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)readBranchVi(is) < 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & pcMask;
            m_state.branchPending = true;
            m_state.branchTarget = target;
            m_state.branchDelay = 1;
        }
        return;
    }
    case 0x2D: // IBGTZ
    {
        uint8_t is = VIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)readBranchVi(is) > 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & pcMask;
            m_state.branchPending = true;
            m_state.branchTarget = target;
            m_state.branchDelay = 1;
        }
        return;
    }
    case 0x2E: // IBLEZ
    {
        uint8_t is = VIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)readBranchVi(is) <= 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & pcMask;
            m_state.branchPending = true;
            m_state.branchTarget = target;
            m_state.branchDelay = 1;
        }
        return;
    }
    case 0x2F: // IBGEZ
    {
        uint8_t is = VIS(instr);
        int16_t imm = IMM11(instr);
        if ((int16_t)readBranchVi(is) >= 0)
        {
            uint32_t target = (m_state.pc + 8 + imm * 8) & pcMask;
            m_state.branchPending = true;
            m_state.branchTarget = target;
            m_state.branchDelay = 1;
        }
        return;
    }

    case 0x40: // Lower1 / lower special. Bit31 set; low 6 bits select integer or special op.
    {
        const uint8_t funct = instr & 0x3Fu;
        const uint8_t vfT = FT(instr);
        const uint8_t vfS = FS(instr);
        const uint8_t viT = VIT(instr);
        const uint8_t viS = VIS(instr);
        const uint8_t viD = VID(instr);
        const uint8_t dest = (instr >> 21) & 0xF;

        switch (funct)
        {
        case 0x30: // IADD
            if (viD != 0)
                m_state.vi[viD] = (int16_t)(m_state.vi[viS] + m_state.vi[viT]);
            return;
        case 0x31: // ISUB
            if (viD != 0)
                m_state.vi[viD] = (int16_t)(m_state.vi[viS] - m_state.vi[viT]);
            return;
        case 0x32: // IADDI
        {
            int16_t imm5 = (int16_t)((int32_t)((instr >> 6) & 0x1F) << 27 >> 27);
            if (viT != 0)
                m_state.vi[viT] = (int16_t)(m_state.vi[viS] + imm5);
            return;
        }
        case 0x34: // IAND
            if (viD != 0)
                m_state.vi[viD] = m_state.vi[viS] & m_state.vi[viT];
            return;
        case 0x35: // IOR
            if (viD != 0)
                m_state.vi[viD] = m_state.vi[viS] | m_state.vi[viT];
            return;

        case 0x3C:
        case 0x3D:
        case 0x3E:
        case 0x3F: // Lower1 special. Dobie decodes this as (instr & 3) | ((instr >> 4) & 0x7C).
        {
            const uint8_t funct2 = (uint8_t)((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
            switch (funct2)
            {
            case 0x30: // MOVE
            {
                float tmp[4];
                std::memcpy(tmp, m_state.vf[vfS], 16);
                applyDest(m_state.vf[vfT], tmp, dest);
                return;
            }
            case 0x31: // MR32 (rotate right by 32 bits = shift xyzw -> yzwx)
            {
                float tmp[4] = {m_state.vf[vfS][1], m_state.vf[vfS][2], m_state.vf[vfS][3], m_state.vf[vfS][0]};
                applyDest(m_state.vf[vfT], tmp, dest);
                return;
            }
            case 0x34: // LQI (Load Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viS]) * 16u;
                noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDest(m_state.vf[vfT], tmp, dest);
                }
                if (viS != 0)
                    m_state.vi[viS] = (int16_t)(m_state.vi[viS] + 1);
                return;
            }
            case 0x35: // SQI (Store Quadword, post-increment)
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viT]) * 16u;
                noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    uint32_t words[4]{};
                    std::memcpy(words, m_state.vf[vfS], sizeof(words));
                    queueStore(addr, words, dest);
                }
                if (viT != 0)
                    m_state.vi[viT] = (int16_t)(m_state.vi[viT] + 1);
                return;
            }
            case 0x36: // LQD (Load Quadword, pre-decrement)
            {
                if (viS != 0)
                    m_state.vi[viS] = (int16_t)(m_state.vi[viS] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viS]) * 16u;
                noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    float tmp[4];
                    std::memcpy(tmp, vuData + addr, 16);
                    applyDest(m_state.vf[vfT], tmp, dest);
                }
                return;
            }
            case 0x37: // SQD (Store Quadword, pre-decrement)
            {
                if (viT != 0)
                    m_state.vi[viT] = (int16_t)(m_state.vi[viT] - 1);
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viT]) * 16u;
                noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    uint32_t words[4]{};
                    std::memcpy(words, m_state.vf[vfS], sizeof(words));
                    queueStore(addr, words, dest);
                }
                return;
            }
            case 0x38: // DIV
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                const float num = normalizeOperand(m_state.vf[vfS][fsf]);
                const float den = normalizeOperand(m_state.vf[vfT][ftf]);
                uint32_t statusDi = 0u;
                float result = 0.0f;
                if (den == 0.0f)
                {
                    statusDi = num == 0.0f ? 0x10u : 0x20u;
                    result = std::signbit(num) != std::signbit(den)
                                 ? -std::numeric_limits<float>::max()
                                 : std::numeric_limits<float>::max();
                }
                else
                {
                    result = num / den;
                }
                uint32_t ignoredFlags = 0u;
                result = normalizeResult(result, ignoredFlags);
                queueQ(result, 7u, statusDi);
                return;
            }
            case 0x39: // SQRT
            {
                int ftf = (instr >> 23) & 0x3;
                const float val = normalizeOperand(m_state.vf[vfT][ftf]);
                queueQ(std::sqrt(std::fabs(val)), 7u,
                       val < 0.0f ? 0x10u : 0u);
                return;
            }
            case 0x3A: // RSQRT
            {
                int fsf = (instr >> 21) & 0x3;
                int ftf = (instr >> 23) & 0x3;
                const float num = normalizeOperand(m_state.vf[vfS][fsf]);
                const float radicand = normalizeOperand(m_state.vf[vfT][ftf]);
                const float den = std::sqrt(std::fabs(radicand));
                uint32_t statusDi = radicand < 0.0f ? 0x10u : 0u;
                float result = 0.0f;
                if (den != 0.0f)
                    result = num / den;
                else
                {
                    statusDi = num == 0.0f ? 0x10u : 0x20u;
                    result = std::signbit(num)
                                 ? -std::numeric_limits<float>::max()
                                 : std::numeric_limits<float>::max();
                }
                uint32_t ignoredFlags = 0u;
                result = normalizeResult(result, ignoredFlags);
                queueQ(result, 13u, statusDi);
                return;
            }
            case 0x3B: // WAITQ
                return;
            case 0x3C: // MTIR (Move To Integer Register)
            {
                // MTIR encodes a two-bit fsf component selector in bits
                // 22:21. It is not a four-bit destination mask.
                const uint32_t comp = (instr >> 21) & 0x3u;
                uint32_t fval;
                std::memcpy(&fval, &m_state.vf[vfS][comp], 4);
                if (viT != 0)
                    m_state.vi[viT] = (int32_t)(int16_t)(fval & 0xFFFF);
                return;
            }
            case 0x3D: // MFIR (Move From Integer Register)
            {
                float result[4];
                int32_t val = (int32_t)(int16_t)(m_state.vi[viS] & 0xFFFF);
                std::memcpy(&result[0], &val, 4);
                result[1] = result[0];
                result[2] = result[0];
                result[3] = result[0];
                applyDest(m_state.vf[vfT], result, dest);
                return;
            }
            case 0x3E: // ILWR - integer load word from address in VI[is]
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viS]) * 16u;
                noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    int comp = 0;
                    if (dest & 0x8)
                        comp = 0;
                    else if (dest & 0x4)
                        comp = 1;
                    else if (dest & 0x2)
                        comp = 2;
                    else
                        comp = 3;
                    uint32_t v;
                    std::memcpy(&v, vuData + addr + comp * 4, 4);
                    if (viT != 0)
                        m_state.vi[viT] = (int32_t)(int16_t)(v & 0xFFFF);
                }
                return;
            }
            case 0x3F: // ISWR - integer store word to address in VI[is]
            {
                uint32_t addr = ((uint32_t)(uint16_t)m_state.vi[viS]) * 16u;
                noteVu0DataAddress(m_unit == Unit::VU0, addr / 16u);
                addr &= (dataSize - 1);
                if (addr + 16 <= dataSize)
                {
                    const uint32_t val =
                        static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[viT] & 0xFFFF));
                    const uint32_t words[4] = {val, val, val, val};
                    queueStore(addr, words, dest);
                }
                return;
            }
            case 0x40: // RNEXT
            {
                const uint32_t x = (m_state.r >> 4) & 1u;
                const uint32_t y = (m_state.r >> 22) & 1u;
                m_state.r = ((m_state.r << 1) ^ x ^ y) & 0x007FFFFFu;
                m_state.r |= 0x3F800000u;
                float value = 0.0f;
                std::memcpy(&value, &m_state.r, sizeof(value));
                const float result[4] = {value, value, value, value};
                applyDest(m_state.vf[vfT], result, dest);
                return;
            }
            case 0x41: // RGET
            {
                float value = 0.0f;
                std::memcpy(&value, &m_state.r, sizeof(value));
                const float result[4] = {value, value, value, value};
                applyDest(m_state.vf[vfT], result, dest);
                return;
            }
            case 0x42: // RINIT
            {
                const uint32_t component = (instr >> 21) & 3u;
                uint32_t bits = 0u;
                std::memcpy(&bits, &m_state.vf[vfS][component], sizeof(bits));
                m_state.r = 0x3F800000u | (bits & 0x007FFFFFu);
                return;
            }
            case 0x43: // RXOR
            {
                const uint32_t component = (instr >> 21) & 3u;
                uint32_t bits = 0u;
                std::memcpy(&bits, &m_state.vf[vfS][component], sizeof(bits));
                m_state.r = 0x3F800000u | ((m_state.r ^ bits) & 0x007FFFFFu);
                return;
            }
            case 0x64: // MFP (Move From P register)
            {
                float result[4] = {m_state.p, m_state.p, m_state.p, m_state.p};
                applyDest(m_state.vf[vfT], result, dest);
                return;
            }
            case 0x68: // XTOP - move current VIF1 TOP into VI register
            {
                if (viT != 0)
                    m_state.vi[viT] = (int32_t)(m_state.top & 0x3FFu);
                return;
            }
            case 0x69: // XITOP - move current VIF1 ITOP into VI register
            {
                if (viT != 0)
                    m_state.vi[viT] = (int32_t)(m_state.itop & 0x3FFu);
                return;
            }
            case 0x6C: // XGKICK - send GIF packet from VU1 data memory
                startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[viS])), m_state.pc);
                return;
            case 0x70: // ESADD
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                queueP(x * x + y * y + z * z, 11u);
                return;
            }
            case 0x71: // ERSADD
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                const float sum = x * x + y * y + z * z;
                queueP(sum != 0.0f ? 1.0f / sum : sum, 18u);
                return;
            }
            case 0x72: // ELENG
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                queueP(std::sqrt(x * x + y * y + z * z), 18u);
                return;
            }
            case 0x73: // ERLENG
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                const float len = std::sqrt(x * x + y * y + z * z);
                queueP(len != 0.0f ? 1.0f / len : len, 24u);
                return;
            }
            case 0x74: // EATANxy
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float y = normalizeOperand(m_state.vf[vfS][1]);
                queueP(x != 0.0f ? vuEatan(y / x) : 0.0f, 54u);
                return;
            }
            case 0x75: // EATANxz
            {
                const float x = normalizeOperand(m_state.vf[vfS][0]);
                const float z = normalizeOperand(m_state.vf[vfS][2]);
                queueP(x != 0.0f ? vuEatan(z / x) : 0.0f, 54u);
                return;
            }
            case 0x76: // ESUM
            {
                float sum = 0.0f;
                for (uint32_t component = 0; component < 4u; ++component)
                    sum += normalizeOperand(m_state.vf[vfS][component]);
                queueP(sum, 12u);
                return;
            }
            case 0x77: // ERSQRT
            {
                const uint32_t component = (instr >> 21) & 3u;
                const float value = normalizeOperand(m_state.vf[vfS][component]);
                float result = value;
                if (result >= 0.0f)
                {
                    result = std::sqrt(result);
                    if (result != 0.0f)
                        result = 1.0f / result;
                }
                queueP(result, 18u);
                return;
            }
            case 0x78: // ESQRT
            {
                const uint32_t component = (instr >> 21) & 3u;
                const float value = normalizeOperand(m_state.vf[vfS][component]);
                queueP(value >= 0.0f ? std::sqrt(value) : value, 12u);
                return;
            }
            case 0x79: // ESIN
            {
                const uint32_t component = (instr >> 21) & 3u;
                const float value = normalizeOperand(m_state.vf[vfS][component]);
                queueP(vuEsin(value), 29u);
                return;
            }
            case 0x7A: // ERCPR
            {
                const uint32_t component = (instr >> 21) & 3u;
                const float value = normalizeOperand(m_state.vf[vfS][component]);
                queueP(value != 0.0f ? 1.0f / value : value, 12u);
                return;
            }
            case 0x7B: // WAITP
                return;
            case 0x7C: // EATAN
            {
                const uint32_t component = (instr >> 21) & 3u;
                queueP(vuEatan(normalizeOperand(m_state.vf[vfS][component])), 54u);
                return;
            }
            case 0x7D: // EEXP
            {
                const uint32_t component = (instr >> 21) & 3u;
                queueP(vuEexp(normalizeOperand(m_state.vf[vfS][component])), 44u);
                return;
            }
            default:
                reportReservedInstruction(false, instr);
                return;
            }
        }
        default:
            reportReservedInstruction(false, instr);
            return;
        }
    }
    default:
        reportReservedInstruction(false, instr);
        break;
    }
}

struct Vu1RunArgs
{
    uint8_t *vuData = nullptr;
    uint32_t dataSize = 0;
    GS *gs = nullptr;
    PS2Memory *memory = nullptr;
    uint32_t codeSize = 0;
    uint64_t budgetEnd = 0;
};

struct Vu1FlagRecord
{
    static constexpr uint32_t kMac = 1u;
    static constexpr uint32_t kStatus = 2u;
    static constexpr uint32_t kSticky = 4u;
    static constexpr uint32_t kClip = 8u;

    uint64_t ready;
    uint32_t mac;
    uint32_t status;
    uint32_t extra;
    uint32_t clip;
    uint32_t writes;
};

struct Vu1FastCtx
{
    static constexpr uint32_t kFlagCapacity = 32u;
    static constexpr uint32_t kFlagMask = kFlagCapacity - 1u;
    static constexpr uint32_t kClipCapacity = 16u;
    static constexpr uint32_t kClipMask = kClipCapacity - 1u;

    uint64_t cycle = 0;
    uint64_t budget = 0;
    uint64_t xgkickDone = 0;
    uint8_t *mem = nullptr;
    GS *gs = nullptr;
    PS2Memory *memory = nullptr;
    uint32_t flagHead = 0;
    uint32_t flagTail = 0;
    uint32_t clipHead = 0;
    uint32_t clipTail = 0;
    bool stickyPending = false;
    Vu1FlagRecord flags[kFlagCapacity];
    Vu1FlagRecord clips[kClipCapacity];
    float (*norm)[4] = nullptr;
};

struct Vu1NormShadow
{
    const void *owner = nullptr;
    bool valid = false;
    alignas(16) float norm[33][4];
};

inline Vu1NormShadow vu1NormShadow;
// VU0 micro mode runs compiled code too (game thread), with its own shadow: its registers are reloaded from the EE
// context on every call, so it must never share or reuse VU1's.
inline Vu1NormShadow vu0NormShadow;
// PS2X_VU0_LEAN: true while VU0's interpreter-only scheduler arrays (ready cycles, latest writes, VF/VI/ACC write
// pipelines) are still all zero since the last full resetScheduler(). Only the interpreter touches them, so a VU0 call
// that ran entirely in compiled code leaves them clean and the next call can skip clearing them (~3.5 KB per VCALLMS).
inline bool vu0SchedulerClean = false;
// Cycles the lean VU0 call skips ahead between calls instead of clearing the ready-cycle arrays: the compiled code
// keeps m_vfReady/m_viReady/m_accReady (stalls between blocks) and never sets one more than a few cycles past the cycle
// it ends on (VF/VI latency <= 4), so after the skip every old entry is already "ready", as if cleared.
inline constexpr uint64_t kVu0LeanCycleGap = 64u;

struct VU1InterpreterAccess
{
    // VU1Interpreter::reset() without its resetScheduler(): for callers that run execute() right after, which resets
    // the scheduler itself (VU0 micro mode did both on every VCALLMS, two rounds of clearing the pipelines).
    static void resetStateOnly(VU1Interpreter &vu)
    {
        std::memset(&vu.m_state, 0, sizeof(vu.m_state));
        vu.m_state.vf[0][3] = 1.0f;
        vu.m_state.q = 1.0f;
        vu.m_state.r = 0x3F800000u;
        vu.m_cycle = 0;
        if (vu.m_unit == VU1Interpreter::Unit::VU1)
            vu1NormShadow.valid = false;
        else
            vu0NormShadow.valid = false;
    }

    // PS2X_VU0_LEAN, VU0 micro mode (VCALLMS): load the EE's VF registers and ACC into the VU0 state without throwing
    // the normalized shadow away. While the shadow is valid it equals vnorm() of the raw registers (compiled code writes
    // both; any interpreted pair invalidates it), so only the registers the EE changed since the last call are copied
    // and renormalized, instead of all 33 (syncAllNorm) on every call. The caller sets the scalar state.
    // PS2X_VU0_LEAN=check, read once into a plain static (these run ~4500 times per frame on the EE thread; a
    // function-local static const costs a thread-safe initialization check on every call).
    static bool vu0LeanCheck()
    {
        static int mode = -1;
        if (mode < 0)
        {
            const char *value = std::getenv("PS2X_VU0_LEAN");
            mode = (value && std::strcmp(value, "check") == 0) ? 1 : 0;
        }
        return mode == 1;
    }

    static void loadVu0Registers(VU1Interpreter &vu, const __m128 *vf, __m128 acc)
    {
        const bool keep = vu0NormShadow.valid && vu0NormShadow.owner == &vu;
        for (uint32_t reg = 1u; reg < 33u; ++reg)
        {
            const __m128 raw = reg < 32u ? vf[reg] : acc;
            float *target = rawReg(vu, reg);
            const __m128i same = _mm_cmpeq_epi32(_mm_castps_si128(raw), _mm_castps_si128(_mm_loadu_ps(target)));
            if (keep && _mm_movemask_epi8(same) == 0xFFFF)
                continue;
            _mm_storeu_ps(target, raw);
            if (keep)
                _mm_store_ps(vu0NormShadow.norm[reg], vnorm(raw));
        }
        vu.m_state.vf[0][0] = 0.0f;
        vu.m_state.vf[0][1] = 0.0f;
        vu.m_state.vf[0][2] = 0.0f;
        vu.m_state.vf[0][3] = 1.0f;
        if (keep)
            _mm_store_ps(vu0NormShadow.norm[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
        // PS2X_VU0_LEAN=check: compare the kept shadow with a full renormalization on every call (the VU0 verify mode
        // cannot cover this: its interpreter run invalidates the shadow every time).
        const bool check = vu0LeanCheck();
        if (check && keep)
        {
            static uint64_t calls = 0u, stale = 0u;
            ++calls;
            for (uint32_t reg = 0u; reg < 33u; ++reg)
            {
                const __m128i same = _mm_cmpeq_epi32(_mm_castps_si128(vnorm(_mm_loadu_ps(rawReg(vu, reg)))),
                                                     _mm_castps_si128(_mm_load_ps(vu0NormShadow.norm[reg])));
                if (_mm_movemask_epi8(same) != 0xFFFF)
                {
                    if (++stale <= 20u)
                        std::fprintf(stderr, "[vu0-lean] stale normalized register %u at call %llu\n", reg,
                                     static_cast<unsigned long long>(calls));
                }
            }
            if ((calls & 0xFFFFu) == 0u)
                std::fprintf(stderr, "[vu0-lean] check: %llu calls with a kept shadow, %llu stale registers\n",
                             static_cast<unsigned long long>(calls), static_cast<unsigned long long>(stale));
        }
    }

    // PS2X_VU0_LEAN: VU1Interpreter::execute() for VU0 micro mode, clearing the interpreter-only scheduler arrays only
    // when the interpreter touched them since they were last cleared (vu0SchedulerClean).
    static void executeVu0Lean(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                               GS &gs, PS2Memory *memory, uint32_t startPC, uint32_t itop, uint32_t maxCycles)
    {
        if (vu0SchedulerClean && (vu.m_vfWriteValid | vu.m_viWriteValid | vu.m_accWriteValid | vu.m_storeValid) == 0u)
        {
            vu.m_cycle += kVu0LeanCycleGap;
            const bool check = vu0LeanCheck();
            if (check)
            {
                // Every ready cycle must already be reached, and the interpreter-only arrays must still be clear.
                uint64_t latest = 0u;
                for (const auto &lanes : vu.m_vfReady)
                    for (const uint64_t ready : lanes)
                        latest = std::max(latest, ready);
                for (const uint64_t ready : vu.m_viReady)
                    latest = std::max(latest, ready);
                for (const uint64_t ready : vu.m_accReady)
                    latest = std::max(latest, ready);
                const auto nonzero = [](const void *data, size_t size)
                {
                    const auto *bytes = static_cast<const uint8_t *>(data);
                    return std::any_of(bytes, bytes + size, [](uint8_t b) { return b != 0u; });
                };
                static uint64_t calls = 0u, reports = 0u;
                const char *dirty = latest > vu.m_cycle                                                 ? "ready cycle ahead"
                                    : nonzero(vu.m_vfLatestWrite.data(), sizeof(vu.m_vfLatestWrite))   ? "vfLatestWrite"
                                    : nonzero(vu.m_viLatestWrite.data(), sizeof(vu.m_viLatestWrite))   ? "viLatestWrite"
                                    : nonzero(vu.m_accLatestWrite.data(), sizeof(vu.m_accLatestWrite)) ? "accLatestWrite"
                                    : vu.m_nextWriteSequence != 0u                                      ? "nextWriteSequence"
                                                                                                        : nullptr;
                if (dirty)
                    ++reports;
                if (dirty && reports <= 20u)
                    std::fprintf(stderr, "[vu0-lean] scheduler not clean on a skipped clear: %s (pc %04x)\n", dirty, startPC);
                if ((++calls & 0xFFFFu) == 0u)
                    std::fprintf(stderr, "[vu0-lean] scheduler check: %llu skipped clears, %llu not clean\n",
                                 static_cast<unsigned long long>(calls), static_cast<unsigned long long>(reports));
            }
            // Only the valid flags are read for these (the entries of m_flagPipeline through m_flagValid), so clearing
            // them is enough; the three memsets were ~0.8% of the game thread at ~4500 calls per frame (PROGRESS §70).
            vu.m_fdiv.valid = false;
            for (auto &entry : vu.m_efu)
                entry.valid = false;
            vu.m_flagValid = 0u;
            vu.m_nextCommitCycle = std::numeric_limits<uint64_t>::max();
            vu.resetXgkick();
            vu.m_nextWriteSequence = 0;
            vu.m_efuResourceReady = 0;
            vu.m_workingClip = vu.m_state.clip;
            vu.m_viBranchBackupValue = 0;
            vu.m_viBranchBackupReg = 0;
            vu.m_viBranchBackupValid = false;
            vu.m_stopRequested = false;
            vu.m_pendingHaltD = false;
            vu.m_pendingHaltT = false;
        }
        else
        {
            vu.resetScheduler();
            vu0SchedulerClean = true;
            vu.m_cycle = 0;
        }
        vu.m_state.pc = startPC & vu.microAddressMask();
        vu.m_state.ebit = false;
        vu.m_state.haltAfterDelaySlot = false;
        vu.m_state.stoppedByD = false;
        vu.m_state.stoppedByT = false;
        vu.m_state.top = 0u;
        vu.m_state.itop = itop;
        vu.m_state.branchPending = false;
        vu.m_state.branchTarget = 0;
        vu.m_state.branchDelay = 0;
        vu.run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    }

    template <typename Ar>
    static void serializeState(VU1Interpreter &vu, Ar &ar)
    {
        ps2x::state::fields(ar, vu.m_state, vu.m_flagPipeline, vu.m_fdiv, vu.m_efu, vu.m_storePipeline, vu.m_vfWritePipeline,
                            vu.m_viWritePipeline, vu.m_accWritePipeline, vu.m_flagValid, vu.m_storeValid, vu.m_vfWriteValid,
                            vu.m_viWriteValid, vu.m_accWriteValid, vu.m_nextCommitCycle, vu.m_xgkick, vu.m_vfReady, vu.m_viReady,
                            vu.m_accReady, vu.m_vfLatestWrite, vu.m_viLatestWrite, vu.m_accLatestWrite, vu.m_cycle,
                            vu.m_nextWriteSequence, vu.m_efuResourceReady, vu.m_workingClip, vu.m_currentUpperInstruction,
                            vu.m_viBranchBackupValue, vu.m_viBranchBackupReg, vu.m_viBranchBackupValid, vu.m_stopRequested,
                            vu.m_pendingHaltD, vu.m_pendingHaltT);
        if constexpr (Ar::kLoading)
        {
            if (vu.m_unit != VU1Interpreter::Unit::VU1)
            {
                vu0NormShadow.valid = false;
                vu0SchedulerClean = false;
            }
            vu.m_decodedCodeCacheValid = false;
            vu.m_cachedVuCode = nullptr;
        }
    }

    enum class StepResult : uint8_t
    {
        Continue,
        Ended,
        Budget,
        Fallback,
    };

    enum class FK : uint8_t
    {
        Add,
        Sub,
        Mul,
        Madd,
        Msub,
        Opmsub,
        Opmula,
    };

    using CompiledProgram = StepResult (*)(VU1Interpreter &vu, const Vu1RunArgs &args);
    using FastBlock = StepResult (*)(VU1Interpreter &vu, Vu1FastCtx &ctx);
    using Decoded = VU1Interpreter::DecodedInstructionPair;

    static constexpr unsigned kPipeFmac = VU1Interpreter::PipelineFmac;
    static constexpr unsigned kPipeLsu = VU1Interpreter::PipelineLsu;
    static constexpr unsigned kPipeFdiv = VU1Interpreter::PipelineFdiv;
    static constexpr unsigned kPipeEfu = VU1Interpreter::PipelineEfu;
    static constexpr unsigned kPipeIalu = VU1Interpreter::PipelineIalu;
    static constexpr unsigned kPipeBranch = VU1Interpreter::PipelineBranch;
    static constexpr unsigned kPipeXgkick = VU1Interpreter::PipelineXgkick;
    static constexpr uint32_t kFastMemSize = 0x4000u;
    static constexpr uint32_t kFastCodeSize = 0x4000u;

    template <uint64_t ImageHash>
    static StepResult runImage(VU1Interpreter &vu, const Vu1RunArgs &args);

    template <uint64_t ImageHash, uint32_t Block>
    static StepResult fastBlock(VU1Interpreter &vu, Vu1FastCtx &ctx);

    static Decoded decodePair(const VU1Interpreter &vu, const uint8_t *vuCode, uint32_t pc)
    {
        return vu.decodeInstructionPair(vuCode, pc);
    }

    static unsigned pipeline(const VU1Interpreter::InstructionUsage &usage)
    {
        return static_cast<unsigned>(usage.pipeline);
    }

    static uint32_t pc(const VU1Interpreter &vu)
    {
        return vu.m_state.pc;
    }

    static bool runLoop(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                        GS &gs, PS2Memory *memory, uint32_t maxCycles, bool allowCompiled);
    static bool runLoopWith(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory, uint32_t maxCycles, CompiledProgram compiled);
    static void runVerified(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory, uint32_t maxCycles);
    static bool runVerifiedWith(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                                GS &gs, PS2Memory *memory, uint32_t maxCycles, CompiledProgram compiled, bool forward);
    static VU1State effectiveState(const VU1Interpreter &vu);

    struct TraceState
    {
        VU1State state{};
        uint64_t cycle = 0;
        std::array<std::array<uint64_t, 4>, 32> vfReady{};
        std::array<uint64_t, 16> viReady{};
        std::array<uint64_t, 4> accReady{};
        std::array<VU1Interpreter::FlagPipelineEntry, VU1Interpreter::kMaxFlagEntries> flagPipeline{};
        uint32_t flagValid = 0;
        VU1Interpreter::ScalarPipelineEntry fdiv{};
        std::array<VU1Interpreter::ScalarPipelineEntry, 2> efu{};
        uint64_t efuResourceReady = 0;
        uint64_t nextCommitCycle = 0;
        uint32_t workingClip = 0;
        int32_t viBranchBackupValue = 0;
        uint8_t viBranchBackupReg = 0;
        bool viBranchBackupValid = false;
    };

    static bool traceCapture(const VU1Interpreter &vu, TraceState &out)
    {
        if ((vu.m_vfWriteValid | vu.m_viWriteValid | vu.m_accWriteValid | vu.m_storeValid) != 0u || vu.m_xgkick.active)
            return false;
        out.state = vu.m_state;
        out.cycle = vu.m_cycle;
        out.vfReady = vu.m_vfReady;
        out.viReady = vu.m_viReady;
        out.accReady = vu.m_accReady;
        out.flagPipeline = vu.m_flagPipeline;
        out.flagValid = vu.m_flagValid;
        out.fdiv = vu.m_fdiv;
        out.efu = vu.m_efu;
        out.efuResourceReady = vu.m_efuResourceReady;
        out.nextCommitCycle = vu.m_nextCommitCycle;
        out.workingClip = vu.m_workingClip;
        out.viBranchBackupValue = vu.m_viBranchBackupValue;
        out.viBranchBackupReg = vu.m_viBranchBackupReg;
        out.viBranchBackupValid = vu.m_viBranchBackupValid;
        return true;
    }

    static void traceRestore(VU1Interpreter &vu, const TraceState &in)
    {
        vu.resetScheduler();
        vu.m_state = in.state;
        vu.m_cycle = in.cycle;
        vu.m_vfReady = in.vfReady;
        vu.m_viReady = in.viReady;
        vu.m_accReady = in.accReady;
        vu.m_flagPipeline = in.flagPipeline;
        vu.m_flagValid = in.flagValid;
        vu.m_fdiv = in.fdiv;
        vu.m_efu = in.efu;
        vu.m_efuResourceReady = in.efuResourceReady;
        vu.m_nextCommitCycle = in.nextCommitCycle;
        vu.m_workingClip = in.workingClip;
        vu.m_viBranchBackupValue = in.viBranchBackupValue;
        vu.m_viBranchBackupReg = in.viBranchBackupReg;
        vu.m_viBranchBackupValid = in.viBranchBackupValid;
    }

    alignas(16) static constexpr uint32_t kDestMaskBits[16][4] = {
        {0u, 0u, 0u, 0u},
        {0u, 0u, 0u, ~0u},
        {0u, 0u, ~0u, 0u},
        {0u, 0u, ~0u, ~0u},
        {0u, ~0u, 0u, 0u},
        {0u, ~0u, 0u, ~0u},
        {0u, ~0u, ~0u, 0u},
        {0u, ~0u, ~0u, ~0u},
        {~0u, 0u, 0u, 0u},
        {~0u, 0u, 0u, ~0u},
        {~0u, 0u, ~0u, 0u},
        {~0u, 0u, ~0u, ~0u},
        {~0u, ~0u, 0u, 0u},
        {~0u, ~0u, 0u, ~0u},
        {~0u, ~0u, ~0u, 0u},
        {~0u, ~0u, ~0u, ~0u},
    };

    static constexpr uint8_t kRev4[16] = {0x0u, 0x8u, 0x4u, 0xCu, 0x2u, 0xAu, 0x6u, 0xEu,
                                          0x1u, 0x9u, 0x5u, 0xDu, 0x3u, 0xBu, 0x7u, 0xFu};

    static VU1_FORCE_INLINE uint64_t rmax(uint64_t a, uint64_t b)
    {
        return a < b ? b : a;
    }

    static VU1_FORCE_INLINE __m128 destMask(uint32_t dest)
    {
        return _mm_load_ps(reinterpret_cast<const float *>(kDestMaskBits[dest]));
    }

    static VU1_FORCE_INLINE __m128 blendDest(__m128 old, __m128 value, uint32_t dest)
    {
        if (dest == 0xFu)
            return value;
        const __m128 mask = destMask(dest);
        return _mm_or_ps(_mm_and_ps(mask, value), _mm_andnot_ps(mask, old));
    }

    static VU1_FORCE_INLINE void storeDest(float *target, __m128 value, uint32_t dest)
    {
        if (dest == 0xFu)
            _mm_storeu_ps(target, value);
        else
            _mm_storeu_ps(target, blendDest(_mm_loadu_ps(target), value, dest));
    }

    static VU1_FORCE_INLINE __m128 vnorm(__m128 value)
    {
        const __m128i bits = _mm_castps_si128(value);
        const __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i zero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
        const __m128i huge = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000));
        __m128i result = _mm_or_si128(_mm_andnot_si128(zero, bits), _mm_and_si128(zero, sign));
        result = _mm_or_si128(_mm_andnot_si128(huge, result),
                              _mm_and_si128(huge, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF))));
        return _mm_castsi128_ps(result);
    }

    static VU1_FORCE_INLINE __m128 vfN(VU1Interpreter &vu, uint32_t reg)
    {
        return vnorm(_mm_loadu_ps(vu.m_state.vf[reg]));
    }

    static VU1_FORCE_INLINE __m128 vfBc(VU1Interpreter &vu, uint32_t reg, uint32_t bc)
    {
        return vnorm(_mm_set1_ps(vu.m_state.vf[reg][bc]));
    }

    static VU1_FORCE_INLINE __m128 accN(VU1Interpreter &vu)
    {
        return vnorm(_mm_loadu_ps(vu.m_state.acc));
    }

    static VU1_FORCE_INLINE __m128 nv(Vu1FastCtx &ctx, uint32_t reg)
    {
        return _mm_load_ps(ctx.norm[reg]);
    }

    static VU1_FORCE_INLINE __m128 nbc(Vu1FastCtx &ctx, uint32_t reg, uint32_t bc)
    {
        return _mm_set1_ps(ctx.norm[reg][bc]);
    }

    static VU1_FORCE_INLINE __m128 nacc(Vu1FastCtx &ctx)
    {
        return _mm_load_ps(ctx.norm[32]);
    }

    static VU1_FORCE_INLINE float *rawReg(VU1Interpreter &vu, uint32_t reg)
    {
        return reg < 32u ? vu.m_state.vf[reg] : vu.m_state.acc;
    }

    static VU1_FORCE_INLINE void writeVf(VU1Interpreter &vu, Vu1FastCtx &ctx, uint32_t reg, __m128 raw, __m128 normalized, uint32_t dest)
    {
        storeDest(rawReg(vu, reg), raw, dest);
        if (dest == 0xFu)
            _mm_store_ps(ctx.norm[reg], normalized);
        else
            _mm_store_ps(ctx.norm[reg], blendDest(_mm_load_ps(ctx.norm[reg]), normalized, dest));
    }

    static VU1_FORCE_INLINE void writeVfFull(VU1Interpreter &vu, Vu1FastCtx &ctx, uint32_t reg, __m128 raw, __m128 normalized)
    {
        _mm_storeu_ps(rawReg(vu, reg), raw);
        _mm_store_ps(ctx.norm[reg], normalized);
    }

    static VU1_FORCE_INLINE void resetVf0(VU1Interpreter &vu, Vu1FastCtx &ctx)
    {
        vu.m_state.vf[0][0] = 0.0f;
        vu.m_state.vf[0][1] = 0.0f;
        vu.m_state.vf[0][2] = 0.0f;
        vu.m_state.vf[0][3] = 1.0f;
        _mm_store_ps(ctx.norm[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
    }

    static void syncAllNorm(VU1Interpreter &vu, Vu1FastCtx &ctx)
    {
        for (uint32_t reg = 0; reg < 33u; ++reg)
            _mm_store_ps(ctx.norm[reg], vnorm(_mm_loadu_ps(rawReg(vu, reg))));
    }

    static VU1_FORCE_INLINE __m128 qN(VU1Interpreter &vu)
    {
        return vnorm(_mm_set1_ps(vu.m_state.q));
    }

    static VU1_FORCE_INLINE __m128 iN(VU1Interpreter &vu)
    {
        return vnorm(_mm_set1_ps(vu.m_state.i));
    }

    static VU1_FORCE_INLINE uint32_t maskBits(__m128d lo, __m128d hi)
    {
        return kRev4[static_cast<uint32_t>(_mm_movemask_pd(lo)) | (static_cast<uint32_t>(_mm_movemask_pd(hi)) << 2)];
    }

    static VU1_FORCE_INLINE void foldFlag(VU1Interpreter &vu, const Vu1FlagRecord &record)
    {
        if (record.writes & Vu1FlagRecord::kMac)
            vu.m_state.mac = record.mac;
        if (record.writes & Vu1FlagRecord::kStatus)
        {
            const uint32_t current = record.status & 0xFu;
            vu.m_state.status = (vu.m_state.status & 0xFF0u) | current | ((current | record.extra) << 6);
        }
        if (record.writes & Vu1FlagRecord::kSticky)
            vu.m_state.status = (vu.m_state.status & 0x03Fu) | (record.status & 0xFC0u);
        if (record.writes & Vu1FlagRecord::kClip)
            vu.m_state.clip = record.clip;
    }

    static VU1_FORCE_INLINE void commitDivResult(VU1Interpreter &vu)
    {
        vu.m_state.q = vu.m_fdiv.value;
        const uint32_t currentDi = vu.m_fdiv.statusDi & 0x30u;
        vu.m_state.status = (vu.m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        vu.m_fdiv = {};
    }

    static VU1_NOINLINE void foldStatusSlow(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        while (ctx.flagHead != ctx.flagTail)
        {
            const Vu1FlagRecord &record = ctx.flags[ctx.flagHead & Vu1FastCtx::kFlagMask];
            if (record.ready > cycle)
                break;
            if (vu.m_fdiv.valid && vu.m_fdiv.readyCycle < record.ready)
                commitDivResult(vu);
            foldFlag(vu, record);
            ++ctx.flagHead;
        }
        if (ctx.flagHead == ctx.flagTail)
            ctx.stickyPending = false;
        if (vu.m_fdiv.valid && vu.m_fdiv.readyCycle <= cycle)
            commitDivResult(vu);
    }

    static VU1_NOINLINE void foldStatus(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        if (ctx.stickyPending)
        {
            foldStatusSlow(vu, ctx, cycle);
            return;
        }
        uint32_t head = ctx.flagHead;
        const uint32_t start = head;
        const uint32_t tail = ctx.flagTail;
        uint32_t lastMac = 0u;
        uint32_t orMac = 0u;
        uint32_t orExtra = 0u;
        while (head != tail)
        {
            const Vu1FlagRecord &record = ctx.flags[head & Vu1FastCtx::kFlagMask];
            if (record.ready > cycle)
                break;
            lastMac = record.mac;
            orMac |= record.mac;
            orExtra |= record.extra;
            ++head;
        }
        if (head != start)
        {
            const auto nibbles = [](uint32_t mac)
            {
                return ((mac & 0x000Fu) != 0u ? 1u : 0u) | ((mac & 0x00F0u) != 0u ? 2u : 0u) |
                       ((mac & 0x0F00u) != 0u ? 4u : 0u) | ((mac & 0xF000u) != 0u ? 8u : 0u);
            };
            ctx.flagHead = head;
            vu.m_state.mac = lastMac;
            vu.m_state.status = (vu.m_state.status & 0xFF0u) | nibbles(lastMac) | ((nibbles(orMac) | orExtra) << 6);
        }
        if (vu.m_fdiv.valid && vu.m_fdiv.readyCycle <= cycle)
            commitDivResult(vu);
    }

    static VU1_NOINLINE void foldClip(Vu1FastCtx &ctx, VU1Interpreter &vu, uint64_t cycle)
    {
        while (ctx.clipHead != ctx.clipTail)
        {
            const Vu1FlagRecord &record = ctx.clips[ctx.clipHead & Vu1FastCtx::kClipMask];
            if (record.ready > cycle)
                break;
            if (record.writes & Vu1FlagRecord::kClip)
                vu.m_state.clip = record.clip;
            ++ctx.clipHead;
        }
    }

    static VU1_FORCE_INLINE void commitStatus(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        if (ctx.flagHead != ctx.flagTail || vu.m_fdiv.valid)
            foldStatus(vu, ctx, cycle);
    }

    static VU1_FORCE_INLINE void commitClip(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        if (ctx.clipHead != ctx.clipTail)
            foldClip(ctx, vu, cycle);
    }

    static VU1_FORCE_INLINE void commitFlags(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        commitStatus(vu, ctx, cycle);
        commitClip(vu, ctx, cycle);
    }

    static VU1_FORCE_INLINE void syncQ(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        if (vu.m_fdiv.valid && vu.m_fdiv.readyCycle <= cycle)
        {
            if (ctx.stickyPending)
                foldStatusSlow(vu, ctx, cycle);
            else
                commitDivResult(vu);
        }
    }

    static void commitEfu(VU1Interpreter &vu, uint64_t cycle)
    {
        for (;;)
        {
            int best = -1;
            uint64_t bestReady = std::numeric_limits<uint64_t>::max();
            for (int index = 0; index < 2; ++index)
            {
                const auto &entry = vu.m_efu[static_cast<size_t>(index)];
                if (entry.valid && entry.readyCycle <= cycle && entry.readyCycle < bestReady)
                {
                    best = index;
                    bestReady = entry.readyCycle;
                }
            }
            if (best < 0)
                return;
            vu.m_state.p = vu.m_efu[static_cast<size_t>(best)].value;
            vu.m_efu[static_cast<size_t>(best)] = {};
        }
    }

    static VU1_NOINLINE void makeStatusRoom(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        const uint32_t before = ctx.flagHead;
        foldStatus(vu, ctx, cycle);
        if (ctx.flagHead == before)
            foldStatusSlow(vu, ctx, ctx.flags[ctx.flagHead & Vu1FastCtx::kFlagMask].ready);
    }

    static VU1_NOINLINE void makeClipRoom(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        const uint32_t before = ctx.clipHead;
        foldClip(ctx, vu, cycle);
        if (ctx.clipHead == before)
            foldClip(ctx, vu, ctx.clips[ctx.clipHead & Vu1FastCtx::kClipMask].ready);
    }

    static VU1_FORCE_INLINE Vu1FlagRecord &statusPush(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        if (ctx.flagTail - ctx.flagHead == Vu1FastCtx::kFlagCapacity)
            makeStatusRoom(vu, ctx, cycle);
        Vu1FlagRecord &record = ctx.flags[ctx.flagTail++ & Vu1FastCtx::kFlagMask];
        record.ready = cycle + VU1Interpreter::kFmacLatency;
        record.mac = 0u;
        record.status = 0u;
        record.extra = 0u;
        record.clip = 0u;
        record.writes = 0u;
        return record;
    }

    static VU1_FORCE_INLINE Vu1FlagRecord &clipPush(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        if (ctx.clipTail - ctx.clipHead == Vu1FastCtx::kClipCapacity)
            makeClipRoom(vu, ctx, cycle);
        Vu1FlagRecord &record = ctx.clips[ctx.clipTail++ & Vu1FastCtx::kClipMask];
        record.ready = cycle + VU1Interpreter::kFmacLatency;
        record.mac = 0u;
        record.status = 0u;
        record.extra = 0u;
        record.clip = 0u;
        record.writes = 0u;
        return record;
    }

    static void loadFlags(VU1Interpreter &vu, Vu1FastCtx &ctx)
    {
        while (vu.m_flagValid != 0u)
        {
            int best = -1;
            uint64_t bestReady = std::numeric_limits<uint64_t>::max();
            for (uint32_t mask = vu.m_flagValid; mask != 0u; mask &= mask - 1u)
            {
                const int index = std::countr_zero(mask);
                const uint64_t ready = vu.m_flagPipeline[static_cast<size_t>(index)].readyCycle;
                if (ready < bestReady)
                {
                    best = index;
                    bestReady = ready;
                }
            }
            auto &entry = vu.m_flagPipeline[static_cast<size_t>(best)];
            const uint32_t writes = (entry.writesMac ? Vu1FlagRecord::kMac : 0u) | (entry.writesStatus ? Vu1FlagRecord::kStatus : 0u) |
                                    (entry.writesSticky ? Vu1FlagRecord::kSticky : 0u) | (entry.writesClip ? Vu1FlagRecord::kClip : 0u);
            const bool clip = entry.writesClip || (!entry.writesMac && !entry.writesStatus && !entry.writesSticky && entry.clip != 0u);
            Vu1FlagRecord &record = clip ? ctx.clips[ctx.clipTail++ & Vu1FastCtx::kClipMask] : ctx.flags[ctx.flagTail++ & Vu1FastCtx::kFlagMask];
            record.ready = entry.readyCycle;
            record.mac = entry.mac;
            record.status = entry.status;
            record.extra = entry.extraSticky;
            record.clip = entry.clip;
            record.writes = writes;
            if (entry.writesSticky)
                ctx.stickyPending = true;
            entry = {};
            vu.m_flagValid &= ~(1u << best);
        }
    }

    static void storeFlags(VU1Interpreter &vu, Vu1FastCtx &ctx)
    {
        uint32_t slot = 0u;
        while (ctx.flagHead != ctx.flagTail || ctx.clipHead != ctx.clipTail)
        {
            const bool takeClip = ctx.flagHead == ctx.flagTail ||
                                  (ctx.clipHead != ctx.clipTail &&
                                   ctx.clips[ctx.clipHead & Vu1FastCtx::kClipMask].ready < ctx.flags[ctx.flagHead & Vu1FastCtx::kFlagMask].ready);
            const Vu1FlagRecord &record = takeClip ? ctx.clips[ctx.clipHead++ & Vu1FastCtx::kClipMask] : ctx.flags[ctx.flagHead++ & Vu1FastCtx::kFlagMask];
            while ((vu.m_flagValid >> slot) & 1u)
                ++slot;
            auto &entry = vu.m_flagPipeline[slot];
            entry = {};
            entry.valid = true;
            entry.readyCycle = record.ready;
            entry.issueCycle = record.ready - VU1Interpreter::kFmacLatency;
            entry.mac = record.mac;
            entry.status = record.status;
            entry.extraSticky = record.extra;
            entry.clip = record.clip;
            entry.writesMac = (record.writes & Vu1FlagRecord::kMac) != 0u;
            entry.writesStatus = (record.writes & Vu1FlagRecord::kStatus) != 0u;
            entry.writesSticky = (record.writes & Vu1FlagRecord::kSticky) != 0u;
            entry.writesClip = (record.writes & Vu1FlagRecord::kClip) != 0u;
            vu.m_flagValid |= 1u << slot;
            vu.noteReadyCycle(entry.readyCycle);
        }
        ctx.stickyPending = false;
    }

    static VU1_FORCE_INLINE void pushFmacFlags(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t mac, uint32_t status,
                                               uint32_t extra)
    {
        Vu1FlagRecord &record = statusPush(vu, ctx, cycle);
        record.mac = mac;
        record.status = status;
        record.extra = extra;
        record.writes = Vu1FlagRecord::kMac | Vu1FlagRecord::kStatus;
    }

    static VU1_FORCE_INLINE void clearSameCycle(Vu1FlagRecord *ring, uint32_t mask, uint32_t head, uint32_t tail, uint64_t cycle, uint32_t writes)
    {
        const uint64_t ready = cycle + VU1Interpreter::kFmacLatency;
        for (uint32_t index = tail; index != head; --index)
        {
            Vu1FlagRecord &record = ring[(index - 1u) & mask];
            if (record.ready != ready)
                break;
            record.writes &= ~writes;
        }
    }

    static VU1_FORCE_INLINE void queueClipAt(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t clip)
    {
        vu.m_workingClip = ((vu.m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
        Vu1FlagRecord &record = clipPush(vu, ctx, cycle);
        record.clip = vu.m_workingClip;
        record.writes = Vu1FlagRecord::kClip;
    }

    static VU1_FORCE_INLINE void queueFssetAt(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t immediate)
    {
        clearSameCycle(ctx.flags, Vu1FastCtx::kFlagMask, ctx.flagHead, ctx.flagTail, cycle, Vu1FlagRecord::kStatus);
        Vu1FlagRecord &record = statusPush(vu, ctx, cycle);
        record.status = immediate & 0xFC0u;
        record.writes = Vu1FlagRecord::kSticky;
        ctx.stickyPending = true;
    }

    static VU1_FORCE_INLINE void queueFcsetAt(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t clip)
    {
        vu.m_workingClip = clip & 0xFFFFFFu;
        clearSameCycle(ctx.clips, Vu1FastCtx::kClipMask, ctx.clipHead, ctx.clipTail, cycle, Vu1FlagRecord::kClip);
        Vu1FlagRecord &record = clipPush(vu, ctx, cycle);
        record.clip = vu.m_workingClip;
        record.writes = Vu1FlagRecord::kClip;
    }

    static VU1_FORCE_INLINE void queueQAt(VU1Interpreter &vu, uint64_t cycle, float value, uint32_t latency, uint32_t statusDi)
    {
        uint32_t ignoredFlags = 0u;
        value = vu.normalizeResult(value, ignoredFlags);
        vu.m_fdiv.valid = true;
        vu.m_fdiv.readyCycle = cycle + latency;
        vu.m_fdiv.value = value;
        vu.m_fdiv.statusDi = statusDi & 0x30u;
        vu.noteReadyCycle(vu.m_fdiv.readyCycle);
    }

    struct FmacFlags
    {
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extra = 0;
    };

    static VU1_FORCE_INLINE __m128 kVf0()
    {
        return _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
    }

    template <uint32_t Lane>
    static VU1_FORCE_INLINE __m128 bcast(__m128 value)
    {
        return _mm_shuffle_ps(value, value, _MM_SHUFFLE(Lane, Lane, Lane, Lane));
    }

    template <uint32_t Lane>
    static VU1_FORCE_INLINE float laneF(__m128 value)
    {
        return _mm_cvtss_f32(_mm_shuffle_ps(value, value, _MM_SHUFFLE(Lane, Lane, Lane, Lane)));
    }

    static VU1_FORCE_INLINE void setReady4(uint64_t *ready, uint64_t value)
    {
        ready[0] = value;
        ready[1] = value;
        ready[2] = value;
        ready[3] = value;
    }

    template <FK K>
    static VU1_NOINLINE __m128 fmacExact(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t dest, __m128 s, __m128 t, __m128 a,
                                         __m128 result, __m128 &normalized, FmacFlags &flags)
    {
        constexpr bool cross = K == FK::Opmsub || K == FK::Opmula;
        constexpr bool productSum = K == FK::Madd || K == FK::Msub || K == FK::Opmsub;
        constexpr bool usesAcc = K == FK::Madd || K == FK::Msub || K == FK::Opmsub;
        const __m128d sLo = _mm_cvtps_pd(s);
        const __m128d sHi = _mm_cvtps_pd(_mm_movehl_ps(s, s));
        const __m128d tLo = _mm_cvtps_pd(t);
        const __m128d tHi = _mm_cvtps_pd(_mm_movehl_ps(t, t));
        __m128d eLo;
        __m128d eHi;
        __m128d pLo = _mm_setzero_pd();
        __m128d pHi = _mm_setzero_pd();
        if constexpr (K == FK::Add)
        {
            eLo = _mm_add_pd(sLo, tLo);
            eHi = _mm_add_pd(sHi, tHi);
        }
        else if constexpr (K == FK::Sub)
        {
            eLo = _mm_sub_pd(sLo, tLo);
            eHi = _mm_sub_pd(sHi, tHi);
        }
        else
        {
            pLo = _mm_mul_pd(sLo, tLo);
            pHi = _mm_mul_pd(sHi, tHi);
            if constexpr (usesAcc)
            {
                const __m128d aLo = _mm_cvtps_pd(a);
                const __m128d aHi = _mm_cvtps_pd(_mm_movehl_ps(a, a));
                if constexpr (K == FK::Madd)
                {
                    eLo = _mm_add_pd(aLo, pLo);
                    eHi = _mm_add_pd(aHi, pHi);
                }
                else
                {
                    eLo = _mm_sub_pd(aLo, pLo);
                    eHi = _mm_sub_pd(aHi, pHi);
                }
            }
            else
            {
                eLo = pLo;
                eHi = pHi;
            }
        }
        if constexpr (cross)
            eHi = _mm_unpacklo_pd(eHi, _mm_setzero_pd());

        const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
        const __m128d maxD = _mm_set1_pd(static_cast<double>(std::numeric_limits<float>::max()));
        const __m128d minD = _mm_set1_pd(static_cast<double>(std::numeric_limits<float>::min()));
        const __m128d aLo = _mm_and_pd(eLo, absMask);
        const __m128d aHi = _mm_and_pd(eHi, absMask);
        const __m128d zLo = _mm_cmpeq_pd(aLo, _mm_setzero_pd());
        const __m128d zHi = _mm_cmpeq_pd(aHi, _mm_setzero_pd());
        const __m128d oLo = _mm_cmpgt_pd(aLo, maxD);
        const __m128d oHi = _mm_cmpgt_pd(aHi, maxD);
        const __m128d smLo = _mm_cmplt_pd(aLo, minD);
        const __m128d smHi = _mm_cmplt_pd(aHi, minD);

        const __m128 smallF = _mm_shuffle_ps(_mm_castpd_ps(smLo), _mm_castpd_ps(smHi), _MM_SHUFFLE(2, 0, 2, 0));
        const __m128 overF = _mm_shuffle_ps(_mm_castpd_ps(oLo), _mm_castpd_ps(oHi), _MM_SHUFFLE(2, 0, 2, 0));
        const __m128 signF = _mm_and_ps(_mm_shuffle_ps(_mm_castpd_ps(eLo), _mm_castpd_ps(eHi), _MM_SHUFFLE(3, 1, 3, 1)),
                                        _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))));
        result = _mm_or_ps(_mm_andnot_ps(smallF, result), _mm_and_ps(smallF, signF));
        result = _mm_or_ps(_mm_andnot_ps(overF, result),
                           _mm_and_ps(overF, _mm_or_ps(signF, _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF)))));

        if (dest != 0u)
        {
            const uint32_t zeroBits = maskBits(zLo, zHi);
            const uint32_t smallBits = maskBits(smLo, smHi) & dest;
            const uint32_t signBits = maskBits(eLo, eHi) & dest;
            const uint32_t underBits = smallBits & ~zeroBits;
            const uint32_t overBits = maskBits(oLo, oHi) & dest;
            const uint32_t mac = smallBits | (signBits << 4) | (underBits << 8) | (overBits << 12);
            const uint32_t status = (smallBits ? 1u : 0u) | (signBits ? 2u : 0u) | (underBits ? 4u : 0u) | (overBits ? 8u : 0u);
            uint32_t extra = 0u;
            if constexpr (productSum)
            {
                const __m128d paLo = _mm_and_pd(pLo, absMask);
                const __m128d paHi = _mm_and_pd(pHi, absMask);
                const uint32_t pZero = maskBits(_mm_cmpeq_pd(paLo, _mm_setzero_pd()), _mm_cmpeq_pd(paHi, _mm_setzero_pd()));
                const uint32_t pSmall = maskBits(_mm_cmplt_pd(paLo, minD), _mm_cmplt_pd(paHi, minD)) & dest;
                const uint32_t pSign = maskBits(pLo, pHi) & dest;
                const uint32_t pOver = maskBits(_mm_cmpgt_pd(paLo, maxD), _mm_cmpgt_pd(paHi, maxD)) & dest;
                extra = (pSmall ? 1u : 0u) | (pSign ? 2u : 0u) | ((pSmall & ~pZero) ? 4u : 0u) | (pOver ? 8u : 0u);
            }
            flags.mac = mac;
            flags.status = status;
            flags.extra = extra;
        }
        normalized = vnorm(result);
        return result;
    }

    template <FK K>
    static VU1_FORCE_INLINE __m128 fmac(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t dest, __m128 s, __m128 t, __m128 a,
                                        __m128 &normalized)
    {
        FmacFlags flags;
        const __m128 result = fmacF<K>(vu, ctx, cycle, dest, s, t, a, normalized, flags);
        if (dest != 0u)
            pushFmacFlags(vu, ctx, cycle, flags.mac, flags.status, flags.extra);
        return result;
    }

    struct FmacCore
    {
        __m128 s;
        __m128 t;
        __m128 result;
        __m128 product;
        __m128 resultZero;
        __m128 productZero;
        bool safe;
    };

    template <FK K>
    static VU1_FORCE_INLINE FmacCore fmacCore(uint32_t dest, __m128 s, __m128 t, __m128 a)
    {
        constexpr bool cross = K == FK::Opmsub || K == FK::Opmula;
        constexpr bool productSum = K == FK::Madd || K == FK::Msub || K == FK::Opmsub;
        if constexpr (cross)
        {
            s = _mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 0, 2, 1));
            t = _mm_shuffle_ps(t, t, _MM_SHUFFLE(3, 1, 0, 2));
        }

        __m128 result;
        __m128 product = _mm_setzero_ps();
        if constexpr (K == FK::Add)
            result = _mm_add_ps(s, t);
        else if constexpr (K == FK::Sub)
            result = _mm_sub_ps(s, t);
        else
        {
            product = _mm_mul_ps(s, t);
            if constexpr (K == FK::Mul || K == FK::Opmula)
                result = product;
            else if constexpr (K == FK::Madd)
                result = _mm_add_ps(a, product);
            else
                result = _mm_sub_ps(a, product);
        }
        if constexpr (cross)
            result = _mm_and_ps(result, destMask(0xEu));

        const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
        const __m128 zero = _mm_setzero_ps();
        const __m128 absResult = _mm_and_ps(result, absMask);
        const __m128 resultRange = _mm_and_ps(_mm_cmpge_ps(absResult, _mm_set1_ps(4.0f * std::numeric_limits<float>::min())),
                                              _mm_cmplt_ps(absResult, _mm_set1_ps(0.5f * std::numeric_limits<float>::max())));
        const __m128 resultZero = _mm_cmpeq_ps(result, zero);
        __m128 safe = resultRange;
        __m128 productZero = zero;
        if constexpr (K == FK::Add)
            safe = _mm_or_ps(safe, _mm_and_ps(resultZero, _mm_cmpeq_ps(s, _mm_xor_ps(t, _mm_set1_ps(-0.0f)))));
        else if constexpr (K == FK::Sub)
            safe = _mm_or_ps(safe, _mm_and_ps(resultZero, _mm_cmpeq_ps(s, t)));
        else
        {
            productZero = _mm_or_ps(_mm_cmpeq_ps(s, zero), _mm_cmpeq_ps(t, zero));
            if constexpr (productSum)
            {
                const __m128 absProduct = _mm_and_ps(product, absMask);
                safe = _mm_and_ps(safe, _mm_cmpgt_ps(absResult, _mm_mul_ps(absProduct, _mm_set1_ps(1.0f / 2097152.0f))));
                safe = _mm_and_ps(safe, _mm_and_ps(_mm_cmpge_ps(absProduct, _mm_set1_ps(std::numeric_limits<float>::min())),
                                                   _mm_cmplt_ps(absProduct, _mm_set1_ps(std::numeric_limits<float>::max()))));
                safe = _mm_or_ps(safe, productZero);
            }
            else
                safe = _mm_or_ps(safe, _mm_and_ps(resultZero, productZero));
        }
        const uint32_t destLanes = kRev4[dest];
        return {s, t, result, product, resultZero, productZero,
                (static_cast<uint32_t>(_mm_movemask_ps(safe)) & destLanes) == destLanes};
    }

    template <FK K>
    static VU1_FORCE_INLINE __m128 fmacF(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t dest, __m128 s, __m128 t, __m128 a,
                                         __m128 &normalized, FmacFlags &flags)
    {
        constexpr bool productSum = K == FK::Madd || K == FK::Msub || K == FK::Opmsub;
        const FmacCore c = fmacCore<K>(dest, s, t, a);
        if (!c.safe)
            return fmacExact<K>(vu, ctx, cycle, dest, c.s, c.t, a, c.result, normalized, flags);
        if (dest != 0u)
        {
            const uint32_t destLanes = kRev4[dest];
            const uint32_t sign = kRev4[_mm_movemask_ps(c.result)] & dest;
            const uint32_t zeroBits = kRev4[_mm_movemask_ps(c.resultZero)] & dest;
            uint32_t extra = 0u;
            if constexpr (productSum)
            {
                extra = (static_cast<uint32_t>(_mm_movemask_ps(c.product)) & destLanes) != 0u ? 2u : 0u;
                if ((static_cast<uint32_t>(_mm_movemask_ps(c.productZero)) & destLanes) != 0u)
                    extra |= 1u;
            }
            flags.mac = zeroBits | (sign << 4);
            flags.status = (zeroBits != 0u ? 1u : 0u) | (sign != 0u ? 2u : 0u);
            flags.extra = extra;
        }
        normalized = c.result;
        return c.result;
    }

    template <FK K>
    static VU1_FORCE_INLINE __m128 fmacV(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t dest, __m128 s, __m128 t, __m128 a,
                                         __m128 &normalized, __m128 &accZero, __m128 &accSign, uint32_t &accScalar)
    {
        constexpr bool productSum = K == FK::Madd || K == FK::Msub || K == FK::Opmsub;
        const FmacCore c = fmacCore<K>(dest, s, t, a);
        if (!c.safe)
        {
            FmacFlags flags;
            const __m128 result = fmacExact<K>(vu, ctx, cycle, dest, c.s, c.t, a, c.result, normalized, flags);
            accScalar |= flags.status | flags.extra;
            return result;
        }
        __m128 zeroLanes = c.resultZero;
        __m128 signLanes = c.result;
        if constexpr (productSum)
        {
            zeroLanes = _mm_or_ps(zeroLanes, c.productZero);
            signLanes = _mm_or_ps(signLanes, c.product);
        }
        if (dest != 0xFu)
        {
            const __m128 mask = destMask(dest);
            zeroLanes = _mm_and_ps(zeroLanes, mask);
            signLanes = _mm_and_ps(signLanes, mask);
        }
        accZero = _mm_or_ps(accZero, zeroLanes);
        accSign = _mm_or_ps(accSign, signLanes);
        normalized = c.result;
        return c.result;
    }

    // FMAC for code whose MAC/status flags are only observed as the segment's folded record (vu1_recomp's dead-flag
    // analysis), the way microVU computes it: the plain SSE operation and nothing else. VU jobs run with round toward
    // zero, FTZ and DAZ (MXCSR 0x6000 | FTZ | DAZ), so an overflow already gives +-FLT_MAX (round toward zero never
    // reaches infinity), a result below FLT_MIN is already +-0, and operands are normalized (no inf/NaN). That is what
    // fmacExact produces too; the range checks fmacCore adds only matter when a MADD/MSUB cancels so far that the
    // product's rounding decides between a tiny result and an exact zero, which PS2X_VU1_VERIFY=obs would report.
    // The sticky zero/sign bits still accumulate exactly like fmacV.
    //
    // One exception is handled: a MADD/MSUB whose product saturates. Round toward zero clamps the product to FLT_MAX
    // before the accumulator is added, so ACC = -FLT_MAX gave 0 where the exact sum overflows to +FLT_MAX (Urban
    // Reign's program at 0x3568: MADD then MINI.w with 255 gave 0 instead of 255). Such lanes take fmacV.
    template <FK K, bool Sticky>
    static VU1_FORCE_INLINE __m128 fmacFastImpl(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t dest, __m128 s, __m128 t,
                                                __m128 a, __m128 &normalized, __m128 &accZero, __m128 &accSign, uint32_t &accScalar)
    {
        constexpr bool cross = K == FK::Opmsub || K == FK::Opmula;
        constexpr bool productSum = K == FK::Madd || K == FK::Msub || K == FK::Opmsub;
        const __m128 s0 = s;
        const __m128 t0 = t;
        if constexpr (cross)
        {
            s = _mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 0, 2, 1));
            t = _mm_shuffle_ps(t, t, _MM_SHUFFLE(3, 1, 0, 2));
        }
        __m128 result;
        __m128 product = _mm_setzero_ps();
        if constexpr (K == FK::Add)
            result = _mm_add_ps(s, t);
        else if constexpr (K == FK::Sub)
            result = _mm_sub_ps(s, t);
        else
        {
            product = _mm_mul_ps(s, t);
            if constexpr (productSum)
            {
                const __m128 absProduct = _mm_and_ps(product, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
                if ((static_cast<uint32_t>(_mm_movemask_ps(_mm_cmpge_ps(absProduct, _mm_set1_ps(std::numeric_limits<float>::max())))) &
                     kRev4[dest]) != 0u)
                    return fmacV<K>(vu, ctx, cycle, dest, s0, t0, a, normalized, accZero, accSign, accScalar);
            }
            if constexpr (K == FK::Mul || K == FK::Opmula)
                result = product;
            else if constexpr (K == FK::Madd)
                result = _mm_add_ps(a, product);
            else
                result = _mm_sub_ps(a, product);
        }
        if constexpr (cross)
            result = _mm_and_ps(result, destMask(0xEu));
        if constexpr (Sticky)
        {
            const __m128 zero = _mm_setzero_ps();
            __m128 zeroLanes = _mm_cmpeq_ps(result, zero);
            __m128 signLanes = result;
            if constexpr (productSum)
            {
                zeroLanes = _mm_or_ps(zeroLanes, _mm_or_ps(_mm_cmpeq_ps(s, zero), _mm_cmpeq_ps(t, zero)));
                signLanes = _mm_or_ps(signLanes, product);
            }
            if (dest != 0xFu)
            {
                const __m128 mask = destMask(dest);
                zeroLanes = _mm_and_ps(zeroLanes, mask);
                signLanes = _mm_and_ps(signLanes, mask);
            }
            accZero = _mm_or_ps(accZero, zeroLanes);
            accSign = _mm_or_ps(accSign, signLanes);
        }
        normalized = result;
        return result;
    }

    template <FK K>
    static VU1_FORCE_INLINE __m128 fmacFastV(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t dest, __m128 s, __m128 t,
                                             __m128 a, __m128 &normalized, __m128 &accZero, __m128 &accSign, uint32_t &accScalar)
    {
        return fmacFastImpl<K, true>(vu, ctx, cycle, dest, s, t, a, normalized, accZero, accSign, accScalar);
    }

    // fmacFastV without the sticky zero/sign accumulation, for images where no instruction reads the status flag
    // (vu1_recomp checks; VU1_RECOMP_STICKY=1 keeps fmacFastV). Only the MAC/status of the segment's last FMAC
    // (fmacF + pushFmacFlags) remain exact; the sticky bits of the final status lose the other FMACs' contributions,
    // which nothing can read (see noteVu0DataAddress for the one path from outside VU1).
    template <FK K>
    static VU1_FORCE_INLINE __m128 fmacFastN(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t dest, __m128 s, __m128 t,
                                             __m128 a, __m128 &normalized)
    {
        __m128 unusedZero = _mm_setzero_ps(), unusedSign = _mm_setzero_ps();
        uint32_t unusedScalar = 0u;
        return fmacFastImpl<K, false>(vu, ctx, cycle, dest, s, t, a, normalized, unusedZero, unusedSign, unusedScalar);
    }

    static VU1_FORCE_INLINE uint32_t stickyOf(__m128 accZero, __m128 accSign)
    {
        return (_mm_movemask_ps(accZero) != 0 ? 1u : 0u) | (_mm_movemask_ps(accSign) != 0 ? 2u : 0u);
    }

    template <uint32_t Shift>
    static VU1_FORCE_INLINE __m128 itof(__m128 raw)
    {
        const __m128 value = _mm_cvtepi32_ps(_mm_castps_si128(raw));
        if constexpr (Shift == 0u)
            return value;
        else
            return _mm_mul_ps(value, _mm_set1_ps(1.0f / static_cast<float>(1u << Shift)));
    }

    template <uint32_t Shift>
    static VU1_FORCE_INLINE __m128 ftoi(__m128 normalized)
    {
        __m128 scaled = normalized;
        if constexpr (Shift != 0u)
            scaled = _mm_mul_ps(normalized, _mm_set1_ps(static_cast<float>(1u << Shift)));
        const __m128i truncated = _mm_cvttps_epi32(scaled);
        const __m128i big = _mm_castps_si128(_mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f)));
        return _mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(big, truncated), _mm_and_si128(big, _mm_set1_epi32(0x7FFFFFFF))));
    }

    static VU1_FORCE_INLINE __m128 absN(__m128 normalized)
    {
        return _mm_andnot_ps(_mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))), normalized);
    }

    static VU1_FORCE_INLINE bool simdClipEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_VU1_SIMD_CLIP");
            return !value || *value != '0';
        }();
        return enabled;
    }

    static VU1_FORCE_INLINE void clipOp(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, __m128 fsRaw, __m128 ftRaw)
    {
        const uint32_t wBits = static_cast<uint32_t>(_mm_extract_ps(ftRaw, 3));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        if (simdClipEnabled())
        {
            const __m128i bits = _mm_castps_si128(fsRaw);
            const __m128i bounds = _mm_set1_epi32(limit);
            const uint32_t positive = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(bits, bounds)))) & 7u;
            const uint32_t negative = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(
                _mm_xor_si128(bits, _mm_set1_epi32(static_cast<int32_t>(0x80000000u))), bounds)))) & 7u;
            const uint32_t plus = (positive & 1u) | ((positive & 2u) << 1u) | ((positive & 4u) << 2u);
            const uint32_t minus = ((negative & 1u) << 1u) | ((negative & 2u) << 2u) | ((negative & 4u) << 3u);
            queueClipAt(vu, ctx, cycle, plus | minus);
            return;
        }
        alignas(16) uint32_t lanes[4];
        _mm_store_ps(reinterpret_cast<float *>(lanes), fsRaw);
        uint32_t flags = 0u;
        for (uint32_t component = 0; component < 3u; ++component)
        {
            const uint32_t bits = lanes[component];
            if (static_cast<int32_t>(bits) > limit)
                flags |= 1u << (component * 2u);
            if (static_cast<int32_t>(bits ^ 0x80000000u) > limit)
                flags |= 2u << (component * 2u);
        }
        queueClipAt(vu, ctx, cycle, flags);
    }

    static VU1_FORCE_INLINE uint32_t qaddr(int32_t base, int32_t imm)
    {
        return (static_cast<uint32_t>(base + imm) * 16u) & (kFastMemSize - 1u);
    }

    // qaddr for images that may be VU0 programs (vu1_recomp emits it for images that fit in 4 KB).
    static VU1_FORCE_INLINE uint32_t qaddrV0(int32_t base, int32_t imm)
    {
        noteVu0DataAddress(true, static_cast<uint32_t>(base + imm) & 0xFFFFu);
        return qaddr(base, imm);
    }

    static VU1_FORCE_INLINE __m128 ldq(const uint8_t *mem, uint32_t address)
    {
        return _mm_loadu_ps(reinterpret_cast<const float *>(mem + address));
    }

    static VU1_FORCE_INLINE uint32_t ldw(const uint8_t *mem, uint32_t address)
    {
        uint32_t value = 0u;
        std::memcpy(&value, mem + address, sizeof(value));
        return value;
    }

    static void xgkickAdvance(VU1Interpreter &vu, uint64_t cycles)
    {
        auto &x = vu.m_xgkick;
        if (!x.active || cycles == 0u || !vu.m_activeVuData || vu.m_activeVuDataSize == 0u)
            return;
        const uint64_t total = x.cycleCredit + cycles;
        uint64_t copies = total / 2u;
        const uint32_t remainder = static_cast<uint32_t>(total % 2u);
        const uint32_t size = vu.m_activeVuDataSize;
        while (copies > 0u && x.active)
        {
            const uint64_t inTag = x.currentTagEnd > x.copiedBytes ? (x.currentTagEnd - x.copiedBytes) / 16u : 0u;
            if (x.currentTagEnd == 0u || inTag <= 1u || x.copiedBytes > VU1Interpreter::XgkickPipeline::kBufferSize - 16u)
            {
                x.cycleCredit = 1u;
                vu.progressXgkick();
                --copies;
                continue;
            }
            uint32_t bulk = static_cast<uint32_t>(std::min<uint64_t>(copies, inTag - 1u)) * 16u;
            copies -= bulk / 16u;
            while (bulk != 0u)
            {
                const uint32_t source = (x.sourceAddress + x.copiedBytes) % size;
                const uint32_t chunk = std::min(bulk, size - source);
                std::memcpy(x.packet.data() + x.copiedBytes, vu.m_activeVuData + source, chunk);
                x.copiedBytes += chunk;
                bulk -= chunk;
            }
        }
        if (x.active)
            x.cycleCredit = remainder;
    }

    static VU1_FORCE_INLINE void xgkickSync(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle)
    {
        if (vu.m_xgkick.active && cycle > ctx.xgkickDone)
            xgkickAdvance(vu, cycle - ctx.xgkickDone);
        ctx.xgkickDone = cycle;
    }

    static VU1_FORCE_INLINE void stq(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t address, __m128 value, uint32_t dest)
    {
        xgkickSync(vu, ctx, cycle);
        storeDest(reinterpret_cast<float *>(ctx.mem + address), value, dest);
    }

    static VU1_FORCE_INLINE void stw(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t address, int32_t vi, uint32_t dest)
    {
        stq(vu, ctx, cycle, address, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(static_cast<uint16_t>(vi & 0xFFFF)))), dest);
    }

    static uint64_t xgkickRun(VU1Interpreter &vu, uint64_t maxCycles)
    {
        const uint32_t credit = vu.m_xgkick.cycleCredit;
        const uint32_t copiedBefore = vu.m_xgkick.copiedBytes;
        xgkickAdvance(vu, maxCycles);
        if (vu.m_xgkick.active)
            return maxCycles;
        const uint64_t copies = (vu.m_xgkick.copiedBytes - copiedBefore) / 16u;
        return copies == 0u ? 0u : 2u * copies - credit;
    }

    static bool xgkickStall(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t &cycle, uint64_t regReady, uint64_t budget)
    {
        xgkickSync(vu, ctx, cycle);
        uint64_t finish = cycle;
        if (vu.m_xgkick.active)
        {
            const uint64_t used = xgkickRun(vu, budget > cycle ? budget - cycle : 0u);
            if (vu.m_xgkick.active)
            {
                ctx.xgkickDone = budget;
                cycle = budget;
                return false;
            }
            finish = cycle + used;
            ctx.xgkickDone = finish;
        }
        const uint64_t target = rmax(regReady, finish);
        if (target <= cycle)
            return true;
        if (target >= budget)
        {
            xgkickSync(vu, ctx, budget);
            cycle = budget;
            return false;
        }
        xgkickSync(vu, ctx, target);
        cycle = target;
        return true;
    }

    static VU1_FORCE_INLINE void xgkickStart(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, int32_t vi, uint32_t pc)
    {
        vu.m_cycle = cycle;
        vu.startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(vi)), pc);
        ctx.xgkickDone = cycle;
    }

    static VU1_FORCE_INLINE void divOp(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, float numRaw, float denRaw)
    {
        syncQ(vu, ctx, cycle);
        const float num = vu.normalizeOperand(numRaw);
        const float den = vu.normalizeOperand(denRaw);
        uint32_t statusDi = 0u;
        float result = 0.0f;
        if (den == 0.0f)
        {
            statusDi = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num) != std::signbit(den)
                         ? -std::numeric_limits<float>::max()
                         : std::numeric_limits<float>::max();
        }
        else
        {
            result = num / den;
        }
        uint32_t ignoredFlags = 0u;
        result = vu.normalizeResult(result, ignoredFlags);
        queueQAt(vu, cycle, result, 7u, statusDi);
    }

    static VU1_FORCE_INLINE void sqrtOp(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, float raw)
    {
        syncQ(vu, ctx, cycle);
        const float val = vu.normalizeOperand(raw);
        queueQAt(vu, cycle, std::sqrt(std::fabs(val)), 7u, val < 0.0f ? 0x10u : 0u);
    }

    static VU1_FORCE_INLINE void rsqrtOp(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, float numRaw, float radicandRaw)
    {
        syncQ(vu, ctx, cycle);
        const float num = vu.normalizeOperand(numRaw);
        const float radicand = vu.normalizeOperand(radicandRaw);
        const float den = std::sqrt(std::fabs(radicand));
        uint32_t statusDi = radicand < 0.0f ? 0x10u : 0u;
        float result = 0.0f;
        if (den != 0.0f)
            result = num / den;
        else
        {
            statusDi = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num)
                         ? -std::numeric_limits<float>::max()
                         : std::numeric_limits<float>::max();
        }
        uint32_t ignoredFlags = 0u;
        result = vu.normalizeResult(result, ignoredFlags);
        queueQAt(vu, cycle, result, 13u, statusDi);
    }

    static void genericUpper(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t upper)
    {
        vu.m_cycle = cycle;
        commitFlags(vu, ctx, cycle);
        storeFlags(vu, ctx);
        vu.execUpper(upper);
        loadFlags(vu, ctx);
    }

    static void genericLower(VU1Interpreter &vu, Vu1FastCtx &ctx, uint64_t cycle, uint32_t lower, uint32_t upper)
    {
        vu.m_cycle = cycle;
        commitFlags(vu, ctx, cycle);
        storeFlags(vu, ctx);
        commitEfu(vu, cycle);
        xgkickSync(vu, ctx, cycle);
        vu.execLower(lower, ctx.mem, kFastMemSize, *ctx.gs, ctx.memory, upper);
        for (uint32_t mask = vu.m_storeValid; mask != 0u; mask &= mask - 1u)
        {
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(mask));
            auto &store = vu.m_storePipeline[index];
            uint32_t words[4]{};
            std::memcpy(words, ctx.mem + store.address, sizeof(words));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    words[component] = store.words[component];
            }
            std::memcpy(ctx.mem + store.address, words, sizeof(words));
            store = {};
        }
        vu.m_storeValid = 0u;
        if (vu.m_xgkick.active && vu.m_xgkick.issueCycle == cycle)
            ctx.xgkickDone = cycle;
        loadFlags(vu, ctx);
    }

    static void materializeWrites(VU1Interpreter &vu)
    {
        for (uint32_t mask = vu.m_vfWriteValid; mask != 0u; mask &= mask - 1u)
        {
            auto &write = vu.m_vfWritePipeline[static_cast<size_t>(std::countr_zero(mask))];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((write.laneMask & laneForComponent(component)) != 0u &&
                    vu.m_vfLatestWrite[write.reg][component] == write.sequence)
                    vu.m_state.vf[write.reg][component] = write.value[component];
            }
            write.valid = false;
        }
        vu.m_vfWriteValid = 0u;
        for (uint32_t mask = vu.m_viWriteValid; mask != 0u; mask &= mask - 1u)
        {
            auto &write = vu.m_viWritePipeline[static_cast<size_t>(std::countr_zero(mask))];
            if (vu.m_viLatestWrite[write.reg] == write.sequence)
                vu.m_state.vi[write.reg] = static_cast<int16_t>(write.value);
            write.valid = false;
        }
        vu.m_viWriteValid = 0u;
        for (uint32_t mask = vu.m_accWriteValid; mask != 0u; mask &= mask - 1u)
        {
            auto &write = vu.m_accWritePipeline[static_cast<size_t>(std::countr_zero(mask))];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((write.laneMask & laneForComponent(component)) != 0u &&
                    vu.m_accLatestWrite[component] == write.sequence)
                    vu.m_state.acc[component] = write.value[component];
            }
            write.valid = false;
        }
        vu.m_accWriteValid = 0u;
    }

    static StepResult runFast(VU1Interpreter &vu, const Vu1RunArgs &args, const FastBlock *table, uint32_t entries)
    {
        if (vu.m_state.branchPending || vu.m_state.ebit || vu.m_state.haltAfterDelaySlot || vu.m_stopRequested ||
            args.dataSize != kFastMemSize || args.codeSize != kFastCodeSize || !args.vuData || vu.m_storeValid != 0u)
            return StepResult::Fallback;
        materializeWrites(vu);
        Vu1FastCtx ctx;
        Vu1NormShadow &normShadow = vu.m_unit == VU1Interpreter::Unit::VU1 ? vu1NormShadow : vu0NormShadow;
        ctx.norm = normShadow.norm;
        if (!normShadow.valid || normShadow.owner != &vu)
        {
            syncAllNorm(vu, ctx);
            normShadow.owner = &vu;
            normShadow.valid = true;
        }
        ctx.cycle = vu.m_cycle;
        ctx.budget = args.budgetEnd;
        ctx.xgkickDone = vu.m_cycle;
        ctx.mem = args.vuData;
        ctx.gs = args.gs;
        ctx.memory = args.memory;
        loadFlags(vu, ctx);
        StepResult result = StepResult::Fallback;
        for (;;)
        {
            const uint32_t pc = vu.m_state.pc;
            const uint32_t index = pc >> 3;
            if ((pc & 7u) != 0u || index >= entries || table[index] == nullptr)
            {
                result = StepResult::Fallback;
                break;
            }
            result = table[index](vu, ctx);
            if (result != StepResult::Continue)
                break;
            if (vu.m_stopRequested)
            {
                result = StepResult::Budget;
                break;
            }
        }
        xgkickSync(vu, ctx, ctx.cycle);
        if (result == StepResult::Ended)
        {
            constexpr uint64_t kAll = std::numeric_limits<uint64_t>::max() / 2u;
            xgkickAdvance(vu, kAll);
            commitFlags(vu, ctx, kAll);
            commitEfu(vu, kAll);
        }
        else
        {
            commitFlags(vu, ctx, ctx.cycle);
            commitEfu(vu, ctx.cycle);
        }
        storeFlags(vu, ctx);
        vu.m_cycle = ctx.cycle;
        vu.m_state.cycles = ctx.cycle;
        return result;
    }

    static VU1_FORCE_INLINE StepResult stepPair(VU1Interpreter &vu,
                                                const VU1Interpreter::DecodedInstructionPair &decoded,
                                                uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory,
                                                uint32_t codeSize, uint64_t budgetEnd)
    {
        bool ended = false;
            uint64_t readyCycle = vu.calculatePairReadyCycle(decoded);
            while (readyCycle > vu.m_cycle)
            {
                if (readyCycle >= budgetEnd)
                {
                    vu.advanceTo(budgetEnd);
                    break;
                }
                vu.advanceTo(readyCycle);
                readyCycle = vu.calculatePairReadyCycle(decoded);
            }
            if (vu.m_cycle >= budgetEnd)
                return StepResult::Budget;

            uint8_t writtenVi = 0u;
            int32_t oldVi = 0;
            if (const uint32_t viWrites = decoded.lowerUsage.viWrite & 0xFFFEu; viWrites != 0u)
            {
                writtenVi = static_cast<uint8_t>(std::countr_zero(viWrites));
                oldVi = vu.m_state.vi[writtenVi];
            }

            const VU1Interpreter::VfAccess upperWrite = decoded.upperUsage.vfWrite;
            const VU1Interpreter::VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
            const bool hasUpperWrite = upperWrite.reg != 0u;
            const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
            const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
            float oldUpperVf[4]{};
            float newUpperVf[4]{};
            float oldLowerVf[4]{};
            float newLowerVf[4]{};
            float oldAcc[4]{};
            float newAcc[4]{};
            if (hasUpperWrite)
                std::memcpy(oldUpperVf, vu.m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
            if (hasDistinctLowerWrite)
                std::memcpy(oldLowerVf, vu.m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
            if (decoded.upperUsage.accWrite != 0u)
                std::memcpy(oldAcc, vu.m_state.acc, sizeof(oldAcc));

            if (decoded.iBit)
            {
                vu.execUpper(decoded.upper);
                float immediate = 0.0f;
                std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
                vu.m_state.i = vu.normalizeOperand(immediate);
            }
            else if (decoded.upperVfShadowReg != 0u)
            {
                float oldVf[4]{};
                float upperVf[4]{};
                std::memcpy(oldVf,
                            vu.m_state.vf[decoded.upperVfShadowReg],
                            sizeof(oldVf));
                vu.execUpper(decoded.upper);
                std::memcpy(upperVf,
                            vu.m_state.vf[decoded.upperVfShadowReg],
                            sizeof(upperVf));
                std::memcpy(vu.m_state.vf[decoded.upperVfShadowReg],
                            oldVf,
                            sizeof(oldVf));
                vu.execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
                std::memcpy(vu.m_state.vf[decoded.upperVfShadowReg],
                            upperVf,
                            sizeof(upperVf));
            }
            else
            {
                vu.execUpper(decoded.upper);
                vu.execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            }

            vu.m_viBranchBackupValid = false;

            if (hasUpperWrite)
            {
                std::memcpy(newUpperVf, vu.m_state.vf[upperWrite.reg], sizeof(newUpperVf));
                std::memcpy(vu.m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
                const uint32_t latency =
                    decoded.upperUsage.vfLatency != 0u
                        ? decoded.upperUsage.vfLatency
                        : decoded.upperUsage.latency;
                vu.queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
            }
            if (hasDistinctLowerWrite)
            {
                std::memcpy(newLowerVf, vu.m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
                std::memcpy(vu.m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
                const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                             ? decoded.lowerUsage.vfLatency
                                             : decoded.lowerUsage.latency;
                vu.queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
            }
            if (decoded.upperUsage.accWrite != 0u)
            {
                std::memcpy(newAcc, vu.m_state.acc, sizeof(newAcc));
                std::memcpy(vu.m_state.acc, oldAcc, sizeof(oldAcc));
                // ACC is forwarded to the next upper instruction. Its arithmetic
                // flags still use the normal four-cycle FMAC timeline.
                vu.queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                              VU1Interpreter::kAccForwardLatency);
            }
            if (writtenVi != 0u)
            {
                const int32_t newVi = vu.m_state.vi[writtenVi];
                vu.m_state.vi[writtenVi] = oldVi;
                const uint32_t latency =
                    decoded.lowerUsage.viLatency != 0u
                        ? decoded.lowerUsage.viLatency
                        : decoded.lowerUsage.latency;
                vu.queueViWrite(writtenVi, newVi, latency);
            }

            vu.markPairWrites(decoded);
            if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
                vu.recordViWriteForBranch(writtenVi, oldVi);

            vu.m_state.vf[0][0] = 0.0f;
            vu.m_state.vf[0][1] = 0.0f;
            vu.m_state.vf[0][2] = 0.0f;
            vu.m_state.vf[0][3] = 1.0f;
            vu.m_state.vi[0] = 0;

            uint32_t nextPc = vu.m_state.pc + 8u;
            if (nextPc >= codeSize)
                nextPc = 0u;
            vu.m_state.pc = nextPc;

            if (vu.m_state.branchPending)
            {
                if (vu.m_state.branchDelay == 0u)
                {
                    vu.m_state.pc = vu.m_state.branchTarget & vu.microAddressMask();
                    vu.m_state.branchPending = false;
                }
                else
                {
                    --vu.m_state.branchDelay;
                }
            }

            const bool dHalt = decoded.dBit && vu.m_state.dBitEnabled;
            const bool tHalt = decoded.tBit && vu.m_state.tBitEnabled;
            const bool haltBit = dHalt || tHalt;
            const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == VU1Interpreter::PipelineBranch;

            if (vu.m_state.haltAfterDelaySlot)
            {
                vu.m_state.stoppedByD = vu.m_pendingHaltD;
                vu.m_state.stoppedByT = vu.m_pendingHaltT;
                ended = true;
            }
            else if (vu.m_state.ebit)
                ended = true;
            else if (haltBit && !haltBranch)
            {
                vu.m_state.stoppedByD = dHalt;
                vu.m_state.stoppedByT = tHalt;
                ended = true;
            }
            else if (decoded.eBit)
                vu.m_state.ebit = true;
            else if (haltBranch)
            {
                vu.m_state.haltAfterDelaySlot = true;
                vu.m_pendingHaltD = dHalt;
                vu.m_pendingHaltT = tHalt;
            }

        vu.advanceOneCycle();
        return ended ? StepResult::Ended : StepResult::Continue;
    }
};

void registerVu1CompiledProgram(uint64_t imageHash, VU1InterpreterAccess::CompiledProgram program);
// Blocks compiled ahead of time for one piece of code (vu1_recomp: a captured image or a microcode upload found on the
// disc). At run time the blocks whose pairs [first, checkEnd) equal the current micro memory are assembled into a
// table for that memory, whatever else is loaded next to them (pairs [checkStart, checkEnd) are compared). noSticky: compiled for code without status flag readers.
struct Vu1CompiledUnitBlock
{
    uint32_t first;      // pairs the block runs
    uint32_t last;
    uint32_t checkStart; // pairs its code was compiled from (itself, delay slot, flag analysis)
    uint32_t checkEnd;
    uint64_t suffixSafe[2]; // bit k: pairs [first + k, last) may be used alone when [first + k, checkEnd) match
    bool noSticky;
    VU1InterpreterAccess::FastBlock fn;
};
struct Vu1CompiledUnit
{
    uint32_t start; // pairs [start, end) of micro memory, as bytes
    uint32_t end;
    const uint8_t *bytes;
    const Vu1CompiledUnitBlock *blocks;
    uint32_t blockCount;
    bool image; // a captured image (false: an upload)
};
void registerVu1CompiledUnit(const Vu1CompiledUnit &unit);
VU1InterpreterAccess::CompiledProgram findCompiledVu1ProgramByHash(uint64_t imageHash);
