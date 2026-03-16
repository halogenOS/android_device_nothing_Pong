/*
 * Copyright (C) 2026 The halogenOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ChargingControl.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

namespace aidl::vendor::lineage::health {

static constexpr const char* kUsbChargingEnabledPath =
        "/sys/class/qcom-battery/usb_charger_en";
static constexpr const char* kWlsChargingEnabledPath =
        "/sys/class/qcom-battery/wls_en";

ndk::ScopedAStatus ChargingControl::getChargingEnabled(bool* _aidl_return) {
    std::string content;
    if (!android::base::ReadFileToString(kUsbChargingEnabledPath, &content, true)) {
        LOG(ERROR) << "Failed to read " << kUsbChargingEnabledPath;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    *_aidl_return = android::base::Trim(content) == "1";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ChargingControl::setChargingEnabled(bool enabled) {
    const auto val = enabled ? "1" : "0";
    bool usbOk = android::base::WriteStringToFile(val, kUsbChargingEnabledPath, true);
    bool wlsOk = android::base::WriteStringToFile(val, kWlsChargingEnabledPath, true);
    if (!usbOk) LOG(ERROR) << "Failed to write to " << kUsbChargingEnabledPath;
    if (!wlsOk) LOG(ERROR) << "Failed to write to " << kWlsChargingEnabledPath;
    return (usbOk || wlsOk) ? ndk::ScopedAStatus::ok()
                             : ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
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
    bool enabled = false;
    getChargingEnabled(&enabled);
    dprintf(fd, "Charging enabled: %s\n", enabled ? "true" : "false");
    dprintf(fd, "Supported mode: TOGGLE\n");
    dprintf(fd, "USB node: %s\n", kUsbChargingEnabledPath);
    dprintf(fd, "WLS node: %s\n", kWlsChargingEnabledPath);
    return STATUS_OK;
}

}  // namespace aidl::vendor::lineage::health
