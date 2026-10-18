#pragma once

#include "capture/animation_frames.h"
#include "capture/save_progress.h"

#include <atomic>
#include <span>
#include <string>

namespace chalkag {
    [[nodiscard]] bool write_apng(
        const std::filesystem::path& output_path,
        std::span<const encoded_animation_frame> frames,
        const std::atomic<bool>& cancelled,
        std::u8string& error,
        const progress_callback& progress = {},
        std::uint32_t play_count = 0
    );
} // namespace chalkag
