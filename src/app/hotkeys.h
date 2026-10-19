#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chalkag {
    struct hotkey_binding
    {
        std::uint32_t modifiers {};
        std::uint32_t key {};
        [[nodiscard]] bool operator==(const hotkey_binding&) const = default;
    };

    using hotkey_bindings = std::array<hotkey_binding, 4>;
    inline constexpr hotkey_bindings default_hotkeys {
        hotkey_binding { 6, 'A' }, hotkey_binding { 6, 'S' }, hotkey_binding { 2, 'R' }, hotkey_binding { 6, 'R' }
    };

    [[nodiscard]] std::optional<hotkey_binding> parse_hotkey(std::u8string_view text);
    [[nodiscard]] std::u8string format_hotkey(hotkey_binding binding);
    [[nodiscard]] std::u8string validate_hotkeys(const hotkey_bindings& bindings);
    [[nodiscard]] std::optional<hotkey_bindings> decode_hotkeys(std::u8string_view text);
    [[nodiscard]] std::u8string encode_hotkeys(const hotkey_bindings& bindings);

    class global_hotkeys final
    {
    public:
        explicit global_hotkeys(void* window) noexcept;
        ~global_hotkeys();
        global_hotkeys(const global_hotkeys&) = delete;
        global_hotkeys& operator=(const global_hotkeys&) = delete;
        [[nodiscard]] std::u8string initialize(const hotkey_bindings& bindings);
        [[nodiscard]] std::u8string apply(const hotkey_bindings& bindings);
        [[nodiscard]] bool active(std::size_t index) const noexcept;
        void clear() noexcept;

    private:
        void* window_ {};
        hotkey_bindings bindings_ {};
        std::array<bool, default_hotkeys.size()> active_ {};
    };
} // namespace chalkag
