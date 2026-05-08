#pragma once

#include "core/types.h"
#include "render/camera.h"
#include "simulation/handle_types.h"

#include <glm/vec2.hpp>

#include <functional>

namespace uldum::render {

// Scripted-camera overlay. Sits on top of Camera and the input-preset's
// per-frame camera pokes (WASD / drag-pan / scroll). Each frame, after
// the preset has run, CameraController::update() may overwrite the
// camera position to enforce a script-driven pan, lock-to-unit, or
// trauma shake — so player input is silently overridden while a
// scripted command owns the camera.
//
// One controller per camera. App creates one for the local Camera; the
// host's ScriptEngine routes per-player camera commands through it
// (own player) or onto the wire (remote players).
class CameraController {
public:
    using UnitPosFn = std::function<glm::vec2(simulation::Unit unit)>;

    void attach(Camera* cam) { m_camera = cam; }

    // Per-frame update. Order: lock > pan > shake. If a lock is active,
    // pan is ignored (the lock wins). Shake adds an offset on top of
    // whatever position the lock / pan / preset settled on.
    void update(f32 dt, const UnitPosFn& get_unit_xy);

    // ── Script-driven commands ───────────────────────────────────────
    //
    // Position arguments below are GROUND TARGET XY (the point on the
    // world floor the camera should look at), matching WC3's
    // SetCameraPosition / PanCameraTo semantics. Eye XY is derived
    // from target via the camera's current pitch / yaw / height so
    // scripts never need to redo the math when zoom or angle change.

    // Instant target teleport. Cancels any in-flight pan and the lock.
    void set_position(f32 target_x, f32 target_y);

    // Animated target pan from the current target to (x, y) over
    // `duration` seconds. Cancels the lock. duration <= 0 collapses
    // to set_position.
    void pan(f32 target_x, f32 target_y, f32 duration);

    // Set camera height (z). Lock + pan stay active.
    void set_zoom(f32 z);

    // Trigger a trauma-decay shake. Re-calling while a shake is active
    // takes the max of the two intensities and resets the timer.
    void shake(f32 intensity, f32 duration);

    // Lock to a unit — camera tracks the unit each frame, centered on
    // the ground-hit point so the unit appears at screen center.
    // Reset by calling unlock_unit() (or set_position / pan, which
    // cancel the lock too).
    void lock_unit(simulation::Unit unit);
    void unlock_unit();

    bool is_locked() const { return m_lock_unit.id != UINT32_MAX; }
    bool is_panning() const { return m_pan_active; }

    // Reset all scripted state (pan, shake, lock). Called on scene
    // switch / session end so lingering commands don't bleed into the
    // next scene.
    void reset();

private:
    Camera* m_camera = nullptr;

    // Animated pan
    bool      m_pan_active   = false;
    glm::vec2 m_pan_start{0};
    glm::vec2 m_pan_target{0};
    f32       m_pan_duration = 0;
    f32       m_pan_elapsed  = 0;

    // Trauma shake
    f32       m_shake_intensity = 0;
    f32       m_shake_duration  = 0;
    f32       m_shake_elapsed   = 0;
    glm::vec2 m_shake_offset{0};   // applied last frame; subtracted before re-applying

    // Lock-to-unit
    simulation::Unit m_lock_unit{};   // .id == UINT32_MAX = unlocked
};

} // namespace uldum::render
