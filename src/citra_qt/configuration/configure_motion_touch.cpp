// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <QCloseEvent>
#include <QBoxLayout>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include "citra_qt/configuration/configure_motion_touch.h"
#include "citra_qt/configuration/configure_touch_from_button.h"
#include "common/logging/log.h"
#include "input_common/main.h"
#include "ui_configure_motion_touch.h"

CalibrationConfigurationDialog::CalibrationConfigurationDialog(QWidget* parent,
                                                               const std::string& host, u16 port,
                                                               u8 pad_index, u16 client_id)
    : QDialog(parent) {
    layout = new QVBoxLayout;
    status_label = new QLabel(tr("Communicating with the server..."));
    cancel_button = new QPushButton(tr("Cancel"));
    connect(cancel_button, &QPushButton::clicked, this, [this] {
        if (!completed) {
            job->Stop();
        }
        accept();
    });
    layout->addWidget(status_label);
    layout->addWidget(cancel_button);
    setLayout(layout);

    using namespace InputCommon::CemuhookUDP;
    job = std::make_unique<CalibrationConfigurationJob>(
        host, port, pad_index, client_id,
        [this](CalibrationConfigurationJob::Status status) {
            QString text;
            switch (status) {
            case CalibrationConfigurationJob::Status::Ready:
                text = tr("Touch the top left corner <br>of your touchpad.");
                break;
            case CalibrationConfigurationJob::Status::Stage1Completed:
                text = tr("Now touch the bottom right corner <br>of your touchpad.");
                break;
            case CalibrationConfigurationJob::Status::Completed:
                text = tr("Configuration completed!");
                break;
            default:
                LOG_ERROR(Frontend, "Unknown calibration status {}", status);
                break;
            }
            QMetaObject::invokeMethod(this, "UpdateLabelText", Q_ARG(QString, text));
            if (status == CalibrationConfigurationJob::Status::Completed) {
                QMetaObject::invokeMethod(this, "UpdateButtonText", Q_ARG(QString, tr("OK")));
            }
        },
        [this](u16 min_x_, u16 min_y_, u16 max_x_, u16 max_y_) {
            completed = true;
            min_x = min_x_;
            min_y = min_y_;
            max_x = max_x_;
            max_y = max_y_;
        });
}

CalibrationConfigurationDialog::~CalibrationConfigurationDialog() = default;

void CalibrationConfigurationDialog::UpdateLabelText(const QString& text) {
    status_label->setText(text);
}

void CalibrationConfigurationDialog::UpdateButtonText(const QString& text) {
    cancel_button->setText(text);
}

constexpr std::array<std::pair<const char*, const char*>, 3> MotionProviders = {{
    {"motion_emu", QT_TRANSLATE_NOOP("ConfigureMotionTouch", "Mouse")},
    {"cemuhookudp", QT_TRANSLATE_NOOP("ConfigureMotionTouch", "CemuhookUDP")},
    {"sdl", QT_TRANSLATE_NOOP("ConfigureMotionTouch", "SDL")},
}};

constexpr std::array<std::pair<const char*, const char*>, 6> MotionEmuModes = {{
    {"rate_hold", QT_TRANSLATE_NOOP("ConfigureMotionTouch", "Right Click (Gyro)")},
    {"rate_continuous", QT_TRANSLATE_NOOP("ConfigureMotionTouch", "Always On (Gyro)")},
    {"tilt_continuous", QT_TRANSLATE_NOOP("ConfigureMotionTouch", "Always On (Tilt New)")},
    {"tilt_hold", QT_TRANSLATE_NOOP("ConfigureMotionTouch", "Right Click (Tilt New)")},
}};

constexpr std::array<std::pair<const char*, const char*>, 2> TouchProviders = {{
    {"emu_window", QT_TRANSLATE_NOOP("ConfigureMotionTouch", "Emulator Window")},
    {"cemuhookudp", QT_TRANSLATE_NOOP("ConfigureMotionTouch", "CemuhookUDP")},
}};

