// SPDX-License-Identifier: Apache-2.0
#include <aidl/android/hardware/drm/BnCryptoPlugin.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <cutils/ashmem.h>
#include <fcntl.h>
#include <media/stagefright/foundation/AString.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>

#include "LegacyDrm.h"

namespace widevine {
namespace {

using drm::Status;

class Mapping {
   public:
    Mapping(void* address, size_t size) : address_(address), size_(size) {}
    ~Mapping() { munmap(address_, size_); }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;

    uint8_t* at(uint64_t offset, uint64_t length) const {
        if (offset > size_ || length > size_ - offset) return nullptr;
        return static_cast<uint8_t*>(address_) + offset;
    }

   private:
    void* const address_;
    const size_t size_;
};

class Crypto final : public drm::BnCryptoPlugin {
   public:
    explicit Crypto(android::CryptoPlugin* plugin) : plugin_(plugin) {}

    ScopedAStatus getLogMessages(std::vector<drm::LogMessage>* result) override {
        result->clear();
        return ScopedAStatus::ok();
    }

    ScopedAStatus notifyResolution(int32_t width, int32_t height) override {
        if (width < 0 || height < 0) return error(Status::BAD_VALUE);
        std::lock_guard lock(mutex_);
        plugin_->notifyResolution(width, height);
        return ScopedAStatus::ok();
    }

    ScopedAStatus requiresSecureDecoderComponent(const std::string& mime, bool* result) override {
        std::lock_guard lock(mutex_);
        *result = plugin_->requiresSecureDecoderComponent(mime.c_str());
        return ScopedAStatus::ok();
    }

    ScopedAStatus setMediaDrmSession(const Bytes& session) override {
        std::lock_guard lock(mutex_);
        return legacyStatus(plugin_->setMediaDrmSession(legacyBytes(session)));
    }

    ScopedAStatus setSharedBufferBase(const drm::SharedBuffer& buffer) override {
        std::lock_guard lock(mutex_);
        if (buffer.handle.fds.empty() && buffer.size == 0) {
            buffers_.erase(buffer.bufferId);
            return ScopedAStatus::ok();
        }
        if (buffer.handle.fds.size() != 1 || buffer.offset != 0 || buffer.size <= 0 ||
            static_cast<uint64_t>(buffer.size) > std::numeric_limits<size_t>::max()) {
            return error(Status::BAD_VALUE);
        }

        int fd = buffer.handle.fds.front().get();
        struct stat info{};
        if (fstat(fd, &info) != 0) return error(Status::BAD_VALUE);
        int64_t backingSize;
        if (S_ISREG(info.st_mode)) {
            // Prevent a client from shrinking the backing file after mmap.
            const int seals = fcntl(fd, F_GET_SEALS);
            if (seals < 0 || !(seals & F_SEAL_SHRINK)) return error(Status::BAD_VALUE);
            backingSize = info.st_size;
        } else if (S_ISCHR(info.st_mode) && ashmem_valid(fd)) {
            backingSize = ashmem_get_size_region(fd);
        } else {
            return error(Status::BAD_VALUE);
        }
        if (backingSize < buffer.size) return error(Status::BAD_VALUE);
        auto size = static_cast<size_t>(buffer.size);
        void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) return error(Status::ERROR_DRM_CANNOT_HANDLE);
        buffers_[buffer.bufferId] = std::make_unique<Mapping>(addr, size);
        return ScopedAStatus::ok();
    }

