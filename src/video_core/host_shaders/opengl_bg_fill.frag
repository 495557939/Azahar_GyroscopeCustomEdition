// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

//? #version 430 core
// DiySC: Background blur fill — separable Gaussian with intermediate FBO
// Pass 0: horizontal blur, source → intermediate (same orientation)
// Pass 1: vertical blur + LCD rotation, intermediate → screen
layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D color_texture;

uniform int   u_direction;    // 0=horizontal pass, 1=vertical+display pass
uniform vec2  u_tex_size;     // current bound texture dimensions (w,h)
uniform float u_blur_sigma;   // Gaussian sigma (pixels)
uniform float u_scale;        // unused (scale applied to quad in C++)
uniform float u_darken;       // 0.0 = no darken, 1.0 = full dark
uniform int   u_max_radius;   // quality-controlled max sample radius

float gaussian(float x, float sigma) {
    return exp(-0.5 * (x * x) / (sigma * sigma));
}

void main() {
    vec2 uv;
    vec2 texel;
    float sigma = max(u_blur_sigma, 0.5);

    // step=1 for artifact-free blur, radius clamped by quality setting
    int radius = min(int(ceil(sigma * 2.5)), u_max_radius);

    if (u_direction == 0) {
        // Pass 0: HORIZONTAL blur. Source→intermediate, same orientation.
        uv = frag_tex_coord;
        texel = vec2(1.0 / u_tex_size.x, 0.0);

        vec3 blurred = vec3(0.0);
        float weight_sum = 0.0;
        for (int i = -radius; i <= radius; i++) {
            float w = gaussian(float(i), sigma);
            blurred += texture(color_texture, uv + texel * float(i)).rgb * w;
            weight_sum += w;
        }
        blurred /= weight_sum;
        color = vec4(blurred, 1.0);
    } else {
        // Pass 1: VERTICAL blur + LCD rotation for display.
        uv = vec2(1.0 - frag_tex_coord.y, 1.0 - frag_tex_coord.x);
        vec2 texel_rot = vec2(0.0, 1.0 / u_tex_size.y);

        vec3 blurred = vec3(0.0);
        float weight_sum = 0.0;
        for (int i = -radius; i <= radius; i++) {
            float w = gaussian(float(i), sigma);
            blurred += texture(color_texture, uv + texel_rot * float(i)).rgb * w;
            weight_sum += w;
        }
        blurred /= weight_sum;
        color = vec4(blurred * (1.0 - u_darken), 1.0);
    }
}