ConfigureMotionTouch::ConfigureMotionTouch(QWidget* parent)
    : QDialog(parent), ui(std::make_unique<Ui::ConfigureMotionTouch>()),
      timeout_timer(std::make_unique<QTimer>()), poll_timer(std::make_unique<QTimer>()) {
    ui->setupUi(this);

    // Create Max Tilt Angle controls (not in .ui file)
    motion_tilt_max_label = new QLabel(tr("Max Tilt Angle:"), this);
    motion_tilt_max_angle = new QDoubleSpinBox(this);
    motion_tilt_max_angle->setRange(1.0, 180.0);
    motion_tilt_max_angle->setDecimals(1);
    motion_tilt_max_angle->setSingleStep(5.0);
    motion_tilt_max_angle->setSuffix(QString::fromUtf8("°"));
    motion_tilt_max_angle->setValue(90.0);

    // Insert new row after sensitivity row (find layout and add before update_period row)
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (group_layout) {
            // Find the index of the update_period row and insert before it
            int update_idx = group_layout->indexOf(ui->motion_update_label);
            if (update_idx < 0) {
                update_idx = group_layout->count();
            }
            auto* row_layout = new QHBoxLayout();
            row_layout->addWidget(motion_tilt_max_label);
            row_layout->addWidget(motion_tilt_max_angle);
            auto* row_widget = new QWidget(this);
            row_widget->setLayout(row_layout);
            group_layout->addWidget(row_widget);
        }
    }

    // Clamp pitch rotation to 180° checkbox
    motion_clamp_pitch = new QCheckBox(tr("Limit Y-axis rotation to 180°"), this);
    motion_clamp_pitch->setToolTip(tr("Prevents the 3DS view from flipping upside down by limiting pitch rotation to ±90°"));
    motion_clamp_pitch->setChecked(true);
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (group_layout) {
            auto* row_layout = new QHBoxLayout();
            row_layout->addWidget(motion_clamp_pitch);
            auto* row_widget = new QWidget(this);
            row_widget->setLayout(row_layout);
            group_layout->addWidget(row_widget);
        }
    }

    // Auto Y-axis tilt checkbox (default on)
    motion_auto_tilt_y = new QCheckBox(tr("Auto Y-axis tilt"), this);
    motion_auto_tilt_y->setToolTip(tr("Automatically tracks vertical device tilt from gyro/mouse input, preventing view from snapping back to flat"));
    motion_auto_tilt_y->setChecked(true);
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (group_layout) {
            auto* row_layout = new QHBoxLayout();
            row_layout->addWidget(motion_auto_tilt_y);
            auto* row_widget = new QWidget(this);
            row_widget->setLayout(row_layout);
            group_layout->addWidget(row_widget);
        }
    }

    // Auto Y-axis tilt (inverted) checkbox
    motion_auto_tilt_y_invert = new QCheckBox(tr("Auto Y-axis tilt (Inverted)"), this);
    motion_auto_tilt_y_invert->setToolTip(tr("Inverts the auto Y-axis tilt direction. Overrides normal auto tilt when checked"));
    motion_auto_tilt_y_invert->setChecked(false);
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (group_layout) {
            auto* row_layout = new QHBoxLayout();
            row_layout->addWidget(motion_auto_tilt_y_invert);
            auto* row_widget = new QWidget(this);
            row_widget->setLayout(row_layout);
            group_layout->addWidget(row_widget);
        }
    }

    // Auto Y-tilt return speed
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (group_layout) {
            auto* row_layout = new QHBoxLayout();
            row_layout->addWidget(new QLabel(tr("Auto Y-tilt return speed:"), this));
            motion_auto_tilt_y_return_speed = new QDoubleSpinBox(this);
            motion_auto_tilt_y_return_speed->setRange(0.0, 100.0);
            motion_auto_tilt_y_return_speed->setSingleStep(1.0);
            motion_auto_tilt_y_return_speed->setDecimals(1);
            motion_auto_tilt_y_return_speed->setSuffix(QStringLiteral(" °/s"));
            motion_auto_tilt_y_return_speed->setValue(0.0);
            motion_auto_tilt_y_return_speed->setToolTip(
                tr("When no gyro input is active, gradually returns Y-axis tilt to Default Tilt at this speed.\n"
                   "0 = disabled. Higher values snap back faster."));
            row_layout->addWidget(motion_auto_tilt_y_return_speed);
            auto* row_widget = new QWidget(this);
            row_widget->setLayout(row_layout);
            group_layout->addWidget(row_widget);
        }
    }

    // Auto Y-tilt max accumulation angle
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (group_layout) {
            auto* row_layout = new QHBoxLayout();
            row_layout->addWidget(new QLabel(tr("Auto Y-tilt max angle:"), this));
            motion_auto_tilt_y_max_angle = new QSpinBox(this);
            motion_auto_tilt_y_max_angle->setRange(1, 180);
            motion_auto_tilt_y_max_angle->setValue(180);
            motion_auto_tilt_y_max_angle->setSuffix(QStringLiteral("°"));
            motion_auto_tilt_y_max_angle->setToolTip(
                tr("Limits how far Auto Y-tilt can deviate from Default Tilt.\n"
                   "180° = unlimited. 25° = ±25° range around Default Tilt."));
            row_layout->addWidget(motion_auto_tilt_y_max_angle);
            auto* row_widget = new QWidget(this);
            row_widget->setLayout(row_layout);
            group_layout->addWidget(row_widget);
        }
    }

    // Prevent Auto Y-tilt flip (keep tilt in [0°,180°] around default_tilt)
    motion_auto_tilt_y_prevent_flip = new QCheckBox(tr("Prevent Auto Y-tilt flip"), this);
    motion_auto_tilt_y_prevent_flip->setToolTip(
        tr("Prevents Y-axis tilt from crossing vertical (0°/180°).\n"
           "Based on Default Tilt, e.g. at 90°: limits range to [0°,180°];\n"
           "at 45°: allows [0°,180°] relative to 45° tilt."));
    motion_auto_tilt_y_prevent_flip->setChecked(true);
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (group_layout) {
            auto* row_layout = new QHBoxLayout();
            row_layout->addWidget(motion_auto_tilt_y_prevent_flip);
            auto* row_widget = new QWidget(this);
            row_widget->setLayout(row_layout);
            group_layout->addWidget(row_widget);
        }
    }

    // [BETA] Auto X-axis tilt checkbox (default off)
    motion_auto_tilt_x = new QCheckBox(tr("[BETA] Auto X-axis tilt"), this);
    motion_auto_tilt_x->setToolTip(tr("BETA: Smoothly tilts the device left/right based on horizontal mouse movement. Returns to neutral when mouse stops"));
    motion_auto_tilt_x->setChecked(false);
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (group_layout) {
            auto* row_layout = new QHBoxLayout();
            row_layout->addWidget(motion_auto_tilt_x);
            auto* row_widget = new QWidget(this);
            row_widget->setLayout(row_layout);
            group_layout->addWidget(row_widget);
        }
    }

    // Auto tilt tracking speed
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (group_layout) {
            auto* row_layout = new QHBoxLayout();
            row_layout->addWidget(new QLabel(tr("Auto X-tilt speed:"), this));
            motion_auto_tilt_speed = new QDoubleSpinBox(this);
            motion_auto_tilt_speed->setRange(0.1, 10.0);
            motion_auto_tilt_speed->setSingleStep(0.1);
            motion_auto_tilt_speed->setDecimals(1);
            motion_auto_tilt_speed->setValue(1.0);
            motion_auto_tilt_speed->setToolTip(tr("Scales [BETA] Auto X-tilt roll tracking speed. Higher = faster roll response to horizontal mouse movement"));
            row_layout->addWidget(motion_auto_tilt_speed);
            auto* row_widget = new QWidget(this);
            row_widget->setLayout(row_layout);
            group_layout->addWidget(row_widget);
        }
    }

    // ── Controller-to-Mouse Linking (only for Mouse motion provider) ──────
    {
        auto* group_layout = ui->motion_group_box->layout();
        if (!group_layout)
            return;

        link_group = new QWidget(this);
        auto* vbox = new QVBoxLayout(link_group);
        vbox->setContentsMargins(0, 4, 0, 0);
        vbox->setSpacing(2);

        auto* header = new QLabel(tr("Controller Link (emulate mouse with gamepad):"), link_group);
        header->setStyleSheet(QStringLiteral("font-weight: bold;"));
        vbox->addWidget(header);

        link_cstick = new QCheckBox(tr("Link 3DS Right Stick (C-Stick)"), link_group);
        link_cstick->setToolTip(tr("Right analog stick movement generates virtual mouse delta"));
        link_cstick->setChecked(true);
        vbox->addWidget(link_cstick);

        link_circle_pad = new QCheckBox(tr("Link 3DS Left Stick (Circle Pad)"), link_group);
        link_circle_pad->setToolTip(tr("Left analog stick movement generates virtual mouse delta"));
        vbox->addWidget(link_circle_pad);

        link_dpad = new QCheckBox(tr("Link 3DS D-Pad"), link_group);
        link_dpad->setToolTip(tr("D-Pad presses generate virtual mouse delta"));
        vbox->addWidget(link_dpad);

        link_abxy = new QCheckBox(tr("Link ABXY 3DS Layout"), link_group);
        link_abxy->setToolTip(tr("ABXY buttons generate virtual mouse delta:\n"
                                 "Top button (Y/N) = Up, Bottom (A/S) = Down,\n"
                                 "Left (X/W) = Left, Right (B/E) = Right"));
        vbox->addWidget(link_abxy);

        auto* speed_row = new QHBoxLayout();
        speed_row->addWidget(new QLabel(tr("Link Speed:"), link_group));
        link_speed = new QDoubleSpinBox(link_group);
        link_speed->setRange(0.1, 10.0);
        link_speed->setSingleStep(0.1);
        link_speed->setDecimals(1);
        link_speed->setValue(1.0);
        link_speed->setToolTip(tr("Multiplier for controller-link virtual mouse delta.\n"
                                  "Higher = faster aiming with controller"));
        speed_row->addWidget(link_speed);
        vbox->addLayout(speed_row);

        group_layout->addWidget(link_group);

        // Show/hide link controls based on motion provider selection
        connect(ui->motion_provider,
                qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) {
                    QString provider = ui->motion_provider->currentData().toString();
                    if (link_group)
                        link_group->setVisible(provider == QStringLiteral("motion_emu"));
                });
    }

    for (const auto& [provider, name] : MotionProviders) {
        ui->motion_provider->addItem(tr(name), QString::fromUtf8(provider));
    }
    for (const auto& [mode_id, mode_name] : MotionEmuModes) {
        ui->motion_mode->addItem(tr(mode_name), QString::fromUtf8(mode_id));
    }
    for (const auto& [provider, name] : TouchProviders) {
        ui->touch_provider->addItem(tr(name), QString::fromUtf8(provider));
    }

    ui->udp_learn_more->setOpenExternalLinks(true);
    ui->udp_learn_more->setText(
        tr("<a "
           "href='https://web.archive.org/web/20240301211230/https://citra-emu.org/wiki/"
           "using-a-controller-or-android-phone-for-motion-or-touch-input'><span "
           "style=\"text-decoration: underline; color:#039be5;\">Learn More</span></a>"));

    timeout_timer->setSingleShot(true);
    connect(timeout_timer.get(), &QTimer::timeout, this, [this]() { SetPollingResult({}, true); });

    connect(poll_timer.get(), &QTimer::timeout, this, [this]() {
        Common::ParamPackage params;
        for (auto& poller : device_pollers) {
            params = poller->GetNextInput();
            // We want all the input systems to be in a "polling" state, but we only care about the
            // input from SDL.
            if (params.Has("engine") && params.Get("engine", "") == "sdl") {
                SetPollingResult(params, false);
                return;
            }
        }
    });

    SetConfiguration();
    UpdateUiDisplay();
    ConnectEvents();
}

