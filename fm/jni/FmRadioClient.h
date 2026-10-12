/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class FmRadioClient {
  public:
    bool open();
    bool close();
    bool powerUp(float mhz);
    bool powerDown();
    bool tune(float mhz);
    float seek(float mhz, bool up);
    std::vector<int16_t> scan();
    bool stopScan();
    int32_t mute(bool enabled);
    bool setRds(bool enabled);
    int32_t readRds();
    std::string programName();
    std::string radioText();
    int32_t activeAf();
};
