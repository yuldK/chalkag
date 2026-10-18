#pragma once

#include <cstdint>
#include <string>

namespace chalkag {
    [[nodiscard]] inline std::u8string recording_time(const std::uint64_t seconds)
    {
        const auto part = [](const std::uint64_t value) {
            const auto digits { std::to_string(value) };
            return (value < 10 ? std::u8string { u8"0" } : std::u8string {})
                + std::u8string { digits.begin(), digits.end() };
        };
        return (seconds >= 3600 ? part(seconds / 3600) + u8":" : std::u8string {}) + part(seconds / 60 % 60) + u8":"
            + part(seconds % 60);
    }
} // namespace chalkag
