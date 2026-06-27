// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPainter>
#include <QWindow>
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <unordered_map>
#include "common/math_util.h"
#include "citra_qt/bootmanager.h"
#include "citra_qt/citra_qt.h"
#include "citra_qt/util/util.h"
#include "common/color.h"
#include "common/microprofile.h"
#include "common/param_package.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/3ds.h"
#include "core/core.h"
#ifdef HAVE_SDL2
#include <SDL.h>
#endif
#include "core/frontend/framebuffer_layout.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "input_common/motion_emu.h"
#include "input_common/mouse/mouse.h"
#include "video_core/custom_textures/custom_tex_manager.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_software/renderer_software.h"

#ifdef ENABLE_OPENGL
// clang-format off
#include <glad/glad.h> // Must be included first
// clang-format on
#include <QOffscreenSurface>
#include <QOpenGLContext>
#endif

#if defined(__APPLE__)
#include "util/metal_util.h"
#elif !defined(WIN32)
#include <qpa/qplatformnativeinterface.h>
#endif

static Frontend::WindowSystemType GetWindowSystemType();

EmuThread::EmuThread(Core::System& system_, Frontend::GraphicsContext& core_context)
    : system{system_}, core_context(core_context) {}

EmuThread::~EmuThread() = default;

static GMainWindow* GetMainWindow() {
    const auto widgets = qApp->topLevelWidgets();
    for (QWidget* w : widgets) {
        if (GMainWindow* main = qobject_cast<GMainWindow*>(w)) {
            return main;
        }
    }

    return nullptr;
}

void EmuThread::run() {
    MicroProfileOnThreadCreate("EmuThread");
    const auto scope = core_context.Acquire();

    if (Settings::values.custom_textures && Settings::values.preload_textures) {
        emit LoadProgress(VideoCore::LoadCallbackStage::Preload, 0, 0, "");
        system.CustomTexManager().PreloadTextures(
            stop_run,
            [this](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total,
                   const std::string& object) { emit LoadProgress(stage, value, total, object); });
    }

    system.GPU().Renderer().Rasterizer()->SetSwitchDiskResourcesCallback(
        [this](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total,
               const std::string& object) {
            emit SwitchDiskResources(stage, value, total, object);
        });

    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0, "");

    u64 program_id{};
    system.GetAppLoader().ReadProgramId(program_id);
    system.GPU().ApplyPerProgramSettings(program_id);

    system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(
        stop_run,
        [this](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total,
               const std::string& object) { emit LoadProgress(stage, value, total, object); });

    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0, "");

    core_context.MakeCurrent();

    if (system.frame_limiter.IsFrameAdvancing()) {
        // Usually the loading screen is hidden after the first frame is drawn. In this case
        // we hide it immediately as we need to wait for user input to start the emulation.
        emit HideLoadingScreen();
        system.frame_limiter.WaitOnce();
    }

    // Holds whether the cpu was running during the last iteration,
    // so that the DebugModeLeft signal can be emitted before the
    // next execution step.
    bool was_active = false;
    while (!stop_run) {
        if (running) {
            if (!was_active)
                emit DebugModeLeft();

            const Core::System::ResultStatus result = system.RunLoop();
            if (result == Core::System::ResultStatus::ShutdownRequested) {
                // Notify frontend we shutdown
                emit ErrorThrown(result, "");
                // End emulation execution
                break;
            }
            if (result != Core::System::ResultStatus::Success) {
                this->SetRunning(false);
                emit ErrorThrown(result, system.GetStatusDetails());
            }

            was_active = running || exec_step;
            if (!was_active && !stop_run)
                emit DebugModeEntered();
        } else if (exec_step) {
            if (!was_active)
                emit DebugModeLeft();

            exec_step = false;
            [[maybe_unused]] const Core::System::ResultStatus result = system.SingleStep();
            emit DebugModeEntered();
            yieldCurrentThread();

            was_active = false;
        } else {
            std::unique_lock lock{running_mutex};
            running_cv.wait(lock, [this] { return IsRunning() || exec_step || stop_run; });
        }
    }

    // Shutdown the core emulation
    system.Shutdown();

#if MICROPROFILE_ENABLED
    MicroProfileOnThreadExit();
#endif
}

#ifdef ENABLE_OPENGL
static std::unique_ptr<QOpenGLContext> CreateQOpenGLContext(bool gles) {
    QSurfaceFormat format;
    if (gles) {
        format.setRenderableType(QSurfaceFormat::RenderableType::OpenGLES);
        format.setVersion(3, 2);
    } else {
        format.setRenderableType(QSurfaceFormat::RenderableType::OpenGL);
        format.setVersion(4, 3);
    }
    format.setProfile(QSurfaceFormat::CoreProfile);

    if (Settings::values.renderer_debug) {
        format.setOption(QSurfaceFormat::FormatOption::DebugContext);
    }

    // TODO: expose a setting for buffer value (ie default/single/double/triple)
    format.setSwapBehavior(QSurfaceFormat::DefaultSwapBehavior);
    format.setSwapInterval(0);

    auto context = std::make_unique<QOpenGLContext>();
    context->setFormat(format);
    if (!context->create()) {
        LOG_ERROR(Frontend, "Unable to create OpenGL context with GLES = {}", gles);
        return nullptr;
    }
    return context;
}

class OpenGLSharedContext : public Frontend::GraphicsContext {
public:
    /// Create the original context that should be shared from
    explicit OpenGLSharedContext() {
        // First, try to create a context with the requested type.
        context = CreateQOpenGLContext(Settings::values.use_gles.GetValue());
        if (context == nullptr) {
            // On failure, fall back to context with flipped type.
            context = CreateQOpenGLContext(!Settings::values.use_gles.GetValue());
            if (context == nullptr) {
                LOG_ERROR(Frontend, "Unable to create any OpenGL context.");
            }
        }

        offscreen_surface = std::make_unique<QOffscreenSurface>(nullptr);
        offscreen_surface->setFormat(context->format());
        offscreen_surface->create();
        surface = offscreen_surface.get();
    }

    /// Create the shared contexts for rendering and presentation
    explicit OpenGLSharedContext(QOpenGLContext* share_context, QSurface* main_surface) {

        // disable vsync for any shared contexts
        auto format = share_context->format();
        format.setSwapInterval(0);

        context = std::make_unique<QOpenGLContext>();
        context->setShareContext(share_context);
        context->setFormat(format);
        if (!context->create()) {
            LOG_ERROR(Frontend, "Unable to create shared OpenGL context");
        }

        surface = main_surface;
    }

    ~OpenGLSharedContext() {
        OpenGLSharedContext::DoneCurrent();
    }

    bool IsGLES() override {
        return context->format().renderableType() == QSurfaceFormat::RenderableType::OpenGLES;
    }

    void SwapBuffers() override {
        context->swapBuffers(surface);
    }

