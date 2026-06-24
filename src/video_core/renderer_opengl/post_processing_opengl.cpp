// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/string_util.h"
#include "video_core/renderer_opengl/post_processing_opengl.h"

#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>

namespace OpenGL {

// The Dolphin shader header is added here for drop-in compatibility with most
// of Dolphin's "glsl" shaders, which use hlsl types, hence the #define's below
// It's fairly complete, but the features it's missing are:
// The font texture for the ascii shader (Citra doesn't have an overlay font)
// GetTime (not used in any shader provided by Dolphin)
// GetOption* (used in only one shader provided by Dolphin; would require more
// configuration/frontend work)
constexpr char dolphin_shader_header[] = R"(

// hlsl to glsl types
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4

// hlsl to glsl function translation
#define frac fract
#define lerp mix

// Output variable
layout (location = 0) out float4 color;
// Input coordinates
layout (location = 0) in float2 frag_tex_coord;
// Screen position (for corner radius SDF)
layout (location = 1) in float2 frag_position;
// Resolution
uniform float4 i_resolution;
uniform float4 o_resolution;
// Layer
uniform int layer;
// Corner radius uniforms
uniform vec2 u_screen_origin;
uniform float u_corner_radius;
uniform vec4 u_bg_color;
uniform float u_edge_blur;
uniform float u_opacity;

uniform sampler2D color_texture;
uniform sampler2D color_texture_r;

// Interfacing functions
float4 Sample()
{
    return texture(color_texture, frag_tex_coord);
}

float4 SampleLocation(float2 location)
{
    return texture(color_texture, location);
}

float4 SampleLayer(int layer)
{
    if(layer == 0)
        return texture(color_texture, frag_tex_coord);
    else
        return texture(color_texture_r, frag_tex_coord);
}

#define SampleOffset(offset) textureOffset(color_texture, frag_tex_coord, offset)

float2 GetResolution()
{
    return i_resolution.xy;
}

float2 GetInvResolution()
{
    return i_resolution.zw;
}

float2 GetIResolution()
{
    return i_resolution.xy;
}

float2 GetIInvResolution()
{
    return i_resolution.zw;
}

float2 GetWindowResolution()
{
  return o_resolution.xy;
}

float2 GetInvWindowResolution()
{
  return o_resolution.zw;
}

float2 GetOResolution()
{
    return o_resolution.xy;
}

float2 GetOInvResolution()
{
    return o_resolution.zw;
}

float2 GetCoordinates()
{
    return frag_tex_coord;
}

void SetOutput(float4 color_in)
{
    // Apply rounded corner SDF on top of the processed color.
    // Only set alpha; GPU blending (GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA)
    // handles the background mix — do NOT pre-multiply here to avoid
    // double-blending with the background.
    vec2 screen_size = vec2(o_resolution.y, o_resolution.x);
    vec2 pos = frag_position - u_screen_origin;
    vec2 half_size = screen_size * 0.5;
    vec2 centered = pos - half_size;

    float r = max(u_corner_radius, 0.001);
    // sdRoundedBox: q = abs(centered) - half_size + r
    vec2 q = abs(centered) - half_size + r;
    float sdf = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float blur_width = max(u_edge_blur, 1.5);
    float shape_alpha = 1.0 - smoothstep(-blur_width, 1.0, sdf);
    float final_alpha = shape_alpha * u_opacity;

    color = float4(color_in.rgb, final_alpha);
}

)";

std::vector<std::string> GetPostProcessingShaderList(bool anaglyph) {
    // Filter shaders live in ./shadersGlsl/ (alongside the exe),
    // separate from the GPU shader cache in UserProfile/shaders/
    std::string shader_dir = FileUtil::GetExeDirectory() + DIR_SEP "shadersGlsl" + DIR_SEP;
    std::vector<std::string> shader_names;

    if (!FileUtil::IsDirectory(shader_dir)) {
        FileUtil::CreateDir(shader_dir);
    }

    if (anaglyph) {
        shader_dir = shader_dir + "anaglyph";
        if (!FileUtil::IsDirectory(shader_dir)) {
            FileUtil::CreateDir(shader_dir);
        }
    }

    // Would it make more sense to just add a directory list function to FileUtil?
    const auto callback = [&shader_names](u64* num_entries_out, const std::string& directory,
                                          const std::string& virtual_name) -> bool {
        const std::string physical_name = directory + DIR_SEP + virtual_name;
        if (!FileUtil::IsDirectory(physical_name)) {
            // The following is done to avoid coupling this to Qt
            std::size_t dot_pos = virtual_name.rfind(".");
            if (dot_pos != std::string::npos) {
                if (Common::ToLower(virtual_name.substr(dot_pos + 1)) == "glsl") {
                    shader_names.push_back(virtual_name.substr(0, dot_pos));
                }
            }
        }
        return true;
    };

    FileUtil::ForeachDirectoryEntry(nullptr, shader_dir, callback);

    std::sort(shader_names.begin(), shader_names.end());

    return shader_names;
}

std::string GetPostProcessingShaderCode(bool anaglyph, std::string_view shader) {
    std::string shader_dir = FileUtil::GetExeDirectory() + DIR_SEP "shadersGlsl" + DIR_SEP;
    std::string shader_path;

    if (anaglyph) {
        shader_dir = shader_dir + "anaglyph";
    }

    // Examining the directory is done because the shader extension might have an odd case
    // This can be eliminated if it is specified that the shader extension must be lowercase
    const auto callback = [&shader, &shader_path](u64* num_entries_out,
                                                  const std::string& directory,
                                                  const std::string& virtual_name) -> bool {
        const std::string physical_name = directory + DIR_SEP + virtual_name;
        if (!FileUtil::IsDirectory(physical_name)) {
            // The following is done to avoid coupling this to Qt
            std::size_t dot_pos = virtual_name.rfind(".");
            if (dot_pos != std::string::npos) {
                if (Common::ToLower(virtual_name.substr(dot_pos + 1)) == "glsl" &&
                    virtual_name.substr(0, dot_pos) == shader) {
                    shader_path = physical_name;
                    return false;
                }
            }
        }
        return true;
    };

    FileUtil::ForeachDirectoryEntry(nullptr, shader_dir, callback);
    if (shader_path.empty()) {
        return "";
    }

    boost::iostreams::stream<boost::iostreams::file_descriptor_source> file;
    FileUtil::OpenFStream<std::ios_base::in>(file, shader_path);
    if (!file.is_open()) {
        return "";
    }

    std::stringstream shader_text;
    shader_text << file.rdbuf();

    return dolphin_shader_header + shader_text.str();
}

} // namespace OpenGL
