#include "asset/texture.h"

#include <ktx.h>

#include <format>
#include <fstream>

namespace uldum::asset {

// All textures are KTX2 + Basis Universal. Transcoded to RGBA8 here.
// See docs/packaging.md for the format policy.

static std::expected<TextureData, std::string> decode_ktx2(ktxTexture2* tex) {
    if (ktxTexture2_NeedsTranscoding(tex)) {
        KTX_error_code rc = ktxTexture2_TranscodeBasis(tex, KTX_TTF_RGBA32, 0);
        if (rc != KTX_SUCCESS) {
            ktxTexture_Destroy(ktxTexture(tex));
            return std::unexpected(std::format("KTX2 transcode failed: error {}", static_cast<int>(rc)));
        }
    }

    // Base mip level only (level 0, layer 0, face 0).
    ktx_size_t offset = 0;
    KTX_error_code rc = ktxTexture_GetImageOffset(ktxTexture(tex), 0, 0, 0, &offset);
    if (rc != KTX_SUCCESS) {
        ktxTexture_Destroy(ktxTexture(tex));
        return std::unexpected(std::format("KTX2 GetImageOffset failed: error {}", static_cast<int>(rc)));
    }

    TextureData out;
    out.width    = tex->baseWidth;
    out.height   = tex->baseHeight;
    out.channels = 4;

    const u8* pixels = ktxTexture_GetData(ktxTexture(tex)) + offset;
    ktx_size_t level0_size = static_cast<ktx_size_t>(out.width) * out.height * 4;
    out.pixels.assign(pixels, pixels + level0_size);

    ktxTexture_Destroy(ktxTexture(tex));
    return out;
}

std::expected<TextureData, std::string> load_texture(std::string_view path) {
    std::string path_str(path);

    ktxTexture2* tex = nullptr;
    KTX_error_code rc = ktxTexture2_CreateFromNamedFile(
        path_str.c_str(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &tex);
    if (rc != KTX_SUCCESS || !tex) {
        return std::unexpected(std::format("Failed to open KTX2 '{}': error {}", path, static_cast<int>(rc)));
    }
    return decode_ktx2(tex);
}

std::expected<TextureData, std::string> load_texture_from_memory(const u8* data, u32 size) {
    ktxTexture2* tex = nullptr;
    KTX_error_code rc = ktxTexture2_CreateFromMemory(
        data, size,
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &tex);
    if (rc != KTX_SUCCESS || !tex) {
        return std::unexpected(std::format("Failed to parse KTX2 from memory: error {}", static_cast<int>(rc)));
    }
    return decode_ktx2(tex);
}

} // namespace uldum::asset
