// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <tuple>
#include "common/math_util.h"
#include "common/quaternion.h"
#include "common/thread.h"
#include "common/vector_math.h"
#include "input_common/motion_emu.h"

namespace InputCommon {

// Implementation class of the motion emulation device
class MotionEmuDevice {
public:
    MotionEmuDevice(int update_millisecond, float sensitivity, float tilt_clamp,
                    MotionEmuMode mode, float deadzone, float default_tilt,
                    float tilt_max_angle, bool invert_pitch, bool invert_yaw, bool per_frame,
                    bool clamp_pitch_180,
                    bool auto_tilt_y, bool auto_tilt_y_invert, bool auto_tilt_x,
                    float auto_tilt_speed, float auto_tilt_y_return_speed,
                    int auto_tilt_y_max_angle, bool auto_tilt_y_prevent_flip)
        : update_millisecond(update_millisecond),
          update_duration(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::milliseconds(update_millisecond))),
          sensitivity(sensitivity), tilt_clamp(tilt_clamp), mode(mode), deadzone(deadzone),
          default_tilt(default_tilt), tilt_max_angle(tilt_max_angle),
          invert_pitch(invert_pitch), invert_yaw(invert_yaw),
          per_frame(per_frame), clamp_pitch_180(clamp_pitch_180),
          auto_tilt_y(auto_tilt_y), auto_tilt_y_invert(auto_tilt_y_invert),
          auto_tilt_x(auto_tilt_x), auto_tilt_speed(auto_tilt_speed),
          auto_tilt_y_return_speed(auto_tilt_y_return_speed),
          auto_tilt_y_max_angle(auto_tilt_y_max_angle),
          auto_tilt_y_prevent_flip(auto_tilt_y_prevent_flip),
          q_accumulated(Common::MakeQuaternion(Common::Vec3<float>(), 0)),
          last_status_time(std::chrono::steady_clock::now()),
          last_rate_compute(std::chrono::steady_clock::now()),
          is_active(mode == MotionEmuMode::RateContinuous ||
                    mode == MotionEmuMode::TiltContinuous),
          motion_emu_thread(&MotionEmuDevice::MotionEmuThread, this) {}

    ~MotionEmuDevice() {
        if (motion_emu_thread.joinable()) {
            shutdown_event.Set();
            motion_emu_thread.join();
        }
    }

    void BeginTilt(int x, int y) {
        mouse_origin = Common::MakeVec(x, y);
        if (mode == MotionEmuMode::RateHold || mode == MotionEmuMode::Absolute ||
            mode == MotionEmuMode::TiltHold) {
            is_active = true;
        }
        // RateContinuous / TiltContinuous auto-activate:
        // these modes are always-on; no right-click needed to start.
        if (mode == MotionEmuMode::RateContinuous ||
            mode == MotionEmuMode::TiltContinuous) {
            is_active = true;
        }
    }

    void Tilt(int x, int y) {
        auto mouse_move = Common::MakeVec(x, y) - mouse_origin;
        std::lock_guard guard{tilt_mutex};

        if (mode == MotionEmuMode::Absolute) {
            if (is_active) {
                if (mouse_move.x == 0 && mouse_move.y == 0) {
                    tilt_angle = 0;
                } else {
                    tilt_direction = mouse_move.Cast<float>();
                    tilt_angle = std::clamp(tilt_direction.Normalize() * sensitivity, 0.0f,
                                            Common::PI * tilt_clamp / 180.0f);
                }
            }
        } else {
            // Rate modes: store delta directly, convert to angular rate in thread
            auto delta_f = mouse_move.Cast<float>();
            if (std::abs(delta_f.x) < deadzone) {
                delta_f.x = 0.0f;
            }
            if (std::abs(delta_f.y) < deadzone) {
                delta_f.y = 0.0f;
            }
            rate_delta = delta_f;
            // Reset origin so next delta is relative to current position
            mouse_origin = Common::MakeVec(x, y);
        }

        // [BETA] Auto X-tilt: horizontal mouse movement sets roll target
        if (auto_tilt_x && is_active) {
            float dx = static_cast<float>(mouse_move.x);
            // 1 pixel ≈ 2° of roll, clamped ±90°, scaled by speed
            auto_roll_target = std::clamp(dx * 2.0f * auto_tilt_speed, -90.0f, 90.0f);
        }
    }

    void EndTilt() {
        std::lock_guard guard{tilt_mutex};
        if (mode == MotionEmuMode::Absolute || mode == MotionEmuMode::RateHold ||
            mode == MotionEmuMode::TiltHold) {
            is_active = false;
        }
        tilt_angle = 0;
        rate_delta = Common::Vec2<float>{};
        tilt_offset_yaw = 0.0f;
        tilt_offset_pitch = 0.0f;
        // Reset auto-roll target only; current roll decays smoothly
        auto_roll_target = 0.0f;
        // Reset auto-tilt accumulators so next activation starts fresh
        q_accumulated = Common::MakeQuaternion(Common::Vec3<float>(), 0);
        accumulated_tilt_pitch = 0.0f;
    }

