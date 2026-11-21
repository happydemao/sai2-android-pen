/*
 * Liquify engine - tile-based, allocation-free in the hot path.
 *
 * Design notes
 * ------------
 * The plugin gets a 32bpp BGRA buffer that is exactly what SAI presents to the
 * screen (see docs/20-canvas-capture.md). We do not know the document <->
 * buffer mapping for anything but the current zoom, so the engine works in
 * buffer space: one buffer pixel is one screen pixel, and at 100% zoom that is
 * one document pixel.
 *
 * A stroke never resamples the whole canvas. It keeps an inverse displacement
 * field ("which source pixel should land here") and rebuilds only the tiles
 * the brush touched. That is what makes it usable at 2560x1600 on a tablet:
 * cost tracks brush area, not canvas size.
 *
 * Everything is plain C99 with no libc allocation, so the same file compiles
 * into the Wine DLL and into the native test harness.
 */
#ifndef LIQUIFY_WARP_H
#define LIQUIFY_WARP_H

#include <stdint.h>
#include <stddef.h>

#define LQ_TILE 64           /* tile edge in pixels */
#define LQ_MAX_TILES_X 64
#define LQ_MAX_TILES_Y 64

typedef enum {
    LQ_FORWARD = 0,   /* push pixels along the drag */
    LQ_BLOAT,         /* push pixels away from the brush centre */
    LQ_PINCH,         /* pull pixels toward the brush centre */
    LQ_TWIRL_CW,      /* rotate pixels clockwise */
    LQ_TWIRL_CCW,     /* rotate pixels counter-clockwise */
    LQ_SMOOTH,        /* local average, no displacement */
    LQ_RECONSTRUCT,   /* pull the accumulated displacement back toward zero */
    LQ_MODE_COUNT
} lq_mode;

typedef struct {
    /* The surface we edit, BGRA, w*h pixels, `stride` bytes per row. */
    uint8_t *base;
    int w, h, stride;

    /* Inverse displacement in pixels, one float pair per buffer pixel. */
    float *fx, *fy;

    /* Per-tile dirty bookkeeping for the current stroke. */
    int tiles_x, tiles_y;
    uint8_t dirty[LQ_MAX_TILES_Y][LQ_MAX_TILES_X];

    /* Undo: one snapshot per stroke, of the tiles that were touched. */
    uint8_t *undo;          /* tiles_x*tiles_y*LQ_TILE*LQ_TILE*4 bytes */
    uint8_t undo_valid[LQ_MAX_TILES_Y][LQ_MAX_TILES_X];
    int have_undo;

    int stroke_active;
    int dirty_min_x, dirty_min_y, dirty_max_x, dirty_max_y;
} lq_ctx;

/* Convenience wrappers: allocate the context on the heap. */
lq_ctx *lq_create(uint8_t *base, int w, int h, int stride);
void    lq_destroy(lq_ctx *c);

/* Set up over a caller-owned buffer. Returns 0 on success. */
int  lq_init(lq_ctx *c, uint8_t *base, int w, int h, int stride);
void lq_free(lq_ctx *c);

/* Stroke framing. lq_stroke_begin snapshots the tiles the brush may touch so
 * that lq_undo() can put them back exactly. */
void lq_stroke_begin(lq_ctx *c, int x, int y, int radius);
void lq_stroke_end(lq_ctx *c);

/* One brush step. (x0,y0)->(x1,y1) is the pointer motion for this step,
 * radius in pixels, strength 0..1, mode from lq_mode. */
void lq_apply(lq_ctx *c, lq_mode mode, float x0, float y0, float x1, float y1,
              float radius, float strength);

/* Rebuild every tile the stroke has dirtied so far. Call once per painted
 * frame, not per brush step. Returns the bounding box actually written. */
void lq_flush(lq_ctx *c, int *minx, int *miny, int *maxx, int *maxy);

/* Revert the current stroke to the snapshot. */
int  lq_undo(lq_ctx *c);

/* Forget the displacement field (new stroke starts from identity). */
void lq_reset_field(lq_ctx *c);

#endif /* LIQUIFY_WARP_H */
