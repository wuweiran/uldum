#pragma once

#include "core/types.h"
#include "platform/platform.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <limits>

namespace uldum::render {

// Top-down game camera. Game coordinates: X=right, Y=forward, Z=up.
//
// Target-based, WC3 style: state is (target point, eye distance, pitch,
// yaw). The eye position is derived each frame as
// `target - distance * forward_dir`. Scripts and the editor's free-fly
// path both manipulate target / distance directly; the eye never gets
// authored — it's a render-time projection of the four primary fields.
//
// Pitch / yaw are stored in radians internally; Lua / data layers
// generally pass degrees and convert at the boundary.
class Camera {
public:
    void init(f32 aspect_ratio);
    void update(const platform::InputState& input, f32 dt);

    // Right-click drag panning (editor). Mouse delta while RMB held;
    // pans the target on the XY ground plane.
    void pan(f32 dx, f32 dy);

    // Direct world-space XY translation (for ground-pinned drag pan).
    // Moves target.xy; eye follows naturally.
    void translate(f32 dx, f32 dy);

    // Scroll wheel zoom (changes target-to-eye distance).
    void zoom(f32 scroll_delta);

    void set_aspect(f32 aspect) { m_aspect = aspect; m_dirty = true; }

    // Whole-pose set. Used by `CameraSetup` apply (scripts) and on
    // session start from the scene's `cameras[0]`. Pitch/yaw radians.
    void set_pose(glm::vec3 target, f32 distance, f32 pitch_rad, f32 yaw_rad) {
        m_target    = target;
        m_distance  = (distance < kMinDistance) ? kMinDistance : distance;
        m_pitch_rad = pitch_rad;
        m_yaw_rad   = yaw_rad;
        m_dirty     = true;
    }

    // Per-axis live setters used by scripted-camera commands. Pitch /
    // yaw / unaffected fields stay put.
    //
    // Clamping convention (mirrors WC3): xy ground-plane gestures
    // (`set_target_xy`, `pan`, `translate`, WASD in `update`) honor the
    // configured bounds; the full-vec3 `set_target` and whole-pose
    // `set_pose` bypass the clamp so scripts and cinematic CameraSetup
    // applies can target points outside the playable area.
    void set_target(glm::vec3 t) { m_target = t; m_dirty = true; }
    void set_target_xy(f32 x, f32 y);
    void set_distance(f32 d) {
        m_distance = (d < kMinDistance) ? kMinDistance : d;
        m_dirty = true;
    }
    void set_pitch_rad(f32 p) { m_pitch_rad = p; m_dirty = true; }
    void set_yaw_rad(f32 y)   { m_yaw_rad   = y; m_dirty = true; }

    // Bounds for clamped xy gestures. Default = unbounded. Scene load
    // calls `set_bounds` with the scene's authored camera-bounds rect.
    void set_bounds(glm::vec2 min_xy, glm::vec2 max_xy) {
        m_bounds_min = min_xy;
        m_bounds_max = max_xy;
    }
    void clear_bounds() {
        constexpr f32 inf = std::numeric_limits<f32>::infinity();
        m_bounds_min = { -inf, -inf };
        m_bounds_max = {  inf,  inf };
    }
    glm::vec2 bounds_min() const { return m_bounds_min; }
    glm::vec2 bounds_max() const { return m_bounds_max; }

    glm::mat4 view_matrix() const;
    glm::mat4 projection_matrix() const;
    glm::mat4 view_projection() const;

    // Derived eye position.
    glm::vec3 position() const;
    // Authored fields.
    glm::vec3 target()    const { return m_target; }
    f32       distance()  const { return m_distance; }
    f32       pitch_rad() const { return m_pitch_rad; }
    f32       yaw_rad()   const { return m_yaw_rad; }
    glm::vec3 forward_dir() const;

    // Frustum culling: 6 planes extracted from the VP matrix (Gribb/Hartmann).
    // Each plane is (nx, ny, nz, d) — point visible if dot(n,p)+d >= 0.
    struct Frustum {
        glm::vec4 planes[6];
        bool is_sphere_visible(const glm::vec3& center, f32 radius) const;
    };
    Frustum frustum() const;

private:
    void recalculate();
    void clamp_target_xy_to_bounds();

    // Game coordinates: X=right, Y=forward, Z=up. Defaults reproduce
    // WC3's built-in game camera: target on the ground at world origin,
    // 1650 unit eye-to-target distance, 56° angle-of-attack, FOV 70°.
    // Eye sits south at (0, -1650 cos56°, 1650 sin56°) ≈ (0, -923, 1370).
    glm::vec3 m_target{0.0f, 0.0f, 0.0f};
    f32 m_distance  = 1650.0f;
    f32 m_pitch_rad = -0.977f;  // -56° (WC3 angle-of-attack 304°)
    f32 m_yaw_rad   = 0.0f;     // 0 = looking +Y

    static constexpr f32 kMinDistance = 128.0f;
    // Upper zoom-out limit for PLAYER input (Q key / scroll). Scripted poses
    // (set_pose / set_distance) honor only kMinDistance, so cinematics can pull
    // back further — mirrors how xy-bounds let scripts bypass gesture clamps.
    static constexpr f32 kMaxDistance = 3500.0f;

    glm::vec2 m_bounds_min{ -std::numeric_limits<f32>::infinity(),
                            -std::numeric_limits<f32>::infinity() };
    glm::vec2 m_bounds_max{  std::numeric_limits<f32>::infinity(),
                             std::numeric_limits<f32>::infinity() };

    f32 m_move_speed   = 1280.0f;   // target-pan speed (units / sec)
    f32 m_zoom_speed   = 640.0f;    // Q/E distance step (units / sec)
    f32 m_fov    = 1.22f;   // ~70° (WC3 game camera FOV)
    f32 m_aspect = 16.0f / 9.0f;
    f32 m_near   = 1.0f;
    f32 m_far    = 64000.0f;

    bool m_dirty = true;
    mutable glm::mat4 m_view{1.0f};
    mutable glm::mat4 m_proj{1.0f};
};

} // namespace uldum::render