    void ToggleActive() {
        // No-op: RateToggle mode removed. Use SetActive for RateContinuous.
    }

    void SetActive(bool active) {
        std::lock_guard guard{tilt_mutex};
        is_active = active;
        if (!active) {
            rate_delta = Common::Vec2<float>{};
            tilt_angle = 0;
        }
    }

    // TiltDelta: overwrite (RateHold center-warp). Use AddDelta for accumulation.
    void TiltDelta(float dx, float dy) {
        if (!is_active)
            return;
        float mag_sq = dx * dx + dy * dy;
        if (mag_sq < deadzone * deadzone)
            return;
        std::lock_guard guard{tilt_mutex};
        rate_delta.x = dx;
        rate_delta.y = dy;
    }

    bool IsActive() const {
        return is_active;
    }

    void GetInvertFlags(bool& out_pitch, bool& out_yaw) const {
        out_pitch = invert_pitch;
        out_yaw = invert_yaw;
    }

    // AddDelta: accumulate pixel delta (used by RateContinuous global timer).
    void AddDelta(float dx, float dy) {
        if (!is_active)
            return;
        std::lock_guard guard{tilt_mutex};
        rate_delta.x += dx;
        rate_delta.y += dy;
        if (dx != 0.0f || dy != 0.0f)
            last_mouse_input_time = std::chrono::steady_clock::now();
    }

    // SetTiltOffset: feed yaw/pitch radians for TiltContinuous / TiltHold gravity modes.
    // Called from the UI 200 Hz timer; the thread/GetStatus applies these on top of default_tilt.
    void SetTiltOffset(float yaw_rad, float pitch_rad) {
        std::lock_guard guard{tilt_mutex};
        tilt_offset_yaw = yaw_rad;
        tilt_offset_pitch = pitch_rad;
        last_mouse_input_time = std::chrono::steady_clock::now();
    }

    float GetTiltMaxAngle() const {
        return tilt_max_angle;
    }

