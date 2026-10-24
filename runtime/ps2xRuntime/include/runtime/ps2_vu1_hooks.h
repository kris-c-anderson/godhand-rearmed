#pragma once

#include <cstdint>
#include <vector>

// Hooks a game can set on VU1's microcode and on its PATH1 output (vu/ps2_vu1_core.cpp). Both are called on the
// thread that runs VU1 (the VIF1 / MTVU worker), in VIF order: an upload is seen before any XGKICK of the code it
// brings. Set them before the game starts; nullptr = none.
namespace ps2x::vu1hooks
{
    // After an MPG upload has been written to VU1 micro memory: code = the whole 16 KB micro memory (it may be
    // changed in place, e.g. to patch the program just uploaded), [byteAddress, byteAddress + size) = what the
    // upload wrote. Anything changed here must match what the ahead-of-time VU1 code was compiled from, or that
    // code stays interpreted.
    using UploadHook = void (*)(uint8_t *code, uint32_t codeSize, uint32_t byteAddress, uint32_t size);
    void setUploadHook(UploadHook hook);
    void runUploadHook(uint8_t *code, uint32_t codeSize, uint32_t byteAddress, uint32_t size);

    // A finished XGKICK packet, before it goes to the GS: packet may be changed in place (same size); extra = GIF
    // packets to send right after it (left empty: none). sourceAddress = where in VU1 data memory the packet was
    // read from (bytes); vuData = VU1 data memory as it is now; kickPc = the micro-memory byte address of the XGKICK
    // that sent it, which tells the program apart.
    using Path1Filter = void (*)(uint8_t *packet, uint32_t size, uint32_t sourceAddress, const uint8_t *vuData,
                                 uint32_t vuDataSize, uint32_t kickPc, std::vector<uint8_t> &extra);
    void setPath1Filter(Path1Filter filter);
    [[nodiscard]] Path1Filter path1Filter();
}
