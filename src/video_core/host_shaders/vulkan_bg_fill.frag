// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

//? #version 450
// DiySC: Background blur fill — separable Gaussian with intermediate FBO (Vulkan)
// Pass 0: horizontal blur, source → intermediate (same orientation)
// Pass 1: vertical blur + LCD rotation, intermediate → screen
layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D color_texture;

layout(push_constant) uniform PushConstants {
    mat4 modelview_matrix;
    vec2 tex_size;
    float blur_sigma;
    float scale;
    float darken;
    int  direction;         // 0=horizontal, 1=vertical+display
    int  max_radius;        // quality-controlled max sample radius
    float _pad;             // 96-byte alignment
} push;

float gaussian(float x, float sigma) {
    return exp(-0.5 * (x * x) / (sigma * sigma));
}

void main() {
    vec2 uv;
    vec2 texel;
    float sigma = max(push.blur_sigma, 0.5);

    int radius = min(int(ceil(sigma * 2.5)), push.max_radius);

    if (push.direction == 0) {
        uv = frag_tex_coord;
        texel = vec2(1.0 / push.tex_size.x, 0.0);

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
        uv = vec2(1.0 - frag_tex_coord.y, 1.0 - frag_tex_coord.x);
        vec2 texel_rot = vec2(0.0, 1.0 / push.tex_size.y);

        vec3 blurred = vec3(0.0);
        float weight_sum = 0.0;
        for (int i = -radius; i <= radius; i++) {
            float w = gaussian(float(i), sigma);
            blurred += texture(color_texture, uv + texel_rot * float(i)).rgb * w;
            weight_sum += w;
        }
        blurred /= weight_sum;
        color = vec4(blurred * (1.0 - push.darken), 1.0);
    }
}
