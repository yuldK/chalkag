#pragma once

#include "capture/region_geometry.h"

#include "luil/app/app_host.h"
#include "luil/ui/ui_interaction.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace chalkag {
    struct open_region_editor_intent
    {
        capture_region initial {};
        capture_region screen_bounds {};
    };

    struct close_region_editor_intent
    {};

    struct region_editor_edit_intent
    {
        luil::text_edit_request request {};
    };

    struct region_editor_composition_intent
    {
        luil::text_composition_event event {};
    };

    struct region_editor_preset_intent
    {
        int width {};
        int height {};
    };

    struct reset_region_editor_intent
    {};

    struct region_draft_result
    {
        std::optional<capture_region> region {};
        std::u8string error {};
    };

    [[nodiscard]] std::u8string encode_region_arguments(capture_region region);
    [[nodiscard]] std::optional<capture_region> decode_region_arguments(std::u8string_view arguments) noexcept;
    [[nodiscard]] region_draft_result validate_region_draft(
        const std::array<std::u8string, 4>& draft, capture_region screen_bounds
    );
    [[nodiscard]] region_draft_result validate_region_draft(capture_region region, capture_region screen_bounds);

    class region_editor
    {
    public:
        [[nodiscard]] bool handle(const luil::app_message& message);
        [[nodiscard]] bool opened() const noexcept;
        [[nodiscard]] luil::ui_popup make_popup(float scale) const;

    private:
        void reset_draft(capture_region region);

        bool opened_ {};
        capture_region initial_ {};
        capture_region screen_bounds_ {};
        std::array<luil::text::text_edit_state, 4> fields_ {};
        std::optional<luil::text_composition_event> composition_ {};
    };

    class region_editor_policy final : public luil::interaction_policy
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
