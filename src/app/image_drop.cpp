#include "app/image_drop.h"
#include "app/app_runtime.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkTypeface.h"
#include "luil/ui/draw_primitives.h"

#include <algorithm>
#include <array>

namespace chalkag {
    bool editable_image_path(const std::u8string_view path) noexcept
    {
        const auto separator { path.find_last_of(u8"/\\") };
        const auto dot { path.find_last_of(u8'.') };

        if (dot == std::u8string_view::npos || (separator != std::u8string_view::npos && dot < separator))
        {
            return false;
        }

        const auto extension { path.substr(dot) };
        constexpr std::array supported { u8".gif", u8".webp", u8".apng", u8".png" };

        for (const std::u8string_view candidate : supported)
        {
            if (extension.size() != candidate.size())
            {
                continue;
            }

            const bool matches { std::equal(
                extension.begin(),
                extension.end(),
                candidate.begin(),
                [](const char8_t actual, const char8_t expected) {
                    const auto lower { actual >= u8'A' && actual <= u8'Z' ? actual + (u8'a' - u8'A') : actual };
                    return lower == expected;
                }
            )
            };

            if (matches)
            {
                return true;
            }
        }

        return false;
    }

    luil::drop_target image_file_drop_target()
    {
        luil::drop_target target {};
        target.accepts = [](const luil::drag_payload& payload) {
            return payload.files.size() == 1 && editable_image_path(payload.files.front());
        };
        target.on_drop
            = [](const luil::drag_payload& payload, const luil::ui_action_context&) -> std::vector<luil::input_action> {
            if (payload.files.size() != 1 || !editable_image_path(payload.files.front()))
            {
                return {};
            }

            // 파일 선택창과 드롭은 같은 열기 명령을 사용한다. 경로는 해석하지 않고 그대로 전달한다.
            return { luil::app_ui_command { .command = static_cast<std::uint32_t>(runtime_command::open_animation),
                                            .argument = payload.files.front()
            }
            };
        };
        return target;
    }

    image_drop_element::image_drop_element(std::u8string owner, const bool persistent)
        : ui_element { { luil::application_element_kind(40), std::move(owner) } }
        , persistent_ { persistent }
    {
        set_drop_target(image_file_drop_target());
        set_hit_opaque(true);
    }

    void image_drop_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void image_drop_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const bool dragging { interaction.drag && interaction.drag->hovered_drop_target == id() };

        if (!persistent_ && !dragging)
        {
            return;
        }

        const auto box { bounds() };
        const float scale { context.scale > 0 ? context.scale : 1 };
        const auto rectangle { SkRect::MakeXYWH(box.x, box.y, box.width, box.height) };
        auto background { luil::solid_paint(context.palette.surface_background) };
        context.canvas.drawRect(rectangle, background);

        const float inset { 12 * scale };
        const auto border_box { rectangle.makeInset(inset, inset) };
        const auto border_shape { SkRRect::MakeRectXY(border_box, 12 * scale, 12 * scale) };
        auto border { luil::solid_paint(dragging ? context.palette.accent : context.palette.input_border) };
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth((dragging ? 2.0f : 1.0f) * scale);
        context.canvas.drawRRect(border_shape, border);

        // 설정창 진입과 빈 편집창에서 같은 안내를 보여 주고, 작은 창에서는 한 줄로 줄인다.
        const bool compact { box.height < 140 * scale };
        const std::u8string_view title { compact ? u8"놓아서 편집하기" : u8"여기로 끌어 편집하기" };
        const SkFont title_font { sk_ref_sp(context.ui_typeface), (compact ? 14.0f : 22.0f) * scale };
        const auto foreground { luil::solid_paint(context.palette.primary_foreground) };
        const float title_x { box.x + (box.width - luil::measure_text(title, title_font)) / 2 };
        const float title_y { box.y + box.height / 2 + (compact ? 5.0f : -36.0f) * scale };
        luil::draw_text(context.canvas, title, title_x, title_y, title_font, foreground);

        if (!compact)
        {
            constexpr std::u8string_view detail { u8"GIF · WebP · APNG · PNG 이미지 한 개" };
            const SkFont detail_font { sk_ref_sp(context.ui_typeface), 12 * scale };
            const auto secondary { luil::solid_paint(context.palette.secondary_foreground) };
            const float detail_x { box.x + (box.width - luil::measure_text(detail, detail_font)) / 2 };
            luil::draw_text(context.canvas, detail, detail_x, title_y + 30 * scale, detail_font, secondary);
        }
    }

    const luil::ui_element* image_drop_element::hit_test(float, float) const
    {
        // 드롭만 받고 기존 설정과 편집 버튼의 마우스 클릭을 가로채지 않는다.
        return nullptr;
    }

    luil::access_info image_drop_element::accessibility() const
    {
        return { .role = luil::access_role::group, .name = u8"이미지를 끌어 편집하기" };
    }
} // namespace chalkag
