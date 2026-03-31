#include "asset/model.h"

#include <cgltf.h>

#include <format>

namespace uldum::asset {

static glm::vec3 read_vec3(const cgltf_accessor* acc, cgltf_size index) {
    glm::vec3 v{0.0f};
    cgltf_accessor_read_float(acc, index, &v.x, 3);
    return v;
}

static glm::vec2 read_vec2(const cgltf_accessor* acc, cgltf_size index) {
    glm::vec2 v{0.0f};
    cgltf_accessor_read_float(acc, index, &v.x, 2);
    return v;
}

std::expected<ModelData, std::string> load_model(std::string_view path) {
    std::string path_str(path);

    cgltf_options options{};
    cgltf_data* data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, path_str.c_str(), &data);
    if (result != cgltf_result_success) {
        return std::unexpected(std::format("Failed to parse glTF '{}': error {}", path, static_cast<int>(result)));
    }

    result = cgltf_load_buffers(&options, data, path_str.c_str());
    if (result != cgltf_result_success) {
        cgltf_free(data);
        return std::unexpected(std::format("Failed to load glTF buffers '{}': error {}", path, static_cast<int>(result)));
    }

    ModelData model;
    model.name = path_str;

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];

        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            MeshData mesh_data;
            mesh_data.name = mesh.name ? mesh.name : std::format("mesh_{}", mi);

            // Find attribute accessors
            const cgltf_accessor* pos_acc = nullptr;
            const cgltf_accessor* norm_acc = nullptr;
            const cgltf_accessor* uv_acc = nullptr;

            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
                const auto& attr = prim.attributes[ai];
                if (attr.type == cgltf_attribute_type_position)  pos_acc  = attr.data;
                if (attr.type == cgltf_attribute_type_normal)    norm_acc = attr.data;
                if (attr.type == cgltf_attribute_type_texcoord)  uv_acc   = attr.data;
            }

            if (!pos_acc) continue;

            // Read vertices
            cgltf_size vertex_count = pos_acc->count;
            mesh_data.vertices.resize(vertex_count);

            for (cgltf_size vi = 0; vi < vertex_count; ++vi) {
                auto& v = mesh_data.vertices[vi];
                v.position = read_vec3(pos_acc, vi);
                if (norm_acc) v.normal   = read_vec3(norm_acc, vi);
                if (uv_acc)   v.texcoord = read_vec2(uv_acc, vi);
            }

            // Read indices
            if (prim.indices) {
                cgltf_size index_count = prim.indices->count;
                mesh_data.indices.resize(index_count);
                for (cgltf_size ii = 0; ii < index_count; ++ii) {
                    mesh_data.indices[ii] = static_cast<u32>(cgltf_accessor_read_index(prim.indices, ii));
                }
            }

            model.meshes.push_back(std::move(mesh_data));
        }
    }

    cgltf_free(data);
    return model;
}

} // namespace uldum::asset
