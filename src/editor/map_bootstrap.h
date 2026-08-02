#pragma once

// New-map bootstrapper. Writes the minimal set of files a map needs to load
// and open in the editor — following the engine's "no silent defaults for
// assets" principle: it emits only what has no engine default (terrain texture,
// overlay decals, skybox faces, and the config that wires them), and leaves
// everything with sensible absent-behavior (types/*, strings/*, scripts/*)
// unwritten.
//
// The bootstrapper produces a loose folder. The editor packs it to .uldpak
// afterward (via uldum_pack) when the user asks for that output form.

#include "core/types.h"

#include <filesystem>
#include <functional>
#include <string>

namespace uldum::editor::map_bootstrap {

struct Options {
    std::string id;                 // map id / folder name (e.g. "my_map")
    std::string name;               // display name
    u32         tiles_x = 32;
    u32         tiles_y = 32;
    std::string input_preset = "rts";   // "rts" or "action"
};

// Encode one RGBA (w*h*4) buffer to a KTX2 file. Supplied by the editor (it owns
// the basisu_encoder link). Returns "" on success or an error message.
using EncodeFn = std::function<std::string(const std::filesystem::path& dest,
                                           const u8* rgba, u32 w, u32 h, bool linear)>;

// Write the full skeleton into `dest_dir` (created if absent). `encode` turns
// generated pixel buffers into KTX2 files. Returns "" on success or an error.
std::string create(const std::filesystem::path& dest_dir,
                   const Options& opts, const EncodeFn& encode);

} // namespace uldum::editor::map_bootstrap
