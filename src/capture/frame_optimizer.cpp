#include "capture/frame_optimizer.h"

#include "include/core/SkPixmap.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace chalkag {
    frame_change frame_optimizer::inspect(const SkPixmap& pixels)
    {
        const int width { pixels.width() };
        const int height { pixels.height() };

        if (width <= 0 || height <= 0 || pixels.colorType() != kBGRA_8888_SkColorType
            || pixels.alphaType() != kOpaque_SkAlphaType || pixels.addr() == nullptr)
        {
            throw std::invalid_argument { "Frame optimizer requires opaque BGRA8888 pixels with positive dimensions." };
        }

        const auto maximum_size { static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) };
        const auto row_count { static_cast<std::size_t>(height) };
        const auto column_count { static_cast<std::size_t>(width) };

        if (column_count > maximum_size / 4)
        {
            throw std::invalid_argument { "Frame optimizer dimensions overflow the pixel buffer." };
        }

        const std::size_t row_size { column_count * 4 };
        const std::size_t stride { pixels.rowBytes() };

        if (stride < row_size || row_count > maximum_size / row_size
            || (row_count > 1 && stride > (maximum_size - row_size) / (row_count - 1)))
        {
            throw std::invalid_argument { "Frame optimizer row stride is invalid or overflows the pixel buffer." };
        }

        const auto* current_pixels { static_cast<const std::uint8_t*>(pixels.addr()) };

        if (width != width_ || height != height_)
        {
            previous_pixels_.resize(row_size * row_count);

            for (std::size_t y {}; y < row_count; ++y)
            {
                std::memcpy(previous_pixels_.data() + y * row_size, current_pixels + y * stride, row_size);
            }

            width_ = width;
            height_ = height;
            return { 0, 0, width, height };
        }

        // 같은 행은 건너뛰고 달라진 행의 좌우 끝만 찾아 최소 변경 사각형을 누적한다.
        int left { width };
        int top { height };
        int right {};
        int bottom {};

        for (int y {}; y < height; ++y)
        {
            const auto row_index { static_cast<std::size_t>(y) };
            const auto* current_row { current_pixels + row_index * stride };
            auto* previous_row { previous_pixels_.data() + row_index * row_size };

            if (std::memcmp(current_row, previous_row, row_size) == 0)
            {
                continue;
            }

            int row_left {};

            while (std::memcmp(
                       current_row + static_cast<std::size_t>(row_left) * 4,
                       previous_row + static_cast<std::size_t>(row_left) * 4,
                       4
                   )
                   == 0)
            {
                ++row_left;
            }

            int row_right { width };

            while (std::memcmp(
                       current_row + static_cast<std::size_t>(row_right - 1) * 4,
                       previous_row + static_cast<std::size_t>(row_right - 1) * 4,
                       4
                   )
                   == 0)
            {
                --row_right;
            }

            left = std::min(left, row_left);
            top = std::min(top, y);
            right = std::max(right, row_right);
            bottom = y + 1;

            std::memcpy(previous_row, current_row, row_size);
        }

        if (bottom == 0)
        {
            return {};
        }

        // WebP ANMF의 위치 단위가 2픽셀이므로 좌상단만 확장한다.
        left &= ~1;
        top &= ~1;
        return { left, top, right - left, bottom - top };
    }

} // namespace chalkag
