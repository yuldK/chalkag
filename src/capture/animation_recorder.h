#pragma once

#include "capture/animation_frames.h"
#include "capture/frame_optimizer.h"
#include "capture/image_capture.h"

#include <filesystem>
#include <vector>

class SkPixmap;

namespace chalkag {
    class animation_recorder
    {
    public:
        animation_recorder(std::filesystem::path session, image_format format);
        // 잘못된 시각·크기는 녹화 전체를 거부한다. 타일 저장 실패는 이후 추가만 막고 앞선 프레임은 보존한다.
        [[nodiscard]] bool append(const SkPixmap& pixels, double timestamp_seconds);
        // 타일 저장에 실패했다면 실패한 프레임의 시각에서 마무리하므로 전달한 시각은 쓰지 않는다.
        [[nodiscard]] std::vector<encoded_animation_frame> finish(double timestamp_seconds);
        [[nodiscard]] bool storage_failed() const noexcept;

    private:
        std::filesystem::path session_;
        image_format format_;
        frame_optimizer optimizer_;
        std::vector<encoded_animation_frame> frames_;
        double last_sample_ {};
        double last_change_ {};
        double storage_boundary_ {};
        int width_ {};
        int height_ {};
        bool failed_ {};
        bool storage_failed_ {};
        bool finished_ {};
    };
} // namespace chalkag
