// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cmath>

#include <QColorDialog>
#include <QEvent>
#include <QDoubleSpinBox>
#include <QtGlobal>
#include "citra_qt/configuration/configuration_shared.h"
#include "citra_qt/configuration/configure_layout.h"
#include "citra_qt/configuration/configure_layout_cycle.h"
#include "common/settings.h"
#include "ui_configure_layout.h"
#ifdef ENABLE_OPENGL
#include "video_core/renderer_opengl/post_processing_opengl.h"
#endif

ConfigureLayout::ConfigureLayout(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureLayout>()) {
    ui->setupUi(this);
    setupPctClipRadiusControls();

    SetupPerGameUI();
    SetConfiguration();

    ui->large_screen_proportion->setEnabled(
        (Settings::values.layout_option.GetValue() == Settings::LayoutOption::LargeScreen));
    connect(ui->layout_combobox,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int currentIndex) {
                ui->large_screen_proportion->setEnabled(
                    currentIndex == (uint)(Settings::LayoutOption::LargeScreen));
            });

    ui->small_screen_position_combobox->setEnabled(
        (Settings::values.layout_option.GetValue() == Settings::LayoutOption::LargeScreen));
    connect(ui->layout_combobox,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int currentIndex) {
                ui->small_screen_position_combobox->setEnabled(
                    currentIndex == (uint)(Settings::LayoutOption::LargeScreen));
            });

    // DiySC: Background blur fill — bottom checkbox overrides top (mutual exclusion)
    connect(ui->bg_blur_bottom_enable, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) {
            ui->bg_blur_top_enable->setChecked(false);
        }
    });
    // Disable wheel on bg blur spinboxes
    for (auto* sb : {ui->bg_blur_darken, ui->bg_blur_size, ui->bg_blur_scale}) {
        sb->installEventFilter(this);
    }

    ui->single_screen_layout_config_group->setEnabled(
        (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SingleScreen) ||
        (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows));
    connect(ui->layout_combobox,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int currentIndex) {
                ui->single_screen_layout_config_group->setEnabled(
                    (currentIndex == (uint)(Settings::LayoutOption::SingleScreen)) ||
                    (currentIndex == (uint)(Settings::LayoutOption::SeparateWindows)));
            });

    ui->custom_layout_group->setEnabled(
        (Settings::values.layout_option.GetValue() == Settings::LayoutOption::CustomLayout));
    connect(ui->layout_combobox,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int currentIndex) {
                ui->custom_layout_group->setEnabled(currentIndex ==
                                                    (uint)(Settings::LayoutOption::CustomLayout));
            });

    ui->custom_pct_layout_group->setEnabled(
        (Settings::values.layout_option.GetValue() == Settings::LayoutOption::CustomLayoutPercent));
    connect(ui->layout_combobox,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int currentIndex) {
                ui->custom_pct_layout_group->setEnabled(
                    currentIndex == (uint)(Settings::LayoutOption::CustomLayoutPercent));
            });

    // DiySC: Make 16:9 and 4:3 checkboxes mutually exclusive
    connect(ui->pct_internal_16x9, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) {
            ui->pct_internal_4x3->setChecked(false);
        }
    });
    connect(ui->pct_internal_4x3, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) {
            ui->pct_internal_16x9->setChecked(false);
        }
    });

    ui->screen_top_leftright_padding->setEnabled(Settings::values.screen_top_stretch.GetValue());

