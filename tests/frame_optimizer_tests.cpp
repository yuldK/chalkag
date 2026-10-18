#include "capture/frame_optimizer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace chalkag {
    namespace {
        int failure_count {};

        void check(const bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failure_count;
            }
        }

        void check_change(const frame_change actual, const frame_change expected, const char* name)
        {
            check(
                actual.x == expected.x && actual.y == expected.y && actual.width == expected.width
                    && actual.height == expected.height && actual.changed() == expected.changed(),
                name
            );
        }

        struct test_image
        {
            int width {};
            int height {};

            std::size_t stride {};
            std::vector<std::uint8_t> bytes {};

            test_image(const int image_width, const int image_height, const std::size_t padding = 0)
                : width { image_width }
                , height { image_height }
                , stride { static_cast<std::size_t>(image_width) * 4 + padding }
                , bytes(stride * static_cast<std::size_t>(image_height), 0)
            {
                for (int y {}; y < height; ++y)
                {
                    for (int x {}; x < width; ++x)
                    {
                        set_pixel(x, y, { 20, 40, 60, 255 });
                    }
                }
            }

            [[nodiscard]] SkPixmap pixmap() const
            {
                return { SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
                         bytes.data(),
                         stride
                };
            }

            void set_pixel(const int x, const int y, const std::array<std::uint8_t, 4> color)
            {
                const auto offset { static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4 };
                std::copy(color.begin(), color.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
            }

            void fill_rectangle(
                const int x,
                const int y,
                const int rectangle_width,
                const int rectangle_height,
                const std::array<std::uint8_t, 4> color
            )
            {
                for (int row { y }; row < y + rectangle_height; ++row)
                {
                    for (int column { x }; column < x + rectangle_width; ++column)
                    {
                        set_pixel(column, row, color);
                    }
                }
            }
        };

        void expect_invalid(frame_optimizer& optimizer, const SkPixmap& pixels, const char* name)
        {
            try
            {
                const auto ignored { optimizer.inspect(pixels) };
                static_cast<void>(ignored);
                check(false, name);
            }
            catch (const std::invalid_argument&)
            {}
        }

        void test_changes()
        {
            frame_optimizer optimizer {};
            test_image image { 9, 7 };
            check_change(optimizer.inspect(image.pixmap()), { 0, 0, 9, 7 }, "first frame covers the full canvas");
            check_change(optimizer.inspect(image.pixmap()), {}, "identical full frame is unchanged");
            image.set_pixel(5, 3, { 1, 2, 3, 255 });
            check_change(
                optimizer.inspect(image.pixmap()), { 4, 2, 2, 2 }, "one odd-position pixel expands only left and top"
            );
            check_change(optimizer.inspect(image.pixmap()), {}, "changed frame becomes the new comparison reference");
            image.set_pixel(8, 6, { 4, 5, 6, 255 });
            check_change(
                optimizer.inspect(image.pixmap()), { 8, 6, 1, 1 }, "bottom-right pixel stays inside odd-sized canvas"
            );
            image.set_pixel(0, 0, { 7, 8, 9, 255 });
            image.set_pixel(8, 6, { 10, 11, 12, 255 });
            check_change(
                optimizer.inspect(image.pixmap()), { 0, 0, 9, 7 }, "distant changes produce a containing full rectangle"
            );
            image.set_pixel(2, 4, { 20, 40, 60, 254 });
            check_change(
                optimizer.inspect(image.pixmap()), { 2, 4, 1, 1 }, "pixel comparison includes the fourth byte exactly"
            );

            frame_optimizer moving_optimizer {};
            test_image moving_image { 20, 14 };
            moving_image.fill_rectangle(3, 3, 3, 2, { 200, 100, 50, 255 });
            check_change(
                moving_optimizer.inspect(moving_image.pixmap()),
                { 0, 0, 20, 14 },
                "initial moving rectangle frame covers canvas"
            );
            moving_image.fill_rectangle(3, 3, 3, 2, { 20, 40, 60, 255 });
            moving_image.fill_rectangle(11, 9, 3, 2, { 200, 100, 50, 255 });
            check_change(
                moving_optimizer.inspect(moving_image.pixmap()),
                { 2, 2, 12, 9 },
                "moving rectangle includes old erased and new painted pixels"
            );
            check_change(
                moving_optimizer.inspect(moving_image.pixmap()), {}, "moving rectangle reference is retained exactly"
            );

            test_image single_pixel { 1, 1 };
            check_change(
                optimizer.inspect(single_pixel.pixmap()), { 0, 0, 1, 1 }, "dimension change resets to full canvas"
            );
            check_change(optimizer.inspect(single_pixel.pixmap()), {}, "one-pixel canvas compares unchanged");
            single_pixel.set_pixel(0, 0, { 0, 0, 0, 255 });
            check_change(optimizer.inspect(single_pixel.pixmap()), { 0, 0, 1, 1 }, "one-pixel canvas detects change");
            check_change(optimizer.inspect(image.pixmap()), { 0, 0, 9, 7 }, "restoring dimensions still resets canvas");
        }

        void test_stride()
        {
            frame_optimizer optimizer {};
            test_image padded { 7, 5, 13 };
            check_change(optimizer.inspect(padded.pixmap()), { 0, 0, 7, 5 }, "padded first frame covers canvas");

            for (int y {}; y < padded.height; ++y)
            {
                const auto padding_start { static_cast<std::size_t>(y) * padded.stride
                                           + static_cast<std::size_t>(padded.width) * 4
                };

                std::fill_n(
                    padded.bytes.begin() + static_cast<std::ptrdiff_t>(padding_start), 13, std::uint8_t { 255 }
                );
            }

            check_change(optimizer.inspect(padded.pixmap()), {}, "row padding changes do not count as changed pixels");
            padded.set_pixel(5, 3, { 1, 2, 3, 255 });
            check_change(
                optimizer.inspect(padded.pixmap()), { 4, 2, 2, 2 }, "unaligned padded rows produce exact change bounds"
            );
            test_image packed { 7, 5 };
            packed.set_pixel(5, 3, { 1, 2, 3, 255 });
            check_change(
                optimizer.inspect(packed.pixmap()),
                {},
                "changing row stride retains the same pixel comparison reference"
            );
        }

        void test_invalid_inputs()
        {
            frame_optimizer optimizer {};
            test_image image { 8, 6 };
            check_change(
                optimizer.inspect(image.pixmap()),
                { 0, 0, 8, 6 },
                "valid reference is initialized before invalid inputs"
            );

            const auto info { SkImageInfo::Make(8, 6, kBGRA_8888_SkColorType, kOpaque_SkAlphaType) };
            expect_invalid(optimizer, {}, "empty pixmap is rejected");
            expect_invalid(optimizer, { info, nullptr, image.stride }, "null pixels are rejected");
            expect_invalid(
                optimizer,
                { SkImageInfo::Make(0, 6, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
                  image.bytes.data(),
                  image.stride
                },
                "zero width is rejected"
            );
            expect_invalid(
                optimizer,
                { SkImageInfo::Make(8, 0, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
                  image.bytes.data(),
                  image.stride
                },
                "zero height is rejected"
            );
            expect_invalid(
                optimizer,
                { SkImageInfo::Make(-1, 6, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
                  image.bytes.data(),
                  image.stride
                },
                "negative width is rejected"
            );
            expect_invalid(
                optimizer,
                { SkImageInfo::Make(8, 6, kRGBA_8888_SkColorType, kOpaque_SkAlphaType),
                  image.bytes.data(),
                  image.stride
                },
                "RGBA format is rejected"
            );
            expect_invalid(
                optimizer,
                { SkImageInfo::Make(8, 6, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
                  image.bytes.data(),
                  image.stride
                },
                "nonopaque alpha type is rejected"
            );
            expect_invalid(optimizer, { info, image.bytes.data(), 31 }, "short row stride is rejected");
            expect_invalid(
                optimizer,
                { info, image.bytes.data(), std::numeric_limits<std::size_t>::max() },
                "overflowing row stride is rejected before reading pixels"
            );

            const auto oversized_info { SkImageInfo::Make(
                std::numeric_limits<int>::max(),
                std::numeric_limits<int>::max(),
                kBGRA_8888_SkColorType,
                kOpaque_SkAlphaType
            )
            };
            expect_invalid(
                optimizer,
                { oversized_info, image.bytes.data(), static_cast<std::size_t>(std::numeric_limits<int>::max()) * 4 },
                "overflowing packed dimensions are rejected before allocation"
            );
            check_change(optimizer.inspect(image.pixmap()), {}, "invalid input preserves the last valid reference");
        }

        void measure_frames(const int width, const int height)
        {
            constexpr int frame_count { 120 };
            test_image image { width, height };
            frame_optimizer optimizer {};
            check(optimizer.inspect(image.pixmap()).changed(), "timing reference starts with a full frame");

            const auto static_start { std::chrono::steady_clock::now() };

            for (int frame {}; frame < frame_count; ++frame)
            {
                check(!optimizer.inspect(image.pixmap()).changed(), "timed static frame stays unchanged");
            }

            const auto static_end { std::chrono::steady_clock::now() };
            const int x { width / 2 };
            const int y { height / 2 };

            for (int frame {}; frame < frame_count; ++frame)
            {
                image.fill_rectangle(x, y, 16, 16, { static_cast<std::uint8_t>(frame), 0, 0, 255 });
                check_change(
                    optimizer.inspect(image.pixmap()),
                    { x & ~1, y & ~1, 16 + (x & 1), 16 + (y & 1) },
                    "timed small change has correct bounds"
                );
            }

            const auto changed_end { std::chrono::steady_clock::now() };
            const std::chrono::duration<double, std::milli> static_elapsed { static_end - static_start };
            const std::chrono::duration<double, std::milli> changed_elapsed { changed_end - static_end };
            std::cout << width << 'x' << height << ": static " << static_elapsed.count() / frame_count
                      << " ms/frame, 16x16 change " << changed_elapsed.count() / frame_count << " ms/frame\n";
        }

        int run_tests()
        {
            test_changes();
            test_stride();
            test_invalid_inputs();
            measure_frames(640, 360);
            measure_frames(1920, 1080);
            return failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

    } // namespace
} // namespace chalkag

int main()
{
    try
    {
        const int result { chalkag::run_tests() };

        if (result == EXIT_SUCCESS)
        {
            std::cout << "Frame optimizer tests passed.\n";
        }

        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
