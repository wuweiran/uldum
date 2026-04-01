#include "render/renderer.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "simulation/world.h"
#include "simulation/components.h"
#include "asset/model.h"
#include "core/log.h"

#include <glm/gtc/matrix_transform.hpp>

#include <fstream>

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

// ── Init / Shutdown ────────────────────────────────────────────────────────

bool Renderer::init(rhi::VulkanRhi& rhi) {
    m_rhi = &rhi;

    f32 aspect = static_cast<f32>(rhi.extent().width) / static_cast<f32>(rhi.extent().height);
    m_camera.init(aspect);

    if (!create_mesh_pipeline()) return false;

    // Create a placeholder box mesh for entities without a real model
    asset::MeshData placeholder;
    // Box: 6 faces, each face has 4 vertices + 2 triangles
    const float s = 1.0f; // half-size
    placeholder.vertices = {
        // Top face (Y+) — visible from above
        {{-s, 2*s, -s}, {0,1,0}, {0,0}}, {{ s, 2*s, -s}, {0,1,0}, {1,0}},
        {{ s, 2*s,  s}, {0,1,0}, {1,1}}, {{-s, 2*s,  s}, {0,1,0}, {0,1}},
        // Front face (Z+)
        {{-s, 0,  s}, {0,0,1}, {0,0}}, {{ s, 0,  s}, {0,0,1}, {1,0}},
        {{ s, 2*s,  s}, {0,0,1}, {1,1}}, {{-s, 2*s,  s}, {0,0,1}, {0,1}},
        // Back face (Z-)
        {{ s, 0, -s}, {0,0,-1}, {0,0}}, {{-s, 0, -s}, {0,0,-1}, {1,0}},
        {{-s, 2*s, -s}, {0,0,-1}, {1,1}}, {{ s, 2*s, -s}, {0,0,-1}, {0,1}},
        // Right face (X+)
        {{ s, 0,  s}, {1,0,0}, {0,0}}, {{ s, 0, -s}, {1,0,0}, {1,0}},
        {{ s, 2*s, -s}, {1,0,0}, {1,1}}, {{ s, 2*s,  s}, {1,0,0}, {0,1}},
        // Left face (X-)
        {{-s, 0, -s}, {-1,0,0}, {0,0}}, {{-s, 0,  s}, {-1,0,0}, {1,0}},
        {{-s, 2*s,  s}, {-1,0,0}, {1,1}}, {{-s, 2*s, -s}, {-1,0,0}, {0,1}},
        // Bottom face (Y-)
        {{-s, 0,  s}, {0,-1,0}, {0,0}}, {{ s, 0,  s}, {0,-1,0}, {1,0}},
        {{ s, 0, -s}, {0,-1,0}, {1,1}}, {{-s, 0, -s}, {0,-1,0}, {0,1}},
    };
    placeholder.indices = {
         0, 2, 1,  0, 3, 2,   // top (reversed — original normal pointed inward)
         4, 5, 6,  4, 6, 7,   // front
         8, 9,10,  8,10,11,   // back
        12,13,14, 12,14,15,   // right
        16,17,18, 16,18,19,   // left
        20,22,21, 20,23,22,   // bottom (reversed — original normal pointed inward)
    };
    m_placeholder_mesh = upload_mesh(m_rhi->allocator(), placeholder);

    log::info(TAG, "Renderer initialized — mesh pipeline + camera ready");
    return true;
}

void Renderer::shutdown() {
    if (!m_rhi) return;
    VkDevice device = m_rhi->device();
    VmaAllocator alloc = m_rhi->allocator();

    destroy_mesh(alloc, m_placeholder_mesh);
    for (auto& [path, mesh] : m_mesh_cache) {
        destroy_mesh(alloc, mesh);
    }
    m_mesh_cache.clear();

    if (m_mesh_pipeline)        vkDestroyPipeline(device, m_mesh_pipeline, nullptr);
    if (m_mesh_pipeline_layout) vkDestroyPipelineLayout(device, m_mesh_pipeline_layout, nullptr);

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

// ── Mesh pipeline ──────────────────────────────────────────────────────────

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

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // Vertex input: matches asset::Vertex { vec3 pos, vec3 normal, vec2 texcoord }
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(asset::Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(asset::Vertex, texcoord)};

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable  = VK_FALSE; // No depth buffer yet — 5c will add it
    depth_stencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments    = &blend_attachment;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates    = dynamic_states;

    // Push constant: mat4 MVP
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset     = 0;
    push_range.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &m_mesh_pipeline_layout) != VK_SUCCESS) {
        log::error(TAG, "Failed to create mesh pipeline layout");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkFormat color_format = m_rhi->swapchain_format();
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount    = 1;
    rendering_ci.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = stages;
    pipeline_ci.pVertexInputState   = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState      = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState   = &multisample;
    pipeline_ci.pDepthStencilState  = &depth_stencil;
    pipeline_ci.pColorBlendState    = &color_blend;
    pipeline_ci.pDynamicState       = &dynamic_state;
    pipeline_ci.layout              = m_mesh_pipeline_layout;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_mesh_pipeline) != VK_SUCCESS) {
        log::error(TAG, "Failed to create mesh pipeline");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    log::info(TAG, "Mesh pipeline created");
    return true;
}

// ── Mesh cache ─────────────────────────────────────────────────────────────

GpuMesh& Renderer::get_or_upload_mesh(const std::string& model_path) {
    auto it = m_mesh_cache.find(model_path);
    if (it != m_mesh_cache.end()) return it->second;

    // Try to load from file — for now just use placeholder
    // Real model loading will read from asset manager
    log::trace(TAG, "Using placeholder mesh for '{}'", model_path);
    m_mesh_cache[model_path] = m_placeholder_mesh;
    return m_mesh_cache[model_path];
}

// ── Draw ───────────────────────────────────────────────────────────────────

void Renderer::draw(VkCommandBuffer cmd, VkExtent2D extent, const simulation::World& world) {
    if (!m_mesh_pipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_mesh_pipeline);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    glm::mat4 vp = m_camera.view_projection();

    // Iterate all entities with Transform + Renderable
    auto& transforms = world.transforms;
    auto& renderables = world.renderables;

    for (u32 i = 0; i < renderables.count(); ++i) {
        u32 id = renderables.ids()[i];
        const auto& renderable = renderables.data()[i];
        if (!renderable.visible) continue;

        const auto* transform = transforms.get(id);
        if (!transform) continue;

        // Model matrix from Transform (game coords: X=right, Y=forward, Z=up)
        glm::mat4 model = glm::translate(glm::mat4{1.0f}, transform->position);
        model = glm::rotate(model, transform->facing, glm::vec3{0.0f, 0.0f, 1.0f});  // rotate around Z (up)
        model = glm::scale(model, glm::vec3{transform->scale});
        // glTF models are Y-up; rotate -90° around X to convert to Z-up
        model = model * glm::rotate(glm::mat4{1.0f}, glm::radians(-90.0f), glm::vec3{1.0f, 0.0f, 0.0f});

        glm::mat4 mvp = vp * model;

        vkCmdPushConstants(cmd, m_mesh_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &mvp);

        GpuMesh& mesh = get_or_upload_mesh(renderable.model_path);
        if (!mesh.vertex_buffer) continue;

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
