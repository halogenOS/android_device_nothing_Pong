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

    // We report LIMIT mode, so the framework hands us the band once and stops
    // toggling -- the enforcement runs entirely here, off a self-contained loop
    // that drives the band against the live battery capacity. This is the whole
    // point of the rework: nothing external (a doze-throttled framework, a
    // renegotiation-induced spurious enable) can ever stop the hold, which is
    // what previously let the battery run to 100%.
    void startEnforcement();
    void stopEnforcement();
    // Resume charging on every front: idempotent, used to recharge below the
    // band and to fully let go when the limit is removed.
    void releaseCharging();

    std::shared_ptr<::aidl::vendor::noth::hardware::charge::ICharge> mCharge;

    std::atomic<bool> mEnforcing{false};
    std::thread mEnforceThread;
    // The band handed to us by the framework via setChargingLimit (max = the
    // configured limit, min = limit - recharge margin).
    std::atomic<int> mMax{100};
    std::atomic<int> mMin{0};
    // True while we are actively holding at the top of the band (vs charging up
    // to it); drives getChargingEnabled() and the dump.
    std::atomic<bool> mHolding{false};
    // Set if the fake-vbat probe misbehaves; backs it off for the current
    // wireless engagement only (reset on each fresh engage).
    std::atomic<bool> mFakeVbatUnsafe{false};
};

}  // namespace aidl::vendor::lineage::health
