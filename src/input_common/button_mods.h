// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
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

} // namespace InputCommon
