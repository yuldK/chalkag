#pragma once

#include <cstdint>

class SkCanvas;

namespace chalkag {
    // 중앙은 알파 0으로 두고 프레임 바깥에만 장식광을 그린다.
    void draw_frame_glow(SkCanvas& canvas, int width, int height, int halo, float scale, std::uint32_t color);
} // namespace chalkag
