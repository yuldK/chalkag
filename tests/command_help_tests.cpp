#include "app/command_window.h"
#include "app/help_text.h"
#include "app/image_drop.h"
#include "capture/recording_time.h"

#include "luil/app/app_host.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "luil/ui/ui_tree.h"
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace chalkag {
    namespace {
        constexpr luil::ui_element_kind command_kind { luil::application_element_kind(0) };
        int failure_count {};

        void check(const bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failure_count;
            }
        }

        bool approximately_equal(const float actual, const float expected) noexcept
        {
            return std::abs(actual - expected) < 0.01f;
        }

        help_target_intent target(
            const char8_t* owner = u8"select-region", const luil::ui_element_kind kind = command_kind
        )
        {
            return { .target = { kind, owner },
                     .started_at = std::chrono::steady_clock::time_point { std::chrono::seconds { 1 } },
                     .maximum_width = 340,
                     .available_above = 600,
                     .available_below = 600
            };
        }

        void send_target(command_driver& driver, const help_target_intent& value)
        {
            driver.handle(luil::app_message { value });
        }

        void check_tree(const std::shared_ptr<const luil::ui_tree>& tree)
        {
            check(tree != nullptr, "frame publishes an immutable UI tree");

            if (tree == nullptr)
            {
                return;
            }

            check(tree->duplicate_ids().empty(), "published tree has no duplicate element IDs");
            check(tree->unarranged().empty(), "published tree arranges every visible element");
        }

        std::shared_ptr<const luil::ui_frame> frame_for(command_driver& driver)
        {
            auto frame { driver.make_frame() };
            check(frame != nullptr, "driver publishes a frame");

            if (frame == nullptr)
            {
                throw std::runtime_error { "Driver did not publish a frame." };
            }

            check_tree(frame->tree);

            for (const auto& popup : frame->popups)
            {
                check_tree(popup.tree);
            }

            return frame;
        }

        const luil::ui_popup* help_popup(const luil::ui_frame& frame)
        {
            if (frame.popups.size() != 1 || frame.popups.front().id != u8"command-help")
            {
                return nullptr;
            }

            return &frame.popups.front();
        }

        const luil::ui_element* help_element(const luil::ui_popup& popup, const char8_t* owner)
        {
            const auto* const element { popup.tree->find({ command_kind, owner }) };
            check(element != nullptr, "custom help publishes the expected text element");

            if (element != nullptr)
            {
                check(
                    element->accessibility().role == luil::access_role::static_text,
                    "help text exposes static_text accessibility"
                );
                check(!element->tab_stop() && !element->interactive(), "help text does not take focus or actions");
                check(element->tooltip().empty(), "custom help text has no nested built-in tooltip");
            }

            return element;
        }

        std::u8string help_name(const luil::ui_popup& popup, const char8_t* owner)
        {
            const auto* const element { help_element(popup, owner) };
            return element != nullptr ? element->accessibility().name : std::u8string {};
        }

        void test_content_and_disabled_reason()
        {
            command_driver driver {};
            region_state_intent region {};
            region.status = u8"영역을 먼저 선택하세요.";
            driver.handle(luil::app_message { region });
            send_target(driver, target());

            const auto frame { frame_for(driver) };
            const auto* const popup { help_popup(*frame) };
            check(popup != nullptr, "select-region target creates one custom help popup");

            if (popup == nullptr)
            {
                return;
            }

            check(
                help_name(*popup, u8"help-title") == u8"캡쳐 영역 선택",
                "select-region title keeps complete accessible name"
            );
            check(
                help_name(*popup, u8"help-shortcut") == u8"Ctrl+Shift+A",
                "select-region shortcut is a separate accessible text"
            );
            check(
                help_name(*popup, u8"help-body")
                    == u8"화면을 드래그하여 캡쳐 영역을 선택합니다.\n모서리로 크기를 조절하고 상단 핸들로 이동합니다. "
                       u8"Esc로 선택을 취소합니다.\n\n영역을 먼저 선택하세요.",
                "select-region body keeps instructions and current full status"
            );
            check(
                popup->width == 340 && popup->height > 0 && popup->border,
                "custom help has bounded positive dimensions and a border"
            );
            check(popup->tree->focus_order().empty(), "custom help popup has no tab stops");

            for (const auto* const owner :
                 { u8"select-region", u8"take-screenshot", u8"toggle-recording", u8"open-options" })
            {
                const auto* const button { frame->tree->find({ command_kind, owner }) };
                check(button != nullptr && button->tooltip().empty(), "toolbar commands remove built-in tooltips");
            }

            send_target(driver, target(u8"take-screenshot"));

            const auto screenshot_frame { frame_for(driver) };
            const auto* const screenshot_popup { help_popup(*screenshot_frame) };
            const auto* const screenshot { screenshot_frame->tree->find({ command_kind, u8"take-screenshot" }) };
            check(
                screenshot != nullptr && !screenshot->enabled(), "screenshot stays disabled without a selected region"
            );
            check(screenshot_popup != nullptr, "disabled screenshot still provides custom help");

            if (screenshot_popup != nullptr)
            {
                check(
                    help_name(*screenshot_popup, u8"help-title") == u8"스크린샷 찍기",
                    "disabled screenshot preserves command title"
                );
                check(
                    help_name(*screenshot_popup, u8"help-shortcut") == u8"Ctrl+Shift+S",
                    "disabled screenshot preserves shortcut"
                );

                const auto body { help_name(*screenshot_popup, u8"help-body") };
                check(
                    body.starts_with(u8"먼저 캡쳐 영역을 선택하세요.\n\n"),
                    "disabled screenshot explains missing region first"
                );
                check(
                    body.find(u8"PNG 이미지로 저장합니다.") != std::u8string::npos,
                    "disabled screenshot retains format instructions"
                );
            }
        }

        void test_popup_lifetime()
        {
            command_driver driver {};

            auto requested { target() };
            send_target(driver, requested);

            const auto before { frame_for(driver) };
            const auto* const popup { help_popup(*before) };
            check(popup != nullptr && static_cast<bool>(popup->dismiss), "custom popup supplies its dismissal action");

            if (popup == nullptr || !popup->dismiss)
            {
                return;
            }

            const auto action { popup->dismiss(luil::popup_dismiss_reason::escape_key) };
            const auto* const message { std::get_if<luil::app_message>(&action) };
            check(
                message != nullptr && message->get<dismiss_help_intent>() != nullptr,
                "popup dismissal returns driver help-dismiss intent"
            );

            if (message != nullptr)
            {
                driver.handle(*message);
            }

            check(frame_for(driver)->popups.empty(), "dismissal removes help from next frame");
            check(help_popup(*before) != nullptr, "previous published frame remains an immutable snapshot");
            send_target(driver, requested);
            check(frame_for(driver)->popups.empty(), "same hover target and start token do not reopen dismissed help");
            requested.maximum_width = 300;
            send_target(driver, requested);
            check(frame_for(driver)->popups.empty(), "same hover token remains dismissed after dimension update");
            requested.started_at += std::chrono::seconds { 1 };
            send_target(driver, requested);
            check(help_popup(*frame_for(driver)) != nullptr, "new hover start token reopens same target");
            driver.handle(luil::app_message { dismiss_help_intent {} });
            requested.target.owner = u8"take-screenshot";
            send_target(driver, requested);
            check(help_popup(*frame_for(driver)) != nullptr, "different target reopens help after dismissal");
            send_target(driver, {});
            check(frame_for(driver)->popups.empty(), "clearing hover target removes help");
            send_target(driver, target(u8"select-region", luil::ui_element_kind::choice));
            check(frame_for(driver)->popups.empty(), "same owner with wrong element kind cannot anchor help");
            send_target(driver, target(u8"missing-command"));
            check(frame_for(driver)->popups.empty(), "missing target cannot create unanchored help");
        }

        void test_suppression()
        {
            command_driver driver {};
            send_target(driver, target());
            runtime_state state {};
            state.busy = true;
            driver.handle(luil::app_message { state });
            check(frame_for(driver)->popups.empty(), "saving suppresses custom help");
            state.busy = false;
            state.recording = true;
            driver.handle(luil::app_message { state });
            check(frame_for(driver)->popups.empty(), "recording suppresses custom help");
            state.recording = false;
            driver.handle(luil::app_message { state });
            region_state_intent region {};
            region.selecting = true;
            driver.handle(luil::app_message { region });
            check(frame_for(driver)->popups.empty(), "region selection suppresses custom help");
            region.selecting = false;
            driver.handle(luil::app_message { region });
            check(help_popup(*frame_for(driver)) != nullptr, "idle state restores help for current target");
        }

        void test_options_and_long_status()
        {
            command_driver driver {};
            runtime_state state {};
            state.options_open = true;
            driver.handle(luil::app_message { state });
            driver.handle(
                luil::app_message { window_metrics_intent { options_window_width, options_window_height, 1 } }
            );
            send_target(driver, target(u8"fps", luil::ui_element_kind::dropdown));

            const auto frame { frame_for(driver) };
            const auto* const choice { frame->tree->find({ luil::ui_element_kind::dropdown, u8"fps" }) };
            check(choice != nullptr && choice->tooltip().empty(), "FPS dropdown exists without built-in tooltip");
            check(
                choice != nullptr && choice->accessibility().name == u8"프레임 속도",
                "FPS dropdown has an accessible name"
            );

            const auto* const slider { frame->tree->find({ command_kind, u8"scale" }) };
            check(
                slider != nullptr && !slider->accessibility().name.empty() && slider->accessibility().range.has_value(),
                "output slider exposes a named accessible range"
            );
            check(
                frame->tree->find({ luil::ui_element_kind::choice, u8"animation-format/gif" }) != nullptr,
                "GIF remains visible without optional FFmpeg"
            );

            const auto* const popup { help_popup(*frame) };
            check(popup != nullptr, "FPS dropdown anchors custom help");

            if (popup != nullptr)
            {
                check(help_name(*popup, u8"help-title") == u8"프레임 속도", "FPS help title identifies dropdown");
                check(
                    help_name(*popup, u8"help-body")
                        == u8"초당 캡쳐할 프레임 수입니다. 높은 FPS는 움직임을 부드럽게 하며 CPU와 디스크 사용량을 "
                           u8"늘립니다.",
                    "FPS help explains frame rate effects"
                );
                check(
                    popup->tree->find({ command_kind, u8"help-shortcut" }) == nullptr,
                    "option without shortcut omits empty shortcut element"
                );
            }

            driver.handle(luil::app_message { options_tab_intent { options_page::output } });
            const auto output_frame { frame_for(driver) };
            const auto* const compression {
                output_frame->tree->find({ luil::ui_element_kind::choice, u8"compression/small" })
            };
            check(
                compression != nullptr && compression->accessibility().selected.value_or(false)
                    && compression->accessibility().name == u8"균형",
                "balanced lossless is the default UI choice"
            );
            check(
                output_frame->tree->find({ luil::ui_element_kind::choice, u8"compression/maximum" }) != nullptr,
                "maximum compression remains an explicit choice"
            );

            send_target(driver, target(u8"compression/small", luil::ui_element_kind::choice));

            const auto compression_frame { frame_for(driver) };
            const auto* const compression_popup { help_popup(*compression_frame) };
            check(
                compression_popup != nullptr
                    && help_name(*compression_popup, u8"help-body").find(u8"저장 버튼") != std::u8string::npos,
                "compression help explains deferred lossless encoding"
            );

            state.settings.capture.compression = lossless_compression::fast;
            state.recording = true;
            driver.handle(luil::app_message { state });

            const auto recording_frame { frame_for(driver) };
            const auto* const fast {
                recording_frame->tree->find({ luil::ui_element_kind::choice, u8"compression/fast" })
            };

            const auto* const recording_group {
                recording_frame->tree->find({ luil::ui_element_kind::choice_group, u8"compression" })
            };
            check(
                fast != nullptr && fast->accessibility().selected.value_or(false) && recording_group != nullptr
                    && !recording_group->enabled(),
                "recording locks the selected compression mode"
            );
            state.recording = false;
            state.busy = true;
            driver.handle(luil::app_message { state });

            const auto saving_frame { frame_for(driver) };
            const auto* const saving {
                saving_frame->tree->find({ luil::ui_element_kind::choice_group, u8"compression" })
            };
            check(saving != nullptr && !saving->enabled(), "post compression keeps settings locked while saving");
            state.busy = false;

            state.status = u8"저장 완료: E:\\아주긴한글폴더이름\\";

            for (int index {}; index < 16; ++index)
            {
                state.status += u8"공백없는한글폴더와파일경로\\";
            }

            state.status += u8"capture-20261006-123456.webp";
            driver.handle(luil::app_message { state });

            auto requested { target(u8"runtime-status") };
            requested.maximum_width = 160;
            requested.available_above = 140;
            requested.available_below = 140;
            send_target(driver, requested);

            const auto status_frame { frame_for(driver) };
            const auto* const status { status_frame->tree->find({ command_kind, u8"runtime-status" }) };
            check(
                status != nullptr && status->accessibility().name == state.status,
                "wrapped status keeps entire path accessible in main window"
            );

            const auto* const status_popup { help_popup(*status_frame) };
            check(status_popup != nullptr, "long runtime status provides expanded custom help");

            if (status_popup != nullptr)
            {
                check(
                    help_name(*status_popup, u8"help-title") == u8"현재 상태 · 저장 결과",
                    "runtime help title describes saved result"
                );
                check(
                    help_name(*status_popup, u8"help-body") == state.status,
                    "popup body retains entire path even when visible lines are elided"
                );

                const auto* const body { help_element(*status_popup, u8"help-body") };
                check(
                    body != nullptr && body->bounds().height > 19, "long unbroken path wraps across multiple body lines"
                );

                const auto full_layout { layout_help_text(state.status, status_popup->width - 24, 12) };
                check(
                    body != nullptr && body->bounds().height < static_cast<float>(full_layout.lines.size()) * 19,
                    "limited popup body visibly truncates long path while accessibility stays complete"
                );
                check(
                    status_popup->height <= 140 && status_popup->width <= 160,
                    "long status help obeys available dimensions"
                );
            }

            state.options_open = false;
            driver.handle(luil::app_message { state });
            check(frame_for(driver)->popups.empty(), "closing options removes help for unavailable option target");
        }

        void test_options_menu_and_scroll()
        {
            command_driver driver {};
            runtime_state state {};
            state.options_open = true;
            driver.handle(luil::app_message { state });
            driver.handle(luil::app_message { window_metrics_intent { options_window_width, 400, 1 } });
            driver.handle(luil::app_message { fps_menu_intent { true } });

            const auto opened { frame_for(driver) };
            check(
                !opened->popups.empty() && opened->popups.front().id == u8"fps-menu",
                "FPS dropdown opens its menu surface"
            );
            driver.handle(luil::app_message { options_scroll_intent { 10000, true } });

            const auto scrolled { frame_for(driver) };
            const auto* const status { scrolled->tree->find({ command_kind, u8"runtime-status" }) };
            check(scrolled->popups.empty(), "scrolling dismisses stale FPS menu anchors");
            check(
                status != nullptr && status->bounds().y >= command_window_height
                    && status->bounds().y + status->bounds().height <= 400,
                "small window scroll reveals footer without clipping it"
            );
            state.recording = true;
            driver.handle(luil::app_message { state });
            driver.handle(luil::app_message { fps_menu_intent { true } });

            const auto locked { frame_for(driver) };
            const auto* const slider { locked->tree->find({ command_kind, u8"scale" }) };
            check(
                locked->popups.empty() && slider != nullptr && !slider->enabled(),
                "recording locks dropdown expansion and output slider"
            );
        }

        void test_popup_placement_and_guards()
        {
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                command_driver driver {};
                driver.handle(luil::app_message { window_metrics_intent { 220 * scale, 64 * scale, scale } });

                auto requested { target() };
                requested.available_above = 0;
                send_target(driver, requested);

                const auto below_frame { frame_for(driver) };
                const auto* const below { help_popup(*below_frame) };
                const auto* const anchor { below_frame->tree->find(requested.target) };
                check(below != nullptr && anchor != nullptr, "placement fixture publishes target and popup");

                if (below == nullptr || anchor == nullptr)
                {
                    continue;
                }

                const auto box { anchor->bounds() };
                check(
                    approximately_equal(below->x, box.x / scale)
                        && approximately_equal(below->y, (box.y + box.height) / scale + 8),
                    "help uses logical coordinates below target when space is available"
                );
                check(
                    approximately_equal(below->tree->root()->bounds().width, below->width * scale)
                        && approximately_equal(below->tree->root()->bounds().height, below->height * scale),
                    "popup tree uses physical dimensions at current DPI scale"
                );
                requested.available_above = 600;
                requested.available_below = 0;
                send_target(driver, requested);

                const auto above_frame { frame_for(driver) };
                const auto* const above { help_popup(*above_frame) };
                check(above != nullptr, "above-only space still displays help");

                if (above != nullptr)
                {
                    check(
                        approximately_equal(above->y, box.y / scale - above->height - 8),
                        "help moves above target when below space cannot fit it"
                    );
                }

                requested.available_below = below->height;
                send_target(driver, requested);

                const auto exact_frame { frame_for(driver) };
                const auto* const exact { help_popup(*exact_frame) };
                check(
                    exact != nullptr && approximately_equal(exact->y, (box.y + box.height) / scale + 8),
                    "exactly fitting below space chooses lower placement"
                );
                requested.available_below -= 0.5f;
                send_target(driver, requested);

                const auto short_frame { frame_for(driver) };
                const auto* const short_popup { help_popup(*short_frame) };
                check(
                    short_popup != nullptr
                        && approximately_equal(short_popup->y, box.y / scale - short_popup->height - 8),
                    "slightly insufficient below space chooses upper placement"
                );
            }

            command_driver driver {};

            auto requested { target() };
            requested.maximum_width = 159;
            send_target(driver, requested);
            check(frame_for(driver)->popups.empty(), "width below 160 suppresses popup");
            requested.maximum_width = 160;
            requested.available_above = 139;
            requested.available_below = 139;
            send_target(driver, requested);
            check(frame_for(driver)->popups.empty(), "available height below 140 suppresses popup");
            requested.available_above = 140;
            send_target(driver, requested);

            const auto minimum_frame { frame_for(driver) };
            const auto* const minimum { help_popup(*minimum_frame) };
            check(minimum != nullptr, "inclusive 160-width and 140-height guards allow popup");

            if (minimum != nullptr)
            {
                check(
                    minimum->width == 160 && minimum->height <= 140,
                    "minimum-space popup stays within configured limits"
                );
            }

            requested.maximum_width = 1000;
            requested.available_above = 600;
            requested.available_below = 600;
            send_target(driver, requested);

            const auto wide_frame { frame_for(driver) };
            const auto* const wide { help_popup(*wide_frame) };
            check(wide != nullptr && wide->width == 340, "popup width caps at 340 even on wide monitor");
            requested.minimum_x = -200;
            requested.maximum_x = 300;
            send_target(driver, requested);

            const auto bounded_frame { frame_for(driver) };
            const auto* const bounded { help_popup(*bounded_frame) };
            check(
                bounded != nullptr && approximately_equal(bounded->x, -40),
                "popup shifts left to remain within target monitor"
            );
            requested.minimum_x = 100;
            requested.maximum_x = 500;
            send_target(driver, requested);

            const auto right_frame { frame_for(driver) };
            const auto* const right { help_popup(*right_frame) };
            check(
                right != nullptr && approximately_equal(right->x, 100),
                "popup shifts right to remain within target monitor"
            );
        }

        void test_caption_button_hit()
        {
            check(!hit_command_caption_button(luil::ui_frame {}, 0, 0), "empty frame keeps native caption hit testing");

            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                for (const bool expanded : { false, true })
                {
                    command_driver driver {};
                    runtime_state state {};
                    state.options_open = expanded;

                    const float width { static_cast<float>(expanded ? options_window_width : command_window_width)
                                        * scale
                    };

                    const float height { static_cast<float>(expanded ? options_window_height : command_window_height)
                                         * scale
                    };
                    driver.handle(luil::app_message { window_metrics_intent { width, height, scale } });

                    for (const bool writable : { false, true })
                    {
                        state.settings_writable = writable;
                        driver.handle(luil::app_message { state });

                        const auto frame { frame_for(driver) };
                        const auto* const pin { frame->tree->find({ command_kind, u8"toggle-topmost" }) };
                        check(pin != nullptr, "caption publishes the pin button");

                        if (pin == nullptr)
                        {
                            continue;
                        }

                        const auto bounds { pin->bounds() };
                        const float y { bounds.y + bounds.height / 2 };
                        check(
                            hit_command_caption_button(*frame, bounds.x + bounds.width / 2, y),
                            "pin routes client input at every scale and window size"
                        );
                        check(
                            hit_command_caption_button(*frame, bounds.x, bounds.y), "pin includes its top-left boundary"
                        );
                        check(
                            !hit_command_caption_button(*frame, bounds.x - 1, y), "caption beside pin remains draggable"
                        );
                        check(
                            !hit_command_caption_button(*frame, bounds.x + bounds.width, y),
                            "pin excludes its right boundary"
                        );
                        check(
                            !hit_command_caption_button(*frame, bounds.x + bounds.width / 2, bounds.y + bounds.height),
                            "pin excludes its bottom boundary"
                        );
                        check(
                            !hit_command_caption_button(*frame, width - 14 * scale, y),
                            "close button retains native caption handling"
                        );

                        const auto* const select { frame->tree->find({ command_kind, u8"select-region" }) };
                        check(
                            select != nullptr
                                && !hit_command_caption_button(*frame, select->bounds().x + 1, select->bounds().y + 1),
                            "toolbar does not override native hit testing"
                        );
                    }
                }
            }
        }

        void test_editor_lifetime()
        {
            command_driver animations {};
            animations.handle(luil::app_message { open_animation_editor_intent {} });
            check(frame_for(animations)->windows.size() == 1, "animation editor owns a secondary window");
            runtime_state capture_state {};
            capture_state.recording = true;
            animations.handle(luil::app_message { capture_state });
            check(frame_for(animations)->windows.empty(), "recording excludes animation editor window");
            capture_state.recording = false;
            capture_state.busy = true;
            animations.handle(luil::app_message { capture_state });
            check(frame_for(animations)->windows.empty(), "capture jobs exclude animation editor window");
            capture_state.busy = false;
            animations.handle(luil::app_message { capture_state });
            animations.handle(luil::app_message { animation_editor_intent { animation_editor_action::close } });
            check(frame_for(animations)->windows.empty(), "closing editor removes its secondary window");
            command_driver driver {};
            send_target(driver, target());
            driver.handle(
                luil::app_message { open_region_editor_intent { { 100, 100, 640, 360 }, { 0, 0, 1920, 1080 } } }
            );

            const auto editing { frame_for(driver) };
            check(
                editing->popups.size() == 1 && editing->popups.front().id == u8"region-editor",
                "editor owns popup surface without competing help"
            );
            runtime_state state {};
            state.settings.always_on_top = true;
            state.recording = true;
            driver.handle(luil::app_message { state });

            const auto recording { frame_for(driver) };
            check(recording->popups.empty(), "recording closes editor before capture");

            const auto* const pin { recording->tree->find({ command_kind, u8"toggle-topmost" }) };
            check(pin != nullptr && pin->enabled(), "topmost remains available while recording");
            state.recording = false;
            driver.handle(luil::app_message { state });
            check(help_popup(*frame_for(driver)) != nullptr, "ending capture restores help rather than stale editor");
            driver.handle(
                luil::app_message { open_region_editor_intent { { 100, 100, 640, 360 }, { 0, 0, 1920, 1080 } } }
            );
            driver.handle(luil::app_message { region_state_intent { .selecting = true } });
            check(frame_for(driver)->popups.empty(), "drag selection closes numeric editor");
        }

        void test_clear_recording_ui()
        {
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                for (const bool expanded : { false, true })
                {
                    command_driver driver {};
                    runtime_state state {};
                    state.options_open = expanded;
                    driver.handle(luil::app_message { state });
                    driver.handle(
                        luil::app_message {
                            window_metrics_intent { (expanded ? options_window_width : command_window_width) * scale,
                                                    (expanded ? options_window_height : command_window_height) * scale,
                                                    scale
                            }
                        }
                    );
                    check(
                        !frame_for(driver)->tree->find({ command_kind, u8"toolbar-discard-recording" }),
                        "clear is hidden without a retained recording"
                    );

                    state.recording = true;
                    driver.handle(luil::app_message { state });
                    check(
                        !frame_for(driver)->tree->find({ command_kind, u8"toolbar-discard-recording" }),
                        "clear is hidden while recording"
                    );
                    state.recording = false;
                    state.pending_recording = true;
                    driver.handle(luil::app_message { state });
                    auto frame { frame_for(driver) };
                    const auto* clear { frame->tree->find({ command_kind, u8"toolbar-discard-recording" }) };
                    const auto* save { frame->tree->find({ command_kind, u8"toggle-recording" }) };
                    const auto* options { frame->tree->find({ command_kind, u8"open-options" }) };
                    check(
                        clear && clear->enabled() && clear->accessibility().name == u8"새 녹화",
                        "retained recording exposes a new recording action"
                    );
                    if (clear && save && options)
                    {
                        check(
                            save->bounds().x + save->bounds().width + 6 * scale <= clear->bounds().x
                                && clear->bounds().x + clear->bounds().width + 6 * scale <= options->bounds().x,
                            "clear fits between save and options at every DPI and window size"
                        );
                        const auto actions { (*clear->action(luil::ui_trigger::left_click))({}) };
                        const auto* action { std::get_if<luil::app_ui_command>(&actions.front()) };
                        check(
                            action && action->command == static_cast<std::uint32_t>(runtime_command::new_recording),
                            "new recording uses the safe replacement command"
                        );
                    }
                    send_target(driver, target(u8"toolbar-discard-recording"));
                    frame = frame_for(driver);
                    const auto* popup { help_popup(*frame) };
                    check(
                        popup && popup->tree->find({ command_kind, u8"help-shortcut" }) == nullptr,
                        "new recording never advertises the discard shortcut"
                    );

                    state.dialog = { message_dialog_kind::discard_recording, 1, u8"녹화 비우기", u8"확인" };
                    driver.handle(luil::app_message { state });
                    check(
                        !frame_for(driver)->tree->find({ command_kind, u8"toolbar-discard-recording" })->enabled(),
                        "confirmation blocks repeated clear commands"
                    );
                    state.dialog = {};
                    state.busy = true;
                    driver.handle(luil::app_message { state });
                    check(
                        !frame_for(driver)->tree->find({ command_kind, u8"toolbar-discard-recording" }),
                        "clear is hidden while saving"
                    );
                    state.busy = false;
                    state.pending_recording = false;
                    driver.handle(luil::app_message { state });
                    check(
                        !frame_for(driver)->tree->find({ command_kind, u8"toolbar-discard-recording" }),
                        "clearing restores the idle toolbar"
                    );
                }
            }
        }

        void test_clear_shortcut_editor()
        {
            shortcut_editor editor {};
            command_policy policy {};
            const auto target { policy.text_target_of(luil::application_element_kind(11)) };
            check(target == static_cast<luil::text_input_target>(204), "clear field routes to its own text target");
            check(!policy.text_target_of(luil::application_element_kind(9)), "color picker is not a shortcut field");
            luil::text_edit_request request {};
            request.target = *target;
            request.command = luil::text::text_edit_command::replace_all;
            request.text = u8"Ctrl+Alt+R";
            request.offset = request.text.size();
            check(editor.handle(luil::app_message { shortcut_edit_intent { request } }), "clear field accepts edits");
            auto expected { default_hotkeys };
            expected[3] = *parse_hotkey(request.text);
            check(editor.bindings() == expected, "clear shortcut can be customized independently");
            request.text.clear();
            request.offset = 0;
            static_cast<void>(editor.handle(luil::app_message { shortcut_edit_intent { request } }));
            expected[3] = {};
            check(editor.bindings() == expected, "clear shortcut can be disabled");
            static_cast<void>(editor.handle(luil::app_message { reset_shortcuts_intent {} }));
            check(editor.bindings() == default_hotkeys, "reset restores the clear shortcut default");
        }

        void test_recording_export_ui()
        {
            check(
                recording_time(0) == u8"00:00" && recording_time(65) == u8"01:05"
                    && recording_time(3605) == u8"01:00:05",
                "recording time formats minute and hour boundaries"
            );
            command_driver driver {};
            runtime_state state {};
            state.options_open = true;
            driver.handle(luil::app_message { window_metrics_intent { 400, 778, 1 } });
            driver.handle(luil::app_message { state });

            const auto idle { frame_for(driver) };
            state.recording = true;
            state.elapsed_seconds = 65;
            driver.handle(luil::app_message { state });

            const auto active { frame_for(driver) };

            for (const auto theme : { luil::color_theme::light, luil::color_theme::dark })
            {
                auto expected { idle->style->neutral_for(theme) };
                const auto actual { active->style->neutral_for(theme) };
                check(
                    actual.caption.background != expected.caption.background,
                    "recording keeps a distinct caption color in both themes"
                );
                expected.caption.background = actual.caption.background;
                check(actual == expected, "recording preserves settings and button palette colors in both themes");
            }

            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                driver.handle(luil::app_message { window_metrics_intent { 400 * scale, 778 * scale, scale } });

                const auto scaled { frame_for(driver) };
                const auto* background { scaled->tree->find({ command_kind, u8"recording-background" }) };
                check(
                    background && background->bounds().y > 0
                        && approximately_equal(
                            background->bounds().y + background->bounds().height, command_window_height * scale
                        ),
                    "recording background ends above settings at every DPI"
                );
            }

            state.recording = false;
            state.pending_recording = true;
            state.recorded_region = { 0, 0, 160, 120 };
            state.recorded_duration = 1.2;
            driver.handle(luil::app_message { state });

            auto frame { frame_for(driver) };
            check(
                !frame->tree->find({ command_kind, u8"recording-background" }),
                "stopping recording removes toolbar tint"
            );

            const auto* shoot { frame->tree->find({ command_kind, u8"take-screenshot" }) };
            const auto* fps { frame->tree->find({ luil::ui_element_kind::dropdown, u8"fps" }) };
            check(
                shoot && !shoot->enabled() && fps && !fps->enabled(),
                "pending draft blocks new capture and keeps collected FPS locked"
            );

            const auto* scale { frame->tree->find({ command_kind, u8"scale" }) };
            check(scale && scale->enabled(), "pending draft allows export scale changes");
            driver.handle(luil::app_message { options_tab_intent { options_page::output } });
            frame = frame_for(driver);

            const auto* save { frame->tree->find({ command_kind, u8"save-recording" }) };
            const auto* quality { frame->tree->find({ command_kind, u8"webp-quality" }) };
            check(
                save && save->enabled() && quality && !quality->enabled(),
                "save tab exposes explicit save and disables lossy quality for lossless mode"
            );
            check(
                !frame->tree->find({ luil::ui_element_kind::check, u8"use-ffmpeg" })
                    && !frame->tree->find({ command_kind, u8"download-ffmpeg" }),
                "save tab no longer requires external encoder controls"
            );
            state.settings.capture.webp_lossless = false;
            driver.handle(luil::app_message { state });
            frame = frame_for(driver);
            quality = frame->tree->find({ command_kind, u8"webp-quality" });
            check(quality && quality->enabled(), "lossy WebP enables quality slider");
            state.busy = true;
            driver.handle(luil::app_message { state });
            frame = frame_for(driver);
            save = frame->tree->find({ command_kind, u8"save-recording" });
            check(save && !save->enabled(), "encoding locks duplicate saves");
        }

        void test_message_dialog_ui()
        {
            command_driver driver {};
            runtime_state state {};
            state.dialog = { message_dialog_kind::information, 7, u8"저장 안내", std::u8string(1200, u8'A') };
            driver.handle(luil::app_message { state });

            auto frame { frame_for(driver) };
            check(
                frame->popups.size() == 1 && frame->popups.front().id == u8"message-dialog",
                "information popup is published as a luil surface"
            );

            if (frame->popups.size() != 1)
            {
                return;
            }

            const auto& popup { frame->popups.front() };
            const auto* body { popup.tree->find({ luil::application_element_kind(10), u8"message-body" }) };
            check(
                body && body->accessibility().name == state.dialog.body && popup.height < 400,
                "long messages wrap with bounded height and full accessible text"
            );
            check(
                popup.tree->root()->accessibility().role == luil::access_role::dialog,
                "message host exposes dialog role"
            );

            const auto* pin { frame->tree->find({ command_kind, u8"toggle-topmost" }) };
            check(pin && !pin->enabled(), "main commands are blocked while a message is open");
            state.dialog.kind = message_dialog_kind::discard_recording;
            driver.handle(luil::app_message { state });
            frame = frame_for(driver);
            check(
                frame->popups.front().tree->find({ luil::application_element_kind(10), u8"message-cancel" }) != nullptr,
                "destructive confirmation offers cancel"
            );
            state.dialog = {};
            driver.handle(luil::app_message { state });
            frame = frame_for(driver);
            check(frame->popups.empty(), "dismissing message removes the surface");
        }

        void test_image_file_drop()
        {
            command_driver driver {};
            runtime_state state {};
            state.options_open = true;
            driver.handle(luil::app_message { state });
            driver.handle(luil::app_message { window_metrics_intent { 400, 810, 1 } });

            luil::drag_payload payload {};
            payload.files = { u8"E:\\그림 폴더\\예제.WEBP" };

            for (const auto page :
                 { options_page::capture, options_page::region, options_page::output, options_page::preferences })
            {
                driver.handle(luil::app_message { options_tab_intent { page } });
                const auto frame { frame_for(driver) };
                const auto* drop { frame->tree->find_drop_target(8, 800, payload) };
                check(drop != nullptr, "every settings page accepts an image anywhere in its content");

                if (drop != nullptr)
                {
                    const auto actions { drop->drop()->on_drop(payload, {}) };
                    const auto* command { actions.size() == 1 ? std::get_if<luil::app_ui_command>(&actions.front())
                                                              : nullptr
                    };
                    check(
                        command && command->command == static_cast<std::uint32_t>(runtime_command::open_animation)
                            && command->argument == payload.files.front(),
                        "drop opens the exact Unicode path without a picker"
                    );
                }

                const auto* button { frame->tree->find({ command_kind, u8"open-options" }) };
                const auto bounds { button->bounds() };
                check(
                    frame->tree->hit_test(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2) == button,
                    "drop overlay does not intercept ordinary toolbar clicks"
                );
            }

            const auto target { image_file_drop_target() };
            for (const auto* path : { u8"C:\\a.gif", u8"C:\\a.PNG", u8"C:\\a.ApNg", u8"C:\\a.webp" })
            {
                payload.files = { path };
                check(target.accepts(payload), "supported image extensions accept mixed case");
            }
            for (const auto* path : { u8"", u8"C:\\a.txt", u8"C:\\a.jpg", u8"C:\\a.png\\folder", u8"C:\\a" })
            {
                payload.files = { path };
                check(
                    !target.accepts(payload) && target.on_drop(payload, {}).empty(),
                    "unsupported paths cannot dispatch open"
                );
            }
            payload.files = { u8"C:\\a.png", u8"C:\\b.webp" };
            check(!target.accepts(payload), "multiple files are rejected rather than silently choosing one");
            payload.files.clear();
            check(!target.accepts(payload), "internal drags cannot masquerade as image file drops");

            payload.files = { u8"C:\\a.png" };
            // OS 파일 진입·이동·이탈 이벤트로 안내의 수명과 DPI별 그리기를 확인한다.
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                driver.handle(luil::app_message { window_metrics_intent { 400 * scale, 810 * scale, scale } });
                const auto frame { frame_for(driver) };
                luil::interaction_controller interaction {};
                interaction.set_tree(frame->tree);
                const auto* overlay {
                    frame->tree->find({ luil::application_element_kind(40), u8"settings-image-drop" })
                };
                SkBitmap bitmap {};
                check(bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(1, 1)), "drop rendering buffer allocates");
                SkCanvas canvas { bitmap };
                const auto box { overlay->bounds() };
                canvas.translate(-box.x - 4 * scale, -box.y - 4 * scale);
                luil::ui_color_palette palette {};
                palette.surface_background = SK_ColorBLACK;
                luil::draw_context context { .canvas = canvas, .palette = palette, .scale = scale };
                const auto rendered { [&] {
                    bitmap.eraseColor(SK_ColorWHITE);
                    overlay->draw(context, interaction.snapshot());
                    return bitmap.getColor(0, 0);
                }
                };
                check(rendered() == SK_ColorWHITE, "idle drop overlay leaves the settings visible");
                static_cast<void>(
                    interaction.process(luil::file_drag_entered_event { 40 * scale, 200 * scale, payload.files, {} })
                );
                check(rendered() == SK_ColorBLACK, "accepted file drag paints the editing hint");
                static_cast<void>(interaction.process(luil::file_drag_moved_event { -10, -10, {} }));
                check(rendered() == SK_ColorWHITE, "dragging outside clears the hint");
                static_cast<void>(interaction.process(luil::file_drag_left_event {}));
                check(
                    !interaction.snapshot().drag && rendered() == SK_ColorWHITE, "leaving or dropping removes the hint"
                );
                static_cast<void>(interaction.process(
                    luil::file_drag_entered_event { 40 * scale, 200 * scale, { u8"C:\\a.txt" }, {} }
                ));
                check(rendered() == SK_ColorWHITE, "unsupported files never advertise an accepted drop");
            }
            driver.handle(luil::app_message { window_metrics_intent { 400, 810, 1 } });
            for (const bool recording : { false, true })
            {
                state.busy = !recording;
                state.recording = recording;
                driver.handle(luil::app_message { state });
                check(
                    frame_for(driver)->tree->find_drop_target(8, 800, payload) == nullptr,
                    "saving and recording block image drops"
                );
            }

            animation_editor editor {};
            static_cast<void>(editor.handle(luil::app_message { open_animation_editor_intent {} }));
            const auto welcome { editor.make_window() };
            const luil::ui_element* browse {};
            for (const auto& child : welcome.tree->root()->children())
            {
                if (child->id().owner == u8"edit-browse")
                {
                    browse = child.get();
                }
            }
            check(
                browse && browse->accessibility().name == u8"이미지 열기", "empty editor offers a file picker button"
            );
            if (browse)
            {
                const auto actions { (*browse->action(luil::ui_trigger::left_click))({}) };
                const auto* command { std::get_if<luil::app_ui_command>(&actions.front()) };
                check(
                    command && command->command == static_cast<std::uint32_t>(runtime_command::open_animation)
                        && command->argument.empty(),
                    "welcome browse opens the native picker"
                );
            }
            check(
                welcome.tree->find_drop_target(40, 200, payload) != nullptr, "empty editor accepts the same image drop"
            );
        }

        void test_recording_button_help()
        {
            command_driver driver {};
            runtime_state state {};
            state.options_open = true;
            state.pending_recording = true;
            driver.handle(luil::app_message { state });
            driver.handle(luil::app_message { options_tab_intent { options_page::output } });

            for (const auto* const owner :
                 { u8"save-recording", u8"discard-recording", u8"edit-recording", u8"open-animation" })
            {
                send_target(driver, target(owner));

                const auto frame { frame_for(driver) };
                const auto* const popup { help_popup(*frame) };
                check(popup != nullptr, "recording and editor buttons publish custom help");

                if (popup != nullptr)
                {
                    const auto body { help_name(*popup, u8"help-body") };
                    check(
                        !body.empty() && body.find(u8"프리셋") == std::u8string::npos,
                        "recording and editor help does not fall back to region presets"
                    );
                }
            }
        }

        void test_saved_file_ui()
        {
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                command_driver driver {};
                runtime_state state {};
                state.options_open = true;
                driver.handle(luil::app_message { state });
                driver.handle(
                    luil::app_message {
                        window_metrics_intent { options_window_width * scale, options_window_height * scale, scale }
                    }
                );
                check(
                    !frame_for(driver)->tree->find({ command_kind, u8"saved-file" }),
                    "file link appears only after an actual saved output exists"
                );
                state.pending_recording = true;
                state.recorded_region = { 0, 0, 852, 735 };
                state.recorded_duration = 4;
                state.saved_summary = u8"마지막 저장 · 1.36 MiB";
                state.saved_output = L"E:/긴 한글 폴더/파일 이름에 공백이 포함된 캡쳐 결과.webp";
                driver.handle(luil::app_message { state });
                driver.handle(luil::app_message { options_tab_intent { options_page::output } });

                const auto frame { frame_for(driver) };
                const auto* card { frame->tree->find({ command_kind, u8"draft-card" }) };
                const auto* summary { frame->tree->find({ command_kind, u8"draft-saved" }) };
                check(
                    card && summary && summary->bounds().x - card->bounds().x >= 16 * scale
                        && card->bounds().y + card->bounds().height - summary->bounds().y - summary->bounds().height
                            >= 12 * scale,
                    "last saved summary has horizontal and bottom padding inside its card at every DPI"
                );

                const auto* file { frame->tree->find({ command_kind, u8"saved-file" }) };
                const auto* folder { frame->tree->find({ command_kind, u8"output-folder" }) };
                check(
                    file && file->accessibility().role == luil::access_role::link
                        && file->accessibility().name == state.saved_output.filename().u8string(),
                    "saved file link exposes its entire Unicode filename and hyperlink role"
                );
                check(
                    folder && folder->accessibility().name == u8"저장 폴더 열기",
                    "folder icon has an accessible action name"
                );

                if (file && folder)
                {
                    check(
                        file->bounds().x + file->bounds().width + 12 * scale <= folder->bounds().x
                            && folder->bounds().y + folder->bounds().height + 12 * scale
                                <= options_window_height * scale,
                        "file link and folder icon have separate hit targets with bottom padding"
                    );

                    for (const auto* element : { file, folder })
                    {
                        const auto* action { element->action(luil::ui_trigger::left_click) };
                        const auto actions { action ? (*action)({ .element = element->id() })
                                                    : std::vector<luil::input_action> {}
                        };

                        const auto* command { actions.size() == 1 ? std::get_if<luil::app_ui_command>(&actions.front())
                                                                  : nullptr
                        };

                        const auto expected { element == file ? runtime_command::open_saved_file
                                                              : runtime_command::open_output_folder
                        };
                        check(
                            command && command->command == static_cast<std::uint32_t>(expected)
                                && command->argument.empty(),
                            "file and folder actions target distinct app-owned paths"
                        );
                    }
                }

                send_target(driver, target(u8"saved-file"));

                const auto help { frame_for(driver) };
                const auto* popup { help_popup(*help) };
                check(
                    popup
                        && help_name(*popup, u8"help-body").find(state.saved_output.u8string()) != std::u8string::npos,
                    "file link help retains the full path even if the filename is elided"
                );
                state.dialog = { message_dialog_kind::information, 7, u8"안내", u8"테스트" };
                driver.handle(luil::app_message { state });

                const auto blocked { frame_for(driver) };
                check(
                    !blocked->tree->find({ command_kind, u8"saved-file" })->enabled()
                        && !blocked->tree->find({ command_kind, u8"output-folder" })->enabled(),
                    "message dialog blocks both external opening actions"
                );
            }
        }

        void test_save_progress_ui()
        {
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                for (const bool expanded : { false, true })
                {
                    command_driver driver {};
                    runtime_state state {};
                    state.busy = true;
                    state.options_open = expanded;
                    state.progress = { save_stage::encoding, 37, 100 };
                    driver.handle(luil::app_message { state });
                    driver.handle(
                        luil::app_message {
                            window_metrics_intent { command_window_width * scale,
                                                    (expanded ? options_window_height : command_window_height) * scale,
                                                    scale
                            }
                        }
                    );

                    auto frame { frame_for(driver) };
                    const auto* progress { frame->tree->find({ command_kind, u8"save-progress" }) };
                    const auto* label { frame->tree->find({ command_kind, u8"save-progress-label" }) };
                    check(
                        progress && progress->accessibility().role == luil::access_role::progress
                            && progress->accessibility().range
                            && approximately_equal(progress->accessibility().range->value, 0.37f),
                        "save progress exposes its measured read-only range"
                    );
                    check(
                        label && label->accessibility().name == u8"프레임 압축 · 37%",
                        "collapsed and expanded windows show the active saving phase"
                    );

                    if (progress)
                    {
                        check(
                            progress->bounds().y + progress->bounds().height <= command_window_height * scale,
                            "saving progress stays visible above scrolling options at every DPI"
                        );
                    }

                    check(
                        frame->tree->find({ command_kind, u8"select-region" }) == nullptr,
                        "busy toolbar gives its space to progress"
                    );
                    state.progress = { save_stage::assembling };
                    driver.handle(luil::app_message { state });
                    frame = frame_for(driver);
                    check(
                        frame->tree->find({ command_kind, u8"save-progress" }) == nullptr
                            && frame->tree->find({ command_kind, u8"save-progress-detail" }) != nullptr,
                        "unknown assembly work displays activity instead of a false percentage"
                    );
                    state.busy = false;
                    driver.handle(luil::app_message { state });
                    frame = frame_for(driver);
                    check(
                        frame->tree->find({ command_kind, u8"save-progress-label" }) == nullptr
                            && frame->tree->find({ command_kind, u8"select-region" }) != nullptr,
                        "completed work restores the toolbar without stale progress"
                    );
                }
            }
        }

        enum class preview_kind
        {
            progress,
            recording,
            saved_file,
            editor,
        };

        class progress_preview_delegate final : public luil::win32::window_delegate
        {
        public:
            explicit progress_preview_delegate(const preview_kind kind)
                : kind_ { kind }
            {}

            void on_started(luil::app_host& host) override
            {
                runtime_state state {};

                if (kind_ == preview_kind::editor)
                {
                    wchar_t path[32768] {};
                    const auto size { GetEnvironmentVariableW(L"CHALKAG_TEST_EDITOR_PREVIEW", path, 32768) };
                    if (size > 0 && size < 32768)
                    {
                        const std::filesystem::path file { path };
                        const auto output { file.parent_path() / L"exports" };
                        std::filesystem::create_directories(output);
                        state.settings.appearance.theme = luil::theme_preference::dark;
                        host.post_app_message(luil::app_message { state });
                        host.post_app_message(
                            luil::app_message { open_animation_editor_intent { {}, file, output, {} } }
                        );
                    }
                    return;
                }

                if (kind_ == preview_kind::saved_file)
                {
                    state.options_open = true;
                    state.pending_recording = true;
                    state.recorded_region = { 0, 0, 852, 735 };
                    state.recording_saved = true;
                    state.recorded_duration = 4;
                    state.saved_summary = u8"마지막 저장 · 1.36 MiB";
                    state.saved_output = L"E:/캡쳐 저장 폴더/capture-20261008-210000.webp";
                    state.status = u8"저장 완료 · 다시 저장하면 새 파일을 만듭니다";
                    state.settings.appearance.theme = luil::theme_preference::light;
                }
                else if (kind_ == preview_kind::recording)
                {
                    state.recording = true;
                    state.options_open = true;
                    state.elapsed_seconds = 65;
                    state.settings.appearance.theme = luil::theme_preference::light;
                }
                else
                {
                    state.busy = true;
                    state.progress = { save_stage::encoding, 37, 100 };
                }

                if (kind_ == preview_kind::saved_file)
                {
                    host.post_app_message(
                        luil::app_message { region_state_intent { .region = state.recorded_region } }
                    );
                }

                host.post_app_message(luil::app_message { state });

                if (kind_ == preview_kind::saved_file)
                {
                    host.post_app_message(luil::app_message { options_tab_intent { options_page::output } });
                }
            }

            [[nodiscard]] luil::app_message make_window_metrics_message(
                const float width, const float height, const float scale
            ) override
            {
                return luil::app_message { window_metrics_intent { width, height, scale } };
            }

        private:
            preview_kind kind_ {};
        };

        int run_progress_preview(const preview_kind kind = preview_kind::progress)
        {
            // 실제 캡쳐 없이 동일한 UI tree를 띄워 UI Automation과 렌더링을 검증한다.
            luil::win32::enable_per_monitor_dpi_awareness();

            const luil::win32::com_sta_scope com {};
            luil::win32::window_config config {};
            config.class_name = L"Chalkag.Progress.Preview";
            config.caption = make_command_caption();
            config.initial_width = kind == preview_kind::saved_file ? options_window_width : command_window_width;
            config.initial_height = kind == preview_kind::progress ? command_window_height : options_window_height;
            config.minimum_client_width = command_window_width;
            config.minimum_client_height = command_window_height;
            config.renderer = luil::renderer_mode::cpu;
            command_driver driver {};
            progress_preview_delegate delegate { kind };
            command_policy policy {};
            luil::win32::window_environment environment {};
            environment.driver = &driver;
            environment.delegate = &delegate;
            environment.policy = &policy;
            return luil::win32::run_application_window(config, environment);
        }

        int run_tests()
        {
            command_driver preferences {};
            runtime_state settings {};
            settings.options_open = true;
            preferences.handle(luil::app_message { settings });
            preferences.handle(luil::app_message { window_metrics_intent { 400, 778, 1 } });
            preferences.handle(luil::app_message { options_tab_intent { options_page::preferences } });

            const auto page { frame_for(preferences) };
            const auto* input { page->tree->find({ luil::application_element_kind(6), u8"shortcut-select" }) };
            check(
                input != nullptr && approximately_equal(input->bounds().y, 620),
                "shortcut input follows theme preview cards in scroll content"
            );
            check(
                page->tree->find({ command_kind, u8"apply-shortcuts" }) != nullptr,
                "preferences expose shortcut apply action"
            );
            const auto* clear_shortcut {
                page->tree->find({ luil::application_element_kind(11), u8"shortcut-discard" })
            };
            const auto* apply_shortcuts { page->tree->find({ command_kind, u8"apply-shortcuts" }) };
            const auto* shortcut_help { page->tree->find({ command_kind, u8"shortcut-help" }) };
            const auto* status { page->tree->find({ command_kind, u8"runtime-status" }) };
            check(
                clear_shortcut && clear_shortcut->text_input()->text == u8"Ctrl+Shift+R"
                    && clear_shortcut->bounds().y + clear_shortcut->bounds().height < apply_shortcuts->bounds().y
                    && shortcut_help->bounds().y + shortcut_help->bounds().height <= status->bounds().y,
                "fourth shortcut field fits above actions and help stays above the status footer"
            );

            const auto* colors { page->tree->find({ luil::ui_element_kind::choice_group, u8"accent-picker" }) };
            check(
                colors != nullptr && colors->accessibility().role == luil::access_role::radio_group,
                "color picker exposes a selection group"
            );

            if (colors != nullptr)
            {
                std::size_t selected {};

                for (const auto& child : colors->children())
                {
                    selected += child->accessibility().selected.value_or(false) ? 1 : 0;

                    const auto box { child->bounds() };
                    const auto parent { colors->bounds() };
                    check(
                        box.x >= parent.x && box.y >= parent.y && box.x + box.width <= parent.x + parent.width + 0.01f
                            && box.y + box.height <= parent.y + parent.height + 0.01f,
                        "color swatches fit within picker bounds"
                    );
                }

                check(selected == 1, "color picker has exactly one selected swatch");
            }

            test_message_dialog_ui();
            test_image_file_drop();
            test_recording_button_help();
            test_saved_file_ui();
            test_save_progress_ui();
            test_clear_recording_ui();
            test_clear_shortcut_editor();
            test_recording_export_ui();
            test_content_and_disabled_reason();
            test_popup_lifetime();
            test_suppression();
            test_options_and_long_status();
            test_options_menu_and_scroll();
            test_popup_placement_and_guards();
            test_caption_button_hit();
            test_editor_lifetime();
            return failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

    } // namespace
} // namespace chalkag