ConfigureMotionTouch::~ConfigureMotionTouch() = default;

void ConfigureMotionTouch::SetConfiguration() {
    const Common::ParamPackage motion_param(Settings::values.current_input_profile.motion_device);
    const Common::ParamPackage touch_param(Settings::values.current_input_profile.touch_device);
    const std::string motion_engine = motion_param.Get("engine", "motion_emu");
    const std::string touch_engine = touch_param.Get("engine", "emu_window");

    ui->motion_provider->setCurrentIndex(
        ui->motion_provider->findData(QString::fromStdString(motion_engine)));
    const std::string motion_mode = motion_param.Get("mode", "absolute");
    int mode_idx = ui->motion_mode->findData(QString::fromStdString(motion_mode));
    ui->motion_mode->setCurrentIndex(mode_idx >= 0 ? mode_idx : 0);
    ui->motion_default_tilt->setValue(motion_param.Get("default_tilt", 90));
    ui->motion_invert_pitch->setChecked(motion_param.Get("invert_pitch", false));
    ui->motion_invert_yaw->setChecked(motion_param.Get("invert_yaw", false));
    ui->touch_provider->setCurrentIndex(
        ui->touch_provider->findData(QString::fromStdString(touch_engine)));
    ui->touch_from_button_checkbox->setChecked(
        Settings::values.current_input_profile.use_touch_from_button);
    ui->touchpad_checkbox->setChecked(Settings::values.current_input_profile.use_touchpad);
    touch_from_button_maps = Settings::values.touch_from_button_maps;
    for (const auto& touch_map : touch_from_button_maps) {
        ui->touch_from_button_map->addItem(QString::fromStdString(touch_map.name));
    }
    ui->touch_from_button_map->setCurrentIndex(
        Settings::values.current_input_profile.touch_from_button_map_index);
    ui->motion_sensitivity->setValue(motion_param.Get("sensitivity", 0.075f));
    motion_tilt_max_angle->setValue(motion_param.Get("tilt_max_angle", 90.0f));
    ui->motion_update_period->setValue(motion_param.Get("update_period", 20));
    ui->motion_per_frame->setChecked(motion_param.Get("per_frame", true));
    motion_clamp_pitch->setChecked(motion_param.Get("clamp_pitch_180", true));
    motion_auto_tilt_y->setChecked(motion_param.Get("auto_tilt_y", true));
    motion_auto_tilt_y_invert->setChecked(motion_param.Get("auto_tilt_y_invert", false));
    motion_auto_tilt_x->setChecked(motion_param.Get("auto_tilt_x", false));
    motion_auto_tilt_speed->setValue(static_cast<double>(motion_param.Get("auto_tilt_speed", 1.0f)));
    motion_auto_tilt_y_return_speed->setValue(
        static_cast<double>(motion_param.Get("auto_tilt_y_return_speed", 0.0f)));
    motion_auto_tilt_y_max_angle->setValue(motion_param.Get("auto_tilt_y_max_angle", 180));
    motion_auto_tilt_y_prevent_flip->setChecked(
        motion_param.Get("auto_tilt_y_prevent_flip", true));

    // Controller-to-mouse linking
    link_cstick->setChecked(motion_param.Get("link_cstick", true));
    link_circle_pad->setChecked(motion_param.Get("link_circle_pad", false));
    link_dpad->setChecked(motion_param.Get("link_dpad", false));
    link_abxy->setChecked(motion_param.Get("link_abxy", false));
    link_speed->setValue(static_cast<double>(motion_param.Get("link_speed", 1.0f)));

    guid = motion_param.Get("guid", "0");
    port = motion_param.Get("port", 0);

    min_x = touch_param.Get("min_x", 100);
    min_y = touch_param.Get("min_y", 50);
    max_x = touch_param.Get("max_x", 1800);
    max_y = touch_param.Get("max_y", 850);

    ui->udp_server->setText(
        QString::fromStdString(Settings::values.current_input_profile.udp_input_address));
    ui->udp_port->setText(QString::number(Settings::values.current_input_profile.udp_input_port));
    ui->udp_pad_index->setCurrentIndex(Settings::values.current_input_profile.udp_pad_index);
}

