#version 310 es
precision highp float;

// Procedural volumetric light shaft (Tyndall / god-ray). No texture, no
// motion. `frag_fade` (raw 0→1→0 envelope) retracts the scattering halo
// toward the core as the glow dies — not a flat alpha dim. See vulkan/glow.frag.

layout(location = 0) in vec4  frag_color;
layout(location = 1) in vec2  frag_uv;
layout(location = 2) in float frag_tyndall;
layout(location = 3) in float frag_fade;

layout(location = 0) out vec4 out_color;

float striations(float v, float amount) {
    float a = sin(v * 22.0);
    float b = sin(v * 9.0 + 1.7);
    float band = 0.5 * a + 0.5 * b;
    return 1.0 + amount * band;
}

void main() {
    float f = clamp(frag_fade, 0.0, 1.0);

    float dx    = (frag_uv.x - 0.5) * 2.0;
    float sharp = mix(9.0, 4.0, f);          // f→0 tight, f→1 wide
    float core  = exp(-dx * dx * sharp);

    float v        = frag_uv.y;
    float vfade    = 1.0 - v;
    float top_cap  = smoothstep(1.0, 0.85, v);
    float base_cap = smoothstep(0.0, 0.06, v);

    float s = striations(v, frag_tyndall * 0.4 * f);

    float intensity = core * vfade * top_cap * base_cap * s;
    intensity *= frag_color.a;

    if (intensity < 0.003) discard;

    vec3 col = frag_color.rgb * (1.0 + core * 0.6);
    out_color = vec4(col * intensity, intensity);
}