    void MakeCurrent() override {
        // We can't track the current state of the underlying context in this wrapper class because
        // Qt may make the underlying context not current for one reason or another. In particular,
        // the WebBrowser uses GL, so it seems to conflict if we aren't careful.
        // Instead of always just making the context current (which does not have any caching to
        // check if the underlying context is already current) we can check for the current context
        // in the thread local data by calling `currentContext()` and checking if its ours.
        if (QOpenGLContext::currentContext() != context.get()) {
            context->makeCurrent(surface);
        }
    }

    void DoneCurrent() override {
        if (QOpenGLContext::currentContext() == context.get()) {
            context->doneCurrent();
        }
    }

    QOpenGLContext* GetShareContext() const {
        return context.get();
    }

private:
    // Avoid using Qt parent system here since we might move the QObjects to new threads
    // As a note, this means we should avoid using slots/signals with the objects too
    std::unique_ptr<QOpenGLContext> context;
    std::unique_ptr<QOffscreenSurface> offscreen_surface{};
    QSurface* surface;
};
#endif

class DummyContext : public Frontend::GraphicsContext {};

class RenderWidget : public QWidget {
public:
    RenderWidget(GRenderWindow* parent) : QWidget(parent) {
        setMouseTracking(true);
        update();
    }

    virtual ~RenderWidget() = default;
};

#ifdef ENABLE_OPENGL
class OpenGLRenderWidget : public RenderWidget {
public:
    explicit OpenGLRenderWidget(GRenderWindow* parent, Core::System& system_, bool is_secondary)
        : RenderWidget(parent), system(system_), is_secondary(is_secondary) {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        if (GetWindowSystemType() == Frontend::WindowSystemType::Wayland) {
            setAttribute(Qt::WA_DontCreateNativeAncestors);
        }
        windowHandle()->setSurfaceType(QWindow::OpenGLSurface);
    }

    void SetContext(std::unique_ptr<Frontend::GraphicsContext>&& context_) {
        context = std::move(context_);
    }

    void Present() {
        if (!isVisible()) {
            return;
        }
        if (!system.IsPoweredOn()) {
            return;
        }
        context->MakeCurrent();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        system.GPU().Renderer().TryPresent(100, is_secondary);
        context->SwapBuffers();
        glFinish();
    }

    void paintEvent(QPaintEvent* event) override {
        Present();
        update();
    }

    QPaintEngine* paintEngine() const override {
        return nullptr;
    }

private:
    std::unique_ptr<Frontend::GraphicsContext> context{};
    Core::System& system;
    bool is_secondary;
};
#endif

#ifdef ENABLE_VULKAN
class VulkanRenderWidget : public RenderWidget {
public:
    explicit VulkanRenderWidget(GRenderWindow* parent) : RenderWidget(parent) {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        if (GetWindowSystemType() == Frontend::WindowSystemType::Wayland) {
            setAttribute(Qt::WA_DontCreateNativeAncestors);
        }
#ifdef __APPLE__
        windowHandle()->setSurfaceType(QWindow::MetalSurface);
#else
        windowHandle()->setSurfaceType(QWindow::VulkanSurface);
#endif
    }

    QPaintEngine* paintEngine() const override {
        return nullptr;
    }
};
#endif

#ifdef ENABLE_SOFTWARE_RENDERER
struct SoftwareRenderWidget : public RenderWidget {
    explicit SoftwareRenderWidget(GRenderWindow* parent, Core::System& system_)
        : RenderWidget(parent), system(system_) {}

    void Present() {
        if (!isVisible()) {
            return;
        }
        if (!system.IsPoweredOn()) {
            return;
        }

        using VideoCore::ScreenId;

        const auto layout{Layout::DefaultFrameLayout(width(), height(), false, false)};
        QPainter painter(this);

        const auto draw_screen = [&](ScreenId screen_id) {
            const auto rect =
                screen_id == ScreenId::TopLeft ? layout.top_screen : layout.bottom_screen;
            const QImage screen =
                LoadFramebuffer(screen_id).scaled(rect.GetWidth(), rect.GetHeight());
            painter.drawImage(rect.left, rect.top, screen);
        };

        painter.fillRect(rect(), qRgb(Settings::values.bg_red.GetValue() * 255,
                                      Settings::values.bg_green.GetValue() * 255,
                                      Settings::values.bg_blue.GetValue() * 255));
        draw_screen(ScreenId::TopLeft);
        draw_screen(ScreenId::Bottom);

        painter.end();
    }

    void paintEvent(QPaintEvent* event) override {
        Present();
        update();
    }

    QImage LoadFramebuffer(VideoCore::ScreenId screen_id) {
        const auto& renderer = static_cast<SwRenderer::RendererSoftware&>(system.GPU().Renderer());
        const auto& info = renderer.Screen(screen_id);
        const int width = static_cast<int>(info.width);
        const int height = static_cast<int>(info.height);
        QImage image{height, width, QImage::Format_RGBA8888};
        std::memcpy(image.bits(), info.pixels.data(), info.pixels.size());
        return image;
    }

private:
    Core::System& system;
};
#endif

static Frontend::WindowSystemType GetWindowSystemType() {
    // Determine WSI type based on Qt platform.
    const QString platform_name = QGuiApplication::platformName();
    if (platform_name == QStringLiteral("windows"))
        return Frontend::WindowSystemType::Windows;
    else if (platform_name == QStringLiteral("xcb"))
        return Frontend::WindowSystemType::X11;
    else if (platform_name == QStringLiteral("wayland") ||
             platform_name == QStringLiteral("wayland-egl"))
        return Frontend::WindowSystemType::Wayland;
    else if (platform_name == QStringLiteral("cocoa") || platform_name == QStringLiteral("ios"))
        return Frontend::WindowSystemType::MacOS;

    LOG_CRITICAL(Frontend, "Unknown Qt platform!");
    return Frontend::WindowSystemType::Windows;
}

static Frontend::EmuWindow::WindowSystemInfo GetWindowSystemInfo(QWindow* window) {
    Frontend::EmuWindow::WindowSystemInfo wsi;
    wsi.type = GetWindowSystemType();

    if (window) {
#if defined(WIN32)
        // Our Win32 Qt external doesn't have the private API.
        wsi.render_surface = reinterpret_cast<void*>(window->winId());
#elif defined(__APPLE__)
        wsi.render_surface = MetalUtil::CreateMetalLayer(window->winId());
#else
        QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
        wsi.display_connection = pni->nativeResourceForWindow("display", window);
        if (wsi.type == Frontend::WindowSystemType::Wayland)
            wsi.render_surface = pni->nativeResourceForWindow("surface", window);
        else
            wsi.render_surface = reinterpret_cast<void*>(window->winId());
#endif
        wsi.render_surface_scale = static_cast<float>(window->devicePixelRatio());
    } else {
        wsi.render_surface = nullptr;
        wsi.render_surface_scale = 1.0f;
    }

    return wsi;
}