void ConfigureMotionTouch::UpdateUiDisplay() {
    const std::string motion_engine = ui->motion_provider->currentData().toString().toStdString();
    const std::string touch_engine = ui->touch_provider->currentData().toString().toStdString();
    ui->touchpad_config_btn->setEnabled(ui->touchpad_checkbox->isChecked());
    // Mouse (motion_emu) specific controls
    bool is_motion_emu = (motion_engine == "motion_emu");
    ui->motion_mode_label->setVisible(is_motion_emu);
    ui->motion_mode->setVisible(is_motion_emu);
    ui->motion_tilt_label->setVisible(is_motion_emu);
    ui->motion_default_tilt->setVisible(is_motion_emu);
    ui->motion_invert_label->setVisible(is_motion_emu);
    ui->motion_invert_pitch->setVisible(is_motion_emu);
    ui->motion_invert_yaw->setVisible(is_motion_emu);
    ui->motion_sensitivity_label->setVisible(is_motion_emu);
    ui->motion_sensitivity->setVisible(is_motion_emu);
    motion_tilt_max_label->setVisible(is_motion_emu);
    motion_tilt_max_angle->setVisible(is_motion_emu);
    ui->motion_update_label->setVisible(is_motion_emu);
    ui->motion_update_period->setVisible(is_motion_emu);
    ui->motion_per_frame->setVisible(is_motion_emu);
    motion_clamp_pitch->setVisible(is_motion_emu);
    motion_auto_tilt_y->setVisible(is_motion_emu);
    motion_auto_tilt_y_invert->setVisible(is_motion_emu);
    motion_auto_tilt_x->setVisible(is_motion_emu);
    motion_auto_tilt_speed->setVisible(is_motion_emu);
    motion_auto_tilt_y_return_speed->setVisible(is_motion_emu);
    motion_auto_tilt_y_max_angle->setVisible(is_motion_emu);
    motion_auto_tilt_y_prevent_flip->setVisible(is_motion_emu);
    if (link_group)
        link_group->setVisible(is_motion_emu);
    // Disable update_period spinbox when per-frame sync is checked
    ui->motion_update_period->setEnabled(!ui->motion_per_frame->isChecked());

    if (motion_engine == "sdl") {
        ui->motion_controller_label->setVisible(true);
        ui->motion_controller_button->setVisible(true);
    } else {
        ui->motion_controller_label->setVisible(false);
        ui->motion_controller_button->setVisible(false);
    }

    if (touch_engine == "cemuhookudp") {
        ui->touch_calibration->setVisible(true);
        ui->touch_calibration_config->setVisible(true);
        ui->touch_calibration_label->setVisible(true);
        ui->touch_calibration->setText(
            QStringLiteral("(%1, %2) - (%3, %4)").arg(min_x).arg(min_y).arg(max_x).arg(max_y));
    } else {
        ui->touch_calibration->setVisible(false);
        ui->touch_calibration_config->setVisible(false);
        ui->touch_calibration_label->setVisible(false);
    }

    if (motion_engine == "cemuhookudp" || touch_engine == "cemuhookudp") {
        ui->udp_config_group_box->setVisible(true);
    } else {
        ui->udp_config_group_box->setVisible(false);
    }
}

