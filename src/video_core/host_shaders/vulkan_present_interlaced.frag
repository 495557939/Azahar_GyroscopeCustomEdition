// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#version 450 core
#extension GL_ARB_separate_shader_objects : enable

layout (location = 0) in vec2 frag_tex_coord;
layout (location = 1) in vec2 frag_position;
layout (location = 0) out vec4 color;

layout (push_constant, std140) uniform DrawInfo {
    mat4 modelview_matrix;
    vec4 i_resolution;
    vec4 o_resolution;
    int screen_id_l;
    int screen_id_r;
    int layer;
    int reverse_interlaced;
    vec2 screen_origin;
    float corner_radius;
    vec4 bg_color;
    float edge_blur;
    float opacity;
    float vignette_enable;
    float vignette_size;
    float overlay_enable;
    float _pad_v;
    vec3 vignette_color;
    vec3 overlay_color;
    float _pad1;
};

layout (set = 0, binding = 0) uniform sampler2D screen_textures[3];

vec4 GetScreen(int screen_id) {
#ifdef ARRAY_DYNAMIC_INDEX
    return texture(screen_textures[screen_id], frag_tex_coord);
#else
    switch (screen_id) {
    case 0: return texture(screen_textures[0], frag_tex_coord);
    case 1: return texture(screen_textures[1], frag_tex_coord);
    case 2: return texture(screen_textures[2], frag_tex_coord);
    }
#endif
}

// DiySC: Rounded corner SDF
float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec4 tex_color_l = GetScreen(screen_id_l);
    vec4 tex_color_r = GetScreen(screen_id_r);

    vec2 screen_size = vec2(o_resolution.y, o_resolution.x);
    vec2 pos = frag_position - screen_origin;
    vec2 half_size = screen_size * 0.5;
    vec2 centered = pos - half_size;

    // Unified SDF: rounded rectangle -> blur entire shape including corners
    float r = max(corner_radius, 0.001);
    float sdf = sdRoundedBox(centered, half_size, r);
    float blur_width = max(edge_blur, 1.5); // minimum 1.5px for natural AA
    // Inward-only blur: transition inside the shape, 1px outside for AA
    float shape_alpha = 1.0 - smoothstep(-blur_width, 1.0, sdf);

    float final_alpha = shape_alpha * opacity;

    float screen_row = o_resolution.x * frag_tex_coord.x;
    vec3 interlaced_rgb;
    if (int(screen_row) % 2 == reverse_interlaced)
        interlaced_rgb = tex_color_l.rgb;
    else
        interlaced_rgb = tex_color_r.rgb;

    vec3 vignette_rgb = interlaced_rgb;
    if (vignette_enable > 0.5) {
        float dist = length(centered / half_size);
        float vignette = smoothstep(vignette_size, 0.0, dist);
        vignette_rgb = mix(interlaced_rgb, vignette_color, vignette);
    }

    vec3 overlay_rgb = vignette_rgb;
    if (overlay_enable > 0.5) {
        overlay_rgb = vignette_rgb * (1.0 - overlay_color) + vignette_rgb * overlay_color * 2.0;
        overlay_rgb = clamp(overlay_rgb, 0.0, 1.0);
    }
    if (final_alpha <= 0.0) discard;

    float screen_row = o_resolution.x * frag_tex_coord.x;
    color = vec4(overlay_rgb, final_alpha);
}
