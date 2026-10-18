#include "capture/native_webp_writer.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <thread>
#include <vector>

namespace chalkag {
    namespace {
        using pixel_buffer = std::vector<std::uint32_t>;
        int failures {};

        void check(const bool condition, const char* description)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << description << '\n';
                ++failures;
            }
        }

        struct test_files
        {
            std::filesystem::path directory;

            ~test_files()
            {
                std::error_code ignored {};
                std::filesystem::remove_all(directory, ignored);
            }
        };

        std::vector<char> bytes_for(const std::filesystem::path& path)
        {
            std::ifstream file { path, std::ios::binary };
            return { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
        }

        std::unique_ptr<SkCodec> codec_for(const std::filesystem::path& path)
        {
            const auto bytes { bytes_for(path) };
            return SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size()));
        }

        bool has_icc(const std::filesystem::path& path)
        {
            const auto bytes { bytes_for(path) };
            std::size_t position { 12 };

            while (position + 8 <= bytes.size())
            {
                if (std::string_view { bytes.data() + position, 4 } == "ICCP")
                {
                    return true;
                }

                std::uint32_t length {};
                std::memcpy(&length, bytes.data() + position + 4, sizeof(length));

                if (length > bytes.size() - position - 8)
                {
                    return false;
                }

                position += 8 + static_cast<std::size_t>(length) + (length & 1);
            }

            return false;
        }

        SkPixmap pixmap(
            const pixel_buffer& pixels,
            const int width,
            const int height,
            const SkAlphaType alpha = kOpaque_SkAlphaType,
            sk_sp<SkColorSpace> color_space = nullptr
        )
        {
            return { SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, alpha, std::move(color_space)),
                     pixels.data(),
                     static_cast<std::size_t>(width) * 4
            };
        }

        pixel_buffer decode(SkCodec& codec, const int frame_index = 0)
        {
            const auto info {
                codec.getInfo().makeColorType(kBGRA_8888_SkColorType).makeAlphaType(kUnpremul_SkAlphaType)
            };
            pixel_buffer pixels(static_cast<std::size_t>(info.width()) * info.height());

            SkCodec::Options options {};
            options.fFrameIndex = frame_index;
            check(
                codec.getPixels(info, pixels.data(), info.minRowBytes(), &options) == SkCodec::kSuccess,
                "WebP pixels decode completely"
            );
            return pixels;
        }

        bool matches_icc_precision(const SkColorSpace* actual, const SkColorSpace* expected)
        {
            if (SkColorSpace::Equals(actual, expected))
            {
                return true;
            }

            if (actual == nullptr || expected == nullptr)
            {
                return false;
            }

            skcms_Matrix3x3 expected_matrix {};
            skcms_Matrix3x3 actual_matrix {};
            skcms_TransferFunction expected_function {};
            skcms_TransferFunction actual_function {};

            if (!expected->toXYZD50(&expected_matrix) || !actual->toXYZD50(&actual_matrix)
                || !expected->isNumericalTransferFn(&expected_function)
                || !actual->isNumericalTransferFn(&actual_function))
            {
                return false;
            }

            // ICC의 s15Fixed16 직렬화 한 단위만 원래 부동소수점 값과 다를 수 있다.
            constexpr float precision { 1.0f / 65536.0f };

            for (std::size_t row {}; row < 3; ++row)
            {
                for (std::size_t column {}; column < 3; ++column)
                {
                    if (std::abs(expected_matrix.vals[row][column] - actual_matrix.vals[row][column]) > precision)
                    {
                        return false;
                    }
                }
            }

            const std::array expected_curve { expected_function.g, expected_function.a, expected_function.b,
                                              expected_function.c, expected_function.d, expected_function.e,
                                              expected_function.f
            };

            const std::array actual_curve { actual_function.g, actual_function.a, actual_function.b, actual_function.c,
                                            actual_function.d, actual_function.e, actual_function.f
            };

            for (std::size_t index {}; index < expected_curve.size(); ++index)
            {
                if (std::abs(expected_curve[index] - actual_curve[index]) > precision)
                {
                    return false;
                }
            }

            return true;
        }

        void verify_pixels(
            const std::filesystem::path& path,
            const pixel_buffer& expected,
            const int width,
            const int height,
            const SkColorSpace* color_space = nullptr
        )
        {
            auto codec { codec_for(path) };
            check(codec != nullptr, "encoded file is recognized as WebP");

            if (!codec)
            {
                return;
            }

            check(
                codec->getInfo().width() == width && codec->getInfo().height() == height,
                "encoded dimensions match exactly"
            );

            const auto default_space { SkColorSpace::MakeSRGB() };
            const auto expected_space { color_space != nullptr ? color_space : default_space.get() };
            const auto decoded_info { codec->getInfo() };
            const auto actual_space { decoded_info.colorSpace() };
            check(
                matches_icc_precision(actual_space, expected_space),
                "source effective color space matches ICC fixed point precision"
            );
            check(has_icc(path) == (color_space != nullptr), "explicit ICC presence and untagged source are preserved");
            check(decode(*codec) == expected, "all decoded RGB and alpha bytes match exactly");
        }

        pixel_buffer sample(const int width, const int height, const int kind)
        {
            pixel_buffer pixels(static_cast<std::size_t>(width) * height);

            std::uint32_t seed { 0x82a915dbU };

            for (int y {}; y < height; ++y)
            {
                for (int x {}; x < width; ++x)
                {
                    seed ^= seed << 13;
                    seed ^= seed >> 17;
                    seed ^= seed << 5;

                    std::uint32_t color {};

                    if (kind == 0)
                    {
                        const bool glyph { y % 28 >= 7 && y % 28 < 20 && x % 11 < 7
                                           && ((x / 11 + y / 28) % 3 == 0 || (x % 11 + y % 28) % 4 == 0)
                        };
                        color = glyph ? 0x00314659U : y % 28 == 0 ? 0x00cad4dfU : 0x00f1f4f8U;
                    }
                    else if (kind == 1)
                    {
                        color = static_cast<std::uint32_t>(
                            (x * 255 / width) << 16 | (y * 255 / height) << 8 | (x + y) * 255 / (width + height)
                        );
                    }
                    else if (kind == 2)
                    {
                        const auto red { static_cast<unsigned>(110 + 70 * std::sin(x * 0.027) * std::cos(y * 0.018)) };
                        const auto green { static_cast<unsigned>(100 + 60 * std::sin((x + y) * 0.012)) };
                        const auto blue { static_cast<unsigned>(100 + 60 * std::cos((x - y) * 0.021)) };
                        color = (red << 16) | (green << 8) | blue;
                        color ^= seed & 0x00030303U;
                    }
                    else
                    {
                        color = seed & 0x00ffffffU;
                    }

                    pixels[static_cast<std::size_t>(y) * width + x] = 0xff000000U | color;
                }
            }

            return pixels;
        }

        std::size_t file_count(const std::filesystem::path& directory)
        {
            std::size_t count {};

            for (const auto& entry : std::filesystem::directory_iterator { directory })
            {
                if (entry.is_regular_file())
                {
                    ++count;
                }
            }

            return count;
        }

        void run_still(const std::filesystem::path& directory)
        {
            const auto pixels { sample(128, 72, 0) };

            for (const auto compression :
                 { lossless_compression::fast, lossless_compression::compact, lossless_compression::maximum })
            {
                const auto output {
                    directory / (compression == lossless_compression::fast ? L"still-fast.webp" : L"still-small.webp")
                };
                check(
                    encode_native_image(output, pixmap(pixels, 128, 72), image_format::webp, compression),
                    "both compression strengths encode lossless opaque pixels"
                );
                verify_pixels(output, pixels, 128, 72);
            }

            auto transparent { sample(96, 64, 1) };

            for (std::size_t index {}; index < transparent.size(); ++index)
            {
                const auto alpha { static_cast<std::uint32_t>(index % 256) };
                transparent[index] = alpha == 0 ? 0U : (transparent[index] & 0x00ffffffU) | (alpha << 24);
            }

            const auto color_space { SkColorSpace::MakeRGB(SkNamedTransferFn::kSRGB, SkNamedGamut::kDisplayP3) };
            const auto different_gamut { SkColorSpace::MakeSRGB() };
            const auto different_curve { color_space->makeLinearGamma() };
            check(
                !matches_icc_precision(color_space.get(), different_gamut.get())
                    && !matches_icc_precision(color_space.get(), different_curve.get()),
                "ICC precision comparison rejects changed gamut and transfer curve"
            );

            for (const auto compression :
                 { lossless_compression::fast, lossless_compression::compact, lossless_compression::maximum })
            {
                const auto output {
                    directory / (compression == lossless_compression::fast ? L"alpha-fast.webp" : L"alpha-small.webp")
                };
                check(
                    encode_native_image(
                        output,
                        pixmap(transparent, 96, 64, kUnpremul_SkAlphaType, color_space),
                        image_format::webp,
                        compression
                    ),
                    "both strengths encode alpha and Display P3 profile"
                );
                verify_pixels(output, transparent, 96, 64, color_space.get());

                const auto baseline_codec { codec_for(output) };
                const auto baseline_info { baseline_codec->getInfo() };
                const auto before { std::filesystem::file_size(output) };
                const std::array frames { encoded_animation_frame { output, 0.1, 2, 4 },
                                          encoded_animation_frame { output, 0.2, 6, 8 }
                };

                std::atomic<bool> cancelled {};
                std::u8string error { u8"stale" };
                check(
                    optimize_webp_frames(frames, cancelled, error) && error.empty(),
                    "profile and alpha source optimizes with repeated path"
                );
                check(std::filesystem::file_size(output) <= before, "post compression never increases alpha file size");

                const auto optimized_codec { codec_for(output) };
                const auto optimized_info { optimized_codec->getInfo() };
                check(
                    SkColorSpace::Equals(baseline_info.colorSpace(), optimized_info.colorSpace()),
                    "post compression preserves decoded baseline color space exactly"
                );
                verify_pixels(output, transparent, 96, 64, color_space.get());
            }
        }

        void run_animation(const std::filesystem::path& directory)
        {
            constexpr int width { 80 };
            constexpr int height { 48 };
            const auto base { sample(width, height, 0) };
            const pixel_buffer tile(12 * 10, 0xff23b781U);
            const auto full_path { directory / L"animation-full.webp" };
            const auto tile_path { directory / L"animation-tile.webp" };
            check(
                encode_native_image(full_path, pixmap(base, width, height), image_format::webp),
                "default strength remains fast for recorded full frame"
            );
            check(
                encode_native_image(tile_path, pixmap(tile, 12, 10), image_format::webp),
                "default strength records changed tile"
            );

            const std::array frames { encoded_animation_frame { full_path, 0.063, 0, 0 },
                                      encoded_animation_frame { tile_path, 0.062, 4, 6 },
                                      encoded_animation_frame { tile_path, 0.125, 20, 22 }
            };

            const auto original_size { std::filesystem::file_size(full_path) + std::filesystem::file_size(tile_path) };
            const auto count { file_count(directory) };
            std::atomic<bool> cancelled {};
            std::u8string error {};
            check(optimize_webp_frames(frames, cancelled, error), "tiles and repeated path optimize successfully");
            check(
                std::filesystem::file_size(full_path) + std::filesystem::file_size(tile_path) <= original_size,
                "combined tile bytes cannot increase"
            );
            check(file_count(directory) == count, "successful optimization leaves no temporary file");
            check(
                frames[1].path == tile_path && frames[1].x == 4 && frames[1].y == 6
                    && frames[1].duration_seconds == 0.062 && frames[2].path == tile_path && frames[2].x == 20
                    && frames[2].y == 22 && frames[2].duration_seconds == 0.125,
                "optimizer preserves repeated path, tile positions and times"
            );

            const auto animation { directory / L"optimized-animation.webp" };
            native_webp_writer writer { animation, width, height };

            for (const auto& frame : frames)
            {
                check(
                    writer.append_frame(
                        frame.path,
                        static_cast<std::uint32_t>(std::llround(frame.duration_seconds * 1000)),
                        frame.x,
                        frame.y
                    ),
                    "optimized tile appends to animation"
                );
            }

            check(writer.finish(), "optimized delta animation finishes");

            auto codec { codec_for(animation) };
            check(codec != nullptr && codec->getFrameCount() == 3, "optimized animation retains three frames");

            if (!codec)
            {
                return;
            }

            const auto metadata { codec->getFrameInfo() };
            auto expected { base };

            for (std::size_t index {}; index < frames.size(); ++index)
            {
                if (index != 0)
                {
                    for (int y {}; y < 10; ++y)
                    {
                        for (int x {}; x < 12; ++x)
                        {
                            expected[static_cast<std::size_t>(frames[index].y + y) * width + frames[index].x + x]
                                = tile[static_cast<std::size_t>(y) * 12 + x];
                        }
                    }
                }

                check(
                    index < metadata.size()
                        && metadata[index].fDuration
                            == static_cast<int>(std::llround(frames[index].duration_seconds * 1000)),
                    "optimized animation duration round trips"
                );
                check(
                    decode(*codec, static_cast<int>(index)) == expected, "optimized tile playback composes exact pixels"
                );
            }
        }

        void put_number(unsigned char* bytes, const std::uint32_t value)
        {
            for (std::size_t index {}; index < 4; ++index)
            {
                bytes[index] = static_cast<unsigned char>(value >> (index * 8));
            }
        }

        void run_invalid(const std::filesystem::path& directory)
        {
            const auto valid { directory / L"valid.webp" };
            const auto pixels { sample(64, 48, 3) };
            check(
                encode_native_image(valid, pixmap(pixels, 64, 48), image_format::webp), "cancellation fixture encodes"
            );

            const auto original { bytes_for(valid) };
            const auto count { file_count(directory) };
            const std::array frames { encoded_animation_frame { valid, 0.1 } };
            std::atomic<bool> cancelled { true };
            std::u8string error {};
            check(
                !optimize_webp_frames(frames, cancelled, error) && !error.empty(),
                "pre-cancelled optimization reports cancellation"
            );
            check(
                bytes_for(valid) == original && file_count(directory) == count,
                "cancellation preserves original and creates no temporary file"
            );
            cancelled = false;

            for (int kind {}; kind < 4; ++kind)
            {
                const auto corrupt { directory / (L"corrupt-" + std::to_wstring(kind) + L".webp") };
                auto bytes { original };

                if (kind == 0)
                {
                    bytes.resize(bytes.size() / 2);
                }
                else if (kind == 1)
                {
                    bytes[0] = 'X';
                }
                else
                {
                    std::array<unsigned char, 26> header { 'R', 'I', 'F', 'F', 18, 0, 0, 0,    'W', 'E', 'B', 'P', 'V',
                                                           'P', '8', 'L', 5,   0,  0, 0, 0x2f, 0,   0,   0,   0,   0
                    };

                    const std::uint32_t dimensions { kind == 2 ? 8191U | (8191U << 14) : 16383U };
                    put_number(header.data() + 21, dimensions);
                    bytes.assign(
                        reinterpret_cast<const char*>(header.data()),
                        reinterpret_cast<const char*>(header.data() + header.size())
                    );
                }

                {
                    std::ofstream output { corrupt, std::ios::binary };
                    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }

                const std::array invalid_frames { encoded_animation_frame { corrupt, 0.1 } };
                const auto before_count { file_count(directory) };
                check(
                    !optimize_webp_frames(invalid_frames, cancelled, error) && !error.empty(),
                    "corrupt, oversized dimension and pixel budget inputs are rejected"
                );
                check(
                    bytes_for(corrupt) == bytes && file_count(directory) == before_count,
                    "rejected input remains intact without temporary file"
                );
            }

            const auto strong { directory / L"already-small.webp" };
            check(
                encode_native_image(strong, pixmap(pixels, 64, 48), image_format::webp, lossless_compression::compact),
                "already optimized fixture encodes"
            );

            const auto strong_bytes { bytes_for(strong) };
            const std::array strong_frames { encoded_animation_frame { strong, 0.1 } };
            check(
                optimize_webp_frames(strong_frames, cancelled, error) && bytes_for(strong) == strong_bytes,
                "equal-size candidate is discarded without rewriting source"
            );
            check(file_count(directory) == count + 5, "discarded candidate leaves no temporary file");
        }

        void run_cancel_during_encode(const std::filesystem::path& directory)
        {
            const auto pixels { sample(640, 360, 3) };
            const auto original_path { directory / L"cancel-encoding.webp" };
            check(
                encode_native_image(original_path, pixmap(pixels, 640, 360), image_format::webp),
                "in-flight cancellation fixture encodes"
            );

            const auto original { bytes_for(original_path) };
            const std::array frames { encoded_animation_frame { original_path, 0.1 } };
            const auto before_count { file_count(directory) };
            std::atomic<bool> cancelled {};
            std::atomic<bool> finished {};
            const auto watch_temporary = [&] {
                while (!finished.load())
                {
                    for (const auto& entry : std::filesystem::directory_iterator { directory })
                    {
                        if (entry.path().filename().wstring().find(L"cancel-encoding.webp.opt-") == 0)
                        {
                            cancelled = true;
                            return;
                        }
                    }

                    std::this_thread::yield();
                }
            };

            std::thread watcher { watch_temporary };
            std::u8string error {};
            const auto result { optimize_webp_frames(frames, cancelled, error) };
            finished = true;
            watcher.join();
            check(
                cancelled.load() && !result && !error.empty(),
                "cancellation during strong encoding is honored before replacement"
            );
            check(
                bytes_for(original_path) == original && file_count(directory) == before_count,
                "in-flight cancellation removes candidate and preserves original bytes"
            );
        }

        void run_parallel(const std::filesystem::path& directory)
        {
            const auto caller { std::this_thread::get_id() };

            for (const auto compression : { lossless_compression::compact, lossless_compression::maximum })
            {
                for (const auto budget : { 1ULL, 256ULL * 1024 * 1024 })
                {
                    std::vector<encoded_animation_frame> frames;
                    const auto pixels { sample(96, 64, 1) };
                    std::uintmax_t original_size {};

                    for (int index {}; index < 8; ++index)
                    {
                        const auto path { directory / (L"parallel-" + std::to_wstring(index) + L".webp") };
                        check(
                            encode_native_image(path, pixmap(pixels, 96, 64), image_format::webp),
                            "parallel source encodes"
                        );
                        original_size += std::filesystem::file_size(path);
                        frames.push_back({ path, 0.0625, index * 2, index * 2 });
                    }

                    // Windows에서 대소문자가 다른 경로도 같은 파일을 동시에 바꾸지 않는다.
                    frames.push_back({ directory / L"PARALLEL-0.WEBP", 0.125, 20, 20 });

                    const auto count { file_count(directory) };
                    std::size_t reported {};
                    const auto progress = [&](const save_progress update) {
                        check(std::this_thread::get_id() == caller, "parallel progress stays on the caller thread");
                        check(
                            update.stage == save_stage::optimizing && update.total == frames.size()
                                && update.completed >= reported && update.completed <= update.total,
                            "parallel progress counts repeated paths once without moving backwards"
                        );
                        reported = update.completed;
                    };

                    std::atomic<bool> cancelled {};
                    std::u8string error;
                    check(
                        optimize_webp_frames(
                            frames,
                            cancelled,
                            error,
                            progress,
                            { .compression = compression, .workers = 4, .memory_budget = budget }
                        ),
                        "parallel and memory-limited optimization succeeds"
                    );
                    check(
                        error.empty() && reported == frames.size() && file_count(directory) == count,
                        "parallel completion reports all frames and removes temporary files"
                    );

                    std::uintmax_t optimized_size {};

                    for (std::size_t index {}; index < 8; ++index)
                    {
                        verify_pixels(frames[index].path, pixels, 96, 64);
                        optimized_size += std::filesystem::file_size(frames[index].path);
                        check(
                            frames[index].x == static_cast<int>(index) * 2
                                && frames[index].y == static_cast<int>(index) * 2
                                && frames[index].duration_seconds == 0.0625,
                            "parallel optimization preserves tile positions and timing"
                        );
                    }

                    check(optimized_size <= original_size, "parallel candidates never increase total bytes");

                    const auto cancel_after_progress = [&](const save_progress update) {
                        if (update.completed > 0)
                        {
                            cancelled = true;
                        }
                    };
                    check(
                        !optimize_webp_frames(frames, cancelled, error, cancel_after_progress, { .workers = 4 })
                            && !error.empty(),
                        "parallel cancellation from progress propagates"
                    );
                    check(file_count(directory) == count, "cancelled parallel workers join and remove temporary files");

                    for (std::size_t index {}; index < 8; ++index)
                    {
                        verify_pixels(frames[index].path, pixels, 96, 64);
                    }
                }
            }
        }

        void run_benchmark(const std::filesystem::path& directory)
        {
            std::cout << "sample,fast_bytes,small_bytes,post_bytes,fast_ms,small_ms,post_ms\n";
            constexpr std::array<const char*, 4> names { "ui_text", "gradient", "photo_like", "noise" };

            for (int kind {}; kind < 4; ++kind)
            {
                const auto pixels { sample(640, 360, kind) };
                const auto fast { directory / (L"bench-fast-" + std::to_wstring(kind) + L".webp") };
                const auto compact_path { directory / (L"bench-small-" + std::to_wstring(kind) + L".webp") };
                const auto start { std::chrono::steady_clock::now() };
                check(
                    encode_native_image(fast, pixmap(pixels, 640, 360), image_format::webp, lossless_compression::fast),
                    "benchmark fast encoding succeeds"
                );

                const auto fast_end { std::chrono::steady_clock::now() };
                check(
                    encode_native_image(
                        compact_path, pixmap(pixels, 640, 360), image_format::webp, lossless_compression::compact
                    ),
                    "benchmark strong encoding succeeds"
                );

                const auto small_end { std::chrono::steady_clock::now() };
                const auto fast_bytes { std::filesystem::file_size(fast) };
                const auto small_bytes { std::filesystem::file_size(compact_path) };
                const std::array frames { encoded_animation_frame { fast, 0.1 } };
                std::atomic<bool> cancelled {};
                std::u8string error {};
                const auto post_start { std::chrono::steady_clock::now() };
                check(optimize_webp_frames(frames, cancelled, error), "benchmark post-recording optimization succeeds");

                const auto post_end { std::chrono::steady_clock::now() };
                const auto post_bytes { std::filesystem::file_size(fast) };
                check(post_bytes <= fast_bytes, "each benchmark post result cannot increase file size");
                verify_pixels(fast, pixels, 640, 360);
                verify_pixels(compact_path, pixels, 640, 360);

                std::cout << names[kind] << ',' << fast_bytes << ',' << small_bytes << ',' << post_bytes << ','
                          << std::chrono::duration<double, std::milli>(fast_end - start).count() << ','
                          << std::chrono::duration<double, std::milli>(small_end - fast_end).count() << ','
                          << std::chrono::duration<double, std::milli>(post_end - post_start).count() << '\n';
            }
        }

    } // namespace
} // namespace chalkag

int main(const int argument_count, char* arguments[])
{
    const auto directory { std::filesystem::current_path()
                           / (L"webp_compression_test_" + std::to_wstring(GetCurrentProcessId()) + L"_"
                              + std::to_wstring(GetTickCount64()))
    };

    std::filesystem::create_directories(directory);
    const chalkag::test_files cleanup { directory };
    chalkag::run_still(directory);
    chalkag::run_animation(directory);
    chalkag::run_invalid(directory);
    chalkag::run_cancel_during_encode(directory);
    chalkag::run_parallel(directory);

    if (argument_count > 1 && std::string_view { arguments[1] } == "--benchmark")
    {
        chalkag::run_benchmark(directory);
    }

    if (!chalkag::failures)
    {
        std::cout << "WebP compression checks passed.\n";
    }

    return chalkag::failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
