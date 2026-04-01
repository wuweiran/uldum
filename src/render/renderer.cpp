#include "render/renderer.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "map/terrain_data.h"
#include "simulation/world.h"
#include "simulation/components.h"
#include "simulation/pathfinding.h"
#include "asset/model.h"
#include "core/log.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <fstream>
#include <vector>

namespace uldum::render {

static constexpr const char* TAG = "Render";

// ── Shader loading helper ──────────────────────────────────────────────────

static VkShaderModule load_shader(VkDevice device, std::string_view path) {
    std::string path_str(path);
    std::ifstream file(path_str, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        log::error(TAG, "Failed to open shader '{}'", path);
        return VK_NULL_HANDLE;
    }
    auto size = file.tellg();
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), size);

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}

// ── Procedural texture generation ─────────────────────────────────────────

static std::vector<u8> generate_solid_texture(u32 size, u8 r, u8 g, u8 b) {
    std::vector<u8> pixels(size * size * 4);
    for (u32 i = 0; i < size * size; ++i) {
        // Add subtle noise for visual interest
        u8 noise = static_cast<u8>((i * 7 + (i / size) * 13) % 16);
        pixels[i * 4]     = static_cast<u8>(std::min(255, r + noise));
        pixels[i * 4 + 1] = static_cast<u8>(std::min(255, g + noise));
        pixels[i * 4 + 2] = static_cast<u8>(std::min(255, b + noise / 2));
        pixels[i * 4 + 3] = 255;
    }
    return pixels;
}

// ── Terrain slope tilt helper ──────────────────────────────────────────────

// Build a rotation matrix that tilts an entity to match the terrain slope.
// terrain_normal is the surface normal at the entity position (Z-up).
static glm::mat4 slope_tilt_matrix(const glm::vec3& terrain_normal) {
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    glm::vec3 n = glm::normalize(terrain_normal);
    f32 dot = glm::dot(up, n);
    if (dot > 0.999f) return glm::mat4{1.0f};  // flat terrain, no tilt

    glm::vec3 axis = glm::cross(up, n);
    f32 axis_len = glm::length(axis);
    if (axis_len < 0.001f) return glm::mat4{1.0f};
    axis /= axis_len;

    f32 angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
    return glm::rotate(glm::mat4{1.0f}, angle, axis);
}

// ── Init / Shutdown ────────────────────────────────────────────────────────

