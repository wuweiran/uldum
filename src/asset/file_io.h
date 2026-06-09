#pragma once

#include "core/types.h"

#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uldum::asset {

// Read an entire file into a byte buffer, or std::nullopt on any failure
// (can't open, un-sizeable stream, short read). The tellg / seekg / read
// dance is fiddly to get right and was copy-pasted into the texture
// loader, the UPK archive reader, and the asset manager's directory
// mount — each guarding the same two failure modes:
//
//   * tellg() returns -1 on a non-seekable stream (pipe, device, a file
//     deleted mid-race on Windows). Casting that to size_t yields
//     SIZE_MAX and the vector ctor throws bad_alloc, killing the loader.
//   * gcount() < size means the file was truncated under us or isn't a
//     regular file; the buffer tail is zero-filled and doesn't match
//     disk, so we must treat it as a failure rather than feed garbage on.
//
// Callers adapt the optional to their own convention (empty vector,
// std::unexpected, bool + member).
inline std::optional<std::vector<u8>> read_whole_file(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    const auto pos = file.tellg();
    if (pos < 0) return std::nullopt;
    const auto size = static_cast<size_t>(pos);
    file.seekg(0);
    std::vector<u8> buf(size);
    file.read(reinterpret_cast<char*>(buf.data()),
              static_cast<std::streamsize>(size));
    if (static_cast<size_t>(file.gcount()) != size) return std::nullopt;
    return buf;
}

} // namespace uldum::asset