std::unique_ptr<Frontend::GraphicsContext> GRenderWindow::main_context;

GRenderWindow::GRenderWindow(QWidget* parent_, EmuThread* emu_thread_, Core::System& system_,
                             bool is_secondary_)
    : QWidget(parent_), EmuWindow(is_secondary_), emu_thread(emu_thread_), system{system_} {

    setAttribute(Qt::WA_AcceptTouchEvents);
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);

    this->setMouseTracking(true);

    // Global mouse polling for RateContinuous mode (free cursor, no warp lock).
    // Fires at 200 Hz, polls QCursor::pos() system-wide → AddDelta (accumulate).
    rc_poll_timer = new QTimer(this);
    rc_poll_timer->setInterval(5);
    rc_poll_timer->setTimerType(Qt::PreciseTimer);
    connect(rc_poll_timer, &QTimer::timeout, this, &GRenderWindow::PollRateContinuous);
    rc_poll_timer->start();

    // RateHold center-warp polling (also 200 Hz, timer-driven for consistency).
    rh_poll_timer = new QTimer(this);
    rh_poll_timer->setInterval(5);
    rh_poll_timer->setTimerType(Qt::PreciseTimer);
    connect(rh_poll_timer, &QTimer::timeout, this, &GRenderWindow::PollRateHold);
    // started/stopped by StartCenterWarp/StopCenterWarp

    // TiltContinuous: always-active gravity tilt (mouse % of screen center)
    tc_poll_timer = new QTimer(this);
    tc_poll_timer->setInterval(5);
    tc_poll_timer->setTimerType(Qt::PreciseTimer);
    connect(tc_poll_timer, &QTimer::timeout, this, &GRenderWindow::PollTiltContinuous);
    tc_poll_timer->start();

    // TiltHold center-warp gravity tilt (right-click cursor lock)
    th_poll_timer = new QTimer(this);
    th_poll_timer->setInterval(5);
    th_poll_timer->setTimerType(Qt::PreciseTimer);
    connect(th_poll_timer, &QTimer::timeout, this, &GRenderWindow::PollTiltHold);
    // started/stopped on right-click hold

    // Controller-to-mouse link polling (100Hz for stick/button reading)
    link_poll_timer = new QTimer(this);
    link_poll_timer->setInterval(10);
    link_poll_timer->setTimerType(Qt::PreciseTimer);
    connect(link_poll_timer, &QTimer::timeout, this, &GRenderWindow::PollControllerLink);
    link_poll_timer->start();
    strict_context_required = QGuiApplication::platformName() == QStringLiteral("wayland") ||
                              QGuiApplication::platformName() == QStringLiteral("wayland-egl");

    GMainWindow* parent = GetMainWindow();
    connect(this, &GRenderWindow::FirstFrameDisplayed, parent, &GMainWindow::OnLoadComplete);
}


void GRenderWindow::StartCenterWarp() {
    setCursor(Qt::BlankCursor);
    rh_center_pos = mapToGlobal(QPoint(width() / 2, height() / 2));
    QCursor::setPos(rh_center_pos);
    rh_warp_active = true;
    rh_poll_timer->start();
}

void GRenderWindow::StopCenterWarp() {
    rh_poll_timer->stop();
    rh_warp_active = false;
    setCursor(Qt::ArrowCursor);
}

void GRenderWindow::PollRateContinuous() {
    auto* motion_emu = InputCommon::GetMotionEmu();
    if (!motion_emu)
        return;

    // Only forward mouse input when running, not paused, and motion source is mouse.
    if (!emu_thread || !emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing())
        return;
    if (!isActiveWindow())
        return;
    if (Common::ParamPackage(Settings::values.current_input_profile.motion_device)
            .Get("engine", "") != "motion_emu")
        return;

    if (motion_emu->GetMode() != InputCommon::MotionEmuMode::RateContinuous)
        return;

    QPoint cur = QCursor::pos();

    if (rc_last_pos_valid) {
        // Cooldown ticks after a screen-edge warp.
        // SetCursorPos generates a synthetic WM_MOUSEMOVE and may race with
        // the mouse driver's raw-input pipeline. Suppress delta for a few ticks
        // to avoid spurious spikes causing ~180° rotation.
        if (rc_warp_cooldown > 0) {
            rc_warp_cooldown--;
            rc_last_pos = cur;
            return;
        }

        // Check for edge wrapping BEFORE computing delta.
        // On the wrap frame we skip the delta entirely — the edge pixel values
        // are unreliable due to the SetCursorPos synthetic event.
        QScreen* screen =
            windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
        if (screen) {
            constexpr int margin = 10;
            constexpr int warp_inset = 50;
            QRect geom = screen->geometry();
            QPoint warped = cur;
            bool need_warp = false;

            if (cur.x() >= geom.right() - margin) {
                warped.setX(geom.left() + warp_inset);
                need_warp = true;
            } else if (cur.x() <= geom.left() + margin) {
                warped.setX(geom.right() - warp_inset);
                need_warp = true;
            }
            if (cur.y() >= geom.bottom() - margin) {
                warped.setY(geom.top() + warp_inset);
                need_warp = true;
            } else if (cur.y() <= geom.top() + margin) {
                warped.setY(geom.bottom() - warp_inset);
                need_warp = true;
            }

            if (need_warp) {
                QCursor::setPos(warped);
                rc_warp_cooldown = 3; // warp tick + 2 recovery ticks (15ms)
                rc_last_pos = warped;
                return;
            }
        }

        // No warp, no cooldown: normal delta tracking
        float dx = static_cast<float>(cur.x() - rc_last_pos.x());
        float dy = static_cast<float>(cur.y() - rc_last_pos.y());

        // Per-tick cap to guard against driver-level cursor correction spikes
        // (some mouse drivers fight SetCursorPos by injecting large synthetic deltas)
        constexpr float max_delta_per_tick = 50.0f;
        dx = std::clamp(dx, -max_delta_per_tick, max_delta_per_tick);
        dy = std::clamp(dy, -max_delta_per_tick, max_delta_per_tick);

        if (dx != 0.0f || dy != 0.0f) {
            motion_emu->AddDelta(dx, dy);
        }
    }

    rc_last_pos = cur;
    rc_last_pos_valid = true;
}

