#pragma once

#include "capture/save_progress.h"

#include "include/core/SkPixmap.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace chalkag::encoding {
    enum class animation_format
    {
        gif,
        webp,
        apng,
    };

    enum class gif_backend
    {
        cgif,
        cgif_fixed_palette,
    };

    // 픽셀 메모리는 호출이 끝날 때까지 호출자가 소유한다. 불투명 또는 straight alpha BGRA 입력을 받는다.
    struct animation_frame
    {
        SkPixmap pixels;
        double duration_seconds {};
    };

    struct encoder_options
    {
        animation_format format { animation_format::webp };
        bool lossless { true };
        int quality { 80 };
        bool compact { true };
        bool maximum_compression { false };
        std::size_t webp_workers {};
        bool webp_threading { false };
        bool gif_dither { true };
        bool webp_animation_optimizer { true };
        gif_backend gif_encoder { gif_backend::cgif };
        // 전체 애니메이션 최적화의 원시 입력 예산을 넘으면 타일별 압축으로 전환한다.
        std::uint64_t animation_memory_budget { 128 * 1024 * 1024 };
        int play_count {};
        progress_callback progress {};
        // 인코딩 중간 파일을 둘 폴더다. 비우면 출력 파일 옆에 숨김 이름으로 만든다.
        // 앱은 사용자에게 보이는 저장 폴더 대신 data/temp를 넘긴다.
        std::filesystem::path scratch_directory {};
    };

    // 0부터 순서대로 읽으며 0을 다시 요청하면 처음으로 되감는다. 픽셀은 다음 읽기까지 유효하다.
    struct animation_source
    {
        std::size_t count {};
        std::function<bool(std::size_t, animation_frame&)> read;
    };

    [[nodiscard]] animation_frame read_animation_frame(const animation_source& source, std::size_t index);

    // 성공한 후보만 최종 파일로 바꾼다. 취소·실패는 기존 파일을 보존한다.
    [[nodiscard]] bool encode_animation(
        const std::filesystem::path& output,
        std::span<const animation_frame> frames,
        const encoder_options& options,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    );
    [[nodiscard]] bool encode_animation(
        const std::filesystem::path& output,
        const animation_source& source,
        const encoder_options& options,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    );
} // namespace chalkag::encoding