#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
    connect(ui->screen_top_stretch, static_cast<void (QCheckBox::*)(int)>(&QCheckBox::stateChanged),
            this,
            [this](bool checkState) { ui->screen_top_leftright_padding->setEnabled(checkState); });
    ui->screen_top_topbottom_padding->setEnabled(Settings::values.screen_top_stretch.GetValue());
    connect(ui->screen_top_stretch, static_cast<void (QCheckBox::*)(int)>(&QCheckBox::stateChanged),
            this,
            [this](bool checkState) { ui->screen_top_topbottom_padding->setEnabled(checkState); });
    ui->screen_bottom_leftright_padding->setEnabled(
        Settings::values.screen_bottom_topbottom_padding.GetValue());
    connect(
        ui->screen_bottom_stretch, static_cast<void (QCheckBox::*)(int)>(&QCheckBox::stateChanged),
        this,
        [this](bool checkState) { ui->screen_bottom_leftright_padding->setEnabled(checkState); });
    ui->screen_bottom_topbottom_padding->setEnabled(
        Settings::values.screen_bottom_topbottom_padding.GetValue());
    connect(
        ui->screen_bottom_stretch, static_cast<void (QCheckBox::*)(int)>(&QCheckBox::stateChanged),
        this,
        [this](bool checkState) { ui->screen_bottom_topbottom_padding->setEnabled(checkState); });
#else
    connect(ui->screen_top_stretch, &QCheckBox::checkStateChanged, this,
            [this](bool checkState) { ui->screen_top_leftright_padding->setEnabled(checkState); });
    ui->screen_top_topbottom_padding->setEnabled(Settings::values.screen_top_stretch.GetValue());
    connect(ui->screen_top_stretch, &QCheckBox::checkStateChanged, this,
            [this](bool checkState) { ui->screen_top_topbottom_padding->setEnabled(checkState); });
    ui->screen_bottom_leftright_padding->setEnabled(
        Settings::values.screen_bottom_topbottom_padding.GetValue());
    connect(
        ui->screen_bottom_stretch, &QCheckBox::checkStateChanged, this,
        [this](bool checkState) { ui->screen_bottom_leftright_padding->setEnabled(checkState); });
    ui->screen_bottom_topbottom_padding->setEnabled(
        Settings::values.screen_bottom_topbottom_padding.GetValue());
    connect(
        ui->screen_bottom_stretch, &QCheckBox::checkStateChanged, this,
        [this](bool checkState) { ui->screen_bottom_topbottom_padding->setEnabled(checkState); });
#endif

    connect(ui->bg_button, &QPushButton::clicked, this, [this] {
        ui->bg_button->setEnabled(false);
        const QColor new_bg_color = QColorDialog::getColor(bg_color);
        if (!new_bg_color.isValid()) {
            ui->bg_button->setEnabled(true);
            return;
        }
        bg_color = new_bg_color;
        QPixmap pixmap(ui->bg_button->size());
        pixmap.fill(bg_color);
        const QIcon color_icon(pixmap);
        ui->bg_button->setIcon(color_icon);
        ui->bg_button->setEnabled(true);
    });

    connect(ui->customize_layouts_to_cycle, &QPushButton::clicked, this, [this] {
        ui->customize_layouts_to_cycle->setEnabled(false);
        QDialog* layout_cycle_dialog = new ConfigureLayoutCycle(this);
        layout_cycle_dialog->exec();
        ui->customize_layouts_to_cycle->setEnabled(true);
    });

    // DiySC: Disable mouse wheel on all percentage SpinBoxes to prevent accidental changes
    const auto pctSpinBoxes = ui->custom_pct_layout_group->findChildren<QDoubleSpinBox*>();
    for (auto* sb : pctSpinBoxes) {
        sb->installEventFilter(this);
    }
}

ConfigureLayout::~ConfigureLayout() = default;

bool ConfigureLayout::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::Wheel) {
        return true; // eat wheel events to prevent accidental value changes
    }
    return QWidget::eventFilter(obj, event);
}

