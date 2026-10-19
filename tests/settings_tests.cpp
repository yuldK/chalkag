#include "app/settings.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace chalkag {
    namespace {
        int failure_count {};

        void check(bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failure_count;
            }
        }

        std::string read_text(const std::filesystem::path& path)
        {
            std::ifstream stream { path, std::ios::binary };
            return { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
        }

        void write_text(const std::filesystem::path& path, const std::string& text)
        {
            std::ofstream stream { path, std::ios::binary | std::ios::trunc };
            stream << text;
            check(stream.good(), "test fixture write");
        }

        app_settings sample_settings()
        {
            app_settings settings;
            settings.capture.fps = 33;
            settings.capture.output_percent = 150;
            settings.capture.include_cursor = true;
            settings.capture.still_format = image_format::webp;
            settings.capture.animation_format = image_format::webp;
            settings.shortcuts = { *parse_hotkey(u8"Ctrl+Alt+F9"),
                                   *parse_hotkey(u8"Ctrl+Alt+S"),
                                   hotkey_binding {},
                                   *parse_hotkey(u8"Ctrl+Alt+R")
            };
            settings.appearance = { luil::theme_preference::dark, u8"violet" };
            settings.window_position = command_window_position { -1700, -150 };
            settings.presets
                = { { u8"왼쪽 화면 🖼", { -1920, -200, 1920, 1080 } }, { u8"작은 영역", { 10, 20, 16, 16 } } };
            return settings;
        }

        void test_round_trip(const settings_store& store)
        {
            std::u8string warning { u8"이전 경고" };
            const auto defaults { store.load(warning) };
            check(warning.empty() && defaults.presets.empty(), "missing file uses defaults without warning");
            check(
                defaults.capture.fps == 16 && defaults.capture.output_percent == 100
                    && !defaults.capture.include_cursor,
                "default capture options"
            );
            check(!defaults.always_on_top, "missing file defaults always on top to false");
            check(!defaults.window_position, "missing file leaves window position to the OS");
            check(
                encode_hotkeys(defaults.shortcuts) == u8"Ctrl+Shift+A\nCtrl+Shift+S\nCtrl+R\nCtrl+Shift+R",
                "fresh settings use the requested shortcuts in action order"
            );
            check(
                defaults.appearance.theme == luil::theme_preference::system
                    && defaults.appearance.accent_id == u8"mint",
                "default system theme and mint accent"
            );
            check(store.path().parent_path().filename() == L"data", "data lives beside executable");

            const auto settings { sample_settings() };
            check(store.save(settings).empty(), "save Unicode presets");

            const auto loaded { store.load(warning) };
            check(warning.empty(), "load saved settings");
            check(
                loaded.capture.fps == 33 && loaded.capture.output_percent == 150 && loaded.capture.include_cursor,
                "capture options round trip"
            );
            check(
                loaded.capture.still_format == image_format::webp
                    && loaded.capture.animation_format == image_format::webp,
                "WebP formats round trip"
            );
            check(loaded.shortcuts == settings.shortcuts, "custom and disabled shortcuts round trip");
            check(loaded.appearance == settings.appearance, "theme and accent round trip");
            check(loaded.window_position == settings.window_position, "negative window position round trip");
            check(loaded.presets.size() == 2, "preset count round trip");

            if (loaded.presets.size() == 2)
            {
                check(
                    loaded.presets[0].name == settings.presets[0].name
                        && loaded.presets[1].name == settings.presets[1].name,
                    "Unicode names round trip"
                );
                check(
                    loaded.presets[0].region == settings.presets[0].region
                        && loaded.presets[1].region == settings.presets[1].region,
                    "negative monitor coordinates round trip"
                );
            }

            const auto text { read_text(store.path()) };
            check(text.size() > 3 && text.substr(0, 3) != "\xef\xbb\xbf", "settings omit UTF-8 BOM");

            for (std::size_t index {}; index < text.size(); ++index)
            {
                if (text[index] == '\n')
                {
                    check(index > 0 && text[index - 1] == '\r', "settings use CRLF");
                }
            }
        }

        void check_rejected_document(const settings_store& store, const nlohmann::json& document, const char* name)
        {
            const auto text { document.dump() };
            write_text(store.path(), text);

            std::u8string warning;
            const auto loaded { store.load(warning) };
            check(!warning.empty() && loaded.presets.empty(), name);
            check(read_text(store.path()) == text, "rejected file remains unchanged");
        }

        void test_invalid_files(const settings_store& store)
        {
            const auto valid { nlohmann::json::parse(read_text(store.path())) };
            auto invalid { valid };
            auto legacy { valid };
            legacy["window"].erase("position");
            legacy.erase("shortcuts");
            legacy.erase("appearance");
            write_text(store.path(), legacy.dump());

            std::u8string legacy_warning;
            check(
                store.load(legacy_warning).shortcuts == default_hotkeys && legacy_warning.empty(),
                "legacy file retains default shortcuts"
            );
            check(
                store.load(legacy_warning).appearance == app_settings {}.appearance,
                "legacy file retains system theme and mint accent"
            );
            check(!store.load(legacy_warning).window_position, "legacy file leaves window position to the OS");

            legacy = valid;
            legacy["shortcuts"].erase("discard_recording");
            write_text(store.path(), legacy.dump());
            auto expected { sample_settings().shortcuts };
            expected[3] = default_hotkeys[3];
            check(
                store.load(legacy_warning).shortcuts == expected && legacy_warning.empty(),
                "three-action settings retain custom bindings and gain the default clear shortcut"
            );
            legacy["shortcuts"]["recording"] = "Ctrl+Shift+R";
            write_text(store.path(), legacy.dump());
            expected[2] = default_hotkeys[3];
            expected[3] = {};
            check(
                store.load(legacy_warning).shortcuts == expected && legacy_warning.empty()
                    && read_text(store.path()) == legacy.dump(),
                "legacy shortcut collision disables only the new shortcut without rewriting the file"
            );

            invalid["window"]["position"] = "0, 0";
            check_rejected_document(store, invalid, "string window position rejected");
            invalid = valid;
            invalid["window"]["position"]["x"] = std::numeric_limits<std::uint64_t>::max();
            check_rejected_document(store, invalid, "window coordinate overflow rejected");
            invalid = valid;
            invalid["window"]["position"].erase("y");
            check_rejected_document(store, invalid, "incomplete window position rejected");
            invalid = valid;
            invalid["appearance"]["theme"] = "unknown";
            check_rejected_document(store, invalid, "unknown theme rejected without changing file");
            invalid = valid;
            invalid["appearance"]["accent_id"] = "unknown";
            check_rejected_document(store, invalid, "unknown accent rejected without changing file");
            invalid = valid;
            invalid["shortcuts"]["screenshot"] = invalid["shortcuts"]["select_region"];
            check_rejected_document(store, invalid, "duplicate shortcuts rejected without rewriting file");
            invalid = valid;
            invalid["shortcuts"]["discard_recording"] = invalid["shortcuts"]["select_region"];
            check_rejected_document(store, invalid, "duplicate clear shortcut rejected");
            invalid = valid;
            invalid["shortcuts"]["discard_recording"] = "Ctrl+F12";
            check_rejected_document(store, invalid, "reserved clear shortcut rejected");
            invalid = valid;
            invalid["shortcuts"]["recording"] = "Ctrl+F12";
            check_rejected_document(store, invalid, "reserved shortcut rejected");
            invalid = valid;
            invalid["capture"]["still_format"] = "gif";
            check_rejected_document(store, invalid, "GIF rejected for still images");
            invalid = valid;
            invalid["capture"]["animation_format"] = "png";
            check_rejected_document(store, invalid, "PNG rejected for animation");
            invalid = valid;
            invalid["capture"]["still_format"] = "apng";
            check_rejected_document(store, invalid, "APNG rejected for still images");
            invalid = valid;
            invalid["capture"]["webp_quality"] = 0;
            check_rejected_document(store, invalid, "invalid lossy quality rejected");
            invalid = valid;
            invalid["capture"]["encoder"] = 1;
            check_rejected_document(store, invalid, "integer encoder rejected");
            invalid = valid;
            invalid["capture"]["encoder"] = "unknown";
            check_rejected_document(store, invalid, "unknown encoder rejected");
            invalid = valid;
            invalid["capture"]["fps"] = "16";
            check_rejected_document(store, invalid, "string FPS rejected");
            invalid = valid;
            invalid["capture"]["output_percent"] = 125;
            check_rejected_document(store, invalid, "unsupported scale rejected");
            invalid = valid;
            invalid["capture"]["include_cursor"] = 1;
            check_rejected_document(store, invalid, "integer cursor flag rejected");
            invalid = valid;
            invalid["presets"][0]["region"]["width"] = 15;
            check_rejected_document(store, invalid, "undersized geometry rejected");
            invalid = valid;
            invalid["presets"][0]["region"]["x"] = std::numeric_limits<int>::max();
            check_rejected_document(store, invalid, "geometry end overflow rejected");
            invalid = valid;
            invalid["presets"][0]["region"]["x"] = std::numeric_limits<std::uint64_t>::max();
            check_rejected_document(store, invalid, "unsigned integer overflow rejected");
            invalid = valid;
            invalid["presets"][1]["name"] = invalid["presets"][0]["name"];
            check_rejected_document(store, invalid, "duplicate names rejected");
            invalid = valid;
            invalid["presets"][0]["name"] = "\n";
            check_rejected_document(store, invalid, "control character name rejected");
            invalid = valid;
            invalid["version"] = 2;
            check_rejected_document(store, invalid, "future version rejected");
            invalid = valid;
            invalid["capture"].erase("fps");
            check_rejected_document(store, invalid, "missing field rejected");

            write_text(store.path(), "{broken");

            std::u8string warning;
            static_cast<void>(store.load(warning));
            check(!warning.empty() && read_text(store.path()) == "{broken", "malformed JSON preserved");
            write_text(store.path(), std::string(1024 * 1024 + 1, ' '));
            static_cast<void>(store.load(warning));
            check(!warning.empty(), "oversized file rejected");
            write_text(store.path(), std::string(24, '[') + "0" + std::string(24, ']'));
            static_cast<void>(store.load(warning));
            check(!warning.empty(), "excessive nesting rejected");
            check(store.save(sample_settings()).empty(), "restore valid settings fixture");
        }

        void test_encoder_compatibility(const settings_store& store)
        {
            auto document { nlohmann::json::parse(read_text(store.path())) };
            document["capture"].erase("encoder");
            document["capture"]["animation_format"] = "gif";

            const auto text { document.dump() };
            write_text(store.path(), text);

            std::u8string warning;
            const auto legacy { store.load(warning) };
            check(
                warning.empty() && legacy.capture.animation_format == image_format::gif,
                "legacy GIF without encoder remains GIF"
            );
            check(read_text(store.path()) == text, "loading legacy settings does not rewrite file");

            for (const auto* format : { "gif", "webp", "apng" })
            {
                document["capture"]["encoder"] = "ffmpeg";
                document["capture"]["animation_format"] = format;
                document["capture"]["webp_lossless"] = false;
                document["capture"]["webp_quality"] = 63;
                write_text(store.path(), document.dump());

                const auto settings { store.load(warning) };
                const auto expected { std::string_view { format } == "gif"        ? image_format::gif
                                          : std::string_view { format } == "webp" ? image_format::webp
                                                                                  : image_format::apng
                };
                check(
                    warning.empty() && settings.capture.animation_format == expected && !settings.capture.webp_lossless
                        && settings.capture.webp_quality == 63,
                    "legacy FFmpeg settings retain format and quality"
                );
                check(store.save(settings).empty(), "native settings can be saved after migration");

                const auto saved { nlohmann::json::parse(read_text(store.path())) };
                check(
                    !saved["capture"].contains("encoder") && saved["capture"]["animation_format"] == format,
                    "explicit save removes obsolete encoder field"
                );
            }

            check(store.save(sample_settings()).empty(), "restore settings fixture");
        }

        void test_window_settings(const settings_store& store)
        {
            auto settings { sample_settings() };
            std::u8string warning;

            for (const bool always_on_top : { true, false })
            {
                settings.always_on_top = always_on_top;
                check(store.save(settings).empty(), "save always on top option");

                const auto loaded { store.load(warning) };
                check(
                    warning.empty() && loaded.always_on_top == always_on_top, "always on top true and false round trip"
                );

                const auto document { nlohmann::json::parse(read_text(store.path())) };
                check(
                    document.at("version") == 1 && document.at("window").is_object()
                        && document.at("window").at("always_on_top").is_boolean(),
                    "window option uses version one boolean schema"
                );
                check(document.at("window").at("always_on_top") == always_on_top, "window JSON stores selected value");
            }

            const auto valid { nlohmann::json::parse(read_text(store.path())) };
            auto legacy { valid };
            legacy.erase("window");

            const auto text { legacy.dump() };
            write_text(store.path(), text);

            const auto loaded { store.load(warning) };
            check(warning.empty() && !loaded.always_on_top, "legacy missing window defaults always on top to false");
            check(
                loaded.capture.fps == settings.capture.fps && loaded.presets.size() == settings.presets.size(),
                "legacy window omission preserves capture and presets"
            );
            check(read_text(store.path()) == text, "loading legacy window settings does not rewrite file");

            auto invalid { valid };

            for (const auto& window : { nlohmann::json {},
                                        nlohmann::json::array(),
                                        nlohmann::json("window"),
                                        nlohmann::json(true),
                                        nlohmann::json(1)
            })
            {
                invalid["window"] = window;
                check_rejected_document(store, invalid, "nonobject window setting rejected");
            }

            invalid["window"] = nlohmann::json::object();
            check_rejected_document(store, invalid, "missing always on top field rejected");

            for (const auto& value : { nlohmann::json {},
                                       nlohmann::json::array(),
                                       nlohmann::json::object(),
                                       nlohmann::json("true"),
                                       nlohmann::json(1)
            })
            {
                invalid = valid;
                invalid["window"]["always_on_top"] = value;
                check_rejected_document(store, invalid, "nonboolean always on top setting rejected");
            }

            check(store.save(sample_settings()).empty(), "restore settings after window validation");
        }

        void test_compression_settings(const settings_store& store)
        {
            for (const auto mode :
                 { lossless_compression::fast, lossless_compression::compact, lossless_compression::maximum })
            {
                auto settings { sample_settings() };
                settings.capture.compression = mode;
                check(store.save(settings).empty(), "compression profile saves");

                std::u8string warning;
                const auto loaded { store.load(warning) };
                check(warning.empty() && loaded.capture.compression == mode, "compression profile round trips");
            }

            auto document { nlohmann::json::parse(read_text(store.path())) };
            document["capture"].erase("compression");

            const auto text { document.dump() };
            write_text(store.path(), text);

            std::u8string warning;
            check(
                store.load(warning).capture.compression == lossless_compression::compact && warning.empty(),
                "legacy capture settings default to small files"
            );
            check(read_text(store.path()) == text, "legacy compression load preserves original file");

            for (const auto& invalid : { nlohmann::json("lossy"), nlohmann::json(false), nlohmann::json(42) })
            {
                document["capture"]["compression"] = invalid;
                check_rejected_document(store, document, "invalid compression preserves existing settings");
            }

            auto settings { sample_settings() };
            settings.capture.compression = static_cast<lossless_compression>(99);
            check(!store.save(settings).empty(), "invalid compression enum refuses save");
            check(store.save(sample_settings()).empty(), "restore valid compression fixture");
        }

        void test_export_settings(const settings_store& store)
        {
            app_settings settings {};
            settings.auto_save = true;
            settings.glow_width = 40;
            settings.capture.webp_lossless = false;
            settings.capture.webp_quality = 67;
            settings.capture.animation_format = image_format::gif;
            check(store.save(settings).empty(), "native GIF and export settings save");

            std::u8string warning;
            const auto loaded { store.load(warning) };
            check(
                warning.empty() && loaded.auto_save && loaded.glow_width == 40 && !loaded.capture.webp_lossless
                    && loaded.capture.webp_quality == 67 && loaded.capture.animation_format == image_format::gif,
                "export and glow settings round trip"
            );

            auto document { nlohmann::json::parse(read_text(store.path())) };
            const auto valid { document };
            document["capture"].erase("auto_save");
            document["capture"].erase("webp_lossless");
            document["capture"].erase("webp_quality");
            document["window"].erase("glow_width");
            write_text(store.path(), document.dump());

            const auto legacy { store.load(warning) };
            check(
                warning.empty() && !legacy.auto_save && legacy.capture.webp_lossless
                    && legacy.capture.webp_quality == 80 && legacy.glow_width == 24,
                "legacy settings default to manual save and lossless WebP"
            );

            for (const auto* key : { "auto_save", "webp_lossless" })
            {
                document = valid;
                document["capture"][key] = "true";
                check_rejected_document(store, document, "export booleans reject strings and preserve original");
            }

            for (const int width : { -1, 41 })
            {
                document = valid;
                document["window"]["glow_width"] = width;
                check_rejected_document(store, document, "glow width rejects out of range values");
            }

            check(store.save(sample_settings()).empty(), "restore valid export fixture");
        }

        void test_failed_save(const settings_store& store)
        {
            const auto original { read_text(store.path()) };
            auto settings { sample_settings() };
            settings.capture.fps = 60;
            check(
                !store.save(settings).empty() && read_text(store.path()) == original,
                "invalid save leaves original unchanged"
            );
            settings = sample_settings();
            settings.capture.webp_quality = 101;
            check(
                !store.save(settings).empty() && read_text(store.path()) == original,
                "invalid quality rejected without changing original"
            );
            settings = sample_settings();
            settings.presets.resize(65, settings.presets[0]);
            check(!store.save(settings).empty() && read_text(store.path()) == original, "too many presets rejected");
            settings = sample_settings();
            settings.presets[0].name.clear();

            for (int index {}; index < 129; ++index)
            {
                settings.presets[0].name += u8"가";
            }

            check(!store.save(settings).empty() && read_text(store.path()) == original, "long preset name rejected");

            settings = sample_settings();
            settings.capture.fps = 10;

            const HANDLE locked_file { CreateFileW(
                store.path().c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            )
            };
            check(locked_file != INVALID_HANDLE_VALUE, "lock original settings file");

            if (locked_file != INVALID_HANDLE_VALUE)
            {
                check(!store.save(settings).empty(), "atomic replacement reports locked file");
                check(read_text(store.path()) == original, "failed atomic replacement preserves old contents");
                CloseHandle(locked_file);
            }

            for (const auto& entry : std::filesystem::directory_iterator { store.path().parent_path() })
            {
                check(entry.path() == store.path(), "failed save removes temporary file");
            }

            check(store.save(settings).empty(), "replace existing settings atomically");

            std::u8string warning;
            check(store.load(warning).capture.fps == 10 && warning.empty(), "replacement contains new options");

            check(store.save({}).empty(), "save defaults with no presets");

            const auto defaults { store.load(warning) };
            check(warning.empty() && defaults.presets.empty(), "empty preset list round trip");
            check(!defaults.always_on_top, "default always on top option round trip");
            check(
                defaults.capture.still_format == image_format::png
                    && defaults.capture.animation_format == image_format::webp,
                "default native PNG and animated WebP options round trip"
            );
        }

    } // namespace
} // namespace chalkag

int main()
{
    const auto directory { std::filesystem::current_path() / L"settings-test-data"
                           / (L"한글-" + std::to_wstring(GetCurrentProcessId()) + L"-"
                              + std::to_wstring(GetTickCount64()))
    };

    std::filesystem::create_directories(directory);

    try
    {
        const chalkag::settings_store store { directory };
        chalkag::test_round_trip(store);
        chalkag::test_invalid_files(store);
        chalkag::test_encoder_compatibility(store);
        chalkag::test_window_settings(store);
        chalkag::test_compression_settings(store);
        chalkag::test_export_settings(store);
        chalkag::test_failed_save(store);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        ++chalkag::failure_count;
    }

    std::filesystem::remove_all(directory);

    if (chalkag::failure_count != 0)
    {
        return EXIT_FAILURE;
    }

    std::cout << "Settings tests passed.\n";
    return EXIT_SUCCESS;
}
