// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include "video_core/renderer_base.h"
#include "video_core/renderer_opengl/frame_dumper_opengl.h"
#include "video_core/renderer_opengl/gl_driver.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_state.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Core {
class System;
}

namespace OpenGL {

/// Structure used for storing information about the textures for each 3DS screen
struct TextureInfo {
    OGLTexture resource;
    u32 width;
    u32 height;
    Pica::PixelFormat format;
    GLenum gl_format;
    GLenum gl_type;
};

/// Structure used for storing information about the display target for each 3DS screen
struct ScreenInfo {
    GLuint display_texture;
    Common::Rectangle<float> display_texcoords;
    TextureInfo texture;
};

class RendererOpenGL : public VideoCore::RendererBase {
public:
    explicit RendererOpenGL(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window,
                            Frontend::EmuWindow* secondary_window);
    ~RendererOpenGL() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return &rasterizer;
    }

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override;
    void PrepareVideoDumping() override;
    void CleanupVideoDumping() override;

private:
    void InitOpenGLObjects();
    void ReloadShader(Settings::StereoRenderOption render_3d);
    void PrepareRendertarget();
    void RenderScreenshot();
    void RenderToMailbox(const Layout::FramebufferLayout& layout,
                         std::unique_ptr<Frontend::TextureMailbox>& mailbox, bool flipped);
    void ConfigureFramebufferTexture(TextureInfo& texture,
                                     const Pica::FramebufferConfig& framebuffer,
                                     const Pica::ColorFill& color_fill);
    void DrawScreens(const Layout::FramebufferLayout& layout, bool flipped);
    void DrawBackgroundFill(const Layout::FramebufferLayout& layout, bool flipped);
    void ApplySecondLayerOpacity(float opacity = 1.0f);
    void ResetSecondLayerOpacity();
    void DrawBottomScreen(const Layout::FramebufferLayout& layout,
                          const Common::Rectangle<u32>& bottom_screen);
    void DrawTopScreen(const Layout::FramebufferLayout& layout,
                       const Common::Rectangle<u32>& top_screen);
    void DrawSingleScreen(const ScreenInfo& screen_info, float x, float y, float w, float h,
                          Layout::DisplayOrientation orientation);
    void DrawSingleScreenStereo(const ScreenInfo& screen_info_l, const ScreenInfo& screen_info_r,
                                float x, float y, float w, float h,
                                Layout::DisplayOrientation orientation);

    // Loads framebuffer from emulated memory into the display information structure
    void LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer, ScreenInfo& screen_info,
                            bool right_eye, const Pica::ColorFill& color_fill);

private:
    Pica::PicaCore& pica;
    Driver driver;
    RasterizerOpenGL rasterizer;
    OpenGLState state;

    // OpenGL object IDs
    OGLVertexArray vertex_array;
    OGLBuffer vertex_buffer;
    OGLProgram shader;
    OGLProgram bg_fill_shader;
    OGLFramebuffer screenshot_framebuffer;
    std::array<OGLSampler, 2> samplers;

    // Display information for top and bottom screens respectively
    std::array<ScreenInfo, 3> screen_infos;

    // Shader uniform location indices
    GLuint uniform_modelview_matrix;
    GLuint uniform_color_texture;
    GLuint uniform_color_texture_r;

    // Shader uniform for Dolphin compatibility
    GLuint uniform_i_resolution;
    GLuint uniform_o_resolution;
    GLuint uniform_layer;

    // DiySC: Rounded corner uniforms
    GLuint uniform_screen_origin;
    GLuint uniform_corner_radius;
    GLuint uniform_bg_color;
    GLuint uniform_edge_blur;
    GLuint uniform_screen_opacity;

    // DiySC: Vignette + overlay uniforms
    GLuint uniform_vignette_enable;
    GLuint uniform_vignette_color;
    GLuint uniform_vignette_size;
    GLuint uniform_overlay_enable;
    GLuint uniform_overlay_color;

    // DiySC: Background fill uniforms
    GLuint bg_fill_uniform_modelview_matrix;
    GLuint bg_fill_uniform_color_texture;
    GLuint bg_fill_uniform_tex_size;
    GLuint bg_fill_uniform_blur_sigma;
    GLuint bg_fill_uniform_scale;
    GLuint bg_fill_uniform_darken;
    GLuint bg_fill_uniform_direction;
    GLuint bg_fill_uniform_max_radius;

    // DiySC: Intermediate FBO for separable Gaussian blur
    OGLTexture bg_fill_intermediate_texture;
    OGLFramebuffer bg_fill_intermediate_fbo;
    u32 bg_fill_last_tex_w = 0;
    u32 bg_fill_last_tex_h = 0;

    // DiySC: Multi-filter stacking — ping-pong FBOs
    OGLTexture filter_tex_a;
    OGLFramebuffer filter_fbo_a;
    OGLTexture filter_tex_b;
    OGLFramebuffer filter_fbo_b;
    OGLProgram filter_pass_shader;
    u32 filter_fbo_w = 0;
    u32 filter_fbo_h = 0;

    // DiySC: Current screen clip values (set per-draw by DrawTopScreen/DrawBottomScreen)
    float current_clip_left = 0.0f;
    float current_clip_right = 0.0f;
    float current_clip_top = 0.0f;
    float current_clip_bottom = 0.0f;
    float current_radius = 0.0f;
    float current_edge_blur = 0.0f;
    float current_opacity = 1.0f;
    bool current_vignette_enabled = false;
    std::array<float, 3> current_vignette_color = {0, 0, 0};
    float current_vignette_size = 0.5f;
    bool current_overlay_enabled = false;
    std::array<float, 3> current_overlay_color = {0.5f, 0.5f, 0.5f};
    float current_screen_x = 0.0f;
    float current_screen_y = 0.0f;

    // Shader attribute input indices
    GLuint attrib_position;
    GLuint attrib_tex_coord;

    FrameDumperOpenGL frame_dumper;
};

} // namespace OpenGL
