// Copyright Citra Emulator Project / Lime3DS Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <QKeySequence>
#include <QWidget>
#include <QGroupBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include "common/param_package.h"
#include "common/settings.h"
#include "input_common/main.h"

class QKeyEvent;
class QLabel;
class QPushButton;
class QSlider;
class QString;
class QTimer;

namespace Ui {
class ConfigureInput;
}

class ConfigureInput : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureInput(Core::System& system, QWidget* parent = nullptr);
    ~ConfigureInput() override;

    /// Save all button configurations to settings file
    void ApplyConfiguration();
    void RetranslateUI();

    /// Load configuration settings.
    void LoadConfiguration();
    void EmitInputKeysChanged();

protected:
    void showEvent(QShowEvent* event) override;

public:
    /// Save the current input profile index
    void ApplyProfile();

public slots:
    void OnHotkeysChanged(QList<QKeySequence> new_key_list);

signals:
    void InputKeysChanged(QList<QKeySequence> new_key_list);

private:
    Core::System& system;
    std::unique_ptr<Ui::ConfigureInput> ui;

    std::unique_ptr<QTimer> timeout_timer;
    std::unique_ptr<QTimer> poll_timer;

    /// This will be the the setting function when an input is awaiting configuration.
    std::optional<std::function<void(const Common::ParamPackage&)>> input_setter;

    static constexpr int MAX_BINDINGS_PER_BUTTON = 10;

    std::array<std::vector<Common::ParamPackage>, Settings::NativeButton::NumButtons> buttons_param;
    std::array<Common::ParamPackage, Settings::NativeAnalog::NumAnalogs> analogs_param;

    static constexpr int ANALOG_SUB_BUTTONS_NUM = 9;

    /// Each button input is represented by multiple QPushButtons (multi-key mapping).
    std::array<std::vector<QPushButton*>, Settings::NativeButton::NumButtons> button_map;
    /// Layout containers holding the horizontal button rows.
    std::array<QWidget*, Settings::NativeButton::NumButtons> button_containers{};
    /// Parent layouts for deferred container insertion (avoid spacing when hidden).
    std::array<QLayout*, Settings::NativeButton::NumButtons> button_container_layouts{};
    std::array<int, Settings::NativeButton::NumButtons> button_container_positions{};

    /// A group of five QPushButtons represent one analog input. The buttons each represent up,
    /// down, left, right, and modifier, respectively. Each direction supports multi-key binding
    /// via a vector of QPushButtons (primary + extras).
    std::array<std::array<std::vector<QPushButton*>, ANALOG_SUB_BUTTONS_NUM>,
               Settings::NativeAnalog::NumAnalogs>
        analog_map_buttons;
    /// Layout containers for analog direction extra binding rows.
    std::array<std::array<QWidget*, ANALOG_SUB_BUTTONS_NUM>, Settings::NativeAnalog::NumAnalogs>
        analog_button_containers{};
    /// Parent layouts for deferred analog container insertion.
    std::array<std::array<QLayout*, ANALOG_SUB_BUTTONS_NUM>, Settings::NativeAnalog::NumAnalogs>
        analog_button_container_layouts{};
    std::array<std::array<int, ANALOG_SUB_BUTTONS_NUM>, Settings::NativeAnalog::NumAnalogs>
        analog_button_container_positions{};

    /// Circle Mod (轻推摇杆) multi-key data. CircleMod is an analog modifier that
    /// applies to all analogs and is not part of NativeButton::Values.
    std::vector<Common::ParamPackage> circlemod_param;
    std::vector<QPushButton*> circlemod_button_map;
    QWidget* circlemod_container = nullptr;
    QLayout* circlemod_leaf_layout = nullptr;
    int circlemod_btn_position = -1;

    /// Touch screen coordinate bindings.
    /// touch_points_param[point][slot] = ParamPackage with engine/key + x (0-100) + y (0-100).
    /// Up to MAX_TOUCH_POINTS points, each with up to MAX_KEYS_PER_POINT keys.
    std::vector<std::vector<Common::ParamPackage>> touch_points_param;
    struct TouchPointWidgets {
        QSlider* x_slider;
        QSlider* y_slider;
        std::vector<QPushButton*> key_buttons;
        QGroupBox* group;
    };
    std::vector<TouchPointWidgets> touch_point_widgets;
    static constexpr int MAX_TOUCH_POINTS = 15;
    static constexpr int MAX_KEYS_PER_POINT = 5;

    // Fixed Motion Override presets
    static constexpr int MAX_FIXED_MOTION_PRESETS = 8;
    static constexpr int MAX_FIXED_MOTION_KEYS = 5;
    struct FixedMotionWidgets {
        QWidget* container;
        QSlider* grav_x;     QDoubleSpinBox* grav_x_label;
        QSlider* grav_y;     QDoubleSpinBox* grav_y_label;
        QSlider* grav_z;     QDoubleSpinBox* grav_z_label;
        QSlider* rate_x;     QDoubleSpinBox* rate_x_label;
        QSlider* rate_y;     QDoubleSpinBox* rate_y_label;
        QSlider* rate_z;     QDoubleSpinBox* rate_z_label;
        std::vector<QPushButton*> slot_btns;
        std::vector<QString> slot_params;
        bool has_binding = false;
    };
    std::vector<FixedMotionWidgets> fixed_motion_widgets;
    QCheckBox* fixed_motion_enabled = nullptr;
    void SetupTouchPointsMultiKeySlots();
    void UpdateFixedMotionSlots();
    void UpdateFixedMotionButtonColors();
    bool eventFilter(QObject* obj, QEvent* event) override;
    void UpdateTouchPointsMultiKeySlots();

    /// Analog inputs are also represented each with a single button, used to configure with an
    /// actual analog stick
    std::array<QPushButton*, Settings::NativeAnalog::NumAnalogs> analog_map_stick;
    std::array<QSlider*, Settings::NativeAnalog::NumAnalogs>
        analog_map_deadzone_and_modifier_slider;
    std::array<QLabel*, Settings::NativeAnalog::NumAnalogs>
        analog_map_deadzone_and_modifier_slider_label;

    static const std::array<std::string, ANALOG_SUB_BUTTONS_NUM> analog_sub_buttons;

    std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> device_pollers;

    /**
     * List of keys currently registered to hotkeys.
     * These can't be bound to any input key.
     * Synchronised with ConfigureHotkeys via signal-slot.
     */
    QList<QKeySequence> hotkey_list;

    /// A flag to indicate if keyboard keys are okay when configuring an input. If this is false,
    /// keyboard events are ignored.
    bool want_keyboard_keys = false;

    /// Generates list of all used keys
    QList<QKeySequence> GetUsedKeyboardKeys();

    void MapFromButton(const Common::ParamPackage& params);
    void AutoMap();

    /// Restore all buttons to their default values.
    void RestoreDefaults();
    /// Clear all input configuration
    void ClearAll();

    /// Update UI to reflect current configuration.
    void UpdateButtonLabels();

    /// Setup multi-key UI slots for a button: creates extra QPushButtons in a horizontal row.
    void SetupMultiKeySlots(int button_id);
    /// Update visibility and text of all binding slots for a button.
    void UpdateMultiKeySlots(int button_id);
    /// Update button background colour based on turbo / toggle flags.
    void UpdateButtonColor(int button_id, int slot);
    void UpdateAnalogButtonColor(int analog_id, int sub_button_id);

    /// Setup multi-key UI slots for an analog direction button.
    void SetupAnalogMultiKeySlots(int analog_id, int sub_button_id);
    /// Update visibility and text of all binding slots for an analog direction.
    void UpdateAnalogMultiKeySlots(int analog_id, int sub_button_id);

    /// Setup multi-key UI slots for CircleMod (analog modifier, not in NativeButton).
    void SetupCircleModMultiKeySlots();
    /// Update visibility and text of all binding slots for CircleMod.
    void UpdateCircleModMultiKeySlots();
    /// Sync circlemod_param back into analogs_param modifier fields.
    void SyncCircleModToAnalogs();
    /// Load circlemod_param from analogs_param modifier fields.
    void LoadCircleModFromAnalogs();

    /// Called when the button was pressed.
    void HandleClick(QPushButton* button,
                     std::function<void(const Common::ParamPackage&)> new_input_setter,
                     InputCommon::Polling::DeviceType type);

    /// The key code of the previous state of the key being currently bound.
    int previous_key_code;

    /// Finish polling and configure input using the input_setter
    void SetPollingResult(const Common::ParamPackage& params, bool abort);

    /// Handle key press events.
    void keyPressEvent(QKeyEvent* event) override;

    /// input profiles
    void NewProfile();
    void DeleteProfile();
    void RenameProfile();

    bool IsProfileNameDuplicate(const QString& name) const;
    void WarnProposedProfileNameIsDuplicate();
};
