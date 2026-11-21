package com.winlator.sai;

/**
 * 液化引擎（Android 端，纯 Java）。
 *
 * 与 liquify/engine/lq.c 逐字节等价（见 liquify/engine/lq_dump.c +
 * android/LiquifyCompare.java 的对比测试）。模型：
 *
 *   - 一笔开始复制一份快照（既是 undo 源，也是重采样源）
 *   - 每个笔刷步只往位移场里累加偏移（1/16 px，double 累加后就近取偶，
 *     和 C 的 int16 + lrint 完全一致）
 *   - 最后一次性按位移场从快照重采样
 *
 * 性能注意：ART 不会 JIT 编译过大的方法。最早把所有模式写在一个
 * warp() 里，结果整段解释执行（1600x1600 一笔 800ms～3s）。现在每个模式
 * 拆成独立的小方法，JIT 能编译，热后一笔 1600x1600 约 35–45ms。
 */
public final class LiquifyWarp {

    public static final int FORWARD = 0;
    public static final int BLOAT = 1;
    public static final int PINCH = 2;
    public static final int TWIRL_CW = 3;
    public static final int TWIRL_CCW = 4;
    public static final int SMOOTH = 5;

    private static final int B = 1024;            /* twirl table resolution */
    private static final float[] TAB_COS = new float[B + 1];
    private static final float[] TAB_SIN = new float[B + 1];

    /** 原生实现（libliquify.so，就是 lq.c）。加载失败时退回纯 Java 实现。 */
    private static final boolean NATIVE_OK;
    private static final String NATIVE_ERR;
    static {
        boolean ok;
        String err = "";
        try {
            System.loadLibrary("liquify");
            ok = true;
        } catch (Throwable t) {
            ok = false;
            err = t.toString();
        }
        NATIVE_OK = ok;
        NATIVE_ERR = err;
    }
    private static native void warpNative(byte[] img, int w, int h, int mode,
                                          float x0, float y0, float x1, float y1,
                                          float radius, float strength, int steps);

    public static boolean nativeAvailable() { return NATIVE_OK; }
    public static String nativeError() { return NATIVE_ERR; }

    private LiquifyWarp() { }

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    private static float falloffD(float d) {
        if (d >= 1.0f) return 0.0f;
        float t = 1.0f - d * d;
        return t * t;
    }

    /** 位移场量化：lq.c 里是 int16 的 1/16 px，用 double 累加 + 就近取偶。 */
    private static float quant(double v) {
        return (float) (Math.rint(v * 16.0) / 16.0);
    }

    public static byte[] warp(byte[] img, int w, int h, int mode,
                              float x0, float y0, float x1, float y1,
                              float radius, float strength, int steps) {
        if (img == null || w <= 0 || h <= 0) return img;
        if (img.length < w * h * 4) return img;
        if (radius < 0.5f) radius = 0.5f;
        if (strength <= 0.0f) return img;
        if (strength > 1.0f) strength = 1.0f;
        if (steps < 1) steps = 1;
        if (steps > 2000) steps = 2000;

        int bx0 = clamp((int) Math.floor(Math.min(x0, x1) - radius - 2), 0, w - 1);
        int bx1 = clamp((int) Math.ceil(Math.max(x0, x1) + radius + 2), 0, w - 1);
        int by0 = clamp((int) Math.floor(Math.min(y0, y1) - radius - 2), 0, h - 1);
        int by1 = clamp((int) Math.ceil(Math.max(y0, y1) + radius + 2), 0, h - 1);
        int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
        if (bw <= 0 || bh <= 0) return img;

        if (NATIVE_OK) {
            /* same lq.c, compiled for arm64: no ART, no interpretation */
            warpNative(img, w, h, mode, x0, y0, x1, y1, radius, strength, steps);
            return img;
        }

        if (mode == SMOOTH)
            return warpSmooth(img, w, h, bx0, bx1, by0, by1,
                    x0, y0, x1, y1, radius, strength, steps);

        byte[] snap = img.clone();
        float[] dx = new float[bw * bh];
        float[] dy = new float[bw * bh];
        float invR = 1.0f / radius;
        float invR2 = invR * invR;

        for (int s = 1; s <= steps; ++s) {
            float t = (float) s / steps;
            float px = x0 + (x1 - x0) * t;
            float py = y0 + (y1 - y0) * t;
            /* 和 C 一样用两个位置相减求步进量（1/16 px 量化对舍入敏感） */
            float mdx = px - (px - (x1 - x0) / steps);
            float mdy = py - (py - (y1 - y0) / steps);
            int ix0 = clamp((int) Math.floor(px - radius - 1), bx0, bx1);
            int ix1 = clamp((int) Math.ceil(px + radius + 1), bx0, bx1);
            int iy0 = clamp((int) Math.floor(py - radius - 1), by0, by1);
            int iy1 = clamp((int) Math.ceil(py + radius + 1), by0, by1);

            if (mode == FORWARD)
                stepForward(dx, dy, bw, bx0, by0, ix0, ix1, iy0, iy1,
                        px, py, mdx, mdy, invR, strength);
            else if (mode == BLOAT || mode == PINCH)
                stepRadial(dx, dy, bw, bx0, by0, ix0, ix1, iy0, iy1,
                        px, py, invR, radius, strength, mode == BLOAT);
            else
                stepTwirl(dx, dy, bw, bx0, by0, ix0, ix1, iy0, iy1,
                        px, py, invR, strength, mode == TWIRL_CW);
        }
        resample(snap, img, dx, dy, w, h, bx0, by0, bw, bh);
        return img;
    }

