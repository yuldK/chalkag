#include "app/hotkeys.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>

namespace chalkag {
    namespace {
        int failures {};

        void check(const bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failures;
            }
        }

        void test_parser()
        {
            check(
                parse_hotkey(u8" shift + control + a ") == std::optional { hotkey_binding { 6, 'A' } },
                "normalize case, spaces and modifier order"
            );
            check(format_hotkey(*parse_hotkey(u8"alt+ctrl+F24")) == u8"Ctrl+Alt+F24", "canonical function key");
            check(
                parse_hotkey(u8"") == std::optional { hotkey_binding {} }
                    && parse_hotkey(u8"none") == parse_hotkey(u8""),
                "disabled binding"
            );
            check(
                parse_hotkey(u8"PrintScreen").has_value() && parse_hotkey(u8"Ctrl+PageDown").has_value(), "named keys"
            );

            for (const auto* text : { u8"A",
                                      u8"Shift+A",
                                      u8"Ctrl+Ctrl+A",
                                      u8"Ctrl+",
                                      u8"Ctrl++A",
                                      u8"Ctrl+Unknown",
                                      u8"Ctrl+한",
                                      u8"F0",
                                      u8"F25",
                                      u8"Ctrl+F12"
            })
            {
                check(!parse_hotkey(text), "reject malformed or reserved shortcut");
            }

            check(
                decode_hotkeys(encode_hotkeys(default_hotkeys)) == std::optional { default_hotkeys },
                "round trip all three actions"
            );
            check(decode_hotkeys(u8"\n\n").has_value(), "all shortcuts may be disabled");
            check(!decode_hotkeys(u8"Ctrl+A\nCtrl+A\nCtrl+B"), "reject duplicate bindings");
            check(
                !decode_hotkeys(u8"Ctrl+A\nCtrl+B") && !decode_hotkeys(u8"Ctrl+A\nCtrl+B\nCtrl+C\n"),
                "require exactly three bindings"
            );
        }

        bool test_registration()
        {
            const auto owner { CreateWindowExW(
                0,
                L"STATIC",
                L"chalkag hotkey test",
                0,
                0,
                0,
                0,
                0,
                HWND_MESSAGE,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr
            )
            };

            const auto blocker { CreateWindowExW(
                0,
                L"STATIC",
                L"chalkag hotkey blocker",
                0,
                0,
                0,
                0,
                0,
                HWND_MESSAGE,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr
            )
            };

            if (!owner || !blocker)
            {
                return false;
            }

            const hotkey_bindings original { hotkey_binding { 7, VK_F20 },
                                             hotkey_binding { 7, VK_F21 },
                                             hotkey_binding { 7, VK_F22 }
            };
            bool available {};
            {
                global_hotkeys hotkeys { owner };
                available = hotkeys.initialize(original).empty()
                    && RegisterHotKey(blocker, 1, 7 | MOD_NOREPEAT, VK_F23) != FALSE;

                if (available)
                {
                    auto swapped { original };
                    std::swap(swapped[0], swapped[2]);
                    check(hotkeys.apply(swapped).empty(), "swap existing shortcuts without collisions");

                    auto duplicate { swapped };
                    duplicate[1] = duplicate[0];
                    check(
                        !hotkeys.apply(duplicate).empty() && hotkeys.active(0) && hotkeys.active(1)
                            && hotkeys.active(2),
                        "validation retains active bindings"
                    );

                    auto conflicting { swapped };
                    conflicting[1].key = VK_F23;
                    check(
                        !hotkeys.apply(conflicting).empty() && hotkeys.active(0) && hotkeys.active(1)
                            && hotkeys.active(2),
                        "occupied chord rolls back all bindings"
                    );
                    check(!RegisterHotKey(blocker, 2, 7, VK_F22), "restored original shortcut remains registered");
                    check(
                        hotkeys.apply(hotkey_bindings {}).empty() && !hotkeys.active(0) && !hotkeys.active(1)
                            && !hotkeys.active(2),
                        "disable all shortcuts"
                    );
                    check(hotkeys.apply(original).empty(), "re-enable after disabling");
                }
            }

            if (available)
            {
                check(RegisterHotKey(blocker, 2, 7, VK_F20) != FALSE, "destructor releases hotkeys");
                UnregisterHotKey(blocker, 2);
            }

            UnregisterHotKey(blocker, 1);
            DestroyWindow(blocker);
            DestroyWindow(owner);
            return available;
        }

    } // namespace
} // namespace chalkag

int main()
{
    chalkag::test_parser();

    const bool available { chalkag::test_registration() };

    if (chalkag::failures != 0)
    {
        return EXIT_FAILURE;
    }

    return available ? EXIT_SUCCESS : 77;
}
