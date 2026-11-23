# SPDX-License-Identifier: Apache-2.0

PRODUCT_PACKAGES += \
    Iwlan \
    PhhIms \
    QualifiedNetworksService

DEVICE_PACKAGE_OVERLAYS += device/samsung/universal7880/ims/overlay

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.ims.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/permissions/android.hardware.telephony.ims.xml \
    device/samsung/universal7880/ims/privapp-permissions-me.phh.ims.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/permissions/privapp-permissions-me.phh.ims.xml
