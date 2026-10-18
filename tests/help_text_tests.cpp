#include "app/help_text.h"

#include "luil/text/utf8_text.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chalkag {
    namespace {
        int failure_count {};

        void check(const bool condition, const char* name)
        {
            if (!condition)
            {
                std::cerr << "FAIL: " << name << '\n';
                ++failure_count;
            }
        }

        float fixed_measure(const std::u8string_view text)
        {
            check(luil::text::utf8_is_valid(text), "measurement always receives complete valid UTF-8");
            float width {};

            for (const auto byte : text)
            {
                if ((static_cast<unsigned char>(byte) & 0xc0U) != 0x80U)
                {
                    width += 1.0f;
                }
            }

            return width;
        }

        void check_lines(const help_text_layout& actual, const std::vector<std::u8string>& expected, const char* name)
        {
            check(actual.lines == expected, name);

            for (const auto& line : actual.lines)
            {
                check(luil::text::utf8_is_valid(line), "wrapped lines retain valid UTF-8");
            }
        }

        std::u8string join_lines(const help_text_layout& layout)
        {
            std::u8string result {};

            for (const auto& line : layout.lines)
            {
                result += line;
            }

            return result;
        }

        void test_wrapping()
        {
            check_lines(wrap_help_text(u8"", 10, fixed_measure), {}, "empty help has no lines");
            check_lines(
                wrap_help_text(u8"plain", 5, fixed_measure), { u8"plain" }, "exactly fitting text stays on one line"
            );
            check_lines(
                wrap_help_text(u8"abcdefghi", 3, fixed_measure),
                { u8"abc", u8"def", u8"ghi" },
                "unbroken ASCII wraps at measured boundary"
            );
            check_lines(
                wrap_help_text(u8"가나다라마바사", 3, fixed_measure),
                { u8"가나다", u8"라마바", u8"사" },
                "unbroken Korean wraps without splitting bytes"
            );
            check_lines(
                wrap_help_text(u8"A😀나🎥Z", 2, fixed_measure),
                { u8"A😀", u8"나🎥", u8"Z" },
                "mixed one three and four-byte characters wrap safely"
            );
            check_lines(
                wrap_help_text(u8"alpha beta gamma", 9, fixed_measure),
                { u8"alpha", u8"beta", u8"gamma" },
                "spaces are preferred over cutting words"
            );
            check_lines(
                wrap_help_text(u8"abc def", 3, fixed_measure),
                { u8"abc", u8"def" },
                "separator after exact-width word adds no empty line"
            );
            check_lines(
                wrap_help_text(u8"word elephant", 7, fixed_measure),
                { u8"word", u8"elephan", u8"t" },
                "oversized word wraps after preferred word boundary"
            );
            check_lines(
                wrap_help_text(u8"  one  two\tthree  ", 5, fixed_measure),
                { u8"one", u8"two", u8"three" },
                "wrap-boundary spaces and tabs are consumed"
            );
            check_lines(
                wrap_help_text(u8"a  b", 4, fixed_measure), { u8"a  b" }, "internal spaces survive when full line fits"
            );
            check_lines(
                wrap_help_text(u8"가\r\n\r\n나\n", 10, fixed_measure),
                { u8"가", u8"", u8"나", u8"" },
                "CRLF and LF preserve explicit empty and trailing lines"
            );
            check_lines(
                wrap_help_text(u8"\na\rb", 10, fixed_measure),
                { u8"", u8"a", u8"b" },
                "leading blank line and standalone CR remain line breaks"
            );
            check_lines(
                wrap_help_text(u8" \t ", 10, fixed_measure),
                { u8"" },
                "whitespace-only paragraph still occupies one line"
            );

            const std::u8string path { u8"E:\\한글폴더\\공백없는아주긴경로\\capture-20261006-123456.webp" };
            const auto layout { wrap_help_text(path, 7, fixed_measure) };
            check(
                layout.lines.size() > 1 && join_lines(layout) == path, "long path wraps without losing any characters"
            );

            for (const auto& line : layout.lines)
            {
                check(fixed_measure(line) <= 7, "long path stays within available width");
            }
        }

        void test_truncation()
        {
            check_lines(
                wrap_help_text(u8"abcdefghi", 3, fixed_measure, 2),
                { u8"abc", u8"de…" },
                "hidden extra line elides last visible line within width"
            );
            check_lines(
                wrap_help_text(u8"가나다라마바사", 3, fixed_measure, 1),
                { u8"가나…" },
                "ellipsis truncates Korean at complete codepoint boundary"
            );
            check_lines(
                wrap_help_text(u8"A😀나🎥Z", 2, fixed_measure, 1),
                { u8"A…" },
                "ellipsis truncates emoji without splitting bytes"
            );
            check_lines(
                wrap_help_text(u8"abc\ndef", 4, fixed_measure, 1),
                { u8"abc…" },
                "explicit hidden paragraph appends fitting ellipsis"
            );
            check_lines(
                wrap_help_text(u8"a\n\nb", 4, fixed_measure, 2),
                { u8"a", u8"…" },
                "hidden text after empty last line still marks truncation"
            );
            check_lines(
                wrap_help_text(u8"abc\n", 4, fixed_measure, 1),
                { u8"abc…" },
                "hidden trailing blank line counts toward line limit"
            );
            check_lines(
                wrap_help_text(u8"abcdef", 3, fixed_measure, 2),
                { u8"abc", u8"def" },
                "exact line limit does not add ellipsis"
            );
            check_lines(
                wrap_help_text(u8"ab", 1, fixed_measure, 1), { u8"…" }, "ellipsis can occupy entire final width"
            );
            check_lines(
                wrap_help_text(u8"ab", 0.5f, fixed_measure, 1),
                { u8"" },
                "final line is empty when ellipsis itself cannot fit"
            );
            check_lines(
                wrap_help_text(u8"😀😀", 0.5f, fixed_measure),
                { u8"😀", u8"😀" },
                "narrow positive width still advances complete codepoints"
            );

            const auto wide_measure = [](const std::u8string_view text) { return fixed_measure(text) * 2.0f; };
            check_lines(
                wrap_help_text(u8"가나다라마", 5, wide_measure, 1),
                { u8"가…" },
                "ellipsis uses actual measurement instead of assumed fixed width"
            );
        }

        void test_invalid_inputs()
        {
            check_lines(wrap_help_text(u8"abc", 0, fixed_measure), {}, "zero width returns empty layout");
            check_lines(wrap_help_text(u8"abc", -1, fixed_measure), {}, "negative width returns empty layout");
            check_lines(
                wrap_help_text(u8"abc", std::numeric_limits<float>::infinity(), fixed_measure),
                {},
                "infinite width returns empty layout"
            );
            check_lines(
                wrap_help_text(u8"abc", std::numeric_limits<float>::quiet_NaN(), fixed_measure),
                {},
                "NaN width returns empty layout"
            );
            check_lines(wrap_help_text(u8"abc", 10, {}), {}, "missing measurement returns empty layout");

            const std::u8string invalid {
                u8'a', static_cast<char8_t>(0xf0), static_cast<char8_t>(0x9f), u8'b', static_cast<char8_t>(0xff)
            };

            const auto layout { wrap_help_text(invalid, 2, fixed_measure) };
            check(
                join_lines(layout) == luil::text::utf8_replace_invalid(invalid),
                "invalid UTF-8 is sanitized before wrapping or measuring"
            );
        }

        void test_element()
        {
            const luil::ui_element_id id { luil::application_element_kind(10), u8"help-test" };
            wrapped_help_element element { id, help_text_layout { { u8"가", u8"", u8"나" } }, 12 };
            check(element.height() == 57, "element height counts empty lines and font padding");
            element.arrange({ .slot = { 10, 20, 200, 57 }, .scale = 1 });

            const auto box { element.bounds() };
            check(
                box.x == 10 && box.y == 20 && box.width == 200 && box.height == 57,
                "element uses provided arranged slot"
            );

            const auto access { element.accessibility() };
            check(
                access.role == luil::access_role::static_text && access.name == u8"가\n\n나",
                "default accessibility preserves wrapped text and blank lines"
            );
            wrapped_help_element named {
                id, help_text_layout { { u8"짧게…" } }, 13, luil::label_color_role::primary, u8"원래의 전체 도움말"
            };
            check(
                named.height() == 20 && named.accessibility().name == u8"원래의 전체 도움말",
                "explicit accessibility name retains full unelided text"
            );
            wrapped_help_element leading_blank { id, help_text_layout { { u8"", u8"a" } } };
            check(leading_blank.accessibility().name == u8"\na", "accessibility preserves leading blank line");
            wrapped_help_element empty { id, {} };
            check(empty.height() == 0 && empty.accessibility().name.empty(), "empty layout has zero natural height");
            check(layout_help_text(u8"a", 100, 0).lines.empty(), "invalid font size returns empty measured layout");
        }

        int run_tests()
        {
            test_wrapping();
            test_truncation();
            test_invalid_inputs();
            test_element();
            return failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

    } // namespace
} // namespace chalkag

int main()
{
    try
    {
        const int result { chalkag::run_tests() };

        if (result == EXIT_SUCCESS)
        {
            std::cout << "Help text tests passed.\n";
        }

        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