void ConfigureLayout::SetConfiguration() {

    if (!Settings::IsConfiguringGlobal()) {
        ConfigurationShared::SetPerGameSetting(ui->layout_combobox,
                                               &Settings::values.layout_option);
    } else {
        ui->layout_combobox->setCurrentIndex(
            static_cast<int>(Settings::values.layout_option.GetValue()));
    }

    ui->toggle_swap_screen->setChecked(Settings::values.swap_screen.GetValue());
    ui->toggle_upright_screen->setChecked(Settings::values.upright_screen.GetValue());
    ui->screen_gap->setValue(Settings::values.screen_gap.GetValue());
    ui->large_screen_proportion->setValue(Settings::values.large_screen_proportion.GetValue());
    ui->small_screen_position_combobox->setCurrentIndex(
        static_cast<int>(Settings::values.small_screen_position.GetValue()));
    ui->custom_top_x->setValue(Settings::values.custom_top_x.GetValue());
    ui->custom_top_y->setValue(Settings::values.custom_top_y.GetValue());
    ui->custom_top_width->setValue(Settings::values.custom_top_width.GetValue());
    ui->custom_top_height->setValue(Settings::values.custom_top_height.GetValue());
    ui->custom_bottom_x->setValue(Settings::values.custom_bottom_x.GetValue());
    ui->custom_bottom_y->setValue(Settings::values.custom_bottom_y.GetValue());
    ui->custom_bottom_width->setValue(Settings::values.custom_bottom_width.GetValue());
    ui->custom_bottom_height->setValue(Settings::values.custom_bottom_height.GetValue());
    ui->custom_second_layer_opacity->setValue(
        Settings::values.custom_second_layer_opacity.GetValue());

    // DiySC: Percentage layout settings
    ui->pct_internal_16x9->setChecked(Settings::values.custom_pct_internal_16x9.GetValue());
    ui->pct_internal_4x3->setChecked(Settings::values.custom_pct_internal_4x3.GetValue());
    ui->custom_pct_top_x->setValue(Settings::values.custom_pct_top_x.GetValue() / 100.0);
    ui->custom_pct_top_y->setValue(Settings::values.custom_pct_top_y.GetValue() / 100.0);
    ui->custom_pct_top_width->setValue(Settings::values.custom_pct_top_width.GetValue() / 100.0);
    ui->custom_pct_top_height->setValue(Settings::values.custom_pct_top_height.GetValue() / 100.0);
    ui->custom_pct_top_stretch_x->setValue(Settings::values.custom_pct_top_stretch_x.GetValue() / 100.0);
    ui->custom_pct_top_stretch_y->setValue(Settings::values.custom_pct_top_stretch_y.GetValue() / 100.0);
    ui->custom_pct_bottom_x->setValue(Settings::values.custom_pct_bottom_x.GetValue() / 100.0);
    ui->custom_pct_bottom_y->setValue(Settings::values.custom_pct_bottom_y.GetValue() / 100.0);
    ui->custom_pct_bottom_width->setValue(Settings::values.custom_pct_bottom_width.GetValue() / 100.0);
    ui->custom_pct_bottom_height->setValue(Settings::values.custom_pct_bottom_height.GetValue() / 100.0);
    ui->custom_pct_bottom_stretch_x->setValue(Settings::values.custom_pct_bottom_stretch_x.GetValue() / 100.0);
    ui->custom_pct_bottom_stretch_y->setValue(Settings::values.custom_pct_bottom_stretch_y.GetValue() / 100.0);
    ui->custom_pct_bottom_opacity->setValue(static_cast<double>(Settings::values.custom_pct_bottom_opacity.GetValue()));

    // DiySC: Set clip and radius values (0-10000 stored, 0-100 displayed)
    if (custom_pct_top_clip_x_) {
        custom_pct_top_clip_x_->setValue(Settings::values.custom_pct_top_clip_x.GetValue() / 100.0);
        custom_pct_top_clip_y_->setValue(Settings::values.custom_pct_top_clip_y.GetValue() / 100.0);
        custom_pct_top_radius_->setValue(Settings::values.custom_pct_top_radius.GetValue() / 100.0);
        custom_pct_bottom_clip_x_->setValue(Settings::values.custom_pct_bottom_clip_x.GetValue() / 100.0);
        custom_pct_bottom_clip_y_->setValue(Settings::values.custom_pct_bottom_clip_y.GetValue() / 100.0);
        custom_pct_bottom_radius_->setValue(Settings::values.custom_pct_bottom_radius.GetValue() / 100.0);
        custom_pct_top_edge_blur_->setValue(Settings::values.custom_pct_top_edge_blur.GetValue() / 100.0);
        custom_pct_bottom_edge_blur_->setValue(Settings::values.custom_pct_bottom_edge_blur.GetValue() / 100.0);
    }

    // DiySC: Background blur fill
    ui->bg_blur_top_enable->setChecked(Settings::values.custom_pct_bg_blur_top_enable.GetValue());
    ui->bg_blur_bottom_enable->setChecked(Settings::values.custom_pct_bg_blur_bottom_enable.GetValue());
    ui->bg_blur_darken->setValue(Settings::values.custom_pct_bg_blur_darken.GetValue() / 100.0);
    ui->bg_blur_size->setValue(Settings::values.custom_pct_bg_blur_size.GetValue() / 100.0);
    ui->bg_blur_scale->setValue(Settings::values.custom_pct_bg_blur_scale.GetValue() / 100.0);
    ui->bg_blur_quality->setCurrentIndex(Settings::values.custom_pct_bg_blur_quality.GetValue());

    ui->screen_top_stretch->setChecked(Settings::values.screen_top_stretch.GetValue());
    ui->screen_top_leftright_padding->setValue(
        Settings::values.screen_top_leftright_padding.GetValue());
    ui->screen_top_topbottom_padding->setValue(
        Settings::values.screen_top_topbottom_padding.GetValue());
    ui->screen_bottom_stretch->setChecked(Settings::values.screen_bottom_stretch.GetValue());
    ui->screen_bottom_leftright_padding->setValue(
        Settings::values.screen_bottom_leftright_padding.GetValue());
    ui->screen_bottom_topbottom_padding->setValue(
        Settings::values.screen_bottom_topbottom_padding.GetValue());
    bg_color =
        QColor::fromRgbF(Settings::values.bg_red.GetValue(), Settings::values.bg_green.GetValue(),
                         Settings::values.bg_blue.GetValue());
    QPixmap pixmap(ui->bg_button->size());
    pixmap.fill(bg_color);
    const QIcon color_icon(pixmap);
    ui->bg_button->setIcon(color_icon);
}

