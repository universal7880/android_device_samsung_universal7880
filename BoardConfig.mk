#
# Copyright (C) 2017 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

DEVICE_PATH := device/samsung/universal7880

BUILD_BROKEN_DUP_RULES := true

# Include path
TARGET_SPECIFIC_HEADER_PATH := $(DEVICE_PATH)/include

# Init

# Firmware
TARGET_NO_BOOTLOADER := true
TARGET_NO_RADIOIMAGE := true

# Platform
TARGET_BOARD_PLATFORM := exynos5
TARGET_SOC := exynos7880
TARGET_BOOTLOADER_BOARD_NAME := universal7880
BOARD_VENDOR := samsung

# CPU
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_ABI2 :=
TARGET_CPU_VARIANT := cortex-a53

TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi
TARGET_2ND_CPU_VARIANT := cortex-a53

include hardware/samsung_slsi-linaro/config/BoardConfig7880.mk

# Extracted with libbootimg
BOARD_CUSTOM_BOOTIMG_MK := $(DEVICE_PATH)/mkbootimg.mk
BOARD_MKBOOTIMG_ARGS := --kernel_offset 0x00008000 --ramdisk_offset 0x02000000 --tags_offset 0x00000100
BOARD_KERNEL_BASE := 0x10000000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_KERNEL_IMAGE_NAME := Image
#BOARD_KERNEL_CMDLINE := The bootloader ignores the cmdline from the boot.img
BOARD_KERNEL_SEPARATED_DT := true
BOARD_USES_FULL_RECOVERY_IMAGE := true
TARGET_KERNEL_CLANG_COMPILE := false
TARGET_KERNEL_LLVM_BINUTILS := false
TARGET_CUSTOM_DTBTOOL := dtbhtoolExynos

# Kernel
TARGET_KERNEL_ARCH := arm64
TARGET_LINUX_KERNEL_VERSION := 3.18

# Kernel config
TARGET_KERNEL_SOURCE := kernel/samsung/universal78X0-3.18
TARGET_KERNEL_ADDITIONAL_FLAGS += PLATFORM_VERSION=$(PLATFORM_VERSION)

# Manifest
DEVICE_MANIFEST_FILE := $(DEVICE_PATH)/manifest.xml
DEVICE_FRAMEWORK_COMPATIBILITY_MATRIX_FILE += \
    vendor/lineage/config/device_framework_matrix.xml \
    $(DEVICE_PATH)/framework_compatibility_matrix.xml \
    $(DEVICE_PATH)/fm/aidl/framework_compatibility_matrix.xml

# Partitions
AB_OTA_UPDATER := false

BOARD_BOOTIMAGE_PARTITION_SIZE     := 33554432
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 39845888
BOARD_USERDATAIMAGE_PARTITION_SIZE := 25656557568
BOARD_CACHEIMAGE_PARTITION_SIZE    := 209715200
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE  := ext4

# blockdev --getbsz /dev/block/mmcblk0p9
BOARD_FLASH_BLOCK_SIZE := 4096

TARGET_USERIMAGES_USE_EXT4 := true

# Root extra folders
BOARD_ROOT_EXTRA_FOLDERS += efs
TARGET_FS_CONFIG_GEN := $(DEVICE_PATH)/config.fs

# Retrofit dynamic partitions use the existing SYSTEM partition.
BOARD_SUPER_PARTITION_BLOCK_DEVICES := SYSTEM
BOARD_SUPER_PARTITION_METADATA_DEVICE := SYSTEM
BOARD_SUPER_PARTITION_SIZE := 4504682496
BOARD_SUPER_PARTITION_SYSTEM_DEVICE_SIZE := $(BOARD_SUPER_PARTITION_SIZE)
BOARD_SUPER_PARTITION_GROUPS := universal7880_dynamic_partitions
# Reserve 4 MiB for liblp metadata and alignment.
BOARD_UNIVERSAL7880_DYNAMIC_PARTITIONS_SIZE := 4500488192
BOARD_UNIVERSAL7880_DYNAMIC_PARTITIONS_PARTITION_LIST := system vendor

BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4
TARGET_COPY_OUT_VENDOR := vendor

# ELF
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

# Bluetooth
BOARD_HAVE_BLUETOOTH := true

# Samsung HALs
TARGET_AUDIOHAL_VARIANT := samsung

