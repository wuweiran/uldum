#include "asset/texture.h"

#include <basisu_transcoder.h>

#include <format>
#include <fstream>
#include <mutex>

namespace uldum::asset {

// All textures are KTX2 containers with UASTC or ETC1S payload (Basis
// Universal supercompression). We transcode to RGBA8 here for GPU upload;
// switching to a hardware-native format (BC7 on desktop, ASTC on Android)
// is a later optimization.

static void ensure_basisu_init() {
    static std::once_flag once;
    std::call_once(once, []() {
        basist::basisu_transcoder_init();
    });
}

static std::expected<TextureData, std::string> decode_from_memory(const u8* data, u32 size) {
    ensure_basisu_init();

    basist::ktx2_transcoder tex;
    if (!tex.init(data, size)) {
        return std::unexpected(std::string("Failed to parse KTX2 container"));
    }
    if (!tex.start_transcoding()) {
        return std::unexpected(std::string("Failed to start KTX2 transcoding"));
    }

    const u32 w = tex.get_width();
    const u32 h = tex.get_height();

    // Base mip level, first layer, first face.
    basist::ktx2_image_level_info info{};
    if (!tex.get_image_level_info(info, 0, 0, 0)) {
        return std::unexpected(std::string("Failed to query KTX2 level 0"));
    }

    TextureData out;
    out.width    = w;
    out.height   = h;
    out.channels = 4;
    out.pixels.resize(static_cast<size_t>(w) * h * 4);

    // Transcode directly to uncompressed RGBA32 (4 bytes per pixel).
    const u32 output_pixel_count = w * h;
    if (!tex.transcode_image_level(
            /*level_index=*/0, /*layer_index=*/0, /*face_index=*/0,
            out.pixels.data(), output_pixel_count,
            basist::transcoder_texture_format::cTFRGBA32)) {
        return std::unexpected(std::string("KTX2 transcode to RGBA32 failed"));
    }

    return out;
}

std::expected<TextureData, std::string> load_texture(std::string_view path) {
    std::string path_str(path);
    std::ifstream file(path_str, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected(std::format("Failed to open texture '{}'", path));
    }
    auto size = file.tellg();
    file.seekg(0);
    std::vector<u8> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return decode_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
}

std::expected<TextureData, std::string> load_texture_from_memory(const u8* data, u32 size) {
    return decode_from_memory(data, size);
}

} // namespace uldum::asset
