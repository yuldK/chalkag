#include "capture/region_geometry.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace chalkag {
    namespace {
        int failure_count {};

        void check(bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failure_count;
            }
        }

        void check_region(capture_region actual, capture_region expected, const char* name)
        {
            check(actual == expected, name);

            if (actual != expected)
            {
                std::cerr << "  actual: " << actual.x << ',' << actual.y << ',' << actual.width << ',' << actual.height
                          << '\n';
            }
        }

        void test_drag()
        {
            const capture_region bounds { -1920, -200, 3840, 1280 };
            check_region(
                region_from_drag({ 200, 300 }, { -400, -100 }, bounds), { -400, -100, 601, 401 }, "reverse drag"
            );

            check_region(
                region_from_drag({ -3000, -500 }, { 2500, 2000 }, bounds), bounds, "drag clamped to virtual screen"
            );

            check_region(
                region_from_drag({ 10, 10 }, { 14, 15 }, bounds), { 10, 10, 5, 6 }, "small selection remains small"
            );
            check_region(
                region_from_drag({ 10, 10 }, { 10, 30 }, bounds), { 10, 10, 1, 21 }, "same column keeps one pixel"
            );
            check_region(region_from_drag({ 10, 10 }, { 10, 10 }, bounds), { 10, 10, 1, 1 }, "click is one pixel");

            // 시작 픽셀과 포인터 아래 픽셀을 모두 포함해 마지막 열·행까지 드래그로 닿는다.
            check_region(
                region_from_drag({ 1904, 1064 }, { 1919, 1079 }, bounds),
                { 1904, 1064, 16, 16 },
                "drag includes pixel under cursor"
            );
            check_region(
                region_from_drag({ 1904, 1064 }, { 5000, 5000 }, bounds),
                { 1904, 1064, 16, 16 },
                "last virtual screen column and row are reachable"
            );
            check_region(
                region_from_drag({ 1919, 1079 }, { 1904, 1064 }, bounds),
                { 1904, 1064, 16, 16 },
                "reverse drag includes start pixel"
            );

            check_region(region_from_drag({ 0, 0 }, { 30, 30 }, {}), {}, "invalid drag bounds");
        }

        void test_fit()
        {
            const capture_region bounds { -100, -50, 300, 200 };
            check_region(fit_region_to_screen({ 190, 140, 60, 40 }, bounds), { 140, 110, 60, 40 }, "fit position");
            check_region(fit_region_to_screen({ -500, -500, 1000, 1000 }, bounds), bounds, "fit oversized region");
            check_region(
                fit_region_to_screen({ 0, 0, 0, -100 }, bounds), { 0, 0, 16, 16 }, "repair missing dimensions"
            );

            check_region(fit_region_to_screen({}, { -4, -3, 8, 6 }), { -4, -3, 8, 6 }, "screen smaller than minimum");
            check_region(fit_region_to_screen({}, bounds, -5), { 0, 0, 1, 1 }, "nonpositive minimum");
            check_region(fit_region_to_screen({}, bounds, 400), bounds, "minimum larger than screen");
            check_region(fit_region_to_screen({ 0, 0, 30, 30 }, { 0, 0, -1, 10 }), {}, "invalid fit bounds");
        }

        void test_window_bounds()
        {
            const capture_region bounds { -1920, -200, 3840, 1280 };
            check_region(
                region_from_window_bounds({ 100, 50, 800, 600 }, bounds), { 100, 50, 800, 600 }, "window inside screen"
            );
            check_region(
                region_from_window_bounds({ -2000, -300, 500, 400 }, bounds),
                { -1920, -200, 420, 300 },
                "window clipped at screen start"
            );
            check_region(
                region_from_window_bounds({ 1800, 1000, 400, 300 }, bounds),
                { 1800, 1000, 120, 80 },
                "window clipped at screen end"
            );
            check_region(
                region_from_window_bounds({ -3000, -1000, 9000, 9000 }, bounds), bounds, "maximized beyond screen"
            );
            check_region(
                region_from_window_bounds({ 10, 10, 8, 4 }, bounds), { 10, 10, 16, 16 }, "tiny window grows to minimum"
            );
            check_region(
                region_from_window_bounds({ 1912, 1076, 30, 30 }, bounds),
                { 1904, 1064, 16, 16 },
                "tiny visible corner grows inside screen"
            );
            check(!region_from_window_bounds({ 3000, 10, 100, 100 }, bounds).valid(), "offscreen window is ignored");
            check(!region_from_window_bounds({ 10, 10, 0, 100 }, bounds).valid(), "empty window is ignored");
            check(!region_from_window_bounds({ 10, 10, 100, 100 }, {}).valid(), "invalid window screen");
        }

        void check_point(screen_point actual, screen_point expected, const char* name)
        {
            check(actual.x == expected.x && actual.y == expected.y, name);

            if (actual.x != expected.x || actual.y != expected.y)
            {
                std::cerr << "  actual: " << actual.x << ',' << actual.y << '\n';
            }
        }

        void test_magnifier_placement()
        {
            const capture_region monitor { 0, 0, 1920, 1080 };
            check_point(magnifier_origin({ 100, 100 }, 120, 160, 20, monitor), { 120, 120 }, "magnifier below right");
            check_point(
                magnifier_origin({ 1850, 100 }, 120, 160, 20, monitor), { 1710, 120 }, "magnifier flips left at edge"
            );
            check_point(
                magnifier_origin({ 100, 1000 }, 120, 160, 20, monitor), { 120, 820 }, "magnifier flips up at edge"
            );
            check_point(
                magnifier_origin({ 1919, 1079 }, 120, 160, 20, monitor), { 1779, 899 }, "magnifier flips at corner"
            );
            check_point(
                magnifier_origin({ -10, 500 }, 120, 160, 20, { -1920, 0, 1920, 1080 }),
                { -150, 520 },
                "magnifier flips on negative monitor"
            );
            check_point(
                magnifier_origin({ 50, 50 }, 120, 160, 20, { 0, 0, 100, 100 }), { 0, 0 }, "small monitor clamps start"
            );
            check_point(
                magnifier_origin({ 30, 30 }, 120, 160, 20, { 0, 0, 1920, 1080 }), { 50, 50 }, "offset avoids pointer"
            );
        }

        void test_adjust()
        {
            const capture_region bounds { -100, -50, 300, 200 };
            const capture_region region { -20, 10, 80, 60 };
            check_region(
                adjust_region(region, resize_edge::none, { 100, 100 }, bounds),
                region,
                "no edge leaves region unchanged"
            );

            check_region(
                adjust_region(region, resize_edge::move, { -1000, -1000 }, bounds),
                { -100, -50, 80, 60 },
                "move to screen start"
            );

            check_region(
                adjust_region(region, resize_edge::move, { 1000, 1000 }, bounds),
                { 120, 90, 80, 60 },
                "move to screen end"
            );

            check_region(
                adjust_region(region, resize_edge::left, { 1000, 40 }, bounds),
                { 44, 10, 16, 60 },
                "left minimum anchored right"
            );

            check_region(
                adjust_region(region, resize_edge::right, { -1000, 40 }, bounds),
                { -20, 10, 16, 60 },
                "right minimum anchored left"
            );

            check_region(
                adjust_region(region, resize_edge::top, { 40, 1000 }, bounds),
                { -20, 54, 80, 16 },
                "top minimum anchored bottom"
            );

            check_region(
                adjust_region(region, resize_edge::bottom, { 40, -1000 }, bounds),
                { -20, 10, 80, 16 },
                "bottom minimum anchored top"
            );

            check_region(
                adjust_region(region, resize_edge::top_left, { -1000, -1000 }, bounds),
                { -100, -50, 160, 120 },
                "top left expands to bounds"
            );

            check_region(
                adjust_region(region, resize_edge::bottom_right, { 1000, 1000 }, bounds),
                { -20, 10, 220, 140 },
                "bottom right expands to bounds"
            );

            check_region(
                adjust_region(region, resize_edge::top_right, { 20, -20 }, bounds),
                { -20, -10, 100, 80 },
                "top right keeps opposite corner"
            );

            check_region(
                adjust_region(region, resize_edge::bottom_left, { -20, 20 }, bounds),
                { -40, 10, 100, 80 },
                "bottom left keeps opposite corner"
            );

            check_region(
                adjust_region(region, resize_edge::left, { 1000, 0 }, bounds, 32),
                { 28, 10, 32, 60 },
                "custom resize minimum"
            );

            check_region(adjust_region(region, resize_edge::move, { 1, 1 }, {}), {}, "invalid adjust bounds");
        }

        void test_extreme_coordinates()
        {
            constexpr int low { std::numeric_limits<int>::min() };
            constexpr int high { std::numeric_limits<int>::max() };
            const capture_region negative_bounds { low, low, high, high };
            check_region(
                region_from_drag({ high, high }, { low, low }, negative_bounds),
                negative_bounds,
                "extreme drag difference"
            );

            check_region(
                adjust_region({ low, low, 16, 16 }, resize_edge::move, { high, high }, negative_bounds),
                { -17, -17, 16, 16 },
                "extreme positive move clamp"
            );

            const capture_region positive_bounds { high - 100, high - 100, 200, 200 };
            check_region(
                fit_region_to_screen({ high, high, 150, 150 }, positive_bounds),
                { high - 50, high - 50, 150, 150 },
                "bounds end exceeds int maximum"
            );

            check_region(
                adjust_region({ high - 100, high - 100, 20, 20 }, resize_edge::move, { high, high }, positive_bounds),
                { high, high, 20, 20 },
                "position stays representable"
            );

            check_region(
                adjust_region(
                    { high - 100, high - 100, 20, 20 }, resize_edge::bottom_right, { high, high }, positive_bounds
                ),
                positive_bounds,
                "extreme resize addition"
            );

            check_region(
                adjust_region({ -100, -100, 20, 20 }, resize_edge::top_left, { low, low }, { -200, -200, 400, 400 }),
                { -200, -200, 120, 120 },
                "extreme negative resize addition"
            );
        }

    } // namespace
} // namespace chalkag

int main()
{
    chalkag::test_drag();
    chalkag::test_window_bounds();
    chalkag::test_magnifier_placement();
    chalkag::test_fit();
    chalkag::test_adjust();
    chalkag::test_extreme_coordinates();

    if (chalkag::failure_count != 0)
    {
        return EXIT_FAILURE;
    }

    std::cout << "Region geometry tests passed.\n";
    return EXIT_SUCCESS;
}