void ConfigureMotionTouch::ConnectEvents() {
    connect(ui->motion_provider, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this]([[maybe_unused]] int index) { UpdateUiDisplay(); });
    connect(ui->touch_provider, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this]([[maybe_unused]] int index) { UpdateUiDisplay(); });
    connect(ui->motion_controller_button, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::information(this, tr("Information"),
                                     tr("After pressing OK, press a button on the controller whose "
                                        "motion you want to track."),
                                     QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {
            ui->motion_controller_button->setText(tr("[press button]"));
            ui->motion_controller_button->setFocus();

            input_setter = [this](const Common::ParamPackage& params) {
                guid = params.Get("guid", "0");
                port = params.Get("port", 0);
            };

            device_pollers =
                InputCommon::Polling::GetPollers(InputCommon::Polling::DeviceType::Button);

            for (auto& poller : device_pollers) {
                poller->Start();
            }

            timeout_timer->start(5000); // Cancel after 5 seconds
            poll_timer->start(200);     // Check for new inputs every 200ms
        }
    });
#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
    connect(ui->touchpad_checkbox, &QCheckBox::stateChanged, this, [this]() { UpdateUiDisplay(); });
#else
    connect(ui->touchpad_checkbox, &QCheckBox::checkStateChanged, this,
            [this]() { UpdateUiDisplay(); });
