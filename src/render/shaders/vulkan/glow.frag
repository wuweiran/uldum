#version 450

// Procedural volumetric light shaft (Tyndall / god-ray look). No texture and
// no motion — static math. The life envelope arrives two ways:
//   • frag_color.a — baked brightness (fade·intensity·color.a)
//   • frag_fade    — the raw 0→1→0 envelope, used to RETRACT the scattering
//                    halo toward the bright core as the glow dies. Real
//                    scattered light loses its faint outer haze first and
//                    pulls in to the core, rather than dimming uniformly —
//                    that's what reads as "Tyndall weakening" vs. a flat fade.
// Output is emissive; the pipeline blends it additively so overlapping shafts
// bloom. UV: u across the shaft width (0..1, core at 0.5), v up (0 base, 1 top).

layout(location = 0) in vec4  frag_color;
layout(location = 1) in vec2  frag_uv;
layout(location = 2) in float frag_tyndall;
layout(location = 3) in float frag_fade;

layout(location = 0) out vec4 out_color;

// Static vertical striations — fixed-phase sines give dusty "god-ray" banding
// without animating. `amount` scales the banding so it can flatten out as the
// glow dies.
float striations(float v, float amount) {
    float a = sin(v * 22.0);
    float b = sin(v * 9.0 + 1.7);
    float band = 0.5 * a + 0.5 * b;       // -1..1
    return 1.0 + amount * band;
}

void main() {
    float f = clamp(frag_fade, 0.0, 1.0);

    // Halo width tracks fade: at full glow the gaussian is wide (soft outer
    // scatter); as it dies the exponent grows so the lobe tightens onto the
    // core — the haze retracts inward instead of dimming in place.
    float dx    = (frag_uv.x - 0.5) * 2.0;          // -1..1
    float sharp = mix(9.0, 4.0, f);                  // f→0 = tight, f→1 = wide
    float core  = exp(-dx * dx * sharp);

    // Vertical profile: bright at the base, fading toward the top, soft caps.
    float v        = frag_uv.y;
    float vfade    = 1.0 - v;
    float top_cap  = smoothstep(1.0, 0.85, v);
    float base_cap = smoothstep(0.0, 0.06, v);

    // Striation depth also fades out — scattering loses structure as it dies.
    float s = striations(v, frag_tyndall * 0.4 * f);

    float intensity = core * vfade * top_cap * base_cap * s;
    intensity *= frag_color.a;                       // baked brightness (fade·intensity)

    if (intensity < 0.003) discard;

    // Additive: brighten the core a touch so it reads as a light source.
    vec3 col = frag_color.rgb * (1.0 + core * 0.6);
    out_color = vec4(col * intensity, intensity);
}
