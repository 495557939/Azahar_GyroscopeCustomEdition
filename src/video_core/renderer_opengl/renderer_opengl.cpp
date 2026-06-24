// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <vector>
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_texture_mailbox.h"
#include "video_core/renderer_opengl/post_processing_opengl.h"
#include "video_core/renderer_opengl/renderer_opengl.h"
#include "video_core/shader/generator/glsl_shader_gen.h"

#include "video_core/host_shaders/opengl_present_anaglyph_frag.h"
#include "video_core/host_shaders/opengl_present_frag.h"
#include "video_core/host_shaders/opengl_present_interlaced_frag.h"
#include "video_core/host_shaders/opengl_bg_fill_frag.h"
#include "video_core/host_shaders/opengl_bg_fill_vert.h"
#include "video_core/host_shaders/opengl_present_vert.h"

namespace OpenGL {

MICROPROFILE_DEFINE(OpenGL_RenderFrame, "OpenGL", "Render Frame", MP_RGB(128, 128, 64));
MICROPROFILE_DEFINE(OpenGL_WaitPresent, "OpenGL", "Wait For Present", MP_RGB(128, 128, 128));

/**
 * Vertex structure that the drawn screen rectangles are composed of.
 */
struct ScreenRectVertex {
    ScreenRectVertex() = default;
    ScreenRectVertex(GLfloat x, GLfloat y, GLfloat u, GLfloat v) {
        position[0] = x;
        position[1] = y;
        tex_coord[0] = u;
        tex_coord[1] = v;
    }

    std::array<GLfloat, 2> position{};
    std::array<GLfloat, 2> tex_coord{};
};

/**
 * Defines a 1:1 pixel ortographic projection matrix with (0,0) on the top-left
 * corner and (width, height) on the lower-bottom.
 *
 * The projection part of the matrix is trivial, hence these operations are represented
 * by a 3x2 matrix.
 *
 * @param flipped Whether the frame should be flipped upside down.
 */
static std::array<GLfloat, 3 * 2> MakeOrthographicMatrix(const float width, const float height,
                                                         bool flipped) {

    std::array<GLfloat, 3 * 2> matrix; // Laid out in column-major order

    // Last matrix row is implicitly assumed to be [0, 0, 1].
    if (flipped) {
        // clang-format off
        matrix[0] = 2.f / width; matrix[2] = 0.f;           matrix[4] = -1.f;
        matrix[1] = 0.f;         matrix[3] = 2.f / height;  matrix[5] = -1.f;
        // clang-format on
    } else {
        // clang-format off
        matrix[0] = 2.f / width; matrix[2] = 0.f;           matrix[4] = -1.f;
        matrix[1] = 0.f;         matrix[3] = -2.f / height; matrix[5] = 1.f;
        // clang-format on
    }

    return matrix;
}

RendererOpenGL::RendererOpenGL(Core::System& system, Pica::PicaCore& pica_,
                               Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window)
    : VideoCore::RendererBase{system, window, secondary_window}, pica{pica_},
      rasterizer{system.Memory(), pica, system.CustomTexManager(), *this, driver},
      frame_dumper{system, window} {
    const bool has_debug_tool = driver.HasDebugTool();
    window.mailbox = std::make_unique<OGLTextureMailbox>(has_debug_tool);
    if (secondary_window) {
        secondary_window->mailbox = std::make_unique<OGLTextureMailbox>(has_debug_tool);
    }
    frame_dumper.mailbox = std::make_unique<OGLVideoDumpingMailbox>();
    InitOpenGLObjects();
}

RendererOpenGL::~RendererOpenGL() = default;

void RendererOpenGL::SwapBuffers() {
    system.perf_stats->StartSwap();
    // Maintain the rasterizer's state as a priority
    OpenGLState prev_state = OpenGLState::GetCurState();
    state.Apply();

    render_window.SetupFramebuffer();

    PrepareRendertarget();
    RenderScreenshot();
#ifdef HAVE_LIBRETRO
    DrawScreens(render_window.GetFramebufferLayout(), false);
    render_window.SwapBuffers();
#else
    const auto& main_layout = render_window.GetFramebufferLayout();
    RenderToMailbox(main_layout, render_window.mailbox, false);

#ifdef ANDROID
    // On Android, if secondary_window is defined at all,
    // it means we have a second display
    if (secondary_window) {
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        RenderToMailbox(secondary_layout, secondary_window->mailbox, false);
        secondary_window->PollEvents();
    }
#else
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        RenderToMailbox(secondary_layout, secondary_window->mailbox, false);
        secondary_window->PollEvents();
    }
#endif

    if (frame_dumper.IsDumping()) {
        try {
            RenderToMailbox(frame_dumper.GetLayout(), frame_dumper.mailbox, true);
        } catch (const OGLTextureMailboxException& exception) {
            LOG_DEBUG(Render_OpenGL, "Frame dumper exception caught: {}", exception.what());
        }
    }
#endif

    system.perf_stats->EndSwap();
    EndFrame();
    prev_state.Apply();
    rasterizer.TickFrame();
}

void RendererOpenGL::RenderScreenshot() {
    if (settings.screenshot_requested.exchange(false)) {
        // Draw this frame to the screenshot framebuffer
        screenshot_framebuffer.Create();
        GLuint old_read_fb = state.draw.read_framebuffer;
        GLuint old_draw_fb = state.draw.draw_framebuffer;
        state.draw.read_framebuffer = state.draw.draw_framebuffer = screenshot_framebuffer.handle;
        state.Apply();

        const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};

        GLuint renderbuffer;
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB8, layout.width, layout.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  renderbuffer);

        DrawScreens(layout, false);

        glReadPixels(0, 0, layout.width, layout.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                     settings.screenshot_bits);

        screenshot_framebuffer.Release();
        state.draw.read_framebuffer = old_read_fb;
        state.draw.draw_framebuffer = old_draw_fb;
        state.Apply();
        glDeleteRenderbuffers(1, &renderbuffer);

        settings.screenshot_complete_callback(true);
    }
}