#endif

    connect(ui->motion_per_frame, &QCheckBox::toggled, this, [this]() { UpdateUiDisplay(); });

    connect(ui->touchpad_config_btn, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::information(this, tr("Information"),
                                     tr("After pressing OK, tap the touchpad on the controller "
                                        "you want to track."),
                                     QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {
            ui->touchpad_config_btn->setText(tr("[press touchpad]"));
            ui->touchpad_config_btn->setFocus();

            input_setter = [this](const Common::ParamPackage& params) {
                tpguid = params.Get("guid", "0");
                tpport = params.Get("port", 0);
                tp = params.Get("touchpad", 0);
            };

            device_pollers =
                InputCommon::Polling::GetPollers(InputCommon::Polling::DeviceType::Touchpad);

            for (auto& poller : device_pollers) {
                poller->Start();
            }

            timeout_timer->start(5000); // Cancel after 5 seconds
            poll_timer->start(200);     // Check for new inputs every 200ms
        }
    });
    connect(ui->udp_test, &QPushButton::clicked, this, &ConfigureMotionTouch::OnCemuhookUDPTest);
    connect(ui->touch_calibration_config, &QPushButton::clicked, this,
            &ConfigureMotionTouch::OnConfigureTouchCalibration);
    connect(ui->touch_from_button_config_btn, &QPushButton::clicked, this,
            &ConfigureMotionTouch::OnConfigureTouchFromButton);
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this,
            &ConfigureMotionTouch::ApplyConfiguration);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, [this] {
        if (CanCloseDialog()) {
            reject();
        }
    });
}

