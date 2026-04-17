#pragma once

#include "core/types.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace uldum::asset {

struct TextureData {
    u32 width    = 0;
    u32 height   = 0;
    u32 channels = 0;  // 1=R, 2=RG, 3=RGB, 4=RGBA
    std::vector<u8> pixels;
};

std::expected<TextureData, std::string> load_texture(std::string_view path);
std::expected<TextureData, std::string> load_texture_from_memory(const u8* data, u32 size);

} // namespace uldum::asset
