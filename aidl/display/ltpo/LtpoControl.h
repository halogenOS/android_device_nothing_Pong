/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 The halogenOS Project
 *
 * LTPO Control HAL for Nothing Phone 2 (Pong)
 * Panel: NT37705 Visionox AMOLED, LTPO 1-120Hz
 */

#pragma once

#include <aidl/custom/hardware/display/ltpo/BnLtpoControl.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace aidl::custom::hardware::display::ltpo {

class LtpoControl : public BnLtpoControl {
  public:
    LtpoControl();
    ~LtpoControl();

    ndk::ScopedAStatus setEnabled(bool enabled) override;
    ndk::ScopedAStatus onIdle() override;
    ndk::ScopedAStatus onActive() override;
    ndk::ScopedAStatus getCurrentHz(int32_t* _aidl_return) override;
    ndk::ScopedAStatus setTargetHz(int32_t targetHz, int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedHz(std::vector<int32_t>* _aidl_return) override;

  private:
    static constexpr const char* kSysfsPath = "/sys/panel_feature/skip_frame_mode";

    // NT37705 SFM no-dimming indices
    struct SfmStep {
        int hz;
        int sfmIndex;
    };
    static constexpr SfmStep kSteps[] = {
        {1, 11},     // 1Hz idle
        {10, 10},    // 10Hz
        {24, 9},     // 24Hz (movies)
        {30, 8},     // 30Hz
        {60, 7},     // 60Hz (videos)
        {120, 16},   // 120Hz (disable SFM)
    };

    bool writeSfm(int index);
    bool isVrrMode() const;

    using Clock = std::chrono::steady_clock;
    static constexpr auto kBoostHold = std::chrono::milliseconds(200);

    int mSfmFd = -1;
    std::atomic<bool> mEnabled{false};
    std::atomic<int> mCurrentHz{120};
    Clock::time_point mBoostUntil{};
    std::mutex mLock;
};

}  // namespace aidl::custom::hardware::display::ltpo
