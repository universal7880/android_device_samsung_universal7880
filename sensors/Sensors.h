/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <android-base/unique_fd.h>
#include <hardware/sensors.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ssp {
struct Sensor {
    sensor_t info{};
    android::base::unique_fd fd;
    std::string bufferPath;
    float scale = 1;
    int64_t activatedAt = 0;
    unsigned pendingFlush = 0;
    bool enabled = false;
};

class Sensors {
  public:
    Sensors();
    ~Sensors();
    int start();
    void stop();
    int getList(const sensor_t** list) const;
    int activate(int handle, bool enabled);
    int batch(int handle, int flags, int64_t period, int64_t latency);
    int flush(int handle);
    int poll(sensors_event_t* events, int count);

  private:
    void discover();
    int disableAll();
    void readLoop();
    void stopOnError();
    int setBatch(Sensor& sensor, int64_t period, int64_t latency);
    int setEnabled(Sensor& sensor, bool enabled);
    void queueEvent(const sensors_event_t& event, bool wakeup);
    void releaseWakeLock();

    std::array<Sensor, 35> sensors;
    std::vector<sensor_t> sensorList;
    android::base::unique_fd controlFd;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<sensors_event_t> events;
    uint64_t enabledMask = 0;
    size_t queuedWakeups = 0;
    bool wakeLockHeld = false;
    bool stopping = true;
};
}  // namespace ssp