int main(const int count, const char* arguments[])
{
    try
    {
        if ((count == 2 && std::string_view { arguments[1] } == "--progress-preview")
            || GetEnvironmentVariableW(L"CHALKAG_TEST_PROGRESS_PREVIEW", nullptr, 0) > 0)
        {
            return chalkag::run_progress_preview();
        }

        if ((count == 2 && std::string_view { arguments[1] } == "--recording-preview")
            || GetEnvironmentVariableW(L"CHALKAG_TEST_RECORDING_PREVIEW", nullptr, 0) > 0)
        {
            return chalkag::run_progress_preview(chalkag::preview_kind::recording);
        }

        if ((count == 2 && std::string_view { arguments[1] } == "--saved-file-preview")
            || GetEnvironmentVariableW(L"CHALKAG_TEST_SAVED_FILE_PREVIEW", nullptr, 0) > 0)
        {
            return chalkag::run_progress_preview(chalkag::preview_kind::saved_file);
        }

        if (GetEnvironmentVariableW(L"CHALKAG_TEST_EDITOR_PREVIEW", nullptr, 0) > 0)
        {
            return chalkag::run_progress_preview(chalkag::preview_kind::editor);
        }

        const int result { chalkag::run_tests() };

        if (result == EXIT_SUCCESS)
        {
            std::cout << "Command help tests passed.\n";
        }

        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
