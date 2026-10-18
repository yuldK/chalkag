#pragma once

#include "include/core/SkPixmap.h"

#include <cstdint>
#include <vector>

namespace chalkag {
    struct frame_change
    {
        int x {};
        int y {};
        int width {};
        int height {};

        [[nodiscard]] bool changed() const noexcept
        {
            return width > 0 && height > 0;
        }
    };

    class frame_optimizer
    {
    public:
        [[nodiscard]] frame_change inspect(const SkPixmap& pixels);

    private:
        std::vector<std::uint8_t> previous_pixels_ {};
        int width_ {};
        int height_ {};
    };
} // namespace chalkag
