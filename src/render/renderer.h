#pragma once

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::render {

class Renderer {
public:
    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    void begin_frame();
    void end_frame();

    // Future API:
    // void submit_mesh(MeshHandle mesh, const Transform& transform, MaterialHandle material);
    // void submit_terrain(const Terrain& terrain);
    // void submit_particles(const ParticleSystem& particles);
    // void set_camera(const Camera& camera);

private:
    rhi::VulkanRhi* m_rhi = nullptr;
};

} // namespace uldum::render
