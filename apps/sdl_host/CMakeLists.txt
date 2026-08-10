cmake_minimum_required(VERSION 3.28)

project(aetherkiri_sdl_host LANGUAGES CXX)

find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_executable(aetherkiri_sdl
    src/main.cpp
    src/host_input.cpp
    src/ui/ui_launcher.cpp
    src/ui/ui_overlay.cpp
)

# The sdl host is now the UI shell (Launcher/Overlay/diagnostics); the
# standalone engine shell is apps/aetherkiri_engine.
set_target_properties(aetherkiri_sdl PROPERTIES OUTPUT_NAME "aetherkiri_ui")

target_compile_features(aetherkiri_sdl PRIVATE cxx_std_17)

target_include_directories(aetherkiri_sdl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # GPU bridge callbacks table + tTVPRect/tjs types (lightweight headers).
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp/core/visual/godot
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp/core/visual
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp/core/tjs2
)

target_link_libraries(aetherkiri_sdl PRIVATE
    engine_api
    imgui::imgui
    $<IF:$<TARGET_EXISTS:SDL3::SDL3>,SDL3::SDL3,SDL3::SDL3-static>
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(aetherkiri_sdl PRIVATE AETHERKIRI_SDL_HOST_DEBUG=1)
endif()
