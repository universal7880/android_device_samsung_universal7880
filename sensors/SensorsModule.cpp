/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <cerrno>
#include <cstring>

#include "Sensors.h"

namespace {
ssp::Sensors& sensors() {
    static ssp::Sensors instance;
    return instance;
}

int closeDevice(hw_device_t*) {
    sensors().stop();
    return 0;
}

int activate(sensors_poll_device_t*, int handle, int enabled) {
    if (enabled != 0 && enabled != 1) return -EINVAL;
    return sensors().activate(handle, enabled);
}

int setDelay(sensors_poll_device_t*, int handle, int64_t period) {
    return sensors().batch(handle, 0, period, 0);
}

int poll(sensors_poll_device_t*, sensors_event_t* events, int count) {
    return sensors().poll(events, count);
}

int batch(sensors_poll_device_1_t*, int handle, int flags, int64_t period, int64_t latency) {
    return sensors().batch(handle, flags, period, latency);
}

int flush(sensors_poll_device_1_t*, int handle) { return sensors().flush(handle); }

int openDevice(const hw_module_t* module, const char* name, hw_device_t** result) {
    if (!name || !result || strcmp(name, SENSORS_HARDWARE_POLL)) return -EINVAL;
    *result = nullptr;
    int status = sensors().start();
    if (status) return status;
    // The module owns one polling device. start() rejects a second open.
    static sensors_poll_device_1_t device{};
    device.common.tag = HARDWARE_DEVICE_TAG;
    device.common.version = SENSORS_DEVICE_API_VERSION_1_3;
    device.common.module = const_cast<hw_module_t*>(module);
    device.common.close = closeDevice;
    device.activate = activate;
    device.setDelay = setDelay;
    device.poll = poll;
    device.batch = batch;
    device.flush = flush;
    *result = &device.common;
    return 0;
}

int getList(sensors_module_t*, const sensor_t** list) {
    if (!list) return -EINVAL;
    return sensors().getList(list);
}

int setOperationMode(unsigned mode) {
    // Samsung's wrapper requests its complete sensor list using mode 5555.
    // This HAL always exposes that list; no framework-private filtering is needed.
    return (mode == 0 || mode == 5555) ? 0 : -EINVAL;
}

hw_module_methods_t methods = {openDevice};
}  // namespace

extern "C" {
sensors_module_t HAL_MODULE_INFO_SYM = [] {
    sensors_module_t module{};
    module.common.tag = HARDWARE_MODULE_TAG;
    module.common.module_api_version = SENSORS_MODULE_API_VERSION_0_1;
    module.common.hal_api_version = HARDWARE_HAL_API_VERSION;
    module.common.id = SENSORS_HARDWARE_MODULE_ID;
    module.common.name = "Exynos7880 SSP Sensors";
    module.common.author = "The LineageOS Project";
    module.common.methods = &methods;
    module.get_sensors_list = getList;
    module.set_operation_mode = setOperationMode;
    return module;
}();
}
