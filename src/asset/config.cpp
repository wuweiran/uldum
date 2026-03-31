#include "asset/config.h"

#include <format>
#include <fstream>

namespace uldum::asset {

std::expected<JsonDocument, std::string> load_config(std::string_view path) {
    std::string path_str(path);

    std::ifstream file(path_str);
    if (!file.is_open()) {
        return std::unexpected(std::format("Failed to open config '{}'", path));
    }

    JsonDocument doc;
    doc.source_path = path_str;

    try {
        doc.data = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected(std::format("JSON parse error in '{}': {}", path, e.what()));
    }

    return doc;
}

} // namespace uldum::asset
