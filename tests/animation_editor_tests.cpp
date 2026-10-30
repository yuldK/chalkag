#include "app/animation_editor.h"
#include "capture/native_webp_writer.h"
#include "include/core/SkCanvas.h"

#include <iostream>
#include <thread>

namespace chalkag {
    namespace {
        void require(const bool condition, const char* message)
        {
            if (!condition)
            {
                throw std::runtime_error { message };
            }
        }

        const luil::ui_element* find_owner(const luil::ui_element* element, const std::u8string_view owner)
        {
            if (element->id().owner == owner)
            {
                return element;
            }

            for (const auto& child : element->children())
            {
                if (const auto* found { find_owner(child.get(), owner) })
                {
                    return found;
                }
            }

            return nullptr;
        }

        std::u8string value(animation_editor& editor, const std::u8string_view owner)
        {
            const auto window { editor.make_window() };
            require(
                window.tree && window.tree->duplicate_ids().empty(),
                "editor elements have unique IDs across tool groups"
            );
            require(window.tree->unarranged().empty(), "editor elements are arranged");

            const auto* element { find_owner(window.tree->root(), owner) };
            return element ? element->accessibility().name : std::u8string {};
        }

        void send(animation_editor& editor, const animation_editor_action command, const double number = 0)
        {
            require(editor.handle(luil::app_message { animation_editor_intent { command, number } }), "intent handled");
        }

        SkColor preview_color(animation_editor& editor, const std::u8string_view owner = u8"animation-preview")
        {
            const auto window { editor.make_window() };
            const auto* preview { find_owner(window.tree->root(), owner) };
            require(preview != nullptr, "preview exists");

            SkBitmap bitmap {};
            require(bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(1, 1)), "preview raster");
            bitmap.eraseColor(SK_ColorBLACK);

            SkCanvas canvas { bitmap };
            const auto box { preview->bounds() };
            canvas.translate(-(box.x + box.width / 2), -(box.y + box.height / 2));

            const luil::ui_color_palette palette {};
            luil::draw_context context { .canvas = canvas,
                                         .palette = palette,
                                         .now = std::chrono::steady_clock::now()
            };
            preview->draw(context, {});
            return bitmap.getColor(0, 0);
        }