void GRenderWindow::PollRateHold() {
    // Center-warp timer for RateHold (right-click cursor lock).
    // Fires at 200 Hz. Reads mouse offset from widget center, pushes AddDelta,
    // then warps cursor back to center — same principle as RateContinuous timer
    // but anchored to the widget center instead of absolute screen tracking.
    auto* motion_emu = InputCommon::GetMotionEmu();
    if (!motion_emu || !rh_warp_active)
        return;

    // Only forward mouse input when running, not paused, and motion source is mouse.
    if (!emu_thread || !emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing())
        return;
    if (!isActiveWindow())
        return;
    if (Common::ParamPackage(Settings::values.current_input_profile.motion_device)
            .Get("engine", "") != "motion_emu")
        return;

    if (motion_emu->GetMode() != InputCommon::MotionEmuMode::RateHold)
        return;

    // Refresh center in case window moved/resized
    rh_center_pos = mapToGlobal(QPoint(width() / 2, height() / 2));

    QPoint cur = QCursor::pos();
    float dx = static_cast<float>(cur.x() - rh_center_pos.x());
    float dy = static_cast<float>(cur.y() - rh_center_pos.y());

    if (dx != 0.0f || dy != 0.0f) {
        motion_emu->AddDelta(dx, dy);
    }

    QCursor::setPos(rh_center_pos);
}
void GRenderWindow::PollTiltContinuous() {
    // Always-active gravity tilt. Reads mouse position as % of screen center,
    // maps to tilt angle (yaw/pitch), feeds to motion_emu via SetTiltOffset.
    auto* motion_emu = InputCommon::GetMotionEmu();
    if (!motion_emu)
        return;

    // Only forward mouse input when running, not paused, and motion source is mouse.
    if (!emu_thread || !emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing())
        return;
    if (!isActiveWindow())
        return;
    if (Common::ParamPackage(Settings::values.current_input_profile.motion_device)
            .Get("engine", "") != "motion_emu")
        return;

    if (motion_emu->GetMode() != InputCommon::MotionEmuMode::TiltContinuous)
        return;

    float max_angle_rad = motion_emu->GetTiltMaxAngle() * Common::PI / 180.0f;
    if (max_angle_rad <= 0.0f)
        return;

    QScreen* screen =
        windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
    if (!screen)
        return;

    QRect geom = screen->geometry();
    QPoint center = geom.center();
    QPoint cur = QCursor::pos();

    // Normalize mouse offset to [-1, 1] relative to half-screen
    float dx = static_cast<float>(cur.x() - center.x()) / (geom.width() * 0.5f);
    float dy = static_cast<float>(cur.y() - center.y()) / (geom.height() * 0.5f);
    dx = std::clamp(dx, -1.0f, 1.0f);
    dy = std::clamp(dy, -1.0f, 1.0f);

    float yaw = dx * max_angle_rad;
    float pitch = -dy * max_angle_rad;  // screen Y inverted
    motion_emu->SetTiltOffset(yaw, pitch);
}

void GRenderWindow::PollTiltHold() {
    // Right-click gravity tilt. Cursor locked to widget center (like RateHold
    // but outputs gravity tilt instead of angular velocity).
    auto* motion_emu = InputCommon::GetMotionEmu();
    if (!motion_emu || !th_warp_active)
        return;

    // Only forward mouse input when running, not paused, and motion source is mouse.
    if (!emu_thread || !emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing())
        return;
    if (!isActiveWindow())
        return;
    if (Common::ParamPackage(Settings::values.current_input_profile.motion_device)
            .Get("engine", "") != "motion_emu")
        return;

    if (motion_emu->GetMode() != InputCommon::MotionEmuMode::TiltHold)
        return;

    float max_angle_rad = motion_emu->GetTiltMaxAngle() * Common::PI / 180.0f;
    if (max_angle_rad <= 0.0f)
        return;

    // Refresh center in case window moved/resized
    th_center_pos = mapToGlobal(QPoint(width() / 2, height() / 2));

    QPoint cur = QCursor::pos();
    float dx = static_cast<float>(cur.x() - th_center_pos.x());
    float dy = static_cast<float>(cur.y() - th_center_pos.y());

    // Normalize to widget half-size, clamp to [-1, 1]
    float half_w = width() * 0.5f;
    float half_h = height() * 0.5f;
    if (half_w > 0.0f) dx /= half_w;
    if (half_h > 0.0f) dy /= half_h;
    dx = std::clamp(dx, -1.0f, 1.0f);
    dy = std::clamp(dy, -1.0f, 1.0f);

    float yaw = dx * max_angle_rad;
    float pitch = -dy * max_angle_rad;
    motion_emu->SetTiltOffset(yaw, pitch);

    // Warp cursor back to center (same as RateHold center-warp)
    QCursor::setPos(th_center_pos);
}

