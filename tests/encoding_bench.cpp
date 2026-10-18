#include "capture/native_gif_writer.h"
#include "capture/native_webp_writer.h"
#include "encoding/image_encoder.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace chalkag {
    namespace {
        void require(const bool condition, const char* message)
        {
            if (!condition)
            {
                throw std::runtime_error { message };
            }
        }

        std::string read(const std::filesystem::path& path)
        {
            std::ifstream file { path, std::ios::binary };
            return { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
        }

        struct fixture
        {
            std::string name;
            std::vector<SkBitmap> bitmaps;
            std::vector<encoding::animation_frame> frames;
            std::vector<encoded_animation_frame> pngs;
            int duplicate_count { 1 };
        };

        fixture create_fixture(const std::filesystem::path& directory, const bool noise, const bool recording = false)
        {
            fixture result { .name = noise ? "noise" : "ui", .duplicate_count = noise ? 1 : 2 };

            if (recording)
            {
                result.name += "5s";
            }

            const int width { recording ? 758 : noise ? 192 : 320 };
            const int height { recording ? 612 : noise ? 128 : 180 };
            const int count { recording ? 120 : noise ? 12 : 24 };
            const int fps { recording ? 24 : 16 };
            std::uint32_t random { 37 };
            const auto inputs { directory / result.name / "inputs" };
            std::filesystem::create_directories(inputs);

            for (int frame {}; frame < count; ++frame)
            {
                SkBitmap bitmap;
                require(
                    bitmap.tryAllocPixels(
                        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)
                    ),
                    "Cannot allocate fixture"
                );

                for (int y {}; y < height; ++y)
                {
                    for (int x {}; x < width; ++x)
                    {
                        std::uint32_t color { 0xff202733 };

                        if (noise)
                        {
                            random = random * 1664525u + 1013904223u;
                            color = 0xff000000u | (random & 0x00ffffffu);
                        }
                        else
                        {
                            if (y < 24)
                            {
                                color = 0xff161b24;
                            }
                            else if (x > 32 && x < 288 && y > 40 && y < 160)
                            {
                                color = 0xffeeeeee;
                            }

                            const int left { 40 + frame / 2 * 12 };

                            if (x >= left && x < left + 24 && y >= 72 && y < 104)
                            {
                                color = 0xffed5c70;
                            }
                        }

                        *bitmap.getAddr32(x, y) = color;
                    }
                }

                const auto png { inputs / (std::to_string(frame) + ".png") };
                require(encode_native_image(png, bitmap.pixmap(), image_format::png), "Cannot write fixture PNG");

                // 기존 앱도 원본 수집 단계에서 동일 프레임 시간을 합치므로 기준 GIF에 반영한다.
                if (!noise && frame % 2 == 1)
                {
                    result.pngs.back().duration_seconds += 1.0 / fps;
                }
                else
                {
                    result.pngs.push_back({ png, 1.0 / fps, 0, 0 });
                }

                result.bitmaps.push_back(std::move(bitmap));
            }

            for (const auto& bitmap : result.bitmaps)
            {
                result.frames.push_back({ bitmap.pixmap(), 1.0 / fps });
            }

            return result;
        }

        fixture create_gif_fixture(const std::filesystem::path& directory, const bool desktop)
        {
            fixture result { .name = desktop ? "desktop720" : "gradient", .duplicate_count = 2 };

            const int width { desktop ? 1280 : 320 };
            const int height { desktop ? 720 : 180 };
            const auto inputs { directory / result.name / "inputs" };
            std::filesystem::create_directories(inputs);

            for (int frame {}; frame < 16; ++frame)
            {
                SkBitmap bitmap;
                require(
                    bitmap.tryAllocPixels(
                        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)
                    ),
                    "Cannot allocate GIF fixture"
                );

                for (int y {}; y < height; ++y)
                {
                    for (int x {}; x < width; ++x)
                    {
                        const auto red { static_cast<std::uint32_t>(x * 255 / width) << 16 };
                        const auto green { static_cast<std::uint32_t>(y * 255 / height) << 8 };
                        const auto blue { static_cast<std::uint32_t>((x + y) * 255 / (width + height)) };
                        std::uint32_t color { 0xff000000u | red | green | blue };

                        if (desktop)
                        {
                            color = x < 180 || y < 40 ? 0xff1d2533 : 0xffeeeeee;

                            if (x > 210 && x < 1200 && y > 70 && y < 530)
                            {
                                color = 0xfff8fafc;

                                if (y % 28 < 3 && x < 1050 - (y / 28 % 3) * 80)
                                {
                                    color = 0xff394a63;
                                }
                            }

                            if (x > 210 && x < 1200 && y >= 560)
                            {
                                color = 0xff000000u | red | green | 0x80u;
                            }
                        }

                        const int left { width / 4 + frame / 2 * (desktop ? 40 : 12) };

                        if (x >= left && x < left + width / 10 && y >= height / 3 && y < height / 2)
                        {
                            color = 0xffed5c70;
                        }

                        *bitmap.getAddr32(x, y) = color;
                    }
                }

                const auto png { inputs / (std::to_string(frame) + ".png") };
                require(encode_native_image(png, bitmap.pixmap(), image_format::png), "Cannot write GIF fixture PNG");

                if (frame % 2)
                {
                    result.pngs.back().duration_seconds += 1.0 / 16;
                }
                else
                {
                    result.pngs.push_back({ png, 1.0 / 16, 0, 0 });
                }

                result.bitmaps.push_back(std::move(bitmap));
            }

            for (const auto& bitmap : result.bitmaps)
            {
                result.frames.push_back({ bitmap.pixmap(), 1.0 / 16 });
            }

            return result;
        }

        struct decoded_result
        {
            int frames {};
            int duration_ms {};
            double mean_error {};
        };

        decoded_result verify(
            const std::filesystem::path& path,
            const fixture& source,
            const bool merged,
            const bool exact,
            const int tolerance_ms = 10
        )
        {
            const auto bytes { read(path) };
            auto codec { SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size())) };
            require(codec != nullptr, "Encoded animation cannot be decoded");

            const auto& first { source.bitmaps.front() };
            require(
                codec->getInfo().width() == first.width() && codec->getInfo().height() == first.height(),
                "Animation dimensions differ"
            );

            const int count { codec->getFrameCount() };
            const int divisor { merged ? source.duplicate_count : 1 };
            require(count == static_cast<int>(source.frames.size()) / divisor, "Animation frame merge count differs");
            require(
                codec->getRepetitionCount() == SkCodec::kRepetitionCountInfinite,
                "Animation is not infinitely repeating"
            );

            SkBitmap decoded;
            require(
                decoded.tryAllocPixels(first.info().makeAlphaType(kPremul_SkAlphaType)), "Cannot allocate decoded frame"
            );
            double difference {};
            int duration {};

            for (int frame {}; frame < count; ++frame)
            {
                SkCodec::FrameInfo info {};
                require(codec->getFrameInfo(frame, &info), "Missing animation frame metadata");
                duration += info.fDuration;

                SkCodec::Options options {};
                options.fFrameIndex = frame;
                options.fPriorFrame = frame > 0 ? frame - 1 : SkCodec::kNoFrame;
                require(
                    codec->getPixels(decoded.info(), decoded.getPixels(), decoded.rowBytes(), &options)
                        == SkCodec::kSuccess,
                    "Animation frame decode failed"
                );

                const auto& expected { source.bitmaps[static_cast<std::size_t>(frame) * divisor] };

                for (int y {}; y < decoded.height(); ++y)
                {
                    for (int x {}; x < decoded.width(); ++x)
                    {
                        const auto actual { *decoded.getAddr32(x, y) };
                        const auto wanted { *expected.getAddr32(x, y) };
                        require((actual >> 24) == 255, "Composited animation has unexpected transparency");

                        if (exact)
                        {
                            require(actual == wanted, "Lossless composited pixels differ");
                        }

                        for (const int shift : { 0, 8, 16 })
                        {
                            difference += std::abs(
                                static_cast<int>((actual >> shift) & 255) - static_cast<int>((wanted >> shift) & 255)
                            );
                        }
                    }
                }
            }

            double expected_duration {};

            for (const auto& frame : source.frames)
            {
                expected_duration += frame.duration_seconds * 1000;
            }

            if (std::abs(duration - expected_duration) > tolerance_ms)
            {
                throw std::runtime_error { "Animation timeline differs: " + std::to_string(duration) + " vs "
                                           + std::to_string(expected_duration)
                };
            }

            return { count, duration, difference / (static_cast<double>(count) * first.width() * first.height() * 3) };
        }

        void benchmark(
            const std::filesystem::path& directory,
            const fixture& source,
            const encoding::encoder_options options,
            const std::string& label,
            const bool baseline,
            std::ostream& csv,
            const int repetitions = 1
        )
        {
            const auto suffix { options.format == encoding::animation_format::gif        ? ".gif"
                                    : options.format == encoding::animation_format::apng ? ".apng"
                                                                                         : ".webp"
            };

            const auto output { directory / source.name / (label + suffix) };
            const std::atomic<bool> cancelled {};
            std::u8string error;
            std::vector<double> timings;

            for (int attempt {}; attempt < repetitions; ++attempt)
            {
                const auto started { std::chrono::steady_clock::now() };
                const bool success {
                    baseline ? write_gif(output, source.pngs, cancelled, error)
                             : encoding::encode_animation(output, source.frames, options, cancelled, error)
                };
                timings.push_back(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
                );

                if (!success)
                {
                    std::cerr << reinterpret_cast<const char*>(error.c_str()) << '\n';
                }

                require(success, "Animation encoding failed");
            }

            std::sort(timings.begin(), timings.end());
            const double elapsed { timings[timings.size() / 2] };
            const bool merged { baseline || options.format != encoding::animation_format::webp || options.lossless
                                || options.webp_animation_optimizer
            };

            const bool lossless { options.format == encoding::animation_format::apng
                                  || (options.format == encoding::animation_format::webp && options.lossless)
            };

            const bool exact { lossless || (options.format == encoding::animation_format::gif && source.name == "ui") };
            const auto decoded { verify(output, source, merged, exact) };
            const auto row = [&](std::ostream& stream) {
                stream << source.name << ',' << label << ',' << std::fixed << std::setprecision(3) << elapsed << ','
                       << std::filesystem::file_size(output) << ',' << decoded.frames << ',' << decoded.duration_ms
                       << ',' << decoded.mean_error << '\n';
            };
            row(std::cout);
            row(csv);
        }

        void validate_failures(const std::filesystem::path& directory, const fixture& source)
        {
            const auto output { directory / "preserve.bin" };
            {
                std::ofstream file { output };
                file << "original";
            }

            std::u8string error;
            const std::atomic<bool> cancelled { true };
            require(
                !encoding::encode_animation(output, source.frames, {}, cancelled, error) && read(output) == "original",
                "Cancellation overwrote existing output"
            );

            const std::atomic<bool> active {};
            auto frames { source.frames };
            frames.back().duration_seconds = -1;
            require(
                !encoding::encode_animation(output, frames, { .compact = false }, active, error)
                    && read(output) == "original",
                "Invalid timing overwrote existing output"
            );
            frames = source.frames;
            frames.back().pixels = {};
            require(
                !encoding::encode_animation(output, frames, { .compact = false }, active, error)
                    && read(output) == "original",
                "Invalid pixels overwrote existing output"
            );

            if (source.name == "noise")
            {
                std::atomic<bool> interrupted {};
                const auto request_cancel = [&interrupted] {
                    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
                    interrupted = true;
                };

                std::jthread cancellation { request_cancel };
                const bool success { encoding::encode_animation(output, source.frames, {}, interrupted, error) };
                cancellation.join();
                require(
                    !success && interrupted.load() && read(output) == "original",
                    "In-flight WebP cancellation overwrote output"
                );
            }

            const auto impossible { directory / "target-directory" };
            std::filesystem::create_directories(impossible);
            require(
                !encoding::encode_animation(impossible, source.frames, { .compact = false }, active, error)
                    && std::filesystem::is_directory(impossible),
                "Publish failure removed destination directory"
            );

            for (const auto& entry : std::filesystem::directory_iterator(directory))
            {
                require(
                    !entry.path().filename().string().starts_with(".encoding-"),
                    "Failed encoding left its scratch directory behind"
                );
            }
        }

        void validate_small_frames(
            const std::filesystem::path& directory,
            const encoding::gif_backend gif_encoder = encoding::gif_backend::cgif
        )
        {
            for (const int width : { 1, 7 })
            {
                SkBitmap first;
                SkBitmap second;
                const auto info { SkImageInfo::Make(width, 5, kBGRA_8888_SkColorType, kOpaque_SkAlphaType) };
                require(
                    first.tryAllocPixels(info, 64) && second.tryAllocPixels(info, 64), "Cannot allocate padded frame"
                );
                first.eraseColor(SK_ColorRED);
                second.eraseColor(SK_ColorBLUE);

                const std::array frames { encoding::animation_frame { first.pixmap(), 0.1 },
                                          encoding::animation_frame { second.pixmap(), 0.2 }
                };

                for (const auto format : { encoding::animation_format::gif,
                                           encoding::animation_format::webp,
                                           encoding::animation_format::apng
                })
                {
                    if (gif_encoder != encoding::gif_backend::cgif && format != encoding::animation_format::gif)
                    {
                        continue;
                    }

                    const auto output { directory
                                        / ("small-" + std::to_string(width) + "-"
                                           + std::to_string(static_cast<int>(format)) + "-"
                                           + std::to_string(static_cast<int>(gif_encoder)))
                    };

                    const std::atomic<bool> cancelled {};
                    std::u8string error;
                    require(
                        encoding::encode_animation(
                            output,
                            frames,
                            { .format = format, .compact = false, .gif_encoder = gif_encoder },
                            cancelled,
                            error
                        ),
                        "Small padded animation encode failed"
                    );

                    const auto bytes { read(output) };
                    auto codec { SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size())) };
                    require(
                        codec && codec->getFrameCount() == 2 && codec->getInfo().width() == width,
                        "Small animation dimensions or frame count differ"
                    );

                    SkBitmap decoded;
                    require(
                        decoded.tryAllocPixels(info.makeAlphaType(kPremul_SkAlphaType)),
                        "Cannot allocate small decoded frame"
                    );

                    SkCodec::Options options {};
                    options.fFrameIndex = 1;
                    require(
                        codec->getPixels(decoded.info(), decoded.getPixels(), decoded.rowBytes(), &options)
                                == SkCodec::kSuccess
                            && *decoded.getAddr32(0, 0) == *second.getAddr32(0, 0),
                        "Small composited frame differs"
                    );
                }
            }
        }

        void compare_gif_pixels(const std::filesystem::path& expected_path, const std::filesystem::path& actual_path)
        {
            const auto expected_bytes { read(expected_path) };
            const auto actual_bytes { read(actual_path) };
            auto expected { SkCodec::MakeFromData(SkData::MakeWithCopy(expected_bytes.data(), expected_bytes.size())) };
            auto actual { SkCodec::MakeFromData(SkData::MakeWithCopy(actual_bytes.data(), actual_bytes.size())) };
            require(
                expected && actual && expected->getFrameCount() == actual->getFrameCount(),
                "CGIF frame count differs from reference"
            );

            const auto info {
                expected->getInfo().makeColorType(kBGRA_8888_SkColorType).makeAlphaType(kPremul_SkAlphaType)
            };

            SkBitmap wanted;
            SkBitmap decoded;
            require(
                wanted.tryAllocPixels(info) && decoded.tryAllocPixels(info), "Cannot allocate GIF comparison bitmaps"
            );

            for (int frame {}; frame < expected->getFrameCount(); ++frame)
            {
                SkCodec::Options options {};
                options.fFrameIndex = frame;
                options.fPriorFrame = frame > 0 ? frame - 1 : SkCodec::kNoFrame;
                require(
                    expected->getPixels(info, wanted.getPixels(), wanted.rowBytes(), &options) == SkCodec::kSuccess
                        && actual->getPixels(info, decoded.getPixels(), decoded.rowBytes(), &options)
                            == SkCodec::kSuccess,
                    "Cannot decode GIF comparison frame"
                );

                SkCodec::FrameInfo expected_frame {};
                SkCodec::FrameInfo actual_frame {};
                require(
                    expected->getFrameInfo(frame, &expected_frame) && actual->getFrameInfo(frame, &actual_frame)
                        && expected_frame.fDuration == actual_frame.fDuration,
                    "CGIF per-frame delay differs"
                );

                for (int y {}; y < info.height(); ++y)
                {
                    require(
                        std::memcmp(
                            wanted.getAddr32(0, y), decoded.getAddr32(0, y), static_cast<std::size_t>(info.width()) * 4
                        ) == 0,
                        "CGIF composited pixels differ from reference"
                    );
                }
            }
        }

        void validate_cgif_failures(
            const std::filesystem::path& directory, const fixture& source, const encoding::gif_backend backend
        )
        {
            const auto output { directory / "cgif-preserve.bin" };
            {
                std::ofstream file { output };
                file << "original";
            }

            const encoding::encoder_options options { .format = encoding::animation_format::gif,
                                                      .gif_encoder = backend
            };

            const std::atomic<bool> cancelled { true };
            const std::atomic<bool> active {};
            std::u8string error;
            require(
                !encoding::encode_animation(output, source.frames, options, cancelled, error)
                    && read(output) == "original",
                "CGIF cancellation overwrote previous output"
            );

            const auto impossible { directory / "cgif-target-directory" };
            std::filesystem::create_directories(impossible);
            require(
                !encoding::encode_animation(impossible, source.frames, options, active, error)
                    && std::filesystem::is_directory(impossible),
                "CGIF publish failure changed destination"
            );

            for (const auto& entry : std::filesystem::directory_iterator(directory))
            {
                require(
                    !entry.path().filename().string().starts_with(".encoding-"),
                    "CGIF failure left scratch directory behind"
                );
            }
        }

        void run_gif_benchmark(const std::filesystem::path& directory, const bool full)
        {
            std::filesystem::create_directories(directory);
            std::ofstream csv { directory / "gif-results.csv" };
            constexpr auto header { "scenario,encoder,encode_ms,bytes,decoded_frames,duration_ms,rgb_mae\n" };
            csv << header;

            std::cout << header;
            const int repetitions { full ? 5 : 1 };

            for (int scenario {}; scenario < (full ? 4 : 3); ++scenario)
            {
                const auto source { scenario < 2 ? create_fixture(directory, scenario == 1)
                                                 : create_gif_fixture(directory, scenario == 3)
                };
                benchmark(
                    directory, source, { .format = encoding::animation_format::gif }, "wic-gif", true, csv, repetitions
                );
                benchmark(
                    directory,
                    source,
                    { .format = encoding::animation_format::gif,
                      .gif_encoder = encoding::gif_backend::cgif_fixed_palette
                    },
                    "mit-cgif-fixed",
                    false,
                    csv,
                    repetitions
                );
                benchmark(
                    directory,
                    source,
                    { .format = encoding::animation_format::gif, .gif_encoder = encoding::gif_backend::cgif },
                    "mit-cgif",
                    false,
                    csv,
                    repetitions
                );

                const auto reference { directory / source.name / "mit-cgif-fixed.gif" };
                compare_gif_pixels(reference, directory / source.name / "mit-cgif.gif");

                if (scenario == 0)
                {
                    for (const auto backend :
                         { encoding::gif_backend::cgif, encoding::gif_backend::cgif_fixed_palette })
                    {
                        validate_cgif_failures(directory, source, backend);
                    }
                }
            }

            for (const auto backend : { encoding::gif_backend::cgif, encoding::gif_backend::cgif_fixed_palette })
            {
                validate_small_frames(directory, backend);
            }
        }

        void run(const std::filesystem::path& directory, const bool full)
        {
            std::filesystem::create_directories(directory);
            std::ofstream csv { directory / "results.csv" };
            constexpr auto header { "scenario,encoder,encode_ms,bytes,decoded_frames,duration_ms,rgb_mae\n" };
            csv << header;

            std::cout << header;

            for (const bool noise : { false, true })
            {
                const auto source { create_fixture(directory, noise) };
                benchmark(directory, source, { .format = encoding::animation_format::gif }, "wic-gif", true, csv);
                benchmark(directory, source, { .format = encoding::animation_format::gif }, "native-cgif", false, csv);
                benchmark(
                    directory,
                    source,
                    { .format = encoding::animation_format::webp, .compact = false },
                    "static-webp-lossless-fast",
                    false,
                    csv
                );

                if (!noise || full)
                {
                    benchmark(
                        directory,
                        source,
                        { .format = encoding::animation_format::webp },
                        "static-webp-lossless-compact",
                        false,
                        csv
                    );
                }

                if (!noise || full)
                {
                    benchmark(
                        directory,
                        source,
                        { .format = encoding::animation_format::webp, .webp_animation_optimizer = false },
                        "delta-webp-lossless-compact",
                        false,
                        csv
                    );
                }

                if (full)
                {
                    benchmark(
                        directory, source, { .maximum_compression = true }, "static-webp-lossless-maximum", false, csv
                    );
                }

                benchmark(
                    directory,
                    source,
                    { .format = encoding::animation_format::webp, .lossless = false, .quality = 80 },
                    "static-webp-q80",
                    false,
                    csv
                );
                benchmark(directory, source, { .format = encoding::animation_format::apng }, "static-apng", false, csv);
                validate_failures(directory, source);
            }

            validate_small_frames(directory);
        }

        void run_webp_benchmark(const std::filesystem::path& directory, const bool full)
        {
            std::filesystem::create_directories(directory);
            std::ofstream csv { directory / "webp-results.csv" };
            constexpr auto header { "scenario,encoder,encode_ms,bytes,decoded_frames,duration_ms,rgb_mae\n" };
            csv << header;

            std::cout << header;

            for (const bool noise : { false, true })
            {
                const auto source { create_fixture(directory, noise) };
                benchmark(directory, source, { .compact = false }, "lossless-fast", false, csv);
                benchmark(directory, source, {}, "balanced-animation", false, csv);

                for (const std::size_t workers : { 1, 4, 8 })
                {
                    benchmark(
                        directory,
                        source,
                        { .webp_workers = workers, .webp_animation_optimizer = false },
                        "balanced-workers-" + std::to_string(workers),
                        false,
                        csv
                    );

                    if (full)
                    {
                        benchmark(
                            directory,
                            source,
                            { .maximum_compression = true,
                              .webp_workers = workers,
                              .webp_threading = false,
                              .webp_animation_optimizer = false
                            },
                            "maximum-workers-" + std::to_string(workers),
                            false,
                            csv
                        );
                    }
                }

                if (full)
                {
                    benchmark(
                        directory,
                        source,
                        { .maximum_compression = true, .webp_threading = false },
                        "maximum-animation-single",
                        false,
                        csv
                    );
                    benchmark(
                        directory,
                        source,
                        { .maximum_compression = true, .webp_threading = true },
                        "maximum-animation-threaded",
                        false,
                        csv
                    );
                }
            }
        }

        void run_webp_recording_benchmark(const std::filesystem::path& directory)
        {
            std::filesystem::create_directories(directory);
            std::ofstream csv { directory / "webp-recording-results.csv" };
            constexpr auto header { "scenario,encoder,encode_ms,bytes,decoded_frames,duration_ms,rgb_mae\n" };
            csv << header;

            std::cout << header;
            const auto source { create_fixture(directory, true, true) };
            benchmark(directory, source, { .compact = false }, "lossless-fast", false, csv);

            for (const std::size_t workers : { 1, 4, 8 })
            {
                benchmark(
                    directory,
                    source,
                    { .webp_workers = workers },
                    "balanced-workers-" + std::to_string(workers),
                    false,
                    csv
                );
            }
        }

        void inspect(
            const std::filesystem::path& directory, const std::string& scenario, const std::filesystem::path& output
        )
        {
            const auto source { create_fixture(directory, scenario == "noise") };
            const auto bytes { read(output) };
            auto codec { SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size())) };
            require(codec != nullptr, "External encoder output cannot be decoded");

            const bool merged { codec->getFrameCount()
                                == static_cast<int>(source.frames.size()) / source.duplicate_count
            };

            const bool exact { output.extension() == ".apng" };

            // 외부 APNG의 분수 밀리초를 SkCodec이 프레임마다 정수로 읽는 오차를 허용한다.
            const auto result { verify(output, source, merged, exact, std::max(10, codec->getFrameCount())) };
            std::cout << scenario << ',' << output.filename().string() << ',' << std::filesystem::file_size(output)
                      << ',' << result.frames << ',' << result.duration_ms << ',' << result.mean_error << '\n';
        }

    } // namespace
} // namespace chalkag