        void test_playback(const std::filesystem::path& directory, const int count)
        {
            recording_snapshot snapshot {};
            snapshot.region = { 0, 0, 64, 32 };

            for (int index {}; index < count; ++index)
            {
                SkBitmap frame {};
                require(
                    frame.tryAllocPixels(SkImageInfo::Make(64, 32, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)),
                    "playback frame"
                );
                frame.eraseColor(SkColorSetRGB(index, 32, 255));

                const auto path { directory / ("playback-" + std::to_string(index) + ".png") };
                require(encode_native_image(path, frame.pixmap(), image_format::png), "playback fixture");
                snapshot.frames.push_back({ path, 0.02, 0, 0 });
            }

            animation_editor editor {};
            require(
                editor.handle(luil::app_message { open_animation_editor_intent { snapshot, {}, directory, {} } }),
                "open playback"
            );

            auto deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 5 } };

            while (value(editor, u8"edit-summary").empty() || preview_color(editor) != SkColorSetRGB(0, 32, 255))
            {
                require(std::chrono::steady_clock::now() < deadline, "initial playback image loads");
                editor.tick(std::chrono::steady_clock::now());

                std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
            }

            send(editor, animation_editor_action::play);

            const auto start { std::chrono::steady_clock::now() };

            for (int index { 1 }; index < count; ++index)
            {
                const auto frame_time { start + std::chrono::milliseconds { index * 20 + 5 } };
                editor.tick(frame_time);
                require(SkColorGetB(preview_color(editor)) == 255, "pending playback keeps the last decoded image");
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 2 };

                while (preview_color(editor) != SkColorSetRGB(index, 32, 255))
                {
                    require(
                        std::chrono::steady_clock::now() < deadline,
                        "current preview outranks queued thumbnails after cache fills"
                    );
                    editor.tick(frame_time);

                    std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
                }

                const auto source { std::to_string(index) };
                const auto owner { u8"frame-" + std::u8string { source.begin(), source.end() } };
                require(
                    SkColorGetB(preview_color(editor, owner)) == 255,
                    "playback thumbnail shares the current decoded image"
                );
            }

            editor.tick(start + std::chrono::milliseconds { count * 20 + 5 });
            require(SkColorGetB(preview_color(editor)) == 255, "loop transition keeps a decoded image");
            send(editor, animation_editor_action::play);
            send(editor, animation_editor_action::select, 0);
            require(preview_color(editor) == SkColorSetRGB(0, 32, 255), "old preview remains cached after all frames");
            require(SkColorGetB(preview_color(editor, u8"frame-0")) == 255, "old thumbnail remains cached");
            send(editor, animation_editor_action::close);
            require(editor.handle(luil::app_message { open_animation_editor_intent {} }), "reopen cached document");
            require(preview_color(editor) == SK_ColorBLACK, "closing releases cached images before reopen");
            editor.tick(std::chrono::steady_clock::now());
            send(editor, animation_editor_action::close);
            require(
                editor.handle(luil::app_message { open_animation_editor_intent {} }),
                "reopen during pending preview decode"
            );

            const auto completed { std::chrono::steady_clock::now() + std::chrono::seconds { 1 } };

            while (std::chrono::steady_clock::now() < completed)
            {
                editor.tick(std::chrono::steady_clock::now());

                std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
            }

            require(preview_color(editor) == SK_ColorBLACK, "closed session's pending result cannot restore its cache");
        }

        void test_edit_safety(const std::filesystem::path& directory, const recording_snapshot& snapshot)
        {
            animation_editor editor {};
            const open_animation_editor_intent source { snapshot, {}, directory, {} };
            require(editor.handle(luil::app_message { source }), "open safety source");
            const auto deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 5 } };
            while (value(editor, u8"edit-summary").empty())
            {
                require(std::chrono::steady_clock::now() < deadline, "safety source loads");
                editor.tick(std::chrono::steady_clock::now());
                std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
            }
            require(!editor.has_unsaved_changes(), "new source starts clean");
            const luil::text_edit_request request { .target = static_cast<luil::text_input_target>(301),
                                                    .command = luil::text::text_edit_command::replace_all,
                                                    .text = u8"123"
            };
            require(
                editor.handle(luil::app_message { animation_editor_edit_intent { request } }), "edit duration input"
            );
            require(editor.has_pending_inputs() && editor.has_unsaved_changes(), "unapplied input needs protection");
            send(editor, animation_editor_action::apply_inputs);
            require(
                !editor.has_pending_inputs() && editor.has_unsaved_changes(), "Enter applies a valid input as an edit"
            );
            send(editor, animation_editor_action::undo);
            require(!editor.has_unsaved_changes(), "an applied input is one undo step");
            send(editor, animation_editor_action::redo);

            // 받아들일 수 없는 값은 칸에 남기고 이동·저장을 막는다.
            const luil::text_edit_request invalid { .target = static_cast<luil::text_input_target>(301),
                                                    .command = luil::text::text_edit_command::replace_all,
                                                    .text = u8"0"
            };
            require(
                editor.handle(luil::app_message { animation_editor_edit_intent { invalid } }), "edit invalid input"
            );
            require(editor.has_invalid_inputs(), "out of range input is reported as invalid");
            const auto position { value(editor, u8"edit-position") };
            send(editor, animation_editor_action::next);
            require(value(editor, u8"edit-position") == position, "frame navigation keeps an invalid input in place");
            send(editor, animation_editor_action::export_file);
            require(
                value(editor, u8"edit-status").starts_with(u8"표시 시간은 1~600,000ms"),
                "export never ignores an invalid input"
            );
            send(editor, animation_editor_action::reset_inputs);
            require(!editor.has_pending_inputs(), "resetting inputs restores the current edit");

            // 올바른 값은 다음 프레임으로 넘어갈 때 먼저 반영한다.
            const luil::text_edit_request pending { .target = static_cast<luil::text_input_target>(301),
                                                    .command = luil::text::text_edit_command::replace_all,
                                                    .text = u8"150"
            };
            require(editor.handle(luil::app_message { animation_editor_edit_intent { pending } }), "edit next input");
            send(editor, animation_editor_action::next);
            require(
                !editor.has_pending_inputs() && value(editor, u8"edit-position") != position,
                "frame navigation applies a valid input first"
            );
            require(editor.has_unsaved_changes(), "applied edit remains unsaved");
            const auto edited { value(editor, u8"edit-summary") };
            require(editor.handle(luil::app_message { source }), "request source replacement");
            require(editor.confirmation(1).has_value(), "source replacement requires confirmation");
            require(value(editor, u8"edit-summary") == edited, "replacement preserves current edit while pending");
            send(editor, animation_editor_action::keep_editing);
            require(!editor.confirmation(1), "cancel replacement removes confirmation");
            send(editor, animation_editor_action::request_exit);
            require(editor.confirmation(1).has_value(), "application exit protects external edits");
            send(editor, animation_editor_action::keep_editing);
            send(editor, animation_editor_action::export_file);
            const auto saved { std::chrono::steady_clock::now() + std::chrono::seconds { 5 } };
            while (editor.has_unsaved_changes())
            {
                require(std::chrono::steady_clock::now() < saved, "edited export completes");
                editor.tick(std::chrono::steady_clock::now());
                std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
            }
            require(value(editor, u8"edit-status").starts_with(u8"저장 완료"), "successful export clears dirty state");
            send(editor, animation_editor_action::undo);
            require(editor.has_unsaved_changes(), "undo after export is a new unsaved change");
        }

    } // namespace
} // namespace chalkag

