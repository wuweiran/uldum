#include "asset/config.h"

#include <format>

namespace uldum::asset {

std::expected<JsonDocument, std::string> load_config_from_memory(const u8* data, u32 size, std::string_view source) {
    JsonDocument doc;
    doc.source_path = std::string(source);

    try {
        doc.data = nlohmann::json::parse(data, data + size);
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected(std::format("JSON parse error: {}", e.what()));
    }

    return doc;
}

} // namespace uldum::asset
