#include "capture/animation_export.h"
#include "capture/animation_recorder.h"
#include "capture/apng_writer.h"
#include "capture/native_webp_writer.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

namespace chalkag {
    namespace {
        int failures {};

        void check(const bool condition, const char* message)
        {
            if (!condition)
            {
                ++failures;

                std::cerr << "FAIL: " << message << '\n';
            }
        }

        std::string read(const std::filesystem::path& path)
        {
            std::ifstream file { path, std::ios::binary };
            return { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
        }

        std::unique_ptr<SkCodec> load(const std::filesystem::path& path)
        {
            const auto bytes { read(path) };
            return SkCodec::MakeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size()));
        }

        // 높은 FPS 입력도 GIF 전체 길이가 실제 시간에서 1cs 안에 머물고 마지막 화면이 남아야 한다.
        void check_gif_timeline(const std::filesystem::path& output, const int fps, const int count)
        {
            SkBitmap bitmap {};
            bitmap.allocPixels(SkImageInfo::Make(24, 16, kBGRA_8888_SkColorType, kOpaque_SkAlphaType));

            const auto color = [count](const int index) {
                return index + 1 == count ? SkColorSetRGB(250, 20, 20) : SkColorSetRGB(20, 20, index * 200 / count);
            };

            const encoding::animation_source source { static_cast<std::size_t>(count),
                                                      [&](const std::size_t index, encoding::animation_frame& frame) {
                                                          bitmap.eraseColor(color(static_cast<int>(index)));
                                                          frame = { bitmap.pixmap(), 1.0 / fps };
                                                          return true;
                                                      }
            };

            const std::atomic<bool> cancelled {};
            std::u8string error {};
            check(
                encoding::encode_animation(
                    output, source, { .format = encoding::animation_format::gif, .gif_dither = false }, cancelled, error
                ),
                "high frame rate GIF encodes"
            );

            const auto codec { load(output) };
            check(codec != nullptr, "high frame rate GIF decodes");

            if (!codec)
            {
                return;
            }

            int total_ms {};
            bool short_delay {};

            for (const auto& info : codec->getFrameInfo())
            {
                total_ms += info.fDuration;
                short_delay = short_delay || info.fDuration < 20;
            }

            const int expected_ms { static_cast<int>(std::lround(count * 1000.0 / fps)) };
            std::cout << "GIF " << fps << " FPS x" << count << ": " << codec->getFrameCount() << " frames, " << total_ms
                      << " ms (expected " << expected_ms << ")\n";
            check(std::abs(total_ms - expected_ms) <= 10, "GIF total duration tracks the real timeline within 1cs");
            check(!short_delay, "GIF frames keep the 2cs minimum delay");

            SkBitmap decoded {};
            decoded.allocPixels(SkImageInfo::Make(24, 16, kBGRA_8888_SkColorType, kPremul_SkAlphaType));

            SkCodec::Options decode_options {};
            decode_options.fFrameIndex = codec->getFrameCount() - 1;
            check(
                codec->getPixels(decoded.info(), decoded.getPixels(), decoded.rowBytes(), &decode_options)
                        == SkCodec::kSuccess
                    && SkColorGetR(decoded.getColor(12, 8)) > 200,
                "GIF keeps the last source frame"
            );
        }

