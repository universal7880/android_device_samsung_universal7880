/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "powerhal-universal7880"

#include <aidl/android/hardware/power/Mode.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace aidl::google::hardware::power::impl::pixel {
namespace {

using ::aidl::android::hardware::power::Mode;
using namespace std::chrono_literals;

bool readNumber(const std::string& path, int* value) {
    std::string contents;
    if (!::android::base::ReadFileToString(path, &contents)) {
        PLOG(ERROR) << "Failed to read " << path;
        return false;
    }
    if (!::android::base::ParseInt(::android::base::Trim(contents), value)) {
        LOG(ERROR) << "Invalid value in " << path;
        return false;
    }
    return true;
}

bool writeNumber(const std::string& path, int value) {
    if (!::android::base::WriteStringToFile(std::to_string(value), path)) {
        PLOG(ERROR) << "Failed to write " << value << " to " << path;
        return false;
    }
    return true;
}

bool writeProperty(const std::string& name, const std::string& value) {
    if (!::android::base::SetProperty(name, value)) {
        LOG(ERROR) << "Failed to set " << name;
        return false;
    }
    return true;
}

struct CpuLimit {
    const std::string path;
    const std::string property;
    int saved = 0;
    int applied = 0;
};

struct Input {
    const std::string path;
    const std::string property;
    bool disabled = false;
};

class PowerMode {
  public:
    PowerMode() {
        // Runtime properties retain ownership across HAL restarts, but reset at reboot.
        for (auto& input : mInputs) {
            input.disabled = ::android::base::GetBoolProperty(input.property, false);
        }
        for (auto& cpu : mCpuLimits) {
            const auto values =
                ::android::base::Split(::android::base::GetProperty(cpu.property, ""), ":");
            int saved, applied;
            if (values.size() == 2 && ::android::base::ParseInt(values[0], &saved) &&
                ::android::base::ParseInt(values[1], &applied) && saved > applied && applied > 0) {
                cpu.saved = saved;
                cpu.applied = applied;
            }
        }
        mWorker = std::thread(&PowerMode::run, this);
    }

    ~PowerMode() {
        {
            std::lock_guard lock(mLock);
            mStopping = true;
            mCondition.notify_all();
        }
        mWorker.join();
    }

    void setInteractive(bool interactive) {
        std::lock_guard lock(mLock);
        if (interactive) {
            for (auto& input : mInputs) {
                if (input.disabled && writeNumber(input.path, 1)) {
                    input.disabled = !writeProperty(input.property, "0");
                }
            }
        }
        if (mInteractive == interactive) {
            return;
        }
        mInteractive = interactive;
        ++mGeneration;
        mCondition.notify_all();
    }

    void setLowPower(bool enabled) {
        std::lock_guard lock(mLock);
        for (auto& cpu : mCpuLimits) {
            const auto maxPath = cpu.path + "/scaling_max_freq";
            if (enabled) {
                if (cpu.saved != 0) {
                    continue;
                }
                int minimum, maximum, hispeed;
                if (!readNumber(maxPath, &maximum) || maximum <= 0 ||
                    !readNumber(cpu.path + "/scaling_min_freq", &minimum) || minimum <= 0 ||
                    minimum > maximum ||
                    !readNumber(cpu.path + "/interactive/hispeed_freq", &hispeed) || hispeed <= 0) {
                    continue;
                }
                // Keep the old governor-based cap without raising another owner's limit.
                const int cap = std::clamp(hispeed, minimum, maximum);
                if (cap < maximum && writeProperty(cpu.property, std::to_string(maximum) + ":" +
                                                                     std::to_string(cap))) {
                    if (writeNumber(maxPath, cap)) {
                        cpu.saved = maximum;
                        cpu.applied = cap;
                        // cpufreq may round the request to an available frequency.
                        int applied;
                        if (readNumber(maxPath, &applied) && applied > 0 && applied != cap) {
                            cpu.applied = applied;
                            writeProperty(cpu.property,
                                          std::to_string(maximum) + ":" + std::to_string(applied));
                        }
                    } else {
                        writeProperty(cpu.property, "");
                    }
                }
            } else if (cpu.saved != 0) {
                int current;
                if (!readNumber(maxPath, &current)) {
                    continue;
                }
                // A thermal or userspace policy may have replaced our request.
                if ((current != cpu.applied || writeNumber(maxPath, cpu.saved)) &&
                    writeProperty(cpu.property, "")) {
                    cpu.saved = 0;
                }
            }
        }
    }

  private:
    void run() {
        std::unique_lock lock(mLock);
        unsigned int generation = 0;
        while (!mStopping) {
            mCondition.wait(lock, [&] { return mStopping || generation != mGeneration; });
            if (mStopping) {
                break;
            }
            generation = mGeneration;
            if (mInteractive) {
                continue;
            }

            // Input must remain available while the display is fading or showing doze content.
            // Bound the wait so screen-off does not leave a periodic wakeup running indefinitely.
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!mStopping && !mInteractive && generation == mGeneration) {
                int brightness;
                if (!readNumber("/sys/class/backlight/panel/brightness", &brightness)) {
                    break;
                }
                if (brightness == 0) {
                    for (auto& input : mInputs) {
                        int enabled;
                        if (!input.disabled && readNumber(input.path, &enabled) && enabled == 1) {
                            if (writeProperty(input.property, "1")) {
                                input.disabled = writeNumber(input.path, 0);
                                if (!input.disabled) {
                                    writeProperty(input.property, "0");
                                }
                            }
                        }
                    }
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    LOG(WARNING) << "Panel is still lit; leaving input devices enabled";
                    break;
                }
                mCondition.wait_for(lock, 50ms, [&] {
                    return mStopping || mInteractive || generation != mGeneration;
                });
            }
        }
    }

    std::mutex mLock;
    std::condition_variable mCondition;
    bool mStopping = false;
    bool mInteractive = true;
    unsigned int mGeneration = 0;
    std::array<Input, 2> mInputs{{
        {"/sys/class/sec/tsp/input/enabled", "vendor.powerhal.touchscreen.disabled"},
        {"/sys/class/sec/sec_touchkey/input/enabled", "vendor.powerhal.touchkey.disabled"},
    }};
    std::array<CpuLimit, 2> mCpuLimits{{
        {"/sys/devices/system/cpu/cpu0/cpufreq", "vendor.powerhal.cpu0.limits"},
        {"/sys/devices/system/cpu/cpu4/cpufreq", "vendor.powerhal.cpu4.limits"},
    }};
    std::thread mWorker;
};

PowerMode& powerMode() {
    static PowerMode instance;
    return instance;
}

}  // namespace

bool isDeviceSpecificModeSupported(Mode /* type */, bool* /* supported */) {
    // Advertise only modes with actions in powerhint.json.
    return false;
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    switch (type) {
        case Mode::INTERACTIVE:
            powerMode().setInteractive(enabled);
            break;
        case Mode::LOW_POWER:
            powerMode().setLowPower(enabled);
            break;
        default:
            break;
    }
    // Let libperfmgr apply the mode's governor actions and boost masks as well.
    return false;
}

}  // namespace aidl::google::hardware::power::impl::pixel
