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

LOCAL_PATH := device/samsung/universal7880

# APEX
OVERRIDE_PRODUCT_COMPRESSED_APEX := false

DEVICE_PACKAGE_OVERLAYS += $(LOCAL_PATH)/overlay

# Display density
PRODUCT_VENDOR_PROPERTIES += ro.sf.lcd_density=420

# Power profiles
PRODUCT_PACKAGES += \
    PowerProfileA5 \
    PowerProfileA7

# VINTF
PRODUCT_ENFORCE_VINTF_MANIFEST_OVERRIDE := true

PRODUCT_PACKAGES += \
    universal7880_framework_compatibility_matrix.4.xml

# Flat device tree for boot image
PRODUCT_HOST_PACKAGES += \
    dtbhtoolExynos

# Ramdisk
PRODUCT_PACKAGES += \
    fstab.samsungexynos7880 \
    init.baseband.rc \
    init.vendor.rilchip.rc \
    init.vendor.rilcommon.rc \
    init.samsung.rc \
    init.samsungexynos7880.rc \
    init.samsungexynos7880.usb.rc \
    init.recovery.samsungexynos7880.rc \
    init.gps.rc \
    mobicore.rc \
    wifi_sec.rc \
    wifi_qcom.rc \
    ueventd.samsungexynos7880.rc \
    secril_config_svc

# Persistent logs for boot debugging. Keep at most nine 2 MiB files.
ifneq ($(filter userdebug eng,$(TARGET_BUILD_VARIANT)),)
PRODUCT_SYSTEM_PROPERTIES += \
    logd.logpersistd=logcatd \
    logd.logpersistd.buffer=all \
    logd.logpersistd.rotate_kbytes=2048 \
    logd.logpersistd.size=8
endif

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.audio.low_latency.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.audio.low_latency.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml \
    frameworks/native/data/etc/android.hardware.camera.flash-autofocus.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.flash-autofocus.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml \
    frameworks/native/data/etc/android.hardware.ethernet.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.ethernet.xml \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.fingerprint.xml \
    frameworks/native/data/etc/android.hardware.nfc.hce.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.hce.xml \
    frameworks/native/data/etc/android.hardware.nfc.hcef.xml:system/etc/permissions/android.hardware.nfc.hcef.xml \
    frameworks/native/data/etc/android.hardware.nfc.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.xml \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/native/data/etc/android.hardware.opengles.aep.xml:system/etc/permissions/android.hardware.opengles.aep.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level.xml \
    frameworks/native/data/etc/android.hardware.vulkan.version-1_0_3.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version.xml \
    frameworks/native/data/etc/android.software.freeform_window_management.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.freeform_window_management.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.barometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.barometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/native/data/etc/android.hardware.sensor.stepcounter.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.stepcounter.xml \
    frameworks/native/data/etc/android.hardware.sensor.stepdetector.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.stepdetector.xml \
    frameworks/native/data/etc/android.hardware.wifi.direct.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.direct.xml \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.software.sip.voip.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.sip.voip.xml \
    frameworks/native/data/etc/android.software.sip.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.sip.xml \
    frameworks/native/data/etc/com.android.nfc_extras.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.android.nfc_extras.xml \
    frameworks/native/data/etc/com.nxp.mifare.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.nxp.mifare.xml \
    frameworks/native/data/etc/handheld_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/handheld_core_hardware.xml

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    bootable/deprecated-ota \
    hardware/google/interfaces \
    hardware/google/pixel \
    hardware/qcom/wlan/legacy \
    hardware/samsung \
    hardware/lineage/interfaces/power-libperfmgr

# Graphics
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := xxhdpi
# A list of dpis to select prebuilt apk, in precedence order.
PRODUCT_AAPT_PREBUILT_DPI := xxhdpi xhdpi hdpi

PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.composer@2.1-service \
    android.hardware.graphics.mapper@2.0-impl \
    android.hardware.memtrack@1.0-impl \
    android.hardware.renderscript@1.0-impl \
    libhwc2on1adapter

PRODUCT_PACKAGES += \
    libion \
    libfimg

# Configstore
PRODUCT_PACKAGES += \
    disable_configstore

# libminijail
PRODUCT_PACKAGES += \
    libminijail_32

# Health
PRODUCT_PACKAGES += \
    android.hardware.health@2.1-impl \
    android.hardware.health@2.1-service

# Camera
PRODUCT_PACKAGES += \
    libexynoscamera_shim \
    libcamera_client_shim \
    camera.exynos5 \
    android.hardware.camera.provider@2.5-service

# Camera service
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/init/vendor.samsung.hardware.camera.provider@2.4-service.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/vendor.samsung.hardware.camera.provider@2.4-service.rc

