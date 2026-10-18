#include "app/shortcut_editor.h"
#include "app/animation_editor.h"

#include "luil/ui/dialog_elements.h"
#include "luil/ui/label_element.h"

namespace chalkag {
    namespace {
        constexpr std::array owners { u8"shortcut-select", u8"shortcut-screenshot", u8"shortcut-recording" };
        constexpr std::array names { u8"영역 선택 단축키", u8"스크린샷 단축키", u8"녹화·정지 단축키" };
        constexpr std::array labels { u8"영역 선택", u8"스크린샷", u8"녹화·정지" };

        std::optional<std::size_t> shortcut_index(const luil::text_input_target target)
        {
            const auto value { static_cast<std::uint32_t>(target) };
            return value >= 201 && value <= 203 ? std::optional { static_cast<std::size_t>(value - 201) }
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

    void shortcut_editor::add_inputs(
        const std::function<void(std::unique_ptr<luil::ui_element>)>& add,
        const float width,
        const float top,
        const float scale,
        const bool editable
    ) const
    {
        for (std::size_t index {}; index < fields_.size(); ++index)
        {
            const float y { top + static_cast<float>(index) * 44 };
            auto label = std::make_unique<luil::label_element>(
                luil::ui_element_id { luil::application_element_kind(0), std::u8string { owners[index] } + u8"-label" },
                luil::label_config { .text = labels[index], .font_size = 12 }
            );
            label->arrange({ { 20 * scale, (y + 4) * scale, 110 * scale, 24 * scale }, scale });
            add(std::move(label));

            const auto target { static_cast<luil::text_input_target>(201 + index) };
            luil::text_input_config config {};
            config.placeholder = names[index];

            auto input = std::make_unique<luil::text_input_element>(
                luil::ui_element_id { luil::application_element_kind(static_cast<std::uint32_t>(6 + index)),
                                      owners[index]
                },
                luil::make_text_input_view(fields_[index], composition_, target),
                std::move(config)
            );
            input->set_access_name(names[index]);
            input->set_enabled(editable);
            input->arrange({ { 140 * scale, y * scale, (width - 160) * scale, 32 * scale }, scale });
            add(std::move(input));
        }
    }

    std::optional<luil::text_input_target> command_policy::text_target_of(const luil::ui_element_kind kind) const
    {
        for (std::uint32_t index {}; index < 3; ++index)
        {
            if (kind == luil::application_element_kind(6 + index))
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

    std::optional<luil::menu_kinds> command_policy::menu() const
    {
        return region_editor_policy {}.menu();
    }

    std::vector<luil::input_action> command_policy::close_menu() const
    {
        return region_editor_policy {}.close_menu();
    }

} // namespace chalkag
