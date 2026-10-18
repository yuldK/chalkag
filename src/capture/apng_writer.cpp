#include "capture/apng_writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace chalkag {
    namespace {
        constexpr std::array<std::uint8_t, 8> png_signature { 137, 80, 78, 71, 13, 10, 26, 10 };
        constexpr std::uintmax_t maximum_png_bytes { 256ULL * 1024 * 1024 };
        constexpr auto crc_table = [] {
            std::array<std::uint32_t, 256> table {};

            for (std::uint32_t index {}; index < table.size(); ++index)
            {
                std::uint32_t value { index };

                for (int bit {}; bit < 8; ++bit)
                {
                    value = (value >> 1) ^ ((value & 1) != 0 ? 0xedb88320U : 0U);
                }

                table[index] = value;
            }

            return table;
        }();

        struct writer_error
        {
            std::u8string message;
        };

        void require(const bool condition, const char8_t* message)
        {
            if (!condition)
            {
                throw writer_error { message };
            }
        }

        std::uint32_t update_crc(std::uint32_t crc, const std::span<const std::uint8_t> bytes) noexcept
        {
            for (const std::uint8_t byte : bytes)
            {
                crc = crc_table[(crc ^ byte) & 0xff] ^ (crc >> 8);
            }

            return crc;
        }

        std::uint32_t read_u32(const std::span<const std::uint8_t> bytes) noexcept
        {
            return (std::uint32_t { bytes[0] } << 24) | (std::uint32_t { bytes[1] } << 16)
                | (std::uint32_t { bytes[2] } << 8) | bytes[3];
        }

        void put_u32(const std::span<std::uint8_t> bytes, const std::uint32_t value) noexcept
        {
            for (std::size_t index {}; index < 4; ++index)
            {
                bytes[index] = static_cast<std::uint8_t>(value >> (24 - index * 8));
            }
        }

        void read_exact(std::ifstream& input, const std::span<std::uint8_t> bytes)
        {
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            require(input.good(), u8"PNG 프레임이 잘렸거나 읽을 수 없습니다.");
        }

        void write_bytes(std::ofstream& output, const std::span<const std::uint8_t> bytes)
        {
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            require(output.good(), u8"APNG 파일을 기록할 수 없습니다.");
        }

        void write_chunk(
            std::ofstream& output,
            const std::string_view type,
            const std::span<const std::uint8_t> bytes,
            const std::span<const std::uint8_t> prefix = {}
        )
        {
            std::array<std::uint8_t, 4> number {};
            put_u32(number, static_cast<std::uint32_t>(prefix.size() + bytes.size()));
            write_bytes(output, number);

            const std::span<const std::uint8_t> type_bytes { reinterpret_cast<const std::uint8_t*>(type.data()), 4 };
            write_bytes(output, type_bytes);
            write_bytes(output, prefix);
            write_bytes(output, bytes);

            const std::uint32_t crc { update_crc(update_crc(update_crc(0xffffffffU, type_bytes), prefix), bytes)
                                      ^ 0xffffffffU
            };
            put_u32(number, crc);
            write_bytes(output, number);
        }

        std::uint32_t next_sequence(std::uint64_t& sequence)
        {
            require(
                sequence <= std::numeric_limits<std::uint32_t>::max(), u8"APNG 청크 수가 형식의 한도를 넘었습니다."
            );
            return static_cast<std::uint32_t>(sequence++);
        }

        std::array<std::uint8_t, 4> frame_delay(const std::uint32_t milliseconds)
        {
            require(milliseconds > 0, u8"APNG 프레임 지연은 밀리초 정밀도에서 양수여야 합니다.");

            const std::uint32_t divisor { std::gcd(milliseconds, 1000U) };
            const std::uint32_t numerator { milliseconds / divisor };
            const std::uint32_t denominator { 1000U / divisor };
            require(
                numerator <= 65535 && denominator <= 65535,
                u8"이 프레임 지연은 APNG의 16비트 분수로 표현할 수 없습니다. 녹화를 짧은 프레임으로 나누세요."
            );
            return { static_cast<std::uint8_t>(numerator >> 8),
                     static_cast<std::uint8_t>(numerator),
                     static_cast<std::uint8_t>(denominator >> 8),
                     static_cast<std::uint8_t>(denominator)
            };
        }

        void write_frame_control(
            std::ofstream& output,
            std::uint64_t& sequence,
            const std::array<std::uint8_t, 13>& header,
            const encoded_animation_frame& frame,
            const std::uint32_t milliseconds
        )
        {
            // W3C PNG 3판: 변경 영역 SOURCE 합성, NONE disposal, 지연은 초 단위 분수다.
            // https://www.w3.org/TR/2025/REC-png-3-20250624/#fcTL-chunk
            std::array<std::uint8_t, 26> control {};
            put_u32(control, next_sequence(sequence));

            std::copy_n(header.begin(), 8, control.begin() + 4);
            put_u32(std::span<std::uint8_t> { control }.subspan(12), static_cast<std::uint32_t>(frame.x));
            put_u32(std::span<std::uint8_t> { control }.subspan(16), static_cast<std::uint32_t>(frame.y));

            const auto delay { frame_delay(milliseconds) };
            std::copy(delay.begin(), delay.end(), control.begin() + 20);
            write_chunk(output, "fcTL", control);
        }

        void write_frame(
            std::ofstream& output,
            const encoded_animation_frame& frame,
            const std::uint32_t milliseconds,
            const bool first,
            const std::uint32_t frame_count,
            const std::uint32_t play_count,
            std::uint64_t& sequence,
            std::array<std::uint8_t, 13>& common_header,
            std::optional<std::uint8_t>& common_srgb,
            std::optional<std::uint32_t>& common_gamma,
            const std::atomic<bool>& cancelled
        )
        {
            require(
                std::filesystem::file_size(frame.path) <= maximum_png_bytes,
                u8"PNG 프레임이 256 MiB 입력 한도를 넘었습니다."
            );

            std::ifstream input { frame.path, std::ios::binary };
            require(input.is_open(), u8"PNG 프레임을 열 수 없습니다.");

            std::array<std::uint8_t, 8> signature {};
            read_exact(input, signature);
            require(signature == png_signature, u8"프레임이 PNG 형식이 아닙니다.");

            bool header_seen {};

            std::array<std::uint8_t, 13> frame_header {};
            bool image_seen {};

            std::uint64_t image_bytes {};
            std::optional<std::uint8_t> srgb {};
            std::optional<std::uint32_t> gamma {};
            std::array<std::uint8_t, 65536> buffer {};

            while (true)
            {
                require(!cancelled.load(), u8"APNG 저장을 취소했습니다.");

                std::array<std::uint8_t, 8> chunk_header {};
                read_exact(input, chunk_header);

                const std::uint32_t length { read_u32(chunk_header) };
                require(length <= maximum_png_bytes, u8"PNG 청크 길이가 입력 한도를 넘었습니다.");

                const std::span<const std::uint8_t> type_bytes { chunk_header.data() + 4, 4 };
                const std::string_view type { reinterpret_cast<const char*>(type_bytes.data()), 4 };
                require(header_seen || type == "IHDR", u8"PNG의 첫 청크가 IHDR이 아닙니다.");

                std::uint32_t crc { update_crc(0xffffffffU, type_bytes) };

                if (type == "IHDR")
                {
                    require(!header_seen && length == 13, u8"PNG IHDR이 중복되었거나 길이가 잘못되었습니다.");

                    std::array<std::uint8_t, 13> header {};
                    read_exact(input, header);
                    crc = update_crc(crc, header);

                    const std::uint32_t width { read_u32(header) };
                    const std::uint32_t height { read_u32(std::span<const std::uint8_t> { header }.subspan(4)) };
                    require(
                        width > 0 && height > 0 && width <= 0x7fffffffU && height <= 0x7fffffffU && header[8] == 8
                            && (header[9] == 2 || header[9] == 6) && header[10] == 0 && header[11] == 0
                            && header[12] == 0,
                        u8"APNG 입력은 인터레이스 없는 RGB8 또는 RGBA8 PNG여야 합니다."
                    );
                    require(frame.x >= 0 && frame.y >= 0, u8"APNG 프레임 좌표는 음수일 수 없습니다.");
                    require(
                        !first || (frame.x == 0 && frame.y == 0), u8"APNG 첫 프레임은 캔버스 원점에서 시작해야 합니다."
                    );
                    require(
                        first || std::equal(header.begin() + 8, header.end(), common_header.begin() + 8),
                        u8"PNG 프레임의 픽셀 형식이 서로 다릅니다."
                    );

                    if (first)
                    {
                        common_header = header;
                        write_bytes(output, png_signature);
                        write_chunk(output, "IHDR", header);

                        std::array<std::uint8_t, 8> animation_control {};
                        put_u32(animation_control, frame_count);
                        put_u32(std::span<std::uint8_t> { animation_control }.subspan(4), play_count);
                        write_chunk(output, "acTL", animation_control);
                    }

                    const std::uint32_t canvas_width { read_u32(common_header) };
                    const std::uint32_t canvas_height {
                        read_u32(std::span<const std::uint8_t> { common_header }.subspan(4))
                    };
                    const auto x { static_cast<std::uint32_t>(frame.x) };
                    const auto y { static_cast<std::uint32_t>(frame.y) };
                    require(
                        x < canvas_width && y < canvas_height && width <= canvas_width - x
                            && height <= canvas_height - y,
                        u8"APNG 프레임이 캔버스 경계를 벗어났습니다."
                    );
                    frame_header = header;
                    header_seen = true;
                }
                else if (type == "sRGB")
                {
                    require(
                        !image_seen && !srgb.has_value() && length == 1, u8"PNG sRGB 청크 위치나 길이가 잘못되었습니다."
                    );
                    read_exact(input, std::span<std::uint8_t> { buffer }.first(1));
                    require(buffer[0] <= 3, u8"PNG sRGB 렌더링 의도가 잘못되었습니다.");
                    srgb = buffer[0];
                    crc = update_crc(crc, std::span<const std::uint8_t> { buffer }.first(1));

                    if (first)
                    {
                        write_chunk(output, "sRGB", std::span<const std::uint8_t> { buffer }.first(1));
                    }
                }
                else if (type == "gAMA")
                {
                    require(
                        !image_seen && !gamma.has_value() && length == 4,
                        u8"PNG gAMA 청크 위치나 길이가 잘못되었습니다."
                    );

                    const auto data { std::span<std::uint8_t> { buffer }.first(4) };
                    read_exact(input, data);
                    gamma = read_u32(data);
                    require(*gamma > 0, u8"PNG 감마 값은 양수여야 합니다.");
                    crc = update_crc(crc, data);

                    if (first)
                    {
                        write_chunk(output, "gAMA", data);
                    }
                }
                else if (type == "IDAT")
                {
                    if (!image_seen)
                    {
                        if (first)
                        {
                            common_srgb = srgb;
                            common_gamma = gamma;
                        }

                        require(
                            srgb == common_srgb && gamma == common_gamma, u8"PNG 프레임의 색 공간 정보가 서로 다릅니다."
                        );
                        write_frame_control(output, sequence, frame_header, frame, milliseconds);
                    }

                    image_seen = true;
                    image_bytes += length;

                    std::uint32_t remaining { length };
                    do
                    {
                        require(!cancelled.load(), u8"APNG 저장을 취소했습니다.");

                        const auto count { static_cast<std::size_t>(
                            std::min<std::uint32_t>(remaining, static_cast<std::uint32_t>(buffer.size()))
                        )
                        };
                        const auto data { std::span<std::uint8_t> { buffer }.first(count) };

                        if (count != 0)
                        {
                            read_exact(input, data);
                        }

                        crc = update_crc(crc, data);

                        if (first)
                        {
                            write_chunk(output, "IDAT", data);
                        }
                        else
                        {
                            std::array<std::uint8_t, 4> frame_sequence {};
                            put_u32(frame_sequence, next_sequence(sequence));
                            write_chunk(output, "fdAT", data, frame_sequence);
                        }

                        remaining -= static_cast<std::uint32_t>(count);
                    } while (remaining != 0);
                }
                else if (type == "IEND")
                {
                    require(
                        image_seen && image_bytes > 0 && length == 0,
                        u8"PNG에 이미지 데이터가 없거나 IEND가 잘못되었습니다."
                    );
                }
                else
                {
                    throw writer_error { u8"APNG 입력에 지원하지 않는 PNG 메타데이터 청크가 있습니다." };
                }

                std::array<std::uint8_t, 4> stored_crc {};
                read_exact(input, stored_crc);
                require(read_u32(stored_crc) == (crc ^ 0xffffffffU), u8"PNG 프레임의 청크 CRC가 일치하지 않습니다.");

                if (type == "IEND")
                {
                    require(
                        input.peek() == std::char_traits<char>::eof(), u8"PNG IEND 뒤에 불필요한 데이터가 있습니다."
                    );
                    return;
                }
            }
        }

    } // namespace

    bool write_apng(
        const std::filesystem::path& output_path,
        const std::span<const encoded_animation_frame> frames,
        const std::atomic<bool>& cancelled,
        std::u8string& error,
        const progress_callback& progress,
        const std::uint32_t play_count
    )
    {
        error.clear();
        bool output_created {};

        try
        {
            require(
                !frames.empty() && frames.size() <= std::numeric_limits<std::uint32_t>::max(),
                u8"APNG 저장에 유효한 프레임 목록이 필요합니다."
            );
            require(!cancelled.load(), u8"APNG 저장을 취소했습니다.");
            require(!std::filesystem::exists(output_path), u8"APNG 출력 파일이 이미 존재합니다.");

            std::ofstream output { output_path, std::ios::binary };
            require(output.is_open(), u8"APNG 출력 파일을 만들 수 없습니다.");
            output_created = true;

            std::uint64_t sequence {};
            std::array<std::uint8_t, 13> header {};
            std::optional<std::uint8_t> srgb {};
            std::optional<std::uint32_t> gamma {};
            long double accumulated_milliseconds {};

            std::uint64_t previous_boundary {};
            report_progress(progress, save_stage::assembling, 0, frames.size());

            for (std::size_t index {}; index < frames.size(); ++index)
            {
                const double seconds { frames[index].duration_seconds };
                require(
                    std::isfinite(seconds) && seconds > 0.0 && seconds <= 65535.0,
                    u8"APNG 프레임 지연은 0초 초과, 65535초 이하여야 합니다."
                );

                // 장마다 반올림하면 16 FPS가 63 ms씩 누적되므로 실제 경과 시간의 경계를 반올림한다.
                accumulated_milliseconds += static_cast<long double>(seconds) * 1000.0L;

                const auto boundary { static_cast<std::uint64_t>(std::llround(accumulated_milliseconds)) };
                const auto milliseconds { static_cast<std::uint32_t>(boundary - previous_boundary) };
                previous_boundary = boundary;
                write_frame(
                    output,
                    frames[index],
                    milliseconds,
                    index == 0,
                    static_cast<std::uint32_t>(frames.size()),
                    play_count,
                    sequence,
                    header,
                    srgb,
                    gamma,
                    cancelled
                );
                report_progress(progress, save_stage::assembling, index + 1, frames.size());
            }

            require(!cancelled.load(), u8"APNG 저장을 취소했습니다.");
            write_chunk(output, "IEND", {});
            output.flush();
            require(output.good(), u8"APNG 파일 기록을 마칠 수 없습니다.");
            output.close();
            require(!output.fail(), u8"APNG 파일을 닫을 수 없습니다.");
            return true;
        }
        catch (const writer_error& failure)
        {
            error = failure.message;
        }
        catch (const std::exception&)
        {
            error = u8"APNG 저장 중 파일 접근이나 메모리 오류가 발생했습니다.";
        }

        if (output_created)
        {
            std::error_code ignored {};
            std::filesystem::remove(output_path, ignored);
        }

        return false;
    }

} // namespace chalkag
