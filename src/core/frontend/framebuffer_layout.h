// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/math_util.h"
#include "common/settings.h"

namespace Layout {

/// Orientation of the 3DS displays
enum class DisplayOrientation {
    Landscape,        // Default orientation of the 3DS
    Portrait,         // 3DS rotated 90 degrees counter-clockwise
    LandscapeFlipped, // 3DS rotated 180 degrees counter-clockwise
    PortraitFlipped,  // 3DS rotated 270 degrees counter-clockwise
};

/// Describes the horizontal coordinates for the right eye screen when using Cardboard VR
struct CardboardSettings {
    u32 top_screen_right_eye;
    u32 bottom_screen_right_eye;
    s32 user_x_shift;
};

/// Describes the layout of the window framebuffer (size and top/bottom screen positions)
struct FramebufferLayout {
    u32 width;
    u32 height;
    bool top_screen_enabled;
    bool bottom_screen_enabled;
    Common::Rectangle<u32> top_screen;
    Common::Rectangle<u32> bottom_screen;
    // is_rotated is true when the screen is in landscape mode - not sure why!
    bool is_rotated = true;
    bool additional_screen_enabled = false;
    // top_opacity is currently not used but could be used in the future
    float top_opacity = 1.0f;
    float bottom_opacity = 1.0f;
    bool additional_screen_is_bottom = false;
    Common::Rectangle<u32> additional_screen;
    CardboardSettings cardboard;

    // DiySC: Per-screen clip percentages (0.0-1.0, each edge independently)
    float top_clip_left = 0.0f;
    float top_clip_right = 0.0f;
    float top_clip_top = 0.0f;
    float top_clip_bottom = 0.0f;
    float bot_clip_left = 0.0f;
    float bot_clip_right = 0.0f;
    float bot_clip_top = 0.0f;
    float bot_clip_bottom = 0.0f;

    // DiySC: Corner radius in pixels (0 = no rounding)
    float top_radius = 0.0f;
    float bot_radius = 0.0f;
    // DiySC: Edge blur radius in pixels (0 = no blur, inward-only)
    float top_edge_blur = 0.0f;
    float bot_edge_blur = 0.0f;

    // DiySC: Position offsets for off-screen movement (negative = off left/top)
    float top_offset_x = 0.0f;
    float top_offset_y = 0.0f;
    float bot_offset_x = 0.0f;
    float bot_offset_y = 0.0f;

    // DiySC: Background blur fill
    bool bg_blur_enabled = false;
    bool bg_blur_is_bottom = false;
    float bg_blur_darken = 0.5f;
    float bg_blur_sigma = 8.0f;
    float bg_blur_scale = 3.0f;
    u32 bg_blur_max_radius = 64;
    // DiySC: Global background vignette
    bool bg_vignette_enabled = false;
    std::array<float, 3> bg_vignette_color = {0, 0, 0};
    float bg_vignette_size = 0.5f;
    // DiySC: Global background overlay
    bool bg_overlay_enabled = false;
    std::array<float, 3> bg_overlay_color = {0.5f, 0.5f, 0.5f};
    // DiySC: Per-screen vignette & overlay
    bool top_vignette_enabled = false;
    std::array<float, 3> top_vignette_color = {0, 0, 0};
    float top_vignette_size = 0.5f;
    bool bot_vignette_enabled = false;
    std::array<float, 3> bot_vignette_color = {0, 0, 0};
    float bot_vignette_size = 0.5f;
    bool top_overlay_enabled = false;
    std::array<float, 3> top_overlay_color = {0.5f, 0.5f, 0.5f};
    bool bot_overlay_enabled = false;
    std::array<float, 3> bot_overlay_color = {0.5f, 0.5f, 0.5f};

    /**
     * Returns the ratio of pixel size of the top screen, compared to the native size of the 3DS
     * screen.
     */
    u32 GetScalingRatio() const;

    static float GetAspectRatioValue(Settings::AspectRatio aspect_ratio);