void RendererOpenGL::PrepareRendertarget() {
    const auto& framebuffer_config = pica.regs.framebuffer_config;
    const auto& regs_lcd = pica.regs_lcd;
    for (u32 i = 0; i < 3; i++) {
        const u32 fb_id = i == 2 ? 1 : 0;
        const auto& framebuffer = framebuffer_config[fb_id];
        auto& texture = screen_infos[i].texture;

        const auto color_fill = fb_id == 0 ? regs_lcd.color_fill_top : regs_lcd.color_fill_bottom;
        if (color_fill.is_enabled) {
            // Resize the texture to let it be reconfigured
            texture.width = 1;
            texture.height = 1;
        }

        if (texture.width != framebuffer.width || texture.height != framebuffer.height ||
            texture.format != framebuffer.color_format) {
            ConfigureFramebufferTexture(texture, framebuffer, color_fill);
        }
        LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1, color_fill);
    }
}

void RendererOpenGL::RenderToMailbox(const Layout::FramebufferLayout& layout,
                                     std::unique_ptr<Frontend::TextureMailbox>& mailbox,
                                     bool flipped) {

    Frontend::Frame* frame;
    {
        MICROPROFILE_SCOPE(OpenGL_WaitPresent);

        frame = mailbox->GetRenderFrame();

        // Clean up sync objects before drawing

        // INTEL driver workaround. We can't delete the previous render sync object until we are
        // sure that the presentation is done
        if (frame->present_fence) {
            glClientWaitSync(frame->present_fence, 0, GL_TIMEOUT_IGNORED);
        }

        // delete the draw fence if the frame wasn't presented
        if (frame->render_fence) {
            glDeleteSync(frame->render_fence);
            frame->render_fence = nullptr;
        }

        // wait for the presentation to be done
        if (frame->present_fence) {
            glWaitSync(frame->present_fence, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(frame->present_fence);
            frame->present_fence = nullptr;
        }
    }

    {
        MICROPROFILE_SCOPE(OpenGL_RenderFrame);
        // Recreate the frame if the size of the window has changed
        if (layout.width != frame->width || layout.height != frame->height) {
            LOG_DEBUG(Render_OpenGL, "Reloading render frame");
            mailbox->ReloadRenderFrame(frame, layout.width, layout.height);
        }

        state.draw.draw_framebuffer = frame->render.handle;
        state.Apply();
        DrawScreens(layout, flipped);
        // Create a fence for the frontend to wait on and swap this frame to OffTex
        frame->render_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        mailbox->ReleaseRenderFrame(frame);
    }
}

/**
 * Loads framebuffer from emulated memory into the active OpenGL texture.
 */
void RendererOpenGL::LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info, bool right_eye,
                                        const Pica::ColorFill& color_fill) {

    if (framebuffer.address_right1 == 0 || framebuffer.address_right2 == 0)
        right_eye = false;

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0
            ? (!right_eye ? framebuffer.address_left1 : framebuffer.address_right1)
            : (!right_eye ? framebuffer.address_left2 : framebuffer.address_right2);

    LOG_TRACE(Render_OpenGL, "0x{:08x} bytes from 0x{:08x}({}x{}), fmt {:x}",
              framebuffer.stride * framebuffer.height, framebuffer_addr, framebuffer.width.Value(),
              framebuffer.height.Value(), framebuffer.format);

    int bpp = Pica::BytesPerPixel(framebuffer.color_format);
    std::size_t pixel_stride = framebuffer.stride / bpp;

    // OpenGL only supports specifying a stride in units of pixels, not bytes, unfortunately
    ASSERT(pixel_stride * bpp == framebuffer.stride);

    // Ensure no bad interactions with GL_UNPACK_ALIGNMENT, which by default
    // only allows rows to have a memory alignement of 4.
    ASSERT(pixel_stride % 4 == 0);

    if (color_fill.is_enabled ||
        !rasterizer.AccelerateDisplay(framebuffer, framebuffer_addr, static_cast<u32>(pixel_stride),
                                      screen_info)) {
        u32 width = framebuffer.width;
        u32 height = framebuffer.height;
        u8 fill_pixel[3];
        // Reset the screen info's display texture to its own permanent texture
        screen_info.display_texture = screen_info.texture.resource.handle;
        screen_info.display_texcoords = Common::Rectangle<f32>(0.f, 0.f, 1.f, 1.f);

        rasterizer.FlushRegion(framebuffer_addr, framebuffer.stride * framebuffer.height);

        u8* framebuffer_data = system.Memory().GetPhysicalPointer(framebuffer_addr);

        if (color_fill.is_enabled) {
            memcpy(fill_pixel, color_fill.AsVector().AsArray(), sizeof(fill_pixel));
            framebuffer_data = fill_pixel;
            width = 1;
            height = 1;
            pixel_stride = 0;
        }

        state.texture_units[0].texture_2d = screen_info.texture.resource.handle;
        state.Apply();

        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)pixel_stride);

        // Update existing texture
        // TODO: Test what happens on hardware when you change the framebuffer dimensions so that
        //       they differ from the LCD resolution.
        // TODO: Applications could theoretically crash Citra here by specifying too large
        //       framebuffer sizes. We should make sure that this cannot happen.
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, screen_info.texture.gl_format,
                        screen_info.texture.gl_type, framebuffer_data);

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        state.texture_units[0].texture_2d = 0;
        state.Apply();
    }
}

/**
 * Initializes the OpenGL state and creates persistent objects.
 */
