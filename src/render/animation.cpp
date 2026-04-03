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

static LocalTransform sample_channel(const asset::AnimationChannel& ch, f32 time) {
    LocalTransform lt;
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

    // Start with identity local transforms
    std::vector<LocalTransform> locals(bone_count);

    // Apply clip channels
    for (auto& ch : clip.channels) {
        if (ch.bone_index < bone_count) {
            locals[ch.bone_index] = sample_channel(ch, time);
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
                     f32 gameplay_duration, bool force_restart) {
    if (state == inst.current_state) {
        if (!force_restart) return;  // same state, no restart requested — keep playing/holding
    }

    inst.previous_state = inst.current_state;
    inst.current_state  = state;
    inst.time           = 0.0f;
    inst.blend_factor   = (state == inst.previous_state) ? 1.0f : 0.0f;  // no crossfade on restart
    inst.finished       = false;

    // Looping: idle and walk loop; attack, spell, death play once
    inst.looping = (state == AnimState::Idle || state == AnimState::Walk);

    // Compute playback speed: scale clip duration to match gameplay timing
    i32 clip_idx = inst.state_to_clip[static_cast<u8>(state)];
    if (gameplay_duration > 0 && clip_idx >= 0 &&
        clip_idx < static_cast<i32>(inst.model->animations.size())) {
        f32 clip_dur = inst.model->animations[clip_idx].duration;
        inst.playback_speed = (clip_dur > 0) ? clip_dur / gameplay_duration : 1.0f;
    } else {
        inst.playback_speed = 1.0f;
    }
}

void update_animation(AnimationInstance& inst, f32 dt) {
    if (!inst.model || inst.model->skeleton.empty()) return;

    i32 clip_idx = inst.state_to_clip[static_cast<u8>(inst.current_state)];
    if (clip_idx >= 0 && clip_idx < static_cast<i32>(inst.model->animations.size())) {
        auto& clip = inst.model->animations[clip_idx];
        inst.time += dt * inst.playback_speed;
        if (inst.looping && clip.duration > 0) {
            inst.time = std::fmod(inst.time, clip.duration);
        } else if (inst.time >= clip.duration) {
            inst.time = clip.duration;
            inst.finished = true;
        }
    }

    // Advance crossfade blend
    if (inst.blend_factor < 1.0f) {
        inst.blend_factor += dt / inst.blend_duration;
        if (inst.blend_factor > 1.0f) inst.blend_factor = 1.0f;
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
        // No clip — use identity (bind pose)
        for (u32 i = 0; i < bone_count; ++i) current_globals[i] = glm::mat4{1.0f};
    }

    // Compute final skinning matrices: global * inverse_bind
    for (u32 i = 0; i < bone_count; ++i) {
        inst.bone_matrices[i] = current_globals[i] * skel.bones[i].inverse_bind_matrix;
    }
}

} // namespace uldum::render