void GRenderWindow::PollControllerLink() {
#ifdef HAVE_SDL2
    // Only active when emulation is running, window is focused, and motion is mouse-based.
    if (!emu_thread || !emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing())
        return;
    if (!isActiveWindow())
        return;

    Common::ParamPackage motion_param(
        Settings::values.current_input_profile.motion_device);
    if (motion_param.Get("engine", "") != "motion_emu")
        return;

    const bool link_cstick = motion_param.Get("link_cstick", false);
    const bool link_circle = motion_param.Get("link_circle_pad", false);
    const bool link_dpad = motion_param.Get("link_dpad", false);
    const bool link_abxy = motion_param.Get("link_abxy", false);
    if (!link_cstick && !link_circle && !link_dpad && !link_abxy)
        return;

    // Read base multiplier settings (default link_speed 1.0)
    const float link_speed = motion_param.Get("link_speed", 1.0f);

    // Per-link invert flags
    const bool cstick_inv_ud  = motion_param.Get("link_cstick_inv_ud", false);
    const bool cstick_inv_lr  = motion_param.Get("link_cstick_inv_lr", false);
    const bool cpad_inv_ud    = motion_param.Get("link_cpad_inv_ud", false);
    const bool cpad_inv_lr    = motion_param.Get("link_cpad_inv_lr", false);
    const bool dpad_inv_ud    = motion_param.Get("link_dpad_inv_ud", false);
    const bool dpad_inv_lr    = motion_param.Get("link_dpad_inv_lr", false);
    const bool abxy_inv_ud    = motion_param.Get("link_abxy_inv_ud", false);
    const bool abxy_inv_lr    = motion_param.Get("link_abxy_inv_lr", false);

    constexpr float DEADZONE = 0.15f;
    constexpr float STICK_BASE = 8.0f;
    constexpr float BUTTON_BASE = 8.0f;

    // Pre-open SDL controller for button/axis checks
    SDL_GameController* ctrl = nullptr;
    {
        int num_joy = SDL_NumJoysticks();
        for (int idx = 0; idx < num_joy; ++idx) {
            if (SDL_IsGameController(idx)) {
                ctrl = SDL_GameControllerOpen(idx);
                if (ctrl) break;
            }
        }
    }

    // Helper: check single binding (keyboard code or SDL input)
    auto CheckBinding = [&](const std::string& raw) -> float {
        if (raw.empty()) return 0.0f;
        Common::ParamPackage p(raw);
        const std::string engine = p.Get("engine", "");
        if (engine == "keyboard") {
            int code = p.Get("code", 0);
            if (code > 0 && (pressed_keys.count(code) > 0))
                return BUTTON_BASE;
#ifdef _WIN32
            // Fallback: Qt arrow keys may be swallowed by focus navigation
            if (code > 0) {
                int vk = code;
                if (vk >= 0x1000000) { // Qt special-key range
                    static const std::unordered_map<int, int> kQtToVk = {
                        {0x01000013, VK_UP},    {0x01000015, VK_DOWN},
                        {0x01000012, VK_LEFT},  {0x01000014, VK_RIGHT},
                        {0x01000020, VK_SPACE}, {0x01000021, VK_NEXT},
                        {0x01000022, VK_PRIOR}, {0x01000023, VK_END},
                        {0x01000024, VK_HOME},  {0x01000007, VK_DELETE},
                        {0x01000000, VK_ESCAPE},{0x01000001, VK_TAB},
                        {0x01000005, VK_RETURN},{0x01000010, VK_MENU},
                    };
                    auto it = kQtToVk.find(vk);
                    if (it != kQtToVk.end()) vk = it->second;
                }
                if (GetAsyncKeyState(vk) & 0x8000)
                    return BUTTON_BASE;
            }
#endif
        } else if (engine == "sdl" && ctrl) {
            if (p.Has("gc_button")) {
                int gc_btn = p.Get("gc_button", -1);
                if (gc_btn >= 0 && SDL_GameControllerGetButton(ctrl,
                        static_cast<SDL_GameControllerButton>(gc_btn)))
                    return BUTTON_BASE;
            }
            if (p.Has("gc_axis")) {
                int gc_axis = p.Get("gc_axis", -1);
                if (gc_axis >= 0) {
                    float raw_v = SDL_GameControllerGetAxis(ctrl,
                        static_cast<SDL_GameControllerAxis>(gc_axis)) * (1.0f / 32768.0f);
                    std::string dir = p.Get("direction", "");
                    // Direction sign is a gate; return positive magnitude
                    if (dir == "+" && raw_v > DEADZONE) return std::abs(raw_v) * STICK_BASE;
                    if (dir == "-" && raw_v < -DEADZONE) return std::abs(raw_v) * STICK_BASE;
                }
            }
            if (p.Has("hat")) {
                int hat = p.Get("hat", -1);
                std::string dir = p.Get("direction", "");
                if (hat >= 0) {
                    Uint8 val = SDL_JoystickGetHat(
                        SDL_GameControllerGetJoystick(ctrl), hat);
                    if (dir == "up" && (val & SDL_HAT_UP)) return BUTTON_BASE;
                    if (dir == "down" && (val & SDL_HAT_DOWN)) return BUTTON_BASE;
                    if (dir == "left" && (val & SDL_HAT_LEFT)) return BUTTON_BASE;
                    if (dir == "right" && (val & SDL_HAT_RIGHT)) return BUTTON_BASE;
                }
            }
            if (p.Has("axis")) {
                int axis = p.Get("axis", -1);
                if (axis >= 0) {
                    float raw_v = SDL_JoystickGetAxis(
                        SDL_GameControllerGetJoystick(ctrl), axis) * (1.0f / 32768.0f);
                    std::string dir = p.Get("direction", "");
                    if (dir == "+" && raw_v > DEADZONE) return std::abs(raw_v) * STICK_BASE;
                    if (dir == "-" && raw_v < -DEADZONE) return std::abs(raw_v) * STICK_BASE;
                }
            }
            if (p.Has("button")) {
                int btn = p.Get("button", -1);
                if (btn >= 0 && SDL_JoystickGetButton(
                        SDL_GameControllerGetJoystick(ctrl), btn))
                    return BUTTON_BASE;
            }
        }
        return 0.0f;
    };

    // Helper: read direction deltas from an analog ParamPackage.
    // Supports multi-bindings (up, up_1, up_2, ...) for keyboard + SDL coexistence.
    auto ReadAnalogDir = [&](const std::string& analog_param, float& dx, float& dy) -> bool {
        Common::ParamPackage ap(analog_param);
        auto BestForDir = [&](const std::string& base) -> float {
            float best = 0.0f;
            // Check legacy single binding (key "up", "down", ...) and slot-0 binding
            float s = CheckBinding(ap.Get(base, ""));
            if (s > best) best = s;
            // Multi-binding: check _0, _1, _2 ... up to _count or a reasonable limit
            for (int i = 0; i <= 4; ++i) {
                s = CheckBinding(ap.Get(base + "_" + std::to_string(i), ""));
                if (s > best) best = s;
            }
            return best;
        };
        float right_s = BestForDir("right");
        float left_s  = BestForDir("left");
        float down_s  = BestForDir("down");
        float up_s    = BestForDir("up");
        dx = right_s - left_s;
        dy = down_s - up_s;
        return (std::abs(dx) > 0.001f || std::abs(dy) > 0.001f);
    };

    float total_dx = 0.0f, total_dy = 0.0f;

    // Read C-Stick / Circle Pad from analog profile (supports keyboard + SDL bindings)
    const auto& profile = Settings::values.current_input_profile;
    float adx = 0.0f, ady = 0.0f;
    if (link_cstick && ReadAnalogDir(profile.analogs[Settings::NativeAnalog::CStick], adx, ady)) {
        if (cstick_inv_lr) adx = -adx;
        if (cstick_inv_ud) ady = -ady;
        total_dx += adx;
        total_dy += ady;
    }
    if (link_circle && ReadAnalogDir(profile.analogs[Settings::NativeAnalog::CirclePad], adx, ady)) {
        if (cpad_inv_lr) adx = -adx;
        if (cpad_inv_ud) ady = -ady;
        total_dx += adx;
        total_dy += ady;
    }

    // Read D-Pad / ABXY from button profile
    struct DirMap { float dx; float dy; };
    static const std::unordered_map<int, DirMap> kDirMap = {
        {Settings::NativeButton::A,     { 1.0f,  0.0f}},
        {Settings::NativeButton::B,     { 0.0f,  1.0f}},
        {Settings::NativeButton::X,     { 0.0f, -1.0f}},
        {Settings::NativeButton::Y,     {-1.0f,  0.0f}},
        {Settings::NativeButton::Up,    { 0.0f, -1.0f}},
        {Settings::NativeButton::Down,  { 0.0f,  1.0f}},
        {Settings::NativeButton::Left,  {-1.0f,  0.0f}},
        {Settings::NativeButton::Right, { 1.0f,  0.0f}},
    };
    auto IsLinked = [&](int btn) -> bool {
        if (btn == Settings::NativeButton::A || btn == Settings::NativeButton::B ||
            btn == Settings::NativeButton::X || btn == Settings::NativeButton::Y)
            return link_abxy;
        if (btn == Settings::NativeButton::Up || btn == Settings::NativeButton::Down ||
            btn == Settings::NativeButton::Left || btn == Settings::NativeButton::Right)
            return link_dpad;
        return false;
    };
    for (int btn = 0; btn < Settings::NativeButton::NumButtons; ++btn) {
        if (!IsLinked(btn)) continue;
        auto it = kDirMap.find(btn);
        if (it == kDirMap.end()) continue;
        float strength = 0.0f;
        for (const auto& binding_str : profile.buttons[btn]) {
            float s = CheckBinding(binding_str);
            if (s > strength) strength = s;
        }
        if (strength > 0.0f) {
            // Determine invert category for this button
            bool is_dpad = (btn == Settings::NativeButton::Up ||
                            btn == Settings::NativeButton::Down ||
                            btn == Settings::NativeButton::Left ||
                            btn == Settings::NativeButton::Right);
            float fx = is_dpad ? (dpad_inv_lr ? -1.0f : 1.0f) : (abxy_inv_lr ? -1.0f : 1.0f);
            float fy = is_dpad ? (dpad_inv_ud ? -1.0f : 1.0f) : (abxy_inv_ud ? -1.0f : 1.0f);
            total_dx += it->second.dx * strength * fx;
            total_dy += it->second.dy * strength * fy;
        }
    }

    if (ctrl)
        SDL_GameControllerClose(ctrl);

    // Apply speed multiplier
    total_dx *= link_speed;
    total_dy *= link_speed;

    // Feed virtual delta to MotionEmu
    auto* motion_emu = InputCommon::GetMotionEmu();
    if (!motion_emu)
        return;

    // Feed horizontal stick delta to [BETA] Auto X-axis tilt.
    // Only feed when controller links are actually configured to avoid
    // overwriting auto_roll_target with 0 every poll cycle.
    if (motion_param.Get("auto_tilt_x", false) &&
        (link_cstick || link_circle || link_dpad || link_abxy)) {
        motion_emu->SetAutoRollTarget(total_dx);
    }

    if (total_dx == 0.0f && total_dy == 0.0f)
        return;

    auto mode = motion_emu->GetMode();
    if (mode == InputCommon::MotionEmuMode::RateContinuous ||
        mode == InputCommon::MotionEmuMode::RateHold) {
        motion_emu->AddDelta(total_dx, total_dy);
    } else if (mode == InputCommon::MotionEmuMode::TiltContinuous ||
               mode == InputCommon::MotionEmuMode::TiltHold) {
        static float accum_x = 0.0f, accum_y = 0.0f;
        accum_x += total_dx * 0.005f;
        accum_y += total_dy * 0.005f;
        accum_x = std::clamp(accum_x, -1.0f, 1.0f);
        accum_y = std::clamp(accum_y, -1.0f, 1.0f);
        float max_angle_rad = motion_emu->GetTiltMaxAngle() * Common::PI / 180.0f;
        motion_emu->SetTiltOffset(accum_x * max_angle_rad,
                                  -accum_y * max_angle_rad);
    }
#endif
}