bool Renderer::init(rhi::VulkanRhi& rhi) {
    m_rhi = &rhi;

    f32 aspect = static_cast<f32>(rhi.extent().width) / static_cast<f32>(rhi.extent().height);
    m_camera.init(aspect);

    if (!create_descriptor_layouts()) return false;
    if (!create_shadow_resources()) return false;
    if (!create_default_texture()) return false;
    if (!create_terrain_textures()) return false;
    if (!create_mesh_pipeline()) return false;
    if (!create_terrain_pipeline()) return false;
    if (!create_shadow_pipeline()) return false;

    // Create a placeholder box mesh for entities without a real model.
    // Defined directly in Z-up game coordinates: base at Z=0, top at Z=2.
    asset::MeshData placeholder;
    const float s = 1.0f;
    const float h = 2.0f;
    // UVs map to the default texture (white with warm tint from texture)
    placeholder.vertices = {
        // Top face (Z+)
        {{-s, -s, h}, {0,0,1}, {0,0}}, {{ s, -s, h}, {0,0,1}, {1,0}},
        {{ s,  s, h}, {0,0,1}, {1,1}}, {{-s,  s, h}, {0,0,1}, {0,1}},
        // Bottom face (Z-)
        {{-s,  s, 0}, {0,0,-1}, {0,0}}, {{ s,  s, 0}, {0,0,-1}, {1,0}},
        {{ s, -s, 0}, {0,0,-1}, {1,1}}, {{-s, -s, 0}, {0,0,-1}, {0,1}},
        // Front face (Y+)
        {{-s, s, 0}, {0,1,0}, {0,0}}, {{ s, s, 0}, {0,1,0}, {1,0}},
        {{ s, s, h}, {0,1,0}, {1,1}}, {{-s, s, h}, {0,1,0}, {0,1}},
        // Back face (Y-)
        {{ s, -s, 0}, {0,-1,0}, {0,0}}, {{-s, -s, 0}, {0,-1,0}, {1,0}},
        {{-s, -s, h}, {0,-1,0}, {1,1}}, {{ s, -s, h}, {0,-1,0}, {0,1}},
        // Right face (X+)
        {{ s, s, 0}, {1,0,0}, {0,0}}, {{ s, -s, 0}, {1,0,0}, {1,0}},
        {{ s, -s, h}, {1,0,0}, {1,1}}, {{ s,  s, h}, {1,0,0}, {0,1}},
        // Left face (X-)
        {{-s, -s, 0}, {-1,0,0}, {0,0}}, {{-s,  s, 0}, {-1,0,0}, {1,0}},
        {{-s,  s, h}, {-1,0,0}, {1,1}}, {{-s, -s, h}, {-1,0,0}, {0,1}},
    };
    // Winding convention: cross(e1, e2) must equal outward face normal (matches terrain).
    placeholder.indices = {
         0, 1, 3,  1, 2, 3,   // top    (+Z)
         4, 5, 7,  5, 6, 7,   // bottom (-Z)
         8,10, 9,  8,11,10,   // front  (+Y)
        12,14,13, 12,15,14,   // back   (-Y)
        16,18,17, 16,19,18,   // right  (+X)
        20,22,21, 20,23,22,   // left   (-X)
    };
    m_placeholder_mesh = upload_mesh(m_rhi->allocator(), placeholder);
    m_placeholder_mesh.native_z_up = true;

    log::info(TAG, "Renderer initialized — textured mesh + terrain pipelines ready");
    return true;
}

void Renderer::shutdown() {
    if (!m_rhi) return;
    VkDevice device = m_rhi->device();
    VmaAllocator alloc = m_rhi->allocator();

    vkDeviceWaitIdle(device);

    destroy_terrain_mesh(alloc, m_terrain);
    destroy_mesh(alloc, m_placeholder_mesh);
    for (auto& [path, mesh] : m_mesh_cache) {
        destroy_mesh(alloc, mesh);
    }
    m_mesh_cache.clear();

    // Destroy textures
    destroy_texture(*m_rhi, m_default_texture);
    for (u32 i = 0; i < m_terrain_material.layer_count; ++i) {
        destroy_texture(*m_rhi, m_terrain_material.layers[i]);
    }
    destroy_texture(*m_rhi, m_terrain_material.splatmap);

    // Destroy shadow resources
    destroy_shadow_map(*m_rhi, m_shadow_map);
    destroy_shadow_buffer(*m_rhi, m_shadow_ubo);

    // Destroy pipelines
    if (m_shadow_pipeline)         vkDestroyPipeline(device, m_shadow_pipeline, nullptr);
    if (m_shadow_pipeline_layout)  vkDestroyPipelineLayout(device, m_shadow_pipeline_layout, nullptr);
    if (m_terrain_pipeline)        vkDestroyPipeline(device, m_terrain_pipeline, nullptr);
    if (m_terrain_pipeline_layout) vkDestroyPipelineLayout(device, m_terrain_pipeline_layout, nullptr);
    if (m_mesh_pipeline)           vkDestroyPipeline(device, m_mesh_pipeline, nullptr);
    if (m_mesh_pipeline_layout)    vkDestroyPipelineLayout(device, m_mesh_pipeline_layout, nullptr);

    // Destroy descriptor infrastructure
    if (m_descriptor_pool)      vkDestroyDescriptorPool(device, m_descriptor_pool, nullptr);
    if (m_shadow_desc_layout)   vkDestroyDescriptorSetLayout(device, m_shadow_desc_layout, nullptr);
    if (m_terrain_desc_layout)  vkDestroyDescriptorSetLayout(device, m_terrain_desc_layout, nullptr);
    if (m_mesh_desc_layout)     vkDestroyDescriptorSetLayout(device, m_mesh_desc_layout, nullptr);

    m_rhi = nullptr;
    log::info(TAG, "Renderer shut down");
}

