/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 The halogenOS Project
 */

#include "LtpoControl.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::custom::hardware::display::ltpo::LtpoControl;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();

    auto svc = ndk::SharedRefBase::make<LtpoControl>();
    const std::string instance = std::string() + LtpoControl::descriptor + "/default";

    auto status = AServiceManager_addService(svc->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register " << instance;

    LOG(INFO) << "ltpo-hal: service registered as " << instance;
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
