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
        void add_inputs(
            const std::function<void(std::unique_ptr<luil::ui_element>)>& add,
            float width,
            float top,
            float scale,
            bool editable
        ) const;

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
    };
} // namespace chalkag
