#pragma once

#include "app/hotkeys.h"
#include "capture/image_capture.h"
#include "luil/theme/appearance.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chalkag {
    struct command_window_position
    {
        int x {};
        int y {};
        bool operator==(const command_window_position&) const = default;
    };

    struct region_preset
    {
        std::u8string name;
        capture_region region;
    };

    struct app_settings
    {
        capture_options capture;
        std::vector<region_preset> presets;
        bool always_on_top { false };
        hotkey_bindings shortcuts { default_hotkeys };
        luil::appearance_settings appearance { .accent_id = u8"mint" };
        int glow_width { 24 };
        bool auto_save {};
        std::optional<command_window_position> window_position {};
    };

    class settings_store
    {
    public:
        explicit settings_store(std::filesystem::path executable_directory);

        [[nodiscard]] app_settings load(std::u8string& warning) const;
        [[nodiscard]] std::u8string save(const app_settings& settings) const;
        [[nodiscard]] const std::filesystem::path& path() const noexcept;

    private:
        std::filesystem::path path_;
    };
} // namespace chalkag
