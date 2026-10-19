package com.explodearena.port;

import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;

/** Streams the PCM produced by the original mixer (16 kHz mono) to the device. */
final class AudioOut implements Runnable {
    private final Engine engine;
    private volatile boolean running = true;
    private Thread thread;

    AudioOut(Engine engine) {
        this.engine = engine;
    }

    void start() {
        thread = new Thread(this, "audio");
        thread.setDaemon(true);
        thread.start();
    }

    void stop() {
        running = false;
        if (thread != null) {
            thread.interrupt();
        }
    }

    @Override
    public void run() {
        // wait until the engine finished booting
        while (running && !engine.isReady()) {
            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                return;
            }
        }
        if (!running) {
            return;
        }
        int rate = Native.audioRate();
        int min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
        int size = Math.max(min * 2, 4096);
        AudioTrack track = null;
        try {
            track = new AudioTrack(AudioManager.STREAM_MUSIC, rate, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, size, AudioTrack.MODE_STREAM);
            track.play();
            short[] buf = new short[512];
            boolean playing = true;
            while (running) {
                if (engine.isPaused()) {
                    if (playing) {
                        track.pause();
                        track.flush();
                        playing = false;
                    }
                    Thread.sleep(40);
                    continue;
                }
                if (!playing) {
                    track.play();
                    playing = true;
                }
                Native.audioRead(buf, buf.length);
                if (track.write(buf, 0, buf.length) < 0) {   // blocks, which paces this loop
                    Thread.sleep(50);                        // track in a bad state: do not spin
                }
            }
        } catch (InterruptedException e) {
            // stopping
        } catch (RuntimeException e) {
            // audio is optional: keep the game running without it
        } finally {
            if (track != null) {
                try {
                    track.stop();
                } catch (RuntimeException ignored) {
                    // not playing
                }
                track.release();
            }
        }
    }
}
