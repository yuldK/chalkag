#pragma once

#include "encoding/image_encoder.h"

namespace chalkag::encoding {
    [[nodiscard]] bool encode_gif(
        const std::filesystem::path& output,
        const animation_source& source,
        const encoder_options& options,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    );
} // namespace chalkag::encoding