void ConfigureLayout::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureLayout::ApplyConfiguration() {
    Settings::values.large_screen_proportion = ui->large_screen_proportion->value();
    Settings::values.screen_gap = ui->screen_gap->value();
    Settings::values.small_screen_position = static_cast<Settings::SmallScreenPosition>(
        ui->small_screen_position_combobox->currentIndex());
    Settings::values.custom_top_x = ui->custom_top_x->value();
    Settings::values.custom_top_y = ui->custom_top_y->value();
    Settings::values.custom_top_width = ui->custom_top_width->value();
    Settings::values.custom_top_height = ui->custom_top_height->value();
    Settings::values.custom_bottom_x = ui->custom_bottom_x->value();
    Settings::values.custom_bottom_y = ui->custom_bottom_y->value();
    Settings::values.custom_bottom_width = ui->custom_bottom_width->value();
    Settings::values.custom_bottom_height = ui->custom_bottom_height->value();
    Settings::values.custom_second_layer_opacity = ui->custom_second_layer_opacity->value();

    // DiySC: Percentage layout settings
    Settings::values.custom_pct_internal_16x9 = ui->pct_internal_16x9->isChecked();
    Settings::values.custom_pct_internal_4x3 = ui->pct_internal_4x3->isChecked();
    Settings::values.custom_pct_top_x = static_cast<u16>(std::round(ui->custom_pct_top_x->value() * 100.0));
    Settings::values.custom_pct_top_y = static_cast<u16>(std::round(ui->custom_pct_top_y->value() * 100.0));
    Settings::values.custom_pct_top_width = static_cast<u16>(std::round(ui->custom_pct_top_width->value() * 100.0));
    Settings::values.custom_pct_top_height = static_cast<u16>(std::round(ui->custom_pct_top_height->value() * 100.0));
    Settings::values.custom_pct_top_stretch_x = static_cast<u16>(std::round(ui->custom_pct_top_stretch_x->value() * 100.0));
    Settings::values.custom_pct_top_stretch_y = static_cast<u16>(std::round(ui->custom_pct_top_stretch_y->value() * 100.0));
    Settings::values.custom_pct_bottom_x = static_cast<u16>(std::round(ui->custom_pct_bottom_x->value() * 100.0));
    Settings::values.custom_pct_bottom_y = static_cast<u16>(std::round(ui->custom_pct_bottom_y->value() * 100.0));
    Settings::values.custom_pct_bottom_width = static_cast<u16>(std::round(ui->custom_pct_bottom_width->value() * 100.0));
    Settings::values.custom_pct_bottom_height = static_cast<u16>(std::round(ui->custom_pct_bottom_height->value() * 100.0));
    Settings::values.custom_pct_bottom_stretch_x = static_cast<u16>(std::round(ui->custom_pct_bottom_stretch_x->value() * 100.0));
    Settings::values.custom_pct_bottom_stretch_y = static_cast<u16>(std::round(ui->custom_pct_bottom_stretch_y->value() * 100.0));
    Settings::values.custom_pct_bottom_opacity = static_cast<u16>(std::round(ui->custom_pct_bottom_opacity->value()));

    // DiySC: Write clip and radius values (0-100 displayed, 0-10000 stored)
    if (custom_pct_top_clip_x_) {
        Settings::values.custom_pct_top_clip_x = static_cast<u16>(std::round(custom_pct_top_clip_x_->value() * 100.0));
        Settings::values.custom_pct_top_clip_y = static_cast<u16>(std::round(custom_pct_top_clip_y_->value() * 100.0));
        Settings::values.custom_pct_top_radius = static_cast<u16>(std::round(custom_pct_top_radius_->value() * 100.0));
        Settings::values.custom_pct_bottom_clip_x = static_cast<u16>(std::round(custom_pct_bottom_clip_x_->value() * 100.0));
        Settings::values.custom_pct_bottom_clip_y = static_cast<u16>(std::round(custom_pct_bottom_clip_y_->value() * 100.0));
        Settings::values.custom_pct_bottom_radius = static_cast<u16>(std::round(custom_pct_bottom_radius_->value() * 100.0));
        Settings::values.custom_pct_top_edge_blur = static_cast<u16>(std::round(custom_pct_top_edge_blur_->value() * 100.0));
        Settings::values.custom_pct_bottom_edge_blur = static_cast<u16>(std::round(custom_pct_bottom_edge_blur_->value() * 100.0));
    }

    // DiySC: Background blur fill
    Settings::values.custom_pct_bg_blur_top_enable = ui->bg_blur_top_enable->isChecked();
    Settings::values.custom_pct_bg_blur_bottom_enable = ui->bg_blur_bottom_enable->isChecked();
    Settings::values.custom_pct_bg_blur_darken = static_cast<u16>(std::round(ui->bg_blur_darken->value() * 100.0));
    Settings::values.custom_pct_bg_blur_size = static_cast<u16>(std::round(ui->bg_blur_size->value() * 100.0));
    Settings::values.custom_pct_bg_blur_scale = static_cast<u16>(std::round(ui->bg_blur_scale->value() * 100.0));
    Settings::values.custom_pct_bg_blur_quality = static_cast<u8>(ui->bg_blur_quality->currentIndex());

    Settings::values.screen_top_stretch = ui->screen_top_stretch->checkState();
    Settings::values.screen_top_leftright_padding = ui->screen_top_leftright_padding->value();
    Settings::values.screen_top_topbottom_padding = ui->screen_top_topbottom_padding->value();
    Settings::values.screen_bottom_stretch = ui->screen_bottom_stretch->checkState();
    Settings::values.screen_bottom_leftright_padding = ui->screen_bottom_leftright_padding->value();
    Settings::values.screen_bottom_topbottom_padding = ui->screen_bottom_topbottom_padding->value();

    ConfigurationShared::ApplyPerGameSetting(&Settings::values.layout_option, ui->layout_combobox);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.swap_screen, ui->toggle_swap_screen,
                                             swap_screen);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.upright_screen,
                                             ui->toggle_upright_screen, upright_screen);

    Settings::values.bg_red = static_cast<float>(bg_color.redF());
    Settings::values.bg_green = static_cast<float>(bg_color.greenF());
    Settings::values.bg_blue = static_cast<float>(bg_color.blueF());
}

