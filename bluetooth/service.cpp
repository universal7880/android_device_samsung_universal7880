/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "bluetooth.universal7880"
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include <thread>

#include "aidl/BluetoothHci.h"
#include "ant/Ant.h"

int main() {
    // One loop for AIDL Bluetooth and one for the existing HIDL ANT client.
    // libbt-vendor serializes their shared controller lifecycle.
    if (!ABinderProcess_setThreadPoolMaxThreadCount(0)) return 1;
    android::hardware::configureRpcThreadpool(1, true);
    auto bluetooth = ndk::SharedRefBase::make<universal7880::BluetoothHci>();
    android::sp<universal7880::Ant> ant = new universal7880::Ant();
    const std::string instance = std::string(bluetooth->descriptor) + "/default";
    if (AServiceManager_addService(bluetooth->asBinder().get(), instance.c_str()) != STATUS_OK ||
        ant->registerAsService() != android::OK) {
        ALOGE("Cannot register Bluetooth/ANT HALs");
        return 1;
    }
    std::thread(android::hardware::joinRpcThreadpool).detach();
    ABinderProcess_joinThreadPool();
    return 1;
}
