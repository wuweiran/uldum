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

} // namespace uldum::asset
