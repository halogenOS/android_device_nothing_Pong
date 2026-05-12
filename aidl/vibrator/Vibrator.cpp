/*
 * SPDX-FileCopyrightText: 2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Vibrator.h"

#include <cutils/properties.h>
#include <inttypes.h>
#include <log/log.h>

#include <thread>

#include <atomic>
#include <chrono>

#include "aac_vibra_function.h"

#define RICHTAP_LIGHT_STRENGTH 69
#define RICHTAP_MEDIUM_STRENGTH 100
#define RICHTAP_STRONG_STRENGTH 150

// Tunables for the rapid-perform cancel heuristic.
// Two perform() calls within this window count as part of a "rapid sequence"
// (e.g. fast typing, Niagara A-Z swipe). Pattern itself is ~18ms, so 100ms
// comfortably covers both two-finger typing pauses and fast swipes.
#define RAPID_PERFORM_WINDOW_MS 100
// Only start draining the looper queue from the Nth rapid call onward. The
// first few performs in a burst run with a clean (or nearly clean) queue, so
// each tick gets to play out — important for fast two-finger typing where
// the user expects every key to give haptic feedback. Subsequent calls in
// the same burst (i.e. an actual A-Z scroll) start draining so the queue
// doesn't accumulate beyond the on-screen animation.
#define RAPID_PERFORM_DRAIN_AFTER 3

enum vibrationMode {
    MODE_NONE,
    MODE_TIMEOUT,
    MODE_PREBAKED,
    MODE_STREAM,
};

static vibrationMode sLastMode = MODE_NONE;
static std::atomic<int64_t> sLastPerformMs{0};
static std::atomic<int> sRapidPerformCount{0};

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

Vibrator::Vibrator() {
    uint32_t deviceType = 0;

    int32_t ret = aac_vibra_init(&deviceType);
    if (ret) {
        ALOGE("AAC init failed: %d\n", ret);
        return;
    }

    aac_vibra_looper_start();

    aac_vibra_setAmplitude(0x7F);

    ALOGI("AAC init success: %u\n", deviceType);
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                    IVibrator::CAP_AMPLITUDE_CONTROL;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    ALOGD("off() lastMode=%d", sLastMode);

    // Stop both the looper-side pattern queue and the non-looper path.
    // looper_stopPerformHe() drains any pending/playing prebaked or
    // looper_on pattern (which is what almost every vibrate() ends up
    // doing); aac_vibra_off() additionally clears the non-looper state.
    // Without the first call a long on(timeoutMs) keeps running until
    // its own timeout — observed as a vibrator "stuck on" after the
    // triggering app is dismissed.
    aac_vibra_looper_stopPerformHe();

    int32_t ret = aac_vibra_off();
    if (ret) {
        ALOGE("AAC off failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    return ndk::ScopedAStatus::ok();
}

// Short on() pulses (typically waveform steps from compose-fallbacks like UDFPS feedback)
// are too brief for the LRA to reach full deflection via aac_vibra_looper_on. Redirect
// them to a strong prebaked pattern which has a tuned attack envelope.
#define SHORT_PULSE_THRESHOLD_MS 30

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    ALOGD("on(timeoutMs=%d) lastMode=%d", timeoutMs, sLastMode);

    if (timeoutMs > 0 && timeoutMs <= SHORT_PULSE_THRESHOLD_MS) {
        ALOGD("redirecting short pulse to prebaked HEAVY_CLICK");
        int32_t ret = aac_vibra_looper_prebaked_effect(0x3007, RICHTAP_STRONG_STRENGTH);
        if (ret >= 0) {
            if (callback != nullptr) {
                std::thread([=] {
                    usleep(ret * 1000);
                    callback->onComplete();
                }).detach();
            }
            sLastMode = MODE_PREBAKED;
            return ndk::ScopedAStatus::ok();
        }
        ALOGW("prebaked redirect failed (%d), falling back to looper_on", ret);
    }

    int32_t ret = aac_vibra_looper_on(timeoutMs);
    if (ret < 0) {
        ALOGE("AAC on failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (callback != nullptr) {
        std::thread([=] {
            usleep(ret * 1000);
            callback->onComplete();
        }).detach();
    }

    sLastMode = MODE_TIMEOUT;
    return ndk::ScopedAStatus::ok();
}

std::optional<uint32_t> mapEffectToPrebakedId(Effect effect, EffectStrength strength) {
    // Pong's libaacvibrator quantizes strengths to only 2 internal levels (0 and 2),
    // making LIGHT/MEDIUM/STRONG via the strength parameter indistinguishable. We
    // instead pick a different physical RichTap pattern per strength bucket so the
    // user actually feels three distinct steps.
    switch (effect) {
        case Effect::CLICK:
            switch (strength) {
                case EffectStrength::LIGHT:  return 0x3003;  // TICK pattern (lightest)
                case EffectStrength::MEDIUM: return 0x3008;  // standard CLICK
                case EffectStrength::STRONG: return 0x3007;  // HEAVY_CLICK
                default:                     return 0x3008;
            }
        case Effect::DOUBLE_CLICK:
            return 0x1001;
        case Effect::TICK:
        case Effect::THUD:
        case Effect::POP:
            return 0x3003;
        case Effect::HEAVY_CLICK:
            return 0x3007;

        default:
            return static_cast<uint32_t>(effect) + 0x1000;
    }
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength es,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    int32_t strength;

    if (effect < Effect::CLICK || effect > Effect::HEAVY_CLICK)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    switch (es) {
        case EffectStrength::LIGHT:
            strength = RICHTAP_LIGHT_STRENGTH;
            break;
        case EffectStrength::MEDIUM:
            strength = RICHTAP_MEDIUM_STRENGTH;
            break;
        case EffectStrength::STRONG:
            strength = RICHTAP_STRONG_STRENGTH;
            break;
        default:
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    if (sLastMode == MODE_STREAM)
        aac_vibra_setAmplitude(0xFF);

    auto mappedEffect = mapEffectToPrebakedId(effect, es);
    if (!mappedEffect.has_value()) {
        ALOGE("Unsupported effect: %d", static_cast<int>(effect));
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    ALOGD("Performing effect_id=0x%x (mapped from %d), strength=%d",
          mappedEffect.value(), static_cast<int>(effect), strength);

    // Rapid-sequence cancel: drain the looper queue ONLY once we're a few
    // performs deep into a burst. The first couple of rapid taps run with a
    // clean queue (preserves haptic feedback for fast two-finger typing),
    // later ones drain so a Niagara-style A-Z swipe doesn't pile up patterns
    // past the on-screen animation.
    int64_t now = nowMs();
    int64_t last = sLastPerformMs.exchange(now);
    if (now - last < RAPID_PERFORM_WINDOW_MS) {
        if (sRapidPerformCount.fetch_add(1) + 1 >= RAPID_PERFORM_DRAIN_AFTER) {
            aac_vibra_looper_stopPerformHe();
        }
    } else {
        sRapidPerformCount.store(0);
    }

    int32_t ret = aac_vibra_looper_prebaked_effect(mappedEffect.value(), strength);

    if (ret < 0) {
        ALOGE("AAC perform failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (callback != nullptr) {
        std::thread([=] {
            usleep(ret * 1000);
            callback->onComplete();
        }).detach();
    }

    *_aidl_return = ret;

    sLastMode = MODE_PREBAKED;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {Effect::CLICK, Effect::DOUBLE_CLICK, Effect::TICK,
                     Effect::THUD,  Effect::POP,          Effect::HEAVY_CLICK};

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    uint8_t tmp = (uint8_t)(amplitude * 0xff);

    ALOGD("setAmplitude(%f -> 0x%02x) lastMode=%d", amplitude, tmp, sLastMode);

    int32_t ret = aac_vibra_setAmplitude(tmp);
    if (ret) {
        ALOGE("AAC set amplitude failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    sLastMode = MODE_STREAM;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool enabled __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(
    std::vector<CompositePrimitive>* supported __unused) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive __unused,
                                                  int32_t* durationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite __unused,
                                     const std::shared_ptr<IVibratorCallback>& callback __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t id __unused, Effect effect __unused,
                                            EffectStrength strength __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t id __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* resonantFreqHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* freqResolutionHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* freqMinimumHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* _aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t* durationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t* maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* supported __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>& composite __unused,
                                         const std::shared_ptr<IVibratorCallback>& callback __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
