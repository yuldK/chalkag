#include "capture/animation_export.h"
#include "capture/animation_recorder.h"
#include "capture/native_webp_writer.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkSamplingOptions.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace chalkag {
    namespace {
        bool decode(const std::filesystem::path& path, SkBitmap& bitmap)
        {
            std::ifstream file { path, std::ios::binary | std::ios::ate };
            const auto size { static_cast<std::streamoff>(file.tellg()) };

            if (!file || size <= 0 || size > 512 * 1024 * 1024)
            {
                return false;
            }

            auto data { SkData::MakeUninitialized(static_cast<std::size_t>(size)) };

            if (!data)
            {
                return false;
            }

            file.seekg(0);
            file.read(static_cast<char*>(data->writable_data()), size);

            auto codec { file ? SkCodec::MakeFromData(std::move(data)) : nullptr };

            if (!codec || static_cast<std::int64_t>(codec->getInfo().width()) * codec->getInfo().height() > 33554432)
            {
                return false;
            }

            const auto info {
                codec->getInfo().makeColorType(kBGRA_8888_SkColorType).makeAlphaType(kPremul_SkAlphaType)
            };
            return bitmap.tryAllocPixels(info)
                && codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes()) == SkCodec::kSuccess;
        }

    } // namespace

    animation_frame_reader::animation_frame_reader(
        const std::span<const encoded_animation_frame> frames,
        const int width,
        const int height,
        const int output_percent,
        const std::atomic<bool>& cancelled
    )
        : frames_ { frames }
        , cancelled_ { cancelled }
    {
        if (frames.empty() || width < 1 || height < 1 || static_cast<std::int64_t>(width) * height > 33554432
            || output_percent < 1 || output_percent > 200)
        {
            throw std::runtime_error { "Invalid source animation" };
        }

        const int target_width {
            static_cast<int>(std::max<std::int64_t>(1, static_cast<std::int64_t>(width) * output_percent / 100))
        };
        const int target_height {
            static_cast<int>(std::max<std::int64_t>(1, static_cast<std::int64_t>(height) * output_percent / 100))
        };

        const auto canvas_info { SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType) };
        const auto output_info {
            SkImageInfo::Make(target_width, target_height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)
        };

        if (static_cast<std::int64_t>(target_width) * target_height > 33554432 || !canvas_.tryAllocPixels(canvas_info)
            || !scaled_.tryAllocPixels(output_info))
        {
            throw std::runtime_error { "Cannot allocate streaming animation bitmaps" };
        }
    }

    encoding::animation_source animation_frame_reader::source()
    {
        return { frames_.size(),
                 [this](const std::size_t index, encoding::animation_frame& frame) { return read(index, frame); }
        };
    }

    const SkBitmap& animation_frame_reader::canvas() const noexcept
    {
        return canvas_;
    }

    bool animation_frame_reader::seek(const std::size_t next, const SkPixmap& canvas)
    {
        if (next == 0 || next > frames_.size() || canvas.width() != canvas_.width()
            || canvas.height() != canvas_.height() || !canvas.readPixels(canvas_.pixmap()))
        {
            return false;
        }

        next_ = next;
        return true;
    }

    bool animation_frame_reader::read(const std::size_t index, encoding::animation_frame& frame)
    {
        if (index == 0)
        {
            next_ = 0;
        }

        if (index != next_ || index >= frames_.size() || cancelled_.load())
        {
            return false;
        }

        // 변경 타일을 이전 화면에 순서대로 합성해야 전체 프레임과 출력 배율을 복원할 수 있다.
        const auto& input { frames_[index] };
        SkBitmap tile;

        if (!decode(input.path, tile) || input.x < 0 || input.y < 0 || input.x > canvas_.width() - tile.width()
            || input.y > canvas_.height() - tile.height()
            || (index == 0
                && (input.x != 0 || input.y != 0 || tile.width() != canvas_.width()
                    || tile.height() != canvas_.height())))
        {
            return false;
        }

        for (int y {}; y < tile.height(); ++y)
        {
            std::memcpy(
                canvas_.getAddr32(input.x, input.y + y),
                tile.getAddr32(0, y),
                static_cast<std::size_t>(tile.width()) * 4
            );
        }

        if (!canvas_.pixmap().scalePixels(scaled_.pixmap(), SkSamplingOptions { SkCubicResampler::Mitchell() }))
        {
            return false;
        }

        frame = { scaled_.pixmap(), input.duration_seconds };
        ++next_;
        return true;
    }

    std::vector<encoded_animation_frame> render_animation_frames(
        const std::span<const encoded_animation_frame> source,
        const int width,
        const int height,
        const capture_options& options,
        const std::filesystem::path& scratch,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    )
    {
        error.clear();

        try
        {
            if (width <= 0 || height <= 0 || static_cast<std::int64_t>(width) * height > 33554432
                || options.output_percent < 1 || options.output_percent > 200)
            {
                error = u8"원본이나 출력 크기가 올바르지 않습니다.";
                return {};
            }

            const int target_width { static_cast<int>(
                std::max<std::int64_t>(1, static_cast<std::int64_t>(width) * options.output_percent / 100)
            )
            };
            const int target_height { static_cast<int>(
                std::max<std::int64_t>(1, static_cast<std::int64_t>(height) * options.output_percent / 100)
            )
            };

            if (source.empty() || width <= 0 || height <= 0 || target_width <= 0 || target_height <= 0
                || options.output_percent < 1 || options.output_percent > 200
                || static_cast<std::int64_t>(width) * height > 33554432
                || static_cast<std::int64_t>(target_width) * target_height > 33554432)
            {
                error = u8"저장할 원본이나 출력 크기가 올바르지 않습니다.";
                return {};
            }

            std::filesystem::create_directories(scratch);
            SkBitmap canvas;
            SkBitmap scaled;

            const auto canvas_info { SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType) };
            const auto output_info {
                SkImageInfo::Make(target_width, target_height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)
            };

            if (!canvas.tryAllocPixels(canvas_info) || !scaled.tryAllocPixels(output_info))
            {
                error = u8"출력 비트맵을 준비할 수 없습니다.";
                return {};
            }

            // 불투명 무손실 형식은 변경 타일을 다시 만들고 손실 형식은 완성된 전체 화면을 저장한다.
            const bool delta
                = (options.animation_format == image_format::apng
                   || (options.animation_format == image_format::webp && options.webp_lossless));

            std::unique_ptr<animation_recorder> recorder;

            if (delta)
            {
                recorder = std::make_unique<animation_recorder>(scratch, options.animation_format);
            }

            std::vector<encoded_animation_frame> result {};
            double timeline {};

            for (const auto& frame : source)
            {
                if (cancelled.load())
                {
                    error = u8"저장을 취소했습니다.";
                    return {};
                }

                SkBitmap tile;

                if (!decode(frame.path, tile) || frame.x < 0 || frame.y < 0 || frame.x > width - tile.width()
                    || frame.y > height - tile.height() || !std::isfinite(frame.duration_seconds)
                    || frame.duration_seconds <= 0
                    || (timeline == 0
                        && (frame.x != 0 || frame.y != 0 || tile.width() != width || tile.height() != height)))
                {
                    error = u8"보관한 원본 프레임을 복원할 수 없습니다.";
                    return {};
                }

                for (int y {}; y < tile.height(); ++y)
                {
                    std::memcpy(
                        canvas.getAddr32(frame.x, frame.y + y),
                        tile.getAddr32(0, y),
                        static_cast<std::size_t>(tile.width()) * 4
                    );
                }

                if (!canvas.pixmap().scalePixels(scaled.pixmap(), SkSamplingOptions { SkCubicResampler::Mitchell() }))
                {
                    error = u8"출력 크기를 변환할 수 없습니다.";
                    return {};
                }

                if (delta)
                {
                    if (!recorder->append(scaled.pixmap(), timeline))
                    {
                        error = u8"변환한 프레임을 저장할 수 없습니다.";
                        return {};
                    }
                }
                else
                {
                    const bool webp { options.animation_format == image_format::webp };
                    const auto path { scratch
                                      / ("frame-" + std::to_string(result.size()) + (webp ? ".webp" : ".png"))
                    };

                    if (!encode_native_image(
                            path,
                            scaled.pixmap(),
                            webp ? image_format::webp : image_format::png,
                            options.compression,
                            options.webp_lossless,
                            options.webp_quality
                        ))
                    {
                        error = u8"변환한 프레임을 압축할 수 없습니다.";
                        return {};
                    }

                    result.push_back({ path, frame.duration_seconds, 0, 0 });
                }

                timeline += frame.duration_seconds;
            }

            if (!delta)
            {
                return result;
            }

            // 타일 압축은 뒤따라 진행되므로 마무리한 뒤에 저장 실패를 확인해야 일부만 내보내지 않는다.
            auto rendered { recorder->finish(timeline) };

            if (recorder->storage_failed())
            {
                error = u8"변환한 프레임을 저장할 수 없습니다.";
                return {};
            }

            return rendered;
        }
        catch (...)
        {
            error = u8"저장 준비 중 파일 또는 메모리 오류가 발생했습니다. 원본은 보관되어 있습니다.";
            return {};
        }
    }

} // namespace chalkag
