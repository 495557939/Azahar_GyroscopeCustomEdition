// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version

#include <sstream>
#include <unordered_map>
#include "common/param_package.h"
#include "common/vector_math.h"
#include "core/frontend/input.h"
#include "input_common/fixed_motion.h"

namespace InputCommon {

// Toggle latch state: key = clean param string → latched (true) or not
static std::unordered_map<std::string, bool> g_fixed_motion_toggle_state;
static std::unordered_map<std::string, bool> g_fixed_motion_prev_raw;  // for edge detect
static std::unordered_map<std::string, int> g_fixed_motion_turbo_phase; // turbo counter

void FixedMotionConfig::LoadFromParams(const std::string& serialized) {
    Common::ParamPackage pkg(serialized);
    enabled = pkg.Get("enabled", false);
    int count = pkg.Get("count", 0);
    if (count < 0) count = 0;
    if (count > MAX_FIXED_MOTION_PRESETS) count = MAX_FIXED_MOTION_PRESETS;
    presets.resize(count);
    for (int i = 0; i < count; i++) {
        auto& p = presets[i];
        p.gravity.x = pkg.Get("grav" + std::to_string(i) + "_x", 0.0f);
        p.gravity.y = pkg.Get("grav" + std::to_string(i) + "_y", -1.0f);
        p.gravity.z = pkg.Get("grav" + std::to_string(i) + "_z", 0.0f);
        p.angular_rate.x = pkg.Get("rate" + std::to_string(i) + "_x", 0.0f);
        p.angular_rate.y = pkg.Get("rate" + std::to_string(i) + "_y", 0.0f);
        p.angular_rate.z = pkg.Get("rate" + std::to_string(i) + "_z", 0.0f);
        int btns = pkg.Get("btns" + std::to_string(i), 0);
        if (btns < 0) btns = 0;
        if (btns > MAX_FIXED_MOTION_KEYS) btns = MAX_FIXED_MOTION_KEYS;
        p.buttons.resize(btns);
        for (int j = 0; j < btns; j++) {
            p.buttons[j] = pkg.Get("btn" + std::to_string(i) + "_" + std::to_string(j), "");
        }
    }
}

std::string FixedMotionConfig::Serialize() const {
    Common::ParamPackage pkg;
    pkg.Set("enabled", enabled ? "1" : "0");
    pkg.Set("count", static_cast<int>(presets.size()));
    for (size_t i = 0; i < presets.size(); i++) {
        const auto& p = presets[i];
        pkg.Set("grav" + std::to_string(i) + "_x", p.gravity.x);
        pkg.Set("grav" + std::to_string(i) + "_y", p.gravity.y);
        pkg.Set("grav" + std::to_string(i) + "_z", p.gravity.z);
        pkg.Set("rate" + std::to_string(i) + "_x", p.angular_rate.x);
        pkg.Set("rate" + std::to_string(i) + "_y", p.angular_rate.y);
        pkg.Set("rate" + std::to_string(i) + "_z", p.angular_rate.z);
        pkg.Set("btns" + std::to_string(i), static_cast<int>(p.buttons.size()));
        for (size_t j = 0; j < p.buttons.size(); j++) {
            pkg.Set("btn" + std::to_string(i) + "_" + std::to_string(j), p.buttons[j]);
        }
    }
    return pkg.Serialize();
}

// Cached button devices — created once, reused every frame
static std::unordered_map<std::string, std::unique_ptr<Input::ButtonDevice>> g_fixed_motion_devices;
static std::string g_fixed_motion_last_config;

std::optional<std::pair<Common::Vec3<float>, Common::Vec3<float>>>
GetFixedMotionOverride(const FixedMotionConfig& config) {
    // Always process FM presets; empty config naturally returns nullopt
    for (const auto& preset : config.presets) {
        for (const auto& btn_str : preset.buttons) {
            if (btn_str.empty())
                continue;
            Common::ParamPackage pp(btn_str);
            bool is_toggle = pp.Get("toggle", "0") == "1";
            bool is_reverse = pp.Get("reverse", "0") == "1";

            // Strip toggle/turbo/reverse from the param string for device creation key
            auto clean_pp = pp;
            clean_pp.Erase("toggle");
            clean_pp.Erase("turbo");
            clean_pp.Erase("reverse");
            std::string clean_str = clean_pp.Serialize();

            // Cache device — create once, reuse
            auto& dev_ptr = g_fixed_motion_devices[clean_str];
            if (!dev_ptr) {
                dev_ptr = Input::CreateDevice<Input::ButtonDevice>(clean_str);
            }
            bool raw_pressed = dev_ptr && dev_ptr->GetStatus();

            if (is_reverse) {
                // Reverse: preset active by default, pressing key releases it
                if (!raw_pressed) {
                    return std::make_pair(preset.gravity, preset.angular_rate);
                }
            } else if (is_toggle) {
                // Edge detection: rising edge toggles latch on/off
                bool& prev = g_fixed_motion_prev_raw[clean_str];
                if (raw_pressed && !prev) {
                    g_fixed_motion_toggle_state[clean_str] =
                        !g_fixed_motion_toggle_state[clean_str];
                }
                prev = raw_pressed;

                if (g_fixed_motion_toggle_state[clean_str]) {
                    return std::make_pair(preset.gravity, preset.angular_rate);
                }
            } else {
                bool is_turbo = pp.Get("turbo", "0") == "1";
                if (is_turbo && raw_pressed) {
                    // Turbo: 1 frame on, 2 frames off (~20 Hz rapid fire)
                    int& phase = g_fixed_motion_turbo_phase[clean_str];
                    if (phase == 0) {
                        phase = 1;
                        return std::make_pair(preset.gravity, preset.angular_rate);
                    }
                    phase = (phase + 1) % 3;
                } else if (!is_turbo && raw_pressed) {
                    return std::make_pair(preset.gravity, preset.angular_rate);
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace InputCommon
