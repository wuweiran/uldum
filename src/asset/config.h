#pragma once

#include "core/types.h"

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace uldum::asset {

struct JsonDocument {
    nlohmann::json  data;
    std::string     source_path;
};

std::expected<JsonDocument, std::string> load_config(std::string_view path);
std::expected<JsonDocument, std::string> load_config_from_memory(const u8* data, u32 size, std::string_view source = "");

} // namespace uldum::asset
