#include "app/command_window.h"
#include "app/accent_picker.h"
#include "app/help_text.h"
#include "app/image_drop.h"
#include "capture/recording_time.h"

#include "luil/generated/codicons.h"
#include "luil/ui/button_element.h"
#include "luil/ui/check_element.h"
#include "luil/ui/choice_group_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/dropdown_element.h"
#include "luil/ui/label_element.h"
#include "luil/ui/menu_element.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/progress_element.h"
#include "luil/ui/root_element.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/slider_element.h"
#include "luil/ui/tab_bar_element.h"
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

        class options_content_element final : public luil::ui_element
        {
        public:
            explicit options_content_element(const float scale)
                : ui_element { { command_kind, u8"options-content" } }
                , scale_ { scale }
            {}

            void add(std::unique_ptr<luil::ui_element> element)
            {
                const auto box { element->bounds() };
                slots_.push_back(
                    { box.x / scale_, box.y / scale_ - command_window_height, box.width / scale_, box.height / scale_ }
                );
                add_child(std::move(element));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);

                const auto elements { children() };

                for (std::size_t index {}; index < elements.size(); ++index)
                {
                    const auto slot { slots_[index] };
                    elements[index]->arrange(context.for_child(
                        { context.slot.x + slot.x * context.scale,
                          context.slot.y + slot.y * context.scale,
                          slot.width * context.scale,
                          slot.height * context.scale
                        }
                    ));
                }
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                draw_children(context, interaction);
            }

        private:
            float scale_ { 1.0f };

            std::vector<luil::rect_f> slots_ {};
        };

        [[nodiscard]] std::shared_ptr<const luil::ui_style> command_style(const bool recording)
        {
            static const auto style = [] {
                auto value { luil::default_ui_style() };
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

        struct command_description
        {
            runtime_command command;

            const char8_t* owner;
            const char8_t* name;
            char32_t glyph;
        };

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
            options_scroll_ = 0;
            shortcuts_.reset(runtime_state_.settings.shortcuts);
            shortcuts_edited_ = false;
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

            if (applied || !state->options_open)
            {
                shortcuts_edited_ = false;
            }

            if (runtime_state_.settings.shortcuts != state->settings.shortcuts || !state->options_open
                || (applied && state->shortcut_error.empty()))
            {
                shortcuts_.reset(state->settings.shortcuts);
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

        const std::array commands {
            command_description { runtime_command::select_region,
                                  u8"select-region",
                                  u8"캡쳐 영역 선택",
                                  luil::codicons::icon_screen_full
            },
            command_description { runtime_command::take_screenshot,
                                  u8"take-screenshot",
                                  u8"스크린샷 찍기",
                                  luil::codicons::icon_device_camera
            },
            command_description { runtime_command::toggle_recording,
                                  u8"toggle-recording",
                                  u8"애니메이션 녹화 / 정지",
                                  luil::codicons::icon_record
            },
            command_description { runtime_command::toggle_options,
                                  u8"open-options",
                                  u8"옵션 / 영역 프리셋",
                                  luil::codicons::icon_settings_gear
            },
            command_description {
                runtime_command::toggle_always_on_top, u8"toggle-topmost", u8"항상 위", luil::codicons::icon_pin
            },
        };
        float left { toolbar_padding * scale };

        for (const command_description& command : commands)
        {
            if (runtime_state_.busy && command.command != runtime_command::toggle_always_on_top)
            {
                continue;
            }

            luil::button_config button_config {};

            button_config.glyph = command.glyph;

            if (command.command == runtime_command::toggle_recording && runtime_state_.recording)
            {
                button_config.glyph = luil::codicons::icon_debug_stop;
            }

            if (command.command == runtime_command::toggle_always_on_top && runtime_state_.settings.always_on_top)
            {
                button_config.glyph = luil::codicons::icon_pinned;
            }

            button_config.icon_size = 16.0f;
            button_config.corner_radius = 6.0f;
            button_config.active = (command.command == runtime_command::select_region
                                    && (region_state_.selecting || region_state_.region.has_value()))
                || (command.command == runtime_command::toggle_recording && runtime_state_.recording)
                || (command.command == runtime_command::toggle_options && runtime_state_.options_open)
                || (command.command == runtime_command::toggle_always_on_top && runtime_state_.settings.always_on_top);

            std::unique_ptr<luil::ui_element> button {};

            if (command.command == runtime_command::toggle_recording)
            {
                luil::text_button_config record_config {};

                record_config.text = runtime_state_.recording ? u8"정지"
                    : runtime_state_.pending_recording        ? u8"저장"
                                                              : u8"녹화";

                record_config.glyph
                    = runtime_state_.pending_recording ? luil::codicons::icon_save : button_config.glyph;

                record_config.visual
                    = runtime_state_.recording ? luil::text_button_visual::danger : luil::text_button_visual::accent;
                button = std::make_unique<luil::text_button_element>(
                    luil::ui_element_id { command_kind, command.owner }, std::move(record_config)
                );
            }
            else
            {
                button = std::make_unique<luil::button_element>(
                    luil::ui_element_id { command_kind, command.owner }, button_config
                );
            }

            button->set_access_name(command.name);

            if (command.command == runtime_command::toggle_always_on_top)
            {
                button->set_access_name(
                    runtime_state_.settings.always_on_top ? u8"항상 위 · 고정됨" : u8"항상 위 · 고정 해제됨"
                );
            }

            if (command.command == runtime_command::toggle_recording)
            {
                button->set_access_name(
                    runtime_state_.recording               ? u8"녹화 정지"
                        : runtime_state_.pending_recording ? u8"보관 녹화 저장"
                                                           : u8"애니메이션 녹화 시작"
                );
            }

            const auto action
                = [id = command.command == runtime_command::toggle_recording && runtime_state_.pending_recording
                       ? runtime_command::save_recording
                       : command.command](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                return { luil::app_ui_command { .command = static_cast<std::uint32_t>(id) } };
            };
            button->set_action(luil::ui_trigger::left_click, action);
            button->set_cursor(luil::ui_cursor::hand);

            if (command.command == runtime_command::select_region)
            {
                button->set_enabled(!runtime_state_.busy && !runtime_state_.recording);
            }
            else if (command.command == runtime_command::toggle_always_on_top)
            {
                button->set_enabled(runtime_state_.settings_writable);
            }
            else
            {
                const bool idle { !runtime_state_.busy && !region_state_.selecting
                                  && region_state_.region.has_value()
                };

                const bool can_shoot { command.command != runtime_command::take_screenshot
                                       || (!runtime_state_.recording && !runtime_state_.pending_recording)
                };

                const bool can_record { true };
                const bool available { command.command == runtime_command::toggle_options
                                       || (idle && can_shoot && can_record)
                };
                button->set_enabled(available);
            }

            if (runtime_state_.dialog.kind != message_dialog_kind::none)
            {
                button->set_enabled(false);
            }

            help_content content { .title = button->accessibility().name };

            if (command.command == runtime_command::select_region)
            {
                content.shortcut = format_hotkey(runtime_state_.settings.shortcuts[0]);
                content.body = u8"화면을 드래그하여 캡쳐 영역을 선택합니다.\n모서리로 크기를 조절하고 상단 핸들로 "
                               u8"이동합니다. Esc로 선택을 취소합니다.";

                if (!region_state_.status.empty())
                {
                    content.body += u8"\n\n" + region_state_.status;
                }
            }
            else if (command.command == runtime_command::take_screenshot)
            {
                content.shortcut = format_hotkey(runtime_state_.settings.shortcuts[1]);
                content.body = u8"선택한 영역을 "
                    + std::u8string { runtime_state_.settings.capture.still_format == image_format::png ? u8"PNG"
                                                                                                        : u8"WebP"
                    }
                    + u8" 이미지로 저장합니다.\n저장 위치: 실행 파일 옆 data/captures";
            }
            else if (command.command == runtime_command::toggle_recording)
            {
                content.shortcut = format_hotkey(runtime_state_.settings.shortcuts[2]);

                if (!content.shortcut.empty())
                {
                    content.shortcut += u8" · 다시 누르면 정지";
                }

                content.body = runtime_state_.pending_recording
                    ? u8"보관한 녹화를 현재 옵션으로 저장합니다.\n저장 탭에서 품질을 조절하고 여러 번 저장할 수 "
                      u8"있습니다. 새 촬영을 시작하려면 녹화 비우기를 누르세요."
                    : u8"선택한 영역을 애니메이션 이미지로 녹화합니다.\n정지 후 원본을 보관합니다. 저장 버튼을 누르면 "
                      u8"현재 옵션으로 저장합니다.";
            }
            else if (command.command == runtime_command::toggle_always_on_top)
            {
                content.body = u8"명령창을 다른 창보다 위에 표시합니다. 다시 누르면 해제합니다.\n재실행해도 설정이 "
                               u8"유지됩니다. 캡쳐 프레임은 항상 위에 표시됩니다.";
            }
            else
            {
                content.body = u8"이미지 형식, FPS, 출력 크기, 커서를 설정하고 영역 프리셋을 관리합니다.\n설정은 변경 "
                               u8"즉시 실행 파일 옆 data 폴더에 저장됩니다.";
            }

            if (!button->enabled())
            {
                button->set_cursor(luil::ui_cursor::arrow);

                const auto reason
                    = command.command == runtime_command::toggle_always_on_top && !runtime_state_.settings_writable
                    ? u8"기존 설정 파일을 읽을 수 없어 변경을 중단했습니다. data/settings.json을 확인하세요."
                    : runtime_state_.busy               ? u8"저장이 끝나면 사용할 수 있습니다."
                    : runtime_state_.recording          ? u8"녹화를 정지한 뒤 사용할 수 있습니다."
                    : region_state_.selecting           ? u8"영역 선택을 완료하거나 Esc로 취소하세요."
                    : !region_state_.region.has_value() ? u8"먼저 캡쳐 영역을 선택하세요."
                                                        : u8"현재 상태에서는 이 명령을 사용할 수 없습니다.";
                content.body = std::u8string { reason } + u8"\n\n" + content.body;
            }

            help.emplace(command.owner, std::move(content));

            if (command.command == runtime_command::toggle_always_on_top)
            {
                button->arrange({ { width - 60.0f * scale, 0, 28.0f * scale, caption_height }, scale });
            }
            else if (command.command == runtime_command::toggle_options)
            {
                button->arrange(
                    { { width - 44.0f * scale,
                        caption_height + toolbar_padding * scale,
                        36.0f * scale,
                        toolbar_button_height * scale
                    },
                      scale
                    }
                );
            }
            else
            {
                button->arrange(
                    { { left,
                        caption_height + toolbar_padding * scale,
                        (command.command == runtime_command::toggle_recording ? 78.0f : 36.0f) * scale,
                        toolbar_button_height * scale
                    },
                      scale
                    }
                );
            }

            root->add(std::move(button));
            left += (36.0f + toolbar_spacing) * scale;
        }

        if (runtime_state_.busy)
        {
            const auto text { save_progress_text(runtime_state_.progress) };
            auto label = std::make_unique<luil::label_element>(
                luil::ui_element_id { command_kind, u8"save-progress-label" },
                luil::label_config { .text = text, .font_size = 12, .color = luil::label_color_role::primary }
            );
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
                auto detail = std::make_unique<luil::label_element>(
                    luil::ui_element_id { command_kind, u8"save-progress-detail" },
                    luil::label_config { .text = u8"완료되면 명령 버튼으로 돌아갑니다", .font_size = 10 }
                );
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

        if (runtime_state_.options_open)
        {
            auto content_root { std::make_unique<options_content_element>(scale) };
            const bool editable { !runtime_state_.busy && !runtime_state_.recording && runtime_state_.settings_writable
                                  && runtime_state_.dialog.kind == message_dialog_kind::none
            };

            const float content_width { width / scale - 40.0f };
            const auto value_text = [](const int value) -> std::u8string {
                const std::string text { std::to_string(value) };
                return { text.begin(), text.end() };
            };

            const auto label = [&](const char8_t* owner,
                                   const std::u8string& text,
                                   const float x,
                                   const float y,
                                   const float length,
                                   const float size = 12.0f,
                                   const bool wrap = false) {
                help.emplace(owner, help_content { .title = u8"캡쳐 안내", .body = text });

                if (wrap)
                {
                    auto element = std::make_unique<wrapped_help_element>(
                        luil::ui_element_id { command_kind, owner },
                        layout_help_text(text, length, size, 2),
                        size,
                        luil::label_color_role::dim,
                        text
                    );
                    element->set_cursor(luil::ui_cursor::arrow);
                    element->arrange({ { x * scale, y * scale, length * scale, 38.0f * scale }, scale });
                    content_root->add(std::move(element));
                    return;
                }

                auto element = std::make_unique<luil::label_element>(
                    luil::ui_element_id { command_kind, owner },
                    luil::label_config { .text = text, .font_size = size, .color = luil::label_color_role::primary }
                );
                element->set_cursor(luil::ui_cursor::arrow);
                element->arrange({ { x * scale, y * scale, length * scale, 24.0f * scale }, scale });
                content_root->add(std::move(element));
            };

            const auto card = [&](const char8_t* owner, const float y, const float card_height) {
                luil::panel_config config {};
                config.corner_radius = 8.0f;

                auto element { std::make_unique<luil::panel_element>(
                    luil::ui_element_id { command_kind, owner }, std::move(config)
                )
                };
                element->arrange({ { 20.0f * scale, y * scale, content_width * scale, card_height * scale }, scale });
                content_root->add(std::move(element));
            };

            const auto choice = [&](const char8_t* owner,
                                    const char8_t* name,
                                    std::vector<luil::choice_item> items,
                                    const std::u8string& selected,
                                    const runtime_command action,
                                    const float y,
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
                                   u8"손실·무손실은 저장 탭에서 선택합니다.";
                        }

                        if (item.value == u8"animation-format/gif")
                        {
                            return u8"추가 설치 없이 GIF를 저장합니다. 색상은 최대 256개이며 프레임 시간은 10ms "
                                   u8"단위입니다.";
                        }

                        if (action == runtime_command::set_animation_format)
                        {
                            return u8"APNG는 무손실입니다. WebP는 저장 탭에서 손실·무손실 압축과 품질을 선택할 수 "
                                   u8"있습니다.";
                        }

                        if (action == runtime_command::set_still_format)
                        {
                            return u8"PNG는 무손실입니다. WebP는 저장 탭에서 손실·무손실 압축을 선택할 수 있습니다.";
                        }

                        if (action == runtime_command::set_theme)
                        {
                            return u8"창과 팝업에 즉시 반영합니다. 시스템은 Windows 앱 테마를 따릅니다. 재실행해도 "
                                   u8"선택이 유지됩니다.";
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

                auto element { std::make_unique<luil::choice_group_element>(std::move(config)) };
                element->set_enabled(editable && available);
                element->arrange(
                    { { 140.0f * scale, y * scale, (content_width - 120.0f) * scale, 30.0f * scale }, scale }
                );
                content_root->add(std::move(element));
            };

            const auto text_button = [&](const char8_t* owner,
                                         const char8_t* text,
                                         const runtime_command action,
                                         const float x,
                                         const float y,
                                         const float length,
                                         const bool enabled,
                                         const luil::text_button_visual visual = luil::text_button_visual::normal) {
                auto element { std::make_unique<luil::text_button_element>(
                    luil::ui_element_id { command_kind, owner },
                    luil::text_button_config { .text = text, .visual = visual }
                )
                };
                element->set_access_name(text);
                element->set_action(
                    luil::ui_trigger::left_click,
                    [action](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                        return { luil::app_ui_command { .command = static_cast<std::uint32_t>(action) } };
                    }
                );
                element->set_enabled(enabled && runtime_state_.dialog.kind == message_dialog_kind::none);
                help_content content {};
                content.title = text;
                content.body = option_button_help(action);
                help.emplace(owner, std::move(content));
                element->arrange({ { x * scale, y * scale, length * scale, 32.0f * scale }, scale });
                content_root->add(std::move(element));
            };
            label(
                u8"options-title",
                page_ == options_page::preferences ? u8"앱 설정" : u8"캡쳐 설정",
                20,
                94,
                content_width,
                16
            );
            label(
                u8"options-description",
                page_ == options_page::preferences ? u8"단축키는 적용 버튼으로 저장합니다"
                                                   : u8"설정은 자동 저장 · 녹화는 저장 버튼으로",
                20,
                122,
                content_width,
                11
            );
            luil::tab_bar_config tabs {};
            tabs.owner = u8"options-tabs";
            tabs.items = { { u8"capture", u8"캡쳐" },
                           { u8"region", u8"영역" },
                           { u8"output", u8"저장" },
                           { u8"preferences", u8"앱 설정" }
            };
            tabs.selected = page_ == options_page::region ? u8"region"
                : page_ == options_page::output           ? u8"output"
                : page_ == options_page::preferences      ? u8"preferences"
                                                          : u8"capture";
            tabs.tab_width = 90.0f;
            tabs.select = [](const std::u8string& key) -> luil::input_action {
                const auto page = key == u8"region" ? options_page::region
                    : key == u8"output"             ? options_page::output
                    : key == u8"preferences"        ? options_page::preferences
                                                    : options_page::capture;
                return luil::make_app_action(options_tab_intent { page });
            };

            auto tab_bar { std::make_unique<luil::tab_bar_element>(std::move(tabs)) };
            tab_bar->set_enabled(runtime_state_.dialog.kind == message_dialog_kind::none);
            tab_bar->arrange({ { 20.0f * scale, 158.0f * scale, content_width * scale, 32.0f * scale }, scale });
            content_root->add(std::move(tab_bar));

            // 수집 옵션과 실제 출력 크기를 보여 주는 캡쳐 탭을 구성한다.
            if (page_ == options_page::capture)
            {
                label(u8"format-heading", u8"저장 형식", 20, 208, content_width, 11);
                label(u8"still-label", u8"스크린샷", 20, 242, 110);
                choice(
                    u8"still-format",
                    u8"정지 이미지 형식",
                    { { u8"png", u8"PNG" }, { u8"webp", u8"WebP" } },
                    runtime_state_.settings.capture.still_format == image_format::png ? u8"png" : u8"webp",
                    runtime_command::set_still_format,
                    240
                );
                label(u8"animation-label", u8"애니메이션", 20, 284, 110);

                const auto format { runtime_state_.settings.capture.animation_format };
                choice(
                    u8"animation-format",
                    u8"애니메이션 형식",
                    { { u8"webp", u8"WebP" }, { u8"apng", u8"APNG" }, { u8"gif", u8"GIF" } },
                    format == image_format::apng      ? u8"apng"
                        : format == image_format::gif ? u8"gif"
                                                      : u8"webp",
                    runtime_command::set_animation_format,
                    282
                );
                label(
                    u8"animation-note",
                    format == image_format::gif ? u8"GIF · Windows 내장 · 256색 · 레거시 호환"
                                                : u8"WebP 손실·무손실 / APNG 무손실 · 저장 탭에서 설정",
                    20,
                    316,
                    content_width,
                    11
                );
                label(u8"output-heading", u8"녹화·출력", 20, 354, content_width, 11);
                label(u8"fps-label", u8"프레임 속도", 20, 390, 160);
                luil::dropdown_config fps {};
                fps.owner = u8"fps";
                fps.placeholder = u8"프레임 속도";

                const int rate { runtime_state_.settings.capture.fps };
                fps.text = value_text(rate) + u8" fps · "
                    + (rate == 10       ? u8"가볍게"
                           : rate == 16 ? u8"균형"
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
                dropdown->arrange(
                    { { (width / scale - 205.0f) * scale, 388.0f * scale, 185.0f * scale, 32.0f * scale }, scale }
                );
                content_root->add(std::move(dropdown));
                help.emplace(
                    u8"fps",
                    help_content { .title = u8"프레임 속도",
                                   .body
                                   = u8"초당 캡쳐할 프레임 수입니다. 높은 FPS는 움직임을 부드럽게 하며 CPU와 디스크 "
                                     u8"사용량을 늘립니다."
                    }
                );
                label(u8"scale-label", u8"출력 크기", 20, 432, 220);
                label(
                    u8"scale-value",
                    value_text(runtime_state_.settings.capture.output_percent) + u8"%",
                    width / scale - 75,
                    432,
                    55,
                    15
                );
                luil::slider_config slider {};
                slider.maximum = 5.0f;
                slider.value = runtime_state_.output_step;

                const auto scale_command = [](const runtime_command action, const float value) -> luil::input_action {
                    const std::string text { std::to_string(value) };
                    return luil::app_ui_command { static_cast<std::uint32_t>(action),
                                                  std::u8string { text.begin(), text.end() }
                    };
                };
                slider.change = [scale_command](const float delta) {
                    return scale_command(runtime_command::adjust_scale, delta);
                };
                slider.change_to = [scale_command](const float value) {
                    return scale_command(runtime_command::set_scale_index, value);
                };

                auto output_slider { std::make_unique<output_slider_element>(std::move(slider)) };
                output_slider->set_access_name(u8"출력 배율 · 25, 50, 75, 100, 150, 200% 단계");
                output_slider->set_enabled(editable);
                output_slider->arrange(
                    { { 20.0f * scale, 463.0f * scale, content_width * scale, 24.0f * scale }, scale }
                );
                content_root->add(std::move(output_slider));
                help_content scale_help {};
                scale_help.title = u8"출력 배율";
                scale_help.body = u8"25·50·75·100·150·200% 단계로 조절합니다. 캡쳐 영역은 유지하며 저장될 이미지의 "
                                  u8"크기만 변경합니다. 방향키로 한 단계씩 이동합니다.";
                help.emplace(u8"scale", std::move(scale_help));
                label(u8"scale-min", u8"25%", 20, 491, 70, 11);
                label(u8"scale-original", u8"원본 100%", 20 + (content_width - 12) * 0.6f - 27, 491, 80, 11);
                label(u8"scale-max", u8"200%", width / scale - 58, 491, 40, 11);
                card(u8"output-card", 528, 64);
                label(u8"output-caption", u8"저장될 이미지", 32, 534, 180, 11);

                const auto selected { runtime_state_.pending_recording
                                          ? std::optional<capture_region> { runtime_state_.recorded_region }
                                          : region_state_.region
                };

                const int percent { runtime_state_.settings.capture.output_percent };
                const auto dimensions = selected ? value_text(selected->width * percent / 100) + u8" × "
                        + value_text(selected->height * percent / 100) + u8" px"
                                                 : std::u8string { u8"영역을 선택하세요" };
                label(u8"output-dimensions", dimensions, 32, 559, 230, 18);
                label(u8"aspect-note", u8"비율 유지", width / scale - 95, 553, 75, 11);

                const bool webp { format == image_format::webp
                                  || runtime_state_.settings.capture.still_format == image_format::webp
                };
                label(u8"compression-label", u8"WebP 압축", 20, 612, 110);

                const auto compression { runtime_state_.settings.capture.compression };
                choice(
                    u8"compression",
                    u8"WebP 저장 방식",
                    { { u8"fast", u8"빠른 저장" }, { u8"small", u8"균형" }, { u8"maximum", u8"최대 압축" } },
                    compression == lossless_compression::fast          ? u8"fast"
                        : compression == lossless_compression::maximum ? u8"maximum"
                                                                       : u8"small",
                    runtime_command::set_compression,
                    610,
                    webp
                );
                label(
                    u8"compression-note",
                    !webp                                           ? u8"WebP 선택 시 사용할 수 있습니다."
                        : compression == lossless_compression::fast ? u8"후처리를 생략해 빠르게 저장합니다."
                        : compression == lossless_compression::maximum
                        ? u8"최대 압축은 저장에 오래 걸릴 수 있습니다."
                        : u8"권장 · 저장 속도와 파일 크기의 균형을 맞춥니다.",
                    20,
                    644,
                    content_width,
                    11
                );
            }
            // 영역 좌표·크기·프리셋과 프레임 외곽 표시를 한 탭에서 관리한다.
            else if (page_ == options_page::region)
            {
                card(u8"region-card", 214, 112);

                const auto selected { region_state_.region };
                label(u8"region-heading", u8"현재 캡쳐 영역", 32, 220, 240, 11);
                label(
                    u8"current-region",
                    selected ? value_text(selected->width) + u8" × " + value_text(selected->height) + u8" px"
                             : std::u8string { u8"아직 선택한 영역이 없습니다" },
                    32,
                    248,
                    content_width - 24,
                    18
                );
                label(
                    u8"region-position",
                    selected ? u8"X " + value_text(selected->x) + u8" · Y " + value_text(selected->y)
                             : std::u8string { u8"위치와 크기로 새 프레임을 만들 수 있습니다" },
                    32,
                    282,
                    content_width - 24,
                    11
                );
                text_button(
                    u8"edit-region",
                    u8"위치·크기 편집",
                    runtime_command::show_region_editor,
                    20,
                    344,
                    content_width,
                    !runtime_state_.busy && !runtime_state_.recording,
                    luil::text_button_visual::accent
                );
                help[u8"edit-region"]
                    = { .title = u8"캡쳐 영역 설정",
                        .body = u8"X·Y 위치와 가로·세로 크기를 픽셀 단위로 입력합니다. 자주 쓰는 크기로 새 프레임을 "
                                u8"만들거나 현재 영역을 정밀하게 조절할 수 있습니다."
                    };
                label(u8"preset-heading", u8"저장한 프리셋", 20, 398, content_width, 11);

                const auto preset = runtime_state_.settings.presets.empty()
                    ? std::u8string { u8"저장된 영역 프리셋이 없습니다." }
                    : value_text(runtime_state_.preset_index + 1) + u8" / "
                        + value_text(static_cast<int>(runtime_state_.settings.presets.size())) + u8" · "
                        + runtime_state_.settings.presets[static_cast<std::size_t>(runtime_state_.preset_index)].name;
                label(u8"preset-name", preset, 20, 430, content_width, 12, true);

                const bool can_apply { !runtime_state_.settings.presets.empty() && !runtime_state_.busy
                                       && !runtime_state_.recording
                };
                text_button(u8"previous-preset", u8"이전", runtime_command::previous_preset, 20, 480, 68, can_apply);
                text_button(u8"next-preset", u8"다음", runtime_command::next_preset, 96, 480, 68, can_apply);
                text_button(
                    u8"apply-preset",
                    u8"이 영역 적용",
                    runtime_command::apply_preset,
                    172,
                    480,
                    content_width - 152,
                    can_apply,
                    luil::text_button_visual::accent
                );
                text_button(
                    u8"save-preset",
                    u8"현재 영역 저장",
                    runtime_command::save_preset,
                    20,
                    526,
                    224,
                    editable && selected.has_value() && !region_state_.selecting
                );
                text_button(
                    u8"delete-preset",
                    u8"삭제",
                    runtime_command::delete_preset,
                    254,
                    526,
                    content_width - 234,
                    can_apply && editable,
                    luil::text_button_visual::danger
                );
                luil::check_config topmost {};
                topmost.owner = u8"always-on-top";
                topmost.label = u8"명령창 항상 위";
                topmost.checked = runtime_state_.settings.always_on_top;
                topmost.toggle = [](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                    return { luil::app_ui_command {
                        .command = static_cast<std::uint32_t>(runtime_command::toggle_always_on_top)
                    }
                    };
                };

                auto topmost_element { std::make_unique<luil::check_element>(std::move(topmost)) };
                topmost_element->set_enabled(runtime_state_.settings_writable);
                topmost_element->arrange(
                    { { 20.0f * scale, 574.0f * scale, content_width * scale, luil::check_row_height * scale }, scale }
                );
                content_root->add(std::move(topmost_element));
                help[u8"always-on-top"]
                    = { .title = u8"명령창 항상 위",
                        .body = u8"명령창을 다른 창보다 위에 표시합니다. 설정은 재실행해도 유지됩니다."
                    };
                label(u8"glow-label", u8"글로우 두께", 20, 618, 160);
                label(
                    u8"glow-value",
                    value_text(runtime_state_.settings.glow_width) + u8" px",
                    width / scale - 80,
                    618,
                    60
                );
                luil::slider_config glow {};
                glow.minimum = 0;
                glow.maximum = 40;
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
                slider->set_enabled(editable);
                slider->arrange({ { 20 * scale, 650 * scale, content_width * scale, 24 * scale }, scale });
                content_root->add(std::move(slider));
                help[u8"glow-width"]
                    = { .title = u8"프레임 글로우 두께",
                        .body
                        = u8"프레임 바깥으로 번지는 빛의 범위를 조절합니다. 0은 끄기입니다. 캡쳐 영역 안은 그대로 "
                          u8"유지됩니다."
                    };
            }
            // 보관 원본을 유지한 채 출력 형식과 압축 옵션으로 다시 저장할 수 있게 한다.
            else if (page_ == options_page::output)
            {
                const auto& settings { runtime_state_.settings };
                card(u8"draft-card", 208, 104);
                label(
                    u8"draft-heading",
                    runtime_state_.pending_recording ? u8"보관 중인 녹화" : u8"정지 후 원본을 보관합니다",
                    36,
                    220,
                    content_width - 32,
                    11
                );

                const auto source { runtime_state_.recorded_region };
                const auto duration { recording_time(static_cast<std::uint64_t>(runtime_state_.recorded_duration)) };
                label(
                    u8"draft-info",
                    runtime_state_.pending_recording
                        ? duration + u8" · " + value_text(source.width) + u8" × " + value_text(source.height) + u8" px"
                        : u8"저장 버튼으로 내보내기",
                    36,
                    248,
                    content_width - 32,
                    14
                );
                label(u8"draft-saved", runtime_state_.saved_summary, 36, 276, content_width - 32, 11);
                label(u8"save-format-label", u8"애니메이션", 20, 332, 110);
                choice(
                    u8"animation-format",
                    u8"애니메이션 형식",
                    { { u8"webp", u8"WebP" }, { u8"apng", u8"APNG" }, { u8"gif", u8"GIF" } },
                    settings.capture.animation_format == image_format::apng      ? u8"apng"
                        : settings.capture.animation_format == image_format::gif ? u8"gif"
                                                                                 : u8"webp",
                    runtime_command::set_animation_format,
                    330
                );

                const bool webp { settings.capture.animation_format == image_format::webp
                                  || settings.capture.still_format == image_format::webp
                };
                label(u8"webp-mode-label", u8"WebP 압축", 20, 376, 110);
                choice(
                    u8"webp-mode",
                    u8"WebP 압축 방식",
                    { { u8"lossless", u8"무손실" }, { u8"lossy", u8"손실" } },
                    settings.capture.webp_lossless ? u8"lossless" : u8"lossy",
                    runtime_command::set_webp_mode,
                    374,
                    webp
                );
                label(u8"quality-label", u8"손실 압축 품질", 20, 417, 230);
                label(u8"quality-value", value_text(settings.capture.webp_quality), width / scale - 72, 417, 52, 14);
                luil::slider_config quality {};
                quality.minimum = 1;
                quality.maximum = 100;
                quality.value = runtime_state_.quality_position;

                const auto quality_command = [](const runtime_command action, const float value) -> luil::input_action {
                    const auto text { std::to_string(value) };
                    return luil::app_ui_command { static_cast<std::uint32_t>(action), { text.begin(), text.end() } };
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
                slider->set_enabled(editable && webp && !settings.capture.webp_lossless);
                slider->arrange({ { 20 * scale, 446 * scale, content_width * scale, 24 * scale }, scale });
                content_root->add(std::move(slider));
                help[u8"webp-quality"] = {
                    .title = u8"WebP 손실 압축 품질",
                    .body = u8"낮을수록 파일이 작아지고 화질이 떨어집니다. 저장 후 품질이나 캡쳐 탭의 출력 크기를 바꿔 "
                            u8"다시 저장할 수 있습니다. 보관 원본에는 영향을 주지 않습니다.",
                };

                const auto check_box = [&](const char8_t* owner,
                                           const char8_t* text,
                                           const bool checked,
                                           const runtime_command action,
                                           const float y) {
                    luil::check_config config {};
                    config.owner = owner;
                    config.label = text;
                    config.checked = checked;
                    config.toggle = [action](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                        return { luil::app_ui_command { .command = static_cast<std::uint32_t>(action) } };
                    };

                    auto element { std::make_unique<luil::check_element>(std::move(config)) };
                    element->set_enabled(editable);
                    element->arrange(
                        { { 20 * scale, y * scale, content_width * scale, luil::check_row_height * scale }, scale }
                    );
                    content_root->add(std::move(element));
                };
                check_box(
                    u8"auto-save",
                    u8"녹화 정지 시 바로 저장",
                    settings.auto_save,
                    runtime_command::toggle_auto_save,
                    488
                );
                text_button(
                    u8"save-recording",
                    u8"녹화 저장",
                    runtime_command::save_recording,
                    20,
                    558,
                    224,
                    runtime_state_.pending_recording && !runtime_state_.busy,
                    luil::text_button_visual::accent
                );
                text_button(
                    u8"discard-recording",
                    u8"녹화 비우기",
                    runtime_command::discard_recording,
                    254,
                    558,
                    content_width - 234,
                    runtime_state_.pending_recording && !runtime_state_.busy,
                    luil::text_button_visual::danger
                );
                text_button(
                    u8"edit-recording",
                    u8"보관 녹화 편집",
                    runtime_command::edit_recording,
                    20,
                    648,
                    content_width,
                    runtime_state_.pending_recording && !runtime_state_.busy
                );
                text_button(
                    u8"open-animation",
                    u8"이미지를 열어 편집",
                    runtime_command::show_animation_editor,
                    20,
                    690,
                    content_width,
                    !runtime_state_.busy && !runtime_state_.recording
                );
                label(
                    u8"save-note",
                    u8"원본은 저장 후에도 유지됩니다. 설정을 바꾸고 다시 저장할 수 있습니다. 새 촬영은 녹화 비우기 후 "
                    u8"시작하세요.",
                    20,
                    607,
                    content_width,
                    11,
                    true
                );
            }
            else
            {
                const auto& appearance { runtime_state_.settings.appearance };
                label(u8"theme-label", u8"테마", 20, 208, 110, 14);
                choice(
                    u8"theme",
                    u8"테마",
                    { { u8"system", u8"시스템" }, { u8"light", u8"밝게" }, { u8"dark", u8"어둡게" } },
                    std::u8string { luil::theme_preference_name(appearance.theme) },
                    runtime_command::set_theme,
                    208
                );
                label(u8"appearance-note", u8"테마와 키 컬러는 즉시 반영하고 저장합니다", 20, 244, content_width, 11);
                label(u8"accent-label", u8"키 컬러", 20, 276, 110, 14);
                label(
                    u8"accent-name",
                    std::u8string { luil::accent_for(appearance.accent_id).label },
                    140,
                    276,
                    content_width - 120
                );

                auto colors { std::make_unique<accent_picker_element>(appearance.accent_id) };
                colors->set_enabled(editable);
                colors->arrange({ { 20 * scale, 310 * scale, content_width * scale, 84 * scale }, scale });

                for (const auto& element : colors->children())
                {
                    help[element->id().owner]
                        = { .title = element->accessibility().name,
                            .body = u8"버튼 강조색과 캡쳐 프레임·글로우에 함께 적용합니다. 재실행해도 유지됩니다."
                        };
                }

                content_root->add(std::move(colors));
                label(u8"shortcuts-heading", u8"단축키", 20, 410, content_width, 14);
                shortcuts_.add_inputs(
                    [&](std::unique_ptr<luil::ui_element> element) { content_root->add(std::move(element)); },
                    width / scale,
                    446,
                    scale,
                    editable
                );

                const auto bindings { shortcuts_.bindings() };
                const auto add_button = [&](
                                            const char8_t* owner, const char8_t* text, const float x, const bool apply
                                        ) {
                    auto element = std::make_unique<luil::text_button_element>(
                        luil::ui_element_id { command_kind, owner },
                        luil::text_button_config { .text = text,
                                                   .visual = apply ? luil::text_button_visual::accent
                                                                   : luil::text_button_visual::normal
                        }
                    );
                    element->set_access_name(text);
                    element->set_enabled(editable && (!apply || bindings.has_value()));
                    element->set_action(
                        luil::ui_trigger::left_click,
                        [bindings, apply](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                            if (!apply)
                            {
                                return { luil::make_app_action(reset_shortcuts_intent {}) };
                            }

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
                    element->arrange({ { x * scale, 582 * scale, 114 * scale, 32 * scale }, scale });
                    content_root->add(std::move(element));
                };
                add_button(u8"reset-shortcuts", u8"기본값", 20, false);
                add_button(u8"apply-shortcuts", u8"단축키 적용", width / scale - 134, true);

                const auto error { shortcuts_.error() };
                label(
                    u8"shortcut-error",
                    error.empty() ? (shortcuts_edited_ ? std::u8string {} : runtime_state_.shortcut_error) : error,
                    20,
                    624,
                    content_width,
                    11,
                    true
                );
                label(
                    u8"shortcut-help",
                    u8"Ctrl+Alt+F9처럼 조합을 입력하세요.\n비워 두면 사용하지 않습니다. 기본값도 적용해야 저장됩니다.",
                    20,
                    676,
                    content_width,
                    11,
                    true
                );
            }

            if (page_ == options_page::capture || page_ == options_page::region)
            {
                luil::check_config cursor {};
                cursor.owner = u8"include-cursor";
                cursor.label = u8"마우스 커서 포함";
                cursor.checked = runtime_state_.settings.capture.include_cursor;
                cursor.toggle = [](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                    return { luil::app_ui_command { .command
                                                    = static_cast<std::uint32_t>(runtime_command::toggle_cursor)
                    }
                    };
                };

                auto cursor_element { std::make_unique<luil::check_element>(std::move(cursor)) };
                cursor_element->set_enabled(editable && !runtime_state_.pending_recording);
                cursor_element->arrange(
                    { { 20.0f * scale, 680.0f * scale, 260.0f * scale, luil::check_row_height * scale }, scale }
                );
                content_root->add(std::move(cursor_element));
            }

            label(u8"runtime-status", runtime_state_.status, 20, 722, content_width, 11, true);

            if (!runtime_state_.saved_output.empty())
            {
                const auto filename { runtime_state_.saved_output.filename().u8string() };
                luil::text_button_config file_config {};

                file_config.text = filename;
                file_config.visual = luil::text_button_visual::link;
                auto file { std::make_unique<luil::text_button_element>(
                    luil::ui_element_id { command_kind, u8"saved-file" }, std::move(file_config)
                )
                };
                file->set_cursor(luil::ui_cursor::hand);
                file->set_enabled(runtime_state_.dialog.kind == message_dialog_kind::none);
                file->set_action(
                    luil::ui_trigger::left_click,
                    [](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                        return { luil::app_ui_command {
                            .command = static_cast<std::uint32_t>(runtime_command::open_saved_file)
                        }
                        };
                    }
                );
                file->arrange({ { 20 * scale, 764 * scale, (content_width - 44) * scale, 32 * scale }, scale });
                content_root->add(std::move(file));
                help[u8"saved-file"]
                    = { .title = u8"저장 파일 열기",
                        .body = u8"기본 이미지 앱으로 엽니다.\n" + runtime_state_.saved_output.u8string()
                    };
            }

            luil::button_config folder_config {};

            folder_config.glyph = luil::codicons::icon_folder_opened;
            folder_config.icon_size = 16;
            folder_config.corner_radius = 6;
            auto folder { std::make_unique<luil::button_element>(
                luil::ui_element_id { command_kind, u8"output-folder" }, std::move(folder_config)
            )
            };
            folder->set_access_name(u8"저장 폴더 열기");
            folder->set_cursor(luil::ui_cursor::hand);
            folder->set_enabled(runtime_state_.dialog.kind == message_dialog_kind::none);
            folder->set_action(
                luil::ui_trigger::left_click, [](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                    return { luil::app_ui_command {
                        .command = static_cast<std::uint32_t>(runtime_command::open_output_folder)
                    }
                    };
                }
            );
            folder->arrange({ { (width / scale - 52) * scale, 764 * scale, 32 * scale, 32 * scale }, scale });
            content_root->add(std::move(folder));
            help[u8"output-folder"]
                = { .title = u8"저장 폴더 열기", .body = u8"실행 파일 옆 data/captures 폴더를 엽니다." };
            help[u8"runtime-status"].title = u8"현재 상태 · 저장 결과";
            help[u8"preset-name"].title = u8"선택한 영역 프리셋";
            help[u8"include-cursor"] = { .title = u8"마우스 커서 포함",
                                         .body = u8"선택한 영역 안의 마우스 커서를 이미지에 함께 저장합니다."
            };
            luil::scroll_area_config scrolling {};
            scrolling.owner = u8"options-scroll";
            scrolling.content_height = static_cast<float>(options_window_height - command_window_height);
            options_scroll_
                = luil::clamp_scroll(scrolling.content_height, height / scale - command_window_height, options_scroll_);
            scrolling.scroll_offset = options_scroll_;
            scrolling.scroll = [](const float delta) -> luil::input_action {
                return luil::make_app_action(options_scroll_intent { delta, false });
            };
            scrolling.scroll_to = [](const float offset) -> luil::input_action {
                return luil::make_app_action(options_scroll_intent { offset, true });
            };

            auto area { std::make_unique<luil::scroll_area_element>(std::move(scrolling)) };
            area->set_content(std::move(content_root));
            area->arrange(
                { { 0, command_window_height * scale, width, std::max(0.0f, height - command_window_height * scale) },
                  scale
                }
            );
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
