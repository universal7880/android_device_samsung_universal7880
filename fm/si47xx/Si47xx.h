/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "RdsDecoder.h"

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
    bool setRds(bool enabled);
    int readRds();
    std::string programName();
    std::string radioText();
    int16_t activeAf();

   private:
    int dupFd();
    bool command(unsigned long request, void* value = nullptr);
    bool readGroup(RdsGroup* group);
    bool tuneLocked(uint32_t channel);
    void resetRds();
    std::mutex fdMutex;
    std::mutex operationMutex;
    std::atomic<bool> cancelled{false};
    int fd = -1;
    bool rdsEnabled = false, muted = false;
    uint32_t currentChannel = 0;
    RdsDecoder decoder;
    unsigned weakSamples = 0;
    size_t afCursor = 0;
    std::chrono::steady_clock::time_point lastAfAttempt{};
};
