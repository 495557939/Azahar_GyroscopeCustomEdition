// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QBoxLayout>
#include <QColorDialog>
#include <QComboBox>
#include <QLabel>
#include <QTimer>
#include "citra_qt/configuration/configuration_shared.h"
#include "citra_qt/configuration/configure_enhancements.h"
#include "common/settings.h"
#include "ui_configure_enhancements.h"
#ifdef ENABLE_OPENGL
#include "video_core/renderer_opengl/post_processing_opengl.h"
#endif

ConfigureEnhancements::ConfigureEnhancements(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureEnhancements>()) {
    ui->setupUi(this);

    // ── Multi-filter slots 2-10 ──
    // Build them BEFORE SetConfiguration so updateShaders can populate them.
    {
        ui->shader_label->setText(tr("Post-Processing Shader 1:"));

        // Calculate the maximum label width so all shader rows align
        int max_label_w = 0;
        for (int s = 1; s <= 10; s++) {
            auto lbl = QLabel(tr("Post-Processing Shader %1:").arg(s));
            max_label_w = std::max(max_label_w,
                lbl.fontMetrics().boundingRect(lbl.text()).width());
        }

        // Swap: move shader widget after texture filter
        auto* parent_group = ui->widget_texture_filter->parentWidget();
        QBoxLayout* vbox = qobject_cast<QBoxLayout*>(parent_group ? parent_group->layout() : nullptr);
        if (vbox) {
            QWidget* shader_widget = ui->shader_combobox->parentWidget();
            int tf_idx = vbox->indexOf(ui->widget_texture_filter);
            if (tf_idx >= 0 && shader_widget) {
                vbox->removeWidget(shader_widget);
                vbox->insertWidget(tf_idx + 1, shader_widget);
            }
        }

        ui->shader_label->setMinimumWidth(max_label_w);
        ui->shader_label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

        for (int slot = 2; slot <= 10; slot++) {
            int idx = slot - 2;
            auto* row = new QWidget(this);
            auto* hlay = new QHBoxLayout(row);
            hlay->setContentsMargins(0, 0, 0, 0);
            auto* label = new QLabel(tr("Post-Processing Shader %1:").arg(slot), row);
            auto* combo = new QComboBox(row);
            combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            label->setMinimumWidth(max_label_w);
            label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
            hlay->addWidget(label);
            hlay->addWidget(combo);
            if (vbox) vbox->addWidget(row);
            filter_labels[idx] = label;
            filter_slots[idx] = combo;
            row->setVisible(false);

            QComboBox* prev = (slot == 2) ? ui->shader_combobox : filter_slots[idx - 1];
            connect(prev, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this] { UpdateFilterSlotVisibility(); });
        }
    }

    SetupPerGameUI();
    SetConfiguration();

    const auto graphics_api = Settings::GetWorkingGraphicsAPI();
    const bool res_scale_enabled = graphics_api != Settings::GraphicsAPI::Software;
    ui->resolution_factor_combobox->setEnabled(res_scale_enabled);

    connect(ui->render_3d_combobox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int currentIndex) {
                updateShaders(static_cast<Settings::StereoRenderOption>(currentIndex));
            });

    ui->toggle_preload_textures->setEnabled(ui->toggle_custom_textures->isChecked());
    ui->toggle_async_custom_loading->setEnabled(ui->toggle_custom_textures->isChecked());
    connect(ui->toggle_custom_textures, &QCheckBox::toggled, this, [this] {
        ui->toggle_preload_textures->setEnabled(ui->toggle_custom_textures->isChecked());
        ui->toggle_async_custom_loading->setEnabled(ui->toggle_custom_textures->isChecked());
    if (!ui->toggle_preload_textures->isEnabled())
        ui->toggle_preload_textures->setChecked(false);
    });

    // Defer visibility update until after SetConfiguration() has populated slot 1
    QTimer::singleShot(0, this, &ConfigureEnhancements::UpdateFilterSlotVisibility);
}

ConfigureEnhancements::~ConfigureEnhancements() = default;

