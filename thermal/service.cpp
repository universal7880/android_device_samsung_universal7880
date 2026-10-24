/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 The LineageOS Project
 */

#include "Thermal.h"

#include <cstdlib>

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::thermal::Thermal;

int main() {
    android::base::InitLogging(nullptr, android::base::LogdLogger(android::base::SYSTEM));
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto service = ndk::SharedRefBase::make<Thermal>();
    CHECK(service->initialize()) << "Cannot initialize Exynos7880 thermal sensors";
    const std::string instance = std::string(Thermal::descriptor) + "/default";
    CHECK_EQ(AServiceManager_addService(service->asBinder().get(), instance.c_str()), STATUS_OK);
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
