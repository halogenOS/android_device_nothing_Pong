/*
 * Copyright (C) 2026 The halogenOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ChargingControl.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>

namespace aidl::vendor::lineage::health {

using ::aidl::vendor::noth::hardware::charge::ICharge;

// The vendor charge daemon (vendor.noth.hardware.charge / "NtChgAidl") owns
// scenario_fcc and continuously rewrites it from a min()-of-voters table. Racing
// it with raw sysfs writes leaks charge current, so instead we cast a vote on its
// battery-health FCC voter: setChargeFcc(<voter>, <mA>). Voting 0 makes the
// daemon hold scenario_fcc at 0; the thermal voter is untouched.
// Voter ids recovered from the daemon: 1=NT_CHG, 2=BATTERY_HEALTH, 3=TEMPERATURE.
static constexpr int kFccVoterBatteryHealth = 2;
static constexpr int kFccStopMilliamps = 0;
static constexpr int kFccResumeMilliamps = 9000;  // non-binding; thermal voter governs

// Wired limiting uses the OEM charge-enable gate: it stops current and makes the
// firmware report NOT_CHARGING, which the framework's recharge margin (the
// limit-4% hysteresis) depends on. The FCC vote is reserved for wireless, which
// has no gate that stops current while keeping the Rx link.
static constexpr const char* kUsbChargingEnabledPath =
        "/sys/class/qcom-battery/usb_charger_en";

// Picks which mechanism to use; the FCC vote is wireless-only by design.
static constexpr const char* kWirelessOnlinePath =
        "/sys/class/power_supply/wireless/online";

static bool wirelessOnline() {
    std::string content;
    return android::base::ReadFileToString(kWirelessOnlinePath, &content, true) &&
           android::base::Trim(content) == "1";
}

std::shared_ptr<ICharge> ChargingControl::getCharge() {
    if (mCharge) return mCharge;
    const auto name = std::string(ICharge::descriptor) + "/default";
    if (!AServiceManager_isDeclared(name.c_str())) {
        LOG(ERROR) << "ICharge service not declared";
        return nullptr;
    }
    mCharge = ICharge::fromBinder(
            ndk::SpAIBinder(AServiceManager_waitForService(name.c_str())));
    if (!mCharge) LOG(ERROR) << "Failed to bind ICharge service";
    return mCharge;
}

void ChargingControl::voteChargeFcc(int milliamps) {
    auto charge = getCharge();
    if (!charge) return;
    int32_t ret = 0;
    auto status = charge->setChargeFcc(kFccVoterBatteryHealth, milliamps, &ret);
    if (!status.isOk())
        LOG(ERROR) << "setChargeFcc(" << kFccVoterBatteryHealth << ", " << milliamps
                   << ") failed: " << status.getDescription();
}

ndk::ScopedAStatus ChargingControl::getChargingEnabled(bool* _aidl_return) {
    *_aidl_return = !mLimitActive.load();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ChargingControl::setChargingEnabled(bool enabled) {
    if (enabled) {
        // Release both mechanisms unconditionally; each is a no-op if it was
        // never engaged, and the source may have changed since the limit hit.
        voteChargeFcc(kFccResumeMilliamps);
        if (!android::base::WriteStringToFile("1", kUsbChargingEnabledPath, true))
            LOG(ERROR) << "Failed to write " << kUsbChargingEnabledPath;
        mLimitActive = false;
    } else if (wirelessOnline()) {
        voteChargeFcc(kFccStopMilliamps);
        mLimitActive = true;
    } else {
        if (!android::base::WriteStringToFile("0", kUsbChargingEnabledPath, true))
            LOG(ERROR) << "Failed to write " << kUsbChargingEnabledPath;
        mLimitActive = true;
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
    dprintf(fd, "FCC voter: %d (battery health), stop=%d mA, resume=%d mA\n",
            kFccVoterBatteryHealth, kFccStopMilliamps, kFccResumeMilliamps);
    dprintf(fd, "USB gate node: %s\n", kUsbChargingEnabledPath);
    return STATUS_OK;
}

}  // namespace aidl::vendor::lineage::health
