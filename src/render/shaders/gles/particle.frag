#version 310 es
precision highp float;
precision highp int;

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_texcoord;
layout(location = 2) flat in uint frag_texture_id;

layout(location = 0) out vec4 out_color;

// Two procedural particle sprites, no textures. The engine maps effect types
// to these (effect.cpp shape_for): orb = sparks, droplet = spray / water.
void main() {
    vec2 c = frag_texcoord - vec2(0.5);
    float dist = length(c) * 2.0;
    float alpha = 0.0;
    vec3 color = frag_color.rgb;

    if (frag_texture_id == 2u) {
        // Ripple — soft wide gaussian annulus; quad grows with age so the ring
        // expands, frag_color.a fades it. A natural wash, not a drawn circle.
        float band = (dist - 0.8) / 0.16;
        alpha = exp(-band * band);
    } else if (frag_texture_id == 1u) {
        // Droplet (teardrop) — spray / water
        float cy = c.y + 0.05;
        float stretch = 1.0 + max(0.0, cy) * 1.5;
        float d = sqrt(c.x * c.x * stretch * stretch + cy * cy) * 2.2;
        alpha = max(0.0, 1.0 - d * d);
    } else {
        // Orb (gaussian — tight bright core + wide soft halo). Default sprite.
        float core = exp(-dist * dist * 6.0);
        float halo = exp(-dist * dist * 1.6);
        alpha = clamp(core + halo * 0.55, 0.0, 1.0);
        color *= 1.0 + core * 1.5;
    }

    alpha *= frag_color.a;
    if (alpha < 0.01) discard;

    out_color = vec4(color, alpha);
}
