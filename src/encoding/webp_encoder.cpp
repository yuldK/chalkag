#include "encoding/webp_encoder.h"
#include "encoding/webp_config.h"

#include <webp/encode.h>
#include <webp/mux.h>

#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace chalkag::encoding {
    namespace {
        void require(const bool condition, const char* message)
        {
            if (!condition)
            {
                throw std::runtime_error { message };
            }
        }

        struct picture_owner
        {
            WebPPicture picture {};

            ~picture_owner()
            {
                WebPPictureFree(&picture);
            }
        };

        struct data_owner
        {
            WebPData data {};

            ~data_owner()
            {
                WebPDataClear(&data);
            }
        };

        struct progress_context
        {
            const std::atomic<bool>& cancelled;
            const progress_callback& callback;
            std::size_t index {};
            std::size_t total {};
            int maximum {};
            bool encoding { true };
        };

        int progress(const int percent, const WebPPicture* picture)
        {
            auto& context { *static_cast<progress_context*>(picture->user_data) };
            context.maximum = std::max(context.maximum, std::clamp(percent, 0, 100));

            if (context.encoding)
            {
                report_progress(
                    context.callback,
                    save_stage::encoding,
                    context.index * 100 + static_cast<std::size_t>(context.maximum),
                    context.total * 100
                );
            }

            return !context.cancelled.load();
        }

    } // namespace

    bool encode_webp(
        const std::filesystem::path& output,
        const animation_source& source,
        const encoder_options& options,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    )
    {
        try
        {
            WebPAnimEncoderOptions animation {};
            WebPConfig config {};
            require(
                WebPAnimEncoderOptionsInit(&animation) && WebPConfigInit(&config),
                "libwebp header ABI does not match the linked library"
            );
            animation.anim_params.loop_count = options.play_count;

            // 기본 키프레임 탐색을 유지하고 압축 노력만 method로 조절한다.
            animation.minimize_size = 0;
            config.lossless = options.lossless;

            const auto compression { !options.compact                  ? lossless_compression::fast
                                         : options.maximum_compression ? lossless_compression::maximum
                                                                       : lossless_compression::compact
            };
            require(
                configure_webp(config, compression, options.lossless, options.quality, options.webp_threading),
                "Invalid WebP preset"
            );
            require(WebPValidateConfig(&config) != 0, "Invalid WebP configuration");

            const auto first { read_animation_frame(source, 0).pixels };
            std::unique_ptr<WebPAnimEncoder, decltype(&WebPAnimEncoderDelete)> encoder {
                WebPAnimEncoderNew(first.width(), first.height(), &animation), WebPAnimEncoderDelete
            };
            require(encoder != nullptr, "Cannot create animated WebP encoder");
            double timeline {};
            progress_context context { cancelled, options.progress, 0, source.count };
            report_progress(options.progress, save_stage::encoding, 0, source.count * 100);

            for (std::size_t index {}; index < source.count; ++index)
            {
                require(!cancelled.load(), "WebP encoding cancelled");

                const auto frame { read_animation_frame(source, index) };
                require(
                    frame.pixels.width() == first.width() && frame.pixels.height() == first.height(),
                    "WebP frame dimensions differ"
                );

                const auto timestamp { std::llround(timeline * 1000) };
                require(
                    timestamp <= std::numeric_limits<int>::max()
                        && frame.pixels.rowBytes() <= std::numeric_limits<int>::max(),
                    "WebP timestamp or stride overflow"
                );
                // 픽셀 소유자는 프레임 추가가 끝날 때까지 살아 있어야 하며 다음 반복에서 버퍼를 재사용한다.
                picture_owner owner;
                auto& picture { owner.picture };
                require(WebPPictureInit(&picture) != 0, "Cannot initialize WebP picture");
                picture.use_argb = 1;
                picture.width = first.width();
                picture.height = first.height();
                picture.progress_hook = progress;
                context.index = index;
                context.maximum = 0;
                picture.user_data = &context;
                require(
                    WebPPictureImportBGRA(
                        &picture,
                        static_cast<const std::uint8_t*>(frame.pixels.addr()),
                        static_cast<int>(frame.pixels.rowBytes())
                    ) != 0,
                    "Cannot import WebP pixels"
                );
                require(
                    WebPAnimEncoderAdd(encoder.get(), &picture, static_cast<int>(timestamp), &config) != 0,
                    WebPAnimEncoderGetError(encoder.get())
                );
                timeline += frame.duration_seconds;
                report_progress(options.progress, save_stage::encoding, (index + 1) * 100, source.count * 100);
            }

            context.encoding = false;
            report_progress(options.progress, save_stage::assembling);

            const auto end { std::llround(timeline * 1000) };
            require(
                end <= std::numeric_limits<int>::max() && !cancelled.load(), "WebP timeline overflow or cancellation"
            );
            require(
                WebPAnimEncoderAdd(encoder.get(), nullptr, static_cast<int>(end), nullptr) != 0,
                "Cannot finalize WebP duration"
            );
            data_owner owner;
            require(
                WebPAnimEncoderAssemble(encoder.get(), &owner.data) != 0 && !cancelled.load(),
                "Cannot assemble WebP animation"
            );

            std::ofstream file { output, std::ios::binary | std::ios::trunc };
            file.write(reinterpret_cast<const char*>(owner.data.bytes), static_cast<std::streamsize>(owner.data.size));
            file.close();
            require(static_cast<bool>(file), "Cannot write WebP file");
            return true;
        }
        catch (const std::exception& failure)
        {
            const std::string detail { failure.what() };
            error = cancelled.load() ? u8"WebP 저장을 취소했습니다."
                                     : u8"정적 WebP 인코더 오류: " + std::u8string { detail.begin(), detail.end() };
            return false;
        }
    }

} // namespace chalkag::encoding
