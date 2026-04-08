#pragma once

#include "core/types.h"
#include "asset/model.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <vector>

namespace uldum::render {

// Animation states — driven by simulation components.
// Clip names in glTF must match: "idle", "walk", "attack", "spell", "death"
enum class AnimState : u8 { Idle, Walk, Attack, Spell, Death, Birth, Count };

// Per-entity animation instance with state machine.
struct AnimationInstance {
    const asset::ModelData* model = nullptr;

    AnimState current_state  = AnimState::Idle;
    AnimState previous_state = AnimState::Idle;
    f32 time           = 0.0f;
    f32 prev_time      = 0.0f;   // snapshot of time at transition (for crossfade)
    f32 prev_speed     = 1.0f;   // previous clip's playback speed
    f32 blend_factor   = 1.0f;   // 0 = fully previous, 1 = fully current
    f32 blend_duration = 0.15f;  // crossfade duration in seconds
    f32 playback_speed = 1.0f;
    bool looping       = true;
    bool finished      = false;

    // Track attack swings to restart animation on each new attack
    u32 attack_swing_id = 0;

    // Two-phase attack animation: wind-up plays at one speed, backswing at another
    f32 attack_dmg_time    = 0.0f;  // clip time where damage point is (dmg_point * clip_dur)
    f32 attack_phase1_speed = 1.0f; // playback speed for [0, dmg_time]
    f32 attack_phase2_speed = 1.0f; // playback speed for [dmg_time, clip_end]

    // Mapping from AnimState to clip index (-1 = no clip, use bind pose)
    std::array<i32, static_cast<usize>(AnimState::Count)> state_to_clip{};

    // Output: final bone matrices, ready for GPU upload
    std::vector<glm::mat4> bone_matrices;
    // Global bone transforms (before inverse bind — for attachment point lookups)
    std::vector<glm::mat4> bone_globals;

    // Per-entity GPU bone buffer (allocated by renderer)
    VkBuffer      bone_buffer     = VK_NULL_HANDLE;
    VmaAllocation bone_alloc      = VK_NULL_HANDLE;
    void*         bone_mapped     = nullptr;
    VkDescriptorSet bone_descriptor = VK_NULL_HANDLE;

    AnimationInstance() { state_to_clip.fill(-1); }
};

// Two-phase attack timing info (wind-up + backswing with different speeds).
struct AttackAnimInfo {
    f32 dmg_point  = 0.5f;  // fraction of clip at damage point
    f32 cast_point = 0.3f;  // seconds for wind-up phase
    f32 backswing  = 0.3f;  // seconds for backswing phase
};

// Request a state change. Restarts animation if force_restart is true.
// gameplay_duration: scale clip to match this timing (0 = default speed).
// attack_info: if non-null, enables two-phase speed for attack animations.
void set_anim_state(AnimationInstance& inst, AnimState state,
                     f32 gameplay_duration = 0, bool force_restart = false,
                     const AttackAnimInfo* attack_info = nullptr);

void update_animation(AnimationInstance& inst, f32 dt);
void evaluate_animation(AnimationInstance& inst);

// Get the local-space position of a named attachment bone. Returns (0,0,0) if not found.
glm::vec3 get_attachment_point(const AnimationInstance& inst, std::string_view bone_name);

} // namespace uldum::render
