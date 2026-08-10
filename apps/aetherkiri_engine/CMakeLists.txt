cmake_minimum_required(VERSION 3.28)

project(aetherkiri_engine LANGUAGES CXX)

find_package(SDL3 CONFIG REQUIRED)

add_executable(aetherkiri_engine
    main.cpp
    ../sdl_host/src/host_input.cpp
)

target_compile_features(aetherkiri_engine PRIVATE cxx_std_17)

target_include_directories(aetherkiri_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdl_host/src
)

target_link_libraries(aetherkiri_engine PRIVATE
    engine_api
    $<IF:$<TARGET_EXISTS:SDL3::SDL3>,SDL3::SDL3,SDL3::SDL3-static>
)
