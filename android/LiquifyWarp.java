package com.winlator.sai;

/**
 * 液化引擎（Android 端，纯 Java）。
 *
 * 这是 liquify/engine/lq.c 的 Java 对应实现，供 app 侧的 {@link LiquifyServer}
 * 在 ARM 上直接处理 1600x1600 文档图。同一套「逆位移场 + 从快照重采样」模型：
 *
 *   - 一笔开始复制一份快照（既是 undo 源，也是重采样源）
 *   - 每个笔刷步只往位移场里累加偏移（float，单位像素）
 *   - 最后一次性按位移场重建笔刷影响到的矩形区域
 *
 * 之所以不在 Wine 里算：Box64 跑 x86 版本一笔 1600x1600 要约 600 ms，
 * Android 原生 Java 同样的算法在 ARM 上快得多，而且省掉一次 10 MB 的
 * 跨进程调度。
 */
public final class LiquifyWarp {

    public static final int FORWARD = 0;
    public static final int BLOAT = 1;
    public static final int PINCH = 2;
    public static final int TWIRL_CW = 3;
    public static final int TWIRL_CCW = 4;
    public static final int SMOOTH = 5;

    private LiquifyWarp() { }

    /* falloff(d) = (1-d^2)^2 sampled over d^2, so the hot loop never calls
     * Math.sqrt. 1024 steps is far finer than the 8-bit result needs. */
    private static final int LUT_N = 1024;
    private static final float[] LUT = new float[LUT_N + 1];
    static {
        for (int i = 0; i <= LUT_N; ++i) {
            float d2 = (float) i / LUT_N;
            float t = 1.0f - d2;
            LUT[i] = t * t;
        }
    }

    private static float falloff(float d) {
        if (d >= 1.0f) return 0.0f;
        float t = 1.0f - d * d;
        return t * t;
    }

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /**
     * @param img  BGRA，行优先，w*h*4 字节；原地修改并返回（同时返回新数组方便调用方）
     * @param mode FORWARD / BLOAT / PINCH / TWIRL_CW / TWIRL_CCW / SMOOTH
     */
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

        /* 影响区域：整条笔迹的包围盒外扩半径 + 2 */
        float minX = Math.min(x0, x1) - radius - 2;
        float maxX = Math.max(x0, x1) + radius + 2;
        float minY = Math.min(y0, y1) - radius - 2;
        float maxY = Math.max(y0, y1) + radius + 2;
        int bx0 = clamp((int) Math.floor(minX), 0, w - 1);
        int bx1 = clamp((int) Math.ceil(maxX), 0, w - 1);
        int by0 = clamp((int) Math.floor(minY), 0, h - 1);
        int by1 = clamp((int) Math.ceil(maxY), 0, h - 1);
        int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
        if (bw <= 0 || bh <= 0) return img;

        if (mode == SMOOTH) return warpSmooth(img, w, h, bx0, bx1, by0, by1,
                x0, y0, x1, y1, radius, strength, steps);

        byte[] snap = img.clone();
        float[] dx = new float[bw * bh];
        float[] dy = new float[bw * bh];

        /* Per-step lookup tables: the brush weight only depends on the squared
         * distance, so sqrt/sin/cos move out of the inner loop. 1024 buckets
         * keep the error well below a pixel. */
        final int B = 1024;
        float[] tabCos = new float[B + 1];
        float[] tabSin = new float[B + 1];
        float[] tabScale = new float[B + 1];
        float[] tabW = new float[B + 1];
        float invR2 = 1.0f / (radius * radius);

