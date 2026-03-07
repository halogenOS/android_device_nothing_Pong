/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 The halogenOS Project
 */

#include "LtpoControl.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace aidl::custom::hardware::display::ltpo {

LtpoControl::LtpoControl() {
    mSfmFd = open(kSysfsPath, O_WRONLY);
    if (mSfmFd < 0) {
        LOG(ERROR) << "ltpo-hal: cannot open " << kSysfsPath << ": " << strerror(errno);
        return;
    }
    mEnabled = true;
    LOG(INFO) << "ltpo-hal: initialized for NT37705";
}

LtpoControl::~LtpoControl() {
    if (mSfmFd >= 0) {
        writeSfm(kSteps[5].sfmIndex);  // 120Hz (disable)
        close(mSfmFd);
    }
}

bool LtpoControl::writeSfm(int index) {
    if (mSfmFd < 0) return false;
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", index);
    if (pwrite(mSfmFd, buf, len, 0) < 0) {
        if (errno != EBUSY)
            LOG(ERROR) << "ltpo-hal: write SFM " << index << ": " << strerror(errno);
        return false;
    }
    return true;
}

bool LtpoControl::isVrrMode() const {
    return android::base::GetProperty("persist.sys.sfm.mode", "idle") == "vrr";
}

ndk::ScopedAStatus LtpoControl::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mLock);
    mEnabled = enabled;
    if (!enabled) {
        if (writeSfm(kSteps[5].sfmIndex))  // 120Hz (disable)
            mCurrentHz = 120;
    }
    LOG(INFO) << "ltpo-hal: " << (enabled ? "enabled" : "disabled");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus LtpoControl::onIdle() {
    int32_t actual;
    return setTargetHz(1, &actual);
}

ndk::ScopedAStatus LtpoControl::onActive() {
    int32_t actual;
    return setTargetHz(120, &actual);
}

ndk::ScopedAStatus LtpoControl::getCurrentHz(int32_t* _aidl_return) {
    *_aidl_return = mCurrentHz;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus LtpoControl::setTargetHz(int32_t targetHz, int32_t* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mEnabled) {
        *_aidl_return = 120;
        return ndk::ScopedAStatus::ok();
    }

    // In idle-only mode, clamp to 1Hz or 120Hz
    if (!isVrrMode()) {
        targetHz = (targetHz <= 1) ? 1 : 120;
    }

    // Touch boost: if 120Hz is requested, set a hold window.
    // During the hold, reject any lower target so the animation stays smooth.
    auto now = Clock::now();
    if (targetHz >= 120) {
        mBoostUntil = now + kBoostHold;
    } else if (now < mBoostUntil) {
        targetHz = 120;
    }

    // Find lowest supported rate >= targetHz
    int bestHz = 120;
    int bestSfm = kSteps[5].sfmIndex;
    for (const auto& step : kSteps) {
        if (step.hz >= targetHz) {
            bestHz = step.hz;
            bestSfm = step.sfmIndex;
            break;
        }
    }

    if (mCurrentHz != bestHz) {
        if (writeSfm(bestSfm))
            mCurrentHz = bestHz;
    }

    *_aidl_return = bestHz;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus LtpoControl::getSupportedHz(std::vector<int32_t>* _aidl_return) {
    _aidl_return->clear();
    for (const auto& step : kSteps) {
        _aidl_return->push_back(step.hz);
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::custom::hardware::display::ltpo