void ConfigureMotionTouch::SetPollingResult(const Common::ParamPackage& params, bool abort) {
    timeout_timer->stop();
    poll_timer->stop();
    for (auto& poller : device_pollers) {
        poller->Stop();
    }

    if (!abort && input_setter) {
        (*input_setter)(params);
    }
    ui->touchpad_config_btn->setText(tr("Configure"));
    ui->motion_controller_button->setText(tr("Configure"));
    input_setter.reset();
}

void ConfigureMotionTouch::OnCemuhookUDPTest() {
    ui->udp_test->setEnabled(false);
    ui->udp_test->setText(tr("Testing"));
    udp_test_in_progress = true;
    InputCommon::CemuhookUDP::TestCommunication(
        ui->udp_server->text().toStdString(), static_cast<u16>(ui->udp_port->text().toInt()),
        static_cast<u8>(ui->udp_pad_index->currentIndex()), 24872,
        [this] {
            LOG_INFO(Frontend, "UDP input test success");
            QMetaObject::invokeMethod(this, "ShowUDPTestResult", Q_ARG(bool, true));
        },
        [this] {
            LOG_ERROR(Frontend, "UDP input test failed");
            QMetaObject::invokeMethod(this, "ShowUDPTestResult", Q_ARG(bool, false));
        });
}

void ConfigureMotionTouch::OnConfigureTouchCalibration() {
    ui->touch_calibration_config->setEnabled(false);
    ui->touch_calibration_config->setText(tr("Configuring"));
    CalibrationConfigurationDialog dialog(
        this, ui->udp_server->text().toStdString(), static_cast<u16>(ui->udp_port->text().toUInt()),
        static_cast<u8>(ui->udp_pad_index->currentIndex()), 24872);
    dialog.exec();
    if (dialog.completed) {
        min_x = dialog.min_x;
        min_y = dialog.min_y;
        max_x = dialog.max_x;
        max_y = dialog.max_y;
        LOG_INFO(Frontend,
                 "UDP touchpad calibration config success: min_x={}, min_y={}, max_x={}, max_y={}",
                 min_x, min_y, max_x, max_y);
        UpdateUiDisplay();
        LOG_ERROR(Frontend, "UDP touchpad calibration config failed");
    }
    ui->touch_calibration_config->setEnabled(true);
    ui->touch_calibration_config->setText(tr("Configure"));
}

