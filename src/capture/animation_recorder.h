#pragma once

#include "capture/animation_frames.h"
#include "capture/frame_optimizer.h"
#include "capture/image_capture.h"

#include <filesystem>
#include <memory>
#include <vector>

class SkPixmap;

namespace chalkag {
    // 변경 타일을 찾는 일은 호출 스레드에서, 타일 압축은 작업 스레드에서 한다.
    // 호출 시각을 그대로 기록하므로 압축이 밀려도 재생 시간은 수집 순간을 따른다.
    class animation_recorder
    {
    public:
        animation_recorder(std::filesystem::path session, image_format format);
        ~animation_recorder();
        animation_recorder(const animation_recorder&) = delete;
        animation_recorder& operator=(const animation_recorder&) = delete;

        // 잘못된 시각·크기는 녹화 전체를 거부한다. 타일 저장 실패는 늦게 알려질 수 있으며 이후 추가만 막는다.
        [[nodiscard]] bool append(const SkPixmap& pixels, double timestamp_seconds);
        // 남은 압축을 기다린다. 타일 저장에 실패했다면 실패한 프레임의 시각에서 앞선 프레임만 마무리한다.
        [[nodiscard]] std::vector<encoded_animation_frame> finish(double timestamp_seconds);
        [[nodiscard]] bool storage_failed() const noexcept;

    private:
        struct encoder_pool;

        std::filesystem::path session_;
        image_format format_;
        frame_optimizer optimizer_;
        std::vector<encoded_animation_frame> frames_;
        std::vector<double> starts_;
        std::unique_ptr<encoder_pool> pool_;
        double last_sample_ {};
        double last_change_ {};
        int width_ {};
        int height_ {};
        bool failed_ {};
        bool finished_ {};
    };
} // namespace chalkag
