#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace chalkag {
    enum class save_stage
    {
        preparing,
        palette,
        encoding,
        optimizing,
        assembling,
        publishing,
        complete,
    };

    struct save_progress
    {
        save_stage stage { save_stage::preparing };
        std::size_t completed {};
        std::size_t total {};

        [[nodiscard]] std::optional<float> fraction() const noexcept
        {
            if (total == 0)
            {
                return std::nullopt;
            }
            return static_cast<float>(static_cast<double>(std::min(completed, total)) / static_cast<double>(total));
        }

        bool operator==(const save_progress&) const = default;
    };

    using progress_callback = std::function<void(save_progress)>;

    // 표시 콜백의 실패가 저장 결과나 C 라이브러리 콜백 경계를 바꾸지 않게 한다.
    inline void report_progress(
        const progress_callback& callback,
        const save_stage stage,
        const std::size_t completed = 0,
        const std::size_t total = 0
    ) noexcept
    {
        try
        {
            if (callback)
            {
                callback({ stage, completed, total });
            }
        }
        catch (...)
        {}
    }

    [[nodiscard]] inline std::u8string save_progress_text(const save_progress& progress)
    {
        const char8_t* name { u8"저장 준비" };
        switch (progress.stage)
        {
        case save_stage::palette:
            name = u8"GIF 색상 준비";
            break;
        case save_stage::encoding:
            name = u8"프레임 압축";
            break;
        case save_stage::optimizing:
            name = u8"무손실 최적화";
            break;
        case save_stage::assembling:
            name = u8"이미지 조립";
            break;
        case save_stage::publishing:
            name = u8"파일 마무리";
            break;
        case save_stage::complete:
            return u8"저장 완료";
        case save_stage::preparing:
            break;
        }
        if (const auto fraction { progress.fraction() })
        {
            const auto number { std::to_string(
                static_cast<int>(
                    100.0 * static_cast<double>(std::min(progress.completed, progress.total))
                    / static_cast<double>(progress.total)
                )
            )
            };
            return std::u8string { name } + u8" · " + std::u8string { number.begin(), number.end() } + u8"%";
        }
        return std::u8string { name } + u8" 중…";
    }
} // namespace chalkag
