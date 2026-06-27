// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version

#pragma once

#include <array>
#include <string>
#include <vector>
#include "common/vector_math.h"

namespace InputCommon {

/// A single fixed motion preset: gravity + angular_rate + button bindings.
struct FixedMotionPreset {
    Common::Vec3<float> gravity{};      // accelerometer (g-force vector)
    Common::Vec3<float> angular_rate{}; // gyro rate (deg/s)
    std::vector<std::string> buttons;   // serialized ParamPackage strings (up to 5)
};

/// Maximum number of fixed motion presets.
constexpr int MAX_FIXED_MOTION_PRESETS = 8;
/// Maximum number of button slots per preset.
constexpr int MAX_FIXED_MOTION_KEYS = 5;

/// Container for all fixed motion presets, loadable from QSettings.
struct FixedMotionConfig {
    bool enabled = false;
    std::vector<FixedMotionPreset> presets;

    void LoadFromParams(const std::string& serialized);
    std::string Serialize() const;
};

/// Returns the active override (gravity, angular_rate) if any bound button is pressed,
/// otherwise returns std::nullopt.
std::optional<std::pair<Common::Vec3<float>, Common::Vec3<float>>>
GetFixedMotionOverride(const FixedMotionConfig& config);

} // namespace InputCommon
