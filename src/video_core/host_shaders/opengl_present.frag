// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

//? #version 430 core

layout(location = 0) in vec2 frag_tex_coord;
layout(location = 1) in vec2 frag_position;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D color_texture;

uniform vec4 i_resolution;
uniform vec4 o_resolution;
uniform int layer;
uniform vec2 u_screen_origin;
uniform float u_corner_radius;
uniform vec4 u_bg_color;
uniform float u_edge_blur;
uniform float u_opacity;
uniform bool u_vignette_enable;
uniform vec3 u_vignette_color;
uniform float u_vignette_size;
uniform bool u_overlay_enable;
uniform vec3 u_overlay_color;

// DiySC: Rounded corner SDF
float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec4 tex_color = texture(color_texture, frag_tex_coord);

    vec2 screen_size = vec2(o_resolution.y, o_resolution.x);
    vec2 pos = frag_position - u_screen_origin;
    vec2 half_size = screen_size * 0.5;
    vec2 centered = pos - half_size;

    // Unified SDF: rounded rectangle -> blur entire shape including corners
    float r = max(u_corner_radius, 0.001);
    float sdf = sdRoundedBox(centered, half_size, r);
    float blur_width = max(u_edge_blur, 1.5); // minimum 1.5px for natural AA
    // Inward-only blur: transition inside the shape, 1px outside for AA
    float shape_alpha = 1.0 - smoothstep(-blur_width, 1.0, sdf);

    float final_alpha = shape_alpha * u_opacity;

    // DiySC: Vignette — dark edge gradient
    vec3 vignette_rgb = tex_color.rgb;
    if (u_vignette_enable) {
        float dist = length(centered / half_size); // 0..1 from center
        float vignette = smoothstep(u_vignette_size, 0.0, dist); // 1 at edge, 0 at center beyond size
        vignette_rgb = mix(tex_color.rgb, u_vignette_color, vignette);
    }

    // DiySC: Overlay — multiplicative tint
    vec3 overlay_rgb = vignette_rgb;
    if (u_overlay_enable) {
        overlay_rgb = vignette_rgb * (1.0 - u_overlay_color) + vignette_rgb * u_overlay_color * 2.0;
        overlay_rgb = clamp(overlay_rgb, 0.0, 1.0);
    }
    if (final_alpha <= 0.0) discard;
    color = vec4(overlay_rgb, final_alpha);
}
