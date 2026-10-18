#pragma once

#include "capture/region_geometry.h"

#include <optional>
#include <string>

namespace chalkag {
    struct region_state_intent
    {
        std::optional<capture_region> region {};
        bool selecting { false };
        std::u8string status {};
    };

    inline constexpr unsigned int select_region_ui_command { 1 };
} // namespace chalkag
