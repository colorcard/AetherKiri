#include <catch2/catch_test_macros.hpp>

#include "sdl3/SdlRenderManager.h"
#include "LayerBitmapIntf.h"
#include "LayerIntf.h"

#include <array>
#include <cstdint>

// CPU-side behavior tests for the in-engine SDL3 render backend textures
// (SDLTexture2D). GPU compositing/readback/batch tests were removed together
// with the Godot GPU bridge; those paths now fall back to software.

TEST_CASE("script pixel colors preserve RGB channel order") {
    const std::array<std::uint8_t, 4> red_pixel = {0xfe, 0x00, 0x00, 0xff};
    SDLTexture2D texture(red_pixel.data(), 4, 1, 1,
                           TVPTextureFormat::RGBA);

    // RGBA bytes read as a little-endian integer are 0xAABBGGRR.
    CHECK(texture.GetPoint(0, 0) == 0xff0000fe);
    // KiriKiri scripts always observe the documented 0xRRGGBB value.
    CHECK(TVPFromActualColor(texture.GetPoint(0, 0)) == 0x00fe0000);
    CHECK(TVPFromActualColor(0xff332211) == 0x00112233);
    CHECK(TVPToActualColor(0x00112233) == 0x00112233);
}


TEST_CASE("SDL textures expose Gray province pixels") {
    std::array<std::uint8_t, 8> pixels = {
        1, 2, 3, 0xee,
        4, 5, 6, 0xee,
    };
    SDLTexture2D texture(pixels.data(), 4, 3, 2,
                           TVPTextureFormat::Gray);

    CHECK(texture.GetPoint(0, 0) == 1);
    CHECK(texture.GetPoint(2, 1) == 6);
    CHECK(texture.GetPoint(-1, 0) == 0);
    CHECK(texture.GetPoint(3, 0) == 0);

    texture.SetPoint(1, 1, 0x1234);
    CHECK(texture.GetPoint(1, 1) == 0x34);

    const auto *row = static_cast<const std::uint8_t *>(
        texture.GetScanLineForRead(1));
    REQUIRE(row != nullptr);
    CHECK(row[0] == 4);
    CHECK(row[1] == 0x34);
    CHECK(row[2] == 6);
    CHECK(row[3] == 0xee);
}


TEST_CASE("SDL texture updates clip off-texture rectangles") {
    SDLTexture2D texture(nullptr, 0, 2, 2, TVPTextureFormat::RGBA);
    const std::array<std::uint8_t, 16> pixels = {
        1, 0, 0, 255, 2, 0, 0, 255,
        3, 0, 0, 255, 4, 0, 0, 255,
    };

    texture.Update(pixels.data(), TVPTextureFormat::RGBA, 8,
                   tTVPRect(-1, -1, 1, 1));

    CHECK(texture.GetPoint(0, 0) == 0xff000004u);
    CHECK(texture.GetPoint(1, 0) == 0u);
    CHECK(texture.GetPoint(0, 1) == 0u);
}


TEST_CASE("SDL texture updates reallocate when the pixel format changes") {
    SDLTexture2D texture(nullptr, 0, 2, 1, TVPTextureFormat::Gray);
    const std::array<std::uint8_t, 8> pixels = {
        1, 2, 3, 255, 4, 5, 6, 255,
    };

    texture.Update(pixels.data(), TVPTextureFormat::RGBA, 8,
                   tTVPRect(0, 0, 2, 1));

    CHECK(texture.GetFormat() == TVPTextureFormat::RGBA);
    CHECK(texture.GetPitch() == 8);
    CHECK(texture.GetPoint(1, 0) == 0xff060504u);
}