# DRM
PRODUCT_PACKAGES += \
    android.hardware.drm@1.0-impl \
    android.hardware.drm@1.0-service \
    android.hardware.drm@1.3 \
    android.hardware.drm-service.clearkey \
    libfwdlockengine

# DRM (Widevine) service
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/init/android.hardware.drm@1.1-service.widevine.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/android.hardware.drm@1.1-service.widevine.rc

# FastCharge
PRODUCT_PACKAGES += \
    vendor.lineage.fastcharge@1.0-service.samsung

# Gatekeeper
PRODUCT_PACKAGES += \
    android.hardware.gatekeeper@1.0-service.software

# Keymaster
PRODUCT_PACKAGES += \
    android.hardware.keymaster@3.0-impl \
    android.hardware.keymaster@3.0-service

# hardware/samsung/AdvancedDisplay (MDNIE)
PRODUCT_PACKAGES += \
    AdvancedDisplay

# Radio
PRODUCT_PACKAGES += \
    android.hardware.radio@1.4 \
    android.hardware.radio.config@1.2 \
    android.hardware.radio.deprecated@1.0 \
    vendor.samsung.hardware.radio@2.0 \
    vendor.samsung.hardware.radio@2.1 \
    libxml2 \
    libprotobuf-cpp-full

# Doze
PRODUCT_PACKAGES += \
    SamsungDoze

# Versioned netutils
PRODUCT_PACKAGES += \
    netutils-wrapper-1.0

# WCNSS
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/wifi/grippower.info:system/etc/firmware/wlan/grippower.info \
    $(LOCAL_PATH)/configs/wifi/qcom_cfg.ini:system/etc/firmware/wlan/qcom_cfg.ini \
    $(LOCAL_PATH)/configs/wifi/WCNSS_cfg.dat:system/etc/firmware/wlan/WCNSS_cfg.dat \
    $(LOCAL_PATH)/configs/wifi/indoorchannel.info:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/indoorchannel.info

# WiFi
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/wifi/p2p_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant_overlay.conf \
    $(LOCAL_PATH)/configs/wifi/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf

PRODUCT_PACKAGES += \
    android.hardware.wifi@1.0 \
    android.hardware.wifi-service \
    hostapd \
    libwpa_client \
    WifiOverlay \
    TetheringConfigOverlay \
    wificond \
    wpa_supplicant \
    wpa_supplicant.conf

# BatteryLifeExtender
PRODUCT_PACKAGES += \
    vendor.lineage.batterylifeextender@1.0-service.samsung

# Bluetooth
PRODUCT_PACKAGES += \
    android.hardware.bluetooth-service.universal7880 \
    AntHalService-Soong \
    libbt-vendor

# NFC
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/nfc/libnfc-sec-vendor.conf:$(TARGET_COPY_OUT_VENDOR)/etc/libnfc-sec-vendor.conf \
    $(LOCAL_PATH)/configs/nfc/libnfc-nci.conf:system/etc/libnfc-nci.conf \
    $(LOCAL_PATH)/configs/nfc/nfcee_access.xml:system/etc/nfcee_access.xml

PRODUCT_PACKAGES += \
    android.hardware.nfc@1.2-service.samsung \
    com.android.nfc_extras \
    libnfc-nci \
    libnfc_nci_jni \
    NfcNci \
    Tag

# Audio
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/audio/mixer_gains.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_gains.xml \
    $(LOCAL_PATH)/configs/audio/mixer_paths.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_paths.xml \
    $(LOCAL_PATH)/configs/audio/mixer_paths_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/mixer_paths_0.xml \
    $(LOCAL_PATH)/configs/audio/audio_effects.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects.xml \
    $(LOCAL_PATH)/configs/audio/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/bluetooth_audio_policy_configuration_7_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth_audio_policy_configuration_7_0.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/usb_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/usb_audio_policy_configuration.xml

PRODUCT_PACKAGES += \
    android.hardware.audio.service.universal7880 \
    android.hardware.audio@7.1-impl:32 \
    android.hardware.audio.effect@7.0-impl:32 \
    android.hardware.bluetooth.audio@2.0-impl:32 \
    audio_amplifier.universal7880_32 \
    audio.primary.universal7880_32 \
    audio.bluetooth.default \
    audio.usb.default \
    audio.r_submix.default \
    libaudiopreprocessing \
    libtinycompress \
    libaudioroute

# FM radio
PRODUCT_PACKAGES += \
    FMRadio \
    vendor.lineage.hardware.fmradio-service.si47xx

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/sysconfig/fmradio-kor.xml:$(TARGET_COPY_OUT_VENDOR)/etc/sysconfig/sku_kor/fmradio.xml