    // Feed an external horizontal input to auto X-tilt (e.g. right stick).
    // Scale: ~20° at BUTTON_BASE=8 with speed=1, clamped ±90°.
    void SetAutoRollTarget(float dx) {
        std::lock_guard guard{tilt_mutex};
        float target = dx * 2.5f * auto_tilt_speed;
        auto_roll_target = std::clamp(target, -90.0f, 90.0f);
    }

    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() {
        // Per-frame sync: compute on-the-fly using actual inter-frame time.
        if (per_frame && mode != MotionEmuMode::Absolute) {
            auto now = std::chrono::steady_clock::now();
            float dt_ms =
                std::chrono::duration<float, std::milli>(now - last_status_time).count();
            last_status_time = now;

            if (dt_ms > 0.0f && dt_ms < 1000.0f) {
                // Same lock order as the thread: tilt_mutex → status_mutex
                std::lock_guard tilt_guard{tilt_mutex};
                if (is_active) {
                    if (mode == MotionEmuMode::TiltContinuous ||
                        mode == MotionEmuMode::TiltHold) {
                        // Tilt modes: gravity from mouse offset (yaw/pitch radians)
                        float effective_tilt_rad = default_tilt * Common::PI / 180.0f;
                        // Auto Y-tilt: accumulate pitch from offset changes
                        if (auto_tilt_y) {
                            float sign = auto_tilt_y_invert ? 1.0f : -1.0f;
                            accumulated_tilt_pitch += sign * tilt_offset_pitch;
                            accumulated_tilt_pitch = ClampAutoTiltPitch(accumulated_tilt_pitch);
                            // Only decay when mouse has been idle
                            auto since_input = std::chrono::duration<float, std::milli>(
                                now - last_mouse_input_time).count();
                            if (auto_tilt_y_return_speed > 0.0f && since_input > 50.0f) {
                                float step = auto_tilt_y_return_speed * Common::PI / 180.0f * (dt_ms / 1000.0f);
                                if (accumulated_tilt_pitch > 0.0f)
                                    accumulated_tilt_pitch = std::max(0.0f, accumulated_tilt_pitch - step);
                                else if (accumulated_tilt_pitch < 0.0f)
                                    accumulated_tilt_pitch = std::min(0.0f, accumulated_tilt_pitch + step);
                            }
                            effective_tilt_rad += accumulated_tilt_pitch;
                        }
                        auto gravity =
                            Common::MakeVec(0.0f, -std::cos(effective_tilt_rad),
                                            -std::sin(effective_tilt_rad));
                        // Rotate gravity by tilt_offset_yaw around Y axis
                        float cy = std::cos(tilt_offset_yaw);
                        float sy = std::sin(tilt_offset_yaw);
                        float gx = gravity.x * cy - gravity.z * sy;
                        float gz = gravity.x * sy + gravity.z * cy;
                        // Rotate by tilt_offset_pitch around X axis
                        float cp = std::cos(tilt_offset_pitch);
                        float sp = std::sin(tilt_offset_pitch);
                        gravity = Common::MakeVec(gx, gravity.y * cp + gz * sp,
                                                  -gravity.y * sp + gz * cp);
                        // [BETA] Auto X-tilt: apply smoothed roll around Z axis
                        if (auto_tilt_x) {
                            // Smooth decay when target is 0; fast approach when active
                            float decay = (auto_roll_target == 0.0f) ? 0.90f : 0.70f;
                            auto_roll = auto_roll * decay + auto_roll_target * (1.0f - decay);
                            float rr = auto_roll * Common::PI / 180.0f;
                            float cr = std::cos(rr);
                            float sr = std::sin(rr);
                            float gx2 = gravity.x * cr - gravity.y * sr;
                            float gy2 = gravity.x * sr + gravity.y * cr;
                            gravity = Common::MakeVec(gx2, gy2, gravity.z);
                        }
                        {
                            std::lock_guard status_guard{status_mutex};
                            status = std::make_tuple(gravity, Common::Vec3<float>{});
                        }
                    } else {
                        // Rate modes: mouse delta to angular velocity.
                        // Use smoothed dt_ms to reduce integer-pixel quantization
                        // jitter (gyro callback ~9.9ms gives only 1-2 AddDelta ticks).
                        // Only recompute when enough time has elapsed (guard against
                        // double-call from accel+gyro HID callbacks firing back-to-back).
                        smoothed_dt_ms = smoothed_dt_ms * 0.85f + dt_ms * 0.15f;
                        auto elapsed = std::chrono::duration<float, std::milli>(
                            now - last_rate_compute).count();
                        // Fixed 5 ms guard: prevents double-compute from accel+gyro
                        // callbacks that fire back-to-back (~0.3 ms apart).
                        if (elapsed >= 5.0f) {
                            last_rate_compute = now;
                            auto diff = rate_delta - last_rate_delta;
                            last_rate_delta = rate_delta;
                            float rate_scale =
                                sensitivity * 1.0f * (1000.0f / smoothed_dt_ms);
                            float pitch_s = invert_pitch ? -1.0f : 1.0f;
                            float yaw_s = invert_yaw ? 1.0f : -1.0f;
                            auto instant_rate = Common::Vec3<float>{
                                pitch_s * diff.y * rate_scale,
                                0.0f,
                                yaw_s * diff.x * rate_scale,
                            };
                            // EMA-smooth the angular rate to eliminate integer-pixel
                            // stepping from the 5-ms AddDelta tick granularity.
                            if (smoothed_rate.Length2() < 0.001f) {
                                smoothed_rate = instant_rate;
                            } else {
                                smoothed_rate = smoothed_rate * 0.75f +
                                                instant_rate * 0.25f;
                            }
                        }
                        auto angular_rate = smoothed_rate;
                        float raw_rate_x = angular_rate.x; // gate: pre-clamp rate

                        // Clamp pitch rotation to ±90° to prevent 3DS view flipping
                        if (clamp_pitch_180) {
                            float dt_sec = smoothed_dt_ms / 1000.0f;
                            float new_pitch = accumulated_pitch + angular_rate.x * dt_sec;
                            if (new_pitch > 90.0f) {
                                angular_rate.x = (90.0f - accumulated_pitch) / dt_sec;
                                accumulated_pitch = 90.0f;
                            } else if (new_pitch < -90.0f) {
                                angular_rate.x = (-90.0f - accumulated_pitch) / dt_sec;
                                accumulated_pitch = -90.0f;
                            } else {
                                accumulated_pitch = new_pitch;
                            }
                        }

                        float effective_tilt_rad = default_tilt * Common::PI / 180.0f;
                        // Auto Y-tilt: accumulate pitch from angular rate
                        if (auto_tilt_y) {
                            float dt_sec = smoothed_dt_ms / 1000.0f;
                            float sign_auto_y = auto_tilt_y_invert ? 1.0f : -1.0f;
                            float delta = sign_auto_y * angular_rate.x * dt_sec *
                                          Common::PI / 180.0f;
                            float new_atp = accumulated_tilt_pitch + delta;
                            // User-configured limits (ClampAutoTiltPitch handles max angle + prevent_flip)
                            new_atp = ClampAutoTiltPitch(new_atp);
                            // If clamped, scale angular_rate.x to match the actual delta
                            if (std::abs(new_atp - accumulated_tilt_pitch - delta) > 0.00001f) {
                                if (std::abs(delta) > 0.00001f)
                                    angular_rate.x *=
                                        (new_atp - accumulated_tilt_pitch) / delta;
                                else
                                    angular_rate.x = 0.0f;
                            }
                            accumulated_tilt_pitch = new_atp;
                            // Decay when user stops: adjust both gravity AND angular rate
                            if (auto_tilt_y_return_speed > 0.0f && std::abs(raw_rate_x) < 0.5f) {
                                float step = auto_tilt_y_return_speed * Common::PI / 180.0f * dt_sec;
                                float old_atp = accumulated_tilt_pitch;
                                if (accumulated_tilt_pitch > 0.0f)
                                    accumulated_tilt_pitch = std::max(0.0f, accumulated_tilt_pitch - step);
                                else if (accumulated_tilt_pitch < 0.0f)
                                    accumulated_tilt_pitch = std::min(0.0f, accumulated_tilt_pitch + step);
                                // Compensate angular_rate so the game camera rotates back
                                // by the same amount that gravity changed
                                float delta_atp = accumulated_tilt_pitch - old_atp;
                                float sign_auto_y = auto_tilt_y_invert ? 1.0f : -1.0f;
                                if (std::abs(sign_auto_y) > 0.0001f)
                                    angular_rate.x += delta_atp * (180.0f / Common::PI) / dt_sec / sign_auto_y;
                            }
                            effective_tilt_rad += accumulated_tilt_pitch;
                        }
                        // Decay accumulated_pitch toward 0 when idle (prevents permanent lock)
                        if (std::abs(raw_rate_x) < 0.5f) {
                            float dt_sec = smoothed_dt_ms / 1000.0f;
                            float decay = std::min(30.0f * dt_sec, 1.0f);
                            accumulated_pitch *= (1.0f - decay);
                            if (std::abs(accumulated_pitch) < 0.1f) accumulated_pitch = 0.0f;
                        }
                        auto gravity =
                            Common::MakeVec(0.0f, -std::cos(effective_tilt_rad),
                                            -std::sin(effective_tilt_rad));

                        // [BETA] Auto X-tilt: apply smoothed roll around Z axis
                        if (auto_tilt_x) {
                            float decay = (auto_roll_target == 0.0f) ? 0.90f : 0.70f;
                            auto_roll = auto_roll * decay + auto_roll_target * (1.0f - decay);
                            float rr = auto_roll * Common::PI / 180.0f;
                            float cr = std::cos(rr);
                            float sr = std::sin(rr);
                            float gx2 = gravity.x * cr - gravity.y * sr;
                            float gy2 = gravity.x * sr + gravity.y * cr;
                            gravity = Common::MakeVec(gx2, gy2, gravity.z);
                        }

                        {
                            std::lock_guard status_guard{status_mutex};
                            status = std::make_tuple(gravity, angular_rate);
                        }
                    }
                }
                // per_frame: rate_delta is managed via diff tracking (last_rate_delta).
                // Do NOT reset here — let AddDelta continue accumulating.
            }
        }
        std::lock_guard guard{status_mutex};
        return status;
    }

private:
    const int update_millisecond;
    const std::chrono::steady_clock::duration update_duration;
    const float sensitivity;
    const float tilt_clamp;
    const MotionEmuMode mode;
    const float deadzone;
    const float default_tilt;
    const float tilt_max_angle;
    const bool invert_pitch;
    const bool invert_yaw;
    const bool per_frame;
    const bool clamp_pitch_180;
    const bool auto_tilt_y;
    const bool auto_tilt_y_invert;
    const bool auto_tilt_x;
    const float auto_tilt_speed;
    const float auto_tilt_y_return_speed;
    const int auto_tilt_y_max_angle;
    const bool auto_tilt_y_prevent_flip;

