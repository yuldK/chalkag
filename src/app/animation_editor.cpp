#include "app/animation_editor.h"
#include "app/app_runtime.h"
#include "app/image_drop.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkFont.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkTypeface.h"
#include "luil/ui/check_element.h"
#include "luil/ui/choice_group_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/draw_primitives.h"
#include "luil/ui/label_element.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/root_element.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/slider_element.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace chalkag {
    namespace {
        constexpr auto editor_kind { luil::application_element_kind(20) };
        constexpr std::array field_names { u8"표시 시간 ms", u8"크롭 X",    u8"크롭 Y",   u8"크롭 너비",
                                           u8"크롭 높이",    u8"출력 너비", u8"출력 높이"
        };

        constexpr std::array field_owners { u8"edit-delay",  u8"crop-x",     u8"crop-y",     u8"crop-width",
                                            u8"crop-height", u8"edit-width", u8"edit-height"
        };

        std::u8string number_text(const std::int64_t value)
        {
            const auto text { std::to_string(value) };
            return { text.begin(), text.end() };
        }

        std::u8string seconds_text(const std::int64_t milliseconds)
        {
            const auto seconds { number_text(milliseconds / 1000) };
            const auto fraction { number_text(1000 + milliseconds % 1000).substr(1) };
            return seconds + u8"." + fraction + u8"초";
        }

        luil::input_action make_editor_action(
            const animation_editor_action command, const double value = 0, const bool alternate = false
        )
        {
            return luil::make_app_action(animation_editor_intent { command, value, alternate });
        }

        luil::ui_action button_action(const animation_editor_action command, const double value = 0)
        {
            return luil::make_message_action(animation_editor_intent { command, value });
        }

        class editor_tools_element final : public luil::ui_element
        {
        public:
            editor_tools_element(const float origin, const float scale)
                : ui_element { { editor_kind, u8"editor-tools-content" } }
                , origin_ { origin }
                , scale_ { scale }
            {}

            void add(std::unique_ptr<luil::ui_element> element)
            {
                const auto box { element->bounds() };
                slots_.push_back(
                    { box.x / scale_ - origin_, box.y / scale_ - 120, box.width / scale_, box.height / scale_ }
                );
                add_child(std::move(element));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);

                for (std::size_t index {}; index < children().size(); ++index)
                {
                    const auto& box { slots_[index] };
                    const luil::rect_f slot { context.slot.x + box.x * context.scale,
                                              context.slot.y + box.y * context.scale,
                                              box.width * context.scale,
                                              box.height * context.scale
                    };
                    const auto child_context { context.for_child(slot) };
                    children()[index]->arrange(child_context);
                }
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                draw_children(context, interaction);
            }

        private:
            float origin_ {}, scale_ {};

            std::vector<luil::rect_f> slots_ {};
        };

        class timeline_element final : public luil::ui_element
        {
        public:
            timeline_element(const float offset, const float scale)
                : ui_element { { editor_kind, u8"animation-timeline" } }
            {
                set_clip_children(true);
                luil::scroll_source source {};
                source.scroll
                    = [](const float delta) { return make_editor_action(animation_editor_action::scroll, delta); };
                source.scale = scale;
                source.axis = luil::scroll_axis::horizontal;
                source.at_start = offset <= 0;
                set_scroll_source(std::move(source));
                set_focus_group(luil::focus_axis::horizontal);
            }

            void add(std::unique_ptr<luil::ui_element> element)
            {
                add_child(std::move(element));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const auto box { bounds() };
                context.canvas.save();
                context.canvas.clipRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height));
                draw_children(context, interaction);
                context.canvas.restore();
            }

            luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::list, .name = u8"프레임 타임라인 · 가로 스크롤" };
            }
        };

        class frame_element final : public luil::ui_element
        {
        public:
            frame_element(
                const std::size_t index,
                const edited_frame frame,
                const luil::ui_image& image,
                const bool selected,
                const bool current,
                const std::optional<std::chrono::steady_clock::time_point> started
            )
                : ui_element { { editor_kind, u8"frame-" + number_text(static_cast<std::int64_t>(frame.source)) } }
                , index_ { index }
                , frame_ { frame }
                , image_ { image }
                , selected_ { selected }
                , current_ { current }
                , started_ { started }
            {
                set_tab_stop(true);
                set_cursor(luil::ui_cursor::hand);
                set_action(
                    luil::ui_trigger::left_click,
                    [index](const luil::ui_action_context& context) -> std::vector<luil::input_action> {
                        return { make_editor_action(
                            animation_editor_action::select, static_cast<double>(index), context.control
                        )
                        };
                    }
                );
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const auto box { bounds() };
                auto paint {
                    luil::solid_paint(selected_ ? context.palette.accent_soft : context.palette.surface_background)
                };
                context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), paint);

                const auto scale { context.scale };

                // 긴 표시 시간은 같은 그림을 이어 그려 시간 폭을 그대로 드러낸다.
                const auto clip { context.canvas.getLocalClipBounds() };
                const float tile_start { box.x + 2 * scale };
                const float first_tile {
                    tile_start + std::max(0.0f, std::floor((clip.left() - tile_start) / (80 * scale))) * 80 * scale
                };

                for (float left { first_tile }; left < std::min(clip.right(), box.x + box.width - 2 * scale);
                     left += 80 * scale)
                {
                    const float width { std::min(80 * scale, box.x + box.width - 2 * scale - left) };
                    luil::image_element image { { editor_kind, u8"thumbnail" },
                                                luil::image_config { .image = image_ }
                    };
                    image.arrange({ { left, box.y + 3 * scale, width, 52 * scale }, scale });
                    image.draw(context, interaction);
                }

                SkFont font { sk_ref_sp(context.ui_typeface), 11 * scale };
                paint = luil::solid_paint(context.palette.primary_foreground);
                const auto caption { number_text(static_cast<std::int64_t>(index_ + 1)) + u8" · "
                                     + number_text(frame_.milliseconds) + u8"ms"
                };
                luil::draw_text_within(
                    context.canvas, caption, box.x + 7 * scale, box.y + 74 * scale, box.width - 14 * scale, font, paint
                );
                paint = luil::solid_paint(selected_ ? context.palette.accent : context.palette.divider);
                paint.setStyle(SkPaint::kStroke_Style);
                paint.setStrokeWidth((selected_ ? 2 : 1) * scale);
                context.canvas.drawRect(
                    SkRect::MakeXYWH(box.x + scale, box.y + scale, box.width - 2 * scale, box.height - 2 * scale), paint
                );

                if (current_)
                {
                    double fraction {};

                    if (started_)
                    {
                        const double elapsed_ms {
                            std::chrono::duration<double, std::milli>(context.now - *started_).count()
                        };
                        const double progress { elapsed_ms / frame_.milliseconds };
                        fraction = std::clamp(progress, 0.0, 1.0);
                    }

                    paint = luil::solid_paint(context.palette.accent);
                    paint.setStrokeWidth(2 * scale);

                    const float x { box.x + static_cast<float>(fraction) * box.width };
                    context.canvas.drawLine(x, box.y, x, box.y + box.height, paint);
                }
            }

            std::optional<std::chrono::steady_clock::time_point> next_update(
                const luil::update_context& context, const luil::interaction_snapshot&
            ) const override
            {
                return current_ && started_ ? std::optional { context.now } : std::nullopt;
            }

            luil::access_info accessibility() const override
            {
                luil::access_info info {};
                info.role = luil::access_role::list_item;
                info.name = u8"프레임 " + number_text(static_cast<std::int64_t>(index_ + 1)) + u8" · "
                    + number_text(frame_.milliseconds) + u8"ms";
                info.selected = selected_;
                return info;
            }

            std::optional<std::vector<luil::input_action>> access_actions(
                const luil::access_request& request
            ) const override
            {
                if (request.command != luil::access_command::select && request.command != luil::access_command::invoke)
                {
                    return std::nullopt;
                }

                return std::vector<luil::input_action> {
                    make_editor_action(animation_editor_action::select, static_cast<double>(index_))
                };
            }

        private:
            std::size_t index_ {};
            edited_frame frame_ {};
            luil::ui_image image_ {};
            bool selected_ {}, current_ {};

            std::optional<std::chrono::steady_clock::time_point> started_ {};
        };

        class crop_preview_element final : public luil::ui_element
        {
        public:
            crop_preview_element(
                luil::ui_image image, const capture_region crop, const int width, const int height, const bool output
            )
                : ui_element { { editor_kind, u8"animation-preview" } }
                , image_ { std::move(image) }
                , crop_ { crop }
                , width_ { width }
                , height_ { height }
                , output_ { output }
            {
                if (!output)
                {
                    set_cursor(luil::ui_cursor::grab);
                    luil::pointer_drag_target drag {};
                    drag.on_press = [this](const luil::ui_action_context& context) { return crop_message(context, 0); };
                    drag.on_move = [this](const luil::ui_action_context&, const luil::ui_action_context& context) {
                        return crop_message(context, 1);
                    };
                    drag.on_release = [this](const luil::ui_action_context& context, luil::drag_velocity) {
                        return crop_message(context, 2);
                    };
                    set_pointer_drag_target(std::move(drag));
                }
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                destination_ = luil::image_destination(
                    context.slot,
                    output_ ? crop_.width : width_,
                    output_ ? crop_.height : height_,
                    luil::image_fit::contain
                );
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const auto box { bounds() };
                auto paint { luil::solid_paint(context.palette.surface_background) };
                context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), paint);

                const float cell { 12 * context.scale };

                for (float y { destination_.y }; y < destination_.y + destination_.height; y += cell)
                {
                    for (float x { destination_.x }; x < destination_.x + destination_.width; x += cell)
                    {
                        const auto alternate { (static_cast<int>((x - destination_.x) / cell)
                                                + static_cast<int>((y - destination_.y) / cell))
                                               % 2
                        };
                        paint
                            = luil::solid_paint(alternate ? context.palette.divider : context.palette.input_background);
                        const float cell_width { std::min(cell, destination_.x + destination_.width - x) };
                        const float cell_height { std::min(cell, destination_.y + destination_.height - y) };
                        const auto cell_bounds { SkRect::MakeXYWH(x, y, cell_width, cell_height) };
                        context.canvas.drawRect(cell_bounds, paint);
                    }
                }

                const float ratio_x { static_cast<float>(image_.width()) / width_ };
                const float ratio_y { static_cast<float>(image_.height()) / height_ };
                luil::image_config config { .image = image_ };

                if (output_)
                {
                    config.source
                        = { crop_.x * ratio_x, crop_.y * ratio_y, crop_.width * ratio_x, crop_.height * ratio_y };
                }

                luil::image_element image { { editor_kind, u8"preview-image" }, config };
                image.arrange({ destination_, context.scale });
                image.draw(context, interaction);

                if (!output_)
                {
                    paint = luil::solid_paint(context.palette.accent);
                    paint.setStyle(SkPaint::kStroke_Style);
                    paint.setStrokeWidth(2 * context.scale);
                    // 원본 좌표를 미리보기 배치로 변환한 테두리를 그린다.
                    const auto crop_bounds { SkRect::MakeXYWH(
                        destination_.x + destination_.width * crop_.x / width_,
                        destination_.y + destination_.height * crop_.y / height_,
                        destination_.width * crop_.width / width_,
                        destination_.height * crop_.height / height_
                    )
                    };
                    context.canvas.drawRect(crop_bounds, paint);
                }
            }

            luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::image,
                         .name = output_ ? u8"크롭 결과 미리보기" : u8"원본 미리보기 · 드래그로 크롭 영역 선택"
                };
            }

        private:
            std::vector<luil::input_action> crop_message(const luil::ui_action_context& context, const int stage) const
            {
                if (destination_.width <= 0 || destination_.height <= 0)
                {
                    return {};
                }

                const float x { std::clamp((context.x - destination_.x) / destination_.width, 0.0f, 1.0f) * width_ };
                const float y { std::clamp((context.y - destination_.y) / destination_.height, 0.0f, 1.0f) * height_ };
                return { luil::make_app_action(animation_crop_intent { x, y, stage }) };
            }

            luil::ui_image image_ {};

            capture_region crop_ {};
            int width_ {}, height_ {};
            bool output_ {};
            luil::rect_f destination_ {};
        };

        class timeline_scroll_element final : public luil::ui_element
        {
        public:
            timeline_scroll_element(const float viewport, const float content, const float offset)
                : ui_element { { editor_kind, u8"timeline-scroll" } }
                , viewport_ { viewport }
                , content_ { content }
                , offset_ { offset }
            {
                set_tab_stop(true);
                set_cursor(luil::ui_cursor::grab);
                set_active_cursor(luil::ui_cursor::grabbing);
                luil::pointer_drag_target drag {};
                drag.on_press = [this](const luil::ui_action_context& context) -> std::vector<luil::input_action> {
                    if (context.x >= thumb_x_ && context.x <= thumb_x_ + thumb_width_)
                    {
                        return {};
                    }

                    const float position { (context.x - bounds().x - thumb_width_ / 2)
                                           / std::max(1.0f, bounds().width - thumb_width_) * maximum()
                    };
                    return { make_editor_action(animation_editor_action::scroll, position, true) };
                };
                drag.on_move = [this](
                                   const luil::ui_action_context& previous, const luil::ui_action_context& current
                               ) -> std::vector<luil::input_action> {
                    const float delta { (current.x - previous.x) / std::max(1.0f, bounds().width - thumb_width_)
                                        * maximum()
                    };
                    return { make_editor_action(animation_editor_action::scroll, delta) };
                };
                set_pointer_drag_target(std::move(drag));
                luil::key_step_target steps {};
                steps.axis = luil::focus_axis::horizontal;
                steps.on_step = [this](const luil::value_step step) -> std::optional<std::vector<luil::input_action>> {
                    if (step == luil::value_step::minimum || step == luil::value_step::maximum)
                    {
                        return std::vector<luil::input_action> { make_editor_action(
                            animation_editor_action::scroll, step == luil::value_step::minimum ? 0 : maximum(), true
                        )
                        };
                    }

                    const bool decrease { step == luil::value_step::decrease
                                          || step == luil::value_step::decrease_page
                    };

                    const bool page { step == luil::value_step::decrease_page
                                      || step == luil::value_step::increase_page
                    };
                    return std::vector<luil::input_action> { make_editor_action(
                        animation_editor_action::scroll, (decrease ? -1 : 1) * (page ? viewport_ : 64)
                    )
                    };
                };
                set_key_step_target(std::move(steps));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                thumb_width_ = std::clamp(
                    context.slot.width * viewport_ / std::max(1.0f, content_), 24 * context.scale, context.slot.width
                );
                thumb_x_ = context.slot.x + (context.slot.width - thumb_width_) * offset_ / std::max(1.0f, maximum());
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot&) const override
            {
                const auto box { bounds() };
                const float height { 6 * context.scale };
                const float y { box.y + (box.height - height) / 2 };
                auto paint { luil::solid_paint(context.palette.surface_background) };
                context.canvas.drawRoundRect(
                    SkRect::MakeXYWH(box.x, y, box.width, height), height / 2, height / 2, paint
                );
                paint = luil::solid_paint(context.palette.secondary_foreground);
                context.canvas.drawRoundRect(
                    SkRect::MakeXYWH(thumb_x_, y, thumb_width_, height), height / 2, height / 2, paint
                );
            }

            luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::scroll_bar,
                         .name = u8"타임라인 가로 스크롤 위치",
                         .range = luil::access_range { 0, maximum(), offset_ }
                };
            }

            std::optional<std::vector<luil::input_action>> access_actions(
                const luil::access_request& request
            ) const override
            {
                if (request.command != luil::access_command::set_value)
                {
                    return std::nullopt;
                }

                return std::vector<luil::input_action> {
                    make_editor_action(animation_editor_action::scroll, request.value, true)
                };
            }

        private:
            float maximum() const
            {
                return std::max(0.0f, content_ - viewport_);
            }

            float viewport_ {}, content_ {}, offset_ {}, thumb_width_ {}, thumb_x_ {};
        };

        class timeline_zoom_element final : public luil::ui_element
        {
        public:
            explicit timeline_zoom_element(luil::slider_config config)
                : ui_element { { editor_kind, u8"timeline-zoom" } }
                , slider_ { id(), std::move(config) }
            {
                if (const auto* drag { slider_.pointer_drag() })
                {
                    set_pointer_drag_target(*drag);
                }

                if (const auto* steps { slider_.key_step() })
                {
                    set_key_step_target(*steps);
                }

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

            luil::access_info accessibility() const override
            {
                auto info { slider_.accessibility() };
                info.name = u8"타임라인 확대 배율";
                return info;
            }

            std::optional<std::vector<luil::input_action>> access_actions(
                const luil::access_request& request
            ) const override
            {
                return slider_.access_actions(request);
            }

        private:
            luil::slider_element slider_;
        };
    } // namespace

    animation_editor::animation_editor() = default;

    animation_editor::~animation_editor()
    {
        cancel();

        if (work_.valid())
        {
            work_.wait();
        }

        if (image_work_.valid())
        {
            image_work_.wait();
        }
    }

    void animation_editor::cancel() noexcept
    {
        cancelled_ = true;
    }

    bool animation_editor::opened() const noexcept
    {
        return opened_;
    }

    bool animation_editor::can_open_image() const noexcept
    {
        return !work_.valid();
    }

    std::optional<int> animation_editor::field_value(const std::size_t index) const
    {
        const auto& text { fields_[index].text };

        if (text.empty() || text.size() > 7 || composition_)
        {
            return std::nullopt;
        }

        int value {};

        for (const auto character : text)
        {
            if (character < u8'0' || character > u8'9')
            {
                return std::nullopt;
            }

            value = value * 10 + character - u8'0';
        }

        return value;
    }

    void animation_editor::refresh_fields()
    {
        if (edit_.frames.empty())
        {
            return;
        }

        const std::array values { edit_.frames[current_].milliseconds,
                                  edit_.crop.x,
                                  edit_.crop.y,
                                  edit_.crop.width,
                                  edit_.crop.height,
                                  edit_.output_width,
                                  edit_.output_height
        };

        for (std::size_t index {}; index < values.size(); ++index)
        {
            luil::text::text_edit_set_text(fields_[index], number_text(values[index]), true);
        }

        composition_.reset();
    }

    void animation_editor::seek(const std::size_t index, const bool add)
    {
        if (edit_.frames.empty())
        {
            return;
        }

        playing_ = false;
        current_ = std::min(index, edit_.frames.size() - 1);

        if (!add)
        {
            selected_.assign(edit_.frames.size(), false);
        }

        selected_[current_] = add ? !selected_[current_] : true;
        selected_count_ = add ? (selected_[current_] ? selected_count_ + 1 : selected_count_ - 1) : 1;

        const double unit { 64.0 * zoom_ / minimum_delay_ };
        const double prefix { timeline_[current_] * unit };
        const double viewport { metrics_.width / metrics_.scale - 360 };
        const double end { timeline_[current_ + 1] * unit };

        if (prefix < scroll_ || end - prefix > viewport)
        {
            scroll_ = static_cast<float>(prefix);
        }
        else if (end > scroll_ + viewport)
        {
            scroll_ = static_cast<float>(end - viewport);
        }

        refresh_fields();
    }

    void animation_editor::commit(animation_edit edit)
    {
        if (!document_ || !valid_animation_edit(edit, document_->width(), document_->height()))
        {
            status_ = u8"크롭 범위, 출력 크기 또는 표시 시간을 확인하세요. 출력은 한 변 16,383px, 3,355만 픽셀까지 "
                      u8"지원합니다.";
            return;
        }

        if (edit == edit_)
        {
            return;
        }

        undo_.push_back(edit_);

        std::size_t bytes {};

        for (const auto& previous : undo_)
        {
            bytes += previous.frames.size() * sizeof(edited_frame);
        }
        while (undo_.size() > 1 && (bytes > 32 * 1024 * 1024 || undo_.size() > 64))
        {
            bytes -= undo_.front().frames.size() * sizeof(edited_frame);
            undo_.erase(undo_.begin());
        }

        redo_.clear();
        edit_ = std::move(edit);
        refresh_timeline();
        seek(std::min(current_, edit_.frames.size() - 1));
        status_ = u8"편집을 적용했습니다. 원본은 유지되며 실행 취소할 수 있습니다.";
    }

    void animation_editor::refresh_timeline()
    {
        // 누적 시간을 한 번 준비하면 재생 위치와 보이는 썸네일 범위를 같은 기준으로 찾을 수 있다.
        timeline_.clear();
        timeline_.reserve(edit_.frames.size() + 1);
        timeline_.push_back(0);
        minimum_delay_ = 600000;

        for (const auto& frame : edit_.frames)
        {
            timeline_.push_back(timeline_.back() + frame.milliseconds);
            minimum_delay_ = std::min(minimum_delay_, frame.milliseconds);
        }

        refresh_reduction();
    }

    void animation_editor::refresh_reduction()
    {
        reduced_preview_ = reduce_animation(edit_, stride_, phase_, preserve_duration_);
        reduced_duration_ = animation_duration(reduced_preview_);
    }

    std::optional<capture_region> animation_editor::crop_draft() const
    {
        std::array<int, 4> values {};

        for (std::size_t index {}; index < values.size(); ++index)
        {
            const auto value { field_value(index + 1) };

            if (!value)
            {
                return std::nullopt;
            }

            values[index] = *value;
        }

        const capture_region crop { values[0], values[1], values[2], values[3] };

        if (!document_ || crop.width < 1 || crop.height < 1 || crop.width > document_->width()
            || crop.height > document_->height() || crop.x > document_->width() - crop.width
            || crop.y > document_->height() - crop.height)
        {
            return std::nullopt;
        }

        return crop;
    }

    luil::ui_image animation_editor::image(const std::size_t source, const int maximum)
    {
        if (!opened_)
        {
            return {};
        }

        const auto key { std::pair { source, maximum } };

        if (maximum > 128)
        {
            std::erase_if(image_requests_, [](const auto& request) { return request.second > 128; });
        }

        if (const auto existing { images_.find(key) }; existing != images_.end())
        {
            return existing->second;
        }

        // 재생 중에는 지나간 썸네일보다 현재 화면의 복원을 먼저 끝낸다.
        if (playing_ && maximum <= 128)
        {
            return {};
        }

        if (image_key_ != key
            && std::find(image_requests_.begin(), image_requests_.end(), key) == image_requests_.end())
        {
            if (image_requests_.size() >= 24)
            {
                image_requests_.pop_back();
            }

            if (maximum > 128)
            {
                image_requests_.push_front(key);
            }
            else
            {
                image_requests_.push_back(key);
            }
        }

        return playing_ && maximum > 128 ? playback_image_ : luil::ui_image {};
    }

    void animation_editor::cache_image(const std::pair<std::size_t, int> key, luil::ui_image image)
    {
        images_.insert_or_assign(key, std::move(image));
    }

    bool animation_editor::handle(const luil::app_message& message)
    {
        if (const auto* open { message.get<open_animation_editor_intent>() })
        {
            opened_ = true;

            if (work_.valid())
            {
                return true;
            }

            cancelled_ = false;

            if (!open->output_directory.empty())
            {
                output_directory_ = open->output_directory;
                options_ = open->options;
            }

            if (open->path.empty() && open->recording.frames.empty() && document_)
            {
                return true;
            }

            pending_title_ = open->path.empty() ? u8"보관 중인 녹화" : open->path.filename().u8string();
            playing_ = false;

            if (open->path.empty() && open->recording.frames.empty())
            {
                title_ = u8"애니메이션 편집";
                cancelled_ = false;
                status_ = u8"이미지를 끌어 놓거나 이미지 열기를 누르세요.";
                return true;
            }

            status_ = u8"원본을 여는 중…";
            work_ = std::async(std::launch::async, [open = *open] {
                work_result result {};

                try
                {
                    result.document = open.path.empty() ? animation_document::from_recording(open.recording)
                                                        : animation_document::open(open.path, result.message);
                }
                catch (...)
                {
                    result.message = u8"원본을 여는 중 메모리 오류가 발생했습니다.";
                }

                return result;
            });
            return true;
        }

        if (const auto* metrics { message.get<animation_editor_metrics>() })
        {
            metrics_ = *metrics;
            return true;
        }

        if (const auto* edit { message.get<animation_editor_edit_intent>() })
        {
            const auto target { static_cast<std::uint32_t>(edit->request.target) };

            if (target >= 301 && target <= 307 && !work_.valid())
            {
                luil::apply_text_edit(fields_[target - 301], edit->request);

                if (target == 306 && lock_aspect_ && document_)
                {
                    if (const auto width { field_value(5) }; width && *width <= 16383)
                    {
                        const auto height { static_cast<std::int64_t>(
                            std::llround(static_cast<double>(*width) * edit_.crop.height / edit_.crop.width)
                        )
                        };
                        luil::text::text_edit_set_text(fields_[6], number_text(height), true);
                    }
                }
            }

            return true;
        }

        if (const auto* composition { message.get<animation_editor_composition_intent>() })
        {
            composition_ = composition->event.composing ? std::optional { composition->event } : std::nullopt;
            return true;
        }

        if (const auto* crop { message.get<animation_crop_intent>() })
        {
            if (!document_ || work_.valid() || preview_output_)
            {
                return true;
            }

            playing_ = false;

            if (crop->stage == 0)
            {
                crop_start_x_ = crop->x;
                crop_start_y_ = crop->y;
            }

            const int x { std::clamp(static_cast<int>(std::min(crop_start_x_, crop->x)), 0, document_->width() - 1) };
            const int y { std::clamp(static_cast<int>(std::min(crop_start_y_, crop->y)), 0, document_->height() - 1) };
            const int width {
                std::clamp(static_cast<int>(std::abs(crop->x - crop_start_x_)), 1, document_->width() - x)
            };

            const int height {
                std::clamp(static_cast<int>(std::abs(crop->y - crop_start_y_)), 1, document_->height() - y)
            };

            const std::array values { x, y, width, height };

            for (std::size_t index {}; index < values.size(); ++index)
            {
                luil::text::text_edit_set_text(fields_[index + 1], number_text(values[index]), true);
            }

            if (crop->stage == 2 && (width > 1 || height > 1))
            {
                auto draft { edit_ };
                draft.crop = { x, y, width, height };
                draft.output_width = width;
                draft.output_height = height;
                commit(std::move(draft));
            }

            return true;
        }

        const auto* intent { message.get<animation_editor_intent>() };

        if (!intent)
        {
            return false;
        }

        const auto command { intent->action };

        if (!std::isfinite(intent->value))
        {
            return true;
        }

        if (command == animation_editor_action::close)
        {
            opened_ = false;
            playing_ = false;
            cancel();
            ++image_generation_;
            images_.clear();
            image_requests_.clear();
            playback_image_ = {};
            return true;
        }

        if (command == animation_editor_action::cancel)
        {
            cancel();
            return true;
        }

        if (!document_ || edit_.frames.empty() || work_.valid())
        {
            return true;
        }

        const auto index { static_cast<std::size_t>(std::clamp(intent->value, 0.0, 1000000.0)) };
        animation_edit draft {};

        switch (command)
        {
        case animation_editor_action::select:
            seek(index, intent->alternate);
            break;

        case animation_editor_action::previous:
            seek(current_ > 0 ? current_ - 1 : 0);
            break;

        case animation_editor_action::next:
            seek(current_ + 1);
            break;

        case animation_editor_action::select_all:
            selected_.assign(edit_.frames.size(), true);
            selected_count_ = selected_.size();
            break;

        case animation_editor_action::range_start:
            range_anchor_ = current_;
            status_ = u8"선택 시작을 지정했습니다. 끝 프레임에서 ‘여기까지 선택’을 누르세요.";
            break;

        case animation_editor_action::range_end:
            selected_.assign(edit_.frames.size(), false);

            // 선택 시작점와 현재 프레임 사이를 포함하는 범위를 만든다.
            for (auto position { std::min(current_, range_anchor_) };
                 position <= std::max(current_, std::min(range_anchor_, edit_.frames.size() - 1));
                 ++position)
            {
                selected_[position] = true;
            }

            selected_count_ = std::max(current_, std::min(range_anchor_, edit_.frames.size() - 1))
                - std::min(current_, range_anchor_) + 1;
            break;

        case animation_editor_action::play:
            playing_ = !playing_;
            playback_start_ = std::chrono::steady_clock::now();
            playback_offset_ = 0;

            for (std::size_t position {}; position < current_; ++position)
            {
                playback_offset_ += edit_.frames[position].milliseconds;
            }

            break;

        case animation_editor_action::erase:
            draft = edit_;
            draft.frames.clear();

            for (std::size_t position {}; position < edit_.frames.size(); ++position)
            {
                if (!selected_[position])
                {
                    draft.frames.push_back(edit_.frames[position]);
                }
            }

            if (draft.frames.empty())
            {
                status_ = u8"최소 한 프레임은 남겨 주세요.";
            }
            else
            {
                commit(std::move(draft));
            }

            break;

        case animation_editor_action::trim_before:
            draft = edit_;
            draft.frames.erase(draft.frames.begin(), draft.frames.begin() + static_cast<std::ptrdiff_t>(current_));
            current_ = 0;
            commit(std::move(draft));
            break;

        case animation_editor_action::trim_after:
            draft = edit_;
            draft.frames.resize(current_ + 1);
            commit(std::move(draft));
            break;

        case animation_editor_action::undo:

        case animation_editor_action::redo: {
            auto& from { command == animation_editor_action::undo ? undo_ : redo_ };
            auto& to { command == animation_editor_action::undo ? redo_ : undo_ };

            if (!from.empty())
            {
                to.push_back(edit_);
                edit_ = std::move(from.back());
                from.pop_back();
                refresh_timeline();
                seek(std::min(current_, edit_.frames.size() - 1));
                status_ = command == animation_editor_action::undo ? u8"실행 취소했습니다." : u8"다시 적용했습니다.";
            }

            break;
        }

        case animation_editor_action::reduction_panel:
            reduction_open_ = !reduction_open_;
            break;

        case animation_editor_action::stride:
            stride_ = std::clamp(static_cast<int>(index), 2, 4);
            phase_ = std::min(phase_, stride_ - 1);
            refresh_reduction();
            break;

        case animation_editor_action::phase:
            phase_ = std::clamp(static_cast<int>(index), 0, stride_ - 1);
            refresh_reduction();
            break;

        case animation_editor_action::preserve_duration:
            preserve_duration_ = !preserve_duration_;
            refresh_reduction();
            break;

        case animation_editor_action::reduce: {
            // 새 편집 상태를 먼저 계산한 뒤 다른 편집과 같은 실행 취소 경로에 넣는다.
            auto reduced { reduce_animation(edit_, stride_, phase_, preserve_duration_) };
            commit(std::move(reduced));
            break;
        }

        case animation_editor_action::delay_all:
            delay_all_ = !delay_all_;
            break;

        case animation_editor_action::apply_delay:
            draft = edit_;

            if (const auto delay { field_value(0) }; delay && *delay >= 1 && *delay <= 600000)
            {
                for (std::size_t position {}; position < draft.frames.size(); ++position)
                {
                    if (delay_all_ || selected_[position])
                    {
                        draft.frames[position].milliseconds = *delay;
                    }
                }

                commit(std::move(draft));
            }
            else
            {
                status_ = u8"표시 시간은 1~600,000ms로 입력하세요.";
            }

            break;

        case animation_editor_action::apply_crop:
            if (auto crop { crop_draft() })
            {
                draft = edit_;
                draft.crop = *crop;
                draft.output_width = crop->width;
                draft.output_height = crop->height;
                commit(std::move(draft));
            }
            else
            {
                status_ = u8"크롭 영역 전체가 원본 안에 있어야 합니다.";
            }

            break;

        case animation_editor_action::reset_crop:
            draft = edit_;
            draft.crop = { 0, 0, document_->width(), document_->height() };
            draft.output_width = document_->width();
            draft.output_height = document_->height();
            commit(std::move(draft));
            break;

        case animation_editor_action::lock_aspect:
            lock_aspect_ = !lock_aspect_;
            break;

        case animation_editor_action::apply_size:
            draft = edit_;

            if (const auto width { field_value(5) }, height { field_value(6) };
                width && height && *width >= 1 && *width <= 16383 && *height >= 1 && *height <= 16383)
            {
                draft.output_width = *width;

                const auto output_height {
                    lock_aspect_ ? std::llround(static_cast<double>(*width) * draft.crop.height / draft.crop.width)
                                 : *height
                };

                if (output_height < 1 || output_height > 16383)
                {
                    status_ = u8"출력 크기는 한 변 1~16,383px로 입력하세요.";
                    break;
                }

                draft.output_height = static_cast<int>(output_height);
                commit(std::move(draft));
            }
            else
            {
                status_ = u8"출력 크기는 한 변 1~16,383px로 입력하세요.";
            }

            break;

        case animation_editor_action::format:
            options_.animation_format = index == 1 ? image_format::apng
                : index == 2                       ? image_format::gif
                                                   : image_format::webp;
            break;

        case animation_editor_action::preview_output:
            preview_output_ = !preview_output_;
            break;

        case animation_editor_action::scroll:
            scroll_
                = intent->alternate ? static_cast<float>(intent->value) : scroll_ + static_cast<float>(intent->value);
            break;

        case animation_editor_action::sidebar_scroll:
            sidebar_scroll_ = intent->alternate ? static_cast<float>(intent->value)
                                                : sidebar_scroll_ + static_cast<float>(intent->value);
            break;

        case animation_editor_action::zoom:
            zoom_ = std::clamp(
                intent->alternate ? static_cast<float>(intent->value) : zoom_ + static_cast<float>(intent->value),
                0.25f,
                4.0f
            );
            break;

        case animation_editor_action::export_file: {
            cancelled_ = false;
            playing_ = false;
            status_ = u8"편집한 애니메이션을 저장하는 중…";

            const auto format { options_.animation_format };
            const auto extension { format == image_format::apng      ? L".apng"
                                       : format == image_format::gif ? L".gif"
                                                                     : L".webp"
            };

            const auto stamp { std::chrono::system_clock::now().time_since_epoch().count() };
            const auto output { output_directory_ / (L"edited_" + std::to_wstring(stamp) + extension) };
            const auto document { document_ };
            const auto options { options_ };
            const auto edit { edit_ };
            work_ = std::async(std::launch::async, [this, document, options, edit, output, format] {
                work_result result {};

                try
                {
                    edited_animation_reader reader { document, edit, format == image_format::gif, cancelled_ };
                    encoding::encoder_options settings {};
                    settings.format = format == image_format::gif ? encoding::animation_format::gif
                        : format == image_format::apng            ? encoding::animation_format::apng
                                                                  : encoding::animation_format::webp;
                    settings.lossless = options.webp_lossless;
                    settings.quality = options.webp_quality;
                    settings.compact = options.compression != lossless_compression::fast;
                    settings.maximum_compression = options.compression == lossless_compression::maximum;
                    settings.play_count = document->play_count();

                    const auto source { reader.source() };

                    if (encoding::encode_animation(output, source, settings, cancelled_, result.message))
                    {
                        result.output = output;
                        result.message = u8"저장 완료: " + output.filename().u8string();
                    }
                }
                catch (...)
                {
                    result.message = u8"저장 중 파일 또는 메모리 오류가 발생했습니다. 편집 내용은 유지됩니다.";
                }

                return result;
            });
            break;
        }

        default:
            break;
        }

        return true;
    }

    std::optional<std::chrono::steady_clock::time_point> animation_editor::next_tick() const
    {
        if (opened_ && playing_)
        {
            return std::chrono::steady_clock::now() + std::chrono::milliseconds { 16 };
        }

        if (work_.valid() || image_work_.valid() || (opened_ && !image_requests_.empty()))
        {
            return std::chrono::steady_clock::now() + std::chrono::milliseconds { 40 };
        }

        return std::nullopt;
    }

    void animation_editor::tick(const std::chrono::steady_clock::time_point now)
    {
        if (work_.valid() && work_.wait_for(std::chrono::seconds { 0 }) == std::future_status::ready)
        {
            auto result { work_.get() };

            if (cancelled_.load())
            {
                result.document.reset();
                result.message = u8"작업을 취소했습니다. 원본과 편집 내용은 유지됩니다.";
            }

            cancelled_ = false;

            if (result.document)
            {
                document_ = std::move(result.document);
                title_ = pending_title_;
                edit_ = initial_animation_edit(*document_);
                refresh_timeline();
                undo_.clear();
                redo_.clear();
                images_.clear();
                ++image_generation_;
                playback_image_ = {};
                image_requests_.clear();
                current_ = 0;
                range_anchor_ = 0;
                scroll_ = 0;
                zoom_ = 1;
                reduction_open_ = false;
                preview_output_ = false;
                seek(0);
                status_ = u8"썸네일을 눌러 선택 · Ctrl로 추가 선택 · 원본 위를 드래그해 크롭";
            }
            else
            {
                status_ = result.message.empty() ? u8"원본을 열 수 없습니다." : result.message;
            }
        }

        if (image_work_.valid() && image_work_.wait_for(std::chrono::seconds { 0 }) == std::future_status::ready)
        {
            auto result { image_work_.get() };
            image_key_.reset();

            if (opened_ && result.generation == image_generation_ && result.document == document_
                && result.image.valid())
            {
                if (result.key.second > 128)
                {
                    playback_image_ = result.image;
                }

                cache_image(result.key, std::move(result.image));

                if (result.thumbnail.valid())
                {
                    const auto key { std::pair { result.key.first, 128 } };
                    cache_image(key, std::move(result.thumbnail));

                    std::erase(image_requests_, key);
                }
            }
        }

        if (opened_ && playing_ && !edit_.frames.empty())
        {
            const auto total { timeline_.back() };
            const auto elapsed { std::chrono::duration_cast<std::chrono::milliseconds>(now - playback_start_).count()
                                 + playback_offset_
            };

            const auto found { std::upper_bound(timeline_.begin(), timeline_.end(), elapsed % total) };
            const auto index { static_cast<std::size_t>(found - timeline_.begin() - 1) };

            if (index != current_)
            {
                current_ = index;
                luil::text::text_edit_set_text(fields_[0], number_text(edit_.frames[index].milliseconds), true);
            }

            static_cast<void>(image(edit_.frames[current_].source, 960));
        }

        if (opened_ && !image_work_.valid() && !image_requests_.empty() && document_ && !work_.valid()
            && (!playing_ || image_requests_.front().second > 128))
        {
            const auto key { image_requests_.front() };
            image_requests_.pop_front();

            const auto document { document_ };
            const auto generation { image_generation_ };
            image_key_ = key;
            image_work_ = std::async(std::launch::async, [this, document, key, generation] {
                image_result result { .document = document, .key = key, .generation = generation };

                try
                {
                    SkBitmap bitmap {}, scaled {};

                    if (!document->read(key.first, bitmap, cancelled_))
                    {
                        return result;
                    }

                    const double ratio {
                        std::min(1.0, static_cast<double>(key.second) / std::max(bitmap.width(), bitmap.height()))
                    };

                    const int width { std::max(1, static_cast<int>(bitmap.width() * ratio)) };
                    const int height { std::max(1, static_cast<int>(bitmap.height() * ratio)) };

                    if (scaled.tryAllocPixels(
                            SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType)
                        )
                        && bitmap.pixmap().scalePixels(
                            scaled.pixmap(), SkSamplingOptions { SkCubicResampler::Mitchell() }
                        ))
                    {
                        result.image = luil::make_rgba_image(
                            width,
                            height,
                            { static_cast<const std::uint8_t*>(scaled.getPixels()), scaled.computeByteSize() }
                        );
                    }

                    if (key.second > 128 && result.image.valid())
                    {
                        // 미리보기와 썸네일은 같은 원본 복원을 공유해 코덱을 되감지 않는다.
                        SkBitmap thumbnail {};
                        const double thumb_ratio { std::min(1.0, 128.0 / std::max(width, height)) };
                        const int thumb_width { std::max(1, static_cast<int>(width * thumb_ratio)) };
                        const int thumb_height { std::max(1, static_cast<int>(height * thumb_ratio)) };

                        if (thumbnail.tryAllocPixels(
                                SkImageInfo::Make(
                                    thumb_width, thumb_height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType
                                )
                            )
                            && scaled.pixmap().scalePixels(
                                thumbnail.pixmap(), SkSamplingOptions { SkCubicResampler::Mitchell() }
                            ))
                        {
                            result.thumbnail = luil::make_rgba_image(
                                thumb_width,
                                thumb_height,
                                { static_cast<const std::uint8_t*>(thumbnail.getPixels()), thumbnail.computeByteSize() }
                            );
                        }
                    }
                }
                catch (...)
                {}
                return result;
            });
        }
    }

    luil::ui_window animation_editor::make_window()
    {
        const float scale { metrics_.scale > 0 ? metrics_.scale : 1 };
        const float width { metrics_.width / scale };
        const float height { metrics_.height / scale };
        const float sidebar { 288 };
        const float right { width - sidebar - 32 };
        const float main_width { right - 40 };
        const bool available { document_ != nullptr && !work_.valid() };
        auto root { std::make_unique<luil::root_element>(u8"animation-editor-content") };
        // 오른쪽 편집 도구는 독립된 스크롤 영역에 조립하고 원본 미리보기와 타임라인을 따로 배치한다.
        auto tools { std::make_unique<editor_tools_element>(right, scale) };
        const auto add = [&](std::unique_ptr<luil::ui_element> element) {
            const auto box { element->bounds() };

            if (box.x >= right * scale && box.y >= 120 * scale && element->id().owner != u8"edit-cancel")
            {
                tools->add(std::move(element));
            }
            else
            {
                root->add(std::move(element));
            }
        };

        const auto label = [&](const char8_t* owner,
                               std::u8string text,
                               const float x,
                               const float y,
                               const float w,
                               const float size = 12) {
            luil::label_config config {};
            config.text = std::move(text);
            config.font_size = size;
            config.color = size >= 14 ? luil::label_color_role::primary : luil::label_color_role::dim;

            auto element {
                std::make_unique<luil::label_element>(luil::ui_element_id { editor_kind, owner }, std::move(config))
            };
            element->arrange({ { x * scale, y * scale, w * scale, 24 * scale }, scale });
            add(std::move(element));
        };

        const auto button = [&](const char8_t* owner,
                                const char8_t* text,
                                const luil::rect_f box,
                                const luil::ui_action& callback,
                                const bool enabled = true,
                                const luil::text_button_visual visual = luil::text_button_visual::normal) {
            auto element { std::make_unique<luil::text_button_element>(
                luil::ui_element_id { editor_kind, owner }, luil::text_button_config { .text = text, .visual = visual }
            )
            };
            element->set_action(luil::ui_trigger::left_click, callback);
            element->set_enabled(enabled);
            element->arrange({ { box.x * scale, box.y * scale, box.width * scale, box.height * scale }, scale });
            add(std::move(element));
        };

        const auto input = [&](const std::size_t index, const float x, const float y, const float w) {
            luil::text_input_config config {};
            config.input.kind = luil::text_input_kind::number;
            config.placeholder = field_names[index];

            const luil::ui_element_id id { luil::application_element_kind(static_cast<std::uint32_t>(30 + index)),
                                           field_owners[index]
            };

            const auto view { luil::make_text_input_view(
                fields_[index], composition_, static_cast<luil::text_input_target>(301 + index)
            )
            };

            auto element { std::make_unique<luil::text_input_element>(id, view, std::move(config)) };
            element->set_access_name(field_names[index]);
            element->set_enabled(available && (index != 6 || !lock_aspect_));
            element->arrange({ { x * scale, y * scale, w * scale, 30 * scale }, scale });
            add(std::move(element));
        };

        const auto check = [&](const char8_t* owner,
                               const char8_t* text,
                               const bool checked,
                               const animation_editor_action command,
                               const float x,
                               const float y,
                               const float w) {
            auto element { std::make_unique<luil::check_element>(luil::check_config {
                .owner = owner, .label = text, .checked = checked, .toggle = button_action(command)
            })
            };
            element->set_enabled(available);
            element->arrange({ { x * scale, y * scale, w * scale, 28 * scale }, scale });
            add(std::move(element));
        };

        const auto choice = [&](const char8_t* owner,
                                const char8_t* name,
                                std::vector<luil::choice_item> choices,
                                const std::u8string& selected,
                                const animation_editor_action command,
                                const float x,
                                const float y,
                                const float w) {
            luil::choice_group_config config {};
            config.owner = owner;
            config.style = luil::choice_style::toggle;
            config.items = std::move(choices);

            for (auto& item : config.items)
            {
                item.value = std::u8string { owner } + u8"/" + item.value;
            }

            config.name = name;
            config.selected = std::u8string { owner } + u8"/" + selected;
            config.select = [command](const std::u8string& key) {
                const auto value { key.substr(key.find_last_of(u8'/') + 1) };
                return make_editor_action(command, std::stoi(std::string { value.begin(), value.end() }));
            };

            auto element { std::make_unique<luil::choice_group_element>(std::move(config)) };
            element->set_access_name(name);
            element->set_enabled(available);
            element->arrange({ { x * scale, y * scale, w * scale, 30 * scale }, scale });
            add(std::move(element));
        };
        luil::ui_window window {};
        window.id = u8"animation-editor";
        window.caption.title = u8"애니메이션 편집 · chalkag";
        window.caption.buttons = { .minimize = true, .maximize = true, .close = true };
        window.caption.close_tooltip = u8"편집창 닫기";
        window.x = 24;
        window.y = 96;
        window.width = 1040;
        window.height = 760;
        window.minimum_width = 960;
        window.minimum_height = 730;
        window.close = [] { return make_editor_action(animation_editor_action::close); };
        window.metrics = [](const float w, const float h, const float s) {
            return luil::app_message { animation_editor_metrics { w, h, s } };
        };

        auto caption { std::make_unique<luil::caption_element>(window.caption) };

        for (const auto& child : caption->children())
        {
            if (child->id() == luil::caption_button_element_id(luil::caption_button_hover::close))
            {
                child->set_action(luil::ui_trigger::left_click, button_action(animation_editor_action::close));
            }
        }

        caption->arrange({ { 0, 0, width * scale, luil::caption_element::height_for(window.caption) * scale }, scale });
        root->add(std::move(caption));
        label(u8"edit-source", title_, 20, 42, main_width - 210, 16);
        button(
            u8"edit-open",
            u8"이미지 열기",
            { width - 268, 40, 116, 32 },
            [](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                return { luil::app_ui_command { .command
                                                = static_cast<std::uint32_t>(runtime_command::open_animation)
                }
                };
            },
            !work_.valid()
        );
        button(
            u8"edit-export",
            u8"편집본 저장",
            { width - 144, 40, 128, 32 },
            button_action(animation_editor_action::export_file),
            available,
            luil::text_button_visual::accent
        );
        button(
            u8"edit-undo",
            u8"실행 취소",
            { 20, 78, 88, 28 },
            button_action(animation_editor_action::undo),
            available && !undo_.empty()
        );
        button(
            u8"edit-redo",
            u8"다시 실행",
            { 116, 78, 88, 28 },
            button_action(animation_editor_action::redo),
            available && !redo_.empty()
        );

        if (document_)
        {
            label(
                u8"edit-summary",
                number_text(static_cast<std::int64_t>(edit_.frames.size())) + u8" 프레임 · "
                    + seconds_text(timeline_.back()) + u8" · 원본 " + number_text(document_->width()) + u8" × "
                    + number_text(document_->height()),
                216,
                80,
                main_width - 196
            );

            const auto draft { crop_draft() };
            const auto preview_image { image(edit_.frames[current_].source, 960) };
            auto preview { std::make_unique<crop_preview_element>(
                preview_image, draft ? *draft : edit_.crop, document_->width(), document_->height(), preview_output_
            )
            };
            preview->set_enabled(available);
            preview->arrange({ { 20 * scale, 120 * scale, main_width * scale, (height - 430) * scale }, scale });
            root->add(std::move(preview));
            button(
                u8"edit-previous",
                u8"이전",
                { 20, height - 296, 60, 30 },
                button_action(animation_editor_action::previous),
                available
            );
            button(
                u8"edit-play",
                playing_ ? u8"일시 정지" : u8"재생",
                { 88, height - 296, 80, 30 },
                button_action(animation_editor_action::play),
                available
            );
            button(
                u8"edit-next",
                u8"다음",
                { 176, height - 296, 60, 30 },
                button_action(animation_editor_action::next),
                available
            );
            label(
                u8"edit-position",
                number_text(static_cast<std::int64_t>(current_ + 1)) + u8" / "
                    + number_text(static_cast<std::int64_t>(edit_.frames.size())),
                248,
                height - 292,
                120
            );
            button(
                u8"edit-preview-mode",
                preview_output_ ? u8"원본 / 크롭" : u8"크롭 결과 보기",
                { main_width - 104, height - 296, 124, 30 },
                button_action(animation_editor_action::preview_output),
                available
            );

            const double unit { 64.0 * zoom_ / minimum_delay_ };
            const auto position = [&](const std::size_t index) { return timeline_[index] * unit; };
            const float maximum { static_cast<float>(std::max(0.0, position(timeline_.size() - 1) - main_width)) };
            scroll_ = std::clamp(scroll_, 0.0f, maximum);

            if (playing_ && (position(current_) < scroll_ || position(current_ + 1) > scroll_ + main_width))
            {
                scroll_ = std::clamp(static_cast<float>(position(current_)), 0.0f, maximum);
            }

            // 화면에 걸치는 프레임만 UI 요소로 만들되 가로 폭은 실제 표시 시간에 비례시킨다.
            auto timeline { std::make_unique<timeline_element>(scroll_, scale) };
            const auto prefix { timeline_[current_] };
            const auto elapsed { std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - playback_start_
                                 )
                                     .count()
                                 + playback_offset_
            };

            const auto cycle { playing_ ? elapsed / timeline_.back() * timeline_.back() : 0 };
            const auto started { playing_ ? std::optional { playback_start_
                                                            + std::chrono::milliseconds { prefix - playback_offset_
                                                                                          + cycle
                                                            }
            }
                                          : std::nullopt
            };

            const auto begin {
                std::upper_bound(timeline_.begin(), timeline_.end(), static_cast<std::int64_t>(scroll_ / unit))
            };

            const auto first { begin == timeline_.begin() ? std::size_t {}
                                                          : static_cast<std::size_t>(begin - timeline_.begin() - 1)
            };

            for (auto index { first }; index < edit_.frames.size() && position(index) < scroll_ + main_width; ++index)
            {
                auto frame { std::make_unique<frame_element>(
                    index,
                    edit_.frames[index],
                    image(edit_.frames[index].source, 128),
                    selected_[index],
                    index == current_,
                    started
                )
                };
                frame->set_enabled(available);
                luil::rect_f box {};
                box.x = (20 + static_cast<float>(position(index) - scroll_)) * scale;
                box.y = (height - 254) * scale;
                box.width = static_cast<float>(position(index + 1) - position(index)) * scale;
                box.height = 86 * scale;
                frame->arrange({ box, scale });
                timeline->add(std::move(frame));
            }

            timeline->arrange({ { 20 * scale, (height - 254) * scale, main_width * scale, 86 * scale }, scale });
            root->add(std::move(timeline));

            auto bar { std::make_unique<timeline_scroll_element>(
                main_width, static_cast<float>(position(timeline_.size() - 1)), scroll_
            )
            };
            bar->set_access_name(u8"타임라인 가로 스크롤 위치");
            bar->set_enabled(available && maximum > 0);
            bar->arrange({ { 20 * scale, (height - 160) * scale, main_width * scale, 20 * scale }, scale });
            root->add(std::move(bar));
            luil::slider_config zoom {};
            zoom.minimum = 0.25f;
            zoom.maximum = 4;
            zoom.value = zoom_;
            zoom.change = [](const float delta) { return make_editor_action(animation_editor_action::zoom, delta); };
            zoom.change_to
                = [](const float value) { return make_editor_action(animation_editor_action::zoom, value, true); };

            auto zoom_control { std::make_unique<timeline_zoom_element>(std::move(zoom)) };
            zoom_control->set_access_name(u8"타임라인 확대 배율");
            zoom_control->set_enabled(available);
            zoom_control->arrange(
                { { (main_width - 84) * scale, (height - 131) * scale, 104 * scale, 20 * scale }, scale }
            );
            root->add(std::move(zoom_control));
            label(
                u8"edit-selection",
                number_text(static_cast<std::int64_t>(selected_count_)) + u8"장 선택 · Ctrl 클릭으로 추가 선택",
                20,
                height - 132,
                main_width - 140,
                11
            );
            button(
                u8"edit-select-all",
                u8"전체 선택",
                { 20, height - 102, 84, 28 },
                button_action(animation_editor_action::select_all),
                available
            );
            button(
                u8"edit-range-start",
                u8"선택 시작",
                { 112, height - 102, 84, 28 },
                button_action(animation_editor_action::range_start),
                available
            );
            button(
                u8"edit-range-end",
                u8"여기까지 선택",
                { 204, height - 102, 102, 28 },
                button_action(animation_editor_action::range_end),
                available
            );
            button(
                u8"edit-delete",
                u8"선택 삭제",
                { 314, height - 102, 90, 28 },
                button_action(animation_editor_action::erase),
                available,
                luil::text_button_visual::danger
            );
            button(
                u8"edit-trim-before",
                u8"현재 앞 자르기",
                { 20, height - 66, 124, 28 },
                button_action(animation_editor_action::trim_before),
                available && current_ > 0
            );
            button(
                u8"edit-trim-after",
                u8"현재 뒤 자르기",
                { 152, height - 66, 124, 28 },
                button_action(animation_editor_action::trim_after),
                available && current_ + 1 < edit_.frames.size()
            );
        }

        if (!document_)
        {
            const float welcome_height { height - 178 };
            const float welcome_width { main_width - 8 };
            auto welcome { std::make_unique<image_drop_element>(u8"edit-welcome-drop", true) };
            welcome->set_enabled(can_open_image());
            welcome->arrange({ { 20 * scale, 120 * scale, welcome_width * scale, welcome_height * scale }, scale });
            root->add(std::move(welcome));

            button(
                u8"edit-browse",
                u8"이미지 열기",
                { 20 + (welcome_width - 136) / 2, 120 + welcome_height / 2 + 16, 136, 36 },
                [](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                    return { luil::app_ui_command { .command
                                                    = static_cast<std::uint32_t>(runtime_command::open_animation)
                    }
                    };
                },
                can_open_image(),
                luil::text_button_visual::accent
            );
        }

        label(u8"edit-delay-label", u8"프레임 간격", right, 120, sidebar, 14);
        input(0, right, 150, 96);
        button(
            u8"edit-apply-delay",
            u8"간격 적용",
            { right + 104, 150, sidebar - 104, 30 },
            button_action(animation_editor_action::apply_delay),
            available
        );
        check(
            u8"edit-delay-all",
            u8"전체 프레임에 적용",
            delay_all_,
            animation_editor_action::delay_all,
            right,
            186,
            sidebar
        );
        button(
            u8"edit-reduction",
            reduction_open_ ? u8"프레임 수 줄이기  ▴" : u8"프레임 수 줄이기  ▾",
            { right, 224, sidebar, 32 },
            button_action(animation_editor_action::reduction_panel),
            available
        );
        float y { 270 };

        if (reduction_open_ && document_)
        {
            choice(
                u8"reduce-stride",
                u8"남길 프레임 비율",
                { { u8"2", u8"½" }, { u8"3", u8"⅓" }, { u8"4", u8"¼" } },
                number_text(stride_),
                animation_editor_action::stride,
                right,
                y,
                sidebar
            );
            y += 38;

            std::vector<luil::choice_item> phases {};

            for (int phase {}; phase < stride_; ++phase)
            {
                phases.push_back(
                    { number_text(phase),
                      stride_ == 2 ? (phase == 0 ? u8"홀수 유지" : u8"짝수 유지") : number_text(phase + 1) + u8"번째"
                    }
                );
            }

            choice(
                u8"reduce-phase",
                u8"묶음에서 남길 프레임",
                std::move(phases),
                number_text(phase_),
                animation_editor_action::phase,
                right,
                y,
                sidebar
            );
            y += 34;
            check(
                u8"reduce-preserve",
                u8"재생 길이 유지",
                preserve_duration_,
                animation_editor_action::preserve_duration,
                right,
                y,
                sidebar
            );
            y += 30;

            const auto& reduced { reduced_preview_ };
            label(
                u8"reduce-summary",
                number_text(static_cast<std::int64_t>(edit_.frames.size())) + u8" → "
                    + number_text(static_cast<std::int64_t>(reduced.frames.size())) + u8"장 · "
                    + seconds_text(reduced_duration_),
                right,
                y,
                sidebar,
                12
            );
            y += 28;
            button(
                u8"reduce-apply",
                u8"프레임 줄이기 적용",
                { right, y, sidebar, 30 },
                button_action(animation_editor_action::reduce),
                available && reduced != edit_
            );
            y += 42;
        }

        label(u8"edit-crop-label", u8"크롭 · 원본 픽셀 기준", right, y, sidebar, 14);
        y += 28;
        label(u8"crop-position-label", u8"X / Y", right, y, sidebar, 11);
        y += 24;
        input(1, right, y, 136);
        input(2, right + 144, y, 144);
        y += 34;
        label(u8"crop-size-label", u8"너비 / 높이", right, y, sidebar, 11);
        y += 24;
        input(3, right, y, 136);
        input(4, right + 144, y, 144);
        y += 38;
        button(
            u8"edit-apply-crop",
            u8"크롭 적용",
            { right, y, 136, 30 },
            button_action(animation_editor_action::apply_crop),
            available
        );
        button(
            u8"edit-reset-crop",
            u8"원본 영역",
            { right + 144, y, 144, 30 },
            button_action(animation_editor_action::reset_crop),
            available
        );
        y += 44;
        label(u8"edit-size-label", u8"출력 크기", right, y, sidebar, 14);
        y += 28;
        input(5, right, y, 136);
        input(6, right + 144, y, 144);
        y += 34;
        check(
            u8"edit-aspect",
            u8"비율 유지 · 너비 기준",
            lock_aspect_,
            animation_editor_action::lock_aspect,
            right,
            y,
            196
        );
        button(
            u8"edit-apply-size",
            u8"적용",
            { right + 204, y, 84, 28 },
            button_action(animation_editor_action::apply_size),
            available
        );
        y += 38;
        choice(
            u8"edit-format",
            u8"편집 결과 저장 형식",
            { { u8"0", u8"WebP" }, { u8"1", u8"APNG" }, { u8"2", u8"GIF" } },
            options_.animation_format == image_format::apng      ? u8"1"
                : options_.animation_format == image_format::gif ? u8"2"
                                                                 : u8"0",
            animation_editor_action::format,
            right,
            y,
            sidebar
        );
        y += 34;
        label(
            u8"edit-export-note",
            options_.animation_format == image_format::gif ? u8"GIF · 투명 영역은 흰색 배경으로 합성"
                                                           : u8"투명도·반복 횟수 유지",
            right,
            y,
            sidebar,
            11
        );

        if (options_.animation_format == image_format::gif)
        {
            y += 22;
            label(u8"edit-gif-timing", u8"간격은 10ms 단위 · 최소 20ms로 저장", right, y, sidebar, 11);
        }

        luil::scroll_area_config tools_config {};

        tools_config.owner = u8"editor-tools";
        tools_config.content_height = y + 34 - 120;
        tools_config.scroll_offset = sidebar_scroll_;
        tools_config.scroll
            = [](const float delta) { return make_editor_action(animation_editor_action::sidebar_scroll, delta); };

        tools_config.scroll_to = [](const float value) {
            return make_editor_action(animation_editor_action::sidebar_scroll, value, true);
        };

        auto area { std::make_unique<luil::scroll_area_element>(std::move(tools_config)) };
        area->set_content(std::move(tools));
        area->arrange(
            { { right * scale, 120 * scale, (sidebar + luil::scroll_area_bar_width) * scale, (height - 158) * scale },
              scale
            }
        );
        sidebar_scroll_ = area->metrics().scroll_offset;
        root->add(std::move(area));
        label(u8"edit-status", status_, 20, height - 28, width - 120, 11);

        if (work_.valid())
        {
            button(
                u8"edit-cancel",
                u8"취소",
                { width - 88, height - 30, 72, 26 },
                button_action(animation_editor_action::cancel)
            );
        }

        auto image_drop { std::make_unique<image_drop_element>(u8"editor-image-drop") };
        image_drop->set_enabled(can_open_image());
        const float caption_height { luil::caption_element::height_for(window.caption) * scale };
        image_drop->arrange({ { 0, caption_height, width * scale, height * scale - caption_height }, scale });
        root->add(std::move(image_drop));

        root->arrange({ { 0, 0, width * scale, height * scale }, scale });
        window.tree = std::make_shared<const luil::ui_tree>(std::move(root));
        return window;
    }

    std::optional<luil::text_input_target> animation_editor_policy::text_target_of(
        const luil::ui_element_kind kind
    ) const
    {
        for (std::uint32_t index {}; index < 7; ++index)
        {
            if (kind == luil::application_element_kind(30 + index))
            {
                return static_cast<luil::text_input_target>(301 + index);
            }
        }

        return std::nullopt;
    }

    luil::input_action animation_editor_policy::make_text_edit_action(const luil::text_edit_request& request) const
    {
        return luil::make_app_action(animation_editor_edit_intent { request });
    }

    luil::input_action animation_editor_policy::make_text_composition_action(
        const luil::text_composition_event& event
    ) const
    {
        return luil::make_app_action(animation_editor_composition_intent { event });
    }

} // namespace chalkag
