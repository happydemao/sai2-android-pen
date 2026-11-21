/*
 * Dump lq.c output for the cross-implementation test.
 *
 * The Android side runs a Java port (android/LiquifyWarp.java). This program
 * produces the reference image for each mode from the exact same input and
 * stroke spec used by LiquifyCompare.java, so the two can be compared pixel by
 * pixel offline.
 *
 * Build/run:
 *   cc -O2 -std=c99 -o /tmp/lq_dump liquify/engine/lq.c liquify/engine/lq_dump.c -lm
 *   /tmp/lq_dump /private/tmp/lqref
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lq.h"

#define W 256
#define H 256

typedef struct { const char *name; int mode; float x0, y0, x1, y1, r, s; int steps; } Spec;

static const Spec SPECS[] = {
    { "forward",   LQ_FORWARD,   60, 60, 180, 140, 40, 0.8f, 20 },
    { "bloat",     LQ_BLOAT,    128, 128, 128, 128, 50, 0.8f, 10 },
    { "pinch",     LQ_PINCH,    128, 128, 128, 128, 50, 0.8f, 10 },
    { "twirl_cw",  LQ_TWIRL_CW, 128, 128, 128, 128, 60, 0.8f, 10 },
    { "twirl_ccw", LQ_TWIRL_CCW,128, 128, 128, 128, 60, 0.8f, 10 },
    { "smooth",    LQ_SMOOTH,   128, 128, 128, 128, 30, 0.8f,  4 },
};

static void make_input(unsigned char *p) {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *q = p + ((size_t)y * W + x) * 4;
            q[0] = (unsigned char)((x * 7 + y * 13) & 0xFF);
            q[1] = (unsigned char)((x * 11 + y * 5) & 0xFF);
            q[2] = (unsigned char)((x * 3 + y * 17) & 0xFF);
            q[3] = 0xFF;
        }
    /* a hard-edged marker so displacement is visible even where the gradient
     * is locally flat */
    for (int y = 100; y < 130; ++y)
        for (int x = 100; x < 130; ++x) {
            unsigned char *q = p + ((size_t)y * W + x) * 4;
            q[0] = 0x00; q[1] = 0x00; q[2] = 0xFF; q[3] = 0xFF;
        }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    for (size_t i = 0; i < sizeof SPECS / sizeof SPECS[0]; ++i) {
        const Spec *sp = &SPECS[i];
        unsigned char *p = (unsigned char*)malloc((size_t)W * H * 4);
        make_input(p);
        lq_ctx *c = lq_create(p, W, H, W * 4);
        if (!c) { fprintf(stderr, "lq_create failed\n"); return 1; }
        lq_stroke_begin(c, (int)sp->x0, (int)sp->y0, (int)sp->r);
        for (int s = 1; s <= sp->steps; ++s) {
            float t = (float)s / sp->steps;
            float px = sp->x0 + (sp->x1 - sp->x0) * t;
            float py = sp->y0 + (sp->y1 - sp->y0) * t;
            lq_apply(c, (lq_mode)sp->mode, px - (sp->x1 - sp->x0) / sp->steps,
                     py - (sp->y1 - sp->y0) / sp->steps, px, py, sp->r, sp->s);
        }
        int a[4];
        lq_flush(c, &a[0], &a[1], &a[2], &a[3]);
        lq_stroke_end(c);
        lq_destroy(c);

        char path[512];
        snprintf(path, sizeof path, "%s/%s.bin", dir, sp->name);
        FILE *f = fopen(path, "wb");
        if (!f) { perror(path); free(p); return 1; }
        fwrite(p, 1, (size_t)W * H * 4, f);
        fclose(f);
        printf("%-10s bbox=%d,%d..%d,%d -> %s\n", sp->name, a[0], a[1], a[2], a[3], path);
        free(p);
    }
    return 0;
}
