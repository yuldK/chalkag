#pragma once

#include "luil/app/app_host.h"

#include <cstdint>
#include <string>

namespace chalkag {
    enum class message_dialog_kind
    {
        none,
        information,
        discard_recording,
        exit_application,
        replace_recording,
        // 저장 중에 종료를 요청했다. 확인하면 저장을 취소하고 끝낸다.
        exit_while_saving,
    };

    struct message_dialog_state
    {
        message_dialog_kind kind {};
        std::uint64_t revision {};
        std::u8string title;
        std::u8string body;
    };

    [[nodiscard]] luil::ui_popup make_message_dialog(const message_dialog_state& state, float scale);
} // namespace chalkag
