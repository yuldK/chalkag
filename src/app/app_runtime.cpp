#include "app/app_runtime.h"

#include "app/clipboard.h"
#include "app/command_window.h"
#include "app/resource_ids.h"
#include "app/single_instance.h"
#include "capture/image_capture.h"
#include "capture/region_controller.h"
#include "luil/app/app_host.h"
#include "luil/ui/ui_tree.h"

#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace chalkag {
    namespace {
        capture_frame_source make_frame_source()
        {
#if defined(CHALKAG_TEST_APPLICATION)
            // 테스트 앱만 합성 프레임을 주입해 사용자 화면 없이 실제 촬영 흐름을 검증한다.
            if (GetEnvironmentVariableW(L"CHALKAG_TEST_SYNTHETIC_CAPTURE", nullptr, 0) > 0)
            {
                return [](const capture_region region, const capture_options options, SkBitmap& bitmap) {
                    const int width {
                        static_cast<int>(static_cast<std::int64_t>(region.width) * options.output_percent / 100)
                    };
                    const int height {
                        static_cast<int>(static_cast<std::int64_t>(region.height) * options.output_percent / 100)
                    };
                    if (!bitmap.tryAllocPixels(
                            SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType)
                        ))
                    {
                        return false;
                    }
                    static std::atomic<unsigned> sequence {};
                    bitmap.eraseColor(
                        sequence.fetch_add(1) % 2 ? SkColorSetRGB(35, 72, 110) : SkColorSetRGB(55, 100, 140)
                    );
                    return true;
                };
            }
#endif
            return {};
        }

        [[nodiscard]] std::filesystem::path executable_directory()
        {
            std::wstring path(32768, L'\0');
            const DWORD size { GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())) };

            if (size == 0 || size >= path.size())
            {
                throw std::runtime_error { "Cannot locate executable." };
            }

            path.resize(size);
            return std::filesystem::path { path }.parent_path();
        }

        [[nodiscard]] HWND find_owner()
        {
            HWND window {};
            EnumThreadWindows(
                GetCurrentThreadId(),
                [](const HWND candidate, const LPARAM result) -> BOOL {
                    wchar_t name[128] {};
                    GetClassNameW(candidate, name, 128);

                    if (std::wstring_view { name } == L"Chalkag.Command.Window")
                    {
                        *reinterpret_cast<HWND*>(result) = candidate;
                        return FALSE;
                    }

                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&window)
            );
            return window;
        }

        [[nodiscard]] int number(const std::u8string& text)
        {
            if (text.empty() || text.size() > 3)
            {
                return 0;
            }

            int result {};

            for (const char8_t digit : text)
            {
                if (digit < u8'0' || digit > u8'9')
                {
                    return 0;
                }

                result = result * 10 + (digit - u8'0');
            }

            return result;
        }

        // 알림 영역 아이콘이 보내는 메시지다.
        constexpr UINT tray_message { WM_APP + 1 };

        [[nodiscard]] std::u8string utf8_number(const int value)
        {
            const std::string text { std::to_string(value) };
            return { text.begin(), text.end() };
        }

    } // namespace

    class app_runtime::impl final
    {
    public:
        explicit impl(luil::app_host& host)
            : host_ { &host }
            , owner_ { find_owner() }
            , hotkeys_ { owner_ }
            , directory_ { executable_directory() }
            , store_ { directory_ }
            , capture_ { directory_, make_frame_source() }
            , region_ { host,
                        [this] {
                            execute(
                                luil::app_ui_command {
                                    .command = static_cast<std::uint32_t>(runtime_command::show_region_editor)
                                }
                            );
                        },
                        false
            }
        {
            if (owner_ == nullptr || !SetWindowSubclass(owner_, owner_proc, 2, reinterpret_cast<DWORD_PTR>(this)))
            {
                throw std::runtime_error { "Cannot initialize capture commands." };
            }

            try
            {
                state_.settings = store_.load(state_.status);

                constexpr std::array scales { 25, 50, 75, 100, 150, 200 };
                state_.output_step = static_cast<float>(
                    std::find(scales.begin(), scales.end(), state_.settings.capture.output_percent) - scales.begin()
                );
                state_.settings_writable = state_.status.empty();
                restore_window_position();
                tracking_position_ = true;
                observe_window_position();
                apply_topmost(state_.settings.always_on_top);
                region_.set_appearance(state_.settings.appearance);
                region_.set_glow_width(state_.settings.glow_width);
                state_.glow_position = static_cast<float>(state_.settings.glow_width);
                state_.quality_position = static_cast<float>(state_.settings.capture.webp_quality);
                state_.shortcut_error = hotkeys_.initialize(state_.settings.shortcuts);
                region_.set_selection_shortcut_text(format_hotkey(state_.settings.shortcuts[0]));
                state_.status += state_.shortcut_error;

                if (state_.status.empty())
                {
                    state_.status = u8"영역 선택: " + shortcut_name(0) + u8" · 사진: " + shortcut_name(1)
                        + u8" · 녹화/정지: " + shortcut_name(2);
                }

                SetTimer(owner_, 200, 100, nullptr);
                update_tray_icon();
                publish();
            }
            catch (...)
            {
                shutdown();
                throw;
            }
        }

        ~impl()
        {
            shutdown();
        }

        void execute(const luil::app_ui_command& command)
        {
            try
            {
                execute_command(command);
            }
            catch (const std::exception&)
            {
                if (!job_.valid())
                {
                    state_.busy = false;
                    state_.recording = capture_.recording();

                    // 실행 스레드 생성 실패 시에도 캡쳐 제외·숨김 상태를 즉시 복원한다.
                    if (!state_.recording)
                    {
                        restore_capture_windows();
                    }
                }

                fail(u8"작업을 시작할 수 없습니다. 메모리와 실행 파일 옆 data 폴더의 쓰기 권한을 확인하세요.");
            }
        }

    private:
        void restore_window_position()
        {
            if (!state_.settings.window_position)
            {
                return;
            }

            const auto position { *state_.settings.window_position };
            const POINT point { position.x, position.y };
            MONITORINFO monitor { sizeof(monitor) };

            if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor))
            {
                return;
            }

            // 첫 이동에서 DPI가 바뀌면 실제 창 크기를 다시 읽어 작업 영역 안으로 맞춘다.
            for (int pass {}; pass < 2; ++pass)
            {
                RECT bounds {};

                if (!GetWindowRect(owner_, &bounds))
                {
                    return;
                }

                const int x { std::clamp(
                    position.x,
                    static_cast<int>(monitor.rcWork.left),
                    static_cast<int>(std::max(monitor.rcWork.left, monitor.rcWork.right - (bounds.right - bounds.left)))
                )
                };
                const int y { std::clamp(
                    position.y,
                    static_cast<int>(monitor.rcWork.top),
                    static_cast<int>(std::max(monitor.rcWork.top, monitor.rcWork.bottom - (bounds.bottom - bounds.top)))
                )
                };
                SetWindowPos(owner_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }

        void observe_window_position()
        {
            if (!tracking_position_ || !state_.settings_writable || IsIconic(owner_) || IsZoomed(owner_))
            {
                return;
            }

            RECT bounds {};

            if (!GetWindowRect(owner_, &bounds))
            {
                return;
            }

            const command_window_position position { static_cast<int>(bounds.left), static_cast<int>(bounds.top) };

            if (state_.settings.window_position == position)
            {
                return;
            }

            state_.settings.window_position = position;
            position_dirty_ = true;
            position_changed_ = std::chrono::steady_clock::now();
        }

        void save_window_position()
        {
            if (!position_dirty_ || !state_.settings_writable)
            {
                return;
            }

            position_dirty_ = false;

            const auto error { store_.save(state_.settings) };

            if (!error.empty())
            {
                state_.status = error;
                publish();
            }
        }

        // 등록할 수 있었던단축키만 저장하고 저장 실패 시 이전 등록으로 되돌린다.
        void apply_shortcuts(const std::u8string& argument)
        {
            if (state_.busy || state_.recording || !state_.settings_writable)
            {
                return;
            }

            const auto bindings { decode_hotkeys(argument) };

            if (!bindings)
            {
                state_.shortcut_error = u8"단축키 조합이 잘못되었거나 서로 중복됩니다.";
            }
            else
            {
                state_.shortcut_error = hotkeys_.apply(*bindings);

                if (state_.shortcut_error.empty())
                {
                    app_settings next { state_.settings };
                    next.shortcuts = *bindings;
                    state_.shortcut_error = store_.save(next);

                    if (state_.shortcut_error.empty())
                    {
                        state_.settings = std::move(next);
                        region_.set_selection_shortcut_text(format_hotkey(state_.settings.shortcuts[0]));
                    }
                    else
                    {
                        const auto rollback { hotkeys_.apply(state_.settings.shortcuts) };

                        if (!rollback.empty())
                        {
                            state_.shortcut_error += u8" " + rollback;
                        }
                    }
                }
            }

            state_.status = state_.shortcut_error.empty() ? u8"단축키를 적용하고 저장했습니다." : state_.shortcut_error;
            ++state_.shortcut_revision;
            publish();
        }

        // 실제 창 상태를 먼저 변경한 뒤 설정을 저장하며 실패하면 창 상태도 복구한다.
        void toggle_always_on_top()
        {
            if (!state_.settings_writable)
            {
                return;
            }

            app_settings next { state_.settings };
            next.always_on_top = !next.always_on_top;

            if (!apply_topmost(next.always_on_top))
            {
                notify(u8"명령창의 항상 위 상태를 바꿀 수 없습니다.");
                return;
            }

            const auto error { store_.save(next) };

            if (!error.empty())
            {
                apply_topmost(state_.settings.always_on_top);
                fail(error);
                return;
            }

            state_.settings = std::move(next);
            publish();
        }

        // 펼침 크기를 DPI에 맞추고 현재 모니터 작업 영역 안으로 창 위치를 보정한다.
        void toggle_options()
        {
            state_.options_open = !state_.options_open;

            const UINT dpi { GetDpiForWindow(owner_) };
            MONITORINFO monitor { sizeof(MONITORINFO) };

            const auto current_monitor { MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST) };
            GetMonitorInfoW(current_monitor, &monitor);
            RECT previous {};
            GetWindowRect(owner_, &previous);

            const int window_width {
                MulDiv(state_.options_open ? options_window_width : command_window_width, static_cast<int>(dpi), 96)
            };

            const int work_left { monitor.rcWork.left };
            const int work_top { monitor.rcWork.top };
            const int work_right { monitor.rcWork.right };
            const int work_bottom { monitor.rcWork.bottom };
            const int desired_height {
                MulDiv(state_.options_open ? options_window_height : command_window_height, static_cast<int>(dpi), 96)
            };

            const int window_height { std::min(desired_height, work_bottom - work_top) };
            const int maximum_x { std::max(work_left, work_right - window_width) };
            const int maximum_y { std::max(work_top, work_bottom - window_height) };
            const int x { std::clamp(static_cast<int>(previous.left), work_left, maximum_x) };
            const int y { std::clamp(static_cast<int>(previous.top), work_top, maximum_y) };
            SetWindowPos(owner_, nullptr, x, y, window_width, window_height, SWP_NOZORDER | SWP_NOACTIVATE);
            publish();
        }

        // 실행 파일 옆 저장 폴더를 준비한 뒤 Windows 탐색기로 연다.
        void open_output_folder()
        {
            std::error_code error {};
            const auto folder { directory_ / L"data" / L"captures" };
            std::filesystem::create_directories(folder, error);

            if (error
                || reinterpret_cast<INT_PTR>(
                       ShellExecuteW(owner_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
                   ) <= 32)
            {
                notify(u8"저장 폴더를 열 수 없습니다. 설치 경로의 쓰기 권한을 확인하세요.");
            }

            return;
        }

        // 파일이 남아 있는지 확인하고 연결된 Windows 이미지 앱으로 연다.
        void open_saved_file()
        {
            if (state_.saved_output.empty())
            {
                return;
            }

            std::error_code error {};

            if (!std::filesystem::is_regular_file(state_.saved_output, error) || error)
            {
                notify(u8"저장 파일을 찾을 수 없습니다. 파일이 이동되거나 삭제되었는지 확인하세요.");
            }
            else if (
                reinterpret_cast<INT_PTR>(
                    ShellExecuteW(owner_, L"open", state_.saved_output.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
                )
                <= 32
            )
            {
                notify(u8"저장 파일을 열 수 없습니다. 이 이미지 형식을 열 수 있는 기본 앱을 확인하세요.");
            }

            return;
        }

        // 원본 마무리와 선택적인 자동 저장은 백그라운드에서 순서대로 수행한다.
        void stop_recording()
        {
            state_.recording = false;
            state_.busy = true;
            reset_progress();

            const bool auto_save { state_.settings.auto_save };
            saving_recording_ = auto_save;
            screenshot_job_ = false;
            state_.recording_saved = false;
            state_.save_cancel_requested = false;
            const auto options { state_.settings.capture };
            state_.status = auto_save ? u8"녹화 마무리·저장 중…" : u8"녹화 원본을 마무리하는 중…";
            publish();
            job_ = std::async(std::launch::async, [this, auto_save, options] {
                const auto result { capture_.stop_recording(false) };
                return result.success && auto_save ? capture_.save_recording(options, progress_observer()) : result;
            });
            return;
        }

        // 현재 출력 옵션의 복사본으로 저장하여 작업 중 설정 변경의 영향을 막는다.
        void save_recording()
        {
            if (!state_.pending_recording)
            {
                return;
            }

            const auto options { state_.settings.capture };
            state_.busy = true;
            saving_recording_ = true;
            screenshot_job_ = false;
            state_.save_cancel_requested = false;
            reset_progress();
            state_.status = u8"보관한 녹화를 저장하는 중… 원본은 유지됩니다.";
            publish();
            job_ = std::async(std::launch::async, [this, options] {
                return capture_.save_recording(options, progress_observer());
            });
            return;
        }

        void execute_command(const luil::app_ui_command& command)
        {
            if (host_ == nullptr)
            {
                return;
            }

            hide_help();

            const auto action { static_cast<runtime_command>(command.command) };

            if (action == runtime_command::dismiss_dialog || action == runtime_command::confirm_dialog
                || action == runtime_command::save_and_exit)
            {
                const auto revision { std::to_string(state_.dialog.revision) };

                if (state_.dialog.kind == message_dialog_kind::none
                    || command.argument != std::u8string { revision.begin(), revision.end() })
                {
                    return;
                }

                const auto kind { state_.dialog.kind };
                state_.dialog = {};

                if (action == runtime_command::dismiss_dialog)
                {
                    state_.quit_requested = false;
                }

                // 저장 후 종료는 저장이 끝난 뒤 창을 닫는다. 실패하면 안내만 남고 앱은 계속 돈다.
                if (action == runtime_command::save_and_exit && kind == message_dialog_kind::exit_application
                    && !state_.busy && !state_.recording)
                {
                    exit_after_job_ = true;
                    save_recording();
                    return;
                }

                if (action == runtime_command::confirm_dialog && kind == message_dialog_kind::exit_while_saving
                    && state_.busy)
                {
                    exit_after_job_ = true;
                    capture_.request_save_cancel();
                    state_.save_cancel_requested = true;
                    state_.status = u8"저장을 취소하고 종료하는 중…";
                    publish();
                    return;
                }

                if (action == runtime_command::confirm_dialog
                    && (kind == message_dialog_kind::discard_recording
                        || kind == message_dialog_kind::replace_recording)
                    && !state_.busy && !state_.recording)
                {
                    capture_.discard_recording();
                    state_.pending_recording = false;
                    state_.recording_saved = false;
                    state_.recorded_region = {};
                    state_.recorded_duration = 0;
                    state_.saved_summary.clear();
                    state_.status = u8"녹화 원본을 비웠습니다. 새 녹화를 시작할 수 있습니다.";

                    if (kind == message_dialog_kind::replace_recording)
                    {
                        execute_command({ static_cast<std::uint32_t>(runtime_command::toggle_recording) });
                        return;
                    }
                }

                publish();

                return;
            }

            if (state_.dialog.kind != message_dialog_kind::none
                && !(action == runtime_command::toggle_recording && state_.recording))
            {
                return;
            }

            if (action == runtime_command::set_shortcuts)
            {
                apply_shortcuts(command.argument);
                return;
            }

            if (action == runtime_command::toggle_always_on_top)
            {
                toggle_always_on_top();
                return;
            }

            if (action == runtime_command::toggle_options)
            {
                toggle_options();
                return;
            }

            if (action == runtime_command::show_output_options || action == runtime_command::show_capture_options)
            {
                if (!state_.options_open)
                {
                    toggle_options();
                }

                host_->post_app_message(
                    luil::app_message { options_tab_intent { action == runtime_command::show_output_options
                                                                 ? options_page::output
                                                                 : options_page::capture
                    }
                    }
                );
                return;
            }

            if (action == runtime_command::open_output_folder)
            {
                open_output_folder();
                return;
            }

            if (action == runtime_command::open_saved_file)
            {
                open_saved_file();
                return;
            }

            if (action == runtime_command::open_editor_output)
            {
                const std::filesystem::path output { command.argument };

                if (output.parent_path() == directory_ / L"data" / L"captures")
                {
                    state_.saved_output = output;
                    open_saved_file();
                }

                return;
            }

            if (action == runtime_command::request_close)
            {
                show_from_tray();

                if (state_.recording)
                {
                    show_dialog(
                        message_dialog_kind::information,
                        u8"녹화 중입니다",
                        u8"녹화를 정지한 뒤 종료하세요. 정지 단축키: " + shortcut_name(2)
                    );
                }
                else if (state_.busy)
                {
                    show_dialog(
                        message_dialog_kind::exit_while_saving,
                        u8"저장 중입니다",
                        state_.pending_recording
                            ? u8"저장을 취소하고 종료하면 보관한 녹화 원본도 비워집니다. 이미 저장한 파일은 유지됩니다."
                            : u8"저장을 취소하고 종료하면 지금 저장 중인 이미지는 만들어지지 않습니다."
                    );
                }
                else if (state_.pending_recording && !state_.recording_saved)
                {
                    show_dialog(
                        message_dialog_kind::exit_application,
                        u8"저장하지 않은 녹화가 있습니다",
                        u8"지금 저장하면 내보내기 탭의 형식·크기·품질로 파일을 만든 뒤 종료합니다. 버리면 보관한 "
                        u8"원본이 "
                        u8"비워집니다."
                    );
                }
                else
                {
                    request_exit();
                }
                return;
            }

            if (action == runtime_command::cancel_save)
            {
                if (state_.busy)
                {
                    capture_.request_save_cancel();
                    state_.save_cancel_requested = true;
                    state_.status = u8"저장을 취소하는 중… 원본은 유지됩니다.";
                    publish();
                }

                return;
            }

            if (action == runtime_command::copy_saved_output)
            {
                copy_saved_output(true);
                return;
            }

            if (action == runtime_command::hide_to_tray)
            {
                hide_to_tray();
                return;
            }

            if (action == runtime_command::show_window)
            {
                show_from_tray(true);
                return;
            }

            if (action == runtime_command::quit_application)
            {
                show_from_tray();
                state_.quit_requested = true;
                publish();
                execute_command({ static_cast<std::uint32_t>(runtime_command::request_close) });
                return;
            }

            if (action == runtime_command::cancel_countdown)
            {
                cancel_countdown();
                return;
            }

            if (state_.busy)
            {
                return;
            }

            if (action == runtime_command::toggle_recording && state_.recording)
            {
                stop_recording();
                return;
            }

            if (state_.recording)
            {
                return;
            }

            const bool capture_command { action == runtime_command::take_screenshot
                                         || action == runtime_command::toggle_recording
                                         || action == runtime_command::new_recording
            };

            // 기다리는 동안 같은 촬영 명령을 다시 누르면 취소하고, 다른 명령은 받지 않는다.
            if (state_.countdown > 0)
            {
                if (capture_command)
                {
                    cancel_countdown();
                }

                return;
            }

            // 녹화 단축키도 버튼과 같이 보관 원본이 있으면 새 녹화로 시작한다.
            if (action == runtime_command::toggle_recording && state_.pending_recording)
            {
                execute_command({ static_cast<std::uint32_t>(runtime_command::new_recording) });
                return;
            }

            if (capture_command && !state_.queued_capture.empty())
            {
                // 영역을 고르는 중에 다시 누르면 고른 뒤 할 일만 바꾼다.
                queue_capture(action);
                return;
            }

            if (action == runtime_command::new_recording)
            {
                if (!region_.current_region() && state_.recorded_region.valid())
                {
                    region_.set_region(state_.recorded_region);
                }

                if (!state_.pending_recording)
                {
                    execute_command({ static_cast<std::uint32_t>(runtime_command::toggle_recording) });
                    return;
                }

                if (!state_.recording_saved)
                {
                    show_dialog(
                        message_dialog_kind::replace_recording,
                        u8"새 녹화를 시작할까요?",
                        u8"아직 저장하지 않은 녹화가 있습니다. 새 녹화를 시작하면 보관 원본을 비웁니다. 현재 녹화를 "
                        u8"남기려면 취소한 뒤 저장하세요."
                    );
                    return;
                }

                capture_.discard_recording();
                state_.pending_recording = false;
                state_.recording_saved = false;
                state_.saved_summary.clear();
                execute_command({ static_cast<std::uint32_t>(runtime_command::toggle_recording) });
                return;
            }

            if (action == runtime_command::select_preset)
            {
                const int index { number(command.argument) };

                if (index >= 0 && index < static_cast<int>(state_.settings.presets.size()))
                {
                    state_.preset_index = index;
                    host_->post_app_message(luil::app_message { preset_menu_intent { false } });
                    publish();
                }

                return;
            }

            if (action == runtime_command::edit_recording || action == runtime_command::open_animation
                || action == runtime_command::show_animation_editor)
            {
                open_animation_editor_intent open {};
                open.output_directory = directory_ / L"data" / L"captures";
                open.options = state_.settings.capture;

                if (action == runtime_command::edit_recording)
                {
                    if (!state_.pending_recording)
                    {
                        return;
                    }

                    open.recording = capture_.snapshot();
                }
                else if (action == runtime_command::open_animation && !command.argument.empty())
                {
                    open.path = std::filesystem::path { command.argument };
                }
                else if (action == runtime_command::open_animation)
                {
                    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog {};

                    if (FAILED(
                            CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
                        ))
                    {
                        notify(u8"이미지 선택 창을 열 수 없습니다.");
                        return;
                    }

                    const COMDLG_FILTERSPEC filters[] { { L"이미지 파일", L"*.gif;*.webp;*.apng;*.png" } };
                    dialog->SetFileTypes(1, filters);
                    dialog->SetTitle(L"편집할 이미지 열기");

                    const auto picker_owner { GetLastActivePopup(owner_) };
                    const auto shown { dialog->Show(picker_owner != nullptr ? picker_owner : owner_) };

                    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                    {
                        return;
                    }

                    Microsoft::WRL::ComPtr<IShellItem> item {};
                    PWSTR path {};

                    if (FAILED(shown) || FAILED(dialog->GetResult(&item))
                        || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        notify(u8"선택한 이미지 경로를 읽을 수 없습니다.");
                        return;
                    }

                    open.path = path;
                    CoTaskMemFree(path);
                }

                host_->post_app_message(luil::app_message { std::move(open) });
                return;
            }

            if (action == runtime_command::save_recording)
            {
                save_recording();
                return;
            }

            if (action == runtime_command::discard_recording)
            {
                if (!state_.pending_recording)
                {
                    return;
                }

                show_dialog(
                    message_dialog_kind::discard_recording,
                    u8"녹화 비우기",
                    u8"보관한 녹화 원본을 비울까요?\n이미 저장한 파일은 유지됩니다."
                );
                return;
            }

            if (action == runtime_command::show_region_editor)
            {
                region_.cancel_selection();
                SetForegroundWindow(owner_);

                const capture_region bounds { screen_bounds() };
                capture_region initial { region_.current_region().value_or(capture_region {}) };

                if (!initial.valid())
                {
                    MONITORINFO monitor { sizeof(MONITORINFO) };
                    GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor);
                    initial.width = std::min(640, bounds.width);
                    initial.height = std::min(360, bounds.height);
                    initial.x = std::clamp(
                        static_cast<int>(
                            monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - initial.width) / 2
                        ),
                        bounds.x,
                        bounds.x + bounds.width - initial.width
                    );
                    initial.y = std::clamp(
                        static_cast<int>(
                            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - initial.height) / 2
                        ),
                        bounds.y,
                        bounds.y + bounds.height - initial.height
                    );
                }

                host_->post_app_message(luil::app_message { open_region_editor_intent { initial, bounds } });
                return;
            }

            if (action == runtime_command::apply_region_editor)
            {
                const auto selected { decode_region_arguments(command.argument) };

                if (!selected.has_value())
                {
                    return;
                }

                const auto validation { validate_region_draft(*selected, screen_bounds()) };

                if (!validation.region.has_value())
                {
                    return;
                }

                region_.set_region(*validation.region);
                close_editor();
                return;
            }

            if (action == runtime_command::select_region)
            {
                close_editor();
                region_.begin_selection();
                return;
            }

            if (action == runtime_command::take_screenshot || action == runtime_command::toggle_recording)
            {
                if (!region_.current_region())
                {
                    queue_capture(action);
                    return;
                }

                if (state_.settings.capture_delay > 0)
                {
                    start_countdown(action);
                    return;
                }

                start_capture(action);
                return;
            }

            if (action == runtime_command::previous_preset || action == runtime_command::next_preset)
            {
                const int count { static_cast<int>(state_.settings.presets.size()) };

                if (count > 0)
                {
                    state_.preset_index
                        = (state_.preset_index + (action == runtime_command::next_preset ? 1 : count - 1)) % count;
                }

                publish();
                return;
            }

            if (action == runtime_command::apply_preset)
            {
                region_.cancel_selection();
                close_editor();

                if (!state_.settings.presets.empty())
                {
                    region_.set_region(state_.settings.presets[static_cast<std::size_t>(state_.preset_index)].region);
                }

                return;
            }

            if (!state_.settings_writable)
            {
                fail(
                    u8"기존 설정 파일을 읽을 수 없어 저장을 중단했습니다. data/settings.json을 확인하거나 별도로 백업 "
                    u8"후 옮겨 주세요."
                );
                return;
            }

            app_settings next { state_.settings };
            float next_step { state_.output_step };

            if (action == runtime_command::set_glow_width || action == runtime_command::adjust_glow_width)
            {
                const std::string text { command.argument.begin(), command.argument.end() };
                float requested {};

                const auto parsed { std::from_chars(text.data(), text.data() + text.size(), requested) };

                if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() || !std::isfinite(requested))
                {
                    return;
                }

                const float position { std::clamp(
                    action == runtime_command::adjust_glow_width ? state_.glow_position + requested : requested,
                    0.0f,
                    40.0f
                )
                };

                const int value { static_cast<int>(position + 0.5f) };

                if (value == next.glow_width)
                {
                    state_.glow_position = position;
                    publish();
                    return;
                }

                state_.glow_position = position;
                next.glow_width = value;
            }
            else if (action == runtime_command::set_theme)
            {
                if (!luil::parse_theme_preference(command.argument, next.appearance.theme))
                {
                    return;
                }
            }
            else if (action == runtime_command::set_accent)
            {
                if (command.argument != luil::system_accent_id && !luil::accent_exists(command.argument))
                {
                    return;
                }

                next.appearance.accent_id = command.argument;
            }
            else if (action == runtime_command::save_preset)
            {
                const auto selected { region_.current_region() };

                if (!selected.has_value() || next.presets.size() >= 64)
                {
                    notify(u8"영역을 선택하세요. 프리셋은 최대 64개까지 저장할 수 있습니다.");
                    return;
                }

                SYSTEMTIME time {};
                GetLocalTime(&time);

                const auto name = u8"영역 " + utf8_number(static_cast<int>(next.presets.size()) + 1) + u8" · "
                    + utf8_number(selected->width) + u8"×" + utf8_number(selected->height) + u8" · "
                    + utf8_number(time.wHour) + u8":" + utf8_number(time.wMinute) + u8":" + utf8_number(time.wSecond)
                    + u8"." + utf8_number(time.wMilliseconds);
                next.presets.push_back({ name, *selected });
            }
            else if (action == runtime_command::delete_preset)
            {
                if (next.presets.empty())
                {
                    return;
                }

                next.presets.erase(next.presets.begin() + state_.preset_index);
            }
            else if (action == runtime_command::undo_delete_preset)
            {
                if (!deleted_preset_ || next.presets.size() >= 64)
                {
                    return;
                }

                if (std::any_of(next.presets.begin(), next.presets.end(), [&](const region_preset& preset) {
                        return preset.name == deleted_preset_->name;
                    }))
                {
                    return;
                }

                next.presets.insert(
                    next.presets.begin()
                        + static_cast<std::ptrdiff_t>(std::min(deleted_preset_index_, next.presets.size())),
                    *deleted_preset_
                );
            }
            else if (action == runtime_command::rename_preset)
            {
                if (next.presets.empty() || command.argument.empty())
                {
                    return;
                }

                next.presets[static_cast<std::size_t>(state_.preset_index)].name = command.argument;
            }
            else if (action == runtime_command::set_fps)
            {
                host_->post_app_message(luil::app_message { fps_menu_intent { false } });

                const int value { number(command.argument) };

                if (value != 10 && value != 16 && value != 24 && value != 30 && value != 33)
                {
                    return;
                }

                if (state_.pending_recording)
                {
                    return;
                }

                next.capture.fps = value;
            }
            else if (action == runtime_command::set_scale)
            {
                const int value { number(command.argument) };

                if (value != 25 && value != 50 && value != 75 && value != 100 && value != 150 && value != 200)
                {
                    return;
                }

                next.capture.output_percent = value;

                constexpr std::array scales { 25, 50, 75, 100, 150, 200 };
                next_step = static_cast<float>(std::find(scales.begin(), scales.end(), value) - scales.begin());
            }
            else if (action == runtime_command::adjust_scale || action == runtime_command::set_scale_index)
            {
                float value {};

                const std::string text { command.argument.begin(), command.argument.end() };
                const auto parsed { std::from_chars(text.data(), text.data() + text.size(), value) };

                if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
                {
                    return;
                }

                if (action == runtime_command::adjust_scale)
                {
                    // 드래그의 작은 델타를 누적하여 프리셋 경계를 넘을 때만 저장한다.
                    value += next_step;
                }

                next_step = std::clamp(value, 0.0f, 5.0f);

                constexpr std::array scales { 25, 50, 75, 100, 150, 200 };
                next.capture.output_percent = scales[static_cast<std::size_t>(std::lround(next_step))];

                if (next.capture.output_percent == state_.settings.capture.output_percent)
                {
                    state_.output_step = next_step;
                    publish();
                    return;
                }
            }
            else if (action == runtime_command::set_still_format)
            {
                next.capture.still_format = command.argument == u8"webp" ? image_format::webp : image_format::png;
            }
            else if (action == runtime_command::set_animation_format)
            {
                if (command.argument == u8"apng")
                {
                    next.capture.animation_format = image_format::apng;
                }
                else if (command.argument == u8"webp")
                {
                    next.capture.animation_format = image_format::webp;
                }
                else if (command.argument == u8"gif")
                {
                    next.capture.animation_format = image_format::gif;
                }
                else
                {
                    return;
                }
            }
            else if (action == runtime_command::toggle_auto_save)
            {
                next.auto_save = !next.auto_save;
            }
            else if (action == runtime_command::toggle_clipboard)
            {
                next.copy_to_clipboard = !next.copy_to_clipboard;
            }
            else if (action == runtime_command::toggle_close_to_tray)
            {
                next.close_to_tray = !next.close_to_tray;
            }
            else if (action == runtime_command::set_capture_delay)
            {
                const int value { number(command.argument) };

                if (command.argument.empty() || (value != 0 && value != 3 && value != 5 && value != 10))
                {
                    return;
                }

                next.capture_delay = value;
            }
            else if (action == runtime_command::set_webp_mode)
            {
                next.capture.webp_lossless = command.argument != u8"lossy";
            }
            else if (action == runtime_command::set_webp_quality || action == runtime_command::adjust_webp_quality)
            {
                float value {};

                const std::string text { command.argument.begin(), command.argument.end() };
                const auto parsed { std::from_chars(text.data(), text.data() + text.size(), value) };

                if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
                {
                    return;
                }

                if (action == runtime_command::adjust_webp_quality)
                {
                    value += state_.quality_position;
                }

                const auto position { std::clamp(value, 1.0f, 100.0f) };
                next.capture.webp_quality = static_cast<int>(std::lround(position));

                if (next.capture.webp_quality == state_.settings.capture.webp_quality)
                {
                    state_.quality_position = position;
                    publish();
                    return;
                }

                state_.quality_position = position;
            }
            else if (action == runtime_command::toggle_cursor)
            {
                if (state_.pending_recording)
                {
                    return;
                }

                next.capture.include_cursor = !next.capture.include_cursor;
            }
            else if (action == runtime_command::set_compression)
            {
                if (command.argument == u8"fast")
                {
                    next.capture.compression = lossless_compression::fast;
                }
                else if (command.argument == u8"small")
                {
                    next.capture.compression = lossless_compression::compact;
                }
                else if (command.argument == u8"maximum")
                {
                    next.capture.compression = lossless_compression::maximum;
                }
                else
                {
                    return;
                }
            }
            else
            {
                return;
            }

            const auto error { store_.save(next) };

            if (!error.empty())
            {
                state_.glow_position = static_cast<float>(state_.settings.glow_width);
                state_.quality_position = static_cast<float>(state_.settings.capture.webp_quality);
                fail(error);
                return;
            }

            if (action == runtime_command::delete_preset)
            {
                deleted_preset_ = state_.settings.presets[static_cast<std::size_t>(state_.preset_index)];
                deleted_preset_index_ = static_cast<std::size_t>(state_.preset_index);
            }
            state_.settings = std::move(next);

            if (action == runtime_command::delete_preset)
            {
                state_.can_undo_delete = true;
            }
            else if (action == runtime_command::undo_delete_preset)
            {
                deleted_preset_.reset();
                state_.can_undo_delete = false;
                state_.preset_index
                    = static_cast<int>(std::min(deleted_preset_index_, state_.settings.presets.size() - 1));
            }

            if (action == runtime_command::set_glow_width || action == runtime_command::adjust_glow_width)
            {
                region_.set_glow_width(state_.settings.glow_width);
            }

            if (action == runtime_command::set_theme || action == runtime_command::set_accent)
            {
                region_.set_appearance(state_.settings.appearance);
            }

            if (action == runtime_command::toggle_close_to_tray)
            {
                update_tray_icon();
            }

            state_.output_step = next_step;

            if (action == runtime_command::save_preset)
            {
                state_.preset_index = static_cast<int>(state_.settings.presets.size()) - 1;
            }
            else
            {
                state_.preset_index = std::clamp(
                    state_.preset_index, 0, std::max(0, static_cast<int>(state_.settings.presets.size()) - 1)
                );
            }

            state_.status = u8"설정을 data/settings.json에 저장했습니다.";
            publish();
        }

    private:
        // 창을 띄우지 않고 상태 줄에 잠깐 보여 준다. 저장 실패처럼 꼭 읽어야 하는 것은 fail을 쓴다.
        void notify(const std::u8string& message, const bool error = true)
        {
            state_.notice = message;
            state_.notice_error = error;
            state_.status = message;
            notice_until_ = std::chrono::steady_clock::now() + std::chrono::seconds { 6 };
            publish();
        }

        [[nodiscard]] static std::u8string capture_name(const runtime_command action)
        {
            return action == runtime_command::take_screenshot ? u8"사진 촬영" : u8"녹화 시작";
        }

        // 영역이 없으면 먼저 고르게 하고, 고르는 즉시 이어서 촬영한다.
        void queue_capture(const runtime_command action)
        {
            queued_action_ = action;
            state_.queued_capture = capture_name(action);
            close_editor();

            if (!region_.selecting())
            {
                region_.begin_selection();
            }

            publish();
        }

        void start_countdown(const runtime_command action)
        {
            countdown_action_ = action;
            countdown_end_ = std::chrono::steady_clock::now() + std::chrono::seconds { state_.settings.capture_delay };
            state_.countdown = state_.settings.capture_delay;
            state_.countdown_label = utf8_number(state_.countdown) + u8"초 후 " + capture_name(action);
            state_.status = state_.countdown_label + u8" · 같은 단축키나 취소 버튼으로 멈춥니다.";
            publish();
        }

        void cancel_countdown()
        {
            if (state_.countdown == 0)
            {
                return;
            }

            state_.countdown = 0;
            state_.countdown_label.clear();
            notify(u8"지연 촬영을 취소했습니다.", false);
        }

        // 사진은 그림과 파일로, 애니메이션은 움직임이 남도록 파일로만 넣는다.
        void copy_saved_output(const bool requested)
        {
            if (state_.saved_output.empty())
            {
                return;
            }

            const auto error { copy_file_to_clipboard(owner_, state_.saved_output, !saved_animation_) };

            if (!error.empty())
            {
                notify(error);
            }
            else if (requested)
            {
                notify(
                    saved_animation_ ? u8"파일을 클립보드에 복사했습니다." : u8"사진을 클립보드에 복사했습니다.", false
                );
            }
        }

        // 종료를 정한 뒤 다음 틱에 닫기를 보내 새 frame의 닫힘 훅이 그 결정을 따르게 한다.
        void request_exit()
        {
            state_.exiting = true;
            publish();
            close_post_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds { 150 };
        }

        void update_tray_icon()
        {
            const bool wanted { state_.settings.close_to_tray || state_.hidden };
            NOTIFYICONDATAW data { sizeof(NOTIFYICONDATAW) };
            data.hWnd = owner_;
            data.uID = 1;

            if (wanted && !tray_added_)
            {
                data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
                data.uCallbackMessage = tray_message;
                data.hIcon = static_cast<HICON>(LoadImageW(
                    GetModuleHandleW(nullptr),
                    MAKEINTRESOURCEW(CHALKAG_ICON_RESOURCE),
                    IMAGE_ICON,
                    GetSystemMetrics(SM_CXSMICON),
                    GetSystemMetrics(SM_CYSMICON),
                    0
                ));
                wcscpy_s(data.szTip, L"chalkag");
                tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;

                if (tray_added_)
                {
                    data.uVersion = NOTIFYICON_VERSION_4;
                    Shell_NotifyIconW(NIM_SETVERSION, &data);
                }

                if (data.hIcon != nullptr)
                {
                    DestroyIcon(data.hIcon);
                }
            }
            else if (!wanted && tray_added_)
            {
                Shell_NotifyIconW(NIM_DELETE, &data);
                tray_added_ = false;
            }
        }

        void hide_to_tray()
        {
            region_.cancel_selection();
            queued_action_.reset();
            state_.queued_capture.clear();
            state_.hidden = true;
            update_tray_icon();

            if (!tray_added_)
            {
                // 알림 영역을 쓸 수 없으면 숨기지 않는다. 숨긴 창을 다시 열 길이 없어진다.
                state_.hidden = false;
                notify(u8"알림 영역 아이콘을 만들 수 없어 창을 숨기지 않았습니다.");
                return;
            }

            publish();
        }

        // 알림 영역에 숨긴 창을 다시 보인다. 사용자가 직접 연 경우에는 앞으로 가져온다.
        void show_from_tray(const bool activate = false)
        {
            if (activate && !owner_hidden_)
            {
                activate_after_show_ = true;
            }

            if (!state_.hidden)
            {
                return;
            }

            activate_after_show_ = true;
            state_.hidden = false;
            update_tray_icon();
            publish();
        }

        void show_tray_menu(const POINT point)
        {
            const HMENU menu { CreatePopupMenu() };

            if (menu == nullptr)
            {
                return;
            }

            AppendMenuW(menu, MF_STRING, 1, L"명령창 열기");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING | (state_.recording ? MF_GRAYED : 0), 2, L"영역 선택");
            AppendMenuW(menu, MF_STRING | (state_.recording ? MF_GRAYED : 0), 3, L"사진");
            AppendMenuW(menu, MF_STRING, 4, state_.recording ? L"녹화 정지" : L"녹화");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 5, L"종료");
            SetForegroundWindow(owner_);
            const auto chosen { TrackPopupMenu(
                menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, owner_, nullptr
            )
            };
            DestroyMenu(menu);
            PostMessageW(owner_, WM_NULL, 0, 0);

            const auto action { chosen == 1       ? runtime_command::show_window
                                    : chosen == 2 ? runtime_command::select_region
                                    : chosen == 3 ? runtime_command::take_screenshot
                                    : chosen == 4 ? runtime_command::toggle_recording
                                    : chosen == 5 ? runtime_command::quit_application
                                                  : runtime_command {}
            };

            if (action != runtime_command {})
            {
                execute({ .command = static_cast<std::uint32_t>(action) });
            }
        }

        // 선택한 영역으로 사진 저장이나 녹화를 바로 시작한다.
        void start_capture(const runtime_command action)
        {
            close_editor();

            const auto selected { region_.current_region() };

            if (!selected.has_value())
            {
                queue_capture(action);
                return;
            }

            const bool frame_excluded { region_.set_capture_active(true) };

            if (!SetWindowDisplayAffinity(owner_, WDA_EXCLUDEFROMCAPTURE))
            {
                // 명령창이 영역에 겹친 경우에는 확실하게 숨긴 뒤 복원한다.
                RECT bounds {};
                GetWindowRect(owner_, &bounds);
                RECT capture_bounds {
                    selected->x, selected->y, selected->x + selected->width, selected->y + selected->height
                };
                RECT overlap {};
                owner_hidden_ = IntersectRect(&overlap, &bounds, &capture_bounds) != FALSE;

                if (owner_hidden_)
                {
                    ShowWindow(owner_, SW_HIDE);
                }
            }

            DwmFlush();

            if (action == runtime_command::toggle_recording)
            {
                if (owner_hidden_ && !hotkeys_.active(2))
                {
                    restore_capture_windows();
                    fail(
                        u8"정지 단축키를 사용할 수 없습니다. 명령창을 캡쳐 영역 밖으로 옮기거나 앱 설정에서 정지 "
                        u8"단축키를 변경하세요."
                    );
                    return;
                }

                const capture_result result { capture_.start_recording(*selected, state_.settings.capture) };

                if (!result.success)
                {
                    restore_capture_windows();
                    fail(result.message);
                    return;
                }

                state_.recording = true;
                state_.saved_summary.clear();
                state_.recording_saved = false;
                started_ = std::chrono::steady_clock::now();
                elapsed_ = -1;
                state_.elapsed_seconds = 0;
                region_.set_recording_elapsed(0);
                state_.status = frame_excluded ? u8"녹화 중 · " + shortcut_name(2) + u8"으로 정지"
                                               : u8"녹화 중 · 프레임 제외 미지원으로 프레임을 숨겼습니다.";
                publish();
            }
            else
            {
                state_.busy = true;
                saving_recording_ = false;
                screenshot_job_ = true;
                state_.save_cancel_requested = false;
                reset_progress();
                state_.status = u8"스크린샷 저장 중…";
                publish();

                const auto options { state_.settings.capture };
                job_ = std::async(std::launch::async, [this, selected, options] {
                    return capture_.take_screenshot(*selected, options, progress_observer());
                });
            }
        }

        [[nodiscard]] std::u8string shortcut_name(const std::size_t index) const
        {
            const auto text { format_hotkey(state_.settings.shortcuts[index]) };
            return text.empty() ? u8"미지정" : text;
        }

        [[nodiscard]] static capture_region screen_bounds()
        {
            return { GetSystemMetrics(SM_XVIRTUALSCREEN),
                     GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN),
                     GetSystemMetrics(SM_CYVIRTUALSCREEN)
            };
        }

        bool apply_topmost(const bool enabled)
        {
            return SetWindowPos(
                       owner_,
                       enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
                       0,
                       0,
                       0,
                       0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                   )
                != FALSE;
        }

        bool editor_open()
        {
            const auto frame { host_->acquire_frame() };
            return frame != nullptr
                && std::any_of(frame->popups.begin(), frame->popups.end(), [](const luil::ui_popup& popup) {
                       return popup.id == u8"region-editor";
                   });
        }

        void close_editor()
        {
            host_->post_app_message(luil::app_message { close_region_editor_intent {} });
            host_->post_app_message(luil::app_message { animation_editor_intent { animation_editor_action::close } });
            hide_help(true);
        }

        void publish()
        {
            if (host_ != nullptr)
            {
                host_->post_app_message(luil::app_message { state_ });
            }
        }

        void show_dialog(const message_dialog_kind kind, std::u8string title, std::u8string body)
        {
            show_from_tray();
            close_editor();
            host_->post_app_message(luil::app_message { fps_menu_intent { false } });
            state_.dialog = { kind, ++dialog_revision_, std::move(title), std::move(body) };
            publish();
        }

        void fail(const std::u8string& message)
        {
            // 선택 입력 창이 안내 상자의 클릭을 받지 않도록 먼저 닫는다.
            region_.cancel_selection();
            state_.status = message;
            publish();
            show_dialog(message_dialog_kind::information, u8"chalkag 안내", message);
        }

        void restore_capture_windows()
        {
            static_cast<void>(region_.set_capture_active(false));
            SetWindowDisplayAffinity(owner_, WDA_NONE);

            if (owner_hidden_)
            {
                ShowWindow(owner_, SW_SHOWNOACTIVATE);
            }

            owner_hidden_ = false;
        }

        void reset_progress()
        {
            const std::lock_guard lock { progress_mutex_ };
            worker_progress_ = {};
            state_.progress = {};
        }

        [[nodiscard]] progress_callback progress_observer()
        {
            return [this](const save_progress progress) {
                const std::lock_guard lock { progress_mutex_ };
                worker_progress_ = progress;
            };
        }

        void poll()
        {
            if (position_dirty_ && !moving_window_
                && std::chrono::steady_clock::now() - position_changed_ >= std::chrono::milliseconds { 600 })
            {
                save_window_position();
            }

            poll_help();

            const auto now { std::chrono::steady_clock::now() };

            if (!state_.notice.empty() && now >= notice_until_)
            {
                state_.notice.clear();
                publish();
            }

            if (close_post_at_ && now >= *close_post_at_)
            {
                close_post_at_.reset();
                PostMessageW(owner_, WM_CLOSE, 0, 0);
            }

            if (activate_after_show_ && IsWindowVisible(owner_))
            {
                activate_after_show_ = false;
                SetForegroundWindow(owner_);
            }

            // 영역을 다 고르면 기다리던 촬영을 이어서 시작한다. 취소했으면 버린다.
            if (queued_action_ && !region_.selecting())
            {
                const auto action { *queued_action_ };
                queued_action_.reset();
                state_.queued_capture.clear();

                if (region_.current_region() && !state_.busy && !state_.recording
                    && state_.dialog.kind == message_dialog_kind::none)
                {
                    execute({ .command = static_cast<std::uint32_t>(action) });
                }
                else
                {
                    publish();
                }
            }

            if (state_.countdown > 0)
            {
                const auto remaining { std::chrono::ceil<std::chrono::seconds>(countdown_end_ - now).count() };

                if (remaining <= 0)
                {
                    state_.countdown = 0;
                    state_.countdown_label.clear();

                    if (!state_.busy && !state_.recording && state_.dialog.kind == message_dialog_kind::none)
                    {
                        start_capture(countdown_action_);
                    }
                    else
                    {
                        publish();
                    }
                }
                else if (remaining != state_.countdown)
                {
                    state_.countdown = static_cast<int>(remaining);
                    state_.countdown_label
                        = utf8_number(state_.countdown) + u8"초 후 " + capture_name(countdown_action_);
                    publish();
                }
            }

            if (state_.busy)
            {
                save_progress latest {};
                {
                    const std::lock_guard lock { progress_mutex_ };
                    latest = worker_progress_;
                }

                if (latest != state_.progress)
                {
                    state_.progress = latest;
                    state_.status = save_progress_text(latest);

                    // 화면 읽기가 끝난 뒤에는 제외 미지원 환경의 명령창도 다시 표시한다.
                    if (owner_hidden_ && latest.stage != save_stage::preparing)
                    {
                        restore_capture_windows();
                    }

                    publish();
                }
            }

            if (job_.valid() && job_.wait_for(std::chrono::seconds { 0 }) == std::future_status::ready)
            {
                capture_result result {};

                try
                {
                    result = job_.get();
                }
                catch (const std::exception&)
                {
                    result.message = u8"캡쳐 또는 인코딩 처리 중 오류가 발생했습니다.";
                }

                state_.busy = false;
                const bool cancelled { state_.save_cancel_requested };
                if (cancelled)
                {
                    capture_.reset_save_cancel();
                }
                state_.save_cancel_requested = false;
                restore_capture_windows();
                state_.pending_recording = capture_.has_pending_recording();
                state_.recorded_region = capture_.recorded_region();
                state_.recorded_duration = capture_.recorded_duration();

                if (result.success && !result.output.empty())
                {
                    state_.saved_output = result.output;
                    saved_animation_ = !screenshot_job_;

                    if (saving_recording_)
                    {
                        state_.recording_saved = true;
                    }

                    if (saving_recording_ || !state_.pending_recording)
                    {
                        std::error_code error;
                        const auto bytes { std::filesystem::file_size(result.output, error) };

                        if (error)
                        {
                            state_.saved_summary = u8"저장 완료 · 파일 크기를 읽을 수 없습니다";
                        }
                        else
                        {
                            const auto unit { bytes >= 1048576 ? u8" MiB" : bytes >= 1024 ? u8" KiB" : u8" B" };
                            const double factor { bytes >= 1048576 ? 1048576.0 : bytes >= 1024 ? 1024.0 : 1.0 };
                            auto size { std::to_string(static_cast<double>(bytes) / factor) };
                            size.resize(size.find('.') + (factor == 1 ? 0 : 3));
                            state_.saved_summary
                                = u8"마지막 저장 · " + std::u8string { size.begin(), size.end() } + unit;
                        }
                    }
                }

                state_.status = result.message;
                publish();

                if (result.success && screenshot_job_ && !result.output.empty() && state_.settings.copy_to_clipboard)
                {
                    copy_saved_output(false);
                }

                if (!result.success && !cancelled)
                {
                    exit_after_job_ = false;
                    fail(result.message);
                }

                if (exit_after_job_)
                {
                    exit_after_job_ = false;
                    request_exit();
                }

                saving_recording_ = false;
                screenshot_job_ = false;
            }

            if (state_.recording && capture_.recording_failed() && !state_.busy)
            {
                execute({ .command = static_cast<std::uint32_t>(runtime_command::toggle_recording) });
            }

            if (state_.recording)
            {
                const auto seconds {
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_)
                        .count()
                };

                if (seconds != elapsed_)
                {
                    elapsed_ = seconds;
                    state_.elapsed_seconds = static_cast<std::uint64_t>(seconds);
                    region_.set_recording_elapsed(state_.elapsed_seconds);
                    state_.status = u8"녹화 중 · " + utf8_number(static_cast<int>(seconds)) + u8"초 · "
                        + shortcut_name(2) + u8" 정지";
                    publish();
                }
            }
        }

        void hide_help(const bool include_editor = false)
        {
            if (host_ != nullptr)
            {
                host_->post_app_message(luil::app_message { dismiss_help_intent {} });
            }

            if (!include_editor && host_ != nullptr && editor_open())
            {
                return;
            }

            // frame 게시보다 캡쳐가 먼저 시작될 수 있어 실제 팝업도 즉시 숨긴다.
            const std::pair<HWND, bool> surfaces { owner_, include_editor };
            EnumThreadWindows(
                GetCurrentThreadId(),
                [](const HWND window, const LPARAM value) -> BOOL {
                    const auto& surfaces { *reinterpret_cast<const std::pair<HWND, bool>*>(value) };
                    wchar_t name[128] {};
                    GetClassNameW(window, name, 128);

                    if (GetWindow(window, GW_OWNER) == surfaces.first
                        && (std::wstring_view { name } == L"Chalkag.Command.Window.Popup"
                            || (surfaces.second && std::wstring_view { name } == L"Chalkag.Command.Window.Secondary")))
                    {
                        ShowWindow(window, SW_HIDE);
                    }

                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&surfaces)
            );
        }

        void poll_help()
        {
            if (host_ == nullptr)
            {
                return;
            }

            if (editor_open() || state_.dialog.kind != message_dialog_kind::none)
            {
                // 프레임의 네이티브 핸들에서 열어도 편집창이 프레임 아래로 가리지 않는다.
                EnumThreadWindows(
                    GetCurrentThreadId(),
                    [](const HWND window, const LPARAM owner) -> BOOL {
                        wchar_t name[128] {};
                        GetClassNameW(window, name, 128);

                        if (GetWindow(window, GW_OWNER) == reinterpret_cast<HWND>(owner)
                            && std::wstring_view { name } == L"Chalkag.Command.Window.Popup"
                            && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
                        {
                            SetWindowPos(
                                window,
                                HWND_TOPMOST,
                                0,
                                0,
                                0,
                                0,
                                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
                            );
                        }

                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(owner_)
                );
                return;
            }

            help_target_intent target {};

            const auto now { std::chrono::steady_clock::now() };
            const HWND foreground { GetForegroundWindow() };

            if (state_.dialog.kind == message_dialog_kind::none && !state_.busy && !state_.recording
                && IsWindowVisible(owner_) && !IsIconic(owner_) && (foreground == owner_ || foreground == nullptr))
            {
                const auto interaction { host_->acquire_interaction() };
                const bool idle { !interaction.drag.has_value() && interaction.pressed == luil::ui_element_id {} };
                const bool keyboard_newer = interaction.focus_visible && interaction.focused_surface.empty()
                    && interaction.focus_started_at.has_value()
                    && (!interaction.hover_started_at.has_value()
                        || *interaction.focus_started_at > *interaction.hover_started_at);

                if (idle && keyboard_newer && now - *interaction.focus_started_at >= luil::tooltip_delay)
                {
                    target.target = interaction.focused;
                    target.started_at = *interaction.focus_started_at;
                }
                else if (
                    idle && !keyboard_newer && interaction.hovered_surface.empty()
                    && interaction.hover_started_at.has_value()
                    && now - *interaction.hover_started_at >= luil::tooltip_delay
                )
                {
                    target.target = interaction.hovered;
                    target.started_at = *interaction.hover_started_at;
                }

                if (idle && interaction.hovered_surface == u8"command-help")
                {
                    target = help_target_;
                }

                if (target.target != luil::ui_element_id {})
                {
                    last_help_seen_ = now;
                }
                else if (
                    idle && interaction.hovered == luil::ui_element_id {}
                    && now - last_help_seen_ < std::chrono::milliseconds { 200 }
                )
                {
                    target = help_target_;
                }

                const auto tree { host_->acquire_ui_tree() };
                const auto* const element { tree != nullptr ? tree->find(target.target) : nullptr };

                if (element != nullptr)
                {
                    const float scale { static_cast<float>(GetDpiForWindow(owner_)) / 96.0f };
                    POINT origin {};
                    ClientToScreen(owner_, &origin);

                    const auto box { element->bounds() };
                    const POINT center { origin.x + static_cast<LONG>(box.x + box.width / 2.0f),
                                         origin.y + static_cast<LONG>(box.y + box.height / 2.0f)
                    };
                    MONITORINFO monitor { sizeof(MONITORINFO) };

                    if (GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &monitor))
                    {
                        target.maximum_width = std::min(
                            340.0f, static_cast<float>(monitor.rcWork.right - monitor.rcWork.left) / scale - 16.0f
                        );
                        target.available_above = std::max(
                            0.0f, (static_cast<float>(origin.y - monitor.rcWork.top) + box.y) / scale - 16.0f
                        );
                        target.available_below = std::max(
                            0.0f,
                            (static_cast<float>(monitor.rcWork.bottom - origin.y) - box.y - box.height) / scale - 16.0f
                        );
                        target.minimum_x = static_cast<float>(monitor.rcWork.left - origin.x) / scale + 8.0f;
                        target.maximum_x = static_cast<float>(monitor.rcWork.right - origin.x) / scale - 8.0f;
                    }
                }
                else
                {
                    target = {};
                }
            }

            if (target != help_target_)
            {
                help_target_ = target;
                host_->post_app_message(luil::app_message { target });
            }
        }

        void shutdown() noexcept
        {
            if (host_ == nullptr)
            {
                return;
            }

            try
            {
                observe_window_position();
                save_window_position();
            }
            catch (...)
            {}
            host_ = nullptr;
            KillTimer(owner_, 200);

            if (tray_added_)
            {
                NOTIFYICONDATAW data { sizeof(NOTIFYICONDATAW) };
                data.hWnd = owner_;
                data.uID = 1;
                Shell_NotifyIconW(NIM_DELETE, &data);
                tray_added_ = false;
            }
            hotkeys_.clear();
            RemoveWindowSubclass(owner_, owner_proc, 2);
            capture_.request_cancel();

            if (job_.valid())
            {
                try
                {
                    static_cast<void>(job_.get());
                }
                catch (...)
                {}
            }

            capture_.cancel_recording();
        }

        static LRESULT CALLBACK owner_proc(
            const HWND window,
            const UINT message,
            const WPARAM wparam,
            const LPARAM lparam,
            UINT_PTR,
            const DWORD_PTR reference
        ) noexcept
        {
            auto* self { reinterpret_cast<impl*>(reference) };

            if (self->host_ != nullptr)
            {
                try
                {
                    if (message == WM_ENTERSIZEMOVE)
                    {
                        self->moving_window_ = true;
                    }
                    else if (message == WM_WINDOWPOSCHANGED)
                    {
                        self->observe_window_position();
                    }
                    else if (message == WM_EXITSIZEMOVE)
                    {
                        self->moving_window_ = false;
                        self->observe_window_position();
                        self->save_window_position();
                    }
                }
                catch (...)
                {}
            }

            if (message == WM_NCHITTEST && self->host_ != nullptr)
            {
                // 제목줄에 넣은 앱 버튼은 창 이동 대신 클라이언트 입력을 받아야 한다.
                POINT point { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };

                if (ScreenToClient(window, &point) != FALSE)
                {
                    try
                    {
                        const auto frame { self->host_->acquire_frame() };

                        if (frame != nullptr
                            && hit_command_caption_button(
                                *frame, static_cast<float>(point.x), static_cast<float>(point.y)
                            ))
                        {
                            return HTCLIENT;
                        }
                    }
                    catch (...)
                    {}
                }
            }

            if (message == WM_DESTROY || (message == WM_ENDSESSION && wparam != 0))
            {
                self->shutdown();
            }
            else if (message == WM_TIMER && wparam == 200)
            {
                try
                {
                    self->poll();
                }
                catch (...)
                {}
                return 0;
            }
            else if (message == tray_message && self->host_ != nullptr)
            {
                try
                {
                    const auto event { LOWORD(lparam) };

                    if (event == WM_CONTEXTMENU)
                    {
                        self->show_tray_menu({ GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam) });
                    }
                    else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK)
                    {
                        self->execute({ .command = static_cast<std::uint32_t>(runtime_command::show_window) });
                    }
                }
                catch (...)
                {}
                return 0;
            }
            else if (message == single_instance_show_message && self->host_ != nullptr)
            {
                try
                {
                    self->execute({ .command = static_cast<std::uint32_t>(runtime_command::show_window) });
                }
                catch (...)
                {}
                return 0;
            }
            else if (message == WM_HOTKEY && wparam >= 101 && wparam < 101 + default_hotkeys.size())
            {
                try
                {
                    const auto index { static_cast<std::size_t>(wparam - 101) };
                    const auto binding { self->state_.settings.shortcuts[index] };

                    // 재등록 직전에 큐에 들어온 이전 단축키는 실행하지 않는다.
                    if (self->hotkeys_.active(index) && LOWORD(lparam) == binding.modifiers
                        && HIWORD(lparam) == binding.key)
                    {
                        const auto action { wparam == 101       ? runtime_command::select_region
                                                : wparam == 102 ? runtime_command::take_screenshot
                                                : wparam == 103 ? runtime_command::toggle_recording
                                                                : runtime_command::discard_recording
                        };
                        self->execute({ .command = static_cast<std::uint32_t>(action) });
                    }
                }
                catch (...)
                {}
                return 0;
            }

            return DefSubclassProc(window, message, wparam, lparam);
        }

        luil::app_host* host_ {};
        HWND owner_ {};
        global_hotkeys hotkeys_;
        bool saving_recording_ {};
        bool screenshot_job_ {};
        bool saved_animation_ {};
        bool exit_after_job_ {};
        bool tray_added_ {};
        bool activate_after_show_ {};
        std::optional<runtime_command> queued_action_ {};
        runtime_command countdown_action_ {};
        std::chrono::steady_clock::time_point countdown_end_ {};
        std::chrono::steady_clock::time_point notice_until_ {};
        std::optional<std::chrono::steady_clock::time_point> close_post_at_ {};
        std::optional<region_preset> deleted_preset_ {};
        std::size_t deleted_preset_index_ {};

        std::filesystem::path directory_ {};
        settings_store store_;
        image_capture capture_;
        region_controller region_;
        runtime_state state_ {};

        std::future<capture_result> job_ {};
        std::mutex progress_mutex_ {};
        save_progress worker_progress_ {};
        bool owner_hidden_ {};
        bool tracking_position_ {};
        bool moving_window_ {};
        bool position_dirty_ {};

        std::chrono::steady_clock::time_point position_changed_ {};
        std::uint64_t dialog_revision_ {};
        std::chrono::steady_clock::time_point started_ {};
        long long elapsed_ { -1 };
        help_target_intent help_target_ {};

        std::chrono::steady_clock::time_point last_help_seen_ {};
    };

    app_runtime::app_runtime(luil::app_host& host)
        : impl_ { std::make_unique<impl>(host) }
    {}

    app_runtime::~app_runtime() = default;

    void app_runtime::execute(const luil::app_ui_command& command)
    {
        impl_->execute(command);
    }

    command_delegate::command_delegate(single_instance* instance)
        : instance_ { instance }
    {}

    command_delegate::~command_delegate() = default;

    void command_delegate::on_started(luil::app_host& host)
    {
        if (instance_ != nullptr)
        {
            static_cast<void>(instance_->bind_window(find_owner()));
        }

        try
        {
            runtime_ = std::make_unique<app_runtime>(host);
        }
        catch (const std::exception&)
        {
            host.post_app_message(
                luil::app_message { region_state_intent { .status = u8"캡쳐 기능 초기화에 실패했습니다." } }
            );
        }
    }

    void command_delegate::execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command)
    {
        if (runtime_ == nullptr)
        {
            on_started(host);
        }

        if (runtime_ != nullptr)
        {
            runtime_->execute(command);
        }
    }

    luil::app_message command_delegate::make_window_metrics_message(
        const float width, const float height, const float scale
    )
    {
        return luil::app_message { window_metrics_intent { width, height, scale } };
    }

} // namespace chalkag