    Settings::StereoRenderOption render_3d_mode = Settings::values.render_3d.GetValue();
};

/**
 * Method to create a rotated copy of a framebuffer layout, used to rotate to upright mode
 */
FramebufferLayout reverseLayout(FramebufferLayout layout);

/**
 * Factory method for constructing a default FramebufferLayout with screens on top of one another
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 * @param is_swapped if true, the bottom screen will be displayed above the top screen
 * @param upright if true, the screens will be rotated 90 degrees anti-clockwise
 * @return Newly created FramebufferLayout object with default screen regions initialized
 */
FramebufferLayout DefaultFrameLayout(u32 width, u32 height, bool is_swapped, bool upright);

/**
 * Factory method for constructing the mobile Full Width (Default) layout
 * Two screens at top, full width (so different heights)
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 * @param is_swapped if true, the bottom screen will be displayed above the top screen
 * @return Newly created FramebufferLayout object with mobile portrait screen regions initialized
 */
FramebufferLayout PortraitTopFullFrameLayout(u32 width, u32 height, bool is_swapped,
                                             bool upright = false);

/**
 * Factory method for constructing the mobile Original layout
 * Two screens at top, equal heights
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 * @param is_swapped if true, the bottom screen will be displayed above the top screen
 * @return Newly created FramebufferLayout object with mobile portrait screen regions initialized
 */
FramebufferLayout PortraitOriginalLayout(u32 width, u32 height, bool is_swapped,
                                         bool upright = false);

/**
 * Factory method for constructing a FramebufferLayout with only the top or bottom screen
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 * @param is_swapped if true, the bottom screen will be displayed (and the top won't be displayed)
 * @param upright if true, the screens will be rotated 90 degrees anti-clockwise
 * @return Newly created FramebufferLayout object with default screen regions initialized
 */
FramebufferLayout SingleFrameLayout(u32 width, u32 height, bool is_swapped, bool upright);

/**
 * Factory method for constructing a Frame with differently sized top and bottom windows
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 * @param is_swapped if true, the bottom screen will be the large display
 * @param upright if true, the screens will be rotated 90 degrees anti-clockwise
 * @param scale_factor The ratio between the large screen with respect to the smaller screen
 * @param vertical_alignment The vertical alignment of the smaller screen relative to the larger
 * screen
 * @return Newly created FramebufferLayout object with default screen regions initialized
 */
FramebufferLayout LargeFrameLayout(u32 width, u32 height, bool is_swapped, bool upright,
                                   float scale_factor,
                                   Settings::SmallScreenPosition small_screen_position);
/**
 * Factory method for constructing a frame with 2.5 times bigger top screen on the right,
 * and 1x top and bottom screen on the left
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 * @param is_swapped if true, the bottom screen will be the large display
 * @param upright if true, the screens will be rotated 90 degrees anti-clockwise
 * @param scale_factor determines the proportion of large to small. Must be >= 1
 * @param small_screen_position determines where the small screen appears relative to the large
 * screen
 * @return Newly created FramebufferLayout object with default screen regions initialized
 */
FramebufferLayout HybridScreenLayout(u32 width, u32 height, bool swapped, bool upright);

/**
 * Factory method for constructing a Frame with the Top screen and bottom
 * screen on separate windows
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 * @param is_secondary if true, the bottom screen will be enabled instead of the top screen
 * @return Newly created FramebufferLayout object with default screen regions initialized
 */
FramebufferLayout SeparateWindowsLayout(u32 width, u32 height, bool is_secondary, bool upright);

/**
 * Method for constructing the secondary layout for Android, based on
 * the appropriate setting.
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 */
FramebufferLayout AndroidSecondaryLayout(u32 width, u32 height);

/**
 * Factory method for constructing a custom FramebufferLayout
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 * @return Newly created FramebufferLayout object with default screen regions initialized
 */
FramebufferLayout CustomFrameLayout(u32 width, u32 height, bool is_swapped,
                                    bool is_portrait_mode = false);

/**
 * DiySC: Percentage-based custom layout with internal aspect ratio support.
 * All positions/sizes are percentages of the window/internal-canvas (0-10000 = 0.00%-100.00%).
 * @param width Window framebuffer width in pixels
 * @param height Window framebuffer height in pixels
 * @param is_swapped if true, screens are swapped
 * @return Newly created FramebufferLayout object
 */
FramebufferLayout CustomPercentFrameLayout(u32 width, u32 height, bool is_swapped);

/**
 * Convenience method to get frame layout by resolution scale
 * Read from the current settings to determine which layout to use.
 * @param res_scale resolution scale factor
 * @param is_portrait_mode defaults to false
 */
FramebufferLayout FrameLayoutFromResolutionScale(u32 res_scale, bool is_secondary = false,
                                                 bool is_portrait_mode = false);

/**
 * Convenience method for transforming a frame layout when using Cardboard VR
 * @param layout frame layout to transform
 * @return layout transformed with the user cardboard settings
 */
FramebufferLayout GetCardboardSettings(const FramebufferLayout& layout);

std::pair<unsigned, unsigned> GetMinimumSizeFromLayout(Settings::LayoutOption layout,
                                                       bool upright_screen);

std::pair<unsigned, unsigned> GetMinimumSizeFromPortraitLayout();

} // namespace Layout
