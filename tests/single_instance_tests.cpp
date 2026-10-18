#include "app/single_instance.h"

#include <commctrl.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chalkag {
    namespace {
        constexpr DWORD primary_result { 11 };
        constexpr DWORD secondary_result { 12 };
        int failures {};

        void check(const bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failures;
            }
        }

        class signal final
        {
        public:
            signal(const std::wstring& identity, const wchar_t* suffix)
            {
                const auto name { L"Local\\" + identity + suffix };
                handle = CreateEventW(nullptr, TRUE, FALSE, name.c_str());

                if (handle == nullptr)
                {
                    throw std::runtime_error { "Cannot create test signal." };
                }
            }

            ~signal()
            {
                CloseHandle(handle);
            }

            signal(const signal&) = delete;
            signal& operator=(const signal&) = delete;
            HANDLE handle {};
        };

        class child final
        {
        public:
            child(const std::wstring& identity, const wchar_t* mode)
            {
                std::wstring executable(32768, L'\0');
                const DWORD size {
                    GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()))
                };

                if (size == 0 || size >= executable.size())
                {
                    throw std::runtime_error { "Cannot locate test executable." };
                }

                executable.resize(size);

                auto arguments { L"\"" + executable + L"\" " + mode + L" " + identity };
                STARTUPINFOW startup { sizeof(startup) };
                PROCESS_INFORMATION process {};

                if (!CreateProcessW(
                        executable.c_str(),
                        arguments.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startup,
                        &process
                    ))
                {
                    throw std::runtime_error { "Cannot start test child." };
                }

                handle = process.hProcess;
                CloseHandle(process.hThread);
            }

            ~child()
            {
                if (WaitForSingleObject(handle, 0) == WAIT_TIMEOUT)
                {
                    // 실패한 검사에서 직접 만든 자식 프로세스만 정리한다.
                    TerminateProcess(handle, EXIT_FAILURE);
                    WaitForSingleObject(handle, 5000);
                }

                CloseHandle(handle);
            }

            child(const child&) = delete;
            child& operator=(const child&) = delete;

            [[nodiscard]] DWORD result() const noexcept
            {
                DWORD result {};

                if (!GetExitCodeProcess(handle, &result))
                {
                    return EXIT_FAILURE;
                }

                return result;
            }

            HANDLE handle {};
        };

        LRESULT CALLBACK activation_observer(
            const HWND window,
            const UINT message,
            const WPARAM wparam,
            const LPARAM lparam,
            UINT_PTR,
            const DWORD_PTR reference
        )
        {
            if (message == RegisterWindowMessageW(L"Chalkag.Activate.Command.v1") && !IsWindowVisible(window))
            {
                SetEvent(reinterpret_cast<HANDLE>(reference));
            }

            return DefSubclassProc(window, message, wparam, lparam);
        }

        int run_child(const std::wstring& identity, const std::wstring_view mode)
        {
            signal ready { identity, L".ready" };
            signal stop { identity, L".stop" };
            signal activated { identity, L".activated" };
            single_instance instance { identity };

            if (!instance.valid())
            {
                return EXIT_FAILURE;
            }

            if (!instance.start_or_activate(mode == L"--probe" ? 1500 : 0))
            {
                return secondary_result;
            }

            if (mode == L"--probe")
            {
                return primary_result;
            }

            SetEvent(ready.handle);

            if (mode != L"--delayed-window")
            {
                return WaitForSingleObject(stop.handle, 10000) == WAIT_OBJECT_0 ? primary_result : EXIT_FAILURE;
            }

            // 창 생성 전의 중복 실행과 숨긴 창의 활성화 메시지를 실제 프로세스 사이에서 검사한다.
            Sleep(250);

            const auto window { CreateWindowExW(
                0,
                L"STATIC",
                L"chalkag instance test",
                WS_POPUP | WS_MINIMIZE,
                0,
                0,
                1,
                1,
                nullptr,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr
            )
            };

            if (window == nullptr || !instance.bind_window(window))
            {
                return EXIT_FAILURE;
            }

            check(IsIconic(window) && !IsWindowVisible(window), "activation fixture starts minimized and hidden");
            SetWindowSubclass(window, activation_observer, 9, reinterpret_cast<DWORD_PTR>(activated.handle));

            while (MsgWaitForMultipleObjects(1, &stop.handle, FALSE, 10000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
            {
                MSG message {};

                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    DispatchMessageW(&message);
                }
            }

            check(!IsWindowVisible(window), "activation preserves capture-hidden windows");
            DestroyWindow(window);
            return failures == 0 ? primary_result : EXIT_FAILURE;
        }

        void test_normal_release(const std::wstring& identity)
        {
            signal ready { identity, L".ready" };
            signal stop { identity, L".stop" };
            child owner { identity, L"--owner" };
            check(WaitForSingleObject(ready.handle, 5000) == WAIT_OBJECT_0, "first process owns the instance lock");
            single_instance next { identity };
            check(
                next.valid() && !next.start_or_activate(0), "another process cannot start before owner creates a window"
            );
            SetEvent(stop.handle);
            check(
                WaitForSingleObject(owner.handle, 5000) == WAIT_OBJECT_0 && owner.result() == primary_result,
                "owner exits normally"
            );
            check(next.start_or_activate(0), "normal exit releases the lock even with another open handle");
        }

        void test_abandoned_lock(const std::wstring& identity)
        {
            signal ready { identity, L".ready" };
            child owner { identity, L"--owner" };
            check(WaitForSingleObject(ready.handle, 5000) == WAIT_OBJECT_0, "crash test owner is ready");
            single_instance replacement { identity };
            check(replacement.valid() && !replacement.start_or_activate(0), "replacement waits for its owner");
            TerminateProcess(owner.handle, 99);
            check(WaitForSingleObject(owner.handle, 5000) == WAIT_OBJECT_0, "test owner has stopped unexpectedly");
            check(replacement.start_or_activate(0), "abandoned lock permits restarting without stale instance state");
        }

        void test_startup_activation(const std::wstring& identity)
        {
            signal ready { identity, L".ready" };
            signal stop { identity, L".stop" };
            signal activated { identity, L".activated" };
            child owner { identity, L"--delayed-window" };
            check(WaitForSingleObject(ready.handle, 5000) == WAIT_OBJECT_0, "delayed window owner is ready");
            child duplicate { identity, L"--probe" };
            check(
                WaitForSingleObject(duplicate.handle, 5000) == WAIT_OBJECT_0 && duplicate.result() == secondary_result,
                "duplicate waits for startup and exits without another instance"
            );
            check(
                WaitForSingleObject(activated.handle, 5000) == WAIT_OBJECT_0,
                "existing window receives activation after delayed creation"
            );
            SetEvent(stop.handle);
            check(
                WaitForSingleObject(owner.handle, 5000) == WAIT_OBJECT_0 && owner.result() == primary_result,
                "activation keeps a capture-hidden owner hidden"
            );
        }

        void test_concurrent_start(const std::wstring& identity)
        {
            signal ready { identity, L".ready" };
            signal stop { identity, L".stop" };

            std::vector<std::unique_ptr<child>> children {};

            for (int index {}; index < 8; ++index)
            {
                children.push_back(std::make_unique<child>(identity, L"--owner"));
            }

            check(WaitForSingleObject(ready.handle, 5000) == WAIT_OBJECT_0, "concurrent startup elects an owner");

            const auto deadline { GetTickCount64() + 5000 };
            std::size_t running {};
            do
            {
                running = 0;

                for (const auto& process : children)
                {
                    running += process->result() == STILL_ACTIVE ? 1 : 0;
                }

                if (running <= 1 || GetTickCount64() >= deadline)
                {
                    break;
                }

                Sleep(20);
            } while (true);
            check(running == 1, "eight competing processes leave exactly one running instance");
            SetEvent(stop.handle);

            std::size_t owners {};
            std::size_t duplicates {};

            for (const auto& process : children)
            {
                check(
                    WaitForSingleObject(process->handle, 5000) == WAIT_OBJECT_0,
                    "concurrent process terminates promptly"
                );
                owners += process->result() == primary_result ? 1 : 0;
                duplicates += process->result() == secondary_result ? 1 : 0;
            }

            check(owners == 1 && duplicates == 7, "single ownership survives concurrent launch races");
        }

    } // namespace
} // namespace chalkag

int wmain(const int count, const wchar_t* arguments[])
{
    try
    {
        if (count == 3)
        {
            return chalkag::run_child(arguments[2], arguments[1]);
        }

        const auto identity { L"Chalkag.Instance.Tests." + std::to_wstring(GetCurrentProcessId()) + L"."
                              + std::to_wstring(GetTickCount64())
        };
        chalkag::test_normal_release(identity + L".release");
        chalkag::test_abandoned_lock(identity + L".crash");
        chalkag::test_startup_activation(identity + L".startup");
        chalkag::test_concurrent_start(identity + L".race");

        if (chalkag::failures == 0)
        {
            std::cout << "Single instance tests passed.\n";
        }

        return chalkag::failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
