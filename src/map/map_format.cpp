// Map file-format version policy — the ONE place a build decides which
// on-disk map versions it accepts. Compiled into uldum_map, whose object files
// are shared by every target in a build tree, so the only build fact available
// here is game-vs-tools (ULDUM_GAME_BUILD, set on the game build tree):
//
//   - Game build: accepts only the current format. A stale map is refused
//     loudly rather than silently mis-read.
//   - Tool builds (dev / editor / worker): accept the set of versions this
//     build retains readers for. Today that is only the current version; as
//     older readers are gated back in (behind their own macros), each adds its
//     version to the accepted set here.
//
// Support (can this build READ version V?) is distinct from migration (editor
// advancing a map's on-disk version V -> V+1). This file is support only.
// Migration lives editor-side and is empty until the first breaking change;
// the reader-dispatch seam below is where retained readers plug in.

#include "map/map.h"

#include <format>

namespace uldum::map {

std::expected<void, std::string> check_map_format(u32 file_format) {
    if (file_format > CURRENT_MAP_FORMAT) {
        return std::unexpected(std::format(
            "map format {} is newer than this build supports (max {}) — update the engine",
            file_format, CURRENT_MAP_FORMAT));
    }

#if defined(ULDUM_GAME_BUILD)
    if (file_format != CURRENT_MAP_FORMAT) {
        return std::unexpected(std::format(
            "map format {} is not supported by this build (expects {}) — rebuild the map",
            file_format, CURRENT_MAP_FORMAT));
    }
#else
    // Tool builds accept every version they retain a reader for. Only the
    // current version exists today; older versions are added here as their
    // readers are gated back in, e.g.:
    //   #if ULDUM_MAP_READ_V<N>
    //       if (file_format == <N>) return {};
    //   #endif
    if (file_format != CURRENT_MAP_FORMAT) {
        return std::unexpected(std::format(
            "map format {} is not supported by this build (readers: {}..{})",
            file_format, CURRENT_MAP_FORMAT, CURRENT_MAP_FORMAT));
    }
#endif

    return {};
}

} // namespace uldum::map
