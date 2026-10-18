#pragma once

#include "capture/animation_frames.h"
#include "capture/region_geometry.h"
#include "encoding/image_encoder.h"
#include "include/core/SkBitmap.h"

#include <memory>
#include <vector>

namespace chalkag {
    struct recording_snapshot
    {
        std::vector<encoded_animation_frame> frames {};
        capture_region region {};
        std::shared_ptr<void> storage {};
    };

    // 원본은 압축 상태로 보관하고 필요한 화면만 복원한다.
    class animation_document
    {
    public:
        static std::shared_ptr<animation_document> open(const std::filesystem::path& path, std::u8string& error);
        static std::shared_ptr<animation_document> from_recording(recording_snapshot snapshot);
        ~animation_document();
        [[nodiscard]] int width() const noexcept;
        [[nodiscard]] int height() const noexcept;
        [[nodiscard]] int play_count() const noexcept;
        [[nodiscard]] const std::vector<int>& durations() const noexcept;
        [[nodiscard]] bool read(std::size_t index, SkBitmap& bitmap, const std::atomic<bool>& cancelled);

    private:
        animation_document();
        class impl;
        std::unique_ptr<impl> impl_;
    };

    struct edited_frame
    {
        std::size_t source {};
        int milliseconds {};
        bool operator==(const edited_frame&) const = default;
    };

    struct animation_edit
    {
        std::vector<edited_frame> frames {};
        capture_region crop {};
        int output_width {};
        int output_height {};
        bool operator==(const animation_edit&) const = default;
    };

    [[nodiscard]] animation_edit initial_animation_edit(const animation_document& document);
    [[nodiscard]] std::int64_t animation_duration(const animation_edit& edit) noexcept;
    [[nodiscard]] animation_edit reduce_animation(
        const animation_edit& edit, int stride, int phase, bool preserve_duration
    );
    [[nodiscard]] bool valid_animation_edit(const animation_edit& edit, int width, int height) noexcept;

    class edited_animation_reader
    {
    public:
        edited_animation_reader(
            std::shared_ptr<animation_document> document,
            animation_edit edit,
            bool flatten_alpha,
            const std::atomic<bool>& cancelled
        );
        [[nodiscard]] encoding::animation_source source();

    private:
        bool read(std::size_t index, encoding::animation_frame& frame);
        std::shared_ptr<animation_document> document_;
        animation_edit edit_;
        bool flatten_alpha_ {};
        bool valid_ {};
        const std::atomic<bool>& cancelled_;
        SkBitmap canvas_ {};
        SkBitmap output_ {};
    };
} // namespace chalkag
