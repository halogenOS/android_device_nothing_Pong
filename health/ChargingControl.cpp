/*
 * Copyright (C) 2026 The halogenOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ChargingControl.h"

#include <android-base/file.h>
#include <android-base/logging.h>

#include <chrono>

namespace aidl::vendor::lineage::health {

// Fast-charge-current scenario limit (mA). The firmware honours this for both
// wired and wireless charging, so writing 0 stops the charge current while the
// charger stays connected -- unlike wls_en, which severs the wireless link.
// Shared with the vendor thermal daemon, so it must be re-asserted.
static constexpr const char* kScenarioFccPath =
        "/sys/class/qcom-battery/scenario_fcc";
static constexpr const char* kFccStop = "0";
// Max entry of the vendor current table: effectively unrestricted; the firmware
// (and the thermal daemon, once it resumes) clamps to what the source allows.
static constexpr const char* kFccResume = "9000";
static constexpr auto kReassertInterval = std::chrono::milliseconds(500);

// OEM charge-enable. scenario_fcc=0 already stops wired current, but this also
// makes the firmware report NOT_CHARGING, which the framework's recharge margin
// (the limit-4% hysteresis) depends on. Wireless has no equivalent gate that
// keeps the link, so it relies on scenario_fcc alone.
static constexpr const char* kUsbChargingEnabledPath =
        "/sys/class/qcom-battery/usb_charger_en";
static constexpr const char* kUsbDisable = "0";
static constexpr const char* kUsbEnable = "1";

ChargingControl::~ChargingControl() {
    stopLimit();
}

void ChargingControl::startLimit() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mLimitActive.exchange(true)) return;
    if (!android::base::WriteStringToFile(kUsbDisable, kUsbChargingEnabledPath, true))
        LOG(ERROR) << "Failed to write " << kUsbChargingEnabledPath;
    mLimitThread = std::thread([this] {
        while (mLimitActive.load()) {
            if (!android::base::WriteStringToFile(kFccStop, kScenarioFccPath, true))
                LOG(ERROR) << "Failed to write " << kScenarioFccPath;
            std::this_thread::sleep_for(kReassertInterval);
        }
    });
}

void ChargingControl::stopLimit() {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mLimitActive.exchange(false)) return;
    if (mLimitThread.joinable()) mLimitThread.join();
    if (!android::base::WriteStringToFile(kFccResume, kScenarioFccPath, true))
        LOG(ERROR) << "Failed to write " << kScenarioFccPath;
    if (!android::base::WriteStringToFile(kUsbEnable, kUsbChargingEnabledPath, true))
        LOG(ERROR) << "Failed to write " << kUsbChargingEnabledPath;
}

ndk::ScopedAStatus ChargingControl::getChargingEnabled(bool* _aidl_return) {
    *_aidl_return = !mLimitActive.load();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ChargingControl::setChargingEnabled(bool enabled) {
    if (enabled) {
        stopLimit();
    } else {
        startLimit();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ChargingControl::setChargingDeadline(int64_t) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ChargingControl::getChargingDeadline(int64_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ChargingControl::getChargingLimit(ChargingLimitInfo*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ChargingControl::setChargingLimit(const ChargingLimitInfo&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ChargingControl::getSupportedMode(int* _aidl_return) {
    *_aidl_return = static_cast<int>(ChargingControlSupportedMode::TOGGLE);
    return ndk::ScopedAStatus::ok();
}

binder_status_t ChargingControl::dump(int fd, const char**, uint32_t) {
    dprintf(fd, "Charging enabled: %s\n", mLimitActive.load() ? "false" : "true");
    dprintf(fd, "Supported mode: TOGGLE\n");
    dprintf(fd, "Limit active: %s\n", mLimitActive.load() ? "true" : "false");
    dprintf(fd, "FCC node: %s (stop=%s, resume=%s)\n",
            kScenarioFccPath, kFccStop, kFccResume);
    dprintf(fd, "USB gate node: %s\n", kUsbChargingEnabledPath);
    return STATUS_OK;
}

}  // namespace aidl::vendor::lineage::health