void ConfigureLayout::SetupPerGameUI() {
    // Block the global settings if a game is currently running that overrides them
    if (Settings::IsConfiguringGlobal()) {
        ui->toggle_swap_screen->setEnabled(Settings::values.swap_screen.UsingGlobal());
        ui->toggle_upright_screen->setEnabled(Settings::values.upright_screen.UsingGlobal());
        return;
    }

    ui->bg_color_group->setVisible(false);

    ConfigurationShared::SetColoredTristate(ui->toggle_swap_screen, Settings::values.swap_screen,
                                            swap_screen);
    ConfigurationShared::SetColoredTristate(ui->toggle_upright_screen,
                                            Settings::values.upright_screen, upright_screen);

    ConfigurationShared::SetColoredComboBox(
        ui->layout_combobox, ui->widget_layout,
        static_cast<int>(Settings::values.layout_option.GetValue(true)));
}

// DiySC: Programmatically create clip and radius controls in the percentage layout group
void ConfigureLayout::setupPctClipRadiusControls() {
    auto addPctRow = [](QGridLayout* grid, int row, const QString& label, QDoubleSpinBox*& spin,
                         double minVal, double maxVal, double defaultVal, double step = 1.0) {
        auto* lbl = new QLabel(label);
        spin = new QDoubleSpinBox();
        spin->setSuffix(QStringLiteral("%"));
        spin->setDecimals(2);
        spin->setMinimum(minVal);
        spin->setMaximum(maxVal);
        spin->setSingleStep(step);
        spin->setValue(defaultVal);
        grid->addWidget(lbl, row, 0);
        grid->addWidget(spin, row, 1);
    };

    // Top screen: add Clip X, Clip Y, Radius rows
    auto* topGrid = ui->gb_pct_top_screen->findChild<QGridLayout*>();
    if (topGrid) {
        int nextRow = topGrid->rowCount();
        addPctRow(topGrid, nextRow++, QStringLiteral("Clip H (%)"), custom_pct_top_clip_x_, 0, 100, 0);
        addPctRow(topGrid, nextRow++, QStringLiteral("Clip V (%)"), custom_pct_top_clip_y_, 0, 100, 0);
        addPctRow(topGrid, nextRow++, QStringLiteral("Corner Radius"), custom_pct_top_radius_, 0, 100, 0);
        addPctRow(topGrid, nextRow++, QStringLiteral("Edge Blur"), custom_pct_top_edge_blur_, 0, 50, 0);
    }

    // Bottom screen: add Clip X, Clip Y, Radius rows
    auto* bottomGrid = ui->gb_pct_bottom_screen->findChild<QGridLayout*>();
    if (bottomGrid) {
        int nextRow = bottomGrid->rowCount();
        addPctRow(bottomGrid, nextRow++, QStringLiteral("Clip H (%)"), custom_pct_bottom_clip_x_, 0, 100, 0);
        addPctRow(bottomGrid, nextRow++, QStringLiteral("Clip V (%)"), custom_pct_bottom_clip_y_, 0, 100, 0);
        addPctRow(bottomGrid, nextRow++, QStringLiteral("Corner Radius"), custom_pct_bottom_radius_, 0, 100, 0);
        addPctRow(bottomGrid, nextRow++, QStringLiteral("Edge Blur"), custom_pct_bottom_edge_blur_, 0, 50, 0);
    }
}
