package com.explodearena.port;

/** Symbian standard scan codes understood by the original game's key handler. */
final class Keys {
    static final int LEFT = 0x0e;
    static final int RIGHT = 0x0f;
    static final int UP = 0x10;
    static final int DOWN = 0x11;
    static final int FIRE = 0xa7;   // joystick press / select
    static final int LSK = 0xa4;    // left soft key
    static final int RSK = 0xa5;    // right soft key
    static final int CLEAR = 0x01;  // "C" key
    static final int DIGIT_0 = 0x30;

    private Keys() {}
}
