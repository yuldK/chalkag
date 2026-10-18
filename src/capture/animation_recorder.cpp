#include "capture/animation_recorder.h"

#include "capture/native_webp_writer.h"

#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace chalkag {
    animation_recorder::animation_recorder(std::filesystem::path session, const image_format format)
        : session_ { std::move(session) }
        , format_ { format }
        , failed_ { format != image_format::webp && format != image_format::apng }
    {}

    bool animation_recorder::append(const SkPixmap& pixels, const double timestamp_seconds)
    {
        if (failed_ || finished_)
        {
            return false;
        }

        if (!std::isfinite(timestamp_seconds) || timestamp_seconds < 0
            || (!frames_.empty() && timestamp_seconds <= last_sample_)
            || (!frames_.empty() && (pixels.width() != width_ || pixels.height() != height_)))
        {
            failed_ = true;
            return false;
        }

        // 같은 화면은 파일을 추가하지 않고 이전 그림의 표시 시간에 포함한다.
        const auto change { optimizer_.inspect(pixels) };

        if (!change.changed())
        {
            if (frames_.empty())
            {
                failed_ = true;
                return false;
            }

            last_sample_ = timestamp_seconds;
            return true;
        }

        SkPixmap tile {};
        const auto rectangle { SkIRect::MakeXYWH(change.x, change.y, change.width, change.height) };
        const auto path {
            session_ / ("frame-" + std::to_string(frames_.size()) + (format_ == image_format::webp ? ".webp" : ".png"))
        };

        // 녹화 중에는 빠른 압축을 유지하고 용량 최적화는 정지 뒤에 수행한다.
        if (!pixels.extractSubset(&tile, rectangle)
            || !encode_native_image(
                path,
                tile,
                format_ == image_format::apng ? image_format::png : image_format::webp,
                lossless_compression::fast
            ))
        {
            failed_ = true;
            return false;
        }

        // 새 그림이 도착한 시각으로 이전 그림의 표시 시간을 확정한다.
        if (!frames_.empty())
        {
            frames_.back().duration_seconds = timestamp_seconds - last_change_;
        }

        frames_.push_back({ path, 0.0, change.x, change.y });
        width_ = pixels.width();
        height_ = pixels.height();
        last_change_ = timestamp_seconds;
        last_sample_ = timestamp_seconds;
        return true;
    }

    std::vector<encoded_animation_frame> animation_recorder::finish(const double timestamp_seconds)
    {
        if (failed_ || finished_)
        {
            return {};
        }

        finished_ = true;

        if (frames_.empty() || !std::isfinite(timestamp_seconds) || timestamp_seconds <= last_sample_)
        {
            return {};
        }

        frames_.back().duration_seconds = timestamp_seconds - last_change_;

        // 누적 시각을 먼저 반올림하여 프레임별 반올림 오차가 전체 길이에 쌓이지 않게 한다.
        std::vector<encoded_animation_frame> result {};
        long double elapsed_ms {};

        std::int64_t previous_ms {};

        for (const auto& frame : frames_)
        {
            elapsed_ms += static_cast<long double>(frame.duration_seconds) * 1000;

            // 부정확한 종료 시각으로 반복 타일이 무한히 늘지 않도록 약 49일의 상한을 둔다.
            if (elapsed_ms > static_cast<long double>(std::numeric_limits<std::uint32_t>::max()))
            {
                return {};
            }

            const std::int64_t end_ms { std::max(previous_ms + 1, static_cast<std::int64_t>(std::round(elapsed_ms))) };
            std::int64_t remaining_ms { end_ms - previous_ms };

            while (remaining_ms > 0)
            {
                // APNG 지연 분자의 범위를 넘는 정지 구간은 같은 타일을 다시 사용한다.
                const std::int64_t duration_ms { std::min<std::int64_t>(remaining_ms, 60000) };
                result.push_back({ frame.path, static_cast<double>(duration_ms) / 1000, frame.x, frame.y });
                remaining_ms -= duration_ms;
            }

            previous_ms = end_ms;
        }

        return result;
    }

} // namespace chalkag
