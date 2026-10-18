#pragma once

#include "capture/animation_frames.h"
#include "capture/image_capture.h"

#include "include/core/SkPixmap.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <span>

namespace chalkag {
    struct webp_optimization_options
    {
        lossless_compression compression { lossless_compression::compact };
        // 0이면 논리 프로세서 절반과 작업 공간 추정 예산 안에서 최대 8개를 사용한다.
        std::size_t workers {};
        std::uint64_t memory_budget { 256ULL * 1024 * 1024 };
        bool internal_threading { false };
    };

    [[nodiscard]] bool encode_native_image(
        const std::filesystem::path& path,
        const SkPixmap& pixels,
        image_format format,
        lossless_compression compression = lossless_compression::fast,
        bool lossless = true,
        int quality = 80,
        const std::atomic<bool>* cancelled = nullptr,
        bool threading = false
    );
    [[nodiscard]] bool optimize_webp_frames(
        std::span<const encoded_animation_frame> frames,
        const std::atomic<bool>& cancelled,
        std::u8string& error,
        const progress_callback& progress = {},
        webp_optimization_options options = {}
    );

    class native_webp_writer
    {
    public:
        native_webp_writer(
            const std::filesystem::path& output,
            int width,
            int height,
            const std::atomic<bool>* cancelled = nullptr,
            std::uint16_t play_count = 0
        );
        [[nodiscard]] bool append_frame(
            const std::filesystem::path& frame, std::uint32_t duration_ms, int x = 0, int y = 0
        );
        [[nodiscard]] bool finish();

    private:
        std::ofstream output_;
        int width_ {};
        int height_ {};
        std::uint64_t size_ {};
        bool valid_ {};
        std::size_t frames_ {};
        const std::atomic<bool>* cancelled_ {};
        bool write(const void* bytes, std::size_t size);
    };
} // namespace chalkag
