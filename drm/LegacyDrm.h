// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <aidl/android/hardware/drm/IDrmFactory.h>
#include <aidl/android/hardware/drm/Status.h>
#include <media/drm/DrmAPI.h>
#include <media/hardware/CryptoAPI.h>

namespace widevine {

namespace drm = ::aidl::android::hardware::drm;
using Bytes = std::vector<uint8_t>;
using ndk::ScopedAStatus;

ScopedAStatus error(drm::Status status);
ScopedAStatus legacyStatus(android::status_t status);
std::shared_ptr<drm::IDrmPlugin> wrapDrm(android::DrmPlugin* plugin);
std::shared_ptr<drm::ICryptoPlugin> wrapCrypto(android::CryptoPlugin* plugin);
drm::SecurityLevel readSecurityLevel(const android::DrmPlugin& plugin);
bool selectSecurityLevel(android::DrmPlugin& plugin, drm::SecurityLevel level);

inline android::Vector<uint8_t> legacyBytes(const Bytes& bytes) {
    android::Vector<uint8_t> result;
    if (!bytes.empty()) result.appendArray(bytes.data(), bytes.size());
    return result;
}

inline Bytes aidlBytes(const android::Vector<uint8_t>& bytes) {
    if (bytes.isEmpty()) return {};
    return {bytes.array(), bytes.array() + bytes.size()};
}

}  // namespace widevine
