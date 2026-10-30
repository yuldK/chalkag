#pragma once

#include "capture/animation_frames.h"
#include "capture/image_capture.h"
#include "encoding/image_encoder.h"
#include "include/core/SkBitmap.h"

#include <atomic>
#include <span>
#include <vector>

namespace chalkag {
    class animation_frame_reader
    {
    public:
        animation_frame_reader(
            std::span<const encoded_animation_frame> frames,
            int width,
            int height,
            int output_percent,
            const std::atomic<bool>& cancelled
        );
        [[nodiscard]] encoding::animation_source source();
        // 지금까지 합성한 원본 크기 화면이다. 다음 읽기 전까지 유효하다.
        [[nodiscard]] const SkBitmap& canvas() const noexcept;
        // 저장해 둔 합성 화면에서 이어 읽도록 다음 프레임 위치를 옮긴다.
        [[nodiscard]] bool seek(std::size_t next, const SkPixmap& canvas);

    private:
        bool read(std::size_t index, encoding::animation_frame& frame);
        std::span<const encoded_animation_frame> frames_;
        SkBitmap canvas_;
        SkBitmap scaled_;
        std::size_t next_ {};
        const std::atomic<bool>& cancelled_;
    };

    // 원본 타일은 읽기만 하고 저장 후보를 별도 폴더에 만든다.
    [[nodiscard]] std::vector<encoded_animation_frame> render_animation_frames(
        std::span<const encoded_animation_frame> source,
        int width,
        int height,
        const capture_options& options,
        const std::filesystem::path& scratch,
        const std::atomic<bool>& cancelled,
        std::u8string& error
    );
} // namespace chalkag
