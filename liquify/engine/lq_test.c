/*
 * Native test for the liquify engine (no Wine, no device).
 *
 * Build/run:
 *   cc -O2 -std=c99 -o /tmp/lq_test liquify/engine/lq.c liquify/engine/lq_test.c -lm
 *   /tmp/lq_test
 *
 * Each mode gets a fresh canvas with one red marker square, one stroke is
 * applied, and the marker's movement is checked against what that mode means.
 * This is the contract both the DLL preview and the Android side rely on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lq.h"

#define W 256
#define H 256

static unsigned char *make_canvas(void) {
    unsigned char *p = (unsigned char*)malloc((size_t)W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        p[i*4+0] = 0xFF; p[i*4+1] = 0xFF; p[i*4+2] = 0xFF; p[i*4+3] = 0;
    }
    /* marker 10x10 at (100,120) */
    for (int y = 120; y < 130; ++y)
        for (int x = 100; x < 110; ++x) {
            unsigned char *q = p + ((size_t)y * W + x) * 4;
            q[0] = 0x00; q[1] = 0x00; q[2] = 0xFF; q[3] = 0;
        }
    return p;
}

/* centroid and count of pure-red pixels */
static void red_stats(const unsigned char *p, double *cx, double *cy, int *n) {
    double sx = 0, sy = 0; int k = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const unsigned char *q = p + ((size_t)y * W + x) * 4;
            if (q[2] > 0xF0 && q[0] < 0x20 && q[1] < 0x20) { sx += x; sy += y; ++k; }
        }
    *cx = k ? sx / k : 0; *cy = k ? sy / k : 0; *n = k;
}

static double dist(double ax, double ay, double bx, double by) {
    return sqrt((ax-bx)*(ax-bx) + (ay-by)*(ay-by));
}

static int failures;

static void check(const char *name, int ok, const char *detail) {
    printf("%-12s %s   %s\n", name, ok ? "PASS" : "FAIL", detail);
    if (!ok) ++failures;
}

