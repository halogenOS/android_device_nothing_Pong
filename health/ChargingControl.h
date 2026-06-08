/*
 * Copyright (C) 2026 The halogenOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/lineage/health/BnChargingControl.h>
#include <aidl/vendor/lineage/health/ChargingControlSupportedMode.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace aidl::vendor::lineage::health {

struct ChargingControl : public BnChargingControl {
    ChargingControl() = default;
    ~ChargingControl();

    ndk::ScopedAStatus getChargingEnabled(bool* _aidl_return) override;
    ndk::ScopedAStatus setChargingEnabled(bool enabled) override;
    ndk::ScopedAStatus setChargingDeadline(int64_t deadline) override;
    ndk::ScopedAStatus getSupportedMode(int* _aidl_return) override;
    ndk::ScopedAStatus getChargingDeadline(int64_t* _aidl_return) override;
    ndk::ScopedAStatus getChargingLimit(ChargingLimitInfo* _aidl_return) override;
    ndk::ScopedAStatus setChargingLimit(const ChargingLimitInfo& limit) override;

    binder_status_t dump(int fd, const char** args, uint32_t numArgs) override;

  private:
    // scenario_fcc is shared with the vendor thermal daemon, which periodically
    // rewrites it. While the limit is engaged we hold it at 0 in a re-assert
    // loop so the daemon can't quietly resume charging behind us.
    void startLimit();
    void stopLimit();

    std::mutex mLock;
    std::atomic<bool> mLimitActive{false};
    std::thread mLimitThread;
};

}  // namespace aidl::vendor::lineage::health
