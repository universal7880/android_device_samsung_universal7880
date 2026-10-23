#!/bin/bash
#
# Copyright (C) 2017-2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

set -e

DEVICE_COMMON=universal7880-common
VENDOR=samsung

# Load extractutils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "$MY_DIR" ]]; then MY_DIR="$PWD"; fi

ANDROID_ROOT="$(cd "${MY_DIR}/../../.." && pwd)"

# Use the build's portable tools for ELF dependency parsing on macOS.
if [[ "$(uname -s)" == "Darwin" ]]; then
    export PATH="${ANDROID_ROOT}/prebuilts/build-tools/path/darwin-x86:${PATH}"
fi

HELPER="${ANDROID_ROOT}/tools/extract-utils/extract_utils.sh"
if [ ! -f "${HELPER}" ]; then
    echo "Unable to find helper script at ${HELPER}"
    exit 1
fi
source "${HELPER}"

# Initialize the helper
setup_vendor "${DEVICE_COMMON}" "${VENDOR}" "${ANDROID_ROOT}" true

# Warning headers and guards
write_headers "universal7880"

write_makefiles "${MY_DIR}/proprietary-files.txt" true

###################################################################################################
# CUSTOM PART START                                                                               #
###################################################################################################

OUTDIR=vendor/$VENDOR/$DEVICE_COMMON

(cat << EOF) >> $ANDROID_ROOT/$OUTDIR/Android.mk
include \$(CLEAR_VARS)

EGL_LIBS := libGLES_mali.so libOpenCL.so libOpenCL.so.1 libOpenCL.so.1.1 vulkan.exynos5.so

EGL_32_SYMLINKS := \$(addprefix \$(TARGET_OUT_VENDOR)/lib/,\$(EGL_LIBS))
\$(EGL_32_SYMLINKS): \$(LOCAL_INSTALLED_MODULE)
	@echo "Symlink: EGL 32-bit lib: \$@"
	@mkdir -p \$(dir \$@)
	@rm -rf \$@
	\$(hide) ln -sf /vendor/lib/egl/libGLES_mali.so \$@

EGL_64_SYMLINKS := \$(addprefix \$(TARGET_OUT_VENDOR)/lib64/,\$(EGL_LIBS))
\$(EGL_64_SYMLINKS): \$(LOCAL_INSTALLED_MODULE)
	@echo "Symlink: EGL 64-bit lib : \$@"
	@mkdir -p \$(dir \$@)
	@rm -rf \$@
	\$(hide) ln -sf /vendor/lib64/egl/libGLES_mali.so \$@

ALL_DEFAULT_INSTALLED_MODULES += \$(EGL_32_SYMLINKS) \$(EGL_64_SYMLINKS)

EOF

###################################################################################################
# CUSTOM PART END                                                                                 #
###################################################################################################

# Done
write_footers

# The vendor directory keeps its existing name; the generator lives in the unified tree.
for generated in "$ANDROIDMK" "$BOARDMK" "$PRODUCTMK" "$ANDROIDBP"; do
    sed "s@device/$VENDOR/$DEVICE_COMMON/setup-makefiles.sh@device/$VENDOR/universal7880/setup-makefiles.sh@" \
        "$generated" > "$generated.tmp"
    mv "$generated.tmp" "$generated"
done
"$ANDROID_ROOT/prebuilts/build-tools/$HOST-x86/bin/bpfmt" -w "$ANDROIDBP"
