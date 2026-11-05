// SPDX-License-Identifier: Apache-2.0
#include "Trustlet.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace samsung::gatekeeper {
namespace {

using android::base::unique_fd;

constexpr char kTrustlet[] = "/vendor/app/mcRegistry/08130000000000000000000000000000.tlbin";
constexpr uint32_t kGatekeeperSpid = 0xfffffffd;
constexpr size_t kSlot = 0x400;
constexpr size_t kContextSize = 108;
constexpr size_t kHandleSize = 58;
constexpr size_t kTokenSize = 69;
constexpr size_t kMaxPasswordSize = 100;
constexpr int32_t kCommandTimeoutMs = 10000;

struct BulkBuffer {
    // The trustlet uses 32-bit addresses in this five-slot shared buffer.
    std::array<uint8_t, 5 * kSlot> bytes{};
    ~BulkBuffer() {
        volatile uint8_t* data = bytes.data();
        for (size_t i = 0; i < bytes.size(); ++i) data[i] = 0;
    }
};

bool readContext(const std::string& path, uint8_t* context) {
    unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd < 0) return errno == ENOENT;
    return android::base::ReadFully(fd, context, kContextSize);
}

bool writeContext(const std::string& path, const uint8_t* context) {
    std::string temporary = path + ".XXXXXX";
    unique_fd fd(mkostemp(temporary.data(), O_CLOEXEC));
    if (fd < 0) return false;
    if (android::base::WriteFully(fd, context, kContextSize) && fsync(fd) == 0 &&
        rename(temporary.c_str(), path.c_str()) == 0) {
        return true;
    }
    unlink(temporary.c_str());
    return false;
}

}  // namespace

Trustlet::Trustlet(std::string contextDirectory) : contextDirectory_(std::move(contextDirectory)) {}
Trustlet::~Trustlet() { close(); }

bool Trustlet::open() {
    if (opened_) return true;
    std::string blob;
    if (!android::base::ReadFileToString(kTrustlet, &blob) || blob.empty() ||
        blob.size() > std::numeric_limits<uint32_t>::max()) {
        LOG(ERROR) << "Cannot read Gatekeeper trustlet";
        return false;
    }
    if (const auto result = mcOpenDevice(MC_DEVICE_ID_DEFAULT); result != MC_DRV_OK) {
        LOG(ERROR) << "Cannot open MobiCore device: " << result;
        return false;
    }
    session_ = {};
    tci_.fill(0);
    const auto result =
            mcOpenTrustlet(&session_, kGatekeeperSpid, reinterpret_cast<uint8_t*>(blob.data()),
                           blob.size(), reinterpret_cast<uint8_t*>(tci_.data()), sizeof(tci_));
    if (result != MC_DRV_OK) {
        LOG(ERROR) << "Cannot open Gatekeeper trustlet: " << result;
        mcCloseDevice(MC_DEVICE_ID_DEFAULT);
        return false;
    }
    opened_ = true;
    return true;
}

void Trustlet::close() {
    if (!opened_) return;
    mcCloseSession(&session_);
    mcCloseDevice(MC_DEVICE_ID_DEFAULT);
    opened_ = false;
    tci_.fill(0);
}

Result Trustlet::enroll(uint32_t uid, const std::vector<uint8_t>& handle,
                        const std::vector<uint8_t>& current, const std::vector<uint8_t>& desired) {
    if (handle.empty() != current.empty() || desired.empty()) return {};
    return transact(Command::Enroll, uid, 0, handle, current, desired);
}

Result Trustlet::verify(uint32_t uid, uint64_t challenge, const std::vector<uint8_t>& handle,
                        const std::vector<uint8_t>& password) {
    if (handle.empty() || password.empty()) return {};
    return transact(Command::Verify, uid, challenge, handle, password, {});
}

