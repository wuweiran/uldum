#include "asset/asset.h"
#include "core/log.h"

namespace uldum::asset {

static constexpr const char* TAG = "Asset";

bool AssetManager::init() {
    log::info(TAG, "AssetManager initialized (stub)");
    return true;
}

void AssetManager::shutdown() {
    log::info(TAG, "AssetManager shut down (stub)");
}

} // namespace uldum::asset
