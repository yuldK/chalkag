#include "capture/region_controller.h"
#include "capture/frame_glow.h"
#include "capture/recording_time.h"

#include "capture/region_events.h"
#include "luil/app/app_host.h"
#include "luil/theme/ui_theme.h"
#include "luil/ui/draw_primitives.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/ports/SkTypeface_win.h"

#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <ole2.h>
#include <shellscalingapi.h>
#include <uiautomation.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace chalkag {
    namespace {
        constexpr wchar_t frame_class[] { L"Chalkag.Region.Frame" };
        constexpr wchar_t glow_class[] { L"Chalkag.Region.Glow" };
        constexpr wchar_t drag_outline_class[] { L"Chalkag.Region.DragOutline" };
        constexpr wchar_t resize_input_class[] { L"Chalkag.Region.ResizeInput" };
        constexpr wchar_t selection_input_class[] { L"Chalkag.Region.SelectionInput" };
        constexpr wchar_t guide_class[] { L"Chalkag.Region.Guide" };
        constexpr wchar_t magnifier_class[] { L"Chalkag.Region.Magnifier" };
        constexpr wchar_t dispatcher_class[] { L"Chalkag.Region.Dispatcher" };
        constexpr UINT refresh_frame_message { WM_APP + 3 };
        constexpr int selection_hotkey { 1 };
        constexpr int edit_control { 101 };
        constexpr COLORREF frame_recording { RGB(246, 100, 119) };
        constexpr int magnifier_samples { 15 };

        class edit_button_provider final : public IRawElementProviderSimple, public IInvokeProvider
        {
        public:
            explicit edit_button_provider(const HWND window) noexcept
                : window_ { window }
            {}

            void invalidate() noexcept
            {
                window_.store(nullptr);
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) noexcept override
            {
                if (result == nullptr)
                {
                    return E_POINTER;
                }

                *result = nullptr;

                if (iid == IID_IUnknown || iid == __uuidof(IRawElementProviderSimple))
                {
                    *result = static_cast<IRawElementProviderSimple*>(this);
                }
                else if (iid == __uuidof(IInvokeProvider))
                {
                    *result = static_cast<IInvokeProvider*>(this);
                }
                else
                {
                    return E_NOINTERFACE;
                }

                AddRef();
                return S_OK;
            }

            ULONG STDMETHODCALLTYPE AddRef() noexcept override
            {
                return ++references_;
            }

            ULONG STDMETHODCALLTYPE Release() noexcept override
            {
                const ULONG remaining { --references_ };

                if (remaining == 0)
                {
                    delete this;
                }

                return remaining;
            }

            HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) noexcept override
            {
                if (result == nullptr)
                {
                    return E_POINTER;
                }

                // 조회는 원자적 HWND만 읽고 실제 버튼 동작은 UI 스레드에 게시한다.
                *result = ProviderOptions_ServerSideProvider;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetPatternProvider(const PATTERNID pattern, IUnknown** result) noexcept override
            {
                if (result == nullptr)
                {
                    return E_POINTER;
                }

                *result = nullptr;

                if (window_.load() == nullptr)
                {
                    return UIA_E_ELEMENTNOTAVAILABLE;
                }

                if (pattern == UIA_InvokePatternId)
                {
                    *result = static_cast<IInvokeProvider*>(this);
                    AddRef();
                }

                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetPropertyValue(const PROPERTYID property, VARIANT* result) noexcept override
            {
                if (result == nullptr)
                {
                    return E_POINTER;
                }

                VariantInit(result);

                const HWND window { window_.load() };

                if (window == nullptr || !IsWindow(window))
                {
                    return UIA_E_ELEMENTNOTAVAILABLE;
                }

                if (property == UIA_ControlTypePropertyId)
                {
                    result->vt = VT_I4;
                    result->lVal = UIA_ButtonControlTypeId;
                }
                else if (property == UIA_NamePropertyId || property == UIA_AutomationIdPropertyId)
                {
                    result->vt = VT_BSTR;
                    result->bstrVal = SysAllocString(property == UIA_NamePropertyId ? L"영역 설정" : L"101");

                    if (result->bstrVal == nullptr)
                    {
                        return E_OUTOFMEMORY;
                    }
                }
                else if (
                    property == UIA_IsEnabledPropertyId || property == UIA_IsControlElementPropertyId
                    || property == UIA_IsContentElementPropertyId || property == UIA_IsInvokePatternAvailablePropertyId
                )
                {
                    result->vt = VT_BOOL;
                    result->boolVal = property == UIA_IsEnabledPropertyId && !IsWindowEnabled(window) ? VARIANT_FALSE
                                                                                                      : VARIANT_TRUE;
                }

                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) noexcept override
            {
                if (result == nullptr)
                {
                    return E_POINTER;
                }

                *result = nullptr;

                const HWND window { window_.load() };

                if (window == nullptr || !IsWindow(window))
                {
                    return UIA_E_ELEMENTNOTAVAILABLE;
                }

                return UiaHostProviderFromHwnd(window, result);
            }

            HRESULT STDMETHODCALLTYPE Invoke() noexcept override
            {
                const HWND window { window_.load() };

                if (window == nullptr || !IsWindow(window))
                {
                    return UIA_E_ELEMENTNOTAVAILABLE;
                }

                if (!IsWindowEnabled(window))
                {
                    return UIA_E_ELEMENTNOTENABLED;
                }

                if (!PostMessageW(window, BM_CLICK, 0, 0))
                {
                    return UIA_E_ELEMENTNOTAVAILABLE;
                }

                return S_OK;
            }

        private:
            std::atomic<ULONG> references_ { 1 };
            std::atomic<HWND> window_ {};
        };

        [[nodiscard]] capture_region virtual_screen() noexcept
        {
            return { GetSystemMetrics(SM_XVIRTUALSCREEN),
                     GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN),
                     GetSystemMetrics(SM_CYVIRTUALSCREEN)
            };
        }

        [[nodiscard]] screen_point pointer_position() noexcept
        {
            POINT point {};
            GetCursorPos(&point);
            return { point.x, point.y };
        }

        [[nodiscard]] bool contains(const RECT& rect, const screen_point point) noexcept
        {
            return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
        }

        [[nodiscard]] std::u8string region_text(const capture_region& region)
        {
            const std::string text { std::to_string(region.width) + " x " + std::to_string(region.height) + " px ("
                                     + std::to_string(region.x) + ", " + std::to_string(region.y) + ")"
            };
            return { text.begin(), text.end() };
        }

        [[nodiscard]] HWND find_command_window() noexcept
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

        [[nodiscard]] std::wstring region_dimensions(const capture_region& region)
        {
            return std::to_wstring(region.width) + L" × " + std::to_wstring(region.height) + L" px";
        }

        [[nodiscard]] std::wstring wide_text(const std::u8string_view text)
        {
            const int length { MultiByteToWideChar(
                CP_UTF8, 0, reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()), nullptr, 0
            )
            };

            std::wstring wide(static_cast<std::size_t>(std::max(0, length)), L'\0');
            MultiByteToWideChar(
                CP_UTF8,
                0,
                reinterpret_cast<const char*>(text.data()),
                static_cast<int>(text.size()),
                wide.data(),
                length
            );
            return wide;
        }

        // 선택 입력 면이 가상 화면 전체를 덮어 WindowFromPoint가 항상 그 면을 돌려주므로
        // 최상위 창을 z 순서대로 훑으며 chalkag 창과 보이지 않는 창을 건너뛴다.
        [[nodiscard]] capture_region window_region_at(const screen_point point, const capture_region screen)
        {
            HWND window { GetTopWindow(nullptr) };

            for (int index = 0; window != nullptr && index < 4096; ++index, window = GetWindow(window, GW_HWNDNEXT))
            {
                RECT bounds {};

                // 비싼 DWM 조회 전에 창 사각형으로 먼저 거른다. 확장 경계는 항상 창 사각형 안에 있다.
                if (!IsWindowVisible(window) || IsIconic(window) || !GetWindowRect(window, &bounds)
                    || !contains(bounds, point))
                {
                    continue;
                }

                wchar_t name[64] {};
                GetClassNameW(window, name, 64);

                const std::wstring_view class_name { name };
                const LONG_PTR style { GetWindowLongPtrW(window, GWL_EXSTYLE) };

                if (class_name.starts_with(L"Chalkag.") || (style & WS_EX_TRANSPARENT) != 0)
                {
                    continue;
                }

                BYTE alpha { 255 };
                DWORD flags {};

                // 완전히 투명한 레이어 창은 화면에 보이지 않으므로 선택 대상이 아니다.
                if ((style & WS_EX_LAYERED) != 0 && GetLayeredWindowAttributes(window, nullptr, &alpha, &flags)
                    && (flags & LWA_ALPHA) != 0 && alpha == 0)
                {
                    continue;
                }

                DWORD cloaked {};

                if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
                {
                    continue;
                }

                // 보이지 않는 크기 조절 테두리를 빼야 창 가장자리 밖의 화면이 캡쳐에 섞이지 않는다.
                RECT visible {};

                if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))))
                {
                    bounds = visible;
                }

                if (!contains(bounds, point))
                {
                    continue;
                }

                // 바탕 화면을 누르면 창 선택 없이 기존 드래그 동작을 유지한다.
                if (class_name == L"Progman" || class_name == L"WorkerW")
                {
                    return {};
                }

                return region_from_window_bounds(
                    { bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top }, screen
                );
            }

            return {};
        }

        void fill_rect(const HDC device, const RECT& rect, const COLORREF color)
        {
            const HBRUSH brush { CreateSolidBrush(color) };
            FillRect(device, &rect, brush);
            DeleteObject(brush);
        }

        [[nodiscard]] SkColor skia_color(const COLORREF color)
        {
            return SkColorSetRGB(GetRValue(color), GetGValue(color), GetBValue(color));
        }

        [[nodiscard]] SkRect skia_rect(const RECT& rect)
        {
            return SkRect::MakeLTRB(
                static_cast<float>(rect.left),
                static_cast<float>(rect.top),
                static_cast<float>(rect.right),
                static_cast<float>(rect.bottom)
            );
        }

        void draw_rect(SkCanvas& canvas, const RECT& rect, const COLORREF color)
        {
            SkPaint paint {};
            paint.setColor(skia_color(color));
            canvas.drawRect(skia_rect(rect), paint);
        }

        void draw_rounded_rect(
            SkCanvas& canvas, const RECT& rect, const float radius, const COLORREF fill, const COLORREF outline
        )
        {
            SkPaint paint {};
            paint.setAntiAlias(true);
            paint.setColor(skia_color(fill));

            auto bounds { skia_rect(rect) };
            bounds.inset(0.5f, 0.5f);
            canvas.drawRoundRect(bounds, radius, radius, paint);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(1.0f);
            paint.setColor(skia_color(outline));
            canvas.drawRoundRect(bounds, radius, radius, paint);
        }

        void draw_label(
            SkCanvas& canvas, const std::wstring_view text, const RECT& rect, const float size, const COLORREF color
        )
        {
            static const sk_sp<SkFontMgr> manager { SkFontMgr_New_DirectWrite() };
            static const sk_sp<SkTypeface> typeface { manager != nullptr
                                                          ? manager->matchFamilyStyle("Segoe UI", SkFontStyle::Normal())
                                                          : nullptr
            };

            const int bytes { WideCharToMultiByte(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr
            )
            };

            std::u8string utf8(static_cast<std::size_t>(bytes), u8'\0');
            WideCharToMultiByte(
                CP_UTF8,
                0,
                text.data(),
                static_cast<int>(text.size()),
                reinterpret_cast<char*>(utf8.data()),
                bytes,
                nullptr,
                nullptr
            );

            const SkFont font { typeface, size };
            const auto label { luil::elide_text(utf8, static_cast<float>(rect.right - rect.left), font) };
            const float width { luil::measure_text(label, font) };
            SkPaint paint {};
            paint.setAntiAlias(true);
            paint.setColor(skia_color(color));
            canvas.save();
            canvas.clipRect(skia_rect(rect));
            luil::draw_text(
                canvas,
                label,
                static_cast<float>(rect.left) + (static_cast<float>(rect.right - rect.left) - width) / 2.0f,
                static_cast<float>(rect.top)
                    + luil::centered_text_baseline(font, static_cast<float>(rect.bottom - rect.top)),
                font,
                paint
            );
            canvas.restore();
        }

        class overlay_bitmap final
        {
        public:
            overlay_bitmap() = default;
            overlay_bitmap(const overlay_bitmap&) = delete;
            overlay_bitmap& operator=(const overlay_bitmap&) = delete;

            ~overlay_bitmap()
            {
                reset();
            }

            void resize(const int width, const int height)
            {
                if (size_.cx == width && size_.cy == height)
                {
                    return;
                }

                reset();

                if (width <= 0 || height <= 0 || static_cast<std::int64_t>(width) * height > 33554432)
                {
                    throw std::runtime_error { "Overlay bitmap dimensions are invalid." };
                }

                BITMAPINFO info {};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = width;
                info.bmiHeader.biHeight = -height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                void* pixels {};
                bitmap_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
                device_ = CreateCompatibleDC(nullptr);

                if (bitmap_ == nullptr || device_ == nullptr)
                {
                    reset();
                    throw std::runtime_error { "Cannot allocate Skia overlay bitmap." };
                }

                previous_ = SelectObject(device_, bitmap_);

                if (!image_.installPixels(
                        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
                        pixels,
                        static_cast<std::size_t>(width) * 4
                    ))
                {
                    reset();
                    throw std::runtime_error { "Cannot initialize Skia overlay pixels." };
                }

                size_ = { width, height };
            }

            [[nodiscard]] SkBitmap& image() noexcept
            {
                return image_;
            }

            void present(const HWND window, POINT origin, const BYTE alpha = 255) const
            {
                POINT source {};
                SIZE size { size_ };
                BLENDFUNCTION blend { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };

                if (!UpdateLayeredWindow(window, nullptr, &origin, &size, device_, &source, 0, &blend, ULW_ALPHA))
                {
                    throw std::runtime_error { "Cannot present Skia overlay bitmap." };
                }
            }

            void copy(const HDC target) const
            {
                // GDI는 Skia가 완성한 픽셀을 복사하는 용도로만 사용한다.
                BitBlt(target, 0, 0, size_.cx, size_.cy, device_, 0, 0, SRCCOPY);
            }

            // 캡쳐와 같은 방식으로 화면 픽셀을 읽고 GDI가 비워 둔 알파를 불투명으로 채운다.
            [[nodiscard]] bool read_screen(const int x, const int y)
            {
                const HDC screen { GetDC(nullptr) };

                const bool copied { screen != nullptr
                                    && BitBlt(device_, 0, 0, size_.cx, size_.cy, screen, x, y, SRCCOPY | CAPTUREBLT)
                                        != FALSE
                };

                if (screen != nullptr)
                {
                    ReleaseDC(nullptr, screen);
                }

                if (!copied)
                {
                    return false;
                }

                GdiFlush();

                for (int row = 0; row < size_.cy; ++row)
                {
                    for (int column = 0; column < size_.cx; ++column)
                    {
                        *image_.getAddr32(column, row) |= 0xff000000U;
                    }
                }

                return true;
            }

        private:
            void reset() noexcept
            {
                image_.reset();

                if (device_ != nullptr)
                {
                    if (previous_ != nullptr)
                    {
                        SelectObject(device_, previous_);
                    }

                    DeleteDC(device_);
                }

                if (bitmap_ != nullptr)
                {
                    DeleteObject(bitmap_);
                }

                device_ = nullptr;
                bitmap_ = nullptr;
                previous_ = nullptr;
                size_ = {};
            }

            HDC device_ {};
            HBITMAP bitmap_ {};
            HGDIOBJ previous_ {};
            SIZE size_ {};

            SkBitmap image_ {};
        };
    } // namespace

    class region_controller::impl final
    {
    public:
        explicit impl(luil::app_host& host, std::function<void()> edit_request, const bool manage_selection_hotkey)
            : host_ { &host }
            , owner_ { find_command_window() }
            , edit_request_ { std::move(edit_request) }
            , manage_selection_hotkey_ { manage_selection_hotkey }
        {
            try
            {
                register_class(dispatcher_class);
                register_class(frame_class);
                register_class(guide_class);
                register_class(glow_class);
                register_class(drag_outline_class);
                register_class(resize_input_class);
                register_class(selection_input_class);
                register_class(magnifier_class);
                dispatcher_ = create_window(dispatcher_class, 0, HWND_MESSAGE);
                frame_ = create_window(frame_class, WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, owner_);
                glow_ = create_window(
                    glow_class,
                    WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                    owner_
                );
                drag_outline_ = create_window(
                    drag_outline_class,
                    WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                    frame_
                );
                resize_input_ = create_window(
                    resize_input_class, WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, frame_
                );

                // 알파 0은 입력도 통과하므로 입력 전용 레이어는 최소 불투명도를 사용한다.
                if (!SetLayeredWindowAttributes(resize_input_, 0, 1, LWA_ALPHA))
                {
                    throw std::runtime_error { "Cannot initialize resize input layer." };
                }

                SetWindowTextW(resize_input_, L"캡쳐 영역 크기 조절");
                selection_input_
                    = create_window(selection_input_class, WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, owner_);

                if (!SetLayeredWindowAttributes(selection_input_, 0, 1, LWA_ALPHA))
                {
                    throw std::runtime_error { "Cannot initialize selection input layer." };
                }

                SetWindowTextW(selection_input_, L"캡쳐 영역 선택 · 클릭하면 창 선택 · Esc 또는 오른쪽 클릭으로 취소");
                guide_
                    = create_window(guide_class, WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, selection_input_);
                magnifier_ = create_window(
                    magnifier_class,
                    WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                    selection_input_
                );

                // 돋보기가 제 표본과 산출물에 찍히지 않아야 하므로 제외를 못 하면 표시하지 않는다.
                magnifier_excluded_ = SetWindowDisplayAffinity(magnifier_, WDA_EXCLUDEFROMCAPTURE) != FALSE;
                magnifier_sample_.resize(magnifier_samples, magnifier_samples);

                if (edit_request_)
                {
                    edit_button_ = CreateWindowExW(
                        0,
                        L"BUTTON",
                        L"영역 설정",
                        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | BS_NOTIFY,
                        0,
                        0,
                        1,
                        1,
                        frame_,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(edit_control)),
                        GetModuleHandleW(nullptr),
                        nullptr
                    );

                    if (edit_button_ == nullptr
                        || !SetWindowSubclass(edit_button_, edit_subclass, 1, reinterpret_cast<DWORD_PTR>(this)))
                    {
                        throw std::runtime_error { "Cannot create capture region edit button." };
                    }

                    edit_provider_ = new edit_button_provider { edit_button_ };
                }

                if (owner_ == nullptr
                    || !SetWindowSubclass(owner_, owner_subclass, 1, reinterpret_cast<DWORD_PTR>(this)))
                {
                    throw std::runtime_error { "Cannot attach capture window lifetime." };
                }

                if (manage_selection_hotkey_)
                {
                    hotkey_registered_
                        = RegisterHotKey(dispatcher_, selection_hotkey, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'A')
                        != FALSE;
                }

                publish(u8"버튼으로 캡쳐 영역을 선택하세요.");
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

        void begin_selection()
        {
            if (capture_active_ || shutting_down_)
            {
                return;
            }

            if (selecting_)
            {
                finish_selection(false);
                return;
            }

            cancel_drag();
            selecting_ = true;
            selection_dragging_ = false;
            selection_moved_ = false;
            preview_ = {};
            snap_region_ = {};
            hover_point_.reset();
            selection_foreground_acquired_ = false;
            ShowWindow(frame_, SW_HIDE);
            foreground_at_selection_ = GetForegroundWindow();

            const capture_region bounds { virtual_screen() };

            if (!SetWindowPos(
                    selection_input_, HWND_TOPMOST, bounds.x, bounds.y, bounds.width, bounds.height, SWP_NOACTIVATE
                ))
            {
                finish_selection(false);
                publish(u8"화면 선택을 시작할 수 없습니다. 선택 창 배치에 실패했습니다.");
                return;
            }

            // 알파 0은 클릭이 통과하므로 최소 불투명도의 밝은 입력 면을 사용한다.
            ShowWindow(selection_input_, SW_SHOW);

            if (SetForegroundWindow(selection_input_))
            {
                SetFocus(selection_input_);
            }

            selection_foreground_acquired_ = GetForegroundWindow() == selection_input_;
            RedrawWindow(selection_input_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            last_selection_input_ = std::chrono::steady_clock::now();
            position_guide();
            SetWindowTextW(guide_, L"드래그로 영역 선택 · 클릭하면 창 선택 · Esc 또는 오른쪽 클릭으로 취소");
            ShowWindow(guide_, SW_SHOWNOACTIVATE);
            SetTimer(dispatcher_, 1, 100, nullptr);
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            selection_hover(pointer_position());
            publish(u8"화면을 드래그하거나 창을 클릭하세요. Esc / 오른쪽 클릭: 취소");
        }

        [[nodiscard]] std::optional<capture_region> current_region() const
        {
            return selecting_ ? std::nullopt : region_;
        }

        void cancel_selection()
        {
            finish_selection(false);
        }

        void set_region(const capture_region region)
        {
            if (capture_active_ || shutting_down_)
            {
                return;
            }

            finish_selection(false);
            cancel_drag();
            region_ = fit_region_to_screen(region, virtual_screen());
            update_frame();
            publish(region_text(*region_));
        }

        void set_appearance(const luil::appearance_settings& appearance)
        {
            appearance_ = appearance;
            refresh_appearance();
        }

        void set_glow_width(const int width)
        {
            glow_width_ = std::clamp(width, 0, 40);
            update_glow();
        }

        void set_selection_shortcut_text(std::u8string text)
        {
            selection_shortcut_ = std::move(text);
        }

        void refresh_appearance() noexcept
        {
            DWORD light {};
            DWORD size { sizeof(light) };

            if (RegGetValueW(
                    HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    L"AppsUseLightTheme",
                    RRF_RT_REG_DWORD,
                    nullptr,
                    &light,
                    &size
                )
                != ERROR_SUCCESS)
            {
                light = 0;
            }

            const bool use_light { appearance_.theme == luil::theme_preference::light
                                   || (appearance_.theme == luil::theme_preference::system && light != 0)
            };

            const auto palette { luil::color_palette_for(
                use_light ? luil::color_theme::light : luil::color_theme::dark, luil::accent_for(appearance_.accent_id)
            )
            };

            const auto color = [background = palette.window_background](const luil::ui_color value) -> COLORREF {
                const auto alpha { (value >> 24) & 255U };
                const auto channel = [value, background, alpha](const unsigned int shift) -> BYTE {
                    return static_cast<BYTE>(
                        (((value >> shift) & 255U) * alpha + ((background >> shift) & 255U) * (255U - alpha)) / 255U
                    );
                };
                return RGB(channel(16), channel(8), channel(0));
            };
            frame_background_ = color(palette.window_background);
            frame_foreground_ = color(palette.primary_foreground);
            frame_accent_ = color(palette.accent);
            frame_hover_ = color(palette.accent_hover);
            frame_corner_ = color(palette.accent_soft);
            frame_border_ = color(palette.control_border);
            frame_muted_ = color(palette.secondary_foreground);
            frame_disabled_ = color(palette.disabled_foreground);
            frame_button_hover_ = color(palette.button_hover_background);
            HIGHCONTRASTW contrast { sizeof(HIGHCONTRASTW) };

            if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
                && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0)
            {
                frame_background_ = GetSysColor(COLOR_WINDOW);
                frame_foreground_ = frame_muted_ = frame_border_ = GetSysColor(COLOR_WINDOWTEXT);
                frame_accent_ = frame_hover_ = frame_corner_ = GetSysColor(COLOR_HIGHLIGHT);
                frame_disabled_ = GetSysColor(COLOR_GRAYTEXT);
                frame_button_hover_ = GetSysColor(COLOR_BTNFACE);
            }

            for (const HWND window : { frame_, edit_button_, resize_input_, guide_ })
            {
                if (window != nullptr)
                {
                    InvalidateRect(window, nullptr, FALSE);
                }
            }

            drag_outline_size_ = {};
            update_glow();
        }

        [[nodiscard]] bool set_capture_active(const bool active)
        {
            cancel_drag();

            if (selecting_)
            {
                finish_selection(false);
            }

            capture_active_ = active;
            recording_elapsed_.reset();

            if (active)
            {
                ShowWindow(resize_input_, SW_HIDE);
            }

            if (edit_button_ != nullptr)
            {
                EnableWindow(edit_button_, !active);
            }

            const DWORD affinity { static_cast<DWORD>(active ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) };
            const bool frame_excluded { SetWindowDisplayAffinity(frame_, affinity) != FALSE };
            const bool glow_excluded { SetWindowDisplayAffinity(glow_, affinity) != FALSE };
            const bool excluded { frame_excluded && glow_excluded };
            capture_excluded_ = excluded;
            SetWindowDisplayAffinity(guide_, affinity);
            update_glow();

            // 제외를 지원하지 않는 환경에서도 조절점이 캡쳐에 섞이지 않는다.
            if (active && !excluded)
            {
                ShowWindow(glow_, SW_HIDE);
                ShowWindow(frame_, SW_HIDE);
            }

            if (!active && region_.has_value())
            {
                update_frame();
            }

            InvalidateRect(frame_, nullptr, FALSE);

            if (edit_button_ != nullptr)
            {
                InvalidateRect(edit_button_, nullptr, FALSE);
            }

            return excluded;
        }

        void set_recording_elapsed(const std::uint64_t seconds)
        {
            if (!capture_active_ || shutting_down_ || recording_elapsed_ == seconds)
            {
                return;
            }

            const bool resize_header {
                !recording_elapsed_ || recording_time(*recording_elapsed_).size() != recording_time(seconds).size()
            };
            recording_elapsed_ = seconds;

            if (resize_header && region_)
            {
                update_frame();
            }
            else
            {
                update_frame_title();
                InvalidateRect(frame_, &header_, FALSE);
            }
        }

    private:
        [[nodiscard]] std::wstring elapsed_text() const
        {
            const auto text { recording_time(recording_elapsed_.value_or(0)) };
            return { text.begin(), text.end() };
        }

        void update_frame_title()
        {
            if (!(selecting_ ? preview_.valid() : region_.has_value()))
            {
                return;
            }

            const capture_region visible { visible_region() };
            const auto recording { recording_elapsed_ ? L" · 녹화 · " + elapsed_text() : std::wstring {} };
            const auto title { L"캡쳐 영역 " + region_dimensions(visible) + L" · " + std::to_wstring(visible.x) + L", "
                               + std::to_wstring(visible.y) + recording
            };
            SetWindowTextW(frame_, title.c_str());
        }

        struct handle_rect
        {
            resize_edge edge {};
            RECT rect {};
            RECT hit_rect {};
        };

        [[nodiscard]] capture_region visible_region() const
        {
            if (selecting_)
            {
                return preview_;
            }

            return drag_edge_ == resize_edge::move ? drag_preview_ : *region_;
        }

        [[nodiscard]] static std::array<RECT, 2> handle_bars(const handle_rect& handle)
        {
            const RECT r { handle.rect };
            const LONG thickness { std::max<LONG>(2, (r.right - r.left) / 3) };

            switch (handle.edge)
            {
            case resize_edge::top_left:
                return { RECT { r.left, r.top, r.right, r.top + thickness },
                         RECT { r.left, r.top, r.left + thickness, r.bottom }
                };

            case resize_edge::top_right:
                return { RECT { r.left, r.top, r.right, r.top + thickness },
                         RECT { r.right - thickness, r.top, r.right, r.bottom }
                };

            case resize_edge::bottom_left:
                return { RECT { r.left, r.bottom - thickness, r.right, r.bottom },
                         RECT { r.left, r.top, r.left + thickness, r.bottom }
                };

            case resize_edge::bottom_right:
                return { RECT { r.left, r.bottom - thickness, r.right, r.bottom },
                         RECT { r.right - thickness, r.top, r.right, r.bottom }
                };

            case resize_edge::left:

            case resize_edge::right:
                return { RECT { r.left + (r.right - r.left - thickness) / 2,
                                r.top,
                                r.left + (r.right - r.left + thickness) / 2,
                                r.bottom
                },
                         RECT {}
                };

            default:
                return { RECT { r.left,
                                r.top + (r.bottom - r.top - thickness) / 2,
                                r.right,
                                r.top + (r.bottom - r.top + thickness) / 2
                },
                         RECT {}
                };
            }
        }

        void update_glow()
        {
            if (glow_ == nullptr)
            {
                return;
            }

            if (glow_width_ == 0 || !IsWindowVisible(frame_) || !(selecting_ ? preview_.valid() : region_.has_value())
                || (drag_edge_ != resize_edge::none && drag_edge_ != resize_edge::move))
            {
                ShowWindow(glow_, SW_HIDE);
                return;
            }

            // 제외 실패 환경에서도 장식이 산출물에 섞이지 않게 한다.
            DWORD affinity {};

            if (capture_active_ && (!GetWindowDisplayAffinity(glow_, &affinity) || affinity != WDA_EXCLUDEFROMCAPTURE))
            {
                ShowWindow(glow_, SW_HIDE);
                return;
            }

            const capture_region visible { visible_region() };
            const float scale { static_cast<float>(GetDpiForWindow(frame_)) / 96.0f };
            const int halo { std::max(1, static_cast<int>(std::ceil(static_cast<float>(glow_width_) * scale))) };
            const int width { visible.width + halo * 2 };
            const int height { visible.height + halo * 2 };
            const COLORREF color { capture_active_ ? frame_recording
                                       : header_hover_ || hovered_edge_ != resize_edge::none
                                           || drag_edge_ != resize_edge::none
                                       ? frame_hover_
                                       : frame_accent_
            };

            // 이동만 하면 기존 알파 비트맵을 재사용하여 큰 프레임을 다시 래스터화하지 않는다.
            if (glow_size_.cx == width && glow_size_.cy == height && glow_color_ == color && glow_scale_ == scale)
            {
                SetWindowPos(
                    glow_, frame_, visible.x - halo, visible.y - halo, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW
                );
                return;
            }

            if (static_cast<std::int64_t>(width) * height > 33554432)
            {
                ShowWindow(glow_, SW_HIDE);
                return;
            }

            BITMAPINFO info {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels {};

            const HBITMAP bitmap { CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) };
            const HDC device { CreateCompatibleDC(nullptr) };

            if (bitmap == nullptr || device == nullptr)
            {
                if (bitmap != nullptr)
                {
                    DeleteObject(bitmap);
                }

                if (device != nullptr)
                {
                    DeleteDC(device);
                }

                ShowWindow(glow_, SW_HIDE);
                return;
            }

            const HGDIOBJ previous { SelectObject(device, bitmap) };
            SkBitmap image {};

            if (image.installPixels(
                    SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
                    pixels,
                    static_cast<std::size_t>(width) * 4
                ))
            {
                SkCanvas canvas { image };
                draw_frame_glow(
                    canvas,
                    visible.width,
                    visible.height,
                    halo,
                    scale,
                    SkColorSetRGB(GetRValue(color), GetGValue(color), GetBValue(color))
                );
                POINT origin { visible.x - halo, visible.y - halo };
                SIZE size { width, height };
                POINT source {};
                BLENDFUNCTION blend { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

                if (UpdateLayeredWindow(glow_, nullptr, &origin, &size, device, &source, 0, &blend, ULW_ALPHA))
                {
                    glow_size_ = size;
                    glow_color_ = color;
                    glow_scale_ = scale;
                    SetWindowPos(glow_, frame_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
                else
                {
                    ShowWindow(glow_, SW_HIDE);
                }
            }

            SelectObject(device, previous);
            DeleteDC(device);
            DeleteObject(bitmap);
        }

        void shutdown() noexcept
        {
            // host는 run_application_window 반환 때 이미 파괴되므로 이후에는 게시하지 않는다.
            host_ = nullptr;
            shutting_down_ = true;
            selecting_ = false;
            drag_edge_ = resize_edge::none;
            release_selection_input();

            if (dispatcher_ != nullptr)
            {
                KillTimer(dispatcher_, 1);

                if (hotkey_registered_)
                {
                    UnregisterHotKey(dispatcher_, selection_hotkey);
                }
            }

            if (GetCapture() == frame_)
            {
                ReleaseCapture();
            }

            if (owner_ != nullptr)
            {
                RemoveWindowSubclass(owner_, owner_subclass, 1);
            }

            for (const HWND window :
                 { drag_outline_, resize_input_, glow_, magnifier_, guide_, selection_input_, frame_, dispatcher_ })
            {
                if (window != nullptr)
                {
                    DestroyWindow(window);
                }
            }

            if (edit_provider_ != nullptr)
            {
                edit_provider_->invalidate();
                edit_provider_->Release();
                edit_provider_ = nullptr;
            }

            glow_ = nullptr;
            magnifier_ = nullptr;
            drag_outline_ = nullptr;
            resize_input_ = nullptr;
            guide_ = nullptr;
            selection_input_ = nullptr;
            frame_ = nullptr;
            edit_button_ = nullptr;
            dispatcher_ = nullptr;
            hotkey_registered_ = false;
        }

        void register_class(const wchar_t* name)
        {
            WNDCLASSW config {};
            // 장식 레이어는 입력과 그리기 없이 알파 비트맵만 제시한다.
            const std::wstring_view class_name { name };
            config.lpfnWndProc
                = class_name == glow_class || class_name == magnifier_class ? DefWindowProcW : window_proc;
            config.hInstance = GetModuleHandleW(nullptr);
            config.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            config.lpszClassName = name;

            if (RegisterClassW(&config) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                throw std::runtime_error { "Cannot register capture window class." };
            }
        }

        [[nodiscard]] HWND create_window(const wchar_t* name, const DWORD extended_style, const HWND owner)
        {
            const HWND window { CreateWindowExW(
                extended_style, name, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), this
            )
            };

            if (window == nullptr)
            {
                throw std::runtime_error { "Cannot create capture window." };
            }

            return window;
        }

        // 선택 종료·취소·입력 상실이 모두 같은 경로에서 마우스 캡쳐와 입력 창을 정리한다.
        void release_selection_input() noexcept
        {
            if (selection_input_ != nullptr)
            {
                if (GetCapture() == selection_input_)
                {
                    ReleaseCapture();
                }

                ShowWindow(selection_input_, SW_HIDE);
            }

            if (magnifier_ != nullptr)
            {
                ShowWindow(magnifier_, SW_HIDE);
            }
        }

        void publish(std::u8string status = {})
        {
            if (host_ != nullptr)
            {
                // 단축키 표기마다 받침이 달라 조사 없이 "단축키를" 형태로 안내한다.
                if (manage_selection_hotkey_ && !hotkey_registered_)
                {
                    status += selection_shortcut_.empty()
                        ? u8" · 영역 선택 단축키를 다른 앱이 사용 중입니다."
                        : u8" · " + selection_shortcut_ + u8" 단축키를 다른 앱이 사용 중입니다.";
                }

                host_->post_app_message(
                    luil::app_message { region_state_intent { region_, selecting_, std::move(status) } }
                );
            }
        }

        void position_guide()
        {
            const screen_point pointer { pointer_position() };
            const POINT native_pointer { pointer.x, pointer.y };
            const HMONITOR monitor { MonitorFromPoint(native_pointer, MONITOR_DEFAULTTONEAREST) };
            MONITORINFO info { sizeof(MONITORINFO) };
            GetMonitorInfoW(monitor, &info);

            const int dpi { static_cast<int>(GetDpiForWindow(owner_)) };
            const int width { MulDiv(460, dpi > 0 ? dpi : 96, 96) };
            const int height { MulDiv(72, dpi > 0 ? dpi : 96, 96) };
            SetWindowPos(
                guide_,
                HWND_TOPMOST,
                info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2,
                info.rcWork.top + 32,
                width,
                height,
                SWP_NOACTIVATE
            );
        }

        void finish_selection(const bool commit)
        {
            if (!selecting_)
            {
                return;
            }

            const HWND foreground { GetForegroundWindow() };
            const bool restore_focus { foreground == selection_input_ || foreground == guide_ || foreground == frame_ };
            selecting_ = false;
            selection_dragging_ = false;
            selection_moved_ = false;
            snap_region_ = {};
            hover_point_.reset();
            magnifier_point_.reset();
            release_selection_input();
            KillTimer(dispatcher_, 1);
            ShowWindow(guide_, SW_HIDE);
            ShowWindow(frame_, SW_HIDE);
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));

            if (commit)
            {
                region_ = fit_region_to_screen(preview_, virtual_screen());
            }

            if (region_.has_value())
            {
                update_frame();
            }

            if (restore_focus && owner_ != nullptr && SetForegroundWindow(owner_))
            {
                SetFocus(owner_);
            }

            publish(commit ? region_text(*region_) : u8"영역 선택을 취소했습니다.");
        }

        void selection_mouse(const UINT message, const screen_point point)
        {
            if (!selecting_)
            {
                return;
            }

            last_selection_input_ = std::chrono::steady_clock::now();

            if (message == WM_RBUTTONUP)
            {
                finish_selection(false);
                return;
            }

            if (message == WM_LBUTTONDOWN)
            {
                SetForegroundWindow(selection_input_);
                SetFocus(selection_input_);
                selection_foreground_acquired_
                    = selection_foreground_acquired_ || GetForegroundWindow() == selection_input_;
                selection_origin_ = point;
                selection_dragging_ = true;
                selection_moved_ = false;
                SetCapture(selection_input_);
            }

            if (!selection_dragging_ && message == WM_MOUSEMOVE)
            {
                selection_hover(point);
                return;
            }

            if (selection_dragging_ && (message == WM_MOUSEMOVE || message == WM_LBUTTONUP))
            {
                // 드래그 임계값을 한 번 넘으면 다시 돌아와도 창 선택으로 바뀌지 않는다.
                selection_moved_ = selection_moved_
                    || std::abs(point.x - selection_origin_.x) > GetSystemMetrics(SM_CXDRAG)
                    || std::abs(point.y - selection_origin_.y) > GetSystemMetrics(SM_CYDRAG);

                if (!selection_moved_ && snap_region_.valid())
                {
                    if (message == WM_LBUTTONUP)
                    {
                        preview_ = snap_region_;
                        finish_selection(true);
                    }
                    else
                    {
                        update_magnifier(point);
                    }

                    return;
                }

                preview_ = region_from_drag(selection_origin_, point, virtual_screen());

                if (preview_.valid())
                {
                    update_frame();
                }

                InvalidateRect(guide_, nullptr, FALSE);
                update_magnifier(point);

                if (message == WM_LBUTTONUP)
                {
                    if (preview_.width >= minimum_region_size && preview_.height >= minimum_region_size)
                    {
                        finish_selection(true);
                    }
                    else
                    {
                        selection_dragging_ = false;
                        selection_moved_ = false;

                        if (GetCapture() == selection_input_)
                        {
                            ReleaseCapture();
                        }

                        ShowWindow(frame_, SW_HIDE);
                        preview_ = {};
                        hover_point_.reset();
                        InvalidateRect(guide_, nullptr, FALSE);
                        update_magnifier(point);
                        publish(u8"최소 16 x 16 px 이상의 영역을 다시 드래그하세요.");
                    }
                }
            }
        }

        // 드래그 전에는 포인터 아래 창의 보이는 경계를 윤곽선으로만 보여 준다.
        void selection_hover(const screen_point point)
        {
            if (!selecting_ || selection_dragging_)
            {
                return;
            }

            if (hover_point_ && hover_point_->x == point.x && hover_point_->y == point.y)
            {
                return;
            }

            hover_point_ = point;
            snap_region_ = window_region_at(point, virtual_screen());

            if (snap_region_ != preview_)
            {
                preview_ = snap_region_;

                if (preview_.valid())
                {
                    update_frame();
                }
                else
                {
                    ShowWindow(frame_, SW_HIDE);
                }

                InvalidateRect(guide_, nullptr, FALSE);
            }

            update_magnifier(point);
        }

        void update_magnifier(const screen_point point)
        {
            magnifier_point_ = point;

            if (magnifier_ == nullptr || !magnifier_excluded_ || !selecting_)
            {
                if (magnifier_ != nullptr)
                {
                    ShowWindow(magnifier_, SW_HIDE);
                }

                return;
            }

            // 돋보기 실패가 선택 자체를 취소하지 않도록 이 경계에서 정리한다.
            try
            {
                render_magnifier(point);
            }
            catch (...)
            {
                ShowWindow(magnifier_, SW_HIDE);
            }
        }

        void render_magnifier(const screen_point point)
        {
            const POINT native { point.x, point.y };
            const HMONITOR monitor { MonitorFromPoint(native, MONITOR_DEFAULTTONEAREST) };
            MONITORINFO info { sizeof(MONITORINFO) };

            if (!GetMonitorInfoW(monitor, &info))
            {
                ShowWindow(magnifier_, SW_HIDE);
                return;
            }

            // 창이 아직 다른 모니터에 있을 수 있으므로 포인터가 있는 모니터의 DPI를 쓴다.
            UINT dpi_x {};
            UINT dpi_y {};

            const int dpi { SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x > 0
                                ? static_cast<int>(dpi_x)
                                : 96
            };
            const int cell { std::max(4, MulDiv(8, dpi, 96)) };
            const int inset { std::max(1, MulDiv(2, dpi, 96)) };
            const int zoom { cell * magnifier_samples };
            const int text_height { MulDiv(38, dpi, 96) };
            const int width { zoom + inset * 2 };
            const int height { zoom + inset * 2 + text_height };
            const int half { magnifier_samples / 2 };

            if (!magnifier_sample_.read_screen(point.x - half, point.y - half))
            {
                SkCanvas sample { magnifier_sample_.image() };
                sample.clear(skia_color(frame_background_));
            }

            magnifier_bitmap_.resize(width, height);

            SkCanvas canvas { magnifier_bitmap_.image() };
            canvas.clear(SK_ColorTRANSPARENT);

            const float radius { static_cast<float>(MulDiv(8, dpi, 96)) };
            draw_rounded_rect(canvas, { 0, 0, width, height }, radius, frame_background_, frame_border_);

            const SkRect zoom_rect { SkRect::MakeXYWH(
                static_cast<float>(inset), static_cast<float>(inset), static_cast<float>(zoom), static_cast<float>(zoom)
            )
            };

            // 확대 픽셀은 보간 없이 그려야 경계 픽셀을 정확히 고를 수 있다.
            canvas.save();
            canvas.clipRRect(SkRRect::MakeRectXY(zoom_rect, radius - 1.0f, radius - 1.0f), true);
            canvas.drawImageRect(
                magnifier_sample_.image().asImage(), zoom_rect, SkSamplingOptions { SkFilterMode::kNearest }
            );
            canvas.restore();

            // 십자선은 중앙 픽셀을 가리지 않도록 그 바깥에만 긋는다.
            const float cell_size { static_cast<float>(cell) };
            const float line { static_cast<float>(std::max(1, MulDiv(1, dpi, 96))) };
            const float center_left { zoom_rect.left() + cell_size * static_cast<float>(half) };
            const float center_top { zoom_rect.top() + cell_size * static_cast<float>(half) };
            const float line_start { (cell_size - line) / 2.0f };
            const float line_end { (cell_size + line) / 2.0f };
            SkPaint cross {};
            cross.setColor(skia_color(frame_accent_));
            cross.setAlpha(150);

            const std::array<SkRect, 4> cross_lines {
                SkRect::MakeLTRB(zoom_rect.left(), center_top + line_start, center_left, center_top + line_end),
                SkRect::MakeLTRB(
                    center_left + cell_size, center_top + line_start, zoom_rect.right(), center_top + line_end
                ),
                SkRect::MakeLTRB(center_left + line_start, zoom_rect.top(), center_left + line_end, center_top),
                SkRect::MakeLTRB(
                    center_left + line_start, center_top + cell_size, center_left + line_end, zoom_rect.bottom()
                ),
            };

            for (const SkRect& cross_line : cross_lines)
            {
                canvas.drawRect(cross_line, cross);
            }

            // 중앙 픽셀은 밝고 어두운 두 겹 테두리로 어떤 배경에서도 보이게 한다.
            SkPaint outline {};
            outline.setStyle(SkPaint::kStroke_Style);
            outline.setStrokeWidth(line);
            outline.setColor(skia_color(frame_background_));

            auto center { SkRect::MakeXYWH(center_left, center_top, cell_size, cell_size) };
            center.outset(line / 2.0f, line / 2.0f);
            canvas.drawRect(center, outline);
            center.inset(line, line);
            outline.setColor(skia_color(frame_foreground_));
            canvas.drawRect(center, outline);

            const auto position { std::to_wstring(point.x) + L", " + std::to_wstring(point.y) };
            const auto size { preview_.valid() ? region_dimensions(preview_) : std::wstring { L"드래그 또는 클릭" } };
            const int text_top { inset * 2 + zoom };
            const int line_height { (text_height - inset) / 2 };
            const float font_size { 11.0f * static_cast<float>(dpi) / 96.0f };
            draw_label(
                canvas,
                position,
                { inset, text_top, width - inset, text_top + line_height },
                font_size,
                frame_foreground_
            );
            draw_label(
                canvas,
                size,
                { inset, text_top + line_height, width - inset, text_top + line_height * 2 },
                font_size,
                frame_muted_
            );

            const screen_point origin { magnifier_origin(
                point,
                width,
                height,
                MulDiv(24, dpi, 96),
                { info.rcMonitor.left,
                  info.rcMonitor.top,
                  info.rcMonitor.right - info.rcMonitor.left,
                  info.rcMonitor.bottom - info.rcMonitor.top
                }
            )
            };

            const auto title { L"돋보기 " + position + L" · " + size };
            SetWindowTextW(magnifier_, title.c_str());
            magnifier_bitmap_.present(magnifier_, { origin.x, origin.y });
            SetWindowPos(
                magnifier_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW
            );
        }

        void update_frame()
        {
            if (rebuilding_frame_)
            {
                return;
            }

            struct frame_update_scope
            {
                bool& rebuilding;

                ~frame_update_scope()
                {
                    rebuilding = false;
                }
            };

            rebuilding_frame_ = true;

            const frame_update_scope update_scope { rebuilding_frame_ };
            const capture_region visible { visible_region() };

            if (!visible.valid())
            {
                return;
            }

            const POINT center { visible.x + visible.width / 2, visible.y + visible.height / 2 };
            const HMONITOR monitor { MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST) };
            MONITORINFO info { sizeof(MONITORINFO) };
            GetMonitorInfoW(monitor, &info);

            // 픽셀 영역은 그대로 두고 핸들·글자만 해당 모니터 DPI에 맞춘다.
            const UINT dpi { GetDpiForWindow(frame_) };
            const int scale_dpi { dpi > 0 ? static_cast<int>(dpi) : 96 };
            const capture_region bounds { virtual_screen() };

            // 오른쪽·아래의 시작 경계도 중앙을 덮지 않도록 반 크기보다 작게 제한한다.
            const int grip { std::min(
                std::max(6, MulDiv(8, scale_dpi, 96)), std::max(1, (std::min(visible.width, visible.height) - 1) / 2)
            )
            };
            const int header_height { MulDiv(30, scale_dpi, 96) };
            const int elapsed_width { recording_elapsed_ ? static_cast<int>(elapsed_text().size() - 5) * 8 : 0 };
            const int minimum_header { (edit_button_ != nullptr ? 224 : 196)
                                       + (recording_elapsed_ ? 90 + elapsed_width : 0)
            };

            const int header_width { std::min(
                bounds.width,
                std::max(MulDiv(minimum_header, scale_dpi, 96), std::min(visible.width, MulDiv(280, scale_dpi, 96)))
            )
            };
            int header_x { visible.x + (visible.width - header_width) / 2 };
            header_x = std::clamp(header_x, bounds.x, bounds.x + std::max(0, bounds.width - header_width));
            int header_y { visible.y - grip - header_height };
            bool header_visible { true };

            if (header_y < info.rcMonitor.top)
            {
                header_y = visible.y + visible.height + grip;

                if (header_y + header_height > info.rcMonitor.bottom)
                {
                    header_visible = false;
                }
            }

            frame_origin_ = { std::min(visible.x - grip, header_x), std::min(visible.y - grip, header_y) };

            const int frame_right { std::max(visible.x + visible.width + grip, header_x + header_width) };
            const int frame_bottom { std::max(visible.y + visible.height + grip, header_y + header_height) };
            inner_ = { visible.x - frame_origin_.x,
                       visible.y - frame_origin_.y,
                       visible.x + visible.width - frame_origin_.x,
                       visible.y + visible.height - frame_origin_.y
            };
            header_ = { header_x - frame_origin_.x,
                        header_y - frame_origin_.y,
                        header_x + header_width - frame_origin_.x,
                        header_y + header_height - frame_origin_.y
            };

            if (!header_visible)
            {
                header_ = {};
            }

            if (edit_button_ != nullptr)
            {
                const int button_size { MulDiv(24, scale_dpi, 96) };
                const int padding { MulDiv(3, scale_dpi, 96) };
                SetWindowPos(
                    edit_button_,
                    nullptr,
                    header_.right - padding - button_size,
                    header_.top + padding,
                    button_size,
                    button_size,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW
                );
                ShowWindow(edit_button_, header_visible && !selecting_ ? SW_SHOWNOACTIVATE : SW_HIDE);
            }

            const int mid_x { (inner_.left + inner_.right) / 2 };
            const int mid_y { (inner_.top + inner_.bottom) / 2 };
            const auto make_handles = [this, mid_x, mid_y](const int size) -> std::array<handle_rect, 8> {
                const int half { size / 2 };
                return {
                    handle_rect { resize_edge::top_left,
                                  { inner_.left - size, inner_.top - size, inner_.left, inner_.top }
                    },
                    handle_rect { resize_edge::top_right,
                                  { inner_.right, inner_.top - size, inner_.right + size, inner_.top }
                    },
                    handle_rect { resize_edge::bottom_left,
                                  { inner_.left - size, inner_.bottom, inner_.left, inner_.bottom + size }
                    },
                    handle_rect { resize_edge::bottom_right,
                                  { inner_.right, inner_.bottom, inner_.right + size, inner_.bottom + size }
                    },
                    handle_rect { resize_edge::left,
                                  { inner_.left - size, mid_y - half, inner_.left, mid_y - half + size }
                    },
                    handle_rect { resize_edge::right,
                                  { inner_.right, mid_y - half, inner_.right + size, mid_y - half + size }
                    },
                    handle_rect { resize_edge::top,
                                  { mid_x - half, inner_.top - size, mid_x - half + size, inner_.top }
                    },
                    handle_rect { resize_edge::bottom,
                                  { mid_x - half, inner_.bottom, mid_x - half + size, inner_.bottom + size }
                    },
                };
            };
            handles_ = make_handles(grip);

            const int hit_size { std::min(
                std::max(grip, MulDiv(28, scale_dpi, 96)),
                std::max(1, (std::min(visible.width, visible.height) - 1) / 2)
            )
            };
            const auto hit_handles { make_handles(hit_size) };

            const RECT screen { bounds.x - frame_origin_.x,
                                bounds.y - frame_origin_.y,
                                bounds.x + bounds.width - frame_origin_.x,
                                bounds.y + bounds.height - frame_origin_.y
            };

            // 화면 끝에서는 조절점을 경계 안으로 옮겨 전체 화면 영역도 다시 줄일 수 있게 한다.
            for (std::size_t index = 0; index < handles_.size(); ++index)
            {
                handle_rect& handle { handles_[index] };

                const LONG left { std::clamp(handle.rect.left, screen.left, screen.right - grip) };
                const LONG top { std::clamp(handle.rect.top, screen.top, screen.bottom - grip) };
                handle.rect = { left, top, left + grip, top + grip };

                const RECT hit { hit_handles[index].rect };

                // 모서리 여백을 안쪽으로 밀면 인접한 변이 대각선 조절로 오인된다.
                IntersectRect(&handle.hit_rect, &hit, &screen);

                // 변은 중앙 조절점뿐 아니라 모서리 사이의 전체 길이에서 잡을 수 있다.
                const int depth { MulDiv(24, scale_dpi, 96) };
                RECT band {};

                switch (handle.edge)
                {
                case resize_edge::left:
                    band = { inner_.left - depth, inner_.top, inner_.left, inner_.bottom };
                    break;

                case resize_edge::right:
                    band = { inner_.right, inner_.top, inner_.right + depth, inner_.bottom };
                    break;

                case resize_edge::top:
                    band = { inner_.left, inner_.top - depth, inner_.right, inner_.top };
                    break;

                case resize_edge::bottom:
                    band = { inner_.left, inner_.bottom, inner_.right, inner_.bottom + depth };
                    break;

                default:
                    continue;
                }

                IntersectRect(&handle.hit_rect, &band, &screen);
            }

            const RECT border { std::max(inner_.left - 2, screen.left),
                                std::max(inner_.top - 2, screen.top),
                                std::min(inner_.right + 2, screen.right),
                                std::min(inner_.bottom + 2, screen.bottom)
            };
            const HRGN shape { CreateRectRgn(border.left, border.top, border.right, border.bottom) };
            const auto hole = CreateRectRgn(
                std::max(inner_.left, border.left + 2),
                std::max(inner_.top, border.top + 2),
                std::min(inner_.right, border.right - 2),
                std::min(inner_.bottom, border.bottom - 2)
            );
            CombineRgn(shape, shape, hole, RGN_DIFF);
            DeleteObject(hole);

            const auto add_rounded_rect = [shape](const RECT& rect, const int radius) {
                const HRGN part {
                    CreateRoundRectRgn(rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2)
                };
                CombineRgn(shape, shape, part, RGN_OR);
                DeleteObject(part);
            };

            if (header_visible)
            {
                add_rounded_rect(header_, MulDiv(10, scale_dpi, 96));
            }

            if (!selecting_)
            {
                for (const handle_rect& handle : handles_)
                {
                    for (const RECT& bar : handle_bars(handle))
                    {
                        if (bar.right > bar.left && bar.bottom > bar.top)
                        {
                            add_rounded_rect(bar, std::max(1, grip / 4));
                        }
                    }
                }
            }

            // 좌표와 네이티브 모양을 모두 바꾼 뒤 한 번만 그려 중간 배치를 노출하지 않는다.
            frame_layout_size_ = { frame_right - frame_origin_.x, frame_bottom - frame_origin_.y };

            if (SetWindowRgn(frame_, shape, FALSE) == 0)
            {
                DeleteObject(shape);
                ShowWindow(frame_, SW_HIDE);
                publish(u8"투명 프레임을 만들 수 없습니다.");
                return;
            }

            update_frame_title();
            render_frame();
            SetWindowPos(frame_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
            ShowWindow(frame_, capture_active_ && !capture_excluded_ ? SW_HIDE : SW_SHOWNOACTIVATE);
            ValidateRect(frame_, nullptr);

            if (edit_button_ != nullptr && IsWindowVisible(edit_button_))
            {
                RedrawWindow(edit_button_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }

            update_glow();
            update_resize_input();

            if (GetDpiForWindow(frame_) != dpi)
            {
                PostMessageW(dispatcher_, refresh_frame_message, 0, 0);
            }
        }

        // 보이는 그립보다 넓은 입력 띠를 만들고 화면 끝에서는 캡쳐 안쪽의 클릭 통과를 유지한다.
        void update_resize_input()
        {
            if (selecting_ || capture_active_ || !IsWindowVisible(frame_))
            {
                ShowWindow(resize_input_, SW_HIDE);
                return;
            }

            RECT extent { handles_.front().hit_rect };

            const HRGN shape { CreateRectRgn(0, 0, 0, 0) };
            const auto add_rect = [shape](const RECT& rect) {
                const HRGN part { CreateRectRgn(rect.left, rect.top, rect.right, rect.bottom) };
                CombineRgn(shape, shape, part, RGN_OR);
                DeleteObject(part);
            };

            for (const handle_rect& handle : handles_)
            {
                UnionRect(&extent, &extent, &handle.hit_rect);
                add_rect(handle.hit_rect);
            }

            // 입력 여백은 안쪽을 덮지 않으며 화면 끝의 기존 조절점만 조작 가능하게 한다.
            const HRGN hole { CreateRectRgn(inner_.left, inner_.top, inner_.right, inner_.bottom) };
            CombineRgn(shape, shape, hole, RGN_DIFF);
            DeleteObject(hole);

            for (const handle_rect& handle : handles_)
            {
                UnionRect(&extent, &extent, &handle.rect);
                add_rect(handle.rect);
            }

            // 보이는 바와 이동 핸들은 원래 창이 받으며 입력 레이어가 덧칠하지 않는다.
            const HRGN visual { CreateRectRgn(0, 0, 0, 0) };

            if (GetWindowRgn(frame_, visual) != ERROR)
            {
                CombineRgn(shape, shape, visual, RGN_DIFF);
            }

            DeleteObject(visual);
            resize_input_origin_ = { frame_origin_.x + extent.left, frame_origin_.y + extent.top };
            OffsetRgn(shape, -extent.left, -extent.top);
            SetWindowPos(
                resize_input_,
                HWND_TOPMOST,
                resize_input_origin_.x,
                resize_input_origin_.y,
                extent.right - extent.left,
                extent.bottom - extent.top,
                SWP_NOACTIVATE | SWP_NOREDRAW
            );

            if (SetWindowRgn(resize_input_, shape, FALSE) == 0)
            {
                DeleteObject(shape);
                ShowWindow(resize_input_, SW_HIDE);
                return;
            }

            ShowWindow(resize_input_, SW_SHOWNOACTIVATE);
            RedrawWindow(resize_input_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }

        [[nodiscard]] resize_edge hit_edge(const screen_point screen) const noexcept
        {
            const screen_point point { screen.x - frame_origin_.x, screen.y - frame_origin_.y };

            if (contains(header_, point))
            {
                return resize_edge::move;
            }

            for (const handle_rect& handle : handles_)
            {
                if (contains(handle.hit_rect, point) || contains(handle.rect, point))
                {
                    return handle.edge;
                }
            }

            if (point.x < inner_.left)
            {
                return resize_edge::left;
            }

            if (point.x >= inner_.right)
            {
                return resize_edge::right;
            }

            if (point.y < inner_.top)
            {
                return resize_edge::top;
            }

            return resize_edge::bottom;
        }

        void set_frame_cursor()
        {
            const resize_edge edge { drag_edge_ == resize_edge::none ? hit_edge(pointer_position()) : drag_edge_ };
            LPCWSTR name { IDC_SIZEALL };

            if (edge == resize_edge::left || edge == resize_edge::right)
            {
                name = IDC_SIZEWE;
            }
            else if (edge == resize_edge::top || edge == resize_edge::bottom)
            {
                name = IDC_SIZENS;
            }
            else if (edge == resize_edge::top_left || edge == resize_edge::bottom_right)
            {
                name = IDC_SIZENWSE;
            }
            else if (edge == resize_edge::top_right || edge == resize_edge::bottom_left)
            {
                name = IDC_SIZENESW;
            }

            SetCursor(LoadCursorW(nullptr, name));
        }

        void cancel_drag()
        {
            if (drag_edge_ == resize_edge::none)
            {
                return;
            }

            drag_edge_ = resize_edge::none;
            ShowWindow(drag_outline_, SW_HIDE);
            region_ = drag_original_;

            if (GetCapture() == frame_)
            {
                ReleaseCapture();
            }

            update_frame();
            publish(region_text(*region_));
        }

        void frame_mouse_move(const screen_point point)
        {
            if (drag_edge_ == resize_edge::none)
            {
                return;
            }

            const capture_region adjusted { adjust_region(
                drag_original_, drag_edge_, { point.x - drag_origin_.x, point.y - drag_origin_.y }, virtual_screen()
            )
            };

            if (drag_preview_ == adjusted)
            {
                return;
            }

            drag_preview_ = adjusted;

            if (drag_edge_ == resize_edge::move)
            {
                update_frame();
            }
            else
            {
                update_drag_outline();
            }
        }

        void update_drag_outline()
        {
            if (drag_edge_ == resize_edge::none || drag_edge_ == resize_edge::move)
            {
                return;
            }

            const int thickness { std::max(3, MulDiv(4, static_cast<int>(GetDpiForWindow(frame_)), 96)) };
            const int width { drag_preview_.width + thickness * 2 };
            const int height { drag_preview_.height + thickness * 2 };
            const bool resized { drag_outline_size_.cx != width || drag_outline_size_.cy != height
                                 || drag_outline_thickness_ != thickness
            };
            drag_outline_thickness_ = thickness;
            drag_outline_bitmap_.resize(width, height);

            if (resized)
            {
                const HRGN shape { CreateRectRgn(0, 0, width, height) };
                const HRGN hole { CreateRectRgn(thickness, thickness, width - thickness, height - thickness) };

                if (shape == nullptr || hole == nullptr)
                {
                    if (shape != nullptr)
                    {
                        DeleteObject(shape);
                    }

                    if (hole != nullptr)
                    {
                        DeleteObject(hole);
                    }

                    throw std::runtime_error { "Cannot create drag outline shape." };
                }

                CombineRgn(shape, shape, hole, RGN_DIFF);
                DeleteObject(hole);

                if (!SetWindowRgn(drag_outline_, shape, FALSE))
                {
                    DeleteObject(shape);
                    throw std::runtime_error { "Cannot apply drag outline shape." };
                }

                drag_outline_size_ = { width, height };
            }

            const auto title { L"이동 미리보기 " + region_dimensions(drag_preview_) + L" · "
                               + std::to_wstring(drag_preview_.x) + L", " + std::to_wstring(drag_preview_.y)
            };
            SetWindowTextW(drag_outline_, title.c_str());

            if (resized)
            {
                SkCanvas canvas { drag_outline_bitmap_.image() };
                canvas.clear(SK_ColorTRANSPARENT);

                const RECT outer { 0, 0, width, height };
                canvas.save();
                canvas.clipRect(
                    SkRect::MakeLTRB(
                        static_cast<float>(thickness),
                        static_cast<float>(thickness),
                        static_cast<float>(width - thickness),
                        static_cast<float>(height - thickness)
                    ),
                    SkClipOp::kDifference
                );
                draw_rect(canvas, outer, frame_background_);

                const RECT stroke { 1, 1, width - 1, height - 1 };
                const int line { thickness - 2 };
                draw_rect(canvas, { stroke.left, stroke.top, stroke.right, stroke.top + line }, frame_hover_);
                draw_rect(canvas, { stroke.left, stroke.bottom - line, stroke.right, stroke.bottom }, frame_hover_);
                draw_rect(canvas, { stroke.left, stroke.top, stroke.left + line, stroke.bottom }, frame_hover_);
                draw_rect(canvas, { stroke.right - line, stroke.top, stroke.right, stroke.bottom }, frame_hover_);
                canvas.restore();
            }

            drag_outline_bitmap_.present(
                drag_outline_, { drag_preview_.x - thickness, drag_preview_.y - thickness }, 230
            );
            SetWindowPos(drag_outline_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
            ShowWindow(drag_outline_, SW_SHOWNOACTIVATE);
            ValidateRect(drag_outline_, nullptr);
        }

        void paint_drag_outline(const HDC print_device = nullptr)
        {
            PAINTSTRUCT paint_data {};

            const HDC device { print_device != nullptr ? print_device : BeginPaint(drag_outline_, &paint_data) };
            const int saved { SaveDC(device) };
            const HRGN shape { CreateRectRgn(0, 0, 0, 0) };

            if (GetWindowRgn(drag_outline_, shape) != ERROR)
            {
                ExtSelectClipRgn(device, shape, RGN_AND);
            }

            DeleteObject(shape);
            drag_outline_bitmap_.copy(device);

            if (saved != 0)
            {
                RestoreDC(device, saved);
            }

            if (print_device == nullptr)
            {
                EndPaint(drag_outline_, &paint_data);
            }
        }

        void frame_hover_at(const screen_point point, const HWND input_window)
        {
            if (drag_edge_ != resize_edge::none)
            {
                return;
            }

            resize_edge hovered { resize_edge::none };

            const bool header_hover { contains(header_, point) };

            if (!header_hover)
            {
                for (const handle_rect& handle : handles_)
                {
                    if (contains(handle.hit_rect, point) || contains(handle.rect, point))
                    {
                        hovered = handle.edge;
                        break;
                    }
                }
            }

            if (hovered != hovered_edge_ || header_hover != header_hover_)
            {
                hovered_edge_ = hovered;
                header_hover_ = header_hover;
                InvalidateRect(frame_, nullptr, FALSE);
            }

            TRACKMOUSEEVENT tracking { sizeof(TRACKMOUSEEVENT), TME_LEAVE, input_window, 0 };
            TrackMouseEvent(&tracking);
        }

        void request_edit()
        {
            if (selecting_ || capture_active_ || !region_.has_value() || !edit_request_)
            {
                return;
            }

            // 콜백은 소유 창을 바꿀 수 있으므로 캡쳐를 먼저 해제하고 마지막에 호출한다.
            cancel_drag();

            if (GetCapture() == edit_button_ || GetCapture() == frame_)
            {
                ReleaseCapture();
            }

            const auto action { edit_request_ };
            action();
        }

        void draw_edit_icon(
            SkCanvas& canvas,
            const RECT& rect,
            const int dpi,
            const bool pressed,
            const bool enabled,
            const bool focused
        )
        {
            const COLORREF background { pressed || edit_hover_ ? frame_button_hover_ : frame_background_ };
            draw_rounded_rect(canvas, rect, static_cast<float>(MulDiv(6, dpi, 96)), background, background);

            const COLORREF ink { !enabled ? frame_disabled_ : edit_hover_ || pressed ? frame_hover_ : frame_muted_ };
            const float center_x { static_cast<float>(rect.left + rect.right) / 2.0f };
            const float center_y { static_cast<float>(rect.top + rect.bottom) / 2.0f };
            const float outer { static_cast<float>(std::max(4, MulDiv(6, dpi, 96))) };
            const float diagonal { static_cast<float>(MulDiv(4, dpi, 96)) };
            SkPaint paint {};
            paint.setAntiAlias(true);
            paint.setColor(skia_color(ink));
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(static_cast<float>(std::max(1, MulDiv(2, dpi, 96))));
            paint.setStrokeCap(SkPaint::kRound_Cap);

            const std::array<SkPoint, 8> teeth {
                SkPoint { outer, 0 },  SkPoint { diagonal, diagonal },
                SkPoint { 0, outer },  SkPoint { -diagonal, diagonal },
                SkPoint { -outer, 0 }, SkPoint { -diagonal, -diagonal },
                SkPoint { 0, -outer }, SkPoint { diagonal, -diagonal },
            };

            for (const SkPoint tooth : teeth)
            {
                canvas.drawLine(
                    center_x + tooth.x() * 2.0f / 3.0f,
                    center_y + tooth.y() * 2.0f / 3.0f,
                    center_x + tooth.x(),
                    center_y + tooth.y(),
                    paint
                );
            }

            canvas.drawCircle(center_x, center_y, static_cast<float>(std::max(2, MulDiv(3, dpi, 96))), paint);

            if (focused)
            {
                paint.setStrokeWidth(1.0f);

                auto focus { skia_rect(rect) };
                focus.inset(2.0f, 2.0f);
                canvas.drawRoundRect(focus, 4.0f, 4.0f, paint);
            }
        }

        void draw_edit_button(const DRAWITEMSTRUCT& item)
        {
            overlay_bitmap bitmap {};
            bitmap.resize(item.rcItem.right, item.rcItem.bottom);

            SkCanvas canvas { bitmap.image() };
            canvas.clear(skia_color(frame_background_));
            draw_edit_icon(
                canvas,
                item.rcItem,
                static_cast<int>(GetDpiForWindow(edit_button_)),
                (item.itemState & ODS_SELECTED) != 0,
                (item.itemState & ODS_DISABLED) == 0,
                (item.itemState & ODS_FOCUS) != 0
            );

            if (drag_edge_ != resize_edge::none && drag_edge_ != resize_edge::move)
            {
                SkPaint opacity {};
                opacity.setAlpha(40);
                opacity.setBlendMode(SkBlendMode::kDstIn);
                canvas.drawPaint(opacity);
            }

            bitmap.copy(item.hDC);
        }

        void frame_key(const WPARAM key)
        {
            if (key == VK_ESCAPE)
            {
                cancel_drag();

                if (owner_ != nullptr)
                {
                    SetForegroundWindow(owner_);
                }

                return;
            }

            if (key == VK_RETURN)
            {
                begin_selection();
                return;
            }

            if (drag_edge_ != resize_edge::none || !region_.has_value())
            {
                return;
            }

            screen_point delta {};

            const int step { (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 10 : 1 };

            switch (key)
            {
            case VK_LEFT:
                delta.x = -step;
                break;

            case VK_RIGHT:
                delta.x = step;
                break;

            case VK_UP:
                delta.y = -step;
                break;

            case VK_DOWN:
                delta.y = step;
                break;

            default:
                return;
            }

            const resize_edge edge { (GetKeyState(VK_CONTROL) & 0x8000) != 0 ? resize_edge::bottom_right
                                                                             : resize_edge::move
            };
            region_ = adjust_region(*region_, edge, delta, virtual_screen());
            update_frame();
            publish(region_text(*region_));
        }

        // 선택 중 화살표는 실제 포인터를 1 px(Shift는 10 px) 옮겨 경계 픽셀을 맞추게 한다.
        [[nodiscard]] bool nudge_pointer(const WPARAM key)
        {
            const int step { (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 10 : 1 };
            screen_point delta {};

            switch (key)
            {
            case VK_LEFT:
                delta.x = -step;
                break;

            case VK_RIGHT:
                delta.x = step;
                break;

            case VK_UP:
                delta.y = -step;
                break;

            case VK_DOWN:
                delta.y = step;
                break;

            default:
                return false;
            }

            POINT cursor {};

            if (GetCursorPos(&cursor))
            {
                SetCursorPos(cursor.x + delta.x, cursor.y + delta.y);
            }

            return true;
        }

        void context_menu()
        {
            const HMENU menu { CreatePopupMenu() };
            std::wstring reselect { L"영역 다시 선택" };

            if (!selection_shortcut_.empty())
            {
                reselect += L"\t" + wide_text(selection_shortcut_);
            }

            AppendMenuW(menu, MF_STRING, 1, reselect.c_str());
            AppendMenuW(menu, MF_STRING, 2, L"영역 해제");

            const screen_point pointer { pointer_position() };
            const UINT command { static_cast<UINT>(
                TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pointer.x, pointer.y, 0, frame_, nullptr)
            )
            };
            DestroyMenu(menu);

            if (command == 1)
            {
                begin_selection();
            }
            else if (command == 2)
            {
                region_.reset();
                ShowWindow(frame_, SW_HIDE);
                publish(u8"캡쳐 영역을 해제했습니다.");
            }
        }

        // 프레임 장식과 이동 핸들만 알파 비트맵에 그려 캡쳐 내부의 화면은 그대로 보이게 한다.
        void render_frame()
        {
            frame_bitmap_.resize(frame_layout_size_.cx, frame_layout_size_.cy);

            SkCanvas canvas { frame_bitmap_.image() };
            canvas.clear(SK_ColorTRANSPARENT);

            const int dpi { static_cast<int>(GetDpiForWindow(frame_)) };
            const COLORREF ring { capture_active_ ? frame_recording
                                      : header_hover_ || hovered_edge_ != resize_edge::none
                                          || drag_edge_ != resize_edge::none
                                      ? frame_hover_
                                      : frame_accent_
            };
            draw_rect(canvas, { inner_.left - 2, inner_.top - 2, inner_.right + 2, inner_.top }, ring);
            draw_rect(canvas, { inner_.left - 2, inner_.bottom, inner_.right + 2, inner_.bottom + 2 }, ring);
            draw_rect(canvas, { inner_.left - 2, inner_.top, inner_.left, inner_.bottom }, ring);
            draw_rect(canvas, { inner_.right, inner_.top, inner_.right + 2, inner_.bottom }, ring);

            const LONG corner { std::min<LONG>(
                MulDiv(14, dpi, 96), std::min(inner_.right - inner_.left, inner_.bottom - inner_.top) / 2
            )
            };

            const COLORREF corner_color { capture_active_ ? RGB(255, 191, 200) : frame_corner_ };

            for (const LONG x : { inner_.left, inner_.right })
            {
                for (const LONG y : { inner_.top, inner_.bottom })
                {
                    const LONG horizontal { x == inner_.left ? x : x - corner };
                    const LONG vertical { y == inner_.top ? y : y - corner };
                    draw_rect(
                        canvas,
                        { horizontal, y == inner_.top ? y - 2 : y, horizontal + corner, y == inner_.top ? y : y + 2 },
                        corner_color
                    );
                    draw_rect(
                        canvas,
                        { x == inner_.left ? x - 2 : x, vertical, x == inner_.left ? x : x + 2, vertical + corner },
                        corner_color
                    );
                }
            }

            const capture_region visible { visible_region() };

            if (header_.right > header_.left)
            {
                const bool hot { header_hover_ || drag_edge_ == resize_edge::move };
                draw_rounded_rect(
                    canvas,
                    header_,
                    static_cast<float>(MulDiv(10, dpi, 96)),
                    hot ? frame_button_hover_ : frame_background_,
                    hot ? frame_hover_ : frame_border_
                );

                const int dot { std::max(1, MulDiv(2, dpi, 96)) };
                const int grip_x { header_.left + MulDiv(12, dpi, 96) };
                const int grip_y { (header_.top + header_.bottom) / 2 };

                if (capture_active_)
                {
                    draw_rounded_rect(
                        canvas,
                        { grip_x - dot, grip_y - dot, grip_x + dot * 2, grip_y + dot * 2 },
                        static_cast<float>(dot * 2),
                        frame_recording,
                        frame_recording
                    );
                }
                else
                {
                    for (const int column : { 0, 1 })
                    {
                        for (const int row : { -1, 0, 1 })
                        {
                            draw_rect(
                                canvas,
                                { grip_x + column * dot * 3,
                                  grip_y + row * dot * 3,
                                  grip_x + column * dot * 3 + dot,
                                  grip_y + row * dot * 3 + dot
                                },
                                frame_muted_
                            );
                        }
                    }
                }

                RECT label { header_ };
                label.left += MulDiv(32, dpi, 96);
                label.right -= MulDiv(edit_button_ != nullptr && !selecting_ ? 32 : 12, dpi, 96);

                std::wstring text { region_dimensions(visible)
                                    + (recording_elapsed_ ? L" · " + elapsed_text() : std::wstring {})
                };

                if (drag_edge_ != resize_edge::none && drag_edge_ != resize_edge::move)
                {
                    text = L"놓으면 적용 · Esc 취소";
                }

                draw_label(canvas, text, label, 12.0f * static_cast<float>(dpi) / 96.0f, frame_foreground_);

                if (edit_button_ != nullptr && !selecting_)
                {
                    const int size { MulDiv(24, dpi, 96) };
                    const int padding { MulDiv(3, dpi, 96) };
                    const RECT button { header_.right - padding - size,
                                        header_.top + padding,
                                        header_.right - padding,
                                        header_.top + padding + size
                    };
                    draw_edit_icon(
                        canvas,
                        button,
                        dpi,
                        (SendMessageW(edit_button_, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0,
                        IsWindowEnabled(edit_button_) != FALSE,
                        GetFocus() == edit_button_
                    );
                }
            }

            if (!selecting_)
            {
                for (const handle_rect& handle : handles_)
                {
                    const bool hot { hovered_edge_ == handle.edge || drag_edge_ == handle.edge };

                    for (const RECT& bar : handle_bars(handle))
                    {
                        if (bar.right > bar.left && bar.bottom > bar.top)
                        {
                            draw_rounded_rect(
                                canvas,
                                bar,
                                static_cast<float>(std::max<LONG>(1, (bar.right - bar.left) / 4)),
                                hot ? corner_color : ring,
                                hot ? corner_color : ring
                            );
                        }
                    }
                }
            }

            if (drag_edge_ != resize_edge::none && drag_edge_ != resize_edge::move)
            {
                SkPaint opacity {};
                opacity.setAlpha(40);
                opacity.setBlendMode(SkBlendMode::kDstIn);
                canvas.drawPaint(opacity);
            }

            frame_bitmap_.present(frame_, { frame_origin_.x, frame_origin_.y });
        }

        void paint(const HWND window, const HDC print_device = nullptr)
        {
            PAINTSTRUCT paint_data {};

            const HDC device { print_device != nullptr ? print_device : BeginPaint(window, &paint_data) };
            const int saved { SaveDC(device) };

            if (window == frame_)
            {
                render_frame();

                const HRGN shape { CreateRectRgn(0, 0, 0, 0) };

                if (GetWindowRgn(window, shape) != ERROR)
                {
                    ExtSelectClipRgn(device, shape, RGN_AND);
                }

                DeleteObject(shape);

                if (print_device != nullptr)
                {
                    frame_bitmap_.copy(device);
                }
            }
            else
            {
                RECT client {};
                GetClientRect(window, &client);
                overlay_bitmap bitmap {};
                bitmap.resize(client.right, client.bottom);

                SkCanvas canvas { bitmap.image() };
                canvas.clear(skia_color(GetSysColor(COLOR_WINDOW)));
                draw_rounded_rect(canvas, client, 8.0f, GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_HIGHLIGHT));

                const int dpi { static_cast<int>(GetDpiForWindow(window)) };
                const int padding { MulDiv(12, dpi, 96) };
                RECT text { padding, padding, client.right - padding, padding + MulDiv(24, dpi, 96) };
                draw_label(
                    canvas,
                    L"드래그로 영역 선택 · 클릭하면 창 선택",
                    text,
                    12.0f * static_cast<float>(dpi) / 96.0f,
                    GetSysColor(COLOR_WINDOWTEXT)
                );
                text.top += MulDiv(26, dpi, 96);
                text.bottom += MulDiv(26, dpi, 96);

                const std::wstring detail { preview_.valid()
                                                ? region_dimensions(preview_) + L" · Esc / 오른쪽 클릭: 취소"
                                                : L"Esc / 오른쪽 클릭: 취소 · 최소 16 x 16 px"
                };
                draw_label(
                    canvas, detail, text, 12.0f * static_cast<float>(dpi) / 96.0f, GetSysColor(COLOR_WINDOWTEXT)
                );
                bitmap.copy(device);
            }

            if (saved != 0)
            {
                RestoreDC(device, saved);
            }

            if (print_device == nullptr)
            {
                EndPaint(window, &paint_data);
            }

            if (window == frame_ && print_device == nullptr)
            {
                update_glow();
            }
        }

        [[nodiscard]] LRESULT dispatch(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam)
        {
            if (window == drag_outline_)
            {
                if (message == WM_NCHITTEST)
                {
                    return HTTRANSPARENT;
                }

                if (message == WM_MOUSEACTIVATE)
                {
                    return MA_NOACTIVATE;
                }

                if (message == WM_PAINT || message == WM_PRINTCLIENT)
                {
                    paint_drag_outline(message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wparam) : nullptr);
                    return 0;
                }
            }

            if (window == selection_input_)
            {
                if (message == WM_PAINT || message == WM_PRINTCLIENT)
                {
                    PAINTSTRUCT paint_data {};

                    const HDC device { message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wparam)
                                                                 : BeginPaint(window, &paint_data)
                    };
                    RECT client {};
                    GetClientRect(window, &client);

                    // 검은 입력 면은 화면을 어둡게 하므로 흰색으로 최소 알파만 유지한다.
                    fill_rect(device, client, RGB(255, 255, 255));

                    if (message == WM_PAINT)
                    {
                        EndPaint(window, &paint_data);
                    }

                    return 0;
                }

                if (message == WM_NCHITTEST)
                {
                    return HTCLIENT;
                }

                if (message == WM_CAPTURECHANGED && selecting_ && selection_dragging_
                    && reinterpret_cast<HWND>(lparam) != selection_input_)
                {
                    finish_selection(false);
                    return 0;
                }
            }

            if (selecting_ && (window == selection_input_ || window == guide_ || window == frame_))
            {
                if (message == WM_SETCURSOR)
                {
                    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                    return TRUE;
                }

                if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
                {
                    if (wparam == VK_ESCAPE)
                    {
                        finish_selection(false);
                        return 0;
                    }

                    if (message == WM_KEYDOWN && nudge_pointer(wparam))
                    {
                        return 0;
                    }
                }

                if (message == WM_CANCELMODE || message == WM_CLOSE)
                {
                    finish_selection(false);
                    return 0;
                }

                if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE
                    || message == WM_RBUTTONUP)
                {
                    POINT point { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };

                    if (ClientToScreen(window, &point))
                    {
                        selection_mouse(message, { point.x, point.y });
                    }

                    return 0;
                }
            }

            if (window == resize_input_)
            {
                if (message == WM_MOUSEACTIVATE)
                {
                    return MA_NOACTIVATE;
                }

                if (message == WM_PAINT)
                {
                    PAINTSTRUCT paint_data {};

                    const HDC device { BeginPaint(window, &paint_data) };
                    fill_rect(device, paint_data.rcPaint, frame_accent_);
                    EndPaint(window, &paint_data);
                    return 0;
                }

                if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP
                    || message == WM_RBUTTONUP)
                {
                    const screen_point screen { resize_input_origin_.x + GET_X_LPARAM(lparam),
                                                resize_input_origin_.y + GET_Y_LPARAM(lparam)
                    };

                    const screen_point point { screen.x - frame_origin_.x, screen.y - frame_origin_.y };

                    if (message == WM_MOUSEMOVE)
                    {
                        if (!selecting_ && !capture_active_)
                        {
                            frame_hover_at(point, window);
                            frame_mouse_move(screen);
                        }

                        return 0;
                    }

                    if ((message == WM_LBUTTONDOWN || message == WM_RBUTTONUP) && !selecting_ && !capture_active_)
                    {
                        SetForegroundWindow(frame_);
                        SetFocus(frame_);
                    }

                    return dispatch(frame_, message, wparam, MAKELPARAM(point.x, point.y));
                }

                if (message == WM_SETCURSOR || message == WM_MOUSELEAVE)
                {
                    return dispatch(frame_, message, wparam, lparam);
                }
            }

            switch (message)
            {
            case WM_SHOWWINDOW:
                if (window == frame_ && wparam == FALSE)
                {
                    if (resize_input_ != nullptr)
                    {
                        ShowWindow(resize_input_, SW_HIDE);
                    }

                    if (glow_ != nullptr)
                    {
                        ShowWindow(glow_, SW_HIDE);
                    }

                    if (drag_outline_ != nullptr)
                    {
                        ShowWindow(drag_outline_, SW_HIDE);
                    }
                }

                break;

            case WM_COMMAND:
                if (window == frame_ && LOWORD(wparam) == edit_control && HIWORD(wparam) == BN_CLICKED)
                {
                    request_edit();
                    return 0;
                }

                break;

            case WM_ERASEBKGND:
                return 1;

            case WM_PRINTCLIENT:
                if (window != dispatcher_
                    && (window != frame_ || (selecting_ ? preview_.valid() : region_.has_value())))
                {
                    paint(window, reinterpret_cast<HDC>(wparam));
                }

                return 0;

            case WM_PAINT:
                if (window != dispatcher_)
                {
                    if (window == frame_ && !(selecting_ ? preview_.valid() : region_.has_value()))
                    {
                        ValidateRect(window, nullptr);
                        return 0;
                    }

                    paint(window);
                    return 0;
                }

                break;

            case WM_HOTKEY:
                begin_selection();
                return 0;

            case refresh_frame_message:
                if (selecting_ ? preview_.valid() : region_.has_value())
                {
                    update_frame();
                }

                return 0;

            case WM_TIMER:
                if (selecting_)
                {
                    const HWND foreground { GetForegroundWindow() };
                    DWORD process {};
                    GetWindowThreadProcessId(foreground, &process);

                    if (process == GetCurrentProcessId())
                    {
                        selection_foreground_acquired_ = true;
                    }

                    if ((foreground != nullptr && process != GetCurrentProcessId()
                         && (selection_foreground_acquired_ || foreground != foreground_at_selection_))
                        || std::chrono::steady_clock::now() - last_selection_input_ > std::chrono::minutes { 5 })
                    {
                        finish_selection(false);
                    }
                    else if (magnifier_point_)
                    {
                        // 포인터가 멈춰 있어도 아래 화면이 바뀌면 확대 표본을 따라 갱신한다.
                        update_magnifier(*magnifier_point_);
                    }
                }

                return 0;

            case WM_DISPLAYCHANGE:
                if (selecting_)
                {
                    finish_selection(false);
                }

                if (region_.has_value())
                {
                    cancel_drag();
                    region_ = fit_region_to_screen(*region_, virtual_screen());
                    update_frame();
                    publish(region_text(*region_));
                }

                return 0;

            case WM_DPICHANGED:
                if (window == frame_ && (selecting_ ? preview_.valid() : region_.has_value()))
                {
                    update_frame();
                }

                return 0;

            case WM_WINDOWPOSCHANGED:
                if ((window == frame_ || window == resize_input_ || window == drag_outline_)
                    && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
                {
                    // 소유 창의 항상 위 옵션을 꺼도 프레임은 화면 위에 별도로 유지한다.
                    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }

                break;

            case WM_SETCURSOR:
                if (window == frame_ && !selecting_)
                {
                    if (capture_active_)
                    {
                        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                    }
                    else
                    {
                        set_frame_cursor();
                    }

                    return TRUE;
                }

                break;

            case WM_LBUTTONDOWN:
                if (window == frame_ && !selecting_ && !capture_active_ && region_.has_value())
                {
                    drag_origin_ = { frame_origin_.x + GET_X_LPARAM(lparam), frame_origin_.y + GET_Y_LPARAM(lparam) };
                    drag_edge_ = (GetKeyState(VK_MENU) & 0x8000) != 0 ? resize_edge::move : hit_edge(drag_origin_);
                    drag_original_ = *region_;
                    drag_preview_ = drag_original_;
                    SetCapture(frame_);

                    // 클릭 통과 창은 아래 형제 창의 그리기를 기다리므로 안내부터 완료한다.
                    RedrawWindow(frame_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                    update_drag_outline();
                    publish(u8"이동·크기 변경 중 · 마우스를 놓으면 적용 · Esc: 취소");
                    return 0;
                }

                break;

            case WM_MOUSEMOVE:
                if (window == frame_ && !selecting_ && !capture_active_)
                {
                    frame_hover_at({ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }, window);
                    frame_mouse_move(
                        { frame_origin_.x + GET_X_LPARAM(lparam), frame_origin_.y + GET_Y_LPARAM(lparam) }
                    );
                    return 0;
                }

                break;

            case WM_MOUSELEAVE:
                if (window == frame_)
                {
                    hovered_edge_ = resize_edge::none;
                    header_hover_ = false;
                    InvalidateRect(frame_, nullptr, FALSE);
                    return 0;
                }

                break;

            case WM_LBUTTONUP:
                if (window == frame_ && drag_edge_ != resize_edge::none)
                {
                    frame_mouse_move(
                        { frame_origin_.x + GET_X_LPARAM(lparam), frame_origin_.y + GET_Y_LPARAM(lparam) }
                    );
                    region_ = drag_preview_;
                    drag_edge_ = resize_edge::none;
                    ShowWindow(drag_outline_, SW_HIDE);
                    ReleaseCapture();
                    update_frame();
                    publish(region_text(*region_));
                    return 0;
                }

                break;

            case WM_CAPTURECHANGED:

            case WM_CANCELMODE:
                cancel_drag();
                return 0;

            case WM_ACTIVATE:
                if (window == frame_ && LOWORD(wparam) == WA_INACTIVE)
                {
                    cancel_drag();
                }

                break;

            case WM_RBUTTONUP:
                if (window == frame_ && !selecting_ && !capture_active_)
                {
                    context_menu();
                    return 0;
                }

                break;

            case WM_KEYDOWN:
                if (window == frame_ && !capture_active_)
                {
                    frame_key(wparam);
                    return 0;
                }

                break;

            case WM_CLOSE:
                if (capture_active_)
                {
                    return 0;
                }

                if (window == guide_)
                {
                    finish_selection(false);
                }
                else if (window == frame_)
                {
                    cancel_drag();
                    region_.reset();
                    ShowWindow(frame_, SW_HIDE);
                    publish(u8"캡쳐 영역을 해제했습니다.");
                }

                return 0;
            }

            return DefWindowProcW(window, message, wparam, lparam);
        }

        static LRESULT CALLBACK
        window_proc(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) noexcept
        {
            auto* self { reinterpret_cast<impl*>(GetWindowLongPtrW(window, GWLP_USERDATA)) };

            if (message == WM_NCCREATE)
            {
                const auto* creation { reinterpret_cast<const CREATESTRUCTW*>(lparam) };
                self = static_cast<impl*>(creation->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }

            if (self != nullptr && !self->shutting_down_)
            {
                try
                {
                    return self->dispatch(window, message, wparam, lparam);
                }
                catch (...)
                {
                    // Win32 콜백 경계를 예외가 넘지 않게 하고 선택 입력을 즉시 해제한다.
                    self->selecting_ = false;
                    self->selection_dragging_ = false;
                    self->release_selection_input();

                    if (self->dispatcher_ != nullptr)
                    {
                        KillTimer(self->dispatcher_, 1);
                    }

                    if (self->guide_ != nullptr)
                    {
                        ShowWindow(self->guide_, SW_HIDE);
                    }

                    if (self->magnifier_ != nullptr)
                    {
                        ShowWindow(self->magnifier_, SW_HIDE);
                    }

                    if (self->frame_ != nullptr)
                    {
                        ShowWindow(self->frame_, SW_HIDE);
                    }

                    if (self->glow_ != nullptr)
                    {
                        ShowWindow(self->glow_, SW_HIDE);
                    }

                    if (self->drag_outline_ != nullptr)
                    {
                        ShowWindow(self->drag_outline_, SW_HIDE);
                    }
                }
            }

            return DefWindowProcW(window, message, wparam, lparam);
        }

        static LRESULT CALLBACK owner_subclass(
            const HWND window,
            const UINT message,
            const WPARAM wparam,
            const LPARAM lparam,
            UINT_PTR,
            const DWORD_PTR reference
        ) noexcept
        {
            auto* self { reinterpret_cast<impl*>(reference) };

            if (message == WM_CLOSE || message == WM_DESTROY || (message == WM_ENDSESSION && wparam != 0))
            {
                self->shutdown();
            }

            const auto result { DefSubclassProc(window, message, wparam, lparam) };

            if (!self->shutting_down_
                && (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED || message == WM_SYSCOLORCHANGE))
            {
                self->refresh_appearance();
            }

            return result;
        }

        static LRESULT CALLBACK edit_subclass(
            const HWND window,
            const UINT message,
            const WPARAM wparam,
            const LPARAM lparam,
            UINT_PTR,
            const DWORD_PTR reference
        ) noexcept
        {
            auto* self { reinterpret_cast<impl*>(reference) };

            if (message == WM_NCDESTROY)
            {
                if (self->edit_provider_ != nullptr)
                {
                    self->edit_provider_->invalidate();
                }

                static_cast<void>(UiaReturnRawElementProvider(window, 0, 0, nullptr));
                RemoveWindowSubclass(window, edit_subclass, 1);
            }

            if (!self->shutting_down_)
            {
                if (message == WM_GETOBJECT && static_cast<LONG>(lparam) == static_cast<LONG>(UiaRootObjectId)
                    && self->edit_provider_ != nullptr)
                {
                    return UiaReturnRawElementProvider(window, wparam, lparam, self->edit_provider_);
                }

                if (message == WM_ERASEBKGND)
                {
                    return 1;
                }

                if (message == WM_PAINT || message == WM_PRINTCLIENT)
                {
                    PAINTSTRUCT paint_data {};

                    const HDC device { message == WM_PAINT ? BeginPaint(window, &paint_data)
                                                           : reinterpret_cast<HDC>(wparam)
                    };
                    DRAWITEMSTRUCT item {};
                    item.CtlType = ODT_BUTTON;
                    item.CtlID = edit_control;
                    item.itemAction = ODA_DRAWENTIRE;
                    item.hwndItem = window;
                    item.hDC = device;
                    GetClientRect(window, &item.rcItem);

                    const LRESULT state { SendMessageW(window, BM_GETSTATE, 0, 0) };

                    if ((state & BST_PUSHED) != 0)
                    {
                        item.itemState |= ODS_SELECTED;
                    }

                    if (!IsWindowEnabled(window))
                    {
                        item.itemState |= ODS_DISABLED;
                    }

                    if (GetFocus() == window)
                    {
                        item.itemState |= ODS_FOCUS;
                    }

                    if (device != nullptr)
                    {
                        self->draw_edit_button(item);
                    }

                    if (message == WM_PAINT)
                    {
                        EndPaint(window, &paint_data);
                    }

                    return 0;
                }

                if (message == WM_MOUSEMOVE && !self->edit_hover_)
                {
                    self->edit_hover_ = true;
                    InvalidateRect(window, nullptr, FALSE);
                    InvalidateRect(self->frame_, nullptr, FALSE);
                    TRACKMOUSEEVENT tracking { sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0 };
                    TrackMouseEvent(&tracking);
                }
                else if (message == WM_MOUSELEAVE)
                {
                    self->edit_hover_ = false;
                    InvalidateRect(window, nullptr, FALSE);
                    InvalidateRect(self->frame_, nullptr, FALSE);
                }
                else if (message == WM_SETCURSOR && IsWindowEnabled(window))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                else if (
                    message == WM_KEYDOWN
                    && (wparam == VK_ESCAPE || wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_UP
                        || wparam == VK_DOWN)
                )
                {
                    return SendMessageW(self->frame_, message, wparam, lparam);
                }
            }

            const LRESULT result { DefSubclassProc(window, message, wparam, lparam) };

            if (!self->shutting_down_
                && (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_SETFOCUS
                    || message == WM_KILLFOCUS || message == BM_SETSTATE))
            {
                InvalidateRect(self->frame_, nullptr, FALSE);
            }

            return result;
        }

        luil::app_host* host_ {};
        HWND owner_ {};
        HWND dispatcher_ {};
        HWND frame_ {};
        SIZE frame_layout_size_ {};
        overlay_bitmap frame_bitmap_ {};
        HWND glow_ {};
        HWND drag_outline_ {};
        overlay_bitmap drag_outline_bitmap_ {};
        SIZE drag_outline_size_ {};
        int drag_outline_thickness_ {};
        HWND resize_input_ {};
        screen_point resize_input_origin_ {};
        SIZE glow_size_ {};
        COLORREF glow_color_ {};
        float glow_scale_ {};
        int glow_width_ { 24 };
        HWND selection_input_ {};
        HWND guide_ {};
        HWND magnifier_ {};
        overlay_bitmap magnifier_bitmap_ {};
        overlay_bitmap magnifier_sample_ {};
        bool magnifier_excluded_ {};
        HWND edit_button_ {};
        edit_button_provider* edit_provider_ {};

        std::function<void()> edit_request_ {};
        luil::appearance_settings appearance_ { .accent_id = u8"mint" };
        COLORREF frame_background_ { RGB(20, 26, 38) };
        COLORREF frame_foreground_ { RGB(231, 237, 248) };
        COLORREF frame_accent_ { RGB(76, 184, 150) };
        COLORREF frame_hover_ { RGB(133, 233, 199) };
        COLORREF frame_corner_ { RGB(176, 246, 222) };
        COLORREF frame_border_ { RGB(53, 66, 85) };
        COLORREF frame_muted_ { RGB(168, 185, 209) };
        COLORREF frame_disabled_ { RGB(86, 97, 115) };
        COLORREF frame_button_hover_ { RGB(35, 48, 68) };
        bool hotkey_registered_ {};
        bool manage_selection_hotkey_ {};
        bool rebuilding_frame_ {};
        bool shutting_down_ {};
        bool capture_active_ {};
        bool capture_excluded_ {};

        std::optional<std::uint64_t> recording_elapsed_ {};
        bool selecting_ {};
        bool selection_dragging_ {};
        bool selection_moved_ {};
        bool selection_foreground_acquired_ {};
        HWND foreground_at_selection_ {};

        std::chrono::steady_clock::time_point last_selection_input_ {};
        std::optional<capture_region> region_ {};
        capture_region preview_ {};
        capture_region snap_region_ {};
        std::optional<screen_point> hover_point_ {};
        std::optional<screen_point> magnifier_point_ {};
        std::u8string selection_shortcut_ { u8"Ctrl+Shift+A" };
        screen_point selection_origin_ {};
        screen_point frame_origin_ {};
        RECT inner_ {};
        RECT header_ {};
        resize_edge hovered_edge_ { resize_edge::none };
        bool header_hover_ {};
        bool edit_hover_ {};

        std::array<handle_rect, 8> handles_ {};
        resize_edge drag_edge_ { resize_edge::none };
        screen_point drag_origin_ {};

        capture_region drag_original_ {};
        capture_region drag_preview_ {};
    };

    region_controller::region_controller(
        luil::app_host& host, std::function<void()> edit_request, const bool manage_selection_hotkey
    )
        : impl_ { std::make_unique<impl>(host, std::move(edit_request), manage_selection_hotkey) }
    {}

    region_controller::~region_controller() = default;

    void region_controller::begin_selection()
    {
        impl_->begin_selection();
    }

    std::optional<capture_region> region_controller::current_region() const
    {
        return impl_->current_region();
    }

    void region_controller::cancel_selection()
    {
        impl_->cancel_selection();
    }

    void region_controller::set_region(const capture_region region)
    {
        impl_->set_region(region);
    }

    void region_controller::set_appearance(const luil::appearance_settings& appearance)
    {
        impl_->set_appearance(appearance);
    }

    void region_controller::set_glow_width(const int width)
    {
        impl_->set_glow_width(width);
    }

    void region_controller::set_selection_shortcut_text(std::u8string text)
    {
        impl_->set_selection_shortcut_text(std::move(text));
    }

    bool region_controller::set_capture_active(const bool active)
    {
        return impl_->set_capture_active(active);
    }

    void region_controller::set_recording_elapsed(const std::uint64_t seconds)
    {
        impl_->set_recording_elapsed(seconds);
    }

} // namespace chalkag
