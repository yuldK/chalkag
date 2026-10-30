#include "app/shortcut_editor.h"
#include "app/animation_editor.h"
#include "app/command_window.h"

#include "luil/ui/dialog_elements.h"
#include "luil/ui/grid_element.h"
#include "luil/ui/label_element.h"

namespace chalkag {
    namespace {
        constexpr std::array owners {
            u8"shortcut-select", u8"shortcut-screenshot", u8"shortcut-recording", u8"shortcut-discard"
        };
        constexpr std::array names {
            u8"영역 선택 단축키", u8"스크린샷 단축키", u8"녹화·정지 단축키", u8"녹화 비우기 단축키"
        };
        constexpr std::array labels { u8"영역 선택", u8"스크린샷", u8"녹화·정지", u8"녹화 비우기" };
        constexpr std::array<std::uint32_t, 4> input_kinds { 6, 7, 8, 11 };

        std::optional<std::size_t> shortcut_index(const luil::text_input_target target)
        {
            const auto value { static_cast<std::uint32_t>(target) };
            return value >= 201 && value < 201 + owners.size() ? std::optional { static_cast<std::size_t>(value - 201) }
                                                               : std::nullopt;
        }

    } // namespace

    shortcut_editor::shortcut_editor()
    {
        reset(default_hotkeys);
    }

    void shortcut_editor::reset(const hotkey_bindings& bindings)
    {
        for (std::size_t index {}; index < fields_.size(); ++index)
        {
            luil::text::text_edit_set_text(fields_[index], format_hotkey(bindings[index]), true);
        }

        composition_.reset();
    }

    bool shortcut_editor::handle(const luil::app_message& message)
    {
        if (message.get<reset_shortcuts_intent>() != nullptr)
        {
            reset(default_hotkeys);
            return true;
        }

        if (const auto* edit { message.get<shortcut_edit_intent>() }; edit != nullptr)
        {
            const auto index { shortcut_index(edit->request.target) };

            if (!index)
            {
                return false;
            }

            luil::apply_text_edit(fields_[*index], edit->request);
            return true;
        }

        if (const auto* event { message.get<shortcut_composition_intent>() }; event != nullptr)
        {
            if (!shortcut_index(event->event.target))
            {
                return false;
            }

            if (event->event.composing)
            {
                composition_ = event->event;
            }
            else
            {
                composition_.reset();
            }

            return true;
        }

        return false;
    }

    std::optional<hotkey_bindings> shortcut_editor::bindings() const
    {
        if (composition_ && composition_->composing)
        {
            return std::nullopt;
        }

        hotkey_bindings result {};

        for (std::size_t index {}; index < fields_.size(); ++index)
        {
            const auto binding { parse_hotkey(fields_[index].text) };

            if (!binding)
            {
                return std::nullopt;
            }

            result[index] = *binding;
        }

        return validate_hotkeys(result).empty() ? std::optional { result } : std::nullopt;
    }

    std::u8string shortcut_editor::error() const
    {
        if (composition_ && composition_->composing)
        {
            return u8"입력을 확정한 뒤 적용하세요.";
        }

        hotkey_bindings values {};

        for (std::size_t index {}; index < fields_.size(); ++index)
        {
            const auto binding { parse_hotkey(fields_[index].text) };

            if (!binding)
            {
                return std::u8string { labels[index] }
                + u8": Ctrl+Shift+A 또는 Ctrl+Alt+F9처럼 입력하세요. F12는 시스템 예약 키입니다.";
            }

            values[index] = *binding;
        }

        return validate_hotkeys(values);
    }

