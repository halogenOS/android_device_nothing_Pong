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
// firmware report NOT_CHARGING. The FCC vote + fake are reserved for wireless,
// which has no gate that stops current while keeping the Rx link.
static constexpr const char* kUsbChargingEnabledPath =
        "/sys/class/qcom-battery/usb_charger_en";

// wirelessOnline() flaps false during the pad's constant renegotiations, so it is
// NOT used to route wired-vs-wireless -- only to debounce "is the pad gone" for
// clearing the fake. USB has no such renegotiation, so its online state is the
// stable signal for "is this a wired charger".
static constexpr const char* kWirelessOnlinePath =
        "/sys/class/power_supply/wireless/online";
static constexpr const char* kUsbOnlinePath = "/sys/class/power_supply/usb/online";

static bool wirelessOnline() {
    std::string content;
    return android::base::ReadFileToString(kWirelessOnlinePath, &content, true) &&
           android::base::Trim(content) == "1";
}

static bool usbOnline() {
    std::string content;
    return android::base::ReadFileToString(kUsbOnlinePath, &content, true) &&
           android::base::Trim(content) == "1";
}

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
// triggers that native EOC at any SoC while leaving the Rx input up, so
// EXTRA_PLUGGED stays WIRELESS (StandByWatch keys on exactly that).
static constexpr const char* kFakeVbatPath = "/proc/charger/nt_fake_vbat";
static constexpr const char* kFakeVbatFull = "4600";
static constexpr const char* kFakeVbatOff = "0";
// wirelessOnline() flaps false during the pad's frequent link renegotiations;
// only treat the pad as gone after this many consecutive offline cycles (2s
// each) so a flap doesn't drop the fake and let the firmware resume charging.
static constexpr int kOfflineClearCycles = 5;

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

void ChargingControl::releaseCharging() {
    voteChargeFcc(kFccResumeMilliamps);
    setBatteryChargeRestricted(false);
    android::base::WriteStringToFile(kFakeVbatOff, kFakeVbatPath, true);
    android::base::WriteStringToFile("1", kUsbChargingEnabledPath, true);
}

void ChargingControl::startEnforcement() {
    if (mEnforcing.exchange(true)) return;  // already running; loop picks up new band
    mEnforceThread = std::thread([this] {
        bool holding = false, prevHolding = false;
        int offline = 0, sample = 0, hot = 0;
        while (mEnforcing.load()) {
            const int cap = readCapacity();
            const int max = mMax.load();
            const int min = mMin.load();

            // Hysteresis: hold at the top of the band, recharge once the level
            // falls back to the bottom; between the two, keep the current state.
            if (cap >= 0) {
                if (cap >= max) holding = true;
                else if (cap <= min) holding = false;
            }

            // Fresh engage: clear the per-engagement fake-vbat backoff so a
            // transient probe failure never disables it for the rest of boot.
            if (holding && !prevHolding) {
                mFakeVbatUnsafe = false;
                sample = hot = 0;
            }

            offline = wirelessOnline() ? 0 : offline + 1;
            const bool wlsGone = offline >= kOfflineClearCycles;

            if (!holding) {
                // Below the band -> charge up.
                releaseCharging();
            } else if (usbOnline()) {
                // Wired hold: the OEM charge-enable gate reports NOT_CHARGING
                // natively. No fake on the wired path by design.
                android::base::WriteStringToFile(kFakeVbatOff, kFakeVbatPath, true);
                android::base::WriteStringToFile("0", kUsbChargingEnabledPath, true);
            } else {
                // Wireless hold: throttle (FCC vote + battery-side restrict) and
                // trigger the firmware's native EOC via fake-vbat, which stops the
                // residual trickle while keeping the Rx link up.
                android::base::WriteStringToFile("1", kUsbChargingEnabledPath, true);
                voteChargeFcc(kFccStopMilliamps);
                setBatteryChargeRestricted(true);
                // Re-assert the fake every cycle (the firmware clears it on each
                // link renegotiation); drop it only after a sustained offline
                // window so a brief flap doesn't let the firmware resume.
                const bool fakeActive = !mFakeVbatUnsafe.load() && !wlsGone;
                android::base::WriteStringToFile(fakeActive ? kFakeVbatFull : kFakeVbatOff,
                                                 kFakeVbatPath, true);
                // Safety: if current rises under the fake, the firmware is not
                // honouring it -- back off for this engagement (FCC/restrict stay).
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
            }

            prevHolding = holding;
            mHolding = holding;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });
}

void ChargingControl::stopEnforcement() {
    if (!mEnforcing.exchange(false)) return;
    if (mEnforceThread.joinable()) mEnforceThread.join();
    mHolding = false;
    releaseCharging();
}

ndk::ScopedAStatus ChargingControl::getChargingEnabled(bool* _aidl_return) {
    *_aidl_return = !mHolding.load();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ChargingControl::setChargingEnabled(bool enabled) {
    // In LIMIT mode the framework drives us through setChargingLimit and never
    // toggles during normal operation; it only calls this once, with true, when
    // the limit is removed (right after setChargingLimit(max=100)). Honour it as
    // a belt-and-braces release -- a no-op while the loop is still enforcing.
    if (enabled && !mEnforcing.load()) releaseCharging();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ChargingControl::setChargingDeadline(int64_t) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ChargingControl::getChargingDeadline(int64_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ChargingControl::getChargingLimit(ChargingLimitInfo* _aidl_return) {
    _aidl_return->max = mEnforcing.load() ? mMax.load() : 100;
    _aidl_return->min = mEnforcing.load() ? mMin.load() : 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ChargingControl::setChargingLimit(const ChargingLimitInfo& limit) {
    // max >= 100 is the framework's "limit removed" signal (it resets to 100/0).
    if (limit.max >= 100) {
        stopEnforcement();
        return ndk::ScopedAStatus::ok();
    }
    mMax = limit.max;
    mMin = limit.min;
    startEnforcement();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ChargingControl::getSupportedMode(int* _aidl_return) {
    *_aidl_return = static_cast<int>(ChargingControlSupportedMode::LIMIT);
    return ndk::ScopedAStatus::ok();
}

binder_status_t ChargingControl::dump(int fd, const char**, uint32_t) {
    dprintf(fd, "Supported mode: LIMIT\n");
    dprintf(fd, "Enforcing: %s\n", mEnforcing.load() ? "true" : "false");
    dprintf(fd, "Band: %d-%d%%\n", mMin.load(), mMax.load());
    dprintf(fd, "Holding: %s\n", mHolding.load() ? "true" : "false");
    dprintf(fd, "FCC voter: %d (battery health), stop=%d mA, resume=%d mA\n",
            kFccVoterBatteryHealth, kFccStopMilliamps, kFccResumeMilliamps);
    dprintf(fd, "USB gate node: %s\n", kUsbChargingEnabledPath);
    return STATUS_OK;
}

}  // namespace aidl::vendor::lineage::health
