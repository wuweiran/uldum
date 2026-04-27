#include "asset/asset.h"
#include "core/log.h"

#include <filesystem>
#include <fstream>

#ifdef ULDUM_PLATFORM_ANDROID
#include <android/asset_manager.h>
#endif

namespace uldum::asset {

static constexpr const char* TAG = "Asset";

bool AssetManager::init(std::string_view engine_root, void* apk_assets) {
    m_engine_root = engine_root;

    s_instance = this;

    // On Android the engine/maps live inside the APK — mount it at the root
    // prefix first so subsequent open_package() calls read the archive bytes
    // via AAssetManager instead of trying the filesystem.
    if (apk_assets) {
        mount_apk_assets(apk_assets);
    }

    open_package("engine.uldpak", "engine");

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

    auto bytes = read_file_bytes(resolved);
    if (bytes.empty()) {
        log::error(TAG, "Failed to load texture '{}': not found in any mounted package", resolved);
        return {};
    }
    auto result = asset::load_texture_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
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

    auto bytes = read_file_bytes(resolved);
    if (bytes.empty()) {
        log::error(TAG, "Failed to load model '{}': not found in any mounted package", resolved);
        return {};
    }
    auto result = asset::load_model_from_memory(bytes.data(), static_cast<u32>(bytes.size()), resolved);
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

    auto bytes = read_file_bytes(resolved);
    if (bytes.empty()) {
        log::error(TAG, "Failed to load config '{}': not found in any mounted package", resolved);
        return {};
    }
    auto result = asset::load_config_from_memory(bytes.data(), static_cast<u32>(bytes.size()), resolved);
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

    auto bytes = read_file_bytes(path_str);
    if (bytes.empty()) {
        log::error(TAG, "Failed to load config '{}': not found in any mounted package", path_str);
        return {};
    }
    auto result = asset::load_config_from_memory(bytes.data(), static_cast<u32>(bytes.size()), path_str);
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

// ── Mounting ────────────────────────────────────────────────────────────────

static std::string normalize_prefix(std::string_view prefix) {
    auto p = upk_normalize_path(prefix);
    if (!p.empty() && p.back() != '/') p += '/';
    return p;
}

bool AssetManager::open_package(std::string_view pkg_path, std::string_view prefix,
                                std::string_view encryption_key) {
    PackageMount m;
    m.prefix = normalize_prefix(prefix);

    // If the package lives inside a previously-mounted APK / directory, read
    // its bytes via the mount chain and open the reader from memory. This is
    // how `engine.uldpak` and `<map>.uldmap` load on Android: an ApkAssetMount
    // at prefix "" was installed first, read_file_bytes finds the package
    // there, and UPKReader opens from those bytes. Falls back to the
    // filesystem path when no existing mount produces the bytes.
    auto bytes = read_file_bytes(pkg_path);
    bool opened = bytes.empty()
        ? m.reader.open(pkg_path, encryption_key)
        : m.reader.open_from_memory(std::move(bytes), encryption_key, pkg_path);
    if (!opened) {
        return false;
    }
    log::info(TAG, "Mounted package '{}' at prefix '{}' ({} files)",
              pkg_path, m.prefix, m.reader.file_count());
    m_mounts.push_back(std::make_unique<Mount>(std::move(m)));
    return true;
}

bool AssetManager::mount_directory(std::string_view fs_dir, std::string_view prefix) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(fs::path(std::string(fs_dir)))) {
        return false;
    }
    DirectoryMount m;
    m.prefix = normalize_prefix(prefix);
    m.fs_root = std::string(fs_dir);
    log::info(TAG, "Mounted directory '{}' at prefix '{}'", m.fs_root, m.prefix);
    m_mounts.push_back(std::make_unique<Mount>(std::move(m)));
    return true;
}

u32 AssetManager::unmount(std::string_view prefix) {
    auto target = normalize_prefix(prefix);
    u32 removed = 0;
    auto it = m_mounts.begin();
    while (it != m_mounts.end()) {
        bool match = std::visit([&](auto& m) { return m.prefix == target; }, **it);
        if (match) {
            it = m_mounts.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        log::info(TAG, "Unmounted {} mount(s) at prefix '{}'", removed, target);
    }
    return removed;
}

bool AssetManager::mount_apk_assets(void* asset_manager, std::string_view prefix) {
#ifdef ULDUM_PLATFORM_ANDROID
    if (!asset_manager) return false;
    ApkAssetMount m;
    m.prefix = normalize_prefix(prefix);
    m.asset_manager = asset_manager;
    log::info(TAG, "Mounted APK assets at prefix '{}'", m.prefix);
    m_mounts.push_back(std::make_unique<Mount>(std::move(m)));
    return true;
#else
    (void)asset_manager;
    (void)prefix;
    return false;  // No-op on non-Android.
#endif
}

std::vector<u8> AssetManager::read_file_bytes(std::string_view path) const {
    auto norm = upk_normalize_path(path);

    for (auto& mount : m_mounts) {
        std::vector<u8> data = std::visit([&](auto& m) -> std::vector<u8> {
            std::string_view lookup = norm;
            if (!m.prefix.empty() && lookup.starts_with(m.prefix)) {
                lookup.remove_prefix(m.prefix.size());
            }
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, PackageMount>) {
                return m.reader.read(lookup);
            } else if constexpr (std::is_same_v<T, DirectoryMount>) {
                std::string abs_path = m.fs_root;
                if (!abs_path.empty() && abs_path.back() != '/' && abs_path.back() != '\\')
                    abs_path += '/';
                abs_path += std::string(lookup);
                std::ifstream file(abs_path, std::ios::binary | std::ios::ate);
                if (!file) return {};
                auto size = file.tellg();
                file.seekg(0);
                std::vector<u8> buf(static_cast<size_t>(size));
                file.read(reinterpret_cast<char*>(buf.data()), size);
                return buf;
            } else {
                // ApkAssetMount — only reachable when a mount was installed, which
                // only happens on Android.
#ifdef ULDUM_PLATFORM_ANDROID
                auto* mgr = static_cast<AAssetManager*>(m.asset_manager);
                if (!mgr) return {};
                std::string lookup_z(lookup);
                AAsset* asset = AAssetManager_open(mgr, lookup_z.c_str(), AASSET_MODE_BUFFER);
                if (!asset) return {};
                off64_t len = AAsset_getLength64(asset);
                std::vector<u8> buf(static_cast<size_t>(len));
                AAsset_read(asset, buf.data(), buf.size());
                AAsset_close(asset);
                return buf;
#else
                (void)m;
                return {};
#endif
            }
        }, *mount);
        if (!data.empty()) return data;
    }
    return {};
}

} // namespace uldum::asset
