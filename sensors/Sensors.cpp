/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "SSPSensors"
#include "Sensors.h"

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <hardware_legacy/power.h>
#include <log/log.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include "SspProtocol.h"

namespace ssp {
namespace {
constexpr const char* kSsp = "/sys/class/sensors/ssp_sensor/";
constexpr const char* kIio = "/sys/bus/iio/devices/";
constexpr const char* kWakeLock = "universal7880-sensors";
constexpr size_t kQueueLimit = 4096;

int writeValue(const std::string& path, int64_t value) {
    android::base::unique_fd fd(open(path.c_str(), O_WRONLY | O_CLOEXEC));
    if (fd < 0) return -errno;
    const std::string text = std::to_string(value) + "\n";
    if (android::base::WriteFully(fd.get(), text.data(), text.size())) return 0;
    return errno ? -errno : -EIO;
}

std::string readValue(const std::string& path) {
    std::string result;
    if (!android::base::ReadFileToString(path, &result)) return {};
    return android::base::Trim(result);
}

struct Descriptor {
    int handle;
    int type;
    const char* iio;
    const char* name;
    const char* vendor;
    const char* typeName;
    float range;
    float resolution;
    float power;
    int minDelay;
    int maxDelay;
    uint32_t flags;
    float scale;
};

// Metadata and conversion factors from the Samsung sensor module; packet layouts
// are defined by drivers/sensorhub/stm32/ssp_sensorlist.h and ssp_iio.c.
constexpr float kAccelScale = 9.80665f / 4096.0f;
constexpr float kGyroScale = 0.035f * 3.14159265358979323846f / 180.0f;
constexpr Descriptor kSensors[] = {
    {1, 1, "accelerometer_sensor", "K6DS3TR Accelerometer", "STM", SENSOR_STRING_TYPE_ACCELEROMETER,
     78.4532f, kAccelScale, .25f, 5000, 160000, 0, kAccelScale},
    {2, 2, "geomagnetic_sensor", "AK09916 Magnetometer", "Asahi Kasei Microdevices",
     SENSOR_STRING_TYPE_MAGNETIC_FIELD, 2000, .06f, 6, 10000, 160000, 0, .06f},
    {4, 4, "gyro_sensor", "K6DS3TR Gyroscope", "STM", SENSOR_STRING_TYPE_GYROSCOPE, 17.453293f,
     kGyroScale, 6.1f, 5000, 160000, 0, kGyroScale},
    {5, 5, "light_sensor", "TMD3725 Light", "AMS, Inc.", SENSOR_STRING_TYPE_LIGHT, 60000, 1, .75f,
     200000, 200000, SENSOR_FLAG_ON_CHANGE_MODE, 1},
    {6, 6, "pressure_sensor", "LPS25H Barometer", "STM", SENSOR_STRING_TYPE_PRESSURE, 1013.25f, 1,
     1, 180000, 180000, 0, 1},
    {8, 8, "proximity_sensor", "TMD3725 Proximity", "AMS, Inc.", SENSOR_STRING_TYPE_PROXIMITY, 8, 8,
     .75f, 0, 0, SENSOR_FLAG_ON_CHANGE_MODE | SENSOR_FLAG_WAKE_UP, 1},
    {14, 14, "uncal_geomagnetic_sensor", "AK09916C Magnetometer UnCalibrated",
     "Asahi Kasei Microdevices", SENSOR_STRING_TYPE_MAGNETIC_FIELD_UNCALIBRATED, 1200, .06f, 6,
     10000, 160000, 0, .06f},
    {16, 16, "uncal_gyro_sensor", "K6DS3TR Gyroscope Uncalibrated", "STM",
     SENSOR_STRING_TYPE_GYROSCOPE_UNCALIBRATED, 17.453293f, kGyroScale, 6.1f, 5000, 160000, 0,
     kGyroScale},
    {17, 17, "sig_motion_sensor", "Samsung Significant Motion", "Samsung Inc.",
     SENSOR_STRING_TYPE_SIGNIFICANT_MOTION, 1, 1, .25f, -1, 0,
     SENSOR_FLAG_ONE_SHOT_MODE | SENSOR_FLAG_WAKE_UP, 1},
    {18, 18, "step_det_sensor", "Samsung Step Detector", "Samsung Inc.",
     SENSOR_STRING_TYPE_STEP_DETECTOR, 1, 1, .25f, 0, 0, SENSOR_FLAG_SPECIAL_REPORTING_MODE, 1},
    {19, 19, "step_cnt_sensor", "Samsung Step Counter", "Samsung Inc.",
     SENSOR_STRING_TYPE_STEP_COUNTER, 4294967295.0f, 1, .25f, 0, 0, SENSOR_FLAG_ON_CHANGE_MODE, 1},
    {22, 22, "tilt_detector", "Samsung Tilt Detector", "Samsung Inc.",
     SENSOR_STRING_TYPE_TILT_DETECTOR, 1, 1, .25f, 0, 0,
     SENSOR_FLAG_SPECIAL_REPORTING_MODE | SENSOR_FLAG_WAKE_UP, 1},
    {25, 25, "pickup_gesture", "Samsung Pick Up Gesture", "Samsung Inc.",
     SENSOR_STRING_TYPE_PICK_UP_GESTURE, 1, 1, .25f, -1, 0,
     SENSOR_FLAG_ONE_SHOT_MODE | SENSOR_FLAG_WAKE_UP, 1},
    {31, 65579, "interrupt_gyro_sensor", "Interrupt Gyroscope", "Samsung Inc.",
     "com.samsung.sensor.interrupt_gyro", 17.453293f, kGyroScale, 6.1f, 20000, 20000,
     SENSOR_FLAG_ON_CHANGE_MODE, kGyroScale},
    {32, 65586, "scontext_iio", "SContext", "Samsung", "com.samsung.sensor.sensorhubs", 1, 1, 0, 0,
     20000, SENSOR_FLAG_SPECIAL_REPORTING_MODE, 1},
    {34, 65587, "light_cct_sensor", "TMD3725 Light CCT", "AMS, Inc.",
     "com.samsung.sensor.light_cct", 60000, 1, .75f, 200000, 200000, 0, 1},
};

struct BatchConfig {
    int64_t timeout;
    int64_t delay;
    int32_t flags;
};
static_assert(offsetof(BatchConfig, flags) == 16);
static_assert(sizeof(BatchConfig) == 24);

int64_t bootTime() {
    timespec value{};
    clock_gettime(CLOCK_BOOTTIME, &value);
    return int64_t(value.tv_sec) * 1000000000LL + value.tv_nsec;
}
}  // namespace

Sensors::Sensors() { discover(); }
Sensors::~Sensors() { stop(); }

void Sensors::discover() {
    DIR* directory = opendir(kIio);
    if (!directory) {
        ALOGE("Cannot enumerate SSP IIO devices: %s", strerror(errno));
        return;
    }
    while (dirent* entry = readdir(directory)) {
        if (strncmp(entry->d_name, "iio:device", 10)) continue;
        std::string path = std::string(kIio) + entry->d_name;
        const std::string name = readValue(path + "/name");
        for (const auto& descriptor : kSensors) {
            if (name != descriptor.iio) continue;
            auto& sensor = sensors[descriptor.handle];
            if (sensor.fd >= 0) continue;
            android::base::unique_fd fd(open(("/dev/" + std::string(entry->d_name)).c_str(),
                                             O_RDONLY | O_NONBLOCK | O_CLOEXEC));
            if (fd < 0) {
                ALOGE("Cannot open %s: %s", entry->d_name, strerror(errno));
                continue;
            }
            // Match the stock buffer depth. Configure it before enabling the buffer.
            if (writeValue(path + "/buffer/enable", 0) ||
                writeValue(path + "/buffer/length", 600) ||
                writeValue(path + "/buffer/enable", 1)) {
                ALOGE("Cannot configure SSP buffer %s", entry->d_name);
                continue;
            }
            sensor.fd = std::move(fd);
            sensor.scale = descriptor.scale;
            auto& info = sensor.info;
            info.name = descriptor.name;
            info.vendor = descriptor.vendor;
            info.stringType = descriptor.typeName;
            info.requiredPermission = (descriptor.handle == 18 || descriptor.handle == 19)
                                          ? "android.permission.ACTIVITY_RECOGNITION"
                                          : "";
            info.version = descriptor.handle == 17 ? 2 : 1;
            info.handle = descriptor.handle;
            info.type = descriptor.type;
            info.maxRange = descriptor.range;
            info.resolution = descriptor.resolution;
            info.power = descriptor.power;
            info.minDelay = descriptor.minDelay;
            info.maxDelay = descriptor.maxDelay;
            info.flags = descriptor.flags;
        }
    }
    closedir(directory);
    for (const auto& sensor : sensors)
        if (sensor.fd >= 0) sensorList.push_back(sensor.info);
}

int Sensors::start() {
    std::lock_guard lock(mutex);
    if (thread.joinable()) return -EBUSY;
    if (sensorList.empty()) return -ENODEV;
    int result = disableAll();
    if (result) return result;
    controlFd.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (controlFd < 0) return -errno;
    enabledMask = 0;
    stopping = false;
    thread = std::thread(&Sensors::readLoop, this);
    return 0;
}

void Sensors::stop() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
        available.notify_all();
        if (controlFd >= 0) {
            uint64_t wake = 1;
            android::base::WriteFully(controlFd.get(), &wake, sizeof(wake));
        }
    }
    if (thread.joinable()) thread.join();
    std::lock_guard lock(mutex);
    if (controlFd >= 0) disableAll();
    for (auto& sensor : sensors) {
        sensor.enabled = false;
        sensor.pendingFlush = 0;
    }
    enabledMask = 0;
    events.clear();
    queuedWakeups = 0;
    releaseWakeLock();
    controlFd.reset();
}

