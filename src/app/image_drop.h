#pragma once

#include "luil/ui/ui_element.h"

namespace chalkag {
    [[nodiscard]] bool editable_image_path(std::u8string_view path) noexcept;
    [[nodiscard]] luil::drop_target image_file_drop_target();

    class image_drop_element final : public luil::ui_element
    {
    public:
        explicit image_drop_element(std::u8string owner, bool persistent = false);
        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] const luil::ui_element* hit_test(float x, float y) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        bool persistent_ {};
    };
} // namespace chalkag
