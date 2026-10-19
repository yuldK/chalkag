#include "app/hotkeys.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <string>

namespace chalkag {
    namespace {
        constexpr std::array key_names {
            std::pair { u8"PRINTSCREEN", VK_SNAPSHOT },
            std::pair { u8"SPACE", VK_SPACE },
            std::pair { u8"ENTER", VK_RETURN },
            std::pair { u8"HOME", VK_HOME },
            std::pair { u8"END", VK_END },
            std::pair { u8"PAGEUP", VK_PRIOR },
            std::pair { u8"PAGEDOWN", VK_NEXT },
            std::pair { u8"INSERT", VK_INSERT },
            std::pair { u8"DELETE", VK_DELETE },
            std::pair { u8"LEFT", VK_LEFT },
            std::pair { u8"RIGHT", VK_RIGHT },
            std::pair { u8"UP", VK_UP },
            std::pair { u8"DOWN", VK_DOWN },
        };

        bool valid_key(const std::uint32_t key)
        {
            return (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')
                || (key >= VK_F1 && key <= VK_F24 && key != VK_F12)
                || std::any_of(key_names.begin(), key_names.end(), [key](const auto& item) {
                       return key == static_cast<std::uint32_t>(item.second);
                   });
        }

        bool valid_binding(const hotkey_binding binding)
        {
            if (binding.key == 0)
            {
                return binding.modifiers == 0;
            }

            if ((binding.modifiers & ~15U) != 0 || !valid_key(binding.key))
            {
                return false;
            }

            // 문자 입력을 전역으로 가로채지 않도록 Shift 단독 조합은 허용하지 않는다.
            return (binding.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)) != 0
                || (binding.key >= VK_F1 && binding.key <= VK_F24) || binding.key == VK_SNAPSHOT;
        }

        std::u8string key_text(const std::uint32_t key)
        {
            if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
            {
                return std::u8string(1, static_cast<char8_t>(key));
            }

            if (key >= VK_F1 && key <= VK_F24)
            {
                const auto number { std::to_string(key - VK_F1 + 1) };
                return u8"F" + std::u8string { number.begin(), number.end() };
            }

            for (const auto& item : key_names)
            {
                if (key == static_cast<std::uint32_t>(item.second))
                {
                    return item.first;
                }
            }

            return {};
        }

        bool register_binding(void* window, const std::size_t index, const hotkey_binding binding)
        {
            return binding.key != 0
                && RegisterHotKey(
                       static_cast<HWND>(window),
                       101 + static_cast<int>(index),
                       binding.modifiers | MOD_NOREPEAT,
                       binding.key
                   )
                != FALSE;
        }

    } // namespace

    std::optional<hotkey_binding> parse_hotkey(const std::u8string_view text)
    {
        if (text.size() > 64)
        {
            return std::nullopt;
        }

        std::u8string value {};

        for (const char8_t character : text)
        {
            if (character == u8' ' || character == u8'\t')
            {
                continue;
            }

            if (character > 127)
            {
                return std::nullopt;
            }

            value.push_back(
                static_cast<char8_t>(character >= u8'a' && character <= u8'z' ? character - u8'a' + u8'A' : character)
            );
        }

        if (value.empty() || value == u8"NONE")
        {
            return hotkey_binding {};
        }

        hotkey_binding binding {};

        std::size_t begin {};

        for (;;)
        {
            const auto end { value.find(u8'+', begin) };

            if (end == std::u8string::npos)
            {
                break;
            }

            const auto token { value.substr(begin, end - begin) };
            const auto modifier = static_cast<std::uint32_t>(
                token == u8"CTRL" || token == u8"CONTROL" ? MOD_CONTROL
                    : token == u8"ALT"                    ? MOD_ALT
                    : token == u8"SHIFT"                  ? MOD_SHIFT
                    : token == u8"WIN"                    ? MOD_WIN
                                                          : 0
            );

            if (modifier == 0 || (binding.modifiers & modifier) != 0)
            {
                return std::nullopt;
            }

            binding.modifiers |= modifier;
            begin = end + 1;
        }

        const auto token { value.substr(begin) };

        if (token.size() == 1)
        {
            binding.key = token.front();
        }
        else if (token.size() >= 2 && token.size() <= 3 && token.front() == u8'F')
        {
            int number {};

            for (std::size_t index { 1 }; index < token.size(); ++index)
            {
                if (token[index] < u8'0' || token[index] > u8'9')
                {
                    return std::nullopt;
                }

                number = number * 10 + token[index] - u8'0';
            }

            if (number < 1 || number > 24)
            {
                return std::nullopt;
            }

            binding.key = VK_F1 + number - 1;
        }
        else
        {
            for (const auto& item : key_names)
            {
                if (token == item.first)
                {
                    binding.key = item.second;
                }
            }
        }

        return binding.key != 0 && valid_binding(binding) ? std::optional { binding } : std::nullopt;
    }

