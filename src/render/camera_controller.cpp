#include "render/camera_controller.h"

#include <glm/common.hpp>

#include <cmath>
#include <cstdlib>

namespace uldum::render {

namespace {

f32 frand() { return static_cast<f32>(std::rand()) / static_cast<f32>(RAND_MAX); }

} // namespace

void CameraController::update(f32 dt, const UnitPosFn& get_unit_xy) {
    if (!m_camera) return;

    // Step 1: undo last frame's shake offset so it doesn't compound
    // on top of tweens / lock below.
    if (m_shake_offset.x != 0 || m_shake_offset.y != 0) {
        glm::vec3 t = m_camera->target();
        m_camera->set_target_xy(t.x - m_shake_offset.x,
                                t.y - m_shake_offset.y);
        m_shake_offset = {0, 0};
    }

    // Step 2: per-axis tweens. Each runs independently so a script can
    // (e.g.) interpolate distance while target tweens to a new point
    // and yaw spins separately.
    glm::vec3 next_target = m_camera->target();
    if (tick_tween(m_target_tween, dt, next_target)) {
        m_camera->set_target(next_target);
    }
    f32 next_distance = m_camera->distance();
    if (tick_tween(m_distance_tween, dt, next_distance)) {
        m_camera->set_distance(next_distance);
    }
    f32 next_pitch = m_camera->pitch_rad();
    if (tick_tween(m_pitch_tween, dt, next_pitch)) {
        m_camera->set_pitch_rad(next_pitch);
    }
    f32 next_yaw = m_camera->yaw_rad();
    if (tick_tween(m_yaw_tween, dt, next_yaw)) {
        m_camera->set_yaw_rad(next_yaw);
    }

    // Step 3: lock wins over the target tween for xy. The lock writes
    // unit.xy into target.xy each frame — target.z stays at whatever
    // tween / authored value put it there.
    if (m_lock_unit.id != UINT32_MAX) {
        glm::vec2 unit_xy = get_unit_xy(m_lock_unit);
        if (std::isnan(unit_xy.x) || std::isnan(unit_xy.y)) {
            m_lock_unit = {};
        } else {
            m_camera->set_target_xy(unit_xy.x, unit_xy.y);
        }
    }

    // Step 4: shake on top of everything else. Trauma-style decay:
    // offset scales with (1 - elapsed/duration)^2 for snappy attack +
    // long tail. XY only — height / pitch / yaw stay steady so framing
    // doesn't drift.
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
            m_shake_offset = { (frand() * 2.0f - 1.0f) * mag,
                               (frand() * 2.0f - 1.0f) * mag };
            glm::vec3 tg = m_camera->target();
            m_camera->set_target_xy(tg.x + m_shake_offset.x,
                                    tg.y + m_shake_offset.y);
        }
    }
}

void CameraController::set_target_position(f32 x, f32 y, f32 z, f32 duration) {
    m_lock_unit = {};
    if (!m_camera) return;
    if (duration <= 0) {
        m_target_tween.active = false;
        m_camera->set_target({x, y, z});
        return;
    }
    m_target_tween.active   = true;
    m_target_tween.start    = m_camera->target();
    m_target_tween.target   = {x, y, z};
    m_target_tween.duration = duration;
    m_target_tween.elapsed  = 0;
}

void CameraController::set_source_distance(f32 distance, f32 duration) {
    if (!m_camera) return;
    if (duration <= 0) {
        m_distance_tween.active = false;
        m_camera->set_distance(distance);
        return;
    }
    m_distance_tween.active   = true;
    m_distance_tween.start    = m_camera->distance();
    m_distance_tween.target   = distance;
    m_distance_tween.duration = duration;
    m_distance_tween.elapsed  = 0;
}

void CameraController::set_source_pitch_rad(f32 pitch_rad, f32 duration) {
    if (!m_camera) return;
    if (duration <= 0) {
        m_pitch_tween.active = false;
        m_camera->set_pitch_rad(pitch_rad);
        return;
    }
    m_pitch_tween.active   = true;
    m_pitch_tween.start    = m_camera->pitch_rad();
    m_pitch_tween.target   = pitch_rad;
    m_pitch_tween.duration = duration;
    m_pitch_tween.elapsed  = 0;
}

void CameraController::set_source_yaw_rad(f32 yaw_rad, f32 duration) {
    if (!m_camera) return;
    if (duration <= 0) {
        m_yaw_tween.active = false;
        m_camera->set_yaw_rad(yaw_rad);
        return;
    }
    m_yaw_tween.active   = true;
    m_yaw_tween.start    = m_camera->yaw_rad();
    m_yaw_tween.target   = yaw_rad;
    m_yaw_tween.duration = duration;
    m_yaw_tween.elapsed  = 0;
}

void CameraController::apply_setup(glm::vec3 target, f32 distance,
                                    f32 pitch_rad, f32 yaw_rad, f32 duration) {
    m_lock_unit = {};
    if (!m_camera) return;
    if (duration <= 0) {
        // Snap: stop any in-flight tweens, slam the whole pose.
        m_target_tween.active   = false;
        m_distance_tween.active = false;
        m_pitch_tween.active    = false;
        m_yaw_tween.active      = false;
        m_camera->set_pose(target, distance, pitch_rad, yaw_rad);
        return;
    }
    set_target_position(target.x, target.y, target.z, duration);
    set_source_distance(distance, duration);
    set_source_pitch_rad(pitch_rad, duration);
    set_source_yaw_rad(yaw_rad, duration);
}

void CameraController::shake(f32 intensity, f32 duration) {
    if (intensity <= 0 || duration <= 0) return;
    f32 remaining = m_shake_duration - m_shake_elapsed;
    m_shake_intensity = (intensity > m_shake_intensity) ? intensity : m_shake_intensity;
    m_shake_duration  = (duration > remaining) ? duration : remaining;
    m_shake_elapsed   = 0;
}

void CameraController::lock_unit(simulation::Unit unit) {
    // Lock overrides target.xy — cancel any in-flight target tween on
    // those axes (z component of a tween still completes if the script
    // mid-tween wants to lift the target while locking).
    m_target_tween.active = false;
    m_lock_unit = unit;
}

void CameraController::unlock_unit() {
    m_lock_unit = {};
}

void CameraController::reset() {
    m_target_tween   = {};
    m_distance_tween = {};
    m_pitch_tween    = {};
    m_yaw_tween      = {};
    m_shake_intensity = 0;
    m_shake_duration  = 0;
    m_shake_elapsed   = 0;
    m_shake_offset    = {0, 0};
    m_lock_unit       = {};
}

} // namespace uldum::render
