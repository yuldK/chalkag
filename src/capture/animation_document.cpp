#include "capture/animation_document.h"
#include "capture/animation_export.h"

#include "include/codec/SkCodec.h"
#include "include/codec/SkCodecAnimation.h"
#include "include/codec/SkEncodedImageFormat.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkSamplingOptions.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <numeric>

namespace chalkag {
    class animation_document::impl
    {
    public:
        std::mutex mutex {};
        std::unique_ptr<SkCodec> codec {};
        recording_snapshot recording {};

        std::unique_ptr<animation_frame_reader> reader {};
        std::atomic<bool> reader_cancelled {};
        std::size_t next {};
        std::vector<int> durations {};
        int width {};
        int height {};
        int plays {};
        bool opaque { true };

        SkBitmap decoded {};
        int decoded_index { SkCodec::kNoFrame };
    };

    animation_document::animation_document()
        : impl_ { std::make_unique<impl>() }
    {}

    animation_document::~animation_document() = default;

    std::shared_ptr<animation_document> animation_document::open(
        const std::filesystem::path& path, std::u8string& error
    )
    {
        error.clear();

        try
        {
            std::ifstream file { path, std::ios::binary | std::ios::ate };
            const auto bytes { static_cast<std::streamoff>(file.tellg()) };

            if (!file || bytes <= 0 || bytes > 512LL * 1024 * 1024)
            {
                throw std::runtime_error { "Invalid image size" };
            }

            auto data { SkData::MakeUninitialized(static_cast<std::size_t>(bytes)) };

            if (!data)
            {
                throw std::bad_alloc {};
            }

            file.seekg(0);
            file.read(static_cast<char*>(data->writable_data()), bytes);

            auto result { std::shared_ptr<animation_document> { new animation_document {} } };
            auto& state { *result->impl_ };
            state.codec = file ? SkCodec::MakeFromData(std::move(data)) : nullptr;

            if (!state.codec)
            {
                throw std::runtime_error { "Unsupported image" };
            }

            // 형식·크기·반복 횟수를 검증한 뒤 프레임별 표시 시간을 편집 정보로 옮긴다.
            const auto image_info { state.codec->getInfo() };
            state.width = image_info.width();
            state.height = image_info.height();

            const auto format { state.codec->getEncodedFormat() };

            if (format != SkEncodedImageFormat::kGIF && format != SkEncodedImageFormat::kWEBP
                && format != SkEncodedImageFormat::kPNG)
            {
                throw std::runtime_error { "Unsupported image format" };
            }

            state.opaque = image_info.isOpaque();

            const auto count { state.codec->getFrameCount() };

            if (state.width < 1 || state.height < 1 || static_cast<std::int64_t>(state.width) * state.height > 33554432
                || count < 1 || count > 1000000)
            {
                throw std::runtime_error { "Animation limit exceeded" };
            }

            const auto repetition { state.codec->getRepetitionCount() };
            state.plays = repetition < 0 ? 0 : repetition + 1;

            if (state.plays > 65535)
            {
                throw std::runtime_error { "Loop count limit exceeded" };
            }

            for (int index {}; index < count; ++index)
            {
                SkCodec::FrameInfo info {};
                const bool has_info { state.codec->getFrameInfo(index, &info) };
                const int delay { has_info ? info.fDuration : 100 };

                if (has_info && info.fAlphaType != kOpaque_SkAlphaType)
                {
                    state.opaque = false;
                }

                // 지연이 없는 파일도 편집·재생 가능한 최소 간격으로 연다.
                state.durations.push_back(std::clamp(delay > 0 ? delay : 100, 1, 600000));
            }

            SkBitmap first {};
            const std::atomic<bool> cancelled {};

            if (!result->read(0, first, cancelled))
            {
                throw std::runtime_error { "Cannot decode first frame" };
            }

            return result;
        }
        catch (...)
        {
            error = u8"이미지를 열 수 없습니다. GIF·WebP·APNG/PNG 형식과 파일 상태를 확인하세요. 입력은 512 MiB, "
                    u8"화면은 3,355만 픽셀까지 지원합니다.";
            return {};
        }
    }

    std::shared_ptr<animation_document> animation_document::from_recording(recording_snapshot snapshot)
    {
        if (snapshot.frames.empty() || snapshot.region.width < 1 || snapshot.region.height < 1)
        {
            return {};
        }

        auto result { std::shared_ptr<animation_document> { new animation_document {} } };
        auto& state { *result->impl_ };
        state.width = snapshot.region.width;
        state.height = snapshot.region.height;

        for (const auto& frame : snapshot.frames)
        {
            const auto milliseconds { std::llround(frame.duration_seconds * 1000) };
            const auto bounded_delay { std::clamp(milliseconds, 1LL, 600000LL) };
            state.durations.push_back(static_cast<int>(bounded_delay));
        }

        state.recording = std::move(snapshot);
        return result;
    }

