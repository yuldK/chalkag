#pragma once

#include "capture/animation_document.h"
#include "capture/region_geometry.h"
#include "capture/save_progress.h"

#include <filesystem>
#include <memory>
#include <string>

namespace chalkag {
    enum class image_format
    {
        png,
        webp,
        gif,
        apng,
    };

    enum class lossless_compression
    {
        fast,
        compact,
        maximum,
    };

    struct capture_options
    {
        int fps { 16 };
        int output_percent { 100 };
        bool include_cursor { false };
        image_format still_format { image_format::png };
        image_format animation_format { image_format::webp };
        lossless_compression compression { lossless_compression::compact };
        bool webp_lossless { true };
        int webp_quality { 80 };
    };

    struct capture_result
    {
        bool success {};
        std::u8string message;
        std::filesystem::path output;
    };

    class image_capture
    {
    public:
        explicit image_capture(std::filesystem::path executable_directory);
        ~image_capture();
        image_capture(const image_capture&) = delete;
        image_capture& operator=(const image_capture&) = delete;

        [[nodiscard]] capture_result take_screenshot(
            capture_region region, capture_options options, const progress_callback& progress = {}
        );
        [[nodiscard]] capture_result start_recording(capture_region region, capture_options options);
        [[nodiscard]] capture_result stop_recording(bool save_immediately = false);
        [[nodiscard]] capture_result save_recording(capture_options options, const progress_callback& progress = {});
        [[nodiscard]] bool has_pending_recording() const noexcept;
        [[nodiscard]] capture_region recorded_region() const noexcept;
        [[nodiscard]] double recorded_duration() const noexcept;
        [[nodiscard]] recording_snapshot snapshot() const;
        void discard_recording();
        [[nodiscard]] bool recording() const;
        [[nodiscard]] bool recording_failed() const noexcept;
        void request_cancel() noexcept;
        void cancel_recording();

    private:
        struct implementation;
        std::unique_ptr<implementation> implementation_;
    };
} // namespace chalkag
