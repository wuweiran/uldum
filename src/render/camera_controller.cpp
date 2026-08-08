#include "render/camera_controller.h"

#include <glm/common.hpp>

#include <cmath>
#include <cstdlib>

namespace uldum::render {

namespace {

f32 frand() { return static_cast<f32>(std::rand()) / static_cast<f32>(RAND_MAX); }

} // namespace

void CameraController::update(f32 dt, const UnitPoseFn& get_unit_pose) {
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
    if (m_pan_at_rate) {
        glm::vec3 delta = m_target_tween.target - next_target;
        f32 distance = glm::length(delta);
        f32 step = m_camera->move_speed() * dt;
        if (distance <= step || distance <= 1e-4f) {
            m_camera->set_target(m_target_tween.target);
            m_pan_at_rate = false;
        } else {
            m_camera->set_target(next_target + delta * (step / distance));
        }
    } else if (tick_tween(m_target_tween, dt, next_target)) {
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
    f32 next_fov = m_camera->fov_rad();
    if (tick_tween(m_fov_tween, dt, next_fov)) {
        m_camera->set_fov_rad(next_fov);
    }

    // Step 3: lock wins over the target tween for xy. The lock writes
    // unit.xy into target.xy each frame — target.z stays at whatever
    // tween / authored value put it there.
    if (m_lock_unit.id != UINT32_MAX) {
        glm::vec3 pose = get_unit_pose(m_lock_unit);
        if (std::isnan(pose.x) || std::isnan(pose.y)) {
            m_lock_unit = {};
        } else {
            f32 x_offset = m_lock_x_offset;
            f32 y_offset = m_lock_y_offset;
            if (m_lock_inherit_orientation) {
                f32 c = std::cos(pose.z);
                f32 s = std::sin(pose.z);
                x_offset = m_lock_x_offset * c - m_lock_y_offset * s;
                y_offset = m_lock_x_offset * s + m_lock_y_offset * c;
                m_camera->set_yaw_rad(pose.z);
            }
            m_camera->set_target_xy(pose.x + x_offset, pose.y + y_offset);
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

void CameraController::set_position(f32 x, f32 y) {
    stop();
    m_lock_unit = {};
    if (m_camera) m_camera->set_target_xy(x, y);
}

void CameraController::pan_to(f32 x, f32 y) {
    if (!m_camera) return;
    m_lock_unit = {};
    m_target_tween.active = false;
    m_target_tween.target = {x, y, m_camera->target().z};
    m_pan_at_rate = true;
}

void CameraController::pan_to(f32 x, f32 y, f32 duration) {
    if (!m_camera) return;
    glm::vec3 target = m_camera->target();
    set_target_position(x, y, target.z, duration);
}

void CameraController::pan_to_with_z(f32 x, f32 y, f32 z) {
    if (!m_camera) return;
    m_lock_unit = {};
    m_target_tween.active = false;
    m_target_tween.target = {x, y, z};
    m_pan_at_rate = true;
}

void CameraController::pan_to_with_z(f32 x, f32 y, f32 z, f32 duration) {
    set_target_position(x, y, z, duration);
}

void CameraController::set_field(CameraField field, f32 value, f32 duration) {
    if (!m_camera) return;
    constexpr f32 DEG_TO_RAD = 0.0174532925f;
    switch (field) {
    case CameraField::TargetDistance:
        set_source_distance(value, duration);
        break;
    case CameraField::AngleOfAttack:
        set_source_pitch_rad(std::remainder(value, 360.0f) * DEG_TO_RAD, duration);
        break;
    case CameraField::FieldOfView:
        set_field_of_view_rad(value * DEG_TO_RAD, duration);
        break;
    case CameraField::Rotation:
        set_source_yaw_rad(value * DEG_TO_RAD, duration);
        break;
    case CameraField::ZOffset: {
        glm::vec3 target = m_camera->target();
        set_target_position(target.x, target.y, value, duration);
        break;
    }
    }
}

void CameraController::adjust_field(CameraField field, f32 delta, f32 duration) {
    if (!m_camera) return;
    constexpr f32 RAD_TO_DEG = 57.2957795f;
    f32 current = 0.0f;
    switch (field) {
    case CameraField::TargetDistance: current = m_camera->distance(); break;
    case CameraField::AngleOfAttack: current = m_camera->pitch_rad() * RAD_TO_DEG; break;
    case CameraField::FieldOfView: current = m_camera->fov_rad() * RAD_TO_DEG; break;
    case CameraField::Rotation: current = m_camera->yaw_rad() * RAD_TO_DEG; break;
    case CameraField::ZOffset: current = m_camera->target().z; break;
    }
    set_field(field, current + delta, duration);
}

void CameraController::stop() {
    m_target_tween.active = false;
    m_pan_at_rate = false;
    m_distance_tween.active = false;
    m_pitch_tween.active = false;
    m_yaw_tween.active = false;
    m_fov_tween.active = false;
}

void CameraController::set_game_camera(f32 distance, f32 pitch_rad,
                                       f32 yaw_rad, f32 fov_rad) {
    m_game_target_z = m_camera ? m_camera->target().z : 0.0f;
    m_game_distance = distance;
    m_game_pitch = pitch_rad;
    m_game_yaw = yaw_rad;
    m_game_fov = fov_rad;
}

void CameraController::reset_to_game_camera(f32 duration) {
    if (!m_camera) return;
    m_lock_unit = {};
    glm::vec3 target = m_camera->target();
    target.z = m_game_target_z;
    apply_setup(target, m_game_distance, m_game_pitch, m_game_yaw, duration);
    set_field_of_view_rad(m_game_fov, duration);
}

void CameraController::set_target_controller(simulation::Unit unit, f32 x_offset,
                                             f32 y_offset, bool inherit_orientation) {
    m_target_tween.active = false;
    m_pan_at_rate = false;
    m_lock_unit = unit;
    m_lock_x_offset = x_offset;
    m_lock_y_offset = y_offset;
    m_lock_inherit_orientation = inherit_orientation;
}

void CameraController::set_target_position(f32 x, f32 y, f32 z, f32 duration) {
    m_lock_unit = {};
    m_pan_at_rate = false;
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

void CameraController::set_field_of_view_rad(f32 fov_rad, f32 duration) {
    if (!m_camera) return;
    if (duration <= 0) {
        m_fov_tween.active = false;
        m_camera->set_fov_rad(fov_rad);
        return;
    }
    m_fov_tween.active = true;
    m_fov_tween.start = m_camera->fov_rad();
    m_fov_tween.target = fov_rad;
    m_fov_tween.duration = duration;
    m_fov_tween.elapsed = 0;
}

void CameraController::apply_setup(glm::vec3 target, f32 distance,
                                    f32 pitch_rad, f32 yaw_rad, f32 duration) {
    m_lock_unit = {};
    m_pan_at_rate = false;
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
    set_target_controller(unit, 0.0f, 0.0f, false);
}

void CameraController::unlock_unit() {
    m_lock_unit = {};
    m_lock_x_offset = 0.0f;
    m_lock_y_offset = 0.0f;
    m_lock_inherit_orientation = false;
}

void CameraController::reset() {
    stop();
    m_target_tween = {};
    m_distance_tween = {};
    m_pitch_tween = {};
    m_yaw_tween = {};
    m_fov_tween = {};
    m_shake_intensity = 0;
    m_shake_duration = 0;
    m_shake_elapsed = 0;
    if (m_camera && (m_shake_offset.x != 0 || m_shake_offset.y != 0)) {
        glm::vec3 target = m_camera->target();
        m_camera->set_target_xy(target.x - m_shake_offset.x,
                                target.y - m_shake_offset.y);
    }
    m_shake_offset = {0, 0};
    unlock_unit();
}

} // namespace uldum::render
