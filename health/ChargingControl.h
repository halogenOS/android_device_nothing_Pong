/*
 * Copyright (C) 2026 The halogenOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/lineage/health/BnChargingControl.h>
#include <aidl/vendor/lineage/health/ChargingControlSupportedMode.h>
#include <aidl/vendor/noth/hardware/charge/ICharge.h>

#include <atomic>
#include <memory>
#include <thread>

namespace aidl::vendor::lineage::health {

struct ChargingControl : public BnChargingControl {
    ndk::ScopedAStatus getChargingEnabled(bool* _aidl_return) override;
    ndk::ScopedAStatus setChargingEnabled(bool enabled) override;
    ndk::ScopedAStatus setChargingDeadline(int64_t deadline) override;
    ndk::ScopedAStatus getSupportedMode(int* _aidl_return) override;
    ndk::ScopedAStatus getChargingDeadline(int64_t* _aidl_return) override;
    ndk::ScopedAStatus getChargingLimit(ChargingLimitInfo* _aidl_return) override;
    ndk::ScopedAStatus setChargingLimit(const ChargingLimitInfo& limit) override;

    binder_status_t dump(int fd, const char** args, uint32_t numArgs) override;

  private:
    // Casts a battery-health FCC vote on the vendor charge daemon. The daemon
    // owns scenario_fcc and recomputes it as min() of all voters, so voting 0
    // makes it hold the current at zero itself -- no sysfs race, and the thermal
    // voter is left intact.
    std::shared_ptr<::aidl::vendor::noth::hardware::charge::ICharge> getCharge();
    void voteChargeFcc(int milliamps);

    // The battery-side stop (BATT_CHG_CTRL_LIM=0) is reset by a contending vendor
    // writer between the framework's sparse calls, so charging resumes in the gaps.
    // Hold it down by re-asserting it from a loop while the wireless limit is active.
    void startRestrictReassert();
    void stopRestrictReassert();

    std::shared_ptr<::aidl::vendor::noth::hardware::charge::ICharge> mCharge;
    std::atomic<bool> mLimitActive{false};
    std::atomic<bool> mReassert{false};
    // Latched when the fake-vbat probe misbehaves; blocks retries until reboot.
    std::atomic<bool> mFakeVbatUnsafe{false};
    std::thread mReassertThread;
};

}  // namespace aidl::vendor::lineage::health
