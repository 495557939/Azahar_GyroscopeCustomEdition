// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <QWidget>
#include "common/common_types.h"

class QDoubleSpinBox;

namespace Settings {
enum class StereoRenderOption : u32;
}

namespace ConfigurationShared {
enum class CheckState;
}

namespace Ui {
class ConfigureLayout;
}

class ConfigureLayout : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureLayout(QWidget* parent = nullptr);
    ~ConfigureLayout();

    void ApplyConfiguration();
    void RetranslateUI();
    void SetConfiguration();

    void SetupPerGameUI();

    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void updateShaders(Settings::StereoRenderOption stereo_option);
    void updateTextureFilter(int index);

    std::unique_ptr<Ui::ConfigureLayout> ui;
    ConfigurationShared::CheckState swap_screen;
    ConfigurationShared::CheckState upright_screen;
    QColor bg_color;

    // DiySC: Clip and radius spinboxes (created programmatically, not in .ui)
    QDoubleSpinBox* custom_pct_top_clip_x_ = nullptr;
    QDoubleSpinBox* custom_pct_top_clip_y_ = nullptr;
    QDoubleSpinBox* custom_pct_top_radius_ = nullptr;
    QDoubleSpinBox* custom_pct_bottom_clip_x_ = nullptr;
    QDoubleSpinBox* custom_pct_bottom_clip_y_ = nullptr;
    QDoubleSpinBox* custom_pct_bottom_radius_ = nullptr;
    QDoubleSpinBox* custom_pct_top_edge_blur_ = nullptr;
    QDoubleSpinBox* custom_pct_bottom_edge_blur_ = nullptr;
    void setupPctClipRadiusControls();
};