    int animation_document::width() const noexcept
    {
        return impl_->width;
    }

    int animation_document::height() const noexcept
    {
        return impl_->height;
    }

    int animation_document::play_count() const noexcept
    {
        return impl_->plays;
    }

    const std::vector<int>& animation_document::durations() const noexcept
    {
        return impl_->durations;
    }

    bool animation_document::read(const std::size_t index, SkBitmap& bitmap, const std::atomic<bool>& cancelled)
    {
        std::lock_guard lock { impl_->mutex };
        auto& state { *impl_ };

        if (index >= state.durations.size() || cancelled.load())
        {
            return false;
        }

        if (state.codec)
        {
            const auto info { SkImageInfo::Make(
                state.width,
                state.height,
                kBGRA_8888_SkColorType,
                state.opaque ? kOpaque_SkAlphaType : kUnpremul_SkAlphaType,
                SkColorSpace::MakeSRGB()
            )
            };

            if (state.decoded_index == static_cast<int>(index))
            {
                return bitmap.tryAllocPixels(info) && state.decoded.pixmap().readPixels(bitmap.pixmap());
            }

            if (state.decoded.empty() && !state.decoded.tryAllocPixels(info))
            {
                return false;
            }

            SkCodec::Options options {};
            options.fFrameIndex = static_cast<int>(index);
            options.fMaxDecodeMemory = 256 * 1024 * 1024;

            SkCodec::FrameInfo required {}, previous {};

            if (state.decoded_index >= 0 && state.decoded_index < options.fFrameIndex
                && state.codec->getFrameInfo(options.fFrameIndex, &required)
                && state.codec->getFrameInfo(state.decoded_index, &previous)
                && required.fRequiredFrame != SkCodec::kNoFrame && state.decoded_index >= required.fRequiredFrame
                && previous.fDisposalMethod != SkCodecAnimation::DisposalMethod::kRestorePrevious)
            {
                options.fPriorFrame = state.decoded_index;
            }

            // 이전 프레임의 blend/disposal 합성은 코덱이 원본을 따라 복원한다.
            if (state.codec->getPixels(info, state.decoded.getPixels(), state.decoded.rowBytes(), &options)
                    != SkCodec::kSuccess
                || cancelled.load())
            {
                state.decoded_index = SkCodec::kNoFrame;
                return false;
            }

            state.decoded_index = options.fFrameIndex;
            return bitmap.tryAllocPixels(info) && state.decoded.pixmap().readPixels(bitmap.pixmap());
        }

        // 녹화 타일은 순차 합성한다. 이전 프레임을 요청하면 원본 처음부터 리더를 다시 시작한다.
        if (!state.reader || index < state.next)
        {
            state.reader = std::make_unique<animation_frame_reader>(
                state.recording.frames, state.width, state.height, 100, state.reader_cancelled
            );
            state.next = 0;
        }

        auto source { state.reader->source() };
        encoding::animation_frame frame {};

        while (state.next <= index)
        {
            if (cancelled.load() || !source.read(state.next++, frame))
            {
                return false;
            }
        }

        return bitmap.tryAllocPixels(frame.pixels.info()) && frame.pixels.readPixels(bitmap.pixmap());
    }

    animation_edit initial_animation_edit(const animation_document& document)
    {
        animation_edit edit {};
        edit.crop = { 0, 0, document.width(), document.height() };
        edit.output_width = document.width();
        edit.output_height = document.height();

        for (std::size_t index {}; index < document.durations().size(); ++index)
        {
            edit.frames.push_back({ index, document.durations()[index] });
        }

        return edit;
    }

    std::int64_t animation_duration(const animation_edit& edit) noexcept
    {
        return std::accumulate(
            edit.frames.begin(), edit.frames.end(), std::int64_t {}, [](const auto sum, const edited_frame& frame) {
                return sum + frame.milliseconds;
            }
        );
    }

