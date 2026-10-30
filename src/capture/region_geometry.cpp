#include "region_geometry.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace chalkag {
    namespace {
        using coordinate = std::int64_t;

        constexpr coordinate maximum_extent { std::numeric_limits<int>::max() };

        struct axis_region
        {
            coordinate position;
            coordinate extent;
        };

        axis_region fit_axis(
            int position, int extent, int bounds_position, int bounds_extent, int minimum_size
        ) noexcept
        {
            const coordinate minimum { std::clamp(minimum_size, 1, bounds_extent) };
            const coordinate size { std::clamp<coordinate>(extent, minimum, bounds_extent) };
            const coordinate last_position {
                std::min(maximum_extent, coordinate { bounds_position } + bounds_extent - size)
            };
            return { std::clamp<coordinate>(position, bounds_position, last_position), size };
        }

        // 시작 픽셀과 포인터 아래 픽셀을 모두 포함해야 오른쪽·아래 끝 픽셀까지 드래그로 고를 수 있다.
        axis_region drag_axis(int origin, int current, int bounds_position, int bounds_extent) noexcept
        {
            const coordinate last { coordinate { bounds_position } + bounds_extent - 1 };
            const coordinate first { std::clamp<coordinate>(origin, bounds_position, last) };
            const coordinate second { std::clamp<coordinate>(current, bounds_position, last) };
            return { std::min(first, second), std::max(first, second) - std::min(first, second) + 1 };
        }

        // 기본 방향에 공간이 없으면 포인터 반대쪽으로 뒤집고 그래도 넘치면 모니터 안으로 민다.
        coordinate place_axis(int pointer, int extent, int offset, int bounds_position, int bounds_extent) noexcept
        {
            const coordinate bounds_end { coordinate { bounds_position } + bounds_extent };
            coordinate position { coordinate { pointer } + offset };

            if (position + extent > bounds_end)
            {
                position = coordinate { pointer } - offset - extent;
            }

            const coordinate last_position { std::max<coordinate>(bounds_position, bounds_end - extent) };
            return std::clamp<coordinate>(position, bounds_position, last_position);
        }

        axis_region adjust_axis(
            axis_region original,
            int delta,
            int bounds_position,
            int bounds_extent,
            int minimum_size,
            bool move,
            bool resize_start,
            bool resize_end
        ) noexcept
        {
            const coordinate bounds_end { coordinate { bounds_position } + bounds_extent };
            const coordinate minimum { std::clamp(minimum_size, 1, bounds_extent) };
            const coordinate end { original.position + original.extent };

            if (move)
            {
                const coordinate last_position { std::min(maximum_extent, bounds_end - original.extent) };
                return { std::clamp(original.position + delta, coordinate { bounds_position }, last_position),
                         original.extent
                };
            }

            if (resize_start)
            {
                const coordinate first_position { std::max(coordinate { bounds_position }, end - maximum_extent) };
                const coordinate last_position { std::min(maximum_extent, end - minimum) };
                const coordinate position { std::clamp(original.position + delta, first_position, last_position) };
                return { position, end - position };
            }

            if (resize_end)
            {
                const coordinate last_end { std::min(bounds_end, original.position + maximum_extent) };
                const coordinate new_end { std::clamp(end + delta, original.position + minimum, last_end) };
                return { original.position, new_end - original.position };
            }

            return original;
        }

        capture_region combine_axes(axis_region horizontal, axis_region vertical) noexcept
        {
            return { static_cast<int>(horizontal.position),
                     static_cast<int>(vertical.position),
                     static_cast<int>(horizontal.extent),
                     static_cast<int>(vertical.extent)
            };
        }

    } // namespace

    bool capture_region::valid() const noexcept
    {
        return width > 0 && height > 0;
    }

    capture_region region_from_drag(screen_point origin, screen_point current, capture_region screen_bounds) noexcept
    {
        if (!screen_bounds.valid())
        {
            return {};
        }

        return combine_axes(
            drag_axis(origin.x, current.x, screen_bounds.x, screen_bounds.width),
            drag_axis(origin.y, current.y, screen_bounds.y, screen_bounds.height)
        );
    }

    capture_region region_from_window_bounds(
        capture_region window, capture_region screen_bounds, int minimum_size
    ) noexcept
    {
        if (!window.valid() || !screen_bounds.valid())
        {
            return {};
        }

        const coordinate left { std::max(window.x, screen_bounds.x) };
        const coordinate top { std::max(window.y, screen_bounds.y) };
        const coordinate right {
            std::min(coordinate { window.x } + window.width, coordinate { screen_bounds.x } + screen_bounds.width)
        };
        const coordinate bottom {
            std::min(coordinate { window.y } + window.height, coordinate { screen_bounds.y } + screen_bounds.height)
        };

        // 화면 밖에만 있는 창은 고를 대상이 아니며 작은 창은 최소 크기로 넓힌다.
        if (right <= left || bottom <= top)
        {
            return {};
        }

        return fit_region_to_screen(
            { static_cast<int>(left),
              static_cast<int>(top),
              static_cast<int>(right - left),
              static_cast<int>(bottom - top)
            },
            screen_bounds,
            minimum_size
        );
    }

    screen_point magnifier_origin(
        screen_point pointer, int width, int height, int offset, capture_region monitor
    ) noexcept
    {
        return { static_cast<int>(place_axis(pointer.x, width, offset, monitor.x, monitor.width)),
                 static_cast<int>(place_axis(pointer.y, height, offset, monitor.y, monitor.height))
        };
    }

    capture_region fit_region_to_screen(capture_region region, capture_region bounds, int minimum_size) noexcept
    {
        if (!bounds.valid())
        {
            return {};
        }

        return combine_axes(
            fit_axis(region.x, region.width, bounds.x, bounds.width, minimum_size),
            fit_axis(region.y, region.height, bounds.y, bounds.height, minimum_size)
        );
    }

    capture_region adjust_region(
        capture_region original, resize_edge edge, screen_point delta, capture_region screen_bounds, int minimum_size
    ) noexcept
    {
        original = fit_region_to_screen(original, screen_bounds, minimum_size);

        if (!original.valid())
        {
            return {};
        }

        const bool move { edge == resize_edge::move };
        const bool left { edge == resize_edge::left || edge == resize_edge::top_left
                          || edge == resize_edge::bottom_left
        };

        const bool right { edge == resize_edge::right || edge == resize_edge::top_right
                           || edge == resize_edge::bottom_right
        };

        const bool top { edge == resize_edge::top || edge == resize_edge::top_left || edge == resize_edge::top_right };
        const bool bottom { edge == resize_edge::bottom || edge == resize_edge::bottom_left
                            || edge == resize_edge::bottom_right
        };

        return combine_axes(
            adjust_axis(
                { original.x, original.width },
                delta.x,
                screen_bounds.x,
                screen_bounds.width,
                minimum_size,
                move,
                left,
                right
            ),
            adjust_axis(
                { original.y, original.height },
                delta.y,
                screen_bounds.y,
                screen_bounds.height,
                minimum_size,
                move,
                top,
                bottom
            )
        );
    }

} // namespace chalkag
