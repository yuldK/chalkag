#include "capture/animation_recorder.h"
#include "capture/apng_writer.h"
#include "capture/native_webp_writer.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"

#include <objbase.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chalkag {
    namespace {
        using pixel_buffer = std::vector<std::uint32_t>;
        using expected_frames = std::map<std::filesystem::path, pixel_buffer>;
        int failure_count {};

        void check(const bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failure_count;
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

        SkPixmap pixmap(const pixel_buffer& pixels, const int width, const int height)
        {
            return { SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
                     pixels.data(),
                     static_cast<std::size_t>(width) * 4
            };
        }

        pixel_buffer background(const int width, const int height)
        {
            pixel_buffer result(static_cast<std::size_t>(width) * height);

            std::uint32_t seed { 0x72a91b3dU };

            for (auto& pixel : result)
            {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                pixel = 0xff000000U | (seed & 0x00ffffffU);
            }

            return result;
        }

        void patch(
            pixel_buffer& pixels, const int width, const int x, const int y, const int side, const std::uint32_t color
        )
        {
            for (int row {}; row < side; ++row)
            {
                std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>((y + row) * width + x), side, color);
            }
        }

        bool write_animation(
            const std::filesystem::path& output,
            const image_format format,
            const int width,
            const int height,
            const std::span<const encoded_animation_frame> frames
        )
        {
            if (format == image_format::apng)
            {
                const std::atomic<bool> cancelled {};
                std::u8string error {};
                const bool success { write_apng(output, frames, cancelled, error) };

                if (!success)
                {
                    std::cerr << reinterpret_cast<const char*>(error.c_str()) << '\n';
                }

                return success;
            }

            native_webp_writer writer { output, width, height };

            for (const auto& frame : frames)
            {
                if (!writer.append_frame(
                        frame.path,
                        static_cast<std::uint32_t>(std::llround(frame.duration_seconds * 1000)),
                        frame.x,
                        frame.y
                    ))
                {
                    return false;
                }
            }

            return writer.finish();
        }

        std::unique_ptr<SkCodec> codec_for(const std::filesystem::path& path)
        {
            std::ifstream file { path, std::ios::binary };
            const std::vector<char> bytes { std::istreambuf_iterator<char> { file },
                                            std::istreambuf_iterator<char> {}
            };
            return SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size()));
        }

        void verify_animation(
            const std::filesystem::path& output,
            const int width,
            const int height,
            const std::span<const encoded_animation_frame> frames,
            const expected_frames& expected
        )
        {
            auto codec { codec_for(output) };
            check(codec != nullptr, "SkCodec recognizes native animation");

            if (codec == nullptr)
            {
                return;
            }

            check(
                codec->getInfo().width() == width && codec->getInfo().height() == height,
                "animation canvas dimensions round trip"
            );
            check(codec->getFrameCount() == static_cast<int>(frames.size()), "animation frame count round trips");

            if (frames.size() > 1)
            {
                check(
                    codec->getRepetitionCount() == SkCodec::kRepetitionCountInfinite,
                    "native animation loops infinitely"
                );
            }

            const auto metadata { codec->getFrameInfo() };
            const auto info { SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType) };

            for (std::size_t index {}; index < frames.size(); ++index)
            {
                if (index < metadata.size())
                {
                    check(
                        metadata[index].fDuration
                            == static_cast<int>(std::llround(frames[index].duration_seconds * 1000)),
                        "each animation frame duration round trips"
                    );
                }

                pixel_buffer decoded(static_cast<std::size_t>(width) * height);

                SkCodec::Options options {};
                options.fFrameIndex = static_cast<int>(index);
                check(
                    codec->getPixels(info, decoded.data(), static_cast<std::size_t>(width) * 4, &options)
                        == SkCodec::kSuccess,
                    "each animation frame composes its entire canvas"
                );

                const auto wanted { expected.find(frames[index].path) };
                check(
                    wanted != expected.end() && decoded == wanted->second,
                    "all decoded pixels exactly match captured true colors"
                );
            }
        }

        std::int64_t duration_ms(const std::span<const encoded_animation_frame> frames)
        {
            std::int64_t result {};

            for (const auto& frame : frames)
            {
                result += std::llround(frame.duration_seconds * 1000);
            }

            return result;
        }

        void run_round_trip(const std::filesystem::path& directory, const image_format format)
        {
            std::filesystem::create_directories(directory);
            constexpr int width { 9 };
            constexpr int height { 7 };
            const auto base { background(width, height) };
            std::vector<pixel_buffer> samples { base };
            auto first { base };
            patch(first, width, 1, 1, 2, 0xff13ba79U);
            samples.push_back(first);

            auto moved { base };
            patch(moved, width, 6, 4, 2, 0xff13ba79U);
            samples.push_back(moved);

            auto odd_pixel { moved };
            odd_pixel[5 * width + 7] = 0xff245a93U;
            samples.push_back(odd_pixel);

            auto last_pixel { odd_pixel };
            last_pixel.back() = 0xff984723U;
            samples.push_back(last_pixel);
            animation_recorder recorder { directory, format };

            for (std::size_t index {}; index < samples.size(); ++index)
            {
                check(
                    recorder.append(pixmap(samples[index], width, height), 2.0 + static_cast<double>(index) / 16),
                    "recorder accepts changed sample and real timestamps"
                );
            }

            const auto frames { recorder.finish(2.0 + static_cast<double>(samples.size()) / 16) };
            check(frames.size() == samples.size(), "changed samples each produce one tile");
            expected_frames expected {};

            for (std::size_t index {}; index < frames.size(); ++index)
            {
                expected.emplace(frames[index].path, samples[index]);

                auto tile { codec_for(frames[index].path) };
                check(tile != nullptr, "encoded tile is a valid lossless image");

                if (tile != nullptr && index == 0)
                {
                    check(
                        tile->getInfo().width() == width && tile->getInfo().height() == height,
                        "first encoded frame covers complete canvas"
                    );
                }

                if (tile != nullptr && index == 3)
                {
                    check(
                        frames[index].x == 6 && frames[index].y == 4 && tile->getInfo().width() == 2
                            && tile->getInfo().height() == 2,
                        "odd changed pixel aligns tile origin to even coordinates"
                    );
                }

                if (tile != nullptr && index == 4)
                {
                    check(
                        frames[index].x == 8 && frames[index].y == 6 && tile->getInfo().width() == 1
                            && tile->getInfo().height() == 1,
                        "bottom-right tile encodes correct non-contiguous source stride"
                    );
                }
            }

            const auto output { directory / (format == image_format::apng ? "result.png" : "result.webp") };
            check(write_animation(output, format, width, height, frames), "delta animation writes successfully");
            verify_animation(output, width, height, frames, expected);
            check(
                recorder.finish(3.0).empty() && !recorder.append(pixmap(base, width, height), 3.1),
                "finished recorder rejects later use"
            );
        }

        void run_static(const std::filesystem::path& directory, const image_format format, const bool long_hold)
        {
            std::filesystem::create_directories(directory);
            const auto pixels { background(11, 8) };
            animation_recorder recorder { directory, format };

            for (int index {}; index < 33; ++index)
            {
                check(
                    recorder.append(pixmap(pixels, 11, 8), static_cast<double>(index) / 33),
                    "unchanged sample is accepted"
                );
            }

            const auto frames { recorder.finish(long_hold ? 70.125 : 1.0) };
            check(frames.size() == (long_hold ? 2U : 1U), "static samples merge and long hold splits safely");
            check(duration_ms(frames) == (long_hold ? 70125 : 1000), "merged hold preserves aggregate elapsed time");
            expected_frames expected {};

            for (const auto& frame : frames)
            {
                expected.emplace(frame.path, pixels);
                check(frame.duration_seconds > 0 && frame.duration_seconds <= 60, "each hold obeys native delay range");
                check(frame.path == frames.front().path, "split hold reuses encoded image without more encoding");
            }

            std::size_t encoded_count {};

            for (const auto& entry : std::filesystem::directory_iterator { directory })
            {
                if (entry.is_regular_file())
                {
                    ++encoded_count;
                }
            }

            check(encoded_count == 1, "33 identical captured samples encode only one temporary image");

            const auto output { directory / (format == image_format::apng ? "result.png" : "result.webp") };
            check(write_animation(output, format, 11, 8, frames), "static hold animation writes successfully");
            verify_animation(output, 11, 8, frames, expected);
        }

        void run_timing(const std::filesystem::path& directory, const image_format format)
        {
            std::filesystem::create_directories(directory);
            animation_recorder recorder { directory, format };

            std::vector<pixel_buffer> samples {};

            for (int index {}; index < 16; ++index)
            {
                samples.emplace_back(32, 0xff000000U | static_cast<std::uint32_t>(index * 0x0f1357));
                check(
                    recorder.append(pixmap(samples.back(), 8, 4), static_cast<double>(index) / 16),
                    "16 FPS changed sample appends"
                );
            }

            const auto frames { recorder.finish(1.0) };
            check(
                frames.size() == 16 && duration_ms(frames) == 1000,
                "16 FPS cumulative quantization preserves exactly 1000 ms"
            );
            expected_frames expected {};

            for (std::size_t index {}; index < frames.size(); ++index)
            {
                check(
                    std::llround(frames[index].duration_seconds * 1000) == (index % 2 == 0 ? 63 : 62),
                    "16 FPS cumulative durations alternate 63 and 62 ms"
                );
                expected.emplace(frames[index].path, samples[index]);
            }

            const auto output { directory / (format == image_format::apng ? "result.png" : "result.webp") };
            check(write_animation(output, format, 8, 4, frames), "16 FPS animation writes");
            verify_animation(output, 8, 4, frames, expected);
        }

        void run_invalid(const std::filesystem::path& directory)
        {
            std::filesystem::create_directories(directory);
            const pixel_buffer pixels(16, 0xff234567U);
            const auto image { pixmap(pixels, 4, 4) };

            for (const double value :
                 { -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
            {
                animation_recorder recorder { directory, image_format::webp };
                check(
                    !recorder.append(image, value) && !recorder.append(image, 0.0) && recorder.finish(1.0).empty(),
                    "invalid append timestamp permanently rejects recorder"
                );
            }

            for (const double value : { -1.0,
                                        0.0,
                                        0.5,
                                        1e20,
                                        std::numeric_limits<double>::quiet_NaN(),
                                        std::numeric_limits<double>::infinity()
            })
            {
                animation_recorder recorder { directory, image_format::webp };
                check(
                    recorder.append(image, 0.0) && recorder.append(image, 0.5),
                    "finish rejection fixture accepts samples"
                );
                check(
                    recorder.finish(value).empty() && recorder.finish(1.0).empty(),
                    "nonfinite or non-increasing finish is terminal"
                );
            }

            animation_recorder repeated { directory, image_format::webp };
            check(
                repeated.append(image, 0.0) && !repeated.append(image, 0.0) && repeated.finish(1.0).empty(),
                "duplicate timestamp rejects session"
            );
            animation_recorder changed_size { directory, image_format::webp };

            const pixel_buffer smaller(4, 0xff234567U);
            check(
                changed_size.append(image, 0.0) && !changed_size.append(pixmap(smaller, 2, 2), 0.5)
                    && changed_size.finish(1.0).empty(),
                "changed canvas dimensions reject recording immediately"
            );
            animation_recorder unsupported { directory, image_format::gif };
            check(!unsupported.append(image, 0.0), "native recorder rejects GIF format");
            animation_recorder failed { directory / "missing", image_format::webp };
            check(!failed.append(image, 0.0) && failed.finish(1.0).empty(), "image encoding failure rejects recording");
            animation_recorder empty { directory, image_format::webp };
            check(empty.finish(1.0).empty(), "empty recorder has no animation");
        }

        void run_storage_failure(const std::filesystem::path& directory, const image_format format)
        {
            std::filesystem::create_directories(directory);
            animation_recorder recorder { directory, format };
            std::vector<pixel_buffer> samples {};

            for (int index {}; index < 4; ++index)
            {
                samples.emplace_back(32, 0xff000000U | static_cast<std::uint32_t>((index + 1) * 0x231507));
            }

            // 세 번째 타일 경로를 디렉터리로 막아 디스크 오류처럼 저장이 실패하게 한다.
            std::filesystem::create_directory(
                directory / (format == image_format::apng ? "frame-2.png" : "frame-2.webp")
            );
            check(
                recorder.append(pixmap(samples[0], 8, 4), 0.0) && recorder.append(pixmap(samples[1], 8, 4), 0.1),
                "frames before storage failure append"
            );
            check(!recorder.append(pixmap(samples[2], 8, 4), 0.2), "blocked tile reports storage failure");
            check(recorder.storage_failed(), "storage failure is distinguishable from invalid input");
            check(!recorder.append(pixmap(samples[3], 8, 4), 0.3), "storage failure blocks later frames");

            const auto frames { recorder.finish(5.0) };
            check(frames.size() == 2, "storage failure keeps frames written before the failure");
            check(duration_ms(frames) == 200, "storage failure ends at the failed frame timestamp");

            expected_frames expected {};

            for (std::size_t index {}; index < frames.size(); ++index)
            {
                expected.emplace(frames[index].path, samples[index]);
            }

            const auto output { directory / (format == image_format::apng ? "result.png" : "result.webp") };
            check(write_animation(output, format, 8, 4, frames), "partial animation writes");
            verify_animation(output, 8, 4, frames, expected);
        }

        double milliseconds_since(const std::chrono::steady_clock::time_point begin)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        }

        void benchmark_case(
            const std::filesystem::path& directory,
            const image_format format,
            const int width,
            const int height,
            const bool optimized,
            const std::string_view workload
        )
        {
            std::filesystem::create_directories(directory);
            const auto base { background(width, height) };
            std::vector<pixel_buffer> samples {};

            for (int index {}; index < 16; ++index)
            {
                auto sample { base };

                if (workload == "changing")
                {
                    for (auto& pixel : sample)
                    {
                        pixel = 0xff000000U | ((pixel + static_cast<std::uint32_t>(index * 0x132759)) & 0x00ffffffU);
                    }
                }
                else
                {
                    patch(sample, width, workload == "static" ? 0 : index * 17, 13, 16, 0xff1375bdU);
                }

                samples.push_back(std::move(sample));
            }

            std::vector<encoded_animation_frame> frames {};
            expected_frames expected {};

            std::vector<std::filesystem::path> source_paths {};

            for (std::size_t index {}; index < samples.size(); ++index)
            {
                if (optimized && index > 0 && samples[index] == samples[index - 1])
                {
                    continue;
                }

                const auto path { directory
                                  / ((optimized ? "frame-" : "baseline-") + std::to_string(source_paths.size())
                                     + (format == image_format::webp ? ".webp" : ".png"))
                };
                source_paths.push_back(path);
                expected.emplace(path, samples[index]);
            }

            const auto begin { std::chrono::steady_clock::now() };

            if (optimized)
            {
                animation_recorder recorder { directory, format };

                for (std::size_t index {}; index < samples.size(); ++index)
                {
                    check(
                        recorder.append(pixmap(samples[index], width, height), static_cast<double>(index) / 16),
                        "optimized benchmark appends sample"
                    );
                }

                frames = recorder.finish(1.0);
            }
            else
            {
                for (std::size_t index {}; index < samples.size(); ++index)
                {
                    const auto& path { source_paths[index] };
                    check(
                        encode_native_image(
                            path,
                            pixmap(samples[index], width, height),
                            format == image_format::apng ? image_format::png : image_format::webp
                        ),
                        "baseline benchmark encodes full frame"
                    );
                    frames.push_back({ path, (index % 2 == 0 ? 63.0 : 62.0) / 1000 });
                }
            }

            const double encoding_ms { milliseconds_since(begin) };
            std::uintmax_t temporary_bytes {};

            for (const auto& frame : frames)
            {
                temporary_bytes += std::filesystem::file_size(frame.path);
            }

            const auto output { directory / (format == image_format::apng ? "result.png" : "result.webp") };
            const auto mux_begin { std::chrono::steady_clock::now() };
            check(write_animation(output, format, width, height, frames), "benchmark animation writes");

            const double mux_ms { milliseconds_since(mux_begin) };
            const double total_ms { encoding_ms + mux_ms };
            verify_animation(output, width, height, frames, expected);

            std::cout << (format == image_format::apng ? "APNG" : "WebP") << ',' << width << 'x' << height << ','
                      << workload << ',' << (optimized ? "optimized" : "full") << ',' << frames.size() << ','
                      << encoding_ms << ',' << mux_ms << ',' << total_ms << ',' << temporary_bytes << ','
                      << std::filesystem::file_size(output) << '\n';
        }

        int run_tests(const bool benchmark)
        {
            const auto nonce { std::chrono::steady_clock::now().time_since_epoch().count() };
            test_files files { std::filesystem::current_path() / "build" / "animation-recorder-tests"
                               / std::to_string(nonce)
            };

            std::filesystem::create_directories(files.directory);

            if (benchmark)
            {
                std::cout << "format,resolution,workload,mode,frames,encode_write_ms,container_ms,total_ms,temporary_"
                             "bytes,output_bytes\n";

                for (const auto format : { image_format::webp, image_format::apng })
                {
                    for (const int width : { 640, 1920 })
                    {
                        for (const auto workload : { "static", "patch", "changing" })
                        {
                            for (const bool optimized : { false, true })
                            {
                                benchmark_case(
                                    files.directory
                                        / (std::to_string(static_cast<int>(format)) + "-" + std::to_string(width) + "-"
                                           + workload + (optimized ? "-optimized" : "-full")),
                                    format,
                                    width,
                                    width == 640 ? 360 : 1080,
                                    optimized,
                                    workload
                                );
                            }
                        }
                    }
                }
            }
            else
            {
                for (const auto format : { image_format::webp, image_format::apng })
                {
                    const auto directory { files.directory / std::to_string(static_cast<int>(format)) };
                    run_round_trip(directory / "moving", format);
                    run_static(directory / "static", format, false);
                    run_static(directory / "long", format, true);
                    run_timing(directory / "timing", format);
                    run_storage_failure(directory / "storage-failure", format);
                }

                run_invalid(files.directory / "invalid");
            }

            return failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

    } // namespace
} // namespace chalkag

int main(const int argc, char** argv)
{
    const HRESULT initialized { CoInitializeEx(nullptr, COINIT_MULTITHREADED) };

    if (FAILED(initialized))
    {
        return EXIT_FAILURE;
    }

    struct com_scope
    {
        ~com_scope()
        {
            CoUninitialize();
        }
    };

    const com_scope com {};

    try
    {
        const int result { chalkag::run_tests(argc == 2 && std::string_view { argv[1] } == "--benchmark") };

        if (result == EXIT_SUCCESS)
        {
            std::cout << "Native animation recorder tests passed.\n";
        }

        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
