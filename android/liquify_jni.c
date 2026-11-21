/*
 * JNI wrapper for the shared liquify engine (liquify/engine/lq.c).
 *
 * Why: the pure-Java port is byte-exact but ART runs it interpreted in this
 * app (it is debuggable, so the system pins dexopt to "verify" and ignores
 * `pm compile -m speed`), which makes a 1600x1600 stroke take ~900 ms - slower
 * than the Box64 x86 engine. The same lq.c compiled for arm64 runs natively.
 *
 * Build (NDK):
 *   $NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang \
 *       -shared -fPIC -O2 -o libliquify.so liquify_jni.c ../liquify/engine/lq.c
 */
#include <jni.h>
#include "lq.h"

JNIEXPORT void JNICALL
Java_com_winlator_sai_LiquifyWarp_warpNative(JNIEnv *env, jclass cls,
                                             jbyteArray img, jint w, jint h,
                                             jint mode,
                                             jfloat x0, jfloat y0,
                                             jfloat x1, jfloat y1,
                                             jfloat radius, jfloat strength,
                                             jint steps) {
    (void)cls;
    if (!img || w <= 0 || h <= 0) return;
    if (steps < 1) steps = 1;
    jsize len = (*env)->GetArrayLength(env, img);
    if (len < (jsize)((size_t)w * 4 * (size_t)h)) return;

    jbyte *base = (*env)->GetByteArrayElements(env, img, NULL);
    if (!base) return;

    lq_ctx *c = lq_create((uint8_t*)base, w, h, w * 4);
    if (c) {
        lq_stroke_begin(c, (int)x0, (int)y0, (int)radius);
        for (int s = 1; s <= steps; ++s) {
            float t = (float)s / (float)steps;
            float px = x0 + (x1 - x0) * t;
            float py = y0 + (y1 - y0) * t;
            lq_apply(c, (lq_mode)mode,
                     px - (x1 - x0) / steps, py - (y1 - y0) / steps,
                     px, py, radius, strength);
        }
        int a[4];
        lq_flush(c, &a[0], &a[1], &a[2], &a[3]);
        lq_stroke_end(c);
        lq_destroy(c);
    }
    (*env)->ReleaseByteArrayElements(env, img, base, 0);
}
