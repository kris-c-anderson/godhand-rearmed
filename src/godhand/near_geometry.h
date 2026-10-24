#pragma once

#include <cstdint>

namespace godhand
{
    // Walls and floors next to the camera drawn instead of dropped (near_geometry.cpp): the scenery renderers' VU1
    // clip test patched to the far plane only, and every PATH1 packet's triangles that the GS cannot take (corners
    // off its 16-bit screen range or behind the camera) cut and sent again. [Video] NearGeometry (GODHAND_NEAR_GEOMETRY,
    // on by default), read here; call before the game starts.
    void installNearGeometry();
    // Counters for the log / menu: renderer tests patched, triangles cut in front of the camera, cut at the near
    // plane, and triangles reaching behind the camera with no stitch information.
    void nearGeometryStats(uint64_t &patched, uint64_t &cut, uint64_t &cutNear, uint64_t &noStitch);
}