int main(void) {
    /* ---- forward: marker must follow the drag (+x) ---- */
    {
        unsigned char *p = make_canvas();
        double x0, y0, x1, y1; int n0, n1;
        red_stats(p, &x0, &y0, &n0);
        lq_ctx *c = lq_create(p, W, H, W * 4);
        lq_stroke_begin(c, 80, 125, 40);
        for (int i = 1; i <= 20; ++i)
            lq_apply(c, LQ_FORWARD, 80 + i - 1, 125, 80 + i, 125, 40, 1.0f);
        int a[4]; lq_flush(c, &a[0], &a[1], &a[2], &a[3]);
        red_stats(p, &x1, &y1, &n1);
        char d[128]; snprintf(d, sizeof d, "cx %.1f -> %.1f (dx %.1f)", x0, x1, x1 - x0);
        check("forward", x1 > x0 + 3 && n1 > n0 / 2, d);
        lq_destroy(c); free(p);
    }

    /* ---- bloat: marker must move away from the brush centre ---- */
    {
        unsigned char *p = make_canvas();      /* marker at (105,125), brush at (105,125) area edge */
        double x0, y0, x1, y1; int n0, n1;
        red_stats(p, &x0, &y0, &n0);
        lq_ctx *c = lq_create(p, W, H, W * 4);
        /* centre left of the marker, so "away" is clearly +x */
        lq_stroke_begin(c, 90, 125, 35);
        /* A few steps only: with many steps the marker leaves the brush
         * radius entirely and the centroid stops describing the mode. */
        for (int i = 0; i < 3; ++i) lq_apply(c, LQ_BLOAT, 90, 125, 90, 125, 35, 1.0f);
        int a[4]; lq_flush(c, &a[0], &a[1], &a[2], &a[3]);
        red_stats(p, &x1, &y1, &n1);
        char d[128]; snprintf(d, sizeof d, "cx %.1f -> %.1f", x0, x1);
        check("bloat", x1 > x0 + 1.0 && n1 > 0, d);
        lq_destroy(c); free(p);
    }

    /* ---- pinch: marker must move toward the brush centre ---- */
    {
        unsigned char *p = make_canvas();
        double x0, y0, x1, y1; int n0, n1;
        red_stats(p, &x0, &y0, &n0);
        lq_ctx *c = lq_create(p, W, H, W * 4);
        lq_stroke_begin(c, 70, 125, 45);
        for (int i = 0; i < 3; ++i) lq_apply(c, LQ_PINCH, 70, 125, 70, 125, 45, 1.0f);
        int a[4]; lq_flush(c, &a[0], &a[1], &a[2], &a[3]);
        red_stats(p, &x1, &y1, &n1);
        char d[128]; snprintf(d, sizeof d, "cx %.1f -> %.1f", x0, x1);
        check("pinch", x1 < x0 - 1.0 && n1 > 0, d);
        lq_destroy(c); free(p);
    }

    /* ---- twirl: a marker right of the centre must move DOWN for a clockwise
     * swirl on screen (y grows downwards), and UP for counter-clockwise. ---- */
    {
        for (int dir = 0; dir < 2; ++dir) {
            unsigned char *p = make_canvas();
            /* move the marker to the right of the brush centre */
            for (int y = 120; y < 130; ++y)
                for (int x = 100; x < 110; ++x) {
                    unsigned char *q = p + ((size_t)y * W + x) * 4;
                    q[0] = 0xFF; q[1] = 0xFF; q[2] = 0xFF;
                }
            for (int y = 123; y < 133; ++y)
                for (int x = 160; x < 170; ++x) {
                    unsigned char *q = p + ((size_t)y * W + x) * 4;
                    q[0] = 0x00; q[1] = 0x00; q[2] = 0xFF;
                }
            double x0, y0, x1, y1; int n0, n1;
            red_stats(p, &x0, &y0, &n0);
            lq_ctx *c = lq_create(p, W, H, W * 4);
            lq_stroke_begin(c, 128, 128, 60);
            for (int i = 0; i < 4; ++i)
                lq_apply(c, dir == 0 ? LQ_TWIRL_CW : LQ_TWIRL_CCW,
                         128, 128, 128, 128, 60, 1.0f);
            int a[4]; lq_flush(c, &a[0], &a[1], &a[2], &a[3]);
            red_stats(p, &x1, &y1, &n1);
            char d[128];
            snprintf(d, sizeof d, "y %.1f -> %.1f", y0, y1);
            if (dir == 0) check("twirl_cw", n1 > 0 && y1 > y0 + 0.5, d);
            else          check("twirl_ccw", n1 > 0 && y1 < y0 - 0.5, d);
            lq_destroy(c); free(p);
        }
    }

    /* ---- smooth: pure-red pixels must shrink (blended edges) ---- */
    {
        unsigned char *p = make_canvas();
        double x0, y0, x1, y1; int n0, n1;
        red_stats(p, &x0, &y0, &n0);
        lq_ctx *c = lq_create(p, W, H, W * 4);
        lq_stroke_begin(c, 105, 125, 30);
        for (int i = 0; i < 6; ++i) lq_apply(c, LQ_SMOOTH, 105, 125, 105, 125, 30, 1.0f);
        int a[4]; lq_flush(c, &a[0], &a[1], &a[2], &a[3]);
        red_stats(p, &x1, &y1, &n1);
        char d[128]; snprintf(d, sizeof d, "pure red %d -> %d", n0, n1);
        check("smooth", n1 < n0, d);
        lq_destroy(c); free(p);
    }

    /* ---- flush is idempotent, undo restores ---- */
    {
        unsigned char *p = make_canvas();
        unsigned char *orig = (unsigned char*)malloc((size_t)W * H * 4);
        memcpy(orig, p, (size_t)W * H * 4);
        lq_ctx *c = lq_create(p, W, H, W * 4);
        lq_stroke_begin(c, 80, 125, 40);
        for (int i = 1; i <= 20; ++i)
            lq_apply(c, LQ_FORWARD, 80 + i - 1, 125, 80 + i, 125, 40, 1.0f);
        int a[4]; lq_flush(c, &a[0], &a[1], &a[2], &a[3]);
        unsigned char *once = (unsigned char*)malloc((size_t)W * H * 4);
        memcpy(once, p, (size_t)W * H * 4);
        lq_flush(c, &a[0], &a[1], &a[2], &a[3]);
        check("idempotent", memcmp(once, p, (size_t)W * H * 4) == 0, "flush twice");
        lq_undo(c);
        check("undo", memcmp(orig, p, (size_t)W * H * 4) == 0, "restores snapshot");
        free(once); free(orig); lq_destroy(c); free(p);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
