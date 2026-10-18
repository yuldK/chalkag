#pragma once

namespace chalkag {
    struct screen_point
    {
        int x {};
        int y {};
    };

    struct capture_region
    {
        int x {};
        int y {};
        int width {};
        int height {};

        [[nodiscard]] bool valid() const noexcept;
        bool operator==(const capture_region&) const = default;
    };

    enum class resize_edge
    {
        none,
        move,
        left,
        right,
        top,
        bottom,
        top_left,
        top_right,
        bottom_left,
        bottom_right,
    };

    inline constexpr int minimum_region_size { 16 };

    [[nodiscard]] capture_region region_from_drag(
        screen_point origin, screen_point current, capture_region screen_bounds
    ) noexcept;
    [[nodiscard]] capture_region adjust_region(
        capture_region original,
        resize_edge edge,
        screen_point delta,
        capture_region screen_bounds,
        int minimum_size = minimum_region_size
    ) noexcept;
    [[nodiscard]] capture_region fit_region_to_screen(
        capture_region region, capture_region bounds, int minimum_size = minimum_region_size
    ) noexcept;
} // namespace chalkag