void ConfigureMotionTouch::closeEvent(QCloseEvent* event) {
    if (CanCloseDialog()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void ConfigureMotionTouch::ShowUDPTestResult(bool result) {
    udp_test_in_progress = false;
    if (result) {
        QMessageBox::information(this, tr("Test Successful"),
                                 tr("Successfully received data from the server."));
    } else {
        QMessageBox::warning(this, tr("Test Failed"),
                             tr("Could not receive valid data from the server.<br>Please verify "
                                "that the server is set up correctly and "
                                "the address and port are correct."));
    }
    ui->udp_test->setEnabled(true);
    ui->udp_test->setText(tr("Test"));
}

void ConfigureMotionTouch::OnConfigureTouchFromButton() {
    ConfigureTouchFromButton dialog{this, touch_from_button_maps,
                                    ui->touch_from_button_map->currentIndex()};
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    touch_from_button_maps = dialog.GetMaps();

    while (ui->touch_from_button_map->count() > 0) {
        ui->touch_from_button_map->removeItem(0);
    }
    for (const auto& touch_map : touch_from_button_maps) {
        ui->touch_from_button_map->addItem(QString::fromStdString(touch_map.name));
    }
    ui->touch_from_button_map->setCurrentIndex(dialog.GetSelectedIndex());
}

bool ConfigureMotionTouch::CanCloseDialog() {
    if (udp_test_in_progress) {
        QMessageBox::warning(this, tr("Azahar"),
                             tr("UDP Test or calibration configuration is in progress.<br>Please "
                                "wait for them to finish."));
        return false;
    }
    return true;
}

void ConfigureMotionTouch::ApplyConfiguration() {
    if (!CanCloseDialog()) {
        return;
    }

    std::string motion_engine = ui->motion_provider->currentData().toString().toStdString();
    std::string touch_engine = ui->touch_provider->currentData().toString().toStdString();

    Common::ParamPackage motion_param{};
    motion_param.Set("engine", motion_engine);
    if (motion_engine == "motion_emu") {
        motion_param.Set("sensitivity", static_cast<float>(ui->motion_sensitivity->value()));
        motion_param.Set("tilt_max_angle", static_cast<float>(motion_tilt_max_angle->value()));
        std::string mode = ui->motion_mode->currentData().toString().toStdString();
        motion_param.Set("mode", mode);
        motion_param.Set("default_tilt", ui->motion_default_tilt->value());
        motion_param.Set("invert_pitch", ui->motion_invert_pitch->isChecked());
        motion_param.Set("invert_yaw", ui->motion_invert_yaw->isChecked());
        motion_param.Set("update_period", ui->motion_update_period->value());
        motion_param.Set("per_frame", ui->motion_per_frame->isChecked());
        motion_param.Set("clamp_pitch_180", motion_clamp_pitch->isChecked());
        motion_param.Set("auto_tilt_y", motion_auto_tilt_y->isChecked());
        motion_param.Set("auto_tilt_y_invert", motion_auto_tilt_y_invert->isChecked());
        motion_param.Set("auto_tilt_x", motion_auto_tilt_x->isChecked());
        motion_param.Set("auto_tilt_speed", static_cast<float>(motion_auto_tilt_speed->value()));
        motion_param.Set("auto_tilt_y_return_speed",
                         static_cast<float>(motion_auto_tilt_y_return_speed->value()));
        motion_param.Set("auto_tilt_y_max_angle", motion_auto_tilt_y_max_angle->value());
        motion_param.Set("auto_tilt_y_prevent_flip", motion_auto_tilt_y_prevent_flip->isChecked());

        // Controller-to-mouse linking
        motion_param.Set("link_cstick", link_cstick->isChecked());
        motion_param.Set("link_circle_pad", link_circle_pad->isChecked());
        motion_param.Set("link_dpad", link_dpad->isChecked());
        motion_param.Set("link_abxy", link_abxy->isChecked());
        motion_param.Set("link_speed", static_cast<float>(link_speed->value()));
    } else if (motion_engine == "sdl") {
        motion_param.Set("guid", guid);
        motion_param.Set("port", port);
    }

    Common::ParamPackage touch_param{};
    touch_param.Set("engine", touch_engine);
    if (touch_engine == "cemuhookudp") {
        touch_param.Set("min_x", min_x);
        touch_param.Set("min_y", min_y);
        touch_param.Set("max_x", max_x);
        touch_param.Set("max_y", max_y);
    }

    Common::ParamPackage touchpad_param{};
    if (ui->touchpad_checkbox->isChecked()) {
        touchpad_param.Set("engine", "sdl");
        touchpad_param.Set("guid", tpguid);
        touchpad_param.Set("port", tpport);
        touchpad_param.Set("touchpad", tp);
    }
    Settings::values.current_input_profile.motion_device = motion_param.Serialize();
    Settings::values.current_input_profile.touch_device = touch_param.Serialize();
    Settings::values.current_input_profile.use_touch_from_button =
        ui->touch_from_button_checkbox->isChecked();
    Settings::values.current_input_profile.touch_from_button_map_index =
        ui->touch_from_button_map->currentIndex();
    Settings::values.current_input_profile.use_touchpad = ui->touchpad_checkbox->isChecked();
    Settings::values.current_input_profile.controller_touch_device = touchpad_param.Serialize();
    Settings::values.touch_from_button_maps = touch_from_button_maps;
    Settings::values.current_input_profile.udp_input_address = ui->udp_server->text().toStdString();
    Settings::values.current_input_profile.udp_input_port =
        static_cast<u16>(ui->udp_port->text().toInt());
    Settings::values.current_input_profile.udp_pad_index =
        static_cast<u8>(ui->udp_pad_index->currentIndex());
    Settings::SaveProfile(Settings::values.current_input_profile_index);
    InputCommon::ReloadInputDevices();

    accept();
}
