// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

// Samsung's OEMCrypto opens the device keybox with fopen(path, "rb").
extern "C" FILE* fopen(const char* path, const char* mode) {
    static const auto libcFopen = reinterpret_cast<decltype(&fopen)>(dlsym(RTLD_NEXT, "fopen"));
    if (!libcFopen) {
        errno = ENOSYS;
        return nullptr;
    }
    if (strcmp(path, "/efs/wv.keys") == 0 && strcmp(mode, "rb") == 0) {
        path = "/mnt/vendor/efs/wv.keys";
    }
    return libcFopen(path, mode);
}
