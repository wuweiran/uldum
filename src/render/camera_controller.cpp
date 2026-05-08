#include "render/camera_controller.h"

#include <glm/common.hpp>

#include <cmath>
#include <cstdlib>

namespace uldum::render {

namespace {

f32 frand() { return static_cast<f32>(std::rand()) / static_cast<f32>(RAND_MAX); }

// Compute the XY offset from the camera's position to the point where
// its forward ray hits the ground plane (z=0). Used by lock-to-unit
// to keep the unit centered on the ground-hit regardless of pitch /
// yaw / height changes mid-lock.
glm::vec2 ground_hit_xy_offset(const Camera& cam) {
    glm::vec3 dir = cam.forward_dir();
    if (dir.z >= -1e-4f) return {0, 0};   // looking sideways / up: nothing to lock to
    f32 t = -cam.position().z / dir.z;
    return { dir.x * t, dir.y * t };
}

} // namespace

void CameraController::update(f32 dt, const UnitPosFn& get_unit_xy) {
    if (!m_camera) return;

    // Step 1: undo last frame's shake offset so it doesn't compound
    // on top of pan / lock / preset input below.
    if (m_shake_offset.x != 0 || m_shake_offset.y != 0) {
        m_camera->set_position_xy(
            m_camera->position().x - m_shake_offset.x,
            m_camera->position().y - m_shake_offset.y);
        m_shake_offset = {0, 0};
    }

    // Step 2: lock wins. Per-frame, slide camera so the unit sits at
    // the ground-hit point (visually centered). If the unit died /
    // was destroyed, drop the lock — get_unit_xy returns {NaN, NaN}.
    if (m_lock_unit.id != UINT32_MAX) {
        glm::vec2 unit_xy = get_unit_xy(m_lock_unit);
        if (std::isnan(unit_xy.x) || std::isnan(unit_xy.y)) {
            m_lock_unit = {};
        } else {
            glm::vec2 off = ground_hit_xy_offset(*m_camera);
            m_camera->set_position_xy(unit_xy.x - off.x, unit_xy.y - off.y);
        }
    }
    // Step 3: animated pan (only if not locked). Stored in ground-
    // target space; eye is derived each frame so a zoom mid-pan
    // doesn't drift the target.
    else if (m_pan_active) {
        m_pan_elapsed += dt;
        f32 t = (m_pan_duration > 0) ? (m_pan_elapsed / m_pan_duration) : 1.0f;
        f32 s = (t >= 1.0f) ? 1.0f : t * t * (3.0f - 2.0f * t);
        f32 tx = m_pan_start.x + (m_pan_target.x - m_pan_start.x) * s;
        f32 ty = m_pan_start.y + (m_pan_target.y - m_pan_start.y) * s;
        glm::vec2 off = ground_hit_xy_offset(*m_camera);
        m_camera->set_position_xy(tx - off.x, ty - off.y);
        if (t >= 1.0f) m_pan_active = false;
    }

    // Step 4: shake on top. Trauma-style decay: offset scales with
    // (1 - elapsed/duration)^2 for a snappy attack and a long tail.
    if (m_shake_elapsed < m_shake_duration && m_shake_intensity > 0) {
        m_shake_elapsed += dt;
        f32 t = m_shake_elapsed / m_shake_duration;
        if (t >= 1.0f) {
            m_shake_intensity = 0;
            m_shake_duration  = 0;
            m_shake_elapsed   = 0;
        } else {
            f32 trauma = 1.0f - t;
            f32 mag    = m_shake_intensity * trauma * trauma;
            // [-1, 1] random axes, no z perturbation (we only shake
            // the ground plane so altitude / framing stays steady).
            m_shake_offset = { (frand() * 2.0f - 1.0f) * mag,
                               (frand() * 2.0f - 1.0f) * mag };
            m_camera->set_position_xy(
                m_camera->position().x + m_shake_offset.x,
                m_camera->position().y + m_shake_offset.y);
        }
    }
}

// Position arguments are GROUND TARGET XY — the point on the world
// floor the camera should look at, mirroring WC3's SetCameraPosition /
// PanCameraTo. Eye XY is derived from target via the current pitch /
// yaw / height so scripts don't redo the math when angles or zoom
// change later.
void CameraController::set_position(f32 target_x, f32 target_y) {
    m_pan_active = false;
    m_lock_unit  = {};
    if (!m_camera) return;
    glm::vec2 off = ground_hit_xy_offset(*m_camera);
    m_camera->set_position_xy(target_x - off.x, target_y - off.y);
}

void CameraController::pan(f32 target_x, f32 target_y, f32 duration) {
    m_lock_unit = {};
    if (!m_camera) return;
    glm::vec2 off = ground_hit_xy_offset(*m_camera);
    if (duration <= 0) {
        m_pan_active = false;
        m_camera->set_position_xy(target_x - off.x, target_y - off.y);
        return;
    }
    // pan_start = current target on the ground (eye + offset), so the
    // animation lerps target → target, not eye → eye.
    m_pan_start    = { m_camera->position().x + off.x,
                       m_camera->position().y + off.y };
    m_pan_target   = { target_x, target_y };
    m_pan_duration = duration;
    m_pan_elapsed  = 0;
    m_pan_active   = true;
}

void CameraController::set_zoom(f32 z) {
    if (m_camera) m_camera->set_z(z);
}

void CameraController::shake(f32 intensity, f32 duration) {
    if (intensity <= 0 || duration <= 0) return;
    // Re-call mid-shake: take the max intensity, reset timer to the
    // longer of the two remaining/incoming windows. Avoids a strong
    // shake collapsing because a weaker one is layered on top.
    f32 remaining = m_shake_duration - m_shake_elapsed;
    m_shake_intensity = (intensity > m_shake_intensity) ? intensity : m_shake_intensity;
    m_shake_duration  = (duration > remaining) ? duration : remaining;
    m_shake_elapsed   = 0;
}

void CameraController::lock_unit(simulation::Unit unit) {
    m_pan_active = false;
    m_lock_unit  = unit;
}

void CameraController::unlock_unit() {
    m_lock_unit = {};
}

void CameraController::reset() {
    m_pan_active      = false;
    m_pan_elapsed     = 0;
    m_pan_duration    = 0;
    m_shake_intensity = 0;
    m_shake_duration  = 0;
    m_shake_elapsed   = 0;
    m_shake_offset    = {0, 0};
    m_lock_unit       = {};
}

} // namespace uldum::render