    Common::Vec2<int> mouse_origin;

    std::mutex tilt_mutex;
    Common::Vec2<float> tilt_direction;
    float tilt_angle = 0;
    Common::Vec2<float> rate_delta{};
    Common::Vec2<float> last_rate_delta{}; // per_frame diff tracking
    Common::Vec3<float> smoothed_rate{};    // EMA-smoothed angular rate for per_frame
    float smoothed_dt_ms = 10.0f;            // EMA-smoothed frame interval (initial guess)
    std::chrono::steady_clock::time_point last_rate_compute;
    float accumulated_pitch = 0.0f;          // Track pitch rotation for clamp_pitch_180
    float accumulated_tilt_pitch = 0.0f;     // Auto Y-tilt: accumulated pitch angle (radians)
    Common::Quaternion<float> q_accumulated; // Auto Y-tilt: persistent orientation quaternion
    float auto_roll = 0.0f;                  // [BETA] Auto X-tilt: current roll angle (degrees)
    float auto_roll_target = 0.0f;           // [BETA] Auto X-tilt: target roll angle (degrees)
    float tilt_offset_yaw = 0.0f;
    float tilt_offset_pitch = 0.0f;
    std::chrono::steady_clock::time_point last_mouse_input_time;
    bool is_active = false;

    // Clamp accumulated_tilt_pitch to user-configured limits
    // Works correctly regardless of which direction accumulated corresponds to "up"
    float ClampAutoTiltPitch(float val) const {
        // 1. Max angle: ±min(auto_tilt_y_max_angle, tilt_max_angle) — symmetric
        float at_max = auto_tilt_y_max_angle * Common::PI / 180.0f;
        float tm_max = tilt_max_angle * Common::PI / 180.0f;
        float max_rad = std::min(at_max, tm_max);
        val = std::clamp(val, -max_rad, max_rad);
        // 2. Prevent flip: keep effective = default_tilt + val within [0°, 180°]
        //    effective = dt_rad + val ∈ [0, π]  →  val ∈ [−dt_rad, π−dt_rad]
        if (auto_tilt_y_prevent_flip) {
            float dt_rad = default_tilt * Common::PI / 180.0f;
            float lo = -dt_rad;
            float hi = Common::PI - dt_rad;
            val = std::clamp(val, lo, hi);
        }
        return val;
    }

