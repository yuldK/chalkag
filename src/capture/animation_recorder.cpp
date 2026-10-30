#include "capture/animation_recorder.h"

#include "capture/native_webp_writer.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace chalkag {
    namespace {
        constexpr std::size_t no_failure { std::numeric_limits<std::size_t>::max() };
        // 대기·압축 중인 타일 사본의 총량 상한이다. 타일은 전체 화면보다 작거나 같으므로
        // 전체 화면 크기로 나눈 수(1~4개)만 동시에 허용하면 이 예산을 넘지 않는다.
        // 33,554,432픽셀 상한 화면에서도 한 장(128MiB)만 허용되어 메모리가 제한된다.
        constexpr std::uint64_t pending_tile_budget { 256ULL * 1024 * 1024 };
        constexpr std::size_t maximum_pending_tiles { 4 };
        constexpr std::size_t encoder_threads { 2 };
    } // namespace

    struct animation_recorder::encoder_pool
    {
        struct job
        {
            SkBitmap tile {};
            std::filesystem::path path {};
            std::size_t index {};
        };

        image_format format {};
        std::mutex mutex {};
        std::condition_variable changed {};
        std::deque<job> queue {};
        std::vector<std::thread> workers {};
        std::size_t outstanding {};
        std::size_t limit { 1 };
        std::size_t failed_index { no_failure };
        std::atomic<bool> failed {};
        bool stopping {};

        explicit encoder_pool(const image_format tile_format)
            : format { tile_format }
        {}

        ~encoder_pool()
        {
            {
                const std::lock_guard lock { mutex };
                stopping = true;
                outstanding -= queue.size();
                queue.clear();
            }

            changed.notify_all();

            for (auto& worker : workers)
            {
                worker.join();
            }
        }

        void fail(const std::size_t index)
        {
            {
                const std::lock_guard lock { mutex };
                failed_index = std::min(failed_index, index);
                failed = true;
                // 실패 뒤의 프레임은 어차피 버리므로 기다리는 압축도 취소한다.
                outstanding -= queue.size();
                queue.clear();
            }

            changed.notify_all();
        }

        bool submit(job work, const std::uint64_t frame_bytes)
        {
            const auto index { work.index };

            try
            {
                std::unique_lock lock { mutex };

                if (workers.empty())
                {
                    limit = static_cast<std::size_t>(std::clamp<std::uint64_t>(
                        pending_tile_budget / std::max<std::uint64_t>(1, frame_bytes), 1, maximum_pending_tiles
                    ));

                    for (std::size_t count {}; count < encoder_threads; ++count)
                    {
                        workers.emplace_back([this] { run(); });
                    }
                }

                // 대기열이 가득 차면 수집 스레드가 기다려 메모리 사용량이 늘지 않게 한다.
                changed.wait(lock, [this] { return outstanding < limit || failed.load(); });

                if (failed.load())
                {
                    return false;
                }

                queue.push_back(std::move(work));
                ++outstanding;
            }
            catch (...)
            {
                fail(index);
                return false;
            }

            changed.notify_all();
            return true;
        }

        void drain()
        {
            std::unique_lock lock { mutex };
            changed.wait(lock, [this] { return outstanding == 0; });
        }

        std::size_t failure()
        {
            const std::lock_guard lock { mutex };
            return failed_index;
        }

        void run()
        {
            // PNG는 스레드마다 WIC 팩터리를 한 번만 만들어 프레임마다 COM을 다시 준비하지 않는다.
            std::optional<wic_png_encoder> png {};

            try
            {
                if (format == image_format::apng)
                {
                    png.emplace();
                }
            }
            catch (...)
            {}

            while (true)
            {
                job work {};
                {
                    std::unique_lock lock { mutex };
                    changed.wait(lock, [this] { return stopping || !queue.empty(); });

                    if (queue.empty())
                    {
                        return;
                    }

                    work = std::move(queue.front());
                    queue.pop_front();
                }

                bool encoded {};

                try
                {
                    // 녹화 중에는 빠른 압축을 유지하고 용량 최적화는 정지 뒤에 수행한다.
                    encoded = format == image_format::apng
                        ? png && png->encode(work.path, work.tile.pixmap())
                        : encode_native_image(
                              work.path, work.tile.pixmap(), image_format::webp, lossless_compression::fast
                          );
                }
                catch (...)
                {}

                {
                    const std::lock_guard lock { mutex };

                    if (!encoded)
                    {
                        failed_index = std::min(failed_index, work.index);
                        failed = true;
                        outstanding -= queue.size();
                        queue.clear();
                    }

                    --outstanding;
                }

                changed.notify_all();
            }
        }
    };

    animation_recorder::animation_recorder(std::filesystem::path session, const image_format format)
        : session_ { std::move(session) }
        , format_ { format }
        , pool_ { std::make_unique<encoder_pool>(format) }
        , failed_ { format != image_format::webp && format != image_format::apng }
    {}

    animation_recorder::~animation_recorder() = default;

    bool animation_recorder::append(const SkPixmap& pixels, const double timestamp_seconds)
    {
        if (failed_ || finished_ || pool_->failed.load())
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

        // 새 그림이 도착한 시각으로 이전 그림의 표시 시간을 확정한다. 압축 실패는 finish에서 이 시각까지 되돌린다.
        if (!frames_.empty())
        {
            frames_.back().duration_seconds = timestamp_seconds - last_change_;
        }

        const auto index { frames_.size() };
        const auto path { session_
                          / ("frame-" + std::to_string(index) + (format_ == image_format::webp ? ".webp" : ".png"))
        };

        frames_.push_back({ path, 0.0, change.x, change.y });
        starts_.push_back(timestamp_seconds);
        width_ = pixels.width();
        height_ = pixels.height();
        last_change_ = timestamp_seconds;
        last_sample_ = timestamp_seconds;

        // 원본 버퍼는 다음 수집에서 덮어쓰므로 변경 영역만 복사해 작업 스레드에 넘긴다.
        encoder_pool::job work { {}, path, index };
        SkPixmap tile {};

        if (!pixels.extractSubset(&tile, SkIRect::MakeXYWH(change.x, change.y, change.width, change.height))
            || !work.tile.tryAllocPixels(tile.info()) || !tile.readPixels(work.tile.pixmap()))
        {
            pool_->fail(index);
            return false;
        }

        return pool_->submit(std::move(work), static_cast<std::uint64_t>(width_) * height_ * 4);
    }

    std::vector<encoded_animation_frame> animation_recorder::finish(const double timestamp_seconds)
    {
        if (failed_ || finished_)
        {
            return {};
        }

        finished_ = true;
        pool_->drain();

        const auto failed_index { pool_->failure() };

        if (failed_index != no_failure)
        {
            // 디스크 부족 같은 저장 실패는 실패한 프레임이 도착한 시각을 마지막 그림의 끝으로 삼아
            // 그때까지 저장한 녹화를 살린다.
            if (failed_index == 0 || failed_index > frames_.size())
            {
                return {};
            }

            const double boundary { starts_[failed_index] };
            frames_.resize(failed_index);
            starts_.resize(failed_index);
            frames_.back().duration_seconds = boundary - starts_.back();
        }
        else
        {
            if (frames_.empty() || !std::isfinite(timestamp_seconds) || timestamp_seconds <= last_sample_)
            {
                return {};
            }

            frames_.back().duration_seconds = timestamp_seconds - last_change_;
        }

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

    bool animation_recorder::storage_failed() const noexcept
    {
        return pool_->failed.load();
    }

} // namespace chalkag
