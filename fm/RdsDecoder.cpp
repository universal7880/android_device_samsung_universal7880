/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "RdsDecoder.h"

#include <algorithm>

void RdsDecoder::reset() { *this = RdsDecoder{}; }

int RdsDecoder::feed(const RdsGroup& g) {
    // BLER 3 means the block could not be corrected by the tuner.
    if (g.errorA >= 3 || g.errorB >= 3 || !g.a) return 0;
    bool versionB = (g.b & 0x0800) != 0;
    if (versionB && g.errorC < 3 && g.c != g.a) return 0;
    int events = 0;
    if (g.a != programId) {
        if (pendingPi != g.a) {
            pendingPi = g.a;
            piRepeats = 1;
            return 0;
        }
        if (++piRepeats < 2) return 0;
        if (!programName.empty()) events |= kProgramName;
        if (!radioText.empty()) events |= kRadioText;
        reset();
        programId = g.a;
    } else {
        pendingPi = 0;
        piRepeats = 0;
    }
    unsigned type = g.b >> 12;
    if (type == 0) {
        if (!versionB && g.errorC < 3) {
            // Method B pairs associate the tuned frequency with an alternative.
            auto first = static_cast<uint8_t>(g.c >> 8);
            auto second = static_cast<uint8_t>(g.c);
            if (!skipAfCode && afCount < 0 && first >= 1 && first <= 204 && second >= 1 &&
                second <= 204) {
                uint32_t f1 = 8750 + first * 10, f2 = 8750 + second * 10;
                uint32_t other = f1 == g.frequency ? f2 : (f2 == g.frequency ? f1 : 0);
                if (other && other != g.frequency && afList.size() < 25 &&
                    std::find(afList.begin(), afList.end(), other) == afList.end())
                    afList.push_back(other);
            }
            afCode(first, g.frequency);
            afCode(second, g.frequency);
        }
        if (g.errorD >= 3) return events;
        unsigned offset = g.b & 3;
        psBuffer[offset * 2] = g.d >> 8;
        psBuffer[offset * 2 + 1] = g.d;
        psMask |= 1u << offset;
        if (psMask == 15) {
            if (havePsCandidate && psCandidate == psBuffer) {
                auto decoded = utf8(psBuffer.data(), psBuffer.size());
                if (decoded != programName) {
                    programName = decoded;
                    events |= kProgramName;
                }
            }
            psCandidate = psBuffer;
            havePsCandidate = true;
            psMask = 0;
        }
    } else if (type == 2) {
        if (g.errorD >= 3 || (!versionB && g.errorC >= 3)) return events;
        // An A/B toggle or a 2A/2B change starts a new text message.
        int mode = (g.b & 0x10) | (versionB ? 1 : 0);
        if (mode != textMode) {
            textMode = mode;
            textMask = 0;
            textBuffer.fill(0);
            if (!radioText.empty()) {
                radioText.clear();
                events |= kRadioText;
            }
        }
        unsigned segment = g.b & 15;
        unsigned width = versionB ? 2 : 4;
        unsigned offset = segment * width;
        if (!versionB) {
            textBuffer[offset++] = g.c >> 8;
            textBuffer[offset++] = g.c;
        }
        textBuffer[offset++] = g.d >> 8;
        textBuffer[offset] = g.d;
        textMask |= 1u << segment;
        size_t length = versionB ? 32 : 64;
        auto end = std::find(textBuffer.begin(), textBuffer.begin() + length, '\r');
        if (end != textBuffer.begin() + length) length = end - textBuffer.begin();
        unsigned last = std::min<unsigned>(length / width, 15);
        unsigned needed = (1u << (last + 1)) - 1;
        if ((textMask & needed) == needed) {
            auto decoded = utf8(textBuffer.data(), length);
            if (decoded != radioText) {
                radioText = decoded;
                events |= kRadioText;
            }
            textMask = 0;
            textBuffer.fill(0);
        }
    }
    return events;
}

void RdsDecoder::afCode(uint8_t code, uint32_t current) {
    if (skipAfCode) {
        skipAfCode = false;
        return;
    }
    if (code == 250) {
        skipAfCode = true;  // The next code describes an LF/MF frequency.
        return;
    }
    if (code >= 224 && code <= 249) {
        afCount = code - 224;
        afBuffer.clear();
        if (!afCount) afList.clear();
    } else if (afCount > 0 && code >= 1 && code <= 204) {
        uint32_t channel = 8750 + code * 10;
        if (std::find(afBuffer.begin(), afBuffer.end(), channel) == afBuffer.end())
            afBuffer.push_back(channel);
        if (afBuffer.size() == static_cast<size_t>(afCount)) {
            afList = afBuffer;
            afList.erase(std::remove(afList.begin(), afList.end(), current), afList.end());
            afCount = -1;
        }
    }
}

std::string RdsDecoder::utf8(const uint8_t* bytes, size_t length) {
    // RDS G0 (EBU Latin), including accented letters used by European stations.
    static constexpr char32_t extended[] =
        U"áàéèíìóòúùÑÇŞß¡Ĳ"
        U"âäêëîïôöûüñçşğıĳ"
        U"ªα©‰Ğěňőπ€£$←↑→↓"
        U"º¹²³±İńűµ¿÷°¼½¾§"
        U"ÁÀÉÈÍÌÓÒÚÙŘČŠŽĐĿ"
        U"ÂÄÊËÎÏÔÖÛÜřčšžđŀ"
        U"ÃÅÆŒŷÝÕØÞŊŔĆŚŹŤð"
        U"ãåæœŵýõøþŋŕćśźť ";
    static_assert(sizeof(extended) / sizeof(extended[0]) == 129);
    std::string result;
    for (size_t i = 0; i < length; ++i) {
        uint32_t c = bytes[i];
        if (c >= 128)
            c = extended[c - 128];
        else if (c < 32 || c == 0x60 || c == 0x7f)
            c = ' ';
        else if (c == 0x24)
            c = 0x00a4;
        if (c < 128)
            result += static_cast<char>(c);
        else if (c < 2048) {
            result += static_cast<char>(0xc0 | (c >> 6));
            result += static_cast<char>(0x80 | (c & 63));
        } else {
            result += static_cast<char>(0xe0 | (c >> 12));
            result += static_cast<char>(0x80 | ((c >> 6) & 63));
            result += static_cast<char>(0x80 | (c & 63));
        }
    }
    return result;
}
