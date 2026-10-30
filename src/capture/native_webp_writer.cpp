#include "capture/native_webp_writer.h"
#include "encoding/webp_config.h"

#include "include/codec/SkCodec.h"
#include "include/codec/SkEncodedImageFormat.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkWebpEncoder.h"
#include <webp/encode.h>

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace chalkag {
    namespace {
        constexpr std::uintmax_t maximum_webp_bytes { 256ULL * 1024 * 1024 };
        constexpr std::uint64_t maximum_pixels { 33554432 };

        struct optimization_error
        {
            std::u8string message;
        };

        void require(const bool condition, const char8_t* message)
        {
            if (!condition)
            {
                throw optimization_error { message };
            }
        }

        void check_cancelled(const std::atomic<bool>& cancelled)
        {
            require(!cancelled.load(), u8"WebP 파일 최적화를 취소했습니다.");
        }

        struct temporary_file
        {
            std::filesystem::path path;

            ~temporary_file()
            {
                if (!path.empty())
                {
                    std::error_code ignored {};
                    std::filesystem::remove(path, ignored);
                }
            }
        };

        void create_temporary(const std::filesystem::path& original, temporary_file& temporary)
        {
            static std::atomic<std::uint64_t> sequence {};

            for (int attempt {}; attempt < 32; ++attempt)
            {
                auto candidate { original };
                candidate += L".opt-" + std::to_wstring(GetCurrentProcessId()) + L"-"
                    + std::to_wstring(sequence.fetch_add(1)) + L".tmp";

                const HANDLE file { CreateFileW(
                    candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr
                )
                };

                if (file != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(file);
                    temporary.path = std::move(candidate);
                    return;
                }

                require(
                    GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS,
                    u8"WebP 최적화 임시 파일을 만들 수 없습니다."
                );
            }

            throw optimization_error { u8"WebP 최적화 임시 파일 이름을 만들 수 없습니다." };
        }

        std::uint32_t read_number(const unsigned char* bytes)
        {
            return std::uint32_t { bytes[0] } | (std::uint32_t { bytes[1] } << 8) | (std::uint32_t { bytes[2] } << 16)
                | (std::uint32_t { bytes[3] } << 24);
        }

        std::unique_ptr<SkCodec> load_lossless_webp(const std::filesystem::path& path, bool& has_icc)
        {
            has_icc = false;

            const auto size { std::filesystem::file_size(path) };
            require(size >= 20 && size <= maximum_webp_bytes, u8"WebP 프레임 파일 크기가 올바르지 않습니다.");

            auto data { SkData::MakeUninitialized(static_cast<std::size_t>(size)) };
            require(data != nullptr, u8"WebP 프레임을 읽을 메모리가 부족합니다.");

            std::ifstream input { path, std::ios::binary };
            input.read(static_cast<char*>(data->writable_data()), static_cast<std::streamsize>(size));
            require(input.good(), u8"WebP 프레임 파일을 읽을 수 없습니다.");

            const auto* bytes { static_cast<const unsigned char*>(data->data()) };
            require(
                !std::memcmp(bytes, "RIFF", 4) && !std::memcmp(bytes + 8, "WEBP", 4)
                    && std::uint64_t { read_number(bytes + 4) } + 8 == size,
                u8"WebP 프레임 컨테이너가 손상되었습니다."
            );

            std::uint64_t position { 12 };
            bool lossless {};

            while (position + 8 <= size)
            {
                const auto* chunk { bytes + position };
                const auto length { read_number(chunk + 4) };
                require(length <= size - position - 8, u8"WebP 프레임 청크가 잘렸습니다.");
                require(
                    std::memcmp(chunk, "ANIM", 4) && std::memcmp(chunk, "ANMF", 4) && std::memcmp(chunk, "VP8 ", 4),
                    u8"단일 무손실 WebP 프레임만 최적화할 수 있습니다."
                );

                if (!std::memcmp(chunk, "ICCP", 4))
                {
                    require(!has_icc && length > 0, u8"WebP 프레임의 색공간 프로필이 올바르지 않습니다.");
                    has_icc = true;
                }

                if (!std::memcmp(chunk, "VP8L", 4))
                {
                    require(
                        !lossless && length >= 5 && chunk[8] == 0x2f, u8"WebP 무손실 프레임 헤더가 올바르지 않습니다."
                    );

                    const auto bits { read_number(chunk + 9) };
                    const std::uint64_t width { (bits & 0x3fff) + 1 };
                    const std::uint64_t height { ((bits >> 14) & 0x3fff) + 1 };
                    require(
                        (bits >> 29) == 0 && width <= 16383 && height <= 16383 && width * height <= maximum_pixels,
                        u8"WebP 프레임 크기가 16383 또는 33554432픽셀 한도를 넘었습니다."
                    );
                    lossless = true;
                }

                position += 8 + std::uint64_t { length } + (length & 1);
            }

            require(lossless && position == size, u8"WebP 무손실 프레임 데이터가 올바르지 않습니다.");

            auto codec { SkCodec::MakeFromData(std::move(data)) };
            require(
                codec != nullptr && codec->getEncodedFormat() == SkEncodedImageFormat::kWEBP
                    && codec->getFrameCount() == 1,
                u8"WebP 프레임을 디코딩할 수 없습니다."
            );
            return codec;
        }

        void optimize_frame(
            const std::filesystem::path& path,
            const std::atomic<bool>& cancelled,
            const lossless_compression compression,
            const bool threading
        )
        {
            bool has_icc {};

            auto codec { load_lossless_webp(path, has_icc) };
            const auto source_info { codec->getInfo() };
            require(
                source_info.width() > 0 && source_info.width() <= 16383 && source_info.height() > 0
                    && source_info.height() <= 16383
                    && std::uint64_t { static_cast<unsigned>(source_info.width()) }
                            * static_cast<unsigned>(source_info.height())
                        <= maximum_pixels,
                u8"WebP 프레임의 디코딩 크기가 한도를 넘었습니다."
            );

            const auto info {
                source_info.makeColorType(kBGRA_8888_SkColorType)
                    .makeAlphaType(source_info.isOpaque() ? kOpaque_SkAlphaType : kUnpremul_SkAlphaType)
            };

            std::vector<std::uint32_t> pixels(static_cast<std::size_t>(info.width()) * info.height());
            require(
                codec->getPixels(info, pixels.data(), info.minRowBytes()) == SkCodec::kSuccess,
                u8"WebP 프레임 픽셀이 손상되었습니다."
            );
            codec.reset();
            check_cancelled(cancelled);
            temporary_file temporary {};
            create_temporary(path, temporary);

            // 무태그 이미지의 디코더 기본 sRGB를 새 ICC 프로필로 기록하지 않는다.
            const auto encoding_info { has_icc ? info : info.makeColorSpace(nullptr) };
            const SkPixmap pixmap { encoding_info, pixels.data(), info.minRowBytes() };
            require(
                encode_native_image(
                    temporary.path, pixmap, image_format::webp, compression, true, 80, &cancelled, threading
                ),
                u8"WebP 프레임을 최적화할 수 없습니다."
            );
            check_cancelled(cancelled);

            if (std::filesystem::file_size(temporary.path) >= std::filesystem::file_size(path))
            {
                return;
            }

            bool candidate_has_icc {};

            auto candidate { load_lossless_webp(temporary.path, candidate_has_icc) };
            const auto candidate_info { candidate->getInfo() };
            require(
                candidate_has_icc == has_icc && candidate_info.dimensions() == info.dimensions()
                    && candidate_info.alphaType() == source_info.alphaType()
                    && SkColorSpace::Equals(candidate_info.colorSpace(), info.colorSpace()),
                u8"최적화한 WebP 프레임의 크기·알파·색공간이 달라졌습니다."
            );

            std::vector<std::uint32_t> verified(pixels.size());
            require(
                candidate->getPixels(info, verified.data(), info.minRowBytes()) == SkCodec::kSuccess
                    && verified == pixels,
                u8"최적화한 WebP 프레임의 픽셀이 달라졌습니다."
            );
            candidate.reset();
            check_cancelled(cancelled);

            // 같은 폴더의 완성된 후보만 원자적으로 교체해 원본 파일을 보호한다.
            require(
                ReplaceFileW(path.c_str(), temporary.path.c_str(), nullptr, 0, nullptr, nullptr) != FALSE,
                u8"최적화한 WebP 프레임으로 파일을 교체할 수 없습니다."
            );
        }

        class file_stream final : public SkWStream
        {
        public:
            explicit file_stream(const std::filesystem::path& path)
                : file_ { path, std::ios::binary }
            {}

            bool write(const void* buffer, std::size_t size) override
            {
                file_.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
                count_ += size;
                return static_cast<bool>(file_);
            }

            void flush() override
            {
                file_.flush();
            }

            std::size_t bytesWritten() const override
            {
                return count_;
            }

            bool good() const
            {
                return static_cast<bool>(file_);
            }

        private:
            std::ofstream file_;
            std::size_t count_ {};
        };

        bool direct_webp(
            file_stream& stream,
            const SkPixmap& pixels,
            const lossless_compression compression,
            const bool lossless,
            const int quality,
            const std::atomic<bool>* cancelled,
            const bool threading
        )
        {
            if (!pixels.addr() || pixels.width() < 1 || pixels.height() < 1 || pixels.width() > 16383
                || pixels.height() > 16383 || static_cast<std::int64_t>(pixels.width()) * pixels.height() > 33554432
                || pixels.rowBytes() < static_cast<std::size_t>(pixels.width()) * 4
                || pixels.rowBytes() > static_cast<std::size_t>(std::numeric_limits<int>::max())
                || pixels.computeByteSize() > std::numeric_limits<UINT>::max())
            {
                return false;
            }

            WebPConfig config {};
            WebPPicture picture {};

            if (!WebPConfigInit(&config) || !WebPPictureInit(&picture))
            {
                return false;
            }

            struct picture_cleanup
            {
                WebPPicture& picture;

                ~picture_cleanup()
                {
                    WebPPictureFree(&picture);
                }
            };

            const picture_cleanup cleanup { picture };

            if (!encoding::configure_webp(config, compression, lossless, quality, threading))
            {
                return false;
            }

            picture.use_argb = 1;
            picture.width = pixels.width();
            picture.height = pixels.height();
            picture.custom_ptr = &stream;
            picture.user_data = const_cast<std::atomic<bool>*>(cancelled);
            picture.writer = [](const std::uint8_t* bytes, const std::size_t size, const WebPPicture* target) {
                return static_cast<file_stream*>(target->custom_ptr)->write(bytes, size) ? 1 : 0;
            };
            picture.progress_hook = [](int, const WebPPicture* target) {
                const auto token { static_cast<const std::atomic<bool>*>(target->user_data) };
                return !token || !token->load() ? 1 : 0;
            };
            return (!cancelled || !cancelled->load()) && WebPValidateConfig(&config)
                && WebPPictureImportBGRA(
                       &picture, static_cast<const std::uint8_t*>(pixels.addr()), static_cast<int>(pixels.rowBytes())
                )
                && WebPEncode(&config, &picture) && (!cancelled || !cancelled->load());
        }

        void put_number(unsigned char* bytes, std::uint32_t value, std::size_t count)
        {
            for (std::size_t index {}; index < count; ++index)
            {
                bytes[index] = static_cast<unsigned char>((value >> (index * 8)) & 0xff);
            }
        }

        bool valid_png_pixels(const SkPixmap& pixels)
        {
            return pixels.colorType() == kBGRA_8888_SkColorType && pixels.width() > 0 && pixels.height() > 0
                && pixels.rowBytes() <= std::numeric_limits<UINT>::max()
                && pixels.computeByteSize() <= std::numeric_limits<UINT>::max();
        }

        bool encode_wic_png(IWICImagingFactory* factory, const std::filesystem::path& path, const SkPixmap& pixels)
        {
            const std::size_t buffer_size { pixels.computeByteSize() };

            if (!factory || !valid_png_pixels(pixels))
            {
                return false;
            }

            Microsoft::WRL::ComPtr<IWICStream> stream;
            Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
            Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;

            if (FAILED(factory->CreateStream(&stream))
                || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
                || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
                || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
                || FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))
                || FAILED(frame->SetSize(static_cast<UINT>(pixels.width()), static_cast<UINT>(pixels.height()))))
            {
                return false;
            }

            WICPixelFormatGUID format { GUID_WICPixelFormat32bppBGRA };

            if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA)
            {
                return false;
            }

            return SUCCEEDED(frame->WritePixels(
                       static_cast<UINT>(pixels.height()),
                       static_cast<UINT>(pixels.rowBytes()),
                       static_cast<UINT>(buffer_size),
                       const_cast<BYTE*>(static_cast<const BYTE*>(pixels.addr()))
                   ))
                && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
        }

        bool encode_wic_png(const std::filesystem::path& path, const SkPixmap& pixels)
        {
            if (!valid_png_pixels(pixels))
            {
                return false;
            }

            const wic_png_encoder encoder {};
            return encoder.encode(path, pixels);
        }

    } // namespace

    struct wic_png_encoder::state
    {
        HRESULT initialized { CoInitializeEx(nullptr, COINIT_MULTITHREADED) };
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory {};

        state()
        {
            if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE)
            {
                static_cast<void>(
                    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))
                );
            }
        }

        ~state()
        {
            // COM을 해제하기 전에 팩터리를 놓아야 해제된 apartment에서 Release하지 않는다.
            factory.Reset();

            if (SUCCEEDED(initialized))
            {
                CoUninitialize();
            }
        }
    };

    wic_png_encoder::wic_png_encoder()
        : state_ { std::make_unique<state>() }
    {}

    wic_png_encoder::~wic_png_encoder() = default;

    bool wic_png_encoder::encode(const std::filesystem::path& path, const SkPixmap& pixels) const
    {
        return encode_wic_png(state_->factory.Get(), path, pixels);
    }

    bool encode_native_image(
        const std::filesystem::path& path,
        const SkPixmap& pixels,
        image_format format,
        lossless_compression compression,
        const bool lossless,
        const int quality,
        const std::atomic<bool>* cancelled,
        const bool threading
    )
    {
        if (quality < 1 || quality > 100
            || (compression != lossless_compression::fast && compression != lossless_compression::compact
                && compression != lossless_compression::maximum))
        {
            return false;
        }

        if (format == image_format::png)
        {
            return encode_wic_png(path, pixels);
        }

        file_stream stream { path };
        bool encoded {};

        if (format == image_format::webp)
        {
            if (pixels.colorType() == kBGRA_8888_SkColorType && pixels.alphaType() != kPremul_SkAlphaType
                && !pixels.colorSpace())
            {
                encoded = direct_webp(stream, pixels, compression, lossless, quality, cancelled, threading);
            }
            else
            {
                SkWebpEncoder::Options options {};
                options.fCompression
                    = lossless ? SkWebpEncoder::Compression::kLossless : SkWebpEncoder::Compression::kLossy;
                options.fQuality = lossless ? (compression == lossless_compression::fast          ? 0.0f
                                                   : compression == lossless_compression::maximum ? 100.0f
                                                                                                  : 75.0f)
                                            : static_cast<float>(quality);
                encoded = SkWebpEncoder::Encode(&stream, pixels, options);
            }
        }

        stream.flush();
        return encoded && stream.good();
    }

    bool optimize_webp_frames(
        const std::span<const encoded_animation_frame> frames,
        const std::atomic<bool>& cancelled,
        std::u8string& error,
        const progress_callback& progress,
        const webp_optimization_options options
    )
    {
        error.clear();

        try
        {
            require(
                options.workers <= 8 && options.memory_budget > 0
                    && (options.compression == lossless_compression::compact
                        || options.compression == lossless_compression::maximum),
                u8"WebP 최적화 작업 설정이 올바르지 않습니다."
            );

            struct job
            {
                std::filesystem::path path;
                std::size_t references {};
            };

            const auto path_less = [](const std::filesystem::path& left, const std::filesystem::path& right) {
                return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
            };

            // 긴 정지 구간이 같은 파일을 여러 번 참조하므로 실제 압축 작업은 경로별로 한 번만 수행한다.
            std::map<std::filesystem::path, std::size_t, decltype(path_less)> unique { path_less };
            std::vector<job> jobs;
            std::uint64_t largest_workspace { 1 };
            std::size_t completed {};
            report_progress(progress, save_stage::optimizing, 0, frames.size());
            check_cancelled(cancelled);

            for (const auto& frame : frames)
            {
                check_cancelled(cancelled);

                auto path { std::filesystem::canonical(frame.path) };
                const auto [entry, inserted] { unique.emplace(path, jobs.size()) };

                if (inserted)
                {
                    bool has_icc {};

                    const auto codec { load_lossless_webp(path, has_icc) };
                    const auto info { codec->getInfo() };

                    // 작업 공간은 디코딩·검증 버퍼와 인코더 탐색 공간을 보수적으로 추정한다.
                    const auto pixel_count { std::uint64_t { static_cast<unsigned>(info.width()) }
                                             * static_cast<unsigned>(info.height())
                    };

                    const auto workspace { pixel_count * 48 + std::filesystem::file_size(path) * 2
                                           + 8ULL * 1024 * 1024
                    };
                    largest_workspace = std::max<std::uint64_t>(largest_workspace, workspace);
                    jobs.push_back({ std::move(path), 0 });
                }

                ++jobs[entry->second].references;
            }

            if (jobs.empty())
            {
                return true;
            }

            const auto hardware { std::max(1u, std::thread::hardware_concurrency()) };
            const auto requested { options.workers ? options.workers
                                                   : std::min<std::size_t>(8, std::max(1u, hardware / 2))
            };

            const auto memory_workers { std::max<std::uint64_t>(1, options.memory_budget / largest_workspace) };
            const auto worker_count { std::min(
                { requested, jobs.size(), static_cast<std::size_t>(std::min<std::uint64_t>(memory_workers, 8)) }
            )
            };

            std::atomic<std::size_t> next {};
            std::atomic<bool> failed {};
            std::mutex mutex;
            std::condition_variable changed;
            std::exception_ptr failure;
            const auto work = [&](const std::stop_token stop) {
                try
                {
                    while (!stop.stop_requested() && !failed.load() && !cancelled.load())
                    {
                        const auto index { next.fetch_add(1) };

                        if (index >= jobs.size())
                        {
                            break;
                        }

                        optimize_frame(
                            jobs[index].path,
                            cancelled,
                            options.compression,
                            options.internal_threading && worker_count == 1
                        );
                        {
                            std::lock_guard lock { mutex };
                            completed += jobs[index].references;
                        }

                        changed.notify_one();
                    }
                }
                catch (...)
                {
                    {
                        std::lock_guard lock { mutex };

                        if (!failure)
                        {
                            failure = std::current_exception();
                        }

                        failed = true;
                    }

                    changed.notify_one();
                }
            };

            // 마지막에 선언해 작업자 생성이 실패해도 참조 중인 상태보다 먼저 join한다.
            std::vector<std::jthread> workers;

            for (std::size_t index {}; index < worker_count; ++index)
            {
                workers.emplace_back(work);
            }

            std::size_t reported {};

            while (reported < frames.size() && !failed.load() && !cancelled.load())
            {
                std::unique_lock lock { mutex };
                changed.wait_for(lock, std::chrono::milliseconds { 100 }, [&] {
                    return completed != reported || failed.load() || cancelled.load();
                });

                const auto snapshot { completed };
                lock.unlock();

                // UI 콜백은 호출 스레드에서만 보내며 작업자 완료 순서와 관계없이 증가한다.
                if (snapshot != reported)
                {
                    report_progress(progress, save_stage::optimizing, reported = snapshot, frames.size());
                }
            }

            workers.clear();
            check_cancelled(cancelled);

            if (failure)
            {
                std::rethrow_exception(failure);
            }

            return true;
        }
        catch (const optimization_error& failure)
        {
            error = failure.message;
        }
        catch (const std::exception&)
        {
            error = u8"WebP 프레임 최적화 중 파일 또는 메모리 오류가 발생했습니다.";
        }

        return false;
    }

    native_webp_writer::native_webp_writer(
        const std::filesystem::path& output,
        int width,
        int height,
        const std::atomic<bool>* cancelled,
        const std::uint16_t play_count
    )
        : output_ { output, std::ios::binary }
        , width_ { width }
        , height_ { height }
        , cancelled_ { cancelled }
    {
        if (width <= 0 || width > 16383 || height <= 0 || height > 16383)
        {
            return;
        }

        std::array<unsigned char, 44> header {};
        std::memcpy(header.data(), "RIFF", 4);
        std::memcpy(header.data() + 8, "WEBPVP8X", 8);
        put_number(header.data() + 16, 10, 4);
        header[20] = 0x12;
        put_number(header.data() + 24, static_cast<std::uint32_t>(width - 1), 3);
        put_number(header.data() + 27, static_cast<std::uint32_t>(height - 1), 3);

        std::memcpy(header.data() + 30, "ANIM", 4);
        put_number(header.data() + 34, 6, 4);
        put_number(header.data() + 42, play_count, 2);
        valid_ = write(header.data(), header.size());
    }

    bool native_webp_writer::write(const void* bytes, std::size_t size)
    {
        if (size_ + size > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 8)
        {
            return false;
        }

        output_.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        size_ += size;
        return static_cast<bool>(output_);
    }

    bool native_webp_writer::append_frame(const std::filesystem::path& frame, std::uint32_t duration_ms, int x, int y)
    {
        if (!valid_ || !duration_ms || duration_ms > 0xffffff || x < 0 || y < 0 || (x & 1) != 0 || (y & 1) != 0
            || x >= width_ || y >= height_ || (cancelled_ && cancelled_->load()))
        {
            return valid_ = false;
        }

        std::ifstream input { frame, std::ios::binary };
        std::array<char, 12> header {};
        input.read(header.data(), header.size());

        if (!input || std::memcmp(header.data(), "RIFF", 4) || std::memcmp(header.data() + 8, "WEBP", 4))
        {
            return valid_ = false;
        }

        std::uint32_t riff_size {};
        std::memcpy(&riff_size, header.data() + 4, sizeof(riff_size));
        const auto end { static_cast<std::uint64_t>(riff_size) + 8 };
        input.seekg(0, std::ios::end);

        const auto length { static_cast<std::streamoff>(input.tellg()) };

        if (!input || length < 12 || end > static_cast<std::uint64_t>(length))
        {
            return valid_ = false;
        }

        std::uint64_t position { 12 };
        std::uint64_t payload_position {};
        std::uint32_t payload_size {};
        std::uint64_t alpha_position {};
        std::uint32_t alpha_size {};
        int frame_width {};
        int frame_height {};

        while (position + 8 <= end)
        {
            std::array<char, 8> chunk {};
            input.seekg(static_cast<std::streamoff>(position));
            input.read(chunk.data(), chunk.size());

            std::uint32_t size {};
            std::memcpy(&size, chunk.data() + 4, sizeof(size));

            if (!input || size > end - position - 8)
            {
                return valid_ = false;
            }

            if (!std::memcmp(chunk.data(), "ALPH", 4))
            {
                if (alpha_position || payload_position)
                {
                    return valid_ = false;
                }

                alpha_position = position;
                alpha_size = size;
            }

            const bool lossless { !std::memcmp(chunk.data(), "VP8L", 4) };

            if (lossless || !std::memcmp(chunk.data(), "VP8 ", 4))
            {
                if (payload_position || size < (lossless ? 5U : 10U))
                {
                    return valid_ = false;
                }

                std::array<unsigned char, 10> signature {};
                input.read(reinterpret_cast<char*>(signature.data()), lossless ? 5 : 10);

                if (lossless)
                {
                    std::uint32_t bits {};
                    std::memcpy(&bits, signature.data() + 1, sizeof(bits));
                    frame_width = static_cast<int>((bits & 0x3fff) + 1);
                    frame_height = static_cast<int>(((bits >> 14) & 0x3fff) + 1);

                    if (signature[0] != 0x2f || (bits >> 29) != 0)
                    {
                        return valid_ = false;
                    }
                }
                else
                {
                    if ((signature[0] & 1) != 0 || signature[3] != 0x9d || signature[4] != 1 || signature[5] != 0x2a)
                    {
                        return valid_ = false;
                    }

                    frame_width = (signature[6] | signature[7] << 8) & 0x3fff;
                    frame_height = (signature[8] | signature[9] << 8) & 0x3fff;
                }

                if (!input || frame_width <= 0 || frame_height <= 0 || frame_width > width_ - x
                    || frame_height > height_ - y)
                {
                    return valid_ = false;
                }

                if (frames_ == 0 && (x != 0 || y != 0 || frame_width != width_ || frame_height != height_))
                {
                    return valid_ = false;
                }

                payload_position = position;
                payload_size = size;
            }

            position += 8 + static_cast<std::uint64_t>(size) + (size & 1);
        }

        if (position != end || !payload_position || payload_size > std::numeric_limits<std::uint32_t>::max() - 25)
        {
            return valid_ = false;
        }

        std::array<unsigned char, 24> frame_header {};
        std::memcpy(frame_header.data(), "ANMF", 4);
        const auto alpha_bytes { alpha_position ? 8ULL + alpha_size + (alpha_size & 1) : 0ULL };

        if (alpha_bytes > std::numeric_limits<std::uint32_t>::max() - 25ULL - payload_size)
        {
            return valid_ = false;
        }

        if (alpha_position && alpha_position + alpha_bytes != payload_position)
        {
            return valid_ = false;
        }

        const auto content_size {
            static_cast<std::uint32_t>(16 + 8 + payload_size + (payload_size & 1) + alpha_bytes)
        };
        put_number(frame_header.data() + 4, content_size, 4);
        put_number(frame_header.data() + 8, static_cast<std::uint32_t>(x / 2), 3);
        put_number(frame_header.data() + 11, static_cast<std::uint32_t>(y / 2), 3);
        put_number(frame_header.data() + 14, static_cast<std::uint32_t>(frame_width - 1), 3);
        put_number(frame_header.data() + 17, static_cast<std::uint32_t>(frame_height - 1), 3);
        put_number(frame_header.data() + 20, duration_ms, 3);
        frame_header[23] = 2;

        if (!write(frame_header.data(), frame_header.size()))
        {
            return valid_ = false;
        }

        input.seekg(static_cast<std::streamoff>(alpha_position ? alpha_position : payload_position));

        std::uint64_t remaining { 8 + static_cast<std::uint64_t>(payload_size) + (payload_size & 1) + alpha_bytes };
        std::array<char, 65536> buffer {};

        while (remaining)
        {
            if (cancelled_ && cancelled_->load())
            {
                return valid_ = false;
            }

            const auto count { static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size())) };
            input.read(buffer.data(), static_cast<std::streamsize>(count));

            if (!input || !write(buffer.data(), count))
            {
                return valid_ = false;
            }

            remaining -= count;
        }

        ++frames_;
        return true;
    }

    bool native_webp_writer::finish()
    {
        if (!valid_ || !frames_)
        {
            return false;
        }

        std::array<unsigned char, 4> bytes {};
        put_number(bytes.data(), static_cast<std::uint32_t>(size_ - 8), 4);
        output_.seekp(4);
        output_.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        output_.flush();
        valid_ = static_cast<bool>(output_);
        output_.close();
        return valid_ && !output_.fail();
    }

} // namespace chalkag
