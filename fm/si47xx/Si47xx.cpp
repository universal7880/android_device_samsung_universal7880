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

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

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
struct SignalQuality {
    uint8_t rssi, threshold, snr;
};
static_assert(sizeof(SignalQuality) == 3);
constexpr unsigned long signalCommand = _IOR(0xfa, 12, SignalQuality);
constexpr unsigned long rdsDataCommand = _IOR(0xfa, 22, RdsGroup);
constexpr unsigned long rdsOnCommand = _IO(0xfa, 23);
constexpr unsigned long rdsResetCommand = _IO(0xfa, 39);
constexpr unsigned long rdsOffCommand = _IO(0xfa, 24);
constexpr unsigned long cancelCommand = _IO(0xfa, 26);
constexpr unsigned long emphasisCommand = _IOW(0xfa, 32, uint8_t);
constexpr unsigned long seekFullCommand = _IOR(0xfa, 40, uint32_t);
// Conservative AF thresholds in the chip's dBuV RSSI and dB SNR units.
constexpr unsigned minAfRssi = 20, minAfImprovement = 6, minAfSnr = 4;

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
    rdsEnabled = false;
    currentChannel = 0;
    resetRds();
    return result;
}

bool Si47xx::powerUp(float mhz) {
    uint32_t channel;
    if (!frequency(mhz, &channel)) return false;
    std::lock_guard<std::mutex> operation(operationMutex);
    rdsEnabled = false;
    currentChannel = 0;
    resetRds();
    if (!command(powerUpCommand)) return false;
    int32_t band = 1;
    int32_t spacing = 10;
    uint8_t volume = 15;
    // The driver's enum defines 1 as 50 us, unlike the chip register bit.
    uint8_t emphasis = 1;
    if (command(muteOnCommand) && command(bandCommand, &band) &&
        command(spacingCommand, &spacing) && command(emphasisCommand, &emphasis) &&
        command(stereoCommand) && command(volumeCommand, &volume) && command(rdsOffCommand) &&
        tuneLocked(channel) && command(muteOffCommand)) {
        muted = false;
        return true;
    }
    command(powerDownCommand);
    currentChannel = 0;
    return false;
}

bool Si47xx::powerDown() {
    stopScan();
    std::lock_guard<std::mutex> operation(operationMutex);
    rdsEnabled = false;
    currentChannel = 0;
    resetRds();
    return command(powerDownCommand);
}

bool Si47xx::tune(float mhz) {
    std::lock_guard<std::mutex> operation(operationMutex);
    uint32_t channel;
    if (!frequency(mhz, &channel)) return false;
    if (!muted && !command(muteOnCommand)) return false;
    bool result = tuneLocked(channel);
    // Restore the caller's mute state even when tuning fails.
    if (!muted && !command(muteOffCommand)) return false;
    return result;
}

float Si47xx::seek(float mhz, bool up) {
    uint32_t channel;
    if (!frequency(mhz, &channel)) return 0;
    std::lock_guard<std::mutex> operation(operationMutex);
    cancelled = false;
    currentChannel = 0;
    resetRds();
    if (!command(tuneCommand, &channel) || cancelled ||
        !command(up ? seekUpCommand : seekDownCommand, &channel) || cancelled || channel < 8750 ||
        channel > 10800)
        return 0;
    currentChannel = channel;
    resetRds();
    if (rdsEnabled && !command(rdsResetCommand)) rdsEnabled = false;
    return channel / 100.0f;
}

std::vector<int16_t> Si47xx::scan() {
    std::lock_guard<std::mutex> operation(operationMutex);
    cancelled = false;
    currentChannel = 0;
    resetRds();
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
    currentChannel = 0;
    resetRds();
    return channels;
}

bool Si47xx::stopScan() {
    cancelled = true;
    bool result = command(cancelCommand);
    // A seek can enter the kernel just after the first cancellation. Keep
    // cancelling until its operation finishes, without blocking on its mutex.
    while (!operationMutex.try_lock()) {
        usleep(10000);
        cancelled = true;
        result = command(cancelCommand) && result;
    }
    operationMutex.unlock();
    return result;
}

int Si47xx::mute(bool enabled) {
    std::lock_guard<std::mutex> operation(operationMutex);
    if (!command(enabled ? muteOnCommand : muteOffCommand)) return -1;
    muted = enabled;
    return 0;
}

void Si47xx::resetRds() {
    decoder.reset();
    weakSamples = 0;
    afCursor = 0;
}

