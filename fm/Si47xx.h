/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class Si47xx {
   public:
    bool open();
    bool close();
    bool powerUp(float mhz);
    bool powerDown();
    bool tune(float mhz);
    float seek(float mhz, bool up);
    std::vector<int16_t> scan();
    bool stopScan();
    int mute(bool enabled);

   private:
    int dupFd();
    bool command(unsigned long request, void* value = nullptr);
    std::mutex fdMutex;
    std::mutex operationMutex;
    std::atomic<bool> cancelled{false};
    int fd = -1;
};
