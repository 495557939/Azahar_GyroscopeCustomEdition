// Copyright 2020 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/settings.h"
#include "core/3ds.h"
#include "input_common/touch_from_button.h"

namespace InputCommon {

class TouchFromButtonDevice final : public Input::TouchDevice {
public:
    TouchFromButtonDevice() {
        // Existing touch-from-button maps (per-map system)
        const auto& maps = Settings::values.touch_from_button_maps;
        int map_idx = Settings::values.current_input_profile.touch_from_button_map_index;
        if (map_idx >= 0 && map_idx < static_cast<int>(maps.size())) {
            for (const auto& config_entry : maps[map_idx].buttons) {
                const Common::ParamPackage package{config_entry};
                map.emplace_back(Input::CreateDevice<Input::ButtonDevice>(config_entry),
                                 std::clamp(package.Get("x", 0), 0, Core::kScreenBottomWidth),
                                 std::clamp(package.Get("y", 0), 0, Core::kScreenBottomHeight));
            }
        }
        // Per-profile touch screen coordinate bindings (percentage-based, nested)
        for (const auto& point_keys :
             Settings::values.current_input_profile.touch_points) {
            for (const auto& tp : point_keys) {
                const Common::ParamPackage pkg{tp};
                int px = static_cast<int>(pkg.Get("x", 50) * Core::kScreenBottomWidth / 100.0f);
                int py = static_cast<int>(pkg.Get("y", 50) * Core::kScreenBottomHeight / 100.0f);
                map.emplace_back(Input::CreateDevice<Input::ButtonDevice>(tp),
                                 std::clamp(px, 0, Core::kScreenBottomWidth),
                                 std::clamp(py, 0, Core::kScreenBottomHeight));
            }
        }
    }

    std::tuple<float, float, bool> GetStatus() const override {
        for (const auto& m : map) {
            const bool state = std::get<0>(m)->GetStatus();
            if (state) {
                const float x = static_cast<float>(std::get<1>(m)) / Core::kScreenBottomWidth;
                const float y = static_cast<float>(std::get<2>(m)) / Core::kScreenBottomHeight;
                return {x, y, true};
            }
        }
        return {};
    }

private:
    // A vector of the mapped button, its x and its y-coordinate
    std::vector<std::tuple<std::unique_ptr<Input::ButtonDevice>, int, int>> map;
};

std::unique_ptr<Input::TouchDevice> TouchFromButtonFactory::Create(
    const Common::ParamPackage& params) {
    return std::make_unique<TouchFromButtonDevice>();
}

} // namespace InputCommon