bool Si47xx::tuneLocked(uint32_t channel) {
    currentChannel = 0;
    resetRds();
    if (!command(tuneCommand, &channel)) return false;
    currentChannel = channel;
    if (rdsEnabled && !command(rdsResetCommand)) {
        rdsEnabled = false;
        return false;
    }
    return true;
}

bool Si47xx::setRds(bool enabled) {
    std::lock_guard<std::mutex> operation(operationMutex);
    resetRds();
    rdsEnabled = false;
    if (!enabled) return command(rdsOffCommand);
    if (!currentChannel || !command(rdsResetCommand) || !command(rdsOnCommand)) return false;
    rdsEnabled = true;
    return true;
}

bool Si47xx::readGroup(RdsGroup* group) {
    int device = dupFd();
    if (device < 0) return false;
    int result = ioctl(device, rdsDataCommand, group);
    int error = errno;
    ::close(device);
    // Empty FIFO is normal with the interrupt-driven Si47xx driver.
    if (result < 0 && error != EBUSY && error != EAGAIN)
        ALOGE("RDS read failed: %s", strerror(error));
    return result >= 0;
}

int Si47xx::readRds() {
    std::unique_lock<std::mutex> operation(operationMutex, std::try_to_lock);
    if (!operation.owns_lock() || !rdsEnabled || !currentChannel) return 0;
    int events = 0;
    for (unsigned i = 0; i < 32; ++i) {
        RdsGroup group{};
        if (!readGroup(&group)) break;
        if (group.frequency == currentChannel) events |= decoder.feed(group);
    }
    SignalQuality signal{};
    if (command(signalCommand, &signal) && signal.rssi < minAfRssi) {
        if (weakSamples < 3) ++weakSamples;
    } else
        weakSamples = 0;
    if (weakSamples >= 3 && decoder.pi() && !decoder.alternatives().empty() &&
        std::chrono::steady_clock::now() - lastAfAttempt >= std::chrono::seconds(30))
        events |= RdsDecoder::kAlternativeFrequency;
    return events;
}

std::string Si47xx::programName() {
    std::lock_guard<std::mutex> operation(operationMutex);
    return decoder.ps();
}

std::string Si47xx::radioText() {
    std::lock_guard<std::mutex> operation(operationMutex);
    return decoder.text();
}

int16_t Si47xx::activeAf() {
    std::lock_guard<std::mutex> operation(operationMutex);
    uint32_t original = currentChannel;
    const auto candidates = decoder.alternatives();
    const uint16_t pi = decoder.pi();
    auto now = std::chrono::steady_clock::now();
    if (!rdsEnabled || !original || !pi || candidates.empty() || weakSamples < 3 ||
        now - lastAfAttempt < std::chrono::seconds(30))
        return original;
    lastAfAttempt = now;
    weakSamples = 0;
    cancelled = false;
    SignalQuality baseline{};
    if (!command(signalCommand, &baseline) || baseline.rssi >= minAfRssi) return original;
    if (!command(muteOnCommand)) return original;
    uint32_t selected = original;
    // Limit probing and rotate candidates so a long AF list cannot monopolize the tuner.
    // Individual tune ioctls still depend on the kernel completing its interrupt wait.
    const auto deadline = now + std::chrono::milliseconds(2500);
    for (size_t i = 0; i < candidates.size() && i < 3 && !cancelled; ++i) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        uint32_t candidate = candidates[afCursor++ % candidates.size()];
        if (!command(tuneCommand, &candidate) || !command(rdsResetCommand)) break;
        usleep(30000);
        SignalQuality signal{};
        if (!command(signalCommand, &signal) || signal.rssi < minAfRssi ||
            signal.rssi < baseline.rssi + minAfImprovement || signal.snr < minAfSnr)
            continue;
        unsigned matches = 0;
        auto until =
            std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(800));
        while (!cancelled && std::chrono::steady_clock::now() < until) {
            RdsGroup group{};
            if (!readGroup(&group)) {
                usleep(20000);
                continue;
            }
            if (group.frequency != candidate || group.errorA >= 3 || !group.a) continue;
            if (group.a != pi) break;
            if (++matches == 2) {
                selected = candidate;
                break;
            }
        }
        if (selected != original) break;
    }
    // The app owns the actual station change. Never leave it playing an unreported frequency.
    bool restored = command(tuneCommand, &original);
    restored = command(rdsResetCommand) && restored;
    bool muteRestored = restored && command(muted ? muteOnCommand : muteOffCommand);
    if (!restored) {
        muted = true;  // Keep an unknown frequency inaudible after a failed restore.
        currentChannel = 0;
        rdsEnabled = false;
        resetRds();
    }
    return restored && muteRestored && !cancelled ? selected : 0;
}