// ── Camera ─────────────────────────────────────────────────────────────────

void Renderer::update_camera(const platform::InputState& input, f32 dt) {
    m_camera.update(input, dt);
}

void Renderer::handle_resize(f32 aspect) {
    m_camera.set_aspect(aspect);
}

void Renderer::set_terrain(const map::TerrainData& terrain) {
    VmaAllocator alloc = m_rhi->allocator();
    destroy_terrain_mesh(alloc, m_terrain);
    m_terrain = build_terrain_mesh(alloc, terrain);

    // Generate splatmap from tile_type data
    if (terrain.is_valid() && !terrain.tile_type.empty()) {
        destroy_texture(*m_rhi, m_terrain_material.splatmap);

        // Build RGBA splatmap: each pixel = blend weights for one tile
        u32 w = terrain.tiles_x;
        u32 h = terrain.tiles_y;
        std::vector<u8> splat_pixels(w * h * 4, 0);
        for (u32 i = 0; i < w * h; ++i) {
            u8 type = terrain.tile_type[i];
            // Set the channel corresponding to the tile type to 255
            u32 channel = std::min(static_cast<u32>(type), 3u);
            splat_pixels[i * 4 + channel] = 255;
        }
        m_terrain_material.splatmap = upload_texture_rgba(*m_rhi, splat_pixels.data(), w, h);

        // Re-allocate terrain descriptor set with new splatmap
        m_terrain_material.descriptor_set = allocate_terrain_descriptor(m_terrain_material);
        log::info(TAG, "Terrain splatmap generated: {}x{}", w, h);
    }
}

// ── Descriptor set layouts + pool ─────────────────────────────────────────

bool Renderer::create_descriptor_layouts() {
    VkDevice device = m_rhi->device();

    // Mesh descriptor set layout: 1 combined image sampler at binding 0
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 1;
        ci.pBindings    = &binding;

        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_mesh_desc_layout) != VK_SUCCESS) {
            log::error(TAG, "Failed to create mesh descriptor set layout");
            return false;
        }
    }

    // Terrain descriptor set layout: 5 combined image samplers (4 layers + 1 splatmap)
    {
        VkDescriptorSetLayoutBinding bindings[5]{};
        for (u32 i = 0; i < 5; ++i) {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 5;
        ci.pBindings    = bindings;

        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_terrain_desc_layout) != VK_SUCCESS) {
            log::error(TAG, "Failed to create terrain descriptor set layout");
            return false;
        }
    }

    // Shadow descriptor set layout: UBO (binding 0) + shadow map sampler (binding 1)
    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 2;
        ci.pBindings    = bindings;

        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_shadow_desc_layout) != VK_SUCCESS) {
            log::error(TAG, "Failed to create shadow descriptor set layout");
            return false;
        }
    }

    // Descriptor pool: enough for mesh materials + terrain + shadow
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 64;
    pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = 4;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_ci.maxSets       = 32;
    pool_ci.poolSizeCount = 2;
    pool_ci.pPoolSizes    = pool_sizes;

    if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &m_descriptor_pool) != VK_SUCCESS) {
        log::error(TAG, "Failed to create descriptor pool");
        return false;
    }

    log::info(TAG, "Descriptor layouts and pool created");
    return true;
}

VkDescriptorSet Renderer::allocate_mesh_descriptor(const GpuTexture& diffuse) {
    VkDevice device = m_rhi->device();

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = m_descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts        = &m_mesh_desc_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) {
        log::error(TAG, "Failed to allocate mesh descriptor set");
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo img_info{};
    img_info.sampler     = diffuse.sampler;
    img_info.imageView   = diffuse.view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &img_info;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return set;
}

