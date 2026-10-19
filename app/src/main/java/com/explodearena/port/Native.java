package com.explodearena.port;

/** JNI bindings to the native engine host (ea_host.c). */
final class Native {
    static {
        System.loadLibrary("explodearena");
    }

    private Native() {}

    static native int init(byte[] code, byte[] pakHead, int pakTotal);
    static native void setAsset(int index, byte[] data);
    /** Forgets all guest state so the engine can be booted again after a fault. */
    static native void reset();
    static native int boot();
    static native int frame(int ticks64Hz);
    static native int key(int scanCode, boolean down);
    /** Copies the visible 176x208 game screen as ARGB_8888 into dst. */
    static native int copyScreen(int[] dst);
    /** Fills out[0..n) with 16-bit mono PCM; returns the sample rate. */
    static native int audioRead(short[] out, int n);
    static native int audioRate();
    static native boolean quitRequested();
    static native int saveConfig();
    static native boolean configDirty();
    static native byte[] getConfig();
    static native void setConfig(byte[] data);
    static native String lastError();
}