    private static void stepForward(float[] dx, float[] dy, int bw,
                                    int bx0, int by0, int ix0, int ix1, int iy0, int iy1,
                                    float px, float py, float mdx, float mdy,
                                    float invR, float strength) {
        for (int y = iy0; y <= iy1; ++y) {
            int row = (y - by0) * bw;
            for (int x = ix0; x <= ix1; ++x) {
                float ddx = x - px, ddy = y - py;
                float d = (float) Math.sqrt(ddx * ddx + ddy * ddy) * invR;
                float wgt = falloffD(d) * strength;
                if (wgt <= 0.0001f) continue;
                int o = row + (x - bx0);
                dx[o] = quant((double) dx[o] - mdx * wgt);
                dy[o] = quant((double) dy[o] - mdy * wgt);
            }
        }
    }

    private static void stepRadial(float[] dx, float[] dy, int bw,
                                   int bx0, int by0, int ix0, int ix1, int iy0, int iy1,
                                   float px, float py, float invR, float radius,
                                   float strength, boolean bloat) {
        float sign = bloat ? -1.0f : 1.0f;
        for (int y = iy0; y <= iy1; ++y) {
            int row = (y - by0) * bw;
            for (int x = ix0; x <= ix1; ++x) {
                float ddx = x - px, ddy = y - py;
                float len = (float) Math.sqrt(ddx * ddx + ddy * ddy);
                float d = len * invR;
                float wgt = falloffD(d) * strength;
                if (wgt <= 0.0001f || len < 0.001f) continue;
                float k = radius * 0.06f * wgt * sign;
                int o = row + (x - bx0);
                dx[o] = quant((double) dx[o] + ddx / len * k);
                dy[o] = quant((double) dy[o] + ddy / len * k);
            }
        }
    }

    private static void stepTwirl(float[] dx, float[] dy, int bw,
                                  int bx0, int by0, int ix0, int ix1, int iy0, int iy1,
                                  float px, float py, float invR, float strength, boolean cw) {
        float dir = cw ? -1.0f : 1.0f;
        for (int i = 0; i <= B; ++i) {
            float d = (float) Math.sqrt((float) i / B);
            float wgt = falloffD(d) * strength;
            float ang = 0.35f * wgt * dir;
            TAB_COS[i] = (float) Math.cos(ang);
            TAB_SIN[i] = (float) Math.sin(ang);
        }
        for (int y = iy0; y <= iy1; ++y) {
            int row = (y - by0) * bw;
            for (int x = ix0; x <= ix1; ++x) {
                float ddx = x - px, ddy = y - py;
                float len2 = ddx * ddx + ddy * ddy;
                float d = (float) Math.sqrt(len2) * invR;
                if (d >= 1.0f) continue;
                float fidx = d * d * B;
                int i0 = (int) fidx;
                if (i0 >= B) i0 = B - 1;
                float fr = fidx - i0;
                float cs = TAB_COS[i0] + (TAB_COS[i0 + 1] - TAB_COS[i0]) * fr;
                float sn = TAB_SIN[i0] + (TAB_SIN[i0 + 1] - TAB_SIN[i0]) * fr;
                if (len2 < 0.25f) continue;
                int o = row + (x - bx0);
                dx[o] = quant((double) dx[o] + (ddx * cs - ddy * sn - ddx));
                dy[o] = quant((double) dy[o] + (ddx * sn + ddy * cs - ddy));
            }
        }
    }

