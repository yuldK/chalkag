#pragma once

#include "capture/animation_document.h"
#include "capture/image_capture.h"
#include "luil/app/app_host.h"
#include "luil/ui/image_element.h"

#include <array>
#include <deque>
#include <future>
#include <map>

namespace chalkag {
    struct open_animation_editor_intent
    {
        recording_snapshot recording {};
        std::filesystem::path path {};
        std::filesystem::path output_directory {};
        capture_options options {};
    };

    enum class animation_editor_action
    {
        close,
        select,
        previous,
        next,
        play,
        select_all,
        range_start,
        range_end,
        erase,
        trim_before,
        trim_after,
        undo,
        redo,
        reduction_panel,
        stride,
        phase,
        preserve_duration,
        reduce,
        apply_delay,
        delay_all,
        apply_crop,
        reset_crop,
        apply_size,
        lock_aspect,
        format,
        export_file,
        cancel,
        preview_output,
        scroll,
        zoom,
        sidebar_scroll
    };

    struct animation_editor_intent
    {
        animation_editor_action action {};
        double value {};
        bool alternate {};
    };

    struct animation_editor_metrics
    {
        float width {}, height {}, scale { 1 };
    };

    struct animation_editor_edit_intent
    {
        luil::text_edit_request request {};
    };

    struct animation_editor_composition_intent
    {
        luil::text_composition_event event {};
    };

    struct animation_crop_intent
    {
        float x {}, y {};
        int stage {};
    };

    class animation_editor
    {
    public:
        animation_editor();
        ~animation_editor();
        [[nodiscard]] bool handle(const luil::app_message& message);
        [[nodiscard]] bool opened() const noexcept;
        [[nodiscard]] bool can_open_image() const noexcept;
        [[nodiscard]] luil::ui_window make_window();
        [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_tick() const;
        void tick(std::chrono::steady_clock::time_point now);
        void cancel() noexcept;

    private:
        struct work_result
        {
            std::shared_ptr<animation_document> document {};
            std::u8string message {};
            std::filesystem::path output {};
        };

        struct image_result
        {
            std::shared_ptr<animation_document> document {};
            std::pair<std::size_t, int> key {};
            std::uint64_t generation {};
            luil::ui_image image {};
            luil::ui_image thumbnail {};
        };

        void commit(animation_edit edit);
        void refresh_fields();
        void refresh_timeline();
        void refresh_reduction();
        void seek(std::size_t index, bool add = false);
        [[nodiscard]] luil::ui_image image(std::size_t source, int maximum);
        void cache_image(std::pair<std::size_t, int> key, luil::ui_image image);
        [[nodiscard]] std::optional<capture_region> crop_draft() const;
        [[nodiscard]] std::optional<int> field_value(std::size_t index) const;

        bool opened_ {};
        animation_editor_metrics metrics_ { 1040, 760, 1 };
        std::shared_ptr<animation_document> document_ {};
        animation_edit edit_ {};
        animation_edit reduced_preview_ {};
        std::vector<std::int64_t> timeline_ {};
        int minimum_delay_ { 1 };
        std::int64_t reduced_duration_ {};
        std::vector<animation_edit> undo_ {}, redo_ {};
        std::vector<bool> selected_ {};
        std::size_t current_ {}, range_anchor_ {};
        std::size_t selected_count_ {};
        std::array<luil::text::text_edit_state, 7> fields_ {};
        std::optional<luil::text_composition_event> composition_ {};
        std::map<std::pair<std::size_t, int>, luil::ui_image> images_ {};
        std::uint64_t image_generation_ {};
        luil::ui_image playback_image_ {};
        std::deque<std::pair<std::size_t, int>> image_requests_ {};
        std::future<image_result> image_work_ {};
        std::optional<std::pair<std::size_t, int>> image_key_ {};
        std::filesystem::path output_directory_ {};
        capture_options options_ {};
        std::u8string title_ {}, pending_title_ {}, status_ {};
        std::future<work_result> work_ {};
        std::atomic<bool> cancelled_ {};
        bool reduction_open_ {}, preserve_duration_ { true }, delay_all_ {}, lock_aspect_ { true }, preview_output_ {};
        int stride_ { 2 }, phase_ {};
        float scroll_ {}, sidebar_scroll_ {}, zoom_ { 1 };
        bool playing_ {};
        std::chrono::steady_clock::time_point playback_start_ {};
        std::int64_t playback_offset_ {};
        float crop_start_x_ {}, crop_start_y_ {};
    };

    class animation_editor_policy final : public luil::interaction_policy
    {
    public:
        [[nodiscard]] std::optional<luil::text_input_target> text_target_of(luil::ui_element_kind kind) const override;
        [[nodiscard]] luil::input_action make_text_edit_action(const luil::text_edit_request& request) const override;
        [[nodiscard]] luil::input_action make_text_composition_action(
            const luil::text_composition_event& event
        ) const override;
    };
} // namespace chalkag
