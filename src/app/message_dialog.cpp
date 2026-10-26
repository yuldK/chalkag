#include "app/message_dialog.h"
#include "app/app_runtime.h"
#include "app/help_text.h"

#include "luil/ui/dialog_elements.h"
#include "luil/ui/modal_host_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/ui_tree.h"

namespace chalkag {
    luil::ui_popup make_message_dialog(const message_dialog_state& state, const float scale)
    {
        constexpr float width { 360 };
        constexpr auto kind { luil::application_element_kind(10) };
        const bool confirmation { state.kind != message_dialog_kind::information };
        const auto revision { std::to_string(state.revision) };
        const auto command = [revision](const runtime_command action) -> luil::input_action {
            return luil::app_ui_command { static_cast<std::uint32_t>(action), { revision.begin(), revision.end() } };
        };

        const auto dismiss { command(runtime_command::dismiss_dialog) };
        luil::stack_config layout {};
        layout.padding = { 20, 20, 20, 20 };

        auto content {
            std::make_unique<luil::stack_element>(luil::ui_element_id { kind, u8"message-content" }, layout)
        };

        auto title { std::make_unique<wrapped_help_element>(
            luil::ui_element_id { kind, u8"message-title" }, layout_help_text(state.title, width - 40, 14, 2), 14.0f
        )
        };

        const auto title_height { title->height() };
        content->add(std::move(title), title_height);
        content->add_gap(12);

        const luil::ui_element_id body_id { kind, u8"message-body" };
        auto body { std::make_unique<wrapped_help_element>(
            body_id, layout_help_text(state.body, width - 40, 12, 12), 12.0f, luil::label_color_role::dim, state.body
        )
        };

        const auto body_height { body->height() };
        content->add(std::move(body), body_height);
        content->add_gap(24);
        luil::stack_config row {};
        row.direction = luil::stack_direction::row;
        row.spacing = 10;

        auto buttons { std::make_unique<luil::stack_element>(luil::ui_element_id { kind, u8"message-buttons" }, row) };
        buttons->add_flexible_gap();

        const auto add_button = [&](const char8_t* owner,
                                    const char8_t* text,
                                    const runtime_command action,
                                    const luil::text_button_visual visual,
                                    const bool default_button) {
            const luil::text_button_config config { .text = text, .visual = visual, .default_button = default_button };
            auto button { std::make_unique<luil::text_button_element>(luil::ui_element_id { kind, owner }, config) };
            button->set_action(
                luil::ui_trigger::left_click,
                [command, action, exit = state.kind == message_dialog_kind::exit_application](
                    const luil::ui_action_context&
                ) -> std::vector<luil::input_action> {
                    if (exit && action == runtime_command::confirm_dialog)
                    {
                        return { luil::ui_command::application_exit };
                    }

                    return { command(action) };
                }
            );
            buttons->add(std::move(button), 96);
        };

        if (confirmation)
        {
            add_button(
                u8"message-cancel", u8"취소", runtime_command::dismiss_dialog, luil::text_button_visual::normal, true
            );
        }

        add_button(
            u8"message-confirm",
            !confirmation                                              ? u8"확인"
                : state.kind == message_dialog_kind::exit_application  ? u8"종료"
                : state.kind == message_dialog_kind::replace_recording ? u8"새 녹화"
                                                                       : u8"비우기",
            runtime_command::confirm_dialog,
            confirmation ? luil::text_button_visual::danger : luil::text_button_visual::accent,
            !confirmation
        );
        content->add(std::move(buttons), 32);

        const float height { 40 + title_height + 12 + body_height + 24 + 32 };
        luil::modal_host_config config {};
        config.owner = u8"message-dialog";
        config.name = state.title;
        config.scrim_opacity = 0;
        config.content_width = width;
        config.content_height = height;
        config.dismiss
            = [dismiss](const luil::ui_action_context&) -> std::vector<luil::input_action> { return { dismiss }; };
        config.focus_entry = { kind, confirmation ? u8"message-cancel" : u8"message-confirm" };
        config.focus_return = { luil::application_element_kind(0), u8"open-options" };
        luil::panel_config surface {};
        surface.corner_radius = 10;
        surface.background = [](const luil::ui_color_palette& palette) { return palette.surface_background; };
        config.surface = std::move(surface);

        auto dialog { std::make_unique<luil::modal_host_element>(std::move(config)) };
        dialog->set_content(std::move(content));
        dialog->arrange({ { 0, 0, width * scale, height * scale }, scale });
        luil::ui_popup popup {};
        popup.id = u8"message-dialog";
        popup.x = 8;
        popup.y = 38;
        popup.width = width;
        popup.height = height;
        popup.tree = std::make_shared<const luil::ui_tree>(std::move(dialog));
        popup.dismiss = [dismiss](const luil::popup_dismiss_reason reason) -> luil::input_action {
            return reason == luil::popup_dismiss_reason::escape_key ? dismiss : luil::input_action {};
        };
        return popup;
    }

} // namespace chalkag
