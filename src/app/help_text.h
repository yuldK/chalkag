#pragma once

#include "luil/ui/label_element.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace chalkag {
    struct help_text_layout
    {
        std::vector<std::u8string> lines {};
    };

    [[nodiscard]] help_text_layout wrap_help_text(
        std::u8string_view text,
        float width,
        const std::function<float(std::u8string_view)>& measure,
        std::size_t max_lines = 0
    );
    [[nodiscard]] help_text_layout layout_help_text(
        std::u8string_view text, float width, float font_size, std::size_t max_lines = 0
    );

    class wrapped_help_element final : public luil::ui_element
    {
    public:
        wrapped_help_element(
            luil::ui_element_id id,
            help_text_layout layout,
            float font_size = 12.0f,
            luil::label_color_role color = luil::label_color_role::dim,
            std::u8string access_name = {}
        );

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;
        [[nodiscard]] float height() const noexcept;

    private:
        help_text_layout layout_ {};
        float font_size_ { 12.0f };
        luil::label_color_role color_ { luil::label_color_role::dim };
        std::u8string access_name_ {};
    };
} // namespace chalkag