    private static void resample(byte[] snap, byte[] img, float[] dx, float[] dy,
                                 int w, int h, int bx0, int by0, int bw, int bh) {
        int stride = w * 4;
        for (int y = 0; y < bh; ++y) {
            int row = y * bw;
            int dyi = by0 + y;
            for (int x = 0; x < bw; ++x) {
                float fx = dx[row + x], fy = dy[row + x];
                if (fx == 0f && fy == 0f) continue;
                sampleBilinear(snap, w, h, stride, bx0 + x + fx, dyi + fy,
                        img, dyi * stride + (bx0 + x) * 4);
            }
        }
    }

    private static void sampleBilinear(byte[] snap, int w, int h, int stride,
                                       float sx, float sy, byte[] dst, int off) {
        if (sx < 0f) sx = 0f;
        if (sy < 0f) sy = 0f;
        if (sx > w - 1) sx = w - 1;
        if (sy > h - 1) sy = h - 1;
        int x0i = (int) sx, y0i = (int) sy;
        int x1i = x0i + 1 < w ? x0i + 1 : x0i;
        int y1i = y0i + 1 < h ? y0i + 1 : y0i;
        float fx = sx - x0i, fy = sy - y0i;
        int o00 = y0i * stride + x0i * 4;
        int o10 = y0i * stride + x1i * 4;
        int o01 = y1i * stride + x0i * 4;
        int o11 = y1i * stride + x1i * 4;
        for (int k = 0; k < 4; ++k) {
            float top = (snap[o00 + k] & 0xFF) + ((snap[o10 + k] & 0xFF) - (snap[o00 + k] & 0xFF)) * fx;
            float bot = (snap[o01 + k] & 0xFF) + ((snap[o11 + k] & 0xFF) - (snap[o01 + k] & 0xFF)) * fx;
            dst[off + k] = (byte) (int) (top + (bot - top) * fy + 0.5f);
        }
    }

    /** 与 lq.c 的 LQ_SMOOTH 完全一致：逐步、按 falloff 半径盒式模糊后按权重混合。 */
    private static byte[] warpSmooth(byte[] img, int w, int h,
                                     int bx0, int bx1, int by0, int by1,
                                     float x0, float y0, float x1, float y1,
                                     float radius, float strength, int steps) {
        float invR = 1.0f / radius;
        for (int s = 1; s <= steps; ++s) {
            float t = (float) s / steps;
            float px = x0 + (x1 - x0) * t;
            float py = y0 + (y1 - y0) * t;
            int ix0 = clamp((int) Math.floor(px - radius - 1), bx0, bx1);
            int ix1 = clamp((int) Math.ceil(px + radius + 1), bx0, bx1);
            int iy0 = clamp((int) Math.floor(py - radius - 1), by0, by1);
            int iy1 = clamp((int) Math.ceil(py + radius + 1), by0, by1);
            smoothStep(img, w, h, ix0, ix1, iy0, iy1, px, py, invR, strength);
        }
        return img;
    }

    private static void smoothStep(byte[] img, int w, int h,
                                   int ix0, int ix1, int iy0, int iy1,
                                   float px, float py, float invR, float strength) {
        for (int y = iy0; y <= iy1; ++y) {
            for (int x = ix0; x <= ix1; ++x) {
                float ddx = x - px, ddy = y - py;
                float d = (float) Math.sqrt(ddx * ddx + ddy * ddy) * invR;
                float wgt = falloffD(d) * strength;
                if (wgt <= 0.0001f) continue;
                int rad = (int) (3.0f * wgt) + 1;
                int a0 = 0, a1 = 0, a2 = 0, a3 = 0, n = 0;
                for (int sy = y - rad; sy <= y + rad; sy += 2) {
                    if (sy < 0 || sy >= h) continue;
                    int base = sy * w * 4;
                    for (int sx = x - rad; sx <= x + rad; sx += 2) {
                        if (sx < 0 || sx >= w) continue;
                        int o = base + sx * 4;
                        a0 += img[o] & 0xFF; a1 += img[o + 1] & 0xFF;
                        a2 += img[o + 2] & 0xFF; a3 += img[o + 3] & 0xFF;
                        ++n;
                    }
                }
                if (n == 0) continue;
                int o = (y * w + x) * 4;
                img[o]     = (byte) ((a0 / n) * wgt + (img[o] & 0xFF) * (1f - wgt) + 0.5f);
                img[o + 1] = (byte) ((a1 / n) * wgt + (img[o + 1] & 0xFF) * (1f - wgt) + 0.5f);
                img[o + 2] = (byte) ((a2 / n) * wgt + (img[o + 2] & 0xFF) * (1f - wgt) + 0.5f);
                img[o + 3] = (byte) ((a3 / n) * wgt + (img[o + 3] & 0xFF) * (1f - wgt) + 0.5f);
            }
        }
    }
}
