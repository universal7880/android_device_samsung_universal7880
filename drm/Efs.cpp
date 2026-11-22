// SPDX-License-Identifier: Apache-2.0

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/stat.h>

namespace {

const char* vendorStatePath(const char* path, std::string& redirected) {
    // OEMCrypto can append file names to its legacy state directory.
    constexpr char legacyDir[] = "/data/mediadrm";
    constexpr size_t legacyDirLength = sizeof(legacyDir) - 1;
    if (path && strncmp(path, legacyDir, legacyDirLength) == 0 &&
        (path[legacyDirLength] == '/' || path[legacyDirLength] == '\0')) {
        redirected = "/data/vendor/mediadrm";
        redirected += path + legacyDirLength;
        return redirected.c_str();
    }
    return path;
}

}  // namespace

// Samsung OEMCrypto opens the device keybox with fopen(path, "rb").
extern "C" FILE* fopen(const char* path, const char* mode) {
    static const auto libcFopen = reinterpret_cast<decltype(&fopen)>(dlsym(RTLD_NEXT, "fopen"));
    std::string redirected;
    if (!libcFopen) {
        errno = ENOSYS;
        return nullptr;
    }

    if (path && strcmp(path, "/efs/wv.keys") == 0 && strcmp(mode, "rb") == 0) {
        FILE* keybox = libcFopen("/mnt/efs/wv.keys", mode);
        if (!keybox) {
            const int error = errno;
            ALOGW("Could not open the Widevine keybox on EFS: errno %d", error);
            errno = error;
        }
        return keybox;
    }
    return libcFopen(vendorStatePath(path, redirected), mode);
}

extern "C" int widevineOpen(const char* path, int flags, ...) __asm__("open");
int widevineOpen(const char* path, int flags, ...) {
    static const auto libcOpen =
            reinterpret_cast<decltype(&widevineOpen)>(dlsym(RTLD_NEXT, "open"));
    std::string redirected;
    if (!libcOpen) {
        errno = ENOSYS;
        return -1;
    }
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args;
        va_start(args, flags);
        const int mode = va_arg(args, int);
        va_end(args);
        return libcOpen(vendorStatePath(path, redirected), flags, mode);
    }
    return libcOpen(vendorStatePath(path, redirected), flags);
}

extern "C" int widevineStat(const char* path, struct stat* info) __asm__("stat");
int widevineStat(const char* path, struct stat* info) {
    static const auto libcStat =
            reinterpret_cast<decltype(&widevineStat)>(dlsym(RTLD_NEXT, "stat"));
    std::string redirected;
    if (!libcStat) {
        errno = ENOSYS;
        return -1;
    }
    return libcStat(vendorStatePath(path, redirected), info);
}

extern "C" int widevineAccess(const char* path, int mode) __asm__("access");
int widevineAccess(const char* path, int mode) {
    static const auto libcAccess =
            reinterpret_cast<decltype(&widevineAccess)>(dlsym(RTLD_NEXT, "access"));
    std::string redirected;
    if (!libcAccess) {
        errno = ENOSYS;
        return -1;
    }
    return libcAccess(vendorStatePath(path, redirected), mode);
}

extern "C" int widevineOpenChecked(const char* path, int flags) __asm__("__open_2");
int widevineOpenChecked(const char* path, int flags) {
    static const auto libcOpenChecked =
            reinterpret_cast<decltype(&widevineOpenChecked)>(dlsym(RTLD_NEXT, "__open_2"));
    std::string redirected;
    if (!libcOpenChecked) {
        errno = ENOSYS;
        return -1;
    }
    return libcOpenChecked(vendorStatePath(path, redirected), flags);
}

extern "C" DIR* widevineOpenDir(const char* path) __asm__("opendir");
DIR* widevineOpenDir(const char* path) {
    static const auto libcOpenDir =
            reinterpret_cast<decltype(&widevineOpenDir)>(dlsym(RTLD_NEXT, "opendir"));
    std::string redirected;
    if (!libcOpenDir) {
        errno = ENOSYS;
        return nullptr;
    }
    return libcOpenDir(vendorStatePath(path, redirected));
}