int Sensors::getList(const sensor_t** list) const {
    *list = sensorList.data();
    return sensorList.size();
}

int Sensors::disableAll() {
    uint64_t mask;
    if (!android::base::ParseUint(readValue(std::string(kSsp) + "enable"), &mask)) return -EIO;
    // The SSP sysfs handler processes only one changed bit per write.
    while (mask) {
        mask &= mask - 1;
        int result = writeValue(std::string(kSsp) + "enable", mask);
        if (result) return result;
    }
    return 0;
}

int Sensors::setEnabled(Sensor& sensor, bool enabled) {
    const uint64_t bit = 1ULL << sensor.info.handle;
    const uint64_t mask = enabled ? enabledMask | bit : enabledMask & ~bit;
    const int64_t activatedAt = enabled ? bootTime() : 0;
    int result = writeValue(std::string(kSsp) + "enable", mask);
    if (result) return result;
    enabledMask = mask;
    sensor.enabled = enabled;
    if (enabled) sensor.activatedAt = activatedAt;
    return 0;
}

int Sensors::activate(int handle, bool enabled) {
    std::lock_guard lock(mutex);
    if (handle <= 0 || size_t(handle) >= sensors.size() || sensors[handle].fd < 0) return -EINVAL;
    if (stopping) return -ENODEV;
    auto& sensor = sensors[handle];
    const bool kernelEnabled = enabledMask & (1ULL << handle);
    if (sensor.enabled == enabled && kernelEnabled == enabled) return 0;
    // A fired one-shot is logically disabled even if its kernel disarm failed.
    // Clear the old enable bit before rearming it, otherwise SSP sees no change.
    if (enabled && !sensor.enabled && kernelEnabled) {
        int result = setEnabled(sensor, false);
        if (result) return result;
    }
    int result = setEnabled(sensor, enabled);
    if (result) return result;
    // Drop already queued measurements from the previous activation, but retain
    // flush completions: clients can deactivate while a flush is pending.
    for (auto it = events.begin(); it != events.end();) {
        if (it->type != SENSOR_TYPE_META_DATA && it->sensor == handle) {
            if (sensor.info.flags & SENSOR_FLAG_WAKE_UP) --queuedWakeups;
            it = events.erase(it);
        } else {
            ++it;
        }
    }
    available.notify_all();
    return 0;
}