    std::unique_ptr<luil::ui_element> shortcut_editor::make_inputs(const bool editable) const
    {
        luil::grid_config config {};
        config.columns = { luil::grid_track::fixed(110), luil::grid_track::flexible() };
        config.rows.assign(default_hotkeys.size(), luil::grid_track::fixed(32));
        config.column_spacing = 10;
        config.row_spacing = 6;

        auto form { std::make_unique<luil::grid_element>(
            luil::ui_element_id { luil::application_element_kind(0), u8"shortcut-form" }, std::move(config)
        )
        };

        for (std::size_t index {}; index < fields_.size(); ++index)
        {
            auto label = std::make_unique<luil::label_element>(
                luil::ui_element_id { luil::application_element_kind(0), std::u8string { owners[index] } + u8"-label" },
                luil::label_config { .text = labels[index], .font_size = 12 }
            );
            form->add(std::move(label), { .row = index, .column = 0, .vertical = luil::layout_alignment::center });

            const auto target { static_cast<luil::text_input_target>(201 + index) };
            luil::text_input_config input_config {};
            input_config.placeholder = names[index];

            auto input = std::make_unique<luil::text_input_element>(
                luil::ui_element_id { luil::application_element_kind(input_kinds[index]), owners[index] },
                luil::make_text_input_view(fields_[index], composition_, target),
                std::move(input_config)
            );
            input->set_access_name(names[index]);
            input->set_enabled(editable);
            form->add(std::move(input), { .row = index, .column = 1 });
        }

        return form;
    }

    float shortcut_editor::inputs_height() noexcept
    {
        return static_cast<float>(default_hotkeys.size()) * 38.0f - 6.0f;
    }

    std::optional<luil::text_input_target> command_policy::text_target_of(const luil::ui_element_kind kind) const
    {
        if (kind == luil::application_element_kind(12))
        {
            return static_cast<luil::text_input_target>(205);
        }

        for (std::uint32_t index {}; index < input_kinds.size(); ++index)
        {
            if (kind == luil::application_element_kind(input_kinds[index]))
            {
                return static_cast<luil::text_input_target>(201 + index);
            }
        }

        if (const auto target { animation_editor_policy {}.text_target_of(kind) })
        {
            return target;
        }

        return region_editor_policy {}.text_target_of(kind);
    }

    luil::input_action command_policy::make_text_edit_action(const luil::text_edit_request& request) const
    {
        if (static_cast<std::uint32_t>(request.target) == 205)
        {
            return luil::make_app_action(preset_name_edit_intent { request });
        }

        if (static_cast<std::uint32_t>(request.target) >= 301 && static_cast<std::uint32_t>(request.target) <= 308)
        {
            return animation_editor_policy {}.make_text_edit_action(request);
        }

        if (shortcut_index(request.target))
        {
            return luil::make_app_action(shortcut_edit_intent { request });
        }

        return region_editor_policy {}.make_text_edit_action(request);
    }

    luil::input_action command_policy::make_text_composition_action(const luil::text_composition_event& event) const
    {
        if (static_cast<std::uint32_t>(event.target) == 205)
        {
            return luil::make_app_action(preset_name_composition_intent { event });
        }

        if (static_cast<std::uint32_t>(event.target) >= 301 && static_cast<std::uint32_t>(event.target) <= 308)
        {
            return animation_editor_policy {}.make_text_composition_action(event);
        }

        if (shortcut_index(event.target))
        {
            return luil::make_app_action(shortcut_composition_intent { event });
        }

        return region_editor_policy {}.make_text_composition_action(event);
    }

    std::vector<luil::input_action> command_policy::on_key(
        const luil::ui_tree* tree, const luil::key_pressed_event& event, const luil::interaction_snapshot&
    )
    {
        return animation_editor_key_actions(tree, event);
    }

    std::vector<luil::input_action> command_policy::on_wheel(
        const luil::ui_tree& tree, const luil::mouse_wheel_event& event, const float scroll_delta
    )
    {
        // 정책을 둔 앱은 휠을 직접 맡는다. 기본값은 빈 목록이라 그대로 두면 모든 휠이 버려진다.
        // 어느 창이 받을지는 각 흘리는 창이 단 scroll 출처로 luil이 정한다.
        return luil::route_wheel(tree, event, scroll_delta);
    }

    std::optional<luil::menu_kinds> command_policy::menu() const
    {
        return region_editor_policy {}.menu();
    }

    std::vector<luil::input_action> command_policy::close_menu() const
    {
        return region_editor_policy {}.close_menu();
    }

} // namespace chalkag