VkDescriptorSet Renderer::allocate_terrain_descriptor(const TerrainMaterial& mat) {
    VkDevice device = m_rhi->device();

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = m_descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts        = &m_terrain_desc_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) {
        log::error(TAG, "Failed to allocate terrain descriptor set");
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo img_infos[5]{};
    VkWriteDescriptorSet writes[5]{};

    // Layer textures (bindings 0-3)
    for (u32 i = 0; i < 4; ++i) {
        const GpuTexture& tex = (i < mat.layer_count) ? mat.layers[i] : m_default_texture;
        img_infos[i].sampler     = tex.sampler;
        img_infos[i].imageView   = tex.view;
        img_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = set;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo      = &img_infos[i];
    }

    // Splatmap (binding 4)
    const GpuTexture& splat = mat.splatmap.image ? mat.splatmap : m_default_texture;
    img_infos[4].sampler     = splat.sampler;
    img_infos[4].imageView   = splat.view;
    img_infos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet          = set;
    writes[4].dstBinding      = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].pImageInfo      = &img_infos[4];

    vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
    return set;
}

// ── Default + terrain textures ────────────────────────────────────────────

bool Renderer::create_default_texture() {
    // 4x4 warm orange texture for placeholder meshes
    auto pixels = generate_solid_texture(4, 220, 160, 80);
    m_default_texture = upload_texture_rgba(*m_rhi, pixels.data(), 4, 4);
    if (!m_default_texture.image) return false;

    m_default_material.diffuse = m_default_texture;
    m_default_material.descriptor_set = allocate_mesh_descriptor(m_default_texture);
    log::info(TAG, "Default texture created");
    return true;
}

bool Renderer::create_terrain_textures() {
    // Generate procedural ground textures (32x32 each)
    constexpr u32 TEX_SIZE = 32;

    // Layer 0: grass (green)
    auto grass = generate_solid_texture(TEX_SIZE, 60, 140, 40);
    m_terrain_material.layers[0] = upload_texture_rgba(*m_rhi, grass.data(), TEX_SIZE, TEX_SIZE);

    // Layer 1: dirt (brown)
    auto dirt = generate_solid_texture(TEX_SIZE, 140, 100, 50);
    m_terrain_material.layers[1] = upload_texture_rgba(*m_rhi, dirt.data(), TEX_SIZE, TEX_SIZE);

    // Layer 2: stone (gray)
    auto stone = generate_solid_texture(TEX_SIZE, 130, 130, 120);
    m_terrain_material.layers[2] = upload_texture_rgba(*m_rhi, stone.data(), TEX_SIZE, TEX_SIZE);

    // Layer 3: sand (tan) — unused for now but available
    auto sand = generate_solid_texture(TEX_SIZE, 200, 180, 120);
    m_terrain_material.layers[3] = upload_texture_rgba(*m_rhi, sand.data(), TEX_SIZE, TEX_SIZE);

    m_terrain_material.layer_count = 4;

    // Splatmap will be generated in set_terrain() from TerrainData::tile_type
    log::info(TAG, "Terrain textures created (4 layers)");
    return true;
}

// ── Pipeline creation helpers ─────────────────────────────────────────────

// Shared pipeline state (vertex input, rasterizer, depth, blend, dynamic)
struct PipelineStateConfig {
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkPipelineInputAssemblyStateCreateInfo input_assembly;
    VkPipelineViewportStateCreateInfo viewport_state;
    VkPipelineRasterizationStateCreateInfo rasterizer;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineDepthStencilStateCreateInfo depth_stencil;
    VkPipelineColorBlendAttachmentState blend_attachment;
    VkPipelineColorBlendStateCreateInfo color_blend;
    VkPipelineDynamicStateCreateInfo dynamic_state;
    VkDynamicState dynamic_states[2];
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[3];
};