void ConfigureEnhancements::UpdateFilterSlotVisibility() {
    // Show slot N+1 only when slot N has a non-None filter selected.
    // Slots 2-10 are always "None" by default (empty list) but their visibility
    // is purely a function of the previous slot's selection.
    auto is_active = [](QComboBox* cb) {
        if (!cb) return false;
        // If empty, treat as "None" so default AA-2D3D in slot 1 still propagates.
        if (cb->count() == 0) return false;
        return cb->currentText() != QStringLiteral("None (builtin)");
    };

    bool prev_active = is_active(ui->shader_combobox);
    for (int i = 2; i <= 10; i++) {
        int idx = i - 2;
        if (!filter_slots[idx]) continue;
        bool visible = prev_active;
        QWidget* row = filter_slots[idx]->parentWidget();
        if (row) row->setVisible(visible);
        if (filter_labels[idx]) filter_labels[idx]->setVisible(visible);
        prev_active = visible && is_active(filter_slots[idx]);
    }
}


void ConfigureEnhancements::SetConfiguration() {

    if (!Settings::IsConfiguringGlobal()) {
        ConfigurationShared::SetPerGameSetting(ui->resolution_factor_combobox,
                                               &Settings::values.resolution_factor);
        ConfigurationShared::SetPerGameSetting(ui->texture_filter_combobox,
                                               &Settings::values.texture_filter);
        ConfigurationShared::SetHighlight(ui->widget_texture_filter,
                                          !Settings::values.texture_filter.UsingGlobal());
    } else {
        // Combo indices: 0=Auto, 1..7=Auto-Nx window, 8..17=Native..10x
        const u32 rf = Settings::values.resolution_factor.GetValue();
        int combo_index = 0;
        if (rf == 0) {
            combo_index = 0;
        } else if (rf >= 11 && rf <= 17) {
            combo_index = static_cast<int>(rf - 10); // 11->1, 12->2, ..., 17->7
        } else {
            combo_index = static_cast<int>(rf) + 7; // 1->8, 2->9, ..., 10->17
        }
        ui->resolution_factor_combobox->setCurrentIndex(combo_index);
        ui->texture_filter_combobox->setCurrentIndex(
            static_cast<int>(Settings::values.texture_filter.GetValue()));
    }

    ui->render_3d_combobox->setCurrentIndex(
        static_cast<int>(Settings::values.render_3d.GetValue()));
    ui->swap_eyes_3d->setChecked(Settings::values.swap_eyes_3d.GetValue());
    ui->factor_3d->setValue(Settings::values.factor_3d.GetValue());
    ui->mono_rendering_eye->setCurrentIndex(
        static_cast<int>(Settings::values.mono_render_option.GetValue()));
    updateShaders(Settings::values.render_3d.GetValue());

    // Set filter slots 2-10 values
    auto set_filter_slot = [&](int slot_idx, const std::string& value) {
        if (slot_idx >= 0 && slot_idx < 9 && filter_slots[slot_idx]) {
            int idx = filter_slots[slot_idx]->findText(QString::fromStdString(value));
            if (idx >= 0) filter_slots[slot_idx]->setCurrentIndex(idx);
        }
    };
    set_filter_slot(0, Settings::values.pp_shader_name_2.GetValue());
    set_filter_slot(1, Settings::values.pp_shader_name_3.GetValue());
    set_filter_slot(2, Settings::values.pp_shader_name_4.GetValue());
    set_filter_slot(3, Settings::values.pp_shader_name_5.GetValue());
    set_filter_slot(4, Settings::values.pp_shader_name_6.GetValue());
    set_filter_slot(5, Settings::values.pp_shader_name_7.GetValue());
    set_filter_slot(6, Settings::values.pp_shader_name_8.GetValue());
    set_filter_slot(7, Settings::values.pp_shader_name_9.GetValue());
    set_filter_slot(8, Settings::values.pp_shader_name_10.GetValue());
    UpdateFilterSlotVisibility();
    ui->toggle_linear_filter->setChecked(Settings::values.filter_mode.GetValue());
    ui->use_integer_scaling->setChecked(Settings::values.use_integer_scaling.GetValue());
    ui->toggle_dump_textures->setChecked(Settings::values.dump_textures.GetValue());
    ui->toggle_custom_textures->setChecked(Settings::values.custom_textures.GetValue());
    ui->toggle_preload_textures->setChecked(Settings::values.preload_textures.GetValue());
    ui->toggle_async_custom_loading->setChecked(Settings::values.async_custom_loading.GetValue());
    ui->disable_right_eye_render->setChecked(Settings::values.disable_right_eye_render.GetValue());
}