# Media
PRODUCT_COPY_FILES += \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_telephony.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_telephony.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_video.xml  \
    $(LOCAL_PATH)/configs/media/media_codecs.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml \
    $(LOCAL_PATH)/configs/media/media_profiles_V1_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_profiles_V1_0.xml

# GPS
PRODUCT_PACKAGES += \
    android.hardware.gnss@1.0-impl \
    android.hardware.gnss@1.0-service.universal7880

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/gps/gps.cfg:$(TARGET_COPY_OUT_VENDOR)/etc/gnss/gps.cfg \
    $(LOCAL_PATH)/configs/gps/gps_debug.conf:system/etc/gps_debug.conf

# Keys
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/keylayout/gpio_keys.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/gpio_keys.kl \
    $(LOCAL_PATH)/configs/keylayout/sec_touchkey.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/sec_touchkey.kl

# Touchscreen
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/idc/ft5x06_ts.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/ft5x06_ts.idc \
    $(LOCAL_PATH)/configs/idc/Synaptics_HID_TouchPad.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/Synaptics_HID_TouchPad.idc \
    $(LOCAL_PATH)/configs/idc/Synaptics_RMI4_TouchPad_Sensor.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/Synaptics_RMI4_TouchPad_Sensor.idc \
    $(LOCAL_PATH)/configs/idc/sec_touchscreen.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/sec_touchscreen.idc

# Power
PRODUCT_PACKAGES += \
    android.hardware.power-service.lineage-libperfmgr

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/power/powerhint.json:$(TARGET_COPY_OUT_VENDOR)/etc/powerhint.json

PRODUCT_VENDOR_PROPERTIES += \
    vendor.powerhal.disp.idle_support=false \
    vendor.powerhal.interaction.min=80 \
    vendor.powerhal.interaction.max=5650 \
    vendor.powerhal.interaction.offset=0

# Lights
PRODUCT_PACKAGES += \
    android.hardware.light-service.samsung

# Fingerprint
PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint-service.samsung \
    libbauthtzcommon_shim

PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.fingerprint.type=home \
    ro.vendor.fingerprint.max_enrollments=5

# FlipFlap
PRODUCT_PACKAGES += \
    FlipFlap

# Touch features
PRODUCT_PACKAGES += \
    vendor.lineage.touch@1.0-service.samsung

# Livedisplay
PRODUCT_PACKAGES += \
    vendor.lineage.livedisplay@2.1-service.universal7880

# Sensors
PRODUCT_PACKAGES += \
    android.hardware.sensors@1.0-impl.samsung \
    android.hardware.sensors@1.0-service \
    android.frameworks.sensorservice@1.0 \
    libsensorndkbridge \
    sensors.universal7880

# Vibrator
PRODUCT_PACKAGES += \
    android.hardware.vibrator@1.0-impl \
    android.hardware.vibrator@1.0-service

# Legacy vendor dependencies
PRODUCT_PACKAGES += \
    libprotobuf-cpp-lite-v29 \
    libprotobuf-cpp-full-v29 \
    libhidlbase-v32 \
    libhidlbase-v32.vendor

# Offmode charger
PRODUCT_PACKAGES += \
    charger_res_images

# USB
PRODUCT_PACKAGES += \
    android.hardware.usb@1.1-service.typec

PRODUCT_PACKAGES += \
    android.hidl.base@1.0 \
    android.hidl.manager@1.0 \
    libhidltransport \
    libhwbinder \
    vndservicemanager

# System.prop
TARGET_SYSTEM_PROP += device/samsung/universal7880/system.prop

$(call inherit-product, hardware/samsung_slsi-linaro/config/config.mk)

# Graphics support libraries
PRODUCT_PACKAGES += \
    gralloc.exynos5 \
    hwcomposer.universal7880 \
    memtrack.universal7880 \
    libion_exynos \
    libexynosscaler \
    libexynosgscaler \
    libcsc

# OMX
PRODUCT_PACKAGES += \
    android.hardware.media.omx@1.0-service \
    libstagefrighthw \
    libExynosOMX_Core \
    libExynosOMX_Resourcemanager \
    libOMX.Exynos.AVC.Decoder \
    libOMX.Exynos.AVC.Encoder \
    libOMX.Exynos.HEVC.Decoder \
    libOMX.Exynos.HEVC.Encoder \
    libOMX.Exynos.MPEG4.Decoder \
    libOMX.Exynos.MPEG4.Encoder \
    libOMX.Exynos.VP8.Decoder \
    libOMX.Exynos.VP8.Encoder \
    libOMX.Exynos.VP9.Decoder \
    libOMX.Exynos.WMV.Decoder

# Shipping API level
$(call inherit-product, $(SRC_TARGET_DIR)/product/product_launched_with_m.mk)

# call the proprietary setup
$(call inherit-product, vendor/samsung/universal7880-common/universal7880-common-vendor.mk)