static PipelineStateConfig make_common_pipeline_state() {
    PipelineStateConfig cfg{};

    cfg.binding.binding   = 0;
    cfg.binding.stride    = sizeof(asset::Vertex);
    cfg.binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    cfg.attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)};
    cfg.attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, normal)};
    cfg.attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(asset::Vertex, texcoord)};

    cfg.vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    cfg.vertex_input.vertexBindingDescriptionCount   = 1;
    cfg.vertex_input.pVertexBindingDescriptions      = &cfg.binding;
    cfg.vertex_input.vertexAttributeDescriptionCount = 3;
    cfg.vertex_input.pVertexAttributeDescriptions    = cfg.attrs;

    cfg.input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    cfg.input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    cfg.viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    cfg.viewport_state.viewportCount = 1;
    cfg.viewport_state.scissorCount  = 1;

    cfg.rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    cfg.rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    cfg.rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    cfg.rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    cfg.rasterizer.lineWidth   = 1.0f;

    cfg.multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    cfg.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    cfg.depth_stencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    cfg.depth_stencil.depthTestEnable  = VK_TRUE;
    cfg.depth_stencil.depthWriteEnable = VK_TRUE;
    cfg.depth_stencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    cfg.blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    cfg.color_blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cfg.color_blend.attachmentCount = 1;
    cfg.color_blend.pAttachments    = &cfg.blend_attachment;

    cfg.dynamic_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
    cfg.dynamic_states[1] = VK_DYNAMIC_STATE_SCISSOR;
    cfg.dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    cfg.dynamic_state.dynamicStateCount = 2;
    cfg.dynamic_state.pDynamicStates    = cfg.dynamic_states;

    return cfg;
}

bool Renderer::create_mesh_pipeline() {
    VkDevice device = m_rhi->device();

    VkShaderModule vert = load_shader(device, "engine/shaders/mesh.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/mesh.frag.spv");
    if (!vert || !frag) {
        log::error(TAG, "Failed to load mesh shaders");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    auto cfg = make_common_pipeline_state();

    cfg.stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    cfg.stages[0].module = vert;
    cfg.stages[0].pName  = "main";
    cfg.stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    cfg.stages[1].module = frag;
    cfg.stages[1].pName  = "main";

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset     = 0;
    push_range.size       = 2 * sizeof(glm::mat4);

    VkDescriptorSetLayout mesh_layouts[] = {m_mesh_desc_layout, m_shadow_desc_layout};
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount         = 2;
    layout_ci.pSetLayouts            = mesh_layouts;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &m_mesh_pipeline_layout) != VK_SUCCESS) {
        log::error(TAG, "Failed to create mesh pipeline layout");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkFormat color_format = m_rhi->swapchain_format();
    VkFormat depth_format = m_rhi->depth_format();
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount    = 1;
    rendering_ci.pColorAttachmentFormats = &color_format;
    rendering_ci.depthAttachmentFormat   = depth_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = cfg.stages;
    pipeline_ci.pVertexInputState   = &cfg.vertex_input;
    pipeline_ci.pInputAssemblyState = &cfg.input_assembly;
    pipeline_ci.pViewportState      = &cfg.viewport_state;
    pipeline_ci.pRasterizationState = &cfg.rasterizer;
    pipeline_ci.pMultisampleState   = &cfg.multisample;
    pipeline_ci.pDepthStencilState  = &cfg.depth_stencil;
    pipeline_ci.pColorBlendState    = &cfg.color_blend;
    pipeline_ci.pDynamicState       = &cfg.dynamic_state;
    pipeline_ci.layout              = m_mesh_pipeline_layout;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_mesh_pipeline) != VK_SUCCESS) {
        log::error(TAG, "Failed to create mesh pipeline");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    log::info(TAG, "Mesh pipeline created (textured + shadow)");
    return true;
}

