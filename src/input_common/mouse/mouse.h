// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <memory>
#include "core/frontend/input.h"

namespace InputCommon {

/**
 * A button device that reads from shared mouse button/wheel state.
 */
class MouseButtonDevice final : public Input::ButtonDevice {
public:
    explicit MouseButtonDevice(std::shared_ptr<std::atomic<u32>> state, u32 mask);
    bool GetStatus() const override;

private:
    std::shared_ptr<std::atomic<u32>> state;
    u32 mask;
};

/**
 * Shared mouse state accessible from the Qt event handler.
 * Bit flags: 0x01=left, 0x02=right, 0x04=middle, 0x08=wheel_up, 0x10=wheel_down
 */
class MouseState {
public:
    MouseState() : button_state(0) {}

    void PressButton(int button) {
        u32 mask = ButtonToMask(button);
        if (mask) button_state.fetch_or(mask);
    }
    void ReleaseButton(int button) {
        u32 mask = ButtonToMask(button);
        if (mask) button_state.fetch_and(~mask);
    }
    void WheelUp() { button_state.fetch_or(0x08); }
    void WheelDown() { button_state.fetch_or(0x10); }
    void ClearWheel() { button_state.fetch_and(~(0x08 | 0x10)); }
    std::shared_ptr<std::atomic<u32>> GetStatePtr() { return state_ptr; }

    static MouseState& Instance() {
        static MouseState instance;
        return instance;
    }

private:
    static u32 ButtonToMask(int button) {
        switch (button) {
        case 0: return 0x01; // left
        case 1: return 0x02; // right
        case 2: return 0x04; // middle
        default: return 0;
        }
    }
    std::atomic<u32> button_state{0};
    std::shared_ptr<std::atomic<u32>> state_ptr{
        std::shared_ptr<std::atomic<u32>>(&button_state, [](auto*) {})};
};

/**
 * Factory that creates MouseButtonDevice from parameters:
 *   "engine":"mouse", "button":"left|right|middle", "axis":"wheel", "value":"up|down"
 */
class MouseButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override;
};

} // namespace InputCommon
