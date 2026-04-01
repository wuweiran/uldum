#include "render/camera.h"

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

    if (m_position.z < 2.0f) m_position.z = 2.0f;
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
    m_proj = glm::perspective(m_fov, m_aspect, m_near, m_far);
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

} // namespace uldum::render
