// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <QDialog>
#include "common/param_package.h"
#include "common/settings.h"
#include "input_common/main.h"
#include "input_common/udp/udp.h"

class QVBoxLayout;
class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QTimer;

namespace Ui {
class ConfigureMotionTouch;
}

/// A dialog for touchpad calibration configuration.
class CalibrationConfigurationDialog : public QDialog {
    Q_OBJECT
public:
    explicit CalibrationConfigurationDialog(QWidget* parent, const std::string& host, u16 port,
                                            u8 pad_index, u16 client_id);
    ~CalibrationConfigurationDialog() override;

private:
    Q_INVOKABLE void UpdateLabelText(const QString& text);
    Q_INVOKABLE void UpdateButtonText(const QString& text);

    QVBoxLayout* layout;
    QLabel* status_label;
    QPushButton* cancel_button;
    std::unique_ptr<InputCommon::CemuhookUDP::CalibrationConfigurationJob> job;

    // Configuration results
    bool completed{};
    u16 min_x{};
    u16 min_y{};
    u16 max_x{};
    u16 max_y{};

    friend class ConfigureMotionTouch;
};

class ConfigureMotionTouch : public QDialog {
    Q_OBJECT

public:
    explicit ConfigureMotionTouch(QWidget* parent = nullptr);
    ~ConfigureMotionTouch() override;

public slots:
    void ApplyConfiguration();

private slots:
    void OnCemuhookUDPTest();
    void OnConfigureTouchCalibration();
    void OnConfigureTouchFromButton();

private:
    void closeEvent(QCloseEvent* event) override;
    Q_INVOKABLE void ShowUDPTestResult(bool result);
    void SetConfiguration();
    void UpdateUiDisplay();
    void ConnectEvents();
    void SetPollingResult(const Common::ParamPackage& params, bool abort);
    bool CanCloseDialog();

    std::unique_ptr<Ui::ConfigureMotionTouch> ui;

    // Programmatically-added controls (not in .ui)
    QLabel* motion_tilt_max_label = nullptr;
    QDoubleSpinBox* motion_tilt_max_angle = nullptr;
    QCheckBox* motion_clamp_pitch = nullptr;
    QCheckBox* motion_auto_tilt_y = nullptr;
    QCheckBox* motion_auto_tilt_y_invert = nullptr;
    QCheckBox* motion_auto_tilt_x = nullptr;
    QDoubleSpinBox* motion_auto_tilt_speed = nullptr;
    QDoubleSpinBox* motion_auto_tilt_x_max_angle = nullptr;
    QDoubleSpinBox* motion_auto_tilt_y_return_speed = nullptr;
    QSpinBox* motion_auto_tilt_y_max_angle = nullptr;
    QCheckBox* motion_auto_tilt_y_prevent_flip = nullptr;

    // Controller-to-mouse linking (only visible when motion provider is Mouse)
    QCheckBox* link_cstick = nullptr;
    QCheckBox* link_circle_pad = nullptr;
    QCheckBox* link_dpad = nullptr;
    QCheckBox* link_abxy = nullptr;
    QDoubleSpinBox* link_speed = nullptr;
    QWidget* link_group = nullptr;  // parent widget for all link controls
    // Per-link invert checkboxes
    QCheckBox* link_cstick_invert_ud = nullptr;
    QCheckBox* link_cstick_invert_lr = nullptr;
    QCheckBox* link_circle_pad_invert_ud = nullptr;
    QCheckBox* link_circle_pad_invert_lr = nullptr;
    QCheckBox* link_dpad_invert_ud = nullptr;
    QCheckBox* link_dpad_invert_lr = nullptr;
    QCheckBox* link_abxy_invert_ud = nullptr;
    QCheckBox* link_abxy_invert_lr = nullptr;

    // Used for SDL input polling
    std::string guid;
    int port;
    std::string tpguid; // guid for touchpad
    int tpport;         // port for touchpad
    int tp;             // which touchpad
    std::unique_ptr<QTimer> timeout_timer;
    std::unique_ptr<QTimer> poll_timer;
    std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> device_pollers;

    /// This will be the the setting function when an input is awaiting configuration.
    std::optional<std::function<void(const Common::ParamPackage&)>> input_setter;

    // Coordinate system of the CemuhookUDP touch provider
    int min_x{};
    int min_y{};
    int max_x{};
    int max_y{};

    bool udp_test_in_progress{};

    std::vector<Settings::TouchFromButtonMap> touch_from_button_maps;
};
