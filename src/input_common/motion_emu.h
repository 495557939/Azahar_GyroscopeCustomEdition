// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/frontend/input.h"

namespace InputCommon {

class MotionEmuDevice;

/// Motion emulation input mode
enum class MotionEmuMode : u8 {
    Absolute,       ///< Original: right-click hold → absolute tilt angle → gravity + angular rate
    RateHold,       ///< Right-click hold → angular rate (deg/sec) from mouse delta (center-warp lock)
    RateContinuous, ///< Mouse always provides motion input (no button needed, free cursor)
    TiltContinuous, ///< Always active: mouse % of screen center → gravity tilt
    TiltHold,       ///< Right-click hold: cursor lock → gravity tilt from center
};

class MotionEmu : public Input::Factory<Input::MotionDevice> {
public:
    /**
     * Creates a motion device emulated from mouse input
     * @param params contains parameters for creating the device:
     *     - "mode": motion emulation mode (absolute/rate_hold/rate_continuous)
     *     - "update_period": update period in milliseconds
     *     - "sensitivity": sensitivity coefficient
     *     - "tilt_clamp": max tilt angle in degrees (absolute mode only)
     *     - "deadzone": minimum pixel delta to process (rate modes only)
     */
    std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage& params) override;

    /**
     * Returns the current motion emulation mode.
     */
    MotionEmuMode GetMode() const;

    /**
     * Signals that a motion sensor tilt has begun.
     * @param x the x-coordinate of the cursor
     * @param y the y-coordinate of the cursor
     */
    void BeginTilt(int x, int y);

    /**
     * Signals that a motion sensor tilt is occurring.
     * @param x the x-coordinate of the cursor
     * @param y the y-coordinate of the cursor
     */
    void Tilt(int x, int y);

    /**
     * Signals that a motion sensor tilt has ended.
     */
    void EndTilt();

    /**
     * Toggles motion active state.
     */
    void ToggleActive();

    /**
     * Explicitly sets motion active state (useful for RateContinuous mode).
     */
    void SetActive(bool active);

    /**
     * Returns whether the underlying motion device is currently active.
     */
    bool IsDeviceActive() const;

    /**
     * Feed raw mouse delta directly (used with cursor capture / relative mouse mode).
     * @param dx pixel delta in X direction
     * @param dy pixel delta in Y direction
     */
    void TiltDelta(float dx, float dy);
    void SetTiltOffset(float yaw_rad, float pitch_rad);  // tilt modes (gravity)
    void AddDelta(float dx, float dy);  // accumulate (RateContinuous global tracking)

    /**
     * Returns invert flags from the most recently created device.
     */
    void GetInvertFlags(bool& out_pitch, bool& out_yaw) const;
    float GetTiltMaxAngle() const;
    void SetAutoRollTarget(float dx);  // feed external horizontal input (e.g. right stick)

private:
    std::weak_ptr<MotionEmuDevice> current_device;
    MotionEmuMode current_mode = MotionEmuMode::Absolute;
};

} // namespace InputCommon
