#include "app/settings.h"
#include "luil/theme/ui_theme.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace chalkag {
    namespace {
        constexpr std::uintmax_t maximum_file_size { 1024 * 1024 };
        constexpr std::size_t maximum_presets { 64 };
        constexpr std::size_t maximum_name_characters { 128 };
        constexpr std::array allowed_fps { 10, 16, 24, 30, 33 };
        constexpr std::array allowed_output_percent { 25, 50, 75, 100, 150, 200 };
        std::atomic<unsigned long> temporary_counter {};

        std::string narrow(std::u8string_view value)
        {
            return { reinterpret_cast<const char*>(value.data()), value.size() };
        }

        std::u8string widen(std::string_view value)
        {
            return { reinterpret_cast<const char8_t*>(value.data()), value.size() };
        }

        void require(bool condition, std::u8string_view message)
        {
            if (!condition)
            {
                throw std::runtime_error { narrow(message) };
            }
        }

        bool valid_name(std::u8string_view name)
        {
            if (name.empty() || name.size() > maximum_name_characters * 4)
            {
                return false;
            }

            const auto byte_count { static_cast<int>(name.size()) };
            const int wide_count { MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(name.data()), byte_count, nullptr, 0
            )
            };

            if (wide_count <= 0)
            {
                return false;
            }

            std::wstring text(static_cast<std::size_t>(wide_count), L'\0');

            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    reinterpret_cast<const char*>(name.data()),
                    byte_count,
                    text.data(),
                    wide_count
                )
                != wide_count)
            {
                return false;
            }

            std::size_t character_count {};
            bool visible_character {};

            for (const wchar_t character : text)
            {
                if (character < 0x20 || (character >= 0x7f && character <= 0x9f))
                {
                    return false;
                }

                if (character < 0xdc00 || character > 0xdfff)
                {
                    ++character_count;
                }

                if (character != L' ')
                {
                    visible_character = true;
                }
            }

            return visible_character && character_count <= maximum_name_characters;
        }

        bool valid_region(capture_region region)
        {
            const auto right { static_cast<std::int64_t>(region.x) + region.width };
            const auto bottom { static_cast<std::int64_t>(region.y) + region.height };
            return region.width >= minimum_region_size && region.height >= minimum_region_size
                && right <= std::numeric_limits<int>::max() && bottom <= std::numeric_limits<int>::max();
        }

        void validate(const app_settings& settings)
        {
            require(settings.glow_width >= 0 && settings.glow_width <= 40, u8"글로우 두께는 0~40px여야 합니다.");

            const auto& appearance { settings.appearance };
            require(
                appearance.theme == luil::theme_preference::system || appearance.theme == luil::theme_preference::light
                    || appearance.theme == luil::theme_preference::dark,
                u8"지원하지 않는 테마입니다."
            );
            require(
                appearance.accent_id == luil::system_accent_id || luil::accent_exists(appearance.accent_id),
                u8"지원하지 않는 키 컬러입니다."
            );

            const auto shortcuts_error { validate_hotkeys(settings.shortcuts) };
            require(shortcuts_error.empty(), shortcuts_error);
            require(
                std::find(allowed_fps.begin(), allowed_fps.end(), settings.capture.fps) != allowed_fps.end(),
                u8"FPS는 10, 16, 24, 30, 33 중 하나여야 합니다."
            );
            require(
                std::find(allowed_output_percent.begin(), allowed_output_percent.end(), settings.capture.output_percent)
                    != allowed_output_percent.end(),
                u8"출력 배율이 지원 범위를 벗어났습니다."
            );
            require(
                settings.capture.still_format == image_format::png
                    || settings.capture.still_format == image_format::webp,
                u8"정지 이미지 형식은 PNG 또는 WebP여야 합니다."
            );
            require(
                settings.capture.animation_format == image_format::gif
                    || settings.capture.animation_format == image_format::webp
                    || settings.capture.animation_format == image_format::apng,
                u8"애니메이션 형식은 WebP, APNG 또는 GIF여야 합니다."
            );
            require(
                settings.capture.compression == lossless_compression::fast
                    || settings.capture.compression == lossless_compression::compact
                    || settings.capture.compression == lossless_compression::maximum,
                u8"지원하지 않는 압축 설정입니다."
            );
            require(
                settings.capture.webp_quality >= 1 && settings.capture.webp_quality <= 100,
                u8"WebP 품질은 1~100이어야 합니다."
            );
            require(settings.presets.size() <= maximum_presets, u8"영역 프리셋은 최대 64개까지 저장할 수 있습니다.");

            for (std::size_t index {}; index < settings.presets.size(); ++index)
            {
                const auto& preset { settings.presets[index] };
                require(valid_name(preset.name), u8"프리셋 이름은 제어 문자 없이 1~128자의 UTF-8 문자열이어야 합니다.");
                require(
                    valid_region(preset.region),
                    u8"프리셋 영역은 최소 16×16이며 좌표 끝점이 정수 범위를 벗어나면 안 됩니다."
                );

                for (std::size_t previous {}; previous < index; ++previous)
                {
                    require(settings.presets[previous].name != preset.name, u8"프리셋 이름은 중복될 수 없습니다.");
                }
            }
        }

        int read_integer(const nlohmann::json& object, const char* key)
        {
            const auto& value { object.at(key) };
            require(value.is_number_integer(), u8"정수 설정 항목의 형식이 잘못되었습니다.");

            if (value.is_number_unsigned())
            {
                const auto number { value.get<std::uint64_t>() };
                require(
                    number <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()),
                    u8"정수 설정 항목이 허용 범위를 벗어났습니다."
                );
                return static_cast<int>(number);
            }

            const auto number { value.get<std::int64_t>() };
            require(
                number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max(),
                u8"정수 설정 항목이 허용 범위를 벗어났습니다."
            );
            return static_cast<int>(number);
        }

        image_format read_format(const nlohmann::json& object, const char* key)
        {
            const auto& value { object.at(key) };
            require(value.is_string(), u8"이미지 형식 설정은 문자열이어야 합니다.");

            const auto format { value.get<std::string>() };

            if (format == "png")
            {
                return image_format::png;
            }

            if (format == "webp")
            {
                return image_format::webp;
            }

            if (format == "gif")
            {
                return image_format::gif;
            }

            if (format == "apng")
            {
                return image_format::apng;
            }

            throw std::runtime_error { narrow(u8"지원하지 않는 이미지 형식입니다.") };
        }

        const char* format_name(image_format format)
        {
            switch (format)
            {
            case image_format::png:
                return "png";

            case image_format::webp:
                return "webp";

            case image_format::gif:
                return "gif";

            case image_format::apng:
                return "apng";
            }

            throw std::runtime_error { narrow(u8"지원하지 않는 이미지 형식입니다.") };
        }

        app_settings decode(const nlohmann::json& document)
        {
            require(
                document.is_object() && read_integer(document, "version") == 1, u8"지원하지 않는 설정 파일 버전입니다."
            );

            const auto& capture { document.at("capture") };
            require(capture.is_object(), u8"캡쳐 설정은 객체여야 합니다.");
            app_settings settings;
            settings.capture.fps = read_integer(capture, "fps");
            settings.capture.output_percent = read_integer(capture, "output_percent");
            require(capture.at("include_cursor").is_boolean(), u8"마우스 커서 포함 설정은 참 또는 거짓이어야 합니다.");
            settings.capture.include_cursor = capture.at("include_cursor").get<bool>();
            settings.capture.still_format = read_format(capture, "still_format");
            settings.capture.animation_format = read_format(capture, "animation_format");

            if (capture.contains("compression"))
            {
                const auto& compression { capture.at("compression") };
                require(compression.is_string(), u8"압축 설정은 문자열이어야 합니다.");

                const auto value { compression.get<std::string>() };
                require(value == "fast" || value == "small" || value == "maximum", u8"지원하지 않는 압축 설정입니다.");
                settings.capture.compression = value == "fast" ? lossless_compression::fast
                    : value == "maximum"                       ? lossless_compression::maximum
                                                               : lossless_compression::compact;
            }

            if (capture.contains("webp_lossless"))
            {
                require(capture.at("webp_lossless").is_boolean(), u8"WebP 무손실 설정은 참 또는 거짓이어야 합니다.");
                settings.capture.webp_lossless = capture.at("webp_lossless").get<bool>();
            }

            if (capture.contains("webp_quality"))
            {
                settings.capture.webp_quality = read_integer(capture, "webp_quality");
            }

            if (capture.contains("auto_save"))
            {
                require(capture.at("auto_save").is_boolean(), u8"바로 저장 설정은 참 또는 거짓이어야 합니다.");
                settings.auto_save = capture.at("auto_save").get<bool>();
            }

            // 이전 인코더 선택은 형식과 품질을 보존하고 현재 내장 인코더로 읽는다.
            if (capture.contains("encoder"))
            {
                const auto& encoder { capture.at("encoder") };
                require(encoder.is_string(), u8"인코더 설정은 문자열이어야 합니다.");
                require(encoder == "native" || encoder == "ffmpeg", u8"지원하지 않는 이전 인코더 설정입니다.");
            }

            if (document.contains("window"))
            {
                const auto& window { document.at("window") };
                require(window.is_object(), u8"창 설정은 객체여야 합니다.");

                const auto& always_on_top { window.at("always_on_top") };
                require(always_on_top.is_boolean(), u8"항상 위 설정은 참 또는 거짓이어야 합니다.");
                settings.always_on_top = always_on_top.get<bool>();

                if (window.contains("glow_width"))
                {
                    settings.glow_width = read_integer(window, "glow_width");
                }

                if (window.contains("position"))
                {
                    const auto& position { window.at("position") };
                    require(position.is_object(), u8"창 위치는 좌표 객체여야 합니다.");
                    settings.window_position
                        = command_window_position { read_integer(position, "x"), read_integer(position, "y") };
                }
            }

            if (document.contains("shortcuts"))
            {
                const auto& shortcuts { document.at("shortcuts") };
                require(shortcuts.is_object(), u8"단축키 설정은 객체여야 합니다.");

                constexpr std::array names { "select_region", "screenshot", "recording" };

                for (std::size_t index {}; index < names.size(); ++index)
                {
                    const auto& value { shortcuts.at(names[index]) };
                    require(value.is_string(), u8"단축키 설정은 문자열이어야 합니다.");

                    const auto binding { parse_hotkey(widen(value.get<std::string>())) };
                    require(binding.has_value(), u8"지원하지 않는 단축키 조합입니다.");
                    settings.shortcuts[index] = *binding;
                }
            }

            if (document.contains("appearance"))
            {
                const auto& appearance { document.at("appearance") };
                require(
                    appearance.is_object() && appearance.at("theme").is_string()
                        && appearance.at("accent_id").is_string(),
                    u8"테마와 키 컬러는 문자열이어야 합니다."
                );
                require(
                    luil::parse_theme_preference(
                        widen(appearance.at("theme").get<std::string>()), settings.appearance.theme
                    ),
                    u8"지원하지 않는 테마입니다."
                );
                settings.appearance.accent_id = widen(appearance.at("accent_id").get<std::string>());
            }

            const auto& presets { document.at("presets") };
            require(
                presets.is_array() && presets.size() <= maximum_presets,
                u8"영역 프리셋 목록이 잘못되었거나 64개를 초과했습니다."
            );

            for (const auto& value : presets)
            {
                require(value.is_object() && value.at("name").is_string(), u8"프리셋 이름은 문자열이어야 합니다.");

                const auto& region { value.at("region") };
                require(region.is_object(), u8"프리셋 영역은 객체여야 합니다.");
                settings.presets.push_back(
                    { widen(value.at("name").get<std::string>()),
                      { read_integer(region, "x"),
                        read_integer(region, "y"),
                        read_integer(region, "width"),
                        read_integer(region, "height")
                      }
                    }
                );
            }

            validate(settings);
            return settings;
        }

        nlohmann::json encode(const app_settings& settings)
        {
            auto presets { nlohmann::json::array() };

            for (const auto& preset : settings.presets)
            {
                nlohmann::json region { { "x", preset.region.x },
                                        { "y", preset.region.y },
                                        { "width", preset.region.width },
                                        { "height", preset.region.height }
                };
                presets.push_back({ { "name", narrow(preset.name) }, { "region", std::move(region) } });
            }

            nlohmann::json capture;
            capture["fps"] = settings.capture.fps;
            capture["output_percent"] = settings.capture.output_percent;
            capture["include_cursor"] = settings.capture.include_cursor;
            capture["still_format"] = format_name(settings.capture.still_format);
            capture["animation_format"] = format_name(settings.capture.animation_format);
            capture["compression"] = settings.capture.compression == lossless_compression::fast ? "fast"
                : settings.capture.compression == lossless_compression::maximum                 ? "maximum"
                                                                                                : "small";
            capture["webp_lossless"] = settings.capture.webp_lossless;
            capture["webp_quality"] = settings.capture.webp_quality;
            capture["auto_save"] = settings.auto_save;
            nlohmann::json window { { "always_on_top", settings.always_on_top } };
            window["glow_width"] = settings.glow_width;

            if (settings.window_position)
            {
                window["position"] = { { "x", settings.window_position->x }, { "y", settings.window_position->y } };
            }

            nlohmann::json shortcuts {};
            shortcuts["select_region"] = narrow(format_hotkey(settings.shortcuts[0]));
            shortcuts["screenshot"] = narrow(format_hotkey(settings.shortcuts[1]));
            shortcuts["recording"] = narrow(format_hotkey(settings.shortcuts[2]));
            nlohmann::json appearance {};
            appearance["theme"] = narrow(luil::theme_preference_name(settings.appearance.theme));
            appearance["accent_id"] = narrow(settings.appearance.accent_id);
            return {
                { "version", 1 },
                { "capture", std::move(capture) },
                { "presets", std::move(presets) },
                { "window", std::move(window) },
                { "shortcuts", std::move(shortcuts) },
                { "appearance", std::move(appearance) },
            };
        }

        bool check_json_depth(int depth, nlohmann::json::parse_event_t, nlohmann::json&)
        {
            require(depth <= 16, u8"설정 파일의 중첩 깊이가 너무 큽니다.");
            return true;
        }

        std::u8string windows_error(std::u8string_view operation, DWORD error)
        {
            return std::u8string { operation } + u8" (Windows 오류 " + widen(std::to_string(error)) + u8")";
        }

        // 같은 폴더에 임시 파일을 완성한 뒤 교체하여 저장 실패가 기존 설정을 손상시키지 않게 한다.
        std::u8string write_atomic(const std::filesystem::path& path, const std::string& text)
        {
            auto temporary { path };
            temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L"."
                + std::to_wstring(temporary_counter.fetch_add(1)) + L".tmp";

            const HANDLE file {
                CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)
            };

            if (file == INVALID_HANDLE_VALUE)
            {
                return windows_error(u8"임시 설정 파일을 만들지 못했습니다.", GetLastError());
            }

            DWORD written {};

            std::u8string error;

            if (!WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
            {
                error = windows_error(u8"설정 파일을 기록하지 못했습니다.", GetLastError());
            }
            else if (written != text.size())
            {
                error = windows_error(u8"설정 파일을 완전히 기록하지 못했습니다.", ERROR_WRITE_FAULT);
            }
            else if (!FlushFileBuffers(file))
            {
                error = windows_error(u8"설정 파일을 디스크에 반영하지 못했습니다.", GetLastError());
            }

            if (!CloseHandle(file) && error.empty())
            {
                error = windows_error(u8"설정 파일을 닫지 못했습니다.", GetLastError());
            }

            if (error.empty()
                && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error = windows_error(u8"기존 설정 파일을 교체하지 못했습니다.", GetLastError());
            }

            if (!error.empty())
            {
                DeleteFileW(temporary.c_str());
            }

            return error;
        }

    } // namespace

    settings_store::settings_store(std::filesystem::path executable_directory)
        : path_ { std::move(executable_directory) / L"data" / L"settings.json" }
    {}

    app_settings settings_store::load(std::u8string& warning) const
    {
        warning.clear();

        try
        {
            std::error_code error;
            const bool exists { std::filesystem::exists(path_, error) };
            require(!error, u8"설정 파일 존재 여부를 확인하지 못했습니다.");

            if (!exists)
            {
                return {};
            }

            const auto size { std::filesystem::file_size(path_) };
            require(size > 0 && size <= maximum_file_size, u8"설정 파일은 비어 있거나 1 MiB 제한을 초과했습니다.");

            std::ifstream stream { path_, std::ios::binary };
            require(stream.is_open(), u8"설정 파일을 열지 못했습니다.");

            std::string text(static_cast<std::size_t>(size), '\0');
            stream.read(text.data(), static_cast<std::streamsize>(text.size()));
            require(
                stream.good() && stream.peek() == std::char_traits<char>::eof(),
                u8"설정 파일을 완전히 읽지 못했거나 읽는 중 변경되었습니다."
            );
            return decode(nlohmann::json::parse(text, check_json_depth));
        }
        catch (const nlohmann::json::exception&)
        {
            warning = u8"설정 파일의 JSON 형식 또는 필수 항목이 잘못되었습니다. 원본 파일을 보존합니다.";
        }
        catch (const std::filesystem::filesystem_error&)
        {
            warning = u8"설정 파일을 읽지 못했습니다. 원본 파일을 보존합니다.";
        }
        catch (const std::exception& error)
        {
            warning = u8"설정을 불러오지 못했습니다: " + widen(error.what()) + u8" 원본 파일을 보존합니다.";
        }

        return {};
    }

    std::u8string settings_store::save(const app_settings& settings) const
    {
        try
        {
            validate(settings);

            const auto document { encode(settings) };
            const std::string text { document.dump(4) };
            std::string windows_text;
            windows_text.reserve(text.size() + text.size() / 10);

            for (const char character : text)
            {
                if (character == '\n')
                {
                    windows_text += '\r';
                }

                windows_text += character;
            }

            windows_text += "\r\n";
            require(windows_text.size() <= maximum_file_size, u8"설정 파일이 1 MiB 제한을 초과했습니다.");

            std::filesystem::create_directories(path_.parent_path());
            return write_atomic(path_, windows_text);
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return u8"실행 파일 옆 data 폴더에 설정을 저장하지 못했습니다.";
        }
        catch (const std::exception& error)
        {
            return u8"설정을 저장하지 못했습니다: " + widen(error.what());
        }
    }

    const std::filesystem::path& settings_store::path() const noexcept
    {
        return path_;
    }

} // namespace chalkag
