#include "app/help_text.h"

#include "luil/text/utf8_text.h"
#include "luil/ui/draw_primitives.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkTypeface_win.h"

#include <cmath>
#include <utility>

namespace chalkag {
    namespace {
        constexpr float help_line_padding { 7.0f };

        bool is_space(const char8_t value) noexcept
        {
            return value == u8' ' || value == u8'\t';
        }

        std::u8string_view trim_spaces(std::u8string_view text) noexcept
        {
            while (!text.empty() && is_space(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && is_space(text.back()))
            {
                text.remove_suffix(1);
            }

            return text;
        }

        void elide_last_line(
            std::u8string& line, const float width, const std::function<float(std::u8string_view)>& measure
        )
        {
            constexpr std::u8string_view ellipsis { u8"…" };

            if (measure(ellipsis) > width)
            {
                line.clear();
                return;
            }

            line = trim_spaces(line);

            while (true)
            {
                const std::u8string candidate { line + std::u8string { ellipsis } };

                if (measure(candidate) <= width)
                {
                    line = candidate;
                    return;
                }

                if (line.empty())
                {
                    return;
                }

                std::size_t boundary { line.size() - 1 };

                while (boundary > 0 && (static_cast<unsigned char>(line[boundary]) & 0xc0U) == 0x80U)
                {
                    --boundary;
                }

                line.resize(boundary);
            }
        }

        sk_sp<SkTypeface> help_typeface()
        {
            // 설치 글꼴 조회는 한 번만 하고 측정과 그리기가 같은 UI 글꼴을 쓴다.
            static const sk_sp<SkFontMgr> manager { SkFontMgr_New_DirectWrite() };
            static const sk_sp<SkTypeface> typeface { manager != nullptr
                                                          ? manager->matchFamilyStyle("Segoe UI", SkFontStyle::Normal())
                                                          : nullptr
            };
            return typeface;
        }

        luil::ui_color help_color(const luil::ui_color_palette& palette, const luil::label_color_role role) noexcept
        {
            switch (role)
            {
            case luil::label_color_role::primary:
                return palette.primary_foreground;

            case luil::label_color_role::error:
                return palette.error_accent;

            case luil::label_color_role::dim:
                return palette.secondary_foreground;
            }

            return palette.secondary_foreground;
        }

    } // namespace

    help_text_layout wrap_help_text(
        const std::u8string_view text,
        const float width,
        const std::function<float(std::u8string_view)>& measure,
        const std::size_t max_lines
    )
    {
        help_text_layout result {};

        if (text.empty() || !std::isfinite(width) || width <= 0.0f || !measure)
        {
            return result;
        }

        std::u8string storage {};
        const auto drawable { luil::drawable_text(text, storage) };
        const auto append_line = [&](const std::u8string_view line) {
            if (max_lines > 0 && result.lines.size() == max_lines)
            {
                elide_last_line(result.lines.back(), width, measure);
                return false;
            }

            result.lines.emplace_back(line);
            return true;
        };

        std::size_t paragraph_start {};

        while (paragraph_start <= drawable.size())
        {
            const auto separator { drawable.find_first_of(u8"\r\n", paragraph_start) };
            const auto paragraph_end { separator == std::u8string_view::npos ? drawable.size() : separator };
            const auto paragraph { trim_spaces(drawable.substr(paragraph_start, paragraph_end - paragraph_start)) };

            if (paragraph.empty())
            {
                if (!append_line({}))
                {
                    return result;
                }
            }
            else
            {
                std::size_t start {};

                while (start < paragraph.size())
                {
                    std::size_t end { start };
                    std::size_t last_space { std::u8string_view::npos };

                    while (end < paragraph.size())
                    {
                        if (is_space(paragraph[end]) && end > start)
                        {
                            last_space = end;
                        }

                        const auto next { end + luil::text::utf8_decode(paragraph, end).length };

                        if (measure(paragraph.substr(start, next - start)) > width)
                        {
                            break;
                        }

                        end = next;
                    }

                    if (end == start)
                    {
                        end += luil::text::utf8_decode(paragraph, start).length;
                    }
                    else if (end < paragraph.size() && last_space != std::u8string_view::npos)
                    {
                        end = last_space;
                    }

                    if (!append_line(trim_spaces(paragraph.substr(start, end - start))))
                    {
                        return result;
                    }

                    start = end;

                    while (start < paragraph.size() && is_space(paragraph[start]))
                    {
                        ++start;
                    }
                }
            }

            if (separator == std::u8string_view::npos)
            {
                break;
            }

            paragraph_start = separator + 1;

            if (drawable[separator] == u8'\r' && paragraph_start < drawable.size()
                && drawable[paragraph_start] == u8'\n')
            {
                ++paragraph_start;
            }
        }

        return result;
    }

    help_text_layout layout_help_text(
        const std::u8string_view text, const float width, const float font_size, const std::size_t max_lines
    )
    {
        if (!std::isfinite(font_size) || font_size <= 0.0f)
        {
            return {};
        }

        const SkFont font { help_typeface(), font_size };
        return wrap_help_text(
            text, width, [&font](const std::u8string_view value) { return luil::measure_text(value, font); }, max_lines
        );
    }

    wrapped_help_element::wrapped_help_element(
        luil::ui_element_id id,
        help_text_layout layout,
        const float font_size,
        const luil::label_color_role color,
        std::u8string access_name
    )
        : ui_element { std::move(id) }
        , layout_ { std::move(layout) }
        , font_size_ { std::isfinite(font_size) && font_size > 0.0f ? font_size : 12.0f }
        , color_ { color }
        , access_name_ { std::move(access_name) }
    {
        if (access_name_.empty())
        {
            bool first_line { true };

            for (const auto& line : layout_.lines)
            {
                if (!first_line)
                {
                    access_name_.push_back(u8'\n');
                }

                access_name_ += line;
                first_line = false;
            }
        }
    }

    void wrapped_help_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void wrapped_help_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        static_cast<void>(interaction);

        const float scale { std::isfinite(context.scale) && context.scale > 0.0f ? context.scale : 1.0f };
        const SkFont font { context.ui_typeface != nullptr ? sk_ref_sp(context.ui_typeface) : help_typeface(),
                            font_size_ * scale
        };

        const SkPaint paint { luil::solid_paint(help_color(context.palette, color_)) };
        const auto box { bounds() };
        const float line_height { (font_size_ + help_line_padding) * scale };
        float baseline { box.y + luil::centered_text_baseline(font, line_height) };
        context.canvas.save();
        context.canvas.clipRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height));

        for (const auto& line : layout_.lines)
        {
            luil::draw_text(context.canvas, line, box.x, baseline, font, paint);
            baseline += line_height;
        }

        context.canvas.restore();
    }

    luil::access_info wrapped_help_element::accessibility() const
    {
        return { .role = luil::access_role::static_text, .name = access_name_ };
    }

    float wrapped_help_element::height() const noexcept
    {
        return static_cast<float>(layout_.lines.size()) * (font_size_ + help_line_padding);
    }

} // namespace chalkag