void ConfigureEnhancements::updateShaders(Settings::StereoRenderOption stereo_option) {
    ui->shader_combobox->blockSignals(true);
    ui->shader_combobox->clear();
    ui->shader_combobox->setEnabled(true);

    if (stereo_option == Settings::StereoRenderOption::Interlaced ||
        stereo_option == Settings::StereoRenderOption::ReverseInterlaced) {
        ui->shader_combobox->addItem(QStringLiteral("Horizontal (builtin)"));
        ui->shader_combobox->setCurrentIndex(0);
        ui->shader_combobox->setEnabled(false);
        return;
    }

    std::string current_shader;
    if (stereo_option == Settings::StereoRenderOption::Anaglyph) {
        ui->shader_combobox->addItem(QStringLiteral("Dubois (builtin)"));
        current_shader = Settings::values.anaglyph_shader_name.GetValue();
    } else {
        ui->shader_combobox->addItem(QStringLiteral("None (builtin)"));
        current_shader = Settings::values.pp_shader_name.GetValue();
    }

    ui->shader_combobox->setCurrentIndex(0);

#ifdef ENABLE_OPENGL
    for (const auto& shader : OpenGL::GetPostProcessingShaderList(
             stereo_option == Settings::StereoRenderOption::Anaglyph)) {
        ui->shader_combobox->addItem(QString::fromStdString(shader));
        if (current_shader == shader)
            ui->shader_combobox->setCurrentIndex(ui->shader_combobox->count() - 1);
    }
    // Populate filter slots 2-10 with the same shader list
    for (int i = 0; i < 9; i++) {
        if (!filter_slots[i]) continue;
        QString current = filter_slots[i]->currentText();
        filter_slots[i]->clear();
        filter_slots[i]->addItem(QStringLiteral("None (builtin)"));
        for (const auto& shader : OpenGL::GetPostProcessingShaderList(
                 stereo_option == Settings::StereoRenderOption::Anaglyph)) {
            filter_slots[i]->addItem(QString::fromStdString(shader));
            if (current == QString::fromStdString(shader))
                filter_slots[i]->setCurrentIndex(filter_slots[i]->count() - 1);
        }
    }
#endif
    ui->shader_combobox->blockSignals(false);
    UpdateFilterSlotVisibility();
}

void ConfigureEnhancements::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureEnhancements::ApplyConfiguration() {
    if (Settings::IsConfiguringGlobal()) {
        // Translate combo index 0..17 back to resolution_factor 0..17
        const int ci = ui->resolution_factor_combobox->currentIndex();
        u32 rf = 1;
        if (ci <= 0) {
            rf = 0;
        } else if (ci <= 7) {
            rf = static_cast<u32>(ci + 10); // 1->11, 2->12, ..., 7->17
        } else {
            rf = static_cast<u32>(ci - 7); // 8->1, 9->2, ..., 17->10
        }
        Settings::values.resolution_factor = rf;
    } else {
        ConfigurationShared::ApplyPerGameSetting(&Settings::values.resolution_factor,
                                                 ui->resolution_factor_combobox);
    }
    Settings::values.render_3d =
        static_cast<Settings::StereoRenderOption>(ui->render_3d_combobox->currentIndex());
    Settings::values.swap_eyes_3d = ui->swap_eyes_3d->isChecked();
    Settings::values.factor_3d = ui->factor_3d->value();
    Settings::values.mono_render_option =
        static_cast<Settings::MonoRenderOption>(ui->mono_rendering_eye->currentIndex());
    if (Settings::values.render_3d.GetValue() == Settings::StereoRenderOption::Anaglyph) {
        Settings::values.anaglyph_shader_name =
            ui->shader_combobox->itemText(ui->shader_combobox->currentIndex()).toStdString();
    } else if (Settings::values.render_3d.GetValue() == Settings::StereoRenderOption::Off) {
        Settings::values.pp_shader_name =
            ui->shader_combobox->itemText(ui->shader_combobox->currentIndex()).toStdString();
        auto save_filter_slot = [&](int idx, Settings::Setting<std::string>& setting) {
            if (idx >= 0 && idx < 9 && filter_slots[idx]) {
                if (filter_slots[idx]->isVisible())
                    setting.SetValue(filter_slots[idx]->currentText().toStdString());
                // Hidden slots: keep whatever value was already set (don't force "None")
            }
        };
        save_filter_slot(0, Settings::values.pp_shader_name_2);
        save_filter_slot(1, Settings::values.pp_shader_name_3);
        save_filter_slot(2, Settings::values.pp_shader_name_4);
        save_filter_slot(3, Settings::values.pp_shader_name_5);
        save_filter_slot(4, Settings::values.pp_shader_name_6);
        save_filter_slot(5, Settings::values.pp_shader_name_7);
        save_filter_slot(6, Settings::values.pp_shader_name_8);
        save_filter_slot(7, Settings::values.pp_shader_name_9);
        save_filter_slot(8, Settings::values.pp_shader_name_10);
    }
    Settings::values.disable_right_eye_render = ui->disable_right_eye_render->isChecked();

    ConfigurationShared::ApplyPerGameSetting(&Settings::values.filter_mode,
                                             ui->toggle_linear_filter, linear_filter);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.use_integer_scaling,
                                             ui->use_integer_scaling, use_integer_scaling);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.texture_filter,
                                             ui->texture_filter_combobox);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.dump_textures,
                                             ui->toggle_dump_textures, dump_textures);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.custom_textures,
                                             ui->toggle_custom_textures, custom_textures);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.preload_textures,
                                             ui->toggle_preload_textures, preload_textures);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.async_custom_loading,
                                             ui->toggle_async_custom_loading, async_custom_loading);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.disable_right_eye_render,
                                             ui->disable_right_eye_render,
                                             disable_right_eye_render);
}

