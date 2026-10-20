package com.explodearena.port;

import java.util.Arrays;

/**
 * Collects key events from the UI thread and hands them to the engine once per frame.
 *
 * The original game polls its key state once per frame. A touch that goes down and up between two
 * frames would therefore be invisible, so such a release is held back until the next frame: every
 * press stays visible to the engine for at least one full frame.
 */
final class KeyFeeder {
    interface Sink {
        void key(int scanCode, boolean down);
    }

    private final Sink sink;
    private final Object lock = new Object();
    private final int[] queue = new int[512];
    private int count = 0;

    // engine-thread state
    private final boolean[] held = new boolean[256];      // what the engine currently believes
    private final boolean[] deferUp = new boolean[256];   // release postponed to the next frame
    private final boolean[] downNow = new boolean[256];   // pressed during the current drain

    KeyFeeder(Sink sink) {
        this.sink = sink;
    }

    /** Any thread. */
    void post(int scanCode, boolean down) {
        synchronized (lock) {
            if (count < queue.length) {
                queue[count++] = (scanCode << 1) | (down ? 1 : 0);
            }
        }
    }

    /** Engine thread: call once before every frame. */
    void drain() {
        int n;
        int[] local = null;
        synchronized (lock) {
            n = count;
            if (n > 0) {
                local = new int[n];
                System.arraycopy(queue, 0, local, 0, n);
                count = 0;
            }
        }
        for (int s = 0; s < 256; s++) {            // releases held back from the previous frame
            if (deferUp[s]) {
                deferUp[s] = false;
                if (held[s]) {
                    send(s, false);
                }
            }
        }
        Arrays.fill(downNow, false);
        for (int i = 0; i < n; i++) {
            int scan = local[i] >> 1;
            boolean isDown = (local[i] & 1) != 0;
            if (scan < 0 || scan > 255) {
                continue;
            }
            if (isDown) {
                deferUp[scan] = false;
                if (!held[scan]) {
                    send(scan, true);
                }
                downNow[scan] = true;
            } else if (downNow[scan]) {
                deferUp[scan] = true;              // pressed and released within one frame
            } else if (held[scan]) {
                send(scan, false);
            }
        }
    }

    /** Forget everything (used when the engine is reset). */
    void clear() {
        Arrays.fill(held, false);
        Arrays.fill(deferUp, false);
        synchronized (lock) {
            count = 0;
        }
    }

    private void send(int scan, boolean down) {
        sink.key(scan, down);
        held[scan] = down;
    }
}
