#pragma once

#include "core/types.h"
#include "platform/platform.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace uldum::render {

// Top-down game camera. Game coordinates: X=right, Y=forward, Z=up.
// WASD moves on the XY ground plane, Q/E adjusts height (Z).
class Camera {
public:
    void init(f32 aspect_ratio);
    void update(const platform::InputState& input, f32 dt);

    // Right-click drag panning (editor). Call with mouse delta while right button held.
    void pan(f32 dx, f32 dy);

    // Direct world-space XY translation (for ground-pinned drag pan).
    void translate(f32 dx, f32 dy);

    // Scroll wheel zoom (moves along view direction).
    void zoom(f32 scroll_delta);

    void set_aspect(f32 aspect) { m_aspect = aspect; m_dirty = true; }

    // Map-defined camera pose applied on session start from the scene's
    // `cameras[0]` entry in objects.json. Pitch/yaw are radians.
    void set_pose(glm::vec3 position, f32 pitch, f32 yaw) {
        m_position = position;
        m_pitch    = pitch;
        m_yaw      = yaw;
        m_dirty    = true;
    }

    glm::mat4 view_matrix() const;
    glm::mat4 projection_matrix() const;
    glm::mat4 view_projection() const;

    glm::vec3 position() const { return m_position; }
    glm::vec3 forward_dir() const;
    f32       pitch()    const { return m_pitch; }
    f32       yaw()      const { return m_yaw; }

    // Frustum culling: 6 planes extracted from the VP matrix (Gribb/Hartmann).
    // Each plane is (nx, ny, nz, d) — point visible if dot(n,p)+d >= 0.
    struct Frustum {
        glm::vec4 planes[6];
        bool is_sphere_visible(const glm::vec3& center, f32 radius) const;
    };
    Frustum frustum() const;

private:
    void recalculate();

    // Game coordinates: X=right, Y=forward, Z=up. World origin is the
    // map center (WC3 convention). Default sits south of origin, elevated,
    // pitched down so the camera looks at the map center. Any map can
    // override this via `cameras[0]` in its scene objects.json.
    glm::vec3 m_position{0.0f, -1024.0f, 1650.0f};
    f32 m_pitch = -0.98f;  // ~56 degrees down (WC3 angle of attack ~304)
    f32 m_yaw   = 0.0f;    // 0 = looking toward +Y (forward)

    f32 m_move_speed   = 1280.0f;
    f32 m_height_speed = 640.0f;
    f32 m_fov    = 1.22f;   // ~70 degrees (WC3-like)
    f32 m_aspect = 16.0f / 9.0f;
    f32 m_near   = 1.0f;
    f32 m_far    = 64000.0f;

    bool m_dirty = true;
    mutable glm::mat4 m_view{1.0f};
    mutable glm::mat4 m_proj{1.0f};
};

} // namespace uldum::render
