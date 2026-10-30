#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace chalkag {
    // 다시 실행했는데 창이 보이지 않으면(알림 영역에 숨김) 이 메시지로 앱에 보이기를 맡긴다.
    inline constexpr UINT single_instance_show_message { WM_APP + 2 };

    // 실제 앱의 진입점에서만 사용한다. 테스트·UI 미리보기에는 제한을 적용하지 않는다.
    class single_instance final
    {
    public:
        explicit single_instance(std::wstring_view identity = L"Chalkag.Command.Instance.v1");
        ~single_instance();
        single_instance(const single_instance&) = delete;
        single_instance& operator=(const single_instance&) = delete;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] bool start_or_activate(DWORD timeout_ms = 5000) noexcept;
        [[nodiscard]] bool bind_window(HWND window) noexcept;

    private:
        [[nodiscard]] bool acquire() noexcept;
        static LRESULT CALLBACK window_proc(
            HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR subclass, DWORD_PTR reference
        ) noexcept;

        std::wstring name_ {};
        HANDLE mutex_ {};
        HWND window_ {};
        UINT activation_message_ {};
        bool owned_ {};
    };
} // namespace chalkag