    ScopedAStatus decrypt(const drm::DecryptArgs& args, int32_t* result) override {
        const bool clear = args.mode == drm::Mode::UNENCRYPTED;
        const std::array<uint8_t, 16> emptyKeyOrIv{};
        const auto validKeyOrIv = [clear](const Bytes& bytes) {
            return bytes.size() == 16 || (clear && bytes.empty());
        };
        if (!validKeyOrIv(args.keyId) || !validKeyOrIv(args.iv) || args.offset < 0 ||
            args.source.offset < 0 || args.source.size < 0 || args.pattern.encryptBlocks < 0 ||
            args.pattern.skipBlocks < 0) {
            return error(Status::BAD_VALUE);
        }
        using Legacy = android::CryptoPlugin;
        Legacy::Mode mode;
        switch (args.mode) {
            case drm::Mode::UNENCRYPTED:
                mode = Legacy::kMode_Unencrypted;
                break;
            case drm::Mode::AES_CTR:
                mode = Legacy::kMode_AES_CTR;
                break;
            case drm::Mode::AES_CBC_CTS:
                mode = Legacy::kMode_AES_WV;
                break;
            case drm::Mode::AES_CBC:
                mode = Legacy::kMode_AES_CBC;
                break;
            default:
                return error(Status::BAD_VALUE);
        }

        std::vector<Legacy::SubSample> samples;
        samples.reserve(args.subSamples.size());
        uint64_t total = 0;
        for (const auto& sample : args.subSamples) {
            if (sample.numBytesOfClearData < 0 || sample.numBytesOfEncryptedData < 0 ||
                (clear && sample.numBytesOfEncryptedData != 0)) {
                return error(Status::BAD_VALUE);
            }
            auto clearBytes = static_cast<uint32_t>(sample.numBytesOfClearData);
            auto encrypted = static_cast<uint32_t>(sample.numBytesOfEncryptedData);
            total += static_cast<uint64_t>(clearBytes) + encrypted;
            if (total > std::numeric_limits<int32_t>::max()) return error(Status::BAD_VALUE);
            samples.push_back({clearBytes, encrypted});
        }
        if (total > static_cast<uint64_t>(args.source.size)) return error(Status::BAD_VALUE);

        std::lock_guard lock(mutex_);
        auto source = buffers_.find(args.source.bufferId);
        if (source == buffers_.end()) return error(Status::BAD_VALUE);
        const auto offset = static_cast<uint64_t>(args.source.offset) + args.offset;
        const auto* input = source->second->at(offset, args.source.size);
        if (!input) return error(Status::BAD_VALUE);

        using Tag = drm::DestinationBuffer::Tag;
        std::unique_ptr<native_handle_t, decltype(&native_handle_delete)> handle(
            nullptr, native_handle_delete);
        void* output;
        if (args.destination.getTag() == Tag::secureMemory) {
            if (!args.secure) return error(Status::BAD_VALUE);
            handle.reset(::android::makeFromAidl(args.destination.get<Tag::secureMemory>()));
            if (!handle) return error(Status::BAD_VALUE);
            output = handle.get();
        } else {
            if (args.secure) return error(Status::BAD_VALUE);
            const auto& dest = args.destination.get<Tag::nonsecureMemory>();
            auto mapping = buffers_.find(dest.bufferId);
            if (mapping == buffers_.end() || dest.offset < 0 || dest.size < 0 ||
                total > static_cast<uint64_t>(dest.size)) {
                return error(Status::BAD_VALUE);
            }
            output = mapping->second->at(dest.offset, dest.size);
            if (!output) return error(Status::BAD_VALUE);
        }

        if (clear && !args.secure) {
            // MediaCodec sends clear codec configuration through the crypto plugin.
            std::memmove(output, input, total);
            *result = total;
            return ScopedAStatus::ok();
        }

        const auto* keyId = args.keyId.empty() ? emptyKeyOrIv.data() : args.keyId.data();
        const auto* iv = args.iv.empty() ? emptyKeyOrIv.data() : args.iv.data();
        Legacy::Pattern pattern{static_cast<uint32_t>(args.pattern.encryptBlocks),
                                static_cast<uint32_t>(args.pattern.skipBlocks)};
        android::AString detail;
        ssize_t written =
            plugin_->decrypt(args.secure, keyId, iv, mode, pattern, input,
                             samples.data(), samples.size(), output, &detail);
        if (written < 0) return legacyStatus(written);
        if (static_cast<uint64_t>(written) > total) return error(Status::ERROR_DRM_DECRYPT);
        *result = written;
        return ScopedAStatus::ok();
    }

   private:
    const std::unique_ptr<android::CryptoPlugin> plugin_;
    std::mutex mutex_;
    std::map<int32_t, std::unique_ptr<Mapping>> buffers_;
};

}  // namespace

std::shared_ptr<drm::ICryptoPlugin> wrapCrypto(android::CryptoPlugin* plugin) {
    return ndk::SharedRefBase::make<Crypto>(plugin);
}

}  // namespace widevine
