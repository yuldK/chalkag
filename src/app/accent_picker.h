#pragma once

#include "luil/ui/ui_element.h"

namespace chalkag {
    class accent_picker_element final : public luil::ui_element
    {
    public:
        explicit accent_picker_element(std::u8string_view selected);
        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;
    };
} // namespace chalkag
