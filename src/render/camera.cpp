#include "render/camera.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace uldum::render {

void Camera::init(f32 aspect_ratio) {
    m_aspect = aspect_ratio;
    m_dirty = true;
}

void Camera::update(const platform::InputState& input, f32 dt) {
    // Game coordinates: X=right, Y=forward, Z=up
    // Forward/backward movement is along Y axis on the ground plane
    glm::vec3 forward{-std::sin(m_yaw), std::cos(m_yaw), 0.0f};
    glm::vec3 right{std::cos(m_yaw), std::sin(m_yaw), 0.0f};

    glm::vec3 move{0.0f};
    if (input.key_w) move += forward;
    if (input.key_s) move -= forward;
    if (input.key_d) move += right;
    if (input.key_a) move -= right;

    if (glm::length(move) > 0.001f) {
        move = glm::normalize(move) * m_move_speed * dt;
        m_position += move;
        m_dirty = true;
    }

    // Height (Z axis)
    if (input.key_e) { m_position.z += m_height_speed * dt; m_dirty = true; }
    if (input.key_q) { m_position.z -= m_height_speed * dt; m_dirty = true; }

    if (m_position.z < 128.0f) m_position.z = 128.0f;
}

void Camera::pan(f32 dx, f32 dy) {
    // Pan camera on XY ground plane. Scale by height so panning feels consistent at any zoom.
    f32 scale = m_position.z * 0.002f;
    glm::vec3 right{std::cos(m_yaw), std::sin(m_yaw), 0.0f};
    glm::vec3 forward{-std::sin(m_yaw), std::cos(m_yaw), 0.0f};
    m_position -= right * dx * scale;
    m_position += forward * dy * scale;
    m_dirty = true;
}

void Camera::translate(f32 dx, f32 dy) {
    m_position.x += dx;
    m_position.y += dy;
    m_dirty = true;
}

void Camera::zoom(f32 scroll_delta) {
    // Move along view direction. Scale step by current height for consistent feel.
    f32 step = m_position.z * 0.15f * scroll_delta;
    glm::vec3 dir = forward_dir();
    m_position += dir * step;
    if (m_position.z < 128.0f) m_position.z = 128.0f;
    m_dirty = true;
}

glm::vec3 Camera::forward_dir() const {
    return {
        -std::cos(m_pitch) * std::sin(m_yaw),
         std::cos(m_pitch) * std::cos(m_yaw),
         std::sin(m_pitch)
    };
}

void Camera::recalculate() {
    if (!m_dirty) return;

    // Look direction from pitch + yaw in game coordinates (Z-up)
    glm::vec3 dir{
        -std::cos(m_pitch) * std::sin(m_yaw),
        std::cos(m_pitch) * std::cos(m_yaw),
        std::sin(m_pitch)
    };

    glm::vec3 target = m_position + dir;
    glm::vec3 up{0.0f, 0.0f, 1.0f};  // Z is up in game coordinates

    m_view = glm::lookAt(m_position, target, up);
    m_proj = glm::perspectiveRH_ZO(m_fov, m_aspect, m_near, m_far);
    // Flip Y for Vulkan clip space
    m_proj[1][1] *= -1.0f;

    m_dirty = false;
}

glm::mat4 Camera::view_matrix() const {
    const_cast<Camera*>(this)->recalculate();
    return m_view;
}

glm::mat4 Camera::projection_matrix() const {
    const_cast<Camera*>(this)->recalculate();
    return m_proj;
}

glm::mat4 Camera::view_projection() const {
    return projection_matrix() * view_matrix();
}

// ── Frustum culling ──────────────────────────────────────────────────────

Camera::Frustum Camera::frustum() const {
    // Gribb/Hartmann: extract 6 planes from the VP matrix.
    glm::mat4 vp = view_projection();
    Frustum f{};
    // Left
    f.planes[0] = {vp[0][3] + vp[0][0], vp[1][3] + vp[1][0],
                    vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]};
    // Right
    f.planes[1] = {vp[0][3] - vp[0][0], vp[1][3] - vp[1][0],
                    vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]};
    // Bottom
    f.planes[2] = {vp[0][3] + vp[0][1], vp[1][3] + vp[1][1],
                    vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]};
    // Top
    f.planes[3] = {vp[0][3] - vp[0][1], vp[1][3] - vp[1][1],
                    vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]};
    // Near
    f.planes[4] = {vp[0][2], vp[1][2], vp[2][2], vp[3][2]};
    // Far
    f.planes[5] = {vp[0][3] - vp[0][2], vp[1][3] - vp[1][2],
                    vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]};

    // Normalize each plane
    for (auto& p : f.planes) {
        f32 len = glm::length(glm::vec3(p));
        if (len > 0.0f) p /= len;
    }
    return f;
}

bool Camera::Frustum::is_sphere_visible(const glm::vec3& center, f32 radius) const {
    for (const auto& p : planes) {
        f32 dist = glm::dot(glm::vec3(p), center) + p.w;
        if (dist < -radius) return false;
    }
    return true;
}

} // namespace uldum::render
