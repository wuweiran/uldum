#include "asset/asset.h"
#include "core/log.h"

#include <filesystem>

namespace uldum::asset {

static constexpr const char* TAG = "Asset";

bool AssetManager::init(std::string_view engine_root) {
    m_engine_root = engine_root;
    log::info(TAG, "AssetManager initialized — root: '{}'", m_engine_root);
    return true;
}

void AssetManager::shutdown() {
    u32 textures = m_textures.count();
    u32 models   = m_models.count();
    u32 configs  = m_configs.count();

    m_texture_cache.clear();
    m_model_cache.clear();
    m_config_cache.clear();
    m_textures.clear();
    m_models.clear();
    m_configs.clear();

    log::info(TAG, "AssetManager shut down — released {} textures, {} models, {} configs",
              textures, models, configs);
}

std::string AssetManager::resolve_path(std::string_view relative) const {
    namespace fs = std::filesystem;
    fs::path p(relative);
    if (p.is_absolute()) return std::string(relative);
    return (fs::path(m_engine_root) / p).string();
}

// ── Texture ────────────────────────────────────────────────────────────────

Handle<TextureData> AssetManager::load_texture(std::string_view path) {
    std::string resolved = resolve_path(path);

    if (auto it = m_texture_cache.find(resolved); it != m_texture_cache.end()) {
        if (m_textures.get(it->second)) return it->second;
        m_texture_cache.erase(it);
    }

    auto result = asset::load_texture(resolved);
    if (!result) {
        log::error(TAG, "{}", result.error());
        return {};
    }

    auto handle = m_textures.add(std::move(*result));
    m_texture_cache[resolved] = handle;

    auto* data = m_textures.get(handle);
    log::info(TAG, "Loaded texture '{}' ({}x{}, {} ch)", path, data->width, data->height, data->channels);
    return handle;
}

void AssetManager::release(Handle<TextureData> h) {
    m_textures.remove(h);
}

// ── Model ──────────────────────────────────────────────────────────────────

Handle<ModelData> AssetManager::load_model(std::string_view path) {
    std::string resolved = resolve_path(path);

    if (auto it = m_model_cache.find(resolved); it != m_model_cache.end()) {
        if (m_models.get(it->second)) return it->second;
        m_model_cache.erase(it);
    }

    auto result = asset::load_model(resolved);
    if (!result) {
        log::error(TAG, "{}", result.error());
        return {};
    }

    auto handle = m_models.add(std::move(*result));
    m_model_cache[resolved] = handle;

    auto* data = m_models.get(handle);
    u32 total_verts = 0, total_indices = 0;
    for (const auto& mesh : data->meshes) {
        total_verts += static_cast<u32>(mesh.vertices.size());
        total_indices += static_cast<u32>(mesh.indices.size());
    }
    log::info(TAG, "Loaded model '{}' ({} meshes, {} verts, {} indices)",
              path, data->meshes.size(), total_verts, total_indices);
    return handle;
}

void AssetManager::release(Handle<ModelData> h) {
    m_models.remove(h);
}

// ── Config ─────────────────────────────────────────────────────────────────

Handle<JsonDocument> AssetManager::load_config(std::string_view path) {
    std::string resolved = resolve_path(path);

    if (auto it = m_config_cache.find(resolved); it != m_config_cache.end()) {
        if (m_configs.get(it->second)) return it->second;
        m_config_cache.erase(it);
    }

    auto result = asset::load_config(resolved);
    if (!result) {
        log::error(TAG, "{}", result.error());
        return {};
    }

    auto handle = m_configs.add(std::move(*result));
    m_config_cache[resolved] = handle;

    log::info(TAG, "Loaded config '{}'", path);
    return handle;
}

Handle<JsonDocument> AssetManager::load_config_absolute(std::string_view abs_path) {
    std::string path_str(abs_path);

    if (auto it = m_config_cache.find(path_str); it != m_config_cache.end()) {
        if (m_configs.get(it->second)) return it->second;
        m_config_cache.erase(it);
    }

    auto result = asset::load_config(path_str);
    if (!result) {
        log::error(TAG, "{}", result.error());
        return {};
    }

    auto handle = m_configs.add(std::move(*result));
    m_config_cache[path_str] = handle;

    log::info(TAG, "Loaded config '{}'", abs_path);
    return handle;
}

void AssetManager::release(Handle<JsonDocument> h) {
    m_configs.remove(h);
}

} // namespace uldum::asset