        void run(const std::filesystem::path& directory)
        {
            std::filesystem::create_directories(directory / "raw");
            SkBitmap bitmap;
            bitmap.allocPixels(SkImageInfo::Make(160, 120, kBGRA_8888_SkColorType, kOpaque_SkAlphaType));

            std::uint32_t random { 37 };

            for (int y {}; y < 120; ++y)
            {
                for (int x {}; x < 160; ++x)
                {
                    random = random * 1664525u + 1013904223u;
                    *bitmap.getAddr32(x, y) = 0xff000000u | (random & 0x00ffffffu);
                }
            }

            animation_recorder recorder { directory / "raw", image_format::apng };
            check(recorder.append(bitmap.pixmap(), 0), "store first raw frame");

            for (int y { 40 }; y < 80; ++y)
            {
                for (int x { 40 }; x < 80; ++x)
                {
                    *bitmap.getAddr32(x, y) = 0xffff0000;
                }
            }

            check(recorder.append(bitmap.pixmap(), 0.1), "store a changed tile");

            const auto raw { recorder.finish(0.3) };
            check(raw.size() == 2, "raw timeline preserves changed frames");

            if (raw.size() != 2)
            {
                return;
            }

            const auto original { read(raw.front().path) };
            const std::atomic<bool> cancelled {};
            std::u8string error;

            for (const auto format : { encoding::animation_format::gif,
                                       encoding::animation_format::webp,
                                       encoding::animation_format::apng
            })
            {
                for (const int scale : { 50, 100 })
                {
                    animation_frame_reader reader { raw, 160, 120, scale, cancelled };

                    const auto output {
                        directory / ("stream-" + std::to_string(static_cast<int>(format)) + "-" + std::to_string(scale))
                    };

                    std::vector<save_progress> updates;
                    check(
                        encoding::encode_animation(
                            output,
                            reader.source(),
                            { .format = format,
                              .progress = [&](const save_progress progress) { updates.push_back(progress); }
                            },
                            cancelled,
                            error
                        ),
                        "stream raw tiles through native production encoder"
                    );
                    check(
                        !updates.empty() && updates.front().stage == save_stage::preparing
                            && updates.back() == save_progress { save_stage::complete, 1, 1 },
                        "all formats report preparation and completion only after the file is published"
                    );
                    bool partial {};

                    for (std::size_t index {}; index < updates.size(); ++index)
                    {
                        const auto current { updates[index] };
                        check(
                            current.total == 0 || current.completed <= current.total,
                            "progress never exceeds a phase's actual work count"
                        );
                        partial = partial
                            || (current.stage == save_stage::encoding && current.completed > 0
                                && current.completed < current.total);

                        if (index > 0)
                        {
                            const auto previous { updates[index - 1] };
                            check(current.stage >= previous.stage, "save phases never go backwards");

                            if (current.stage == previous.stage && current.total != 0 && previous.total != 0)
                            {
                                check(
                                    *current.fraction() >= *previous.fraction(),
                                    "processed work within a save phase never goes backwards"
                                );
                            }
                        }
                    }

                    check(partial, "multi-frame exports provide partial encoding progress");

                    auto streamed { load(output) };
                    check(
                        streamed && streamed->getFrameCount() == 2 && streamed->getInfo().width() == 160 * scale / 100,
                        "streamed export dimensions and frame count"
                    );

                    if (streamed)
                    {
                        SkBitmap decoded;
                        decoded.allocPixels(
                            streamed->getInfo().makeColorType(kBGRA_8888_SkColorType).makeAlphaType(kPremul_SkAlphaType)
                        );

                        SkCodec::Options decode_options {};
                        decode_options.fFrameIndex = 1;
                        check(
                            streamed->getPixels(
                                decoded.info(), decoded.getPixels(), decoded.rowBytes(), &decode_options
                            ) == SkCodec::kSuccess,
                            "streamed second composited frame decodes"
                        );
                        check(
                            (*decoded.getAddr32(60 * scale / 100, 60 * scale / 100) & 0x00ff0000) > 0x00e00000,
                            "streamed delta region retains red pixels"
                        );

                        const auto delays { streamed->getFrameInfo() };
                        check(
                            delays.size() == 2 && delays[0].fDuration == 100 && delays[1].fDuration == 200,
                            "streamed export retains actual recording duration"
                        );
                    }
                }
            }

            {
                animation_frame_reader reader { raw, 160, 120, 100, cancelled };
                bool optimization_seen {};

                const auto optimization_progress = [&](const save_progress update) {
                    optimization_seen
                        = optimization_seen || (update.stage == save_stage::optimizing && update.completed > 0);
                };
                check(
                    encoding::encode_animation(
                        directory / "progress-fallback.webp",
                        reader.source(),
                        { .format = encoding::animation_format::webp,
                          .animation_memory_budget = 0,
                          .progress = optimization_progress
                        },
                        cancelled,
                        error
                    ),
                    "streaming WebP fallback reports its extra optimization pass"
                );
                check(optimization_seen, "frame optimization reports real completed work");
                animation_frame_reader throwing_reader { raw, 160, 120, 100, cancelled };
                check(
                    encoding::encode_animation(
                        directory / "progress-observer.apng",
                        throwing_reader.source(),
                        { .format = encoding::animation_format::apng,
                          .progress = [](save_progress) { throw std::runtime_error { "Observer failure" }; }
                        },
                        cancelled,
                        error
                    ),
                    "observer failure cannot abort image saving or escape native callbacks"
                );

                std::atomic<bool> interrupted {};
                animation_frame_reader interrupted_reader { raw, 160, 120, 100, interrupted };

                const auto preserved { directory / "progress-cancel.gif" };
                std::ofstream { preserved } << "existing-file";
                bool completed {};

                const auto cancellation_progress = [&](const save_progress update) {
                    if (update.stage == save_stage::encoding && update.completed > 0)
                    {
                        interrupted = true;
                    }

                    completed = completed || update.stage == save_stage::complete;
                };
                check(
                    !encoding::encode_animation(
                        preserved,
                        interrupted_reader.source(),
                        { .format = encoding::animation_format::gif, .progress = cancellation_progress },
                        interrupted,
                        error
                    ),
                    "cancellation from progress aborts an export"
                );
                check(
                    !completed && read(preserved) == "existing-file",
                    "cancelled export never reports completion or replaces an existing file"
                );

                for (const bool maximum : { false, true })
                {
                    interrupted = false;
                    completed = false;
                    animation_frame_reader parallel_reader { raw, 160, 120, 100, interrupted };

                    const auto parallel_cancel = [&](const save_progress update) {
                        if (update.stage == save_stage::optimizing && update.completed > 0)
                        {
                            interrupted = true;
                        }

                        completed = completed || update.stage == save_stage::complete;
                    };
                    check(
                        !encoding::encode_animation(
                            preserved,
                            parallel_reader.source(),
                            { .maximum_compression = maximum,
                              .webp_workers = 4,
                              .animation_memory_budget = 0,
                              .progress = parallel_cancel
                            },
                            interrupted,
                            error
                        ),
                        "balanced and maximum parallel optimization cancellation aborts export"
                    );
                    check(
                        !completed && !error.empty() && read(preserved) == "existing-file",
                        "parallel cancellation preserves existing output and never reports completion"
                    );

                    for (const auto& entry : std::filesystem::directory_iterator(directory))
                    {
                        check(
                            !entry.path().filename().string().starts_with(".encoding-"),
                            "cancelled parallel export removes its scratch directory"
                        );
                    }
                }
            }

            check(!save_progress {}.fraction(), "unknown work counts do not invent a percentage");
            check(
                save_progress_text({ save_stage::encoding, 1, 4 }) == u8"프레임 압축 · 25%",
                "percentage label matches actual phase work"
            );

            capture_options options {};
            options.animation_format = image_format::apng;

            auto rendered { render_animation_frames(raw, 160, 120, options, directory / "apng", cancelled, error) };
            check(
                !rendered.empty() && error.empty() && write_apng(directory / "result.apng", rendered, cancelled, error),
                "export raw delta APNG"
            );

            auto codec { load(directory / "result.apng") };
            check(
                codec && codec->getFrameCount() == 2 && codec->getInfo().width() == 160,
                "APNG decodes at original dimensions"
            );

            if (codec)
            {
                SkBitmap decoded;
                decoded.allocPixels(bitmap.info().makeAlphaType(kPremul_SkAlphaType));

                SkCodec::Options decode_options {};
                decode_options.fFrameIndex = 1;
                check(
                    codec->getPixels(decoded.info(), decoded.getPixels(), decoded.rowBytes(), &decode_options)
                        == SkCodec::kSuccess,
                    "decode second composited APNG frame"
                );
                check(
                    *decoded.getAddr32(10, 10) == *bitmap.getAddr32(10, 10) && *decoded.getAddr32(60, 60) == 0xffff0000,
                    "lossless re-export preserves base pixels and applies delta"
                );
            }

            // 앱의 보관 녹화 저장과 같은 리더·encode_animation(GIF) 경로로 시간과 픽셀을 확인한다.
            {
                animation_frame_reader reader { raw, 160, 120, 50, cancelled };
                const auto source { reader.source() };
                check(
                    encoding::encode_animation(
                        directory / "result.gif",
                        source,
                        { .format = encoding::animation_format::gif },
                        cancelled,
                        error
                    ),
                    "product GIF export succeeds"
                );
            }

            if (!error.empty())
            {
                std::cerr << reinterpret_cast<const char*>(error.c_str()) << '\n';
            }

            codec = load(directory / "result.gif");
            check(
                codec && codec->getFrameCount() == 2 && codec->getInfo().width() == 80
                    && codec->getInfo().height() == 60
                    && codec->getRepetitionCount() == SkCodec::kRepetitionCountInfinite,
                "GIF dimensions, frames and infinite repeat decode"
            );

            if (codec)
            {
                std::cout << "GIF: " << codec->getFrameCount() << " frames, " << codec->getInfo().width() << 'x'
                          << codec->getInfo().height() << " repeat " << codec->getRepetitionCount() << '\n';

                const auto info { codec->getFrameInfo() };

                for (const auto& entry : info)
                {
                    std::cout << "delay: " << entry.fDuration << '\n';
                }

                check(
                    info.size() == 2 && info[0].fDuration == 100 && info[1].fDuration == 200,
                    "GIF preserves centisecond timing"
                );

                SkBitmap decoded;
                decoded.allocPixels(SkImageInfo::Make(80, 60, kBGRA_8888_SkColorType, kPremul_SkAlphaType));

                SkCodec::Options decode_options {};
                decode_options.fFrameIndex = 1;
                check(
                    codec->getPixels(decoded.info(), decoded.getPixels(), decoded.rowBytes(), &decode_options)
                        == SkCodec::kSuccess,
                    "GIF second frame pixels decode"
                );
                check(
                    (*decoded.getAddr32(30, 30) & 0x00ff0000) > 0x00e00000, "GIF changed region keeps red palette color"
                );
            }

            check_gif_timeline(directory / "gif-60.gif", 60, 60);
            check_gif_timeline(directory / "gif-33.gif", 33, 33);
            check_gif_timeline(directory / "gif-100.gif", 100, 37);

            options.animation_format = image_format::webp;
            options.output_percent = 50;
            options.webp_lossless = false;
            options.webp_quality = 65;
            rendered = render_animation_frames(raw, 160, 120, options, directory / "lossy", cancelled, error);
            check(rendered.size() == 2 && error.empty(), "render lossy WebP without editing raw frames");
            {
                native_webp_writer writer { directory / "result.webp", 80, 60 };
                bool success { !rendered.empty() };

                for (const auto& frame : rendered)
                {
                    success = writer.append_frame(
                                  frame.path, static_cast<std::uint32_t>(frame.duration_seconds * 1000 + 0.5)
                              )
                        && success;
                }

                check(success && writer.finish(), "mux VP8 lossy animation");
            }

            codec = load(directory / "result.webp");
            check(codec && codec->getFrameCount() == 2 && codec->getInfo().width() == 80, "lossy WebP frames decode");

            if (rendered.empty())
            {
                return;
            }

            const auto lossy_size { std::filesystem::file_size(rendered.front().path) };
            check(
                encode_native_image(
                    directory / "lossless.webp", bitmap.pixmap(), image_format::webp, lossless_compression::compact
                ),
                "encode lossless reference"
            );
            check(
                lossy_size < std::filesystem::file_size(directory / "lossless.webp"),
                "lossy export reduces noisy reference size"
            );

            const std::atomic<bool> stopped { true };
            check(
                render_animation_frames(raw, 160, 120, options, directory / "cancel", stopped, error).empty()
                    && !error.empty(),
                "cancelled export fails cleanly"
            );
            options.output_percent = 0;
            check(
                render_animation_frames(raw, 160, 120, options, directory / "invalid", cancelled, error).empty(),
                "invalid export rejected"
            );
            check(
                read(raw.front().path) == original && std::filesystem::exists(raw.back().path),
                "re-export, failure and cancellation preserve raw bytes"
            );
        }

    } // namespace
} // namespace chalkag

int main()
{
    const auto directory { std::filesystem::current_path()
                           / (L"animation-export-" + std::to_wstring(GetCurrentProcessId()))
    };

    try
    {
        chalkag::run(directory);
    }
    catch (const std::exception& error)
    {
        ++chalkag::failures;

        std::cerr << error.what() << '\n';
    }

    std::filesystem::remove_all(directory);
    return chalkag::failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
