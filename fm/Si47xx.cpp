/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "Si47xx"
#include "Si47xx.h"

#include <fcntl.h>
#include <log/log.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cmath>

namespace {
// Samsung's Si47xx private ABI uses 10 kHz units, not V4L2 frequencies.
constexpr unsigned long powerUpCommand = _IO(0xfa, 0);
constexpr unsigned long powerDownCommand = _IO(0xfa, 1);
constexpr unsigned long bandCommand = _IOW(0xfa, 2, int32_t);
constexpr unsigned long spacingCommand = _IOW(0xfa, 3, int32_t);
constexpr unsigned long tuneCommand = _IOW(0xfa, 4, uint32_t);
constexpr unsigned long seekUpCommand = _IOR(0xfa, 6, uint32_t);
constexpr unsigned long seekDownCommand = _IOR(0xfa, 7, uint32_t);
constexpr unsigned long volumeCommand = _IOW(0xfa, 15, uint8_t);
constexpr unsigned long muteOnCommand = _IO(0xfa, 17);
constexpr unsigned long muteOffCommand = _IO(0xfa, 18);
constexpr unsigned long stereoCommand = _IO(0xfa, 20);
constexpr unsigned long rdsOffCommand = _IO(0xfa, 24);
constexpr unsigned long cancelCommand = _IO(0xfa, 26);
constexpr unsigned long emphasisCommand = _IOW(0xfa, 32, uint8_t);
constexpr unsigned long seekFullCommand = _IOR(0xfa, 40, uint32_t);

bool frequency(float mhz, uint32_t* result) {
    if (!std::isfinite(mhz) || mhz < 87.5f || mhz > 108.0f) return false;
    *result = std::lround(mhz * 100.0f);
    return true;
}
}  // namespace

int Si47xx::dupFd() {
    std::lock_guard<std::mutex> lock(fdMutex);
    return fd < 0 ? -1 : fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

bool Si47xx::command(unsigned long request, void* value) {
    int device = dupFd();
    if (device < 0) return false;
    int result = ioctl(device, request, value);
    if (result < 0) ALOGE("ioctl %#lx failed: %m", request);
    ::close(device);
    return result >= 0;
}

bool Si47xx::open() {
    std::lock_guard<std::mutex> operation(operationMutex);
    std::lock_guard<std::mutex> lock(fdMutex);
    if (fd >= 0) return true;
    fd = ::open("/dev/radio0", O_RDWR | O_CLOEXEC);
    if (fd < 0) ALOGE("Cannot open /dev/radio0: %m");
    return fd >= 0;
}

bool Si47xx::close() {
    stopScan();
    std::lock_guard<std::mutex> operation(operationMutex);
    std::lock_guard<std::mutex> lock(fdMutex);
    if (fd < 0) return true;
    // release() in the kernel does not switch the tuner off.
    bool result = ioctl(fd, powerDownCommand, nullptr) >= 0;
    ::close(fd);
    fd = -1;
    return result;
}

bool Si47xx::powerUp(float mhz) {
    std::lock_guard<std::mutex> operation(operationMutex);
    uint32_t channel;
    if (!frequency(mhz, &channel) || !command(powerUpCommand)) return false;
    int32_t band = 1;
    int32_t spacing = 10;
    uint8_t volume = 15;
    // The driver's enum defines 1 as 50 us, unlike the chip register bit.
    uint8_t emphasis = 1;
    if (command(muteOnCommand) && command(bandCommand, &band) &&
        command(spacingCommand, &spacing) && command(emphasisCommand, &emphasis) &&
        command(stereoCommand) && command(volumeCommand, &volume) && command(rdsOffCommand) &&
        command(tuneCommand, &channel) && command(muteOffCommand))
        return true;
    command(powerDownCommand);
    return false;
}

bool Si47xx::powerDown() {
    stopScan();
    std::lock_guard<std::mutex> operation(operationMutex);
    return command(powerDownCommand);
}

bool Si47xx::tune(float mhz) {
    std::lock_guard<std::mutex> operation(operationMutex);
    uint32_t channel;
    return frequency(mhz, &channel) && command(tuneCommand, &channel);
}

float Si47xx::seek(float mhz, bool up) {
    std::lock_guard<std::mutex> operation(operationMutex);
    uint32_t channel;
    cancelled = false;
    if (!frequency(mhz, &channel) || !command(tuneCommand, &channel) || cancelled ||
        !command(up ? seekUpCommand : seekDownCommand, &channel) || cancelled || channel < 8750 ||
        channel > 10800)
        return 0;
    return channel / 100.0f;
}

std::vector<int16_t> Si47xx::scan() {
    std::lock_guard<std::mutex> operation(operationMutex);
    cancelled = false;
    std::vector<int16_t> channels;
    uint32_t channel = 10800;
    // Seek with wrap once so a station at the lower band edge is included.
    if (!command(tuneCommand, &channel) || cancelled || !command(seekUpCommand, &channel))
        return channels;
    uint32_t previous = 0;
    while (!cancelled && channel >= 8750 && channel <= 10800 && channel > previous &&
           channels.size() < 206) {
        channels.push_back(channel);
        previous = channel;
        if (cancelled || !command(seekFullCommand, &channel)) break;
    }
    return channels;
}

bool Si47xx::stopScan() {
    cancelled = true;
    bool result = command(cancelCommand);
    // A seek can enter the kernel just after the first cancellation. Keep
    // cancelling until its operation finishes, without blocking on its mutex.
    while (!operationMutex.try_lock()) {
        usleep(10000);
        result = command(cancelCommand) && result;
    }
    operationMutex.unlock();
    return result;
}

int Si47xx::mute(bool enabled) {
    std::lock_guard<std::mutex> operation(operationMutex);
    return command(enabled ? muteOnCommand : muteOffCommand) ? 0 : -1;
}
