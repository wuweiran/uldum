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

    void set_aspect(f32 aspect) { m_aspect = aspect; m_dirty = true; }

    glm::mat4 view_matrix() const;
    glm::mat4 projection_matrix() const;
    glm::mat4 view_projection() const;

    glm::vec3 position() const { return m_position; }

private:
    void recalculate();

    // Game coordinates: X=right, Y=forward, Z=up
    glm::vec3 m_position{45.0f, 30.0f, 25.0f};
    f32 m_pitch = -0.8f;   // radians, negative = looking down
    f32 m_yaw   = 0.0f;    // 0 = looking toward +Y (forward)

    f32 m_move_speed   = 20.0f;
    f32 m_height_speed = 10.0f;
    f32 m_fov    = 0.785f;  // ~45 degrees
    f32 m_aspect = 16.0f / 9.0f;
    f32 m_near   = 0.1f;
    f32 m_far    = 1000.0f;

    bool m_dirty = true;
    mutable glm::mat4 m_view{1.0f};
    mutable glm::mat4 m_proj{1.0f};
};

} // namespace uldum::render