GRenderWindow::~GRenderWindow() = default;

void GRenderWindow::MakeCurrent() {
    main_context->MakeCurrent();
}

void GRenderWindow::DoneCurrent() {
    main_context->DoneCurrent();
}

void GRenderWindow::PollEvents() {
    if (!first_frame) {
        first_frame = true;
        emit FirstFrameDisplayed();
    }
}

// On Qt 5.0+, this correctly gets the size of the framebuffer (pixels).
//
// Older versions get the window size (density independent pixels),
// and hence, do not support DPI scaling ("retina" displays).
// The result will be a viewport that is smaller than the extent of the window.
void GRenderWindow::OnFramebufferSizeChanged() {
    // Screen changes potentially incur a change in screen DPI, hence we should update the
    // framebuffer size
    const qreal pixel_ratio = windowPixelRatio();
    const u32 width = static_cast<u32>(this->width() * pixel_ratio);
    const u32 height = static_cast<u32>(this->height() * pixel_ratio);
    UpdateCurrentFramebufferLayout(width, height);
}

void GRenderWindow::BackupGeometry() {
    geometry = QWidget::saveGeometry();
}

void GRenderWindow::RestoreGeometry() {
    // We don't want to back up the geometry here (obviously)
    QWidget::restoreGeometry(geometry);
}

void GRenderWindow::restoreGeometry(const QByteArray& geometry) {
    // Make sure users of this class don't need to deal with backing up the geometry themselves
    QWidget::restoreGeometry(geometry);
    BackupGeometry();
}

QByteArray GRenderWindow::saveGeometry() {
    // If we are a top-level widget, store the current geometry
    // otherwise, store the last backup
    if (parent() == nullptr) {
        return QWidget::saveGeometry();
    }

    return geometry;
}

qreal GRenderWindow::windowPixelRatio() const {
    return devicePixelRatioF();
}

std::pair<u32, u32> GRenderWindow::ScaleTouch(const QPointF pos) const {
    const qreal pixel_ratio = windowPixelRatio();
    return {static_cast<u32>(std::max(std::round(pos.x() * pixel_ratio), qreal{0.0})),
            static_cast<u32>(std::max(std::round(pos.y() * pixel_ratio), qreal{0.0}))};
}

void GRenderWindow::closeEvent(QCloseEvent* event) {
    emit Closed();
    QWidget::closeEvent(event);
}

void GRenderWindow::keyPressEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->PressKey(event->key());
    pressed_keys.insert(event->key());
}

void GRenderWindow::keyReleaseEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->ReleaseKey(event->key());
    pressed_keys.erase(event->key());
}

void GRenderWindow::mousePressEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem) {
        return; // touch input is handled in TouchBeginEvent
    }

    auto pos = event->pos();

    // Forward mouse button state for multi-key mapping
    if (event->button() == Qt::LeftButton)
        InputCommon::MouseState::Instance().PressButton(0);
    else if (event->button() == Qt::RightButton)
        InputCommon::MouseState::Instance().PressButton(1);
    else if (event->button() == Qt::MiddleButton)
        InputCommon::MouseState::Instance().PressButton(2);

    // Only forward mouse input to the game when emulation is actively running
    // (not paused or stopped), window has focus, and motion source is mouse.
    if (!emu_thread || !emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing())
        return;
    if (!isActiveWindow())
        return;

    const bool motion_is_mouse =
        Common::ParamPackage(Settings::values.current_input_profile.motion_device)
            .Get("engine", "") == "motion_emu";
    if (!motion_is_mouse)
        return;

    auto* motion_emu = InputCommon::GetMotionEmu();
    auto mode = motion_emu->GetMode();

    if (event->button() == Qt::LeftButton) {
        const auto [x, y] = ScaleTouch(pos);
        this->TouchPressed(x, y);
    } else if (event->button() == Qt::RightButton) {
        // Right-click only applies to Absolute, RateHold, TiltHold.
        // RateContinuous / TiltContinuous are always-on — no right-click action.
        if (mode == InputCommon::MotionEmuMode::Absolute) {
            motion_emu->BeginTilt(pos.x(), pos.y());
        } else if (mode == InputCommon::MotionEmuMode::RateHold) {
            motion_emu->BeginTilt(pos.x(), pos.y());
            StartCenterWarp();
        } else if (mode == InputCommon::MotionEmuMode::TiltHold) {
            motion_emu->BeginTilt(pos.x(), pos.y());
            // Cursor-lock: hide + warp to center
            setCursor(Qt::BlankCursor);
            th_center_pos = mapToGlobal(QPoint(width() / 2, height() / 2));
            QCursor::setPos(th_center_pos);
            th_warp_active = true;
            th_poll_timer->start();
        }
    }
    emit MouseActivity();
}

