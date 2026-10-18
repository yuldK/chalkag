#pragma once

#include "capture/animation_frames.h"

#include <atomic>
#include <span>

namespace chalkag {
    [[nodiscard]] bool write_gif(
        const std::filesystem::path& output,
        std::span<const encoded_animation_frame> frames,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    );
} // namespace chalkag
