#include "app/command_window.h"
#include "app/accent_picker.h"
#include "app/help_text.h"
#include "app/image_drop.h"
#include "capture/recording_time.h"

#include "luil/generated/codicons.h"
#include "luil/ui/badge_element.h"
#include "luil/ui/button_element.h"
#include "luil/ui/check_element.h"
#include "luil/ui/choice_group_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/dropdown_element.h"
#include "luil/ui/glyph_element.h"
#include "luil/ui/label_element.h"
#include "luil/ui/list_element.h"
#include "luil/ui/menu_element.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/progress_element.h"
#include "luil/ui/root_element.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/slider_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/tab_bar_element.h"
#include "luil/ui/theme_picker_element.h"
#include "luil/ui/ui_tree.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chalkag {
    namespace {
        constexpr luil::ui_element_kind command_kind { luil::application_element_kind(0) };
        constexpr float toolbar_padding { 8.0f };
        constexpr float toolbar_spacing { 6.0f };
        constexpr float toolbar_button_height { 36.0f };

        class output_slider_element final : public luil::ui_element
        {
        public:
            explicit output_slider_element(luil::slider_config config)
                : ui_element { { command_kind, u8"scale" } }
                , slider_ { { command_kind, u8"scale" }, config }
            {
                if (const auto* target { slider_.pointer_drag() }; target != nullptr)
                {
                    set_pointer_drag_target(*target);
                }

                luil::key_step_target steps {};
                steps.axis = luil::focus_axis::horizontal;
                steps.on_step = [change = config.change, change_to = config.change_to](
                                    const luil::value_step step
                                ) -> std::optional<std::vector<luil::input_action>> {
                    if (step == luil::value_step::minimum || step == luil::value_step::maximum)
                    {
                        return std::vector<luil::input_action> {
                            change_to(step == luil::value_step::minimum ? 0.0f : 5.0f)
                        };
                    }

                    const bool decrease { step == luil::value_step::decrease
                                          || step == luil::value_step::decrease_page
                    };
                    return std::vector<luil::input_action> { change(decrease ? -1.0f : 1.0f) };
                };
                set_key_step_target(std::move(steps));
                set_cursor(luil::ui_cursor::resize_horizontal);
                set_tab_stop(true);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                slider_.set_enabled(enabled());
                slider_.arrange(context);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                slider_.draw(context, interaction);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                auto info { slider_.accessibility() };
                info.name = u8"출력 배율 · 25, 50, 75, 100, 150, 200% 단계";
                return info;
            }

            [[nodiscard]] std::optional<std::vector<luil::input_action>> access_actions(
                const luil::access_request& request
            ) const override
            {
                return slider_.access_actions(request);
            }

        private:
            luil::slider_element slider_;
        };

        [[nodiscard]] std::shared_ptr<const luil::ui_style> command_style(const bool recording)
        {
            static const auto style = [] {
                auto value { luil::default_ui_style() };
                value.tab_bar.variant = luil::tab_bar_variant::connected;
                value.metrics.control_corner_radius = 7.0f;
                value.metrics.row_corner_radius = 7.0f;
                value.light.window_background = luil::make_ui_color(255, 255, 255);
                value.light.surface_background = luil::make_ui_color(243, 246, 245);
                value.light.input_background = value.light.surface_background;
                value.light.caption.background = value.light.window_background;
                value.dark.window_background = luil::make_ui_color(32, 36, 39);
                value.dark.surface_background = luil::make_ui_color(42, 48, 51);
                value.dark.input_background = value.dark.surface_background;
                value.dark.caption.background = value.dark.window_background;
                return std::make_shared<const luil::ui_style>(value);
            }();

            static const auto recording_style = [] {
                auto value { *style };
                value.light.caption.background = luil::make_ui_color(255, 226, 230);
                value.dark.caption.background = luil::make_ui_color(82, 31, 39);
                return std::make_shared<const luil::ui_style>(value);
            }();
            return recording ? recording_style : style;
        }

        std::u8string_view option_button_help(const runtime_command action)
        {
            // 옵션 버튼의 안내는 같은 버튼이 실행하는 명령에 맞춰 고른다.
            switch (action)
            {
            case runtime_command::save_preset:
                return u8"현재 영역의 위치와 크기를 프리셋으로 저장합니다.";

            case runtime_command::apply_preset:
                return u8"선택한 프리셋을 캡쳐 영역으로 적용합니다.";

            case runtime_command::delete_preset:
                return u8"선택한 영역 프리셋을 삭제합니다.";

            case runtime_command::previous_preset:
            case runtime_command::next_preset:
                return u8"저장한 영역 프리셋을 선택합니다. 적용 버튼으로 현재 영역에 불러오세요.";

            case runtime_command::open_output_folder:
                return u8"실행 파일 옆 data/captures 폴더를 엽니다.";

            case runtime_command::save_recording:
                return u8"보관한 녹화 원본을 현재 형식·배율·압축 설정으로 저장합니다. 원본은 저장 후에도 유지됩니다.";

            case runtime_command::discard_recording:
                return u8"보관한 녹화 원본을 비웁니다. 이미 저장한 파일은 유지됩니다. 확인 후 새 녹화를 시작할 수 "
                       u8"있습니다.";

            case runtime_command::edit_recording:
                return u8"보관한 녹화 원본을 편집창에서 엽니다. 프레임 삭제·간격 조절·크롭·출력 크기 변경을 할 수 "
                       u8"있습니다.";

            case runtime_command::show_animation_editor:
                return u8"이미지를 여기에 끌어 놓거나 편집창의 이미지 열기로 GIF·WebP·APNG·PNG 파일을 선택합니다. "
                       u8"편집본은 새 파일로 "
                       u8"저장합니다.";

            default:
                return {};
            }
        }

        struct help_content
        {
            std::u8string title {};
            std::u8string shortcut {};
            std::u8string body {};
        };

        [[nodiscard]] luil::ui_popup make_help_popup(
            const help_content& content, const luil::rect_f& anchor, const help_target_intent& target, const float scale
        )
        {
            constexpr float padding { 12.0f };
            constexpr float gap { 6.0f };
            const float width { std::min(340.0f, target.maximum_width) };
            const float text_width { width - padding * 2.0f };
            const float maximum_height { std::max(target.available_above, target.available_below) };
            auto root { std::make_unique<luil::root_element>() };
            float y { padding };

            const auto add_text = [&](const char8_t* owner,
                                      const std::u8string& text,
                                      const float size,
                                      const luil::label_color_role color,
                                      const std::size_t max_lines) {
                if (text.empty())
                {
                    return;
                }

                auto element { std::make_unique<wrapped_help_element>(
                    luil::ui_element_id { command_kind, owner },
                    layout_help_text(text, text_width, size, max_lines),
                    size,
                    color,
                    text
                )
                };

                const float height { element->height() };
                element->arrange({ { padding * scale, y * scale, text_width * scale, height * scale }, scale });
                root->add(std::move(element));
                y += height + gap;
            };
            add_text(u8"help-title", content.title, 13.0f, luil::label_color_role::primary, 2);
            add_text(u8"help-shortcut", content.shortcut, 11.0f, luil::label_color_role::dim, 1);

            const auto body_lines { static_cast<std::size_t>(std::max(1.0f, (maximum_height - y - padding) / 19.0f)) };
            add_text(u8"help-body", content.body, 12.0f, luil::label_color_role::primary, body_lines);

            const float height { y - gap + padding };
            root->arrange({ { 0.0f, 0.0f, width * scale, height * scale }, scale });
            luil::panel_config panel_config {};

            panel_config.background = [](const luil::ui_color_palette& palette) { return palette.tooltip_background; };
            auto panel { std::make_unique<luil::panel_element>(
                luil::ui_element_id { command_kind, u8"help-background" }, std::move(panel_config)
            )
            };

            // 안내 위에서도 hover를 유지하지만 버튼과 초점을 만들지는 않는다.
            panel->set_cursor(luil::ui_cursor::arrow);
            panel->set_tab_stop(false);
            panel->set_content(std::move(root));
            panel->arrange({ { 0.0f, 0.0f, width * scale, height * scale }, scale });
            luil::ui_popup popup {};
            popup.id = u8"command-help";
            popup.x
                = std::clamp(anchor.x / scale, target.minimum_x, std::max(target.minimum_x, target.maximum_x - width));
            popup.y = target.available_below >= height ? (anchor.y + anchor.height) / scale + 8.0f
                                                       : anchor.y / scale - height - 8.0f;
            popup.width = width;
            popup.height = height;
            popup.tree = std::make_shared<const luil::ui_tree>(std::move(panel));
            popup.dismiss = [](luil::popup_dismiss_reason) -> luil::input_action {
                return luil::make_app_action(dismiss_help_intent {});
            };
            return popup;
        }

    } // namespace

    luil::caption_config make_command_caption()
    {
        luil::caption_config caption {};
        caption.title = u8"chalkag";
        caption.application_icon = luil::codicons::icon_device_camera;
        caption.close_tooltip = u8"닫기";
        caption.metrics.height = 30;
        caption.metrics.title_font_size = 12;
        caption.metrics.title_left_padding = 8;
        caption.metrics.application_icon_slot_width = 28;
        caption.metrics.application_icon_size = 14;
        caption.metrics.title_icon_gap = 4;
        caption.metrics.button_width = 28;
        caption.metrics.button_icon_size = 12;
        caption.buttons = { .minimize = false, .maximize = false, .close = true };
        return caption;
    }

    bool hit_command_caption_button(const luil::ui_frame& frame, const float x, const float y)
    {
        const auto* const hit { frame.tree != nullptr ? frame.tree->hit_test(x, y) : nullptr };
        return hit != nullptr && hit->id().kind == command_kind && hit->id().owner == u8"toggle-topmost";
    }

    void command_driver::handle(luil::app_message message)
    {
        if (message.get<revert_shortcuts_intent>())
        {
            shortcuts_.reset(runtime_state_.settings.shortcuts);
            shortcuts_edited_ = false;
            return;
        }
        if (const auto* menu { message.get<preset_menu_intent>() })
        {
            preset_menu_open_ = menu->open;
            return;
        }
        if (const auto* scroll { message.get<preset_scroll_intent>() })
        {
            preset_scroll_ = scroll->absolute ? scroll->value : preset_scroll_ + scroll->value;
            return;
        }
        if (const auto* edit { message.get<preset_name_edit_intent>() })
        {
            luil::apply_text_edit(preset_name_, edit->request);
            return;
        }
        if (const auto* composition { message.get<preset_name_composition_intent>() })
        {
            preset_composition_ = composition->event.composing ? std::optional { composition->event } : std::nullopt;
            return;
        }

        if (animation_editor_.handle(message))
        {
            return;
        }

        if (const auto* const scroll { message.get<options_scroll_intent>() }; scroll != nullptr)
        {
            options_scroll_ = scroll->absolute ? scroll->value : options_scroll_ + scroll->value;
            fps_menu_open_ = false;
            help_dismissed_ = true;
            return;
        }

        if (const auto* const tab { message.get<options_tab_intent>() }; tab != nullptr)
        {
            page_ = tab->page;
            preset_menu_open_ = false;
            options_scroll_ = 0;
            fps_menu_open_ = false;
            help_dismissed_ = true;
            return;
        }

        if (const auto* const menu { message.get<fps_menu_intent>() }; menu != nullptr)
        {
            fps_menu_open_ = menu->open && runtime_state_.options_open && page_ == options_page::capture
                && !runtime_state_.busy && !runtime_state_.recording && runtime_state_.settings_writable;
            help_dismissed_ = true;
            return;
        }

        if (editor_.handle(message))
        {
            if (editor_.opened())
            {
                fps_menu_open_ = false;
            }

            return;
        }

        if (page_ == options_page::preferences && !runtime_state_.busy && !runtime_state_.recording
            && runtime_state_.settings_writable && shortcuts_.handle(message))
        {
            shortcuts_edited_ = true;
            return;
        }

        if (message.get<dismiss_help_intent>() != nullptr)
        {
            help_dismissed_ = true;
            return;
        }

        if (const auto* const target { message.get<help_target_intent>() }; target != nullptr)
        {
            if (target->target != help_target_.target || target->started_at != help_target_.started_at)
            {
                help_dismissed_ = false;
            }

            help_target_ = *target;
            return;
        }

        if (message.get<close_intent>() != nullptr)
        {
            closed_.store(true);
            return;
        }

        if (const auto* const metrics { message.get<window_metrics_intent>() }; metrics != nullptr)
        {
            metrics_ = *metrics;
            return;
        }

        if (const auto* const state { message.get<region_state_intent>() }; state != nullptr)
        {
            region_state_ = *state;

            if (state->selecting)
            {
                fps_menu_open_ = false;
                static_cast<void>(editor_.handle(luil::app_message { close_region_editor_intent {} }));
            }
        }

        if (const auto* const state { message.get<runtime_state>() }; state != nullptr)
        {
            const bool applied { runtime_state_.shortcut_revision != state->shortcut_revision };

            if (applied && state->shortcut_error.empty())
            {
                shortcuts_edited_ = false;
            }

            if (runtime_state_.settings.shortcuts != state->settings.shortcuts
                || (applied && state->shortcut_error.empty()))
            {
                shortcuts_.reset(state->settings.shortcuts);
            }

            const auto preset_name {
                state->settings.presets.empty()
                    ? std::u8string {}
                    : state->settings.presets[static_cast<std::size_t>(state->preset_index)].name
            };
            if (preset_name != preset_name_source_)
            {
                preset_name_source_ = preset_name;
                luil::text::text_edit_set_text(preset_name_, preset_name, true);
                preset_composition_.reset();
            }
            if (runtime_state_.preset_index != state->preset_index)
            {
                preset_menu_open_ = false;
            }

            runtime_state_ = *state;

            if (!state->options_open || state->busy || state->recording
                || state->dialog.kind != message_dialog_kind::none)
            {
                fps_menu_open_ = false;
            }

            if (state->busy || state->recording)
            {
                static_cast<void>(editor_.handle(luil::app_message { close_region_editor_intent {} }));
            }
        }
    }

    std::shared_ptr<const luil::ui_frame> command_driver::make_frame()
    {
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        const float width { metrics_.width > 0.0f ? metrics_.width : command_window_width * scale };
        const float height { metrics_.height > 0.0f ? metrics_.height : command_window_height * scale };
        // 이번 상태의 제목줄과 명령 영역을 먼저 만들고 펼침 옵션·보조 창을 뒤에 붙인다.
        luil::caption_config caption_config { make_command_caption() };

        if (runtime_state_.recording)
        {
            caption_config.title = u8"녹화 · " + recording_time(runtime_state_.elapsed_seconds);
        }
        else if (runtime_state_.busy)
        {
            caption_config.title = u8"chalkag · 저장 중";
        }
        else if (runtime_state_.countdown > 0)
        {
            caption_config.title = u8"chalkag · " + runtime_state_.countdown_label;
        }

        const float caption_height { luil::caption_element::height_for(caption_config) * scale };

        auto root { std::make_unique<luil::root_element>() };
        std::unordered_map<std::u8string, help_content> help {};
        root->arrange({ { 0.0f, 0.0f, width, height }, scale });

        auto caption { std::make_unique<luil::caption_element>(caption_config) };
        caption->arrange({ { 0.0f, 0.0f, width, caption_height }, scale });
        root->add(std::move(caption));

        if (runtime_state_.recording)
        {
            // 녹화 표시는 명령 영역에만 칠하고 설정과 버튼의 테마 팔레트는 유지한다.
            luil::panel_config background {};
            background.background = [](const luil::ui_color_palette& palette) { return palette.caption.background; };

            auto panel { std::make_unique<luil::panel_element>(
                luil::ui_element_id { command_kind, u8"recording-background" }, background
            )
            };
            panel->arrange(
                { { 0,
                    caption_height,
                    width,
                    std::max(0.0f, std::min(height, command_window_height * scale) - caption_height)
                },
                  scale
                }
            );
            root->add(std::move(panel));
        }

        const bool dialog_open { runtime_state_.dialog.kind != message_dialog_kind::none };
        const bool counting { runtime_state_.countdown > 0 };

        // 버튼·링크에 공통으로 붙이는 명령과 손가락 커서다.
        const auto command_action = [](const runtime_command action, std::u8string argument = {}) {
            return [action, argument](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                return { luil::app_ui_command { static_cast<std::uint32_t>(action), argument } };
            };
        };

        const auto make_text_button = [&](const char8_t* owner,
                                          const std::u8string& text,
                                          const runtime_command action,
                                          const bool enabled,
                                          const luil::text_button_visual visual = luil::text_button_visual::normal,
                                          const char32_t glyph = 0) {
            auto element { std::make_unique<luil::text_button_element>(
                luil::ui_element_id { command_kind, owner },
                luil::text_button_config { .text = text, .visual = visual, .glyph = glyph }
            )
            };
            element->set_access_name(text);
            element->set_action(luil::ui_trigger::left_click, command_action(action));
            element->set_enabled(enabled && !dialog_open);
            element->set_cursor(element->enabled() ? luil::ui_cursor::hand : luil::ui_cursor::arrow);
            return element;
        };

        const auto make_label = [&](const char8_t* owner,
                                    const std::u8string& text,
                                    const float size = 12.0f,
                                    const luil::label_color_role color = luil::label_color_role::primary) {
            help.emplace(owner, help_content { .title = u8"캡쳐 안내", .body = text });
            auto element { std::make_unique<luil::label_element>(
                luil::ui_element_id { command_kind, owner },
                luil::label_config { .text = text, .font_size = size, .color = color }
            )
            };
            element->set_cursor(luil::ui_cursor::arrow);
            return element;
        };

        // 폭에 맞춰 줄을 바꾼 안내와 그 높이다. 높이는 담는 쪽이 세어 stack에 넘긴다.
        const auto make_note = [&](const char8_t* owner,
                                   const std::u8string& text,
                                   const float text_width,
                                   const float size = 11.0f,
                                   const std::size_t max_lines = 3,
                                   const luil::label_color_role color = luil::label_color_role::dim) {
            help.emplace(owner, help_content { .title = u8"캡쳐 안내", .body = text });
            auto element { std::make_unique<wrapped_help_element>(
                luil::ui_element_id { command_kind, owner },
                layout_help_text(text, text_width, size, max_lines),
                size,
                color,
                text
            )
            };
            element->set_cursor(luil::ui_cursor::arrow);
            const float note_height { text.empty() ? 0.0f : element->height() };
            return std::pair { std::move(element), note_height };
        };

        const auto row_config = [](const float spacing = 8.0f) {
            return luil::stack_config { .direction = luil::stack_direction::row, .spacing = spacing };
        };

        // 누를 수 없는 명령은 안내 맨 앞에 지금 막힌 이유를 적는다.
        const auto explain_disabled = [&](const luil::ui_element& element, const char8_t* owner) {
            if (element.enabled())
            {
                return;
            }

            const auto reason { dialog_open                    ? u8"열린 안내를 닫은 뒤 사용할 수 있습니다."
                                    : runtime_state_.busy      ? u8"저장이 끝나면 사용할 수 있습니다."
                                    : runtime_state_.recording ? u8"녹화를 정지한 뒤 사용할 수 있습니다."
                                    : counting ? u8"지연 촬영을 기다리는 중입니다. 취소 버튼으로 멈출 수 있습니다."
                                    : region_state_.selecting ? u8"영역 선택을 완료하거나 Esc로 취소하세요."
                                                              : u8"현재 상태에서는 이 명령을 사용할 수 없습니다."
            };
            auto& content { help[owner] };
            content.body = std::u8string { reason } + u8"\n\n" + content.body;
        };

        // 첫 줄은 영역 선택·사진·녹화(정지)·저장과 옵션이다.
        if (!runtime_state_.busy)
        {
            auto toolbar { std::make_unique<luil::stack_element>(
                luil::ui_element_id { command_kind, u8"toolbar" }, row_config(toolbar_spacing)
            )
            };

            auto select { make_text_button(
                u8"select-region",
                u8"영역 선택",
                runtime_command::select_region,
                !runtime_state_.recording && !counting,
                region_state_.region ? luil::text_button_visual::normal : luil::text_button_visual::accent,
                luil::codicons::icon_screen_full
            )
            };
            select->set_access_name(u8"캡쳐 영역 선택");
            help_content select_help { .title = u8"캡쳐 영역 선택",
                                       .shortcut = format_hotkey(runtime_state_.settings.shortcuts[0]),
                                       .body = u8"화면을 드래그하여 캡쳐 영역을 선택합니다.\n모서리로 크기를 조절하고 "
                                               u8"상단 핸들로 이동합니다. Esc로 선택을 취소합니다."
            };
            if (!region_state_.status.empty())
            {
                select_help.body += u8"\n\n" + region_state_.status;
            }
            help.emplace(u8"select-region", std::move(select_help));
            explain_disabled(*select, u8"select-region");
            toolbar->add_fit(std::move(select));

            const bool can_shoot { !runtime_state_.recording && !region_state_.selecting && !counting };
            auto shot { make_text_button(
                u8"take-screenshot",
                u8"사진",
                runtime_command::take_screenshot,
                can_shoot,
                luil::text_button_visual::normal,
                luil::codicons::icon_device_camera
            )
            };
            shot->set_access_name(u8"스크린샷 찍기");
            help.emplace(
                u8"take-screenshot",
                help_content { .title = u8"스크린샷 찍기",
                               .shortcut = format_hotkey(runtime_state_.settings.shortcuts[1]),
                               .body = u8"선택한 영역을 "
                                   + std::u8string { runtime_state_.settings.capture.still_format == image_format::png
                                                         ? u8"PNG"
                                                         : u8"WebP"
                                   }
                                   + u8" 이미지로 저장합니다. 영역이 없으면 먼저 영역을 고른 뒤 바로 촬영합니다.\n"
                                     u8"저장 위치: 실행 파일 옆 data/captures"
                }
            );
            explain_disabled(*shot, u8"take-screenshot");
            toolbar->add_fit(std::move(shot));

            // 녹화 버튼은 녹화와 정지만 맡는다. 보관 원본이 있으면 새 녹화로 시작한다.
            const bool recording { runtime_state_.recording };
            auto record { make_text_button(
                u8"toggle-recording",
                recording ? u8"정지" : u8"녹화",
                recording                              ? runtime_command::toggle_recording
                    : runtime_state_.pending_recording ? runtime_command::new_recording
                                                       : runtime_command::toggle_recording,
                (recording || (!region_state_.selecting && !counting)),
                recording ? luil::text_button_visual::danger : luil::text_button_visual::normal,
                recording ? luil::codicons::icon_debug_stop : luil::codicons::icon_record
            )
            };
            record->set_access_name(
                recording                              ? u8"녹화 정지"
                    : runtime_state_.pending_recording ? u8"새 녹화 시작"
                                                       : u8"애니메이션 녹화 시작"
            );
            help.emplace(
                u8"toggle-recording",
                help_content {
                    .title = record->accessibility().name,
                    .shortcut = format_hotkey(runtime_state_.settings.shortcuts[2])
                        + (recording ? std::u8string {} : std::u8string { u8" · 다시 누르면 정지" }),
                    .body = recording ? u8"녹화를 정지하고 원본을 보관합니다. 저장 버튼으로 파일을 만듭니다."
                        : runtime_state_.pending_recording
                        ? u8"보관한 녹화를 비우고 새로 녹화합니다. 아직 저장하지 않았다면 먼저 확인합니다."
                        : u8"선택한 영역을 애니메이션 이미지로 녹화합니다. 영역이 없으면 먼저 영역을 고른 뒤 바로 "
                          u8"시작합니다.\n정지 후 원본을 보관하고 저장 버튼으로 파일을 만듭니다."
                }
            );
            explain_disabled(*record, u8"toggle-recording");
            toolbar->add_fit(std::move(record));

            if (runtime_state_.pending_recording && !recording)
            {
                auto save { make_text_button(
                    u8"toolbar-save-recording",
                    runtime_state_.recording_saved ? u8"다시 저장" : u8"저장",
                    runtime_command::save_recording,
                    !counting && !region_state_.selecting,
                    runtime_state_.recording_saved ? luil::text_button_visual::normal
                                                   : luil::text_button_visual::accent,
                    luil::codicons::icon_save
                )
                };
                save->set_access_name(u8"보관 녹화 저장");
                help.emplace(
                    u8"toolbar-save-recording",
                    help_content { .title = u8"보관 녹화 저장",
                                   .body
                                   = u8"보관한 녹화를 내보내기 탭의 형식·크기·품질로 저장합니다. 원본은 저장 후에도 "
                                     u8"남아 다른 설정으로 다시 저장할 수 있습니다."
                    }
                );
                toolbar->add_fit(std::move(save));
            }

            if (counting)
            {
                auto cancel { make_text_button(
                    u8"cancel-countdown",
                    u8"취소",
                    runtime_command::cancel_countdown,
                    true,
                    luil::text_button_visual::danger
                )
                };
                cancel->set_access_name(u8"지연 촬영 취소");
                help.emplace(
                    u8"cancel-countdown",
                    help_content { .title = u8"지연 촬영 취소", .body = u8"기다리는 촬영을 시작하지 않습니다." }
                );
                toolbar->add_fit(std::move(cancel));
            }

            toolbar->add_flexible_gap();

            luil::button_config options_config {};
            options_config.glyph = luil::codicons::icon_settings_gear;
            options_config.icon_size = 16.0f;
            options_config.corner_radius = 6.0f;
            options_config.active = runtime_state_.options_open;
            auto options { std::make_unique<luil::button_element>(
                luil::ui_element_id { command_kind, u8"open-options" }, options_config
            )
            };
            options->set_access_name(u8"옵션 / 영역 프리셋");
            options->set_action(luil::ui_trigger::left_click, command_action(runtime_command::toggle_options));
            options->set_cursor(luil::ui_cursor::hand);
            options->set_enabled(!dialog_open);
            help.emplace(
                u8"open-options",
                help_content { .title = u8"옵션 / 영역 프리셋",
                               .body
                               = u8"형식·FPS·출력 크기·단축키를 설정하고 영역 프리셋을 관리합니다.\n바꾼 설정은 실행 "
                                 u8"파일 옆 data 폴더에 바로 저장됩니다."
                }
            );
            toolbar->add(std::move(options), toolbar_button_height);
            toolbar->arrange(
                { { toolbar_padding * scale,
                    caption_height + toolbar_padding * scale,
                    width - toolbar_padding * 2.0f * scale,
                    toolbar_button_height * scale
                },
                  scale
                }
            );
            root->add(std::move(toolbar));
        }
        else
        {
            const auto text { save_progress_text(runtime_state_.progress) };
            auto label { make_label(u8"save-progress-label", text) };
            label->arrange(
                { { toolbar_padding * scale,
                    caption_height + 4.0f * scale,
                    width - toolbar_padding * 2.0f * scale,
                    24.0f * scale
                },
                  scale
                }
            );
            root->add(std::move(label));

            if (const auto fraction { runtime_state_.progress.fraction() })
            {
                auto progress = std::make_unique<luil::progress_element>(
                    luil::ui_element_id { command_kind, u8"save-progress" },
                    luil::progress_config { .value = *fraction, .thickness = 5 }
                );
                progress->set_tooltip(text);
                progress->arrange(
                    { { toolbar_padding * scale,
                        caption_height + 32.0f * scale,
                        width - toolbar_padding * 2.0f * scale,
                        5.0f * scale
                    },
                      scale
                    }
                );
                root->add(std::move(progress));
            }
            else
            {
                auto detail { make_label(
                    u8"save-progress-detail", u8"완료되면 명령 버튼으로 돌아갑니다", 10.0f, luil::label_color_role::dim
                )
                };
                detail->arrange(
                    { { toolbar_padding * scale,
                        caption_height + 28.0f * scale,
                        width - toolbar_padding * 2.0f * scale,
                        18.0f * scale
                    },
                      scale
                    }
                );
                root->add(std::move(detail));
            }
        }

        // 항상 위 핀은 제목줄에 둔다.
        {
            luil::button_config pin_config {};
            pin_config.glyph
                = runtime_state_.settings.always_on_top ? luil::codicons::icon_pinned : luil::codicons::icon_pin;
            pin_config.icon_size = 16.0f;
            pin_config.corner_radius = 6.0f;
            pin_config.active = runtime_state_.settings.always_on_top;
            auto pin { std::make_unique<luil::button_element>(
                luil::ui_element_id { command_kind, u8"toggle-topmost" }, pin_config
            )
            };
            pin->set_access_name(
                runtime_state_.settings.always_on_top ? u8"항상 위 · 고정됨" : u8"항상 위 · 고정 해제됨"
            );
            pin->set_action(luil::ui_trigger::left_click, command_action(runtime_command::toggle_always_on_top));
            pin->set_enabled(runtime_state_.settings_writable && !dialog_open);
            pin->set_cursor(pin->enabled() ? luil::ui_cursor::hand : luil::ui_cursor::arrow);
            help.emplace(
                u8"toggle-topmost",
                help_content {
                    .title = pin->accessibility().name,
                    .body = runtime_state_.settings_writable
                        ? u8"명령창을 다른 창보다 위에 표시합니다. 다시 누르면 해제합니다.\n재실행해도 설정이 "
                          u8"유지됩니다. "
                          u8"캡쳐 프레임은 항상 위에 표시됩니다."
                        : u8"기존 설정 파일을 읽을 수 없어 변경을 중단했습니다. data/settings.json을 확인하세요."
                }
            );
            pin->arrange({ { width - 60.0f * scale, 0, 28.0f * scale, caption_height }, scale });
            root->add(std::move(pin));
        }

        // 둘째 줄은 이미지 열기·녹화 편집 링크와 현재 상태 배지다.
        {
            auto links { std::make_unique<luil::stack_element>(
                luil::ui_element_id { command_kind, u8"quick-links" }, row_config(14.0f)
            )
            };
            const bool can_open { !runtime_state_.busy && !runtime_state_.recording && !counting };
            auto open { make_text_button(
                u8"quick-open-image",
                u8"이미지 열기",
                runtime_command::open_animation,
                can_open,
                luil::text_button_visual::link
            )
            };
            help.emplace(
                u8"quick-open-image",
                help_content { .title = u8"이미지 열기",
                               .body = std::u8string { option_button_help(runtime_command::show_animation_editor) }
                }
            );
            links->add_fit(std::move(open));

            if (runtime_state_.pending_recording)
            {
                auto edit { make_text_button(
                    u8"quick-edit-recording",
                    u8"녹화 편집",
                    runtime_command::edit_recording,
                    can_open,
                    luil::text_button_visual::link
                )
                };
                help.emplace(
                    u8"quick-edit-recording",
                    help_content { .title = u8"녹화 편집",
                                   .body = std::u8string { option_button_help(runtime_command::edit_recording) }
                    }
                );
                links->add_fit(std::move(edit));
            }

            links->add_flexible_gap();

            if (runtime_state_.busy)
            {
                auto cancel { make_text_button(
                    u8"cancel-save",
                    runtime_state_.save_cancel_requested ? u8"취소 중…" : u8"저장 취소",
                    runtime_command::cancel_save,
                    !runtime_state_.save_cancel_requested,
                    luil::text_button_visual::link
                )
                };
                links->add_fit(std::move(cancel));
            }
            else
            {
                const std::u8string state_text {
                    counting                                     ? runtime_state_.countdown_label
                        : runtime_state_.recording               ? u8"녹화 중"
                        : !runtime_state_.queued_capture.empty() ? u8"영역을 고르면 " + runtime_state_.queued_capture
                        : runtime_state_.pending_recording
                        ? (runtime_state_.recording_saved ? u8"저장됨 · 원본 보관" : u8"저장 전 · 원본 보관")
                        : region_state_.region ? u8"촬영 준비됨"
                                               : u8"영역 선택부터 시작"
                };
                auto badge { std::make_unique<luil::badge_element>(
                    luil::ui_element_id { command_kind, u8"capture-state" },
                    luil::badge_config { .text = state_text,
                                         .tone = (runtime_state_.pending_recording && !runtime_state_.recording_saved)
                                                 || counting
                                             ? luil::badge_tone::warning
                                             : luil::badge_tone::accent
                    }
                )
                };
                links->add_fit(std::move(badge));
            }

            links->arrange({ { 12.0f * scale, 84.0f * scale, width - 24.0f * scale, 26.0f * scale }, scale });
            root->add(std::move(links));
        }

        // 셋째 줄은 안내 → 경고 → 저장 결과 → 진행 상태 순서로 하나만 보인다.
        {
            const float status_width { width / scale - 24.0f };
            const std::u8string warning {
                !runtime_state_.settings_writable
                    ? std::u8string { u8"설정을 읽을 수 없습니다 · data/settings.json을 확인하세요." }
                    : !runtime_state_.shortcut_error.empty()
                    ? std::u8string { u8"단축키 등록 확인 필요 · 앱 설정에서 조합을 변경하세요." }
                    : std::u8string {}
            };
            const bool show_saved { !runtime_state_.saved_output.empty() && !runtime_state_.busy
                                    && !runtime_state_.recording && runtime_state_.notice.empty() && warning.empty()
                                    && !counting && runtime_state_.queued_capture.empty()
            };

            if (show_saved)
            {
                auto result { std::make_unique<luil::stack_element>(
                    luil::ui_element_id { command_kind, u8"saved-result" }, row_config(10.0f)
                )
                };
                auto check { std::make_unique<luil::glyph_element>(
                    luil::ui_element_id { command_kind, u8"saved-icon" },
                    luil::glyph_config { .glyph = luil::codicons::icon_pass, .font_size = 14.0f }
                )
                };
                result->add(std::move(check), 16.0f);
                const bool unsaved_draft { runtime_state_.pending_recording && !runtime_state_.recording_saved };
                result->add_fit(make_label(
                    u8"saved-caption",
                    unsaved_draft ? u8"마지막 저장" : u8"저장 완료",
                    11.0f,
                    luil::label_color_role::dim
                ));
                const auto filename { runtime_state_.saved_output.filename().u8string() };
                auto file { make_text_button(
                    u8"saved-file", filename, runtime_command::open_saved_file, true, luil::text_button_visual::link
                )
                };
                help[u8"saved-file"] = { .title = u8"저장 파일 열기", .body = runtime_state_.saved_output.u8string() };
                result->add_flexible(std::move(file));
                auto copy { make_text_button(
                    u8"copy-saved", u8"복사", runtime_command::copy_saved_output, true, luil::text_button_visual::link
                )
                };
                help[u8"copy-saved"]
                    = { .title = u8"클립보드에 복사",
                        .body = u8"저장한 파일을 클립보드에 넣습니다. 사진은 그림으로도 붙여 넣을 수 있습니다."
                    };
                result->add_fit(std::move(copy));
                auto folder { make_text_button(
                    u8"output-folder",
                    u8"저장 폴더 열기",
                    runtime_command::open_output_folder,
                    true,
                    luil::text_button_visual::link
                )
                };
                help[u8"output-folder"]
                    = { .title = u8"저장 폴더 열기",
                        .body = std::u8string { option_button_help(runtime_command::open_output_folder) }
                    };
                result->add_fit(std::move(folder));
                result->arrange({ { 12.0f * scale, 116.0f * scale, width - 24.0f * scale, 28.0f * scale }, scale });
                root->add(std::move(result));
            }
            else
            {
                const auto& status { !runtime_state_.notice.empty() ? runtime_state_.notice
                                         : !warning.empty()         ? warning
                                         : !runtime_state_.queued_capture.empty()
                                         ? std::u8string { u8"화면을 드래그하면 바로 시작합니다 · Esc로 취소" }
                                         : !region_state_.region && !runtime_state_.busy && !runtime_state_.recording
                                             && !counting
                                         ? std::u8string { u8"① 영역 선택 → ② 사진 또는 녹화 → ③ 저장" }
                                         : runtime_state_.status
                };
                const bool alert { runtime_state_.notice.empty() ? !warning.empty() : runtime_state_.notice_error };
                auto [label, label_height] { make_note(
                    u8"command-status",
                    status,
                    status_width,
                    11.0f,
                    2,
                    alert ? luil::label_color_role::error : luil::label_color_role::primary
                )
                };
                label->set_tooltip(status);
                label->arrange(
                    { { 12.0f * scale, 116.0f * scale, width - 24.0f * scale, std::max(label_height, 18.0f) * scale },
                      scale
                    }
                );
                root->add(std::move(label));
            }
        }

        if (runtime_state_.options_open)
        {
            const bool editable { !runtime_state_.busy && !runtime_state_.recording && runtime_state_.settings_writable
                                  && !dialog_open && !counting
            };
            const float content_width { width / scale - 40.0f };
            const float options_top { static_cast<float>(command_window_height) };
            const auto value_text = [](const int value) -> std::u8string {
                const std::string text { std::to_string(value) };
                return { text.begin(), text.end() };
            };

            // 머리는 제목·설명 한 줄과 탭이다. 흘리지 않는다.
            {
                auto header { std::make_unique<luil::stack_element>(
                    luil::ui_element_id { command_kind, u8"options-header" }, row_config(10.0f)
                )
                };
                header->add_fit(make_label(u8"options-title", u8"옵션", 15.0f));
                header->add_flexible(make_label(
                    u8"options-description",
                    page_ == options_page::preferences ? u8"단축키는 적용을 눌러야 저장됩니다"
                                                       : u8"바꾼 설정은 바로 저장됩니다",
                    11.0f,
                    luil::label_color_role::dim
                ));
                header->arrange(
                    { { 20.0f * scale, (options_top + 4.0f) * scale, content_width * scale, 26.0f * scale }, scale }
                );
                root->add(std::move(header));
            }

            luil::tab_bar_config tabs {};
            tabs.owner = u8"options-tabs";
            tabs.items = { { u8"capture", u8"캡쳐" },
                           { u8"region", u8"영역" },
                           { u8"output", u8"내보내기" },
                           { u8"preferences", u8"앱 설정" }
            };
            tabs.selected = page_ == options_page::region ? u8"region"
                : page_ == options_page::output           ? u8"output"
                : page_ == options_page::preferences      ? u8"preferences"
                                                          : u8"capture";
            tabs.tab_width = content_width / 4;
            tabs.min_tab_width = 80;
            if (runtime_state_.pending_recording)
            {
                tabs.items[2].badge = luil::tab_badge {
                    .text = runtime_state_.recording_saved ? u8"완료" : u8"1",
                    .tone = runtime_state_.recording_saved ? luil::badge_tone::accent : luil::badge_tone::warning,
                    .description = runtime_state_.recording_saved ? u8"저장됨" : u8"저장 전 녹화 1개"
                };
            }
            if (shortcuts_edited_)
            {
                tabs.items[3].indicator = luil::tab_indicator { .description = u8"적용 전 단축키 변경" };
            }
            tabs.select = [](const std::u8string& key) -> luil::input_action {
                const auto page = key == u8"region" ? options_page::region
                    : key == u8"output"             ? options_page::output
                    : key == u8"preferences"        ? options_page::preferences
                                                    : options_page::capture;
                return luil::make_app_action(options_tab_intent { page });
            };

            auto tab_bar { std::make_unique<luil::tab_bar_element>(std::move(tabs)) };
            tab_bar->set_enabled(!dialog_open);
            tab_bar->arrange(
                { { 20.0f * scale, (options_top + 36.0f) * scale, content_width * scale, 32.0f * scale }, scale }
            );
            root->add(std::move(tab_bar));

            // 본문은 위에서 아래로 쌓고, 담은 높이를 세어 흘리는 창의 내용 높이로 쓴다.
            luil::stack_config page_config {};
            page_config.padding = { 20.0f, 14.0f, 20.0f, 20.0f };
            auto page { std::make_unique<luil::stack_element>(
                luil::ui_element_id { command_kind, u8"options-content" }, page_config
            )
            };
            float page_height { page_config.padding.top + page_config.padding.bottom };
            const auto add = [&](std::unique_ptr<luil::ui_element> element, const float length) {
                page_height += length;
                page->add(std::move(element), length);
            };
            const auto gap = [&](const float length) {
                page_height += length;
                page->add_gap(length);
            };
            const auto add_note = [&](const char8_t* owner, const std::u8string& text, const std::size_t lines = 3) {
                auto [element, note_height] { make_note(owner, text, content_width, 11.0f, lines) };

                if (note_height > 0.0f)
                {
                    add(std::move(element), note_height);
                }
            };
            const auto heading = [&](const char8_t* owner, const std::u8string& text) {
                add(make_label(owner, text, 13.0f), 24.0f);
                gap(6.0f);
            };
            // 왼쪽 라벨과 오른쪽 조작 요소를 한 줄에 둔다.
            const auto field = [&](const char8_t* owner,
                                   const std::u8string& text,
                                   std::unique_ptr<luil::ui_element> control,
                                   const float control_height = 30.0f) {
                auto line { std::make_unique<luil::stack_element>(
                    luil::ui_element_id { command_kind, std::u8string { owner } + u8"-row" }, row_config(12.0f)
                )
                };
                line->add(make_label(owner, text), 108.0f);
                line->add_flexible(std::move(control));
                add(std::move(line), control_height);
            };
            const auto choice = [&](const char8_t* owner,
                                    const char8_t* name,
                                    std::vector<luil::choice_item> items,
                                    const std::u8string& selected,
                                    const runtime_command action,
                                    const bool available = true) {
                luil::choice_group_config config {};
                config.owner = owner;
                config.name = name;
                config.style = luil::choice_style::toggle;

                const std::u8string prefix { std::u8string { owner } + u8"/" };

                for (auto& item : items)
                {
                    item.value = prefix + item.value;

                    const auto body = [&]() -> const char8_t* {
                        if (action == runtime_command::set_compression)
                        {
                            return u8"저장 버튼을 누른 뒤 압축합니다. 균형은 무손실 화질을 유지하며 시간과 용량을 "
                                   u8"절충합니다. 최대 압축은 더 작은 파일을 탐색하므로 오래 걸릴 수 있습니다. "
                                   u8"손실·무손실은 바로 위 WebP 압축에서 선택합니다.";
                        }

                        if (item.value == u8"animation-format/gif")
                        {
                            return u8"추가 설치 없이 GIF를 저장합니다. 색상은 최대 256개이며 프레임 시간은 10ms "
                                   u8"단위입니다.";
                        }

                        if (action == runtime_command::set_animation_format)
                        {
                            return u8"APNG는 무손실입니다. WebP는 아래 WebP 압축에서 손실·무손실과 품질을 선택할 수 "
                                   u8"있습니다.";
                        }

                        if (action == runtime_command::set_still_format)
                        {
                            return u8"PNG는 무손실입니다. WebP는 내보내기 탭에서 손실·무손실 압축을 선택할 수 "
                                   u8"있습니다.";
                        }

                        if (action == runtime_command::set_capture_delay)
                        {
                            return u8"사진·녹화를 누른 뒤 이 시간만큼 기다렸다가 시작합니다. 메뉴를 열어 둔 화면을 "
                                   u8"찍을 때 "
                                   u8"씁니다.";
                        }

                        return u8"기본 인코더는 추가 설치 없이 동작합니다.";
                    }();
                    help.emplace(
                        item.value,
                        help_content { .title = std::u8string { name } + u8" · " + item.label, .body = body }
                    );
                }

                config.items = std::move(items);
                config.selected = prefix + selected;
                config.select = [action, prefix](const std::u8string& value) -> luil::input_action {
                    return luil::app_ui_command { static_cast<std::uint32_t>(action), value.substr(prefix.size()) };
                };

                const float choice_height { luil::choice_group_element::height_for(config) };
                auto element { std::make_unique<luil::choice_group_element>(std::move(config)) };
                element->set_enabled(editable && available);
                return std::pair<std::unique_ptr<luil::ui_element>, float> { std::move(element), choice_height };
            };
            const auto check_box = [&](const char8_t* owner,
                                       const std::u8string& text,
                                       const bool checked,
                                       const runtime_command action,
                                       const bool enabled,
                                       const std::u8string& body) {
                luil::check_config config {};
                config.owner = owner;
                config.label = text;
                config.checked = checked;
                config.toggle = command_action(action);
                auto element { std::make_unique<luil::check_element>(std::move(config)) };
                element->set_enabled(enabled && !dialog_open);
                help[owner] = { .title = text, .body = body };
                add(std::move(element), luil::check_row_height);
            };
            // 같은 폭으로 나눈 버튼 줄이다.
            const auto button_row = [&](const char8_t* owner, std::vector<std::unique_ptr<luil::ui_element>> buttons) {
                auto line { std::make_unique<luil::stack_element>(
                    luil::ui_element_id { command_kind, owner }, row_config(10.0f)
                )
                };
                for (auto& button : buttons)
                {
                    line->add_flexible(std::move(button));
                }
                add(std::move(line), 32.0f);
            };
            const auto option_button = [&](const char8_t* owner,
                                           const char8_t* text,
                                           const runtime_command action,
                                           const bool enabled,
                                           const luil::text_button_visual visual = luil::text_button_visual::normal) {
                auto element { make_text_button(owner, text, action, enabled, visual) };
                help.emplace(
                    owner, help_content { .title = text, .body = std::u8string { option_button_help(action) } }
                );
                return element;
            };

            const auto& settings { runtime_state_.settings };
            const auto selected_region { region_state_.region };

            // 캡쳐 탭은 무엇을 어떻게 수집할지만 둔다. 출력 크기·품질은 내보내기 탭이 맡는다.
            if (page_ == options_page::capture)
            {
                heading(u8"still-heading", u8"사진");
                {
                    auto [element, choice_height] { choice(
                        u8"still-format",
                        u8"정지 이미지 형식",
                        { { u8"png", u8"PNG" }, { u8"webp", u8"WebP" } },
                        settings.capture.still_format == image_format::png ? u8"png" : u8"webp",
                        runtime_command::set_still_format
                    )
                    };
                    field(u8"still-label", u8"형식", std::move(element), choice_height);
                }
                gap(8.0f);
                {
                    const int percent { settings.capture.output_percent };
                    auto line { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"still-size-row" }, row_config(12.0f)
                    )
                    };
                    line->add(make_label(u8"still-size-label", u8"저장 크기"), 108.0f);
                    line->add_flexible(make_label(
                        u8"output-dimensions",
                        selected_region ? value_text(selected_region->width * percent / 100) + u8" × "
                                + value_text(selected_region->height * percent / 100) + u8" px · " + value_text(percent)
                                + u8"%"
                                        : std::u8string { u8"영역을 선택하세요" }
                    ));
                    auto link { make_text_button(
                        u8"output-settings",
                        u8"크기·품질 변경",
                        runtime_command::show_output_options,
                        true,
                        luil::text_button_visual::link
                    )
                    };
                    help[u8"output-settings"]
                        = { .title = u8"출력 크기·품질",
                            .body
                            = u8"내보내기 탭에서 출력 크기와 WebP 품질·압축 속도를 바꿉니다. 사진과 애니메이션에 함께 "
                              u8"적용됩니다."
                        };
                    line->add_fit(std::move(link));
                    add(std::move(line), 26.0f);
                }
                gap(6.0f);
                check_box(
                    u8"copy-clipboard",
                    u8"사진을 클립보드에도 복사",
                    settings.copy_to_clipboard,
                    runtime_command::toggle_clipboard,
                    editable,
                    u8"사진을 저장한 뒤 그림과 파일을 클립보드에 넣습니다. 채팅·문서에 바로 붙여 넣을 수 있습니다."
                );
                gap(18.0f);
                heading(u8"record-heading", u8"녹화");
                {
                    luil::dropdown_config fps {};
                    fps.owner = u8"fps";
                    fps.placeholder = u8"프레임 속도";
                    const int rate { settings.capture.fps };
                    fps.text = value_text(rate) + u8" fps · "
                        + (rate == 10       ? u8"가볍게"
                               : rate == 16 ? u8"균형"
                               : rate == 24 ? u8"자연스럽게"
                               : rate == 33 ? u8"고속"
                                            : u8"부드럽게");
                    fps.open = fps_menu_open_;
                    fps.toggle = luil::make_message_action(fps_menu_intent { !fps_menu_open_ });
                    fps.set_open = [](const bool open) -> luil::input_action {
                        return luil::make_app_action(fps_menu_intent { open });
                    };
                    auto dropdown { std::make_unique<luil::dropdown_element>(std::move(fps)) };
                    dropdown->set_access_name(u8"프레임 속도");
                    dropdown->set_enabled(editable && !runtime_state_.pending_recording);
                    help.emplace(
                        u8"fps",
                        help_content {
                            .title = u8"프레임 속도",
                            .body = u8"초당 캡쳐할 프레임 수입니다. 높은 FPS는 움직임을 부드럽게 하며 CPU와 디스크 "
                                    u8"사용량을 늘립니다."
                        }
                    );
                    field(u8"fps-label", u8"프레임 속도", std::move(dropdown), 32.0f);
                }
                if (runtime_state_.pending_recording)
                {
                    gap(4.0f);
                    add_note(u8"fps-locked", u8"보관 녹화가 있는 동안 프레임 속도와 커서 포함은 바꿀 수 없습니다.");
                }
                gap(18.0f);
                heading(u8"common-heading", u8"공통");
                {
                    const int delay { settings.capture_delay };
                    auto [element, choice_height] { choice(
                        u8"capture-delay",
                        u8"촬영 지연",
                        { { u8"0", u8"바로" }, { u8"3", u8"3초" }, { u8"5", u8"5초" }, { u8"10", u8"10초" } },
                        value_text(delay),
                        runtime_command::set_capture_delay
                    )
                    };
                    field(u8"delay-label", u8"촬영 지연", std::move(element), choice_height);
                }
                gap(8.0f);
                check_box(
                    u8"include-cursor",
                    u8"마우스 커서 포함",
                    settings.capture.include_cursor,
                    runtime_command::toggle_cursor,
                    editable && !runtime_state_.pending_recording,
                    u8"촬영 당시의 마우스 포인터를 사진과 녹화에 넣습니다."
                );
            }
            // 영역 좌표·크기와 프리셋을 관리한다.
            else if (page_ == options_page::region)
            {
                {
                    luil::panel_config card_config {};
                    card_config.corner_radius = 8.0f;
                    auto card { std::make_unique<luil::panel_element>(
                        luil::ui_element_id { command_kind, u8"region-card" }, std::move(card_config)
                    )
                    };
                    luil::stack_config inner_config {};
                    inner_config.padding = luil::edge_insets::symmetric(16.0f, 12.0f);
                    inner_config.spacing = 2.0f;
                    auto inner { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"region-card-content" }, inner_config
                    )
                    };
                    inner->add(
                        make_label(u8"region-heading", u8"현재 캡쳐 영역", 11.0f, luil::label_color_role::dim), 20.0f
                    );
                    inner->add(
                        make_label(
                            u8"current-region",
                            selected_region ? value_text(selected_region->width) + u8" × "
                                    + value_text(selected_region->height) + u8" px"
                                            : std::u8string { u8"아직 선택한 영역이 없습니다" },
                            18.0f
                        ),
                        30.0f
                    );
                    inner->add(
                        make_label(
                            u8"region-position",
                            selected_region
                                ? u8"X " + value_text(selected_region->x) + u8" · Y " + value_text(selected_region->y)
                                : std::u8string { u8"위치와 크기를 입력해 새 프레임을 만들 수 있습니다" },
                            11.0f,
                            luil::label_color_role::dim
                        ),
                        20.0f
                    );
                    card->set_content(std::move(inner));
                    add(std::move(card), 98.0f);
                }
                gap(10.0f);
                {
                    auto edit { option_button(
                        u8"edit-region",
                        u8"위치·크기 편집",
                        runtime_command::show_region_editor,
                        !runtime_state_.busy && !runtime_state_.recording && !counting,
                        luil::text_button_visual::accent
                    )
                    };
                    help[u8"edit-region"]
                        = { .title = u8"캡쳐 영역 설정",
                            .body
                            = u8"X·Y 위치와 가로·세로 크기를 픽셀 단위로 입력합니다. 자주 쓰는 크기로 새 프레임을 "
                              u8"만들거나 현재 영역을 정밀하게 조절할 수 있습니다."
                        };
                    add(std::move(edit), 32.0f);
                }
                gap(22.0f);
                heading(u8"preset-heading", u8"저장한 프리셋");

                const bool has_presets { !settings.presets.empty() };
                const bool can_apply { has_presets && !runtime_state_.busy && !runtime_state_.recording && !counting };
                {
                    const auto preset {
                        has_presets ? value_text(runtime_state_.preset_index + 1) + u8" / "
                                + value_text(static_cast<int>(settings.presets.size())) + u8" · "
                                + settings.presets[static_cast<std::size_t>(runtime_state_.preset_index)].name
                                    : std::u8string { u8"저장된 영역 프리셋이 없습니다" }
                    };
                    luil::dropdown_config presets {};
                    presets.owner = u8"preset-picker";
                    presets.text = preset;
                    presets.placeholder = u8"프리셋 선택";
                    presets.open = preset_menu_open_;
                    presets.set_open
                        = [](const bool open) { return luil::make_app_action(preset_menu_intent { open }); };
                    auto dropdown { std::make_unique<luil::dropdown_element>(std::move(presets)) };
                    dropdown->set_access_name(u8"영역 프리셋 선택");
                    dropdown->set_enabled(can_apply && !dialog_open);
                    help[u8"preset-picker"]
                        = { .title = u8"영역 프리셋",
                            .body = std::u8string { option_button_help(runtime_command::next_preset) }
                        };
                    add(std::move(dropdown), 32.0f);
                }
                gap(8.0f);
                {
                    auto line { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"preset-name-row" }, row_config(10.0f)
                    )
                    };
                    luil::text_input_config name_config {};
                    name_config.placeholder = u8"프리셋 이름";
                    auto name { std::make_unique<luil::text_input_element>(
                        luil::ui_element_id { luil::application_element_kind(12), u8"preset-name-input" },
                        luil::make_text_input_view(
                            preset_name_, preset_composition_, static_cast<luil::text_input_target>(205)
                        ),
                        name_config
                    )
                    };
                    name->set_access_name(u8"프리셋 이름");
                    name->set_enabled(can_apply && editable);
                    line->add_flexible(std::move(name));
                    auto rename { std::make_unique<luil::text_button_element>(
                        luil::ui_element_id { command_kind, u8"rename-preset" },
                        luil::text_button_config { .text = u8"이름 변경" }
                    )
                    };
                    rename->set_access_name(u8"이름 변경");
                    rename->set_enabled(can_apply && editable && !preset_name_.text.empty() && !preset_composition_);
                    rename->set_action(
                        luil::ui_trigger::left_click, command_action(runtime_command::rename_preset, preset_name_.text)
                    );
                    line->add(std::move(rename), 96.0f);
                    add(std::move(line), 32.0f);
                }
                gap(8.0f);
                {
                    std::vector<std::unique_ptr<luil::ui_element>> buttons {};
                    buttons.push_back(option_button(
                        u8"apply-preset",
                        u8"프리셋 적용",
                        runtime_command::apply_preset,
                        can_apply,
                        luil::text_button_visual::accent
                    ));
                    buttons.push_back(option_button(
                        u8"save-preset",
                        u8"현재 영역 저장",
                        runtime_command::save_preset,
                        editable && selected_region.has_value()
                    ));
                    button_row(u8"preset-buttons", std::move(buttons));
                }
                gap(8.0f);
                {
                    auto line { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"preset-delete-row" }, row_config(10.0f)
                    )
                    };
                    line->add_fit(option_button(
                        u8"delete-preset",
                        u8"프리셋 삭제",
                        runtime_command::delete_preset,
                        can_apply && editable,
                        luil::text_button_visual::danger
                    ));
                    if (runtime_state_.can_undo_delete)
                    {
                        line->add_fit(option_button(
                            u8"undo-delete-preset", u8"삭제 취소", runtime_command::undo_delete_preset, editable
                        ));
                    }
                    line->add_flexible_gap();
                    add(std::move(line), 32.0f);
                }
            }
            // 보관 원본을 유지한 채 출력 형식·크기·압축으로 다시 저장할 수 있게 한다.
            else if (page_ == options_page::output)
            {
                const bool pending { runtime_state_.pending_recording && !runtime_state_.busy };
                {
                    luil::panel_config card_config {};
                    card_config.corner_radius = 8.0f;
                    auto card { std::make_unique<luil::panel_element>(
                        luil::ui_element_id { command_kind, u8"draft-card" }, std::move(card_config)
                    )
                    };
                    luil::stack_config inner_config {};
                    inner_config.padding = luil::edge_insets::symmetric(16.0f, 12.0f);
                    inner_config.spacing = 2.0f;
                    auto inner { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"draft-card-content" }, inner_config
                    )
                    };
                    const auto source { runtime_state_.recorded_region };
                    const auto duration {
                        recording_time(static_cast<std::uint64_t>(runtime_state_.recorded_duration))
                    };
                    inner->add(
                        make_label(
                            u8"draft-heading",
                            runtime_state_.pending_recording ? u8"보관 중인 녹화" : u8"보관 중인 녹화 없음",
                            11.0f,
                            luil::label_color_role::dim
                        ),
                        20.0f
                    );
                    inner->add(
                        make_label(
                            u8"draft-info",
                            runtime_state_.pending_recording
                                ? duration + u8" · " + value_text(source.width) + u8" × " + value_text(source.height)
                                    + u8" px"
                                : std::u8string { u8"녹화를 정지하면 원본을 여기에 보관합니다" },
                            runtime_state_.pending_recording ? 15.0f : 12.0f
                        ),
                        26.0f
                    );
                    inner->add(
                        make_label(u8"draft-saved", runtime_state_.saved_summary, 11.0f, luil::label_color_role::dim),
                        20.0f
                    );
                    card->set_content(std::move(inner));
                    add(std::move(card), 94.0f);
                }
                gap(10.0f);
                {
                    std::vector<std::unique_ptr<luil::ui_element>> buttons {};
                    buttons.push_back(option_button(
                        u8"save-recording",
                        u8"녹화 저장",
                        runtime_command::save_recording,
                        pending && !counting,
                        luil::text_button_visual::accent
                    ));
                    buttons.push_back(option_button(
                        u8"discard-recording",
                        u8"녹화 비우기",
                        runtime_command::discard_recording,
                        pending && !counting,
                        luil::text_button_visual::danger
                    ));
                    button_row(u8"draft-buttons", std::move(buttons));
                }
                gap(8.0f);
                {
                    std::vector<std::unique_ptr<luil::ui_element>> buttons {};
                    buttons.push_back(option_button(
                        u8"edit-recording", u8"보관 녹화 편집", runtime_command::edit_recording, pending && !counting
                    ));
                    buttons.push_back(option_button(
                        u8"open-animation",
                        u8"이미지를 열어 편집",
                        runtime_command::show_animation_editor,
                        !runtime_state_.busy && !runtime_state_.recording && !counting
                    ));
                    button_row(u8"editor-buttons", std::move(buttons));
                }
                gap(8.0f);
                check_box(
                    u8"auto-save",
                    u8"녹화 정지 시 바로 저장",
                    settings.auto_save,
                    runtime_command::toggle_auto_save,
                    editable,
                    u8"녹화를 정지하면 아래 설정으로 바로 파일을 만듭니다. 원본은 저장 후에도 남습니다."
                );
                gap(18.0f);
                heading(u8"format-heading", u8"형식");
                {
                    const auto format { settings.capture.animation_format };
                    auto [element, choice_height] { choice(
                        u8"animation-format",
                        u8"애니메이션 형식",
                        { { u8"webp", u8"WebP" }, { u8"apng", u8"APNG" }, { u8"gif", u8"GIF" } },
                        format == image_format::apng      ? u8"apng"
                            : format == image_format::gif ? u8"gif"
                                                          : u8"webp",
                        runtime_command::set_animation_format
                    )
                    };
                    field(u8"save-format-label", u8"애니메이션", std::move(element), choice_height);
                }
                gap(8.0f);
                const bool webp { settings.capture.animation_format == image_format::webp
                                  || settings.capture.still_format == image_format::webp
                };
                {
                    auto [element, choice_height] { choice(
                        u8"webp-mode",
                        u8"WebP 압축 방식",
                        { { u8"lossless", u8"무손실" }, { u8"lossy", u8"손실" } },
                        settings.capture.webp_lossless ? u8"lossless" : u8"lossy",
                        runtime_command::set_webp_mode,
                        webp
                    )
                    };
                    field(u8"webp-mode-label", u8"WebP 압축", std::move(element), choice_height);
                }
                gap(10.0f);
                {
                    auto line { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"quality-row" }, row_config(8.0f)
                    )
                    };
                    line->add_flexible(make_label(u8"quality-label", u8"손실 압축 품질"));
                    line->add_fit(make_label(u8"quality-value", value_text(settings.capture.webp_quality), 14.0f));
                    add(std::move(line), 24.0f);
                }
                {
                    luil::slider_config quality {};
                    quality.minimum = 1;
                    quality.maximum = 100;
                    quality.value = runtime_state_.quality_position;
                    quality.magnets = { 50, 75, 80, 90, 100 };
                    const auto quality_command
                        = [](const runtime_command action, const float value) -> luil::input_action {
                        const auto text { std::to_string(value) };
                        return luil::app_ui_command { static_cast<std::uint32_t>(action),
                                                      { text.begin(), text.end() }
                        };
                    };
                    quality.change = [quality_command](const float delta) {
                        return quality_command(runtime_command::adjust_webp_quality, delta);
                    };
                    quality.change_to = [quality_command](const float value) {
                        return quality_command(runtime_command::set_webp_quality, value);
                    };
                    auto slider { std::make_unique<luil::slider_element>(
                        luil::ui_element_id { command_kind, u8"webp-quality" }, std::move(quality)
                    )
                    };
                    slider->set_access_name(u8"WebP 손실 압축 품질 · 1~100");
                    slider->set_tooltip(u8"WebP 손실 압축 품질 · 1~100");
                    slider->set_enabled(editable && webp && !settings.capture.webp_lossless);
                    help[u8"webp-quality"]
                        = { .title = u8"WebP 손실 압축 품질",
                            .body
                            = u8"낮을수록 파일이 작아지고 화질이 떨어집니다. 저장 후 품질이나 출력 크기를 바꿔 다시 "
                              u8"저장할 수 있습니다. 보관 원본에는 영향을 주지 않습니다."
                        };
                    add(std::move(slider), 24.0f);
                }
                gap(18.0f);
                heading(u8"size-heading", u8"크기·압축");
                {
                    const auto source_region { runtime_state_.pending_recording
                                                   ? std::optional<capture_region> { runtime_state_.recorded_region }
                                                   : selected_region
                    };
                    const int output_percent { settings.capture.output_percent };
                    auto line { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"scale-row" }, row_config(8.0f)
                    )
                    };
                    line->add_flexible(make_label(u8"export-scale-label", u8"출력 크기 · 사진과 애니메이션"));
                    line->add_fit(make_label(
                        u8"export-output-dimensions",
                        value_text(output_percent) + u8"% · "
                            + (source_region ? value_text(source_region->width * output_percent / 100) + u8" × "
                                       + value_text(source_region->height * output_percent / 100) + u8" px"
                                             : std::u8string { u8"영역을 선택하세요" })
                    ));
                    add(std::move(line), 24.0f);
                }
                {
                    luil::slider_config scale_config {};
                    scale_config.maximum = 5;
                    scale_config.value = runtime_state_.output_step;
                    scale_config.magnets = { 0, 1, 2, 3, 4, 5 };
                    const auto scale_action
                        = [](const runtime_command command, const float value) -> luil::input_action {
                        const auto text { std::to_string(value) };
                        return luil::app_ui_command { static_cast<std::uint32_t>(command),
                                                      { text.begin(), text.end() }
                        };
                    };
                    scale_config.change = [scale_action](const float delta) {
                        return scale_action(runtime_command::adjust_scale, delta);
                    };
                    scale_config.change_to = [scale_action](const float value) {
                        return scale_action(runtime_command::set_scale_index, value);
                    };
                    auto scale_slider { std::make_unique<output_slider_element>(scale_config) };
                    scale_slider->set_access_name(u8"출력 배율 · 25, 50, 75, 100, 150, 200% 단계");
                    scale_slider->set_enabled(editable);
                    help[u8"scale"]
                        = { .title = u8"출력 배율",
                            .body
                            = u8"25·50·75·100·150·200% 단계로 조절합니다. 캡쳐 영역은 유지하며 저장될 이미지의 크기만 "
                              u8"변경합니다. 방향키로 한 단계씩 이동합니다."
                        };
                    add(std::move(scale_slider), 24.0f);
                }
                add_note(u8"export-scale-note", u8"25 · 50 · 75 · 100 · 150 · 200%", 1);
                gap(12.0f);
                const auto compression { settings.capture.compression };
                {
                    auto [element, choice_height] { choice(
                        u8"compression",
                        u8"WebP 저장 방식",
                        { { u8"fast", u8"빠른 저장" }, { u8"small", u8"균형" }, { u8"maximum", u8"최대 압축" } },
                        compression == lossless_compression::fast          ? u8"fast"
                            : compression == lossless_compression::maximum ? u8"maximum"
                                                                           : u8"small",
                        runtime_command::set_compression,
                        webp
                    )
                    };
                    field(u8"compression-label", u8"압축 속도", std::move(element), choice_height);
                }
                gap(4.0f);
                add_note(
                    u8"compression-note",
                    !webp                                           ? u8"WebP 선택 시 사용할 수 있습니다."
                        : compression == lossless_compression::fast ? u8"후처리를 생략해 빠르게 저장합니다."
                        : compression == lossless_compression::maximum
                        ? u8"최대 압축은 저장에 오래 걸릴 수 있습니다."
                        : u8"권장 · 저장 속도와 파일 크기의 균형을 맞춥니다."
                );
            }
            // 테마·명령창·프레임·단축키처럼 앱 전체에 걸리는 설정이다.
            else
            {
                const auto& appearance { settings.appearance };
                heading(u8"theme-heading", u8"테마");
                {
                    luil::theme_picker_config picker {};
                    picker.owner = u8"theme";
                    picker.selected = appearance.theme;
                    picker.accent_id = appearance.accent_id;
                    // 앱 설정은 늘 흘리므로 스크롤 막대 폭을 빼야 카드 셋이 한 줄에 선다.
                    const float inner_width { content_width - luil::scroll_area_bar_width };
                    picker.card_width = (inner_width - 24) / 3;
                    picker.preview_height = 48;
                    picker.select = [](const luil::theme_preference theme) -> luil::input_action {
                        return luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::set_theme),
                                                      std::u8string { luil::theme_preference_name(theme) }
                        };
                    };
                    const float picker_height { luil::theme_picker_element::height_for(picker, inner_width) };
                    auto themes { std::make_unique<luil::theme_picker_element>(picker) };
                    themes->set_enabled(editable);
                    add(std::move(themes), picker_height);
                }
                gap(12.0f);
                {
                    auto line { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"accent-row" }, row_config(12.0f)
                    )
                    };
                    line->add(make_label(u8"accent-label", u8"키 컬러"), 108.0f);
                    line->add_flexible(
                        make_label(u8"accent-name", std::u8string { luil::accent_for(appearance.accent_id).label })
                    );
                    add(std::move(line), 24.0f);
                }
                gap(6.0f);
                {
                    luil::accent_picker_config color_config {};
                    color_config.owner = u8"accent-picker";
                    color_config.selected = appearance.accent_id;
                    color_config.name = u8"키 컬러";
                    color_config.select = [](const std::u8string& accent) -> luil::input_action {
                        return luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::set_accent), accent };
                    };
                    const float colors_height { luil::accent_picker_element::height_for(
                        color_config, content_width - luil::scroll_area_bar_width
                    ) };
                    auto colors { std::make_unique<luil::accent_picker_element>(color_config) };
                    colors->set_enabled(editable);
                    for (const auto& element : colors->children())
                    {
                        help[element->id().owner]
                            = { .title = element->accessibility().name,
                                .body
                                = u8"버튼 강조색과 캡쳐 프레임·글로우에 함께 적용합니다. 재실행해도 유지됩니다."
                            };
                    }
                    add(std::move(colors), colors_height);
                }
                gap(4.0f);
                add_note(u8"appearance-note", u8"테마와 키 컬러는 바로 반영하고 저장합니다");
                gap(18.0f);
                heading(u8"window-heading", u8"명령창·프레임");
                check_box(
                    u8"always-on-top",
                    u8"명령창 항상 위",
                    settings.always_on_top,
                    runtime_command::toggle_always_on_top,
                    runtime_state_.settings_writable,
                    u8"명령창을 다른 창보다 위에 표시합니다. 설정은 재실행해도 유지됩니다."
                );
                gap(6.0f);
                check_box(
                    u8"close-to-tray",
                    u8"닫기 버튼을 누르면 알림 영역으로 숨기기",
                    settings.close_to_tray,
                    runtime_command::toggle_close_to_tray,
                    editable,
                    u8"닫아도 앱이 알림 영역에 남아 단축키로 계속 촬영할 수 있습니다. 알림 영역 아이콘으로 다시 열거나 "
                    u8"종료합니다."
                );
                gap(12.0f);
                {
                    auto line { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"glow-row" }, row_config(8.0f)
                    )
                    };
                    line->add_flexible(make_label(u8"glow-label", u8"프레임 글로우 두께"));
                    line->add_fit(make_label(u8"glow-value", value_text(settings.glow_width) + u8" px"));
                    add(std::move(line), 24.0f);
                }
                {
                    luil::slider_config glow {};
                    glow.minimum = 0;
                    glow.maximum = 40;
                    glow.magnets = { 0, 12, 24, 40 };
                    glow.value = runtime_state_.glow_position;
                    glow.change_to = [](const float value) -> luil::input_action {
                        const auto number { std::to_string(static_cast<int>(std::clamp(value, 0.0f, 40.0f) + 0.5f)) };
                        return luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::set_glow_width),
                                                      { number.begin(), number.end() }
                        };
                    };
                    glow.change = [](const float delta) -> luil::input_action {
                        const auto number { std::to_string(delta) };
                        return luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::adjust_glow_width),
                                                      { number.begin(), number.end() }
                        };
                    };
                    auto slider { std::make_unique<luil::slider_element>(
                        luil::ui_element_id { command_kind, u8"glow-width" }, std::move(glow)
                    )
                    };
                    slider->set_access_name(u8"프레임 글로우 두께 · 0~40px");
                    slider->set_tooltip(u8"프레임 글로우 두께 · 0~40px");
                    slider->set_enabled(editable);
                    help[u8"glow-width"]
                        = { .title = u8"프레임 글로우 두께",
                            .body
                            = u8"프레임 바깥으로 번지는 빛의 범위를 조절합니다. 0은 끄기입니다. 캡쳐 영역 안은 그대로 "
                              u8"유지됩니다."
                        };
                    add(std::move(slider), 24.0f);
                }
                gap(18.0f);
                heading(u8"shortcuts-heading", u8"단축키");
                add(shortcuts_.make_inputs(editable), shortcut_editor::inputs_height());
                gap(10.0f);
                {
                    const auto bindings { shortcuts_.bindings() };
                    auto line { std::make_unique<luil::stack_element>(
                        luil::ui_element_id { command_kind, u8"shortcut-buttons" }, row_config(10.0f)
                    )
                    };
                    auto reset { std::make_unique<luil::text_button_element>(
                        luil::ui_element_id { command_kind, u8"reset-shortcuts" },
                        luil::text_button_config { .text = u8"기본값" }
                    )
                    };
                    reset->set_access_name(u8"기본값");
                    reset->set_enabled(editable);
                    reset->set_action(
                        luil::ui_trigger::left_click, luil::make_message_action(reset_shortcuts_intent {})
                    );
                    line->add_fit(std::move(reset));
                    auto revert { std::make_unique<luil::text_button_element>(
                        luil::ui_element_id { command_kind, u8"revert-shortcuts" },
                        luil::text_button_config { .text = u8"변경 취소" }
                    )
                    };
                    revert->set_access_name(u8"변경 취소");
                    revert->set_enabled(editable && shortcuts_edited_);
                    revert->set_action(
                        luil::ui_trigger::left_click, luil::make_message_action(revert_shortcuts_intent {})
                    );
                    line->add_fit(std::move(revert));
                    line->add_flexible_gap();
                    auto apply { std::make_unique<luil::text_button_element>(
                        luil::ui_element_id { command_kind, u8"apply-shortcuts" },
                        luil::text_button_config { .text = u8"단축키 적용", .visual = luil::text_button_visual::accent }
                    )
                    };
                    apply->set_access_name(u8"단축키 적용");
                    apply->set_enabled(editable && bindings.has_value());
                    apply->set_action(
                        luil::ui_trigger::left_click,
                        [bindings](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                            if (!bindings)
                            {
                                return {};
                            }

                            return { luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::set_shortcuts),
                                                            encode_hotkeys(*bindings)
                            }
                            };
                        }
                    );
                    line->add_fit(std::move(apply));
                    add(std::move(line), 32.0f);
                }
                gap(8.0f);
                const auto error { shortcuts_.error() };
                add_note(
                    u8"shortcut-error",
                    error.empty() ? (shortcuts_edited_ ? std::u8string {} : runtime_state_.shortcut_error) : error
                );
                add_note(
                    u8"shortcut-help",
                    shortcuts_edited_ ? u8"적용 전 변경이 있습니다. 탭을 이동해도 입력은 유지됩니다."
                                      : u8"Ctrl+Alt+F9처럼 입력 후 적용하세요. 빈 값은 사용하지 않습니다."
                );
            }

            gap(18.0f);
            {
                // 상태 줄은 비어 있어도 자리를 지켜 저장 결과가 생길 때 아래 내용이 밀리지 않게 한다.
                auto [element, note_height] { make_note(u8"runtime-status", runtime_state_.status, content_width) };
                add(std::move(element), std::max(note_height, 18.0f));
            }
            help[u8"runtime-status"].title = u8"현재 상태 · 저장 결과";

            const float area_top { options_top + 74.0f };
            luil::scroll_area_config scrolling {};
            scrolling.owner = u8"options-scroll";
            scrolling.content_height = page_height;
            options_scroll_ = luil::clamp_scroll(scrolling.content_height, height / scale - area_top, options_scroll_);
            scrolling.scroll_offset = options_scroll_;
            scrolling.scroll = [](const float delta) -> luil::input_action {
                return luil::make_app_action(options_scroll_intent { delta, false });
            };
            scrolling.scroll_to = [](const float offset) -> luil::input_action {
                return luil::make_app_action(options_scroll_intent { offset, true });
            };

            auto area { std::make_unique<luil::scroll_area_element>(std::move(scrolling)) };
            area->set_content(std::move(page));
            area->arrange({ { 0, area_top * scale, width, std::max(0.0f, height - area_top * scale) }, scale });
            root->add(std::move(area));
        }

        // 설정창 어느 위치에 놓아도 편집으로 연결하고, 끌기가 들어오면 같은 안내 영역을 강조한다.
        auto image_drop { std::make_unique<image_drop_element>(u8"settings-image-drop") };
        image_drop->set_enabled(
            !runtime_state_.busy && !runtime_state_.recording && !region_state_.selecting
            && runtime_state_.dialog.kind == message_dialog_kind::none && animation_editor_.can_open_image()
        );
        image_drop->arrange({ { 0, caption_height, width, height - caption_height }, scale });
        root->add(std::move(image_drop));

        auto frame { std::make_shared<luil::ui_frame>() };
        root->set_enabled(!animation_editor_.confirmation_open());
        frame->close = [dirty = animation_editor_.has_unsaved_changes(),
                        active = runtime_state_.busy || runtime_state_.recording,
                        unsaved = runtime_state_.pending_recording && !runtime_state_.recording_saved,
                        tray = runtime_state_.settings.close_to_tray && !runtime_state_.quit_requested,
                        exiting = runtime_state_.exiting]() -> luil::input_action {
            if (exiting)
            {
                return luil::ui_command::application_exit;
            }
            // 알림 영역으로 숨기면 녹화·편집 상태를 그대로 둔다.
            if (tray)
            {
                return luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::hide_to_tray) };
            }
            if (active)
            {
                return luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::request_close) };
            }
            if (dirty)
            {
                return luil::make_app_action(animation_editor_intent { animation_editor_action::request_exit });
            }
            if (unsaved)
            {
                return luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::request_close) };
            }
            return luil::ui_command::application_exit;
        };
        frame->main_hidden = runtime_state_.hidden;
        frame->tree = std::make_shared<const luil::ui_tree>(std::move(root));

        if (animation_editor_.opened() && !runtime_state_.busy && !runtime_state_.recording && !region_state_.selecting)
        {
            frame->windows.push_back(animation_editor_.make_window());
        }

        frame->appearance = runtime_state_.settings.appearance;
        frame->style = command_style(runtime_state_.recording);

        if (fps_menu_open_)
        {
            luil::menu_config menu {};
            menu.owner = u8"fps-menu";
            menu.items = { { u8"10", u8"10 fps · 가볍게" },
                           { u8"16", u8"16 fps · 균형" },
                           { u8"24", u8"24 fps · 자연스럽게" },
                           { u8"30", u8"30 fps · 부드럽게" },
                           { u8"33", u8"33 fps · 고속" }
            };
            menu.select = [](const std::u8string& key) -> luil::input_action {
                return luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::set_fps), key };
            };

            const float menu_height { luil::menu_element::height_for(menu) };
            auto element { std::make_unique<luil::menu_element>(std::move(menu)) };
            element->set_focus_trap(true);
            element->set_focus_return({ luil::ui_element_kind::dropdown, u8"fps" });
            element->arrange({ { 0, 0, 185.0f * scale, menu_height * scale }, scale });
            luil::ui_popup popup {};
            popup.id = u8"fps-menu";

            const auto* anchor { frame->tree->find({ luil::ui_element_kind::dropdown, u8"fps" }) };
            const auto box { anchor->bounds() };
            popup.x = box.x / scale;
            popup.y = (box.y + box.height) / scale + 3;

            if (popup.y + menu_height > height / scale)
            {
                popup.y = std::max(0.0f, box.y / scale - menu_height - 3);
            }

            popup.width = 185;
            popup.height = menu_height;
            popup.tree = std::make_shared<const luil::ui_tree>(std::move(element));
            popup.dismiss = [](luil::popup_dismiss_reason) -> luil::input_action {
                return luil::make_app_action(fps_menu_intent { false });
            };
            frame->popups.push_back(std::move(popup));
        }

        if (preset_menu_open_ && page_ == options_page::region && runtime_state_.options_open)
        {
            luil::list_config list {};
            list.owner = u8"preset-list";
            list.row_height = 32;
            const auto selected_key { std::to_string(runtime_state_.preset_index) };
            list.selected = { selected_key.begin(), selected_key.end() };
            for (std::size_t index {}; index < runtime_state_.settings.presets.size(); ++index)
            {
                const auto key { std::to_string(index) };
                list.items.push_back(
                    { std::u8string { key.begin(), key.end() }, runtime_state_.settings.presets[index].name }
                );
            }
            list.select = [](const std::u8string& key) -> luil::input_action {
                return luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::select_preset), key };
            };
            list.scroll_offset = preset_scroll_;
            list.scroll
                = [](const float delta) { return luil::make_app_action(preset_scroll_intent { delta, false }); };
            list.scroll_to
                = [](const float offset) { return luil::make_app_action(preset_scroll_intent { offset, true }); };
            const float popup_height { std::min(240.0f, static_cast<float>(list.items.size()) * 32) };
            auto element { std::make_unique<luil::list_element>(std::move(list)) };
            element->set_access_name(u8"저장한 영역 프리셋");
            element->arrange({ { 0, 0, (width / scale - 40) * scale, popup_height * scale }, scale });
            const auto* anchor { frame->tree->find({ luil::ui_element_kind::dropdown, u8"preset-picker" }) };
            if (anchor)
            {
                luil::ui_popup popup {};
                popup.id = u8"preset-menu";
                popup.x = anchor->bounds().x / scale;
                popup.y = (anchor->bounds().y + anchor->bounds().height) / scale;
                popup.width = width / scale - 40;
                popup.height = popup_height;
                popup.tree = std::make_shared<const luil::ui_tree>(std::move(element));
                popup.dismiss
                    = [](luil::popup_dismiss_reason) { return luil::make_app_action(preset_menu_intent { false }); };
                frame->popups.push_back(std::move(popup));
            }
        }

        if (editor_.opened() && !runtime_state_.busy && !runtime_state_.recording && !region_state_.selecting)
        {
            frame->popups.push_back(editor_.make_popup(scale));
        }

        if (!fps_menu_open_ && !editor_.opened() && !help_dismissed_ && !runtime_state_.busy
            && !runtime_state_.recording && !region_state_.selecting && help_target_.maximum_width >= 160.0f
            && std::max(help_target_.available_above, help_target_.available_below) >= 140.0f)
        {
            const auto content { help.find(help_target_.target.owner) };
            const auto* const element { frame->tree->find(help_target_.target) };

            if (content != help.end() && element != nullptr)
            {
                frame->popups.push_back(make_help_popup(content->second, element->bounds(), help_target_, scale));
            }
        }

        if (runtime_state_.dialog.kind != message_dialog_kind::none)
        {
            frame->popups.clear();
            frame->popups.push_back(make_message_dialog(runtime_state_.dialog, scale));
        }
        else if (const auto confirmation { animation_editor_.confirmation(scale) })
        {
            frame->popups.clear();
            frame->popups.push_back(*confirmation);
        }

        return frame;
    }

    std::optional<std::chrono::steady_clock::time_point> command_driver::next_tick()
    {
        return animation_editor_.next_tick();
    }

    void command_driver::tick(const std::chrono::steady_clock::time_point now)
    {
        animation_editor_.tick(now);
    }

    void command_driver::cancel() noexcept
    {
        animation_editor_.cancel();
    }

    luil::app_message command_driver::make_close_message()
    {
        return luil::app_message { close_intent {} };
    }

    bool command_driver::shutdown_completed() const
    {
        return closed_.load();
    }

} // namespace chalkag
