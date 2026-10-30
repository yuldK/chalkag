cmake_minimum_required(VERSION 4.2.0 FATAL_ERROR)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(chalkag VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "chalkag requires Windows x64 and MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
# 하위 라이브러리도 표준 라이브러리의 예외 해제 규약을 따른다.
add_compile_options(/EHsc)

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/luil/CMakeLists.txt")
    message(FATAL_ERROR "Initialize dependencies with scripts/bootstrap.ps1 first.")
endif()

# 명령창은 CPU 렌더러로 충분하다. 캡쳐 엔진은 UI 렌더러와 독립적으로 구성한다.
set(LUIL_BUILD_TESTS OFF)
set(LUIL_BUILD_EXAMPLES OFF)
set(LUIL_BUILD_TOOLING OFF)
set(LUIL_ENABLE_DIRECT3D OFF CACHE BOOL "Build the luil Direct3D renderer")
set(LUIL_ENABLE_WEBVIEW OFF CACHE BOOL "Build the luil WebView host")
set(LUIL_INSTALL OFF)
add_subdirectory(third_party/luil SYSTEM)

# 공개 리소스 생성 함수가 소비자 호출 범위에서 읽는 경로다.
set(LUIL_SOURCE_DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/luil/src")
set(LUIL_ASSET_DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/luil/assets/codicons")
set(LUIL_NLOHMANN_JSON_ROOT "${PROJECT_SOURCE_DIR}/third_party/luil/third_party/nlohmann_json")
set(chalkag_generated_directory "${PROJECT_BINARY_DIR}/generated")
luil_generate_third_party_notices("${chalkag_generated_directory}/third_party_notices.txt")
file(READ "${PROJECT_SOURCE_DIR}/LICENSE" chalkag_license)
file(APPEND "${chalkag_generated_directory}/third_party_notices.txt" "\n\nchalkag\n${chalkag_license}\n")
file(READ "${PROJECT_SOURCE_DIR}/assets/README.md" chalkag_icon_notice)
file(APPEND "${chalkag_generated_directory}/third_party_notices.txt" "\n\n${chalkag_icon_notice}\n")
file(READ "${PROJECT_SOURCE_DIR}/third_party/cgif/LICENSE" chalkag_cgif_notice)
file(READ "${PROJECT_SOURCE_DIR}/third_party/webp-encoding-headers/COPYING" chalkag_webp_notice)
file(APPEND "${chalkag_generated_directory}/third_party_notices.txt" "\n\nCGIF\n${chalkag_cgif_notice}\n\nlibwebp\n${chalkag_webp_notice}\n")
luil_configure_resource_script("${chalkag_generated_directory}/luil_resources.rc"
    "${chalkag_generated_directory}/third_party_notices.txt")
configure_file(src/app/chalkag.rc.in "${chalkag_generated_directory}/chalkag.rc" @ONLY)

add_library(chalkag_cgif STATIC
    third_party/cgif/src/cgif.c
    third_party/cgif/src/cgif_raw.c)
target_include_directories(chalkag_cgif SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/cgif/inc")
target_compile_definitions(chalkag_cgif PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(chalkag_cgif PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON FOLDER "Libraries")
add_library(chalkag_image_encoding STATIC
    src/encoding/image_encoder.cpp src/encoding/gif_encoder.cpp src/encoding/webp_encoder.cpp
    src/capture/apng_writer.cpp src/capture/native_webp_writer.cpp
    src/capture/animation_recorder.cpp src/capture/frame_optimizer.cpp
    src/capture/animation_export.cpp src/capture/animation_document.cpp)
target_include_directories(chalkag_image_encoding PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_include_directories(chalkag_image_encoding SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/webp-encoding-headers")
target_link_libraries(chalkag_image_encoding PUBLIC luil::skia PRIVATE chalkag_cgif windowscodecs ole32)
target_compile_definitions(chalkag_image_encoding PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(chalkag_image_encoding PRIVATE /W4 /WX /utf-8 /permissive-)
set_target_properties(chalkag_image_encoding PROPERTIES FOLDER "Libraries")

add_executable(chalkag WIN32
    src/main.cpp
    src/app/command_window.cpp
    src/app/animation_editor.cpp
    src/app/image_drop.cpp
    src/app/image_drop.h
    src/app/command_window.h
    src/app/message_dialog.cpp
    src/app/help_text.cpp
    src/app/help_text.h
    src/app/app_runtime.cpp
    src/app/clipboard.cpp
    src/app/clipboard.h
    src/app/settings.cpp
    src/app/single_instance.cpp
    src/app/single_instance.h
    src/app/region_editor.cpp
    src/app/region_editor.h
    src/app/hotkeys.cpp
    src/app/shortcut_editor.cpp
    src/app/accent_picker.cpp
    src/capture/image_capture.cpp
    src/capture/region_controller.cpp
    src/capture/frame_glow.cpp
    src/capture/frame_glow.h
    src/capture/region_controller.h
    src/capture/region_events.h
    src/capture/region_geometry.cpp
    src/capture/region_geometry.h
    "${chalkag_generated_directory}/luil_resources.rc"
    "${chalkag_generated_directory}/chalkag.rc")
target_include_directories(chalkag PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(chalkag PRIVATE chalkag_image_encoding luil::luil user32 gdi32 comctl32 windowscodecs ole32 oleaut32 uiautomationcore dwmapi shell32 advapi32)
target_include_directories(chalkag PRIVATE "${LUIL_NLOHMANN_JSON_ROOT}/include")
target_compile_definitions(chalkag PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(chalkag PRIVATE /W4 /WX /utf-8 /permissive- /Zc:preprocessor)
set_target_properties(chalkag PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:chalkag>")
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT chalkag)
set_target_properties(luil luil_core PROPERTIES FOLDER "Libraries")
source_group(TREE "${PROJECT_SOURCE_DIR}/src" PREFIX "Source" FILES
    src/main.cpp src/app/command_window.cpp src/app/command_window.h
    src/capture/region_controller.cpp src/capture/region_controller.h src/capture/region_events.h
    src/capture/region_geometry.cpp src/capture/region_geometry.h)
source_group("Resource" FILES "${chalkag_generated_directory}/luil_resources.rc" "${chalkag_generated_directory}/chalkag.rc")

install(TARGETS chalkag RUNTIME DESTINATION .)

option(CHALKAG_BUILD_TESTS "Build capture tests" OFF)
option(CHALKAG_BUILD_ENCODING_PROTOTYPE "Build the native image encoding benchmark" OFF)
if(CHALKAG_BUILD_ENCODING_PROTOTYPE)
    add_executable(chalkag_encoding_bench tests/encoding_bench.cpp src/capture/native_gif_writer.cpp)
    target_link_libraries(chalkag_encoding_bench PRIVATE chalkag_image_encoding windowscodecs ole32)
    target_compile_definitions(chalkag_encoding_bench PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_encoding_bench PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_encoding_bench PROPERTIES FOLDER "Experiments")
endif()
if(CHALKAG_BUILD_TESTS)
    enable_testing()
    add_executable(chalkag_animation_document_tests tests/animation_document_tests.cpp)
    target_link_libraries(chalkag_animation_document_tests PRIVATE chalkag_image_encoding)
    target_compile_definitions(chalkag_animation_document_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_animation_document_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_animation_document_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_animation_document COMMAND chalkag_animation_document_tests "${PROJECT_BINARY_DIR}/animation-document-test/$<CONFIG>")
    add_executable(chalkag_animation_editor_tests tests/animation_editor_tests.cpp src/app/animation_editor.cpp src/app/image_drop.cpp)
    target_link_libraries(chalkag_animation_editor_tests PRIVATE chalkag_image_encoding luil::luil)
    target_compile_definitions(chalkag_animation_editor_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_animation_editor_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_animation_editor_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_animation_editor COMMAND chalkag_animation_editor_tests "${PROJECT_BINARY_DIR}/animation-editor-test/$<CONFIG>")
    # 테스트 앱만 다중 실행을 허용하며 설치 대상에는 포함하지 않는다.
    get_target_property(chalkag_application_sources chalkag SOURCES)
    add_executable(chalkag_test_app WIN32 ${chalkag_application_sources})
    target_include_directories(chalkag_test_app PRIVATE $<TARGET_PROPERTY:chalkag,INCLUDE_DIRECTORIES>)
    target_link_libraries(chalkag_test_app PRIVATE $<TARGET_PROPERTY:chalkag,LINK_LIBRARIES>)
    target_compile_definitions(chalkag_test_app PRIVATE $<TARGET_PROPERTY:chalkag,COMPILE_DEFINITIONS> CHALKAG_TEST_APPLICATION)
    target_compile_options(chalkag_test_app PRIVATE $<TARGET_PROPERTY:chalkag,COMPILE_OPTIONS>)
    set_target_properties(chalkag_test_app PROPERTIES FOLDER "Tests"
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/test-app"
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:chalkag_test_app>")
    add_executable(chalkag_single_instance_tests tests/single_instance_tests.cpp src/app/single_instance.cpp)
    target_include_directories(chalkag_single_instance_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_single_instance_tests PRIVATE user32 comctl32 advapi32)
    target_compile_definitions(chalkag_single_instance_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_single_instance_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_single_instance_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_single_instance COMMAND chalkag_single_instance_tests)
    add_executable(chalkag_streaming_tests tests/streaming_encoding_tests.cpp)
    target_link_libraries(chalkag_streaming_tests PRIVATE chalkag_image_encoding)
    target_compile_definitions(chalkag_streaming_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_streaming_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_streaming_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_streaming_encoding COMMAND chalkag_streaming_tests)
    if(CHALKAG_BUILD_ENCODING_PROTOTYPE)
        add_test(NAME capture_static_encoding COMMAND chalkag_encoding_bench "${PROJECT_BINARY_DIR}/encoding-smoke/$<CONFIG>")
        add_test(NAME capture_mit_gif COMMAND chalkag_encoding_bench "${PROJECT_BINARY_DIR}/gif-smoke/$<CONFIG>" --gif-bench)
    endif()
    add_executable(chalkag_help_text_tests tests/help_text_tests.cpp src/app/help_text.cpp)
    target_include_directories(chalkag_help_text_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_help_text_tests PRIVATE luil::luil)
    target_compile_definitions(chalkag_help_text_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_help_text_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_help_text_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_help_text COMMAND chalkag_help_text_tests)
    add_executable(chalkag_command_help_tests tests/command_help_tests.cpp src/app/command_window.cpp src/app/animation_editor.cpp src/app/image_drop.cpp src/app/help_text.cpp src/app/message_dialog.cpp
        src/app/region_editor.cpp src/app/hotkeys.cpp src/app/shortcut_editor.cpp src/app/accent_picker.cpp src/capture/region_geometry.cpp)
    target_include_directories(chalkag_command_help_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_command_help_tests PRIVATE luil::luil chalkag_image_encoding)
    target_sources(chalkag_command_help_tests PRIVATE "${chalkag_generated_directory}/luil_resources.rc")
    target_compile_definitions(chalkag_command_help_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_command_help_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_command_help_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_command_help COMMAND chalkag_command_help_tests)
    add_executable(chalkag_region_editor_tests tests/region_editor_tests.cpp src/app/region_editor.cpp src/app/help_text.cpp src/capture/region_geometry.cpp)
    target_include_directories(chalkag_region_editor_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_region_editor_tests PRIVATE luil::luil)
    target_compile_definitions(chalkag_region_editor_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_region_editor_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_region_editor_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_region_editor COMMAND chalkag_region_editor_tests)
    add_executable(chalkag_region_tests tests/region_geometry_tests.cpp src/capture/region_geometry.cpp)
    target_include_directories(chalkag_region_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(chalkag_region_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_region_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_region_geometry COMMAND chalkag_region_tests)
    add_executable(chalkag_region_controller_tests tests/region_controller_tests.cpp
        src/capture/region_controller.cpp src/capture/region_geometry.cpp src/capture/frame_glow.cpp)
    target_include_directories(chalkag_region_controller_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_region_controller_tests PRIVATE luil::luil user32 gdi32 comctl32 oleaut32 uiautomationcore)
    target_compile_definitions(chalkag_region_controller_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_region_controller_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_region_controller_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_region_controller COMMAND chalkag_region_controller_tests)
    set_tests_properties(capture_region_controller PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(chalkag_settings_tests tests/settings_tests.cpp src/app/settings.cpp src/app/hotkeys.cpp src/capture/region_geometry.cpp)
    target_link_libraries(chalkag_settings_tests PRIVATE luil::luil_core user32)
    add_executable(chalkag_hotkey_tests tests/hotkey_tests.cpp src/app/hotkeys.cpp)
    target_include_directories(chalkag_hotkey_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_hotkey_tests PRIVATE user32)
    target_compile_definitions(chalkag_hotkey_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_hotkey_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_hotkey_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_hotkeys COMMAND chalkag_hotkey_tests)
    set_tests_properties(capture_hotkeys PROPERTIES SKIP_RETURN_CODE 77)
    target_include_directories(chalkag_settings_tests PRIVATE "${PROJECT_SOURCE_DIR}/src" "${LUIL_NLOHMANN_JSON_ROOT}/include")
    target_compile_definitions(chalkag_settings_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_settings_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_settings_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_settings COMMAND chalkag_settings_tests)
    add_executable(chalkag_image_tests tests/image_capture_tests.cpp src/capture/image_capture.cpp
        src/capture/region_geometry.cpp)
    target_include_directories(chalkag_image_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_image_tests PRIVATE chalkag_image_encoding windowscodecs ole32 user32 gdi32 dwmapi)
    target_compile_definitions(chalkag_image_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_image_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_image_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_image_validation COMMAND chalkag_image_tests "${PROJECT_BINARY_DIR}/capture-native-test-runtime")
    add_executable(chalkag_apng_tests tests/apng_writer_tests.cpp src/capture/apng_writer.cpp)
    target_include_directories(chalkag_apng_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_apng_tests PRIVATE luil::skia windowscodecs ole32)
    target_compile_definitions(chalkag_apng_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_apng_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_apng_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_apng_writer COMMAND chalkag_apng_tests)
    add_executable(chalkag_frame_optimizer_tests tests/frame_optimizer_tests.cpp src/capture/frame_optimizer.cpp)
    target_include_directories(chalkag_frame_optimizer_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_frame_optimizer_tests PRIVATE luil::skia)
    target_compile_definitions(chalkag_frame_optimizer_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_frame_optimizer_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_frame_optimizer_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_frame_optimizer COMMAND chalkag_frame_optimizer_tests)
    add_executable(chalkag_animation_recorder_tests tests/animation_recorder_tests.cpp)
    target_include_directories(chalkag_animation_recorder_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_animation_recorder_tests PRIVATE chalkag_image_encoding windowscodecs ole32)
    target_compile_definitions(chalkag_animation_recorder_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_animation_recorder_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_animation_recorder_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_animation_recorder COMMAND chalkag_animation_recorder_tests)
    add_executable(chalkag_animation_export_tests tests/animation_export_tests.cpp
        src/capture/native_gif_writer.cpp)
    target_include_directories(chalkag_animation_export_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_animation_export_tests PRIVATE chalkag_image_encoding windowscodecs ole32)
    target_compile_definitions(chalkag_animation_export_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_animation_export_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_animation_export_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_animation_export COMMAND chalkag_animation_export_tests)
    add_executable(chalkag_webp_compression_tests tests/webp_compression_tests.cpp)
    target_include_directories(chalkag_webp_compression_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(chalkag_webp_compression_tests PRIVATE chalkag_image_encoding windowscodecs ole32)
    target_compile_definitions(chalkag_webp_compression_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(chalkag_webp_compression_tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(chalkag_webp_compression_tests PROPERTIES FOLDER "Tests")
    add_test(NAME capture_webp_compression COMMAND chalkag_webp_compression_tests)
endif()