    animation_edit reduce_animation(
        const animation_edit& edit, const int stride, const int phase, const bool preserve_duration
    )
    {
        if (stride < 2 || stride > 4 || phase < 0 || phase >= stride || edit.frames.size() < 2)
        {
            return edit;
        }

        auto result { edit };
        result.frames.clear();

        for (std::size_t begin {}; begin < edit.frames.size(); begin += static_cast<std::size_t>(stride))
        {
            const auto end { std::min(edit.frames.size(), begin + static_cast<std::size_t>(stride)) };
            const auto selected { begin + static_cast<std::size_t>(phase) };

            if (selected >= end && !result.frames.empty())
            {
                // 남길 위치가 없는 마지막 묶음의 시간은 앞 프레임에 합친다.
                if (preserve_duration)
                {
                    std::int64_t milliseconds { result.frames.back().milliseconds };

                    for (auto index { begin }; index < end; ++index)
                    {
                        milliseconds += edit.frames[index].milliseconds;
                    }

                    if (milliseconds > 600000)
                    {
                        return edit;
                    }

                    result.frames.back().milliseconds = static_cast<int>(milliseconds);
                }

                continue;
            }

            // 전체 목록이 한 묶음보다 짧아도 최소 한 장은 남긴다.
            auto frame { edit.frames[std::min(selected, end - 1)] };

            if (preserve_duration)
            {
                std::int64_t milliseconds {};

                for (auto index { begin }; index < end; ++index)
                {
                    milliseconds += edit.frames[index].milliseconds;
                }

                if (milliseconds > 600000)
                {
                    return edit;
                }

                frame.milliseconds = static_cast<int>(milliseconds);
            }

            result.frames.push_back(frame);
        }

        return result;
    }

    bool valid_animation_edit(const animation_edit& edit, const int width, const int height) noexcept
    {
        const auto& crop { edit.crop };
        return !edit.frames.empty() && crop.x >= 0 && crop.y >= 0 && crop.width > 0 && crop.height > 0
            && crop.width <= width && crop.height <= height && crop.x <= width - crop.width
            && crop.y <= height - crop.height && edit.output_width > 0 && edit.output_height > 0
            && edit.output_width <= 16383 && edit.output_height <= 16383
            && static_cast<std::int64_t>(edit.output_width) * edit.output_height <= 33554432
            && std::all_of(edit.frames.begin(), edit.frames.end(), [](const auto& frame) {
                   return frame.milliseconds > 0 && frame.milliseconds <= 600000;
               });
    }

    edited_animation_reader::edited_animation_reader(
        std::shared_ptr<animation_document> document,
        animation_edit edit,
        const bool flatten_alpha,
        const std::atomic<bool>& cancelled
    )
        : document_ { std::move(document) }
        , edit_ { std::move(edit) }
        , flatten_alpha_ { flatten_alpha }
        , valid_ { document_ && valid_animation_edit(edit_, document_->width(), document_->height()) }
        , cancelled_ { cancelled }
    {}

    encoding::animation_source edited_animation_reader::source()
    {
        return { edit_.frames.size(),
                 [this](const std::size_t index, encoding::animation_frame& frame) { return read(index, frame); }
        };
    }

    bool edited_animation_reader::read(const std::size_t index, encoding::animation_frame& frame)
    {
        if (!valid_ || index >= edit_.frames.size()
            || !document_->read(edit_.frames[index].source, canvas_, cancelled_))
        {
            return false;
        }

        SkPixmap subset {};
        const auto& crop { edit_.crop };
        const auto info {
            SkImageInfo::Make(edit_.output_width, edit_.output_height, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType)
        };

        if (!canvas_.pixmap().extractSubset(&subset, SkIRect::MakeXYWH(crop.x, crop.y, crop.width, crop.height))
            || !output_.tryAllocPixels(info)
            || !subset.scalePixels(output_.pixmap(), SkSamplingOptions { SkCubicResampler::Mitchell() }))
        {
            return false;
        }

        if (flatten_alpha_)
        {
            for (int y {}; y < output_.height(); ++y)
            {
                for (int x {}; x < output_.width(); ++x)
                {
                    auto* pixel { reinterpret_cast<std::uint8_t*>(output_.getAddr32(x, y)) };

                    const unsigned alpha { pixel[3] };

                    for (int channel {}; channel < 3; ++channel)
                    {
                        pixel[channel]
                            = static_cast<std::uint8_t>((pixel[channel] * alpha + 255 * (255 - alpha) + 127) / 255);
                    }

                    pixel[3] = 255;
                }
            }

            output_.setAlphaType(kOpaque_SkAlphaType);
        }
        else if (canvas_.alphaType() == kOpaque_SkAlphaType)
        {
            output_.setAlphaType(kOpaque_SkAlphaType);
        }

        frame = { output_.pixmap(), static_cast<double>(edit_.frames[index].milliseconds) / 1000 };
        return !cancelled_.load();
    }

} // namespace chalkag
