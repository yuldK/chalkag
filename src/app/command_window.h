#pragma once

#include "app/animation_editor.h"
#include "app/app_runtime.h"
#include "app/region_editor.h"
#include "app/shortcut_editor.h"
#include "capture/region_events.h"
#include "luil/app/app_host.h"
#include "luil/win32/win32_window.h"

#include <atomic>
#include <chrono>
#include <memory>

namespace chalkag {
    class single_instance;
    inline constexpr int command_window_width { 248 };
    inline constexpr int command_window_height { 82 };

    enum class options_page
    {
        capture,
        region,
        output,
        preferences
    };

    struct options_tab_intent
    {
        options_page page {};
    };

    struct fps_menu_intent
    {
        bool open {};
    };

    struct options_scroll_intent
    {
        float value {};
        bool absolute {};
    };

    [[nodiscard]] luil::caption_config make_command_caption();
    [[nodiscard]] bool hit_command_caption_button(const luil::ui_frame& frame, float x, float y);

    struct window_metrics_intent
    {
        float width { 0.0f };
        float height { 0.0f };
        float scale { 1.0f };
    };

    struct close_intent
    {};

    struct help_target_intent
    {
        luil::ui_element_id target {};
        std::chrono::steady_clock::time_point started_at {};
        float maximum_width { 340.0f };
        float available_above { 600.0f };
        float available_below { 600.0f };
        float minimum_x { -100000.0f };
        float maximum_x { 100000.0f };

        [[nodiscard]] bool operator==(const help_target_intent&) const = default;
    };

    struct dismiss_help_intent
    {};

    class command_driver final : public luil::logic_driver
    {
    public:
        void handle(luil::app_message message) override;
        [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_tick() override;
        void tick(std::chrono::steady_clock::time_point now) override;
        void cancel() noexcept override;
        [[nodiscard]] std::shared_ptr<const luil::ui_frame> make_frame() override;
        [[nodiscard]] luil::app_message make_close_message() override;
        [[nodiscard]] bool shutdown_completed() const override;

    private:
        window_metrics_intent metrics_ {};
        region_state_intent region_state_ {};
        runtime_state runtime_state_ {};
        region_editor editor_ {};
        animation_editor animation_editor_ {};
        shortcut_editor shortcuts_ {};
        help_target_intent help_target_ {};
        bool help_dismissed_ {};
        options_page page_ {};
        bool shortcuts_edited_ {};
        bool fps_menu_open_ {};
        float options_scroll_ {};
        std::atomic<bool> closed_ { false };
    };

    class command_delegate final : public luil::win32::window_delegate
    {
    public:
        explicit command_delegate(single_instance* instance = nullptr);
        ~command_delegate() override;
        void on_started(luil::app_host& host) override;
        void execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command) override;
        [[nodiscard]] luil::app_message make_window_metrics_message(float width, float height, float scale) override;

    private:
        std::unique_ptr<app_runtime> runtime_;
        single_instance* instance_ {};
    };
} // namespace chalkag
