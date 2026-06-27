// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <memory>
#include <utility>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSlider>
#include <QTimer>
#include <SDL.h>
#include "citra_qt/configuration/config.h"
#include "citra_qt/configuration/configure_input.h"
#include "citra_qt/configuration/configure_motion_touch.h"
#include "common/param_package.h"
#include "input_common/fixed_motion.h"
#include "core/core.h"
#include "core/hle/service/hid/hid.h"
#include "ui_configure_input.h"
#include "input_common/keyboard.h"

const std::array<std::string, ConfigureInput::ANALOG_SUB_BUTTONS_NUM>
    ConfigureInput::analog_sub_buttons{{
        "up",
        "down",
        "left",
        "right",
        "up_left",
        "up_right",
        "down_left",
        "down_right",
        "modifier",
    }};

enum class AnalogSubButtons {
    up,
    down,
    left,
    right,
    up_left,
    up_right,
    down_left,
    down_right,
    modifier,
};

static QString GetKeyName(int key_code) {
    switch (key_code) {
    case Qt::Key_Shift:
        return QObject::tr("Shift");
    case Qt::Key_Control:
        return QObject::tr("Ctrl");
    case Qt::Key_Alt:
        return QObject::tr("Alt");
    case Qt::Key_Meta:
        return QString{};
    default:
        return QKeySequence(key_code).toString();
    }
}

static void SetAnalogButton(const Common::ParamPackage& input_param,
                            Common::ParamPackage& analog_param, const std::string& button_name) {
    if (analog_param.Get("engine", "") != "analog_from_button") {
        analog_param = {
            {"engine", "analog_from_button"},
        };
    }
    analog_param.Set(button_name, input_param.Serialize());
}

// ── Analog multi-key helpers ──────────────────────────────────────────────

static void EnsureAnalogFromButton(Common::ParamPackage& p) {
    // Only set the engine field; do NOT clobber existing bindings on other
    // directions (up/down/left/right and the diagonals up_left/up_right/
    // down_left/down_right) which would otherwise all disappear.
    if (p.Get("engine", "") != "analog_from_button")
        p.Set("engine", "analog_from_button");
}

/// Total number of bindings stored for a direction (0 if none).
static int AnalogButtonCount(const Common::ParamPackage& p, const std::string& dir) {
    int cnt = p.Get(dir + "_count", -1);
    if (cnt >= 0)
        return cnt;
    // Legacy: single binding stored directly under "dir"
    return p.Has(dir) ? 1 : 0;
}

/// Get the Nth binding as a ParamPackage (empty if out of range).
static Common::ParamPackage AnalogButtonN(const Common::ParamPackage& p,
                                           const std::string& dir, int n) {
    if (p.Has(dir + "_" + std::to_string(n)))
        return Common::ParamPackage{p.Get(dir + "_" + std::to_string(n), "")};
    // Legacy fallback for n==0
    if (n == 0 && p.Has(dir))
        return Common::ParamPackage{p.Get(dir, "")};
    return {};
}

/// Set (replace) the Nth binding and update count if needed.
static void SetAnalogButtonN(Common::ParamPackage& p, const std::string& dir, int n,
                             const Common::ParamPackage& input) {
    EnsureAnalogFromButton(p);
    int old_cnt = AnalogButtonCount(p, dir);
    // Migrate legacy single key to multi-key format on first multi-key use
    if (old_cnt == 1 && !p.Has(dir + "_0") && p.Has(dir)) {
        p.Set(dir + "_0", p.Get(dir, ""));
        p.Erase(dir);
    }
    p.Set(dir + "_" + std::to_string(n), input.Serialize());
    int new_cnt = std::max(n + 1, old_cnt);
    // Remove orphaned bindings beyond the new count
    for (int i = new_cnt; i < old_cnt; i++)
        p.Erase(dir + "_" + std::to_string(i));
    p.Set(dir + "_count", new_cnt);
}

/// Erase the Nth binding and compact remaining bindings.
static void EraseAnalogButtonN(Common::ParamPackage& p, const std::string& dir, int n) {
    int cnt = AnalogButtonCount(p, dir);
    if (n >= cnt)
        return;
    for (int i = n; i < cnt - 1; i++)
        p.Set(dir + "_" + std::to_string(i),
              p.Get(dir + "_" + std::to_string(i + 1), ""));
    p.Erase(dir + "_" + std::to_string(cnt - 1));
    if (cnt - 1 == 0) {
        p.Erase(dir + "_count");
        p.Erase(dir); // also erase legacy key
    } else {
        p.Set(dir + "_count", cnt - 1);
    }
}

/// Clear all bindings for a direction.
static void ClearAnalogButtons(Common::ParamPackage& p, const std::string& dir) {
    int cnt = AnalogButtonCount(p, dir);
    for (int i = 0; i < cnt; i++)
        p.Erase(dir + "_" + std::to_string(i));
    p.Erase(dir + "_count");
    p.Erase(dir);
}

/// Build a clear, cross-controller display name for an SDL_GameControllerButton enum.
/// Distinguishes controller buttons from keyboard keys — e.g. "Button_A" vs just "A".
static QString GcButtonToDisplayName(int gc_button) {
    if (gc_button < 0 || gc_button >= SDL_CONTROLLER_BUTTON_MAX)
        return QObject::tr("GC %1").arg(gc_button);

    // Common buttons — readable labels that won't be confused with keyboard keys
    static const std::unordered_map<int, const char*> names{
        {SDL_CONTROLLER_BUTTON_A, "Button_A"},
        {SDL_CONTROLLER_BUTTON_B, "Button_B"},
        {SDL_CONTROLLER_BUTTON_X, "Button_X"},
        {SDL_CONTROLLER_BUTTON_Y, "Button_Y"},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, "DPad_Up"},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, "DPad_Down"},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, "DPad_Left"},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "DPad_Right"},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L_Bumper"},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R_Bumper"},
        {SDL_CONTROLLER_BUTTON_BACK, "Select"},
        {SDL_CONTROLLER_BUTTON_START, "Start"},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3"},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3"},
        {SDL_CONTROLLER_BUTTON_GUIDE, "Guide"},
        {SDL_CONTROLLER_BUTTON_MISC1, "Misc1"},
        {SDL_CONTROLLER_BUTTON_PADDLE1, "Paddle1"},
        {SDL_CONTROLLER_BUTTON_PADDLE2, "Paddle2"},
        {SDL_CONTROLLER_BUTTON_PADDLE3, "Paddle3"},
        {SDL_CONTROLLER_BUTTON_PADDLE4, "Paddle4"},
        {SDL_CONTROLLER_BUTTON_TOUCHPAD, "Touchpad"},
    };
    if (auto it = names.find(gc_button); it != names.end())
        return QString::fromUtf8(it->second);

    // Fallback: use SDL's own name
    if (const char* sdl_name = SDL_GameControllerGetStringForButton(
            static_cast<SDL_GameControllerButton>(gc_button));
        sdl_name && sdl_name[0])
        return QString::fromUtf8(sdl_name);

    return QObject::tr("GC %1").arg(gc_button);
}

static QString ButtonToText(const Common::ParamPackage& param) {
    if (!param.Has("engine")) {
        return QObject::tr("[not set]");
    }
    const auto engine_str = param.Get("engine", "");
    // NOTE: "keyboard" engine string may occasionally have len 9 instead of 8
    // due to a toolchain-specific deserialization quirk.  Detect keyboard
    // engine via ownership of a "code" key (all keyboard bindings have one)
    // as the primary signal.
    if (engine_str == "keyboard" || param.Has("code")) {
        int code = param.Get("code", 0);
        QString name = GetKeyName(code);
        return name;
    }

    if (engine_str == "sdl") {
        if (param.Has("hat")) {
            const QString hat_str = QString::fromStdString(param.Get("hat", ""));
            const QString direction_str = QString::fromStdString(param.Get("direction", ""));

            return QObject::tr("Hat %1 %2").arg(hat_str, direction_str);
        }

        if (param.Has("axis")) {
            const QString axis_str = QString::fromStdString(param.Get("axis", ""));
            const QString direction_str = QString::fromStdString(param.Get("direction", ""));

            return QObject::tr("Axis %1%2").arg(axis_str, direction_str);
        }

        if (param.Has("button")) {
            const QString button_str = QString::fromStdString(param.Get("button", ""));

            return QObject::tr("Button %1").arg(button_str);
        }

        // Adaptive controller mapping: gc_button is an SDL_GameControllerButton enum
        // that maps to a cross-controller-compatible virtual button name (e.g. "Button_A").
        if (param.Has("gc_button")) {
            return GcButtonToDisplayName(param.Get("gc_button", 0));
        }

        // Adaptive controller mapping: gc_axis maps analog stick axes and triggers
        // to cross-controller-compatible names with direction distinction.
        if (param.Has("gc_axis")) {
            int gc_axis = param.Get("gc_axis", 0);
            const bool is_positive = (param.Get("direction", "+") == "+");
            switch (gc_axis) {
            case SDL_CONTROLLER_AXIS_LEFTX:   // 0
                return is_positive ? QStringLiteral("L_Stick_Right") : QStringLiteral("L_Stick_Left");
            case SDL_CONTROLLER_AXIS_LEFTY:   // 1
                return is_positive ? QStringLiteral("L_Stick_Down") : QStringLiteral("L_Stick_Up");
            case SDL_CONTROLLER_AXIS_RIGHTX:  // 2
                return is_positive ? QStringLiteral("R_Stick_Right") : QStringLiteral("R_Stick_Left");
            case SDL_CONTROLLER_AXIS_RIGHTY:  // 3
                return is_positive ? QStringLiteral("R_Stick_Down") : QStringLiteral("R_Stick_Up");
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  // 4
                return QStringLiteral("L_Trigger");
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: // 5
                return QStringLiteral("R_Trigger");
            }
            return QObject::tr("GC Axis %1").arg(gc_axis);
        }

        return {};
    }

    if (engine_str == "gcpad") {
        if (param.Has("axis")) {
            const QString axis_str = QString::fromStdString(param.Get("axis", ""));
            const QString direction_str = QString::fromStdString(param.Get("direction", ""));

            return QObject::tr("GC Axis %1%2").arg(axis_str, direction_str);
        }
        if (param.Has("button")) {
            const QString button_str = QString::number(int(std::log2(param.Get("button", 0))));
            return QObject::tr("GC Button %1").arg(button_str);
        }
        return GetKeyName(param.Get("code", 0));
    }

    if (engine_str == "mouse") {
        if (param.Has("button")) {
            const QString button_str = QString::fromStdString(param.Get("button", ""));
            if (button_str == QLatin1String("left"))
                return QObject::tr("Mouse: Left");
            if (button_str == QLatin1String("right"))
                return QObject::tr("Mouse: Right");
            if (button_str == QLatin1String("middle"))
                return QObject::tr("Mouse: Middle");
            return QObject::tr("Mouse %1").arg(button_str);
        }
        if (param.Has("axis") && param.Get("axis", "") == "wheel") {
            const QString value_str = QString::fromStdString(param.Get("value", ""));
            if (value_str == QLatin1String("up"))
                return QObject::tr("Mouse: Wheel Up");
            if (value_str == QLatin1String("down"))
                return QObject::tr("Mouse: Wheel Down");
            return QObject::tr("Mouse Wheel");
        }
        return QObject::tr("Mouse");
    }

    return QObject::tr("[unknown]");
}

static QString AnalogToText(const Common::ParamPackage& param, const std::string& dir) {
    if (!param.Has("engine")) {
        return QObject::tr("[not set]");
    }

    const auto engine_str = param.Get("engine", "");
    if (engine_str == "analog_from_button") {
        // Support multi-key: show first binding
        int cnt = AnalogButtonCount(param, dir);
        if (cnt > 0)
            return ButtonToText(AnalogButtonN(param, dir, 0));
        return ButtonToText(Common::ParamPackage{param.Get(dir, "")});
    }

    const QString axis_x_str{QString::fromStdString(param.Get("axis_x", ""))};
    const QString axis_y_str{QString::fromStdString(param.Get("axis_y", ""))};
    static const QString plus_str{QString::fromStdString("+")};
    static const QString minus_str{QString::fromStdString("-")};
    if (engine_str == "sdl" || engine_str == "gcpad") {
        if (dir == "modifier") {
            return QObject::tr("[unused]");
        }
        // Adaptive analog: show virtual stick name from gc_axis pair
        if (param.Has("gc_axis_x") && param.Has("gc_axis_y")) {
            int gx = param.Get("gc_axis_x", 0);
            // LEFTX=0 (CirclePad), RIGHTX=2 (CStick)
            const char* stick = (gx == 2) ? "R_Stick" : "L_Stick";
            if (dir == "left")  return QObject::tr("%1_Left").arg(stick);
            if (dir == "right") return QObject::tr("%1_Right").arg(stick);
            if (dir == "up")    return QObject::tr("%1_Up").arg(stick);
            if (dir == "down")  return QObject::tr("%1_Down").arg(stick);
            return {};
        }
        if (dir == "left") {
            return QObject::tr("Axis %1%2").arg(axis_x_str, minus_str);
        }
        if (dir == "right") {
            return QObject::tr("Axis %1%2").arg(axis_x_str, plus_str);
        }
        if (dir == "up") {
            return QObject::tr("Axis %1%2").arg(axis_y_str, plus_str);
        }
        if (dir == "down") {
            return QObject::tr("Axis %1%2").arg(axis_y_str, minus_str);
        }
        return {};
    }
    return QObject::tr("[unknown]");
}

