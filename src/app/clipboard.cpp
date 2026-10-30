#include "app/clipboard.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"

#include <shlobj.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

namespace chalkag {
    namespace {
        // 전역 메모리를 클립보드에 넘기기 전까지만 소유한다.
        class global_memory final
        {
        public:
            explicit global_memory(const std::size_t size)
                : handle_ { GlobalAlloc(GMEM_MOVEABLE, size) }
            {}

            ~global_memory()
            {
                if (handle_ != nullptr)
                {
                    GlobalFree(handle_);
                }
            }

            global_memory(const global_memory&) = delete;
            global_memory& operator=(const global_memory&) = delete;

            [[nodiscard]] bool valid() const noexcept
            {
                return handle_ != nullptr;
            }

            [[nodiscard]] bool write(const void* data, const std::size_t size, const std::size_t offset = 0) const
            {
                void* target { GlobalLock(handle_) };

                if (target == nullptr)
                {
                    return false;
                }

                std::memcpy(static_cast<std::byte*>(target) + offset, data, size);
                GlobalUnlock(handle_);
                return true;
            }

            // 클립보드가 소유권을 가져가면 여기서는 더 해제하지 않는다.
            [[nodiscard]] bool hand_over(const UINT format)
            {
                if (SetClipboardData(format, handle_) == nullptr)
                {
                    return false;
                }

                handle_ = nullptr;
                return true;
            }

        private:
            HGLOBAL handle_ {};
        };

        class clipboard_scope final
        {
        public:
            explicit clipboard_scope(const HWND owner)
            {
                // 다른 앱이 잠깐 잡고 있을 수 있어 몇 번 다시 연다.
                for (int attempt {}; attempt < 5 && !open_; ++attempt)
                {
                    open_ = OpenClipboard(owner) != FALSE;

                    if (!open_)
                    {
                        Sleep(20);
                    }
                }
            }

            ~clipboard_scope()
            {
                if (open_)
                {
                    CloseClipboard();
                }
            }

            clipboard_scope(const clipboard_scope&) = delete;
            clipboard_scope& operator=(const clipboard_scope&) = delete;

            [[nodiscard]] bool open() const noexcept
            {
                return open_;
            }

        private:
            bool open_ {};
        };

        [[nodiscard]] std::vector<char> read_file(const std::filesystem::path& path)
        {
            std::ifstream file { path, std::ios::binary };
            return { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
        }

        // 첫 프레임을 위에서 아래로 쌓은 32비트 BGRA로 푼다. 알파는 미리 곱하지 않은 값이다.
        [[nodiscard]] std::optional<SkBitmap> decode_first_frame(const std::vector<char>& bytes)
        {
            auto codec { SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size())) };

            if (codec == nullptr)
            {
                return std::nullopt;
            }

            const auto info {
                codec->getInfo().makeColorType(kBGRA_8888_SkColorType).makeAlphaType(kUnpremul_SkAlphaType)
            };
            SkBitmap bitmap {};

            if (!bitmap.tryAllocPixels(info) || codec->getPixels(bitmap.pixmap()) != SkCodec::kSuccess)
            {
                return std::nullopt;
            }

            return bitmap;
        }

        [[nodiscard]] bool put_image(const SkBitmap& bitmap)
        {
            const auto width { bitmap.width() };
            const auto height { bitmap.height() };
            const auto pixel_bytes { static_cast<std::size_t>(width) * height * 4 };
            BITMAPV5HEADER header {};
            header.bV5Size = sizeof(header);
            header.bV5Width = width;
            header.bV5Height = -height;
            header.bV5Planes = 1;
            header.bV5BitCount = 32;
            header.bV5Compression = BI_BITFIELDS;
            header.bV5SizeImage = static_cast<DWORD>(pixel_bytes);
            header.bV5RedMask = 0x00ff0000;
            header.bV5GreenMask = 0x0000ff00;
            header.bV5BlueMask = 0x000000ff;
            header.bV5AlphaMask = 0xff000000;
            header.bV5CSType = LCS_sRGB;
            header.bV5Intent = LCS_GM_IMAGES;

            global_memory memory { sizeof(header) + pixel_bytes };

            if (!memory.valid() || !memory.write(&header, sizeof(header)))
            {
                return false;
            }

            // 비트맵의 줄 간격과 상관없이 여백 없는 줄로 옮긴다.
            std::vector<std::byte> rows(pixel_bytes);

            for (int row {}; row < height; ++row)
            {
                std::memcpy(
                    rows.data() + static_cast<std::size_t>(row) * width * 4,
                    bitmap.getAddr32(0, row),
                    static_cast<std::size_t>(width) * 4
                );
            }

            return memory.write(rows.data(), rows.size(), sizeof(header)) && memory.hand_over(CF_DIBV5);
        }

        [[nodiscard]] bool put_png(const std::vector<char>& bytes)
        {
            const UINT format { RegisterClipboardFormatW(L"PNG") };
            global_memory memory { bytes.size() };
            return format != 0 && memory.valid() && memory.write(bytes.data(), bytes.size())
                && memory.hand_over(format);
        }

        [[nodiscard]] bool put_file(const std::filesystem::path& path)
        {
            const std::wstring text { path.wstring() };
            const auto text_bytes { (text.size() + 2) * sizeof(wchar_t) };
            DROPFILES header {};
            header.pFiles = sizeof(header);
            header.fWide = TRUE;

            global_memory memory { sizeof(header) + text_bytes };

            if (!memory.valid() || !memory.write(&header, sizeof(header)))
            {
                return false;
            }

            // 파일 목록은 NUL로 끝나는 경로들 뒤에 NUL을 하나 더 둔다.
            std::vector<wchar_t> list(text.begin(), text.end());
            list.push_back(L'\0');
            list.push_back(L'\0');
            return memory.write(list.data(), text_bytes, sizeof(header)) && memory.hand_over(CF_HDROP);
        }
    } // namespace

    std::u8string copy_file_to_clipboard(const HWND owner, const std::filesystem::path& path, const bool include_image)
    {
        std::error_code error {};

        if (!std::filesystem::is_regular_file(path, error) || error)
        {
            return u8"복사할 파일을 찾을 수 없습니다. 파일이 이동되거나 삭제되었는지 확인하세요.";
        }

        std::vector<char> bytes {};
        std::optional<SkBitmap> image {};

        if (include_image)
        {
            bytes = read_file(path);
            image = bytes.empty() ? std::nullopt : decode_first_frame(bytes);
        }

        const clipboard_scope clipboard { owner };

        if (!clipboard.open() || !EmptyClipboard())
        {
            return u8"다른 앱이 클립보드를 사용 중입니다. 잠시 뒤 다시 복사하세요.";
        }

        bool copied { put_file(path) };

        if (image && put_image(*image))
        {
            copied = true;
        }

        auto extension { path.extension().wstring() };
        for (auto& character : extension)
        {
            character = static_cast<wchar_t>(towlower(character));
        }

        if (include_image && extension == L".png" && !bytes.empty() && put_png(bytes))
        {
            copied = true;
        }

        return copied ? std::u8string {} : u8"클립보드에 넣지 못했습니다. 메모리 상태를 확인하세요.";
    }
} // namespace chalkag