void GRenderWindow::mouseMoveEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem) {
        return; // touch input is handled in TouchUpdateEvent
    }

    // Only forward mouse input to the game when emulation is actively running
    // (not paused or stopped), window has focus, and motion source is mouse.
    if (!emu_thread || !emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing())
        return;
    if (!isActiveWindow())
        return;

    const bool motion_is_mouse =
        Common::ParamPackage(Settings::values.current_input_profile.motion_device)
            .Get("engine", "") == "motion_emu";
    if (!motion_is_mouse)
        return;

    auto* motion_emu = InputCommon::GetMotionEmu();
    auto mode = motion_emu->GetMode();
    bool is_rate_mode = (mode == InputCommon::MotionEmuMode::RateHold ||
                       mode == InputCommon::MotionEmuMode::RateContinuous);
    bool is_tilt_mode = (mode == InputCommon::MotionEmuMode::TiltContinuous ||
                        mode == InputCommon::MotionEmuMode::TiltHold);

    if ((is_rate_mode || is_tilt_mode) && motion_emu->IsDeviceActive()) {
        // Timer-driven modes: mouseMoveEvent does nothing for tilt/rotation.
    } else {
        // Absolute mode or not active: touch path
        if (cursor().shape() == Qt::BlankCursor && !th_warp_active) {
            setCursor(Qt::ArrowCursor);
        }
        auto pos = event->pos();
        const auto [x, y] = ScaleTouch(pos);
        this->TouchMoved(x, y);
        if (mode == InputCommon::MotionEmuMode::Absolute) {
            motion_emu->Tilt(pos.x(), pos.y());
        }
        // Tilt modes: gravity set via timer, skip Tilt()
    }
    emit MouseActivity();
}

void GRenderWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem) {
        return; // touch input is handled in TouchEndEvent
    }

    // Forward mouse button release for multi-key mapping
    if (event->button() == Qt::LeftButton)
        InputCommon::MouseState::Instance().ReleaseButton(0);
    else if (event->button() == Qt::RightButton)
        InputCommon::MouseState::Instance().ReleaseButton(1);
    else if (event->button() == Qt::MiddleButton)
        InputCommon::MouseState::Instance().ReleaseButton(2);

    // Only forward mouse input to the game when emulation is actively running
    // (not paused or stopped), window has focus, and motion source is mouse.
    if (!emu_thread || !emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing())
        return;
    if (!isActiveWindow())
        return;

    const bool motion_is_mouse =
        Common::ParamPackage(Settings::values.current_input_profile.motion_device)
            .Get("engine", "") == "motion_emu";
    if (!motion_is_mouse)
        return;

    auto* motion_emu = InputCommon::GetMotionEmu();
    auto mode = motion_emu->GetMode();

    if (event->button() == Qt::LeftButton) {
        this->TouchReleased();
    } else if (event->button() == Qt::RightButton) {
        if (mode == InputCommon::MotionEmuMode::Absolute ||
            mode == InputCommon::MotionEmuMode::RateHold) {
            motion_emu->EndTilt();
            StopCenterWarp();
        } else if (mode == InputCommon::MotionEmuMode::TiltHold) {
            motion_emu->EndTilt();
            th_poll_timer->stop();
            th_warp_active = false;
            setCursor(Qt::ArrowCursor);
            motion_emu->SetTiltOffset(0.0f, 0.0f);  // snap back to neutral
        }
        // RateContinuous / TiltContinuous: right-click release does nothing
    }
    emit MouseActivity();
}

void GRenderWindow::wheelEvent(QWheelEvent* event) {
    // Forward mouse wheel events for multi-key mapping
    if (event->angleDelta().y() > 0)
        InputCommon::MouseState::Instance().WheelUp();
    else if (event->angleDelta().y() < 0)
        InputCommon::MouseState::Instance().WheelDown();
    // Let the event propagate for other handlers
    QWidget::wheelEvent(event);
}

void GRenderWindow::TouchBeginEvent(const QTouchEvent* event) {
    // TouchBegin always has exactly one touch point, so take the .first()
    const auto [x, y] = ScaleTouch(event->points().first().position());
    this->TouchPressed(x, y);
}

void GRenderWindow::TouchUpdateEvent(const QTouchEvent* event) {
    QPointF pos;
    int active_points = 0;

    // average all active touch points
    for (const auto& tp : event->points()) {
        if (tp.state() & (Qt::TouchPointPressed | Qt::TouchPointMoved | Qt::TouchPointStationary)) {
            active_points++;
            pos += tp.position();
        }
    }

    pos /= active_points;

    const auto [x, y] = ScaleTouch(pos);
    this->TouchMoved(x, y);
}

void GRenderWindow::TouchEndEvent() {
    this->TouchReleased();
}

bool GRenderWindow::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::TouchBegin:
        TouchBeginEvent(static_cast<QTouchEvent*>(event));
        return true;
    case QEvent::TouchUpdate:
        TouchUpdateEvent(static_cast<QTouchEvent*>(event));
        return true;
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        TouchEndEvent();
        return true;
    default:
        break;
    }

    return QWidget::event(event);
}

void GRenderWindow::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    if (auto* keyboard = InputCommon::GetKeyboard(); keyboard) {
        keyboard->ReleaseAllKeys();
    }
    // On focus loss: exit center-warping, restore visible cursor
    StopCenterWarp();
    has_focus = false;
}

void GRenderWindow::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    has_focus = true;
}

void GRenderWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    OnFramebufferSizeChanged();
}

