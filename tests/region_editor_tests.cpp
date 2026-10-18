#include "app/app_runtime.h"
#include "app/region_editor.h"

#include "luil/ui/dialog_elements.h"
#include "luil/ui/ui_tree.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace chalkag {
    namespace {
        constexpr luil::ui_element_kind editor_kind { luil::application_element_kind(5) };
        constexpr capture_region screen { -1920, -1080, 3840, 2160 };
        constexpr capture_region initial { -100, -50, 640, 360 };
        constexpr std::array field_owners { u8"region-x", u8"region-y", u8"region-width", u8"region-height" };
        int failure_count {};

        void check(const bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failure_count;
            }
        }

        void open(region_editor& editor, const capture_region region = initial, const capture_region bounds = screen)
        {
            check(
                editor.handle(luil::app_message { open_region_editor_intent { region, bounds } }),
                "editor handles opening intent"
            );
        }

        luil::ui_element_id field_id(const std::size_t index)
        {
            return { luil::application_element_kind(static_cast<std::uint32_t>(index) + 1), field_owners[index] };
        }

        luil::text_input_target field_target(const std::size_t index)
        {
            return static_cast<luil::text_input_target>(101 + index);
        }

        const luil::ui_element& find(const luil::ui_popup& popup, const luil::ui_element_id& id)
        {
            const auto* const element { popup.tree != nullptr ? popup.tree->find(id) : nullptr };

            if (element == nullptr)
            {
                throw std::runtime_error { "Expected editor element is missing." };
            }

            return *element;
        }

        const luil::ui_element& find(const luil::ui_popup& popup, const char8_t* owner)
        {
            return find(popup, luil::ui_element_id { editor_kind, owner });
        }

        std::vector<luil::input_action> invoke(const luil::ui_element& element)
        {
            const auto* const action { element.action(luil::ui_trigger::left_click) };
            return action != nullptr ? (*action)({ .element = element.id() }) : std::vector<luil::input_action> {};
        }

        void apply_actions(region_editor& editor, const std::vector<luil::input_action>& actions)
        {
            for (const auto& action : actions)
            {
                if (const auto* const message { std::get_if<luil::app_message>(&action) }; message != nullptr)
                {
                    check(editor.handle(*message), "editor consumes its UI action intent");
                }
            }
        }

        void replace_field(
            region_editor& editor, const std::size_t index, const std::u8string& text, const std::uint64_t sequence = 0
        )
        {
            luil::text_edit_request request {};
            request.target = field_target(index);
            request.command = luil::text::text_edit_command::replace_all;
            request.offset = text.size();
            request.text = text;
            request.sequence = sequence;
            check(
                editor.handle(luil::app_message { region_editor_edit_intent { request } }),
                "editor applies text edit intent"
            );
        }

        capture_region applied_region(const luil::ui_popup& popup)
        {
            const auto actions { invoke(find(popup, u8"apply-region-editor")) };

            if (actions.size() != 1)
            {
                throw std::runtime_error { "Expected one region apply action." };
            }

            const auto* const command { std::get_if<luil::app_ui_command>(&actions.front()) };
            check(
                command != nullptr
                    && command->command == static_cast<std::uint32_t>(runtime_command::apply_region_editor),
                "apply action routes to the region application command"
            );

            const auto decoded { command != nullptr ? decode_region_arguments(command->argument) : std::nullopt };

            if (!decoded)
            {
                throw std::runtime_error { "Apply action does not encode a valid region." };
            }

            return *decoded;
        }

        void test_arguments()
        {
            check(
                decode_region_arguments(encode_region_arguments(initial)) == initial,
                "negative region coordinates round trip"
            );

            const capture_region extremes { std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), 16, 16 };
            check(
                decode_region_arguments(encode_region_arguments(extremes)) == extremes,
                "signed integer extrema round trip without overflow"
            );
            check(
                decode_region_arguments(u8"+1,-0,0016,+16") == capture_region { 1, 0, 16, 16 },
                "strict decimal parser accepts explicit signs and leading zeroes"
            );

            constexpr std::array invalid_arguments {
                u8"",
                u8"1,2,3",
                u8"1,2,3,4,5",
                u8"1,2,3,4,",
                u8",2,3,4",
                u8"1,,3,4",
                u8"1,2,3,",
                u8"1,2,3,4x",
                u8" 1,2,3,4",
                u8"1,2,3,4\n",
                u8"1,2,3,+",
                u8"--1,2,3,4",
                u8"1.0,2,3,4",
                u8"０,2,3,4",
                u8"2147483648,2,3,4",
                u8"-2147483649,2,3,4",
                u8"99999999999999999999999999999999999999,2,3,4",
            };

            for (const auto* const value : invalid_arguments)
            {
                check(!decode_region_arguments(value), "malformed or overflowing region arguments are rejected");
            }

            std::u8string nul { u8"1,2,3,4" };
            nul.push_back(u8'\0');
            check(!decode_region_arguments(nul), "embedded NUL cannot trail a valid command argument");
        }

        void test_validation()
        {
            check(
                validate_region_draft(initial, screen).region == initial,
                "negative coordinates inside virtual desktop are valid"
            );

            const capture_region full { screen };
            check(validate_region_draft(full, screen).region == full, "exact screen boundary is accepted");

            const capture_region corner { 1904, 1064, 16, 16 };
            check(
                validate_region_draft(corner, screen).region == corner, "minimum region can touch bottom-right boundary"
            );

            constexpr std::array invalid_regions {
                capture_region { -100, -50, 15, 360 },
                capture_region { -100, -50, 640, 0 },
                capture_region { -100, -50, -16, 360 },
                capture_region { -1921, 0, 16, 16 },
                capture_region { 0, -1081, 16, 16 },
                capture_region { 1905, 0, 16, 16 },
                capture_region { 0, 1065, 16, 16 },
                capture_region { std::numeric_limits<int>::max(), 0, 16, 16 },
                capture_region { 0, std::numeric_limits<int>::max(), 16, 16 },
            };

            for (const auto region : invalid_regions)
            {
                const auto validation { validate_region_draft(region, screen) };
                check(
                    !validation.region && !validation.error.empty(),
                    "invalid size position or endpoint returns explanatory error"
                );
            }

            const capture_region left_edge { std::numeric_limits<int>::min(), -8, 16, 16 };
            const capture_region left_screen { std::numeric_limits<int>::min(), -8, 1920, 1080 };
            check(
                validate_region_draft(left_edge, left_screen).region == left_edge,
                "minimum signed coordinate remains valid with safe endpoints"
            );
            check(
                !validate_region_draft(initial, capture_region { 1, 0, std::numeric_limits<int>::max(), 1080 }).region,
                "overflowing screen endpoint is rejected safely"
            );
            check(!validate_region_draft(initial, capture_region {}).region, "unknown screen bounds are rejected");
            check(
                !validate_region_draft(initial, capture_region { 0, 0, 15, 15 }).region,
                "screen smaller than minimum region is rejected"
            );

            std::array<std::u8string, 4> draft { u8"-100", u8"-50", u8"640", u8"360" };
            check(validate_region_draft(draft, screen).region == initial, "complete numeric draft validates");

            for (const auto* const value : { u8"", u8"-", u8"+", u8"16a", u8"2147483648" })
            {
                draft[2] = value;

                const auto validation { validate_region_draft(draft, screen) };
                check(
                    !validation.region && !validation.error.empty(),
                    "unfinished or nonnumeric draft returns an error without coercion"
                );
            }
        }

        void test_popup_and_snapshots()
        {
            region_editor editor {};
            check(!editor.opened() && editor.make_popup(1).tree == nullptr, "closed editor publishes no popup");
            open(editor);
            check(editor.opened(), "open intent opens editor");

            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                const auto popup { editor.make_popup(scale) };
                check(
                    popup.id == u8"region-editor" && popup.width == 400 && popup.height <= 360,
                    "editor popup stays compact with stable ID"
                );
                check(
                    popup.tree->duplicate_ids().empty() && popup.tree->unarranged().empty(),
                    "editor tree has unique and arranged elements"
                );
                check(
                    std::abs(popup.tree->root()->bounds().width - 400 * scale) < 0.01f,
                    "editor physical tree scales with DPI"
                );
                check(
                    find(popup, u8"region-editor-title").accessibility().name == u8"캡쳐 영역 설정",
                    "editor publishes accessible title"
                );
                check(
                    find(popup, u8"region-editor-hint").accessibility().name.find(u8"음수 좌표") != std::u8string::npos,
                    "editor explains physical pixels and negative coordinates"
                );

                for (std::size_t index {}; index < 4; ++index)
                {
                    const auto& input { find(popup, field_id(index)) };
                    check(
                        input.accessibility().role == luil::access_role::edit && !input.accessibility().name.empty(),
                        "numeric input has edit role and descriptive accessibility name"
                    );

                    const auto* const typed { dynamic_cast<const luil::text_input_element*>(&input) };
                    check(
                        typed != nullptr && typed->config().input.kind == luil::text_input_kind::number
                            && typed->config().input.signed_number,
                        "numeric inputs request signed number keyboard"
                    );
                }

                check(applied_region(popup) == initial, "unchanged draft applies initial region exactly");
            }

            const auto before { editor.make_popup(1) };
            replace_field(editor, 2, u8"1280");

            const auto after { editor.make_popup(1) };
            check(
                applied_region(before) == initial && applied_region(after) == capture_region { -100, -50, 1280, 360 },
                "published apply action captures immutable validated draft"
            );
            check(
                editor.make_popup(1).tree->find(field_id(2))->accessibility().value == u8"1280",
                "repeated popup generation leaves draft unchanged"
            );

            const auto error_box { find(after, u8"region-editor-error").bounds() };
            replace_field(editor, 0, u8"-");

            const auto incomplete { editor.make_popup(1) };
            check(
                find(incomplete, field_id(0)).accessibility().value == u8"-", "unfinished signed input survives typing"
            );
            check(
                !find(incomplete, u8"apply-region-editor").enabled()
                    && invoke(find(incomplete, u8"apply-region-editor")).empty(),
                "invalid draft cannot issue apply action"
            );

            const auto bad_error_box { find(incomplete, u8"region-editor-error").bounds() };
            check(
                bad_error_box.y == error_box.y && bad_error_box.height == error_box.height,
                "error row keeps fixed geometry when validation changes"
            );
            apply_actions(editor, invoke(find(incomplete, u8"reset-region-editor")));
            check(applied_region(editor.make_popup(1)) == initial, "reset restores current region captured at open");
            apply_actions(editor, invoke(find(editor.make_popup(1), u8"cancel-region-editor")));
            check(!editor.opened(), "cancel button closes editor");
            check(
                !editor.handle(luil::app_message { region_editor_edit_intent {} }),
                "closed editor ignores queued text edits"
            );
        }

        void test_presets()
        {
            region_editor editor {};
            open(editor, { 1800, 1000, 16, 16 });

            const auto popup { editor.make_popup(1) };
            apply_actions(editor, invoke(find(popup, u8"region-size-1920")));
            check(
                applied_region(editor.make_popup(1)) == capture_region { 0, 0, 1920, 1080 },
                "preset keeps desired size and clamps origin within screen"
            );
            open(editor, { 0, 0, 320, 240 }, { 0, 0, 800, 600 });

            const auto small { editor.make_popup(1) };
            check(find(small, u8"region-size-640").enabled(), "640 preset fits small screen");
            check(
                !find(small, u8"region-size-1280").enabled() && !find(small, u8"region-size-1920").enabled(),
                "oversized presets are visibly disabled"
            );
            check(
                editor.handle(luil::app_message { region_editor_preset_intent { 1920, 1080 } }),
                "oversized preset intent is consumed safely"
            );
            check(
                applied_region(editor.make_popup(1)) == capture_region { 0, 0, 320, 240 },
                "oversized preset does not alter draft"
            );
        }

        void test_ime_and_policy()
        {
            region_editor editor {};
            region_editor_policy policy {};
            open(editor);

            for (std::size_t index {}; index < 4; ++index)
            {
                check(
                    policy.text_target_of(field_id(index).kind) == field_target(index),
                    "policy routes each input kind to its distinct text target"
                );
            }

            check(
                !policy.text_target_of(luil::application_element_kind(0)) && !policy.text_target_of(editor_kind),
                "policy does not intercept other app controls"
            );
            luil::text_composition_event event {};
            event.target = field_target(2);
            event.text = u8"6가";
            event.caret = event.text.size();
            event.composing_begin = 1;
            event.composing_end = event.text.size();
            event.composing = true;

            const auto composition_action { policy.make_text_composition_action(event) };
            const auto* const composition_message { std::get_if<luil::app_message>(&composition_action) };
            check(
                composition_message != nullptr && editor.handle(*composition_message),
                "policy delivers composition intent to editor"
            );

            const auto composing { editor.make_popup(1) };
            const auto snapshot { find(composing, field_id(2)).text_input() };
            check(
                snapshot && snapshot->composing && snapshot->text == u8"640" && snapshot->composition_text == u8"6가",
                "IME display is preserved separately from committed numeric draft"
            );
            check(
                !find(composing, u8"apply-region-editor").enabled(),
                "uncommitted composition cannot apply stale numeric value"
            );
            event.composing = false;
            check(
                editor.handle(luil::app_message { region_editor_composition_intent { event } }),
                "composition end clears temporary display"
            );
            replace_field(editor, 2, u8"1280", 77);

            const auto committed { editor.make_popup(1) };
            const auto committed_snapshot { find(committed, field_id(2)).text_input() };
            check(
                committed_snapshot && !committed_snapshot->composing && committed_snapshot->text == u8"1280"
                    && committed_snapshot->applied_sequence == 77,
                "committed IME edit acknowledges sequence and retains new value"
            );
            luil::text_edit_request undo {};
            undo.target = field_target(2);
            undo.command = luil::text::text_edit_command::undo;

            const auto undo_action { policy.make_text_edit_action(undo) };
            const auto* const undo_message { std::get_if<luil::app_message>(&undo_action) };
            check(undo_message != nullptr && editor.handle(*undo_message), "policy preserves editor undo commands");
            check(applied_region(editor.make_popup(1)) == initial, "text editing undo restores previous number");
            check(
                std::holds_alternative<std::monostate>(policy.make_text_edit_action({})),
                "unknown text target produces no editor action"
            );
        }

        void test_keyboard_and_dismiss()
        {
            region_editor editor {};
            region_editor_policy policy {};
            open(editor);

            const auto popup { editor.make_popup(1) };
            const auto* const trap { popup.tree->focus_trap() };
            check(
                trap != nullptr && trap->focus_entry() == field_id(2),
                "editor traps focus and initially selects width input"
            );
            check(
                trap != nullptr
                    && trap->focus_return()
                        == luil::ui_element_id { luil::application_element_kind(0), u8"select-region" },
                "editor records stable focus return command"
            );
            check(
                popup.tree->default_button() != nullptr
                    && popup.tree->default_button()->id().owner == u8"apply-region-editor",
                "apply is the sole default Enter button"
            );
            luil::interaction_controller controller { &policy };
            controller.set_tree(popup.tree);
            check(controller.snapshot().focused == field_id(2), "controller enters width input when editor appears");

            const auto enter_actions { controller.process(luil::key_pressed_event { .key = luil::key_code::enter }) };
            check(
                enter_actions.size() == 1 && std::get_if<luil::app_ui_command>(&enter_actions.front()) != nullptr,
                "Enter in numeric input invokes validated apply command"
            );
            static_cast<void>(controller.process(luil::key_pressed_event { .key = luil::key_code::tab }));
            check(controller.snapshot().focused == field_id(3), "Tab advances from width to height");

            const auto order { popup.tree->focus_order() };

            for (std::size_t index {}; index < order.size(); ++index)
            {
                static_cast<void>(controller.process(luil::key_pressed_event { .key = luil::key_code::tab }));
            }

            check(controller.snapshot().focused == field_id(3), "Tab cycles inside focus trap without escaping editor");

            const auto escape_actions { controller.process(luil::key_pressed_event { .key = luil::key_code::escape }) };
            apply_actions(editor, escape_actions);
            check(!editor.opened(), "Escape routes through editor dismissal and closes it");

            for (const auto reason : { luil::popup_dismiss_reason::activation_changed,
                                       luil::popup_dismiss_reason::surface_moved,
                                       luil::popup_dismiss_reason::surface_resized
            })
            {
                check(
                    std::holds_alternative<std::monostate>(popup.dismiss(reason)),
                    "editor remains open during activation movement and resizing"
                );
            }

            for (const auto reason :
                 { luil::popup_dismiss_reason::pointer_press_outside, luil::popup_dismiss_reason::escape_key })
            {
                const auto action { popup.dismiss(reason) };
                const auto* const message { std::get_if<luil::app_message>(&action) };
                check(
                    message != nullptr && message->get<close_region_editor_intent>() != nullptr,
                    "outside press and Escape dismiss popup through close intent"
                );
            }
        }

        int run_tests()
        {
            test_arguments();
            test_validation();
            test_popup_and_snapshots();
            test_presets();
            test_ime_and_policy();
            test_keyboard_and_dismiss();
            return failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

    } // namespace
} // namespace chalkag

int main()
{
    try
    {
        const int result { chalkag::run_tests() };

        if (result == EXIT_SUCCESS)
        {
            std::cout << "Region editor tests passed.\n";
        }

        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
