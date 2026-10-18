#include "capture/image_capture.h"

#include "capture/animation_export.h"
#include "capture/animation_recorder.h"
#include "capture/native_webp_writer.h"
#include "encoding/image_encoder.h"
#include "include/core/SkPixmap.h"
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace chalkag {
    namespace {
        using steady_clock = std::chrono::steady_clock;

        struct screen_bitmap
        {
            HDC screen { GetDC(nullptr) };
            HDC memory { screen ? CreateCompatibleDC(screen) : nullptr };
            HBITMAP bitmap {};
            HGDIOBJ previous {};
            void* pixels {};

            ~screen_bitmap()
            {
                if (previous)
                {
                    SelectObject(memory, previous);
                }

                if (bitmap)
                {
                    DeleteObject(bitmap);
                }

                if (memory)
                {
                    DeleteDC(memory);
                }

                if (screen)
                {
                    ReleaseDC(nullptr, screen);
                }
            }

            bool create(int width, int height)
            {
                if (!screen || !memory)
                {
                    return false;
                }

                BITMAPINFO info {};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = width;
                info.bmiHeader.biHeight = -height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);

                if (!bitmap)
                {
                    return false;
                }

                previous = SelectObject(memory, bitmap);
                return previous && previous != HGDI_ERROR;
            }
        };

        capture_result failure(std::u8string message)
        {
            return { false, std::move(message), {} };
        }

        bool valid_options(capture_region region, capture_options options)
        {
            const auto pixels { static_cast<std::int64_t>(region.width) * region.height };

            if (!region.valid() || pixels > 33554432 || options.fps < 1 || options.fps > 60
                || options.output_percent < 1 || options.output_percent > 200)
            {
                return false;
            }

            if (options.webp_quality < 1 || options.webp_quality > 100
                || (options.compression != lossless_compression::fast
                    && options.compression != lossless_compression::compact
                    && options.compression != lossless_compression::maximum))
            {
                return false;
            }

            const auto width {
                std::max<std::int64_t>(1, static_cast<std::int64_t>(region.width) * options.output_percent / 100)
            };

            const auto height {
                std::max<std::int64_t>(1, static_cast<std::int64_t>(region.height) * options.output_percent / 100)
            };
            return width * height <= 33554432;
        }

        bool webp_dimensions_supported(capture_region region, capture_options options)
        {
            const auto width {
                std::max<std::int64_t>(1, static_cast<std::int64_t>(region.width) * options.output_percent / 100)
            };

            const auto height {
                std::max<std::int64_t>(1, static_cast<std::int64_t>(region.height) * options.output_percent / 100)
            };
            return width <= 16383 && height <= 16383;
        }

        std::int64_t clock_nanoseconds(steady_clock::time_point time) noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
        }

        class screen_frame
        {
        public:
            bool initialize(capture_region region, capture_options options)
            {
                region_ = region;
                options_ = options;
                width_ = static_cast<int>(
                    std::max<std::int64_t>(1, static_cast<std::int64_t>(region.width) * options.output_percent / 100)
                );
                height_ = static_cast<int>(
                    std::max<std::int64_t>(1, static_cast<std::int64_t>(region.height) * options.output_percent / 100)
                );
                scaled_ = width_ != region.width || height_ != region.height;
                return original_.create(region.width, region.height) && (!scaled_ || resized_.create(width_, height_));
            }

            bool read()
            {
                if (!BitBlt(
                        original_.memory,
                        0,
                        0,
                        region_.width,
                        region_.height,
                        original_.screen,
                        region_.x,
                        region_.y,
                        SRCCOPY | CAPTUREBLT
                    ))
                {
                    return false;
                }

                if (options_.include_cursor)
                {
                    CURSORINFO cursor { sizeof(CURSORINFO) };

                    if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING))
                    {
                        ICONINFO icon {};

                        if (GetIconInfo(cursor.hCursor, &icon))
                        {
                            DrawIconEx(
                                original_.memory,
                                cursor.ptScreenPos.x - region_.x - static_cast<int>(icon.xHotspot),
                                cursor.ptScreenPos.y - region_.y - static_cast<int>(icon.yHotspot),
                                cursor.hCursor,
                                0,
                                0,
                                0,
                                nullptr,
                                DI_NORMAL
                            );

                            if (icon.hbmColor)
                            {
                                DeleteObject(icon.hbmColor);
                            }

                            if (icon.hbmMask)
                            {
                                DeleteObject(icon.hbmMask);
                            }
                        }
                    }
                }

                if (scaled_)
                {
                    SetStretchBltMode(resized_.memory, HALFTONE);
                    SetBrushOrgEx(resized_.memory, 0, 0, nullptr);

                    if (!StretchBlt(
                            resized_.memory,
                            0,
                            0,
                            width_,
                            height_,
                            original_.memory,
                            0,
                            0,
                            region_.width,
                            region_.height,
                            SRCCOPY
                        ))
                    {
                        return false;
                    }
                }

                if (!GdiFlush())
                {
                    return false;
                }

                const auto pixel_count { static_cast<std::size_t>(width_) * height_ };
                auto* bytes { static_cast<unsigned char*>(scaled_ ? resized_.pixels : original_.pixels) };

                for (std::size_t index {}; index < pixel_count; ++index)
                {
                    bytes[index * 4 + 3] = 255;
                }

                return true;
            }

            SkPixmap pixels() const
            {
                return { SkImageInfo::Make(width_, height_, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
                         scaled_ ? resized_.pixels : original_.pixels,
                         static_cast<std::size_t>(width_) * 4
                };
            }

        private:
            screen_bitmap original_ {};
            screen_bitmap resized_ {};

            capture_region region_ {};
            capture_options options_ {};
            int width_ {};
            int height_ {};
            bool scaled_ {};
        };

        bool save_native(
            const std::filesystem::path& path,
            capture_region region,
            capture_options options,
            image_format format,
            const std::atomic<bool>& cancelled,
            const progress_callback& progress
        )
        {
            screen_frame frame {};

            if (cancelled.load() || !frame.initialize(region, options) || !frame.read())
            {
                return false;
            }

            report_progress(progress, save_stage::encoding);
            return encode_native_image(
                path,
                frame.pixels(),
                format,
                options.webp_lossless ? lossless_compression::fast : options.compression,
                options.webp_lossless,
                options.webp_quality,
                &cancelled
            );
        }

        std::wstring unique_name()
        {
            static std::atomic<unsigned long> sequence {};
            SYSTEMTIME time {};
            GetLocalTime(&time);
            wchar_t name[96] {};
            swprintf_s(
                name,
                L"capture_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%lu",
                time.wYear,
                time.wMonth,
                time.wDay,
                time.wHour,
                time.wMinute,
                time.wSecond,
                time.wMilliseconds,
                GetCurrentProcessId(),
                sequence.fetch_add(1)
            );
            return name;
        }

    } // namespace

    struct image_capture::implementation
    {
        std::filesystem::path directory;
        std::filesystem::path session;
        std::shared_ptr<void> storage {};
        std::filesystem::path output;
        capture_options options;
        std::atomic<bool> stop_requested {};
        std::atomic<std::int64_t> stop_nanoseconds {};
        std::atomic<bool> cancelled {};
        std::atomic<bool> failed {};
        std::thread worker;
        std::mutex mutex;
        std::condition_variable wake;
        std::u8string error;
        std::size_t frame_count {};
        double elapsed {};

        std::atomic<bool> pending {};
        capture_region original_region {};
        std::vector<encoded_animation_frame> frames;

        explicit implementation(std::filesystem::path executable_directory)
            : directory { std::filesystem::absolute(std::move(executable_directory)) }
        {}

        bool prepare(image_format format)
        {
            std::error_code code;
            std::filesystem::create_directories(directory / L"data" / L"captures", code);

            if (code)
            {
                return false;
            }

            std::filesystem::create_directories(directory / L"data" / L"temp", code);

            if (code)
            {
                return false;
            }

            for (int attempt {}; attempt < 10; ++attempt)
            {
                const auto name { unique_name() };
                auto candidate { directory / L"data" / L"temp" / name };
                const auto extension { format == image_format::png        ? L".png"
                                           : format == image_format::webp ? L".webp"
                                           : format == image_format::apng ? L".apng"
                                                                          : L".gif"
                };

                auto destination { directory / L"data" / L"captures" / (name + extension) };

                if (std::filesystem::create_directory(candidate, code))
                {
                    session = std::move(candidate);

                    const auto release_storage = [](void* value) {
                        const auto* path { static_cast<std::filesystem::path*>(value) };
                        std::error_code ignored {};
                        std::filesystem::remove_all(*path, ignored);
                        delete path;
                    };
                    storage = std::shared_ptr<void> { new std::filesystem::path { session }, release_storage };
                    output = std::move(destination);
                    return true;
                }

                if (code)
                {
                    return false;
                }
            }

            return false;
        }

        void cleanup()
        {
            pending = false;

            std::vector<encoded_animation_frame> {}.swap(frames);

            if (!session.empty())
            {
                storage.reset();
                session.clear();
            }
        }

        struct cleanup_scope
        {
            implementation& state;
            bool active { true };

            ~cleanup_scope()
            {
                if (active)
                {
                    state.cleanup();
                }
            }
        };

        capture_result publish(const std::filesystem::path& temporary)
        {
            if (cancelled.load())
            {
                return failure(u8"캡처 작업을 취소했습니다.");
            }

            std::error_code code;

            if (!std::filesystem::is_regular_file(temporary, code) || std::filesystem::file_size(temporary, code) == 0
                || code)
            {
                return failure(u8"인코더가 완전한 이미지 파일을 만들지 못했습니다.");
            }

            if (!MoveFileW(temporary.c_str(), output.c_str()))
            {
                return failure(u8"캡처 결과를 data/captures에 저장할 수 없습니다.");
            }

            return { true, u8"저장 완료: " + output.filename().u8string(), output };
        }

        void collect(capture_region region)
        {
            struct failure_scope
            {
                implementation& state;

                ~failure_scope()
                {
                    state.failed.store(!state.error.empty());
                }
            };

            const failure_scope failure { *this };

            try
            {
                animation_recorder recorder { session, image_format::apng };

                // 출력 배율과 무관하게 원본 크기로 수집해야 정지 뒤 다른 크기로 다시 저장할 수 있다.
                capture_options raw_options { options };
                raw_options.output_percent = 100;
                screen_frame buffer {};

                if (!buffer.initialize(region, raw_options))
                {
                    error = u8"녹화 비트맵을 준비할 수 없습니다.";
                    return;
                }

                // 목표 FPS는 대기 간격에만 쓰고 재생 시간은 실제 수집 시각으로 기록한다.
                const auto start { steady_clock::now() };
                auto next { start };
                const auto interval { std::chrono::nanoseconds { 1000000000LL / options.fps } };

                while (!stop_requested.load() && !cancelled.load())
                {
                    const auto captured { steady_clock::now() };
                    const auto requested_stop { stop_nanoseconds.load() };

                    if (requested_stop && clock_nanoseconds(captured) >= requested_stop)
                    {
                        break;
                    }

                    if (!buffer.read())
                    {
                        error = u8"녹화 프레임을 캡처할 수 없습니다.";
                        break;
                    }

                    const auto latest_stop { stop_nanoseconds.load() };

                    if (latest_stop && clock_nanoseconds(captured) >= latest_stop)
                    {
                        break;
                    }

                    const double timestamp { std::chrono::duration<double>(captured - start).count() };
                    const auto pixels { buffer.pixels() };
                    if (!recorder.append(pixels, timestamp))
                    {
                        error = u8"변경된 녹화 프레임을 보관할 수 없습니다. 디스크 공간을 확인하세요.";
                        break;
                    }

                    ++frame_count;

                    const auto now { steady_clock::now() };
                    next += interval;

                    if (next <= now)
                    {
                        next += interval * ((now - next) / interval + 1);
                    }

                    std::unique_lock lock { mutex };
                    wake.wait_until(lock, next, [this] { return stop_requested.load(); });
                }

                auto end { steady_clock::now() };
                const auto requested_stop { stop_nanoseconds.load() };

                if (requested_stop)
                {
                    const steady_clock::time_point boundary {
                        std::chrono::duration_cast<steady_clock::duration>(std::chrono::nanoseconds { requested_stop })
                    };
                    end = std::clamp(boundary, start, end);
                }

                elapsed = std::chrono::duration<double>(end - start).count();

                if (frame_count)
                {
                    frames = recorder.finish(elapsed);

                    if (frames.empty() && error.empty())
                    {
                        error = u8"녹화 원본을 마무리할 수 없습니다.";
                    }
                }
            }
            catch (...)
            {
                error = u8"녹화 임시 파일을 준비할 수 없습니다.";
            }
        }

        void request_stop() noexcept
        {
            std::int64_t expected {};
            stop_nanoseconds.compare_exchange_strong(expected, clock_nanoseconds(steady_clock::now()));
            stop_requested = true;
            wake.notify_all();
        }
    };

    image_capture::image_capture(std::filesystem::path executable_directory)
        : implementation_ { std::make_unique<implementation>(std::move(executable_directory)) }
    {}

    image_capture::~image_capture()
    {
        cancel_recording();
    }

    capture_result image_capture::take_screenshot(
        capture_region region, capture_options options, const progress_callback& progress
    )
    {
        auto& state { *implementation_ };

        if (state.cancelled.load())
        {
            return failure(u8"캡처 작업을 취소했습니다.");
        }

        if (recording() || has_pending_recording())
        {
            return failure(u8"녹화 중이거나 보관한 녹화가 있습니다. 먼저 녹화를 비우세요.");
        }

        if (!valid_options(region, options)
            || (options.still_format != image_format::png && options.still_format != image_format::webp))
        {
            return failure(u8"캡처 영역이나 이미지 옵션이 유효하지 않습니다.");
        }

        if (options.still_format == image_format::webp && !webp_dimensions_supported(region, options))
        {
            return failure(u8"WebP 출력은 가로·세로 각각 16383픽셀 이하여야 합니다. 영역이나 출력 배율을 줄이세요.");
        }

        implementation::cleanup_scope cleanup { state };
        report_progress(progress, save_stage::preparing);

        if (!state.prepare(options.still_format))
        {
            return failure(u8"실행 파일 옆 data 폴더에 쓸 수 없습니다.");
        }

        const auto native { state.session
                            / (options.still_format == image_format::png ? L"still.png" : L"still.webp")
        };

        if (!save_native(native, region, options, options.still_format, state.cancelled, progress))
        {
            return failure(u8"화면을 캡처하거나 이미지를 저장할 수 없습니다.");
        }

        if (options.still_format == image_format::webp && options.webp_lossless
            && options.compression != lossless_compression::fast)
        {
            const std::array frames { encoded_animation_frame { native, 1.0, 0, 0 } };
            std::u8string message;

            if (!optimize_webp_frames(
                    frames, state.cancelled, message, progress, { .compression = options.compression }
                ))
            {
                return failure(std::move(message));
            }
        }

        report_progress(progress, save_stage::publishing);

        const auto result { state.publish(native) };

        if (result.success)
        {
            report_progress(progress, save_stage::complete, 1, 1);
        }

        return result;
    }

    capture_result image_capture::start_recording(capture_region region, capture_options options)
    {
        auto& state { *implementation_ };

        if (state.cancelled.load())
        {
            return failure(u8"캡처 작업을 취소했습니다.");
        }

        if (recording() || has_pending_recording())
        {
            return failure(u8"이미 녹화 중이거나 보관한 녹화가 있습니다. 먼저 저장한 뒤 비우세요.");
        }

        if (!valid_options(region, options) || options.animation_format == image_format::png)
        {
            return failure(u8"녹화 영역이나 애니메이션 옵션이 유효하지 않습니다.");
        }

        if (options.animation_format == image_format::webp && !webp_dimensions_supported(region, options))
        {
            return failure(
                u8"WebP 출력은 가로·세로 각각 16383픽셀 이하여야 합니다. 영역이나 출력 배율을 줄이거나 "
                u8"APNG를 선택하세요."
            );
        }

        implementation::cleanup_scope cleanup { state };

        if (!state.prepare(options.animation_format))
        {
            return failure(u8"실행 파일 옆 data 폴더에 쓸 수 없습니다.");
        }

        state.options = options;
        state.original_region = region;
        state.stop_requested = false;
        state.stop_nanoseconds = 0;
        state.failed = false;
        state.error.clear();
        state.frame_count = 0;
        state.elapsed = 0;
        state.frames.clear();

        try
        {
            state.worker = std::thread { [&state, region] { state.collect(region); } };
        }
        catch (...)
        {
            state.cleanup();
            return failure(u8"녹화 작업 스레드를 시작할 수 없습니다.");
        }

        cleanup.active = false;
        return { true, u8"녹화 시작", {} };
    }

    capture_result image_capture::stop_recording(const bool save_immediately)
    {
        auto& state { *implementation_ };

        if (!recording())
        {
            return failure(u8"진행 중인 녹화가 없습니다.");
        }

        state.request_stop();
        state.worker.join();
        implementation::cleanup_scope cleanup { state };

        if (state.cancelled.load())
        {
            return failure(u8"녹화를 취소했습니다.");
        }

        if (!state.error.empty())
        {
            return failure(state.error);
        }

        if (state.frames.empty())
        {
            return failure(u8"저장할 녹화 프레임이 없습니다.");
        }

        state.pending = true;
        cleanup.active = false;
        return save_immediately
            ? save_recording(state.options)
            : capture_result { true, u8"녹화를 보관했습니다. 옵션을 조절한 뒤 저장 버튼을 누르세요.", {} };
    }

    capture_result image_capture::save_recording(const capture_options settings, const progress_callback& progress)
    {
        auto& state { *implementation_ };

        if (!has_pending_recording() || recording())
        {
            return failure(u8"저장할 보관 녹화가 없습니다.");
        }

        if (!valid_options(state.original_region, settings)
            || (settings.animation_format != image_format::apng && settings.animation_format != image_format::webp
                && settings.animation_format != image_format::gif))
        {
            return failure(u8"저장 옵션이 유효하지 않습니다. 원본은 보관되어 있습니다.");
        }

        if (settings.animation_format == image_format::webp
            && !webp_dimensions_supported(state.original_region, settings))
        {
            return failure(u8"WebP 출력은 가로·세로 16383픽셀 이하여야 합니다. 출력 크기를 줄이세요.");
        }

        try
        {
            report_progress(progress, save_stage::preparing);

            struct scratch_scope
            {
                std::filesystem::path path;

                ~scratch_scope()
                {
                    std::error_code ignored;
                    std::filesystem::remove_all(path, ignored);
                }
            };

            const auto name { unique_name() };
            const scratch_scope scratch { state.session / (L"export-" + name) };
            const auto extension { settings.animation_format == image_format::gif        ? L".gif"
                                       : settings.animation_format == image_format::apng ? L".apng"
                                                                                         : L".webp"
            };
            state.output = state.directory / L"data" / L"captures" / (name + extension);

            std::filesystem::create_directories(scratch.path);
            const auto temporary { scratch.path / (L"result" + std::wstring { extension }) };
            animation_frame_reader reader { state.frames,
                                            state.original_region.width,
                                            state.original_region.height,
                                            settings.output_percent,
                                            state.cancelled
            };

            const encoding::encoder_options options {
                .format = settings.animation_format == image_format::gif ? encoding::animation_format::gif
                    : settings.animation_format == image_format::apng    ? encoding::animation_format::apng
                                                                         : encoding::animation_format::webp,
                .lossless = settings.webp_lossless,
                .quality = settings.webp_quality,
                .compact = settings.compression != lossless_compression::fast,
                .maximum_compression = settings.compression == lossless_compression::maximum,
                .progress =
                    [progress](const save_progress update) {
                        if (update.stage != save_stage::complete)
                        {
                            report_progress(progress, update.stage, update.completed, update.total);
                        }
                    },
            };

            std::u8string message;

            const auto source { reader.source() };
            if (!encoding::encode_animation(temporary, source, options, state.cancelled, message))
            {
                return failure(message);
            }

            report_progress(progress, save_stage::publishing);

            const auto result { state.publish(temporary) };

            if (result.success)
            {
                report_progress(progress, save_stage::complete, 1, 1);
            }

            return result;
        }
        catch (...)
        {
            return failure(u8"저장 중 파일 또는 메모리 오류가 발생했습니다. 원본은 보관되어 있습니다.");
        }
    }

    bool image_capture::has_pending_recording() const noexcept
    {
        return implementation_->pending.load();
    }

    capture_region image_capture::recorded_region() const noexcept
    {
        return has_pending_recording() ? implementation_->original_region : capture_region {};
    }

    double image_capture::recorded_duration() const noexcept
    {
        return has_pending_recording() ? implementation_->elapsed : 0;
    }

    recording_snapshot image_capture::snapshot() const
    {
        if (!has_pending_recording() || recording())
        {
            return {};
        }

        return { implementation_->frames, implementation_->original_region, implementation_->storage };
    }

    void image_capture::discard_recording()
    {
        if (!recording())
        {
            implementation_->cleanup();
        }
    }

    bool image_capture::recording() const
    {
        return implementation_->worker.joinable();
    }

    bool image_capture::recording_failed() const noexcept
    {
        return implementation_->failed.load();
    }

    void image_capture::request_cancel() noexcept
    {
        implementation_->cancelled = true;
        implementation_->request_stop();
    }

    void image_capture::cancel_recording()
    {
        request_cancel();
        auto& state { *implementation_ };

        if (state.worker.joinable())
        {
            state.worker.join();
        }

        state.cleanup();
    }

} // namespace chalkag