bool Renderer::create_terrain_pipeline() {
    VkDevice device = m_rhi->device();

    VkShaderModule vert = load_shader(device, "engine/shaders/terrain.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/terrain.frag.spv");
    if (!vert || !frag) {
        log::error(TAG, "Failed to load terrain shaders");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    auto cfg = make_common_pipeline_state();

    cfg.stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    cfg.stages[0].module = vert;
    cfg.stages[0].pName  = "main";
    cfg.stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    cfg.stages[1].module = frag;
    cfg.stages[1].pName  = "main";

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset     = 0;
    push_range.size       = 2 * sizeof(glm::mat4);

    VkDescriptorSetLayout terrain_layouts[] = {m_terrain_desc_layout, m_shadow_desc_layout};
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount         = 2;
    layout_ci.pSetLayouts            = terrain_layouts;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &m_terrain_pipeline_layout) != VK_SUCCESS) {
        log::error(TAG, "Failed to create terrain pipeline layout");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkFormat color_format = m_rhi->swapchain_format();
    VkFormat depth_format = m_rhi->depth_format();
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount    = 1;
    rendering_ci.pColorAttachmentFormats = &color_format;
    rendering_ci.depthAttachmentFormat   = depth_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = cfg.stages;
    pipeline_ci.pVertexInputState   = &cfg.vertex_input;
    pipeline_ci.pInputAssemblyState = &cfg.input_assembly;
    pipeline_ci.pViewportState      = &cfg.viewport_state;
    pipeline_ci.pRasterizationState = &cfg.rasterizer;
    pipeline_ci.pMultisampleState   = &cfg.multisample;
    pipeline_ci.pDepthStencilState  = &cfg.depth_stencil;
    pipeline_ci.pColorBlendState    = &cfg.color_blend;
    pipeline_ci.pDynamicState       = &cfg.dynamic_state;
    pipeline_ci.layout              = m_terrain_pipeline_layout;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_terrain_pipeline) != VK_SUCCESS) {
        log::error(TAG, "Failed to create terrain pipeline");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    log::info(TAG, "Terrain pipeline created (splatmap + shadow)");
    return true;
}

// ── Mesh cache ─────────────────────────────────────────────────────────────

GpuMesh& Renderer::get_or_upload_mesh(const std::string& model_path) {
    auto it = m_mesh_cache.find(model_path);
    if (it != m_mesh_cache.end()) return it->second;

    log::trace(TAG, "Using placeholder mesh for '{}'", model_path);
    m_mesh_cache[model_path] = m_placeholder_mesh;
    return m_mesh_cache[model_path];
}

// ── Shadow pipeline + resources ────────────────────────────────────────────

bool Renderer::create_shadow_pipeline() {
    VkDevice device = m_rhi->device();

    VkShaderModule vert = load_shader(device, "engine/shaders/shadow.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/shadow.frag.spv");
    if (!vert || !frag) {
        log::error(TAG, "Failed to load shadow shaders");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    auto cfg = make_common_pipeline_state();
    cfg.stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    cfg.stages[0].module = vert;
    cfg.stages[0].pName  = "main";
    cfg.stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    cfg.stages[1].module = frag;
    cfg.stages[1].pName  = "main";

    cfg.rasterizer.depthBiasEnable         = VK_TRUE;
    cfg.rasterizer.depthBiasConstantFactor = 2.0f;
    cfg.rasterizer.depthBiasSlopeFactor    = 1.5f;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset     = 0;
    push_range.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &m_shadow_pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                 = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.depthAttachmentFormat = depth_format;

    cfg.color_blend.attachmentCount = 0;
    cfg.color_blend.pAttachments    = nullptr;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = cfg.stages;
    pipeline_ci.pVertexInputState   = &cfg.vertex_input;
    pipeline_ci.pInputAssemblyState = &cfg.input_assembly;
    pipeline_ci.pViewportState      = &cfg.viewport_state;
    pipeline_ci.pRasterizationState = &cfg.rasterizer;
    pipeline_ci.pMultisampleState   = &cfg.multisample;
    pipeline_ci.pDepthStencilState  = &cfg.depth_stencil;
    pipeline_ci.pColorBlendState    = &cfg.color_blend;
    pipeline_ci.pDynamicState       = &cfg.dynamic_state;
    pipeline_ci.layout              = m_shadow_pipeline_layout;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_shadow_pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    log::info(TAG, "Shadow pipeline created (depth-only)");
    return true;
}

bool Renderer::create_shadow_resources() {
    if (!create_shadow_map(*m_rhi, m_shadow_map)) return false;
    if (!create_shadow_buffer(*m_rhi, m_shadow_ubo)) return false;
    m_shadow_desc_set = allocate_shadow_descriptor();
    return m_shadow_desc_set != VK_NULL_HANDLE;
}

VkDescriptorSet Renderer::allocate_shadow_descriptor() {
    VkDevice device = m_rhi->device();

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = m_descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts        = &m_shadow_desc_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) {
        log::error(TAG, "Failed to allocate shadow descriptor set");
        return VK_NULL_HANDLE;
    }

    VkDescriptorBufferInfo buf_info{};
    buf_info.buffer = m_shadow_ubo.buffer;
    buf_info.offset = 0;
    buf_info.range  = sizeof(ShadowUBO);

    VkDescriptorImageInfo img_info{};
    img_info.sampler     = m_shadow_map.sampler;
    img_info.imageView   = m_shadow_map.depth_view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo     = &buf_info;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = set;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo      = &img_info;

    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    log::info(TAG, "Shadow descriptor set allocated");
    return set;
}

