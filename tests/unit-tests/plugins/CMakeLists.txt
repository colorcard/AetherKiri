cmake_minimum_required(VERSION 3.16)
project(TestPlugins LANGUAGES CXX)

add_executable(motionplayer-dll
    main.cpp
    motionplayer-dll.cpp
    psb-media.cpp
    registry.cpp
    kbad-data-pack.cpp
    textrender-timing.cpp
    text-stream.cpp
    xp3-cx-decoder.cpp
    font-compat.cpp
    font-baseline.cpp
    sdl-texture.cpp
    krkrgles-aliases.cpp
    kag-load-contract.cpp
    render-simd.cpp
    script-compatibility.cpp
    project-storage.cpp
)

set(_KRKR2PLUGIN_LINK_ITEM "$<LINK_LIBRARY:WHOLE_ARCHIVE,krkr2plugin>")

if(LINUX)
    # Same fix as bridge/engine_api: LINK_LIBRARY:WHOLE_ARCHIVE collides
    # with krkr2plugin's plain back-edges in the transitive graph. Drive the
    # linker directly for one whole-archive pass and re-declare the
    # sub-plugins explicitly.
    target_link_libraries(motionplayer-dll PRIVATE
        Catch2::Catch2
        krkr2core)
    set(_KRKR2PLUGIN_ARCHIVE "${CMAKE_BINARY_DIR}/cpp/plugins/libkrkr2plugin.a")
    target_link_options(motionplayer-dll PRIVATE
        "LINKER:SHELL:--whole-archive ${_KRKR2PLUGIN_ARCHIVE} --no-whole-archive")
    target_link_libraries(motionplayer-dll PRIVATE
        psdfile psbfile motionplayer kagparserex layerExDraw psdparse)
    # core_visual (LoadJPEG) references libjpeg symbols; CMake 3.x link
    # ordering needs it declared explicitly after the core archives.
    find_package(JPEG REQUIRED)
    target_link_libraries(motionplayer-dll PRIVATE JPEG::JPEG)
    target_link_options(motionplayer-dll PRIVATE "-Wl,--no-as-needed")
else()
    target_link_libraries(motionplayer-dll
        PRIVATE
            Catch2::Catch2
        PUBLIC
            krkr2core
            ${_KRKR2PLUGIN_LINK_ITEM}
    )
endif()

target_include_directories(motionplayer-dll PRIVATE
    "${TEST_CONFIG_DIR}"
    "${CMAKE_SOURCE_DIR}/cpp/plugins"
)

if(AETHERKIRI_INTERNAL_ENABLED)
    find_package(lz4 CONFIG REQUIRED)
    target_link_libraries(motionplayer-dll PRIVATE lz4::lz4)
    target_compile_definitions(motionplayer-dll PRIVATE
        AETHERKIRI_EXPECT_INTERNAL_EMOTE=1)
endif()

if(AETHERKIRI_INTERNAL_ENABLED AND COMMAND aetherinternal_extend_krkr2plugin_tests)
    aetherinternal_extend_krkr2plugin_tests(motionplayer-dll)
endif()

catch_discover_tests(motionplayer-dll)
