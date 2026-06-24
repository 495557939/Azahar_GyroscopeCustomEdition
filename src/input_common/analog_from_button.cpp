// Copyright Citra Emulator Project / Lime3DS Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <string>
#include <vector>
#include "input_common/analog_from_button.h"

namespace InputCommon {

class Analog final : public Input::AnalogDevice {
public:
    using Button = std::unique_ptr<Input::ButtonDevice>;
    using Buttons = std::vector<Button>;

    Analog(Buttons up_, Buttons down_, Buttons left_, Buttons right_, Buttons up_left_,
           Buttons up_right_, Buttons down_left_, Buttons down_right_, Buttons modifier_,
           float modifier_scale_)
        : up(std::move(up_)), down(std::move(down_)), left(std::move(left_)),
          right(std::move(right_)), up_left(std::move(up_left_)), up_right(std::move(up_right_)),
          down_left(std::move(down_left_)), down_right(std::move(down_right_)),
          modifier(std::move(modifier_)), modifier_scale(modifier_scale_) {}

    static bool CheckAny(const Buttons& buttons) {
        for (const auto& b : buttons)
            if (b && b->GetStatus()) return true;
        return false;
    }

    std::tuple<float, float> GetStatus() const override {
        constexpr float SQRT_HALF = 0.707106781f;
        int x = 0, y = 0;

        if (CheckAny(right))
            ++x;
        if (CheckAny(left))
            --x;
        if (CheckAny(up))
            ++y;
        if (CheckAny(down))
            --y;
        if (CheckAny(up_right)) {
            ++x;
            ++y;
        }
        if (CheckAny(up_left)) {
            --x;
            ++y;
        }
        if (CheckAny(down_right)) {
            ++x;
            --y;
        }
        if (CheckAny(down_left)) {
            --x;
            --y;
        }

        float coef = CheckAny(modifier) ? modifier_scale : 1.0f;
        return std::make_tuple(x * coef * (y == 0 ? 1.0f : SQRT_HALF),
                               y * coef * (x == 0 ? 1.0f : SQRT_HALF));
    }

private:
    Buttons up;
    Buttons down;
    Buttons left;
    Buttons right;
    Buttons up_left;
    Buttons up_right;
    Buttons down_left;
    Buttons down_right;
    Buttons modifier;
    float modifier_scale;
};

static std::vector<std::unique_ptr<Input::ButtonDevice>>
CreateMultiDevices(const Common::ParamPackage& params, const std::string& base,
                   const std::string& null_engine) {
    std::vector<std::unique_ptr<Input::ButtonDevice>> devices;
    int count = params.Get(base + "_count", 0);
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            auto dev = Input::CreateDevice<Input::ButtonDevice>(
                params.Get(base + "_" + std::to_string(i), null_engine));
            if (dev)
                devices.push_back(std::move(dev));
        }
    } else {
        // Legacy single-binding fallback
        auto dev = Input::CreateDevice<Input::ButtonDevice>(params.Get(base, null_engine));
        if (dev)
            devices.push_back(std::move(dev));
    }
    return devices;
}

std::unique_ptr<Input::AnalogDevice> AnalogFromButton::Create(const Common::ParamPackage& params) {
    const std::string null_engine = Common::ParamPackage{{"engine", "null"}}.Serialize();
    return std::make_unique<Analog>(
        CreateMultiDevices(params, "up", null_engine),
        CreateMultiDevices(params, "down", null_engine),
        CreateMultiDevices(params, "left", null_engine),
        CreateMultiDevices(params, "right", null_engine),
        CreateMultiDevices(params, "up_left", null_engine),
        CreateMultiDevices(params, "up_right", null_engine),
        CreateMultiDevices(params, "down_left", null_engine),
        CreateMultiDevices(params, "down_right", null_engine),
        CreateMultiDevices(params, "modifier", null_engine),
        params.Get("modifier_scale", 0.5f));
}

} // namespace InputCommon