ConfigureInput::ConfigureInput(Core::System& _system, QWidget* parent)
    : QWidget(parent), system(_system), ui(std::make_unique<Ui::ConfigureInput>()),
      timeout_timer(std::make_unique<QTimer>()), poll_timer(std::make_unique<QTimer>()) {
    ui->setupUi(this);
    setFocusPolicy(Qt::ClickFocus);

    for (const auto& profile : Settings::values.input_profiles) {
        ui->profile->addItem(QString::fromStdString(profile.name));
    }

    ui->profile->setCurrentIndex(Settings::values.current_input_profile_index);

    // Store primary .ui buttons (first binding slot per button)
    std::array<QPushButton*, Settings::NativeButton::NumButtons> _primary_buttons = {
        ui->buttonA,      ui->buttonB,        ui->buttonX,        ui->buttonY,
        ui->buttonDpadUp, ui->buttonDpadDown, ui->buttonDpadLeft, ui->buttonDpadRight,
        ui->buttonL,      ui->buttonR,        ui->buttonStart,    ui->buttonSelect,
        ui->buttonDebug,  ui->buttonGpio14,   ui->buttonZL,       ui->buttonZR,
        ui->buttonHome,   ui->buttonPower,
    };
    for (int i = 0; i < Settings::NativeButton::NumButtons; i++) {
        if (_primary_buttons[i]) {
            button_map[i].push_back(_primary_buttons[i]);
        }
    }

    // Initialize analog direction button vectors with primary .ui buttons
    {
        std::array<QPushButton*, ANALOG_SUB_BUTTONS_NUM> circle_primary = {
            ui->buttonCircleUp,    ui->buttonCircleDown,   ui->buttonCircleLeft,
            ui->buttonCircleRight, ui->buttonCircleUpLeft, ui->buttonCircleUpRight,
            ui->buttonCircleDownLeft, ui->buttonCircleDownRight, nullptr};
        std::array<QPushButton*, ANALOG_SUB_BUTTONS_NUM> cstick_primary = {
            ui->buttonCStickUp,    ui->buttonCStickDown,   ui->buttonCStickLeft,
            ui->buttonCStickRight, ui->buttonCStickUpLeft, ui->buttonCStickUpRight,
            ui->buttonCStickDownLeft, ui->buttonCStickDownRight, nullptr};
        for (int i = 0; i < ANALOG_SUB_BUTTONS_NUM; i++) {
            if (circle_primary[i]) analog_map_buttons[0][i].push_back(circle_primary[i]);
            if (cstick_primary[i]) analog_map_buttons[1][i].push_back(cstick_primary[i]);
        }
    }

    analog_map_stick = {ui->buttonCircleAnalog, ui->buttonCStickAnalog};
    analog_map_deadzone_and_modifier_slider = {ui->sliderCirclePadDeadzoneAndModifier,
                                               ui->sliderCStickDeadzoneAndModifier};
    analog_map_deadzone_and_modifier_slider_label = {ui->labelCirclePadDeadzoneAndModifier,
                                                     ui->labelCStickDeadzoneAndModifier};

    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; button_id++) {
        if (button_map[button_id].empty())
            continue;
        SetupMultiKeySlots(button_id);

        // Connect click handlers for all binding slots
        for (int slot = 0; slot < (int)button_map[button_id].size(); slot++) {
            QPushButton* btn = button_map[button_id][slot];
            btn->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(btn, &QPushButton::clicked, [this, button_id, slot, btn]() {
                HandleClick(
                    btn,
                    [this, button_id, slot](Common::ParamPackage params) {
                        // Workaround for ZL & ZR for analog triggers like on XBOX controllors.
                        if (button_id == Settings::NativeButton::ZL ||
                            button_id == Settings::NativeButton::ZR) {
                            params.Set("direction", "+");
                            params.Set("threshold", "0.5");
                        }
                        // Ensure buttons_param has enough slots
                        while ((int)buttons_param[button_id].size() <= slot)
                            buttons_param[button_id].resize(slot + 1);
                        buttons_param[button_id][slot] = std::move(params);
                        // Trim trailing empty params
                        while (!buttons_param[button_id].empty() &&
                               buttons_param[button_id].back().Serialize().empty())
                            buttons_param[button_id].pop_back();
                        UpdateMultiKeySlots(button_id);
                        ApplyConfiguration();
                        Settings::SaveProfile(ui->profile->currentIndex());
                    },
                    InputCommon::Polling::DeviceType::Button);
            });
            connect(btn, &QPushButton::customContextMenuRequested, this,
                    [this, button_id, slot](const QPoint& menu_location) {
                        QMenu context_menu;
                        // Clear this specific binding
                        if ((int)buttons_param[button_id].size() > slot &&
                            !buttons_param[button_id][slot].Serialize().empty()) {
                            context_menu.addAction(tr("Clear"), this, [=] {
                                if ((int)buttons_param[button_id].size() > slot)
                                    buttons_param[button_id].erase(
                                        buttons_param[button_id].begin() + slot);
                                UpdateMultiKeySlots(button_id);
                                ApplyConfiguration();
                                Settings::SaveProfile(ui->profile->currentIndex());
                            });
                        }
                        context_menu.addAction(tr("Clear All"), this, [=] {
                            buttons_param[button_id].clear();
                            UpdateMultiKeySlots(button_id);
                            ApplyConfiguration();
                            Settings::SaveProfile(ui->profile->currentIndex());
                        });
                        context_menu.addAction(tr("Restore Default"), this, [=] {
                            buttons_param[button_id].clear();
                            buttons_param[button_id].clear();
                            for (const auto& b : QtConfig::default_buttons[button_id])
                                buttons_param[button_id].push_back(Common::ParamPackage{b});
                            UpdateMultiKeySlots(button_id);
                            ApplyConfiguration();
                            Settings::SaveProfile(ui->profile->currentIndex());
                        });
                        context_menu.addSeparator();
                        // Mouse button bindings
                        context_menu.addAction(tr("Mouse: Left Button"), this, [=] {
                            if ((int)buttons_param[button_id].size() <= slot)
                                buttons_param[button_id].resize(slot + 1);
                            buttons_param[button_id][slot] =
                                Common::ParamPackage{"engine:mouse,button:left"};
                            UpdateMultiKeySlots(button_id);
                            ApplyConfiguration();
                            Settings::SaveProfile(ui->profile->currentIndex());
                        });
                        context_menu.addAction(tr("Mouse: Right Button"), this, [=] {
                            if ((int)buttons_param[button_id].size() <= slot)
                                buttons_param[button_id].resize(slot + 1);
                            buttons_param[button_id][slot] =
                                Common::ParamPackage{"engine:mouse,button:right"};
                            UpdateMultiKeySlots(button_id);
                            ApplyConfiguration();
                            Settings::SaveProfile(ui->profile->currentIndex());
                        });
                        context_menu.addAction(tr("Mouse: Middle Button"), this, [=] {
                            if ((int)buttons_param[button_id].size() <= slot)
                                buttons_param[button_id].resize(slot + 1);
                            buttons_param[button_id][slot] =
                                Common::ParamPackage{"engine:mouse,button:middle"};
                            UpdateMultiKeySlots(button_id);
                            ApplyConfiguration();
                            Settings::SaveProfile(ui->profile->currentIndex());
                        });
                        context_menu.addAction(tr("Mouse: Wheel Up"), this, [=] {
                            if ((int)buttons_param[button_id].size() <= slot)
                                buttons_param[button_id].resize(slot + 1);
                            buttons_param[button_id][slot] =
                                Common::ParamPackage{"engine:mouse,axis:wheel,value:up"};
                            UpdateMultiKeySlots(button_id);
                            ApplyConfiguration();
                            Settings::SaveProfile(ui->profile->currentIndex());
                        });
                        context_menu.addAction(tr("Mouse: Wheel Down"), this, [=] {
                            if ((int)buttons_param[button_id].size() <= slot)
                                buttons_param[button_id].resize(slot + 1);
                            buttons_param[button_id][slot] =
                                Common::ParamPackage{"engine:mouse,axis:wheel,value:down"};
                            UpdateMultiKeySlots(button_id);
                            ApplyConfiguration();
                            Settings::SaveProfile(ui->profile->currentIndex());
                        });
                        context_menu.addSeparator();
                        // Turbo / Toggle — always show menu for buttons that have bindings
                        {
                            bool has_binding = false;
                            for (int s = 0; s < (int)buttons_param[button_id].size(); s++)
                                if (!buttons_param[button_id][s].Serialize().empty())
                                    has_binding = true;
                            if (has_binding) {
                            bool is_turbo = (slot < (int)buttons_param[button_id].size() &&
                                buttons_param[button_id][slot].Get("turbo", "0") == "1");
                            bool is_latch = (slot < (int)buttons_param[button_id].size() &&
                                buttons_param[button_id][slot].Get("latch", "0") == "1");
                            // Toggle is per-button: check ALL slots
                            bool is_toggle = false;
                            for (int s = 0; s < (int)buttons_param[button_id].size(); s++)
                                if (buttons_param[button_id][s].Get("toggle", "0") == "1")
                                    is_toggle = true;
                            if (is_turbo) {
                                context_menu.addAction(tr("Cancel Turbo"), this, [=] {
                                    buttons_param[button_id][slot].Erase("turbo");
                                    UpdateButtonColor(button_id, slot);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                            } else if (is_latch) {
                                context_menu.addAction(tr("Cancel Toggle"), this, [=] {
                                    buttons_param[button_id][slot].Erase("latch");
                                    UpdateButtonColor(button_id, slot);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                            } else if (is_toggle) {
                                context_menu.addAction(tr("Cancel Reverse"), this, [=] {
                                    // Erase toggle from ALL slots
                                    for (int s = 0; s < (int)buttons_param[button_id].size(); s++)
                                        buttons_param[button_id][s].Erase("toggle");
                                    for (int s = 0; s < (int)button_map[button_id].size(); s++)
                                        UpdateButtonColor(button_id, s);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                            } else {
                                context_menu.addAction(tr("Set Turbo"), this, [=] {
                                    buttons_param[button_id][slot].Erase("toggle");
                                    buttons_param[button_id][slot].Erase("latch");
                                    buttons_param[button_id][slot].Set("turbo", "1");
                                    UpdateButtonColor(button_id, slot);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                                context_menu.addAction(tr("Set Toggle"), this, [=] {
                                    buttons_param[button_id][slot].Erase("turbo");
                                    buttons_param[button_id][slot].Erase("toggle");
                                    buttons_param[button_id][slot].Set("latch", "1");
                                    // Clear toggle (reverse) from other slots to avoid inversion
                                    for (int s = 0; s < (int)buttons_param[button_id].size(); s++)
                                        if (s != slot) buttons_param[button_id][s].Erase("toggle");
                                    for (int s = 0; s < (int)button_map[button_id].size(); s++)
                                        UpdateButtonColor(button_id, s);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                                context_menu.addAction(tr("Set Reverse"), this, [=] {
                                    // Set toggle on ALL slots (per-button toggle)
                                    for (int s = 0; s < (int)buttons_param[button_id].size(); s++) {
                                        buttons_param[button_id][s].Erase("turbo");
                                        buttons_param[button_id][s].Erase("latch");
                                        buttons_param[button_id][s].Set("toggle", "1");
                                    }
                                    // Update ALL slot colors
                                    for (int s = 0; s < (int)button_map[button_id].size(); s++)
                                        UpdateButtonColor(button_id, s);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                            }
                            } // if (has_binding)
                        }
                        context_menu.exec(
                            button_map[button_id][slot]->mapToGlobal(menu_location));
                    });
        }
    }

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; sub_button_id++) {
            if (analog_map_buttons[analog_id][sub_button_id].empty() ||
                !analog_map_buttons[analog_id][sub_button_id][0])
                continue;

            SetupAnalogMultiKeySlots(analog_id, sub_button_id);

            // Connect click and context menu for every binding slot (primary + extras)
            for (int slot = 0; slot < MAX_BINDINGS_PER_BUTTON; slot++) {
                QPushButton* btn = analog_map_buttons[analog_id][sub_button_id][slot];
                btn->setContextMenuPolicy(Qt::CustomContextMenu);

                connect(btn, &QPushButton::clicked, this,
                        [this, analog_id, sub_button_id, slot]() {
                            HandleClick(
                                analog_map_buttons[analog_id][sub_button_id][slot],
                                [this, analog_id, sub_button_id, slot](
                                    const Common::ParamPackage& params) {
                                    SetAnalogButtonN(analogs_param[analog_id],
                                                     analog_sub_buttons[sub_button_id], slot,
                                                     params);
                                    UpdateAnalogMultiKeySlots(analog_id, sub_button_id);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                },
                                InputCommon::Polling::DeviceType::Button);
                        });

                connect(btn, &QPushButton::customContextMenuRequested, this,
                        [this, analog_id, sub_button_id, slot](const QPoint& menu_location) {
                            QMenu context_menu;
                            const auto& dir = analog_sub_buttons[sub_button_id];
                            int cnt = AnalogButtonCount(analogs_param[analog_id], dir);
                            // Clear this specific binding
                            if (cnt > 0 && slot < cnt) {
                                auto binding = AnalogButtonN(analogs_param[analog_id], dir, slot);
                                if (!binding.Serialize().empty()) {
                                    context_menu.addAction(tr("Clear"), this, [=] {
                                EraseAnalogButtonN(analogs_param[analog_id], dir, slot);
                                UpdateAnalogMultiKeySlots(analog_id, sub_button_id);
                                ApplyConfiguration();
                                Settings::SaveProfile(ui->profile->currentIndex());
                            });
                                }
                            }
                            // Clear all bindings for this direction
                            if (cnt > 0) {
                                context_menu.addAction(tr("Clear All"), this, [=] {
                                    ClearAnalogButtons(analogs_param[analog_id], dir);
                                    UpdateAnalogMultiKeySlots(analog_id, sub_button_id);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                    // Force repaint
                                    auto* btn = analog_map_buttons[analog_id][sub_button_id][0];
                                    if (btn) btn->update();
                                });
                            }
                            context_menu.addAction(tr("Restore Default"), this, [=] {
                                ClearAnalogButtons(analogs_param[analog_id], dir);
                                Common::ParamPackage params{InputCommon::GenerateKeyboardParam(
                                    QtConfig::default_analogs[analog_id][sub_button_id])};
                                SetAnalogButtonN(analogs_param[analog_id], dir, 0, params);
                                UpdateAnalogMultiKeySlots(analog_id, sub_button_id);
                                ApplyConfiguration();
                                Settings::SaveProfile(ui->profile->currentIndex());
                            });
                            context_menu.addSeparator();
                            // Mouse bindings
                            auto addMouseAction = [&](const QString& label,
                                                       const Common::ParamPackage& pkg) {
                                context_menu.addAction(label, this, [=] {
                                    int n = AnalogButtonCount(analogs_param[analog_id], dir);
                                    SetAnalogButtonN(analogs_param[analog_id], dir,
                                                     slot < n ? slot : n, pkg);
                                    UpdateAnalogMultiKeySlots(analog_id, sub_button_id);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                            };
                            addMouseAction(tr("Mouse: Left Button"),
                                           {{"engine", "mouse"}, {"button", "left"}});
                            addMouseAction(tr("Mouse: Right Button"),
                                           {{"engine", "mouse"}, {"button", "right"}});
                            addMouseAction(tr("Mouse: Middle Button"),
                                           {{"engine", "mouse"}, {"button", "middle"}});
                            addMouseAction(
                                tr("Mouse: Wheel Up"),
                                {{"engine", "mouse"}, {"axis", "wheel"}, {"value", "up"}});
                            addMouseAction(
                                tr("Mouse: Wheel Down"),
                                {{"engine", "mouse"}, {"axis", "wheel"}, {"value", "down"}});
                            context_menu.addSeparator();
                            // Turbo / Toggle for analog directions
                            {
                                const auto& dir = analog_sub_buttons[sub_button_id];
                                int cnt = AnalogButtonCount(analogs_param[analog_id], dir);
                                if (cnt > 0 && slot < cnt) {
                                    std::string turbo_key = dir + "_turbo";
                                    std::string toggle_key = dir + "_toggle";
                                    bool is_turbo = analogs_param[analog_id].Get(turbo_key, "0") == "1";
                                    bool is_toggle = analogs_param[analog_id].Get(toggle_key, "0") == "1";
                                    if (is_turbo) {
                                        context_menu.addAction(tr("Cancel Turbo"), this, [=] {
                                            analogs_param[analog_id].Erase(turbo_key);
                                            UpdateAnalogButtonColor(analog_id, sub_button_id);
                                            ApplyConfiguration();
                                            Settings::SaveProfile(ui->profile->currentIndex());
                                        });
                                    } else if (is_toggle) {
                                        context_menu.addAction(tr("Cancel Reverse"), this, [=] {
                                            analogs_param[analog_id].Erase(toggle_key);
                                            UpdateAnalogButtonColor(analog_id, sub_button_id);
                                            ApplyConfiguration();
                                            Settings::SaveProfile(ui->profile->currentIndex());
                                        });
                                    } else {
                                        context_menu.addAction(tr("Set Turbo"), this, [=] {
                                            analogs_param[analog_id].Erase(toggle_key);
                                            analogs_param[analog_id].Set(turbo_key, "1");
                                            UpdateAnalogButtonColor(analog_id, sub_button_id);
                                            ApplyConfiguration();
                                            Settings::SaveProfile(ui->profile->currentIndex());
                                        });
                                        context_menu.addAction(tr("Set Reverse"), this, [=] {
                                            analogs_param[analog_id].Erase(turbo_key);
                                            analogs_param[analog_id].Set(toggle_key, "1");
                                            UpdateAnalogButtonColor(analog_id, sub_button_id);
                                            ApplyConfiguration();
                                            Settings::SaveProfile(ui->profile->currentIndex());
                                        });
                                    }
                                }
                            }
                            context_menu.exec(
                                analog_map_buttons[analog_id][sub_button_id][slot]->mapToGlobal(
                                    menu_location));
                        });
            }
        }
        connect(analog_map_stick[analog_id], &QPushButton::clicked, this, [this, analog_id]() {
            if (QMessageBox::information(
                    this, tr("Information"),
                    tr("After pressing OK, first move your joystick horizontally, "
                       "and then vertically."),
                    QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {
                HandleClick(
                    analog_map_stick[analog_id],
                    [this, analog_id](const Common::ParamPackage& params) {
                        analogs_param[analog_id] = params;
                        ApplyConfiguration();
                        Settings::SaveProfile(ui->profile->currentIndex());
                    },
                    InputCommon::Polling::DeviceType::Analog);
            }
        });
        connect(analog_map_deadzone_and_modifier_slider[analog_id], &QSlider::valueChanged, this,
                [this, analog_id] {
                    const int slider_value =
                        analog_map_deadzone_and_modifier_slider[analog_id]->value();
                    const auto engine = analogs_param[analog_id].Get("engine", "");
                    if (engine == "sdl" || engine == "gcpad") {
                        analog_map_deadzone_and_modifier_slider_label[analog_id]->setText(
                            tr("Deadzone: %1%").arg(slider_value));
                        analogs_param[analog_id].Set("deadzone", slider_value / 100.0f);
                    } else {
                        analog_map_deadzone_and_modifier_slider_label[analog_id]->setText(
                            tr("Modifier Scale: %1%").arg(slider_value));
                        analogs_param[analog_id].Set("modifier_scale", slider_value / 100.0f);
                    }
                    ApplyConfiguration();
                    Settings::SaveProfile(ui->profile->currentIndex());
                });
    }

    // ----- CircleMod (轻推摇杆) multi-key setup -----
    // CircleMod is an analog modifier that applies to both CirclePad and CStick.
    // It is not part of NativeButton::Values, so we maintain a separate multi-key
    // vector (circlemod_param) and sync it into analogs_param on apply.
    ui->buttonCircleMod->setContextMenuPolicy(Qt::CustomContextMenu);
    circlemod_button_map.push_back(ui->buttonCircleMod);
    SetupCircleModMultiKeySlots();

    // Helper: apply circlemod changes → analogs → save
    auto applyCircleMod = [this]() {
        UpdateCircleModMultiKeySlots();
        // Update primary button text (slot 0 — not handled by UpdateCircleModMultiKeySlots)
        if (circlemod_button_map[0]) {
            circlemod_button_map[0]->setText(
                circlemod_param.empty()
                    ? tr("[not set]")
                    : ButtonToText(circlemod_param[0]));
        }
        SyncCircleModToAnalogs();
        ApplyConfiguration();
        Settings::SaveProfile(ui->profile->currentIndex());
    };
    // Helper: build context menu for any CircleMod binding slot
    auto buildCircleModMenu = [this, applyCircleMod](int slotCapture, const QPoint& menu_location,
                                     QPushButton* sourceButton) {
        QMenu context_menu;
        if (slotCapture < (int)circlemod_param.size() &&
            !circlemod_param[slotCapture].Serialize().empty()) {
            context_menu.addAction(tr("Clear"), this, [this, slotCapture, applyCircleMod] {
                if (slotCapture < (int)circlemod_param.size())
                    circlemod_param.erase(circlemod_param.begin() + slotCapture);
                applyCircleMod();
            });
        }
        context_menu.addAction(tr("Clear All"), this, [this, applyCircleMod] {
            circlemod_param.clear();
            applyCircleMod();
        });
        context_menu.addAction(tr("Restore Default"), this, [this, applyCircleMod] {
            circlemod_param.clear();
            if (QtConfig::default_analogs[0][4] != 0)
                circlemod_param.push_back(Common::ParamPackage{
                    InputCommon::GenerateKeyboardParam(
                        QtConfig::default_analogs[0][4])});
            applyCircleMod();
        });
        context_menu.addSeparator();
        // Mouse bindings
        auto setMouseBinding = [this, slotCapture, applyCircleMod](const char* engine_str) {
            while ((int)circlemod_param.size() <= slotCapture)
                circlemod_param.resize(slotCapture + 1);
            circlemod_param[slotCapture] = Common::ParamPackage{engine_str};
            applyCircleMod();
        };
        context_menu.addAction(tr("Mouse: Left Button"), this,
                               [setMouseBinding] { setMouseBinding("engine:mouse,button:left"); });
        context_menu.addAction(tr("Mouse: Right Button"), this,
                               [setMouseBinding] { setMouseBinding("engine:mouse,button:right"); });
        context_menu.addAction(tr("Mouse: Middle Button"), this,
                               [setMouseBinding] { setMouseBinding("engine:mouse,button:middle"); });
        context_menu.addAction(tr("Mouse: Wheel Up"), this,
                               [setMouseBinding] { setMouseBinding("engine:mouse,axis:wheel,value:up"); });
        context_menu.addAction(tr("Mouse: Wheel Down"), this,
                               [setMouseBinding] { setMouseBinding("engine:mouse,axis:wheel,value:down"); });
        context_menu.exec(sourceButton->mapToGlobal(menu_location));
    };

    // Primary button (slot 0) click handler
    connect(ui->buttonCircleMod, &QPushButton::clicked, this, [this, applyCircleMod]() {
        HandleClick(
            ui->buttonCircleMod,
            [this, applyCircleMod](const Common::ParamPackage& params) {
                while (circlemod_param.empty())
                    circlemod_param.resize(1);
                circlemod_param[0] = params;
                // Trim trailing empty
                while (!circlemod_param.empty() &&
                       circlemod_param.back().Serialize().empty())
                    circlemod_param.pop_back();
                applyCircleMod();
            },
            InputCommon::Polling::DeviceType::Button);
    });
    // Primary button context menu
    connect(ui->buttonCircleMod, &QPushButton::customContextMenuRequested, this,
            [this, buildCircleModMenu](const QPoint& pos) {
                buildCircleModMenu(0, pos, ui->buttonCircleMod);
            });

    // Set gridLayout_7 row stretch programmatically (the .ui string format
    // is not understood by the Qt uic). Row 0/1 (FaceButtons, DPad, CirclePad,
    // CStick) should expand; row 2/3 (Shoulder Buttons, Misc.) should stay compact.
    if (ui->gridLayout_7) {
        ui->gridLayout_7->setRowStretch(0, 1);
        ui->gridLayout_7->setRowStretch(1, 1);
        ui->gridLayout_7->setRowStretch(2, 0);
    }

    // --- Touch screen coordinate mapping ---
    // 15 independent touch points; each has its own X/Y sliders and up to 5 key bindings.
    {
        auto* touchGroup = new QGroupBox(tr("Touch Screen Mapping"), this);
        touchGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        touchGroup->setMinimumHeight(0);
        auto* touchOuterLayout = new QVBoxLayout(touchGroup);
        touchOuterLayout->setSizeConstraint(QLayout::SetMinimumSize);

        for (int point = 0; point < MAX_TOUCH_POINTS; point++) {
            // Each point gets its own sub-group with a numbered title
            auto* pointBox = new QGroupBox(
                tr("Point %1").arg(point + 1), touchGroup);
            auto* pointLayout = new QVBoxLayout(pointBox);

            // X slider (independent per point)
            auto* xSlider = new QSlider(Qt::Horizontal, pointBox);
            xSlider->setRange(0, 100);
            xSlider->setValue(50);
            auto* xLabel = new QLabel(tr("X: 50%"), pointBox);
            auto* xRow = new QHBoxLayout();
            xRow->addWidget(new QLabel(tr("Screen X %:"), pointBox));
            xRow->addWidget(xSlider);
            xRow->addWidget(xLabel);
            pointLayout->addLayout(xRow);

            // Y slider (independent per point)
            auto* ySlider = new QSlider(Qt::Horizontal, pointBox);
            ySlider->setRange(0, 100);
            ySlider->setValue(50);
            auto* yLabel = new QLabel(tr("Y: 50%"), pointBox);
            auto* yRow = new QHBoxLayout();
            yRow->addWidget(new QLabel(tr("Screen Y %:"), pointBox));
            yRow->addWidget(ySlider);
            yRow->addWidget(yLabel);
            pointLayout->addLayout(yRow);

            // 5 key binding slots; only slot 0 visible by default, others revealed
            // as the user binds them (or one extra is shown when slot 0 is bound).
            std::vector<QPushButton*> slotBtns;
            for (int s = 0; s < MAX_KEYS_PER_POINT; s++) {
                QPushButton* keyBtn = new QPushButton(tr("[not set]"), pointBox);
                keyBtn->setMinimumHeight(24);
                keyBtn->setContextMenuPolicy(Qt::CustomContextMenu);
                // Slot 0 is the primary; hide slots 1..N until needed.
                // Final visibility is fully determined by UpdateTouchPointsMultiKeySlots.
                keyBtn->setVisible(s == 0);
                pointLayout->addWidget(keyBtn);
                slotBtns.push_back(keyBtn);

                int cap = s;
                connect(keyBtn, &QPushButton::clicked, this,
                        [this, point, cap, keyBtn, xSlider, ySlider]() {
                            HandleClick(
                                keyBtn,
                                [this, point, cap, xSlider, ySlider](Common::ParamPackage params) {
                                    int xp = xSlider->value();
                                    int yp = ySlider->value();
                                    params.Set("x", xp);
                                    params.Set("y", yp);
                                    while ((int)touch_points_param.size() <= point)
                                        touch_points_param.resize(point + 1);
                                    while ((int)touch_points_param[point].size() <= cap)
                                        touch_points_param[point].push_back(
                                            Common::ParamPackage{});
                                    touch_points_param[point][cap] = params;
                                    UpdateTouchPointsMultiKeySlots();
                                },
                                InputCommon::Polling::DeviceType::Button);
                        });

                connect(keyBtn, &QPushButton::customContextMenuRequested, this,
                        [this, point, cap, keyBtn](const QPoint& pos) {
                            QMenu context_menu;
                            bool has_binding = (point < (int)touch_points_param.size() &&
                                cap < (int)touch_points_param[point].size() &&
                                !touch_points_param[point][cap].Serialize().empty());
                            bool is_turbo = false;
                            if (has_binding) {
                                is_turbo = (touch_points_param[point][cap].Get("turbo", "0") == "1");
                            }
                            bool any_toggle = false;
                            bool is_latch = false;
                            if (point < (int)touch_points_param.size()) {
                                for (const auto& p : touch_points_param[point]) {
                                    if (!p.Serialize().empty() && p.Get("toggle", "0") == "1") {
                                        any_toggle = true; break;
                                    }
                                }
                            }
                            if (has_binding) {
                                is_latch = (touch_points_param[point][cap].Get("latch", "0") == "1");
                            }
                            if (has_binding && !is_turbo && !any_toggle && !is_latch) {
                                context_menu.addAction(tr("Clear"), this, [this, point, cap] {
                                    if (point < (int)touch_points_param.size() &&
                                        cap < (int)touch_points_param[point].size()) {
                                        touch_points_param[point].erase(
                                            touch_points_param[point].begin() + cap);
                                    }
                                    UpdateTouchPointsMultiKeySlots();
                                });
                            }
                            if (is_turbo) {
                                context_menu.addAction(tr("Cancel Turbo"), this, [this, point, cap] {
                                    if (point < (int)touch_points_param.size() &&
                                        cap < (int)touch_points_param[point].size()) {
                                        touch_points_param[point][cap].Erase("turbo");
                                    }
                                    UpdateTouchPointsMultiKeySlots();
                                });
                            }
                            if (any_toggle) {
                                context_menu.addAction(tr("Cancel Reverse"), this, [this, point] {
                                    if (point < (int)touch_points_param.size()) {
                                        for (auto& p : touch_points_param[point])
                                            p.Erase("toggle");
                                    }
                                    UpdateTouchPointsMultiKeySlots();
                                });
                            }
                            if (is_latch) {
                                context_menu.addAction(tr("Cancel Toggle"), this, [this, point, cap] {
                                    if (point < (int)touch_points_param.size() &&
                                        cap < (int)touch_points_param[point].size()) {
                                        touch_points_param[point][cap].Erase("latch");
                                    }
                                    UpdateTouchPointsMultiKeySlots();
                                });
                            }
                            if (has_binding && !any_toggle && !is_latch) {
                                context_menu.addAction(tr("Set Turbo"), this, [this, point, cap] {
                                    if (point < (int)touch_points_param.size() &&
                                        cap < (int)touch_points_param[point].size()) {
                                        touch_points_param[point][cap].Erase("toggle");
                                        touch_points_param[point][cap].Erase("latch");
                                        touch_points_param[point][cap].Set("turbo", "1");
                                    }
                                    UpdateTouchPointsMultiKeySlots();
                                });
                                context_menu.addAction(tr("Set Toggle"), this, [this, point, cap] {
                                    if (point < (int)touch_points_param.size() &&
                                        cap < (int)touch_points_param[point].size()) {
                                        touch_points_param[point][cap].Erase("turbo");
                                        touch_points_param[point][cap].Erase("toggle");
                                        touch_points_param[point][cap].Set("latch", "1");
                                    }
                                    UpdateTouchPointsMultiKeySlots();
                                });
                                context_menu.addAction(tr("Set Reverse"), this, [this, point] {
                                    if (point < (int)touch_points_param.size()) {
                                        for (auto& p : touch_points_param[point]) {
                                            p.Erase("turbo");
                                            p.Erase("latch");
                                            p.Set("toggle", "1");
                                        }
                                    }
                                    UpdateTouchPointsMultiKeySlots();
                                });
                            }
                            context_menu.addAction(tr("Clear All"), this, [this, point] {
                                if (point < (int)touch_points_param.size())
                                    touch_points_param[point].clear();
                                UpdateTouchPointsMultiKeySlots();
                            });
                            context_menu.exec(keyBtn->mapToGlobal(pos));
                        });
            }

            // Slider value-change handlers: update labels and the per-point coords
            connect(xSlider, &QSlider::valueChanged, this,
                    [this, point, xLabel](int v) {
                        xLabel->setText(QStringLiteral("X: %1%").arg(v));
                        if (point < (int)touch_points_param.size())
                            for (auto& p : touch_points_param[point]) p.Set("x", v);
                    });
            connect(ySlider, &QSlider::valueChanged, this,
                    [this, point, yLabel](int v) {
                        yLabel->setText(QStringLiteral("Y: %1%").arg(v));
                        if (point < (int)touch_points_param.size())
                            for (auto& p : touch_points_param[point]) p.Set("y", v);
                    });

        touch_point_widgets.push_back({xSlider, ySlider, slotBtns, pointBox});
        touchOuterLayout->addWidget(pointBox);
    }

    // Don't call UpdateTouchPointsMultiKeySlots here — touch points are pre-created
    // (not lazily created like button slots). Visibility must be deferred until
    // the widget tree is fully shown, otherwise setVisible(true) on hidden children
    // has no effect.
    // UpdateTouchPointsMultiKeySlots() is called via showEvent() instead.

    // Add to gridLayout_7, row 3 (below Misc/Shoulders), spanning both columns
        ui->gridLayout_7->addWidget(touchGroup, 3, 0, 1, 2);
        ui->gridLayout_7->setRowStretch(3, 0);
        ui->gridLayout_7->setRowMinimumHeight(3, 0);
    }
    // Fixed Motion Override section
    {
        auto* fmGroup = new QGroupBox(tr("Fixed Motion Override"), this);
        auto* fmLayout = new QVBoxLayout(fmGroup);

        fixed_motion_enabled = new QCheckBox(tr("Enable Fixed Motion Override"), fmGroup);
        fixed_motion_enabled->setChecked(true);
        fixed_motion_enabled->setVisible(false);
        fmLayout->addWidget(fixed_motion_enabled);

        fixed_motion_widgets.resize(MAX_FIXED_MOTION_PRESETS);
        for (int p = 0; p < MAX_FIXED_MOTION_PRESETS; p++) {
            auto& w = fixed_motion_widgets[p];
            w.container = new QWidget(fmGroup);
            w.container->setVisible(p == 0);
            auto* pLayout = new QVBoxLayout(w.container);
            pLayout->setContentsMargins(4, 2, 4, 2);

            auto* titleLabel = new QLabel(tr("Preset %1").arg(p + 1), w.container);
            QFont boldFont = titleLabel->font();
            boldFont.setBold(true);
            titleLabel->setFont(boldFont);
            pLayout->addWidget(titleLabel);

            auto makeSlider = [&](const QString& label, float minV, float maxV, float init, int steps) {
                auto* row = new QHBoxLayout();
                auto* nameLabel = new QLabel(label, w.container);
                nameLabel->setMinimumWidth(70);
                nameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                row->addWidget(nameLabel);
                auto* slider = new QSlider(Qt::Horizontal, w.container);
                slider->setRange(0, steps);
                slider->setValue(static_cast<int>((init - minV) / (maxV - minV) * steps));
                auto* valBox = new QDoubleSpinBox(w.container);
                valBox->setMinimumWidth(80);
                valBox->setDecimals(2);
                valBox->setMinimum(-99999.0);
                valBox->setMaximum(99999.0);
                valBox->setValue(static_cast<double>(init));
                valBox->setKeyboardTracking(false);
                row->addWidget(slider);
                row->addWidget(valBox);
                pLayout->addLayout(row);
                QObject::connect(slider, &QSlider::valueChanged, w.container,
                    [valBox, minV, maxV, steps](int v) {
                        if (!valBox->hasFocus()) {
                            double real = minV + (maxV - minV) * v / steps;
                            valBox->setValue(real);
                        }
                    });
                QObject::connect(valBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    w.container, [slider, minV, maxV, steps](double v) {
                        int pos = static_cast<int>((v - minV) / (maxV - minV) * steps);
                        slider->setValue(std::clamp(pos, 0, steps));
                    });
                slider->setFocusPolicy(Qt::StrongFocus);
                slider->installEventFilter(this);
                return std::make_pair(slider, valBox);
            };

            auto gx = makeSlider(tr("Tilt Dir X:"), -1.0f, 1.0f, 0.0f, 200);
            w.grav_x = gx.first; w.grav_x_label = gx.second;
            auto gy = makeSlider(tr("Tilt Dir Y:"), -1.0f, 1.0f, -1.0f, 200);
            w.grav_y = gy.first; w.grav_y_label = gy.second;
            auto gz = makeSlider(tr("Tilt Dir Z:"), -1.0f, 1.0f, 0.0f, 200);
            w.grav_z = gz.first; w.grav_z_label = gz.second;

            auto rx = makeSlider(tr("Rate X:"), -5000.0f, 5000.0f, 0.0f, 500);
            w.rate_x = rx.first; w.rate_x_label = rx.second;
            auto ry = makeSlider(tr("Rate Y:"), -5000.0f, 5000.0f, 0.0f, 500);
            w.rate_y = ry.first; w.rate_y_label = ry.second;
            auto rz = makeSlider(tr("Rate Z:"), -5000.0f, 5000.0f, 0.0f, 500);
            w.rate_z = rz.first; w.rate_z_label = rz.second;

            // Button bindings
            auto* btnRow = new QHBoxLayout();
            btnRow->addWidget(new QLabel(tr("Keys:"), w.container));
            w.slot_params.resize(MAX_FIXED_MOTION_KEYS);
            for (int s = 0; s < MAX_FIXED_MOTION_KEYS; s++) {
                auto* btn = new QPushButton(tr("[not set]"), w.container);
                btn->setContextMenuPolicy(Qt::CustomContextMenu);
                btn->setMinimumWidth(90);
                btn->setVisible(s == 0);
                w.slot_btns.push_back(btn);
                btnRow->addWidget(btn);

                int cap = s;
                connect(btn, &QPushButton::clicked, this, [this, p, cap, btn]() {
                    HandleClick(btn,
                        [this, p, cap](Common::ParamPackage params) {
                            while ((int)fixed_motion_widgets[p].slot_params.size() <= cap)
                                fixed_motion_widgets[p].slot_params.resize(cap + 1);
                            fixed_motion_widgets[p].slot_params[cap] =
                                QString::fromStdString(params.Serialize());
                            UpdateFixedMotionSlots();
                        },
                        InputCommon::Polling::DeviceType::Button);
                    // Add analog pollers for triggers/sticks (after Button pollers are set up)
                    {
                        auto analog = InputCommon::Polling::GetPollers(InputCommon::Polling::DeviceType::Analog);
                        for (auto& p : analog) {
                            p->Start();
                            device_pollers.push_back(std::move(p));
                        }
                    }
                });

                connect(btn, &QPushButton::customContextMenuRequested, this,
                    [this, p, cap, btn](const QPoint& pos) {
                        QMenu context_menu;
                        bool has_binding = (cap < (int)fixed_motion_widgets[p].slot_params.size() &&
                            !fixed_motion_widgets[p].slot_params[cap].isEmpty());
                        bool this_toggle = false;
                        bool this_reverse = false;
                        bool is_turbo = false;
                        if (has_binding) {
                            Common::ParamPackage pp2(fixed_motion_widgets[p].slot_params[cap].toStdString());
                            is_turbo = (pp2.Get("turbo", "0") == "1");
                            this_toggle = (pp2.Get("toggle", "0") == "1");
                        }
                        // Check if any slot in this preset has reverse
                        for (size_t sn = 0; sn < fixed_motion_widgets[p].slot_params.size(); sn++) {
                            if (!fixed_motion_widgets[p].slot_params[sn].isEmpty()) {
                                Common::ParamPackage pp(fixed_motion_widgets[p].slot_params[sn].toStdString());
                                if (pp.Get("reverse", "0") == "1") { this_reverse = true; break; }
                            }
                        }
                        // Clear: always show when bound (even if turbo/toggle/reverse)
                        if (has_binding && !this_toggle && !this_reverse) {
                            context_menu.addAction(tr("Clear"), this, [this, p, cap] {
                                if (cap < (int)fixed_motion_widgets[p].slot_params.size()) {
                                    fixed_motion_widgets[p].slot_params.erase(
                                        fixed_motion_widgets[p].slot_params.begin() + cap);
                                    fixed_motion_widgets[p].slot_params.resize(MAX_FIXED_MOTION_KEYS);
                                }
                                UpdateFixedMotionSlots();
                                UpdateFixedMotionButtonColors();
                            });
                        }
                        if (is_turbo) {
                            context_menu.addAction(tr("Cancel Turbo"), this, [this, p, cap] {
                                if (cap < (int)fixed_motion_widgets[p].slot_params.size()) {
                                    Common::ParamPackage pp(fixed_motion_widgets[p].slot_params[cap].toStdString());
                                    pp.Erase("turbo");
                                    fixed_motion_widgets[p].slot_params[cap] = QString::fromStdString(pp.Serialize());
                                    UpdateFixedMotionButtonColors();
                                }
                            });
                        }
                        if (this_toggle) {
                            context_menu.addAction(tr("Cancel Toggle"), this, [this, p, cap] {
                                if (cap < (int)fixed_motion_widgets[p].slot_params.size()) {
                                    Common::ParamPackage pp(fixed_motion_widgets[p].slot_params[cap].toStdString());
                                    pp.Erase("toggle");
                                    fixed_motion_widgets[p].slot_params[cap] = QString::fromStdString(pp.Serialize());
                                    UpdateFixedMotionButtonColors();
                                }
                            });
                        }
                        if (this_reverse) {
                            context_menu.addAction(tr("Cancel Reverse"), this, [this, p] {
                                for (size_t sn = 0; sn < fixed_motion_widgets[p].slot_params.size(); sn++) {
                                    Common::ParamPackage pp(fixed_motion_widgets[p].slot_params[sn].toStdString());
                                    pp.Erase("reverse");
                                    fixed_motion_widgets[p].slot_params[sn] = QString::fromStdString(pp.Serialize());
                                }
                                UpdateFixedMotionButtonColors();
                            });
                        }
                        if (has_binding && !this_toggle && !this_reverse) {
                            context_menu.addAction(tr("Set Turbo"), this, [this, p, cap] {
                                if (cap < (int)fixed_motion_widgets[p].slot_params.size() &&
                                    !fixed_motion_widgets[p].slot_params[cap].isEmpty()) {
                                    Common::ParamPackage pp(fixed_motion_widgets[p].slot_params[cap].toStdString());
                                    pp.Erase("toggle");
                                    pp.Set("turbo", "1");
                                    fixed_motion_widgets[p].slot_params[cap] = QString::fromStdString(pp.Serialize());
                                    UpdateFixedMotionButtonColors();
                                }
                            });
                            context_menu.addAction(tr("Set Toggle"), this, [this, p, cap] {
                                if (cap < (int)fixed_motion_widgets[p].slot_params.size() &&
                                    !fixed_motion_widgets[p].slot_params[cap].isEmpty()) {
                                    Common::ParamPackage pp(fixed_motion_widgets[p].slot_params[cap].toStdString());
                                    pp.Erase("turbo");
                                    pp.Set("toggle", "1");
                                    fixed_motion_widgets[p].slot_params[cap] = QString::fromStdString(pp.Serialize());
                                    UpdateFixedMotionButtonColors();
                                }
                            });
                            context_menu.addAction(tr("Set Reverse"), this, [this, p, cap] {
                                if (cap < (int)fixed_motion_widgets[p].slot_params.size() &&
                                    !fixed_motion_widgets[p].slot_params[cap].isEmpty()) {
                                    Common::ParamPackage pp(fixed_motion_widgets[p].slot_params[cap].toStdString());
                                    pp.Erase("turbo");
                                    pp.Erase("toggle");
                                    pp.Set("reverse", "1");
                                    fixed_motion_widgets[p].slot_params[cap] = QString::fromStdString(pp.Serialize());
                                    UpdateFixedMotionButtonColors();
                                }
                            });
                        }
                        context_menu.addAction(tr("Clear All"), this, [this, p] {
                            fixed_motion_widgets[p].slot_params.clear();
                            UpdateFixedMotionSlots();
                            UpdateFixedMotionButtonColors();
                        });
                        context_menu.exec(btn->mapToGlobal(pos));
                    });
            }
            pLayout->addLayout(btnRow);
            fmLayout->addWidget(w.container);
        }

        ui->gridLayout_7->addWidget(fmGroup, 4, 0, 1, 2);
        ui->gridLayout_7->setRowStretch(4, 0);
    UpdateFixedMotionSlots();
    }


    connect(ui->buttonMotionTouch, &QPushButton::clicked, this, [this] {
        ui->buttonMotionTouch->setEnabled(false);
        QDialog* motion_touch_dialog = new ConfigureMotionTouch(this);
        motion_touch_dialog->exec();
        ui->buttonMotionTouch->setEnabled(true);
    });

    ui->buttonDelete->setEnabled(ui->profile->count() > 1);

    connect(ui->buttonAutoMap, &QPushButton::clicked, this, &ConfigureInput::AutoMap);
    connect(ui->buttonClearAll, &QPushButton::clicked, this, &ConfigureInput::ClearAll);
    connect(ui->buttonRestoreDefaults, &QPushButton::clicked, this,
            &ConfigureInput::RestoreDefaults);
    connect(ui->buttonNew, &QPushButton::clicked, this, &ConfigureInput::NewProfile);
    connect(ui->buttonDelete, &QPushButton::clicked, this, &ConfigureInput::DeleteProfile);
    connect(ui->buttonRename, &QPushButton::clicked, this, &ConfigureInput::RenameProfile);

    connect(ui->profile, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        ApplyConfiguration();
        Settings::SaveProfile(Settings::values.current_input_profile_index);
        Settings::LoadProfile(i);
        LoadConfiguration();
    });

    timeout_timer->setSingleShot(true);
    connect(timeout_timer.get(), &QTimer::timeout, this, [this]() { SetPollingResult({}, true); });

    connect(poll_timer.get(), &QTimer::timeout, this, [this]() {
        Common::ParamPackage params;
        for (auto& poller : device_pollers) {
            params = poller->GetNextInput();
            if (params.Has("engine")) {
                SetPollingResult(params, false);
                return;
            }
        }
    });

    LoadConfiguration();
}

ConfigureInput::~ConfigureInput() = default;

bool ConfigureInput::event(QEvent* ev) {
    // Intercept Tab/Backtab before QWidget::event() handles focus navigation.
    // keyPressEvent must be deferred via singleShot — calling it synchronously
    // from within event() dispatch would release the keyboard grab mid-dispatch.
    if (ev->type() == QEvent::KeyPress && input_setter) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            QTimer::singleShot(0, this, [this, key = ke->key()]() {
                QKeyEvent e(QEvent::KeyPress, key, Qt::NoModifier);
                keyPressEvent(&e);
            });
            return true;
        }
    }
    return QWidget::event(ev);
}

void ConfigureInput::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Touch point extra buttons and FM preset slots need the widget tree to be
    // visible before setVisible(true/false) takes effect on pre-created hidden children.
    UpdateTouchPointsMultiKeySlots();
    UpdateFixedMotionSlots();
}

void ConfigureInput::ApplyConfiguration() {

    Settings::values.use_artic_base_controller = ui->use_artic_controller->isChecked();
    Settings::values.use_adaptive_controller_mapping = ui->use_adaptive_controller_mapping->isChecked();

    for (int i = 0; i < Settings::NativeButton::NumButtons; i++) {
        Settings::values.current_input_profile.buttons[i].clear();
        for (const auto& param : buttons_param[i]) {
            std::string serialized = param.Serialize();
            if (!serialized.empty() && serialized != "[empty]")
                Settings::values.current_input_profile.buttons[i].push_back(std::move(serialized));
        }
    }
    SyncCircleModToAnalogs();
    std::transform(analogs_param.begin(), analogs_param.end(),
                   Settings::values.current_input_profile.analogs.begin(),
                   [](const Common::ParamPackage& param) { return param.Serialize(); });

    // Touch screen coordinate bindings (nested: points -> keys)

    // Fixed Motion Override save
    {
        InputCommon::FixedMotionConfig fm_cfg;
        fm_cfg.enabled = fixed_motion_enabled ? fixed_motion_enabled->isChecked() : false;
        for (size_t i = 0; i < fixed_motion_widgets.size(); i++) {
            InputCommon::FixedMotionPreset fp;
            fp.gravity = Common::MakeVec(
                static_cast<float>(fixed_motion_widgets[i].grav_x_label->value()),
                static_cast<float>(fixed_motion_widgets[i].grav_y_label->value()),
                static_cast<float>(fixed_motion_widgets[i].grav_z_label->value()));
            fp.angular_rate = Common::MakeVec(
                static_cast<float>(fixed_motion_widgets[i].rate_x_label->value()),
                static_cast<float>(fixed_motion_widgets[i].rate_y_label->value()),
                static_cast<float>(fixed_motion_widgets[i].rate_z_label->value()));
            for (const auto& s : fixed_motion_widgets[i].slot_params) {
                if (!s.isEmpty()) fp.buttons.push_back(s.toStdString());
            }
            fm_cfg.presets.push_back(std::move(fp));
        }
        Settings::values.current_input_profile.fixed_motion_config = fm_cfg.Serialize();
    }

    Settings::values.current_input_profile.touch_points.clear();
    for (const auto& point_keys : touch_points_param) {
        std::vector<std::string> serialized_point;
        serialized_point.reserve(point_keys.size());
        for (const auto& p : point_keys) {
            std::string s = p.Serialize();
            if (!s.empty())
                serialized_point.push_back(std::move(s));
        }
        // Always write the entry, even if empty, so slot positions are preserved
        Settings::values.current_input_profile.touch_points.push_back(
            std::move(serialized_point));
    }

    // Sync memory profile back to the input_profiles array so config->Save() persists it
    Settings::SaveProfile(Settings::values.current_input_profile_index);

    // Trigger reload so turbo/toggle wrappers take effect immediately
    if (system.IsPoweredOn()) {
        auto hid = Service::HID::GetModule(system);
        if (hid)
            hid->ReloadInputDevices();
    }
}

void ConfigureInput::ApplyProfile() {
    Settings::values.current_input_profile_index = ui->profile->currentIndex();
}

void ConfigureInput::EmitInputKeysChanged() {
    emit InputKeysChanged(GetUsedKeyboardKeys());
}

void ConfigureInput::OnHotkeysChanged(QList<QKeySequence> new_key_list) {
    hotkey_list = new_key_list;
}

QList<QKeySequence> ConfigureInput::GetUsedKeyboardKeys() {
    QList<QKeySequence> list;
    for (int button = 0; button < Settings::NativeButton::NumButtons; button++) {
        for (const auto& button_param : buttons_param[button]) {
            if (!button_param.Serialize().empty() && button_param.Get("engine", "") == "keyboard") {
                list << QKeySequence(button_param.Get("code", 0));
            }
        }
    }

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; ++analog_id) {
        const auto& analog_param = analogs_param[analog_id];
        if (analog_param.Get("engine", "") == "analog_from_button") {
            for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; ++sub_button_id) {
                const Common::ParamPackage sub_button{
                    analog_param.Get(analog_sub_buttons[sub_button_id], "")};
                list << QKeySequence(sub_button.Get("code", 0));
            }
        }
    }

    return list;
}

void ConfigureInput::LoadConfiguration() {
    ui->use_artic_controller->setChecked(Settings::values.use_artic_base_controller.GetValue());
    ui->use_artic_controller->setEnabled(!system.IsPoweredOn());

    ui->use_adaptive_controller_mapping->setChecked(
        Settings::values.use_adaptive_controller_mapping.GetValue());

    for (int i = 0; i < Settings::NativeButton::NumButtons; i++) {
        buttons_param[i].clear();
        for (const auto& str : Settings::values.current_input_profile.buttons[i]) {
            buttons_param[i].push_back(Common::ParamPackage(str));
        }
    }
    std::transform(Settings::values.current_input_profile.analogs.begin(),
                   Settings::values.current_input_profile.analogs.end(), analogs_param.begin(),
                   [](const std::string& str) { return Common::ParamPackage(str); });

    // Load Fixed Motion Override
    {
        // Clear all previous binding data
        for (auto& w : fixed_motion_widgets) {
            w.slot_params.clear();
            w.slot_params.resize(MAX_FIXED_MOTION_KEYS);
            w.container->setVisible(false);
        }
        if (!Settings::values.current_input_profile.fixed_motion_config.empty()) {
            InputCommon::FixedMotionConfig fm_cfg;
            fm_cfg.LoadFromParams(Settings::values.current_input_profile.fixed_motion_config);
            if (fixed_motion_enabled)
                fixed_motion_enabled->setChecked(fm_cfg.enabled);
            for (size_t i = 0; i < fm_cfg.presets.size() && i < fixed_motion_widgets.size(); i++) {
                auto& w = fixed_motion_widgets[i];
                w.container->setVisible(true);
                w.grav_x_label->setValue(fm_cfg.presets[i].gravity.x);
                w.grav_y_label->setValue(fm_cfg.presets[i].gravity.y);
                w.grav_z_label->setValue(fm_cfg.presets[i].gravity.z);
                w.rate_x_label->setValue(fm_cfg.presets[i].angular_rate.x);
                w.rate_y_label->setValue(fm_cfg.presets[i].angular_rate.y);
                w.rate_z_label->setValue(fm_cfg.presets[i].angular_rate.z);
                for (size_t j = 0; j < fm_cfg.presets[i].buttons.size() && j < w.slot_params.size(); j++) {
                    w.slot_params[j] = QString::fromStdString(fm_cfg.presets[i].buttons[j]);
                }
            }
        }
    }
    UpdateFixedMotionSlots();

    LoadCircleModFromAnalogs();
    // Load touch screen coordinate bindings (nested: points -> keys)
    touch_points_param.clear();
    for (const auto& point_keys :
         Settings::values.current_input_profile.touch_points) {
        std::vector<Common::ParamPackage> per_point;
        per_point.reserve(point_keys.size());
        for (const auto& s : point_keys) {
            per_point.push_back(Common::ParamPackage(s));
        }
        touch_points_param.push_back(std::move(per_point));
    }
    UpdateButtonLabels();
    for (int i = 0; i < Settings::NativeButton::NumButtons; i++)
        UpdateMultiKeySlots(i);
    // Sync slider positions from loaded params
    for (int point = 0; point < (int)touch_point_widgets.size() &&
                        point < (int)touch_points_param.size();
         point++) {
        const auto& w = touch_point_widgets[point];
        if (touch_points_param[point].empty()) continue;
        // Use first binding's x/y; if absent, default 50
        int xv = touch_points_param[point][0].Get("x", 50);
        int yv = touch_points_param[point][0].Get("y", 50);
        w.x_slider->setValue(xv);
        w.y_slider->setValue(yv);
    }
}

void ConfigureInput::RestoreDefaults() {
    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; button_id++) {
        buttons_param[button_id].clear();
        for (const auto& b : QtConfig::default_buttons[button_id])
            buttons_param[button_id].push_back(Common::ParamPackage{b});
        // Explicitly add SDL virtual controller defaults (same workaround
        // as in config.cpp ReadControlValues — toolchain brace-init bug).
        switch (button_id) {
        case Settings::NativeButton::Up:
            buttons_param[button_id].push_back(
                Common::ParamPackage{"engine:sdl,gc_button:11,port:0"});
            break;
        case Settings::NativeButton::Down:
            buttons_param[button_id].push_back(
                Common::ParamPackage{"engine:sdl,gc_button:12,port:0"});
            break;
        case Settings::NativeButton::Left:
            buttons_param[button_id].push_back(
                Common::ParamPackage{"engine:sdl,gc_button:13,port:0"});
            break;
        case Settings::NativeButton::Right:
            buttons_param[button_id].push_back(
                Common::ParamPackage{"engine:sdl,gc_button:14,port:0"});
            break;
        case Settings::NativeButton::L:
            buttons_param[button_id].push_back(
                Common::ParamPackage{"engine:sdl,gc_button:9,port:0"});
            break;
        case Settings::NativeButton::R:
            buttons_param[button_id].push_back(
                Common::ParamPackage{"engine:sdl,gc_button:10,port:0"});
            break;
        case Settings::NativeButton::ZL:
            buttons_param[button_id].push_back(
                Common::ParamPackage{"direction:+,engine:sdl,gc_axis:4,port:0,threshold:0.5"});
            break;
        case Settings::NativeButton::ZR:
            buttons_param[button_id].push_back(
                Common::ParamPackage{"direction:+,engine:sdl,gc_axis:5,port:0,threshold:0.5"});
            break;
        }
    }

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        analogs_param[analog_id] = Common::ParamPackage{InputCommon::GenerateAnalogParamFromKeys(
            QtConfig::default_analogs[analog_id][0], QtConfig::default_analogs[analog_id][1],
            QtConfig::default_analogs[analog_id][2], QtConfig::default_analogs[analog_id][3],
            QtConfig::default_analogs[analog_id][4], 0.5f)};
        // Move old-format keys (e.g. "up") to _0 multi-key format so CreateMultiDevices
        // picks them up when _count > 0. Skip "modifier" which may be empty.
        for (const auto* dir : {"up", "down", "left", "right"}) {
            const std::string val = analogs_param[analog_id].Get(dir, "");
            if (!val.empty()) {
                analogs_param[analog_id].Set(std::string(dir) + "_0", val);
                analogs_param[analog_id].Erase(dir);
                analogs_param[analog_id].Set(std::string(dir) + "_count", 1);
            }
        }
        // Add SDL virtual controller stick defaults as secondary multi-key bindings.
        const int axis_x = (analog_id == 0) ? 0 : 2;
        const int axis_y = (analog_id == 0) ? 1 : 3;
        auto add_sdl = [&](const std::string& dir, const std::string& sign, int axis) {
            std::string sdl = "direction:" + sign + ",engine:sdl,gc_axis:" +
                              std::to_string(axis) + ",port:0,threshold:0.5";
            analogs_param[analog_id].Set(dir + "_1", sdl);
            analogs_param[analog_id].Set(dir + "_count", 2);
        };
        add_sdl("up", "-", axis_y);
        add_sdl("down", "+", axis_y);
        add_sdl("left", "-", axis_x);
        add_sdl("right", "+", axis_x);
    }
    circlemod_param.clear();
    if (QtConfig::default_analogs[0][4] != 0)
        circlemod_param.push_back(Common::ParamPackage{
            InputCommon::GenerateKeyboardParam(
                QtConfig::default_analogs[0][4])});
    touch_points_param.clear();
    // Reset touch point XY sliders to 50% default
    for (auto& w : touch_point_widgets) {
        w.x_slider->setValue(50);
        w.y_slider->setValue(50);
    }
    // Restore "Adaptive Button Mapping" to its default (enabled)
    ui->use_adaptive_controller_mapping->setChecked(true);
    UpdateButtonLabels();
    for (int i = 0; i < Settings::NativeButton::NumButtons; i++)
        UpdateMultiKeySlots(i);
    UpdateCircleModMultiKeySlots();
    UpdateTouchPointsMultiKeySlots();

    // Reset motion/touch device defaults
    Settings::values.current_input_profile.motion_device =
        "engine:motion_emu,update_period:20,sensitivity:0.075,"
        "tilt_clamp:90.0,tilt_max_angle:90.0,"
        "mode:rate_continuous,default_tilt:90,"
        "invert_pitch:true,invert_yaw:false,per_frame:true,"
        "clamp_pitch_180:true,auto_tilt_y:true,"
        "auto_tilt_y_invert:false,auto_tilt_x:false,"
        "auto_tilt_speed:1.0";
    Settings::values.current_input_profile.touch_device = "engine:emu_window";
    // Reset Fixed Motion Override
    Settings::values.current_input_profile.fixed_motion_config.clear();
    for (auto& w : fixed_motion_widgets) {
        w.slot_params.clear();
        w.grav_x_label->setValue(0.0);
        w.grav_y_label->setValue(-1.0);
        w.grav_z_label->setValue(0.0);
        w.rate_x_label->setValue(1.0);
        w.rate_y_label->setValue(1.0);
        w.rate_z_label->setValue(1.0);
        w.container->setVisible(false);
    }
    if (!fixed_motion_widgets.empty())
        fixed_motion_widgets[0].container->setVisible(true);
    UpdateFixedMotionSlots();
    ApplyConfiguration();
    Settings::SaveProfile(Settings::values.current_input_profile_index);
}

void ConfigureInput::ClearAll() {
    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; button_id++) {
        if (!button_map[button_id].empty() && button_map[button_id][0]->isEnabled())
            buttons_param[button_id].clear();
    }
    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        analogs_param[analog_id].Clear();
    }
    circlemod_param.clear();
    touch_points_param.clear();
    UpdateButtonLabels();
    for (int i = 0; i < Settings::NativeButton::NumButtons; i++)
        UpdateMultiKeySlots(i);
    UpdateCircleModMultiKeySlots();
    UpdateTouchPointsMultiKeySlots();

    ApplyConfiguration();
    Settings::SaveProfile(Settings::values.current_input_profile_index);
}

void ConfigureInput::SetupMultiKeySlots(int button_id) {
    auto* primary = button_map[button_id][0];
    if (!primary) return;

    QWidget* pw = primary->parentWidget();
    if (!pw || !pw->layout()) return;

    // Find the leaf layout containing the primary button — only store for
    // lazy creation in UpdateMultiKeySlots. NO widgets are created here.
    QLayout* leafLayout = nullptr;
    std::function<QLayout*(QLayout*)> findLeaf =
        [&](QLayout* l) -> QLayout* {
        if (l->indexOf(primary) >= 0) return l;
        for (int i = 0; i < l->count(); i++) {
            if (auto* child = l->itemAt(i)->layout())
                if (auto* found = findLeaf(child)) return found;
        }
        return nullptr;
    };
    leafLayout = findLeaf(pw->layout());
    if (!leafLayout) return;

    button_container_layouts[button_id] = leafLayout;
    button_container_positions[button_id] = leafLayout->indexOf(primary);
}

void ConfigureInput::UpdateMultiKeySlots(int button_id) {
    // Trim empty params from the back
    while (!buttons_param[button_id].empty() &&
           buttons_param[button_id].back().Serialize().empty())
        buttons_param[button_id].pop_back();

    int activeCount = (int)buttons_param[button_id].size();
    // Only show extra slots when the primary button actually has a binding.
    // Buttons with empty defaults (Home, Power, Debug, Gpio14) should not
    // reveal [extra not set] slots until the user explicitly sets slot 0.
    // NOTE: ParamPackage::Serialize() returns "[empty]" for empty data, not "".
    bool hasPrimary = activeCount > 0 &&
        !buttons_param[button_id][0].Serialize().empty() &&
        buttons_param[button_id][0].Serialize() != "[empty]";
    bool needExtras = hasPrimary;

    // Lazy creation: build the extra-slot container ONLY when bindings exist.
    // When bindings go to zero, destroy everything to guarantee zero layout footprint.
    QWidget*& container = button_containers[button_id];
    if (needExtras && !container) {
        // --- Create on demand ---
        auto* primary = button_map[button_id][0];
        QWidget* pw = primary->parentWidget();
        QLayout* leafLayout = button_container_layouts[button_id];
        int btnIdx = button_container_positions[button_id];

        container = new QWidget(pw);
        QVBoxLayout* vbox = new QVBoxLayout(container);
        vbox->setContentsMargins(0, 0, 0, 0);
        vbox->setSpacing(1);

        for (int slot = 1; slot < MAX_BINDINGS_PER_BUTTON; slot++) {
            QPushButton* extra = new QPushButton(tr("[extra not set]"), container);
            extra->setMinimumHeight(24);
            extra->hide();
            vbox->addWidget(extra);
            button_map[button_id].push_back(extra);

            extra->setContextMenuPolicy(Qt::CustomContextMenu);
            int slotCapture = slot;
            connect(extra, &QPushButton::clicked, [this, button_id, slotCapture, extra]() {
                HandleClick(
                    extra,
                    [this, button_id, slotCapture](Common::ParamPackage params) {
                        if (button_id == Settings::NativeButton::ZL ||
                            button_id == Settings::NativeButton::ZR) {
                            params.Set("direction", "+");
                            params.Set("threshold", "0.5");
                        }
                        while ((int)buttons_param[button_id].size() <= slotCapture)
                            buttons_param[button_id].resize(slotCapture + 1);
                        buttons_param[button_id][slotCapture] = std::move(params);
                        while (!buttons_param[button_id].empty() &&
                               buttons_param[button_id].back().Serialize().empty())
                            buttons_param[button_id].pop_back();
                        UpdateMultiKeySlots(button_id);
                    },
                    InputCommon::Polling::DeviceType::Button);
            });
            connect(extra, &QPushButton::customContextMenuRequested, this,
                    [this, button_id, slotCapture](const QPoint&) {
                        QMenu context_menu;
                        if ((int)buttons_param[button_id].size() > slotCapture &&
                            !buttons_param[button_id][slotCapture].Serialize().empty()) {
                            context_menu.addAction(tr("Clear"), this, [=] {
                                if ((int)buttons_param[button_id].size() > slotCapture)
                                    buttons_param[button_id].erase(
                                        buttons_param[button_id].begin() + slotCapture);
                                UpdateMultiKeySlots(button_id);
                            });
                        }
                        context_menu.addAction(tr("Clear All"), this, [=] {
                            buttons_param[button_id].clear();
                            UpdateMultiKeySlots(button_id);
                        });
                        context_menu.addAction(tr("Restore Default"), this, [=] {
                            buttons_param[button_id].clear();
                            buttons_param[button_id].clear();
                            for (const auto& b : QtConfig::default_buttons[button_id])
                                buttons_param[button_id].push_back(Common::ParamPackage{b});
                            UpdateMultiKeySlots(button_id);
                        });
                        context_menu.addSeparator();
                        // Mouse button bindings
                        context_menu.addAction(tr("Mouse: Left Button"), this, [=] {
                            while ((int)buttons_param[button_id].size() <= slotCapture)
                                buttons_param[button_id].resize(slotCapture + 1);
                            buttons_param[button_id][slotCapture] =
                                Common::ParamPackage{"engine:mouse,button:left"};
                            UpdateMultiKeySlots(button_id);
                        });
                        context_menu.addAction(tr("Mouse: Right Button"), this, [=] {
                            while ((int)buttons_param[button_id].size() <= slotCapture)
                                buttons_param[button_id].resize(slotCapture + 1);
                            buttons_param[button_id][slotCapture] =
                                Common::ParamPackage{"engine:mouse,button:right"};
                            UpdateMultiKeySlots(button_id);
                        });
                        context_menu.addAction(tr("Mouse: Middle Button"), this, [=] {
                            while ((int)buttons_param[button_id].size() <= slotCapture)
                                buttons_param[button_id].resize(slotCapture + 1);
                            buttons_param[button_id][slotCapture] =
                                Common::ParamPackage{"engine:mouse,button:middle"};
                            UpdateMultiKeySlots(button_id);
                        });
                        context_menu.addAction(tr("Mouse: Wheel Up"), this, [=] {
                            while ((int)buttons_param[button_id].size() <= slotCapture)
                                buttons_param[button_id].resize(slotCapture + 1);
                            buttons_param[button_id][slotCapture] =
                                Common::ParamPackage{"engine:mouse,axis:wheel,value:up"};
                            UpdateMultiKeySlots(button_id);
                        });
                        context_menu.addAction(tr("Mouse: Wheel Down"), this, [=] {
                            while ((int)buttons_param[button_id].size() <= slotCapture)
                                buttons_param[button_id].resize(slotCapture + 1);
                            buttons_param[button_id][slotCapture] =
                                Common::ParamPackage{"engine:mouse,axis:wheel,value:down"};
                            UpdateMultiKeySlots(button_id);
                        });
                        context_menu.addSeparator();
                        // Turbo / Toggle / Reverse — same as primary button slots
                        {
                            bool has_binding = false;
                            for (int s = 0; s < (int)buttons_param[button_id].size(); s++)
                                if (!buttons_param[button_id][s].Serialize().empty())
                                    has_binding = true;
                            if (has_binding) {
                            bool is_turbo = (slotCapture < (int)buttons_param[button_id].size() &&
                                buttons_param[button_id][slotCapture].Get("turbo", "0") == "1");
                            bool is_latch = (slotCapture < (int)buttons_param[button_id].size() &&
                                buttons_param[button_id][slotCapture].Get("latch", "0") == "1");
                            bool is_toggle = false;
                            for (int s = 0; s < (int)buttons_param[button_id].size(); s++)
                                if (buttons_param[button_id][s].Get("toggle", "0") == "1")
                                    is_toggle = true;
                            if (is_turbo) {
                                context_menu.addAction(tr("Cancel Turbo"), this, [=] {
                                    buttons_param[button_id][slotCapture].Erase("turbo");
                                    UpdateButtonColor(button_id, slotCapture);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                            } else if (is_latch) {
                                context_menu.addAction(tr("Cancel Toggle"), this, [=] {
                                    buttons_param[button_id][slotCapture].Erase("latch");
                                    UpdateButtonColor(button_id, slotCapture);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                            } else if (is_toggle) {
                                context_menu.addAction(tr("Cancel Reverse"), this, [=] {
                                    for (int s = 0; s < (int)buttons_param[button_id].size(); s++)
                                        buttons_param[button_id][s].Erase("toggle");
                                    for (int s = 0; s < (int)button_map[button_id].size(); s++)
                                        UpdateButtonColor(button_id, s);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                            } else {
                                context_menu.addAction(tr("Set Turbo"), this, [=] {
                                    buttons_param[button_id][slotCapture].Erase("toggle");
                                    buttons_param[button_id][slotCapture].Erase("latch");
                                    buttons_param[button_id][slotCapture].Set("turbo", "1");
                                    UpdateButtonColor(button_id, slotCapture);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                                context_menu.addAction(tr("Set Toggle"), this, [=] {
                                    buttons_param[button_id][slotCapture].Erase("turbo");
                                    buttons_param[button_id][slotCapture].Erase("toggle");
                                    buttons_param[button_id][slotCapture].Set("latch", "1");
                                    // Clear toggle (reverse) from other slots to avoid inversion
                                    for (int s = 0; s < (int)buttons_param[button_id].size(); s++)
                                        if (s != slotCapture) buttons_param[button_id][s].Erase("toggle");
                                    for (int s = 0; s < (int)button_map[button_id].size(); s++)
                                        UpdateButtonColor(button_id, s);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                                context_menu.addAction(tr("Set Reverse"), this, [=] {
                                    for (int s = 0; s < (int)buttons_param[button_id].size(); s++) {
                                        buttons_param[button_id][s].Erase("turbo");
                                        buttons_param[button_id][s].Erase("latch");
                                        buttons_param[button_id][s].Set("toggle", "1");
                                    }
                                    for (int s = 0; s < (int)button_map[button_id].size(); s++)
                                        UpdateButtonColor(button_id, s);
                                    ApplyConfiguration();
                                    Settings::SaveProfile(ui->profile->currentIndex());
                                });
                            }
                            } // if (has_binding)
                        }
                        context_menu.exec(QCursor::pos());
                    });
        }

        // Insert container into layout (first time only)
        if (auto* box = qobject_cast<QBoxLayout*>(leafLayout))
            box->insertWidget(btnIdx + 1, container);
        else if (auto* grid = qobject_cast<QGridLayout*>(leafLayout)) {
            int row, col, rowSpan, colSpan;
            grid->getItemPosition(btnIdx, &row, &col, &rowSpan, &colSpan);
            grid->addWidget(container, row + rowSpan, col, 1, colSpan);
        }
    }

    int showCount = hasPrimary ? std::min(activeCount + 1, MAX_BINDINGS_PER_BUTTON) : 1;

    // Update text for the primary and any extras
    for (int slot = 0; slot < (int)button_map[button_id].size(); slot++) {
        auto* btn = button_map[button_id][slot];
        if (slot < showCount) {
            QString txt;
            if (slot < activeCount) {
                txt = ButtonToText(buttons_param[button_id][slot]);
                btn->setText(txt);
            } else if (slot > 0) {
                txt = tr("[extra not set]");
                btn->setText(txt);
            } else {
                txt = tr("[not set]");
                btn->setText(txt);
            }
            btn->show();
        } else {
            if (slot > 0) btn->hide();
        }
    }

    // Destroy extras when no bindings remain
    if (!needExtras && container) {
        auto* leafLayout = button_container_layouts[button_id];
        if (leafLayout && leafLayout->indexOf(container) >= 0)
            leafLayout->removeWidget(container);
        container->deleteLater();
        container = nullptr;
        // Trim button_map back to just the primary
        button_map[button_id].resize(1);
    }

    if (container)
        container->setVisible(needExtras);

    // Update colors for all slots
    for (int slot = 0; slot < (int)button_map[button_id].size(); slot++)
        UpdateButtonColor(button_id, slot);
}

void ConfigureInput::UpdateButtonColor(int button_id, int slot) {
    if (slot >= (int)buttons_param[button_id].size()) {
        if (slot < (int)button_map[button_id].size())
            button_map[button_id][slot]->setStyleSheet({});
        return;
    }
    const auto& pkg = buttons_param[button_id][slot];
    bool is_latch = pkg.Get("latch", "0") == "1";
    bool is_turbo = pkg.Get("turbo", "0") == "1";
    bool is_toggle = pkg.Get("toggle", "0") == "1";
    if (slot < (int)button_map[button_id].size()) {
        if (is_latch)
            button_map[button_id][slot]->setStyleSheet(
                QStringLiteral("background-color: #e65100; color: white;"));
        else if (is_turbo)
            button_map[button_id][slot]->setStyleSheet(
                QStringLiteral("background-color: #2e7d32; color: white;"));
        else if (is_toggle)
            button_map[button_id][slot]->setStyleSheet(
                QStringLiteral("background-color: #1565c0; color: white;"));
        else
            button_map[button_id][slot]->setStyleSheet({});
    }
}

void ConfigureInput::SetupAnalogMultiKeySlots(int analog_id, int sub_button_id) {
    if (analog_map_buttons[analog_id][sub_button_id].empty()) return;
    auto* primary = analog_map_buttons[analog_id][sub_button_id][0];
    if (!primary) return;

    QWidget* pw = primary->parentWidget();
    if (!pw || !pw->layout()) return;

    // Find the leaf layout containing the primary button
    QLayout* leafLayout = nullptr;
    std::function<QLayout*(QLayout*)> findLeaf = [&](QLayout* l) -> QLayout* {
        if (l->indexOf(primary) >= 0) return l;
        for (int i = 0; i < l->count(); i++) {
            if (auto* child = l->itemAt(i)->layout())
                if (auto* found = findLeaf(child)) return found;
        }
        return nullptr;
    };
    leafLayout = findLeaf(pw->layout());
    if (!leafLayout) return;

    int btnIdx = leafLayout->indexOf(primary);

    // Create a container widget for extra binding slots.
    // Deferred insertion: kept out of layout until showCount > 1
    QWidget* container = new QWidget(pw);
    QVBoxLayout* vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(1);

    for (int slot = 1; slot < MAX_BINDINGS_PER_BUTTON; slot++) {
        QPushButton* extra = new QPushButton(tr("[extra not set]"), container);
        extra->setMinimumHeight(24);
        extra->hide();
        vbox->addWidget(extra);
        analog_map_buttons[analog_id][sub_button_id].push_back(extra);
    }

    container->hide();
    analog_button_containers[analog_id][sub_button_id] = container;
    analog_button_container_layouts[analog_id][sub_button_id] = leafLayout;
    analog_button_container_positions[analog_id][sub_button_id] = btnIdx;
}

void ConfigureInput::UpdateAnalogMultiKeySlots(int analog_id, int sub_button_id) {
    const auto& dir = analog_sub_buttons[sub_button_id];
    int cnt = AnalogButtonCount(analogs_param[analog_id], dir);
    int showCount = std::min(cnt + 1, (int)analog_map_buttons[analog_id][sub_button_id].size());
    showCount = std::max(showCount, 1);

    auto* container = analog_button_containers[analog_id][sub_button_id];
    auto* leafLayout = analog_button_container_layouts[analog_id][sub_button_id];
    int btnIdx = analog_button_container_positions[analog_id][sub_button_id];

    // Deferred container management: insert when extras needed, remove when not
    bool hasExtras = showCount > 1;
    bool inLayout = leafLayout && leafLayout->indexOf(container) >= 0;

    if (hasExtras && !inLayout) {
        if (auto* box = qobject_cast<QBoxLayout*>(leafLayout))
            box->insertWidget(btnIdx + 1, container);
        else if (auto* grid = qobject_cast<QGridLayout*>(leafLayout)) {
            int row, col, rowSpan, colSpan;
            grid->getItemPosition(btnIdx, &row, &col, &rowSpan, &colSpan);
            grid->addWidget(container, row + rowSpan, col, 1, colSpan);
        }
    } else if (!hasExtras && inLayout) {
        leafLayout->removeWidget(container);
    }
    if (container)
        container->setVisible(hasExtras);

    for (int slot = 0; slot < (int)analog_map_buttons[analog_id][sub_button_id].size(); slot++) {
        auto* btn = analog_map_buttons[analog_id][sub_button_id][slot];
        if (slot < showCount) {
            if (slot < cnt)
                btn->setText(ButtonToText(AnalogButtonN(analogs_param[analog_id], dir, slot)));
            else if (slot > 0)
                btn->setText(tr("[extra not set]"));
            else
                btn->setText(tr("[not set]"));
            btn->show();
        } else {
            if (slot > 0) btn->hide();
        }
    }
    // Update colors for analog direction buttons
    UpdateAnalogButtonColor(analog_id, sub_button_id);
}

void ConfigureInput::UpdateAnalogButtonColor(int analog_id, int sub_button_id) {
    const auto& dir = analog_sub_buttons[sub_button_id];
    std::string turbo_key = dir + "_turbo";
    std::string toggle_key = dir + "_toggle";
    bool is_turbo = analogs_param[analog_id].Get(turbo_key, "0") == "1";
    bool is_toggle = analogs_param[analog_id].Get(toggle_key, "0") == "1";
    for (int slot = 0; slot < (int)analog_map_buttons[analog_id][sub_button_id].size(); slot++) {
        auto* btn = analog_map_buttons[analog_id][sub_button_id][slot];
        if (!btn) continue;
        if (is_turbo)
            btn->setStyleSheet(QStringLiteral("background-color: #2e7d32; color: white;"));
        else if (is_toggle)
            btn->setStyleSheet(QStringLiteral("background-color: #1565c0; color: white;"));
        else
            btn->setStyleSheet({});
    }
}

void ConfigureInput::UpdateButtonLabels() {
    for (int button = 0; button < Settings::NativeButton::NumButtons; button++) {
        for (int slot = 0; slot < (int)button_map[button].size(); slot++) {
            if (button_map[button][slot] && slot < (int)buttons_param[button].size() &&
                !buttons_param[button][slot].Serialize().empty())
                button_map[button][slot]->setText(ButtonToText(buttons_param[button][slot]));
            else if (button_map[button][slot])
                button_map[button][slot]->setText(
                    slot > 0 ? tr("[extra not set]") : tr("[not set]"));
        }
    }

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; sub_button_id++) {
            if (!analog_map_buttons[analog_id][sub_button_id].empty() &&
                analog_map_buttons[analog_id][sub_button_id][0]) {
                UpdateAnalogMultiKeySlots(analog_id, sub_button_id);
            }
        }
        analog_map_stick[analog_id]->setText(tr("Set Analog Stick"));

        auto& param = analogs_param[analog_id];
        auto* const analog_stick_slider = analog_map_deadzone_and_modifier_slider[analog_id];
        auto* const analog_stick_slider_label =
            analog_map_deadzone_and_modifier_slider_label[analog_id];

        if (param.Has("engine")) {
            const auto engine{param.Get("engine", "")};
            if (engine == "sdl" || engine == "gcpad") {
                if (!param.Has("deadzone")) {
                    param.Set("deadzone", 0.1f);
                }
                const auto slider_value = static_cast<int>(param.Get("deadzone", 0.1f) * 100);
                analog_stick_slider_label->setText(tr("Deadzone: %1%").arg(slider_value));
                analog_stick_slider->setValue(slider_value);
            } else {
                if (!param.Has("modifier_scale")) {
                    param.Set("modifier_scale", 0.5f);
                }
                const auto slider_value = static_cast<int>(param.Get("modifier_scale", 0.5f) * 100);
                analog_stick_slider_label->setText(tr("Modifier Scale: %1%").arg(slider_value));
                analog_stick_slider->setValue(slider_value);
            }
        }
    }

    ui->buttonCircleMod->setText(
        circlemod_param.empty() || circlemod_param[0].Serialize().empty()
            ? tr("[not set]")
            : ButtonToText(circlemod_param[0]));
    UpdateCircleModMultiKeySlots();

    // Touch screen points
    UpdateTouchPointsMultiKeySlots();

    EmitInputKeysChanged();
}

void ConfigureInput::MapFromButton(const Common::ParamPackage& params) {
    Common::ParamPackage aux_param;
    bool mapped = false;
    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; button_id++) {
        aux_param = InputCommon::GetControllerButtonBinds(params, button_id);
        if (aux_param.Has("engine")) {
            buttons_param[button_id].clear();
            buttons_param[button_id].push_back(aux_param);
            mapped = true;
        }
    }
    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        aux_param = InputCommon::GetControllerAnalogBinds(params, analog_id);
        if (aux_param.Has("engine")) {
            analogs_param[analog_id] = aux_param;
            mapped = true;
        }
    }
    if (!mapped) {
        QMessageBox::warning(
            this, tr("Warning"),
            tr("Auto mapping failed. Your controller may not have a corresponding mapping"));
    }
}

void ConfigureInput::AutoMap() {
    ui->buttonAutoMap->setEnabled(false);
    if (QMessageBox::information(this, tr("Information"),
                                 tr("After pressing OK, press any button on your joystick"),
                                 QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Cancel) {
        ui->buttonAutoMap->setEnabled(true);
        return;
    }
    input_setter = [this](const Common::ParamPackage& params) {
        MapFromButton(params);
        ApplyConfiguration();
        Settings::SaveProfile(ui->profile->currentIndex());
    };
    device_pollers = InputCommon::Polling::GetPollers(InputCommon::Polling::DeviceType::Button);
    want_keyboard_keys = false;
    for (auto& poller : device_pollers) {
        poller->Start();
    }
    timeout_timer->start(5000); // Cancel after 5 seconds
    poll_timer->start(200);     // Check for new inputs every 200ms
    ui->buttonAutoMap->setEnabled(true);
}

void ConfigureInput::HandleClick(QPushButton* button,
                                 std::function<void(const Common::ParamPackage&)> new_input_setter,
                                 InputCommon::Polling::DeviceType type) {
    // Cancel any previous capture that may still be in progress
    if (input_setter) {
        SetPollingResult({}, true);
    }
    previous_key_code = QKeySequence(button->text())[0].toCombined();
    button->setText(tr("[press key]"));
    button->setFocus();
    // Block Tab focus chain during capture so Tab can be bound
    capture_old_focus_policy = button->focusPolicy();
    capture_button = button;
    button->setFocusPolicy(Qt::NoFocus);

    input_setter = new_input_setter;

    device_pollers = InputCommon::Polling::GetPollers(type);

    // Keyboard keys can only be used as button devices
    want_keyboard_keys = type == InputCommon::Polling::DeviceType::Button;

    for (auto& poller : device_pollers) {
        poller->Start();
    }

    grabKeyboard();
    grabMouse();
    // Clear any stuck keys from previous dialog interactions
    if (auto* kb = InputCommon::GetKeyboard(); kb) {
        kb->ReleaseAllKeys();
    }
    timeout_timer->start(5000); // Cancel after 5 seconds
    poll_timer->start(200);     // Check for new inputs every 200ms
}

void ConfigureInput::SetPollingResult(const Common::ParamPackage& params, bool abort) {
    // Restore focus policy if we changed it for Tab capture
    if (capture_button) {
        capture_button->setFocusPolicy(capture_old_focus_policy);
        capture_button = nullptr;
    }
    releaseKeyboard();
    releaseMouse();
    // Clear any key states that may have been set during the grab
    if (auto* kb = InputCommon::GetKeyboard(); kb) {
        kb->ReleaseAllKeys();
    }
    timeout_timer->stop();
    poll_timer->stop();
    for (auto& poller : device_pollers) {
        poller->Stop();
    }

    if (!abort && input_setter) {
        (*input_setter)(params);
    }

    UpdateButtonLabels();
    input_setter.reset();
}

void ConfigureInput::keyPressEvent(QKeyEvent* event) {
    if (!input_setter || !event)
        return;

    // Tab/Backtab: handled via event() override, treat like any other key here
    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
        event->accept();
        if (want_keyboard_keys) {
            SetPollingResult(Common::ParamPackage{InputCommon::GenerateKeyboardParam(event->key())}, false);
        }
        return;
    }

    if (event->key() != Qt::Key_Escape) {
        if (want_keyboard_keys) {
            // Only prevent conflicts with hotkeys, allow same key across multiple buttons
            if (hotkey_list.contains(QKeySequence(event->key()))) {
                SetPollingResult({}, true);
                QMessageBox::critical(this, tr("Error!"),
                                      tr("You're using a key that's already bound to a hotkey."));
                return;
            }
            SetPollingResult(Common::ParamPackage{InputCommon::GenerateKeyboardParam(event->key())},
                             false);
        } else {
            // Escape key wasn't pressed and we don't want any keyboard keys, so don't stop
            // polling
            return;
        }
    }
    SetPollingResult({}, true);
    previous_key_code = 0;
}

void ConfigureInput::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureInput::NewProfile() {
    ui->buttonNew->setEnabled(false);
    const QString name =
        QInputDialog::getText(this, tr("New Profile"), tr("Enter the name for the new profile."));
    if (name.isEmpty()) {
        ui->buttonNew->setEnabled(true);
        return;
    }
    if (IsProfileNameDuplicate(name)) {
        WarnProposedProfileNameIsDuplicate();
        ui->buttonNew->setEnabled(true);
        return;
    }

    ApplyConfiguration();
    Settings::SaveProfile(ui->profile->currentIndex());
    Settings::CreateProfile(name.toStdString());
    ui->profile->addItem(name);
    ui->profile->setCurrentIndex(Settings::values.current_input_profile_index);
    LoadConfiguration();
    ui->buttonDelete->setEnabled(ui->profile->count() > 1);
    ui->buttonNew->setEnabled(true);
}

void ConfigureInput::DeleteProfile() {
    ui->buttonDelete->setEnabled(false);
    const auto answer = QMessageBox::question(
        this, tr("Delete Profile"), tr("Delete profile %1?").arg(ui->profile->currentText()));
    if (answer != QMessageBox::Yes) {
        ui->buttonDelete->setEnabled(true);
        return;
    }
    const int index = ui->profile->currentIndex();
    ui->profile->removeItem(index);
    ui->profile->setCurrentIndex(0);
    Settings::DeleteProfile(index);
    LoadConfiguration();
    ui->buttonDelete->setEnabled(ui->profile->count() > 1);
}

void ConfigureInput::RenameProfile() {
    ui->buttonRename->setEnabled(false);
    const QString new_name = QInputDialog::getText(this, tr("Rename Profile"), tr("New name:"));
    if (new_name.isEmpty()) {
        ui->buttonRename->setEnabled(true);
        return;
    }
    if (IsProfileNameDuplicate(new_name)) {
        WarnProposedProfileNameIsDuplicate();
        ui->buttonRename->setEnabled(true);
        return;
    }

    ui->profile->setItemText(ui->profile->currentIndex(), new_name);
    Settings::RenameCurrentProfile(new_name.toStdString());
    Settings::SaveProfile(ui->profile->currentIndex());
    ui->buttonRename->setEnabled(true);
}

bool ConfigureInput::IsProfileNameDuplicate(const QString& name) const {
    return ui->profile->findText(name, Qt::MatchFixedString | Qt::MatchCaseSensitive) != -1;
}

void ConfigureInput::WarnProposedProfileNameIsDuplicate() {
    QMessageBox::warning(this, tr("Duplicate profile name"),
                         tr("Profile name already exists. Please choose a different name."));
}

// ----- CircleMod multi-key slot management -----

void ConfigureInput::SetupCircleModMultiKeySlots() {
    auto* primary = circlemod_button_map[0];
    if (!primary)
        return;
    QWidget* pw = primary->parentWidget();
    if (!pw || !pw->layout())
        return;
    // Find the leaf layout containing the primary button
    std::function<QLayout*(QLayout*)> findLeaf =
        [&](QLayout* l) -> QLayout* {
        if (l->indexOf(primary) >= 0)
            return l;
        for (int i = 0; i < l->count(); i++) {
            if (auto* child = l->itemAt(i)->layout())
                if (auto* found = findLeaf(child))
                    return found;
        }
        return nullptr;
    };
    circlemod_leaf_layout = findLeaf(pw->layout());
    if (!circlemod_leaf_layout)
        return;
    circlemod_btn_position = circlemod_leaf_layout->indexOf(primary);
}

void ConfigureInput::UpdateCircleModMultiKeySlots() {
    // Trim trailing empty params
    while (!circlemod_param.empty() &&
           circlemod_param.back().Serialize().empty())
        circlemod_param.pop_back();

    int activeCount = (int)circlemod_param.size();
    bool needExtras = activeCount > 0;

    // Lazy creation/destruction of the extra-slot container.
    if (needExtras && !circlemod_container) {
        auto* primary = circlemod_button_map[0];
        QWidget* pw = primary->parentWidget();

        circlemod_container = new QWidget(pw);
        QVBoxLayout* vbox = new QVBoxLayout(circlemod_container);
        vbox->setContentsMargins(0, 0, 0, 0);
        vbox->setSpacing(1);

        for (int slot = 1; slot < MAX_BINDINGS_PER_BUTTON; slot++) {
            QPushButton* extra = new QPushButton(tr("[extra not set]"), circlemod_container);
            extra->setMinimumHeight(24);
            extra->hide();
            vbox->addWidget(extra);
            circlemod_button_map.push_back(extra);

            extra->setContextMenuPolicy(Qt::CustomContextMenu);
            int slotCapture = slot;
            connect(extra, &QPushButton::clicked, [this, slotCapture, extra]() {
                HandleClick(
                    extra,
                    [this, slotCapture](Common::ParamPackage params) {
                        while ((int)circlemod_param.size() <= slotCapture)
                            circlemod_param.resize(slotCapture + 1);
                        circlemod_param[slotCapture] = std::move(params);
                        while (!circlemod_param.empty() &&
                               circlemod_param.back().Serialize().empty())
                            circlemod_param.pop_back();
                        UpdateCircleModMultiKeySlots();
                        SyncCircleModToAnalogs();
                        ApplyConfiguration();
                        Settings::SaveProfile(ui->profile->currentIndex());
                    },
                    InputCommon::Polling::DeviceType::Button);
            });
            connect(extra, &QPushButton::customContextMenuRequested, this,
                    [this, slotCapture, extra](const QPoint& pos) {
                        QMenu context_menu;
                        if (slotCapture < (int)circlemod_param.size() &&
                            !circlemod_param[slotCapture].Serialize().empty()) {
                            context_menu.addAction(tr("Clear"), this, [this, slotCapture] {
                                if (slotCapture < (int)circlemod_param.size())
                                    circlemod_param.erase(circlemod_param.begin() + slotCapture);
                                UpdateCircleModMultiKeySlots();
                                // Update primary button text (slot 0 — not handled by UpdateCircleModMultiKeySlots)
                                if (circlemod_button_map[0]) {
                                    circlemod_button_map[0]->setText(
                                        circlemod_param.empty()
                                            ? tr("[not set]")
                                            : ButtonToText(circlemod_param[0]));
                                }
                                SyncCircleModToAnalogs();
                                ApplyConfiguration();
                                Settings::SaveProfile(ui->profile->currentIndex());
                            });
                        }
                        context_menu.addAction(tr("Clear All"), this, [this] {
                            circlemod_param.clear();
                            UpdateCircleModMultiKeySlots();
                            if (circlemod_button_map[0])
                                circlemod_button_map[0]->setText(tr("[not set]"));
                            SyncCircleModToAnalogs();
                            ApplyConfiguration();
                            Settings::SaveProfile(ui->profile->currentIndex());
                        });
                        context_menu.addAction(tr("Restore Default"), this, [this] {
                            circlemod_param.clear();
                            circlemod_param.push_back(Common::ParamPackage{
                                InputCommon::GenerateKeyboardParam(
                                    QtConfig::default_analogs[0][4])});
                            UpdateCircleModMultiKeySlots();
                            SyncCircleModToAnalogs();
                            ApplyConfiguration();
                            Settings::SaveProfile(ui->profile->currentIndex());
                        });
                        context_menu.exec(extra->mapToGlobal(pos));
                    });
        }

        // Insert container after the primary button in the leaf layout
        circlemod_leaf_layout->removeWidget(circlemod_container);
        QBoxLayout* box = qobject_cast<QBoxLayout*>(circlemod_leaf_layout);
        if (box)
            box->insertWidget(circlemod_btn_position + 1, circlemod_container);
        else
            circlemod_leaf_layout->addWidget(circlemod_container);
    } else if (!needExtras && circlemod_container) {
        // Destroy container when no bindings remain
        if (circlemod_leaf_layout && circlemod_leaf_layout->indexOf(circlemod_container) >= 0)
            circlemod_leaf_layout->removeWidget(circlemod_container);
        circlemod_container->deleteLater();
        circlemod_container = nullptr;
        // Trim button_map back to just the primary
        circlemod_button_map.resize(1);
    }

    if (circlemod_container)
        circlemod_container->setVisible(needExtras);

    // Update visibility and text for extra buttons (slot 0 = primary, handled in UpdateButtonLabels)
    int showCount = std::min(activeCount + 1, MAX_BINDINGS_PER_BUTTON);
    for (int slot = 1; slot < (int)circlemod_button_map.size(); slot++) {
        QPushButton* btn = circlemod_button_map[slot];
        if (!btn)
            continue;
        if (slot < showCount) {
            if (slot < activeCount)
                btn->setText(ButtonToText(circlemod_param[slot]));
            else
                btn->setText(tr("[extra not set]"));
            btn->show();
        } else {
            btn->hide();
        }
    }
}

void ConfigureInput::SyncCircleModToAnalogs() {
    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        ClearAnalogButtons(analogs_param[analog_id], "modifier");
        for (int i = 0; i < (int)circlemod_param.size(); i++) {
            if (!circlemod_param[i].Serialize().empty())
                SetAnalogButtonN(analogs_param[analog_id], "modifier", i, circlemod_param[i]);
        }
    }
}

void ConfigureInput::LoadCircleModFromAnalogs() {
    circlemod_param.clear();
    // Read multi-key format: modifier_0, modifier_1, ..., modifier_count
    int cnt = analogs_param[0].Get("modifier_count", 0);
    for (int i = 0; i < cnt; i++) {
        std::string key = "modifier_" + std::to_string(i);
        if (analogs_param[0].Has(key)) {
            circlemod_param.push_back(Common::ParamPackage(analogs_param[0].Get(key, "")));
        }
    }
    // Fall back to legacy single "modifier:" key, but skip code:0 (cleared default)
    if (circlemod_param.empty() && analogs_param[0].Has("modifier")) {
        auto pkg = Common::ParamPackage(analogs_param[0].Get("modifier", ""));
        if (pkg.Get("code", 0) != 0)
            circlemod_param.push_back(std::move(pkg));
    }
}

void ConfigureInput::SetupTouchPointsMultiKeySlots() {
    // Already set up in constructor; this is a placeholder for consistency.
}

void ConfigureInput::UpdateTouchPointsMultiKeySlots() {
    // touch_points_param is now: vector<vector<ParamPackage>>
    // outer index = point (0..MAX_TOUCH_POINTS-1)
    // inner index = key binding slot (0..MAX_KEYS_PER_POINT-1)
    //
    // Visibility rules:
    //   - The whole Point group box is shown only if any binding exists in this point,
    //     OR if the previous point has a binding (so you can configure the next one).
    //   - Within a Point: slot 0 is always shown. Slot k>0 is shown only if
    //     slots [0..k-1] all have bindings (i.e. you fill slots in order).
    for (int point = 0; point < (int)touch_point_widgets.size(); point++) {
        const auto& w = touch_point_widgets[point];

        // Has the user bound anything in this point?
        bool any_bound = false;
        if (point < (int)touch_points_param.size()) {
            for (const auto& b : touch_points_param[point]) {
                if (!b.Serialize().empty()) {
                    any_bound = true;
                    break;
                }
            }
        }
        // Should the previous point's box be visible because of a next-point chain?
        // (No - we just hide later points until they're activated.)
        // Visibility of the group box itself:
        // Point 0 is always visible so user can start a new mapping.
        // Point k>0 is visible only if a binding exists in Point k-1 (chain).
        bool has_prev_bound = false;
        if (point > 0 && (point - 1) < (int)touch_points_param.size()) {
            for (const auto& b : touch_points_param[point - 1]) {
                if (!b.Serialize().empty()) {
                    has_prev_bound = true;
                    break;
                }
            }
        }
        w.group->setVisible(point == 0 || any_bound || has_prev_bound);

        // Per-slot text and visibility
        for (int s = 0; s < (int)w.key_buttons.size(); s++) {
            QPushButton* btn = w.key_buttons[s];
            if (!btn) continue;
            bool bound = (point < (int)touch_points_param.size() &&
                          s < (int)touch_points_param[point].size() &&
                          !touch_points_param[point][s].Serialize().empty());
            if (bound) {
                btn->setText(ButtonToText(touch_points_param[point][s]));
                // Color: latch=orange, turbo=green, toggle(reverse)=blue
                const auto& pkg = touch_points_param[point][s];
                if (pkg.Get("latch", "0") == "1")
                    btn->setStyleSheet(QStringLiteral("background-color: #e65100; color: white;"));
                else if (pkg.Get("turbo", "0") == "1")
                    btn->setStyleSheet(QStringLiteral("background-color: #2e7d32; color: white;"));
                else if (pkg.Get("toggle", "0") == "1")
                    btn->setStyleSheet(QStringLiteral("background-color: #1565c0; color: white;"));
                else
                    btn->setStyleSheet({});
            } else {
                btn->setText(tr("[not set]"));
                btn->setStyleSheet({});
            }
            // Slot 0 is always visible when the point group is visible,
            // so the user can always set the primary key.
            if (s == 0) {
                btn->setVisible(w.group->isVisible());
                btn->show();
            } else {
                bool prev_all_bound = true;
                for (int k = 0; k < s; k++) {
                    bool kb = (point < (int)touch_points_param.size() &&
                               k < (int)touch_points_param[point].size() &&
                               !touch_points_param[point][k].Serialize().empty());
                    if (!kb) {
                        prev_all_bound = false;
                        break;
                    }
                }
                btn->setVisible(w.group->isVisible() && prev_all_bound);
            }
        }
    }
}

void ConfigureInput::UpdateFixedMotionSlots() {
    if (fixed_motion_widgets.empty()) return;
    // Find the highest preset index that has any binding
    int max_bound_idx = -1;
    for (int p = 0; p < (int)fixed_motion_widgets.size(); p++) {
        for (const auto& s : fixed_motion_widgets[p].slot_params) {
            if (!s.isEmpty()) { max_bound_idx = p; break; }
        }
    }
    // Show presets 0 through (max_bound + 1), so the next empty slot is always ready
    int tail_idx = (max_bound_idx >= 0) ? (max_bound_idx + 1) : 0;
    if (tail_idx >= (int)fixed_motion_widgets.size())
        tail_idx = (int)fixed_motion_widgets.size() - 1;
    for (int p = 0; p < (int)fixed_motion_widgets.size(); p++) {
        auto& w = fixed_motion_widgets[p];
        bool should_show = (p <= tail_idx);
        w.container->setVisible(should_show);
        if (!should_show) continue;
        for (int s = 0; s < (int)w.slot_btns.size(); s++) {
            bool has_current = (s < (int)w.slot_params.size() && !w.slot_params[s].isEmpty());
            bool prev_is_set = (s == 0) || (s - 1 < (int)w.slot_params.size() && !w.slot_params[s - 1].isEmpty());
            bool show = (s == 0) || prev_is_set;
            w.slot_btns[s]->setVisible(show);
            if (has_current) {
                w.slot_btns[s]->setText(ButtonToText(Common::ParamPackage(w.slot_params[s].toStdString())));
            } else if (show) {
                w.slot_btns[s]->setText(tr("[not set]"));
            }
        }
    }
    UpdateFixedMotionButtonColors();
}

void ConfigureInput::UpdateFixedMotionButtonColors() {
    for (int p = 0; p < (int)fixed_motion_widgets.size(); p++) {
        auto& w = fixed_motion_widgets[p];
        if (!w.container) continue;
        // Check if any slot has reverse → all buttons turn blue
        bool any_reverse = false;
        for (size_t s = 0; s < w.slot_params.size(); s++) {
            if (!w.slot_params[s].isEmpty()) {
                Common::ParamPackage pp(w.slot_params[s].toStdString());
                if (pp.Get("reverse", "0") == "1") { any_reverse = true; break; }
            }
        }
        for (int s = 0; s < (int)w.slot_btns.size(); s++) {
            if (s >= (int)w.slot_btns.size() || !w.slot_btns[s]) continue;
            if (any_reverse)
                w.slot_btns[s]->setStyleSheet(QStringLiteral("background-color: #1565c0; color: white;"));
            else if (s < (int)w.slot_params.size() && !w.slot_params[s].isEmpty()) {
                Common::ParamPackage pp(w.slot_params[s].toStdString());
                if (pp.Get("toggle", "0") == "1")
                    w.slot_btns[s]->setStyleSheet(QStringLiteral("background-color: #e65100; color: white;"));
                else if (pp.Get("turbo", "0") == "1")
                    w.slot_btns[s]->setStyleSheet(QStringLiteral("background-color: #2e7d32; color: white;"));
                else
                    w.slot_btns[s]->setStyleSheet({});
            } else {
                w.slot_btns[s]->setStyleSheet({});
            }
        }
    }
}

bool ConfigureInput::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::Wheel && qobject_cast<QSlider*>(obj)) {
        return true;
    }
    return QWidget::eventFilter(obj, event);
}
