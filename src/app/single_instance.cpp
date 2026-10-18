#include "app/single_instance.h"

#include <commctrl.h>
#include <sddl.h>

#include <memory>
#include <vector>

namespace chalkag {
    namespace {
        struct close_handle
        {
            void operator()(void* value) const noexcept
            {
                if (value != nullptr)
                {
                    CloseHandle(value);
                }
            }
        };

        struct free_local
        {
            void operator()(void* value) const noexcept
            {
                if (value != nullptr)
                {
                    LocalFree(value);
                }
            }
        };

        [[nodiscard]] std::wstring current_user_sid()
        {
            HANDLE token {};

            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                return {};
            }

            const std::unique_ptr<void, close_handle> token_scope { token };
            DWORD size {};
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);

            if (size == 0)
            {
                return {};
            }

            std::vector<BYTE> storage(size);

            if (!GetTokenInformation(token, TokenUser, storage.data(), size, &size))
            {
                return {};
            }

            wchar_t* text {};

            if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(storage.data())->User.Sid, &text))
            {
                return {};
            }

            const std::unique_ptr<void, free_local> text_scope { text };
            return text;
        }

        struct window_search
        {
            const wchar_t* name {};
            HWND result {};
        };

        BOOL CALLBACK find_instance_window(const HWND window, const LPARAM reference)
        {
            auto& search { *reinterpret_cast<window_search*>(reference) };

            if (GetPropW(window, search.name) == nullptr)
            {
                return TRUE;
            }

            search.result = window;
            return FALSE;
        }

    } // namespace

    single_instance::single_instance(const std::wstring_view identity)
    {
        const auto sid { current_user_sid() };

        if (sid.empty())
        {
            return;
        }

        name_ = L"Local\\" + std::wstring { identity } + L"." + sid;

        // 동일 사용자의 일반 실행과 관리자 실행이 같은 잠금을 열 수 있도록 중간 무결성을 사용한다.
        const auto descriptor { L"D:P(A;;0x00100001;;;" + sid + L")S:(ML;;NW;;;ME)" };
        PSECURITY_DESCRIPTOR security {};

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                descriptor.c_str(), SDDL_REVISION_1, &security, nullptr
            ))
        {
            return;
        }

        const std::unique_ptr<void, free_local> security_scope { security };
        SECURITY_ATTRIBUTES attributes { sizeof(attributes), security, FALSE };
        activation_message_ = RegisterWindowMessageW(L"Chalkag.Activate.Command.v1");

        if (activation_message_ == 0)
        {
            return;
        }

        mutex_ = CreateMutexExW(&attributes, name_.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);

        if (mutex_ != nullptr)
        {
            owned_ = acquire();
        }
    }

    single_instance::~single_instance()
    {
        if (window_ != nullptr)
        {
            RemovePropW(window_, name_.c_str());
            RemoveWindowSubclass(window_, window_proc, reinterpret_cast<UINT_PTR>(this));
        }

        if (owned_)
        {
            ReleaseMutex(mutex_);
        }

        if (mutex_ != nullptr)
        {
            CloseHandle(mutex_);
        }
    }

    bool single_instance::valid() const noexcept
    {
        return mutex_ != nullptr;
    }

    bool single_instance::acquire() noexcept
    {
        if (owned_)
        {
            return true;
        }

        const DWORD result { WaitForSingleObject(mutex_, 0) };
        owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        return owned_;
    }

    bool single_instance::start_or_activate(const DWORD timeout_ms) noexcept
    {
        if (!valid())
        {
            return false;
        }

        const auto started { GetTickCount64() };
        do
        {
            if (acquire())
            {
                return true;
            }

            window_search search { name_.c_str() };
            EnumWindows(find_instance_window, reinterpret_cast<LPARAM>(&search));

            if (search.result != nullptr)
            {
                DWORD process {};
                GetWindowThreadProcessId(search.result, &process);

                // 다시 실행한 프로세스의 전경 전환 권한을 기존 창에 전달한다.
                AllowSetForegroundWindow(process);

                if (PostMessageW(search.result, activation_message_, 0, 0))
                {
                    return false;
                }
            }

            if (GetTickCount64() - started >= timeout_ms)
            {
                return false;
            }

            // 첫 프로세스가 아직 창을 만들거나 종료하는 중이면 잠금과 창을 다시 확인한다.
            Sleep(25);
        } while (true);
    }

    bool single_instance::bind_window(const HWND window) noexcept
    {
        if (!owned_ || window_ != nullptr || window == nullptr)
        {
            return false;
        }

        if (!SetWindowSubclass(
                window, window_proc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)
            ))
        {
            return false;
        }

        if (!SetPropW(window, name_.c_str(), reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(GetCurrentProcessId()))))
        {
            RemoveWindowSubclass(window, window_proc, reinterpret_cast<UINT_PTR>(this));
            return false;
        }

        window_ = window;

        // 일반 실행에서 관리자 창을 복원할 때에도 이 무인자 메시지만 허용한다.
        ChangeWindowMessageFilterEx(window, activation_message_, MSGFLT_ALLOW, nullptr);
        return true;
    }

    LRESULT CALLBACK single_instance::window_proc(
        const HWND window,
        const UINT message,
        const WPARAM wparam,
        const LPARAM lparam,
        UINT_PTR,
        const DWORD_PTR reference
    ) noexcept
    {
        auto& self { *reinterpret_cast<single_instance*>(reference) };

        if (message == self.activation_message_)
        {
            // 캡쳐 제외를 지원하지 않아 숨긴 창은 촬영 중 다시 노출하지 않는다.
            if (IsWindowVisible(window))
            {
                if (IsIconic(window))
                {
                    ShowWindow(window, SW_RESTORE);
                }

                SetForegroundWindow(window);
            }

            return 0;
        }

        if (message == WM_NCDESTROY)
        {
            RemovePropW(window, self.name_.c_str());
            RemoveWindowSubclass(window, window_proc, reinterpret_cast<UINT_PTR>(&self));
            self.window_ = nullptr;
        }

        return DefSubclassProc(window, message, wparam, lparam);
    }

} // namespace chalkag
