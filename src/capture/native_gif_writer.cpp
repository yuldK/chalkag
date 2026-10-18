#include "capture/native_gif_writer.h"

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace chalkag {
    namespace {
        using Microsoft::WRL::ComPtr;

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

        bool loop_forever(IWICBitmapEncoder* encoder)
        {
            ComPtr<IWICMetadataQueryWriter> metadata;

            if (FAILED(encoder->GetMetadataQueryWriter(&metadata)))
            {
                return false;
            }

            std::array<unsigned char, 11> application { 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0' };
            std::array<unsigned char, 4> data { 3, 1, 0, 0 };
            PROPVARIANT value {};
            value.vt = VT_VECTOR | VT_UI1;
            value.caub = { static_cast<ULONG>(application.size()), application.data() };

            if (FAILED(metadata->SetMetadataByName(L"/appext/application", &value)))
            {
                return false;
            }

            value.caub = { static_cast<ULONG>(data.size()), data.data() };
            return SUCCEEDED(metadata->SetMetadataByName(L"/appext/data", &value));
        }

        bool encode_frame(
            IWICImagingFactory* factory,
            IWICBitmapEncoder* encoder,
            const encoded_animation_frame& source,
            const USHORT delay,
            UINT& canvas_width,
            UINT& canvas_height
        )
        {
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> bitmap;
            UINT width {};
            UINT height {};

            if (FAILED(factory->CreateDecoderFromFilename(
                    source.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder
                ))
                || FAILED(decoder->GetFrame(0, &bitmap)) || FAILED(bitmap->GetSize(&width, &height)) || source.x != 0
                || source.y != 0 || width == 0 || height == 0 || width > 65535 || height > 65535)
            {
                return false;
            }

            if (canvas_width == 0)
            {
                canvas_width = width;
                canvas_height = height;
            }

            if (canvas_width != width || canvas_height != height)
            {
                return false;
            }

            ComPtr<IWICPalette> palette;
            ComPtr<IWICFormatConverter> converter;

            if (FAILED(factory->CreatePalette(&palette))
                || FAILED(palette->InitializeFromBitmap(bitmap.Get(), 256, FALSE))
                || FAILED(factory->CreateFormatConverter(&converter))
                || FAILED(converter->Initialize(
                    bitmap.Get(),
                    GUID_WICPixelFormat8bppIndexed,
                    WICBitmapDitherTypeErrorDiffusion,
                    palette.Get(),
                    0,
                    WICBitmapPaletteTypeCustom
                )))
            {
                return false;
            }

            ComPtr<IWICBitmapFrameEncode> frame;
            WICPixelFormatGUID format { GUID_WICPixelFormat8bppIndexed };

            if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))
                || FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format))
                || format != GUID_WICPixelFormat8bppIndexed || FAILED(frame->SetPalette(palette.Get())))
            {
                return false;
            }

            ComPtr<IWICMetadataQueryWriter> metadata;

            if (FAILED(frame->GetMetadataQueryWriter(&metadata)))
            {
                return false;
            }

            PROPVARIANT value {};
            value.vt = VT_UI2;
            value.uiVal = delay;

            if (FAILED(metadata->SetMetadataByName(L"/grctlext/Delay", &value)))
            {
                return false;
            }

            value.vt = VT_UI1;
            value.bVal = 1;
            return SUCCEEDED(metadata->SetMetadataByName(L"/grctlext/Disposal", &value))
                && SUCCEEDED(frame->WriteSource(converter.Get(), nullptr)) && SUCCEEDED(frame->Commit());
        }

    } // namespace

    bool write_gif(
        const std::filesystem::path& output,
        const std::span<const encoded_animation_frame> frames,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    )
    {
        error.clear();

        const com_scope scope {};
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;

        if (frames.empty() || (FAILED(scope.status) && scope.status != RPC_E_CHANGED_MODE)
            || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
            || FAILED(factory->CreateStream(&stream))
            || FAILED(stream->InitializeFromFilename(output.c_str(), GENERIC_WRITE))
            || FAILED(factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder))
            || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        {
            error = u8"Windows GIF 인코더를 준비할 수 없습니다.";
            return false;
        }

        if (!loop_forever(encoder.Get()))
        {
            error = u8"GIF 반복 재생 정보를 준비할 수 없습니다.";
            return false;
        }

        double timeline {};

        std::uint64_t centiseconds {};
        UINT width {};
        UINT height {};

        for (const auto& frame : frames)
        {
            if (cancelled.load())
            {
                error = u8"GIF 저장을 취소했습니다.";
                return false;
            }

            if (!std::isfinite(frame.duration_seconds) || frame.duration_seconds <= 0 || frame.duration_seconds > 600)
            {
                error = u8"GIF 프레임 시간이 올바르지 않습니다.";
                return false;
            }

            timeline += frame.duration_seconds;

            const auto end { static_cast<std::uint64_t>(std::llround(timeline * 100)) };

            // 1센티초 지연을 느리게 해석하는 재생기를 피하고 누적 반올림 오차를 보정한다.
            const auto duration { std::max<std::uint64_t>(2, end > centiseconds ? end - centiseconds : 0) };

            if (duration > std::numeric_limits<USHORT>::max()
                || !encode_frame(factory.Get(), encoder.Get(), frame, static_cast<USHORT>(duration), width, height))
            {
                error = u8"Windows GIF 프레임을 저장할 수 없습니다.";
                return false;
            }

            centiseconds += duration;
        }

        if (FAILED(encoder->Commit()))
        {
            error = u8"GIF 반복 재생 정보를 저장할 수 없습니다.";
            return false;
        }

        return true;
    }

} // namespace chalkag
