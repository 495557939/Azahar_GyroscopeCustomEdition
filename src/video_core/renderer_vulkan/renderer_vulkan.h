// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "common/math_util.h"
#include "video_core/renderer_base.h"
#ifdef HAVE_LIBRETRO
#include "citra_libretro/libretro_vk.h"
#else
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#endif
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Core {
class System;
}

namespace Memory {
class MemorySystem;
}

namespace Pica {
class PicaCore;
}

namespace Layout {
struct FramebufferLayout;
}

namespace VideoCore {
class GPU;
}

namespace Vulkan {

struct TextureInfo {
    u32 width;
    u32 height;
    Pica::PixelFormat format;
    vk::Image image;
    vk::ImageView image_view;
    VmaAllocation allocation;
};

struct ScreenInfo {
    TextureInfo texture;
    Common::Rectangle<f32> texcoords;
    vk::ImageView image_view;
};

struct PresentUniformData {
    std::array<f32, 4 * 4> modelview;
    Common::Vec4f i_resolution;
    Common::Vec4f o_resolution;
    int screen_id_l = 0;
    int screen_id_r = 0;
    int layer = 0;
    int reverse_interlaced = 0;
    Common::Vec2f screen_origin;
    float corner_radius = 0.0f;
    float _pad0 = 0.0f; // std140 alignment: vec4 must be 16-byte aligned
    Common::Vec4f bg_color{0.0f, 0.0f, 0.0f, 1.0f};
    float edge_blur = 0.0f;
    float opacity = 1.0f;
    // DiySC: Vignette + Overlay (std140: vec3=16 bytes, need padding after each)
    float vignette_enable = 0.0f;
    float vignette_size = 0.5f;
    float overlay_enable = 0.0f;
    float _pad_v = 0.0f;
    Common::Vec3f vignette_color{0.0f, 0.0f, 0.0f};
    float _pad_vc = 0.0f; // std140 vec3 padding (12→16)
    Common::Vec3f overlay_color{0.5f, 0.5f, 0.5f};
    float _pad_oc = 0.0f; // std140 vec3 padding (12→16)
    float _pad1 = 0.0f;
};
static_assert(sizeof(PresentUniformData) == 204,
              "PresentUniformData does not match shader layout!");

// DiySC: Background fill push constant
struct BgFillUniformData {
    std::array<f32, 4 * 4> modelview;
    Common::Vec2f tex_size;
    float blur_sigma = 8.0f;
    float scale = 3.0f;
    float darken = 0.5f;
    s32 direction = 0;   // 0=horizontal pass, 1=vertical+display pass
    s32 max_radius = 64; // quality-controlled
    float _pad = 0.0f;
};
static_assert(sizeof(BgFillUniformData) == 96,
              "BgFillUniformData does not match shader layout!");

class RendererVulkan : public VideoCore::RendererBase {
    static constexpr std::size_t PRESENT_PIPELINES = 3;

public:
    explicit RendererVulkan(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window,
                            Frontend::EmuWindow* secondary_window);
    ~RendererVulkan() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return &rasterizer;
    }

    void NotifySurfaceChanged(bool second) override;

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override {}

private:
    void ReloadPipeline(Settings::StereoRenderOption render_3d);
    void CompileShaders();
    void BuildLayouts();
    void BuildPipelines();
    void ConfigureFramebufferTexture(TextureInfo& texture,
                                     const Pica::FramebufferConfig& framebuffer);
    void ConfigureRenderPipeline();
    void PrepareRendertarget();
    void RenderScreenshot();
    void RenderScreenshotWithStagingCopy();
    bool TryRenderScreenshotWithHostMemory();
    void PrepareDraw(Frame* frame, const Layout::FramebufferLayout& layout);
    void RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                        bool flipped);

    void DrawScreens(Frame* frame, const Layout::FramebufferLayout& layout, bool flipped);
    void DrawBackgroundFill(Frame* frame, const Layout::FramebufferLayout& layout, bool flipped);
    void DrawBottomScreen(const Layout::FramebufferLayout& layout,
                          const Common::Rectangle<u32>& bottom_screen);

    void DrawTopScreen(const Layout::FramebufferLayout& layout,
                       const Common::Rectangle<u32>& top_screen);
    void DrawSingleScreen(u32 screen_id, float x, float y, float w, float h,
                          Layout::DisplayOrientation orientation);
    void DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y, float w,
                                float h, Layout::DisplayOrientation orientation);

    void ApplySecondLayerOpacity(float alpha);

    void DrawCursor(const Layout::FramebufferLayout& layout);

    void LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer, ScreenInfo& screen_info,
                            bool right_eye);
    void FillScreen(Common::Vec3<u8> color, const TextureInfo& texture);

private:
    Memory::MemorySystem& memory;
    Pica::PicaCore& pica;

#ifdef HAVE_LIBRETRO
    LibRetroVKInstance instance;
#else
    Instance instance;
#endif
    Scheduler scheduler;
    RenderManager renderpass_cache;
    PresentWindow main_present_window;
    StreamBuffer vertex_buffer;
    DescriptorUpdateQueue update_queue;
    RasterizerVulkan rasterizer;
    std::unique_ptr<PresentWindow> secondary_present_window_ptr;

    DescriptorHeap present_heap;
    vk::UniquePipelineLayout present_pipeline_layout;
    std::array<vk::Pipeline, PRESENT_PIPELINES> present_pipelines;
    std::array<vk::ShaderModule, PRESENT_PIPELINES> present_shaders;
    std::array<vk::Sampler, 2> present_samplers;
    vk::ShaderModule bg_fill_vertex_shader{};
    vk::ShaderModule bg_fill_fragment_shader{};
    vk::Pipeline bg_fill_pipeline{};
    vk::UniquePipelineLayout bg_fill_pipeline_layout{};
    vk::ShaderModule present_vertex_shader;
    u32 current_pipeline = 0;

    std::array<ScreenInfo, 3> screen_infos{};
    PresentUniformData draw_info{};
    vk::ClearColorValue clear_color{};

    // DiySC: Current screen clip and radius state
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

    vk::ShaderModule cursor_vertex_shader{};
    vk::ShaderModule cursor_fragment_shader{};
    vk::Pipeline cursor_pipeline{};
    vk::UniquePipelineLayout cursor_pipeline_layout{};
};

} // namespace Vulkan
