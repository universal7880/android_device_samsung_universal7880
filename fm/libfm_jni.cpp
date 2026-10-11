/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <jni.h>

#include "Si47xx.h"

namespace {
Si47xx radio;
jboolean openDev(JNIEnv*, jobject) { return radio.open(); }
jboolean closeDev(JNIEnv*, jobject) { return radio.close(); }
jboolean powerUp(JNIEnv*, jobject, jfloat mhz) { return radio.powerUp(mhz); }
jboolean powerDown(JNIEnv*, jobject, jint) { return radio.powerDown(); }
jboolean tune(JNIEnv*, jobject, jfloat mhz) { return radio.tune(mhz); }
jfloat seek(JNIEnv*, jobject, jfloat mhz, jboolean up) { return radio.seek(mhz, up); }
jshortArray autoScan(JNIEnv* env, jobject) {
    auto channels = radio.scan();
    auto result = env->NewShortArray(channels.size());
    if (result && !channels.empty())
        env->SetShortArrayRegion(result, 0, channels.size(), channels.data());
    return result;
}
jboolean stopScan(JNIEnv*, jobject) { return radio.stopScan(); }
jint setMute(JNIEnv*, jobject, jboolean enabled) { return radio.mute(enabled); }
// RDS decoding and alternative-frequency switching are not implemented yet.
jint isRdsSupport(JNIEnv*, jobject) { return 0; }
jint setRds(JNIEnv*, jobject, jboolean) { return -1; }
jshort readRds(JNIEnv*, jobject) { return 0; }
jbyteArray getText(JNIEnv*, jobject) { return nullptr; }
jshort activeAf(JNIEnv*, jobject) { return 0; }
// The wired headset is the only antenna; there is no internal-antenna switch.
jint switchAntenna(JNIEnv*, jobject, jint antenna) { return antenna == 0 ? 0 : -1; }

#define METHOD(name, signature) \
    { const_cast<char*>(#name), const_cast<char*>(signature), reinterpret_cast<void*>(name) }
const JNINativeMethod methods[] = {
    METHOD(openDev, "()Z"),
    METHOD(closeDev, "()Z"),
    METHOD(powerUp, "(F)Z"),
    METHOD(powerDown, "(I)Z"),
    METHOD(tune, "(F)Z"),
    METHOD(seek, "(FZ)F"),
    METHOD(autoScan, "()[S"),
    METHOD(stopScan, "()Z"),
    METHOD(setMute, "(Z)I"),
    METHOD(isRdsSupport, "()I"),
    METHOD(setRds, "(Z)I"),
    METHOD(readRds, "()S"),
    {const_cast<char*>("getPs"), const_cast<char*>("()[B"), reinterpret_cast<void*>(getText)},
    {const_cast<char*>("getLrText"), const_cast<char*>("()[B"), reinterpret_cast<void*>(getText)},
    METHOD(activeAf, "()S"),
    METHOD(switchAntenna, "(I)I"),
};
}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    auto clazz = env->FindClass("com/android/fmradio/FmNative");
    if (!clazz) return JNI_ERR;
    int result = env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0]));
    env->DeleteLocalRef(clazz);
    return result == 0 ? JNI_VERSION_1_6 : JNI_ERR;
}
