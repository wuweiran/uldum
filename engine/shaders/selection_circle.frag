#version 450

// Selection circle fragment shader — trivial passthrough of the
// push-constant color. Alpha blending is enabled in the pipeline state.

layout(location = 0) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = in_color;
}
