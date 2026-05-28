#include "render/camera.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace uldum::render {

void Camera::init(f32 aspect_ratio) {
    m_aspect = aspect_ratio;
    m_dirty = true;
}

glm::vec3 Camera::forward_dir() const {
    return {
        -std::cos(m_pitch_rad) * std::sin(m_yaw_rad),
         std::cos(m_pitch_rad) * std::cos(m_yaw_rad),
         std::sin(m_pitch_rad)
    };
}

glm::vec3 Camera::position() const {
    return m_target - m_distance * forward_dir();
}

void Camera::update(const platform::InputState& input, f32 dt) {
    // Free-fly: WASD pans the target on the XY ground plane; Q/E
    // adjusts eye distance (zoom out / in). Eye follows from the
    // derived position; target.z stays at its authored value.
    glm::vec3 forward{-std::sin(m_yaw_rad), std::cos(m_yaw_rad), 0.0f};
    glm::vec3 right{std::cos(m_yaw_rad), std::sin(m_yaw_rad), 0.0f};

    glm::vec3 move{0.0f};
    if (input.key_w) move += forward;
    if (input.key_s) move -= forward;
    if (input.key_d) move += right;
    if (input.key_a) move -= right;

    if (glm::length(move) > 0.001f) {
        move = glm::normalize(move) * m_move_speed * dt;
        m_target.x += move.x;
        m_target.y += move.y;
        clamp_target_xy_to_bounds();
        m_dirty = true;
    }

    if (input.key_q) { m_distance += m_zoom_speed * dt; m_dirty = true; }
    if (input.key_e) { m_distance -= m_zoom_speed * dt; m_dirty = true; }
    if (m_distance < kMinDistance) m_distance = kMinDistance;
}

void Camera::pan(f32 dx, f32 dy) {
    // Right-click drag pan. Step scales with eye height so panning
    // feels consistent at any zoom; height ≈ distance * sin(-pitch).
    f32 height = m_distance * std::sin(-m_pitch_rad);
    if (height < kMinDistance) height = kMinDistance;
    f32 scale = height * 0.002f;
    glm::vec3 right{std::cos(m_yaw_rad), std::sin(m_yaw_rad), 0.0f};
    glm::vec3 forward{-std::sin(m_yaw_rad), std::cos(m_yaw_rad), 0.0f};
    glm::vec3 delta = -right * dx * scale + forward * dy * scale;
    m_target.x += delta.x;
    m_target.y += delta.y;
    clamp_target_xy_to_bounds();
    m_dirty = true;
}

void Camera::translate(f32 dx, f32 dy) {
    m_target.x += dx;
    m_target.y += dy;
    clamp_target_xy_to_bounds();
    m_dirty = true;
}

void Camera::set_target_xy(f32 x, f32 y) {
    m_target.x = x;
    m_target.y = y;
    clamp_target_xy_to_bounds();
    m_dirty = true;
}

void Camera::clamp_target_xy_to_bounds() {
    if (m_target.x < m_bounds_min.x) m_target.x = m_bounds_min.x;
    if (m_target.x > m_bounds_max.x) m_target.x = m_bounds_max.x;
    if (m_target.y < m_bounds_min.y) m_target.y = m_bounds_min.y;
    if (m_target.y > m_bounds_max.y) m_target.y = m_bounds_max.y;
}

void Camera::zoom(f32 scroll_delta) {
    // Negative scroll_delta is the conventional "scroll up = zoom in"
    // — shorten distance. Step proportional to current distance so the
    // feel is smooth across the whole range.
    f32 step = m_distance * 0.15f * scroll_delta;
    m_distance -= step;
    if (m_distance < kMinDistance) m_distance = kMinDistance;
    m_dirty = true;
}

void Camera::recalculate() {
    if (!m_dirty) return;
    glm::vec3 eye = position();
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    m_view = glm::lookAt(eye, m_target, up);
#if defined(ULDUM_BACKEND_GLES)
    // GLES NDC: y-up, z in [-1, +1]. Use the NO (-1..+1 depth) variant
    // and don't flip Y — that flip is a Vulkan-only correction (Vulkan
    // NDC has y-down).
    m_proj = glm::perspectiveRH_NO(m_fov, m_aspect, m_near, m_far);
#else
    // Vulkan NDC: y-down, z in [0, 1]. Flip Y to match.
    m_proj = glm::perspectiveRH_ZO(m_fov, m_aspect, m_near, m_far);
    m_proj[1][1] *= -1.0f;
#endif
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
    glm::mat4 vp = view_projection();
    Frustum f{};
    f.planes[0] = {vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]};
    f.planes[1] = {vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]};
    f.planes[2] = {vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]};
    f.planes[3] = {vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]};
    f.planes[4] = {vp[0][2], vp[1][2], vp[2][2], vp[3][2]};
    f.planes[5] = {vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]};
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
