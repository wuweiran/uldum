#pragma once

#include "core/types.h"
#include "render/camera.h"
#include "simulation/entity_types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <functional>

namespace uldum::render {

// Scripted-camera overlay. Sits on top of Camera and the input-preset's
// per-frame camera pokes (WASD / drag-pan / scroll). Each frame, after
// the preset has run, CameraController::update() may overwrite the
// camera state to enforce a script-driven tween, lock-to-unit, or
// trauma shake — so player input is silently overridden while a
// scripted command owns the camera.
//
// Target-based. The camera tracks four primary axes — target (xyz),
// distance, pitch, yaw — and the controller can tween each one
// independently with its own duration. A whole-pose `apply_setup` is
// just "start a tween on every axis at once".
//
// One controller per camera. App creates one for the local Camera; the
// host's ScriptEngine routes per-player camera commands through it
// (own player) or onto the wire (remote players).
enum class CameraField : u8 {
    TargetDistance = 0,
    AngleOfAttack  = 2,
    FieldOfView    = 3,
    Rotation       = 5,
    ZOffset        = 6,
};

class CameraController {
public:
    using UnitPoseFn = std::function<glm::vec3(simulation::Unit unit)>;

    void attach(Camera* cam) { m_camera = cam; }

    // Per-frame update. Order: lock > axis tweens > shake. If a lock is
    // active, the target.xy is overwritten with the unit's position
    // each frame (so target tweens on x/y get superseded). Pitch / yaw /
    // distance tweens still apply under a lock.
    void update(f32 dt, const UnitPoseFn& get_unit_pose);

    // ── Script-driven commands ───────────────────────────────────────
    // Per-axis live setters. `duration` ≤ 0 snaps the axis instantly;
    // > 0 starts a smoothstep tween on that axis over the given seconds.
    // Other axes keep their current state (no implicit reset).

    void set_position(f32 x, f32 y);
    void pan_to(f32 x, f32 y);
    void pan_to(f32 x, f32 y, f32 duration);
    void pan_to_with_z(f32 x, f32 y, f32 z);
    void pan_to_with_z(f32 x, f32 y, f32 z, f32 duration);
    void set_field(CameraField field, f32 value, f32 duration);
    void adjust_field(CameraField field, f32 delta, f32 duration);
    void stop();
    void reset_to_game_camera(f32 duration);
    void set_game_camera(f32 distance, f32 pitch_rad,
                         f32 yaw_rad, f32 fov_rad);

    void set_target_controller(simulation::Unit unit, f32 x_offset,
                               f32 y_offset, bool inherit_orientation);

    void set_target_position(f32 x, f32 y, f32 z, f32 duration);
    void set_source_distance(f32 distance, f32 duration);
    void set_source_pitch_rad(f32 pitch_rad, f32 duration);
    void set_source_yaw_rad(f32 yaw_rad, f32 duration);
    void set_field_of_view_rad(f32 fov_rad, f32 duration);

    // Apply a whole CameraSetup (the cinematic primitive). Starts
    // an independent tween on every axis using the same duration.
    // duration == 0 snaps; > 0 interpolates.
    void apply_setup(glm::vec3 target, f32 distance,
                     f32 pitch_rad, f32 yaw_rad, f32 duration);

    // Trauma-decay shake. Re-calling mid-shake takes the max intensity
    // and the longer remaining window.
    void shake(f32 intensity, f32 duration);

    // Lock target to a unit. Each frame the controller writes the
    // unit's XY into target.xy (z stays at its current value). pitch /
    // yaw / distance tweens continue. Reset via set_target_position
    // (which cancels the lock) or unlock_unit().
    void lock_unit(simulation::Unit unit);
    void unlock_unit();

    bool is_locked() const { return m_lock_unit.id != UINT32_MAX; }

    // Reset all scripted state (tweens, shake, lock). Called on scene
    // switch / session end so lingering commands don't bleed into the
    // next scene.
    void reset();

private:
    Camera* m_camera = nullptr;

    // Per-axis tween. A finished tween has `active = false`; the axis
    // is then driven only by gameplay input / the lock / direct sets.
    template <typename T>
    struct AxisTween {
        bool active = false;
        T    start{};
        T    target{};
        f32  duration = 0;
        f32  elapsed  = 0;
    };

    static f32 smoothstep01(f32 t) {
        if (t <= 0) return 0;
        if (t >= 1) return 1;
        return t * t * (3.0f - 2.0f * t);
    }

    template <typename T>
    bool tick_tween(AxisTween<T>& tw, f32 dt, T& out) {
        if (!tw.active) return false;
        tw.elapsed += dt;
        f32 t = (tw.duration > 0) ? (tw.elapsed / tw.duration) : 1.0f;
        f32 s = smoothstep01(t);
        out = tw.start + (tw.target - tw.start) * s;
        if (t >= 1.0f) tw.active = false;
        return true;
    }

    AxisTween<glm::vec3> m_target_tween;
    bool m_pan_at_rate = false;
    AxisTween<f32>       m_distance_tween;
    AxisTween<f32>       m_pitch_tween;
    AxisTween<f32>       m_yaw_tween;
    AxisTween<f32>       m_fov_tween;

    f32 m_game_target_z = 0.0f;
    f32 m_game_distance = 1650.0f;
    f32 m_game_pitch = -0.977f;
    f32 m_game_yaw = 0.0f;
    f32 m_game_fov = 0.698f;

    // Trauma shake (applied to target.xy each frame; undone before the
    // next frame's tweens / lock to avoid compounding).
    f32       m_shake_intensity = 0;
    f32       m_shake_duration  = 0;
    f32       m_shake_elapsed   = 0;
    glm::vec2 m_shake_offset{0};

    // Lock-to-unit (.id == UINT32_MAX = unlocked).
    simulation::Unit m_lock_unit{};
    f32 m_lock_x_offset = 0.0f;
    f32 m_lock_y_offset = 0.0f;
    bool m_lock_inherit_orientation = false;
};

} // namespace uldum::render
