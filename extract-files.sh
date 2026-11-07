#!/bin/bash
#
# Copyright (C) 2018-2020 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

set -e

DEVICE=universal7880
VENDOR=samsung

# Load extractutils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "$MY_DIR" ]]; then MY_DIR="$PWD"; fi

ANDROID_ROOT="${MY_DIR}/../../.."

HELPER="${ANDROID_ROOT}/tools/extract-utils/extract_utils.sh"
if [ ! -f "${HELPER}" ]; then
    echo "Unable to find helper script at ${HELPER}"
    exit 1
fi
source "${HELPER}"

# Default to sanitizing the vendor folder before extraction
CLEAN_VENDOR=true

KANG=
SECTION=

while [ "${#}" -gt 0 ]; do
    case "${1}" in
        -n | --no-cleanup )
                CLEAN_VENDOR=false
                ;;
        -k | --kang )
                KANG="--kang"
                ;;
        -s | --section )
                SECTION="${2}"; shift
                CLEAN_VENDOR=false
                ;;
        * )
                SRC="${1}"
                ;;
    esac
    shift
done

if [ -z "${SRC}" ]; then
    SRC="adb"
fi

# Initialize the helper
setup_vendor "${DEVICE}" "${VENDOR}" "${ANDROID_ROOT}" false "${CLEAN_VENDOR}"

extract "${MY_DIR}/proprietary-files.txt" "${SRC}" "${KANG}" --section "${SECTION}"

# Fix proprietary blobs
BLOB_ROOT="$ANDROID_ROOT"/vendor/"$VENDOR"/"$DEVICE"/proprietary

# Replace protobuf with vndk29 compat libs for specified libs
"${PATCHELF}" --replace-needed libprotobuf-cpp-lite.so libprotobuf-cpp-lite-v29.so $BLOB_ROOT/vendor/lib/mediadrm/libwvdrmengine.so

# Replace libvndsecril-client with libsecril-client
patchelf --replace-needed libvndsecril-client.so libsecril-client.so $BLOB_ROOT/vendor/lib/libwrappergps.so
patchelf --replace-needed libvndsecril-client.so libsecril-client.so $BLOB_ROOT/vendor/lib64/libwrappergps.so

# Link each legacy blob to its compatibility shim without linker injection.
for LIBDIR in lib lib64; do
    CAMERA_LIBDIR="$BLOB_ROOT/vendor/$LIBDIR"

    for CAMERA_BLOB in hw/camera.vendor.exynos5.so libexynoscamera.so libexynoscamera3.so; do
        "${PATCHELF}" --replace-needed libcamera_client.so libcamera_compat.so \
            "$CAMERA_LIBDIR/$CAMERA_BLOB"
    done

    "${PATCHELF}" --add-needed libcamera_client_shim.so "$CAMERA_LIBDIR/libexynoscamera.so"
    "${PATCHELF}" --add-needed libexynoscamera_shim.so "$CAMERA_LIBDIR/libexynoscamera.so"
    "${PATCHELF}" --add-needed libbauthtzcommon_shim.so "$BLOB_ROOT/vendor/$LIBDIR/libbauthserver.so"
done

"${MY_DIR}/setup-makefiles.sh"
