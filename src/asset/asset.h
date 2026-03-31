#pragma once

#include "core/handle.h"
#include "asset/texture.h"
#include "asset/model.h"
#include "asset/config.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace uldum::asset {

class AssetManager {
public:
    bool init(std::string_view engine_root);
    void shutdown();

    // Load assets — returns existing handle if already loaded at that path.
    Handle<TextureData>  load_texture(std::string_view path);
    Handle<ModelData>    load_model(std::string_view path);
    Handle<JsonDocument> load_config(std::string_view path);

    // Get loaded data by handle (nullptr if invalid/released).
    TextureData*  get(Handle<TextureData> h)  { return m_textures.get(h); }
    ModelData*    get(Handle<ModelData> h)     { return m_models.get(h); }
    JsonDocument* get(Handle<JsonDocument> h)  { return m_configs.get(h); }

    // Release a loaded asset.
    void release(Handle<TextureData> h);
    void release(Handle<ModelData> h);
    void release(Handle<JsonDocument> h);

    // Stats
    u32 texture_count() const { return m_textures.count(); }
    u32 model_count()   const { return m_models.count(); }
    u32 config_count()  const { return m_configs.count(); }

private:
    std::string resolve_path(std::string_view relative) const;

    ResourcePool<TextureData>  m_textures;
    ResourcePool<ModelData>    m_models;
    ResourcePool<JsonDocument> m_configs;

    // Path → handle cache (avoids loading the same file twice)
    std::unordered_map<std::string, Handle<TextureData>>  m_texture_cache;
    std::unordered_map<std::string, Handle<ModelData>>    m_model_cache;
    std::unordered_map<std::string, Handle<JsonDocument>> m_config_cache;

    std::string m_engine_root;
};

} // namespace uldum::asset
