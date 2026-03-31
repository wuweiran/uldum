#pragma once

#include <string_view>

namespace uldum::asset {

class AssetManager {
public:
    bool init();
    void shutdown();

    // Future API:
    // template<typename T> Handle<T> load(std::string_view path);
    // template<typename T> T* get(Handle<T> handle);
    // void release(HandleBase handle);
    // void collect_garbage();
};

} // namespace uldum::asset