void RendererOpenGL::InitOpenGLObjects() {
    glClearColor(Settings::values.bg_red.GetValue(), Settings::values.bg_green.GetValue(),
                 Settings::values.bg_blue.GetValue(), 1.0f);

    for (std::size_t i = 0; i < samplers.size(); i++) {
        samplers[i].Create();
        glSamplerParameteri(samplers[i].handle, GL_TEXTURE_MIN_FILTER,
                            i == 0 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameteri(samplers[i].handle, GL_TEXTURE_MAG_FILTER,
                            i == 0 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameteri(samplers[i].handle, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(samplers[i].handle, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    ReloadShader(Settings::values.render_3d.GetValue());

    // Generate VBO handle for drawing
    vertex_buffer.Create();

    // Generate VAO
    vertex_array.Create();

    state.draw.vertex_array = vertex_array.handle;
    state.draw.vertex_buffer = vertex_buffer.handle;
    state.draw.uniform_buffer = 0;
    state.Apply();

    // Attach vertex data to VAO
    glBufferData(GL_ARRAY_BUFFER, sizeof(ScreenRectVertex) * 4, nullptr, GL_STREAM_DRAW);
    glVertexAttribPointer(attrib_position, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, position));
    glVertexAttribPointer(attrib_tex_coord, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, tex_coord));
    glEnableVertexAttribArray(attrib_position);
    glEnableVertexAttribArray(attrib_tex_coord);

    // Allocate textures for each screen
    for (auto& screen_info : screen_infos) {
        screen_info.texture.resource.Create();

        // Allocation of storage is deferred until the first frame, when we
        // know the framebuffer size.

        state.texture_units[0].texture_2d = screen_info.texture.resource.handle;
        state.Apply();

        glActiveTexture(GL_TEXTURE0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        screen_info.display_texture = screen_info.texture.resource.handle;
    }

    state.texture_units[0].texture_2d = 0;
    state.Apply();
}

void RendererOpenGL::ReloadShader(Settings::StereoRenderOption render_3d) {
    // Link shaders and get variable locations
    std::string shader_data = fragment_shader_precision_OES;
    if (render_3d == Settings::StereoRenderOption::Anaglyph) {
        if (Settings::values.anaglyph_shader_name.GetValue() == "Dubois (builtin)") {
            shader_data += HostShaders::OPENGL_PRESENT_ANAGLYPH_FRAG;
        } else {
            std::string shader_text = OpenGL::GetPostProcessingShaderCode(
                true, Settings::values.anaglyph_shader_name.GetValue());
            if (shader_text.empty()) {
                // Should probably provide some information that the shader couldn't load
                shader_data += HostShaders::OPENGL_PRESENT_ANAGLYPH_FRAG;
            } else {
                shader_data += shader_text;
            }
        }
    } else if (render_3d == Settings::StereoRenderOption::Interlaced ||
               render_3d == Settings::StereoRenderOption::ReverseInterlaced) {
        shader_data += HostShaders::OPENGL_PRESENT_INTERLACED_FRAG;
    } else {
        if (Settings::values.pp_shader_name.GetValue() == "None (builtin)") {
            shader_data += HostShaders::OPENGL_PRESENT_FRAG;
        } else {
            std::string shader_text = OpenGL::GetPostProcessingShaderCode(
                false, Settings::values.pp_shader_name.GetValue());
            if (shader_text.empty()) {
                // Should probably provide some information that the shader couldn't load
                shader_data += HostShaders::OPENGL_PRESENT_FRAG;
            } else {
                shader_data += shader_text;
            }
        }
    }
    shader.Create(HostShaders::OPENGL_PRESENT_VERT, shader_data);
    state.draw.shader_program = shader.handle;
    state.Apply();
    uniform_modelview_matrix = glGetUniformLocation(shader.handle, "modelview_matrix");
    uniform_color_texture = glGetUniformLocation(shader.handle, "color_texture");
    if (render_3d == Settings::StereoRenderOption::Anaglyph ||
        render_3d == Settings::StereoRenderOption::Interlaced ||
        render_3d == Settings::StereoRenderOption::ReverseInterlaced) {
        uniform_color_texture_r = glGetUniformLocation(shader.handle, "color_texture_r");
    }
    if (render_3d == Settings::StereoRenderOption::Interlaced ||
        render_3d == Settings::StereoRenderOption::ReverseInterlaced) {
        GLuint uniform_reverse_interlaced =
            glGetUniformLocation(shader.handle, "reverse_interlaced");
        if (render_3d == Settings::StereoRenderOption::ReverseInterlaced)
            glUniform1i(uniform_reverse_interlaced, 1);
        else
            glUniform1i(uniform_reverse_interlaced, 0);
    }
    uniform_i_resolution = glGetUniformLocation(shader.handle, "i_resolution");
    uniform_o_resolution = glGetUniformLocation(shader.handle, "o_resolution");
    uniform_layer = glGetUniformLocation(shader.handle, "layer");
    uniform_screen_origin = glGetUniformLocation(shader.handle, "u_screen_origin");
    uniform_corner_radius = glGetUniformLocation(shader.handle, "u_corner_radius");
    uniform_bg_color = glGetUniformLocation(shader.handle, "u_bg_color");
    uniform_edge_blur = glGetUniformLocation(shader.handle, "u_edge_blur");
    uniform_screen_opacity = glGetUniformLocation(shader.handle, "u_opacity");
    uniform_vignette_enable = glGetUniformLocation(shader.handle, "u_vignette_enable");
    uniform_vignette_color = glGetUniformLocation(shader.handle, "u_vignette_color");
    uniform_vignette_size = glGetUniformLocation(shader.handle, "u_vignette_size");
    uniform_overlay_enable = glGetUniformLocation(shader.handle, "u_overlay_enable");
    uniform_overlay_color = glGetUniformLocation(shader.handle, "u_overlay_color");
    attrib_position = glGetAttribLocation(shader.handle, "vert_position");
    attrib_tex_coord = glGetAttribLocation(shader.handle, "vert_tex_coord");

    // DiySC: Background fill shader
    std::string bg_frag = fragment_shader_precision_OES;
    bg_frag += HostShaders::OPENGL_BG_FILL_FRAG;
    bg_fill_shader.Create(HostShaders::OPENGL_BG_FILL_VERT, bg_frag);
    bg_fill_uniform_modelview_matrix = glGetUniformLocation(bg_fill_shader.handle, "modelview_matrix");
    bg_fill_uniform_color_texture = glGetUniformLocation(bg_fill_shader.handle, "color_texture");
    bg_fill_uniform_tex_size = glGetUniformLocation(bg_fill_shader.handle, "u_tex_size");
    bg_fill_uniform_blur_sigma = glGetUniformLocation(bg_fill_shader.handle, "u_blur_sigma");
    bg_fill_uniform_scale = glGetUniformLocation(bg_fill_shader.handle, "u_scale");
    bg_fill_uniform_darken = glGetUniformLocation(bg_fill_shader.handle, "u_darken");
    bg_fill_uniform_direction = glGetUniformLocation(bg_fill_shader.handle, "u_direction");
    bg_fill_uniform_max_radius = glGetUniformLocation(bg_fill_shader.handle, "u_max_radius");
}

void RendererOpenGL::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const Pica::FramebufferConfig& framebuffer,
                                                 const Pica::ColorFill& color_fill) {
    Pica::PixelFormat format = framebuffer.color_format;
    GLint internal_format{};
    u32 width, height;

    texture.format = format;
    width = texture.width = framebuffer.width;
    height = texture.height = framebuffer.height;
    if (color_fill.is_enabled) {
        width = 1;
        height = 1;
        format = Pica::PixelFormat::RGB8;
    }

    switch (format) {
    case Pica::PixelFormat::RGBA8:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = driver.IsOpenGLES() ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT_8_8_8_8;
        break;

    case Pica::PixelFormat::RGB8:
        // This pixel format uses BGR since GL_UNSIGNED_BYTE specifies byte-order, unlike every
        // specific OpenGL type used in this function using native-endian (that is, little-endian
        // mostly everywhere) for words or half-words.
        // TODO: check how those behave on big-endian processors.
        internal_format = GL_RGB;

        // GLES Dosen't support BGR , Use RGB instead
        texture.gl_format = driver.IsOpenGLES() ? GL_RGB : GL_BGR;
        texture.gl_type = GL_UNSIGNED_BYTE;
        break;

    case Pica::PixelFormat::RGB565:
        internal_format = GL_RGB;
        texture.gl_format = GL_RGB;
        texture.gl_type = GL_UNSIGNED_SHORT_5_6_5;
        break;

    case Pica::PixelFormat::RGB5A1:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GL_UNSIGNED_SHORT_5_5_5_1;
        break;

    case Pica::PixelFormat::RGBA4:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GL_UNSIGNED_SHORT_4_4_4_4;
        break;

    default:
        UNIMPLEMENTED();
    }

    state.texture_units[0].texture_2d = texture.resource.handle;
    state.Apply();

    glActiveTexture(GL_TEXTURE0);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, texture.gl_format,
                 texture.gl_type, nullptr);

    state.texture_units[0].texture_2d = 0;
    state.Apply();
}

/**
 * Draws a single texture to the emulator window, rotating the texture to correct for the 3DS's LCD
 * rotation.
 */
void RendererOpenGL::DrawSingleScreen(const ScreenInfo& screen_info, float x, float y, float w,
                                      float h, Layout::DisplayOrientation orientation) {
    const auto& texcoords = screen_info.display_texcoords;

    // DiySC: Compute clipped texture coordinates
    auto clipped_tc = texcoords;
    if (current_clip_left > 0.0f || current_clip_right > 0.0f ||
        current_clip_top > 0.0f || current_clip_bottom > 0.0f) {
        const float tc_x_range = clipped_tc.right - clipped_tc.left;
        const float tc_y_range = clipped_tc.top - clipped_tc.bottom;
        clipped_tc.left += tc_x_range * current_clip_left;
        clipped_tc.right -= tc_x_range * current_clip_right;
        clipped_tc.top -= tc_y_range * current_clip_top;
        clipped_tc.bottom += tc_y_range * current_clip_bottom;
    }

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, clipped_tc.bottom, clipped_tc.left),
            ScreenRectVertex(x + w, y, clipped_tc.bottom, clipped_tc.right),
            ScreenRectVertex(x, y + h, clipped_tc.top, clipped_tc.left),
            ScreenRectVertex(x + w, y + h, clipped_tc.top, clipped_tc.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, clipped_tc.bottom, clipped_tc.right),
            ScreenRectVertex(x + w, y, clipped_tc.top, clipped_tc.right),
            ScreenRectVertex(x, y + h, clipped_tc.bottom, clipped_tc.left),
            ScreenRectVertex(x + w, y + h, clipped_tc.top, clipped_tc.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, clipped_tc.top, clipped_tc.right),
            ScreenRectVertex(x + w, y, clipped_tc.top, clipped_tc.left),
            ScreenRectVertex(x, y + h, clipped_tc.bottom, clipped_tc.right),
            ScreenRectVertex(x + w, y + h, clipped_tc.bottom, clipped_tc.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, clipped_tc.top, clipped_tc.left),
            ScreenRectVertex(x + w, y, clipped_tc.bottom, clipped_tc.left),
            ScreenRectVertex(x, y + h, clipped_tc.top, clipped_tc.right),
            ScreenRectVertex(x + w, y + h, clipped_tc.bottom, clipped_tc.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_OpenGL, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u32 scale_factor = GetResolutionScaleFactor();
    const GLuint sampler = samplers[Settings::values.filter_mode.GetValue()].handle;
    glUniform4f(uniform_i_resolution, static_cast<float>(screen_info.texture.width * scale_factor),
                static_cast<float>(screen_info.texture.height * scale_factor),
                1.0f / static_cast<float>(screen_info.texture.width * scale_factor),
                1.0f / static_cast<float>(screen_info.texture.height * scale_factor));
    glUniform4f(uniform_o_resolution, h, w, 1.0f / h, 1.0f / w);

    // DiySC: Set rounded corner uniforms
    glUniform2f(uniform_screen_origin, current_screen_x, current_screen_y);
    glUniform1f(uniform_corner_radius, current_radius);
    glUniform4f(uniform_bg_color, Settings::values.bg_red.GetValue() / 255.0f,
                Settings::values.bg_green.GetValue() / 255.0f,
                Settings::values.bg_blue.GetValue() / 255.0f, 1.0f);
    glUniform1f(uniform_edge_blur, current_edge_blur);
    glUniform1f(uniform_screen_opacity, current_opacity);
    glUniform1i(uniform_vignette_enable, current_vignette_enabled ? 1 : 0);
    if (current_vignette_enabled) {
        glUniform3fv(uniform_vignette_color, 1, current_vignette_color.data());
        glUniform1f(uniform_vignette_size, current_vignette_size);
    }
    glUniform1i(uniform_overlay_enable, current_overlay_enabled ? 1 : 0);
    if (current_overlay_enabled) {
        glUniform3fv(uniform_overlay_color, 1, current_overlay_color.data());
    }

    state.texture_units[0].texture_2d = screen_info.display_texture;
    state.texture_units[0].sampler = sampler;
    state.Apply();

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    state.texture_units[0].texture_2d = 0;
    state.texture_units[0].sampler = 0;
    state.Apply();
}

/**
 * Draws a single texture to the emulator window, rotating the texture to correct for the 3DS's LCD
 * rotation.
 */
void RendererOpenGL::DrawSingleScreenStereo(const ScreenInfo& screen_info_l,
                                            const ScreenInfo& screen_info_r, float x, float y,
                                            float w, float h,
                                            Layout::DisplayOrientation orientation) {
    const auto& texcoords = screen_info_l.display_texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_OpenGL, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u32 scale_factor = GetResolutionScaleFactor();
    const GLuint sampler = samplers[Settings::values.filter_mode.GetValue()].handle;
    glUniform4f(uniform_i_resolution,
                static_cast<float>(screen_info_l.texture.width * scale_factor),
                static_cast<float>(screen_info_l.texture.height * scale_factor),
                1.0f / static_cast<float>(screen_info_l.texture.width * scale_factor),
                1.0f / static_cast<float>(screen_info_l.texture.height * scale_factor));
    glUniform4f(uniform_o_resolution, h, w, 1.0f / h, 1.0f / w);
    state.texture_units[0].texture_2d = screen_info_l.display_texture;
    state.texture_units[1].texture_2d = screen_info_r.display_texture;
    state.texture_units[0].sampler = sampler;
    state.texture_units[1].sampler = sampler;
    state.Apply();

    // DiySC: Apply clip and radius for stereo modes
    if (uniform_screen_origin != -1) {
        glUniform2f(uniform_screen_origin, current_screen_x, current_screen_y);
    }
    if (uniform_corner_radius != -1) {
        glUniform1f(uniform_corner_radius, current_radius);
    }
    if (uniform_bg_color != -1) {
        glUniform4f(uniform_bg_color, Settings::values.bg_red.GetValue() / 255.0f,
                    Settings::values.bg_green.GetValue() / 255.0f,
                    Settings::values.bg_blue.GetValue() / 255.0f, 1.0f);
    }
    if (uniform_edge_blur != -1) {
        glUniform1f(uniform_edge_blur, current_edge_blur);
    }
    if (uniform_screen_opacity != -1) {
        glUniform1f(uniform_screen_opacity, current_opacity);
    }
    // Adjust texcoords for clip
    if (current_clip_left > 0.0f || current_clip_right > 0.0f || current_clip_top > 0.0f ||
        current_clip_bottom > 0.0f) {
        float tex_w = texcoords.right - texcoords.left;
        float tex_h = texcoords.bottom - texcoords.top;
        float clip_l_norm = (current_clip_left / w) * tex_w;
        float clip_r_norm = (current_clip_right / w) * tex_w;
        float clip_t_norm = (current_clip_top / h) * tex_h;
        float clip_b_norm = (current_clip_bottom / h) * tex_h;
        for (auto& v : vertices) {
            v.tex_coord[0] = std::clamp(v.tex_coord[0],
                texcoords.left + clip_l_norm, texcoords.right - clip_r_norm);
            v.tex_coord[1] = std::clamp(v.tex_coord[1],
                texcoords.top + clip_t_norm, texcoords.bottom - clip_b_norm);
        }
    }

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices.data());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    state.texture_units[0].texture_2d = 0;
    state.texture_units[1].texture_2d = 0;
    state.texture_units[0].sampler = 0;
    state.texture_units[1].sampler = 0;
    state.Apply();
}

/**
 * Draws the emulated screens to the emulator window.
 */
void RendererOpenGL::DrawBackgroundFill(const Layout::FramebufferLayout& layout, bool flipped) {
    if (!layout.bg_blur_enabled) return;

    const auto& screen_info = screen_infos[layout.bg_blur_is_bottom ? 1 : 0];
    u32 tex_w = screen_info.texture.width;
    u32 tex_h = screen_info.texture.height;

    // Save current framebuffer so we can restore it after
    GLint prev_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    // Create/recreate intermediate FBO when source texture size changes
    if (tex_w != bg_fill_last_tex_w || tex_h != bg_fill_last_tex_h) {
        bg_fill_intermediate_texture.Release();
        bg_fill_intermediate_fbo.Release();

        bg_fill_intermediate_texture.Create();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bg_fill_intermediate_texture.handle);
        // GL_RGBA8 is guaranteed color-renderable; GL_RGB8 is not on all drivers
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)tex_w, (GLsizei)tex_h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        bg_fill_intermediate_fbo.Create();
        glBindFramebuffer(GL_FRAMEBUFFER, bg_fill_intermediate_fbo.handle);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               bg_fill_intermediate_texture.handle, 0);
        auto fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR(Render_OpenGL, "bg_fill intermediate FBO incomplete: 0x{:X}", fbo_status);
            bg_fill_intermediate_fbo.Release();
            bg_fill_intermediate_texture.Release();
            bg_fill_last_tex_w = 0;
            bg_fill_last_tex_h = 0;
            // Restore state and abort
            glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
            glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
            state.draw.shader_program = shader.handle;
            state.Apply();
            return;
        }

        bg_fill_last_tex_w = tex_w;
        bg_fill_last_tex_h = tex_h;
    }

    // Use the bg_fill shader for both passes
    state.draw.shader_program = bg_fill_shader.handle;
    state.Apply();

    // ── Pass 0: Horizontal blur → intermediate FBO ──
    {
        glBindFramebuffer(GL_FRAMEBUFFER, bg_fill_intermediate_fbo.handle);
        glViewport(0, 0, (GLsizei)tex_w, (GLsizei)tex_h);

        std::array<GLfloat, 6> ortho = MakeOrthographicMatrix((float)tex_w, (float)tex_h, false);
        glUniformMatrix3x2fv(bg_fill_uniform_modelview_matrix, 1, GL_FALSE, ortho.data());

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, screen_info.display_texture);
        glUniform1i(bg_fill_uniform_color_texture, 0);

        GLfloat tsize[2] = {(GLfloat)tex_w, (GLfloat)tex_h};
        glUniform2fv(bg_fill_uniform_tex_size, 1, tsize);
        glUniform1f(bg_fill_uniform_blur_sigma, layout.bg_blur_sigma);
        glUniform1i(bg_fill_uniform_max_radius, layout.bg_blur_max_radius);
        glUniform1i(bg_fill_uniform_direction, 0);

        struct BgVertex { GLfloat p[2]; GLfloat t[2]; };
        BgVertex v[4] = {
            {{0.0f, 0.0f}, {0.0f, 0.0f}},
            {{(GLfloat)tex_w, 0.0f}, {1.0f, 0.0f}},
            {{0.0f, (GLfloat)tex_h}, {0.0f, 1.0f}},
            {{(GLfloat)tex_w, (GLfloat)tex_h}, {1.0f, 1.0f}},
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // ── Pass 1: Vertical blur + LCD rotation → original framebuffer ──
    {
        glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
        glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

        // Compute centered rectangle with correct 3DS aspect ratio and zoom
        float src_ar = layout.bg_blur_is_bottom ? (4.0f / 3.0f) : (5.0f / 3.0f);
        float window_ar = (float)layout.width / (float)layout.height;
        float draw_w, draw_h;
        if (window_ar > src_ar) {
            draw_h = (float)layout.height * layout.bg_blur_scale;
            draw_w = draw_h * src_ar;
        } else {
            draw_w = (float)layout.width * layout.bg_blur_scale;
            draw_h = draw_w / src_ar;
        }
        float draw_x = ((float)layout.width - draw_w) / 2.0f;
        float draw_y = ((float)layout.height - draw_h) / 2.0f;

        std::array<GLfloat, 6> ortho = MakeOrthographicMatrix((float)layout.width, (float)layout.height, flipped);
        glUniformMatrix3x2fv(bg_fill_uniform_modelview_matrix, 1, GL_FALSE, ortho.data());

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bg_fill_intermediate_texture.handle);
        glUniform1i(bg_fill_uniform_color_texture, 0);

        GLfloat tsize[2] = {(GLfloat)tex_w, (GLfloat)tex_h};
        glUniform2fv(bg_fill_uniform_tex_size, 1, tsize);
        glUniform1f(bg_fill_uniform_blur_sigma, layout.bg_blur_sigma);
        glUniform1i(bg_fill_uniform_max_radius, layout.bg_blur_max_radius);
        glUniform1f(bg_fill_uniform_darken, layout.bg_blur_darken);
        glUniform1i(bg_fill_uniform_direction, 1);

        struct BgVertex { GLfloat p[2]; GLfloat t[2]; };
        BgVertex v[4] = {
            {{draw_x, draw_y}, {0.0f, 0.0f}},
            {{draw_x + draw_w, draw_y}, {1.0f, 0.0f}},
            {{draw_x, draw_y + draw_h}, {0.0f, 1.0f}},
            {{draw_x + draw_w, draw_y + draw_h}, {1.0f, 1.0f}},
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Restore main shader
    state.draw.shader_program = shader.handle;
    state.Apply();
    std::array<GLfloat, 6> ortho = MakeOrthographicMatrix((float)layout.width, (float)layout.height, flipped);
    glUniformMatrix3x2fv(uniform_modelview_matrix, 1, GL_FALSE, ortho.data());
    glUniform1i(uniform_color_texture, 0);
}

void RendererOpenGL::DrawScreens(const Layout::FramebufferLayout& layout, bool flipped) {
    if (settings.bg_color_update_requested.exchange(false)) {
        // Update background color before drawing
        glClearColor(Settings::values.bg_red.GetValue(), Settings::values.bg_green.GetValue(),
                     Settings::values.bg_blue.GetValue(), 1.0f);
    }

    if (settings.shader_update_requested.exchange(false)) {
        // Update fragment shader before drawing
        shader.Release();
        bg_fill_shader.Release();
        // Link shaders and get variable locations
        ReloadShader(layout.render_3d_mode);
    }

    const auto& top_screen = layout.top_screen;
    const auto& bottom_screen = layout.bottom_screen;

    glViewport(0, 0, layout.width, layout.height);
    if (render_window.NeedsClearing()) {
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // Set projection matrix
    std::array<GLfloat, 3 * 2> ortho_matrix =
        MakeOrthographicMatrix((float)layout.width, (float)layout.height, flipped);
    glUniformMatrix3x2fv(uniform_modelview_matrix, 1, GL_FALSE, ortho_matrix.data());

    // DiySC: Draw background blur fill first (behind screens, no clearing)
    // Only toggle blend enable (bg fill is opaque); don't disturb other state
    if (layout.bg_blur_enabled) {
        bool prev_blend = state.blend.enabled;
        state.blend.enabled = false;
        state.Apply();
        DrawBackgroundFill(layout, flipped);
        state.blend.enabled = prev_blend;
        state.Apply();
        glUniformMatrix3x2fv(uniform_modelview_matrix, 1, GL_FALSE, ortho_matrix.data());
    }

    // Bind texture in Texture Unit 0
    glUniform1i(uniform_color_texture, 0);

    const bool stereo_single_screen =
        layout.render_3d_mode == Settings::StereoRenderOption::Anaglyph ||
        layout.render_3d_mode == Settings::StereoRenderOption::Interlaced ||
        layout.render_3d_mode == Settings::StereoRenderOption::ReverseInterlaced;

    // Bind a second texture for the right eye if in Anaglyph mode
    if (stereo_single_screen) {
        glUniform1i(uniform_color_texture_r, 1);
    }

    glUniform1i(uniform_layer, 0);
    if (!Settings::values.swap_screen.GetValue()) {
        DrawTopScreen(layout, top_screen);
        glUniform1i(uniform_layer, 0);
        current_opacity = layout.bottom_opacity;
        ApplySecondLayerOpacity(layout.bottom_opacity);
        DrawBottomScreen(layout, bottom_screen);
    } else {
        DrawBottomScreen(layout, bottom_screen);
        glUniform1i(uniform_layer, 0);
        current_opacity = layout.top_opacity;
        ApplySecondLayerOpacity(layout.top_opacity);
        DrawTopScreen(layout, top_screen);
    }

    if (layout.additional_screen_enabled) {
        const auto& additional_screen = layout.additional_screen;
        if (!layout.additional_screen_is_bottom) {
            DrawTopScreen(layout, additional_screen);
        } else {
            DrawBottomScreen(layout, additional_screen);
        }
    }
    ResetSecondLayerOpacity();
}

void RendererOpenGL::ApplySecondLayerOpacity(float opacity) {
    // DiySC: Use per-pixel alpha blending for soft edges and rounded corners
    state.blend.src_rgb_func = GL_SRC_ALPHA;
    state.blend.dst_rgb_func = GL_ONE_MINUS_SRC_ALPHA;
    state.blend.src_a_func = GL_ONE;
    state.blend.dst_a_func = GL_ZERO;
    state.blend.color.alpha = 0.0f;
    // Global opacity is passed as uniform and multiplied in shader
    // Set via current_opacity in DrawTopScreen/DrawBottomScreen
}

void RendererOpenGL::ResetSecondLayerOpacity() {
    // DiySC: Use per-pixel alpha even for first layer (for rounded corners)
    state.blend.src_rgb_func = GL_SRC_ALPHA;
    state.blend.dst_rgb_func = GL_ONE_MINUS_SRC_ALPHA;
    state.blend.src_a_func = GL_ONE;
    state.blend.dst_a_func = GL_ZERO;
    state.blend.color.alpha = 0.0f;
}

void RendererOpenGL::DrawTopScreen(const Layout::FramebufferLayout& layout,
                                   const Common::Rectangle<u32>& top_screen) {
    if (!layout.top_screen_enabled) {
        return;
    }

    // DiySC: Set current per-screen clip and radius state
    current_clip_left = layout.top_clip_left;
    current_clip_right = layout.top_clip_right;
    current_clip_top = layout.top_clip_top;
    current_clip_bottom = layout.top_clip_bottom;
    current_radius = layout.top_radius;
    current_edge_blur = layout.top_edge_blur;
    current_opacity = 1.0f; // First layer is always fully opaque
    current_vignette_enabled = layout.top_vignette_enabled;
    current_vignette_color = layout.top_vignette_color;
    current_vignette_size = layout.top_vignette_size;
    current_overlay_enabled = layout.top_overlay_enabled;
    current_overlay_color = layout.top_overlay_color;
    current_screen_x = static_cast<float>(top_screen.left) + layout.top_offset_x;
    current_screen_y = static_cast<float>(top_screen.top) + layout.top_offset_y;

    int leftside, rightside;
    leftside = Settings::values.swap_eyes_3d.GetValue() ? 1 : 0;
    rightside = Settings::values.swap_eyes_3d.GetValue() ? 0 : 1;

    const float top_screen_left = static_cast<float>(top_screen.left) + layout.top_offset_x;
    const float top_screen_top = static_cast<float>(top_screen.top) + layout.top_offset_y;
    const float top_screen_width = static_cast<float>(top_screen.GetWidth());
    const float top_screen_height = static_cast<float>(top_screen.GetHeight());

    const auto orientation = layout.is_rotated ? Layout::DisplayOrientation::Landscape
                                               : Layout::DisplayOrientation::Portrait;
    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        const int eye = static_cast<int>(Settings::values.mono_render_option.GetValue());
        DrawSingleScreen(screen_infos[eye], top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySide: {
        DrawSingleScreen(screen_infos[leftside], top_screen_left / 2, top_screen_top,
                         top_screen_width / 2, top_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(screen_infos[rightside],
                         static_cast<float>((top_screen_left / 2) + (layout.width / 2)),
                         top_screen_top, top_screen_width / 2, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(screen_infos[leftside], top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(screen_infos[rightside],
                         static_cast<float>(top_screen_left + layout.width / 2), top_screen_top,
                         top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(screen_infos[leftside], top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(
            screen_infos[rightside],
            static_cast<float>(layout.cardboard.top_screen_right_eye + (layout.width / 2)),
            top_screen_top, top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(screen_infos[leftside], screen_infos[rightside], top_screen_left,
                               top_screen_top, top_screen_width, top_screen_height, orientation);
        break;
    }
    }
}

void RendererOpenGL::DrawBottomScreen(const Layout::FramebufferLayout& layout,
                                      const Common::Rectangle<u32>& bottom_screen) {
    if (!layout.bottom_screen_enabled) {
        return;
    }

    // DiySC: Set current per-screen clip and radius state
    current_clip_left = layout.bot_clip_left;
    current_clip_right = layout.bot_clip_right;
    current_clip_top = layout.bot_clip_top;
    current_clip_bottom = layout.bot_clip_bottom;
    current_radius = layout.bot_radius;
    current_edge_blur = layout.bot_edge_blur;
    current_vignette_enabled = layout.bot_vignette_enabled;
    current_vignette_color = layout.bot_vignette_color;
    current_vignette_size = layout.bot_vignette_size;
    current_overlay_enabled = layout.bot_overlay_enabled;
    current_overlay_color = layout.bot_overlay_color;
    current_screen_x = static_cast<float>(bottom_screen.left) + layout.bot_offset_x;
    current_screen_y = static_cast<float>(bottom_screen.top) + layout.bot_offset_y;

    const float bottom_screen_left = static_cast<float>(bottom_screen.left) + layout.bot_offset_x;
    const float bottom_screen_top = static_cast<float>(bottom_screen.top) + layout.bot_offset_y;
    const float bottom_screen_width = static_cast<float>(bottom_screen.GetWidth());
    const float bottom_screen_height = static_cast<float>(bottom_screen.GetHeight());

    const auto orientation = layout.is_rotated ? Layout::DisplayOrientation::Landscape
                                               : Layout::DisplayOrientation::Portrait;

    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        DrawSingleScreen(screen_infos[2], bottom_screen_left, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySide: // Bottom screen is identical on both sides
    {

        DrawSingleScreen(screen_infos[2], bottom_screen_left / 2, bottom_screen_top,
                         bottom_screen_width / 2, bottom_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(
            screen_infos[2], static_cast<float>((bottom_screen_left / 2) + (layout.width / 2)),
            bottom_screen_top, bottom_screen_width / 2, bottom_screen_height, orientation);

        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(screen_infos[2], bottom_screen_left, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(screen_infos[2], bottom_screen_left + layout.width / 2, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(screen_infos[2], bottom_screen_left, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        glUniform1i(uniform_layer, 1);
        DrawSingleScreen(
            screen_infos[2],
            static_cast<float>(layout.cardboard.bottom_screen_right_eye + (layout.width / 2)),
            bottom_screen_top, bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(screen_infos[2], screen_infos[2], bottom_screen_left,
                               bottom_screen_top, bottom_screen_width, bottom_screen_height,
                               orientation);
        break;
    }
    }
}

void RendererOpenGL::TryPresent(int timeout_ms, bool is_secondary) {
    const auto& window = is_secondary ? *secondary_window : render_window;
    const auto& layout = window.GetFramebufferLayout();
    auto frame = window.mailbox->TryGetPresentFrame(timeout_ms);
    if (!frame) {
        LOG_DEBUG(Render_OpenGL, "TryGetPresentFrame returned no frame to present");
        return;
    }

    // Clearing before a full overwrite of a fbo can signal to drivers that they can avoid a
    // readback since we won't be doing any blending
    glClear(GL_COLOR_BUFFER_BIT);

    // Recreate the presentation FBO if the color attachment was changed
    if (frame->color_reloaded) {
        LOG_DEBUG(Render_OpenGL, "Reloading present frame");
        window.mailbox->ReloadPresentFrame(frame, layout.width, layout.height);
    }
    glWaitSync(frame->render_fence, 0, GL_TIMEOUT_IGNORED);
    // INTEL workaround.
    // Normally we could just delete the draw fence here, but due to driver bugs, we can just delete
    // it on the emulation thread without too much penalty
    // glDeleteSync(frame.render_sync);
    // frame.render_sync = 0;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, frame->present.handle);
    glBlitFramebuffer(0, 0, frame->width, frame->height, 0, 0, layout.width, layout.height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // ── Multi-filter stacking: collect active filters ──
    {
        std::vector<std::string> active_filters;
        auto add_filter = [&](const std::string& name) {
            if (!name.empty() && name != "None (builtin)" && name != "None")
                active_filters.push_back(name);
        };
        add_filter(Settings::values.pp_shader_name.GetValue());
        add_filter(Settings::values.pp_shader_name_2.GetValue());
        add_filter(Settings::values.pp_shader_name_3.GetValue());
        add_filter(Settings::values.pp_shader_name_4.GetValue());
        add_filter(Settings::values.pp_shader_name_5.GetValue());
        add_filter(Settings::values.pp_shader_name_6.GetValue());
        add_filter(Settings::values.pp_shader_name_7.GetValue());
        add_filter(Settings::values.pp_shader_name_8.GetValue());
        add_filter(Settings::values.pp_shader_name_9.GetValue());
        add_filter(Settings::values.pp_shader_name_10.GetValue());

        if (active_filters.size() > 1) {
            GLint prev_read_fbo = 0, prev_draw_fbo = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);

            // Ensure ping-pong FBOs exist at output resolution
            if (filter_fbo_w != (u32)layout.width || filter_fbo_h != (u32)layout.height) {
                filter_tex_a.Release();
                filter_fbo_a.Release();
                filter_tex_b.Release();
                filter_fbo_b.Release();

                auto create_fbo = [&](OGLTexture& tex, OGLFramebuffer& fbo) {
                    tex.Create();
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, tex.handle);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, layout.width, layout.height, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    fbo.Create();
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo.handle);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                           tex.handle, 0);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                };
                create_fbo(filter_tex_a, filter_fbo_a);
                create_fbo(filter_tex_b, filter_fbo_b);
                filter_fbo_w = layout.width;
                filter_fbo_h = layout.height;
            }

            // Blit default framebuffer → FBO A (capture combined screen output)
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, filter_fbo_a.handle);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, layout.width, layout.height, 0, 0,
                              layout.width, layout.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

            // Prepare a simple fullscreen blit shader for filter passes
            if (filter_pass_shader.handle == 0) {
                std::string vert = fragment_shader_precision_OES;
                vert += R"(
layout(location = 0) in vec2 vert_position;
layout(location = 1) in vec2 vert_tex_coord;
layout(location = 0) out vec2 frag_tex_coord;
layout(location = 1) out vec2 frag_position;
void main() {
    frag_tex_coord = vert_tex_coord;
    frag_position = vert_position;
    gl_Position = vec4(vert_tex_coord * 2.0 - 1.0, 0.0, 1.0);
}
)";
                filter_pass_shader.Create(vert.c_str(), nullptr);
            }

            // Ping-pong through each filter
            OGLFramebuffer* src = &filter_fbo_a;
            OGLFramebuffer* dst = &filter_fbo_b;
            OGLTexture* src_tex = &filter_tex_a;

            // Fullscreen quad vertex + texcoord (triangle strip: 4 vertices)
            const float quad_verts[] = {
                -1.0f, -1.0f, 0.0f, 1.0f,   // bottom-left
                 1.0f, -1.0f, 1.0f, 1.0f,   // bottom-right
                -1.0f,  1.0f, 0.0f, 0.0f,   // top-left
                 1.0f,  1.0f, 1.0f, 0.0f,   // top-right
            };

            for (size_t fi = 0; fi < active_filters.size(); fi++) {
                std::string shader_text = OpenGL::GetPostProcessingShaderCode(
                    false, active_filters[fi]);
                if (shader_text.empty()) continue;

                std::string full_frag = fragment_shader_precision_OES + shader_text;
                std::string vert_src =
                    fragment_shader_precision_OES + std::string(R"(
layout(location = 0) in vec2 vert_position;
layout(location = 1) in vec2 vert_tex_coord;
layout(location = 0) out vec2 frag_tex_coord;
layout(location = 1) out vec2 frag_position;
void main() {
    frag_tex_coord = vert_tex_coord;
    frag_position = vert_position * 0.5 + 0.5; // [-1,1] → [0,1]
    gl_Position = vec4(vert_position, 0.0, 1.0);
}
)");
                OGLProgram filter_prog;
                filter_prog.Create(vert_src.c_str(), full_frag.c_str());

                glUseProgram(filter_prog.handle);
                glUniform1i(glGetUniformLocation(filter_prog.handle, "color_texture"), 0);
                GLint ires_loc = glGetUniformLocation(filter_prog.handle, "i_resolution");
                GLint ores_loc = glGetUniformLocation(filter_prog.handle, "o_resolution");
                if (ires_loc >= 0)
                    glUniform4f(ires_loc, (float)layout.width, (float)layout.height,
                                1.0f / layout.width, 1.0f / layout.height);
                if (ores_loc >= 0)
                    glUniform4f(ores_loc, (float)layout.width, (float)layout.height,
                                1.0f / layout.width, 1.0f / layout.height);

                glBindFramebuffer(GL_FRAMEBUFFER, dst->handle);
                glViewport(0, 0, layout.width, layout.height);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, src_tex->handle);
                glClear(GL_COLOR_BUFFER_BIT);

                // Set up fullscreen quad vertex attribs inline
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                      quad_verts);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                      quad_verts + 2);
                glEnableVertexAttribArray(0);
                glEnableVertexAttribArray(1);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                std::swap(src, dst);
                src_tex = (src == &filter_fbo_a) ? &filter_tex_a : &filter_tex_b;
            }

            // Blit final result back to default framebuffer
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, src->handle);
            glBlitFramebuffer(0, 0, layout.width, layout.height, 0, 0,
                              layout.width, layout.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw_fbo);
        }
    }

    // Delete the fence if we're re-presenting to avoid leaking fences
    if (frame->present_fence) {
        glDeleteSync(frame->present_fence);
    }

    /* insert fence for the main thread to block on */
    frame->present_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void RendererOpenGL::PrepareVideoDumping() {
    auto* mailbox = static_cast<OGLVideoDumpingMailbox*>(frame_dumper.mailbox.get());
    {
        std::scoped_lock lock{mailbox->swap_chain_lock};
        mailbox->quit = false;
    }
    frame_dumper.StartDumping();
}

void RendererOpenGL::CleanupVideoDumping() {
    frame_dumper.StopDumping();
    auto* mailbox = static_cast<OGLVideoDumpingMailbox*>(frame_dumper.mailbox.get());
    {
        std::scoped_lock lock{mailbox->swap_chain_lock};
        mailbox->quit = true;
    }
    mailbox->free_cv.notify_one();
}

} // namespace OpenGL