bool GRenderWindow::InitRenderTarget() {
    {
        // Create a dummy render widget so that Qt
        // places the render window at the correct position.
        const RenderWidget dummy_widget{this};
    }

    first_frame = false;

    const auto graphics_api = Settings::GetWorkingGraphicsAPI();
    switch (graphics_api) {
#ifdef ENABLE_SOFTWARE_RENDERER
    case Settings::GraphicsAPI::Software:
        InitializeSoftware();
        break;
#endif
#ifdef ENABLE_OPENGL
    case Settings::GraphicsAPI::OpenGL:
        if (!InitializeOpenGL() || !LoadOpenGL()) {
            return false;
        }
        break;
#endif
#ifdef ENABLE_VULKAN
    case Settings::GraphicsAPI::Vulkan:
        InitializeVulkan();
        break;
#endif
    default:
        LOG_CRITICAL(Frontend,
                     "Unknown or unsupported graphics API {}, falling back to available default",
                     graphics_api);
#ifdef ENABLE_OPENGL
        if (!InitializeOpenGL() || !LoadOpenGL()) {
            return false;
        }
#elif ENABLE_VULKAN
        InitializeVulkan();
#elif ENABLE_SOFTWARE_RENDERER
        InitializeSoftware();
#else
// TODO: Add a null renderer backend for this, perhaps.
#error "At least one renderer must be enabled."
#endif
        break;
    }

    // Update the Window System information with the new render target
    window_info = GetWindowSystemInfo(child_widget->windowHandle());

    child_widget->resize(Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight);

    layout()->addWidget(child_widget);
    // Reset minimum required size to avoid resizing issues on the main window after restarting.
    setMinimumSize(1, 1);

    resize(Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight);
    OnMinimalClientAreaChangeRequest(GetActiveConfig().min_client_area_size);
    OnFramebufferSizeChanged();
    BackupGeometry();

    return true;
}

void GRenderWindow::ReleaseRenderTarget() {
    if (child_widget) {
        layout()->removeWidget(child_widget);
        child_widget->deleteLater();
        child_widget = nullptr;
    }
    main_context.reset();
}

void GRenderWindow::CaptureScreenshot(u32 res_scale, const QString& screenshot_path) {
    auto& renderer = system.GPU().Renderer();
    if (res_scale == 0) {
        res_scale = renderer.GetResolutionScaleFactor();
    }

    const auto layout{Layout::FrameLayoutFromResolutionScale(res_scale, is_secondary)};
    screenshot_image = QImage(QSize(layout.width, layout.height), QImage::Format_RGB32);
    renderer.RequestScreenshot(
        screenshot_image.bits(),
        [this, screenshot_path](bool invert_y) {
            const std::string std_screenshot_path = screenshot_path.toStdString();
            if (GetMirroredImage(screenshot_image, false, invert_y).save(screenshot_path)) {
                LOG_INFO(Frontend, "Screenshot saved to \"{}\"", std_screenshot_path);
            } else {
                LOG_ERROR(Frontend, "Failed to save screenshot to \"{}\"", std_screenshot_path);
            }
        },
        layout);
}

void GRenderWindow::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    setMinimumSize(minimal_size.first, minimal_size.second);
}

#ifdef ENABLE_OPENGL
bool GRenderWindow::InitializeOpenGL() {
    if (!QOpenGLContext::supportsThreadedOpenGL()) {
        QMessageBox::warning(this, tr("OpenGL not available!"),
                             tr("OpenGL shared contexts are not supported."));
        return false;
    }

    // TODO: One of these flags might be interesting: WA_OpaquePaintEvent, WA_NoBackground,
    // WA_DontShowOnScreen, WA_DeleteOnClose
    auto child = new OpenGLRenderWidget(this, system, is_secondary);
    child_widget = child;
    child_widget->windowHandle()->create();

    if (!main_context) {
        main_context = std::make_unique<OpenGLSharedContext>();
    }

    auto child_context = CreateSharedContext();
    child->SetContext(std::move(child_context));

    auto format = child_widget->windowHandle()->format();
    format.setSwapInterval(Settings::values.use_vsync.GetValue());
    child_widget->windowHandle()->setFormat(format);

    return true;
}

static void* GetProcAddressGL(const char* name) {
    return reinterpret_cast<void*>(QOpenGLContext::currentContext()->getProcAddress(name));
}

bool GRenderWindow::LoadOpenGL() {
    auto context = CreateSharedContext();
    auto scope = context->Acquire();
    const auto gles = context->IsGLES();

    auto gl_load_func = gles ? gladLoadGLES2Loader : gladLoadGLLoader;
    if (!gl_load_func(GetProcAddressGL)) {
        QMessageBox::warning(
            this, tr("Error while initializing OpenGL!"),
            tr("Your GPU may not support OpenGL, or you do not have the latest graphics driver."));
        return false;
    }

    const QString renderer =
        QString::fromUtf8(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    if (!gles && !GLAD_GL_VERSION_4_3) {
        LOG_ERROR(Frontend, "GPU does not support OpenGL 4.3: {}", renderer.toStdString());
        QMessageBox::warning(this, tr("Error while initializing OpenGL 4.3!"),
                             tr("Your GPU may not support OpenGL 4.3, or you do not have the "
                                "latest graphics driver.<br><br>GL Renderer:<br>%1")
                                 .arg(renderer));
        return false;
    } else if (gles && !GLAD_GL_ES_VERSION_3_2) {
        LOG_ERROR(Frontend, "GPU does not support OpenGL ES 3.2: {}", renderer.toStdString());
        QMessageBox::warning(this, tr("Error while initializing OpenGL ES 3.2!"),
                             tr("Your GPU may not support OpenGL ES 3.2, or you do not have the "
                                "latest graphics driver.<br><br>GL Renderer:<br>%1")
                                 .arg(renderer));
        return false;
    }

    return true;
}
#endif

#ifdef ENABLE_VULKAN
void GRenderWindow::InitializeVulkan() {
    auto child = new VulkanRenderWidget(this);
    child_widget = child;
    child_widget->windowHandle()->create();
    main_context = std::make_unique<DummyContext>();
}
#endif

#ifdef ENABLE_SOFTWARE_RENDERER
void GRenderWindow::InitializeSoftware() {
    child_widget = new SoftwareRenderWidget(this, system);
    main_context = std::make_unique<DummyContext>();
}
#endif

void GRenderWindow::OnEmulationStarting(EmuThread* emu_thread) {
    this->emu_thread = emu_thread;
}

void GRenderWindow::OnEmulationStopping() {
    emu_thread = nullptr;
}

void GRenderWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
}

std::unique_ptr<Frontend::GraphicsContext> GRenderWindow::CreateSharedContext() const {
#ifdef ENABLE_OPENGL
    const auto graphics_api = Settings::GetWorkingGraphicsAPI();
    if (graphics_api == Settings::GraphicsAPI::OpenGL) {
        auto gl_context = static_cast<OpenGLSharedContext*>(main_context.get());
        // Bind the shared contexts to the main surface in case the backend wants to take over
        // presentation
        return std::make_unique<OpenGLSharedContext>(gl_context->GetShareContext(),
                                                     child_widget->windowHandle());
    }
#endif
    return std::make_unique<DummyContext>();
}
