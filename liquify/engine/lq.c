/*
 * Liquify engine - see lq.h for the model.
 *
 * Stroke model
 * ------------
 * lq_stroke_begin() copies the whole surface once (that copy is both the undo
 * source and the resample source). Every brush step only accumulates an
 * inverse displacement field. lq_flush() rebuilds the touched tiles by
 * sampling the copy - so flushing twice with the same field is idempotent and
 * a stroke can be previewed as often as we like.
 *
 * Field storage is per tile, allocated on first touch, so a small brush stroke
 * over a big canvas costs a few hundred KB, not 70 MB.
 */
#include "lq.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    int16_t *dx;      /* 1/16 px units, LQ_TILE*LQ_TILE */
    int16_t *dy;
    uint8_t dirty;
} lq_tile;

struct lq_priv {
    lq_ctx pub;
    lq_tile *tiles;
    uint8_t *snapshot;      /* w*h*4, the stroke's starting image */
    int snap_valid;
    int dirty_min_x, dirty_min_y, dirty_max_x, dirty_max_y;
    int dirty_any;
};

#define PRIV(c) ((struct lq_priv *)(c))
#define FIX 16.0f

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline float falloff(float d) {
    if (d >= 1.0f) return 0.0f;
    float t = 1.0f - d * d;
    return t * t;
}

static lq_tile *tile_at(lq_ctx *c, int tx, int ty) {
    struct lq_priv *p = PRIV(c);
    if (tx < 0 || ty < 0 || tx >= c->tiles_x || ty >= c->tiles_y) return NULL;
    return &p->tiles[(size_t)ty * c->tiles_x + tx];
}

static lq_tile *tile_get(lq_ctx *c, int tx, int ty) {
    lq_tile *t = tile_at(c, tx, ty);
    if (!t) return NULL;
    if (!t->dx) {
        t->dx = (int16_t*)calloc((size_t)LQ_TILE * LQ_TILE, sizeof(int16_t));
        t->dy = (int16_t*)calloc((size_t)LQ_TILE * LQ_TILE, sizeof(int16_t));
        if (!t->dx || !t->dy) {
            free(t->dx); free(t->dy);
            t->dx = t->dy = NULL;
            return NULL;
        }
    }
    return t;
}

static void mark_dirty(lq_ctx *c, int x0, int y0, int x1, int y1) {
    struct lq_priv *p = PRIV(c);
    int tx0 = clampi(x0 / LQ_TILE, 0, c->tiles_x - 1);
    int ty0 = clampi(y0 / LQ_TILE, 0, c->tiles_y - 1);
    int tx1 = clampi(x1 / LQ_TILE, 0, c->tiles_x - 1);
    int ty1 = clampi(y1 / LQ_TILE, 0, c->tiles_y - 1);
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            lq_tile *t = tile_get(c, tx, ty);
            if (t) t->dirty = 1;
        }
    if (!p->dirty_any) {
        p->dirty_min_x = x0; p->dirty_min_y = y0;
        p->dirty_max_x = x1; p->dirty_max_y = y1;
        p->dirty_any = 1;
    } else {
        if (x0 < p->dirty_min_x) p->dirty_min_x = x0;
        if (y0 < p->dirty_min_y) p->dirty_min_y = y0;
        if (x1 > p->dirty_max_x) p->dirty_max_x = x1;
        if (y1 > p->dirty_max_y) p->dirty_max_y = y1;
    }
}

