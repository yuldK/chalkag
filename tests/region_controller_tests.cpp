#include "capture/frame_glow.h"
#include "capture/region_controller.h"
#include "capture/region_events.h"
#include "luil/app/app_host.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include <windows.h>

#include <ole2.h>
#include <uiautomation.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace chalkag {
    namespace {
        constexpr int skipped { 77 };
        constexpr wchar_t owner_class[] { L"Chalkag.Command.Window" };
        int failure_count {};
        bool hotkey_available_before_test {};

        void check(const bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failure_count;
            }
        }

        struct close_signal
        {};

        class test_driver final : public luil::logic_driver
        {
        public:
            void handle(luil::app_message message) override
            {
                if (message.get<close_signal>() != nullptr)
                {
                    closed_.store(true);
                }

                if (const auto* state { message.get<region_state_intent>() }; state != nullptr)
                {
                    {
                        const std::lock_guard lock { mutex_ };
                        state_ = *state;
                    }

                    changed_.notify_all();
                }
            }

            [[nodiscard]] std::shared_ptr<const luil::ui_frame> make_frame() override
            {
                return std::make_shared<const luil::ui_frame>();
            }

            [[nodiscard]] luil::app_message make_close_message() override
            {
                return luil::app_message { close_signal {} };
            }

            [[nodiscard]] bool shutdown_completed() const override
            {
                return closed_.load();
            }

            void expect(const std::optional<capture_region> region, const bool selecting, const char* name)
            {
                std::unique_lock lock { mutex_ };
                check(
                    changed_.wait_for(
                        lock,
                        std::chrono::seconds { 2 },
                        [&] { return state_.region == region && state_.selecting == selecting; }
                    ),
                    name
                );
            }

        private:
            std::atomic<bool> closed_ {};
            std::mutex mutex_ {};
            std::condition_variable changed_ {};
            region_state_intent state_ {};
        };

        LRESULT CALLBACK owner_proc(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam)
        {
            if (message == WM_NCCREATE)
            {
                const auto* creation { reinterpret_cast<const CREATESTRUCTW*>(lparam) };
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
            }

            if (message == WM_CLOSE)
            {
                // 실제 명령창처럼 host를 먼저 파괴하여 subclass의 선행 정리를 검증한다.
                auto* host {
                    reinterpret_cast<std::unique_ptr<luil::app_host>*>(GetWindowLongPtrW(window, GWLP_USERDATA))
                };
                host->reset();
                DestroyWindow(window);
                return 0;
            }

            return DefWindowProcW(window, message, wparam, lparam);
        }

        class fixture final
        {
        public:
            explicit fixture(const bool enable_edit = true)
                : host { std::make_unique<luil::app_host>(luil::app_host::config {}, driver, nullptr) }
            {
                owner = CreateWindowExW(
                    0,
                    owner_class,
                    L"Test owner",
                    WS_POPUP,
                    0,
                    0,
                    1,
                    1,
                    nullptr,
                    nullptr,
                    GetModuleHandleW(nullptr),
                    &host
                );

                if (owner == nullptr)
                {
                    throw std::runtime_error { "Cannot create isolated test owner." };
                }

                try
                {
                    if (enable_edit)
                    {
                        controller = std::make_unique<region_controller>(*host, [this] {
                            ++edit_requests;
                            edit_thread = GetCurrentThreadId();
                            edit_capture_released = GetCapture() == nullptr;
                        });
                    }
                    else
                    {
                        controller = std::make_unique<region_controller>(*host);
                    }
                }
                catch (...)
                {
                    DestroyWindow(owner);
                    throw;
                }

                frame = FindWindowW(L"Chalkag.Region.Frame", nullptr);
                glow = FindWindowW(L"Chalkag.Region.Glow", nullptr);
                drag_outline = FindWindowW(L"Chalkag.Region.DragOutline", nullptr);
                resize_input = FindWindowW(L"Chalkag.Region.ResizeInput", nullptr);
                selection_input = FindWindowW(L"Chalkag.Region.SelectionInput", nullptr);
                guide = FindWindowW(L"Chalkag.Region.Guide", nullptr);
                dispatcher = FindWindowExW(HWND_MESSAGE, nullptr, L"Chalkag.Region.Dispatcher", nullptr);
                edit_button = FindWindowExW(frame, nullptr, L"BUTTON", L"영역 설정");

                if (frame == nullptr || glow == nullptr || drag_outline == nullptr || resize_input == nullptr
                    || selection_input == nullptr || guide == nullptr || dispatcher == nullptr)
                {
                    controller.reset();
                    DestroyWindow(owner);
                    throw std::runtime_error { "Capture windows were not created." };
                }
            }

            ~fixture()
            {
                controller.reset();

                if (IsWindow(owner))
                {
                    DestroyWindow(owner);
                }
            }

            void mouse(const UINT message, const screen_point point) const
            {
                // 실제 커서와 사용자 입력을 건드리지 않고 선택 창의 네이티브 입력 경로를 검증한다.
                POINT client { point.x, point.y };
                check(
                    ScreenToClient(selection_input, &client) != FALSE,
                    "selection converts screen coordinates into input window coordinates"
                );
                SendMessageW(
                    selection_input, message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(client.x, client.y)
                );
            }

            void select(const capture_region region)
            {
                controller->begin_selection();
                check(IsWindowVisible(guide) != FALSE, "selection guide is visible on isolated desktop");
                check(IsWindowVisible(selection_input) != FALSE, "selection input is visible only during selection");
                mouse(WM_LBUTTONDOWN, { region.x, region.y });
                check(GetCapture() == selection_input, "selection window captures drag input directly");
                mouse(WM_MOUSEMOVE, { region.x + region.width, region.y + region.height });
                mouse(WM_LBUTTONUP, { region.x + region.width, region.y + region.height });
                driver.expect(region, false, "mouse release commits selected region");
                check(
                    !IsWindowVisible(selection_input) && GetCapture() == nullptr,
                    "committing hides input layer and releases mouse capture"
                );
            }

            test_driver driver {};

            std::unique_ptr<luil::app_host> host {};
            HWND owner {};

            std::unique_ptr<region_controller> controller {};
            HWND frame {};
            HWND glow {};
            HWND drag_outline {};
            HWND resize_input {};
            HWND selection_input {};
            HWND guide {};
            HWND dispatcher {};
            HWND edit_button {};
            int edit_requests {};
            DWORD edit_thread {};
            bool edit_capture_released {};
        };

        void check_hollow(const HWND window, const capture_region region, const int inset = 0)
        {
            RECT position {};
            check(GetWindowRect(window, &position) != FALSE, "frame position exists");

            const HRGN shape { CreateRectRgn(0, 0, 0, 0) };
            RECT clear_area {};
            clear_area.left = region.x + inset - position.left;
            clear_area.top = region.y + inset - position.top;
            clear_area.right = region.x + region.width - inset - position.left;
            clear_area.bottom = region.y + region.height - inset - position.top;

            const HRGN interior { CreateRectRgn(clear_area.left, clear_area.top, clear_area.right, clear_area.bottom) };
            const HRGN intersection { CreateRectRgn(0, 0, 0, 0) };
            check(GetWindowRgn(window, shape) != ERROR, "frame has a native window region");

            const POINT center { region.x + region.width / 2 - position.left,
                                 region.y + region.height / 2 - position.top
            };

            const bool center_clear { PtInRegion(shape, center.x, center.y) == FALSE };

            if (!center_clear)
            {
                std::cerr << "HOLLOW DIAGNOSTIC: capture=" << region.x << ',' << region.y << ',' << region.width << ','
                          << region.height << " frame=" << position.left << ',' << position.top << ',' << position.right
                          << ',' << position.bottom << " local_center=" << center.x << ',' << center.y
                          << " dpi=" << GetDpiForWindow(window) << " inset=" << inset << '\n';
            }

            check(center_clear, "capture center passes through frame");

            if (clear_area.right > clear_area.left && clear_area.bottom > clear_area.top)
            {
                check(
                    CombineRgn(intersection, shape, interior, RGN_AND) == NULLREGION,
                    "entire capture interior passes through frame"
                );
            }

            DeleteObject(intersection);
            DeleteObject(interior);
            DeleteObject(shape);
        }

        void check_closed(const fixture& test)
        {
            check(!IsWindow(test.owner), "owner closes");
            check(
                !IsWindow(test.frame) && !IsWindow(test.glow) && !IsWindow(test.drag_outline)
                    && !IsWindow(test.resize_input) && !IsWindow(test.selection_input) && !IsWindow(test.guide)
                    && !IsWindow(test.dispatcher),
                "owned capture windows and both input layers are destroyed"
            );
            check(!IsWindow(test.edit_button), "edit button is destroyed with frame");
            check(GetCapture() == nullptr, "capture is released before owner teardown");
            check(test.host == nullptr, "owner destroys production host after controller cleanup");

            const bool hotkey { RegisterHotKey(nullptr, 91, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, '1') != FALSE };
            check(hotkey == hotkey_available_before_test, "owner teardown restores prior hotkey availability");

            if (hotkey)
            {
                UnregisterHotKey(nullptr, 91);
            }
        }

        void send_pointer(const HWND window, const UINT message, const screen_point screen)
        {
            POINT point { screen.x, screen.y };
            check(ScreenToClient(window, &point) != FALSE, "pointer message converts to current client coordinates");
            SendMessageW(window, message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(point.x, point.y));
        }

        void check_drag_outline(const fixture& test, const capture_region expected)
        {
            check(
                IsWindowVisible(test.drag_outline) != FALSE, "drag destination outline is visible before mouse release"
            );

            const LONG_PTR style { GetWindowLongPtrW(test.drag_outline, GWL_EXSTYLE) };
            constexpr LONG_PTR required { WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST };
            check(
                (style & required) == required,
                "destination outline is transparent, click-through, nonactivating and topmost"
            );
            check(
                !GetLayeredWindowAttributes(test.drag_outline, nullptr, nullptr, nullptr),
                "destination outline uses a per-pixel Skia alpha bitmap"
            );
            RECT position {};
            GetWindowRect(test.drag_outline, &position);

            const int thickness { expected.x - position.left };
            check(
                thickness >= 3 && position.top == expected.y - thickness
                    && position.right == expected.x + expected.width + thickness
                    && position.bottom == expected.y + expected.height + thickness,
                "destination outline surrounds the precise candidate capture rectangle"
            );
            check_hollow(test.drag_outline, expected);
            check(
                !GetUpdateRect(test.drag_outline, nullptr, FALSE),
                "changed outline is painted synchronously without stale resize pixels"
            );
            check(GetCapture() == test.frame, "destination feedback does not steal the drag from the real frame");

            const POINT edge { expected.x - 1, expected.y + expected.height / 2 };
            check(
                WindowFromPoint(edge) != test.drag_outline, "even the visible preview border passes mouse input through"
            );

            const HDC device { CreateCompatibleDC(nullptr) };
            const HBITMAP bitmap {
                CreateBitmap(position.right - position.left, position.bottom - position.top, 1, 32, nullptr)
            };

            const HGDIOBJ previous { SelectObject(device, bitmap) };
            constexpr COLORREF clear { RGB(31, 47, 71) };
            const HBRUSH brush { CreateSolidBrush(clear) };
            const RECT client { 0, 0, position.right - position.left, position.bottom - position.top };
            FillRect(device, &client, brush);
            SendMessageW(test.drag_outline, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(device), PRF_CLIENT);
            check(
                GetPixel(device, 2, thickness + expected.height / 2) != clear,
                "destination border actually paints a visible colored stroke"
            );
            check(
                GetPixel(device, thickness + expected.width / 2, thickness + expected.height / 2) == clear,
                "destination painting leaves the capture interior untouched"
            );
            DeleteObject(brush);
            SelectObject(device, previous);
            DeleteObject(bitmap);
            DeleteDC(device);
            HUIANODE node {};
            check(
                SUCCEEDED(UiaNodeFromHandle(test.drag_outline, &node)) && node != nullptr,
                "destination preview exposes a native UI Automation node"
            );

            if (node != nullptr)
            {
                VARIANT name {};

                auto title { L"이동 미리보기 " + std::to_wstring(expected.width) + L" × "
                             + std::to_wstring(expected.height) + L" px · "
                };
                title += std::to_wstring(expected.x) + L", " + std::to_wstring(expected.y);
                check(
                    SUCCEEDED(UiaGetPropertyValue(node, UIA_NamePropertyId, &name)) && name.vt == VT_BSTR
                        && name.bstrVal != nullptr && std::wstring_view { name.bstrVal } == title,
                    "UI Automation reports the candidate dimensions and position"
                );
                VariantClear(&name);
                UiaNodeRelease(node);
            }
        }

        void check_moving_frame(const fixture& test, const capture_region expected)
        {
            check(!IsWindowVisible(test.drag_outline), "live Skia movement does not leave a second frame behind");
            check(
                (GetWindowLongPtrW(test.frame, GWL_EXSTYLE) & WS_EX_LAYERED) != 0,
                "moving frame presents Skia pixels through a layered window"
            );
            check_hollow(test.frame, expected);
            check_hollow(test.resize_input, expected);
            check(GetCapture() == test.frame, "live movement preserves pointer capture");
            check(
                !GetUpdateRect(test.frame, nullptr, FALSE), "live frame pixels finish before the next pointer message"
            );
            RECT position {};
            GetWindowRect(test.frame, &position);

            const int width { position.right - position.left };
            const int height { position.bottom - position.top };
            const HDC device { CreateCompatibleDC(nullptr) };
            const HBITMAP bitmap { CreateBitmap(width, height, 1, 32, nullptr) };
            const HGDIOBJ previous { SelectObject(device, bitmap) };
            constexpr COLORREF clear { RGB(31, 47, 71) };
            const HBRUSH brush { CreateSolidBrush(clear) };
            const RECT client { 0, 0, width, height };
            FillRect(device, &client, brush);
            SendMessageW(test.frame, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(device), PRF_CLIENT);
            check(
                GetPixel(
                    device,
                    expected.x + expected.width / 2 - position.left,
                    expected.y + expected.height / 2 - position.top
                ) == clear,
                "Skia movement leaves the destination capture center untouched"
            );
            check(
                GetPixel(device, expected.x + expected.width / 2 - position.left, expected.y - 1 - position.top)
                    != clear,
                "Skia border paints at the exact destination instead of retaining old pixels"
            );
            DeleteObject(brush);
            SelectObject(device, previous);
            DeleteObject(bitmap);
            DeleteDC(device);
        }

        [[nodiscard]] BYTE overlay_pixel_alpha(const HWND window, const screen_point point)
        {
            RECT position {};
            GetWindowRect(window, &position);
            BITMAPINFO info {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = position.right - position.left;
            info.bmiHeader.biHeight = position.top - position.bottom;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            void* pixels {};

            const HBITMAP bitmap { CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) };
            const HDC device { CreateCompatibleDC(nullptr) };
            const HGDIOBJ previous { SelectObject(device, bitmap) };
            SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(device), PRF_CLIENT);
            GdiFlush();

            const auto index { static_cast<std::size_t>(point.y - position.top)
                                   * static_cast<std::size_t>(info.bmiHeader.biWidth)
                               + static_cast<std::size_t>(point.x - position.left)
            };

            const BYTE alpha { static_cast<BYTE>(static_cast<const std::uint32_t*>(pixels)[index] >> 24) };
            SelectObject(device, previous);
            DeleteDC(device);
            DeleteObject(bitmap);
            return alpha;
        }

        void check_selection_input(fixture& test, const capture_region bounds)
        {
            check(!IsWindowVisible(test.selection_input), "selection input is hidden while idle");
            test.controller->begin_selection();
            test.driver.expect(std::nullopt, true, "selection begins without a committed region");
            RECT position {};
            GetWindowRect(test.selection_input, &position);
            check(
                position.left == bounds.x && position.top == bounds.y && position.right == bounds.x + bounds.width
                    && position.bottom == bounds.y + bounds.height,
                "selection input spans all monitors in physical screen coordinates"
            );

            const LONG_PTR style { GetWindowLongPtrW(test.selection_input, GWL_EXSTYLE) };
            check(
                (style & (WS_EX_LAYERED | WS_EX_TOPMOST)) == (WS_EX_LAYERED | WS_EX_TOPMOST)
                    && (style & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) == 0,
                "selection input accepts mouse and keyboard without a global hook"
            );
            BYTE alpha {};
            DWORD flags {};
            check(
                GetLayeredWindowAttributes(test.selection_input, nullptr, &alpha, &flags) && alpha == 1
                    && flags == LWA_ALPHA,
                "selection uses minimum nonzero opacity"
            );

            const HDC screen { GetDC(nullptr) };
            const HDC device { CreateCompatibleDC(screen) };
            const HBITMAP bitmap { CreateCompatibleBitmap(screen, 1, 1) };
            const HGDIOBJ previous { SelectObject(device, bitmap) };
            SendMessageW(test.selection_input, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(device), PRF_CLIENT);
            check(
                GetPixel(device, 0, 0) == RGB(255, 255, 255),
                "selection input uses a bright surface instead of darkening the desktop"
            );
            SelectObject(device, previous);
            DeleteObject(bitmap);
            DeleteDC(device);
            ReleaseDC(nullptr, screen);

            const screen_point start { bounds.x + 10, bounds.y + bounds.height - 20 };
            check(
                WindowFromPoint({ start.x, start.y }) == test.selection_input,
                "native hit testing reaches the transparent selection input"
            );
            test.mouse(WM_LBUTTONDOWN, start);
            test.mouse(WM_LBUTTONUP, { start.x + 5, start.y + 5 });
            test.driver.expect(std::nullopt, true, "undersized drag remains in selection mode");
            check(
                GetCapture() == nullptr && !IsWindowVisible(test.frame),
                "undersized drag releases capture and hides the preview"
            );
            test.mouse(WM_RBUTTONUP, start);
            test.driver.expect(std::nullopt, false, "right click cancels selection without a global hook");
            check(!IsWindowVisible(test.selection_input), "right click hides the desktop input layer");
        }

        void check_resize_input(fixture& test, const capture_region region, const capture_region bounds)
        {
            check(IsWindowVisible(test.resize_input), "expanded resize input is visible for an idle frame");

            const LONG_PTR style { GetWindowLongPtrW(test.resize_input, GWL_EXSTYLE) };
            check(
                (style & (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST))
                        == (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST)
                    && (style & WS_EX_TRANSPARENT) == 0,
                "resize input catches clicks without activating on hover"
            );
            BYTE alpha {};
            DWORD flags {};
            check(
                GetLayeredWindowAttributes(test.resize_input, nullptr, &alpha, &flags) != FALSE && alpha == 1
                    && flags == LWA_ALPHA,
                "input layer uses minimum nonzero opacity"
            );
            check_hollow(test.resize_input, region);

            const int offset { MulDiv(14, static_cast<int>(GetDpiForWindow(test.frame)), 96) };
            const int notch { MulDiv(4, static_cast<int>(GetDpiForWindow(test.frame)), 96) };
            const int lateral { MulDiv(7, static_cast<int>(GetDpiForWindow(test.frame)), 96) };
            const std::array<screen_point, 8> points {
                screen_point { region.x - offset, region.y - offset },
                screen_point { region.x + region.width + offset, region.y - offset },
                screen_point { region.x - notch, region.y + region.height + notch },
                screen_point { region.x + region.width + notch, region.y + region.height + notch },
                screen_point { region.x - offset, region.y + region.height / 2 },
                screen_point { region.x + region.width + offset, region.y + region.height / 2 },
                screen_point { region.x + region.width / 2, region.y - offset },
                screen_point { region.x + region.width / 2 + lateral, region.y + region.height + notch },
            };

            constexpr std::array edges {
                resize_edge::top_left, resize_edge::top_right, resize_edge::bottom_left, resize_edge::bottom_right,
                resize_edge::left,     resize_edge::right,     resize_edge::top,         resize_edge::bottom,
            };

            for (std::size_t index = 0; index < points.size(); ++index)
            {
                test.controller->set_region(region);
                RedrawWindow(test.resize_input, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                RECT input_position {};
                GetWindowRect(test.resize_input, &input_position);

                const HRGN shape { CreateRectRgn(0, 0, 0, 0) };
                check(GetWindowRgn(test.resize_input, shape) != ERROR, "resize input has a native hit region");

                const screen_point point { points[index] };
                check(
                    PtInRegion(shape, point.x - input_position.left, point.y - input_position.top) != FALSE,
                    "padding beyond each visible resize bar belongs to the hit region"
                );
                DeleteObject(shape);

                const HWND target { WindowFromPoint({ point.x, point.y }) };

                if (target != test.resize_input)
                {
                    std::cerr << "HIT DIAGNOSTIC: index=" << index << " point=" << point.x << ',' << point.y
                              << " target=" << target << " frame=" << test.frame << " input=" << test.resize_input
                              << '\n';
                }

                check(target == test.resize_input, "native hit testing reaches the nearly transparent resize padding");
                send_pointer(test.resize_input, WM_LBUTTONDOWN, point);
                check(GetCapture() == test.frame, "clicking resize padding captures pointer in the real frame");
                RECT frame_before {};
                RECT glow_before {};
                GetWindowRect(test.frame, &frame_before);
                GetWindowRect(test.glow, &glow_before);

                const screen_point end { point.x + 4, point.y + 3 };
                send_pointer(test.frame, WM_MOUSEMOVE, end);
                RECT frame_during {};
                RECT glow_during {};
                GetWindowRect(test.frame, &frame_during);
                GetWindowRect(test.glow, &glow_during);
                check(
                    EqualRect(&frame_before, &frame_during) && EqualRect(&glow_before, &glow_during),
                    "resize drag keeps frame and glow geometry fixed until release"
                );
                check(
                    test.controller->current_region() == region,
                    "in-progress resize does not replace committed capture geometry"
                );

                const capture_region expected { adjust_region(region, edges[index], { 4, 3 }, bounds) };
                check_drag_outline(test, expected);

                const screen_point border { region.x + region.width / 2, region.y - 1 };
                check(
                    overlay_pixel_alpha(test.frame, border) == 40 && !IsWindowVisible(test.glow),
                    "resizing dims the original Skia frame to 16 percent and hides its glow"
                );
                RECT button {};
                GetWindowRect(test.edit_button, &button);
                check(
                    overlay_pixel_alpha(
                        test.edit_button, { (button.left + button.right) / 2, (button.top + button.bottom) / 2 }
                    ) == 40,
                    "native edit control prints at the same reduced opacity as its Skia frame"
                );
                send_pointer(test.frame, WM_LBUTTONUP, end);
                check(
                    !IsWindowVisible(test.drag_outline), "committing a resize hides the temporary destination outline"
                );
                check(
                    test.controller->current_region() == expected,
                    "dragging padding resizes its matching edge or corner"
                );
                check(
                    overlay_pixel_alpha(test.frame, { expected.x + expected.width / 2, expected.y - 1 }) == 255,
                    "resize completion restores the full-opacity Skia frame"
                );
                test.driver.expect(expected, false, "padding drag publishes resized capture geometry");
                check(GetCapture() == nullptr, "padding drag releases pointer capture");
                check(
                    !GetUpdateRect(test.frame, nullptr, FALSE),
                    "released frame is painted before returning to the message loop"
                );
                check_hollow(test.resize_input, expected);
            }

            test.controller->set_region(region);

            const screen_point header { region.x + region.width / 2, region.y + region.height + offset };
            check(
                WindowFromPoint({ header.x, header.y }) == test.frame,
                "visible movement header takes precedence over resize padding behind it"
            );
            send_pointer(test.frame, WM_LBUTTONDOWN, header);

            const screen_point end { header.x + 4, header.y + 3 };
            send_pointer(test.frame, WM_MOUSEMOVE, end);
            check(
                test.controller->current_region() == region,
                "live movement keeps committed capture geometry until release"
            );
            check_moving_frame(test, adjust_region(region, resize_edge::move, { 4, 3 }, bounds));

            const screen_point second_end { header.x + 15, header.y + 20 };
            send_pointer(test.frame, WM_MOUSEMOVE, second_end);
            check_moving_frame(test, adjust_region(region, resize_edge::move, { 15, 20 }, bounds));

            for (int index { 1 }; index <= 20; ++index)
            {
                const screen_point delta { index * 7, index * 5 };
                send_pointer(test.frame, WM_MOUSEMOVE, { header.x + delta.x, header.y + delta.y });
                check_moving_frame(test, adjust_region(region, resize_edge::move, delta, bounds));
            }

            send_pointer(test.frame, WM_LBUTTONUP, end);
            check(!IsWindowVisible(test.drag_outline), "committing a movement hides its preview");
            check(
                test.controller->current_region() == adjust_region(region, resize_edge::move, { 4, 3 }, bounds),
                "header overlapping resize padding moves rather than resizes capture"
            );
            test.controller->set_region(region);
            send_pointer(test.frame, WM_LBUTTONDOWN, header);
            send_pointer(test.frame, WM_MOUSEMOVE, { header.x + 40, header.y + 30 });
            SendMessageW(test.frame, WM_KEYDOWN, VK_ESCAPE, 0);
            check(
                test.controller->current_region() == region && GetCapture() == nullptr,
                "Escape discards a deferred drag without moving the frame"
            );
            check(!IsWindowVisible(test.drag_outline), "Escape hides destination feedback");
            send_pointer(test.frame, WM_LBUTTONDOWN, header);
            send_pointer(test.frame, WM_MOUSEMOVE, { header.x + 40, header.y + 30 });
            SetCapture(test.owner);
            check(
                test.controller->current_region() == region && GetCapture() == test.owner,
                "capture loss discards a deferred drag without committing preview geometry"
            );
            check(
                !IsWindowVisible(test.drag_outline),
                "capture loss hides destination feedback without taking capture back"
            );
            ReleaseCapture();
        }

        void check_continuous_edges(fixture& test, const capture_region bounds)
        {
            if (bounds.height < 240)
            {
                return;
            }

            const capture_region region {
                bounds.x + 40, bounds.y + 120, std::min(420, bounds.width - 80), std::min(180, bounds.height - 180)
            };

            constexpr std::array edges { resize_edge::left, resize_edge::right, resize_edge::top, resize_edge::bottom };

            for (const resize_edge edge : edges)
            {
                const bool vertical { edge == resize_edge::left || edge == resize_edge::right };
                const int length { vertical ? region.height : region.width };
                const std::array offsets { 2, length / 4, length / 2, length * 3 / 4, length - 3 };

                for (const int offset : offsets)
                {
                    test.controller->set_region(region);

                    const int dpi { static_cast<int>(GetDpiForWindow(test.frame)) };

                    // 이동 핸들은 화면 상황에 따라 위나 아래에 배치되므로 얕은 여백을 사용한다.
                    const int depth { MulDiv(vertical ? 22 : 5, dpi, 96) };
                    screen_point point {};

                    if (edge == resize_edge::left)
                    {
                        point = { region.x - depth, region.y + offset };
                    }
                    else if (edge == resize_edge::right)
                    {
                        point = { region.x + region.width + depth, region.y + offset };
                    }
                    else if (edge == resize_edge::top)
                    {
                        point = { region.x + offset, region.y - depth };
                    }
                    else
                    {
                        point = { region.x + offset, region.y + region.height + depth };
                    }

                    const HWND target { WindowFromPoint({ point.x, point.y }) };
                    check(
                        target == test.resize_input || target == test.frame,
                        "full edge accepts resize input near both corners and between visible grips"
                    );
                    send_pointer(target, WM_LBUTTONDOWN, point);
                    check(GetCapture() == test.frame, "continuous edge starts a real capture-frame drag");

                    const screen_point end { point.x + 6, point.y + 4 };
                    send_pointer(test.frame, WM_MOUSEMOVE, end);

                    const capture_region expected { adjust_region(region, edge, { 6, 4 }, bounds) };
                    check_drag_outline(test, expected);
                    send_pointer(test.frame, WM_LBUTTONUP, end);
                    check(
                        test.controller->current_region() == expected,
                        "near-corner side drag changes only its horizontal or vertical axis"
                    );
                    test.driver.expect(expected, false, "continuous edge publishes the correct resized region");
                    check_hollow(test.resize_input, expected);
                }
            }

            test.controller->set_region(region);
            check_hollow(test.resize_input, region);
        }

        int run_tests()
        {
            for (const int side : { 16, 120 })
            {
                for (const float scale : { 1.0f, 1.5f, 2.0f })
                {
                    const int halo { static_cast<int>(12.0f * scale) };
                    SkBitmap bitmap {};
                    bitmap.allocN32Pixels(side + halo * 2, side + halo * 2);

                    SkCanvas canvas { bitmap };
                    draw_frame_glow(canvas, side, side, halo, scale, SkColorSetRGB(76, 184, 150));
                    bool clear { true };

                    for (int y { halo }; y < halo + side; ++y)
                    {
                        for (int x { halo }; x < halo + side; ++x)
                        {
                            clear = clear && SkColorGetA(bitmap.getColor(x, y)) == 0;
                        }
                    }

                    check(clear, "glow leaves every capture pixel fully transparent at all tested DPI scales");

                    const SkColor light { bitmap.getColor(halo - 2, halo + side / 2) };
                    check(
                        SkColorGetA(light) > 0 && SkColorGetA(light) < 180 && SkColorGetG(light) > SkColorGetR(light),
                        "glow is a translucent colored halo outside the frame"
                    );
                    check(SkColorGetA(bitmap.getColor(0, 0)) == 0, "glow does not tint the whole outer window");
                }
            }

            const HRESULT initialized { CoInitializeEx(nullptr, COINIT_MULTITHREADED) };

            if (FAILED(initialized))
            {
                throw std::runtime_error { "Cannot initialize isolated UI Automation client." };
            }

            struct com_scope
            {
                ~com_scope()
                {
                    CoUninitialize();
                }
            };

            const com_scope com {};
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            WNDCLASSW config {};
            config.lpfnWndProc = owner_proc;
            config.hInstance = GetModuleHandleW(nullptr);
            config.lpszClassName = owner_class;

            if (RegisterClassW(&config) == 0)
            {
                throw std::runtime_error { "Cannot register isolated owner class." };
            }

            const capture_region bounds { GetSystemMetrics(SM_XVIRTUALSCREEN),
                                          GetSystemMetrics(SM_YVIRTUALSCREEN),
                                          GetSystemMetrics(SM_CXVIRTUALSCREEN),
                                          GetSystemMetrics(SM_CYVIRTUALSCREEN)
            };

            if (bounds.width < 200 || bounds.height < 160)
            {
                std::cout << "SKIP: isolated desktop has insufficient screen dimensions.\n";
                return skipped;
            }

            std::array<BYTE, 256> keyboard {};

            // 실행 중인 앱이 같은 전역 단축키를 사용하더라도 테스트가 빼앗지 않는다.
            hotkey_available_before_test
                = RegisterHotKey(nullptr, 91, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, '1') != FALSE;

            if (hotkey_available_before_test)
            {
                UnregisterHotKey(nullptr, 91);
            }

            check(SetKeyboardState(keyboard.data()) != FALSE, "isolated thread keyboard state initialized");
            {
                fixture test {};

                const capture_region region { bounds.x + 40, bounds.y + 40, 120, 80 };
                check_selection_input(test, bounds);
                test.select(region);
                check_hollow(test.frame, region);
                check(
                    (GetWindowLongPtrW(test.frame, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
                    "capture frame remains topmost independently of owner"
                );
                SetWindowPos(test.owner, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SetWindowPos(test.owner, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                check(
                    (GetWindowLongPtrW(test.frame, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
                    "owner topmost toggle preserves capture frame topmost policy"
                );
                check(
                    (GetWindowLongPtrW(test.resize_input, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
                    "owner topmost toggle preserves resize input topmost policy"
                );
                check(
                    GetWindow(test.resize_input, GW_OWNER) == test.frame,
                    "resize input follows capture frame rather than command window ownership"
                );
                check(
                    test.edit_button != nullptr && IsWindowVisible(test.edit_button),
                    "native accessible region edit button is visible"
                );
                check(
                    (GetWindowLongPtrW(test.edit_button, GWL_STYLE) & BS_TYPEMASK) == BS_PUSHBUTTON,
                    "edit control retains native pushbutton accessibility type"
                );
                SendMessageW(test.edit_button, BM_CLICK, 0, 0);
                check(
                    test.edit_requests == 1 && test.edit_thread == GetCurrentThreadId() && test.edit_capture_released,
                    "edit request runs on UI thread after releasing capture"
                );
                check(test.controller->current_region() == region, "edit button does not move capture geometry");
                HUIANODE edit_node {};
                HUIAPATTERNOBJECT invoke_pattern {};
                check(
                    SUCCEEDED(UiaNodeFromHandle(test.edit_button, &edit_node)) && edit_node != nullptr,
                    "native edit button exposes UI Automation node"
                );

                if (edit_node != nullptr)
                {
                    VARIANT property {};
                    check(
                        SUCCEEDED(UiaGetPropertyValue(edit_node, UIA_ControlTypePropertyId, &property))
                            && property.vt == VT_I4 && property.lVal == UIA_ButtonControlTypeId,
                        "edit provider exposes Button control type"
                    );
                    VariantClear(&property);
                    check(
                        SUCCEEDED(UiaGetPropertyValue(edit_node, UIA_NamePropertyId, &property))
                            && property.vt == VT_BSTR && property.bstrVal != nullptr
                            && std::wstring_view { property.bstrVal } == L"영역 설정",
                        "edit provider exposes accessible name"
                    );
                    VariantClear(&property);
                    check(
                        SUCCEEDED(UiaGetPropertyValue(edit_node, UIA_AutomationIdPropertyId, &property))
                            && property.vt == VT_BSTR && property.bstrVal != nullptr
                            && std::wstring_view { property.bstrVal } == L"101",
                        "edit provider preserves automation identifier"
                    );
                    VariantClear(&property);
                    check(
                        SUCCEEDED(UiaGetPatternProvider(edit_node, UIA_InvokePatternId, &invoke_pattern))
                            && invoke_pattern != nullptr,
                        "edit provider exposes Invoke pattern"
                    );

                    if (invoke_pattern != nullptr)
                    {
                        check(
                            SUCCEEDED(InvokePattern_Invoke(invoke_pattern)),
                            "UI Automation invoke posts native button action"
                        );
                        MSG click {};
                        check(
                            PeekMessageW(&click, test.edit_button, BM_CLICK, BM_CLICK, PM_REMOVE) != FALSE,
                            "Invoke queues UI thread button click"
                        );
                        DispatchMessageW(&click);
                        check(
                            test.edit_requests == 2 && test.edit_thread == GetCurrentThreadId()
                                && test.edit_capture_released,
                            "provider Invoke routes edit request on UI thread"
                        );
                    }
                }

                // 자식 버튼의 그리기도 전용 데스크톱에서 동기 검증한다.
                RedrawWindow(test.frame, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                check(IsWindowVisible(test.glow), "frame paints a visible outer glow layer");
                RECT initial_glow {};
                GetWindowRect(test.glow, &initial_glow);
                test.controller->set_glow_width(40);
                RECT wide_glow {};
                GetWindowRect(test.glow, &wide_glow);
                check(
                    wide_glow.right - wide_glow.left > initial_glow.right - initial_glow.left,
                    "increasing glow thickness expands the native halo window"
                );
                test.controller->set_glow_width(0);
                check(!IsWindowVisible(test.glow), "zero thickness hides the glow without hiding the capture frame");
                test.controller->set_glow_width(24);
                check(IsWindowVisible(test.glow), "restoring thickness shows the glow again");
                RECT frame_position {};
                GetWindowRect(test.frame, &frame_position);

                const HRGN corner_shape { CreateRectRgn(0, 0, 0, 0) };
                GetWindowRgn(test.frame, corner_shape);

                const int offset { MulDiv(4, static_cast<int>(GetDpiForWindow(test.frame)), 96) };
                check(
                    !PtInRegion(
                        corner_shape, region.x - offset - frame_position.left, region.y - offset - frame_position.top
                    ),
                    "corner grip leaves the square center empty"
                );
                RECT input_position {};
                GetWindowRect(test.resize_input, &input_position);
                GetWindowRgn(test.resize_input, corner_shape);
                check(
                    PtInRegion(
                        corner_shape, region.x - offset - input_position.left, region.y - offset - input_position.top
                    ) != FALSE,
                    "corner notch accepts clicks without rendering a square grip"
                );
                DeleteObject(corner_shape);
                check_resize_input(test, region, bounds);
                check_continuous_edges(test, bounds);
                test.controller->set_region(region);

                const LONG_PTR glow_style { GetWindowLongPtrW(test.glow, GWL_EXSTYLE) };
                check(
                    (glow_style & (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE))
                        == (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE),
                    "glow passes clicks without activating or moving focus"
                );
                RECT original_frame {};
                GetWindowRect(test.frame, &original_frame);
                test.controller->begin_selection();
                check(!IsWindowVisible(test.resize_input), "replacement selection hides resize padding");
                test.driver.expect(region, true, "replacement selection starts with prior committed region");
                test.mouse(WM_LBUTTONDOWN, { bounds.x + 10, bounds.y + 10 });
                test.mouse(WM_MOUSEMOVE, { bounds.x + 190, bounds.y + 140 });
                SendMessageW(test.selection_input, WM_KEYDOWN, VK_ESCAPE, 0);
                test.driver.expect(region, false, "cancel preserves prior committed geometry");
                check(
                    !IsWindowVisible(test.selection_input) && GetCapture() == nullptr,
                    "Escape hides selection input and releases an active drag"
                );
                RECT restored_frame {};
                GetWindowRect(test.frame, &restored_frame);
                check(EqualRect(&original_frame, &restored_frame) != FALSE, "cancel restores frame bounds");
                check_hollow(test.frame, region);

                test.controller->begin_selection();
                test.mouse(WM_LBUTTONDOWN, { bounds.x + 10, bounds.y + 10 });
                SetCapture(test.owner);
                test.driver.expect(
                    region, false, "capture stolen by another window cancels selection and preserves prior geometry"
                );
                check(
                    !IsWindowVisible(test.selection_input) && GetCapture() == test.owner,
                    "capture loss hides the input layer without releasing another window capture"
                );
                ReleaseCapture();
                test.controller->begin_selection();
                SendMessageW(test.selection_input, WM_CANCELMODE, 0, 0);
                test.driver.expect(region, false, "system cancel mode closes selection input");

                test.controller->begin_selection();
                RECT guide_position {};
                GetWindowRect(test.guide, &guide_position);

                const screen_point reverse_origin { guide_position.right - 20, guide_position.bottom - 10 };
                check(
                    WindowFromPoint({ reverse_origin.x, reverse_origin.y }) == test.guide,
                    "selection guide remains visible above the input layer"
                );
                send_pointer(test.guide, WM_LBUTTONDOWN, reverse_origin);
                check(
                    GetCapture() == test.selection_input,
                    "guide clicks are forwarded to selection input instead of passing to the desktop"
                );
                test.mouse(WM_LBUTTONUP, { reverse_origin.x - region.width, reverse_origin.y - region.height });
                test.driver.expect(
                    capture_region { reverse_origin.x - region.width,
                                     reverse_origin.y - region.height,
                                     region.width,
                                     region.height
                    },
                    false,
                    "reverse drag across native windows commits normalized screen coordinates"
                );
                test.controller->set_region(region);
                test.driver.expect(region, false, "restore fixture geometry after reverse selection");

                capture_region moved { region };
                ++moved.x;
                SendMessageW(test.frame, WM_KEYDOWN, VK_RIGHT, 0);
                test.driver.expect(moved, false, "arrow key moves region by one physical pixel");
                keyboard[VK_CONTROL] = 0x80;
                check(SetKeyboardState(keyboard.data()) != FALSE, "isolated control modifier set");

                capture_region resized { moved };
                ++resized.width;
                SendMessageW(test.frame, WM_KEYDOWN, VK_RIGHT, 0);
                test.driver.expect(resized, false, "control arrow resizes while retaining origin");
                keyboard[VK_CONTROL] = 0;
                SetKeyboardState(keyboard.data());
                check_hollow(test.frame, resized);

                test.select(bounds);

                // 화면 경계의 조절점은 안쪽으로 배치하되 중앙 화면은 가리지 않는다.
                check_hollow(test.frame, bounds, MulDiv(16, static_cast<int>(GetDpiForWindow(test.frame)), 96));
                check_hollow(test.resize_input, bounds, MulDiv(16, static_cast<int>(GetDpiForWindow(test.frame)), 96));
                check(!IsWindowVisible(test.edit_button), "header stays hidden when no space exists outside capture");

                const std::array<capture_region, 4> tiny_regions {
                    capture_region { bounds.x, bounds.y, 16, 16 },
                    capture_region { bounds.x + bounds.width - 16, bounds.y, 16, 16 },
                    capture_region { bounds.x, bounds.y + bounds.height - 16, 16, 16 },
                    capture_region { bounds.x + bounds.width - 16, bounds.y + bounds.height - 16, 16, 16 },
                };

                for (const capture_region tiny : tiny_regions)
                {
                    test.controller->set_region(tiny);

                    const int dpi { static_cast<int>(GetDpiForWindow(test.frame)) };
                    SendMessageW(test.frame, WM_DPICHANGED, MAKELONG(dpi, dpi), 0);
                    RedrawWindow(test.frame, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                    check_hollow(test.frame, tiny, 7);
                    check_hollow(test.resize_input, tiny, 7);
                    check(
                        test.controller->current_region() == tiny,
                        "DPI repaint preserves 16 by 16 physical pixel geometry at screen corners"
                    );
                }

                test.select(region);

                const bool excluded { test.controller->set_capture_active(true) };
                const auto frame_title = [&test] {
                    std::wstring title(256, L'\0');
                    title.resize(
                        static_cast<std::size_t>(
                            GetWindowTextW(test.frame, title.data(), static_cast<int>(title.size()))
                        )
                    );
                    return title;
                };
                test.controller->set_recording_elapsed(0);
                check(frame_title().find(L"녹화 · 00:00") != std::wstring::npos, "frame exposes recording start time");
                test.controller->set_recording_elapsed(65);
                check(frame_title().find(L"녹화 · 01:05") != std::wstring::npos, "frame exposes updated elapsed time");
                test.controller->set_recording_elapsed(3605);
                check(
                    frame_title().find(L"녹화 · 01:00:05") != std::wstring::npos,
                    "frame includes hours for long recordings"
                );
                check_hollow(test.frame, region);
                check(
                    !IsWindowVisible(test.resize_input), "capture hides resize input regardless of exclusion support"
                );
                send_pointer(test.resize_input, WM_LBUTTONDOWN, { region.x - offset, region.y - offset });
                check(GetCapture() == nullptr, "capture lock rejects stale input-layer clicks");
                check(!IsWindowEnabled(test.edit_button), "recording locks region edit button");
                SendMessageW(test.edit_button, BM_CLICK, 0, 0);
                SendMessageW(test.frame, WM_KEYDOWN, VK_RIGHT, 0);
                check(
                    test.edit_requests == 2 && test.controller->current_region() == region,
                    "capture active ignores editing and arrow input"
                );

                if (edit_node != nullptr)
                {
                    VARIANT enabled {};
                    check(
                        SUCCEEDED(UiaGetPropertyValue(edit_node, UIA_IsEnabledPropertyId, &enabled))
                            && enabled.vt == VT_BOOL && enabled.boolVal == VARIANT_FALSE,
                        "provider exposes disabled capture state"
                    );
                    VariantClear(&enabled);
                }

                if (invoke_pattern != nullptr)
                {
                    check(
                        InvokePattern_Invoke(invoke_pattern) == UIA_E_ELEMENTNOTENABLED,
                        "provider rejects Invoke during capture"
                    );
                }

                if (excluded)
                {
                    DWORD affinity {};
                    check(
                        GetWindowDisplayAffinity(test.frame, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE,
                        "visible active frame is excluded from capture"
                    );
                    check(
                        GetWindowDisplayAffinity(test.glow, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE,
                        "outer glow is also excluded from capture"
                    );
                    RedrawWindow(test.frame, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                }
                else
                {
                    check(!IsWindowVisible(test.frame), "unsupported exclusion hides active frame");
                    check(!IsWindowVisible(test.glow), "unsupported exclusion hides outer glow");
                }

                static_cast<void>(test.controller->set_capture_active(false));
                check(frame_title().find(L"녹화") == std::wstring::npos, "capture end clears frame timer");
                test.controller->set_recording_elapsed(120);
                check(
                    frame_title().find(L"녹화") == std::wstring::npos,
                    "late timer update cannot restore stopped recording state"
                );
                check(
                    IsWindowVisible(test.frame) && IsWindowEnabled(test.edit_button),
                    "capture end restores visible editable frame"
                );
                check(IsWindowVisible(test.resize_input), "capture end restores resize padding");
                check_hollow(test.frame, region);
                SendMessageW(test.frame, WM_LBUTTONDOWN, MK_LBUTTON, 0);
                check(GetCapture() == test.frame, "frame drag captures mouse on isolated desktop");
                SendMessageW(test.owner, WM_CLOSE, 0, 0);
                check_closed(test);

                if (invoke_pattern != nullptr)
                {
                    check(
                        FAILED(InvokePattern_Invoke(invoke_pattern)),
                        "stale provider Invoke fails after window teardown"
                    );
                    UiaPatternRelease(invoke_pattern);
                }

                if (edit_node != nullptr)
                {
                    UiaNodeRelease(edit_node);
                }
            }

            {
                fixture test { false };
                check(test.edit_button == nullptr, "legacy constructor creates no edit button without callback");
                test.controller->begin_selection();
                test.driver.expect(std::nullopt, true, "selection is active before owner close");
                test.mouse(WM_LBUTTONDOWN, { bounds.x + 10, bounds.y + 10 });
                SendMessageW(test.owner, WM_CLOSE, 0, 0);
                check_closed(test);
            }

            return failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

    } // namespace
} // namespace chalkag

int main()
{
    // 입력 데스크톱으로 전환하지 않으므로 테스트 창은 사람의 데스크톱에 나타나지 않는다.
    const std::wstring name { L"Chalkag.Region.Test." + std::to_wstring(GetCurrentProcessId()) + L"."
                              + std::to_wstring(GetTickCount64())
    };

    const HDESK desktop { CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr) };

    if (desktop == nullptr)
    {
        std::cout << "SKIP: cannot create isolated desktop (" << GetLastError() << ").\n";
        return chalkag::skipped;
    }

    int result { EXIT_FAILURE };

    const auto run = [&] {
        if (!SetThreadDesktop(desktop))
        {
            std::cout << "SKIP: cannot attach test thread to isolated desktop (" << GetLastError() << ").\n";
            result = chalkag::skipped;
            return;
        }

        try
        {
            result = chalkag::run_tests();
        }
        catch (const std::exception& error)
        {
            std::cerr << "FAIL: " << error.what() << '\n';
            result = EXIT_FAILURE;
        }
    };

    std::thread worker { run };
    worker.join();
    CloseDesktop(desktop);

    if (result == EXIT_SUCCESS)
    {
        std::cout << "Region controller integration tests passed.\n";
    }

    return result;
}
