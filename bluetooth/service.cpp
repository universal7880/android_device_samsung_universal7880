/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "bluetooth.universal7880"
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include "ant/Ant.h"
#include "hidl/bluetooth_hci.h"

int main() {
    // Preserve the AOSP Bluetooth service's serialized lifecycle calls.
    // UART and ANT receive work run on their own threads.
    android::hardware::configureRpcThreadpool(1, true);
    android::sp<android::hardware::bluetooth::V1_1::implementation::BluetoothHci> bluetooth =
        new android::hardware::bluetooth::V1_1::implementation::BluetoothHci();
    android::sp<universal7880::Ant> ant = new universal7880::Ant();
    if (bluetooth->registerAsService() != android::OK || ant->registerAsService() != android::OK) {
        ALOGE("Cannot register Bluetooth/ANT HALs");
        return 1;
    }
    android::hardware::joinRpcThreadpool();
    return 1;
}
