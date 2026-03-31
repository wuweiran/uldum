#pragma once

#include "core/types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace uldum::asset {

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 texcoord{0.0f};
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<u32>    indices;
    std::string         name;
};

struct ModelData {
    std::vector<MeshData> meshes;
    std::string           name;
};

std::expected<ModelData, std::string> load_model(std::string_view path);

} // namespace uldum::asset
