#pragma once

#include "capture/region_geometry.h"
#include "luil/theme/appearance.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace luil {
    class app_host;
} // namespace luil

namespace chalkag {
    // 모든 창·입력 훅은 luil의 UI 스레드에서 생성하고 정리한다.
    class region_controller final
    {
    public:
        // 영역 편집 요청은 프레임을 소유한 UI 스레드에서 전달한다.
        explicit region_controller(
            luil::app_host& host, std::function<void()> edit_request = {}, bool manage_selection_hotkey = true
        );
        ~region_controller();
        region_controller(const region_controller&) = delete;
        region_controller& operator=(const region_controller&) = delete;

        void begin_selection();
        void cancel_selection();
        [[nodiscard]] std::optional<capture_region> current_region() const;
        // 드래그로 영역을 고르는 중이다.
        [[nodiscard]] bool selecting() const;
        void set_region(capture_region region);
        void set_appearance(const luil::appearance_settings& appearance);
        void set_glow_width(int width);
        // 영역 다시 선택 메뉴와 단축키 충돌 안내에 쓰는 표기다. 빈 문자열이면 단축키를 표시하지 않는다.
        void set_selection_shortcut_text(std::u8string text);
        // 캡쳐 중에는 영역 좌표를 고정하고 프레임을 산출물에서 제외한다.
        [[nodiscard]] bool set_capture_active(bool active);
        void set_recording_elapsed(std::uint64_t seconds);

    private:
        class impl;
        std::unique_ptr<impl> impl_;
    };
} // namespace chalkag
