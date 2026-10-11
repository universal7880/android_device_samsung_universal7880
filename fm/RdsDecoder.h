/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Layout of Samsung's SI47XX_IOC_RDS_DATA_GET payload on both ARM ABIs.
struct RdsGroup {
    uint16_t a, b, c, d;
    uint8_t rssi;
    uint32_t frequency;
    uint8_t errorA, errorB, errorC, errorD;
};
static_assert(sizeof(RdsGroup) == 20);
static_assert(offsetof(RdsGroup, frequency) == 12);

class RdsDecoder {
   public:
    static constexpr int kProgramName = 0x0008;
    static constexpr int kRadioText = 0x0040;
    static constexpr int kAlternativeFrequency = 0x0080;
    void reset();
    int feed(const RdsGroup& group);
    uint16_t pi() const { return programId; }
    const std::string& ps() const { return programName; }
    const std::string& text() const { return radioText; }
    const std::vector<uint32_t>& alternatives() const { return afList; }

   private:
    void afCode(uint8_t code, uint32_t current);
    static std::string utf8(const uint8_t* bytes, size_t length);
    uint16_t programId = 0, pendingPi = 0;
    unsigned piRepeats = 0;
    std::array<uint8_t, 8> psBuffer{}, psCandidate{};
    unsigned psMask = 0;
    bool havePsCandidate = false;
    std::array<uint8_t, 64> textBuffer{};
    unsigned textMask = 0;
    int textMode = -1;
    std::string programName, radioText;
    int afCount = -1;
    bool skipAfCode = false;
    std::vector<uint32_t> afBuffer, afList;
};
