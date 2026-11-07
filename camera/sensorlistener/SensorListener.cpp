// SPDX-License-Identifier: Apache-2.0

#include <aidl/android/frameworks/sensorservice/BnEventQueueCallback.h>
#include <aidl/android/frameworks/sensorservice/ISensorManager.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace android {

struct SensorListenerEvent;

namespace {

using aidl::android::frameworks::sensorservice::BnEventQueueCallback;
using aidl::android::frameworks::sensorservice::IEventQueue;
using aidl::android::frameworks::sensorservice::ISensorManager;
using aidl::android::hardware::sensors::Event;
using aidl::android::hardware::sensors::SensorInfo;
using aidl::android::hardware::sensors::SensorType;

constexpr char kSensorService[] = "android.frameworks.sensorservice.ISensorManager/default";
constexpr int kAccelerometer = 1;
constexpr int kGyroscope = 2;

// The camera owns this buffer. Only write the fields belonging to the
// requested sensor, at the offsets used by both stock camera ABIs.
constexpr size_t kAccelerometerOffset = 0;
constexpr size_t kGyroscopeOffset = 16;
constexpr size_t kGyroscopeTimestampOffset = 32;

class SensorCallback final : public BnEventQueueCallback {
  public:
    explicit SensorCallback(const SensorInfo& sensor)
        : handle_(sensor.sensorHandle), type_(sensor.type) {}

    ndk::ScopedAStatus onEvent(const Event& event) override {
        using Payload = Event::EventPayload;
        if (event.sensorHandle != handle_ || event.sensorType != type_ ||
            event.payload.getTag() != Payload::vec3) {
            return ndk::ScopedAStatus::ok();
        }

        const auto& vector = event.payload.get<Payload::vec3>();
        {
            std::lock_guard lock(mutex_);
            if (!enabled_) {
                return ndk::ScopedAStatus::ok();
            }
            axes_ = {vector.x, vector.y, vector.z};
            timestamp_ = event.timestamp;
            hasSample_ = true;
            freshSample_ = true;
        }
        ready_.notify_all();
        return ndk::ScopedAStatus::ok();
    }

    int read(SensorListenerEvent* output, bool wait) {
        std::unique_lock lock(mutex_);
        if (!enabled_) {
            return -EINVAL;
        }
        if (!hasSample_) {
            return -EAGAIN;
        }
        if (wait) {
            ready_.wait_for(lock, std::chrono::seconds(2),
                            [this] { return freshSample_ || !enabled_; });
            if (!enabled_) {
                return -EINVAL;
            }
        }

        auto* bytes = reinterpret_cast<unsigned char*>(output);
        const bool gyro = type_ == SensorType::GYROSCOPE;
        std::memcpy(bytes + (gyro ? kGyroscopeOffset : kAccelerometerOffset), axes_.data(),
                    sizeof(float) * axes_.size());
        if (gyro) {
            std::memcpy(bytes + kGyroscopeTimestampOffset, &timestamp_, sizeof(timestamp_));
        }
        freshSample_ = false;
        return 0;
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            enabled_ = false;
        }
        ready_.notify_all();
    }

  private:
    const int32_t handle_;
    const SensorType type_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::array<float, 3> axes_{};
    int64_t timestamp_ = 0;
    bool enabled_ = true;
    bool hasSample_ = false;
    bool freshSample_ = false;
};

class Listener {
  public:
    ~Listener() {
        if (callback_) {
            callback_->stop();
        }
    }

    int enable(int sensor, int periodUs) {
        std::lock_guard lock(mutex_);
        if (sensor != kAccelerometer && sensor != kGyroscope) {
            return -ENODEV;
        }
        if (queue_) {
            return -EINVAL;
        }

        auto manager = ISensorManager::fromBinder(
                ndk::SpAIBinder(AServiceManager_checkService(kSensorService)));
        if (!manager) {
            return -ENODEV;
        }

        SensorInfo info;
        const auto type = sensor == kGyroscope ? SensorType::GYROSCOPE : SensorType::ACCELEROMETER;
        if (!manager->getDefaultSensor(type, &info).isOk()) {
            return -ENODEV;
        }

        ABinderProcess_startThreadPool();
        auto callback = ndk::SharedRefBase::make<SensorCallback>(info);
        std::shared_ptr<IEventQueue> queue;
        if (!manager->createEventQueue(callback, &queue).isOk() || !queue) {
            return -EIO;
        }
        if (!queue->enableSensor(info.sensorHandle, std::max(periodUs, 0), 0).isOk()) {
            return -EIO;
        }

        sensor_ = sensor;
        handle_ = info.sensorHandle;
        queue_ = std::move(queue);
        callback_ = std::move(callback);
        return 0;
    }

    int disable(int sensor) {
        std::lock_guard lock(mutex_);
        if (!queue_ || sensor != sensor_) {
            return -EINVAL;
        }

        callback_->stop();
        const bool ok = queue_->disableSensor(handle_).isOk();
        queue_.reset();
        callback_.reset();
        return ok ? 0 : -EIO;
    }

    int read(int sensor, SensorListenerEvent* output, bool wait) {
        std::shared_ptr<SensorCallback> callback;
        {
            std::lock_guard lock(mutex_);
            if (!queue_ || sensor != sensor_) {
                return -EINVAL;
            }
            callback = callback_;
        }
        return callback->read(output, wait);
    }

  private:
    std::mutex mutex_;
    int sensor_ = 0;
    int32_t handle_ = 0;
    std::shared_ptr<IEventQueue> queue_;
    std::shared_ptr<SensorCallback> callback_;
};

}  // namespace

__attribute__((visibility("default"))) void* sensor_listener_load() {
    return new (std::nothrow) Listener;
}

__attribute__((visibility("default"))) int sensor_listener_unload(void** handle) {
    if (!handle) {
        return -EINVAL;
    }
    delete static_cast<Listener*>(*handle);
    *handle = nullptr;
    return 0;
}

__attribute__((visibility("default"))) int sensor_listener_enable_sensor(void* handle, int sensor,
                                                                          int periodUs) {
    return handle ? static_cast<Listener*>(handle)->enable(sensor, periodUs) : -EINVAL;
}

__attribute__((visibility("default"))) int sensor_listener_disable_sensor(void* handle,
                                                                           int sensor) {
    return handle ? static_cast<Listener*>(handle)->disable(sensor) : -EINVAL;
}

__attribute__((visibility("default"))) int sensor_listener_get_data(void* handle, int sensor,
                                                                    SensorListenerEvent* output,
                                                                    bool wait) {
    return handle && output ? static_cast<Listener*>(handle)->read(sensor, output, wait) : -EINVAL;
}

}  // namespace android
