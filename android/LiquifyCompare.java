package com.winlator.sai;

/**
 * Cross-implementation reference check: runs LiquifyWarp with the exact same
 * input image and stroke specs as liquify/engine/lq_dump.c and writes raw BGRA
 * dumps. A Python script then compares both sides pixel by pixel.
 *
 * This is a developer tool; it is not compiled into the APK (build-apk.sh only
 * compiles LiquifyWarp.java and LiquifyServer.java).
 *
 *   javac -d /tmp/ltest android/LiquifyWarp.java android/LiquifyCompare.java
 *   java -cp /tmp/ltest com.winlator.sai.LiquifyCompare /private/tmp/lqjava
 */
public final class LiquifyCompare {
    static final int W = 256, H = 256;

    static final class Spec {
        final String name; final int mode;
        final float x0, y0, x1, y1, r, s; final int steps;
        Spec(String n, int m, float x0, float y0, float x1, float y1,
             float r, float s, int steps) {
            this.name = n; this.mode = m; this.x0 = x0; this.y0 = y0;
            this.x1 = x1; this.y1 = y1; this.r = r; this.s = s; this.steps = steps;
        }
    }

    static final Spec[] SPECS = {
        new Spec("forward",   LiquifyWarp.FORWARD,   60, 60, 180, 140, 40, 0.8f, 20),
        new Spec("bloat",     LiquifyWarp.BLOAT,    128, 128, 128, 128, 50, 0.8f, 10),
        new Spec("pinch",     LiquifyWarp.PINCH,    128, 128, 128, 128, 50, 0.8f, 10),
        new Spec("twirl_cw",  LiquifyWarp.TWIRL_CW, 128, 128, 128, 128, 60, 0.8f, 10),
        new Spec("twirl_ccw", LiquifyWarp.TWIRL_CCW,128, 128, 128, 128, 60, 0.8f, 10),
        new Spec("smooth",    LiquifyWarp.SMOOTH,   128, 128, 128, 128, 30, 0.8f,  4),
    };

    static byte[] makeInput() {
        byte[] p = new byte[W * H * 4];
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int o = (y * W + x) * 4;
                p[o] = (byte) ((x * 7 + y * 13) & 0xFF);
                p[o + 1] = (byte) ((x * 11 + y * 5) & 0xFF);
                p[o + 2] = (byte) ((x * 3 + y * 17) & 0xFF);
                p[o + 3] = (byte) 0xFF;
            }
        for (int y = 100; y < 130; ++y)
            for (int x = 100; x < 130; ++x) {
                int o = (y * W + x) * 4;
                p[o] = 0; p[o + 1] = 0; p[o + 2] = (byte) 0xFF; p[o + 3] = (byte) 0xFF;
            }
        return p;
    }

    public static void main(String[] args) throws Exception {
        String dir = args.length > 0 ? args[0] : ".";
        for (Spec sp : SPECS) {
            byte[] img = makeInput();
            long t0 = System.currentTimeMillis();
            LiquifyWarp.warp(img, W, H, sp.mode, sp.x0, sp.y0, sp.x1, sp.y1,
                             sp.r, sp.s, sp.steps);
            long t1 = System.currentTimeMillis();
            java.io.FileOutputStream f = new java.io.FileOutputStream(dir + "/" + sp.name + ".bin");
            try { f.write(img); } finally { f.close(); }
            System.out.printf("%-10s %d ms -> %s/%s.bin%n", sp.name, (t1 - t0), dir, sp.name);
        }
    }
}
