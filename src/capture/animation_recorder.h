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
        [[nodiscard]] bool append(const SkPixmap& pixels, double timestamp_seconds);
        [[nodiscard]] std::vector<encoded_animation_frame> finish(double timestamp_seconds);

    private:
        std::filesystem::path session_;
        image_format format_;
        frame_optimizer optimizer_;
        std::vector<encoded_animation_frame> frames_;
        double last_sample_ {};
        double last_change_ {};
        int width_ {};
        int height_ {};
        bool failed_ {};
        bool finished_ {};
    };
} // namespace chalkag
