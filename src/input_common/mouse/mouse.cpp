// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "input_common/mouse/mouse.h"
#include "common/param_package.h"

namespace InputCommon {

MouseButtonDevice::MouseButtonDevice(std::shared_ptr<std::atomic<u32>> state, u32 mask)
    : state(std::move(state)), mask(mask) {}

bool MouseButtonDevice::GetStatus() const {
    if (!state) return false;
    u32 current = state->load();
    // For wheel events, clear after reading (one-shot)
    if (mask & (0x08 | 0x10)) {
        bool result = (current & mask) != 0;
        state->fetch_and(~mask);
        return result;
    }
    return (current & mask) != 0;
}

std::unique_ptr<Input::ButtonDevice> MouseButtonFactory::Create(
    const Common::ParamPackage& params) {
    auto state = MouseState::Instance().GetStatePtr();

    // Mouse button bindings
    std::string button = params.Get("button", "");
    if (button == "left")
        return std::make_unique<MouseButtonDevice>(state, 0x01);
    else if (button == "right")
        return std::make_unique<MouseButtonDevice>(state, 0x02);
    else if (button == "middle")
        return std::make_unique<MouseButtonDevice>(state, 0x04);

    // Mouse wheel bindings
    std::string axis = params.Get("axis", "");
    std::string value = params.Get("value", "");
    if (axis == "wheel" && value == "up")
        return std::make_unique<MouseButtonDevice>(state, 0x08);
    else if (axis == "wheel" && value == "down")
        return std::make_unique<MouseButtonDevice>(state, 0x10);

    return nullptr;
}

} // namespace InputCommon
