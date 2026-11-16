package com.hcode91.gp2xdroid;

/** JNI entry points into the C++ emulator core. */
public final class Native {
    static { System.loadLibrary("gp2xdroid"); }

    public static final int W = 320, H = 240;

    // Models (must match jni_bridge.cpp)
    public static final int F100 = 0, F200 = 1, WIZ = 2, CAANOO = 3;

    // Button bits (GP2X layout, must match core/src/gp2x.h)
    public static final int UP = 1, UPLEFT = 1 << 1, LEFT = 1 << 2, DOWNLEFT = 1 << 3, DOWN = 1 << 4,
            DOWNRIGHT = 1 << 5, RIGHT = 1 << 6, UPRIGHT = 1 << 7, START = 1 << 8, SELECT = 1 << 9,
            L = 1 << 10, R = 1 << 11, A = 1 << 12, B = 1 << 13, X = 1 << 14, Y = 1 << 15,
            VOLUP = 1 << 16, VOLDOWN = 1 << 17, PUSH = 1 << 18, HOME = 1 << 19;

    /** Returns an empty string on success, otherwise an error message. */
    public static native String init(int model, String root, String sd, String game, int mhz);
    /** Runs one 1/60 s frame. Returns false once the game has exited. */
    public static native boolean runFrame(int[] pixels);
    public static native void setInput(int buttons, boolean touch, int x, int y);
    public static native int readAudio(short[] buf);
    public static native String exitReason();
    public static native void shutdown();
    public static native String console();
}