    Common::Event shutdown_event;

    std::tuple<Common::Vec3<float>, Common::Vec3<float>> status;
    std::mutex status_mutex;

    std::chrono::steady_clock::time_point last_status_time;

    // Note: always keep the thread declaration at the end so that other objects are initialized
    // before this!
    std::thread motion_emu_thread;

    void MotionEmuThread() {
        auto update_time = std::chrono::steady_clock::now();
        Common::Quaternion<float> q = Common::MakeQuaternion(Common::Vec3<float>(), 0);
        Common::Quaternion<float> old_q;

        while (!shutdown_event.WaitUntil(update_time)) {
            update_time += update_duration;

            if (mode == MotionEmuMode::Absolute) {
                old_q = q;
                {
                    std::lock_guard guard{tilt_mutex};
                    q = Common::MakeQuaternion(
                        Common::MakeVec(-tilt_direction.y, 0.0f, tilt_direction.x), tilt_angle);
                }

                // Auto Y-tilt: accumulate per-frame rotation delta so gravity persists
                if (auto_tilt_y) {
                    float sign = auto_tilt_y_invert ? 1.0f : -1.0f;
                    // q and old_q are the per-frame rotation quaternions;
                    // delta_q = q * old_q.Inverse() gives the frame-to-frame change.
                    // Only accumulate when there is actual rotation (prevents repeat adds when
                    // mouse is held still with constant tilt_angle).
                    if (tilt_angle > 0.0f) {
                        auto delta_q = q * old_q.Inverse();
                        if (sign < 0.0f) {
                            delta_q = delta_q.Inverse();
                        }
                        q_accumulated = delta_q * q_accumulated;
                        q_accumulated = q_accumulated.Normalized();
                    }
                }

                auto inv_q = (auto_tilt_y ? q_accumulated : q).Inverse();
                auto gravity = Common::MakeVec(0.0f, -1.0f, 0.0f);
                auto angular_rate = ((q - old_q) * q.Inverse()).xyz * 2;
                angular_rate *= 1000.0f / update_millisecond / Common::PI * 180.0f;
                gravity = QuaternionRotate(inv_q, gravity);
                angular_rate = QuaternionRotate(q.Inverse(), angular_rate);

                // [BETA] Auto X-tilt: apply smoothed roll around Z (forward) axis
                if (auto_tilt_x) {
                    float decay = (auto_roll_target == 0.0f) ? 0.90f : 0.70f;
                    auto_roll = auto_roll * decay + auto_roll_target * (1.0f - decay);
                    float rr = auto_roll * Common::PI / 180.0f;
                    float cr = std::cos(rr);
                    float sr = std::sin(rr);
                    float gx2 = gravity.x * cr - gravity.y * sr;
                    float gy2 = gravity.x * sr + gravity.y * cr;
                    gravity = Common::MakeVec(gx2, gy2, gravity.z);
                }

                {
                    std::lock_guard guard{status_mutex};
                    status = std::make_tuple(gravity, angular_rate);
                }
            } else if (!per_frame) {
                // ---- Thread-based non-Absolute modes ----
                if (mode == MotionEmuMode::TiltContinuous ||
                    mode == MotionEmuMode::TiltHold) {
                    // Tilt mode: gravity from mouse offset
                    float yaw, pitch;
                    {
                        std::lock_guard guard{tilt_mutex};
                        yaw = tilt_offset_yaw;
                        pitch = tilt_offset_pitch;
                    }
                    float effective_tilt_rad = default_tilt * Common::PI / 180.0f;
                    // Auto Y-tilt: accumulate pitch from offset changes
                    if (auto_tilt_y) {
                        float sign = auto_tilt_y_invert ? 1.0f : -1.0f;
                        accumulated_tilt_pitch += sign * pitch;
                        accumulated_tilt_pitch = ClampAutoTiltPitch(accumulated_tilt_pitch);
                        // Only decay when mouse has been idle for a while
                        auto since_input = std::chrono::duration<float, std::milli>(
                            std::chrono::steady_clock::now() - last_mouse_input_time).count();
                        if (auto_tilt_y_return_speed > 0.0f && since_input > 50.0f) {
                            float step = auto_tilt_y_return_speed * Common::PI / 180.0f *
                                         (update_millisecond / 1000.0f);
                            if (accumulated_tilt_pitch > 0.0f)
                                accumulated_tilt_pitch = std::max(0.0f, accumulated_tilt_pitch - step);
                            else if (accumulated_tilt_pitch < 0.0f)
                                accumulated_tilt_pitch = std::min(0.0f, accumulated_tilt_pitch + step);
                        }
                        effective_tilt_rad += accumulated_tilt_pitch;
                    }
                    auto gravity =
                        Common::MakeVec(0.0f, -std::cos(effective_tilt_rad),
                                        -std::sin(effective_tilt_rad));
                    float cy = std::cos(yaw);
                    float sy = std::sin(yaw);
                    float gx = gravity.x * cy - gravity.z * sy;
                    float gz = gravity.x * sy + gravity.z * cy;
                    float cp = std::cos(pitch);
                    float sp = std::sin(pitch);
                    gravity = Common::MakeVec(gx, gravity.y * cp + gz * sp,
                                              -gravity.y * sp + gz * cp);
                    // [BETA] Auto X-tilt: apply smoothed roll around Z axis
                    if (auto_tilt_x) {
                        float decay = (auto_roll_target == 0.0f) ? 0.90f : 0.70f;
                        auto_roll = auto_roll * decay + auto_roll_target * (1.0f - decay);
                        float rr = auto_roll * Common::PI / 180.0f;
                        float cr = std::cos(rr);
                        float sr = std::sin(rr);
                        float gx2 = gravity.x * cr - gravity.y * sr;
                        float gy2 = gravity.x * sr + gravity.y * cr;
                        gravity = Common::MakeVec(gx2, gy2, gravity.z);
                    }
                    {
                        std::lock_guard guard{status_mutex};
                        status = std::make_tuple(gravity, Common::Vec3<float>{});
                    }
                } else {
                    // Rate mode: mouse delta to angular velocity
                    Common::Vec3<float> angular_rate{};
                    {
                        std::lock_guard guard{tilt_mutex};
                        if (is_active) {
                            float rate_scale =
                                sensitivity * 1.0f * (1000.0f / update_millisecond);
                            float pitch_s = invert_pitch ? -1.0f : 1.0f;
                            float yaw_s = invert_yaw ? 1.0f : -1.0f;
                            angular_rate.x = pitch_s * rate_delta.y * rate_scale;
                            angular_rate.y = 0.0f;
                            angular_rate.z = yaw_s * rate_delta.x * rate_scale;
                        }
                        rate_delta = Common::Vec2<float>{};
                    }

                    float raw_rate_x = angular_rate.x; // gate: pre-clamp rate

                    // Clamp pitch rotation to ±90° to prevent 3DS view flipping
                    if (clamp_pitch_180) {
                        float dt_sec = update_millisecond / 1000.0f;
                        float new_pitch = accumulated_pitch + angular_rate.x * dt_sec;
                        if (new_pitch > 90.0f) {
                            angular_rate.x = (90.0f - accumulated_pitch) / dt_sec;
                            accumulated_pitch = 90.0f;
                        } else if (new_pitch < -90.0f) {
                            angular_rate.x = (-90.0f - accumulated_pitch) / dt_sec;
                            accumulated_pitch = -90.0f;
                        } else {
                            accumulated_pitch = new_pitch;
                        }
                    }

                    float effective_tilt_rad = default_tilt * Common::PI / 180.0f;
                    // Auto Y-tilt: accumulate pitch from angular rate
                    if (auto_tilt_y) {
                        float dt_sec = update_millisecond / 1000.0f;
                        float sign = auto_tilt_y_invert ? 1.0f : -1.0f;
                        float delta = sign * angular_rate.x * dt_sec * Common::PI / 180.0f;
                        float new_atp = accumulated_tilt_pitch + delta;
                        // User-configured limits (ClampAutoTiltPitch handles max angle + prevent_flip)
                        new_atp = ClampAutoTiltPitch(new_atp);
                        // If clamped, scale angular_rate.x to match the actual delta
                        if (std::abs(new_atp - accumulated_tilt_pitch - delta) > 0.00001f) {
                            if (std::abs(delta) > 0.00001f)
                                angular_rate.x *= (new_atp - accumulated_tilt_pitch) / delta;
                            else
                                angular_rate.x = 0.0f;
                        }
                        accumulated_tilt_pitch = new_atp;
                        // Decay: adjust both gravity AND angular rate for camera sync
                        if (auto_tilt_y_return_speed > 0.0f && std::abs(raw_rate_x) < 0.5f) {
                            float step = auto_tilt_y_return_speed * Common::PI / 180.0f * dt_sec;
                            float old_atp = accumulated_tilt_pitch;
                            if (accumulated_tilt_pitch > 0.0f)
                                accumulated_tilt_pitch = std::max(0.0f, accumulated_tilt_pitch - step);
                            else if (accumulated_tilt_pitch < 0.0f)
                                accumulated_tilt_pitch = std::min(0.0f, accumulated_tilt_pitch + step);
                            float delta_atp = accumulated_tilt_pitch - old_atp;
                            float sign_auto_y = auto_tilt_y_invert ? 1.0f : -1.0f;
                            if (std::abs(sign_auto_y) > 0.0001f)
                                angular_rate.x += delta_atp * (180.0f / Common::PI) / dt_sec / sign_auto_y;
                        }
                        effective_tilt_rad += accumulated_tilt_pitch;
                    }
                    // Decay accumulated_pitch toward 0 when idle (prevents permanent lock)
                    if (std::abs(raw_rate_x) < 0.5f) {
                        float dt_sec = update_millisecond / 1000.0f;
                        float decay = std::min(30.0f * dt_sec, 1.0f);
                        accumulated_pitch *= (1.0f - decay);
                        if (std::abs(accumulated_pitch) < 0.1f) accumulated_pitch = 0.0f;
                    }
                    auto gravity =
                        Common::MakeVec(0.0f, -std::cos(effective_tilt_rad),
                                        -std::sin(effective_tilt_rad));

                    // [BETA] Auto X-tilt: apply smoothed roll around Z axis
                    if (auto_tilt_x) {
                        float decay = (auto_roll_target == 0.0f) ? 0.90f : 0.70f;
                        auto_roll = auto_roll * decay + auto_roll_target * (1.0f - decay);
                        float rr = auto_roll * Common::PI / 180.0f;
                        float cr = std::cos(rr);
                        float sr = std::sin(rr);
                        float gx2 = gravity.x * cr - gravity.y * sr;
                        float gy2 = gravity.x * sr + gravity.y * cr;
                        gravity = Common::MakeVec(gx2, gy2, gravity.z);
                    }

                    {
                        std::lock_guard guard{status_mutex};
                        status = std::make_tuple(gravity, angular_rate);
                    }
                }
            }
        }
    }

};