# FM radio
PRODUCT_SOONG_NAMESPACES += $(DEVICE_PATH)/fm
$(call soong_config_set_string_list,libfmjni,external_srcs,//$(DEVICE_PATH)/fm:libfmjni-universal7880-srcs)
$(call soong_config_set_string_list,libfmjni,external_shared_libs,libbinder_ndk liblog //$(DEVICE_PATH)/fm:vendor.lineage.hardware.fmradio-V1-ndk)

# Wifi
BOARD_WLAN_DEVICE := qcwcn
BOARD_HOSTAPD_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
WIFI_DRIVER_FW_PATH_AP := "ap"
WIFI_DRIVER_FW_PATH_STA := "sta"
WIFI_DRIVER_FW_PATH_P2P := "p2p"
WPA_SUPPLICANT_VERSION := VER_0_8_X
WIFI_AVOID_IFACE_RESET_MAC_CHANGE := true
WIFI_HIDL_UNIFIED_SUPPLICANT_SERVICE_RC_ENTRY := true

# Build fingerprint
BUILD_FINGERPRINT := "samsung/a7y17lteskt/a7y17lteskt:9/PPR1.180610.011/A720SKSU5CUJ2:user/release-keys"
PRIVATE_BUILD_DESC := "a7y17lteskt-user 9 PPR1.180610.011 A720SKSU5CUJ2 release-keys"

# RIL
BOARD_MODEM_TYPE := ss333
BOARD_PROVIDES_LIBRIL := true
ENABLE_VENDOR_RIL_SERVICE := true
TARGET_USES_VND_SECRIL := true

# Security Patch Level
VENDOR_SECURITY_PATCH := 2021-10-01

# Release tools
TARGET_RELEASETOOLS_EXTENSIONS := $(DEVICE_PATH)

# Recovery
#RECOVERY_VARIANT := twrp
BOARD_HAS_DOWNLOAD_MODE := true
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/ramdisk/fstab.samsungexynos7880

# TWRP
ifeq ($(RECOVERY_VARIANT),twrp)
PRODUCT_COPY_FILES += $(DEVICE_PATH)/twrp/twrp.fstab:recovery/root/etc/twrp.fstab
TW_THEME := portrait_hdpi
TW_BRIGHTNESS_PATH := /sys/class/backlight/panel/brightness
TW_MAX_BRIGHTNESS := 255
TW_DEFAULT_BRIGHTNESS := 162
TW_NO_REBOOT_BOOTLOADER := true
TW_INCLUDE_CRYPTO := true
TW_INCLUDE_NTFS_3G := true
TW_HAS_DOWNLOAD_MODE := true
TW_NO_EXFAT_FUSE := true
TW_EXCLUDE_SUPERSU := true
TARGET_RECOVERY_PIXEL_FORMAT := ABGR_8888
RECOVERY_SDCARD_ON_DATA := true
BOARD_HAS_NO_REAL_SDCARD := true
TW_USE_TOOLBOX=true
TW_EXCLUDE_TWRPAPP := true
endif

# Seccomp filters
BOARD_SECCOMP_POLICY += $(DEVICE_PATH)/seccomp

# Power
$(call soong_config_set,power_libperfmgr,mode_extension_lib,//$(DEVICE_PATH):libperfmgr-ext-universal7880)

# SELinux
BOARD_SEPOLICY_TEE_FLAVOR := mobicore
include device/samsung_slsi/sepolicy/sepolicy.mk
# Use the common MobiCore policy without pre-Treble init device/data access.
BOARD_VENDOR_SEPOLICY_DIRS := $(filter-out \
    device/samsung_slsi/sepolicy/tee/mobicore/legacy, \
    $(BOARD_VENDOR_SEPOLICY_DIRS))
include device/lineage/sepolicy/libperfmgr/sepolicy.mk
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += $(DEVICE_PATH)

# Kernel config
TARGET_KERNEL_CONFIG := lineage-universal7880_defconfig
KERNEL_CONFIG_OVERRIDE += CONFIG_RD_XZ=y

# OTA
TARGET_OTA_ASSERT_DEVICE := universal7880,a5y17lte,a5y17ltexx,a5y17ltecan,a5y17ltektt,a5y17ltelgt,a5y17lteskt,a7y17lte,a7y17ltexx,a7y17lteskt

# Legacy audio Soong configuration
TARGET_SAMSUNG_AUDIO_RIL_CLIENT_LIBRARY := //vendor/samsung/universal7880:libsecril-client
TARGET_SAMSUNG_AUDIO_HEADER_LIBRARY := //$(DEVICE_PATH):libaudio_headers_universal7880
include hardware/samsung/audio/config.mk