// ── Shadow depth pass ─────────────────────────────────────────────────────

void Renderer::draw_shadow_pass(VkCommandBuffer cmd, const simulation::World& world) {
    if (!m_shadow_pipeline) return;

    glm::vec3 light_dir{0.3f, -0.5f, 0.8f};
    glm::vec3 scene_center{64.0f, 64.0f, 2.5f};
    f32 scene_radius = 80.0f;
    glm::mat4 light_vp = compute_light_vp(light_dir, scene_center, scene_radius);

    ShadowUBO ubo{light_vp};
    std::memcpy(m_shadow_ubo.mapped, &ubo, sizeof(ubo));

    VkImageMemoryBarrier2 to_depth{};
    to_depth.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_depth.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_depth.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_depth.dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    to_depth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_depth.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    to_depth.newLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    to_depth.image         = m_shadow_map.depth_image;
    to_depth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_depth;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView   = m_shadow_map.depth_view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType            = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea       = {{0, 0}, {m_shadow_map.size, m_shadow_map.size}};
    rendering.layerCount       = 1;
    rendering.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadow_pipeline);

    VkViewport vp{};
    vp.width    = static_cast<float>(m_shadow_map.size);
    vp.height   = static_cast<float>(m_shadow_map.size);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{{0, 0}, {m_shadow_map.size, m_shadow_map.size}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Terrain
    if (m_terrain.gpu_mesh.vertex_buffer) {
        glm::mat4 light_mvp = light_vp;
        vkCmdPushConstants(cmd, m_shadow_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &light_mvp);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_terrain.gpu_mesh.vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_terrain.gpu_mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_terrain.gpu_mesh.index_count, 1, 0, 0, 0);
    }

    // Entities
    auto& transforms = world.transforms;
    auto& renderables = world.renderables;
    for (u32 i = 0; i < renderables.count(); ++i) {
        u32 id = renderables.ids()[i];
        const auto& renderable = renderables.data()[i];
        if (!renderable.visible) continue;
        const auto* transform = transforms.get(id);
        if (!transform) continue;
        GpuMesh& mesh = get_or_upload_mesh(renderable.model_path);
        if (!mesh.vertex_buffer) continue;

        glm::mat4 model = glm::translate(glm::mat4{1.0f}, transform->position);
        if (m_pathfinder) {
            glm::vec3 normal = m_pathfinder->sample_normal(transform->position.x, transform->position.y);
            model = model * slope_tilt_matrix(normal);
        }
        model = glm::rotate(model, transform->facing, glm::vec3{0.0f, 0.0f, 1.0f});
        model = glm::scale(model, glm::vec3{transform->scale});
        if (!mesh.native_z_up) {
            model = model * glm::rotate(glm::mat4{1.0f}, glm::radians(-90.0f), glm::vec3{1.0f, 0.0f, 0.0f});
        }

        glm::mat4 light_mvp = light_vp * model;
        vkCmdPushConstants(cmd, m_shadow_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &light_mvp);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer, &offset);
        if (mesh.index_buffer) {
            vkCmdBindIndexBuffer(cmd, mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh.index_count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd, mesh.vertex_count, 1, 0, 0);
        }
    }

    vkCmdEndRendering(cmd);

    // Transition shadow map for sampling
    VkImageMemoryBarrier2 to_read{};
    to_read.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_read.srcStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_read.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    to_read.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    to_read.image         = m_shadow_map.depth_image;
    to_read.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    dep.pImageMemoryBarriers = &to_read;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// ── Draw ───────────────────────────────────────────────────────────────────

