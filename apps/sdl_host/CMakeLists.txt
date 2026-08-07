cmake_minimum_required(VERSION 3.28)

project(aetherkiri_sdl_host LANGUAGES CXX)

find_package(SDL3 CONFIG REQUIRED)

add_executable(aetherkiri_sdl
    src/main.cpp
    src/host_input.cpp
)

target_compile_features(aetherkiri_sdl PRIVATE cxx_std_17)

target_include_directories(aetherkiri_sdl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(aetherkiri_sdl PRIVATE
    engine_api
    $<IF:$<TARGET_EXISTS:SDL3::SDL3>,SDL3::SDL3,SDL3::SDL3-static>
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(aetherkiri_sdl PRIVATE AETHERKIRI_SDL_HOST_DEBUG=1)
endif()
