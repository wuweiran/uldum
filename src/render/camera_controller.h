#pragma once

#include "core/types.h"
#include "render/camera.h"
#include "simulation/handle_types.h"

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
class CameraController {
public:
    using UnitPosFn = std::function<glm::vec2(simulation::Unit unit)>;

    void attach(Camera* cam) { m_camera = cam; }

    // Per-frame update. Order: lock > axis tweens > shake. If a lock is
    // active, the target.xy is overwritten with the unit's position
    // each frame (so target tweens on x/y get superseded). Pitch / yaw /
    // distance tweens still apply under a lock.
    void update(f32 dt, const UnitPosFn& get_unit_xy);

    // ── Script-driven commands ───────────────────────────────────────
    // Per-axis live setters. `duration` ≤ 0 snaps the axis instantly;
    // > 0 starts a smoothstep tween on that axis over the given seconds.
    // Other axes keep their current state (no implicit reset).

    // Pan target point. (x, y, z) is the world point the camera looks
    // at; z is typically 0 (ground) but can be raised for elevated
    // looks. Cancels the lock.
    void set_target_position(f32 x, f32 y, f32 z, f32 duration);

    // Adjust eye-to-target distance (the camera "zoom").
    void set_source_distance(f32 distance, f32 duration);

    // Adjust pitch (angle of attack, radians). Future API may expose
    // this from Lua; today it's used by setup-apply.
    void set_source_pitch_rad(f32 pitch_rad, f32 duration);

    // Adjust yaw (rotation, radians). Same: used by setup-apply today.
    void set_source_yaw_rad(f32 yaw_rad, f32 duration);

    // Apply a whole CameraSetup (the WC3 cinematic primitive). Starts
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
    AxisTween<f32>       m_distance_tween;
    AxisTween<f32>       m_pitch_tween;
    AxisTween<f32>       m_yaw_tween;

    // Trauma shake (applied to target.xy each frame; undone before the
    // next frame's tweens / lock to avoid compounding).
    f32       m_shake_intensity = 0;
    f32       m_shake_duration  = 0;
    f32       m_shake_elapsed   = 0;
    glm::vec2 m_shake_offset{0};

    // Lock-to-unit (.id == UINT32_MAX = unlocked).
    simulation::Unit m_lock_unit{};
};

} // namespace uldum::render
