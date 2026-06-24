// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

//? #version 450
// DiySC: Background blur fill vertex shader — full-window textured quad (Vulkan)
// Push constants match BgFillUniformData in renderer_vulkan.h
layout(location = 0) in vec2 vert_position;
layout(location = 1) in vec2 vert_tex_coord;
layout(location = 0) out vec2 frag_tex_coord;

layout(push_constant) uniform PushConstants {
    mat4 modelview_matrix;   // 64 bytes — matches C++ std::array<f32, 16>
    // Remaining fields (tex_size, blur_sigma, etc.) only used in fragment shader
} push;

void main() {
    gl_Position = vec4(mat2(push.modelview_matrix) * vert_position + push.modelview_matrix[2].xy, 0.0, 1.0);
    frag_tex_coord = vert_tex_coord;
}