    std::u8string format_hotkey(const hotkey_binding binding)
    {
        if (binding.key == 0)
        {
            return {};
        }

        std::u8string text {};

        for (const auto& [modifier, name] : std::array { std::pair { MOD_CONTROL, u8"Ctrl+" },
                                                         std::pair { MOD_ALT, u8"Alt+" },
                                                         std::pair { MOD_SHIFT, u8"Shift+" },
                                                         std::pair { MOD_WIN, u8"Win+" }
        })
        {
            if ((binding.modifiers & modifier) != 0)
            {
                text += name;
            }
        }

        return text + key_text(binding.key);
    }

    std::u8string validate_hotkeys(const hotkey_bindings& bindings)
    {
        for (std::size_t index {}; index < bindings.size(); ++index)
        {
            if (!valid_binding(bindings[index]))
            {
                return u8"Ctrl·Alt·Win과 문자/숫자, F1–F24(F12 제외), PrintScreen 등을 조합하세요. 비우면 사용하지 "
                       u8"않습니다.";
            }

            if (bindings[index].key != 0)
            {
                for (std::size_t previous {}; previous < index; ++previous)
                {
                    if (bindings[previous] == bindings[index])
                    {
                        return u8"각 기능에 서로 다른 단축키를 지정하세요.";
                    }
                }
            }
        }

        return {};
    }

    std::optional<hotkey_bindings> decode_hotkeys(std::u8string_view text)
    {
        hotkey_bindings bindings {};

        for (std::size_t index {}; index < bindings.size(); ++index)
        {
            const auto end { text.find(u8'\n') };

            if ((index + 1 < bindings.size()) != (end != std::u8string_view::npos))
            {
                return std::nullopt;
            }

            const auto binding { parse_hotkey(text.substr(0, end)) };

            if (!binding)
            {
                return std::nullopt;
            }

            bindings[index] = *binding;

            if (end != std::u8string_view::npos)
            {
                text.remove_prefix(end + 1);
            }
        }

        return validate_hotkeys(bindings).empty() ? std::optional { bindings } : std::nullopt;
    }

    std::u8string encode_hotkeys(const hotkey_bindings& bindings)
    {
        std::u8string text {};

        for (std::size_t index {}; index < bindings.size(); ++index)
        {
            if (index != 0)
            {
                text += u8"\n";
            }

            text += format_hotkey(bindings[index]);
        }

        return text;
    }

    global_hotkeys::global_hotkeys(void* window) noexcept
        : window_ { window }
    {}

    global_hotkeys::~global_hotkeys()
    {
        clear();
    }

    void global_hotkeys::clear() noexcept
    {
        for (std::size_t index {}; index < active_.size(); ++index)
        {
            if (active_[index])
            {
                UnregisterHotKey(static_cast<HWND>(window_), 101 + static_cast<int>(index));
            }
        }

        active_ = {};
    }

    bool global_hotkeys::active(const std::size_t index) const noexcept
    {
        return index < active_.size() && active_[index];
    }

    std::u8string global_hotkeys::initialize(const hotkey_bindings& bindings)
    {
        if (const auto error { validate_hotkeys(bindings) }; !error.empty())
        {
            return error;
        }

        clear();
        bindings_ = bindings;

        std::u8string warning {};

        for (std::size_t index {}; index < bindings.size(); ++index)
        {
            active_[index] = register_binding(window_, index, bindings[index]);

            if (bindings[index].key != 0 && !active_[index])
            {
                warning += format_hotkey(bindings[index])
                    + u8" 단축키를 등록하지 못했습니다. 다른 앱의 사용 여부를 확인하세요. ";
            }
        }

        return warning;
    }

    std::u8string global_hotkeys::apply(const hotkey_bindings& bindings)
    {
        if (const auto error { validate_hotkeys(bindings) }; !error.empty())
        {
            return error;
        }

        const auto previous { bindings_ };
        const auto previous_active { active_ };
        clear();

        for (std::size_t index {}; index < bindings.size(); ++index)
        {
            active_[index] = register_binding(window_, index, bindings[index]);

            if (bindings[index].key == 0 || active_[index])
            {
                continue;
            }

            clear();
            bool restored { true };

            for (std::size_t old {}; old < previous.size(); ++old)
            {
                if (previous_active[old])
                {
                    active_[old] = register_binding(window_, old, previous[old]);
                    restored = restored && active_[old];
                }
            }

            return format_hotkey(bindings[index]) + u8" 단축키를 등록하지 못했습니다. 다른 앱의 사용 여부를 확인하세요."
                + (restored ? u8" 기존 단축키를 유지했습니다."
                            : u8" 일부 기존 단축키도 복원하지 못했습니다. 다시 적용하세요.");
        }

        bindings_ = bindings;
        return {};
    }

} // namespace chalkag
