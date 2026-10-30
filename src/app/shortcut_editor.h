#pragma once

#include "app/hotkeys.h"
#include "app/region_editor.h"

#include <functional>

namespace chalkag {
    struct shortcut_edit_intent
    {
        luil::text_edit_request request {};
    };

    struct shortcut_composition_intent
    {
        luil::text_composition_event event {};
    };

    struct reset_shortcuts_intent
    {};

    class shortcut_editor
    {
    public:
        shortcut_editor();
        void reset(const hotkey_bindings& bindings);
        [[nodiscard]] bool handle(const luil::app_message& message);
        [[nodiscard]] std::optional<hotkey_bindings> bindings() const;
        [[nodiscard]] std::u8string error() const;
        // 라벨과 입력 칸을 줄마다 짝지은 폼이다. 높이는 `inputs_height`다.
        [[nodiscard]] std::unique_ptr<luil::ui_element> make_inputs(bool editable) const;
        [[nodiscard]] static float inputs_height() noexcept;

    private:
        std::array<luil::text::text_edit_state, default_hotkeys.size()> fields_ {};
        std::optional<luil::text_composition_event> composition_ {};
    };

    class command_policy final : public luil::interaction_policy
    {
    public:
        [[nodiscard]] std::optional<luil::text_input_target> text_target_of(luil::ui_element_kind kind) const override;
        [[nodiscard]] luil::input_action make_text_edit_action(const luil::text_edit_request& request) const override;
        [[nodiscard]] luil::input_action make_text_composition_action(
            const luil::text_composition_event& event
        ) const override;
        [[nodiscard]] std::optional<luil::menu_kinds> menu() const override;
        [[nodiscard]] std::vector<luil::input_action> close_menu() const override;
        [[nodiscard]] std::vector<luil::input_action> on_key(
            const luil::ui_tree* tree, const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot
        ) override;
        [[nodiscard]] std::vector<luil::input_action> on_wheel(
            const luil::ui_tree& tree, const luil::mouse_wheel_event& event, float scroll_delta
        ) override;
    };
} // namespace chalkag
