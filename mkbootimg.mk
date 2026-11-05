# Copyright (C) 2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0

include hardware/samsung/mkbootimg.mk

# Keep recovery small enough for the bootloader while boot uses gzip.
$(recovery_ramdisk): COMPRESSION_COMMAND := $(XZ) -f -c --check=crc32 --lzma2=dict=32MiB
$(recovery_ramdisk): $(XZ)