void Renderer::draw_shadows(VkCommandBuffer cmd, const simulation::World& world) {
    draw_shadow_pass(cmd, world);
}

void Renderer::draw(VkCommandBuffer cmd, VkExtent2D extent, const simulation::World& world) {
    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{{0, 0}, extent};
    glm::mat4 vp = m_camera.view_projection();

    // ── Draw terrain with splatmap pipeline ──────────────────────────────
    if (m_terrain_pipeline && m_terrain.gpu_mesh.vertex_buffer && m_terrain_material.descriptor_set) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_terrain_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkDescriptorSet terrain_sets[] = {m_terrain_material.descriptor_set, m_shadow_desc_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_terrain_pipeline_layout, 0, 2, terrain_sets, 0, nullptr);

        glm::mat4 terrain_model{1.0f};
        glm::mat4 terrain_mvp = vp * terrain_model;
        struct { glm::mat4 mvp; glm::mat4 model; } terrain_push{terrain_mvp, terrain_model};
        vkCmdPushConstants(cmd, m_terrain_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(terrain_push), &terrain_push);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_terrain.gpu_mesh.vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_terrain.gpu_mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_terrain.gpu_mesh.index_count, 1, 0, 0, 0);
    }

    // ── Draw entities with mesh pipeline ─────────────────────────────────
    if (!m_mesh_pipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_mesh_pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind material (set 0) + shadow (set 1)
    if (m_default_material.descriptor_set && m_shadow_desc_set) {
        VkDescriptorSet mesh_sets[] = {m_default_material.descriptor_set, m_shadow_desc_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_mesh_pipeline_layout, 0, 2, mesh_sets, 0, nullptr);
    }

    auto& transforms = world.transforms;
    auto& renderables = world.renderables;

    for (u32 i = 0; i < renderables.count(); ++i) {
        u32 id = renderables.ids()[i];
        const auto& renderable = renderables.data()[i];
        if (!renderable.visible) continue;

        const auto* transform = transforms.get(id);
        if (!transform) continue;

        GpuMesh& mesh = get_or_upload_mesh(renderable.model_path);
        if (!mesh.vertex_buffer) continue;

        glm::mat4 model = glm::translate(glm::mat4{1.0f}, transform->position);
        // Terrain slope tilt (visual only)
        if (m_pathfinder) {
            glm::vec3 normal = m_pathfinder->sample_normal(transform->position.x, transform->position.y);
            model = model * slope_tilt_matrix(normal);
        }
        model = glm::rotate(model, transform->facing, glm::vec3{0.0f, 0.0f, 1.0f});
        model = glm::scale(model, glm::vec3{transform->scale});
        if (!mesh.native_z_up) {
            model = model * glm::rotate(glm::mat4{1.0f}, glm::radians(-90.0f), glm::vec3{1.0f, 0.0f, 0.0f});
        }

        glm::mat4 mvp = vp * model;
        struct { glm::mat4 mvp; glm::mat4 model; } push{mvp, model};
        vkCmdPushConstants(cmd, m_mesh_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(push), &push);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer, &offset);

        if (mesh.index_buffer) {
            vkCmdBindIndexBuffer(cmd, mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh.index_count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd, mesh.vertex_count, 1, 0, 0);
        }
    }
}

} // namespace uldum::render
