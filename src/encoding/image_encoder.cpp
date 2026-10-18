#include "encoding/image_encoder.h"
#include "encoding/gif_encoder.h"
#include "encoding/webp_encoder.h"

#include "capture/animation_recorder.h"
#include "capture/apng_writer.h"
#include "capture/native_webp_writer.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace chalkag::encoding {
    namespace {
        struct scratch_directory
        {
            std::filesystem::path path;

            ~scratch_directory()
            {
                if (!path.empty())
                {
                    std::error_code ignored;
                    std::filesystem::remove_all(path, ignored);
                }
            }
        };

        bool native_animation(
            const std::filesystem::path& output,
            const std::filesystem::path& scratch,
            const animation_source& source,
            const encoder_options& options,
            const std::atomic<bool>& cancelled,
            std::u8string& error
        )
        {
            // 불투명 무손실 출력은 변경 영역만 저장하고 투명·손실 출력은 전체 프레임을 사용한다.
            const auto first_frame { read_animation_frame(source, 0) };
            const auto& first { first_frame.pixels };
            const bool apng { options.format == animation_format::apng };
            const bool delta { (apng || options.lossless) && first.alphaType() == kOpaque_SkAlphaType };
            const auto format { apng ? image_format::apng : image_format::webp };
            std::vector<encoded_animation_frame> encoded;
            report_progress(options.progress, save_stage::encoding, 0, source.count);

            if (delta)
            {
                animation_recorder recorder { scratch, format };
                double timestamp {};

                for (std::size_t index {}; index < source.count; ++index)
                {
                    const auto frame { read_animation_frame(source, index) };

                    if (frame.pixels.width() != first.width() || frame.pixels.height() != first.height()
                        || cancelled.load() || !recorder.append(frame.pixels, timestamp))
                    {
                        return false;
                    }

                    timestamp += frame.duration_seconds;
                    report_progress(options.progress, save_stage::encoding, index + 1, source.count);
                }

                encoded = recorder.finish(timestamp);

                if (encoded.empty())
                {
                    return false;
                }

                const webp_optimization_options optimization {
                    .compression
                    = options.maximum_compression ? lossless_compression::maximum : lossless_compression::compact,
                    .workers = options.webp_workers,
                    .internal_threading = options.webp_threading,
                };

                if (!apng && options.compact
                    && !optimize_webp_frames(encoded, cancelled, error, options.progress, optimization))
                {
                    return false;
                }
            }
            else
            {
                for (std::size_t index {}; index < source.count; ++index)
                {
                    const auto frame { read_animation_frame(source, index) };

                    if (frame.pixels.width() != first.width() || frame.pixels.height() != first.height())
                    {
                        return false;
                    }

                    const auto path { scratch / (std::to_string(encoded.size()) + (apng ? ".png" : ".webp")) };
                    const auto compression { !options.compact                  ? lossless_compression::fast
                                                 : options.maximum_compression ? lossless_compression::maximum
                                                                               : lossless_compression::compact
                    };

                    const auto frame_format { apng ? image_format::png : image_format::webp };

                    if (cancelled.load()
                        || !encode_native_image(
                            path,
                            frame.pixels,
                            frame_format,
                            compression,
                            options.lossless,
                            options.quality,
                            &cancelled,
                            options.webp_threading
                        ))
                    {
                        return false;
                    }

                    // 투명 프레임도 APNG 지연 분수의 한도를 넘지 않게 같은 그림으로 나눈다.
                    double remaining { frame.duration_seconds };

                    while (apng && remaining > 65.535)
                    {
                        encoded.push_back({ path, 60, 0, 0 });
                        remaining -= 60;
                    }

                    encoded.push_back({ path, remaining, 0, 0 });
                    report_progress(options.progress, save_stage::encoding, index + 1, source.count);
                }
            }

            if (apng)
            {
                return write_apng(
                    output, encoded, cancelled, error, options.progress, static_cast<std::uint32_t>(options.play_count)
                );
            }

            report_progress(options.progress, save_stage::assembling, 0, encoded.size());
            native_webp_writer writer {
                output, first.width(), first.height(), &cancelled, static_cast<std::uint16_t>(options.play_count)
            };
            double timeline {};

            std::uint64_t milliseconds {};
            std::size_t completed {};

            for (const auto& frame : encoded)
            {
                timeline += frame.duration_seconds;

                const auto end { static_cast<std::uint64_t>(std::llround(timeline * 1000)) };
                const auto duration { std::max<std::uint64_t>(1, end > milliseconds ? end - milliseconds : 0) };

                if (duration > 0xffffff
                    || !writer.append_frame(frame.path, static_cast<std::uint32_t>(duration), frame.x, frame.y))
                {
                    return false;
                }

                milliseconds += duration;
                report_progress(options.progress, save_stage::assembling, ++completed, encoded.size());
            }

            return writer.finish();
        }

    } // namespace

    animation_frame read_animation_frame(const animation_source& source, const std::size_t index)
    {
        animation_frame frame;

        if (index >= source.count || !source.read || !source.read(index, frame))
        {
            throw std::runtime_error { "Cannot read animation frame" };
        }

        const auto& pixels { frame.pixels };

        // 버퍼 구조와 재생 시간을 각각 검사하여 잘못된 입력을 인코더에 넘기지 않는다.
        const auto pixel_count { static_cast<std::int64_t>(pixels.width()) * pixels.height() };
        const bool supported_alpha { pixels.alphaType() == kOpaque_SkAlphaType
                                     || pixels.alphaType() == kUnpremul_SkAlphaType
        };

        const bool valid_duration { std::isfinite(frame.duration_seconds) && frame.duration_seconds > 0
                                    && frame.duration_seconds <= 600
        };

        if (!pixels.addr() || pixels.width() < 1 || pixels.height() < 1 || pixels.width() > 65535
            || pixels.height() > 65535 || pixel_count > 33554432 || pixels.colorType() != kBGRA_8888_SkColorType
            || !supported_alpha || pixels.rowBytes() < static_cast<std::size_t>(pixels.width()) * 4
            || pixels.computeByteSize() > std::numeric_limits<UINT>::max() || !valid_duration)
        {
            throw std::runtime_error { "Invalid animation pixels or duration" };
        }

        return frame;
    }

    bool encode_animation(
        const std::filesystem::path& output,
        const std::span<const animation_frame> frames,
        const encoder_options& options,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    )
    {
        const auto read = [frames](const std::size_t index, animation_frame& frame) {
            if (index >= frames.size())
            {
                return false;
            }

            frame = frames[index];
            return true;
        };

        const animation_source source { frames.size(), read };
        return encode_animation(output, source, options, cancelled, error);
    }

    bool encode_animation(
        const std::filesystem::path& output,
        const animation_source& source,
        const encoder_options& options,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    )
    {
        error.clear();

        if (!source.read || source.count == 0 || source.count > 1000000 || output.empty() || options.quality < 1
            || options.quality > 100 || options.webp_workers > 8 || options.play_count < 0 || options.play_count > 65535
            || (options.format != animation_format::gif && options.format != animation_format::webp
                && options.format != animation_format::apng)
            || (options.gif_encoder != gif_backend::cgif && options.gif_encoder != gif_backend::cgif_fixed_palette))
        {
            error = u8"정적 인코더의 입력 프레임이나 옵션이 올바르지 않습니다.";
            return false;
        }

        if (cancelled.load())
        {
            error = u8"이미지 저장을 취소했습니다.";
            return false;
        }

        try
        {
            report_progress(options.progress, save_stage::preparing);

            const auto first { read_animation_frame(source, 0).pixels };
            static std::atomic<std::uint64_t> sequence {};
            const auto absolute { std::filesystem::absolute(output) };
            std::filesystem::create_directories(absolute.parent_path());

            // 완성 전에는 임시 디렉터리만 변경하고 성공한 후보를 마지막에 교체한다.
            scratch_directory scratch;

            for (int attempt {}; attempt < 64 && scratch.path.empty(); ++attempt)
            {
                const auto candidate { absolute.parent_path()
                                       / (L".encoding-" + std::to_wstring(GetCurrentProcessId()) + L"-"
                                          + std::to_wstring(sequence.fetch_add(1)))
                };

                if (std::filesystem::create_directory(candidate))
                {
                    scratch.path = candidate;
                }
            }

            if (scratch.path.empty())
            {
                throw std::runtime_error { "Cannot create encoding scratch directory" };
            }

            const auto candidate { scratch.path / L"candidate" };
            bool success {};

            if (options.format == animation_format::gif)
            {
                success = encode_gif(candidate, source, options, cancelled, error);
            }
            else if (
                options.format == animation_format::webp && options.webp_animation_optimizer && options.compact
                && source.count <= static_cast<std::size_t>(std::numeric_limits<int>::max() / 600000)
                && static_cast<std::uint64_t>(first.width()) * first.height() * 4
                    <= options.animation_memory_budget / source.count
            )
            {
                success = encode_webp(candidate, source, options, cancelled, error);
            }
            else
            {
                success = native_animation(candidate, scratch.path, source, options, cancelled, error);
            }

            if (!success || cancelled.load())
            {
                if (error.empty())
                {
                    error = cancelled.load() ? u8"이미지 저장을 취소했습니다." : u8"정적 이미지 인코딩에 실패했습니다.";
                }

                return false;
            }

            report_progress(options.progress, save_stage::publishing);

            if (cancelled.load())
            {
                error = u8"이미지 저장을 취소했습니다.";
                return false;
            }

            if (!MoveFileExW(candidate.c_str(), absolute.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error = u8"인코딩한 파일을 최종 위치에 저장할 수 없습니다.";
                return false;
            }

            report_progress(options.progress, save_stage::complete, 1, 1);
            return true;
        }
        catch (const std::exception&)
        {
            error = cancelled.load() ? u8"이미지 저장을 취소했습니다."
                                     : u8"이미지 인코더의 파일 또는 메모리 처리에 실패했습니다.";
            return false;
        }
    }

} // namespace chalkag::encoding
