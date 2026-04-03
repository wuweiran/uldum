#include "render/procedural_skeleton.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <numbers>

namespace uldum::render {

// Box dimensions in game units (WC3 scale)
static constexpr float HALF_W = 16.0f;    // half-width (X)
static constexpr float HALF_D = 16.0f;    // half-depth (Y)
static constexpr float HEIGHT = 64.0f;    // total height (Z)
static constexpr float MID_Z  = HEIGHT * 0.5f;

asset::ModelData create_procedural_test_model() {
    asset::ModelData model;
    model.name = "procedural_test";

    // ── Skeleton: 2 bones ─────────────────────────────────────────────
    // Bone 0 (root/hips): at origin
    // Bone 1 (torso): at mid-height, child of bone 0
    model.skeleton.bones.resize(2);

    auto& root = model.skeleton.bones[0];
    root.name = "root";
    root.parent_index = -1;
    root.inverse_bind_matrix = glm::mat4{1.0f};  // root at origin

    auto& torso = model.skeleton.bones[1];
    torso.name = "torso";
    torso.parent_index = 0;
    // Torso bone is at (0, 0, MID_Z) in bind pose → inverse is translate(0, 0, -MID_Z)
    torso.inverse_bind_matrix = glm::translate(glm::mat4{1.0f}, glm::vec3{0, 0, -MID_Z});

    // ── Skinned mesh: box with bone weights ───────────────────────────
    asset::SkinnedMeshData mesh;
    mesh.name = "test_box";

    // Helper: add a quad face (4 verts, 6 indices)
    auto add_face = [&](glm::vec3 p0, glm::vec3 p1, glm::vec3 p2, glm::vec3 p3, glm::vec3 normal) {
        u32 base = static_cast<u32>(mesh.vertices.size());

        auto make_vert = [&](glm::vec3 pos) -> asset::SkinnedVertex {
            asset::SkinnedVertex v;
            v.position = pos;
            v.normal   = normal;
            v.texcoord = {0, 0};
            // Weight: bottom half → bone 0, top half → bone 1
            if (pos.z <= MID_Z) {
                v.bone_indices = {0, 0, 0, 0};
                v.bone_weights = {1, 0, 0, 0};
            } else {
                v.bone_indices = {1, 0, 0, 0};
                v.bone_weights = {1, 0, 0, 0};
            }
            return v;
        };

        mesh.vertices.push_back(make_vert(p0));
        mesh.vertices.push_back(make_vert(p1));
        mesh.vertices.push_back(make_vert(p2));
        mesh.vertices.push_back(make_vert(p3));

        // Two triangles: 0-1-3, 1-2-3 (CCW)
        mesh.indices.push_back(base);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 3);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 3);
    };

    float s = HALF_W, d = HALF_D, h = HEIGHT;

    // Top face (+Z)
    add_face({-s, -d, h}, { s, -d, h}, { s,  d, h}, {-s,  d, h}, {0, 0, 1});
    // Bottom face (-Z)
    add_face({-s,  d, 0}, { s,  d, 0}, { s, -d, 0}, {-s, -d, 0}, {0, 0, -1});
    // Front face (+Y)
    add_face({-s, d, 0}, {-s, d, h}, { s, d, h}, { s, d, 0}, {0, 1, 0});
    // Back face (-Y)
    add_face({ s, -d, 0}, { s, -d, h}, {-s, -d, h}, {-s, -d, 0}, {0, -1, 0});
    // Right face (+X)
    add_face({ s, d, 0}, { s, d, h}, { s, -d, h}, { s, -d, 0}, {1, 0, 0});
    // Left face (-X)
    add_face({-s, -d, 0}, {-s, -d, h}, {-s, d, h}, {-s, d, 0}, {-1, 0, 0});

    model.skinned_meshes.push_back(std::move(mesh));

    // ── Animations ────────────────────────────────────────────────────
    // All animations only move bone 1 (torso). Bone 0 (root) stays at origin.
    // Torso local rest position is (0, 0, MID_Z).
    glm::vec3 torso_rest{0, 0, MID_Z};

    // Idle: very subtle sway (rotation around X axis)
    {
        asset::AnimationClip clip;
        clip.name = "idle";
        clip.duration = 3.0f;

        asset::AnimationChannel ch;
        ch.bone_index = 1;
        ch.timestamps = {0.0f, 0.75f, 1.5f, 2.25f, 3.0f};
        f32 sway = glm::radians(1.5f);
        ch.translations = {torso_rest, torso_rest, torso_rest, torso_rest, torso_rest};
        ch.rotations = {
            glm::quat{1, 0, 0, 0},
            glm::angleAxis(sway, glm::vec3{1, 0, 0}),
            glm::quat{1, 0, 0, 0},
            glm::angleAxis(-sway, glm::vec3{1, 0, 0}),
            glm::quat{1, 0, 0, 0},
        };
        ch.scales = {{1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}};

        clip.channels.push_back(std::move(ch));
        model.animations.push_back(std::move(clip));
    }

    // Walk: bob up/down + lean forward
    {
        asset::AnimationClip clip;
        clip.name = "walk";
        clip.duration = 0.8f;

        asset::AnimationChannel ch;
        ch.bone_index = 1;
        ch.timestamps = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f};
        f32 bob = 3.0f;
        f32 lean = glm::radians(8.0f);
        ch.translations = {
            torso_rest + glm::vec3{0, 0, 0},
            torso_rest + glm::vec3{0, 0, bob},
            torso_rest + glm::vec3{0, 0, 0},
            torso_rest + glm::vec3{0, 0, bob},
            torso_rest + glm::vec3{0, 0, 0},
        };
        ch.rotations = {
            glm::angleAxis(lean, glm::vec3{1, 0, 0}),
            glm::angleAxis(lean, glm::vec3{1, 0, 0}),
            glm::angleAxis(lean, glm::vec3{1, 0, 0}),
            glm::angleAxis(lean, glm::vec3{1, 0, 0}),
            glm::angleAxis(lean, glm::vec3{1, 0, 0}),
        };
        ch.scales = {{1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}};

        clip.channels.push_back(std::move(ch));
        model.animations.push_back(std::move(clip));
    }

    // Attack: lean forward sharply then back
    {
        asset::AnimationClip clip;
        clip.name = "attack";
        clip.duration = 0.6f;

        asset::AnimationChannel ch;
        ch.bone_index = 1;
        ch.timestamps = {0.0f, 0.15f, 0.3f, 0.6f};
        f32 swing = glm::radians(30.0f);
        ch.translations = {torso_rest, torso_rest, torso_rest, torso_rest};
        ch.rotations = {
            glm::quat{1, 0, 0, 0},
            glm::angleAxis(-swing * 0.3f, glm::vec3{1, 0, 0}),  // wind up (lean back)
            glm::angleAxis(swing, glm::vec3{1, 0, 0}),           // swing forward
            glm::quat{1, 0, 0, 0},                                // return
        };
        ch.scales = {{1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}};

        clip.channels.push_back(std::move(ch));
        model.animations.push_back(std::move(clip));
    }

    // Spell: lean back then raise up (casting gesture)
    {
        asset::AnimationClip clip;
        clip.name = "spell";
        clip.duration = 0.5f;

        asset::AnimationChannel ch;
        ch.bone_index = 1;
        ch.timestamps = {0.0f, 0.15f, 0.35f, 0.5f};
        f32 lean_back = glm::radians(15.0f);
        f32 raise = glm::radians(10.0f);
        ch.translations = {torso_rest, torso_rest, torso_rest + glm::vec3{0, 0, 4.0f}, torso_rest};
        ch.rotations = {
            glm::quat{1, 0, 0, 0},
            glm::angleAxis(-lean_back, glm::vec3{1, 0, 0}),  // lean back (gathering)
            glm::angleAxis(raise, glm::vec3{1, 0, 0}),        // raise up (casting)
            glm::quat{1, 0, 0, 0},                             // return
        };
        ch.scales = {{1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}};

        clip.channels.push_back(std::move(ch));
        model.animations.push_back(std::move(clip));
    }

    // Death: fall forward
    {
        asset::AnimationClip clip;
        clip.name = "death";
        clip.duration = 0.8f;

        asset::AnimationChannel root_ch;
        root_ch.bone_index = 0;
        root_ch.timestamps = {0.0f, 0.8f};
        root_ch.translations = {{0, 0, 0}, {0, 0, 0}};
        root_ch.rotations = {
            glm::quat{1, 0, 0, 0},
            glm::angleAxis(glm::radians(90.0f), glm::vec3{1, 0, 0}),  // fall forward
        };
        root_ch.scales = {{1,1,1}, {1,1,1}};

        asset::AnimationChannel torso_ch;
        torso_ch.bone_index = 1;
        torso_ch.timestamps = {0.0f, 0.8f};
        torso_ch.translations = {torso_rest, torso_rest};
        torso_ch.rotations = {glm::quat{1, 0, 0, 0}, glm::quat{1, 0, 0, 0}};
        torso_ch.scales = {{1,1,1}, {1,1,1}};

        clip.channels.push_back(std::move(root_ch));
        clip.channels.push_back(std::move(torso_ch));
        model.animations.push_back(std::move(clip));
    }

    return model;
}

} // namespace uldum::render