Result Trustlet::transact(Command command, uint32_t uid, uint64_t challenge,
                          const std::vector<uint8_t>& handle, const std::vector<uint8_t>& password,
                          const std::vector<uint8_t>& desired) {
    if ((!handle.empty() && handle.size() != kHandleSize) || password.size() > kMaxPasswordSize ||
        desired.size() > kMaxPasswordSize) {
        return {};
    }

    char suffix[9];
    snprintf(suffix, sizeof(suffix), "%x", uid);
    const std::string primary = contextDirectory_ + "/gk_context_" + suffix;
    const std::string backup = contextDirectory_ + "/gk_context_sub_" + suffix;
    const bool enroll = command == Command::Enroll;
    BulkBuffer bulk;
    auto* context = bulk.bytes.data() + kSlot;
    if (!enroll && (!readContext(primary, context) ||
                    !readContext(backup, context + kContextSize))) {
        LOG(ERROR) << "Cannot read Gatekeeper context";
        return {};
    }
    std::copy(handle.begin(), handle.end(), bulk.bytes.begin());
    std::copy(password.begin(), password.end(), bulk.bytes.begin() + 2 * kSlot);
    std::copy(desired.begin(), desired.end(), bulk.bytes.begin() + 3 * kSlot);

    timespec now{};
    if (clock_gettime(CLOCK_BOOTTIME, &now) != 0 || !open()) return {};
    const size_t mapSize = (enroll ? 5 : 4) * kSlot;
    mcBulkMap_t map{};
    if (const auto result = mcMap(&session_, bulk.bytes.data(), mapSize, &map); result != MC_DRV_OK) {
        LOG(ERROR) << "Cannot map Gatekeeper buffer: " << result;
        close();
        return {};
    }

    const uint32_t address = map.sVirtualAddr;
    const uint64_t timestamp = uint64_t{1000} * now.tv_sec + now.tv_nsec / 1000000;
    tci_.fill(0);
    tci_[0] = static_cast<uint32_t>(command);
    tci_[4] = uid;
    memcpy(&tci_[12], &timestamp, sizeof(timestamp));

    const uint32_t outputOffset = (enroll ? 4 : 3) * kSlot;
    tci_[14] = address + outputOffset;
    if (enroll) {
        tci_[5] = address;
        tci_[6] = handle.size();
        tci_[7] = address + 2 * kSlot;
        tci_[8] = password.size();
        tci_[9] = address + 3 * kSlot;
        tci_[10] = desired.size();
        tci_[17] = address + kSlot;
    } else {
        memcpy(&tci_[6], &challenge, sizeof(challenge));
        tci_[8] = address;
        tci_[9] = handle.size();
        tci_[10] = address + 2 * kSlot;
        tci_[11] = password.size();
        tci_[15] = kTokenSize;
        tci_[18] = address + kSlot;
    }

    auto transport = mcNotify(&session_);
    if (transport == MC_DRV_OK) transport = mcWaitNotification(&session_, kCommandTimeoutMs);
    if (transport != MC_DRV_OK) {
        LOG(ERROR) << "Gatekeeper transaction failed: " << transport;
        close();
        return {};
    }
    if (const auto result = mcUnmap(&session_, bulk.bytes.data(), &map); result != MC_DRV_OK) {
        LOG(ERROR) << "Cannot unmap Gatekeeper buffer: " << result;
        close();
        return {};
    }

    // A rejected password can still update the trustlet's retry counter.
    const bool accepted = tci_[1] == 0;
    const size_t expected = enroll ? kHandleSize : kTokenSize;
    if (accepted && tci_[15] != expected) {
        LOG(ERROR) << "Unexpected Gatekeeper response size: " << tci_[15];
        return {};
    }
    if (!enroll || accepted) {
        if (!writeContext(primary, context) || !writeContext(backup, context)) {
            PLOG(ERROR) << "Cannot save Gatekeeper context";
            return {};
        }
        unique_fd dir(::open(contextDirectory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (dir < 0 || fsync(dir) != 0) {
            PLOG(ERROR) << "Cannot sync Gatekeeper context";
            return {};
        }
    }
    if (!accepted) {
        const uint32_t timeout = std::min(tci_[16], uint32_t{INT32_MAX});
        return {.timeoutMs = static_cast<int32_t>(timeout), .data = {}};
    }
    auto output = bulk.bytes.begin() + outputOffset;
    return {.success = true,
            .reenroll = !enroll && (tci_[17] & 0xff) != 0,
            .data = {output, output + expected}};
}

}  // namespace samsung::gatekeeper
