#version 450

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_texcoord;
layout(location = 2) flat in uint frag_texture_id;  // shape_id

layout(location = 0) out vec4 out_color;

void main() {
    vec2 c = frag_texcoord - vec2(0.5);
    float dist = length(c) * 2.0;  // 0 at center, 1 at edge
    float angle = atan(c.y, c.x);
    float alpha = 0.0;
    vec3 color = frag_color.rgb;

    switch (frag_texture_id) {
    case 0u:  // Soft circle (default)
        alpha = smoothstep(1.0, 0.6, dist);
        break;

    case 1u:  // Spark (4-pointed star)
        {
            float star = abs(cos(angle * 2.0));
            alpha = (1.0 - dist) * (0.3 + star * 0.7);
            alpha = clamp(alpha * 2.0, 0.0, 1.0);
        }
        break;

    case 2u:  // Blood splatter (irregular blob)
        {
            float n = fract(sin(dot(frag_texcoord, vec2(127.1, 311.7))) * 43758.5453) * 0.3;
            alpha = max(0.0, 1.0 - dist - n);
        }
        break;

    case 3u:  // Light beam (vertical streak with bright core)
        {
            // Narrow horizontally, tall vertically
            float dx = abs(c.x) * 4.0;   // narrow width
            float dy = abs(c.y) * 1.5;   // tall height
            float beam = exp(-dx * dx * 3.0) * exp(-dy * dy * 1.0);
            // Bright core
            float core = exp(-dist * dist * 5.0);
            alpha = beam + core * 0.5;
            // Emissive: boost color to brighten underlying scene
            color *= 1.0 + (beam + core) * 3.0;
        }
        break;

    case 4u:  // Water droplet (teardrop)
        {
            float cy = c.y + 0.05;
            float stretch = 1.0 + max(0.0, cy) * 1.5;
            float d = sqrt(c.x * c.x * stretch * stretch + cy * cy) * 2.2;
            alpha = max(0.0, 1.0 - d * d);
        }
        break;

    default:
        alpha = smoothstep(1.0, 0.6, dist);
        break;
    }

    alpha *= frag_color.a;
    if (alpha < 0.01) discard;

    out_color = vec4(color, alpha);
}
