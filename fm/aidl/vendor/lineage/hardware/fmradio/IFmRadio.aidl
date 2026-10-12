/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package vendor.lineage.hardware.fmradio;

/** Device tuner access; frequencies are MHz, scan results and AF are in 10 kHz units. */
@VintfStability
interface IFmRadio {
    boolean open();
    boolean close();
    boolean powerUp(float mhz);
    boolean powerDown();
    boolean tune(float mhz);
    float seek(float mhz, boolean up);
    int[] scan();
    boolean stopScan();
    int mute(boolean enabled);
    boolean setRds(boolean enabled);
    /** Returns the legacy FMRadio RDS event bitmask. */
    int readRds();
    String programName();
    String radioText();
    int activeAf();
}
