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
                region_from_drag({ 200, 300 }, { -400, -100 }, bounds), { -400, -100, 600, 400 }, "reverse drag"
            );

            check_region(
                region_from_drag({ -3000, -500 }, { 2500, 2000 }, bounds), bounds, "drag clamped to virtual screen"
            );

            check_region(
                region_from_drag({ 10, 10 }, { 14, 15 }, bounds), { 10, 10, 4, 5 }, "small selection remains small"
            );
            check(!region_from_drag({ 10, 10 }, { 10, 30 }, bounds).valid(), "zero width is invalid");

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
