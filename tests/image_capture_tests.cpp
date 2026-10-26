#include "capture/image_capture.h"
#include "capture/native_webp_writer.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"

#include <dwmapi.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace chalkag {
    namespace {
        int failures {};

        void check(bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failures;
            }
        }

        void check_png(const std::filesystem::path& path)
        {
            const auto com { CoInitializeEx(nullptr, COINIT_MULTITHREADED) };
            Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            UINT width {};
            UINT height {};
            bool decoded { SUCCEEDED(
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))
            )
            };

            if (decoded)
            {
                decoded = SUCCEEDED(factory->CreateDecoderFromFilename(
                    path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder
                ));
            }

            if (decoded)
            {
                decoded = SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height));
            }

            check(decoded && width == 48 && height == 32, "PNG scales to exact output dimensions");

            if (decoded)
            {
                decoded = SUCCEEDED(factory->CreateFormatConverter(&converter))
                    && SUCCEEDED(converter->Initialize(
                        frame.Get(),
                        GUID_WICPixelFormat32bppBGRA,
                        WICBitmapDitherTypeNone,
                        nullptr,
                        0,
                        WICBitmapPaletteTypeCustom
                    ));
                BYTE pixel[4] {};
                WICRect rectangle { 24, 16, 1, 1 };
                check(
                    decoded && SUCCEEDED(converter->CopyPixels(&rectangle, 4, 4, pixel)) && pixel[0] == 160
                        && pixel[1] == 96 && pixel[2] == 32 && pixel[3] == 255,
                    "PNG contains opaque fixture color"
                );
            }

            converter.Reset();
            frame.Reset();
            decoder.Reset();
            factory.Reset();

            if (SUCCEEDED(com))
            {
                CoUninitialize();
            }
        }

        std::unique_ptr<SkCodec> open_codec(const std::filesystem::path& path)
        {
            std::ifstream file { path, std::ios::binary | std::ios::ate };
            const auto size { static_cast<std::streamoff>(file.tellg()) };

            if (size <= 0)
            {
                return nullptr;
            }

            std::vector<char> bytes(static_cast<std::size_t>(size));
            file.seekg(0);
            file.read(bytes.data(), size);

            if (!file)
            {
                return nullptr;
            }

            return SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size()));
        }

        void native_checks(const std::filesystem::path& directory)
        {
            const auto fixture { directory / L"native-fixtures" };
            std::filesystem::create_directories(fixture);
            SkBitmap bitmap;
            check(
                bitmap.tryAllocPixels(SkImageInfo::Make(48, 32, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)),
                "allocate synthetic pixels"
            );
            bitmap.eraseColor(SkColorSetRGB(32, 96, 160));

            const auto first { fixture / L"first.webp" };
            check(encode_native_image(first, bitmap.pixmap(), image_format::webp), "native WebP needs no FFmpeg");

            const auto png { fixture / L"native.png" };
            check(encode_native_image(png, bitmap.pixmap(), image_format::png), "Windows PNG encoder is available");
            check_png(png);
            bitmap.eraseColor(SkColorSetRGB(224, 64, 16));

            const auto second { fixture / L"second.webp" };
            check(encode_native_image(second, bitmap.pixmap(), image_format::webp), "encode second native frame");

            for (const int fps : { 10, 16, 33 })
            {
                const auto output { fixture / (L"animation_" + std::to_wstring(fps) + L".webp") };
                native_webp_writer writer { output, 48, 32 };

                const auto duration { static_cast<std::uint32_t>(1000.0 / fps + 0.5) };
                check(
                    writer.append_frame(first, duration) && writer.append_frame(second, duration) && writer.finish(),
                    "native animation mux succeeds"
                );

                auto codec { open_codec(output) };
                check(codec && codec->getFrameCount() == 2, "WebP animation decodes two frames");

                if (codec)
                {
                    const auto frames { codec->getFrameInfo() };
                    check(
                        frames.size() == 2 && frames[0].fDuration == static_cast<int>(duration)
                            && frames[1].fDuration == static_cast<int>(duration),
                        "millisecond frame timing roundtrip"
                    );

                    SkBitmap decoded;
                    check(
                        decoded.tryAllocPixels(codec->getInfo().makeColorType(kBGRA_8888_SkColorType)),
                        "allocate decoded frame"
                    );

                    for (int index {}; index < 2; ++index)
                    {
                        SkCodec::Options options {};
                        options.fFrameIndex = index;
                        check(
                            codec->getPixels(decoded.info(), decoded.getPixels(), decoded.rowBytes(), &options)
                                == SkCodec::kSuccess,
                            "decode independent native frame"
                        );

                        const auto pixel { decoded.getColor(24, 16) };
                        check(
                            pixel == (index ? SkColorSetRGB(224, 64, 16) : SkColorSetRGB(32, 96, 160)),
                            "lossless color roundtrip"
                        );
                    }
                }
            }

            native_webp_writer mismatch { fixture / L"mismatch.webp", 47, 32 };
            check(!mismatch.append_frame(first, 30) && !mismatch.finish(), "mismatched canvas rejected");
            native_webp_writer zero { fixture / L"zero.webp", 48, 32 };
            check(!zero.append_frame(first, 0), "zero duration rejected");

            SkPixmap corner;
            check(
                bitmap.pixmap().extractSubset(&corner, SkIRect::MakeXYWH(46, 30, 2, 2)),
                "extract bottom-right tile with padded stride"
            );

            const auto corner_png { fixture / L"corner.png" };
            const auto corner_webp { fixture / L"corner.webp" };
            check(
                encode_native_image(corner_png, corner, image_format::png),
                "WIC accepts exact final-row buffer size for cropped tile"
            );
            check(encode_native_image(corner_webp, corner, image_format::webp), "encode native WebP corner tile");

            const auto delta_output { fixture / L"delta.webp" };
            native_webp_writer delta { delta_output, 48, 32 };
            check(
                delta.append_frame(first, 20) && delta.append_frame(corner_webp, 30, 46, 30) && delta.finish(),
                "WebP bounded delta tile mux succeeds"
            );

            auto delta_codec { open_codec(delta_output) };
            check(delta_codec && delta_codec->getFrameCount() == 2, "WebP delta decodes two frames");

            if (delta_codec)
            {
                SkBitmap decoded;
                check(
                    decoded.tryAllocPixels(delta_codec->getInfo().makeColorType(kBGRA_8888_SkColorType)),
                    "allocate delta canvas"
                );

                SkCodec::Options frame_options {};
                frame_options.fFrameIndex = 1;
                check(
                    delta_codec->getPixels(decoded.info(), decoded.getPixels(), decoded.rowBytes(), &frame_options)
                        == SkCodec::kSuccess,
                    "decode composited WebP delta"
                );
                check(
                    decoded.getColor(47, 31) == SkColorSetRGB(224, 64, 16)
                        && decoded.getColor(0, 0) == SkColorSetRGB(32, 96, 160),
                    "WebP delta preserves untouched pixels"
                );
            }

            native_webp_writer odd { fixture / L"odd.webp", 48, 32 };
            check(
                odd.append_frame(first, 20) && !odd.append_frame(corner_webp, 30, 45, 30),
                "WebP odd tile offset rejected"
            );
            native_webp_writer outside { fixture / L"outside.webp", 48, 32 };
            check(
                outside.append_frame(first, 20) && !outside.append_frame(corner_webp, 30, 48, 30),
                "WebP outside tile rejected"
            );
            native_webp_writer missing_canvas { fixture / L"missing-canvas.webp", 48, 32 };
            check(
                !missing_canvas.append_frame(corner_webp, 30, 46, 30), "WebP first frame must initialize full canvas"
            );

            std::atomic<bool> cancelled { true };
            native_webp_writer cancelled_writer { fixture / L"cancelled.webp", 48, 32, &cancelled };
            check(!cancelled_writer.append_frame(first, 30), "native mux cancellation prevents writing a frame");

            SkBitmap wide_png;
            check(
                wide_png.tryAllocPixels(SkImageInfo::Make(18000, 16, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)),
                "allocate PNG wider than WebP maximum"
            );
            wide_png.eraseColor(SkColorSetRGB(32, 96, 160));

            const auto wide_png_path { fixture / L"wide.png" };
            check(
                encode_native_image(wide_png_path, wide_png.pixmap(), image_format::png),
                "PNG remains available beyond WebP axis limit"
            );

            auto wide_png_codec { open_codec(wide_png_path) };
            check(wide_png_codec && wide_png_codec->getInfo().width() == 18000, "wide PNG decodes at original width");

            for (const auto dimensions : { std::pair { 640, 360 }, std::pair { 1920, 1080 } })
            {
                SkBitmap benchmark;
                check(
                    benchmark.tryAllocPixels(
                        SkImageInfo::Make(
                            dimensions.first, dimensions.second, kBGRA_8888_SkColorType, kOpaque_SkAlphaType
                        )
                    ),
                    "allocate benchmark pixels"
                );
                auto* pixels { static_cast<std::uint32_t*>(benchmark.getPixels()) };

                for (int y {}; y < dimensions.second; ++y)
                {
                    for (int x {}; x < dimensions.first; ++x)
                    {
                        pixels[static_cast<std::size_t>(y) * dimensions.first + x] = 0xff000000u
                            | (static_cast<std::uint32_t>((x * 7 + y * 3) & 255) << 16)
                            | (static_cast<std::uint32_t>((x / 8 + y * 11) & 255) << 8)
                            | static_cast<std::uint32_t>((x * 3 + y / 4) & 255);
                    }
                }

                for (const auto format : { image_format::png, image_format::webp })
                {
                    const auto output { fixture
                                        / (L"benchmark_" + std::to_wstring(dimensions.first)
                                           + (format == image_format::png ? L".png" : L".webp"))
                    };

                    const auto start { std::chrono::steady_clock::now() };
                    check(encode_native_image(output, benchmark.pixmap(), format), "native benchmark encode");

                    const auto milliseconds {
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                    };

                    std::cout << "Native " << dimensions.first << 'x' << dimensions.second
                              << (format == image_format::png ? " PNG " : " WebP ") << milliseconds << " ms\n";
                }
            }
        }

        struct synthetic_window
        {
            HBRUSH brush { CreateSolidBrush(RGB(32, 96, 160)) };
            HWND handle {};

            synthetic_window()
            {
                WNDCLASSW window_class {};
                window_class.lpfnWndProc = DefWindowProcW;
                window_class.hInstance = GetModuleHandleW(nullptr);
                window_class.hbrBackground = brush;
                window_class.lpszClassName = L"Chalkag.Capture.Test.Fixture";
                RegisterClassW(&window_class);
                handle = CreateWindowExW(
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                    window_class.lpszClassName,
                    L"Capture test fixture",
                    WS_POPUP,
                    100,
                    100,
                    256,
                    192,
                    nullptr,
                    nullptr,
                    window_class.hInstance,
                    nullptr
                );
                ShowWindow(handle, SW_SHOWNOACTIVATE);
                UpdateWindow(handle);
                DwmFlush();
            }

            ~synthetic_window()
            {
                DestroyWindow(handle);
                UnregisterClassW(L"Chalkag.Capture.Test.Fixture", GetModuleHandleW(nullptr));
                DeleteObject(brush);
            }

            capture_region region() const
            {
                POINT point { 32, 32 };
                ClientToScreen(handle, &point);
                return { point.x, point.y, 96, 64 };
            }
        };

        void run_checks(const std::filesystem::path& directory, bool screen)
        {
            image_capture capture { directory };
            check(!capture.recording(), "initial recording state");
            check(!capture.recording_failed(), "initial worker failure state");
            check(!capture.stop_recording().success, "stop without a recording");

            capture_options options {};
            check(!capture.take_screenshot({}, options).success, "empty region rejected");

            capture_options invalid_compression {};
            invalid_compression.compression = static_cast<lossless_compression>(99);
            check(
                !capture.take_screenshot({ 0, 0, 16, 16 }, invalid_compression).success,
                "invalid screenshot compression rejects before capture"
            );
            check(
                !capture.start_recording({ 0, 0, 16, 16 }, invalid_compression).success,
                "invalid recording compression rejects before capture"
            );
            options.output_percent = 0;
            check(!capture.take_screenshot({ 0, 0, 96, 64 }, options).success, "zero scale rejected");
            options.output_percent = 50;
            options.fps = 0;
            check(!capture.start_recording({ 0, 0, 96, 64 }, options).success, "zero FPS rejected");
            options.fps = 16;
            options.output_percent = 200;
            options.still_format = image_format::webp;

            const auto wide_still { capture.take_screenshot({ 0, 0, 9000, 16 }, options) };
            check(
                !wide_still.success && wide_still.message.find(u8"16383") != std::u8string::npos,
                "scaled WebP width rejected before capture"
            );

            const auto tall_recording { capture.start_recording({ 0, 0, 16, 9000 }, options) };
            check(
                !tall_recording.success && tall_recording.message.find(u8"16383") != std::u8string::npos
                    && !capture.recording(),
                "scaled WebP height rejected before recording starts"
            );
            options.output_percent = 50;
            options.animation_format = image_format::gif;
            check(!capture.has_pending_recording(), "initial draft is empty for native GIF");
            options.animation_format = image_format::webp;

            if (screen)
            {
                synthetic_window fixture {};
                check(fixture.handle != nullptr, "synthetic capture window created");

                const auto region { fixture.region() };
                options.still_format = image_format::png;

                const auto result { capture.take_screenshot(region, options) };
                check(result.success, "native screenshot succeeds");

                if (result.success)
                {
                    check_png(result.output);
                }

                const HWND cover = CreateWindowExW(
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                    L"STATIC",
                    L"",
                    WS_POPUP | SS_BLACKRECT,
                    region.x,
                    region.y,
                    region.width,
                    region.height,
                    nullptr,
                    nullptr,
                    GetModuleHandleW(nullptr),
                    nullptr
                );
                check(
                    cover && SetWindowDisplayAffinity(cover, WDA_EXCLUDEFROMCAPTURE), "fixture exclusion is available"
                );
                ShowWindow(cover, SW_SHOWNOACTIVATE);
                UpdateWindow(cover);
                DwmFlush();

                const auto excluded { capture.take_screenshot(region, options) };
                check(excluded.success, "screenshot with excluded overlay succeeds");

                if (excluded.success)
                {
                    check_png(excluded.output);
                }

                DestroyWindow(cover);
                DwmFlush();
                {
                    options.still_format = image_format::webp;
                    check(capture.take_screenshot(region, options).success, "WebP screenshot succeeds");

                    for (const auto format : { image_format::apng, image_format::webp })
                    {
                        options.animation_format = format;

                        for (const int fps : { 10, 16, 33 })
                        {
                            options.fps = fps;

                            const auto started { capture.start_recording(region, options) };
                            check(started.success && capture.recording(), "recording starts");

                            std::this_thread::sleep_for(std::chrono::milliseconds { 350 });
                            const auto held { capture.stop_recording() };
                            check(
                                held.success && held.output.empty() && capture.has_pending_recording(),
                                "stop holds raw frames without saving"
                            );

                            capture_options invalid { options };
                            invalid.webp_quality = 0;
                            check(
                                !capture.save_recording(invalid).success && capture.has_pending_recording(),
                                "failed export preserves draft"
                            );

                            const auto stopped { capture.save_recording(options) };

                            if (!stopped.success)
                            {
                                std::cerr << reinterpret_cast<const char*>(stopped.message.c_str()) << '\n';
                            }
                            else
                            {
                                std::wcout << fps << L" FPS output: " << stopped.output.wstring() << L'\n';
                            }

                            check(
                                stopped.success && !capture.recording() && capture.has_pending_recording(),
                                "save retains raw recording for another export"
                            );
                            options.animation_format = image_format::gif;
                            options.output_percent = 25;

                            const auto resaved { capture.save_recording(options) };
                            check(
                                resaved.success && resaved.output != stopped.output,
                                "resave same draft as downscaled native GIF"
                            );
                            options.animation_format = format;
                            options.output_percent = 50;
                            capture.discard_recording();
                            check(!capture.has_pending_recording(), "discard allows the next recording");
                        }
                    }
                }

                check(std::filesystem::is_empty(directory / L"data" / L"temp"), "session temporary files removed");
            }

            capture.request_cancel();
            check(
                !capture.take_screenshot({ 0, 0, 96, 64 }, options).success,
                "shutdown cancellation prevents a new capture"
            );
        }

        void check_retained_still(const std::filesystem::path& directory)
        {
            std::atomic<int> supplied {};
            image_capture capture {
                directory,
                [&](const capture_region region, const capture_options options, SkBitmap& bitmap) {
                    const int width { region.width * options.output_percent / 100 };
                    const int height { region.height * options.output_percent / 100 };
                    if (!bitmap.tryAllocPixels(
                            SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)
                        ))
                    {
                        return false;
                    }
                    bitmap.eraseColor(SkColorSetRGB(35, 72, 110));
                    ++supplied;
                    return true;
                }
            };
            capture_options options {};
            check(capture.start_recording({ 0, 0, 96, 64 }, options).success, "synthetic recording starts");
            const auto deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 5 } };
            while (supplied.load() == 0 && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
            }

            check(capture.stop_recording().success, "synthetic recording is retained");
            const auto source { capture.snapshot() };
            check(!source.frames.empty(), "synthetic source contains frames");
            if (source.frames.empty())
            {
                return;
            }
            const auto photo { capture.take_screenshot({ 0, 0, 48, 32 }, options) };
            check(photo.success && capture.has_pending_recording(), "still capture preserves retained recording");
            check(capture.recorded_region() == source.region, "still capture leaves recording geometry unchanged");
            check(capture.snapshot().frames.size() == source.frames.size(), "still cleanup preserves source frames");
            check(std::filesystem::exists(source.frames.front().path), "still cleanup preserves source storage");
            capture_options invalid { options };
            invalid.output_percent = 0;
            check(
                !capture.take_screenshot({ 0, 0, 48, 32 }, invalid).success && capture.has_pending_recording(),
                "failed still capture preserves retained source"
            );
            const auto cancel_still { capture.take_screenshot({ 0, 0, 48, 32 }, options, [&](save_progress progress) {
                if (progress.stage == save_stage::preparing)
                {
                    capture.request_save_cancel();
                }
            })
            };
            check(
                !cancel_still.success && capture.has_pending_recording(), "cancelled still capture preserves raw source"
            );
            capture.reset_save_cancel();
            check(capture.take_screenshot({ 0, 0, 48, 32 }, options).success, "new still works after cancellation");
            const auto cancelled { capture.save_recording(options, [&](save_progress progress) {
                if (progress.stage == save_stage::preparing)
                {
                    capture.request_save_cancel();
                }
            })
            };
            check(!cancelled.success && capture.has_pending_recording(), "cancelled export preserves raw source");
            capture.reset_save_cancel();
            check(capture.save_recording(options).success, "retained source exports after cancelled export");
            check(capture.snapshot().frames.size() == source.frames.size(), "resaving preserves original frame count");
            capture.discard_recording();
            const int before_next { supplied.load() };
            const auto next_deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 5 } };
            check(
                capture.start_recording({ 0, 0, 48, 32 }, options).success,
                "new recording starts after retained still and export"
            );
            while (supplied.load() <= before_next && std::chrono::steady_clock::now() < next_deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
            }
            capture.request_save_cancel();
            check(
                capture.stop_recording().success && capture.has_pending_recording(),
                "save cancellation during finalization keeps raw source"
            );
            capture.reset_save_cancel();
            capture.request_cancel();
            capture.reset_save_cancel();
            check(
                !capture.take_screenshot({ 0, 0, 48, 32 }, options).success,
                "save cancellation reset never clears shutdown"
            );
        }

    } // namespace
} // namespace chalkag

int wmain(int argument_count, wchar_t* arguments[])
{
    const auto directory { argument_count > 1 ? std::filesystem::path { arguments[1] }
                                              : std::filesystem::current_path()
                                   / (L"capture_test_" + std::to_wstring(GetCurrentProcessId()))
    };

    const bool screen { argument_count > 2 && std::wstring { arguments[2] } == L"--screen" };
    chalkag::run_checks(directory, screen);
    chalkag::native_checks(directory);
    chalkag::check_retained_still(directory / L"retained-still");

    if (!chalkag::failures)
    {
        std::cout << "Image capture checks passed.\n";
    }

    return chalkag::failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