int lq_init(lq_ctx *c, uint8_t *base, int w, int h, int stride) {
    memset(c, 0, sizeof *c);
    struct lq_priv *p = PRIV(c);
    if (!base || w <= 0 || h <= 0 || stride < w * 4) return -1;
    c->base = base; c->w = w; c->h = h; c->stride = stride;
    c->tiles_x = (w + LQ_TILE - 1) / LQ_TILE;
    c->tiles_y = (h + LQ_TILE - 1) / LQ_TILE;
    if (c->tiles_x > LQ_MAX_TILES_X) c->tiles_x = LQ_MAX_TILES_X;
    if (c->tiles_y > LQ_MAX_TILES_Y) c->tiles_y = LQ_MAX_TILES_Y;
    p->tiles = (lq_tile*)calloc((size_t)c->tiles_x * c->tiles_y, sizeof(lq_tile));
    p->snapshot = (uint8_t*)malloc((size_t)stride * h);
    if (!p->tiles || !p->snapshot) return -1;
    return 0;
}

lq_ctx *lq_create(uint8_t *base, int w, int h, int stride) {
    struct lq_priv *p = (struct lq_priv*)calloc(1, sizeof *p);
    if (!p) return NULL;
    if (lq_init(&p->pub, base, w, h, stride) != 0) { free(p); return NULL; }
    return &p->pub;
}

void lq_destroy(lq_ctx *c) {
    if (!c) return;
    struct lq_priv *p = PRIV(c);
    lq_free(c);
    free(p);
}

void lq_free(lq_ctx *c) {
    struct lq_priv *p = PRIV(c);
    if (!p->tiles) return;
    for (size_t i = 0; i < (size_t)c->tiles_x * c->tiles_y; ++i) {
        free(p->tiles[i].dx); free(p->tiles[i].dy);
    }
    free(p->tiles); p->tiles = NULL;
    free(p->snapshot); p->snapshot = NULL;
    c->base = NULL;
}

void lq_reset_field(lq_ctx *c) {
    struct lq_priv *p = PRIV(c);
    for (size_t i = 0; i < (size_t)c->tiles_x * c->tiles_y; ++i) {
        lq_tile *t = &p->tiles[i];
        if (t->dx) {
            memset(t->dx, 0, (size_t)LQ_TILE * LQ_TILE * sizeof(int16_t));
            memset(t->dy, 0, (size_t)LQ_TILE * LQ_TILE * sizeof(int16_t));
        }
        t->dirty = 0;
    }
    p->dirty_any = 0;
}

void lq_stroke_begin(lq_ctx *c, int x, int y, int radius) {
    struct lq_priv *p = PRIV(c);
    (void)x; (void)y; (void)radius;
    lq_reset_field(c);
    /* One copy per stroke: it is both the undo source and what the field
     * resamples from, which keeps repeated flushes idempotent. */
    for (int row = 0; row < c->h; ++row)
        memcpy(p->snapshot + (size_t)row * c->stride,
               c->base + (size_t)row * c->stride, (size_t)c->w * 4);
    p->snap_valid = 1;
    p->dirty_any = 0;
    c->stroke_active = 1;
    c->have_undo = 1;
}

void lq_stroke_end(lq_ctx *c) {
    c->stroke_active = 0;
}

