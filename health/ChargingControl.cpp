/*
 * Copyright (C) 2026 The halogenOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ChargingControl.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>

#include <chrono>
#include <cstdlib>

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

// Matches the framework's RECHARGE_MARGIN: a genuine recharge only fires once the
// level has fallen this far below the cap, so an "enable" arriving while still
// within this band of the cap is a spurious renegotiation blip, not a recharge.
static constexpr int kRechargeMargin = 4;

static int readCapacity() {
    std::string content;
    if (!android::base::ReadFileToString("/sys/class/power_supply/battery/capacity",
                                         &content, true))
        return -1;
    return atoi(android::base::Trim(content).c_str());
}

// Spacewar (pure QTI, no scenario_fcc layer) holds the wireless limit purely with
// the battery-side charge-current limit (BATT_CHG_CTRL_LIM, driven by restrict).
// On Pong the OEM scenario_fcc voter caps the bulk of the current but leaves a
// small trickle; driving the battery-side limit to 0 as well aims to kill it.
static constexpr const char* kRestrictCurPath =
        "/sys/class/qcom-battery/restrict_cur";
static constexpr const char* kRestrictChgPath =
        "/sys/class/qcom-battery/restrict_chg";

static void setBatteryChargeRestricted(bool restricted) {
    if (restricted) {
        // restrict_cur=0 stores a 0uA FCC, restrict_chg=1 applies it.
        android::base::WriteStringToFile("0", kRestrictCurPath, true);
        android::base::WriteStringToFile("1", kRestrictChgPath, true);
    } else {
        android::base::WriteStringToFile("0", kRestrictChgPath, true);
    }
}

// The FCC paths above only throttle: the firmware keeps a ~37mA wireless
// maintenance trickle that creeps past the limit overnight. What does fully
// stop current with the link alive is the firmware's own end-of-charge
// (observed at natural 100%). BATT_FAKE_VBAT is the factory aging-test
// override for the battery voltage the firmware sees; faking a full battery
// triggers that native EOC at any SoC. EXPERIMENTAL: unit (mV vs uV) is
// unverified, so the hold loop watches current_now and aborts the fake if
// charging current rises instead of stopping.
static constexpr const char* kFakeVbatPath = "/proc/charger/nt_fake_vbat";
static constexpr const char* kFakeVbatFull = "4600";
static constexpr const char* kFakeVbatOff = "0";
// wirelessOnline() flaps false during the pad's frequent link renegotiations;
// only treat the pad as gone after this many consecutive offline cycles (2s
// each) so a flap doesn't drop the fake and let the firmware resume charging.
static constexpr int kOfflineClearCycles = 5;

void ChargingControl::startRestrictReassert() {
    if (mReassert.exchange(true)) return;
    // Retry the fake on every fresh engagement: a transient must never disable
    // it for the rest of the boot.
    mFakeVbatUnsafe = false;
    mReassertThread = std::thread([this] {
        int offline = 0, sample = 0, hot = 0;
        while (mReassert.load()) {
            setBatteryChargeRestricted(true);
            // Re-assert the fake every cycle (the firmware clears it on each link
            // renegotiation). wirelessOnline() flaps false mid-renegotiation
            // while the pad is still delivering, so only treat the pad as gone
            // after a sustained offline window; the restrict above caps current
            // to a trickle during any gap, so brief flaps don't leak charge.
            offline = wirelessOnline() ? 0 : offline + 1;
            bool offPad = offline >= kOfflineClearCycles;
            bool fakeActive = !mFakeVbatUnsafe.load() && !offPad;
            android::base::WriteStringToFile(fakeActive ? kFakeVbatFull : kFakeVbatOff,
                                             kFakeVbatPath, true);
            // Safety: if charging current rises under the fake, the firmware is
            // not honouring it -- back off the fake for the rest of THIS
            // engagement (a fresh engage retries). FCC restrict stays engaged.
            if (fakeActive && ++sample > 3) {
                std::string cur;
                android::base::ReadFileToString(
                        "/sys/class/power_supply/battery/current_now", &cur, true);
                hot = atoi(android::base::Trim(cur).c_str()) > 150000 ? hot + 1 : 0;
                if (hot >= 2) {
                    android::base::WriteStringToFile(kFakeVbatOff, kFakeVbatPath, true);
                    mFakeVbatUnsafe = true;
                    LOG(ERROR) << "WLS-FAKE: charging under fake vbat, backing off";
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });
}

void ChargingControl::stopRestrictReassert() {
    if (!mReassert.exchange(false)) return;
    if (mReassertThread.joinable()) mReassertThread.join();
    setBatteryChargeRestricted(false);
    if (!android::base::WriteStringToFile(kFakeVbatOff, kFakeVbatPath, true))
        LOG(ERROR) << "Failed to clear " << kFakeVbatPath;
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
        // A wireless link renegotiation briefly flips the firmware to CHARGING at
        // limit-1, and the framework misreads that as "charging up" and calls
        // enable -- which would stop the hold and, in doze, let the battery charge
        // well past the cap before the framework reacts again. While we're holding
        // the wireless cap and the battery is still within RECHARGE_MARGIN of it,
        // this is that spurious blip, not a real recharge: keep holding.
        if (mReassert.load() && readCapacity() > mCapLimit.load() - kRechargeMargin) {
            LOG(INFO) << "Ignoring near-cap enable (holding at ~" << mCapLimit.load() << "%)";
            return ndk::ScopedAStatus::ok();
        }
        // Release every mechanism unconditionally; each is a no-op if it was
        // never engaged, and the source may have changed since the limit hit.
        stopRestrictReassert();
        voteChargeFcc(kFccResumeMilliamps);
        if (!android::base::WriteStringToFile("1", kUsbChargingEnabledPath, true))
            LOG(ERROR) << "Failed to write " << kUsbChargingEnabledPath;
        mLimitActive = false;
    } else if (wirelessOnline()) {
        // Record the cap (the lowest level a disable ever fires at ~ the limit),
        // so spurious near-cap enables above can be distinguished from recharges.
        int cap = readCapacity();
        if (cap >= 0 && cap < mCapLimit.load()) mCapLimit = cap;
        voteChargeFcc(kFccStopMilliamps);
        startRestrictReassert();
        mLimitActive = true;
    } else {
        stopRestrictReassert();
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
