#pragma once

#include "app/message_dialog.h"
#include "app/settings.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace luil {
    class app_host;
    struct app_ui_command;
} // namespace luil

namespace chalkag {
    enum class runtime_command : std::uint32_t
    {
        select_region = 1,
        take_screenshot,
        toggle_recording,
        toggle_options,
        set_fps,
        set_scale,
        set_still_format,
        set_animation_format,
        toggle_cursor,
        save_preset,
        previous_preset,
        next_preset,
        apply_preset,
        delete_preset,
        open_output_folder,
        toggle_always_on_top,
        show_region_editor,
        apply_region_editor,
        set_compression,
        adjust_scale,
        set_scale_index,
        set_shortcuts,
        set_theme,
        set_accent,
        set_glow_width,
        adjust_glow_width,
        save_recording,
        discard_recording,
        toggle_auto_save,
        set_webp_mode,
        set_webp_quality,
        adjust_webp_quality,
        dismiss_dialog,
        confirm_dialog,
        open_saved_file,
        edit_recording,
        open_animation,
        show_animation_editor,
    };

    struct runtime_state
    {
        app_settings settings {};
        bool options_open {};
        bool busy {};
        save_progress progress {};
        bool recording {};
        bool settings_writable { true };
        int preset_index {};
        float output_step { 3.0f };
        std::u8string status {};
        std::u8string shortcut_error {};
        std::uint64_t shortcut_revision {};
        std::uint64_t elapsed_seconds {};
        float glow_position { 24 };
        bool pending_recording {};
        capture_region recorded_region {};
        double recorded_duration {};
        float quality_position { 80 };
        std::u8string saved_summary {};
        std::filesystem::path saved_output {};
        message_dialog_state dialog {};
    };

    inline constexpr int options_window_width { 400 };
    inline constexpr int options_window_height { 810 };

    class app_runtime final
    {
    public:
        explicit app_runtime(luil::app_host& host);
        ~app_runtime();
        void execute(const luil::app_ui_command& command);

    private:
        class impl;
        std::unique_ptr<impl> impl_;
    };
} // namespace chalkag