/* Accumulate the inverse displacement for one brush step. */
void lq_apply(lq_ctx *c, lq_mode mode, float x0, float y0, float x1, float y1,
              float radius, float strength) {
    if (!c->stroke_active) return;
    if (radius < 0.5f) radius = 0.5f;
    if (strength <= 0.0f) return;
    if (strength > 1.0f) strength = 1.0f;

    int r = (int)ceilf(radius) + 1;
    int cx = (int)x1, cy = (int)y1;
    int bx0 = clampi(cx - r, 0, c->w - 1), bx1 = clampi(cx + r, 0, c->w - 1);
    int by0 = clampi(cy - r, 0, c->h - 1), by1 = clampi(cy + r, 0, c->h - 1);

    float mdx = x1 - x0, mdy = y1 - y0;
    float inv_r = 1.0f / radius;

    for (int y = by0; y <= by1; ++y) {
        for (int x = bx0; x <= bx1; ++x) {
            float ddx = (float)x - x1, ddy = (float)y - y1;
            float d = sqrtf(ddx * ddx + ddy * ddy) * inv_r;
            float w = falloff(d) * strength;
            if (w <= 0.0001f) continue;

            float ox = 0.0f, oy = 0.0f;
            switch (mode) {
            case LQ_FORWARD:
                /* Sample from behind the drag: content follows the pointer. */
                ox = -mdx * w;
                oy = -mdy * w;
                break;
            case LQ_BLOAT:
            case LQ_PINCH: {
                /* Radial push. Bloat pulls content outward, so the sample point
                 * sits closer to the centre; pinch does the opposite. */
                float len = sqrtf(ddx * ddx + ddy * ddy);
                if (len < 0.001f) break;
                float k = radius * 0.06f * w * (mode == LQ_BLOAT ? -1.0f : 1.0f);
                ox = ddx / len * k;
                oy = ddy / len * k;
                break;
            }
            case LQ_TWIRL_CW:
            case LQ_TWIRL_CCW: {
                float len = sqrtf(ddx * ddx + ddy * ddy);
                if (len < 0.5f) break;
                /* Screen space is y-down, so a *clockwise* visual swirl needs
                 * the sample rotated by a negative angle: the content from
                 * -ang lands at p, i.e. content turns clockwise. The first
                 * version had the two directions swapped, which the native
                 * test now pins down. */
                float ang = 0.35f * w * (mode == LQ_TWIRL_CW ? -1.0f : 1.0f);
                float cs = cosf(ang), sn = sinf(ang);
                /* sample = R(ang) * (p - C), so the content appears to rotate
                 * by -ang. Field stores sample - p. */
                float rx = ddx * cs - ddy * sn;
                float ry = ddx * sn + ddy * cs;
                ox = rx - ddx;
                oy = ry - ddy;
                break;
            }
            case LQ_SMOOTH: {
                /* Blur is destructive on purpose: it has no displacement to
                 * express. Weighted toward the centre of the brush. */
                int rad = (int)(3.0f * w) + 1;
                int acc[4] = {0, 0, 0, 0}, n = 0;
                for (int sy = y - rad; sy <= y + rad; sy += 2) {
                    if (sy < 0 || sy >= c->h) continue;
                    const uint8_t *row = c->base + (size_t)sy * c->stride;
                    for (int sx = x - rad; sx <= x + rad; sx += 2) {
                        if (sx < 0 || sx >= c->w) continue;
                        const uint8_t *px = row + (size_t)sx * 4;
                        acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2]; acc[3] += px[3];
                        ++n;
                    }
                }
                if (n > 0) {
                    uint8_t *dst = c->base + (size_t)y * c->stride + (size_t)x * 4;
                    for (int k = 0; k < 4; ++k)
                        dst[k] = (uint8_t)((acc[k] / n) * w + dst[k] * (1.0f - w) + 0.5f);
                }
                continue;
            }
            default:
                break;
            }
            if (ox == 0.0f && oy == 0.0f) continue;

            int tx = x / LQ_TILE, ty = y / LQ_TILE;
            lq_tile *t = tile_get(c, tx, ty);
            if (!t) continue;
            size_t o = (size_t)(y % LQ_TILE) * LQ_TILE + (x % LQ_TILE);
            float nx = (float)t->dx[o] / FIX + ox;
            float ny = (float)t->dy[o] / FIX + oy;
            if (nx > 2047.0f) nx = 2047.0f;
            if (nx < -2047.0f) nx = -2047.0f;
            if (ny > 2047.0f) ny = 2047.0f;
            if (ny < -2047.0f) ny = -2047.0f;
            t->dx[o] = (int16_t)lrintf(nx * FIX);
            t->dy[o] = (int16_t)lrintf(ny * FIX);
        }
    }
    mark_dirty(c, bx0, by0, bx1, by1);
    /* Snapshot must follow destructive smoothing, otherwise a later flush
     * would erase it. */
    if (mode == LQ_SMOOTH) {
        for (int y = by0; y <= by1; ++y)
            memcpy(PRIV(c)->snapshot + (size_t)y * c->stride,
                   c->base + (size_t)y * c->stride, (size_t)c->w * 4);
    }
}

