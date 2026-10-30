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

    // 창의 보이는 경계를 가상 화면으로 자르고 최소 크기를 맞춘다. 화면과 겹치지 않으면 빈 영역이다.
    [[nodiscard]] capture_region region_from_window_bounds(
        capture_region window, capture_region screen_bounds, int minimum_size = minimum_region_size
    ) noexcept;

    // 돋보기는 포인터 오른쪽 아래에 두고 모니터 끝에서는 반대쪽으로 뒤집어 선택 지점을 가리지 않는다.
    [[nodiscard]] screen_point magnifier_origin(
        screen_point pointer, int width, int height, int offset, capture_region monitor
    ) noexcept;
} // namespace chalkag
