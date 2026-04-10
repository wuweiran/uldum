#include "render/animation.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <span>

namespace uldum::render {

// ── Keyframe interpolation ────────────────────────────────────────────────

// Find the two keyframe indices and interpolation factor for a given time.
static void find_keyframe(const std::vector<f32>& timestamps, f32 time, u32& i0, u32& i1, f32& t) {
    if (timestamps.size() <= 1) {
        i0 = i1 = 0;
        t = 0;
        return;
    }

    // Clamp to range
    if (time <= timestamps.front()) { i0 = i1 = 0; t = 0; return; }
    if (time >= timestamps.back()) { i0 = i1 = static_cast<u32>(timestamps.size()) - 1; t = 0; return; }

    // Binary search
    for (u32 i = 0; i < static_cast<u32>(timestamps.size()) - 1; ++i) {
        if (time < timestamps[i + 1]) {
            i0 = i;
            i1 = i + 1;
            f32 dt = timestamps[i1] - timestamps[i0];
            t = (dt > 0.0001f) ? (time - timestamps[i0]) / dt : 0;
            return;
        }
    }
    i0 = i1 = static_cast<u32>(timestamps.size()) - 1;
    t = 0;
}

// Sample a single animation channel at a given time → local TRS
struct LocalTransform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 scale{1.0f};
};

static LocalTransform sample_channel(const asset::AnimationChannel& ch, f32 time,
                                     const LocalTransform& rest) {
    LocalTransform lt = rest;  // start from rest pose, not identity
    u32 i0, i1;
    f32 t;

    if (!ch.translations.empty()) {
        find_keyframe(ch.timestamps, time, i0, i1, t);
        i0 = std::min(i0, static_cast<u32>(ch.translations.size()) - 1);
        i1 = std::min(i1, static_cast<u32>(ch.translations.size()) - 1);
        lt.translation = glm::mix(ch.translations[i0], ch.translations[i1], t);
    }

    if (!ch.rotations.empty()) {
        find_keyframe(ch.timestamps, time, i0, i1, t);
        i0 = std::min(i0, static_cast<u32>(ch.rotations.size()) - 1);
        i1 = std::min(i1, static_cast<u32>(ch.rotations.size()) - 1);
        lt.rotation = glm::slerp(ch.rotations[i0], ch.rotations[i1], t);
    }

    if (!ch.scales.empty()) {
        find_keyframe(ch.timestamps, time, i0, i1, t);
        i0 = std::min(i0, static_cast<u32>(ch.scales.size()) - 1);
        i1 = std::min(i1, static_cast<u32>(ch.scales.size()) - 1);
        lt.scale = glm::mix(ch.scales[i0], ch.scales[i1], t);
    }

    return lt;
}

static glm::mat4 trs_to_matrix(const LocalTransform& lt) {
    glm::mat4 m{1.0f};
    m = glm::translate(m, lt.translation);
    m = m * glm::toMat4(lt.rotation);
    m = glm::scale(m, lt.scale);
    return m;
}

// ── Pose evaluation ───────────────────────────────────────────────────────

