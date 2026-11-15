/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <jni.h>

#include "FmRadioClient.h"

namespace {
FmRadioClient radio;
jboolean openDev(JNIEnv*, jobject) { return radio.open(); }
jboolean closeDev(JNIEnv*, jobject) { return radio.close(); }
jboolean powerUp(JNIEnv*, jobject, jfloat mhz) { return radio.powerUp(mhz); }
jboolean powerDown(JNIEnv*, jobject, jint) { return radio.powerDown(); }
jboolean tune(JNIEnv*, jobject, jfloat mhz) { return radio.tune(mhz); }
jfloat seek(JNIEnv*, jobject, jfloat mhz, jboolean up) { return radio.seek(mhz, up); }
jshortArray autoScan(JNIEnv* env, jobject) {
    auto channels = radio.scan();
    // The HAL uses 10 kHz units; FMRadio expects 100 kHz units.
    for (auto& channel : channels) channel /= 10;
    auto result = env->NewShortArray(channels.size());
    if (result && !channels.empty())
        env->SetShortArrayRegion(result, 0, channels.size(), channels.data());
    return result;
}
jboolean stopScan(JNIEnv*, jobject) { return radio.stopScan(); }
jint setMute(JNIEnv*, jobject, jboolean enabled) { return radio.mute(enabled); }
jint isRdsSupport(JNIEnv*, jobject) { return 1; }
jint setRds(JNIEnv*, jobject, jboolean enabled) { return radio.setRds(enabled) ? 1 : 0; }
jshort readRds(JNIEnv*, jobject) { return radio.readRds(); }
jbyteArray textBytes(JNIEnv* env, const std::string& text) {
    auto result = env->NewByteArray(text.size());
    if (result && !text.empty())
        env->SetByteArrayRegion(result, 0, text.size(),
                                reinterpret_cast<const jbyte*>(text.data()));
    return result;
}
jbyteArray getPs(JNIEnv* env, jobject) { return textBytes(env, radio.programName()); }
jbyteArray getLrText(JNIEnv* env, jobject) { return textBytes(env, radio.radioText()); }
jshort activeAf(JNIEnv*, jobject) {
    int32_t channel = radio.activeAf();
    return channel > 0 ? channel / 10 : channel;
}
// The wired headset is the only antenna; there is no internal-antenna switch.
jint switchAntenna(JNIEnv*, jobject, jint antenna) { return antenna == 0 ? 0 : -1; }

#define METHOD(name, signature) \
    { const_cast<char*>(#name), const_cast<char*>(signature), reinterpret_cast<void*>(name) }
const JNINativeMethod methods[] = {
    METHOD(openDev, "()Z"),        METHOD(closeDev, "()Z"),   METHOD(powerUp, "(F)Z"),
    METHOD(powerDown, "(I)Z"),     METHOD(tune, "(F)Z"),      METHOD(seek, "(FZ)F"),
    METHOD(autoScan, "()[S"),      METHOD(stopScan, "()Z"),   METHOD(setMute, "(Z)I"),
    METHOD(isRdsSupport, "()I"),   METHOD(setRds, "(Z)I"),    METHOD(readRds, "()S"),
    METHOD(getPs, "()[B"),         METHOD(getLrText, "()[B"), METHOD(activeAf, "()S"),
    METHOD(switchAntenna, "(I)I"),
};
}  // namespace

#ifdef FM_RADIO_VENDOR_AUDIO
extern "C" JNIEXPORT jint JNICALL
Java_com_android_fmradio_FmAudio_openAudio(JNIEnv*, jclass) {
    return radio.openAudio();
}
#endif

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    auto clazz = env->FindClass("com/android/fmradio/FmNative");
    if (!clazz) return JNI_ERR;
    int result = env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0]));
    env->DeleteLocalRef(clazz);
    return result == 0 ? JNI_VERSION_1_6 : JNI_ERR;
}
