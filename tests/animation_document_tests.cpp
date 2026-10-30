#include "capture/animation_document.h"
#include "capture/animation_recorder.h"
#include "capture/native_webp_writer.h"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
    void require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error { message };
        }
    }

    SkBitmap bitmap(const std::uint32_t color, const SkAlphaType alpha = kOpaque_SkAlphaType)
    {
        SkBitmap result {};
        require(result.tryAllocPixels(SkImageInfo::Make(16, 8, kBGRA_8888_SkColorType, alpha)), "allocate");

        for (int y {}; y < 8; ++y)
        {
            for (int x {}; x < 16; ++x)
            {
                *result.getAddr32(x, y) = color;
            }
        }

        return result;
    }

} // namespace

int main(const int argc, char** argv)
{
    if (argc != 2)
    {
        return 1;
    }

    try
    {
        const std::filesystem::path directory { argv[1] };
        std::filesystem::create_directories(directory);
        std::filesystem::create_directories(directory / "tiles");
        const std::atomic<bool> cancelled {};
        std::u8string error {};
        auto canvas { bitmap(0xff112233) };
        chalkag::animation_recorder recorder { directory / "tiles", chalkag::image_format::apng };
        require(recorder.append(canvas.pixmap(), 0), "first capture");
        *canvas.getAddr32(2, 2) = 0xffaabbcc;
        require(recorder.append(canvas.pixmap(), 0.1), "delta capture");
        *canvas.getAddr32(5, 2) = 0xff556677;
        require(recorder.append(canvas.pixmap(), 0.3), "last capture");

        auto document { chalkag::animation_document::from_recording({ recorder.finish(0.6), { 0, 0, 16, 8 }, {} }) };
        require(document != nullptr, "retained document");

        auto edit { chalkag::initial_animation_edit(*document) };
        require(chalkag::animation_duration(edit) == 600, "original timing");

        const auto reduced { chalkag::reduce_animation(edit, 2, 1, true) };
        require(
            reduced.frames.size() == 1 && reduced.frames[0].source == 1 && reduced.frames[0].milliseconds == 600
                && chalkag::animation_duration(reduced) == 600,
            "even frames exclude odd tail and retain its duration"
        );

        const auto twice { chalkag::reduce_animation(reduced, 2, 0, true) };
        require(
            twice.frames.size() == 1 && twice.frames[0].milliseconds == 600, "repeated reduction uses edited order"
        );
        require(chalkag::animation_duration(chalkag::reduce_animation(edit, 2, 0, false)) == 400, "shorter reduction");
        edit.frames.erase(edit.frames.begin() + 1);
        edit.frames.erase(edit.frames.begin());
        edit.crop = { 2, 1, 8, 4 };
        edit.output_width = 8;
        edit.output_height = 4;
        chalkag::edited_animation_reader reader { document, edit, false, cancelled };

        auto source { reader.source() };
        const auto last { chalkag::encoding::read_animation_frame(source, 0) };
        require(
            *last.pixels.addr32(0, 1) == 0xffaabbcc && *last.pixels.addr32(3, 1) == 0xff556677,
            "deleted delta still participates in reconstruction and crop"
        );

        auto invalid { edit };
        invalid.crop.x = 15;
        require(!chalkag::valid_animation_edit(invalid, 16, 8), "crop bounds");
        invalid = edit;
        invalid.output_width = 16384;
        require(!chalkag::valid_animation_edit(invalid, 16, 8), "output bounds");
        chalkag::edited_animation_reader invalid_reader { document, invalid, false, cancelled };
        chalkag::encoding::animation_frame invalid_frame {};
        require(!invalid_reader.source().read(0, invalid_frame), "invalid export is rejected before decoding");

        auto transparent { bitmap(0x80402010, kUnpremul_SkAlphaType) };
        auto clear { bitmap(0x00000000, kUnpremul_SkAlphaType) };
        *clear.getAddr32(4, 3) = 0xff123456;

        const std::array frames { chalkag::encoding::animation_frame { transparent.pixmap(), 0.04 },
                                  chalkag::encoding::animation_frame { clear.pixmap(), 0.12 }
        };

        for (const auto format :
             { chalkag::encoding::animation_format::webp, chalkag::encoding::animation_format::apng })
        {
            const auto path { directory
                              / (format == chalkag::encoding::animation_format::webp ? "alpha.webp" : "alpha.apng")
            };
            chalkag::encoding::encoder_options options { .format = format, .compact = false, .play_count = 3 };

            if (!chalkag::encoding::encode_animation(path, frames, options, cancelled, error))
            {
                throw std::runtime_error { std::string { error.begin(), error.end() } };
            }

            auto imported { chalkag::animation_document::open(path, error) };
            require(
                imported && imported->durations().size() == 2 && imported->durations()[0] == 40
                    && imported->durations()[1] == 120 && imported->play_count() == 3,
                "import timing and loop count"
            );

            SkBitmap decoded {};
            require(imported->read(0, decoded, cancelled), "decode alpha frame");
            require((*decoded.getAddr32(0, 0) >> 24) == 128, "partial alpha");
            require(
                imported->read(1, decoded, cancelled) && (*decoded.getAddr32(0, 0) >> 24) == 0
                    && *decoded.getAddr32(4, 3) == 0xff123456,
                "transparent replacement and source pixels"
            );

            const auto changed { chalkag::initial_animation_edit(*imported) };
            chalkag::edited_animation_reader gif_reader { imported, changed, true, cancelled };

            const auto gif_path { directory / "flattened.gif" };
            options.format = chalkag::encoding::animation_format::gif;
            options.play_count = 1;
            require(
                chalkag::encoding::encode_animation(gif_path, gif_reader.source(), options, cancelled, error),
                "GIF matte export"
            );

            auto gif { chalkag::animation_document::open(gif_path, error) };
            require(
                gif && gif->play_count() == 1 && gif->read(1, decoded, cancelled)
                    && *decoded.getAddr32(0, 0) == 0xffffffff,
                "GIF finite playback and white matte"
            );
        }

        const std::array long_frames { chalkag::encoding::animation_frame { transparent.pixmap(), 65.537 },
                                       chalkag::encoding::animation_frame { clear.pixmap(), 0.12 }
        };
        chalkag::encoding::encoder_options long_options { .format = chalkag::encoding::animation_format::apng,
                                                          .compact = false,
                                                          .play_count = 3
        };

        const auto long_path { directory / "long-alpha.apng" };
        require(
            chalkag::encoding::encode_animation(long_path, long_frames, long_options, cancelled, error),
            "long transparent APNG delay exports without losing milliseconds"
        );

        auto long_document { chalkag::animation_document::open(long_path, error) };
        require(
            long_document && long_document->durations() == std::vector<int> { 60000, 5537, 120 }
                && long_document->play_count() == 3,
            "split APNG delay and loop count are preserved"
        );

        SkBitmap long_decoded {};
        require(
            long_document->read(1, long_decoded, cancelled) && (*long_decoded.getAddr32(0, 0) >> 24) == 128,
            "split frame preserves partial alpha"
        );
        require(
            long_document->read(2, long_decoded, cancelled) && (*long_decoded.getAddr32(0, 0) >> 24) == 0,
            "frame after long delay replaces transparency"
        );

        const std::array fractional_frames { chalkag::encoding::animation_frame { transparent.pixmap(), 60.0001 },
                                             chalkag::encoding::animation_frame { clear.pixmap(), 0.12 }
        };

        const auto fractional_path { directory / "fractional-alpha.apng" };
        require(
            chalkag::encoding::encode_animation(fractional_path, fractional_frames, long_options, cancelled, error),
            "submillisecond tail does not become a zero-delay split frame"
        );

        auto fractional_document { chalkag::animation_document::open(fractional_path, error) };
        require(
            fractional_document && fractional_document->durations() == std::vector<int> { 60000, 120 },
            "representable delay retains millisecond rounding"
        );
        chalkag::encoding::encoder_options lossy { .lossless = false, .compact = false, .play_count = 2 };

        const auto lossy_path { directory / "lossy-alpha.webp" };
        require(chalkag::encoding::encode_animation(lossy_path, frames, lossy, cancelled, error), "lossy alpha chunks");

        auto imported { chalkag::animation_document::open(lossy_path, error) };
        SkBitmap decoded {};
        require(
            imported && imported->play_count() == 2 && imported->read(0, decoded, cancelled)
                && (*decoded.getAddr32(0, 0) >> 24) == 128,
            "lossy alpha preserved"
        );

        // 투명한 검정과 불투명한 흰색 체크를 줄이면 회색이 아니라 반투명한 흰색이어야 한다.
        SkBitmap checker {};
        require(
            checker.tryAllocPixels(SkImageInfo::Make(2, 2, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType)),
            "allocate checker"
        );
        *checker.getAddr32(0, 0) = 0x00000000;
        *checker.getAddr32(1, 1) = 0x00000000;
        *checker.getAddr32(1, 0) = 0xffffffff;
        *checker.getAddr32(0, 1) = 0xffffffff;

        for (const auto color_type : { kBGRA_8888_SkColorType, kRGBA_8888_SkColorType })
        {
            SkBitmap single {};
            require(
                single.tryAllocPixels(SkImageInfo::Make(1, 1, color_type, kUnpremul_SkAlphaType)), "allocate scaled"
            );
            require(chalkag::scale_pixels_premultiplied(checker.pixmap(), single.pixmap()), "premultiplied scale");

            const auto* pixel { static_cast<const std::uint8_t*>(single.getPixels()) };
            std::cout << "checker 2x2 -> 1x1: " << int { pixel[0] } << ',' << int { pixel[1] } << ','
                      << int { pixel[2] } << ',' << int { pixel[3] } << '\n';
            require(
                pixel[0] >= 253 && pixel[1] >= 253 && pixel[2] >= 253 && pixel[3] >= 118 && pixel[3] <= 138,
                "transparent neighbours do not darken scaled edges"
            );
        }

        const std::atomic<bool> stopped { true };
        require(!imported->read(0, decoded, stopped), "cancel decode");
        require(!chalkag::animation_document::open(directory / "missing.webp", error), "invalid import");

        std::cout << "Animation document checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