// Interface wrapper held by input receiver as a unique_ptr. It holds the implementation class as
// a shared_ptr, which is also observed by the factory class as a weak_ptr. In this way the factory
// can forward all the inputs to the implementation only when it is valid.
class MotionEmuDeviceWrapper : public Input::MotionDevice {
public:
    MotionEmuDeviceWrapper(int update_millisecond, float sensitivity, float tilt_clamp,
                           MotionEmuMode mode, float deadzone, float default_tilt,
                           float tilt_max_angle,
                           bool invert_pitch, bool invert_yaw, bool per_frame,
                           bool clamp_pitch_180,
                           bool auto_tilt_y, bool auto_tilt_y_invert, bool auto_tilt_x,
                           float auto_tilt_speed, float auto_tilt_y_return_speed,
                           int auto_tilt_y_max_angle, bool auto_tilt_y_prevent_flip) {
        device = std::make_shared<MotionEmuDevice>(update_millisecond, sensitivity, tilt_clamp,
                                                    mode, deadzone, default_tilt,
                                                    tilt_max_angle,
                                                    invert_pitch, invert_yaw, per_frame,
                                                    clamp_pitch_180,
                                                    auto_tilt_y, auto_tilt_y_invert, auto_tilt_x,
                                                    auto_tilt_speed, auto_tilt_y_return_speed,
                                                    auto_tilt_y_max_angle,
                                                    auto_tilt_y_prevent_flip);
    }

