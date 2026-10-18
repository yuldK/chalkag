#include "app/command_window.h"
#include "app/resource_ids.h"
#include "app/single_instance.h"

#include <windows.h>

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
#if !defined(CHALKAG_TEST_APPLICATION)
    chalkag::single_instance instance {};

    if (!instance.valid())
    {
        OutputDebugStringW(L"chalkag: 단일 인스턴스 잠금을 만들 수 없습니다.\n");
        return 1;
    }

    if (!instance.start_or_activate())
    {
        return 0;
    }
#endif

    luil::win32::enable_per_monitor_dpi_awareness();

    const luil::win32::com_sta_scope com {};

    luil::win32::window_config config {};
    config.class_name = L"Chalkag.Command.Window";
    config.icon_resource = CHALKAG_ICON_RESOURCE;
    config.caption = chalkag::make_command_caption();
    config.initial_width = chalkag::command_window_width;
    config.initial_height = chalkag::command_window_height;
    config.minimum_client_width = chalkag::command_window_width;
    config.minimum_client_height = chalkag::command_window_height;
    config.resize_border_thickness = 0;
    config.resize_corner_thickness = 0;
    config.renderer = luil::renderer_mode::cpu;
    config.accept_file_drop = true;
    config.startup_error.title = u8"chalkag 시작 오류";
    config.startup_error.preface = u8"명령창을 열 수 없습니다.";

    chalkag::command_driver driver {};
#if defined(CHALKAG_TEST_APPLICATION)
    chalkag::command_delegate delegate {};
#else
    chalkag::command_delegate delegate { &instance };
#endif
    chalkag::command_policy policy {};
    luil::win32::window_environment environment {};
    environment.driver = &driver;
    environment.delegate = &delegate;
    environment.policy = &policy;
    return luil::win32::run_application_window(config, environment);
}