// Evaluate a clip at the given time, compute global bone matrices.
static void evaluate_clip(const asset::AnimationClip& clip, const asset::Skeleton& skel,
                           f32 time, std::span<glm::mat4> out_globals) {
    u32 bone_count = static_cast<u32>(skel.bones.size());

    // Start with rest pose local transforms (not identity!)
    std::vector<LocalTransform> locals(bone_count);
    for (u32 i = 0; i < bone_count; ++i) {
        locals[i].translation = skel.bones[i].rest_translation;
        locals[i].rotation    = skel.bones[i].rest_rotation;
        locals[i].scale       = skel.bones[i].rest_scale;
    }

    // Apply clip channels (overrides rest pose for animated components only)
    for (auto& ch : clip.channels) {
        if (ch.bone_index < bone_count) {
            locals[ch.bone_index] = sample_channel(ch, time, locals[ch.bone_index]);
        }
    }

    // Convert to matrices and compute global transforms (parent → child hierarchy walk)
    for (u32 i = 0; i < bone_count; ++i) {
        glm::mat4 local = trs_to_matrix(locals[i]);
        if (skel.bones[i].parent_index >= 0) {
            out_globals[i] = out_globals[skel.bones[i].parent_index] * local;
        } else {
            out_globals[i] = local;
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────

void set_anim_state(AnimationInstance& inst, AnimState state,
                     f32 gameplay_duration, bool force_restart,
                     const AttackAnimInfo* attack_info) {
    if (state == inst.current_state) {
        if (!force_restart) return;  // same state, no restart requested — keep playing/holding
    }

    inst.prev_time      = inst.time;
    inst.prev_speed     = inst.playback_speed;
    inst.previous_state = inst.current_state;
    inst.current_state  = state;
    inst.time           = 0.0f;
    inst.blend_factor   = (state == inst.previous_state) ? 1.0f : 0.0f;  // skip crossfade on restart
    inst.finished       = false;

    // Looping: idle and walk loop; attack, spell, death play once
    inst.looping = (state == AnimState::Idle || state == AnimState::Walk);

    // Compute playback speed
    i32 clip_idx = inst.state_to_clip[static_cast<u8>(state)];
    f32 clip_dur = 0;
    if (clip_idx >= 0 && clip_idx < static_cast<i32>(inst.model->animations.size())) {
        clip_dur = inst.model->animations[clip_idx].duration;
    }

    // Two-phase attack: wind-up and backswing play at different speeds
    if ((state == AnimState::Attack || state == AnimState::Spell) && attack_info && clip_dur > 0) {
        f32 dmg_frac = std::clamp(attack_info->dmg_point, 0.01f, 0.99f);
        inst.attack_dmg_time = dmg_frac * clip_dur;
        f32 phase1_clip = inst.attack_dmg_time;          // clip time for wind-up
        f32 phase2_clip = clip_dur - inst.attack_dmg_time; // clip time for backswing
        inst.attack_phase1_speed = (attack_info->cast_point > 0) ? phase1_clip / attack_info->cast_point : 1.0f;
        inst.attack_phase2_speed = (attack_info->backswing > 0) ? phase2_clip / attack_info->backswing : 1.0f;
        inst.playback_speed = inst.attack_phase1_speed;  // start with wind-up speed
    } else if (gameplay_duration < 0) {
        // Negative = direct speed multiplier (used for walk speed sync)
        inst.playback_speed = -gameplay_duration;
    } else if (gameplay_duration > 0 && clip_dur > 0) {
        inst.playback_speed = clip_dur / gameplay_duration;
    } else {
        inst.playback_speed = 1.0f;
    }
}

void update_animation(AnimationInstance& inst, f32 dt) {
    if (!inst.model || inst.model->skeleton.empty()) return;

    i32 clip_idx = inst.state_to_clip[static_cast<u8>(inst.current_state)];
    if (clip_idx >= 0 && clip_idx < static_cast<i32>(inst.model->animations.size())) {
        auto& clip = inst.model->animations[clip_idx];

        // Two-phase animation: switch speed when crossing damage/cast point
        if ((inst.current_state == AnimState::Attack || inst.current_state == AnimState::Spell)
            && inst.attack_dmg_time > 0) {
            f32 speed = (inst.time < inst.attack_dmg_time)
                ? inst.attack_phase1_speed : inst.attack_phase2_speed;
            inst.time += dt * speed;
        } else {
            inst.time += dt * inst.playback_speed;
        }

        if (inst.looping && clip.duration > 0) {
            inst.time = std::fmod(inst.time, clip.duration);
        } else if (inst.time >= clip.duration) {
            inst.time = clip.duration;
            inst.finished = true;
        }
    }

    // Advance crossfade blend and previous clip time
    if (inst.blend_factor < 1.0f) {
        inst.blend_factor += dt / inst.blend_duration;
        if (inst.blend_factor > 1.0f) inst.blend_factor = 1.0f;

        // Keep advancing previous clip so the blend source isn't frozen
        i32 prev_clip = inst.state_to_clip[static_cast<u8>(inst.previous_state)];
        if (prev_clip >= 0 && prev_clip < static_cast<i32>(inst.model->animations.size())) {
            inst.prev_time += dt * inst.prev_speed;
        }
    }
}

void evaluate_animation(AnimationInstance& inst) {
    if (!inst.model || inst.model->skeleton.empty()) return;

    auto& skel = inst.model->skeleton;
    u32 bone_count = static_cast<u32>(skel.bones.size());
    inst.bone_matrices.resize(bone_count);

    // Evaluate current clip
    std::vector<glm::mat4> current_globals(bone_count, glm::mat4{1.0f});
    i32 clip_idx = inst.state_to_clip[static_cast<u8>(inst.current_state)];
    if (clip_idx >= 0 && clip_idx < static_cast<i32>(inst.model->animations.size())) {
        evaluate_clip(inst.model->animations[clip_idx], skel, inst.time, current_globals);
    } else {
        // No clip — use rest pose
        for (u32 i = 0; i < bone_count; ++i) {
            LocalTransform lt;
            lt.translation = skel.bones[i].rest_translation;
            lt.rotation    = skel.bones[i].rest_rotation;
            lt.scale       = skel.bones[i].rest_scale;
            glm::mat4 local = trs_to_matrix(lt);
            if (skel.bones[i].parent_index >= 0) {
                current_globals[i] = current_globals[skel.bones[i].parent_index] * local;
            } else {
                current_globals[i] = local;
            }
        }
    }

    // Crossfade: blend with previous clip if transitioning
    if (inst.blend_factor < 1.0f) {
        std::vector<glm::mat4> prev_globals(bone_count, glm::mat4{1.0f});
        i32 prev_clip_idx = inst.state_to_clip[static_cast<u8>(inst.previous_state)];
        if (prev_clip_idx >= 0 && prev_clip_idx < static_cast<i32>(inst.model->animations.size())) {
            evaluate_clip(inst.model->animations[prev_clip_idx], skel, inst.prev_time, prev_globals);
        } else {
            // Rest pose for previous
            for (u32 i = 0; i < bone_count; ++i) {
                LocalTransform lt;
                lt.translation = skel.bones[i].rest_translation;
                lt.rotation    = skel.bones[i].rest_rotation;
                lt.scale       = skel.bones[i].rest_scale;
                glm::mat4 local = trs_to_matrix(lt);
                if (skel.bones[i].parent_index >= 0) {
                    prev_globals[i] = prev_globals[skel.bones[i].parent_index] * local;
                } else {
                    prev_globals[i] = local;
                }
            }
        }

        // Lerp global bone matrices
        f32 t = inst.blend_factor;
        for (u32 i = 0; i < bone_count; ++i) {
            for (u32 col = 0; col < 4; ++col) {
                current_globals[i][col] = glm::mix(prev_globals[i][col], current_globals[i][col], t);
            }
        }
    }

    // Store global transforms for attachment point lookups
    inst.bone_globals = current_globals;

    // Compute final skinning matrices: global * inverse_bind
    for (u32 i = 0; i < bone_count; ++i) {
        inst.bone_matrices[i] = current_globals[i] * skel.bones[i].inverse_bind_matrix;
    }
}

glm::vec3 get_attachment_point(const AnimationInstance& inst, std::string_view bone_name) {
    if (!inst.model) return {0, 0, 0};
    auto& skel = inst.model->skeleton;

    // Try exact name, then with "attach_" prefix (Blender convention)
    std::string prefixed = "attach_" + std::string(bone_name);

    for (u32 i = 0; i < static_cast<u32>(skel.bones.size()); ++i) {
        if (skel.bones[i].name == bone_name || skel.bones[i].name == prefixed) {
            if (i < inst.bone_globals.size()) {
                return glm::vec3(inst.bone_globals[i][3]);
            }
            break;
        }
    }
    return {0, 0, 0};
}

} // namespace uldum::render