        for (int s = 1; s <= steps; ++s) {
            float t = (float) s / steps;
            float px = x0 + (x1 - x0) * t;
            float py = y0 + (y1 - y0) * t;
            /* Mirror lq.c's arithmetic exactly: it receives x0 = px - step and
             * derives the delta by subtraction, which matters because the field
             * is quantised to 1/16 px. */
            float x0arg = px - (x1 - x0) / steps;
            float y0arg = py - (y1 - y0) / steps;
            float mdx = px - x0arg;
            float mdy = py - y0arg;
            int ix0 = clamp((int) Math.floor(px - radius - 1), bx0, bx1);
            int ix1 = clamp((int) Math.ceil(px + radius + 1), bx0, bx1);
            int iy0 = clamp((int) Math.floor(py - radius - 1), by0, by1);
            int iy1 = clamp((int) Math.ceil(py + radius + 1), by0, by1);

            if (mode == TWIRL_CW || mode == TWIRL_CCW) {
                float dir = (mode == TWIRL_CW) ? -1.0f : 1.0f;
                for (int i = 0; i <= B; ++i) {
                    float d2 = (float) i / B;
                    float wgt = LUT[i * LUT_N / B] * strength;
                    float ang = 0.35f * wgt * dir;
                    tabCos[i] = (float) Math.cos(ang);
                    tabSin[i] = (float) Math.sin(ang);
                }
            } else if (mode == BLOAT || mode == PINCH) {
                float sgn = (mode == BLOAT) ? -1.0f : 1.0f;
                for (int i = 0; i <= B; ++i) {
                    float d2 = (float) i / B;
                    float wgt = LUT[i * LUT_N / B] * strength;
                    float d = (float) Math.sqrt(d2);
                    tabScale[i] = (d < 1e-3f) ? 0.0f : (0.06f * wgt * sgn / d);
                }
            } else {
                for (int i = 0; i <= B; ++i)
                    tabW[i] = LUT[i * LUT_N / B] * strength;
            }

            for (int y = iy0; y <= iy1; ++y) {
                int row = (y - by0) * bw;
                for (int x = ix0; x <= ix1; ++x) {
                    float ddx = x - px, ddy = y - py;
                    float d2 = (ddx * ddx + ddy * ddy) * invR2;
                    if (d2 >= 1.0f) continue;
                    int bi = (int) (d2 * B);
                    float ox, oy;
                    if (mode == FORWARD) {
                        /* Reproduce lq.c's falloff arithmetic (sqrt then d*d)
                         * so the 1/16 px field quantisation is identical. */
                        float dl = (float) Math.sqrt(ddx * ddx + ddy * ddy) * (1.0f / radius);
                        float t1 = 1.0f - dl * dl;
                        float wgt = t1 * t1 * strength;
                        if (wgt <= 0.0001f) continue;
                        ox = -mdx * wgt;
                        oy = -mdy * wgt;
                    } else if (mode == BLOAT || mode == PINCH) {
                        float t1 = 1.0f - d2;
                        float wgt = t1 * t1 * strength;
                        if (wgt <= 0.0001f) continue;
                        float len = (float) Math.sqrt(ddx * ddx + ddy * ddy);
                        if (len < 0.001f) continue;
                        float k = radius * 0.06f * wgt * (mode == BLOAT ? -1.0f : 1.0f);
                        ox = ddx / len * k;
                        oy = ddy / len * k;
                    } else if (mode == TWIRL_CW || mode == TWIRL_CCW) {
                        /* angle depends continuously on the weight; a table over
                         * d^2 keeps sin/cos out of the inner loop (error ~1e-3 rad) */
                        /* linear interpolation between buckets removes the
                         * remaining visible difference vs lq.c */
                        float fidx = d2 * B;
                        int i0 = (int) fidx;
                        if (i0 >= B) i0 = B - 1;
                        float fr = fidx - i0;
                        float cs = tabCos[i0] + (tabCos[i0 + 1] - tabCos[i0]) * fr;
                        float sn = tabSin[i0] + (tabSin[i0 + 1] - tabSin[i0]) * fr;
                        ox = ddx * cs - ddy * sn - ddx;
                        oy = ddx * sn + ddy * cs - ddy;
                    } else {
                        continue;
                    }
                    int o = row + (x - bx0);
                    /* lq.c stores the field as int16 in 1/16 px and accumulates
                     * in double with lrint (round half to even); do it in double
                     * too, otherwise a handful of edge pixels quantise apart. */
                    dx[o] = (float) (Math.rint(((double) dx[o] + ox) * 16.0) / 16.0);
                    dy[o] = (float) (Math.rint(((double) dy[o] + oy) * 16.0) / 16.0);
                }
            }
        }

        /* 重建：对影响区域按位移场从快照双线性采样 */
        int stride = w * 4;
        for (int y = by0; y <= by1; ++y) {
            int row = (y - by0) * bw;
            for (int x = bx0; x <= bx1; ++x) {
                int o = row + (x - bx0);
                float fx = dx[o], fy = dy[o];
                if (fx == 0f && fy == 0f) continue;
                sampleBilinear(snap, w, h, stride, x + fx, y + fy, img, y * stride + x * 4);
            }
        }

        return img;
    }

    /** Exact port of lq.c's LQ_SMOOTH: per brush step, a box blur whose
     *  radius and blend weight follow the falloff, applied in place. */
    private static byte[] warpSmooth(byte[] img, int w, int h,
                                     int bx0, int bx1, int by0, int by1,
                                     float x0, float y0, float x1, float y1,
                                     float radius, float strength, int steps) {
        final int B = LUT_N;
        float invR2 = 1.0f / (radius * radius);
        for (int s = 1; s <= steps; ++s) {
            float t = (float) s / steps;
            float px = x0 + (x1 - x0) * t;
            float py = y0 + (y1 - y0) * t;
            int ix0 = clamp((int) Math.floor(px - radius - 1), bx0, bx1);
            int ix1 = clamp((int) Math.ceil(px + radius + 1), bx0, bx1);
            int iy0 = clamp((int) Math.floor(py - radius - 1), by0, by1);
            int iy1 = clamp((int) Math.ceil(py + radius + 1), by0, by1);
            for (int y = iy0; y <= iy1; ++y) {
                for (int x = ix0; x <= ix1; ++x) {
                    float ddx = x - px, ddy = y - py;
                    float d2 = (ddx * ddx + ddy * ddy) * invR2;
                    if (d2 >= 1.0f) continue;
                    float t1 = 1.0f - d2;
                    float wgt = t1 * t1 * strength;
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
        return img;
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

    private static void smoothBox(byte[] img, int w, int h,
                                  int bx0, int bx1, int by0, int by1, int rad) {
        int stride = w * 4;
        byte[] src = img.clone();
        for (int y = by0; y <= by1; ++y) {
            for (int x = bx0; x <= bx1; ++x) {
                int acc0 = 0, acc1 = 0, acc2 = 0, acc3 = 0, n = 0;
                for (int sy = Math.max(by0, y - rad); sy <= Math.min(by1, y + rad); sy += 2) {
                    int base = sy * stride;
                    for (int sx = Math.max(bx0, x - rad); sx <= Math.min(bx1, x + rad); sx += 2) {
                        int o = base + sx * 4;
                        acc0 += src[o] & 0xFF;
                        acc1 += src[o + 1] & 0xFF;
                        acc2 += src[o + 2] & 0xFF;
                        acc3 += src[o + 3] & 0xFF;
                        ++n;
                    }
                }
                if (n == 0) continue;
                int o = y * stride + x * 4;
                img[o] = (byte) (acc0 / n);
                img[o + 1] = (byte) (acc1 / n);
                img[o + 2] = (byte) (acc2 / n);
                img[o + 3] = (byte) (acc3 / n);
            }
        }
    }
}
