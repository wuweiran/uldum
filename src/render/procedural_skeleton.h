#pragma once

#include "asset/model.h"

namespace uldum::render {

// Generate a procedural skinned model for testing the animation pipeline.
// A box mesh split into two halves: bottom half weighted to bone 0 (root/hips),
// top half weighted to bone 1 (torso). Includes idle, walk, attack, death animations.
asset::ModelData create_procedural_test_model();

} // namespace uldum::render