    bool IsDeviceActive() const {
        return device->IsActive();
    }

    // TiltDelta: overwrite (RateHold center-warp). Use AddDelta for accumulation.
    void TiltDelta(float dx, float dy) {
        device->TiltDelta(dx, dy);
    }

    void AddDelta(float dx, float dy) {
        device->AddDelta(dx, dy);
    }

    void SetTiltOffset(float yaw_rad, float pitch_rad) {
        device->SetTiltOffset(yaw_rad, pitch_rad);
    }

    float GetTiltMaxAngle() const {
        return device->GetTiltMaxAngle();
    }

    void SetAutoRollTarget(float dx) {
        device->SetAutoRollTarget(dx);
    }

    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
        return device->GetStatus();
    }

    void GetInvertFlags(bool& out_pitch, bool& out_yaw) const {
        device->GetInvertFlags(out_pitch, out_yaw);
    }

    std::shared_ptr<MotionEmuDevice> device;
};

// Parse mode string to enum
static MotionEmuMode ParseMotionMode(const std::string& mode_str) {
    if (mode_str == "rate_hold")
        return MotionEmuMode::RateHold;
    if (mode_str == "rate_continuous")
        return MotionEmuMode::RateContinuous;
    if (mode_str == "tilt_continuous")
        return MotionEmuMode::TiltContinuous;
    if (mode_str == "tilt_hold")
        return MotionEmuMode::TiltHold;
    return MotionEmuMode::Absolute; // default
}

