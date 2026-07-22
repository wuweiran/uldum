#version 310 es
precision highp float;

// Procedural volumetric light shaft (Tyndall / god-ray). No texture, no motion.
// Scattering-density field raised to a growing EXPONENT as `frag_fade` drops,
// so the lit volume erodes toward the source (no hard rim). See vulkan/glow.frag.

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

    float dx      = (frag_uv.x - 0.5) * 2.0;
    float core    = exp(-dx * dx * 5.0);
    float v       = frag_uv.y;
    float vprof   = (1.0 - v) * (1.0 - v);
    float density = core * vprof;

    float k     = mix(7.0, 1.0, f);
    float shape = pow(density, k);
    float tail  = smoothstep(0.0, 0.25, f);

    float base_cap = smoothstep(0.0, 0.05, v);
    float s = striations(v, frag_tyndall * 0.4 * f);

    float intensity = shape * tail * base_cap * s;
    intensity *= frag_color.a;

    if (intensity < 0.003) discard;

    vec3 col = frag_color.rgb * (1.0 + core * 0.6);
    out_color = vec4(col * intensity, intensity);
}
