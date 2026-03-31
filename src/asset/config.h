#pragma once

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

} // namespace uldum::asset
