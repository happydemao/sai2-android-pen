/* Minimal stub for builds of wintab32.c from the public tree.
 * The release DLL used a private canvas-timelapse recorder; it is not part of
 * the pressure path. Keep these no-ops so the WinTab bridge builds standalone.
 */
#ifndef TIMELAPSE_WIN_H
#define TIMELAPSE_WIN_H
static void tl_startup(void) {}
static void tl_on_pressure(int p) { (void)p; }
static void tl_on_mouse_up(void) {}
#endif
