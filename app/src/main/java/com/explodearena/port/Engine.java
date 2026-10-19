package com.explodearena.port;

import android.content.Context;
import android.content.res.AssetManager;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Game thread: boots the original engine inside the native host, then runs it at the original
 * 32 frames per second, feeding it key events and presenting every frame.
 */
final class Engine implements Runnable {
    interface Host {
        void showStatus(String text);
        void showFrame(int[] argb);
        void onQuit();
    }

    private static final int ASSET_COUNT = 98;
    private static final int PAK_TOTAL = 412088;
    private static final long FRAME_NANOS = 31250000L;   // 32 fps (2 ticks of the 64 Hz Symbian tick counter)
    private static final String CONFIG_FILE = "game.cfg";

    private final Context context;
    private final Host host;
    private final Object keyLock = new Object();
    private final int[] keyQueue = new int[512];
    private int keyCount = 0;
    private final int[] pixels = new int[176 * 208];

    private volatile boolean running = true;
    private volatile boolean paused = false;
    private volatile boolean ready = false;
    private volatile boolean saveRequested = false;
    private Thread thread;

    Engine(Context context, Host host) {
        this.context = context.getApplicationContext();
        this.host = host;
    }

    void start() {
        thread = new Thread(this, "engine");
        thread.start();
    }

    void stop() {
        running = false;
        if (thread != null) {
            thread.interrupt();
            try {
                thread.join(3000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
    }

    boolean isReady() {
        return ready;
    }

    boolean isPaused() {
        return paused;
    }

    void setPaused(boolean p) {
        if (p && ready) {
            saveRequested = true;   // persist settings whenever the app leaves the foreground
        }
        paused = p;
    }

    /** Thread-safe: may be called from the UI thread. */
    void postKey(int scanCode, boolean down) {
        synchronized (keyLock) {
            if (keyCount < keyQueue.length) {
                keyQueue[keyCount++] = (scanCode << 1) | (down ? 1 : 0);
            }
        }
    }

    private void drainKeys() {
        int n;
        int[] local;
        synchronized (keyLock) {
            n = keyCount;
            if (n == 0) {
                return;
            }
            local = new int[n];
            System.arraycopy(keyQueue, 0, local, 0, n);
            keyCount = 0;
        }
        for (int i = 0; i < n; i++) {
            Native.key(local[i] >> 1, (local[i] & 1) != 0);
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(64 * 1024);
        byte[] buf = new byte[16 * 1024];
        int r;
        while ((r = in.read(buf)) > 0) {
            out.write(buf, 0, r);
        }
        in.close();
        return out.toByteArray();
    }

    private void loadAssets() throws IOException {
        AssetManager am = context.getAssets();
        byte[] code = readAll(am.open("ea/code.bin"));
        byte[] head = readAll(am.open("ea/pakhead.bin"));
        if (Native.init(code, head, PAK_TOTAL) != 0) {
            throw new IOException("native init failed");
        }
        for (int i = 0; i < ASSET_COUNT; i++) {
            Native.setAsset(i, readAll(am.open(String.format(java.util.Locale.US, "ea/%02d.bin", i))));
        }
    }

    private void loadConfig() {
        try {
            FileInputStream in = context.openFileInput(CONFIG_FILE);
            byte[] cfg = readAll(in);
            if (cfg.length > 0) {
                Native.setConfig(cfg);
            }
        } catch (IOException e) {
            // first run: the game starts with its default settings
        }
    }

    private void persistConfig() {
        try {
            byte[] cfg = Native.getConfig();
            if (cfg != null && cfg.length > 0) {
                FileOutputStream out = context.openFileOutput(CONFIG_FILE, Context.MODE_PRIVATE);
                out.write(cfg);
                out.close();
            }
        } catch (IOException e) {
            // settings are non-critical
        }
    }

    private int restarts = 0;
    private long restartWindowStart = 0;

    /** The original code hit something this port does not provide (e.g. Bluetooth): restart it instead of hanging. */
    private boolean recover() {
        long now = System.nanoTime();
        if (now - restartWindowStart > 60000000000L) {
            restartWindowStart = now;
            restarts = 0;
        }
        if (++restarts > 3) {
            return false;
        }
        host.showStatus("Restarting game...");
        Native.reset();
        return Native.boot() == 0;
    }

    @Override
    public void run() {
        try {
            host.showStatus("Loading...");
            loadAssets();
            loadConfig();
            if (Native.boot() != 0) {
                host.showStatus("Engine error: " + Native.lastError());
                return;
            }
            ready = true;
        } catch (IOException e) {
            host.showStatus("Could not load game data: " + e.getMessage());
            return;
        }

        long start = System.nanoTime();
        long next = start;
        while (running) {
            if (paused) {
                try {
                    Thread.sleep(40);
                } catch (InterruptedException e) {
                    break;
                }
                if (saveRequested) {
                    saveRequested = false;
                    Native.saveConfig();
                    persistConfig();
                }
                next = System.nanoTime();
                continue;
            }
            drainKeys();
            int ticks = (int) (((System.nanoTime() - start) * 64L) / 1000000000L);
            if (Native.frame(ticks) < 0) {
                if (!recover()) {
                    host.showStatus("Engine error: " + Native.lastError());
                    return;
                }
                start = System.nanoTime();
                next = start;
                continue;
            }
            if (Native.copyScreen(pixels) == 0) {
                host.showFrame(pixels);
            }
            if (Native.configDirty()) {
                persistConfig();
            }
            if (Native.quitRequested()) {
                Native.saveConfig();
                persistConfig();
                host.onQuit();
                return;
            }
            next += FRAME_NANOS;
            long now = System.nanoTime();
            long wait = next - now;
            if (wait > 0) {
                try {
                    Thread.sleep(wait / 1000000L, (int) (wait % 1000000L));
                } catch (InterruptedException e) {
                    break;
                }
            } else if (wait < -250000000L) {
                next = now;   // fell far behind (e.g. after a pause): do not try to catch up
            }
        }
    }
}
