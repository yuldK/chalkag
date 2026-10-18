#pragma once

#include <filesystem>

namespace chalkag {
    struct encoded_animation_frame
    {
        std::filesystem::path path;
        double duration_seconds {};
        int x {};
        int y {};
    };
} // namespace chalkag