int Sensors::setBatch(Sensor& sensor, int64_t period, int64_t latency) {
    android::base::unique_fd fd(open("/dev/batch_io", O_RDONLY | O_CLOEXEC));
    if (fd < 0) return -errno;
    BatchConfig config{latency, period, 0};
    // The Samsung command encodes pointer size, not sizeof(BatchConfig).
    const unsigned long command = _IOR(0xfc, sensor.info.handle, void*);
    if (ioctl(fd.get(), command, &config) < 0) return -errno;
    return 0;
}

int Sensors::batch(int handle, int flags, int64_t period, int64_t latency) {
    std::lock_guard lock(mutex);
    if (handle <= 0 || size_t(handle) >= sensors.size() || sensors[handle].fd < 0 || period < 0 ||
        latency < 0 || (flags & ~SENSORS_BATCH_DRY_RUN))
        return -EINVAL;
    if (stopping) return -ENODEV;
    if (period == 0) period = 200000000;
    const auto& info = sensors[handle].info;
    if (info.minDelay > 0) period = std::max(period, int64_t(info.minDelay) * 1000);
    if (info.maxDelay > 0) period = std::min(period, int64_t(info.maxDelay) * 1000);
    if (flags & SENSORS_BATCH_DRY_RUN) return 0;
    return setBatch(sensors[handle], std::max(period, int64_t(1)), latency);
}

