#version 310 es
precision highp float;
precision highp int;

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_texcoord;
layout(location = 2) flat in uint frag_texture_id;

layout(location = 0) out vec4 out_color;

// Procedural particle sprites, no textures. Output is PREMULTIPLIED alpha
// (rgb already multiplied by coverage) so one pipeline serves two looks with
// blend One/OneMinusSrcAlpha:
//   • additive (a=0): overlaps ADD → hot glowy sparks.
//   • alpha-over (a=coverage): normal transparency → spray / water.
void main() {
    vec2 c = frag_texcoord - vec2(0.5);
    float dist = length(c) * 2.0;  // 0 at center, 1 at edge midpoint
    float cov = 0.0;
    bool  additive = false;
    vec3  color = frag_color.rgb;

    if (frag_texture_id == 2u) {
        // Ripple — soft wide annulus; quad grows with age so the ring expands.
        float band = (dist - 0.8) / 0.16;
        cov = exp(-band * band);
    } else if (frag_texture_id == 1u) {
        // Droplet (teardrop) — spray / water
        float cy = c.y + 0.05;
        float stretch = 1.0 + max(0.0, cy) * 1.5;
        float d = sqrt(c.x * c.x * stretch * stretch + cy * cy) * 2.2;
        cov = max(0.0, 1.0 - d * d);
    } else {
        // Orb (spark) — gaussian: tight bright core + soft halo, WINDOWED to 0 at
        // the rim so it stays a clean round dot at any size (no fuzzy-square
        // faceting). Size variation applied at spawn. Additive so clustered sparks
        // sum into a hot core; frag_color.a (life) fades it out.
        float core = exp(-dist * dist * 11.0);
        float halo = exp(-dist * dist * 5.0);
        float mask = smoothstep(1.0, 0.5, dist);
        cov = clamp(core + halo * 0.35, 0.0, 1.0) * mask;
        color *= 1.0 + core * 4.5;
        additive = true;
    }

    cov *= frag_color.a;
    if (cov < 0.01) discard;

    out_color = vec4(color * cov, additive ? 0.0 : cov);
}