void ConfigureEnhancements::SetupPerGameUI() {
    // Block the global settings if a game is currently running that overrides them
    if (Settings::IsConfiguringGlobal()) {
        ui->widget_resolution->setEnabled(Settings::values.resolution_factor.UsingGlobal());
        ui->widget_texture_filter->setEnabled(Settings::values.texture_filter.UsingGlobal());
        ui->toggle_linear_filter->setEnabled(Settings::values.filter_mode.UsingGlobal());
        ui->use_integer_scaling->setEnabled(Settings::values.use_integer_scaling.UsingGlobal());
        ui->toggle_dump_textures->setEnabled(Settings::values.dump_textures.UsingGlobal());
        ui->toggle_custom_textures->setEnabled(Settings::values.custom_textures.UsingGlobal());
        ui->toggle_preload_textures->setEnabled(Settings::values.preload_textures.UsingGlobal());
        ui->toggle_async_custom_loading->setEnabled(
            Settings::values.async_custom_loading.UsingGlobal());
        ui->disable_right_eye_render->setEnabled(
            Settings::values.disable_right_eye_render.UsingGlobal());
        return;
    }

    ui->render_3d_combobox->setEnabled(false);
    ui->factor_3d->setEnabled(false);
    ui->mono_rendering_eye->setEnabled(false);

    ui->widget_shader->setVisible(false);

    ConfigurationShared::SetColoredTristate(ui->toggle_linear_filter, Settings::values.filter_mode,
                                            linear_filter);
    ConfigurationShared::SetColoredTristate(
        ui->use_integer_scaling, Settings::values.use_integer_scaling, use_integer_scaling);
    ConfigurationShared::SetColoredTristate(ui->toggle_dump_textures,
                                            Settings::values.dump_textures, dump_textures);
    ConfigurationShared::SetColoredTristate(ui->toggle_custom_textures,
                                            Settings::values.custom_textures, custom_textures);
    ConfigurationShared::SetColoredTristate(ui->toggle_preload_textures,
                                            Settings::values.preload_textures, preload_textures);
    ConfigurationShared::SetColoredTristate(ui->toggle_async_custom_loading,
                                            Settings::values.async_custom_loading,
                                            async_custom_loading);
    ConfigurationShared::SetColoredTristate(ui->disable_right_eye_render,
                                            Settings::values.disable_right_eye_render,
                                            disable_right_eye_render);

    {
        const u32 rf = Settings::values.resolution_factor.GetValue(true);
        int combo_index = 0;
        if (rf == 0) {
            combo_index = 0;
        } else if (rf >= 11 && rf <= 17) {
            combo_index = static_cast<int>(rf - 10);
        } else {
            combo_index = static_cast<int>(rf) + 7;
        }
        ConfigurationShared::SetColoredComboBox(ui->resolution_factor_combobox,
                                                ui->widget_resolution, combo_index);
    }

    ConfigurationShared::SetColoredComboBox(
        ui->texture_filter_combobox, ui->widget_texture_filter,
        static_cast<int>(Settings::values.texture_filter.GetValue(true)));
}
