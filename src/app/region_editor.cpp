#include "app/region_editor.h"
#include "app/app_runtime.h"
#include "app/command_window.h"

#include "app/help_text.h"

#include "luil/ui/dialog_elements.h"
#include "luil/ui/label_element.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/root_element.h"
#include "luil/ui/ui_tree.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace chalkag {
    namespace {
        constexpr luil::ui_element_kind editor_kind { luil::application_element_kind(5) };
        constexpr float editor_width { 400.0f };
        constexpr float editor_height { 360.0f };
        constexpr std::array field_owners { u8"region-x", u8"region-y", u8"region-width", u8"region-height" };
        constexpr std::array field_labels { u8"왼쪽 X", u8"위쪽 Y", u8"너비", u8"높이" };
        constexpr std::array field_names { u8"영역 왼쪽 X", u8"영역 위쪽 Y", u8"영역 너비", u8"영역 높이" };

        luil::ui_element_id field_id(const std::size_t index)
        {
            return { luil::application_element_kind(static_cast<std::uint32_t>(index) + 1), field_owners[index] };
        }

        luil::text_input_target field_target(const std::size_t index) noexcept
        {
            return static_cast<luil::text_input_target>(101 + index);
        }

        std::optional<std::size_t> field_index(const luil::text_input_target target) noexcept
        {
            const auto value { static_cast<std::uint32_t>(target) };

            if (value < 101 || value > 104)
            {
                return std::nullopt;
            }

            return static_cast<std::size_t>(value - 101);
        }

        std::optional<int> parse_integer(std::u8string_view value) noexcept
        {
            if (value.empty())
            {
                return std::nullopt;
            }

            const bool negative { value.front() == u8'-' };

            if (negative || value.front() == u8'+')
            {
                value.remove_prefix(1);
            }

            if (value.empty())
            {
                return std::nullopt;
            }

            const auto maximum { static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + (negative ? 1U : 0U) };
            std::uint64_t magnitude {};

            for (const auto character : value)
            {
                if (character < u8'0' || character > u8'9')
                {
                    return std::nullopt;
                }

                const auto digit { static_cast<std::uint64_t>(character - u8'0') };

                if (magnitude > (maximum - digit) / 10)
                {
                    return std::nullopt;
                }

                magnitude = magnitude * 10 + digit;
            }

            const auto signed_value { negative ? -static_cast<std::int64_t>(magnitude)
                                               : static_cast<std::int64_t>(magnitude)
            };
            return static_cast<int>(signed_value);
        }

        std::u8string number_text(const int value)
        {
            const auto text { std::to_string(value) };
            return { text.begin(), text.end() };
        }

        std::int64_t right_of(const capture_region region) noexcept
        {
            return static_cast<std::int64_t>(region.x) + region.width;
        }

        std::int64_t bottom_of(const capture_region region) noexcept
        {
            return static_cast<std::int64_t>(region.y) + region.height;
        }

        bool valid_screen(const capture_region screen) noexcept
        {
            return screen.width >= minimum_region_size && screen.height >= minimum_region_size
                && right_of(screen) <= std::numeric_limits<int>::max()
                && bottom_of(screen) <= std::numeric_limits<int>::max();
        }

        bool preset_fits(const capture_region screen, const int width, const int height) noexcept
        {
            return valid_screen(screen) && width >= minimum_region_size && height >= minimum_region_size
                && width <= screen.width && height <= screen.height;
        }

        luil::ui_action close_action()
        {
            return luil::make_message_action(close_region_editor_intent {});
        }

    } // namespace

    std::u8string encode_region_arguments(const capture_region region)
    {
        return number_text(region.x) + u8"," + number_text(region.y) + u8"," + number_text(region.width) + u8","
            + number_text(region.height);
    }

    std::optional<capture_region> decode_region_arguments(const std::u8string_view arguments) noexcept
    {
        std::array<int, 4> values {};
        std::size_t start {};

        for (std::size_t index {}; index < values.size(); ++index)
        {
            const auto separator { arguments.find(u8',', start) };

            if ((index + 1 < values.size()) != (separator != std::u8string_view::npos))
            {
                return std::nullopt;
            }

            const auto end { separator == std::u8string_view::npos ? arguments.size() : separator };
            const auto value { parse_integer(arguments.substr(start, end - start)) };

            if (!value)
            {
                return std::nullopt;
            }

            values[index] = *value;
            start = end + 1;
        }

        return capture_region { values[0], values[1], values[2], values[3] };
    }

    region_draft_result validate_region_draft(const capture_region region, const capture_region screen_bounds)
    {
        if (!valid_screen(screen_bounds))
        {
            return { .error = u8"화면 범위를 확인할 수 없습니다. 영역 창을 다시 열어 주세요." };
        }

        if (region.width < minimum_region_size || region.height < minimum_region_size)
        {
            return { .error = u8"너비와 높이는 각각 16px 이상이어야 합니다." };
        }

        if (right_of(region) > std::numeric_limits<int>::max() || bottom_of(region) > std::numeric_limits<int>::max())
        {
            return { .error = u8"좌표와 크기의 합이 지원하는 범위를 넘었습니다." };
        }

        if (region.x < screen_bounds.x || region.y < screen_bounds.y || right_of(region) > right_of(screen_bounds)
            || bottom_of(region) > bottom_of(screen_bounds))
        {
            return { .error = u8"영역 전체가 화면 안에 들어오도록 좌표와 크기를 조절하세요." };
        }

        return { .region = region };
    }

    region_draft_result validate_region_draft(
        const std::array<std::u8string, 4>& draft, const capture_region screen_bounds
    )
    {
        std::array<int, 4> values {};

        for (std::size_t index {}; index < draft.size(); ++index)
        {
            const auto value { parse_integer(draft[index]) };

            if (!value)
            {
                return { .error = std::u8string { field_labels[index] }
                             + u8"에 부호와 정수만 입력하세요. 값이 너무 크면 줄여 주세요."
                };
            }

            values[index] = *value;
        }

        return validate_region_draft(capture_region { values[0], values[1], values[2], values[3] }, screen_bounds);
    }

    void region_editor::reset_draft(const capture_region region)
    {
        const std::array values { region.x, region.y, region.width, region.height };

        for (std::size_t index {}; index < fields_.size(); ++index)
        {
            luil::text::text_edit_set_text(fields_[index], number_text(values[index]), true);
        }

        fields_[2].anchor = 0;
        composition_.reset();
    }

    bool region_editor::handle(const luil::app_message& message)
    {
        if (const auto* const open { message.get<open_region_editor_intent>() }; open != nullptr)
        {
            initial_ = open->initial;
            screen_bounds_ = open->screen_bounds;
            reset_draft(initial_);
            opened_ = true;
            return true;
        }

        if (message.get<close_region_editor_intent>() != nullptr)
        {
            opened_ = false;
            composition_.reset();
            return true;
        }

        if (!opened_)
        {
            return false;
        }

        if (const auto* const edit { message.get<region_editor_edit_intent>() }; edit != nullptr)
        {
            const auto index { field_index(edit->request.target) };

            if (!index)
            {
                return false;
            }

            luil::apply_text_edit(fields_[*index], edit->request);
            return true;
        }

        if (const auto* const composition { message.get<region_editor_composition_intent>() }; composition != nullptr)
        {
            if (!field_index(composition->event.target))
            {
                return false;
            }

            if (composition->event.composing)
            {
                composition_ = composition->event;
            }
            else if (composition_ && composition_->target == composition->event.target)
            {
                composition_.reset();
            }

            return true;
        }

        if (message.get<reset_region_editor_intent>() != nullptr)
        {
            reset_draft(initial_);
            return true;
        }

        if (const auto* const preset { message.get<region_editor_preset_intent>() }; preset != nullptr)
        {
            if (!preset_fits(screen_bounds_, preset->width, preset->height))
            {
                return true;
            }

            const auto x { parse_integer(fields_[0].text).value_or(initial_.x) };
            const auto y { parse_integer(fields_[1].text).value_or(initial_.y) };
            const auto left { std::clamp<std::int64_t>(x, screen_bounds_.x, right_of(screen_bounds_) - preset->width) };
            const auto top {
                std::clamp<std::int64_t>(y, screen_bounds_.y, bottom_of(screen_bounds_) - preset->height)
            };
            reset_draft({ static_cast<int>(left), static_cast<int>(top), preset->width, preset->height });
            return true;
        }

        return false;
    }

    bool region_editor::opened() const noexcept
    {
        return opened_;
    }

    luil::ui_popup region_editor::make_popup(const float requested_scale) const
    {
        if (!opened_)
        {
            return {};
        }

        const float scale { std::isfinite(requested_scale) && requested_scale > 0.0f ? requested_scale : 1.0f };
        auto root { std::make_unique<luil::root_element>(u8"region-editor-content") };
        const auto label = [&](const char8_t* owner,
                               const std::u8string& text,
                               const float x,
                               const float y,
                               const float width,
                               const float height,
                               const float font_size,
                               const luil::label_color_role color) {
            auto element { std::make_unique<wrapped_help_element>(
                luil::ui_element_id { editor_kind, owner },
                layout_help_text(text, width, font_size, 2),
                font_size,
                color,
                text
            )
            };
            element->arrange({ { x * scale, y * scale, width * scale, height * scale }, scale });
            root->add(std::move(element));
        };
        label(u8"region-editor-title", u8"캡쳐 영역 설정", 20, 14, 360, 26, 16, luil::label_color_role::primary);
        label(
            u8"region-editor-hint",
            u8"좌표와 크기는 물리 픽셀(px) 기준입니다.\n왼쪽·위쪽 모니터의 음수 좌표도 입력할 수 있습니다.",
            20,
            44,
            360,
            36,
            11,
            luil::label_color_role::dim
        );

        for (std::size_t index {}; index < fields_.size(); ++index)
        {
            const float x { index % 2 == 0 ? 20.0f : 208.0f };
            const float y { index < 2 ? 88.0f : 146.0f };
            const std::u8string owner { std::u8string { field_owners[index] } + u8"-label" };
            label(owner.c_str(), field_labels[index], x, y, 172, 18, 11, luil::label_color_role::dim);
            luil::text_input_config config {};
            config.placeholder = field_names[index];
            config.input = { .kind = luil::text_input_kind::number, .signed_number = true };

            auto input { std::make_unique<luil::text_input_element>(
                field_id(index),
                luil::make_text_input_view(fields_[index], composition_, field_target(index)),
                std::move(config)
            )
            };
            input->set_access_name(field_names[index]);
            input->arrange({ { x * scale, (y + 18) * scale, 172 * scale, 32 * scale }, scale });
            root->add(std::move(input));
        }

        const auto add_button = [&](const char8_t* owner,
                                    const char8_t* text,
                                    const luil::rect_f box,
                                    const luil::ui_action& action,
                                    const bool enabled,
                                    const luil::text_button_visual visual = luil::text_button_visual::normal,
                                    const bool default_button = false) {
            auto button = std::make_unique<luil::text_button_element>(
                luil::ui_element_id { editor_kind, owner },
                luil::text_button_config { .text = text, .visual = visual, .default_button = default_button }
            );
            button->set_action(luil::ui_trigger::left_click, action);
            button->set_enabled(enabled);
            button->set_cursor(enabled ? luil::ui_cursor::hand : luil::ui_cursor::arrow);
            button->arrange({ { box.x * scale, box.y * scale, box.width * scale, box.height * scale }, scale });
            root->add(std::move(button));
        };
        label(u8"region-editor-size-title", u8"크기 빠르게 맞추기", 20, 207, 208, 22, 11, luil::label_color_role::dim);
        add_button(
            u8"reset-region-editor",
            u8"현재 영역으로",
            { 244, 204, 136, 26 },
            luil::make_message_action(reset_region_editor_intent {}),
            true,
            luil::text_button_visual::link
        );

        constexpr std::array presets { capture_region { 0, 0, 640, 360 },
                                       capture_region { 0, 0, 1280, 720 },
                                       capture_region { 0, 0, 1920, 1080 }
        };

        constexpr std::array preset_names { u8"640 × 360", u8"1280 × 720", u8"1920 × 1080" };
        constexpr std::array preset_owners { u8"region-size-640", u8"region-size-1280", u8"region-size-1920" };

        for (std::size_t index {}; index < presets.size(); ++index)
        {
            const auto preset { presets[index] };
            add_button(
                preset_owners[index],
                preset_names[index],
                { 20 + static_cast<float>(index) * 124, 234, 112, 26 },
                luil::make_message_action(region_editor_preset_intent { preset.width, preset.height }),
                preset_fits(screen_bounds_, preset.width, preset.height)
            );
        }

        std::array<std::u8string, 4> draft {};

        for (std::size_t index {}; index < fields_.size(); ++index)
        {
            draft[index] = fields_[index].text;
        }

        auto validation { validate_region_draft(draft, screen_bounds_) };

        if (composition_ && composition_->composing)
        {
            validation.region.reset();
            validation.error = u8"입력을 확정한 뒤 적용하세요.";
        }

        label(
            u8"region-editor-error",
            validation.error.empty() ? std::u8string { u8"값을 확인한 뒤 적용하세요." } : validation.error,
            20,
            268,
            360,
            38,
            12,
            validation.error.empty() ? luil::label_color_role::dim : luil::label_color_role::error
        );
        add_button(u8"cancel-region-editor", u8"취소", { 200, 314, 84, 30 }, close_action(), true);
        luil::ui_action apply {};

        if (validation.region)
        {
            const auto region { *validation.region };
            apply = [region](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                return { luil::app_ui_command { .command
                                                = static_cast<std::uint32_t>(runtime_command::apply_region_editor),
                                                .argument = encode_region_arguments(region)
                }
                };
            };
        }

        add_button(
            u8"apply-region-editor",
            u8"적용",
            { 296, 314, 84, 30 },
            apply,
            validation.region.has_value(),
            luil::text_button_visual::accent,
            true
        );
        root->arrange({ { 0, 0, editor_width * scale, editor_height * scale }, scale });
        luil::panel_config panel_config {};

        panel_config.background = [](const luil::ui_color_palette& palette) { return palette.surface_background; };
        panel_config.corner_radius = 10;
        panel_config.border = [](const luil::ui_color_palette& palette) { return palette.divider; };
        auto panel { std::make_unique<luil::panel_element>(
            luil::ui_element_id { editor_kind, u8"region-editor-card" }, std::move(panel_config)
        )
        };
        panel->set_content(std::move(root));
        panel->set_focus_trap(true);
        panel->set_focus_entry(field_id(2));
        panel->set_focus_return({ luil::application_element_kind(0), u8"select-region" });
        panel->set_dismiss_action(close_action());
        panel->arrange({ { 0, 0, editor_width * scale, editor_height * scale }, scale });
        luil::ui_popup popup {};
        popup.id = u8"region-editor";
        popup.x = 8;
        popup.y = 68;
        popup.width = editor_width;
        popup.height = editor_height;
        popup.tree = std::make_shared<const luil::ui_tree>(std::move(panel));
        popup.dismiss = [](const luil::popup_dismiss_reason reason) -> luil::input_action {
            if (reason == luil::popup_dismiss_reason::pointer_press_outside
                || reason == luil::popup_dismiss_reason::escape_key)
            {
                return luil::make_app_action(close_region_editor_intent {});
            }

            return {};
        };
        return popup;
    }

    std::optional<luil::text_input_target> region_editor_policy::text_target_of(const luil::ui_element_kind kind) const
    {
        for (std::size_t index {}; index < 4; ++index)
        {
            if (kind == luil::application_element_kind(static_cast<std::uint32_t>(index) + 1))
            {
                return field_target(index);
            }
        }

        return std::nullopt;
    }

    luil::input_action region_editor_policy::make_text_edit_action(const luil::text_edit_request& request) const
    {
        if (!field_index(request.target))
        {
            return {};
        }

        return luil::make_app_action(region_editor_edit_intent { request });
    }

    luil::input_action region_editor_policy::make_text_composition_action(
        const luil::text_composition_event& event
    ) const
    {
        if (!field_index(event.target))
        {
            return {};
        }

        return luil::make_app_action(region_editor_composition_intent { event });
    }

    std::optional<luil::menu_kinds> region_editor_policy::menu() const
    {
        return luil::menu_kinds { luil::ui_element_kind::menu, luil::ui_element_kind::menu_item };
    }

    std::vector<luil::input_action> region_editor_policy::close_menu() const
    {
        return { luil::make_app_action(fps_menu_intent { false }) };
    }

} // namespace chalkag
