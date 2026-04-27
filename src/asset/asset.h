#pragma once

#include "core/handle.h"
#include "asset/texture.h"
#include "asset/model.h"
#include "asset/config.h"
#include "asset/upk.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <variant>

namespace uldum::asset {

class AssetManager {
public:
    // Initialize the asset manager. Optional `apk_assets` is an
    // AAssetManager* (from GameActivity); when non-null it is mounted at the
    // root prefix so `open_package("engine.uldpak", ...)` can read the
    // package from inside the APK. Desktop callers pass nullptr.
    bool init(std::string_view engine_root, void* apk_assets = nullptr);
    void shutdown();

    // Load assets — returns existing handle if already loaded at that path.
    // Paths are relative to engine root.
    Handle<TextureData>  load_texture(std::string_view path);
    Handle<ModelData>    load_model(std::string_view path);
    Handle<JsonDocument> load_config(std::string_view path);

    // Load from absolute path (for map files outside engine root).
    Handle<JsonDocument> load_config_absolute(std::string_view abs_path);

    // Open a package archive at `pkg_path` and mount it at virtual `prefix`
    // (e.g., "engine" or "maps/test_map.uldmap"). Returns false if the file
    // is missing or invalid.
    bool open_package(std::string_view pkg_path, std::string_view prefix = "",
                      std::string_view encryption_key = "");

    // Mount a filesystem directory at virtual `prefix`. Reads for paths under
    // the prefix are served from `fs_dir` via ifstream. Used in dev/editor
    // flows where the source tree is the authoritative asset layout.
    // Returns false if `fs_dir` is not a directory.
    bool mount_directory(std::string_view fs_dir, std::string_view prefix = "");

    // Mount an Android APK's `assets/` directory at virtual `prefix`. Reads
    // for paths under the prefix are served via `AAssetManager_open`. The
    // `asset_manager` argument is an `AAssetManager*` from GameActivity
    // (`app->activity->assetManager`), passed as `void*` to avoid leaking
    // Android headers into desktop translation units. Returns false on
    // non-Android builds or if `asset_manager` is null.
    bool mount_apk_assets(void* asset_manager, std::string_view prefix = "");

    // Remove every mount whose prefix matches `prefix` (after the same
    // normalize-and-add-trailing-slash rule used at mount time). Used
    // by MapManager::unload_map to release a map's package mount on
    // session end so the mount list doesn't accumulate stale entries
    // across sessions. Returns the number of mounts removed.
    u32 unmount(std::string_view prefix);

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

    // Read raw bytes from packages or filesystem. Used by all loaders.
    std::vector<u8> read_file_bytes(std::string_view path) const;

    // Global instance for code that can't easily pass AssetManager around.
    static AssetManager* instance() { return s_instance; }

private:
    static inline AssetManager* s_instance = nullptr;
    std::string resolve_path(std::string_view relative) const;

    // Mounts (checked in order, first match wins).
    // Three kinds: packed archive, filesystem directory, or Android APK asset
    // (only populated on Android; harmless as a variant member elsewhere).
    struct PackageMount {
        std::string prefix;
        UPKReader   reader;
    };
    struct DirectoryMount {
        std::string prefix;
        std::string fs_root;
    };
    struct ApkAssetMount {
        std::string prefix;
        void*       asset_manager = nullptr;  // AAssetManager*
    };
    using Mount = std::variant<PackageMount, DirectoryMount, ApkAssetMount>;
    std::vector<std::unique_ptr<Mount>> m_mounts;

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