std::unique_ptr<Input::MotionDevice> MotionEmu::Create(const Common::ParamPackage& params) {
    int update_period = params.Get("update_period", 20);
    float sensitivity = params.Get("sensitivity", 0.05f);
    float tilt_clamp = params.Get("tilt_clamp", 90.0f);
    float deadzone = params.Get("deadzone", 0.0f);
    float default_tilt = params.Get("default_tilt", 90.0f);
    float tilt_max_angle = params.Get("tilt_max_angle", 90.0f);
    bool invert_pitch = params.Get("invert_pitch", true);
    bool invert_yaw = params.Get("invert_yaw", false);
    bool per_frame = params.Get("per_frame", true);
    bool clamp_pitch_180 = params.Get("clamp_pitch_180", true);
    bool auto_tilt_y = params.Get("auto_tilt_y", true);
    bool auto_tilt_y_invert = params.Get("auto_tilt_y_invert", false);
    bool auto_tilt_x = params.Get("auto_tilt_x", false);
    float auto_tilt_speed = params.Get("auto_tilt_speed", 1.0f);
    float auto_tilt_y_return_speed = params.Get("auto_tilt_y_return_speed", 0.0f);
    int auto_tilt_y_max_angle = params.Get("auto_tilt_y_max_angle", 180);
    bool auto_tilt_y_prevent_flip = params.Get("auto_tilt_y_prevent_flip", true);
    MotionEmuMode mode = ParseMotionMode(params.Get("mode", "absolute"));

    current_mode = mode;

    auto device_wrapper = std::make_unique<MotionEmuDeviceWrapper>(update_period, sensitivity,
                                                                    tilt_clamp, mode, deadzone,
                                                                    default_tilt,
                                                                    tilt_max_angle,
                                                                    invert_pitch, invert_yaw,
                                                                    per_frame, clamp_pitch_180,
                                                                    auto_tilt_y, auto_tilt_y_invert,
                                                                    auto_tilt_x, auto_tilt_speed,
                                                                    auto_tilt_y_return_speed,
                                                                    auto_tilt_y_max_angle,
                                                                    auto_tilt_y_prevent_flip);
    // Previously created device is disconnected here. Having two motion devices for 3DS is not
    // expected.
    current_device = device_wrapper->device;
    return std::move(device_wrapper);
}

MotionEmuMode MotionEmu::GetMode() const {
    return current_mode;
}

void MotionEmu::BeginTilt(int x, int y) {
    if (auto ptr = current_device.lock()) {
        ptr->BeginTilt(x, y);
    }
}

void MotionEmu::Tilt(int x, int y) {
    if (auto ptr = current_device.lock()) {
        ptr->Tilt(x, y);
    }
}

void MotionEmu::EndTilt() {
    if (auto ptr = current_device.lock()) {
        ptr->EndTilt();
    }
}

void MotionEmu::ToggleActive() {
    if (auto ptr = current_device.lock()) {
        ptr->ToggleActive();
    }
}

void MotionEmu::SetActive(bool active) {
    if (auto ptr = current_device.lock()) {
        ptr->SetActive(active);
    }
}

bool MotionEmu::IsDeviceActive() const {
    if (auto ptr = current_device.lock()) {
        return ptr->IsActive();
    }
    return false;
}

void MotionEmu::TiltDelta(float dx, float dy) {
    if (auto ptr = current_device.lock()) {
        ptr->TiltDelta(dx, dy);
    }
}

void MotionEmu::AddDelta(float dx, float dy) {
    if (auto ptr = current_device.lock()) {
        ptr->AddDelta(dx, dy);
    }
}

void MotionEmu::SetTiltOffset(float yaw_rad, float pitch_rad) {
    if (auto ptr = current_device.lock()) {
        ptr->SetTiltOffset(yaw_rad, pitch_rad);
    }
}

void MotionEmu::GetInvertFlags(bool& out_pitch, bool& out_yaw) const {
    if (auto ptr = current_device.lock()) {
        ptr->GetInvertFlags(out_pitch, out_yaw);
    } else {
        out_pitch = false;
        out_yaw = false;
    }
}

float MotionEmu::GetTiltMaxAngle() const {
    if (auto ptr = current_device.lock()) {
        return ptr->GetTiltMaxAngle();
    }
    return 90.0f;
}

void MotionEmu::SetAutoRollTarget(float dx) {
    if (auto ptr = current_device.lock()) {
        ptr->SetAutoRollTarget(dx);
    }
}

} // namespace InputCommon
