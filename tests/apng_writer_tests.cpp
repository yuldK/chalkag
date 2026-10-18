#include "capture/apng_writer.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
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

        struct test_files
        {
            std::filesystem::path directory;

            ~test_files()
            {
                std::error_code ignored {};

                for (const auto& entry : std::filesystem::directory_iterator { directory, ignored })
                {
                    std::filesystem::remove(entry.path(), ignored);
                }

                std::filesystem::remove(directory, ignored);
            }
        };

        void write_bytes(const std::filesystem::path& path, const std::span<const std::uint8_t> bytes)
        {
            std::ofstream file { path, std::ios::binary };
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            check(file.good(), "test fixture writes successfully");
        }

        std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path)
        {
            std::ifstream file { path, std::ios::binary };
            return { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
        }

        void check_result(const HRESULT result)
        {
            if (FAILED(result))
            {
                throw std::runtime_error { "WIC PNG fixture failed: "
                                           + std::to_string(static_cast<unsigned long>(result))
                };
            }
        }

        std::uint32_t test_crc(const std::span<const std::uint8_t> bytes)
        {
            // 표 방식의 writer와 독립적인 비트 연산으로 올바른 CRC를 가진 메타데이터를 만든다.
            std::uint32_t crc { 0xffffffffU };

            for (const std::uint8_t byte : bytes)
            {
                crc ^= byte;

                for (int bit {}; bit < 8; ++bit)
                {
                    crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320U : 0U);
                }
            }

            return crc ^ 0xffffffffU;
        }

        void add_text_chunk(const std::filesystem::path& path)
        {
            auto png { read_bytes(path) };
            const std::array<std::uint8_t, 9> payload { 'N', 'o', 't', 'e', 0, 't', 'e', 's', 't' };
            std::vector<std::uint8_t> chunk { 0, 0, 0, 9, 't', 'E', 'X', 't' };
            chunk.insert(chunk.end(), payload.begin(), payload.end());

            const std::uint32_t crc { test_crc(std::span<const std::uint8_t> { chunk }.subspan(4)) };

            for (int shift { 24 }; shift >= 0; shift -= 8)
            {
                chunk.push_back(static_cast<std::uint8_t>(crc >> shift));
            }

            png.insert(png.begin() + 33, chunk.begin(), chunk.end());
            write_bytes(path, png);
        }

        void change_gamma(const std::filesystem::path& path, const std::uint32_t value)
        {
            auto png { read_bytes(path) };

            for (std::size_t offset { 8 }; offset + 12 <= png.size();)
            {
                const std::uint32_t length { (std::uint32_t { png[offset] } << 24)
                                             | (std::uint32_t { png[offset + 1] } << 16)
                                             | (std::uint32_t { png[offset + 2] } << 8) | png[offset + 3]
                };

                const std::string type { reinterpret_cast<const char*>(png.data() + offset + 4), 4 };

                if (type == "gAMA" && length == 4)
                {
                    for (std::size_t index {}; index < 4; ++index)
                    {
                        png[offset + 8 + index] = static_cast<std::uint8_t>(value >> (24 - index * 8));
                    }

                    const std::uint32_t crc { test_crc(std::span<const std::uint8_t> { png }.subspan(offset + 4, 8)) };

                    for (std::size_t index {}; index < 4; ++index)
                    {
                        png[offset + 12 + index] = static_cast<std::uint8_t>(crc >> (24 - index * 8));
                    }

                    write_bytes(path, png);
                    return;
                }

                offset += static_cast<std::size_t>(length) + 12;
            }

            throw std::runtime_error { "WIC PNG fixture did not include gAMA." };
        }

        void make_png_pixels(
            const std::filesystem::path& path,
            const std::span<const std::uint8_t> rgba,
            const int width,
            const int height,
            const bool opaque = false
        )
        {
            const std::size_t channels { opaque ? 3U : 4U };
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * channels);

            for (std::size_t offset {}; offset < pixels.size(); offset += channels)
            {
                const std::size_t source { offset / channels * 4 };
                pixels[offset] = rgba[source + 2];
                pixels[offset + 1] = rgba[source + 1];
                pixels[offset + 2] = rgba[source];

                if (!opaque)
                {
                    pixels[offset + 3] = rgba[source + 3];
                }
            }

            {
                Microsoft::WRL::ComPtr<IWICImagingFactory> factory {};
                Microsoft::WRL::ComPtr<IWICStream> stream {};
                Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder {};
                Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame {};

                check_result(CoCreateInstance(
                    CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())
                ));

                check_result(factory->CreateStream(stream.GetAddressOf()));
                check_result(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
                check_result(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()));
                check_result(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
                check_result(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr));
                check_result(frame->Initialize(nullptr));
                check_result(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)));
                GUID format { opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA };

                const GUID requested_format { format };
                check_result(frame->SetPixelFormat(&format));
                check(
                    IsEqualGUID(format, requested_format) != FALSE, "WIC retains requested RGB8 or RGBA8 pixel format"
                );

                check_result(frame->WritePixels(
                    static_cast<UINT>(height),
                    static_cast<UINT>(static_cast<std::size_t>(width) * channels),
                    static_cast<UINT>(pixels.size()),
                    pixels.data()
                ));

                check_result(frame->Commit());
                check_result(encoder->Commit());
            }

            const auto encoded { read_bytes(path) };
            check(
                encoded.size() > 33 && encoded[24] == 8 && encoded[25] == (opaque ? 2 : 6),
                "WIC PNG IHDR is RGB8 or RGBA8 as requested"
            );
        }

        void make_png(
            const std::filesystem::path& path,
            const std::array<std::uint8_t, 4> color,
            const int width = 3,
            const bool opaque = false,
            const bool comments = false
        )
        {
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * 2 * 4);

            for (std::size_t offset {}; offset < pixels.size(); offset += 4)
            {
                std::copy(color.begin(), color.end(), pixels.begin() + static_cast<std::ptrdiff_t>(offset));
            }

            make_png_pixels(path, pixels, width, 2, opaque);

            if (comments)
            {
                add_text_chunk(path);
            }
        }

        void paint_rectangle(
            std::vector<std::uint8_t>& pixels,
            const int canvas_width,
            const int x,
            const int y,
            const int width,
            const int height,
            const std::array<std::uint8_t, 4> color
        )
        {
            for (int row {}; row < height; ++row)
            {
                for (int column {}; column < width; ++column)
                {
                    const std::size_t offset { (static_cast<std::size_t>(y + row) * canvas_width + x + column) * 4 };
                    std::copy(color.begin(), color.end(), pixels.begin() + static_cast<std::ptrdiff_t>(offset));
                }
            }
        }

        void decode_delta_animation(
            const std::filesystem::path& path,
            const std::span<const std::vector<std::uint8_t>> expected,
            const std::span<const int> durations
        )
        {
            const auto bytes { read_bytes(path) };
            auto codec { SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size())) };
            check(codec != nullptr, "Skia recognizes APNG delta output");

            if (codec == nullptr)
            {
                return;
            }

            check(
                codec->getInfo().width() == 6 && codec->getInfo().height() == 4,
                "delta APNG preserves first frame canvas dimensions"
            );
            check(codec->getFrameCount() == static_cast<int>(expected.size()), "delta APNG frame count round trips");

            const auto frame_info { codec->getFrameInfo() };
            const SkImageInfo info { SkImageInfo::Make(6, 4, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType) };

            for (std::size_t index {}; index < expected.size(); ++index)
            {
                check(
                    index < frame_info.size() && frame_info[index].fDuration == durations[index],
                    "delta APNG frame duration round trips"
                );

                std::vector<std::uint8_t> pixels(6 * 4 * 4);
                SkCodec::Options options {};
                options.fFrameIndex = static_cast<int>(index);
                check(
                    codec->getPixels(info, pixels.data(), 6 * 4, &options) == SkCodec::kSuccess,
                    "Skia reconstructs the full canvas from delta frames"
                );
                check(
                    pixels == expected[index], "moving and erased opaque rectangles round trip every full-canvas pixel"
                );
            }
        }

        void decode_animation(
            const std::filesystem::path& path,
            const std::span<const std::array<std::uint8_t, 4>> colors,
            const std::span<const int> durations
        )
        {
            const auto bytes { read_bytes(path) };
            auto codec { SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size())) };
            check(codec != nullptr, "Skia Rust PNG recognizes APNG output");

            if (codec == nullptr)
            {
                return;
            }

            check(codec->getInfo().width() == 3 && codec->getInfo().height() == 2, "APNG canvas dimensions round trip");
            check(codec->getFrameCount() == static_cast<int>(colors.size()), "APNG frame count round trips");

            if (colors.size() > 1)
            {
                check(codec->getRepetitionCount() == SkCodec::kRepetitionCountInfinite, "APNG loops indefinitely");
            }
            else
            {
                // SkCodec는 한 장을 정지 그림으로 취급하므로 반복 횟수는 실제 acTL에서 확인한다.
                check(
                    bytes.size() >= 49 && bytes[37] == 'a' && bytes[38] == 'c' && bytes[39] == 'T' && bytes[40] == 'L'
                        && bytes[45] == 0 && bytes[46] == 0 && bytes[47] == 0 && bytes[48] == 0,
                    "one frame APNG still carries infinite-loop acTL"
                );
            }

            const auto frame_info { codec->getFrameInfo() };

            for (std::size_t index {}; index < colors.size(); ++index)
            {
                check(
                    index < frame_info.size() && frame_info[index].fDuration == durations[index],
                    "APNG frame duration round trips in milliseconds"
                );

                std::array<std::uint8_t, 24> pixels {};
                const SkImageInfo info { SkImageInfo::Make(3, 2, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType) };
                SkCodec::Options options {};
                options.fFrameIndex = static_cast<int>(index);
                check(
                    codec->getPixels(info, pixels.data(), 12, &options) == SkCodec::kSuccess,
                    "Skia decodes each full SOURCE frame"
                );

                for (std::size_t offset {}; offset < pixels.size(); offset += 4)
                {
                    check(
                        std::equal(
                            colors[index].begin(),
                            colors[index].end(),
                            pixels.begin() + static_cast<std::ptrdiff_t>(offset)
                        ),
                        "APNG frame color and alpha round trip exactly"
                    );
                }
            }
        }

        void expect_failure(
            const std::filesystem::path& output,
            const std::span<const encoded_animation_frame> frames,
            const std::atomic<bool>& cancelled,
            const char* name
        )
        {
            std::u8string error {};
            check(!write_apng(output, frames, cancelled, error), name);
            check(!error.empty(), "APNG failure explains its cause");
            check(!std::filesystem::exists(output), "APNG failure leaves no partial output");
        }

        int run_tests()
        {
            const auto nonce { std::chrono::steady_clock::now().time_since_epoch().count() };
            test_files files { std::filesystem::current_path() / "build" / "apng-writer-tests"
                               / std::to_string(nonce)
            };

            std::filesystem::create_directories(files.directory);
            const std::array<std::array<std::uint8_t, 4>, 3> colors {
                std::array<std::uint8_t, 4> { 255, 0, 0, 255 },
                std::array<std::uint8_t, 4> { 0, 0, 255, 255 },
                std::array<std::uint8_t, 4> { 0, 255, 0, 128 },
            };

            std::vector<encoded_animation_frame> frames {};
            const std::array<int, 3> durations { 37, 123, 250 };

            for (std::size_t index {}; index < colors.size(); ++index)
            {
                const auto path { files.directory / (std::to_string(index) + ".png") };
                make_png(path, colors[index]);
                frames.push_back({ path, durations[index] / 1000.0 });
            }

            std::atomic<bool> cancelled {};
            std::u8string error {};
            const auto output { files.directory / "animation.png" };
            check(write_apng(output, frames, cancelled, error), "native APNG mux succeeds");

            if (!error.empty())
            {
                std::cerr << reinterpret_cast<const char*>(error.c_str()) << '\n';
            }

            check(error.empty(), "successful APNG mux has no error");
            decode_animation(output, colors, durations);

            const auto before { read_bytes(output) };
            check(!write_apng(output, frames, cancelled, error), "existing output is protected");
            check(read_bytes(output) == before, "existing output remains unchanged");

            const auto single_output { files.directory / "single.png" };
            check(
                write_apng(
                    single_output, std::span<const encoded_animation_frame> { frames }.first(1), cancelled, error
                ),
                "one frame APNG is valid"
            );
            decode_animation(
                single_output,
                std::span<const std::array<std::uint8_t, 4>> { colors }.first(1),
                std::span<const int> { durations }.first(1)
            );

            const std::vector<encoded_animation_frame> sixteen_frames(16, { frames.front().path, 0.0625 });
            const std::vector<std::array<std::uint8_t, 4>> sixteen_colors(16, colors.front());
            std::vector<int> sixteen_durations {};

            for (int index {}; index < 16; ++index)
            {
                sixteen_durations.push_back(index % 2 == 0 ? 63 : 62);
            }

            const auto sixteen_output { files.directory / "sixteen-fps.png" };
            check(write_apng(sixteen_output, sixteen_frames, cancelled, error), "16 FPS timing mux succeeds");
            decode_animation(sixteen_output, sixteen_colors, sixteen_durations);

            const auto sixteen_bytes { read_bytes(sixteen_output) };
            auto sixteen_codec {
                SkCodec::MakeFromData(SkData::MakeWithCopy(sixteen_bytes.data(), sixteen_bytes.size()))
            };
            int total_duration {};

            if (sixteen_codec != nullptr)
            {
                for (const auto& info : sixteen_codec->getFrameInfo())
                {
                    total_duration += info.fDuration;
                }
            }

            check(total_duration == 1000, "16 FPS cumulative durations total exactly one second");

            const std::array<std::uint8_t, 4> background { 12, 24, 36, 255 };
            const std::array<std::uint8_t, 4> rectangle { 240, 80, 16, 255 };
            std::vector<std::uint8_t> canvas(6 * 4 * 4);
            paint_rectangle(canvas, 6, 0, 0, 6, 4, background);
            paint_rectangle(canvas, 6, 1, 1, 2, 2, rectangle);

            const auto delta_first { files.directory / "delta-first.png" };
            make_png_pixels(delta_first, canvas, 6, 4);

            std::vector<std::vector<std::uint8_t>> expected_delta { canvas };
            paint_rectangle(canvas, 6, 1, 1, 2, 2, background);
            paint_rectangle(canvas, 6, 3, 1, 2, 2, rectangle);
            expected_delta.push_back(canvas);

            std::vector<std::uint8_t> moved_tile(4 * 2 * 4);
            paint_rectangle(moved_tile, 4, 0, 0, 4, 2, background);
            paint_rectangle(moved_tile, 4, 2, 0, 2, 2, rectangle);

            const auto delta_moved { files.directory / "delta-moved.png" };
            make_png_pixels(delta_moved, moved_tile, 4, 2);
            paint_rectangle(canvas, 6, 3, 1, 2, 2, background);
            expected_delta.push_back(canvas);

            std::vector<std::uint8_t> erased_tile(2 * 2 * 4);
            paint_rectangle(erased_tile, 2, 0, 0, 2, 2, background);

            const auto delta_erased { files.directory / "delta-erased.png" };
            make_png_pixels(delta_erased, erased_tile, 2, 2);
            paint_rectangle(canvas, 6, 5, 3, 1, 1, colors[1]);
            expected_delta.push_back(canvas);

            const auto delta_corner { files.directory / "delta-corner.png" };
            make_png_pixels(delta_corner, colors[1], 1, 1);

            std::vector<encoded_animation_frame> delta_frames { { delta_first, 0.037 },
                                                                { delta_moved, 0.123, 1, 1 },
                                                                { delta_erased, 0.250, 3, 1 },
                                                                { delta_corner, 0.062, 5, 3 }
            };

            const std::array<int, 4> delta_durations { 37, 123, 250, 62 };
            const auto delta_output { files.directory / "delta-animation.png" };
            check(write_apng(delta_output, delta_frames, cancelled, error), "APNG delta rectangles mux successfully");

            if (!error.empty())
            {
                std::cerr << reinterpret_cast<const char*>(error.c_str()) << '\n';
            }

            decode_delta_animation(delta_output, expected_delta, delta_durations);

            auto invalid_delta { delta_frames };
            invalid_delta.front().x = 1;
            expect_failure(
                files.directory / "first-offset.png", invalid_delta, cancelled, "first APNG frame offset is rejected"
            );
            invalid_delta = delta_frames;
            invalid_delta[1].x = -1;
            expect_failure(
                files.directory / "negative-offset.png",
                invalid_delta,
                cancelled,
                "negative APNG delta offset is rejected"
            );
            invalid_delta[1].x = std::numeric_limits<int>::max();
            expect_failure(
                files.directory / "overflow-offset.png",
                invalid_delta,
                cancelled,
                "large APNG delta offset is rejected without overflow"
            );
            invalid_delta[1].x = 3;
            expect_failure(
                files.directory / "right-overflow.png",
                invalid_delta,
                cancelled,
                "APNG delta width extending past canvas is rejected"
            );
            invalid_delta = delta_frames;
            invalid_delta[1].y = 3;
            expect_failure(
                files.directory / "bottom-overflow.png",
                invalid_delta,
                cancelled,
                "APNG delta height extending past canvas is rejected"
            );
            invalid_delta[1].y = -1;
            expect_failure(
                files.directory / "negative-y.png", invalid_delta, cancelled, "negative APNG delta y offset is rejected"
            );

            auto corrupt { read_bytes(frames.front().path) };
            corrupt.back() ^= 1;

            const auto corrupt_path { files.directory / "bad-crc.png" };
            write_bytes(corrupt_path, corrupt);

            std::vector<encoded_animation_frame> bad_frames { frames };
            bad_frames[1].path = corrupt_path;
            expect_failure(
                files.directory / "bad-crc-output.png", bad_frames, cancelled, "corrupt later-frame CRC is rejected"
            );
            corrupt = read_bytes(frames.front().path);
            corrupt.resize(corrupt.size() - 3);

            const auto truncated { files.directory / "truncated.png" };
            write_bytes(truncated, corrupt);
            bad_frames[1].path = truncated;
            expect_failure(
                files.directory / "truncated-output.png", bad_frames, cancelled, "truncated chunk is rejected"
            );

            const auto wrong_size { files.directory / "wrong-size.png" };
            make_png(wrong_size, colors[0], 4);
            bad_frames[1].path = wrong_size;
            expect_failure(
                files.directory / "wrong-size-output.png", bad_frames, cancelled, "mismatched canvas is rejected"
            );

            const auto opaque { files.directory / "opaque.png" };
            make_png(opaque, colors[0], 3, true);
            bad_frames[1].path = opaque;
            expect_failure(
                files.directory / "wrong-format-output.png",
                bad_frames,
                cancelled,
                "mismatched RGB and RGBA headers are rejected"
            );

            std::vector<encoded_animation_frame> rgb_frames { { opaque, 0.125 }, { opaque, 0.250 } };
            check(
                write_apng(files.directory / "rgb.png", rgb_frames, cancelled, error),
                "matching opaque RGB8 frames are supported"
            );

            const auto metadata { files.directory / "metadata.png" };
            make_png(metadata, colors[0], 3, false, true);
            bad_frames[1].path = metadata;
            expect_failure(
                files.directory / "metadata-output.png",
                bad_frames,
                cancelled,
                "unsupported metadata is rejected explicitly"
            );

            const auto gamma_mismatch { files.directory / "gamma-mismatch.png" };
            make_png(gamma_mismatch, colors[0]);
            change_gamma(gamma_mismatch, 50000);
            bad_frames[1].path = gamma_mismatch;
            expect_failure(
                files.directory / "gamma-mismatch-output.png",
                bad_frames,
                cancelled,
                "mismatched frame gAMA values are rejected"
            );
            change_gamma(gamma_mismatch, 0);
            expect_failure(
                files.directory / "gamma-zero-output.png", bad_frames, cancelled, "zero gAMA with valid CRC is rejected"
            );

            bad_frames = frames;
            bad_frames[1].duration_seconds = 65.537;
            expect_failure(
                files.directory / "long-delay.png",
                bad_frames,
                cancelled,
                "unrepresentable delay is rejected without truncation"
            );
            bad_frames[1].duration_seconds = std::numeric_limits<double>::infinity();
            expect_failure(
                files.directory / "infinite-delay.png", bad_frames, cancelled, "nonfinite delay is rejected"
            );
            bad_frames[1].duration_seconds = 0.0;
            expect_failure(files.directory / "zero-delay.png", bad_frames, cancelled, "zero delay is rejected");
            cancelled.store(true);
            expect_failure(files.directory / "cancelled.png", frames, cancelled, "cancelled APNG mux is rejected");
            cancelled.store(false);
            expect_failure(files.directory / "empty.png", {}, cancelled, "empty animation is rejected");
            return failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

    } // namespace
} // namespace chalkag

int main()
{
    const HRESULT com_result { CoInitializeEx(nullptr, COINIT_MULTITHREADED) };

    if (FAILED(com_result))
    {
        std::cerr << "FAIL: cannot initialize WIC test COM apartment.\n";
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
        const int result { chalkag::run_tests() };

        if (result == EXIT_SUCCESS)
        {
            std::cout << "Native APNG writer tests passed.\n";
        }

        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