int Sensors::flush(int handle) {
    std::lock_guard lock(mutex);
    if (handle <= 0 || size_t(handle) >= sensors.size()) return -EINVAL;
    auto& sensor = sensors[handle];
    if (stopping || !sensor.enabled ||
        (sensor.info.flags & SENSOR_FLAG_MASK_REPORTING_MODE) == SENSOR_FLAG_ONE_SHOT_MODE)
        return -EINVAL;
    int result = writeValue(std::string(kSsp) + "ssp_flush", handle);
    if (!result) ++sensor.pendingFlush;
    return result;
}

void Sensors::releaseWakeLock() {
    if (wakeLockHeld) release_wake_lock(kWakeLock);
    wakeLockHeld = false;
}

void Sensors::queueEvent(const sensors_event_t& event, bool wakeup) {
    if (wakeup) {
        if (!wakeLockHeld) wakeLockHeld = acquire_wake_lock(PARTIAL_WAKE_LOCK, kWakeLock) >= 0;
        ++queuedWakeups;
    }
    events.push_back(event);
    available.notify_all();
}

int Sensors::poll(sensors_event_t* output, int count) {
    if (!output || count <= 0) return -EINVAL;
    std::unique_lock lock(mutex);
    // Entering poll() acknowledges the caller's previous delivery. Recheck
    // after every notification: deactivate() can empty the queue while we wait.
    while (true) {
        if (!queuedWakeups) releaseWakeLock();
        if (stopping) return -ENODEV;
        if (!events.empty()) break;
        available.wait(lock);
    }
    int copied = 0;
    while (copied < count && !events.empty()) {
        const auto& event = events.front();
        const int handle =
            event.type == SENSOR_TYPE_META_DATA ? event.meta_data.sensor : event.sensor;
        if (handle > 0 && size_t(handle) < sensors.size() &&
            (sensors[handle].info.flags & SENSOR_FLAG_WAKE_UP))
            --queuedWakeups;
        output[copied++] = event;
        events.pop_front();
    }
    available.notify_all();
    return copied;
}

