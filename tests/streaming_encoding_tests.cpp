#include "encoding/image_encoder.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace chalkag {
    namespace {
        void require(const bool condition, const char* message)
        {
            if (!condition)
            {
                throw std::runtime_error { message };
            }
        }

        std::uint64_t private_bytes()
        {
            PROCESS_MEMORY_COUNTERS_EX counters {};
            counters.cb = sizeof(counters);
            require(
                K32GetProcessMemoryInfo(
                    GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)
                ) != 0,
                "Cannot sample process memory"
            );
            return counters.PrivateUsage;
        }

        class memory_monitor
        {
        public:
            memory_monitor()
                : baseline_ { private_bytes() }
                , peak_ { baseline_ }
                , sampler_ { start_sampler(this) }
            {}

            std::uint64_t growth()
            {
                sampler_.request_stop();
                sampler_.join();
                return peak_.load() - baseline_;
            }

        private:
            static std::jthread start_sampler(memory_monitor* owner)
            {
                const auto sample = [owner](std::stop_token stop) {
                    while (!stop.stop_requested())
                    {
                        const auto value { private_bytes() };
                        owner->peak_ = std::max(owner->peak_.load(), value);

                        std::this_thread::sleep_for(std::chrono::milliseconds { 5 });
                    }
                };
                return std::jthread { sample };
            }

            std::uint64_t baseline_;
            std::atomic<std::uint64_t> peak_;
            std::jthread sampler_;
        };

        class reusable_source
        {
        public:
            reusable_source(const int width, const int height, const std::size_t count)
                : count_ { count }
            {
                require(
                    bitmap_.tryAllocPixels(
                        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)
                    ),
                    "Cannot allocate source canvas"
                );
                bitmap_.eraseColor(SK_ColorBLUE);
            }

            encoding::animation_source source()
            {
                const auto read = [this](const std::size_t index, encoding::animation_frame& frame) {
                    if (index == 0)
                    {
                        next_ = 0;
                        ++passes_;
                    }

                    require(
                        index == next_ && index < count_, "Frame provider must only be read sequentially or rewound"
                    );
                    *bitmap_.getAddr32(0, 0) = index % 2 == 0 ? 0xffff0000 : 0xff00ff00;
                    frame = { bitmap_.pixmap(), 0.0625 };
                    ++next_;
                    return true;
                };
                return { count_, read };
            }

        private:
            SkBitmap bitmap_;
            std::size_t count_;
            std::size_t next_ {};
            std::size_t passes_ {};
        };

        void check_output(
            const std::filesystem::path& path, const int width, const int height, const int count, const bool gif
        )
        {
            auto codec { SkCodec::MakeFromData(SkData::MakeFromFileName(path.string().c_str())) };
            require(
                codec && codec->getFrameCount() == count && codec->getInfo().width() == width
                    && codec->getInfo().height() == height,
                "Streaming output frame count or dimensions differ"
            );

            const auto frames { codec->getFrameInfo() };
            int duration {};

            for (const auto& frame : frames)
            {
                duration += frame.fDuration;
            }

            require(std::abs(duration - count * 62.5) <= (gif ? 10 : 1), "Long recording duration drifted");

            SkBitmap decoded;
            require(
                decoded.tryAllocPixels(SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType)),
                "Cannot allocate verification canvas"
            );

            for (int index {}; index < count; ++index)
            {
                SkCodec::Options options {};
                options.fFrameIndex = index;
                options.fPriorFrame = index == 0 ? SkCodec::kNoFrame : index - 1;
                require(
                    codec->getPixels(decoded.info(), decoded.getPixels(), decoded.rowBytes(), &options)
                        == SkCodec::kSuccess,
                    "Cannot decode streaming frame"
                );
                require(
                    *decoded.getAddr32(0, 0) == (index % 2 == 0 ? 0xffff0000 : 0xff00ff00)
                        && *decoded.getAddr32(width - 1, height - 1) == 0xff0000ff,
                    "Streaming frame changed source colors or delta composition"
                );
            }
        }

        void run(const std::filesystem::path& directory)
        {
            std::filesystem::create_directories(directory);
            const std::atomic<bool> active {};
            std::u8string error;

            for (const bool gif : { true, false })
            {
                reusable_source input { 320, 180, 512 };
                memory_monitor memory;

                const auto output { directory / (gif ? "long.gif" : "long.webp") };
                require(
                    encoding::encode_animation(
                        output,
                        input.source(),
                        { .format = gif ? encoding::animation_format::gif : encoding::animation_format::webp,
                          .compact = false
                        },
                        active,
                        error
                    ),
                    "Long sequential encoding failed"
                );

                const auto growth { memory.growth() };
                std::cout << (gif ? "GIF" : "WebP") << " 512 frames, 32s: peak private growth " << growth / 1024
                          << " KiB\n";
                require(growth < 80 * 1024 * 1024, "Streaming encoding appears to retain all raw input frames");
                check_output(output, 320, 180, 512, gif);
            }

            {
                reusable_source input { 1280, 720, 128 };
                memory_monitor memory;

                const auto output { directory / "large.webp" };
                require(
                    encoding::encode_animation(
                        output, input.source(), { .format = encoding::animation_format::webp }, active, error
                    ),
                    "Automatic bounded-memory WebP fallback failed"
                );

                const auto growth { memory.growth() };
                std::cout << "WebP 720p, 128 frames, 450 MiB raw input: peak private growth " << growth / 1024
                          << " KiB\n";
                require(growth < 128 * 1024 * 1024, "WebP fallback exceeded its regression memory ceiling");
                check_output(output, 1280, 720, 128, false);
            }

            {
                reusable_source input { 16, 16, 5000 };

                const auto output { directory / "five-minutes.webp" };
                require(
                    encoding::encode_animation(
                        output,
                        input.source(),
                        { .format = encoding::animation_format::webp, .compact = false },
                        active,
                        error
                    ),
                    "Five-minute streaming recording failed"
                );
                check_output(output, 16, 16, 5000, false);
            }

            for (const auto format : { encoding::animation_format::gif, encoding::animation_format::webp })
            {
                const auto output { directory / "preserve.bin" };
                {
                    std::ofstream file { output, std::ios::binary | std::ios::trunc };
                    file << "original";
                }

                reusable_source input { 1280, 720, 128 };

                std::atomic<bool> cancelled {};
                const auto request_cancel = [&cancelled] {
                    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
                    cancelled = true;
                };

                std::jthread cancel { request_cancel };
                const auto started { std::chrono::steady_clock::now() };
                const auto success { encoding::encode_animation(
                    output, input.source(), { .format = format, .animation_memory_budget = 0 }, cancelled, error
                )
                };
                cancel.join();

                std::ifstream file { output, std::ios::binary };
                std::string bytes { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
                require(
                    !success && !error.empty() && bytes == "original", "Streaming cancellation replaced existing output"
                );

                const auto elapsed {
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                        .count()
                };

                std::cout << "Cancellation " << static_cast<int>(format) << ": " << elapsed << " ms\n";
                require(elapsed < 5000, "Cancellation did not return within one encoder operation");
            }

            for (const auto& item : std::filesystem::directory_iterator(directory))
            {
                require(
                    !item.path().filename().string().starts_with(".encoding-"),
                    "Streaming encoder leaked scratch directory"
                );
            }
        }

    } // namespace
} // namespace chalkag

int main()
{
    const auto directory { std::filesystem::current_path()
                           / ("streaming-test-" + std::to_string(GetCurrentProcessId()))
    };
    int result {};

    try
    {
        chalkag::run(directory);
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }

    std::filesystem::remove_all(directory);
    return result;
}