int main(const int argc, char** argv)
{
    if (argc != 2)
    {
        return 1;
    }

    try
    {
        const std::filesystem::path directory { argv[1] };
        std::filesystem::create_directories(directory);
        SkBitmap bitmap {};
        chalkag::require(
            bitmap.tryAllocPixels(SkImageInfo::Make(64, 32, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)), "allocate"
        );
        bitmap.eraseColor(SK_ColorBLUE);

        const auto path { directory / "frame.png" };
        chalkag::require(
            chalkag::encode_native_image(path, bitmap.pixmap(), chalkag::image_format::png), "fixture PNG"
        );
        chalkag::recording_snapshot snapshot {};
        snapshot.region = { 0, 0, 64, 32 };

        for (int index {}; index < 24; ++index)
        {
            snapshot.frames.push_back({ path, index == 7 ? 0.5 : 0.1, 0, 0 });
        }

        chalkag::animation_editor editor {};
        chalkag::require(
            editor.handle(luil::app_message { chalkag::open_animation_editor_intent { snapshot, {}, directory, {} } }),
            "open retained source"
        );

        const auto deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 5 } };

        while (chalkag::value(editor, u8"edit-summary").empty())
        {
            chalkag::require(std::chrono::steady_clock::now() < deadline, "load completes");
            editor.tick(std::chrono::steady_clock::now());

            std::this_thread::sleep_for(std::chrono::milliseconds { 5 });
        }

        chalkag::require(
            chalkag::value(editor, u8"edit-summary").starts_with(u8"24 프레임 · 2.800"), "retained frame timing"
        );
        chalkag::send(editor, chalkag::animation_editor_action::reduction_panel);
        chalkag::send(editor, chalkag::animation_editor_action::phase, 1);
        chalkag::require(
            chalkag::value(editor, u8"reduce-summary").starts_with(u8"24 → 12장 · 2.800"),
            "reduction preview keeps timing"
        );
        chalkag::send(editor, chalkag::animation_editor_action::reduce);
        chalkag::send(editor, chalkag::animation_editor_action::reduce);
        chalkag::require(
            chalkag::value(editor, u8"edit-summary").starts_with(u8"6 프레임 · 2.800"), "repeat half reduction"
        );
        chalkag::send(editor, chalkag::animation_editor_action::undo);
        chalkag::send(editor, chalkag::animation_editor_action::undo);
        chalkag::require(
            chalkag::value(editor, u8"edit-summary").starts_with(u8"24 프레임 · 2.800"), "undo restores metadata"
        );
        chalkag::send(editor, chalkag::animation_editor_action::preserve_duration);
        chalkag::require(
            chalkag::value(editor, u8"reduce-summary").starts_with(u8"24 → 12장 · 1.600"), "shorter preview updates"
        );
        chalkag::send(editor, chalkag::animation_editor_action::stride, 4);
        static_cast<void>(chalkag::value(editor, u8"reduce-summary"));
        chalkag::send(editor, chalkag::animation_editor_action::select_all);
        chalkag::send(editor, chalkag::animation_editor_action::erase);
        chalkag::require(
            chalkag::value(editor, u8"edit-status") == u8"최소 한 프레임은 남겨 주세요.", "empty output is prevented"
        );
        chalkag::send(editor, chalkag::animation_editor_action::select, 23);
        chalkag::send(editor, chalkag::animation_editor_action::trim_before);
        chalkag::require(
            chalkag::value(editor, u8"edit-summary").starts_with(u8"1 프레임 · 0.100"),
            "trim includes selected last frame"
        );
        chalkag::send(editor, chalkag::animation_editor_action::undo);
        chalkag::send(editor, chalkag::animation_editor_action::close);
        chalkag::require(!editor.opened(), "close hides editor");
        chalkag::require(editor.handle(luil::app_message { chalkag::open_animation_editor_intent {} }), "reopen");
        chalkag::require(
            chalkag::value(editor, u8"edit-summary").starts_with(u8"24 프레임 · 2.800"),
            "reopen keeps edits and cached timeline"
        );
        chalkag::send(editor, chalkag::animation_editor_action::close);
        chalkag::capture_options changed_options {};

        changed_options.animation_format = chalkag::image_format::apng;
        chalkag::require(
            editor.handle(
                luil::app_message { chalkag::open_animation_editor_intent { {}, {}, directory, changed_options } }
            ),
            "reopen with current save options"
        );

        const auto changed_window { editor.make_window() };
        const auto* selected_format { chalkag::find_owner(changed_window.tree->root(), u8"edit-format/1") };
        chalkag::require(
            selected_format && selected_format->accessibility().selected.value_or(false),
            "reopen reflects current animation format"
        );
        chalkag::require(
            chalkag::value(editor, u8"edit-summary").starts_with(u8"24 프레임 · 2.800"),
            "save option refresh preserves edits"
        );
        chalkag::test_playback(directory, 60);
        chalkag::test_edit_safety(directory, snapshot);
        chalkag::test_playback(directory, 120);

        std::cout << "Animation editor checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
