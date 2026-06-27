// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <unordered_map>
#include "input_common/main.h"

namespace InputCommon {

/// Wraps a ButtonDevice to rapidly toggle on/off while the underlying button is held.
/// ON for 1 frame, OFF for 2 frames → ~20 Hz rapid fire.
class TurboButton final : public Input::ButtonDevice {
public:
    explicit TurboButton(std::unique_ptr<Input::ButtonDevice> inner_)
        : inner(std::move(inner_)) {}

    bool GetStatus() const override {
        bool held = inner->GetStatus();
        if (!held) {
            phase = 0;
            return false;
        }
        bool result = (phase == 0);
        phase = (phase + 1) % 3;
        return result;
    }

private:
    std::unique_ptr<Input::ButtonDevice> inner;
    mutable int phase = 0;
};

/// Shared latching toggle: any bound key in the same button set toggles
/// a single shared ON/OFF state. Press any Toggle-key in the set to flip.
class ToggleButton final : public Input::ButtonDevice {
public:
    ToggleButton(std::unique_ptr<Input::ButtonDevice> inner_, int button_idx_)
        : inner(std::move(inner_)), btn(button_idx_) {
        if (s_latch.find(btn) == s_latch.end())
            s_latch[btn] = false;
    }

    bool GetStatus() const override {
        bool raw = inner->GetStatus();
        if (raw && !prev_raw) {
            // Rising edge on this key flips the shared state
            s_latch[btn] = !s_latch[btn];
        }
        prev_raw = raw;
        return s_latch[btn];
    }

    /// Clear the shared latch for a button index (called when non-latch key pressed)
    static void ClearLatch(int btn_idx) { s_latch[btn_idx] = false; }

private:
    std::unique_ptr<Input::ButtonDevice> inner;
    int btn;
    mutable bool prev_raw = false;
    static inline std::unordered_map<int, bool> s_latch;
};

} // namespace InputCommon
