#include "app/accent_picker.h"
#include "app/app_runtime.h"

#include "luil/ui/draw_primitives.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

namespace chalkag {
    namespace {
        constexpr std::size_t columns { 9 };
        constexpr float row_height { 28 };

        class accent_swatch_element final : public luil::ui_element
        {
        public:
            accent_swatch_element(const luil::accent_definition& accent, const bool selected)
                : ui_element { { luil::application_element_kind(10), u8"accent/" + std::u8string { accent.id } } }
                , color_ { accent.swatch }
                , name_ { std::u8string { accent.label } + u8" 키 컬러" }
                , selected_ { selected }
            {
                set_cursor(luil::ui_cursor::hand);
                set_search_label(name_);
                set_action(
                    luil::ui_trigger::left_click,
                    [value
                     = std::u8string { accent.id }](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                        return { luil::app_ui_command { static_cast<std::uint32_t>(runtime_command::set_accent),
                                                        value
                        }
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
                const float scale { context.scale };
                const auto box { bounds() };
                luil::draw_hover_fill(context, box, id(), interaction, enabled(), 6);

                const float x { box.x + box.width / 2 };
                const float y { box.y + box.height / 2 };
                auto paint { luil::solid_paint(enabled() ? color_ : luil::with_alpha(color_, 0.3f)) };
                paint.setAntiAlias(true);
                context.canvas.drawCircle(x, y, 8 * scale, paint);

                if (selected_ || (interaction.focus_visible && interaction.focused == id()))
                {
                    paint.setColor(context.palette.primary_foreground);
                    paint.setStyle(SkPaint::kStroke_Style);
                    paint.setStrokeWidth(1.2f * scale);
                    context.canvas.drawCircle(x, y, 11 * scale, paint);
                }

                if (selected_)
                {
                    paint.setColor(luil::foreground_on(color_));
                    paint.setStyle(SkPaint::kStroke_Style);
                    paint.setStrokeWidth(1.7f * scale);
                    paint.setStrokeCap(SkPaint::kRound_Cap);
                    context.canvas.drawLine(x - 3 * scale, y, x - scale, y + 2 * scale, paint);
                    context.canvas.drawLine(x - scale, y + 2 * scale, x + 3 * scale, y - 2 * scale, paint);
                }
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::radio_button, .name = name_, .selected = selected_ };
            }

        private:
            luil::ui_color color_ {};

            std::u8string name_ {};
            bool selected_ {};
        };
    } // namespace

    accent_picker_element::accent_picker_element(const std::u8string_view selected)
        : ui_element { { luil::application_element_kind(9), u8"accent-picker" } }
    {
        set_focus_group(luil::focus_axis::horizontal);
        set_focus_entry({ luil::application_element_kind(10), u8"accent/" + std::u8string { selected } });

        if (const auto system { luil::system_accent() }; system)
        {
            add_child(std::make_unique<accent_swatch_element>(*system, selected == system->id));
        }

        for (const auto& accent : luil::accent_catalog())
        {
            add_child(std::make_unique<accent_swatch_element>(accent, selected == accent.id));
        }
    }

    void accent_picker_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);

        const float width { context.slot.width / static_cast<float>(columns) };
        const auto elements { children() };

        for (std::size_t index {}; index < elements.size(); ++index)
        {
            const float x { context.slot.x + static_cast<float>(index % columns) * width };
            const float y { context.slot.y + static_cast<float>(index / columns) * row_height * context.scale };
            elements[index]->arrange(context.for_child({ x, y, width, row_height * context.scale }));
        }
    }

    void accent_picker_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        draw_children(context, interaction);
    }

    luil::access_info accent_picker_element::accessibility() const
    {
        return { .role = luil::access_role::radio_group, .name = u8"키 컬러" };
    }

} // namespace chalkag
