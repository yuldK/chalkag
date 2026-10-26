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