int main(const int argc, char** argv)
{
    std::cout << std::unitbuf;

    try
    {
        const auto directory { argc > 1 ? std::filesystem::path { argv[1] }
                                        : std::filesystem::current_path() / "encoding-results"
        };

        if (argc == 5 && std::string_view { argv[1] } == "--inspect")
        {
            chalkag::inspect(argv[2], argv[3], argv[4]);
        }
        else
        {
            const std::string_view mode { argc > 2 ? argv[2] : "" };

            if (argc > 3)
            {
                throw std::runtime_error { "Too many benchmark arguments" };
            }

            if (mode == "--webp-recording-bench")
            {
                chalkag::run_webp_recording_benchmark(directory);
            }
            else if (mode == "--webp-bench" || mode == "--webp-bench-full")
            {
                chalkag::run_webp_benchmark(directory, mode == "--webp-bench-full");
            }
            else if (mode == "--gif-bench" || mode == "--gif-bench-full")
            {
                chalkag::run_gif_benchmark(directory, mode == "--gif-bench-full");
            }
            else
            {
                if (argc > 3 || (!mode.empty() && mode != "--full"))
                {
                    throw std::runtime_error { "Unsupported benchmark mode; see README.md for available commands" };
                }

                chalkag::run(directory, mode == "--full");
            }
        }

        return 0;
    }
    catch (const std::exception& failure)
    {
        std::cerr << "FAIL: " << failure.what() << '\n';
        return 1;
    }
}
