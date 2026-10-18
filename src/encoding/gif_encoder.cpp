#include "encoding/gif_encoder.h"

#include <cgif.h>

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace chalkag::encoding {
    namespace {
        using Microsoft::WRL::ComPtr;

        void require(const bool condition, const char* message)
        {
            if (!condition)
            {
                throw std::runtime_error { message };
            }
        }

        struct com_scope
        {
            HRESULT status { CoInitializeEx(nullptr, COINIT_MULTITHREADED) };

            ~com_scope()
            {
                if (SUCCEEDED(status))
                {
                    CoUninitialize();
                }
            }
        };

        ComPtr<IWICBitmap> bitmap_from(IWICImagingFactory* factory, const SkPixmap& pixels)
        {
            ComPtr<IWICBitmap> bitmap;
            require(
                SUCCEEDED(factory->CreateBitmapFromMemory(
                    static_cast<UINT>(pixels.width()),
                    static_cast<UINT>(pixels.height()),
                    GUID_WICPixelFormat32bppBGRA,
                    static_cast<UINT>(pixels.rowBytes()),
                    static_cast<UINT>(pixels.computeByteSize()),
                    const_cast<BYTE*>(static_cast<const BYTE*>(pixels.addr())),
                    &bitmap
                )),
                "Cannot create WIC bitmap"
            );
            return bitmap;
        }

        ComPtr<IWICPalette> global_palette(
            IWICImagingFactory* factory,
            const animation_source& source,
            const std::atomic<bool>& cancelled,
            const progress_callback& progress
        )
        {
            // 모든 프레임을 고르게 표본화하여 팔레트 생성 메모리를 256KiB로 제한한다.
            std::vector<std::uint32_t> samples(65536, 0xff000000u);
            const std::size_t quota { std::max<std::size_t>(1, samples.size() / source.count) };
            std::size_t destination {};
            report_progress(progress, save_stage::palette, 0, source.count);

            for (std::size_t frame_index {}; frame_index < source.count; ++frame_index)
            {
                require(!cancelled.load(), "GIF palette cancelled");

                const auto frame { read_animation_frame(source, frame_index) };
                const auto& pixels { frame.pixels };
                const auto count { static_cast<std::size_t>(pixels.width()) * pixels.height() };

                for (std::size_t index {}; index < quota; ++index)
                {
                    const auto pixel_index { index * count / quota };
                    const auto destination_index { source.count > samples.size()
                                                       ? frame_index * samples.size() / source.count
                                                       : destination++
                    };
                    samples[destination_index] = *pixels.addr32(
                        static_cast<int>(pixel_index % pixels.width()), static_cast<int>(pixel_index / pixels.width())
                    );
                }

                report_progress(progress, save_stage::palette, frame_index + 1, source.count);
            }

            const SkPixmap pixels { SkImageInfo::Make(256, 256, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
                                    samples.data(),
                                    256 * 4
            };

            const auto bitmap { bitmap_from(factory, pixels) };
            ComPtr<IWICPalette> palette;

            // 마지막 인덱스는 변경되지 않은 픽셀의 투명 표시용으로 예약한다.
            require(
                SUCCEEDED(factory->CreatePalette(&palette))
                    && SUCCEEDED(palette->InitializeFromBitmap(bitmap.Get(), 255, FALSE)),
                "Cannot generate GIF palette"
            );
            return palette;
        }

        std::vector<std::uint8_t> quantize(
            IWICImagingFactory* factory, IWICPalette* palette, const SkPixmap& pixels, const bool dither
        )
        {
            const auto bitmap { bitmap_from(factory, pixels) };
            ComPtr<IWICFormatConverter> converter;

            std::vector<std::uint8_t> indices(static_cast<std::size_t>(pixels.width()) * pixels.height());
            require(
                SUCCEEDED(factory->CreateFormatConverter(&converter))
                    && SUCCEEDED(converter->Initialize(
                        bitmap.Get(),
                        GUID_WICPixelFormat8bppIndexed,
                        dither ? WICBitmapDitherTypeErrorDiffusion : WICBitmapDitherTypeNone,
                        palette,
                        0,
                        WICBitmapPaletteTypeCustom
                    ))
                    && SUCCEEDED(converter->CopyPixels(
                        nullptr, static_cast<UINT>(pixels.width()), static_cast<UINT>(indices.size()), indices.data()
                    )),
                "Cannot quantize GIF frame"
            );
            return indices;
        }

        struct cgif_output
        {
            std::ofstream stream;
            const std::atomic<bool>& cancelled;
        };

        int write_cgif(void* context, const std::uint8_t* bytes, const std::size_t size)
        {
            auto& output { *static_cast<cgif_output*>(context) };

            if (output.cancelled.load())
            {
                return CGIF_EWRITE;
            }

            output.stream.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
            return output.stream ? CGIF_OK : CGIF_EWRITE;
        }

        void close_cgif(CGIF* encoder)
        {
            static_cast<void>(cgif_close(encoder));
        }

        void encode_cgif(
            const std::filesystem::path& path,
            IWICImagingFactory* factory,
            IWICPalette* palette,
            const std::span<const WICColor> colors,
            const animation_source& source,
            const encoder_options& options,
            const std::atomic<bool>& cancelled
        )
        {
            // 동일한 WIC 팔레트와 색 인덱스를 사용하여 양자화 품질 차이를 제거한다.
            const std::size_t entries { options.gif_encoder == gif_backend::cgif_fixed_palette ? 255 : colors.size() };
            std::vector<std::uint8_t> rgb(entries * 3, 0);

            for (std::size_t index {}; index < colors.size(); ++index)
            {
                rgb[index * 3] = static_cast<std::uint8_t>(colors[index] >> 16);
                rgb[index * 3 + 1] = static_cast<std::uint8_t>(colors[index] >> 8);
                rgb[index * 3 + 2] = static_cast<std::uint8_t>(colors[index]);
            }

            cgif_output output { std::ofstream { path, std::ios::binary | std::ios::trunc }, cancelled };
            require(static_cast<bool>(output.stream), "Cannot open CGIF candidate");
            CGIF_Config config {};
            config.pGlobalPalette = rgb.data();
            config.attrFlags = CGIF_ATTR_IS_ANIMATED;
            config.width = static_cast<std::uint16_t>(read_animation_frame(source, 0).pixels.width());
            config.height = static_cast<std::uint16_t>(read_animation_frame(source, 0).pixels.height());
            config.numGlobalPaletteEntries = static_cast<std::uint16_t>(entries);
            config.numLoops
                = options.play_count == 0 ? CGIF_INFINITE_LOOP : static_cast<std::uint16_t>(options.play_count - 1);

            if (options.play_count == 1)
            {
                config.attrFlags |= CGIF_ATTR_NO_LOOP;
            }

            config.pWriteFn = write_cgif;
            config.pContext = &output;

            std::unique_ptr<CGIF, decltype(&close_cgif)> encoder { cgif_newgif(&config), close_cgif };
            require(encoder != nullptr, "Cannot create CGIF encoder");
            double timeline {};

            std::uint64_t centiseconds {};
            report_progress(options.progress, save_stage::encoding, 0, source.count);

            for (std::size_t frame_index {}; frame_index < source.count; ++frame_index)
            {
                require(!cancelled.load(), "CGIF encoding cancelled");

                const auto frame { read_animation_frame(source, frame_index) };
                require(
                    frame.pixels.width() == config.width && frame.pixels.height() == config.height,
                    "GIF frame dimensions differ"
                );

                auto indices { quantize(factory, palette, frame.pixels, options.gif_dither) };
                timeline += frame.duration_seconds;

                const auto end { static_cast<std::uint64_t>(std::llround(timeline * 100)) };
                const auto delay { std::max<std::uint64_t>(2, end > centiseconds ? end - centiseconds : 0) };
                centiseconds += delay;
                CGIF_FrameConfig settings {};
                settings.pImageData = indices.data();
                settings.delay = static_cast<std::uint16_t>(delay);
                settings.genFlags = CGIF_FRAME_GEN_USE_TRANSPARENCY | CGIF_FRAME_GEN_USE_DIFF_WINDOW;
                require(cgif_addframe(encoder.get(), &settings) == CGIF_OK, "CGIF cannot add frame");
                report_progress(options.progress, save_stage::encoding, frame_index + 1, source.count);
            }

            report_progress(options.progress, save_stage::assembling);
            require(cgif_close(encoder.release()) == CGIF_OK, "CGIF cannot finish file");
            output.stream.close();
            require(static_cast<bool>(output.stream), "Cannot close CGIF candidate");
        }

    } // namespace

    bool encode_gif(
        const std::filesystem::path& output,
        const animation_source& source,
        const encoder_options& options,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    )
    {
        try
        {
            const com_scope scope {};
            ComPtr<IWICImagingFactory> factory;
            require(
                (SUCCEEDED(scope.status) || scope.status == RPC_E_CHANGED_MODE)
                    && SUCCEEDED(
                        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))
                    ),
                "Cannot initialize WIC"
            );

            const auto palette { global_palette(factory.Get(), source, cancelled, options.progress) };
            std::array<WICColor, 256> colors {};
            UINT count {};
            require(SUCCEEDED(palette->GetColors(255, colors.data(), &count)), "Cannot read palette");
            require(count > 0 && count <= 255, "Invalid WIC palette size");
            encode_cgif(output, factory.Get(), palette.Get(), { colors.data(), count }, source, options, cancelled);
            return true;
        }
        catch (const std::exception& failure)
        {
            const std::string detail { failure.what() };
            error = cancelled.load() ? u8"GIF 저장을 취소했습니다."
                                     : u8"정적 GIF 인코더 오류: " + std::u8string { detail.begin(), detail.end() };
            return false;
        }
    }

} // namespace chalkag::encoding
