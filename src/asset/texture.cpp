#include "asset/texture.h"

#include <stb_image.h>

#include <format>

namespace uldum::asset {

std::expected<TextureData, std::string> load_texture(std::string_view path) {
    std::string path_str(path);
    int w, h, channels;
    u8* pixels = stbi_load(path_str.c_str(), &w, &h, &channels, 0);
    if (!pixels) {
        return std::unexpected(std::format("Failed to load texture '{}': {}",
                                           path, stbi_failure_reason()));
    }

    TextureData data;
    data.width    = static_cast<u32>(w);
    data.height   = static_cast<u32>(h);
    data.channels = static_cast<u32>(channels);
    data.pixels.assign(pixels, pixels + w * h * channels);

    stbi_image_free(pixels);
    return data;
}

std::expected<TextureData, std::string> load_texture_from_memory(const u8* data, u32 size) {
    int w, h, channels;
    u8* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 0);
    if (!pixels) {
        return std::unexpected(std::format("Failed to load texture from memory: {}",
                                           stbi_failure_reason()));
    }

    TextureData tex;
    tex.width    = static_cast<u32>(w);
    tex.height   = static_cast<u32>(h);
    tex.channels = static_cast<u32>(channels);
    tex.pixels.assign(pixels, pixels + w * h * channels);

    stbi_image_free(pixels);
    return tex;
}

} // namespace uldum::asset