static inline void sample_bilinear(const uint8_t *snap, int stride, int w, int h,
                                   float sx, float sy, uint8_t out[4]) {
    if (sx < 0.0f) sx = 0.0f;
    if (sy < 0.0f) sy = 0.0f;
    if (sx > (float)(w - 1)) sx = (float)(w - 1);
    if (sy > (float)(h - 1)) sy = (float)(h - 1);
    int x0 = (int)sx, y0 = (int)sy;
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    float fx = sx - (float)x0, fy = sy - (float)y0;
    const uint8_t *p00 = snap + (size_t)y0 * stride + (size_t)x0 * 4;
    const uint8_t *p10 = snap + (size_t)y0 * stride + (size_t)x1 * 4;
    const uint8_t *p01 = snap + (size_t)y1 * stride + (size_t)x0 * 4;
    const uint8_t *p11 = snap + (size_t)y1 * stride + (size_t)x1 * 4;
    for (int k = 0; k < 4; ++k) {
        float top = p00[k] + (p10[k] - p00[k]) * fx;
        float bot = p01[k] + (p11[k] - p01[k]) * fx;
        out[k] = (uint8_t)(top + (bot - top) * fy + 0.5f);
    }
}

void lq_flush(lq_ctx *c, int *minx, int *miny, int *maxx, int *maxy) {
    struct lq_priv *p = PRIV(c);
    if (!p->dirty_any) {
        if (minx) *minx = *miny = *maxx = *maxy = -1;
        return;
    }
    int tx0 = p->dirty_min_x / LQ_TILE, tx1 = p->dirty_max_x / LQ_TILE;
    int ty0 = p->dirty_min_y / LQ_TILE, ty1 = p->dirty_max_y / LQ_TILE;
    tx0 = clampi(tx0, 0, c->tiles_x - 1); tx1 = clampi(tx1, 0, c->tiles_x - 1);
    ty0 = clampi(ty0, 0, c->tiles_y - 1); ty1 = clampi(ty1, 0, c->tiles_y - 1);

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            lq_tile *t = tile_at(c, tx, ty);
            if (!t || !t->dirty || !t->dx) continue;
            int x0 = tx * LQ_TILE, y0 = ty * LQ_TILE;
            int x1 = x0 + LQ_TILE; if (x1 > c->w) x1 = c->w;
            int y1 = y0 + LQ_TILE; if (y1 > c->h) y1 = c->h;
            for (int y = y0; y < y1; ++y) {
                uint8_t *drow = c->base + (size_t)y * c->stride;
                size_t trow = (size_t)(y - y0) * LQ_TILE;
                for (int x = x0; x < x1; ++x) {
                    size_t o = trow + (size_t)(x - x0);
                    int16_t dx = t->dx[o], dy = t->dy[o];
                    if (!dx && !dy) continue;
                    uint8_t px[4];
                    sample_bilinear(p->snapshot, c->stride, c->w, c->h,
                                    (float)x + (float)dx / FIX,
                                    (float)y + (float)dy / FIX, px);
                    memcpy(drow + (size_t)x * 4, px, 4);
                }
            }
            t->dirty = 0;
        }
    }
    if (minx) *minx = p->dirty_min_x;
    if (miny) *miny = p->dirty_min_y;
    if (maxx) *maxx = p->dirty_max_x;
    if (maxy) *maxy = p->dirty_max_y;
    p->dirty_any = 0;
}

int lq_undo(lq_ctx *c) {
    struct lq_priv *p = PRIV(c);
    if (!p->snap_valid) return -1;
    for (int row = 0; row < c->h; ++row)
        memcpy(c->base + (size_t)row * c->stride,
               p->snapshot + (size_t)row * c->stride, (size_t)c->w * 4);
    lq_reset_field(c);
    return 0;
}
