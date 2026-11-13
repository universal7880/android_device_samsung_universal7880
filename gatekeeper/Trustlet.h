// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <MobiCoreDriverApi.h>

namespace samsung::gatekeeper {

struct Result {
    bool success = false;
    bool reenroll = false;
    int32_t timeoutMs = 0;
    std::vector<uint8_t> data;
};

class Trustlet {
  public:
    explicit Trustlet(std::string contextDirectory = "/mnt/efs/TEE");
    ~Trustlet();

    Trustlet(const Trustlet&) = delete;
    Trustlet& operator=(const Trustlet&) = delete;

    Result enroll(uint32_t uid, const std::vector<uint8_t>& handle,
                  const std::vector<uint8_t>& current, const std::vector<uint8_t>& desired);
    Result verify(uint32_t uid, uint64_t challenge, const std::vector<uint8_t>& handle,
                  const std::vector<uint8_t>& password);

  private:
    enum class Command : uint32_t { Enroll = 1, Verify = 2 };
    Result transact(Command command, uint32_t uid, uint64_t challenge,
                    const std::vector<uint8_t>& handle, const std::vector<uint8_t>& password,
                    const std::vector<uint8_t>& desired);
    bool open();
    void close();

    std::string contextDirectory_;
    mcSessionHandle_t session_{};
    std::array<uint32_t, 20> tci_{};
    bool opened_ = false;
};

}  // namespace samsung::gatekeeper
