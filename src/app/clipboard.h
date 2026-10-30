#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace chalkag {
    // 저장한 파일을 클립보드에 넣는다. 실패하면 사용자에게 보일 이유를 돌려준다.
    //  - 파일 자체(CF_HDROP)는 항상 넣어 채팅·탐색기에 붙여 넣을 수 있게 한다.
    //  - include_image면 첫 프레임을 그림(CF_DIBV5)으로도 넣고, PNG 파일은 원본 바이트를 "PNG" 형식으로 함께 넣는다.
    [[nodiscard]] std::u8string copy_file_to_clipboard(
        HWND owner, const std::filesystem::path& path, bool include_image
    );
} // namespace chalkag
