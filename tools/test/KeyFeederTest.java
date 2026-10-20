package com.explodearena.port;

import java.util.*;

public class KeyFeederTest {
    static final List<String> log = new ArrayList<>();
    static int frame = 0;
    static KeyFeeder f = new KeyFeeder(new KeyFeeder.Sink() {
        public void key(int s, boolean d) { log.add(frame + ":" + (d ? "down " : "up ") + s); }
    });
    static void check(boolean ok, String msg) { if (!ok) { System.out.println("FAIL: " + msg + " log=" + log); System.exit(1); } }
    static void frames(int n) { for (int i = 0; i < n; i++) { frame++; f.drain(); } }

    public static void main(String[] a) {
        // 1) tap shorter than a frame: down seen in frame 1, up one frame later
        f.post(0xa7, true); f.post(0xa7, false); frames(3);
        check(log.equals(Arrays.asList("1:down 167", "2:up 167")), "quick tap");
        log.clear();
        // 2) long hold: down at once, up at once when posted
        f.post(0x10, true); frames(3); f.post(0x10, false); frames(1);
        check(log.equals(Arrays.asList("4:down 16", "7:up 16")), "hold");
        log.clear();
        // 3) down-up-down inside one batch ends held, exactly one down, and no premature release
        f.post(0x35, true); f.post(0x35, false); f.post(0x35, true); frames(4);
        check(log.equals(Arrays.asList("8:down 53")), "bounce keeps key held: " + log);
        f.post(0x35, false); frames(1);
        check(log.get(log.size() - 1).equals("12:up 53"), "final release");
        log.clear();
        // 4) two keys at once (BOMB sends joystick-press + key 5)
        f.post(0xa7, true); f.post(0x35, true); frames(2); f.post(0xa7, false); f.post(0x35, false); frames(1);
        check(log.size() == 4 && log.get(0).endsWith("down 167") && log.get(1).endsWith("down 53"), "pair press " + log);
        log.clear();
        // 5) randomized invariants
        Random r = new Random(7);
        boolean[] want = new boolean[256]; boolean[] sinkState = new boolean[256]; int[] lastDownFrame = new int[256];
        final boolean[] bad = new boolean[1];
        KeyFeeder g = new KeyFeeder(new KeyFeeder.Sink() {
            public void key(int s, boolean d) {
                if (d && sinkState[s]) { bad[0] = true; System.out.println("duplicate down " + s); }
                if (!d && !sinkState[s]) { bad[0] = true; System.out.println("up without down " + s); }
                if (!d && lastDownFrame[s] == frame) { bad[0] = true; System.out.println("key " + s + " released in the frame it was pressed"); }
                sinkState[s] = d; if (d) lastDownFrame[s] = frame;
            }
        });
        int[] keys = {0x0e, 0x0f, 0x10, 0x11, 0xa7, 0x35, 0x38};
        for (int it = 0; it < 20000; it++) {
            int ev = r.nextInt(4);
            for (int e = 0; e < ev; e++) { int k = keys[r.nextInt(keys.length)]; boolean d = r.nextBoolean(); g.post(k, d); want[k] = d; }
            frame++; g.drain();
        }
        frame++; g.drain(); frame++; g.drain();      // settle
        for (int k : keys) check(sinkState[k] == want[k], "final state of key " + k);
        check(!bad[0], "random invariants");
        System.out.println("KeyFeeder: all tests passed");
    }
}
