#version 450

// Procedural volumetric light shaft (Tyndall / god-ray). No texture, no motion.
// A scattering-density field (radial gaussian × base→top falloff) raised to a
// growing EXPONENT as `frag_fade` drops: low density is punished harder, so the
// outer haze and far tip erode first while the core lingers — the lit volume
// retracts toward the source with no hard rim (a threshold would read as a
// shrinking blob). frag_color.a is the static brightness cap; additive blend.
// UV: u across width (0..1, core at 0.5), v up (0 base, 1 top).

layout(location = 0) in vec4  frag_color;
layout(location = 1) in vec2  frag_uv;
layout(location = 2) in float frag_tyndall;
layout(location = 3) in float frag_fade;

layout(location = 0) out vec4 out_color;

// Static vertical striations — fixed-phase sines give dusty "god-ray" banding
// without animating. `amount` scales the banding so it flattens as the glow dies.
float striations(float v, float amount) {
    float a = sin(v * 22.0);
    float b = sin(v * 9.0 + 1.7);
    float band = 0.5 * a + 0.5 * b;       // -1..1
    return 1.0 + amount * band;
}

void main() {
    float f = clamp(frag_fade, 0.0, 1.0);

    // Scattering density, normalized to 1 at the base core.
    float dx      = (frag_uv.x - 0.5) * 2.0;         // -1..1
    float core    = exp(-dx * dx * 5.0);             // radial gaussian
    float v       = frag_uv.y;
    float vprof   = (1.0 - v) * (1.0 - v);           // dense at base, thin at top
    float density = core * vprof;

    // Erosion by exponent: f=1 → k=1 (full soft beam); f→0 → k large, so
    // density^k collapses toward the core with a continuous gradient (no rim).
    float k     = mix(7.0, 1.0, f);
    float shape = pow(density, k);

    // Terminal wink-out so the retracted core doesn't pop when it clears.
    float tail  = smoothstep(0.0, 0.25, f);

    float base_cap = smoothstep(0.0, 0.05, v);       // soft foot at the ground
    float s = striations(v, frag_tyndall * 0.4 * f);

    float intensity = shape * tail * base_cap * s;
    intensity *= frag_color.a;                        // static brightness cap

    if (intensity < 0.003) discard;

    // Additive: brighten the core a touch so it reads as a light source.
    vec3 col = frag_color.rgb * (1.0 + core * 0.6);
    out_color = vec4(col * intensity, intensity);
}
