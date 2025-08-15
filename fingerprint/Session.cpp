/*
 * Copyright (C) 2024 The LineageOS Project
 * Copyright (C) 2025 The halogenOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <thread>
#include <chrono>
#include <atomic>

#include <android-base/file.h>
#include <android-base/stringprintf.h>

#include "Session.h"
#include "Legacy2Aidl.h"

#include "CancellationSignal.h"

#define FOD_HBM_PATH "/sys/devices/platform/soc/soc:qcom,dsi-display-primary/force_fod_ui"

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {

static inline void setFodHbm(bool status) {
    ::android::base::WriteStringToFile(status ? "1" : "0", FOD_HBM_PATH);
}

static inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static std::atomic<uint64_t> gSeq{0};
static inline uint64_t nextSeq() { return ++gSeq; }

void onClientDeath(void* cookie) {
    ALOGI("FingerprintService has died");
    Session* session = static_cast<Session*>(cookie);
    if (session && !session->isClosed()) {
        session->close();
    }
}

Session::Session(fingerprint_device_t* device, int32_t userId,
            std::shared_ptr<ISessionCallback> cb, LockoutTracker lockoutTracker,
            WorkerThread* worker)
            : mDevice(device), mLockoutTracker(lockoutTracker),
              mWorker(worker), mClosed(false), mUserId(userId), mCb(cb) {
    mDeathRecipient = AIBinder_DeathRecipient_new(onClientDeath);

    std::string path = ::android::base::StringPrintf("/data/vendor_de/%d/fpdata/", mUserId);
    mDevice->set_active_group(mDevice, mUserId, path.c_str());
}

ndk::ScopedAStatus Session::generateChallenge() {
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, tq] {
        int64_t ts = now_ms();
        ALOGD("generateChallenge() queueDelay=%lldms", (long long)(ts - tq));
        uint64_t challenge = mDevice->pre_enroll(mDevice);
        ALOGI("generateChallenge: %ld", challenge);
        mCb->onChallengeGenerated(challenge);
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, challenge, tq] {
        int64_t ts = now_ms();
        ALOGD("revokeChallenge() queueDelay=%lldms", (long long)(ts - tq));
        ALOGI("revokeChallenge: %ld", challenge);
        setFodHbm(false);
        ALOGD("goodixExtCmd(UP)");
        mDevice->goodixExtCmd(mDevice, 0, 0);
        mDevice->post_enroll(mDevice);
        mCb->onChallengeRevoked(challenge);
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enroll(const HardwareAuthToken& hat,
                                   std::shared_ptr<ICancellationSignal>* out) {
    ALOGI("enroll");
    int64_t tq = now_ms();
    uint64_t seq = nextSeq();
    mWorker->schedule(Callable::from([this, hat, tq, seq] {
        int64_t ts = now_ms();
        ALOGD("[seq=%llu] enroll queueDelay=%lldms", (unsigned long long)seq, (long long)(ts - tq));
        hw_auth_token_t authToken;
        translate(hat, authToken);
        int error = mDevice->enroll(mDevice, &authToken, mUserId, 60);
        if (error) {
            ALOGE("[seq=%llu] enroll failed: %d", (unsigned long long)seq, error);
            mCb->onError(Error::UNABLE_TO_PROCESS, error);
        } else {
            ALOGD("[seq=%llu] setFodHbm(true)", (unsigned long long)seq);
            setFodHbm(true);
        }
    }));

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<ICancellationSignal>* out) {
    ALOGI("authenticate");
    mUiReady = false;
    int64_t tq = now_ms();
    uint64_t seq = nextSeq();

    mWorker->schedule(Callable::from([this, operationId, tq, seq] {
        int64_t ts = now_ms();
        ALOGD("[seq=%llu] authenticate queueDelay=%lldms opId=%lld",
              (unsigned long long)seq, (long long)(ts - tq), (long long)operationId);
        int error = mDevice->authenticate(mDevice, operationId, mUserId);
        if (error) {
            ALOGE("[seq=%llu] authenticate failed: %d", (unsigned long long)seq, error);
            mCb->onError(Error::UNABLE_TO_PROCESS, error);
        } else {
            ALOGD("[seq=%llu] setFodHbm(true)", (unsigned long long)seq);
            setFodHbm(true);
        }
    }));

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<ICancellationSignal>* out) {
    ALOGI("detectInteraction");
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, tq] {
        int64_t ts = now_ms();
        ALOGD("detectInteraction queueDelay=%lldms", (long long)(ts - tq));
        ALOGD("Detect interaction is not supported");
        mCb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorCode */);
    }));
    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    ALOGI("enumerateEnrollments");
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, tq] {
        int64_t ts = now_ms();
        ALOGD("enumerateEnrollments queueDelay=%lldms", (long long)(ts - tq));
        int error = mDevice->enumerate(mDevice);
        if (error) {
            ALOGE("enumerate failed: %d", error);
        }
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
    ALOGI("removeEnrollments, size: %zu", enrollmentIds.size());
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, enrollmentIds, tq] {
        int64_t ts = now_ms();
        ALOGD("removeEnrollments queueDelay=%lldms", (long long)(ts - tq));
        for (int32_t fid : enrollmentIds) {
            int error = mDevice->remove(mDevice, mUserId, fid);
            if (error) {
                ALOGE("remove failed: %d", error);
            }
        }
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, tq] {
        int64_t ts = now_ms();
        ALOGD("getAuthenticatorId queueDelay=%lldms", (long long)(ts - tq));
        uint64_t auth_id = mDevice->get_authenticator_id(mDevice);
        ALOGI("getAuthenticatorId: %ld", auth_id);
        mCb->onAuthenticatorIdRetrieved(auth_id);
        ALOGD("goodixExtCmd(UP)");
        mDevice->goodixExtCmd(mDevice, 0, 0);
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, tq] {
        int64_t ts = now_ms();
        ALOGD("invalidateAuthenticatorId queueDelay=%lldms", (long long)(ts - tq));
        uint64_t auth_id = mDevice->get_authenticator_id(mDevice);
        ALOGI("invalidateAuthenticatorId: %ld", auth_id);
        mCb->onAuthenticatorIdInvalidated(auth_id);
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const HardwareAuthToken& /*hat*/) {
    ALOGI("resetLockout");
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, tq] {
        int64_t ts = now_ms();
        ALOGD("resetLockout queueDelay=%lldms", (long long)(ts - tq));
        clearLockout(true);
        mIsLockoutTimerAborted = true;
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerDown(int32_t /*pointerId*/, int32_t x, int32_t y, float minor,
                                          float major) {
    ALOGI("onPointerDown");
    int64_t tq = now_ms();
    uint64_t seq = nextSeq();
    mWorker->schedule(Callable::from([this, x, y, minor, major, tq, seq] {
        int64_t ts = now_ms();
        ALOGD("[seq=%llu] onPointerDown queueDelay=%lldms mUiReady=%d",
              (unsigned long long)seq, (long long)(ts - tq), (int)mUiReady.load());
        int64_t ws = now_ms();
        {
            std::unique_lock<std::mutex> lk(mUiMutex);
            bool ready = mUiCv.wait_for(lk, std::chrono::milliseconds(400),
                                        [this]{ return mUiReady.load(); });
            int64_t we = now_ms();
            ALOGD("[seq=%llu] onPointerDown wait ready=%d waited=%lldms",
                  (unsigned long long)seq, (int)ready, (long long)(we - ws));
        }
        ALOGD("[seq=%llu] goodixExtCmd(DOWN)", (unsigned long long)seq);
        mDevice->goodixExtCmd(mDevice, 1, 0);
        checkSensorLockout();
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerUp(int32_t /*pointerId*/) {
    ALOGI("onPointerUp");
    int64_t tq = now_ms();
    uint64_t seq = nextSeq();
    mWorker->schedule(Callable::from([this, tq, seq] {
        int64_t ts = now_ms();
        ALOGD("[seq=%llu] onPointerUp queueDelay=%lldms", (unsigned long long)seq, (long long)(ts - tq));
        ALOGD("[seq=%llu] goodixExtCmd(UP)", (unsigned long long)seq);
        mDevice->goodixExtCmd(mDevice, 0, 0);
        mUiReady = false;              // reset gate after touch ends
        mUiCv.notify_all();
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onUiReady() {
    ALOGI("onUiReady");
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, tq] {
        int64_t ts = now_ms();
        ALOGD("onUiReady queueDelay=%lldms", (long long)(ts - tq));
        bool prev = mUiReady.load();
        {
            std::lock_guard<std::mutex> lk(mUiMutex);
            mUiReady = true;           // mark overlay/HBM ready; no Goodix call here
        }
        ALOGD("onUiReady set mUiReady=true (was %d), notifying", (int)prev);
        mUiCv.notify_all();
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
        int64_t operationId, const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return authenticate(operationId, out);
}

ndk::ScopedAStatus Session::enrollWithContext(const keymaster::HardwareAuthToken& hat,
                                              const common::OperationContext& /*context*/,
                                              std::shared_ptr<common::ICancellationSignal>* out) {
    return enroll(hat, out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onPointerDownWithContext(const PointerContext& context) {
    return onPointerDown(context.pointerId, context.x, context.y, context.minor, context.major);
}

ndk::ScopedAStatus Session::onPointerUpWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}

ndk::ScopedAStatus Session::onContextChanged(const common::OperationContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerCancelWithContext(const PointerContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setIgnoreDisplayTouches(bool /*shouldIgnore*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::cancel() {
    ALOGI("cancel");
    int64_t tq = now_ms();
    uint64_t seq = nextSeq();
    mWorker->schedule(Callable::from([this, tq, seq] {
        int64_t ts = now_ms();
        ALOGD("[seq=%llu] cancel queueDelay=%lldms", (unsigned long long)seq, (long long)(ts - tq));
        setFodHbm(false);
        ALOGD("[seq=%llu] goodixExtCmd(UP)", (unsigned long long)seq);
        mDevice->goodixExtCmd(mDevice, 0, 0);
        mUiReady = false;              // ensure next auth starts clean
        mUiCv.notify_all();
        int ret = mDevice->cancel(mDevice);
        if (ret == 0) {
            mCb->onError(Error::CANCELED, 0 /* vendorCode */);
        }
    }));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::close() {
    ALOGI("close");
    int64_t tq = now_ms();
    mWorker->schedule(Callable::from([this, tq] {
        int64_t ts = now_ms();
        ALOGD("close queueDelay=%lldms", (long long)(ts - tq));
        setFodHbm(false);
        ALOGD("goodixExtCmd(UP)");
        mDevice->goodixExtCmd(mDevice, 0, 0);
        mUiReady = false;
        mUiCv.notify_all();
    }));
    mClosed = true;
    mCb->onSessionClosed();
    AIBinder_DeathRecipient_delete(mDeathRecipient);
    return ndk::ScopedAStatus::ok();
}

binder_status_t Session::linkToDeath(AIBinder* binder) {
    return AIBinder_linkToDeath(binder, mDeathRecipient, this);
}

bool Session::isClosed() {
    return mClosed;
}

// Translate from errors returned by traditional HAL (see fingerprint.h) to
// AIDL-compliant Error
Error Session::VendorErrorFilter(int32_t error, int32_t* vendorCode) {
    *vendorCode = 0;
    switch (error) {
        case FINGERPRINT_ERROR_HW_UNAVAILABLE:
            return Error::HW_UNAVAILABLE;
        case FINGERPRINT_ERROR_UNABLE_TO_PROCESS:
            return Error::UNABLE_TO_PROCESS;
        case FINGERPRINT_ERROR_TIMEOUT:
            return Error::TIMEOUT;
        case FINGERPRINT_ERROR_NO_SPACE:
            return Error::NO_SPACE;
        case FINGERPRINT_ERROR_CANCELED:
            return Error::CANCELED;
        case FINGERPRINT_ERROR_UNABLE_TO_REMOVE:
            return Error::UNABLE_TO_REMOVE;
        case FINGERPRINT_ERROR_LOCKOUT: {
            *vendorCode = FINGERPRINT_ERROR_LOCKOUT;
            return Error::VENDOR;
        }
        default:
            if (error >= FINGERPRINT_ERROR_VENDOR_BASE) {
                // vendor specific code.
                *vendorCode = error - FINGERPRINT_ERROR_VENDOR_BASE;
                return Error::VENDOR;
            }
    }
    ALOGE("Unknown error from fingerprint vendor library: %d", error);
    return Error::UNABLE_TO_PROCESS;
}

// Translate acquired messages returned by traditional HAL (see fingerprint.h)
// to AIDL-compliant AcquiredInfo
AcquiredInfo Session::VendorAcquiredFilter(int32_t info, int32_t* vendorCode) {
    *vendorCode = 0;
    switch (info) {
        case FINGERPRINT_ACQUIRED_GOOD:
            return AcquiredInfo::GOOD;
        case FINGERPRINT_ACQUIRED_PARTIAL:
            return AcquiredInfo::PARTIAL;
        case FINGERPRINT_ACQUIRED_INSUFFICIENT:
            return AcquiredInfo::INSUFFICIENT;
        case FINGERPRINT_ACQUIRED_IMAGER_DIRTY:
            return AcquiredInfo::SENSOR_DIRTY;
        case FINGERPRINT_ACQUIRED_TOO_SLOW:
            return AcquiredInfo::TOO_SLOW;
        case FINGERPRINT_ACQUIRED_TOO_FAST:
            return AcquiredInfo::TOO_FAST;
        default:
            if (info >= FINGERPRINT_ACQUIRED_VENDOR_BASE) {
                // vendor specific code.
                *vendorCode = info - FINGERPRINT_ACQUIRED_VENDOR_BASE;
                return AcquiredInfo::VENDOR;
            }
    }
    ALOGE("Unknown acquiredmsg from fingerprint vendor library: %d", info);
    return AcquiredInfo::INSUFFICIENT;
}

bool Session::checkSensorLockout() {
    LockoutMode lockoutMode = mLockoutTracker.getMode();

    if (lockoutMode != LockoutMode::NONE) {
        setFodHbm(false);
        ALOGD("goodixExtCmd(UP) due to lockout");
        mDevice->goodixExtCmd(mDevice, 0, 0);
    }

    if (lockoutMode == LockoutMode::PERMANENT) {
        ALOGE("Fail: lockout permanent");
        mCb->onLockoutPermanent();
        mIsLockoutTimerAborted = true;
        return true;
    } else if (lockoutMode == LockoutMode::TIMED) {
        int64_t timeLeft = mLockoutTracker.getLockoutTimeLeft();
        ALOGE("Fail: lockout timed: %ld", timeLeft);
        mCb->onLockoutTimed(timeLeft);
        if (!mIsLockoutTimerStarted) startLockoutTimer(timeLeft);
        return true;
    }

    return false;
}

void Session::clearLockout(bool clearAttemptCounter) {
    mLockoutTracker.reset(clearAttemptCounter);
    mCb->onLockoutCleared();
}

void Session::startLockoutTimer(int64_t timeout) {
    mIsLockoutTimerAborted = false;
    std::function<void()> action =
            std::bind(&Session::lockoutTimerExpired, this);
    std::thread([timeout, action]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        action();
    }).detach();

    mIsLockoutTimerStarted = true;
}

void Session::lockoutTimerExpired() {
    if (!mIsLockoutTimerAborted)
        clearLockout(false);

    mIsLockoutTimerStarted = false;
    mIsLockoutTimerAborted = false;
}

void Session::notify(const fingerprint_msg_t* msg) {
    int64_t t = now_ms();
    switch (msg->type) {
        case FINGERPRINT_ERROR: {
            int32_t vendorCode = 0;
            Error result = VendorErrorFilter(msg->data.error, &vendorCode);
            ALOGD("notify(ERROR) t=%lld result=%hhd vendor=%d", (long long)t, result, vendorCode);
            mCb->onError(result, vendorCode);
            mUiReady = false;         // reset gate on any terminal error
            mUiCv.notify_all();
        } break;
        case FINGERPRINT_ACQUIRED: {
            int32_t vendorCode = 0;
            AcquiredInfo result =
                    VendorAcquiredFilter(msg->data.acquired.acquired_info, &vendorCode);
            if (result != AcquiredInfo::VENDOR) {
                ALOGD("notify(ACQUIRED) t=%lld result=%d vendor=%d", (long long)t, result, vendorCode);
                mCb->onAcquired(result, vendorCode);
            } else {
                ALOGW("notify(ACQUIRED VENDOR) t=%lld vendor=%d", (long long)t, vendorCode);
            }
        } break;
        case FINGERPRINT_TEMPLATE_ENROLLING: {
            ALOGD("notify(ENROLLING) t=%lld fid=%d gid=%d rem=%d",
                  (long long)t, msg->data.enroll.finger.fid,
                  msg->data.enroll.finger.gid,
                  msg->data.enroll.samples_remaining);
            mCb->onEnrollmentProgress(msg->data.enroll.finger.fid,
                                      msg->data.enroll.samples_remaining);
        } break;
        case FINGERPRINT_TEMPLATE_REMOVED: {
            ALOGD("notify(REMOVED) t=%lld fid=%d gid=%d rem=%d",
                  (long long)t, msg->data.removed.finger.fid,
                  msg->data.removed.finger.gid,
                  msg->data.removed.remaining_templates);
            std::vector<int> enrollments;
            enrollments.push_back(msg->data.removed.finger.fid);
            mCb->onEnrollmentsRemoved(enrollments);
        } break;
        case FINGERPRINT_AUTHENTICATED: {
            ALOGD("notify(AUTH) t=%lld fid=%d gid=%d",
                  (long long)t,
                  msg->data.authenticated.finger.fid,
                  msg->data.authenticated.finger.gid);
            if (msg->data.authenticated.finger.fid != 0) {
                const hw_auth_token_t hat = msg->data.authenticated.hat;
                HardwareAuthToken authToken;
                translate(hat, authToken);

                mCb->onAuthenticationSucceeded(msg->data.authenticated.finger.fid, authToken);
                mLockoutTracker.reset(true);
                setFodHbm(false);
                ALOGD("goodixExtCmd(UP) after success");
                mDevice->goodixExtCmd(mDevice, 0, 0);
            } else {
                mCb->onAuthenticationFailed();
                mLockoutTracker.addFailedAttempt();
                checkSensorLockout();
            }
            mUiReady = false;         // reset after success or fail
            mUiCv.notify_all();
        } break;
        case FINGERPRINT_TEMPLATE_ENUMERATING: {
            ALOGD("notify(ENUM) t=%lld fid=%d gid=%d rem=%d",
                  (long long)t,
                  msg->data.enumerated.finger.fid,
                  msg->data.enumerated.finger.gid,
                  msg->data.enumerated.remaining_templates);
            static std::vector<int> enrollments;
            enrollments.push_back(msg->data.enumerated.finger.fid);
            if (msg->data.enumerated.remaining_templates == 0) {
                mCb->onEnrollmentsEnumerated(enrollments);
                enrollments.clear();
            }
        } break;
    }
}

} // namespace fingerprint
} // namespace biometrics
} // namespace hardware
} // namespace android
} // namespace aidl