void Sensors::stopOnError() {
    std::lock_guard lock(mutex);
    stopping = true;
    disableAll();
    events.clear();
    queuedWakeups = 0;
    releaseWakeLock();
    available.notify_all();
}

void Sensors::readLoop() {
    std::vector<pollfd> descriptors{{controlFd.get(), POLLIN, 0}};
    std::vector<int> handles{0};
    for (size_t handle = 1; handle < sensors.size(); ++handle) {
        if (sensors[handle].fd < 0) continue;
        descriptors.push_back({sensors[handle].fd.get(), POLLIN, 0});
        handles.push_back(handle);
    }
    while (true) {
        int result;
        do {
            result = ::poll(descriptors.data(), descriptors.size(), -1);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            ALOGE("SSP poll failed: %s", strerror(errno));
            stopOnError();
            return;
        }
        if (descriptors[0].revents) return;
        for (size_t i = 1; i < descriptors.size(); ++i) {
            if (descriptors[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                ALOGE("SSP device %d failed: poll flags %#x", handles[i], descriptors[i].revents);
                stopOnError();
                return;
            }
            if (!(descriptors[i].revents & POLLIN)) continue;
            const int handle = handles[i];
            auto& sensor = sensors[handle];
            const size_t recordSize = payloadSize(handle) + sizeof(int64_t);
            std::array<uint8_t, 72 * 32> buffer;
            const ssize_t length = read(sensor.fd.get(), buffer.data(), recordSize * 32);
            if (length < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (length <= 0 || size_t(length) % recordSize) {
                ALOGE("Invalid SSP read for sensor %d: %zd", handle, length);
                stopOnError();
                return;
            }
            for (size_t offset = 0; offset < size_t(length); offset += recordSize) {
                Sample sample;
                if (!decode(handle, buffer.data() + offset, recordSize, sensor.scale, &sample))
                    continue;
                std::unique_lock lock(mutex);
                available.wait(lock, [&] { return stopping || events.size() < kQueueLimit; });
                if (stopping) return;
                sensors_event_t event{};
                bool wakeup = sensor.info.flags & SENSOR_FLAG_WAKE_UP;
                if (sample.flush) {
                    if (!sensor.pendingFlush) continue;
                    --sensor.pendingFlush;
                    event.version = META_DATA_VERSION;
                    event.type = SENSOR_TYPE_META_DATA;
                    event.meta_data.what = META_DATA_FLUSH_COMPLETE;
                    event.meta_data.sensor = handle;
                } else {
                    if (!sensor.enabled ||
                        (sample.timestamp > 0 && sample.timestamp < sensor.activatedAt))
                        continue;
                    event.version = sizeof(event);
                    event.sensor = handle;
                    event.type = sensor.info.type;
                    event.timestamp = sample.timestamp > 0 ? sample.timestamp : bootTime();
                    std::copy(sample.values.begin(), sample.values.end(), event.data);
                    if (handle == 19)
                        event.u64.step_counter = sample.steps;
                    else if (handle == 1 || handle == 2 || handle == 4)
                        event.acceleration.status = sample.accuracy;
                    queueEvent(event, wakeup);
                    if ((sensor.info.flags & SENSOR_FLAG_MASK_REPORTING_MODE) ==
                        SENSOR_FLAG_ONE_SHOT_MODE) {
                        int error = setEnabled(sensor, false);
                        if (error) {
                            // Never deliver a second trigger for one activation.
                            sensor.enabled = false;
                            ALOGE("Cannot disarm one-shot sensor %d: %d", handle, error);
                        }
                    }
                    continue;
                }
                queueEvent(event, wakeup);
            }
        }
    }
}
}  // namespace ssp
