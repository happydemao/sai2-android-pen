/*
 * Minimal custom wintab32.dll for PaintTool SAI Ver.2 under Wine on macOS.
 *
 * Purpose: feed SAI pen pressure. SAI already gets POSITION from the normal
 * cursor (it draws lines fine); this DLL supplies the WinTab pressure stream.
 *
 * Architecture:
 *   - No synthetic/test injection (so no phantom stroke on startup).
 *   - A producer thread reads the current pressure (0..g_max_press) from a small file
 *     C:\wt_pressure.txt (which a native macOS helper writes from the real
 *     Wacom tablet). When pressure > 0 (pen down) it emits a packet at the
 *     LIVE cursor position and posts WT_PACKET to SAI's window; SAI then calls
 *     WTPacket() to read it. When pressure == 0 (pen up) it stays idle and SAI
 *     just draws with the mouse as usual.
 *
 * Build (from ~/Documents/wineORwhiskyFORSAI/wintab-src):
 *   x86_64-w64-mingw32-gcc -shared -O2 -o wintab32.dll wintab32.c wintab32.def \
 *       -lgdi32 -luser32 -municode
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <stddef.h>   /* offsetof, for the wire-format assertions */

#include "wintab_core.h"   /* the pure logic (parse/map/conflate) — unit-tested natively */
#include "../engine/lq.h"  /* the liquify engine, shared with the Android side */

#define SAMPLE_PORT 47800   /* UDP; the mac helper sends every pen sample here */

/* ---- WinTab constants / structs (subset; we don't have wintab.h in mingw) - */
typedef DWORD WTPKT;
typedef struct { LONG axMin, axMax; UINT axUnits; DWORD axResolution; } AXIS;
typedef struct { int orAzimuth, orAltitude, orTwist; } ORIENTATION;

#define LCNAMELEN 40
typedef struct {
    WCHAR lcName[LCNAMELEN];
    UINT  lcOptions, lcStatus, lcLocks, lcMsgBase, lcDevice, lcPktRate;
    WTPKT lcPktData, lcPktMode, lcMoveMask;
    DWORD lcBtnDnMask, lcBtnUpMask;
    LONG  lcInOrgX, lcInOrgY, lcInOrgZ, lcInExtX, lcInExtY, lcInExtZ;
    LONG  lcOutOrgX, lcOutOrgY, lcOutOrgZ, lcOutExtX, lcOutExtY, lcOutExtZ;
    DWORD lcSensX, lcSensY, lcSensZ;
    BOOL  lcSysMode;
    int   lcSysOrgX, lcSysOrgY, lcSysExtX, lcSysExtY;
    DWORD lcSysSensX, lcSysSensY;
} LOGCONTEXTW;

/* packet field-mask bits (WTPKT) */
#define PK_CONTEXT 0x0001u
#define PK_STATUS 0x0002u
#define PK_TIME 0x0004u
#define PK_CHANGED 0x0008u
#define PK_SERIAL_NUMBER 0x0010u
#define PK_CURSOR 0x0020u
#define PK_BUTTONS 0x0040u
#define PK_X 0x0080u
#define PK_Y 0x0100u
#define PK_Z 0x0200u
#define PK_NORMAL_PRESSURE 0x0400u
#define PK_TANGENT_PRESSURE 0x0800u
#define PK_ORIENTATION 0x1000u
#define PK_ROTATION 0x2000u
#define OUR_PKTDATA (PK_STATUS|PK_CURSOR|PK_BUTTONS|PK_X|PK_Y|PK_NORMAL_PRESSURE|PK_ORIENTATION) /* 0x15e2 */

#define WT_DEFBASE 0x7ff0
#define WT_PACKET  (WT_DEFBASE+0)
/* Full-scale pressure. CHOSEN AT RUNTIME so the user can trade resolution
 * against jitter from the setup window (issue #21): more levels means finer
 * control but also that sensor noise stops being quantised away.
 *
 * Read once from C:\\wt_pmax.txt, which the mac side writes BEFORE launching
 * SAI. Both halves read the same file, so the helper's scaling and the axis we
 * advertise to SAI can never disagree -- and SAI reads the axis at WTOpen, so
 * it must be settled before then. Missing or invalid file -> 1023 (the long-standing
 * default). */
/* Build stamp: the only way to answer "is the fix even loaded?" from the
 * outside. It goes into the log line AND into the status file the app reads. */
#define WT_BUILD_STAMP __DATE__ " " __TIME__

#define MAX_PRESS_CEILING 8191
static int g_max_press = 1023;
static void send_plain_key(char c);
static void send_key_state(char c, int down);
static void ms_log(const char *fmt, ...);
static void ensure_plugin_threads(void);
static int g_last_press;                       /* last WinTab pressure (0..4095) */
static volatile int g_eraser;                  /* pen's eraser end is down */
static int g_ui_press_max;
#define MAX_PRESS  g_max_press

static void load_max_press(void) {
    FILE *f = fopen("C:\\wt_pmax.txt", "rb");
    if (!f) return;
    char b[32] = {0};
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    b[n] = '\0';
    int v = atoi(b);
    if (v >= 255 && v <= MAX_PRESS_CEILING) g_max_press = v;
}
#define IN_EXT     32767

/* our packet, in field order matching OUR_PKTDATA (36 bytes) */
typedef struct {
    UINT status;
    UINT cursor;
    UINT buttons;
    LONG x;
    LONG y;
    UINT pressure;
    int  orAzimuth, orAltitude, orTwist;
} OURPKT;

/* The wire format, asserted at compile time in BOTH files that define it.
 * SAI reads these 36 bytes positionally, by the field order OUR_PKTDATA
 * advertises (0x15e2): status, cursor, buttons, x, y, pressure, orientation.
 * Reorder a field, change a type, or let padding creep in, and pressure lands
 * in whatever slot moved into its place — strokes keep drawing, at the wrong
 * width, with nothing failing anywhere. wtprobe.c carries its own copy of this
 * struct so it can stand in for SAI, so the two must agree; these assertions
 * are what makes that "must" mean something. */
_Static_assert(sizeof(OURPKT) == 36, "OURPKT must stay 36 bytes: SAI reads it positionally");
_Static_assert(offsetof(OURPKT, status)   ==  0, "OURPKT.status moved");
_Static_assert(offsetof(OURPKT, cursor)   ==  4, "OURPKT.cursor moved");
_Static_assert(offsetof(OURPKT, buttons)  ==  8, "OURPKT.buttons moved");
_Static_assert(offsetof(OURPKT, x)        == 12, "OURPKT.x moved");
_Static_assert(offsetof(OURPKT, y)        == 16, "OURPKT.y moved");
_Static_assert(offsetof(OURPKT, pressure) == 20, "OURPKT.pressure moved: SAI would read the wrong field");
_Static_assert(offsetof(OURPKT, orAzimuth) == 24, "OURPKT.orAzimuth moved");

/* Build one packet in the field order requested by the app's lcPktData.
 * Wintab lays fields out low-bit-first, tightly packed. SAI asks for 0x15e2
 * (36 bytes) and this reproduces the old OURPKT byte-for-byte; FireAlpaca
 * asks for 0x1fe0 (40 bytes, includes Z and tangential pressure). Honour the
 * app's mask instead of forcing SAI's, otherwise polling apps read pressure
 * from the wrong offset. */
static int wt_packet_size(WTPKT mask) {
    int n = 0;
    if (mask & PK_CONTEXT) n += 4;
    if (mask & PK_STATUS) n += 4;
    if (mask & PK_TIME) n += 4;
    if (mask & PK_CHANGED) n += 4;
    if (mask & PK_SERIAL_NUMBER) n += 4;
    if (mask & PK_CURSOR) n += 4;
    if (mask & PK_BUTTONS) n += 4;
    if (mask & PK_X) n += 4;
    if (mask & PK_Y) n += 4;
    if (mask & PK_Z) n += 4;
    if (mask & PK_NORMAL_PRESSURE) n += 4;
    if (mask & PK_TANGENT_PRESSURE) n += 4;
    if (mask & PK_ORIENTATION) n += 12;
    if (mask & PK_ROTATION) n += 12;
    return n;
}

static int wt_build_packet(const OURPKT *pk, UINT serial, WTPKT mask, void *buf) {
    unsigned char *p = (unsigned char *)buf;
    #define PUT32(v) do { UINT _v = (UINT)(v); memcpy(p, &_v, 4); p += 4; } while (0)
    if (mask & PK_CONTEXT) PUT32(0xC0FFEE01u);
    if (mask & PK_STATUS) PUT32(pk->status);
    if (mask & PK_TIME) PUT32(GetTickCount());
    if (mask & PK_CHANGED) PUT32(mask);
    if (mask & PK_SERIAL_NUMBER) PUT32(serial);
    if (mask & PK_CURSOR) PUT32(pk->cursor);
    if (mask & PK_BUTTONS) PUT32(pk->buttons);
    if (mask & PK_X) PUT32(pk->x);
    if (mask & PK_Y) PUT32(pk->y);
    if (mask & PK_Z) PUT32(0);
    if (mask & PK_NORMAL_PRESSURE) PUT32(pk->pressure);
    if (mask & PK_TANGENT_PRESSURE) PUT32(0);
    if (mask & PK_ORIENTATION) { PUT32(pk->orAzimuth); PUT32(pk->orAltitude); PUT32(pk->orTwist); }
    if (mask & PK_ROTATION) { PUT32(0); PUT32(0); PUT32(0); }
    #undef PUT32
    return (int)(p - (unsigned char *)buf);
}

/* WinTab categories */
#define WTI_INTERFACE 1
#define WTI_DEFCONTEXT 3
#define WTI_DEFSYSCTX 4
#define WTI_DEVICES 100
#define WTI_CURSORS 200
#define WTI_DDCTXS 400

/* device info indices */
#define DVC_NAME 1
#define DVC_VERSION 2
#define DVC_X 3
#define DVC_Y 4
#define DVC_Z 5
#define DVC_NPRESSURE 15
#define DVC_TPRESSURE 16
#define DVC_ORIENTATION 17
#define DVC_ROTATION 18
#define DVC_PKTRATE 19
#define DVC_PKTDATA 20
#define DVC_PKTMODE 21
#define DVC_HARDWARE 22
#define DVC_NCSRTYPES 23
#define DVC_FIRSTCSR 24
#define DVC_DEFX 25
#define DVC_DEFY 26
#define DVC_DEFZ 27

/* cursor info indices */
#define CSR_NAME 1
#define CSR_ACTIVE 2
#define CSR_PKTDATA 3
#define CSR_BUTTONS 4
#define CSR_BUTTONBITS 5
#define CSR_BTNNAMES 6
#define CSR_BUTTONMAP 7
#define CSR_SYSBTNMAP 8
#define CSR_NPBUTTON 9
#define CSR_TPBUTTON 10
#define CSR_NPBTNMARKS 11
#define CSR_NPBREPEAT 12
#define CSR_NPBTNACTION 13
#define CSR_PHYSID 14
#define CSR_TYPE 15
#define CSR_CAPABILITIES 16

static HWND  g_hwnd;
static UINT  g_serial = 1;
static BOOL  g_open;
static int   g_screenW = 1352, g_screenH = 878;   /* primary screen (fallback) */
static int   g_virtW = 1352, g_virtH = 878;       /* full virtual desktop (all displays) */
static int   g_virtX, g_virtY;                    /* ...and where it starts: negative when a
                                                     screen sits left of / above the main one.
                                                     All four are re-read about once a second
                                                     (refresh_virtual), never only at load:
                                                     that was issue #37. Guarded by g_cs. */
static int   g_newscreen;   /* pen reached a screen SAI's tablet area does not cover (#37) */
static int   g_follow_cursor;   /* self-check failed: following Wine's cursor instead */
static OURPKT g_last;          /* most recent packet */
static UINT   g_last_serial;
#define RING_SZ 256            /* recent packets by serial, so WTPacket can return
                                  the one SAI asked for — returning only the latest
                                  dropped intermediate points when SAI processed
                                  message bursts, causing straight "boxy" segments */
static OURPKT g_ring[RING_SZ];
static UINT   g_ring_serial[RING_SZ];
static LOGCONTEXTW g_ctx;      /* the context the app asked for in WTOpenW/WTSetW */
static BOOL  g_have_ctx;
/* Polling mode: apps such as FireAlpaca never fetch via WTPacket; they poll
 * WTPacketsGet. In that mode every sample must go into the ring (no conflation
 * tied to g_fetched), and delivery has its own cursor. */
static volatile int g_polling;
static UINT g_deliver_serial;
static int g_host_sai;
static int g_post_window = 96;
/* The mask the app originally asked for. WTOpenW still returns OUR_PKTDATA to
 * SAI, but polling apps (FireAlpaca) interpret the buffer with their own mask;
 * WTPacketsGet builds that layout. */
static WTPKT g_app_pktdata;
static CRITICAL_SECTION g_cs;
static FILE  *g_log;
static volatile DWORD g_last_tip_tick;   /* GetTickCount of the last pen-tip
                                            transition; opens the click de-dup
                                            window (see CLICK DE-DUP below) */

static void log_line(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

/* Canvas timelapse recorder. Included here (not compiled separately) so the
 * build command stays a single translation unit — see the note at the top of
 * timelapse_win.h. Must come after log_line(), which it calls. Entirely inert
 * unless WT_TIMELAPSE is set. */
#include "timelapse_win.h"

static void fill_default_context(LOGCONTEXTW *lc) {
    memset(lc, 0, sizeof(*lc));
    static const WCHAR nm[] = L"OurDefault";
    memcpy(lc->lcName, nm, sizeof(nm));
    /* CXO_MESSAGES only. KNOWN QUIRK: while the pen is in range, SAI suppresses
     * the pen's synthesized mouse clicks, so the TOP MENU ROW ("File", ...)
     * ignores pen taps (canvas/brush panels are fine — they run on the WinTab
     * packets). Use the mouse/trackpad for the menu.
     * Tried: advertising CXO_SYSTEM (0x0001) like a real Wacom driver makes SAI
     * accept the mouse click, but SAI then processes BOTH the mouse click AND
     * our buttons=1 packet -> menus open and instantly close (double-click).
     * Real drivers dodge this because Windows tags pen-synthesized mouse events
     * (GetMessageExtraInfo signature) so apps can de-dup; Wine's mouse events
     * carry no tag. No DLL-side fix without risking canvas double-strokes. */
    lc->lcOptions  = 0x0004;
    lc->lcMsgBase  = WT_DEFBASE;
    lc->lcPktRate  = 133;
    lc->lcPktData  = OUR_PKTDATA;
    lc->lcMoveMask = OUR_PKTDATA;
    lc->lcInExtX = IN_EXT; lc->lcInExtY = IN_EXT;
    /* output/system extent spans the full VIRTUAL desktop (all monitors) with
     * its true aspect ratio (x8 for sub-pixel resolution). The helper reports
     * the pen position within this same combined space, so a 2nd monitor maps
     * correctly instead of producing a doubled cursor. A square extent stretched
     * drawings; a single-screen extent broke the 2nd display. Single screen:
     * virtual == primary, so this is identical to before. */
    /* The system area starts where the desktop really starts. A real Wacom
     * driver reports the virtual desktop's origin here; before #37 we always
     * said 0,0, which is wrong as soon as a screen sits left of or above the
     * main one (the desktop then starts at a negative coordinate). */
    EnterCriticalSection(&g_cs);
    int vx = g_virtX, vy = g_virtY, vw = g_virtW, vh = g_virtH;
    LeaveCriticalSection(&g_cs);
    lc->lcOutExtX = vw * 8; lc->lcOutExtY = vh * 8;
    lc->lcSysOrgX = vx; lc->lcSysOrgY = vy;
    lc->lcSysExtX = vw; lc->lcSysExtY = vh;
    lc->lcSensX = lc->lcSensY = lc->lcSensZ = 65536;
}

/* latest sample from the mac helper. Format: "p x y w h" (pressure, pen pos in
 * macOS coords: origin bottom-left y-up, and mac screen size in points) or a
 * bare "p" (no position -> caller falls back to the Wine cursor). A failed
 * open/parse (file caught mid-rewrite) returns the LAST good sample — treating
 * torn reads as pen-up caused stroke gaps SAI bridged with straight segments. */
typedef WTC_SAMPLE SAMPLE;   /* defined (and parsed) in wintab_core.h */
static SAMPLE g_sample;   /* last good sample */

static SAMPLE read_sample(void) {
    FILE *f = fopen("C:\\wt_pressure.txt", "rb");
    if (!f) return g_sample;
    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    (void)wtc_parse_sample(buf, &g_sample);   /* failed parse keeps the last good sample */
    return g_sample;
}

static int read_pressure(void) { return read_sample().press; }

static int   g_was_down;
static DWORD g_posted;
static UINT  g_fetched;    /* last serial SAI actually pulled via WTPacket */
static unsigned long g_fetch_count;   /* total WTPacket fetches = points SAI actually drew */
static int   g_dirty;      /* g_last holds a fresher sample than we've posted */
#define POST_WINDOW 96    /* max unfetched packets before we conflate. 1 = min lag but
                              boxy fast curves (drops in-between points); higher = smoother
                              curves, more lag. Measuring SAI's draw rate to tune this. */
static DWORD g_ring_time[RING_SZ];  /* GetTickCount when each serial was posted (latency probe) */
/* g_posted is counted from two threads once the hold posts from SAI's own */
static void bump_posted(void) { InterlockedIncrement((LONG volatile *)&g_posted); }

/* ---- SCREENS (issue #37) ----------------------------------------------------
 * Re-read the Windows virtual desktop. Wine updates it the moment macOS
 * reports a display change; reading it only once at load is what left the DLL
 * describing a mirrored 1920x1080 desktop after the user switched to extended.
 * Cheap enough to call from the hot path; it throttles itself to once a second.
 *
 * When it changes we also tell SAI, the way a WinTab driver does: WT_INFOCHANGE
 * (the device/desktop info changed) and WT_CTXUPDATE for its context. If SAI
 * responds by reading its context again (WTGetW), it learns the new area and
 * the new screen gets pressure without a restart. If it does not, nothing is
 * lost: wtc_place() keeps placing the pen inside the area SAI still believes
 * in, and steps aside on screens outside it. Which of the two SAI does is in
 * the log ("SAI re-read its context"). WT_NO_CTX_NOTIFY=1 turns the messages
 * off, should SAI ever react badly to them. */
#define WT_CTXUPDATE  (WT_DEFBASE+3)
#define WT_CTXOVERLAP (WT_DEFBASE+WTC_CTXOVERLAP_OFFSET)
#define WT_INFOCHANGE (WT_DEFBASE+6)
#define CXS_ONTOP     0x0004
static int g_notify_ctx = 1;
/* the window SAI gave WTOpenW is its WinTab control window, the only place
 * SAI re-reads its tablet area (see wtc_sai_rereads_on_overlap) */
static int g_ctx_rereads;
/* self-check: the tolerance and sample count live in wintab_core.h
 * (WTC_SELFCHECK_TOL / _NEED) next to the decision, so the tests use the same
 * numbers. WT_NO_SELFCHECK=1 turns it off. */
#define SELFCHECK_TOL  WTC_SELFCHECK_TOL
#define SELFCHECK_NEED WTC_SELFCHECK_NEED
static int g_selfcheck = 1;
/* WT_LOG_POS=1 (with WT_DEBUG): log where EVERY positioned sample goes, inside
 * SAI's area or not. For tools/multimon/harness.py, which checks placement
 * with hover-only samples so nothing ever clicks in SAI. Off by default: one
 * line per sample is far too much for normal debugging. */
static int g_log_pos;
/* Stroke-start trace (wtc_st_* in wintab_core.h): one "START ..." line per
 * stroke in the debug log. On whenever the debug log is; WT_NO_START_TRACE=1
 * turns it off. Touched from the producer thread and SAI's UI thread, always
 * under g_cs. */
static int g_trace_start;
static WTC_STARTTRACE g_st;
static WTC_MIDSTAT g_mid;          /* the rest of the stroke, for comparison */
/* The app's times for the sample being emitted (see WTC_TIMES). Set by the
 * producer just before emit_sample() and read only inside it, on that same
 * thread. has = 0 for the file fallback, which carries no times. */
static WTC_TIMES g_dg_tm;

/* A pen-down that got no click (wtc_nc_* in wintab_core.h): what SAI's UI
 * thread received meanwhile. All under g_cs, like the trace. */
static WTC_NOCLICK g_nc;
static int   g_mm_have;            /* a WM_MOUSEMOVE has been seen at all */
static DWORD g_mm_tick;            /* ...and when the newest one was taken */
static HWND  g_last_click_hwnd;    /* where the last real click into SAI went */

/* One "NOCLICK ..." line. Runs on the producer thread, so it only uses calls
 * that SEND NOTHING to SAI's UI thread: WindowFromPoint() asks every window it
 * crosses (WM_NCHITTEST), and a UI thread that is busy would then hold up the
 * pen from here. ChildWindowFromPointEx() is plain geometry. */
static void log_noclick(const WTC_NOCLICK *tally, long move_age, int32_t sx, int32_t sy,
                        unsigned waited_ms, HWND last_click) {
    char tl[160], cls[64] = "?";
    HWND root = g_hwnd ? GetAncestor(g_hwnd, GA_ROOT) : NULL;
    HWND child = NULL, fg = GetForegroundWindow();
    RECT wr = {0, 0, 0, 0};
    POINT pt = { sx, sy }, cur = {0, 0};
    int inside = 0;
    GUITHREADINFO gti;
    memset(&gti, 0, sizeof gti);
    gti.cbSize = sizeof gti;
    if (root) {
        POINT c = pt;
        inside = GetWindowRect(root, &wr) && PtInRect(&wr, pt);
        ScreenToClient(root, &c);
        child = ChildWindowFromPointEx(root, c, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
        if (child) GetClassNameA(child, cls, (int)sizeof cls);
        GetGUIThreadInfo(GetWindowThreadProcessId(root, NULL), &gti);
    }
    GetCursorPos(&cur);
    wtc_nc_format(tally, move_age, tl, sizeof tl);
    log_line("NOCLICK pen-down at (%d,%d) got no click in %ums: %s | under pen: child=%p class=%s "
             "inside-sai-window=%d main=%p last-click-went-to=%p | fg=%p (sai=%d) active=%p capture=%p "
             "| cursor=(%ld,%ld) button=%s",
             (int)sx, (int)sy, waited_ms, tl, (void *)child, cls, inside, (void *)root, (void *)last_click,
             (void *)fg, (fg && root && GetAncestor(fg, GA_ROOT) == root) ? 1 : 0,
             (void *)gti.hwndActive, (void *)gti.hwndCapture,
             (long)cur.x, (long)cur.y, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? "down" : "up");
}

/* Write out the stroke-start trace if it is due (or `force`). The line is built
 * under the lock and written outside it, so SAI's thread never waits on disk. */
static void st_report(DWORD now, int force) {
    char line[2048];
    int have = 0, lost = 0;
    WTC_NOCLICK tally;
    long move_age = -1;
    int32_t sx = 0, sy = 0;
    unsigned waited = 0;
    HWND last_click = NULL;
    if (!g_trace_start) return;
    EnterCriticalSection(&g_cs);
    if (g_st.active && (force || wtc_st_due(&g_st, now))) {
        /* decided BEFORE formatting, which closes the trace */
        lost = wtc_nc_lost(g_st.clicked, &g_nc);
        tally = g_nc;
        if (g_mm_have) move_age = (long)(DWORD)(now - g_mm_tick);
        sx = g_st.pk[0].sx; sy = g_st.pk[0].sy;
        waited = (unsigned)(DWORD)(now - g_st.t0);
        last_click = g_last_click_hwnd;
        wtc_st_format(&g_st, line, sizeof line);
        have = 1;
    }
    LeaveCriticalSection(&g_cs);
    if (have) log_line("%s", line);
    if (have && lost) log_noclick(&tally, move_age, sx, sy, waited, last_click);
}

/* ---- HOLD THE FIRST PEN-DOWN PACKETS UNTIL THE CLICK ------------------------
 * The rules and the reasons are in wintab_core.h (wtc_hold_*). Here: the state,
 * all under g_cs because the producer thread fills the line and SAI's UI thread
 * (the message hook) empties it; and the one place that posts held packets.
 * The hold is always on; g_hold_on stays as a name so the hook's users read
 * the same as before. */
static HHOOK g_click_hook;         /* defined with the hook, further down */
static WTC_HOLD g_hold;
static int g_hold_on = 1;   /* SAI: on; polling apps: off */
static int   g_ld_have;            /* SAI has taken a left click at all */
static DWORD g_ld_tick;            /* ...and when the newest one */
static unsigned long g_hold_by[3]; /* holds ended by: 0 time limit, 1 click, 2 pen lifted */

/* Post whatever is held, oldest first. Safe from either thread and safe to
 * call when nothing is held. `only_if_expired`: do nothing unless the time
 * limit has passed. */
static void hold_release(int why, int only_if_expired) {
    uint32_t ser[WTC_HOLD_CAP];
    int n = 0, was = 0;
    DWORD waited = 0, now = GetTickCount();
    EnterCriticalSection(&g_cs);
    if (g_hold.active && (!only_if_expired || wtc_hold_expired(&g_hold, now))) {
        was = 1;
        waited = now - g_hold.t0;
        n = wtc_hold_release(&g_hold, ser);
        g_hold_by[why]++;
    }
    LeaveCriticalSection(&g_cs);
    if (!was || !g_hwnd) return;
    for (int i = 0; i < n; i++) {
        PostMessageW(g_hwnd, WT_PACKET, ser[i], (LPARAM)0xC0FFEE01);
        bump_posted();
    }
    /* Only the time limit is worth a line: it means no click came, which is
     * the case the hold cannot help. (Never log from the hook thread.) */
    if (why == 0) log_line("HOLD: no click in %lums, posted %d held packet(s) as they were",
                           (unsigned long)waited, n);
}

/* The hold is always on (wintab_core.h says why there is no switch). This
 * only says so in the log, once, so a log shows which DLL it came from. */
static void check_hold(DWORD now) {
    static int said;
    (void)now;
    if (said) return;
    said = 1;
    log_line("HOLD: ON (first pen-down packets wait for SAI to take the click, at most %dms)",
             WTC_HOLD_MAX_MS);
}

static void read_virtual(int *x, int *y, int *w, int *h) {
    *x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    *y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    *w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (*w <= 0 || *h <= 0) {          /* no multi-monitor metrics: primary only */
        *x = 0; *y = 0;
        *w = GetSystemMetrics(SM_CXSCREEN);
        *h = GetSystemMetrics(SM_CYSCREEN);
    }
}

static void refresh_virtual(void) {
    static DWORD last;
    DWORD now = GetTickCount();
    if (last && now - last < 1000) return;
    last = now;
    int x, y, w, h;
    read_virtual(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    EnterCriticalSection(&g_cs);
    int changed = x != g_virtX || y != g_virtY || w != g_virtW || h != g_virtH;
    int ox = g_virtX, oy = g_virtY, ow = g_virtW, oh = g_virtH;
    if (changed) { g_virtX = x; g_virtY = y; g_virtW = w; g_virtH = h; }
    LONG sx = g_ctx.lcSysOrgX, sy = g_ctx.lcSysOrgY, sw = g_ctx.lcSysExtX, sh = g_ctx.lcSysExtY;
    BOOL have = g_have_ctx;
    LeaveCriticalSection(&g_cs);
    if (!changed) return;
    log_line("SCREENS changed: desktop (%d,%d %dx%d) -> (%d,%d %dx%d); SAI's tablet area %s(%ld,%ld %ldx%ld)",
             ox, oy, ow, oh, x, y, w, h, have ? "" : "not opened yet ", sx, sy, sw, sh);
    /* a fresh layout: whether the pen can reach every screen is re-learned */
    g_newscreen = 0;
    if (g_notify_ctx && g_open && g_hwnd) {
        PostMessageW(g_hwnd, WT_INFOCHANGE, 0, 0);
        PostMessageW(g_hwnd, WT_CTXUPDATE, (WPARAM)0xC0FFEE01, 0);
        log_line("SCREENS: told SAI (WT_INFOCHANGE + WT_CTXUPDATE)");
        if (g_ctx_rereads) {
            PostMessageW(g_hwnd, WT_CTXOVERLAP, (WPARAM)0xC0FFEE01, CXS_ONTOP);
            log_line("SCREENS: asked SAI's WinTab window to re-read its area (WT_CTXOVERLAP)");
        } else {
            log_line("SCREENS: SAI's context is on its main window, which never re-reads; "
                     "a new screen needs a restart for pressure");
        }
    }
}

/* build a WinTab packet from one sample and post it to SAI. Streams
 * CONTINUOUSLY, exactly like a real Wacom driver: pen-down samples carry
 * buttons=1 + pressure, HOVER samples (pen up, in range) carry buttons=0 +
 * pressure 0 but STILL carry position. SAI positions its brush cursor from
 * these packets, so dropping hover samples froze the cursor at the last drawn
 * point until Wine's mouse path caught up — now the cursor tracks the pen
 * continuously. The pen-down -> up transition is naturally the first
 * buttons=0 packet, which ends the stroke. */
static void emit_sample(const SAMPLE *s) {
    if (!g_open || !g_hwnd) return;

    int down = s->press > 0;
    /* nothing to do: pen up, no position, and not ending a stroke
     * (e.g. the bare "0" file kill switch while already idle) */
    if (!down && !s->has_pos && !g_was_down) return;

    /* start from the last packet so a position-less pen-up ends the stroke
     * at the last drawn point rather than jumping */
    EnterCriticalSection(&g_cs);
    OURPKT pk = g_last;
    LeaveCriticalSection(&g_cs);
    pk.status = 0;
    pk.cursor = 0;
    pk.buttons  = down ? 1 : 0;
    /* the pen keeps the index it always had (0); the eraser end reports
     * cursor 2, which is what WinTab apps switch their eraser tool on */
    pk.cursor   = g_eraser ? 2 : 0;
    pk.pressure = down ? (UINT)s->press : 0;
    g_last_press = down ? (int)pk.pressure : 0;
    if (g_last_press > g_ui_press_max) g_ui_press_max = g_last_press;
    /* screen position of this sample, for the stroke-start trace only */
    static int32_t trX, trY;

    /* map position into the OUTPUT coordinate space of the context SAI
     * opened (WinTab packets are in lcOut coords). WinTab convention:
     * positive lcOutExtY means Y grows upward. Helper coords are mac
     * bottom-left y-up (already the WinTab direction) and are fixed-point
     * (x,y,w,h uniformly scaled) — map DIRECTLY into out space in one
     * 64-bit step, no intermediate screen-pixel quantization. */
    if (s->has_pos) {
        LONG oX = 0, oY = 0, eX = IN_EXT, eY = IN_EXT;
        refresh_virtual();
        EnterCriticalSection(&g_cs);
        WTC_RECT virt = { g_virtX, g_virtY, g_virtW, g_virtH };
        WTC_RECT sys = virt;            /* no context yet: assume it covers everything */
        if (g_have_ctx) {
            oX = g_ctx.lcOutOrgX; eX = g_ctx.lcOutExtX;
            oY = g_ctx.lcOutOrgY; eY = g_ctx.lcOutExtY;
            sys.x = g_ctx.lcSysOrgX; sys.y = g_ctx.lcSysOrgY;
            sys.w = g_ctx.lcSysExtX; sys.h = g_ctx.lcSysExtY;
        }
        LeaveCriticalSection(&g_cs);

        /* SELF-CHECK (see wtc_selfcheck_step). Only while hovering, and only
         * while Wine's cursor is over SAI's own window: elsewhere Wine is not
         * told where the cursor is, and a stale cursor would look like a fault
         * of ours. That guard is NOT enough on its own: a cursor frozen when
         * the pen left for another screen is still "over SAI's window".
         * wtc_selfcheck_step() leaves those samples out; keep the comparison
         * going through it, not through wtc_cursor_streak() directly. */
        SAMPLE use = *s;
        if (!down && g_selfcheck) {
            static WTC_SELFCHECK sc;
            POINT cur; RECT wr;
            if (GetCursorPos(&cur) && GetWindowRect(g_hwnd, &wr) && PtInRect(&wr, cur)) {
                int32_t ex, ey;
                wtc_sample_to_screen(s, virt, &ex, &ey);
                int streak = wtc_selfcheck_step(&sc, ex, ey, cur.x, cur.y, SELFCHECK_TOL);
                if (!g_follow_cursor && streak >= SELFCHECK_NEED) {
                    g_follow_cursor = 1;
                    log_line("SELFCHECK: pen at (%d,%d) but Wine's cursor at (%ld,%ld) for %d samples; "
                             "following the cursor", ex, ey, (long)cur.x, (long)cur.y, streak);
                } else if (g_follow_cursor && streak == 0) {
                    g_follow_cursor = 0;
                    log_line("SELFCHECK: pen and cursor agree again at (%d,%d)", ex, ey);
                }
            }
        }
        if (g_follow_cursor) {
            POINT cur;
            if (GetCursorPos(&cur)) use = wtc_screen_to_sample(s, virt, cur.x, cur.y);
        }

        if (g_trace_start) wtc_sample_to_screen(&use, virt, &trX, &trY);
        int32_t px, py;
        int inside = wtc_place(&use, virt, sys, oX, oY, eX, eY, IN_EXT, &px, &py);
        if (g_log_pos) {
            int32_t gx, gy;
            wtc_sample_to_screen(&use, virt, &gx, &gy);
            log_line("POS screen=(%d,%d) inside=%d out=(%ld,%ld) follow=%d",
                     gx, gy, inside, (long)px, (long)py, g_follow_cursor);
        }
        if (!inside) {
            /* A screen SAI's tablet area does not cover: any coordinate we
             * posted would put ink somewhere else. Step aside instead. End a
             * stroke in progress where it last was, then stay silent, so SAI
             * takes the pen as an ordinary mouse there (right place, no
             * pressure). The app reads newscreen=1 and suggests a restart. */
            if (!g_newscreen)
                log_line("SCREENS: pen on a screen outside SAI's tablet area (%d,%d %dx%d); "
                         "stepping aside so it works as a mouse there", sys.x, sys.y, sys.w, sys.h);
            g_newscreen = 1;
            if (g_was_down) {
                pk.buttons = 0; pk.pressure = 0;      /* pk.x/y still hold the last point */
                down = 0;
            } else {
                return;
            }
        } else {
            pk.x = px; pk.y = py;
        }
    }
    /* else: keep pk.x/pk.y from g_last (position-less pen-up) */

    int transition = (down != g_was_down);
    if (transition) {         /* log tip transitions to investigate stray clicks */
        g_last_tip_tick = GetTickCount();   /* opens the click de-dup window */
        log_line("PEN %s buttons=%u pk=(%ld,%ld) press=%u t=%lu",
             down ? "DOWN" : "UP", pk.buttons, (long)pk.x, (long)pk.y, pk.pressure, GetTickCount());
    }
    g_was_down = down;

    /* CONFLATION: always keep g_last fresh, but only PostMessage a new WT_PACKET
     * when SAI isn't already behind (or on a tip transition, which must never be
     * dropped). Posting a packet per sample flooded SAI faster than it drains
     * its message queue, so the drawn point trailed the cursor during fast
     * motion (worse the faster you move). Now we self-pace to SAI's consumption:
     * intermediate samples collapse into g_last, so SAI always gets a CURRENT
     * point and the trail stays tight. Skipped points are on-path; SAI
     * interpolates. A pending fresh sample is flushed by flush_pending(). */
    UINT ser = 0; BOOL post;
    int held = 0;
    /* HOLD (wtc_hold_* in wintab_core.h). A lift ends it at once: the held
     * packets go out first, then the pen-up below, in that order. Otherwise
     * only the time limit is checked here. */
    if (!down) hold_release(2, 0);
    else       hold_release(0, 1);
    EnterCriticalSection(&g_cs);
    g_last = pk;
    if (transition && down &&
        wtc_hold_should_start(g_hold_on, g_click_hook != NULL, g_ld_have, GetTickCount(), g_ld_tick))
        wtc_hold_start(&g_hold, GetTickCount());
    if (down && g_hold.active) {
        /* Gets its serial and its place in the ring now, so WTPacket can serve
         * it and the trace knows it; only the PostMessage waits. */
        uint32_t reuse = 0;
        held = 1;
        post = TRUE;                    /* it WILL be posted: not "merged" for the trace */
        if (wtc_hold_add(&g_hold, g_serial, &reuse)) ser = g_serial++;
        else ser = reuse;               /* line full: replaces the newest held one */
        g_last_serial = ser;
        g_ring_serial[ser % RING_SZ] = ser;
        g_ring[ser % RING_SZ] = pk;
        g_ring_time[ser % RING_SZ] = GetTickCount();
        g_dirty = 0;
    } else if (g_polling || (post = wtc_should_post(g_serial, g_fetched, transition, g_post_window)) != 0) {
        if (g_polling) post = TRUE;   /* polling apps consume the ring, not messages */
        ser = g_serial++;
        g_last_serial = ser;
        g_ring_serial[ser % RING_SZ] = ser;
        g_ring[ser % RING_SZ] = pk;
        g_ring_time[ser % RING_SZ] = GetTickCount();
        g_dirty = 0;
    } else {
        g_dirty = 1;          /* freshest not yet delivered; flush when SAI catches up */
    }
    /* stroke-start trace: recorded BEFORE the PostMessage below, so SAI cannot
     * fetch this serial before the trace knows it exists */
    int st_prev = 0;
    char mid_line[256]; mid_line[0] = '\0';
    if (g_trace_start) {
        DWORD t = GetTickCount();
        if (transition && down) {
            st_prev = g_st.active;          /* the last stroke's line is still open */
            if (!st_prev) {
                wtc_st_down_t(&g_st, t, post, ser, (int)pk.pressure, trX, trY, &g_dg_tm);
                memset(&g_nc, 0, sizeof g_nc);      /* count mouse messages from this pen-down */
            }
            wtc_mid_down(&g_mid, t);
        } else {
            wtc_st_sample_t(&g_st, t, down, post, ser, (int)pk.pressure, trX, trY, &g_dg_tm);
            if (down) wtc_mid_sample(&g_mid, t, trX, trY, &g_dg_tm);
            else if (transition) wtc_mid_format(&g_mid, mid_line, sizeof mid_line);   /* pen-up */
        }
    }
    LeaveCriticalSection(&g_cs);
    if (mid_line[0]) log_line("%s", mid_line);
    if (st_prev) {
        /* A new stroke inside the last one's report window: write the old line
         * out first, then start the new trace. Done in two steps because the
         * write happens outside the lock. */
        st_report(GetTickCount(), 1);
        EnterCriticalSection(&g_cs);
        wtc_st_down_t(&g_st, GetTickCount(), post, ser, (int)pk.pressure, trX, trY, &g_dg_tm);
        memset(&g_nc, 0, sizeof g_nc);
        LeaveCriticalSection(&g_cs);
    }

    if (post && !held) { PostMessageW(g_hwnd, WT_PACKET, ser, (LPARAM)0xC0FFEE01); bump_posted(); }
    st_report(GetTickCount(), 0);
}

/* deliver the freshest pending packet once SAI has caught up — covers the case
 * where samples stopped (pen still/lifted) while we were conflating, so the
 * final position isn't left undelivered. */
static void flush_pending(void) {
    if (!g_open || !g_hwnd) return;
    UINT ser = 0; BOOL post = FALSE;
    EnterCriticalSection(&g_cs);
    if (g_dirty && wtc_should_post(g_serial, g_fetched, 0, g_post_window)) {
        ser = g_serial++;
        g_last_serial = ser;
        g_ring_serial[ser % RING_SZ] = ser;
        g_ring[ser % RING_SZ] = g_last;
        g_ring_time[ser % RING_SZ] = GetTickCount();
        g_dirty = 0;
        post = TRUE;
    }
    LeaveCriticalSection(&g_cs);
    if (post) { PostMessageW(g_hwnd, WT_PACKET, ser, (LPARAM)0xC0FFEE01); bump_posted(); }
}

/* ---- WIN32 WAKE ------------------------------------------------------------
 * After an app switch Wine sometimes fails to restore the Win32 foreground /
 * active window, even though macOS shows SAI as the active app. SAI's own
 * WM_MOUSEACTIVATE handler returns MA_NOACTIVATEANDEAT, so in that state every
 * click on the canvas is swallowed as an "activating click" and the canvas
 * looks dead to BOTH pen and mouse — while the menu bar (a different code path)
 * still responds. macOS-side re-activation can't fix that; but this DLL lives
 * INSIDE SAI's process, so it can restore the Win32 state directly.
 * The mac helper bumps C:\wt_wake.txt to ask for it. Disable: WT_NO_WIN32_WAKE.
 *
 * SetActiveWindow/SetFocus are THREAD-LOCAL: they must run on SAI's UI thread,
 * not our producer thread (from here they'd target our message-less thread and
 * earlier attempts disturbed SAI's input). We marshal onto the UI thread with a
 * thread-scoped WH_GETMESSAGE hook: the hook proc is our own code but Windows
 * runs it on the HOOKED thread whenever that thread pulls a message — SAI's UI
 * thread keeps pumping while stuck (the menu row still works), so posting any
 * message guarantees a prompt tick. Fallback: AttachThreadInput, which shares
 * the input state so the calls apply from this thread. */
typedef struct { HWND h; LONG area; } BESTWND;

static volatile HWND  g_wake_target;   /* window the hook should activate */
static volatile LONG  g_wake_done;
static HHOOK          g_wake_hook;
/* Loaded by the Test pen probe rather than by SAI. Set once at load from the
 * exe's own name (wtc_host_is_probe). The probe must never run the wake: it
 * would take the Windows foreground away from SAI. */
static int            g_host_is_probe;

/* Wake diagnostics are ALWAYS logged (separate file, append) — wakes are rare
 * (user-triggered) so there's no per-packet cost, and field reports without
 * WT_DEBUG were blind exactly when we needed data. See issue #2. */
static void wake_log(const char *fmt, ...) {
    FILE *f = fopen("C:\\wt_wakelog.txt", "a");
    if (!f) return;
    fprintf(f, "[%lu] ", (unsigned long)GetTickCount());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* runs ON SAI'S UI THREAD (thread-scoped hook) — the only place the two broken
 * layers can BOTH be repaired (verified live against a stuck SAI, issue #2):
 *
 * 1. WINESERVER FOREGROUND can be parked on another process's window (seen:
 *    the desktop window, class #32769). Wine enforces the Windows foreground
 *    lock: SetForegroundWindow from a non-foreground process fails with
 *    ERROR_ACCESS_DENIED (5). Classic bypass, confirmed working under this
 *    Wine: AttachThreadInput to the CURRENT foreground owner's thread, which
 *    makes our input state count as foreground, then retry.
 *
 * 2. THE COCOA KEY WINDOW: winemac.drv only makes the Mac window key on a
 *    focus CHANGE. After the bug the Win32 state can already read "correct"
 *    (focus==w) so a plain SetFocus(w) no-ops at win32u level and the driver
 *    is never called — the Mac window stays non-key and every click becomes a
 *    swallowed "activating click". Toggling focus through NULL forces the
 *    driver call. Driver entry points run in the CALLING process, so this is
 *    only effective from inside SAI — exactly here. */
static LRESULT CALLBACK wake_hook_proc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && g_wake_target && !g_wake_done) {
        HWND w = g_wake_target;
        InterlockedExchange(&g_wake_done, 1);   /* once per request */
        HWND fgBefore = GetForegroundWindow();
        BOOL bypass = FALSE;
        BringWindowToTop(w);
        SetLastError(0);
        BOOL ok = SetForegroundWindow(w);
        DWORD err = GetLastError();
        if (!ok || GetForegroundWindow() != w) {
            DWORD fgTid = fgBefore ? GetWindowThreadProcessId(fgBefore, NULL) : 0;
            if (fgTid && fgTid != GetCurrentThreadId() &&
                AttachThreadInput(GetCurrentThreadId(), fgTid, TRUE)) {
                bypass = TRUE;
                SetLastError(0);
                ok = SetForegroundWindow(w);
                err = GetLastError();
                AttachThreadInput(GetCurrentThreadId(), fgTid, FALSE);
            }
        }
        SetActiveWindow(w);
        SetFocus(NULL);       /* force the focus change through the driver */
        SetFocus(w);
        /* 3rd broken layer (the one that finally matched the field logs): SAI
         * keeps its OWN "am I active" flag, set only by activation MESSAGES.
         * Wine can restore the activation state without a state CHANGE, so no
         * message is ever generated and SAI's flag stays stale — canvas clicks
         * keep getting eaten even with fg/active/focus all correct (observed
         * live: 6 perfectly healthy wakes on a still-stuck SAI). Synthesize the
         * sequence a real activation would deliver; we're ON the UI thread, so
         * these are direct window-proc calls. */
        SendMessageW(w, WM_NCACTIVATE, TRUE, 0);
        SendMessageW(w, WM_ACTIVATEAPP, TRUE, 0);
        SendMessageW(w, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        wake_log("hook: target=%p fgBefore=%p sfw=%d err=%lu bypass=%d "
                 "fg=%p active=%p focus=%p (activation msgs sent)",
                 w, fgBefore, ok, (unsigned long)err, bypass,
                 GetForegroundWindow(), GetActiveWindow(), GetFocus());
    }
    return CallNextHookEx(g_wake_hook, code, wp, lp);
}

static BOOL CALLBACK find_main_window(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    BESTWND *b = (BESTWND *)lp;
    if (area > b->area) { b->area = area; b->h = h; }
    return TRUE;
}

/* marshal one wake onto the UI thread; returns 1 if the hook ran */
static int run_wake_hook(HWND w, DWORD tid) {
    g_wake_target = w;
    InterlockedExchange(&g_wake_done, 0);
    g_wake_hook = SetWindowsHookExW(WH_GETMESSAGE, wake_hook_proc, NULL, tid);
    if (!g_wake_hook) {
        wake_log("win32_wake: SetWindowsHookEx failed (%lu)", (unsigned long)GetLastError());
        g_wake_target = NULL;
        return 0;
    }
    /* poke the UI thread so the hook fires now; WM_NULL is a no-op to SAI */
    PostMessageW(w, WM_NULL, 0, 0);
    for (int i = 0; i < 40 && !g_wake_done; i++) Sleep(5);   /* <=200 ms */
    UnhookWindowsHookEx(g_wake_hook);
    g_wake_hook = NULL;
    g_wake_target = NULL;
    if (!g_wake_done) wake_log("win32_wake: hook never ticked");
    return g_wake_done != 0;
}

static void win32_wake(void) {
    /* Second guard (the first is wtc_wake_requested): in the probe this would
     * find no visible window, fall back to the probe's hidden one, and take
     * the foreground from SAI for it. See wtc_host_is_probe in wintab_core.h. */
    if (g_host_is_probe) return;
    BESTWND best = { NULL, 0 };
    EnumWindows(find_main_window, (LPARAM)&best);
    HWND w = best.h ? best.h : (g_hwnd ? GetAncestor(g_hwnd, GA_ROOT) : NULL);
    if (!w) { wake_log("win32_wake: no window found"); return; }

    DWORD tid = GetWindowThreadProcessId(w, NULL);
    wake_log("win32_wake: target=%p uiThread=%lu fg=%p", w, (unsigned long)tid,
             GetForegroundWindow());

    run_wake_hook(w, tid);

    /* VERIFY it took (and stuck): a sibling Wine process — e.g. the one the
     * helper's macOS-side bounce activates — can steal wineserver foreground
     * right back. Identify the thief for the log and retry once, again via
     * the UI thread (a naked call from this thread hits the foreground lock). */
    Sleep(250);
    HWND fgNow = GetForegroundWindow();
    if (fgNow == w) { wake_log("win32_wake: OK, foreground is ours"); return; }
    DWORD thiefPid = 0;
    if (fgNow) GetWindowThreadProcessId(fgNow, &thiefPid);
    wake_log("win32_wake: fg=%p ownerPid=%lu (we are pid %lu) — retrying",
             fgNow, (unsigned long)thiefPid, (unsigned long)GetCurrentProcessId());
    run_wake_hook(w, tid);
    Sleep(150);
    fgNow = GetForegroundWindow();
    wake_log("win32_wake: after retry fg=%p (%s)", fgNow, fgNow == w ? "ok" : "STILL LOST");
}

/* poll the wake marker (cheap, ~6x/sec) */
/* ---- PINCH TO ZOOM (issue #22) --------------------------------------------
 * macOS pinch arrives as a "magnify" gesture, which Wine does not translate
 * into anything SAI understands — so only two-finger scroll zooms, and every
 * Mac reflex to pinch does nothing.
 *
 * We do NOT synthesise macOS events for this: that would need the Accessibility
 * permission, which this project deliberately dropped. Instead the mac helper
 * (which already sees the gesture through the tap it uses for the tablet) writes
 * a running step count to C:\wt_zoom.txt, and we post the wheel message from
 * INSIDE SAI's own process — no permission involved at all.
 *
 * WM_MOUSEWHEEL rather than PageUp/PageDown on purpose: it carries a position,
 * so SAI zooms at the cursor instead of the canvas centre, and it is the path
 * already known to work here (two-finger scroll zooms today).
 * Disable: WT_NO_PINCH_ZOOM=1. */
/* Our injected zoom wheels carry this bit in the low word of wParam, which
 * normally holds MK_* mouse-button/modifier flags. Bits above MK_XBUTTON2
 * (0x0040) are unused, so 0x8000 is free and self-describing: the scroll->pan
 * rewrite below can recognise our own messages without any shared state across
 * threads (PostMessage is asynchronous, so a flag would race). */
#define ZOOM_TAG 0x8000

static void post_zoom(int steps) {
    HWND w = g_hwnd ? GetAncestor(g_hwnd, GA_ROOT) : NULL;
    if (!w) return;
    POINT pt;
    if (!GetCursorPos(&pt)) { pt.x = 0; pt.y = 0; }
    int dir = steps > 0 ? 1 : -1;
    int n = steps > 0 ? steps : -steps;
    if (n > 8) n = 8;                       /* a violent pinch shouldn't fling the zoom */
    for (int i = 0; i < n; i++) {
        PostMessageW(w, WM_MOUSEWHEEL,
                     MAKEWPARAM(ZOOM_TAG, (short)(dir * WHEEL_DELTA)),
                     MAKELPARAM((short)pt.x, (short)pt.y));
    }
    log_line("pinch: posted %d wheel step(s) at (%ld,%ld)", dir * n, (long)pt.x, (long)pt.y);
}

/* Switches read ONCE, at load (DllMain). They used to be getenv() calls on the
 * producer's idle tick and, for the scroll one, on SAI's own thread for every
 * message it took off its queue. */
static int g_no_pinch_zoom, g_no_win32_wake;

/* Pinch as a control datagram ("Z <steps>", wintab_core.h): the normal way. */
static void zoom_datagram(int steps) {
    if (g_no_pinch_zoom) return;
    post_zoom(steps);
}

/* Pinch through wt_zoom.txt: only while the pen port is not ours, when no
 * datagram can reach us. `port_ours` forgets the file's value, so the total
 * that built up while datagrams did the work is not replayed as one big zoom
 * the next time the port is lost. */
static void check_zoom(DWORD now, int port_ours) {
    static DWORD lastCheck;
    static long lastVal;
    static int primed;
    if (g_no_pinch_zoom) return;
    if (!wtc_zoom_file_wanted(port_ours)) { primed = 0; return; }
    if (now - lastCheck < WTC_POLL_ZOOM_FILE_MS) return;   /* responsive: a pinch is a live gesture */
    lastCheck = now;
    FILE *f = fopen("C:\\wt_zoom.txt", "rb");
    if (!f) return;
    char b[32];
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    b[n] = 0;
    long v = atol(b);
    if (!primed) { primed = 1; lastVal = v; return; }   /* ignore the value present at startup */
    if (v != lastVal) {
        long d = v - lastVal;
        lastVal = v;
        post_zoom((int)d);
    }
}

/* The newest wake token acted on or seen, shared by the datagram and the file
 * so one request never wakes twice. Producer thread only. */
static char g_wake_last[32];

/* Wake as a control datagram ("K <token>", wintab_core.h): the normal way. */
static void wake_datagram(const char *token) {
    if (g_no_win32_wake) return;
    if (wtc_wake_datagram(token, g_wake_last, sizeof g_wake_last, g_host_is_probe))
        win32_wake();
}

static void check_win32_wake(DWORD now, int port_ours) {
    static DWORD lastCheck;
    /* ON by default (the wake now runs on SAI's UI thread via the hook, so the
     * earlier wrong-thread hazard is gone). Opt out with WT_NO_WIN32_WAKE=1. */
    if (g_no_win32_wake) return;
    /* The datagram is the fast way; the file is read 150 ms apart only while
     * the port is not ours, else once a second as a safety net. */
    if (now - lastCheck < wtc_wake_file_every(port_ours)) return;
    lastCheck = now;
    FILE *f = fopen("C:\\wt_wake.txt", "rb");
    if (!f) return;
    char b[32];
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    b[n] = 0;
    /* once per new request, never for the one left from before we started,
     * and never in the Test pen probe. lastVal must be at least as big as b. */
    if (wtc_wake_requested(b, g_wake_last, sizeof g_wake_last, g_host_is_probe))
        win32_wake();
}

/* ---- CLICK DE-DUP (pen tap = double click bug) -----------------------------
 * Every physical pen tap reaches SAI TWICE: once as our WinTab packet
 * (buttons=1, which drives the canvas and the tool panels) and once as the
 * mouse click Wine synthesizes from the same tap. Real Windows drivers tag
 * their synthesized mouse events so apps can de-dup; Wine's carry no tag, and
 * SAI's own heuristic misses often enough that a single pen tap on a brush
 * slot could register as TWO clicks and pop the double-click Property dialog
 * (field logs showed one clean DOWN/UP pair per tap — the duplicate is the
 * mouse click, not our stream). So we de-dup for SAI: a permanent
 * WH_GETMESSAGE hook on its UI thread rewrites left-button messages to
 * WM_NULL when they arrive within CLICK_DEDUP_MS of a pen-tip transition.
 * Scope:
 *   - main window only (GA_ROOT match): dialogs are separate windows and may
 *     be mouse-driven — never touched.
 *   - client-area messages only: the menu bar is WM_NCLBUTTONDOWN, so the
 *     issue-#1 menu-strip fix is unaffected.
 *   - only within CLICK_DEDUP_MS of a REAL tip transition from our stream; a
 *     human can't switch pen->mouse that fast, so real mouse clicks pass.
 *
 * OFF BY DEFAULT since v0.1.6 — see issue #19. The "main window only" scoping
 * above is WRONG: the canvas is a child of the main window too, so GA_ROOT
 * matches it just as happily as a brush slot. That made this hook swallow the
 * WM_LBUTTONDOWN that STARTS EVERY STROKE, ~6ms after each pen-down:
 *     PEN DOWN press=218
 *     CLICK dedup: ate msg=0x201 dt=6ms          (0x201 = WM_LBUTTONDOWN)
 * WinTab pressure kept arriving perfectly (fetched=274/274, press up to 1023),
 * so every log looked healthy while the pen simply could not draw — and no
 * amount of reinstalling could fix it, because the bug shipped in this DLL.
 * A double-click dialog is a nuisance; a pen that cannot draw is not, so the
 * default now favours drawing and #8 is reopened.
 * Enable:  WT_CLICK_DEDUP=1   (accepts the #8 double-click risk)
 * Disable: WT_NO_CLICK_DEDUP=1 (still honoured; now redundant) */
#define CLICK_DEDUP_MS WTC_CLICK_DEDUP_MS   /* single source of truth: wintab_core.h */
static HHOOK g_click_hook;
static unsigned long g_clicks_eaten;
/* Resolved once in ensure_click_dedup(). The hook is shared with scroll->pan,
 * which is ON by default, so "is the hook installed" does NOT mean "the user
 * asked for de-dup" — click_hook_proc must consult this, or enabling #24
 * silently re-arms the #19 stroke-eater for everyone. */
static int g_want_dedup;
static int g_want_pan;     /* scroll -> pan was asked for (it is, unless WT_NO_SCROLL_PAN).
                            * Read once at hook install: the hook runs on SAI's
                            * thread for every message SAI takes, and used to
                            * call getenv() there each time. */

/* ---- TWO-FINGER SCROLL PANS INSTEAD OF ZOOMING (issue #24) ----------------
 * winemac.drv turns a two-finger scroll into WM_MOUSEWHEEL, and SAI zooms on
 * the wheel. That is correct Windows behaviour and wrong Mac behaviour: here
 * pinch zooms (#22), so scroll should pan, as it does in every native app.
 *
 * Done inside SAI rather than on the macOS side deliberately. Suppressing the
 * scroll in our CGEventTap would mean the tap could no longer be listen-only,
 * which costs the Accessibility permission this project dropped. From in here
 * we can simply rewrite the message before SAI sees it — no permission at all.
 *
 * SAI has no "pan by N pixels" message, but its own shortcut table binds the
 * arrow keys to Scroll View Up/Down/Left/Right, so we synthesise those.
 * Disable: WT_NO_SCROLL_PAN=1. */
/* Movement per emitted arrow press, in raw wheel units.
 *
 * A first version divided the incoming delta by WHEEL_DELTA (120) to get whole
 * notches. That was wrong twice over: integer division DISCARDED everything
 * below one notch, so a small trackpad gesture moved nothing at all until
 * enough accumulated and then jumped -- which reads as both "jerky" and "needs
 * a huge gesture". A Mac trackpad sends many small deltas precisely so that
 * small movements are possible.
 *
 * So accumulate the raw delta instead and emit one arrow press per PAN_UNIT.
 * Smaller unit = finer and more sensitive; larger = coarser and harder to
 * trigger by accident. 30 proved TOO sensitive in use -- a palm brushing the
 * trackpad moved the canvas -- so the default sits well above that. Tune with
 * WT_PAN_UNIT. */
#define PAN_UNIT_DEFAULT 70
static int pan_unit(void) {
    const char *e = getenv("WT_PAN_UNIT");
    int v = e ? atoi(e) : 0;
    return (v >= 5 && v <= 240) ? v : PAN_UNIT_DEFAULT;
}

/* Leftover delta between messages, so nothing is thrown away and slow gestures
 * still accumulate into movement.
 *
 * The remainder is DISCARDED once scrolling stops, though: otherwise stray
 * single-unit deltas -- a sleeve, a palm, a hand resting on the trackpad --
 * quietly add up over minutes and eventually lurch the canvas with no gesture
 * at all. Carrying a remainder is only meaningful within one continuous
 * gesture. */
#define PAN_GESTURE_GAP_MS 250
static int   g_pan_accum_x, g_pan_accum_y;
static DWORD g_pan_last_tick;

static void post_pan_keys(HWND w, int vk, int n) {
    if (n > 6) n = 6;                                  /* cap a flung scroll */
    for (int i = 0; i < n; i++) {
        PostMessageW(w, WM_KEYDOWN, vk, 1);
        PostMessageW(w, WM_KEYUP,   vk, 1 | (1u << 30) | (1u << 31));
    }
}

/* raw wheel delta in, arrow presses out. dy > 0 scrolls the view up, matching
 * the wheel's own sign convention. */
static void post_pan(HWND w, int rawX, int rawY) {
    const int unit = pan_unit();
    DWORD now = GetTickCount();
    if (now - g_pan_last_tick > PAN_GESTURE_GAP_MS) {   /* new gesture: start clean */
        g_pan_accum_x = g_pan_accum_y = 0;
    }
    g_pan_last_tick = now;
    g_pan_accum_y += rawY;
    g_pan_accum_x += rawX;
    int stepsY = g_pan_accum_y / unit;
    int stepsX = g_pan_accum_x / unit;
    g_pan_accum_y -= stepsY * unit;                    /* keep the remainder */
    g_pan_accum_x -= stepsX * unit;
    if (stepsY) post_pan_keys(w, stepsY > 0 ? VK_UP   : VK_DOWN,  stepsY > 0 ? stepsY : -stepsY);
    /* Horizontal is inverted relative to vertical: WM_MOUSEHWHEEL counts
     * positive to the RIGHT, while WM_MOUSEWHEEL counts positive AWAY from the
     * user. Mapping both the same way made sideways panning mirror the
     * trackpad. Flip with WT_PAN_INVERT_X=1 if a device disagrees. */
    if (stepsX) {
        int leftFirst = getenv("WT_PAN_INVERT_X") ? 1 : 0;
        int vk = (stepsX > 0) == (leftFirst != 0) ? VK_LEFT : VK_RIGHT;
        post_pan_keys(w, vk, stepsX > 0 ? stepsX : -stepsX);
    }
}

static LRESULT CALLBACK click_hook_proc(int code, WPARAM wp, LPARAM lp) {
    MSG *m = (MSG *)lp;
    (void)wp;
    /* HOLD: SAI is taking a left click off its queue right now, on this
     * thread, and has not acted on it yet. Whatever pen-down packets were kept
     * back are posted here, so they are the next thing SAI reads after the
     * click. The click itself is not touched. Any window of SAI's counts: a
     * click in a panel releases too, where waiting would gain nothing. */
    if (code >= 0 && m && wp == PM_REMOVE &&
        (m->message == WM_LBUTTONDOWN || m->message == WM_LBUTTONDBLCLK)) {
        EnterCriticalSection(&g_cs);
        g_ld_have = 1; g_ld_tick = GetTickCount();
        LeaveCriticalSection(&g_cs);
        hold_release(1, 0);
    }
    /* Diagnostic only, nothing is changed: count the mouse messages SAI takes
     * off its queue, for the NOCLICK line (wtc_nc_* in wintab_core.h). Counted
     * once each, when really taken (PM_REMOVE), not when merely peeked at. */
    if (code >= 0 && m && g_trace_start && wp == PM_REMOVE && wtc_nc_is_mouse(m->message)) {
        int same_root = g_hwnd && GetAncestor(m->hwnd, GA_ROOT) == GetAncestor(g_hwnd, GA_ROOT);
        EnterCriticalSection(&g_cs);
        wtc_nc_msg(&g_nc, m->message, same_root);
        if (m->message == WM_MOUSEMOVE) { g_mm_have = 1; g_mm_tick = GetTickCount(); }
        if (same_root && m->message == WM_LBUTTONDOWN) g_last_click_hwnd = m->hwnd;
        LeaveCriticalSection(&g_cs);
    }
    /* scroll -> pan, unless this is one of OUR pinch-zoom wheels */
    if (code >= 0 && m && (m->message == WM_MOUSEWHEEL || m->message == WM_MOUSEHWHEEL) &&
        g_hwnd && g_want_pan &&
        !(LOWORD(m->wParam) & ZOOM_TAG)) {
        int raw = (short)HIWORD(m->wParam);          /* keep sub-notch precision */
        HWND root = GetAncestor(g_hwnd, GA_ROOT);
        if (raw != 0 && root) {
            if (m->message == WM_MOUSEWHEEL) post_pan(root, 0, raw);
            else                             post_pan(root, raw, 0);
        }
        m->message = WM_NULL; m->wParam = 0; m->lParam = 0;   /* SAI must not also zoom */
        return CallNextHookEx(g_click_hook, code, wp, lp);
    }
    if (code >= 0 && m && g_hwnd &&
        (m->message == WM_LBUTTONDOWN || m->message == WM_LBUTTONUP ||
         m->message == WM_LBUTTONDBLCLK)) {
        DWORD dt = GetTickCount() - g_last_tip_tick;
        /* stroke-start trace: where and when SAI takes the click that starts
         * the stroke. Only clicks into SAI's own window, like the de-dup, and
         * only when SAI takes the message off the queue: a PeekMessage that
         * merely looks (PM_NOREMOVE) passes through here too, earlier. */
        if (g_trace_start && wp == PM_REMOVE && m->message == WM_LBUTTONDOWN &&
            GetAncestor(m->hwnd, GA_ROOT) == GetAncestor(g_hwnd, GA_ROOT)) {
            POINT cp = { (short)LOWORD(m->lParam), (short)HIWORD(m->lParam) };
            ClientToScreen(m->hwnd, &cp);
            EnterCriticalSection(&g_cs);
            wtc_st_click(&g_st, GetTickCount(), cp.x, cp.y);
            LeaveCriticalSection(&g_cs);
        }
        /* Decision lives in wintab_core.h so the test suite can reach it — the
         * g_want_dedup term is what #24 dropped, and inlining it here again is
         * how it would get dropped a third time. */
        if (wtc_should_eat_click(g_want_dedup, dt, CLICK_DEDUP_MS,
                                 GetAncestor(m->hwnd, GA_ROOT) == GetAncestor(g_hwnd, GA_ROOT))) {
            log_line("CLICK dedup: ate msg=%#x hwnd=%p dt=%lums total=%lu",
                     m->message, (void *)m->hwnd, (unsigned long)dt, ++g_clicks_eaten);
            m->message = WM_NULL;
            m->wParam = 0;
            m->lParam = 0;
        } else {
            /* diagnosis aid: a click we saw and let PASS — tells us whether
             * Wine mouse clicks exist at all and why they were skipped
             * (too late after the tip, or aimed at another root window) */
            log_line("CLICK seen: msg=%#x hwnd=%p root=%p (main=%p) dt=%lums",
                     m->message, (void *)m->hwnd,
                     (void *)GetAncestor(m->hwnd, GA_ROOT),
                     (void *)(g_hwnd ? GetAncestor(g_hwnd, GA_ROOT) : NULL),
                     (unsigned long)dt);
        }
        /* Timelapse: a mouse-up over SAI ends a stroke just as pen-up does.
         * Without this the recorder captures NOTHING when drawing with a mouse
         * or trackpad — the pressure path never fires because there is no
         * pressure. Runs even for an eaten click: the de-dup suppresses the
         * message reaching SAI, not the fact that the button was released. */
        if (m->message == WM_LBUTTONUP &&
            GetAncestor(m->hwnd, GA_ROOT) == GetAncestor(g_hwnd, GA_ROOT))
            tl_on_mouse_up();
    }
    return CallNextHookEx(g_click_hook, code, wp, lp);
}

/* install once SAI's window exists (cheap; called from producer housekeeping) */
static void ensure_click_dedup(void) {
    if (g_click_hook || !g_hwnd) return;
    /* The hook serves TWO features, so it is installed if EITHER wants it: the
     * click de-dup (opt-in since #19) and scroll->pan (#24, on by default).
     *
     * WARNING — installing this hook does NOT mean the de-dup was requested.
     * Because scroll->pan defaults on, this function installs the hook for
     * essentially everyone. #24 widened the early return here and left
     * click_hook_proc reading only the 400 ms window, which handed every user
     * #19 back (pen could not draw at all) while the log still said "dedup=off".
     * The consent flag therefore has to travel to the hook proc — that is what
     * g_want_dedup is for. A third feature on this hook needs its OWN flag;
     * do not widen these. See wtc_should_eat_click() in wintab_core.h. */
    int want_dedup = getenv("WT_CLICK_DEDUP") && !getenv("WT_NO_CLICK_DEDUP");
    int want_pan   = !getenv("WT_NO_SCROLL_PAN");
    g_want_dedup = want_dedup;      /* the hook proc gates the EATING on this */
    g_want_pan = want_pan;          /* ...and the scroll rewrite on this: its own flag */
    /* The hold (wtc_hold_*) is the third user of this hook, with its own flag
     * as the warning above asks: g_hold_on. It only ever reads the click. */
    if (!want_dedup && !want_pan && !g_hold_on) return;
    {   /* Log ONCE: this runs from producer housekeeping on every iteration, and
         * an unguarded line here drowned the log — a field capture came back
         * 2055 spam lines out of 2111, hiding the packet flow it exists to show. */
        static int said;
        if (!said) {
            said = 1;
            log_line("msg hook: dedup=%s scroll-pan=%s hold=%s", want_dedup ? "on" : "off",
                     want_pan ? "on" : "off", g_hold_on ? "on" : "off");
        }
    }
    HWND root = GetAncestor(g_hwnd, GA_ROOT);
    DWORD tid = GetWindowThreadProcessId(root ? root : g_hwnd, NULL);
    if (!tid) return;
    g_click_hook = SetWindowsHookExW(WH_GETMESSAGE, click_hook_proc, NULL, tid);
    log_line("click dedup: hook %s on thread %lu",
             g_click_hook ? "installed" : "FAILED", (unsigned long)tid);
}

/* parse "p [x y w h]" into a SAMPLE (pure logic lives in wintab_core.h) */
static int parse_sample(const char *buf, SAMPLE *out) { return wtc_parse_sample(buf, out); }

/* Write the DLL -> app status file (format and rationale: wtc_format_status).
 *
 * Rewritten whole, every time: it is seven short lines, the app only ever reads
 * it complete, and a partial rewrite would be read as a state we are not in.
 * Unlike the debug log this is ALWAYS on — the whole point is that the answer
 * exists before anyone thinks to switch logging on. Cost is one small write a
 * second, off the drawing path.
 */
static void write_status(unsigned long recv, unsigned long posted) {
    char buf[256];
    int n = wtc_format_status(buf, sizeof buf, WT_BUILD_STAMP, g_open ? 1 : 0,
                              recv, posted, g_fetch_count, g_max_press);
    FILE *f;
    if (n <= 0) return;
    /* #37 lines; if they do not fit, the long-standing part still goes out */
    n += wtc_format_screens(buf + n, sizeof buf - (size_t)n, g_newscreen, g_follow_cursor);
    f = fopen("C:\\wt_status.txt", "wb");
    if (!f) return;
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
}

/* Bind the pen port. INVALID_SOCKET (and the Winsock error in *err) when the
 * port is taken, typically by a Test pen probe still running inside Wine. */
static SOCKET open_sample_socket(int *err) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) { *err = WSAGetLastError(); return INVALID_SOCKET; }
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(SAMPLE_PORT);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        *err = WSAGetLastError();
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD tv = 15;          /* ms; short so pending flush + housekeeping are prompt */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    return s;
}

/* producer: block on the UDP socket and post EACH datagram the instant it
 * arrives (no fixed poll interval — removes up to 6 ms of cursor-vs-ink lag
 * and packet clumping). A 100 ms recv timeout lets us do heartbeat logging
 * and the file fallback (kill switch / manual test) while the pen is idle.
 *
 * INSTRUMENTATION (Phase 1): the helper stamps each datagram with a monotonic
 * sequence number; we count datagrams received and any sequence GAPS (= true
 * transport loss). Compare in the log: helper's captured count vs our recv=,
 * and posted= vs SAI's WTPacket fetch count — this locates where samples are
 * lost (capture vs transport vs SAI-side) instead of guessing. */
static DWORD WINAPI producer(LPVOID arg) {
    DWORD lastBeat = 0, lastDatagram = 0, lastStatus = 0;
    unsigned long recvCount = 0, gaps = 0;
    long lastSeq = -1;
    SOCKET sock = INVALID_SOCKET;
    int wsaOK = 0;
    DWORD lastBindTry = 0, portTakenAt = 0;
    (void)arg;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) == 0) {
        int err = 0;
        wsaOK = 1;
        lastBindTry = GetTickCount();
        sock = open_sample_socket(&err);
        if (sock != INVALID_SOCKET) {
            log_line("producer: listening on UDP 127.0.0.1:%d (blocking, immediate post)", SAMPLE_PORT);
        } else {
            /* The housekeeping below retries (wtc_should_retry_bind). Also in
             * the always-on wake log: nothing else on the far side shows that
             * SAI is reading the file instead of the live stream. */
            portTakenAt = lastBindTry;
            log_line("producer: UDP bind :%d failed (%d), reading the file until the port is free "
                     "(retrying every %d ms)", SAMPLE_PORT, err, WTC_PORT_RETRY_MS);
            if (!g_host_is_probe)
                wake_log("producer: pen port %d taken (%d); SAI reads the file until it is free",
                         SAMPLE_PORT, err);
        }
    }

    for (;;) {
        char buf[128];
        int r = -1;
        if (sock != INVALID_SOCKET)
            r = recv(sock, buf, sizeof(buf)-1, 0);

        if (r > 0) {
            buf[r] = 0;
            /* parsed in wintab_core.h, where the tests can reach it; the
             * times at the end are optional (an app from before they existed) */
            WTC_DGRAM dg;
            WTC_CONTROL ctl;
            if (wtc_parse_control(buf, &ctl)) {
                /* not a pen sample: the app asking for a zoom or a wake */
                if (ctl.kind == WTC_CTL_ZOOM) zoom_datagram(ctl.zoom);
                else if (ctl.kind == WTC_CTL_ERASER) {
                    if (g_eraser != ctl.eraser) {
                        g_eraser = ctl.eraser;
                        ms_log("ERASER udp=%d", ctl.eraser);
                        /* SAI ignores the WinTab cursor index, but its own tool
                         * shortcuts work: E = eraser, N = brush. */
                        send_key_state('e', g_eraser);
                    }
                }
                else                          wake_datagram(ctl.wake);
            } else if (wtc_parse_datagram(buf, &dg)) {
                long sq = dg.seq; int p = dg.p, x = dg.x, y = dg.y, w = dg.w, h = dg.h;
                g_dg_tm = dg.tm;
                recvCount++;
                /* backlog probe DURING a stroke: how far ahead is what we've
                 * posted vs what SAI has actually pulled? A growing gap = SAI's
                 * message queue is behind = the lag you feel. */
                /* RATE probe: from two consecutive lines, deliver rate =
                 * (recv2-recv1)/(tick2-tick1) and SAI draw rate =
                 * (fetches2-fetches1)/(tick2-tick1). If deliver >> draw, SAI is
                 * the bottleneck (boxy is its limit); if both high, we can feed
                 * more; if deliver is low, the capture/tap is dropping. */
                if ((recvCount & 63) == 0)
                    log_line("RATE recv=%lu fetches=%lu gap=%d tick=%lu",
                         recvCount, g_fetch_count, (int)(g_serial - 1 - g_fetched),
                         (unsigned long)GetTickCount());
                if (lastSeq >= 0 && sq > lastSeq + 1) gaps += (unsigned long)(sq - lastSeq - 1);
                lastSeq = sq;
                SAMPLE s;
                s.press = p > MAX_PRESS ? MAX_PRESS : p;
                s.x = x; s.y = y; s.w = w; s.h = h;
                s.has_pos = (w > 0 && h > 0);
                emit_sample(&s);        /* post immediately: lowest latency */
                tl_on_pressure(s.press); /* timelapse: we ARE the pen, so stroke
                                          * ends are known exactly rather than
                                          * guessed from mouse events */
                lastDatagram = GetTickCount();
                /* A long stroke never reaches the housekeeping tick below — we
                 * `continue` for as long as datagrams keep arriving — so the
                 * status file would freeze exactly while everything works.
                 * Refresh it from the hot path too, rarely enough to be free. */
                if ((recvCount & 255) == 0) write_status(recvCount, g_posted);
            }
            flush_pending();            /* deliver freshest if SAI just caught up */
            continue;                   /* drain any backlog before housekeeping */
        }

        /* recv timed out (idle) or no socket -> housekeeping */
        DWORD now = GetTickCount();
        /* Port taken at load (or since)? Keep asking for it, so SAI goes back
         * to the live stream the moment whatever held it lets go, instead of
         * reading the file for the rest of the session. */
        if (wsaOK && wtc_should_retry_bind(sock != INVALID_SOCKET, now, lastBindTry, WTC_PORT_RETRY_MS)) {
            int err = 0;
            lastBindTry = now;
            sock = open_sample_socket(&err);
            if (sock != INVALID_SOCKET) {
                log_line("producer: pen port free again after %lu ms; listening on UDP 127.0.0.1:%d",
                         (unsigned long)(now - portTakenAt), SAMPLE_PORT);
                if (!g_host_is_probe)
                    wake_log("producer: pen port free again after %lu ms; SAI is back on the live stream",
                             (unsigned long)(now - portTakenAt));
            }
        }
        /* Timelapse: feed the debounce a zero on the idle path too. The helper
         * stops sending entirely once the pen leaves range, so a stroke that
         * ends by lifting away would otherwise never see enough zero samples to
         * pass the debounce, and the last stroke before a pause would go
         * unrecorded. The recv timeout makes this a steady ~10 Hz tick. */
        tl_on_pressure(0);
        check_hold(now);                /* the hold: say once in the log that it is on */
        hold_release(0, 1);             /* pen down and still, no click: the time limit */
        ensure_click_dedup();           /* pen-tap double-click fix, once hwnd exists */
        /* both arrive as datagrams; these read the fallback files */
        check_win32_wake(now, sock != INVALID_SOCKET);
        check_zoom(now, sock != INVALID_SOCKET);
        refresh_virtual();              /* screens changed? (#37) tell SAI promptly */
        flush_pending();                /* pen still/lifted: deliver the final point */
        st_report(now, 0);              /* a tap's START line: no samples follow it */
        if (now - lastStatus > 1000) {
            lastStatus = now;
            write_status(recvCount, g_posted);
        }
        if (now - lastBeat > 2000) {
            lastBeat = now;
            log_line("producer: open=%d hwnd=%p posted=%lu recv=%lu gaps=%lu fetched=%u udp=%s",
                 g_open, g_hwnd, (unsigned long)g_posted, recvCount, gaps, g_fetched,
                 sock == INVALID_SOCKET ? "off" : (now - lastDatagram < 1000 ? "live" : "quiet"));
            if (g_hold_on)
                log_line("HOLD so far: ended by click=%lu time-limit=%lu pen-lifted=%lu",
                         g_hold_by[1], g_hold_by[0], g_hold_by[2]);
        }
        /* file fallback only while the socket is quiet, and EDGE-TRIGGERED:
         * emit only when the file's contents change. A static file (e.g. the
         * last pen-up sample sitting there while you use the mouse) must NOT
         * keep re-emitting hover packets — that told SAI a pen was present and
         * blocked mouse painting. `echo 0 > file` kill switch still works. */
        static DWORD lastFileRead;
        if (wtc_press_file_due(sock != INVALID_SOCKET, now, lastDatagram, lastFileRead)) {
            static char lastFile[128];
            lastFileRead = now;
            FILE *f = fopen("C:\\wt_pressure.txt", "rb");
            if (f) {
                char b[128];
                size_t nb = fread(b, 1, sizeof(b)-1, f);
                fclose(f);
                b[nb] = 0;
                if (strcmp(b, lastFile) != 0) {
                    strcpy(lastFile, b);
                    SAMPLE s;
                    g_dg_tm.has = 0;        /* the file carries no times */
                    if (parse_sample(b, &s)) emit_sample(&s);
                }
            }
        }
        if (sock == INVALID_SOCKET) Sleep(6);   /* avoid spin in file-only mode */
    }
    return 0;
}

/* ---------------- exported WinTab API (only what SAI uses) --------------- */

/* ---- SAI compatibility: the exact WTInfo surface and packet layout of
 * 0.3.10. SAI became slower after the universal WinTab changes; the full
 * device/cursor info made it choose a different path. Other apps keep the
 * full surface below. ---- */
static int host_is_sai(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    cached = 0;
    if (n > 0) {
        for (DWORD i = 0; i + 8 <= n; i++) {
            if ((path[i] == L's' || path[i] == L'S') &&
                (path[i+1] == L'a' || path[i+1] == L'A') &&
                (path[i+2] == L'i' || path[i+2] == L'I') &&
                (path[i+3] == L'2' || path[i+3] == L'2') &&
                path[i+4] == L'.' &&
                (path[i+5] == L'e' || path[i+5] == L'E') &&
                (path[i+6] == L'x' || path[i+6] == L'X') &&
                (path[i+7] == L'e' || path[i+7] == L'E')) {
                cached = 1; break;
            }
        }
    }
    return cached;
}

UINT WINAPI wt_info_sai_w(UINT cat, UINT idx, LPVOID out) {
    ensure_plugin_threads();
    log_line("WTInfoW cat=%u idx=%u out=%p", cat, idx, out);
    switch (cat) {
    case 0: return 200;
    case WTI_INTERFACE:
        switch (idx) {
        case 1: { static const WCHAR id[]=L"Wine OwnTab 1.1"; if(out) memcpy(out,id,sizeof(id)); return sizeof(id); }
        case 2: { WORD v=(1)|(1<<8); if(out)*(WORD*)out=v; return sizeof(WORD); }   /* SPECVERSION 1.1 */
        case 3: { WORD v=(0)|(1<<8); if(out)*(WORD*)out=v; return sizeof(WORD); }
        case 4: case 6: { UINT n=1; if(out)*(UINT*)out=n; return sizeof(UINT); }    /* NDEVICES/NCONTEXTS */
        case 5: { UINT n=2; if(out)*(UINT*)out=n; return sizeof(UINT); }            /* NCURSORS */
        default: return 0;
        }
    case WTI_DEFCONTEXT: case WTI_DDCTXS:
        if (idx==0) { if(out) fill_default_context((LOGCONTEXTW*)out); return sizeof(LOGCONTEXTW); }
        return 0;
    case WTI_DEVICES:
        switch (idx) {
        case 15: { AXIS a={0,MAX_PRESS,0,0}; if(out)*(AXIS*)out=a; return sizeof(AXIS); } /* NPRESSURE */
        case 17: { AXIS a[3]={{0,3600,0,0},{0,900,0,0},{0,0,0,0}}; if(out)memcpy(out,a,sizeof(a)); return sizeof(a); } /* ORIENTATION */
        case 23: { UINT n=2; if(out)*(UINT*)out=n; return sizeof(UINT); }           /* NCSRTYPES */
        case 24: { UINT n=1; if(out)*(UINT*)out=n; return sizeof(UINT); }           /* FIRSTCSR */
        case 1: { static const WCHAR n[]=L"Wine OwnTab"; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        default: return 0;
        }
    case WTI_CURSORS + 0: case WTI_CURSORS + 1:
        switch (idx) {
        case 1: { static const WCHAR pen[]=L"Pen", er[]=L"Eraser";
                  const WCHAR *n = (cat == WTI_CURSORS + 1) ? er : pen;
                  if(out)memcpy(out,n,(cat == WTI_CURSORS + 1 ? sizeof(er) : sizeof(pen)));
                  return (cat == WTI_CURSORS + 1) ? (UINT)sizeof(er) : (UINT)sizeof(pen); }
        case 15: { UINT n = (cat == WTI_CURSORS + 1) ? 0 : 1; if(out)*(UINT*)out=n; return sizeof(UINT); } /* CSR_TYPE */
        case 8: { WTPKT m=OUR_PKTDATA; if(out)*(WTPKT*)out=m; return sizeof(WTPKT); }
        default: { UINT z=0; if(out)*(UINT*)out=z; return sizeof(UINT); }
        }
    default: return 0;
    }
}


/* ---- ANSI variants (SAI's "Ver.1 compatible" path may use these) ---------- */

typedef struct {
    char  lcName[LCNAMELEN];
    UINT  lcOptions, lcStatus, lcLocks, lcMsgBase, lcDevice, lcPktRate;
    WTPKT lcPktData, lcPktMode, lcMoveMask;
    DWORD lcBtnDnMask, lcBtnUpMask;
    LONG  lcInOrgX, lcInOrgY, lcInOrgZ, lcInExtX, lcInExtY, lcInExtZ;
    LONG  lcOutOrgX, lcOutOrgY, lcOutOrgZ, lcOutExtX, lcOutExtY, lcOutExtZ;
    DWORD lcSensX, lcSensY, lcSensZ;
    BOOL  lcSysMode;
    int   lcSysOrgX, lcSysOrgY, lcSysExtX, lcSysExtY;
    DWORD lcSysSensX, lcSysSensY;
} LOGCONTEXTA;

static void fill_default_context_a(LOGCONTEXTA *lc) {
    LOGCONTEXTW w; fill_default_context(&w);
    memset(lc, 0, sizeof(*lc));
    strcpy(lc->lcName, "OurDefault");
    memcpy(&lc->lcOptions, &w.lcOptions, sizeof(LOGCONTEXTA) - LCNAMELEN);
}


UINT WINAPI wt_info_sai_a(UINT cat, UINT idx, LPVOID out) {
    ensure_plugin_threads();
    log_line("WTInfoA cat=%u idx=%u out=%p", cat, idx, out);
    switch (cat) {
    case 0: return 200;
    case WTI_INTERFACE:
        switch (idx) {
        case 1: { static const char id[]="Wine OwnTab 1.1"; if(out) memcpy(out,id,sizeof(id)); return sizeof(id); }
        case 2: { WORD v=(1)|(1<<8); if(out)*(WORD*)out=v; return sizeof(WORD); }
        case 3: { WORD v=(0)|(1<<8); if(out)*(WORD*)out=v; return sizeof(WORD); }
        case 4: case 5: case 6: { UINT n=1; if(out)*(UINT*)out=n; return sizeof(UINT); }
        default: return 0;
        }
    case WTI_DEFCONTEXT: case WTI_DDCTXS:
        if (idx==0) { if(out) fill_default_context_a((LOGCONTEXTA*)out); return sizeof(LOGCONTEXTA); }
        return 0;
    case WTI_DEVICES:
        switch (idx) {
        case 15: { AXIS a={0,MAX_PRESS,0,0}; if(out)*(AXIS*)out=a; return sizeof(AXIS); }
        case 17: { AXIS a[3]={{0,3600,0,0},{0,900,0,0},{0,0,0,0}}; if(out)memcpy(out,a,sizeof(a)); return sizeof(a); }
        case 23: { UINT n=2; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case 24: { UINT n=1; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case 1: { static const char n[]="Wine OwnTab"; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        default: return 0;
        }
    case WTI_CURSORS + 0: case WTI_CURSORS + 1:
        switch (idx) {
        case 1: { static const char pen[]="Pen", er[]="Eraser";
                  if (cat == WTI_CURSORS + 1) { if(out)memcpy(out,er,sizeof(er)); return sizeof(er); }
                  if(out)memcpy(out,pen,sizeof(pen)); return sizeof(pen); }
        case 15: { UINT n = (cat == WTI_CURSORS + 1) ? 0 : 1; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case 8: { WTPKT m=OUR_PKTDATA; if(out)*(WTPKT*)out=m; return sizeof(WTPKT); }
        default: { UINT z=0; if(out)*(UINT*)out=z; return sizeof(UINT); }
        }
    default: return 0;
    }
}


UINT WINAPI WTInfoW(UINT cat, UINT idx, LPVOID out) {
    if (host_is_sai()) return wt_info_sai_w(cat, idx, out);
    log_line("WTInfoW cat=%u idx=%u out=%p", cat, idx, out);
    switch (cat) {
    case 0: return 200;
    case WTI_INTERFACE:
        switch (idx) {
        case 1: { static const WCHAR id[]=L"Wine OwnTab 1.1"; if(out) memcpy(out,id,sizeof(id)); return sizeof(id); }
        case 2: { WORD v=(1)|(1<<8); if(out)*(WORD*)out=v; return sizeof(WORD); }
        case 3: { WORD v=(1)|(1<<8); if(out)*(WORD*)out=v; return sizeof(WORD); }
        case 4: case 5: case 6: { UINT n=1; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case 7: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }  /* CTXOPTIONS */
        case 8: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }  /* CTXSAVESIZE */
        case 9: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }  /* NEXTENSIONS */
        case 10:{ UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }  /* NEXTSIZE */
        default: return 0;
        }
    case WTI_DEFCONTEXT: case WTI_DEFSYSCTX: case WTI_DDCTXS:
        if (idx==0) { if(out) fill_default_context((LOGCONTEXTW*)out); return sizeof(LOGCONTEXTW); }
        return 0;
    case WTI_DEVICES:
        switch (idx) {
        case DVC_NAME: { static const WCHAR n[]=L"Wine OwnTab"; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        case DVC_X: case DVC_Y: case DVC_Z: { AXIS a={0,IN_EXT,0,0}; if(out)*(AXIS*)out=a; return sizeof(AXIS); }
        case DVC_NPRESSURE: case DVC_TPRESSURE: { AXIS a={0,MAX_PRESS,0,0}; if(out)*(AXIS*)out=a; return sizeof(AXIS); }
        case DVC_ORIENTATION: case DVC_ROTATION: { AXIS a[3]={{0,3600,0,0},{0,900,0,0},{0,0,0,0}}; if(out)memcpy(out,a,sizeof(a)); return sizeof(a); }
        case DVC_PKTRATE: { UINT n=133; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case DVC_PKTDATA: { WTPKT m=OUR_PKTDATA; if(out)*(WTPKT*)out=m; return sizeof(WTPKT); }
        case DVC_PKTMODE: { WTPKT m=0; if(out)*(WTPKT*)out=m; return sizeof(WTPKT); }
        case DVC_HARDWARE: { DWORD n=0; if(out)*(DWORD*)out=n; return sizeof(DWORD); }
        case DVC_NCSRTYPES: { UINT n=1; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case DVC_FIRSTCSR: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case DVC_DEFX: case DVC_DEFY: case DVC_DEFZ: { LONG n=0; if(out)*(LONG*)out=n; return sizeof(LONG); }
        default: return 0;
        }
    case WTI_CURSORS:
        switch (idx) {
        case CSR_NAME: { static const WCHAR n[]=L"Pen"; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        case CSR_ACTIVE: { BOOL b=TRUE; if(out)*(BOOL*)out=b; return sizeof(BOOL); }
        case CSR_PKTDATA: { WTPKT m=OUR_PKTDATA; if(out)*(WTPKT*)out=m; return sizeof(WTPKT); }
        case CSR_BUTTONS: { UINT n=3; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case CSR_BUTTONBITS: { UINT n=3; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case CSR_BTNNAMES: { static const WCHAR n[]=L"Left\0Middle\0Right\0"; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        case CSR_BUTTONMAP: { BYTE m[3]={0,1,2}; if(out)memcpy(out,m,sizeof(m)); return sizeof(m); }
        case CSR_SYSBTNMAP: { BYTE m[3]={0,1,2}; if(out)memcpy(out,m,sizeof(m)); return sizeof(m); }
        case CSR_NPBUTTON: { UINT n=3; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case CSR_TPBUTTON: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case CSR_NPBTNMARKS: case CSR_NPBREPEAT: case CSR_NPBTNACTION: { UINT n[3]={0,0,0}; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        case CSR_PHYSID: case CSR_TYPE: case CSR_CAPABILITIES: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }
        default: { UINT z=0; if(out)*(UINT*)out=z; return sizeof(UINT); }
        }
    default: return 0;
    }
}

static void log_ctx(const char *who, const LOGCONTEXTW *lc) {
    log_line("%s: pktData=%#lx pktMode=%#lx moveMask=%#lx opts=%#x msgBase=%#x rate=%u "
         "in=(%ld,%ld ext %ld,%ld) out=(%ld,%ld ext %ld,%ld) sys=(%d,%d ext %d,%d) sysMode=%d",
         who, (unsigned long)lc->lcPktData, (unsigned long)lc->lcPktMode,
         (unsigned long)lc->lcMoveMask, lc->lcOptions, lc->lcMsgBase, lc->lcPktRate,
         (long)lc->lcInOrgX, (long)lc->lcInOrgY, (long)lc->lcInExtX, (long)lc->lcInExtY,
         (long)lc->lcOutOrgX, (long)lc->lcOutOrgY, (long)lc->lcOutExtX, (long)lc->lcOutExtY,
         lc->lcSysOrgX, lc->lcSysOrgY, lc->lcSysExtX, lc->lcSysExtY, lc->lcSysMode);
}


/* ---- GDI canvas survey probe (development only; C:\wt_canvas_capture) ----
 *
 * Hot path must stay free of file I/O: an earlier version logged straight to
 * disk from inside SetDIBitsToDevice, which stalled SAI during strokes badly
 * enough to hang the whole tablet UI. Everything now goes to a fixed memory
 * buffer and only hits the SD card when a trigger file asks for it.
 *
 * C:\wt_canvas_capture   enables the survey at load time
 * C:\wt_memscan          on appearance: dump the log, snapshot the canvas DIB,
 *                        search process memory for copies of its pixels
 */
typedef BOOL (WINAPI *FnBitBlt)(HDC,int,int,int,int,HDC,int,int,DWORD);
typedef int  (WINAPI *FnSetDIBitsToDevice)(HDC,int,int,DWORD,DWORD,int,int,UINT,UINT,const void*,const BITMAPINFO*,UINT);
typedef HBITMAP (WINAPI *FnCreateDIBSection)(HDC,const BITMAPINFO*,UINT,void**,HANDLE,DWORD);
typedef int (WINAPI *FnStretchDIBits)(HDC,int,int,int,int,int,int,int,int,const void*,const BITMAPINFO*,UINT,DWORD);
typedef HBITMAP (WINAPI *FnCreateCompatibleBitmap)(HDC,int,int);
typedef BOOL (WINAPI *FnDeleteObject)(HGDIOBJ);

static FnBitBlt                g_orig_bitblt;
static FnSetDIBitsToDevice     g_orig_setdibits;
static FnCreateDIBSection      g_orig_cdibs;
static FnStretchDIBits         g_orig_stretchdibits;
static FnCreateCompatibleBitmap g_orig_ccbmp;
static FnDeleteObject          g_orig_delobj;

static int g_gdi_hooks_done;

#define GDI_LOGSZ (96 * 1024)
static char   g_gdi_buf[GDI_LOGSZ];
static int    g_gdi_len;

static void gdi_log(const char *fmt, ...) {
    if (g_gdi_len >= GDI_LOGSZ - 256) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_gdi_buf + g_gdi_len, (size_t)(GDI_LOGSZ - g_gdi_len), fmt, ap);
    va_end(ap);
    if (n > 0) {
        g_gdi_len += n;
        if (g_gdi_len > GDI_LOGSZ) g_gdi_len = GDI_LOGSZ;
        g_gdi_buf[g_gdi_len < GDI_LOGSZ ? g_gdi_len : GDI_LOGSZ - 1] = 0;
        if (g_gdi_len < GDI_LOGSZ - 1) g_gdi_buf[g_gdi_len++] = '\n';
    }
}

static void gdi_flush(void) {
    FILE *f = fopen("C:\\wt_gdi.log", "wb");
    if (!f) return;
    fwrite(g_gdi_buf, 1, (size_t)g_gdi_len, f);
    fclose(f);
    gdi_log("==== log flushed (%d bytes) ====", g_gdi_len);
}

static const char *gdi_class_of(HDC hdc) {
    static char buf[96];
    buf[0] = 0;
    if (!hdc) return "(null-dc)";
    HWND w = WindowFromDC(hdc);
    if (!w) return "(memory-dc)";
    char cls[64] = "", title[96] = "";
    if (!GetClassNameA(w, cls, (int)sizeof cls)) cls[0] = 0;
    GetWindowTextA(w, title, (int)sizeof title);
    _snprintf(buf, sizeof buf - 1, "%p[%s|%s]", (void*)w, cls, title);
    buf[sizeof buf - 1] = 0;
    return buf;
}

/* Last canvas-scale present: the DIB SAI composites the document into, plus
 * where it landed. Everything below works off this snapshot. */
static void *volatile g_cv_bits;
static volatile int g_cv_w, g_cv_h, g_cv_stride, g_cv_bpp;
static volatile int g_cv_x, g_cv_y, g_cv_cx, g_cv_cy;
static volatile int g_cv_sx, g_cv_sy;   /* DIB source coords of the last blit */

/* Every large 32bpp DIB SAI creates, so the document capture can look at all
 * of them instead of trusting whichever one presented last (SAI keeps more
 * than one canvas-sized buffer, and the last present is not always the one
 * carrying the current stroke). */
typedef struct { void *bits; int w, h; } DibRec;
static DibRec g_dibs[8];
static volatile int g_dib_n;
static volatile HWND g_cv_dst;
static volatile int g_want_capture;   /* set by the scan thread, consumed by a blit */
static volatile int g_want_write;     /* C:\wt_write: paint a marker into the DIB */
static volatile int g_write_done;
static volatile int g_needle_ready;
static unsigned char g_needle[256];
static volatile unsigned long g_blit_count;

/* Ring of recent presents. The full-canvas DIB we latch is only refreshed by
 * whole-canvas blits, while a stroke repaints dirty rectangles; without this
 * history it looks as if nothing repaints at all during drawing. */
typedef struct {
    unsigned long long bits;
    int w, h, stride, bpp, x, y, cx, cy, sx, sy, start, lines;
    unsigned long long dst;
} BlitRec;
#define BLIT_HIST 96
static BlitRec g_blit_hist[BLIT_HIST];
static volatile int g_blit_hist_n;
static volatile int g_blit_hist_need;

/* ---- caller stack sampling (C:\wt_stack) --------------------------------
 * SetDIBitsToDevice is reached from a handful of places inside sai2.exe, so
 * the immediate return address only says "who presents". What decides the
 * plugin architecture is "who composites": the frames above. Winlator/Box64
 * gives us no usable hardware breakpoints, so walk the raw stack upward and
 * keep only words that (a) point into sai2.exe's image and (b) are preceded
 * by a call encoding. Everything else on the stack is locals and false hits.
 */
static void ms_log(const char *fmt, ...);
static volatile int g_want_stack;
static volatile int g_stack_ready;
static volatile unsigned long long g_stack_bits;
static volatile unsigned long long g_stack_ret;
static volatile unsigned long long g_stack_base;
static volatile int g_stack_len;
static unsigned char g_stack_copy[16384];

static int probe_image_range(unsigned long long *base, unsigned long long *end) {
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)((BYTE*)exe + dos->e_lfanew);
    *base = (unsigned long long)(uintptr_t)exe;
    *end = *base + nt->OptionalHeader.SizeOfImage;
    return 1;
}

/* Runs inside the paint path: copy a window of the caller stack into a static
 * buffer and nothing else. Parsing and file output happen later, on the scan
 * thread, because anything slow here stalls the canvas just like the old
 * per-event logging did. */
static void stack_capture(void) {
    unsigned char anchor = 0;
    unsigned long long start = (unsigned long long)(uintptr_t)&anchor & ~0xFFFull;
    g_stack_base = start;
    g_stack_ret = (unsigned long long)(uintptr_t)__builtin_return_address(0);
    g_stack_ready = 0;
    int len = 0;
    while (len < (int)sizeof g_stack_copy) {
        unsigned long long a = start + (unsigned long long)len;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mbi, sizeof mbi)) break;
        if (mbi.State != MEM_COMMIT) break;
        if (mbi.Protect & PAGE_GUARD) break;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)))
            break;
        unsigned long long region_end =
            (unsigned long long)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        int chunk = (int)(region_end - a);
        if (chunk > (int)sizeof g_stack_copy - len) chunk = (int)sizeof g_stack_copy - len;
        if (chunk <= 0) break;
        memcpy(g_stack_copy + len, (void*)(uintptr_t)a, (size_t)chunk);
        len += chunk;
    }
    g_stack_len = len;
    g_stack_ready = 1;
}

static int looks_like_call_site(unsigned long long v) {
    const unsigned char *p = (const unsigned char*)(uintptr_t)v;
    if (p[-5] == 0xE8) return 1;                                  /* call rel32 */
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return 1;        /* call r/m64 */
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return 1;        /* call [rip+x] */
    return 0;
}

/* Scan thread: turn the copied stack window into a readable frame list. */
static void stack_report(void) {
    unsigned long long base = 0, end = 0;
    if (!probe_image_range(&base, &end)) return;
    FILE *f = fopen("C:\\wt_stack.log", "wb");
    if (!f) return;
    fprintf(f, "immediate_ret=%p rva=%llx bits=%p window=%p+%d\n",
            (void*)(uintptr_t)g_stack_ret, g_stack_ret - base,
            (void*)(uintptr_t)g_stack_bits, (void*)(uintptr_t)g_stack_base, g_stack_len);
    int n = 0;
    for (int off = 0; off + 8 <= g_stack_len; off += 8) {
        unsigned long long v;
        memcpy(&v, g_stack_copy + off, 8);
        if (v < base + 8 || v >= end) continue;
        if (!looks_like_call_site(v)) continue;
        fprintf(f, "frame +%04x %p rva=%llx\n", off, (void*)(uintptr_t)v, v - base);
        if (++n >= 60) break;
    }
    fprintf(f, "frames=%d\n", n);
    fclose(f);
    ms_log("STACK captured %d bytes, %d candidate frames -> C:\\wt_stack.log",
           g_stack_len, n);
}

static void gdi_dump_canvas(void) {
    void *bits = g_cv_bits;
    int w = g_cv_w, h = g_cv_h, stride = g_cv_stride, bpp = g_cv_bpp;
    if (!bits || w < 512 || h < 512 || stride <= 0) return;
    BITMAPFILEHEADER fh; memset(&fh, 0, sizeof fh);
    BITMAPINFOHEADER ih; memset(&ih, 0, sizeof ih);
    DWORD dataSize = (DWORD)((long long)stride * h);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + dataSize;
    ih.biSize = sizeof ih;
    ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1;
    ih.biBitCount = (WORD)bpp; ih.biCompression = BI_RGB;
    ih.biSizeImage = dataSize;
    FILE *f = fopen("C:\\wt_canvas.bmp", "wb");
    if (!f) return;
    fwrite(&fh, 1, sizeof fh, f);
    fwrite(&ih, 1, sizeof ih, f);
    const unsigned char *p = (const unsigned char*)bits;
    for (int y = h - 1; y >= 0; --y) fwrite(p + (size_t)y * stride, 1, (size_t)stride, f);
    fclose(f);
    gdi_log("DUMP canvas view %dx%d stride=%d bpp=%d -> C:\\wt_canvas.bmp", w, h, stride, bpp);
}

/* Pick 256 bytes of the view DIB that change the most between neighbouring
 * pixels: a flat white canvas would match half of memory. */
static int gdi_make_needle(void) {
    const unsigned char *p = (const unsigned char*)g_cv_bits;
    int w = g_cv_w, h = g_cv_h, stride = g_cv_stride;
    if (!p || w < 600 || h < 600) return 0;
    int besty = -1, bestx = -1, bestscore = -1;
    for (int y = 4; y < h - 4; y += 2) {
        const unsigned char *row = p + (size_t)y * stride;
        for (int x = 4; x + 72 < w; x += 4) {
            int score = 0;
            for (int i = 0; i < 63; ++i) {
                const unsigned char *a = row + (size_t)(x + i) * 4;
                const unsigned char *b = row + (size_t)(x + i + 1) * 4;
                if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) score++;
            }
            if (score > bestscore) { bestscore = score; besty = y; bestx = x; }
        }
    }
    if (bestscore < 2 || besty < 0) {
        gdi_log("no distinctive needle (best score %d)", bestscore);
        return 0;
    }
    memcpy(g_needle, p + (size_t)besty * stride + (size_t)bestx * 4, sizeof g_needle);
    gdi_log("needle at view (%d,%d) score=%d first=%02x%02x%02x%02x",
            bestx, besty, bestscore, g_needle[0], g_needle[1], g_needle[2], g_needle[3]);
    return 1;
}

static BOOL WINAPI probe_BitBlt(HDC dst, int x, int y, int cx, int cy,
                                HDC src, int sx, int sy, DWORD rop) {
    if (g_gdi_hooks_done) {
        ++g_blit_count;
        if (cx * cy >= 60000) gdi_log("BitBlt %dx%d at (%d,%d) dst=%s", cx, cy, x, y, gdi_class_of(dst));
    }
    return g_orig_bitblt(dst, x, y, cx, cy, src, sx, sy, rop);
}

static int WINAPI probe_SetDIBitsToDevice(HDC hdc, int x, int y, DWORD cx, DWORD cy,
                                          int sx, int sy, UINT start, UINT lines,
                                          const void *bits, const BITMAPINFO *bi, UINT usage) {
    if (g_gdi_hooks_done && bits && bi) {
        int w = bi->bmiHeader.biWidth, h = bi->bmiHeader.biHeight;
        if (h < 0) h = -h;
        if (w >= 512 && h >= 512) {
            {
                int idx = g_blit_hist_n;
                if (idx < BLIT_HIST) g_blit_hist_n = idx + 1;
                else idx = BLIT_HIST - 1;
                if (g_blit_hist_n >= BLIT_HIST) {
                    memmove(g_blit_hist, g_blit_hist + 1,
                            sizeof(BlitRec) * (BLIT_HIST - 1));
                    idx = BLIT_HIST - 1;
                }
                BlitRec *r = &g_blit_hist[idx];
                r->bits = (unsigned long long)(uintptr_t)bits;
                r->w = w; r->h = h; r->stride = 0; r->bpp = bi->bmiHeader.biBitCount;
                r->x = x; r->y = y; r->cx = (int)cx; r->cy = (int)cy;
                r->sx = sx; r->sy = sy; r->start = (int)start; r->lines = (int)lines;
                r->dst = (unsigned long long)(uintptr_t)WindowFromDC(hdc);
            }
            g_cv_bits = (void*)bits; g_cv_w = w; g_cv_h = h;
            g_cv_stride = bi->bmiHeader.biSizeImage ? bi->bmiHeader.biSizeImage
                                                    : ((w * bi->bmiHeader.biBitCount + 31) / 32) * 4;
            g_cv_bpp = bi->bmiHeader.biBitCount;
            g_cv_x = x; g_cv_y = y; g_cv_cx = (int)cx; g_cv_cy = (int)cy;
            g_cv_sx = sx; g_cv_sy = sy;
            g_cv_dst = WindowFromDC(hdc);
            if (g_want_write) {
                g_want_write = 0;
                /* Fill a block well inside the visible document. If the next
                 * repaint keeps it, this DIB is the document itself and the
                 * whole plugin can work through it. If it vanishes, SAI only
                 * recomposites into it and the layer buffers must be found. */
                unsigned char *d = (unsigned char*)bits;
                int rx = g_cv_stride * 4;
                int bx = w / 2 - 160, by = h / 2 - 160, bw = 320, bh = 320;
                if (bx + bw < w && by + bh < h) {
                    for (int yy = by; yy < by + bh; ++yy) {
                        unsigned char *row = d + (size_t)yy * g_cv_stride;
                        for (int xx = bx; xx < bx + bw; ++xx) {
                            row[xx * 4 + 0] = 0xFF;  /* B */
                            row[xx * 4 + 1] = 0x00;  /* G */
                            row[xx * 4 + 2] = 0xFF;  /* R */
                            row[xx * 4 + 3] = 0xFF;  /* A */
                        }
                    }
                    g_write_done = 1;
                    gdi_log("WRITE TEST: painted 320x320 magenta at DIB (%d,%d) stride=%d bits=%p",
                            bx, by, g_cv_stride, bits);
                    gdi_dump_canvas();
                    gdi_flush();
                }
                (void)rx;
            }
            if (g_want_stack) {
                g_want_stack = 0;
                g_stack_bits = (unsigned long long)(uintptr_t)bits;
                gdi_log("STACK captured on canvas present dst=%s bits=%p", gdi_class_of(hdc), bits);
                stack_capture();
            }
            if (g_want_capture) {
                g_want_capture = 0;
                gdi_log("canvas present %dx%d stride=%d bpp=%d dst=%s rect=(%d,%d %dx%d) src=(%d,%d) start=%u lines=%u",
                        w, h, g_cv_stride, g_cv_bpp, gdi_class_of(hdc), x, y, (int)cx, (int)cy, sx, sy, start, lines);
                gdi_dump_canvas();
                g_needle_ready = gdi_make_needle() ? 1 : 0;
                gdi_flush();
            }
        }
    }
    return g_orig_setdibits(hdc, x, y, cx, cy, sx, sy, start, lines, bits, bi, usage);
}

static int WINAPI probe_StretchDIBits(HDC hdc, int xd, int yd, int wd, int hd,
                                      int xs, int ys, int ws, int hs,
                                      const void *bits, const BITMAPINFO *bi,
                                      UINT usage, DWORD rop) {
    if (g_gdi_hooks_done && bits && bi && wd * hd >= 60000)
        gdi_log("StretchDIBits dst %dx%d at (%d,%d) src %dx%d bits=%p", wd, hd, xd, yd, ws, hs, bits);
    return g_orig_stretchdibits(hdc, xd, yd, wd, hd, xs, ys, ws, hs, bits, bi, usage, rop);
}

static HBITMAP WINAPI probe_CreateDIBSection(HDC hdc, const BITMAPINFO *bi, UINT usage,
                                             void **bits, HANDLE section, DWORD offset) {
    HBITMAP h = g_orig_cdibs(hdc, bi, usage, bits, section, offset);
    if (g_gdi_hooks_done && h && bi)
        gdi_log("CreateDIBSection %dx%d bpp=%d bits=%p",
                bi->bmiHeader.biWidth, bi->bmiHeader.biHeight,
                bi->bmiHeader.biBitCount, bits ? *bits : NULL);
    if (h && bi && bits && *bits && bi->bmiHeader.biBitCount == 32) {
        int w = bi->bmiHeader.biWidth, hh = bi->bmiHeader.biHeight;
        if (w < 0) w = -w;
        if (hh < 0) hh = -hh;
        if (w >= 256 && hh >= 256 && g_dib_n < 8) {
            g_dibs[g_dib_n].bits = *bits;
            g_dibs[g_dib_n].w = w;
            g_dibs[g_dib_n].h = hh;
            ++g_dib_n;
        }
    }
    return h;
}

static HBITMAP WINAPI probe_CreateCompatibleBitmap(HDC hdc, int w, int h) {
    HBITMAP b = g_orig_ccbmp(hdc, w, h);
    if (g_gdi_hooks_done && b && (long long)w * h >= 200000)
        gdi_log("CreateCompatibleBitmap %dx%d -> %p", w, h, (void*)b);
    return b;
}

static BOOL WINAPI probe_DeleteObject(HGDIOBJ obj) {
    return g_orig_delobj(obj);
}

/* ---- memory search ------------------------------------------------------ */

static void ms_log(const char *fmt, ...) {
    FILE *f = fopen("C:\\wt_memscan.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static int ms_search(const unsigned char *needle, int len, unsigned char *hay, size_t n, size_t *at) {
    if (n < (size_t)len) return 0;
    unsigned char first = needle[0];
    size_t last = n - (size_t)len;
    for (size_t i = 0; i <= last; ++i) {
        if (hay[i] != first) continue;
        if (!memcmp(hay + i, needle, (size_t)len)) { *at = i; return 1; }
    }
    return 0;
}

static int ms_load_pattern(unsigned char *out, int maxlen) {
    FILE *f = fopen("C:\\wt_pattern.bin", "rb");
    if (!f) return 0;
    int n = (int)fread(out, 1, (size_t)maxlen, f);
    fclose(f);
    return n >= 8 ? n : 0;
}

static void ms_run(void) {
    ms_log("==== scan ====");
    unsigned char pat[256];
    int patlen = ms_load_pattern(pat, (int)sizeof pat);
    if (patlen > 0) {
        memcpy(g_needle, pat, (size_t)patlen);
        ms_log("pattern search: %d bytes, first=%02x%02x%02x%02x%02x%02x%02x%02x",
               patlen, pat[0], pat[1], pat[2], pat[3], pat[4], pat[5], pat[6], pat[7]);
    } else {
        patlen = (int)sizeof g_needle;
    }
    ms_log("view %dx%d stride=%d bits=%p",
           g_cv_w, g_cv_h, g_cv_stride, g_cv_bits);
    HANDLE self = GetCurrentProcess();
    unsigned char *hay = (unsigned char*)malloc(4u << 20);
    if (!hay) { ms_log("no memory for buffer"); return; }
    unsigned __int64 addr = 0x10000;
    int hits = 0, regions = 0, big = 0;
    while (addr < 0x7FFFFFFF0000ull) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)(uintptr_t)addr, &mbi, sizeof mbi)) break;
        unsigned __int64 base = (unsigned __int64)(uintptr_t)mbi.BaseAddress;
        unsigned __int64 size = (unsigned __int64)mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) &&
            !(mbi.Protect & PAGE_GUARD)) {
            ++regions;
            if (size >= (4u << 20)) {
                ++big;
                ms_log("  big region %p  %llu MB", (void*)(uintptr_t)base,
                       (unsigned long long)(size >> 20));
            }
            for (unsigned __int64 off = 0; off < size; off += (4u << 20)) {
                SIZE_T chunk = (SIZE_T)((size - off > (4u << 20)) ? (4u << 20) : (size - off));
                SIZE_T got = 0;
                if (!ReadProcessMemory(self, (LPCVOID)(uintptr_t)(base + off), hay, chunk, &got)) continue;
                size_t at;
                if (ms_search(g_needle, patlen, hay, got, &at)) {
                    ++hits;
                    ms_log("HIT %p  region=%p size=%llu KB prot=%08lx off=%llu",
                           (void*)(uintptr_t)(base + off + at), (void*)(uintptr_t)base,
                           (unsigned long long)(size >> 10), (unsigned long)mbi.Protect,
                           (unsigned long long)(off + at));
                    if (hits >= 32) { ms_log("hits capped"); free(hay); return; }
                }
            }
        }
        if (size == 0) break;
        addr = base + size;
    }
    free(hay);
    ms_log("done: %d regions (%d >=4MB), %d hits", regions, big, hits);
}

/* ---- code anchor: who writes the canvas DIB? -----------------------------
 * The compositor is an internal SAI function with no import to hook. Put a
 * PAGE_GUARD on one page of the canvas DIB and let the first write fault tell
 * us the RIP that did it. Subtract the module base to get a stable file
 * offset, then disassemble offline.
 */
static PVOID g_veh;
static unsigned __int64 g_watch_addr;
static volatile int g_watch_hits;

static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (g_watch_hits >= 24) return EXCEPTION_CONTINUE_SEARCH;
    unsigned __int64 fault = (unsigned __int64)(uintptr_t)er->ExceptionInformation[1];
    HMODULE mod = NULL;
    char name[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)er->ExceptionAddress, &mod) && mod)
        GetModuleFileNameA(mod, name, (int)sizeof name);
    HMODULE exe = GetModuleHandleA(NULL);
    CONTEXT *c = ep->ContextRecord;
    unsigned long long *sp = (unsigned long long*)(uintptr_t)c->Rsp;
    ms_log("WATCH hit #%d fault=%p addr=%p rip=%p mod=%s(%p)+%llx rdi=%llx rsi=%llx "
           "rcx=%llx rdx=%llx r8=%llx r9=%llx rsp=%llx ret=%llx [rsp+8]=%llx "
           "[rsp+10]=%llx [rsp+18]=%llx",
           g_watch_hits, (void*)(uintptr_t)fault, (void*)(uintptr_t)er->ExceptionAddress,
           (void*)(uintptr_t)c->Rip,
           name[0] ? name : "?", (void*)mod,
           (unsigned long long)((unsigned __int64)(uintptr_t)c->Rip
                                - (unsigned __int64)(uintptr_t)mod),
           (unsigned long long)c->Rdi, (unsigned long long)c->Rsi,
           (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
           (unsigned long long)c->R8, (unsigned long long)c->R9,
           (unsigned long long)c->Rsp,
           (unsigned long long)(sp ? sp[0] : 0),
           (unsigned long long)(sp ? sp[1] : 0),
           (unsigned long long)(sp ? sp[2] : 0),
           (unsigned long long)(sp ? sp[3] : 0));
    (void)exe;
    /* Raw stack window: the faulting instruction is inside a shared row-copy
     * helper, so the interesting caller is one frame up and can be recovered
     * offline by validating call endings (see pe_dis.py chain). */
    {
        char line[1024]; int len = 0;
        unsigned long long sbase = (unsigned long long)c->Rsp;
        len += _snprintf(line + len, sizeof line - (size_t)len, "STACKBASE %llx", sbase);
        MEMORY_BASIC_INFORMATION q;
        int ok = VirtualQuery((LPCVOID)(uintptr_t)sbase, &q, sizeof q) &&
                 q.State == MEM_COMMIT && !(q.Protect & PAGE_GUARD);
        if (ok)
            for (int i = 0; i < 48 && len < (int)sizeof line - 24; ++i) {
                unsigned long long v =
                    ((volatile unsigned long long*)(uintptr_t)(sbase + (unsigned)i * 8))[0];
                len += _snprintf(line + len, sizeof line - (size_t)len, " %llx", v);
            }
        ms_log("%s", line);
    }
    ++g_watch_hits;
    /* One-shot: make the page writable again so SAI is not stuck faulting. */
    DWORD old;
    VirtualProtect((LPVOID)(uintptr_t)(g_watch_addr & ~0xFFFull), 0x1000ull,
                   PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* ---- memory fingerprint / diff -------------------------------------------
 * Format-agnostic way to find where a document edit lands: hash every 64 KB
 * chunk of the process, make the user draw one dot, hash again, and dump the
 * chunks whose hash moved. The layer storage must be among them, whatever its
 * pixel layout is.
 */
#define SNAP_CHUNK (64 * 1024)
typedef struct { unsigned __int64 addr; unsigned int size; unsigned __int64 hash; } SnapEnt;
static SnapEnt *g_snap;
static int g_snap_n, g_snap_cap;

static unsigned __int64 fnv1a(const unsigned char *p, size_t n) {
    unsigned __int64 h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* One-off proof that PAGE_GUARD + VEH works under Box64/Wine. Arming the real
 * canvas DIB is pointless until this control experiment passes: if a write to
 * our own guarded page produces no VEH hit, a missed canvas hit means nothing.
 */
static void veh_control_test(void) {
    void *p = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) { ms_log("VEHTEST: VirtualAlloc failed"); return; }
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, watch_veh);
    g_watch_addr = (unsigned __int64)(uintptr_t)p;
    g_watch_hits = 0;
    DWORD old = 0;
    int armed = 0;
    if (g_veh && VirtualProtect(p, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old)) armed = 1;
    ms_log("VEHTEST: page=%p armed=%d", p, armed);
    volatile unsigned int *w = (volatile unsigned int*)p;
    *w = 0x12345678u;
    ms_log("VEHTEST: after write value=%08x hits=%d", *w, g_watch_hits);
    VirtualFree(p, 0, MEM_RELEASE);
}

static void snap_collect(void) {
    HANDLE self = GetCurrentProcess();
    unsigned char *buf = (unsigned char*)malloc(SNAP_CHUNK);
    if (!buf) return;
    g_snap_n = 0;
    unsigned __int64 addr = 0x10000;
    while (addr < 0x7FFFFFFF0000ull) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)(uintptr_t)addr, &mbi, sizeof mbi)) break;
        unsigned __int64 base = (unsigned __int64)(uintptr_t)mbi.BaseAddress;
        unsigned __int64 size = (unsigned __int64)mbi.RegionSize;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) &&
            !(mbi.Protect & PAGE_GUARD)) {
            for (unsigned __int64 off = 0; off < size; off += SNAP_CHUNK) {
                SIZE_T want = (SIZE_T)((size - off > SNAP_CHUNK) ? SNAP_CHUNK : (size - off));
                SIZE_T got = 0;
                if (!ReadProcessMemory(self, (LPCVOID)(uintptr_t)(base + off), buf, want, &got)) continue;
                if (got < 16) continue;
                if (g_snap_n >= g_snap_cap) {
                    int nc = g_snap_cap ? g_snap_cap * 2 : 65536;
                    SnapEnt *nn = (SnapEnt*)realloc(g_snap, (size_t)nc * sizeof(SnapEnt));
                    if (!nn) break;
                    g_snap = nn; g_snap_cap = nc;
                }
                g_snap[g_snap_n].addr = base + off;
                g_snap[g_snap_n].size = (unsigned int)got;
                g_snap[g_snap_n].hash = fnv1a(buf, got);
                ++g_snap_n;
            }
        }
        if (size == 0) break;
        addr = base + size;
    }
    free(buf);
}

static void snap_diff(void) {
    HANDLE self = GetCurrentProcess();
    unsigned char *buf = (unsigned char*)malloc(SNAP_CHUNK);
    if (!buf || !g_snap) { free(buf); return; }
    int changed = 0, dumps = 0;
    for (int i = 0; i < g_snap_n; ++i) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(self, (LPCVOID)(uintptr_t)g_snap[i].addr, buf,
                               g_snap[i].size, &got) || got != g_snap[i].size) continue;
        unsigned __int64 h = fnv1a(buf, got);
        if (h == g_snap[i].hash) continue;
        ++changed;
        ms_log("CHANGED %p size=%u", (void*)(uintptr_t)g_snap[i].addr, g_snap[i].size);
        if (dumps < 24) {
            char path[64];
            _snprintf(path, sizeof path - 1, "C:\\wt_diff_%02d.bin", dumps);
            path[sizeof path - 1] = 0;
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(buf, 1, got, f); fclose(f); }
            ms_log("  dumped %s", path);
            ++dumps;
        }
    }
    ms_log("diff done: %d chunks changed of %d, %d dumped", changed, g_snap_n, dumps);
    free(buf);
}

/* ---- deterministic mouse strokes (C:\wt_mouse.txt) ----------------------
 * adb input reaches Winlator's touch layer, not Wine's mouse queue: clicks
 * land on dialogs but a drag on the canvas child never becomes
 * WM_LBUTTONDOWN + WM_MOUSEMOVE, which is why the earlier diff experiment
 * changed no document pixels. Injecting through mouse_event from inside the
 * process produces a real mouse sequence SAI has to treat like a pen drag.
 *
 * File format: "x0 y0 x1 y1 steps delay_ms"
 */
static void inject_stroke(int x0, int y0, int x1, int y1, int steps, int delay) {
    if (steps < 2) steps = 2;
    if (steps > 2000) steps = 2000;
    if (delay < 0) delay = 0;
    if (delay > 50) delay = 50;
    SetCursorPos(x0, y0);
    Sleep(30);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep((DWORD)delay);
    for (int i = 1; i <= steps; ++i) {
        int x = x0 + (x1 - x0) * i / steps;
        int y = y0 + (y1 - y0) * i / steps;
        SetCursorPos(x, y);
        Sleep((DWORD)delay);
    }
    Sleep(30);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    ms_log("mouse stroke (%d,%d)->(%d,%d) steps=%d delay=%d", x0, y0, x1, y1,
           steps, delay);
}

static void do_mouse_file(void) {
    FILE *cf = fopen("C:\\wt_mouse.txt", "r");
    if (!cf) return;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, steps = 40, delay = 8;
    int n = fscanf(cf, "%d %d %d %d %d %d", &x0, &y0, &x1, &y1, &steps, &delay);
    fclose(cf);
    DeleteFileA("C:\\wt_mouse.txt");
    if (n >= 4) inject_stroke(x0, y0, x1, y1, steps, delay);
}

/* Arm PAGE_GUARD inside the canvas DIB that the last present handed us.
 * File: "<offset_hex> <npages>". Keeping the offset rather than an absolute
 * address means the same trigger survives SAI re-creating the DIB. */
static void arm_canvas_guard(unsigned int off, int np) {
    if (!g_cv_bits || !g_cv_stride) { ms_log("GUARD: no canvas present seen yet"); return; }
    unsigned long long a = (unsigned long long)(uintptr_t)g_cv_bits + off;
    a &= ~0xFFFull;
    if (np < 1) np = 1;
    if (np > 64) np = 64;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, watch_veh);
    g_watch_addr = a;
    g_watch_hits = 0;
    DWORD old;
    int armed = 0;
    for (int i = 0; i < np; ++i)
        if (VirtualProtect((LPVOID)(uintptr_t)(a + (unsigned long long)i * 0x1000),
                           0x1000ull, PAGE_READWRITE | PAGE_GUARD, &old))
            ++armed;
    ms_log("GUARD armed %d page(s) at %llx (dib=%p +%x, %dx%d stride %d)",
           armed, a, g_cv_bits, off, g_cv_w, g_cv_h, g_cv_stride);
}

/* Does PAGE_GUARD actually fire on a CreateDIBSection view? A heap page
 * worked in the VEHTEST, but DIB bits are section-backed and Wine may map the
 * same pages more than once, so prove it on the real canvas before trusting a
 * negative result from the compositor watch. */
static void guard_poke_test(unsigned int off) {
    if (!g_cv_bits) { ms_log("GUARDPOKE: no canvas present seen yet"); return; }
    unsigned char *p = (unsigned char*)g_cv_bits + off;
    p = (unsigned char*)((uintptr_t)p & ~0xFFFull);
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, watch_veh);
    g_watch_addr = (unsigned __int64)(uintptr_t)p;
    g_watch_hits = 0;
    DWORD old = 0;
    int armed = VirtualProtect(p, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old) ? 1 : 0;
    unsigned char before = *p;
    *p = (unsigned char)(before ^ 0x5A);
    unsigned char after = *p;
    *p = before;
    ms_log("GUARDPOKE: page=%p armed=%d read=%02x wrote=%02x hits=%d",
           p, armed, before, after, g_watch_hits);
}

/* Guard the entire canvas DIB instead of guessing which rows a stroke lands
 * on. One VirtualProtect call, 36 MB of pages; the first touch anywhere tells
 * us whether SAI's own code writes the DIB at all, and from what RIP. */
static void guard_whole_dib(void) {
    if (!g_cv_bits || !g_cv_stride || !g_cv_h) { ms_log("GUARDWHOLE: no canvas"); return; }
    size_t len = (size_t)g_cv_stride * (size_t)g_cv_h;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, watch_veh);
    g_watch_addr = (unsigned __int64)(uintptr_t)g_cv_bits;
    g_watch_hits = 0;
    DWORD old = 0;
    int armed = VirtualProtect(g_cv_bits, len, PAGE_READWRITE | PAGE_GUARD, &old) ? 1 : 0;
    ms_log("GUARDWHOLE: dib=%p len=%zu armed=%d (stride %d h %d)",
           g_cv_bits, len, armed, g_cv_stride, g_cv_h);
}

/* Find every "surface" object: {+8 w, +0xc h, +0x10 stride bytes, +0x18 bits,
 * +0x20 second buffer}. The present path proved this layout with an 800x800
 * view surface, so scanning for it is the fastest way to discover the
 * document-resolution layer buffers we actually need to edit.
 *
 * Candidates are logged with a pixel signature (how many f8f8f8 paper pixels
 * in the first 64 KB) so view surfaces, layer surfaces and scratch buffers can
 * be told apart without dumping tens of megabytes. */


/* Find decoded image blocks: 64 KB-aligned buffers that look like a 256x256
 * 32bpp slice of the canvas, i.e. mostly paper white *and* carrying ink.
 * The surface scan misses these because a decoded layer tile is not wrapped in
 * a surface object - the compositor reads raw tile pixels. */
/* Can the plugin deliver pixels to SAI through the OS clipboard? If Ctrl+V
 * turns this CF_DIB into a new layer, then a liquify result can be committed
 * through a supported SAI path without reverse engineering its tile format. */
static void clipboard_test(void) {
    int w = 256, h = 256;
    size_t stride = (size_t)w * 4;
    size_t imgsz = stride * (size_t)h;
    size_t total = sizeof(BITMAPINFOHEADER) + imgsz;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!mem) { ms_log("CLIP: GlobalAlloc failed"); return; }
    unsigned char *p = (unsigned char*)GlobalLock(mem);
    if (!p) { GlobalFree(mem); ms_log("CLIP: GlobalLock failed"); return; }
    BITMAPINFOHEADER *ih = (BITMAPINFOHEADER*)p;
    memset(ih, 0, sizeof *ih);
    ih->biSize = sizeof *ih;
    ih->biWidth = w;
    ih->biHeight = h;          /* bottom-up, which is what CF_DIB expects */
    ih->biPlanes = 1;
    ih->biBitCount = 32;
    ih->biCompression = BI_RGB;
    ih->biSizeImage = (DWORD)imgsz;
    unsigned char *bits = p + sizeof *ih;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *q = bits + (size_t)y * stride + (size_t)x * 4;
            int inside = (x > 40 && x < 216 && y > 40 && y < 216);
            if (inside) { q[0] = 0x00; q[1] = 0xFF; q[2] = 0x00; q[3] = 0xFF; }
            else { q[0] = 0xFF; q[1] = 0x00; q[2] = 0xFF; q[3] = 0xFF; }
        }
    GlobalUnlock(mem);
    if (!OpenClipboard(NULL)) { GlobalFree(mem); ms_log("CLIP: OpenClipboard failed"); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, mem)) {
        GlobalFree(mem);
        ms_log("CLIP: SetClipboardData failed");
    } else {
        ms_log("CLIP: 256x256 green-on-magenta CF_DIB placed on clipboard");
    }
    CloseClipboard();
}

/* ---- document capture / paste (C:\wt_doccap, C:\wt_paste) ---------------
 * The view DIB is screen-space, but the canvas child window only shows the
 * paper inside it: mapping the child's client rect through the last blit's
 * (dest -> src) pair gives the paper's rect in the DIB, and the paper's own
 * pixels are one uniform colour. Cropping that rect yields document pixels at
 * the current zoom, which is what the liquify commit needs to paste back.
 */
static int g_doc_x, g_doc_y, g_doc_w, g_doc_h, g_doc_zoom;

static int row_white_run(const unsigned char *row, int npx, int *out_start) {
    int best = 0, bs = 0, run = 0, st = 0;
    for (int x = 0; x < npx; ++x) {
        const unsigned char *q = row + (size_t)x * 4;
        int w = (q[0] >= 0xF0 && q[1] >= 0xF0 && q[2] >= 0xF0);
        if (w) { if (run == 0) st = x; ++run; if (run > best) { best = run; bs = st; } }
        else run = 0;
    }
    *out_start = bs;
    return best;
}

typedef struct { int top, bot, left, right; } Blk;

/* Find the inked paper inside one 32bpp DIB. Returns 1 and fills the rect. */
static int paper_in_dib(unsigned char *d, int dw, int dh, Blk *out, int *out_dark) {
    static Blk blocks[64];
    int nb = 0, cur = -1;
    for (int y = 0; y < dh; ++y) {
        int st = 0;
        int len = row_white_run(d + (size_t)y * (size_t)dw * 4, dw, &st);
        if (len < 256) { cur = -1; continue; }
        if (cur >= 0) {
            Blk *b = &blocks[cur];
            if (st <= b->right + 8 && st + len - 1 >= b->left - 8 &&
                len >= (b->right - b->left + 1) * 6 / 10) {
                b->bot = y;
                if (st < b->left) b->left = st;
                if (st + len - 1 > b->right) b->right = st + len - 1;
                continue;
            }
            cur = -1;
        }
        if (nb < 64) {
            blocks[nb].top = blocks[nb].bot = y;
            blocks[nb].left = st;
            blocks[nb].right = st + len - 1;
            cur = nb++;
        }
    }
    int best = -1; long best_area = 0;
    for (int i = 0; i < nb; ++i) {
        Blk *b = &blocks[i];
        int w = b->right - b->left + 1, h = b->bot - b->top + 1;
        if (w < 64 || h < 64) continue;
        long area = (long)w * h;
        if (area <= best_area) continue;
        int dark = 0;
        for (int y = b->top; y <= b->bot; y += 2) {
            const unsigned char *row = d + (size_t)y * (size_t)dw * 4;
            for (int x = b->left; x <= b->right; x += 2) {
                const unsigned char *q = row + (size_t)x * 4;
                if (q[0] <= 0x30 && q[1] <= 0x30 && q[2] <= 0x30) ++dark;
            }
        }
        if (dark < 8) continue;
        best = i; best_area = area; *out_dark = dark;
    }
    if (best < 0) return 0;
    *out = blocks[best];
    return 1;
}

static void doc_capture(void) {
    if (g_dib_n == 0 && !g_cv_bits) { ms_log("DOCCAP: no DIB known"); return; }
    Blk best; int best_dark = 0; long best_area = 0;
    void *best_bits = NULL; int best_w = 0, best_h = 0;
    int tried = 0;
    for (int i = 0; i <= g_dib_n; ++i) {
        unsigned char *d;
        int dw, dh;
        if (i < g_dib_n) { d = (unsigned char*)g_dibs[i].bits; dw = g_dibs[i].w; dh = g_dibs[i].h; }
        else { if (!g_cv_bits || !g_cv_stride || !g_cv_h) break;
               d = (unsigned char*)g_cv_bits; dw = g_cv_stride / 4; dh = g_cv_h; }
        if (!d || dw < 64 || dh < 64) continue;
        ++tried;
        Blk b; int dark = 0;
        if (!paper_in_dib(d, dw, dh, &b, &dark)) continue;
        long area = (long)(b.right - b.left + 1) * (b.bot - b.top + 1);
        if (area > best_area) {
            best = b; best_area = area; best_dark = dark;
            best_bits = d; best_w = dw; best_h = dh;
        }
    }
    if (!best_bits) { ms_log("DOCCAP: no inked paper in %d DIB(s)", tried); return; }
    unsigned char *d = (unsigned char*)best_bits;
    int dw = best_w, dh = best_h;
    g_doc_x = best.left; g_doc_y = best.top;
    g_doc_w = best.right - best.left + 1;
    g_doc_h = best.bot - best.top + 1;

    char title[128] = ""; GetWindowTextA(g_cv_dst, title, (int)sizeof title - 1);
    int zoom = 100;
    /* the title only carries a zoom when it looks like "(150%)"; the document
     * name ends with "(*)" and atoi on that produced nonsense scales */
    const char *pc = strrchr(title, '(');
    if (pc && strchr(pc, '%') && strchr(pc, '%') < pc + 12) zoom = atoi(pc + 1);
    if (zoom <= 0 || zoom > 6400) zoom = 100;
    g_doc_zoom = zoom;

    size_t dstride = (size_t)dw * 4;
    size_t stride = (size_t)g_doc_w * 4;
    size_t imgsz = stride * (size_t)g_doc_h;
    unsigned char *img = (unsigned char*)malloc(imgsz);
    if (!img) { ms_log("DOCCAP: out of memory"); return; }
    for (int y = 0; y < g_doc_h; ++y)
        memcpy(img + (size_t)y * stride,
               d + (size_t)(g_doc_y + y) * dstride + (size_t)g_doc_x * 4, stride);

    FILE *f = fopen("C:\\wt_doc.txt", "wb");
    if (f) { fprintf(f, "%d %d %d %d %d\n", g_doc_x, g_doc_y, g_doc_w, g_doc_h, zoom); fclose(f); }
    f = fopen("C:\\wt_doc.bin", "wb");
    if (f) { fwrite(img, 1, imgsz, f); fclose(f); }
    BITMAPFILEHEADER fh; memset(&fh, 0, sizeof fh);
    BITMAPINFOHEADER ih; memset(&ih, 0, sizeof ih);
    ih.biSize = sizeof ih; ih.biWidth = g_doc_w; ih.biHeight = g_doc_h;
    ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)imgsz;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)imgsz;
    f = fopen("C:\\wt_doc.bmp", "wb");
    if (f) {
        fwrite(&fh, 1, sizeof fh, f);
        fwrite(&ih, 1, sizeof ih, f);
        for (int y = g_doc_h - 1; y >= 0; --y) fwrite(img + (size_t)y * stride, 1, stride, f);
        fclose(f);
    }
    ms_log("DOCCAP: paper %d,%d %dx%d zoom=%d from %dx%d dark=%d dibs=%d title=%s",
           g_doc_x, g_doc_y, g_doc_w, g_doc_h, zoom, dw, dh, best_dark, g_dib_n, title);
    free(img);
}

/* Put a BGRA image (top-down rows, C:\wt_paste.bin + "w h" in wt_paste.txt)
 * onto the clipboard as CF_DIB so SAI's Ctrl+V turns it into a layer. */
static void doc_paste(void) {
    int w = 0, h = 0;
    FILE *cf = fopen("C:\\wt_paste.txt", "r");
    if (cf) { if (fscanf(cf, "%d %d", &w, &h) != 2) w = h = 0; fclose(cf); }
    if (w <= 0 || h <= 0) { ms_log("PASTE: bad size %dx%d", w, h); return; }
    size_t stride = (size_t)w * 4;
    size_t imgsz = stride * (size_t)h;
    FILE *pf = fopen("C:\\wt_paste.bin", "rb");
    if (!pf) { ms_log("PASTE: no wt_paste.bin"); return; }
    unsigned char *img = (unsigned char*)malloc(imgsz);
    size_t got = img ? fread(img, 1, imgsz, pf) : 0;
    fclose(pf);
    if (!img || got != imgsz) { free(img); ms_log("PASTE: short read %zu/%zu", got, imgsz); return; }

    /* Publish CF_DIBV5 only: when both formats are present SAI reads CF_DIB
     * (no alpha) and composites transparent pixels as opaque black, which made
     * an aligned, padded paste turn the canvas black. DIBV5 carries the alpha
     * mask, so the padding stays transparent and the content lands where it was
     * copied from. */
    size_t total = sizeof(BITMAPV5HEADER) + 12 + imgsz;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!mem) { free(img); ms_log("PASTE: alloc failed"); return; }
    unsigned char *p = (unsigned char*)GlobalLock(mem);
    if (!p) { GlobalFree(mem); free(img); ms_log("PASTE: lock failed"); return; }
    BITMAPV5HEADER *v5 = (BITMAPV5HEADER*)p;
    memset(v5, 0, sizeof *v5);
    v5->bV5Size = sizeof *v5;
    v5->bV5Width = w;
    v5->bV5Height = h;                 /* positive = bottom-up */
    v5->bV5Planes = 1;
    v5->bV5BitCount = 32;
    v5->bV5Compression = BI_BITFIELDS;
    v5->bV5SizeImage = (DWORD)imgsz;
    v5->bV5RedMask   = 0x00FF0000u;
    v5->bV5GreenMask = 0x0000FF00u;
    v5->bV5BlueMask  = 0x000000FFu;
    v5->bV5AlphaMask = 0xFF000000u;
    v5->bV5CSType = 0;
    {
        uint32_t masks[3] = { v5->bV5RedMask, v5->bV5GreenMask, v5->bV5BlueMask };
        memcpy(p + sizeof *v5, masks, sizeof masks);      /* GDI repeats the colour masks */
        unsigned char *dst = p + sizeof *v5 + 12;
        for (int y = 0; y < h; ++y)
            memcpy(dst + (size_t)y * stride, img + (size_t)(h - 1 - y) * stride, stride);
    }
    GlobalUnlock(mem);
    free(img);
    if (!OpenClipboard(NULL)) { GlobalFree(mem); ms_log("PASTE: OpenClipboard failed"); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIBV5, mem)) { GlobalFree(mem); ms_log("PASTE: SetClipboardData(DIBV5) failed"); }
    else ms_log("PASTE: %dx%d CF_DIBV5 placed (send Ctrl+V)", w, h);
    CloseClipboard();
}

/* Read whatever SAI put on the clipboard (Ctrl+A / Ctrl+C). If SAI copies
 * the active layer as CF_DIB, the plugin gets exact document pixels through a
 * supported path instead of guessing which DIB/paper rectangle is live. */
static void clipboard_read(void) {
    if (!OpenClipboard(NULL)) { ms_log("CLIPREAD: OpenClipboard failed"); return; }
    HANDLE h = GetClipboardData(CF_DIB);
    if (!h) { CloseClipboard(); ms_log("CLIPREAD: no CF_DIB"); return; }
    unsigned char *p = (unsigned char*)GlobalLock(h);
    if (!p) { CloseClipboard(); ms_log("CLIPREAD: lock failed"); return; }
    BITMAPINFOHEADER *ih = (BITMAPINFOHEADER*)p;
    int w = ih->biWidth, hh = ih->biHeight;
    int bpp = ih->biBitCount, comp = ih->biCompression;
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    size_t imgsz = stride * (size_t)(hh < 0 ? -hh : hh);
    ms_log("CLIPREAD: %dx%d bpp=%d comp=%d imgsz=%zu hdr=%u",
           w, hh, bpp, comp, imgsz, ih->biSize);
    if (w > 0 && comp == 0 && imgsz > 0 && imgsz <= (48u << 20) &&
        (bpp == 24 || bpp == 32)) {
        /* Normalise to 32bpp BGRA, top-down, so the liquify engine has one
         * format to care about regardless of what SAI puts on the clipboard. */
        const unsigned char *src = p + ih->biSize;
        int aw = w, ah = hh < 0 ? -hh : hh;
        size_t sstride = (((size_t)w * bpp + 31) / 32) * 4;
        size_t dstride = (size_t)aw * 4;
        unsigned char *out = (unsigned char*)malloc(dstride * (size_t)ah);
        if (out) {
            for (int y = 0; y < ah; ++y) {
                int sy = (hh > 0) ? (ah - 1 - y) : y;   /* CF_DIB is bottom-up */
                const unsigned char *srow = src + (size_t)sy * sstride;
                unsigned char *drow = out + (size_t)y * dstride;
                for (int x = 0; x < aw; ++x) {
                    if (bpp == 32) memcpy(drow + (size_t)x * 4, srow + (size_t)x * 4, 4);
                    else {
                        drow[x*4+0] = srow[x*3+0];
                        drow[x*4+1] = srow[x*3+1];
                        drow[x*4+2] = srow[x*3+2];
                        drow[x*4+3] = 0xFF;
                    }
                }
            }
            FILE *f = fopen("C:\\wt_clipin.bin", "wb");
            if (f) { fwrite(out, 1, dstride * (size_t)ah, f); fclose(f); }
            FILE *g = fopen("C:\\wt_clipin.txt", "wb");
            if (g) { fprintf(g, "%d %d 32\n", aw, ah); fclose(g); }
            ms_log("CLIPREAD: dumped %dx%d (from bpp=%d) to C:\\wt_clipin.bin", aw, ah, bpp);
            free(out);
        } else {
            ms_log("CLIPREAD: out of memory for %dx%d", aw, ah);
        }
    }
    GlobalUnlock(h);
    CloseClipboard();
}

/* ---- liquify prototype (C:\wt_lq) ---------------------------------------
 * Pipeline: SAI Ctrl+A/Ctrl+C -> wt_clipread (document pixels) -> warping here
 * -> wt_paste.bin on the clipboard -> SAI Ctrl+V (new layer).
 *
 * This is the whole product loop minus the interactive brush; the file
 * "C:\wt_lq.txt" holds "<mode> <x0> <y0> <x1> <y1> <radius> <strength> <steps>",
 * all in document pixels. Applying the same spec twice must give the same
 * pixels (the engine is idempotent per field), which is what the export test
 * checks.
 */
static int lq_mode_by_name(const char *s) {
    if (!strcmp(s, "forward")) return LQ_FORWARD;
    if (!strcmp(s, "bloat")) return LQ_BLOAT;
    if (!strcmp(s, "pinch")) return LQ_PINCH;
    if (!strcmp(s, "twirl_cw")) return LQ_TWIRL_CW;
    if (!strcmp(s, "twirl_ccw")) return LQ_TWIRL_CCW;
    if (!strcmp(s, "smooth")) return LQ_SMOOTH;
    return -1;
}

static void lq_run(void) {
    int w = 0, h = 0, bpp = 0;
    FILE *cf = fopen("C:\\wt_clipin.txt", "r");
    if (cf) { if (fscanf(cf, "%d %d %d", &w, &h, &bpp) != 3) w = h = 0; fclose(cf); }
    if (w <= 0 || h <= 0) { ms_log("LQ: no wt_clipin.txt"); return; }
    size_t stride = (size_t)w * 4;
    size_t imgsz = stride * (size_t)h;
    unsigned char *img = (unsigned char*)malloc(imgsz);
    FILE *pf = fopen("C:\\wt_clipin.bin", "rb");
    if (!pf || !img) { if (pf) fclose(pf); free(img); ms_log("LQ: no input image"); return; }
    size_t got = fread(img, 1, imgsz, pf);
    fclose(pf);
    if (got != imgsz) { free(img); ms_log("LQ: short read %zu/%zu", got, imgsz); return; }

    char mode[32] = "forward";
    float x0 = w * 0.4f, y0 = h * 0.5f, x1 = w * 0.6f, y1 = h * 0.5f;
    float radius = w * 0.1f, strength = 0.6f;
    int steps = 40;
    cf = fopen("C:\\wt_lq.txt", "r");
    if (cf) {
        if (fscanf(cf, "%31s %f %f %f %f %f %f %d", mode, &x0, &y0, &x1, &y1,
                   &radius, &strength, &steps) < 1) strcpy(mode, "forward");
        fclose(cf);
    }
    int m = lq_mode_by_name(mode);
    if (m < 0) { free(img); ms_log("LQ: unknown mode %s", mode); return; }
    if (steps < 1) steps = 1;
    if (steps > 2000) steps = 2000;

    DWORD t0 = GetTickCount();
    lq_ctx *c = lq_create(img, w, h, (int)stride);
    if (!c) { free(img); ms_log("LQ: lq_create failed"); return; }
    lq_stroke_begin(c, (int)x0, (int)y0, (int)radius);
    for (int i = 1; i <= steps; ++i) {
        float t = (float)i / (float)steps;
        float px = x0 + (x1 - x0) * t, py = y0 + (y1 - y0) * t;
        lq_apply(c, (lq_mode)m, x0 + (x1 - x0) * (t - 1.0f / steps),
                 y0 + (y1 - y0) * (t - 1.0f / steps), px, py, radius, strength);
    }
    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    lq_flush(c, &minx, &miny, &maxx, &maxy);
    DWORD t1 = GetTickCount();
    lq_stroke_end(c);
    lq_destroy(c);

    FILE *of = fopen("C:\\wt_paste.bin", "wb");
    if (of) { fwrite(img, 1, imgsz, of); fclose(of); }
    FILE *tf = fopen("C:\\wt_paste.txt", "wb");
    if (tf) { fprintf(tf, "%d %d", w, h); fclose(tf); }
    ms_log("LQ: mode=%s %dx%d bbox=%d,%d..%d,%d %lums -> wt_paste.bin",
           mode, w, h, minx, miny, maxx, maxy, (unsigned long)(t1 - t0));
    free(img);
    doc_paste();
}

/* ---- Android-side processing over localhost ---------------------------
 * The DLL only shuttles pixels; the five modes run in the app's Java engine
 * (LiquifyServer / LiquifyWarp), which is native ARM code instead of Box64.
 *
 * Request : 'LQ01', w,h,mode,steps, x0,y0,x1,y1,radius,strength, len, pixels
 * Response: status, len, pixels
 * Trigger : C:\wt_lqnet with "<mode> <x0> <y0> <x1> <y1> <radius> <strength> <steps>"
 */
static int lq_net_ex(const char *specpath) {
    int w = 0, h = 0, bpp = 0;
    FILE *cf = fopen("C:\\wt_clipin.txt", "r");
    if (cf) { if (fscanf(cf, "%d %d %d", &w, &h, &bpp) != 3) w = h = 0; fclose(cf); }
    if (w <= 0 || h <= 0) { ms_log("LQNET: no wt_clipin.txt"); return 0; }
    size_t imgsz = (size_t)w * 4 * (size_t)h;
    unsigned char *img = (unsigned char*)malloc(imgsz);
    FILE *pf = fopen("C:\\wt_clipin.bin", "rb");
    if (!pf || !img) { if (pf) fclose(pf); free(img); ms_log("LQNET: no input"); return 0; }
    size_t got = fread(img, 1, imgsz, pf);
    fclose(pf);
    if (got != imgsz) { free(img); ms_log("LQNET: short read"); return 0; }

    char mode[32] = "forward"; int m = 0;
    float x0 = w * 0.4f, y0 = h * 0.5f, x1 = w * 0.6f, y1 = h * 0.5f;
    float radius = w * 0.1f, strength = 0.6f;
    int steps = 40;
    cf = fopen(specpath, "r");
    if (cf) {
        if (fscanf(cf, "%31s %f %f %f %f %f %f %d", mode, &x0, &y0, &x1, &y1,
                   &radius, &strength, &steps) < 1) strcpy(mode, "forward");
        fclose(cf);
    }
    if (!strcmp(mode, "bloat")) m = 1;
    else if (!strcmp(mode, "pinch")) m = 2;
    else if (!strcmp(mode, "twirl_cw")) m = 3;
    else if (!strcmp(mode, "twirl_ccw")) m = 4;
    else if (!strcmp(mode, "smooth")) m = 5;
    else m = 0;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { free(img); ms_log("LQNET: WSAStartup failed"); return 0; }
    SOCKET sk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sk == INVALID_SOCKET) { free(img); WSACleanup(); ms_log("LQNET: socket failed"); return 0; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(47801);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sk, (struct sockaddr*)&sa, sizeof sa) != 0) {
        closesocket(sk); WSACleanup(); free(img);
        ms_log("LQNET: connect 127.0.0.1:47801 failed (server not running?)");
        return 0;
    }
    DWORD t0 = GetTickCount();
    int ok = 1;
    /* Java's DataInputStream is big-endian; the first version sent host
     * (little-endian) words, the server rejected the request and closed. */
    unsigned int u;
    u = htonl(0x4C513031u); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    u = htonl((unsigned)w); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    u = htonl((unsigned)h); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    u = htonl((unsigned)m); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    u = htonl((unsigned)steps); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    float f;
    f = x0; u = htonl(*(unsigned*)&f); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    f = y0; u = htonl(*(unsigned*)&f); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    f = x1; u = htonl(*(unsigned*)&f); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    f = y1; u = htonl(*(unsigned*)&f); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    f = radius; u = htonl(*(unsigned*)&f); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    f = strength; u = htonl(*(unsigned*)&f); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    u = htonl((unsigned)imgsz); ok &= send(sk, (const char*)&u, 4, 0) == 4;
    size_t sent = 0;
    while (ok && sent < imgsz) {
        int n = send(sk, (const char*)img + sent, (int)(imgsz - sent), 0);
        if (n <= 0) { ok = 0; break; }
        sent += (size_t)n;
    }
    unsigned int status = 0xFFFFFFFF, rlen = 0;
    int rn = recv(sk, (char*)&status, 4, 0);
    if (rn != 4) ok = 0; else status = ntohl(status);
    if (ok) { rn = recv(sk, (char*)&rlen, 4, 0); if (rn != 4) ok = 0; else rlen = ntohl(rlen); }
    if (ok && status == 0 && rlen == imgsz) {
        size_t got2 = 0;
        while (got2 < imgsz) {
            int n = recv(sk, (char*)img + got2, (int)(imgsz - got2), 0);
            if (n <= 0) { ok = 0; break; }
            got2 += (size_t)n;
        }
    } else ok = 0;
    unsigned int ms = 0;
    if (ok) { int rn2 = recv(sk, (char*)&ms, 4, 0); if (rn2 == 4) ms = ntohl(ms); }
    closesocket(sk);
    WSACleanup();
    DWORD t1 = GetTickCount();
    if (!ok) { free(img); ms_log("LQNET: request failed (status=%u len=%u)", status, rlen); return 0; }

    FILE *of = fopen("C:\\wt_paste.bin", "wb");
    if (of) { fwrite(img, 1, imgsz, of); fclose(of); }
    FILE *tf = fopen("C:\\wt_paste.txt", "wb");
    if (tf) { fprintf(tf, "%d %d", w, h); fclose(tf); }
    ms_log("LQNET: mode=%s %dx%d round trip %lums (java warp %ums)",
           mode, w, h, (unsigned long)(t1 - t0), ms);
    free(img);
    doc_paste();
    return 1;
}

/* ---- in-process liquify flow (C:\wt_auto) ------------------------------
 * Up to now the copy/warp/paste steps were driven from the host with adb
 * keystrokes. A real plugin has to do it itself: SendInput Ctrl+A / Ctrl+C,
 * read the CF_DIB, run the warp, publish the result and SendInput Ctrl+V.
 *
 * The spec comes from C:\wt_lq.txt (same format as wt_lq), so the same trigger
 * can request any of the five modes.
 */
static void send_vk(WORD vk, int up) {
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

static void send_ctrl_key(WORD vk) {
    send_vk(VK_CONTROL, 0);
    send_vk(vk, 0);
    Sleep(30);
    send_vk(vk, 1);
    send_vk(VK_CONTROL, 1);
}

static int clipboard_has_dib(void) {
    if (!OpenClipboard(NULL)) return 0;
    int has = GetClipboardData(CF_DIB) != NULL;
    CloseClipboard();
    return has;
}

/* Like auto_flow, but copies the current *selection* instead of the whole
 * canvas: SAI's Ctrl+C then yields only the selection's bounding box, and the
 * paste lands back at the original position, so the warp stays inside the
 * selection. */
static void auto_flow_sel(void) {
    if (OpenClipboard(NULL)) { EmptyClipboard(); CloseClipboard(); }
    send_ctrl_key('C');
    int got = 0;
    for (int i = 0; i < 40; ++i) { Sleep(100); if (clipboard_has_dib()) { got = 1; break; } }
    if (!got) { ms_log("AUTOSEL: SAI did not copy a selection"); return; }
    clipboard_read();
    if (!lq_net_ex("C:\\wt_lq.txt")) {
        ms_log("AUTOSEL: server unavailable, warping locally");
        lq_run();
    }
    Sleep(150);
    send_ctrl_key('V');
    ms_log("AUTOSEL: done (selection warped in place)");
}

static void auto_flow(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) { ms_log("AUTO: no foreground window"); return; }
    ms_log("AUTO: foreground=%p class=%s", (void*)fg, gdi_class_of(GetDC(fg)));

    /* drop any stale clipboard content so "has DIB" means SAI copied */
    if (OpenClipboard(NULL)) { EmptyClipboard(); CloseClipboard(); }

    send_ctrl_key('A');
    Sleep(120);
    send_ctrl_key('C');
    int got = 0;
    for (int i = 0; i < 40; ++i) {
        Sleep(100);
        if (clipboard_has_dib()) { got = 1; break; }
    }
    if (!got) { ms_log("AUTO: SAI did not put a CF_DIB on the clipboard"); return; }
    clipboard_read();
    send_ctrl_key('D');           /* drop the marching-ants selection */

    /* the server path is the product one; fall back to the local engine when
     * the app was not patched (e.g. a stock build) */
    if (!lq_net_ex("C:\\wt_lq.txt")) {
        ms_log("AUTO: server unavailable, warping locally");
        lq_run();
    }
    Sleep(150);
    send_ctrl_key('V');
    ms_log("AUTO: done (pasted as a new layer)");
}

/* ---- interactive liquify overlay (C:\wt_ui) ---------------------------
 * The prototype until now was "press a key, get a result". A usable plugin
 * needs a brush: this creates a layered window exactly over the canvas paper,
 * shows the document pixels in it, warps them under a drag with the same lq.c
 * engine (only the dirty tiles are rebuilt, so a 100 px brush costs a few ms
 * even under Box64), and commits with a right click (publish + Ctrl+V).
 *
 * C:\wt_ui.txt: "<mode> <radius> <strength>"
 * Mouse: left drag = warp, right click = commit, Esc = cancel.
 */
typedef struct {
    unsigned char *doc;
    int dw, dh;
    lq_ctx *ctx;
    int mode, radius;
    float strength;
    int down;
    float lastx, lasty;
} UiState;

static UiState g_ui;
static HWND g_ui_hwnd;
static HDC g_ui_dc;
static HBITMAP g_ui_bmp;
static unsigned char *g_ui_bits;
typedef struct { HWND best; LONG area; } UiCanvasFind;

static BOOL CALLBACK ui_enum_canvas(HWND h, LPARAM lp) {
    UiCanvasFind *st = (UiCanvasFind *)lp;
    RECT r;
    if (!IsWindowVisible(h)) return TRUE;
    GetClientRect(h, &r);
    LONG a = (r.right - r.left) * (r.bottom - r.top);
    if (r.right - r.left < 200 || r.bottom - r.top < 150) return TRUE;
    char cls[64] = "";
    GetClassNameA(h, cls, (int)sizeof cls - 1);
    if (strstr(cls, "sfl") && a > st->area) { st->best = h; st->area = a; }
    return TRUE;
}

/* The canvas child is where the document is drawn. The GDI hooks discover it
 * when the capture switch is on, but the plugin must work without them. */
static HWND ui_canvas_hwnd(void) {
    if (g_cv_dst && IsWindow(g_cv_dst)) return g_cv_dst;
    if (!g_hwnd) return NULL;
    UiCanvasFind st = { NULL, 0 };
    EnumChildWindows(g_hwnd, ui_enum_canvas, (LPARAM)&st);
    return st.best ? st.best : g_hwnd;
}

static void ui_update_window(void);
static void ui_render_rect(int dx0, int dy0, int dx1, int dy1);
static void ui_draw_bar(void);
static void ui_draw_status(void);
static void ui_commit(void);
static void ui_close(void);

static int g_ui_w, g_ui_h, g_ui_x, g_ui_y;   /* g_ui_w/h = visible paper size */
static float g_ui_sc = 1.0f;                  /* doc px -> surface px */
static float g_ui_offx, g_ui_offy;            /* doc coords at surface (0,bar+h) */
static int g_doc_offx, g_doc_offy;            /* content offset inside the document */
static int g_paper_x, g_paper_y;              /* paper top-left, screen coords */
static int g_paper_w, g_paper_h;              /* paper size on screen */
static int g_cv_screen_x, g_cv_screen_y;      /* canvas client origin, screen coords */
static volatile int g_ui_starting;
#define UI_BAR_H 34
static int g_ui_clicks;                        /* taps seen, so a tap can act */

static const char *UI_MODES[7] = { "FWD", "BLT", "PNC", "CW", "CCW", "SMO", "REC" };

#define UI_BTN_N 13
static int g_ui_merge;   /* merge into the layer below after commit */

/* current radius shown on the paper so the size is always visible */
/* Find the captured content inside the canvas viewport so a trimmed copy can
 * be put back where it came from. Returns 1 on a confident match and fills the
 * content offset in document pixels. */
/* The canvas child shows the paper rectangle over a uniform grey backdrop.
 * Detect that rectangle: it is also the only way to know the real document
 * size, since SAI trims its clipboard copy to the painted content. */
static int g_paper_dib_x, g_paper_dib_y, g_paper_dib_w, g_paper_dib_h;
static int ui_detect_paper(void) {
    if (!g_cv_bits || g_cv_w < 64 || g_cv_h < 64) return 0;
    const unsigned char *d = (const unsigned char *)g_cv_bits;
    int bb = d[0], bg = d[1], br = d[2];
    int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
    for (int y = 0; y < g_cv_h; y += 2) {
        const unsigned char *row = d + (size_t)y * g_cv_stride;
        for (int x = 0; x < g_cv_w; x += 2) {
            const unsigned char *p = row + (size_t)x * 4;
            int diff = abs(p[0] - bb) + abs(p[1] - bg) + abs(p[2] - br);
            if (diff > 24) {
                if (x0 < 0 || x < x0) x0 = x;
                if (y0 < 0 || y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x0 < 0 || x1 - x0 < 32 || y1 - y0 < 32) return 0;
    g_paper_dib_x = x0; g_paper_dib_y = y0;
    g_paper_dib_w = x1 - x0; g_paper_dib_h = y1 - y0;
    return 1;
}

#define LOC_N 10
static int ui_locate_content(void) {
    if (!g_cv_bits || !g_ui.doc || g_ui.dw < 16 || g_ui.dh < 16) return 0;
    const int cw = g_ui.dw, ch = g_ui.dh;
    if (cw > g_cv_w || ch > g_cv_h) return 0;
    unsigned char cap[LOC_N * LOC_N];
    int mean = 0, sq = 0;
    for (int gy = 0; gy < LOC_N; ++gy) {
        for (int gx = 0; gx < LOC_N; ++gx) {
            int x = gx * cw / LOC_N, y = gy * ch / LOC_N;
            const unsigned char *p = g_ui.doc + ((size_t)y * cw + x) * 4;
            int v = (p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8;
            cap[gy * LOC_N + gx] = (unsigned char)v;
            mean += v; sq += v * v;
        }
    }
    int n = LOC_N * LOC_N;
    mean /= n;
    /* a flat capture cannot be placed by matching */
    if (sq / n - mean * mean < 16) return 0;

    const unsigned char *dib = (const unsigned char *)g_cv_bits;
    int bestx = -1, besty = -1;
    long bestd = (long)1 << 60;
    const int step = 6;
    for (int vy = 0; vy + ch <= g_cv_h; vy += step) {
        for (int vx = 0; vx + cw <= g_cv_w; vx += step) {
            long d = 0;
            for (int gy = 0; gy < LOC_N && d < bestd; ++gy) {
                const unsigned char *row = dib + (size_t)(vy + gy * ch / LOC_N) * g_cv_stride;
                for (int gx = 0; gx < LOC_N; ++gx) {
                    const unsigned char *p = row + (size_t)(vx + gx * cw / LOC_N) * 4;
                    int v = (p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8;
                    int diff = v - cap[gy * LOC_N + gx];
                    d += (long)diff * diff;
                }
            }
            if (d < bestd) { bestd = d; bestx = vx; besty = vy; }
        }
    }
    if (bestx < 0) return 0;
    /* A weak match means the content is hidden, the snapshot is stale or the
     * capture is uniform: guessing an offset there is what pushed earlier
     * pastes far off the canvas. Refuse instead (same rule as the reference
     * implementation: never pad with an unverified position). */
    if (bestd > (long)LOC_N * LOC_N * 900L) {
        ms_log("UI: locate weak (d=%ld), not padding", bestd);
        return 0;
    }
    g_doc_offx = bestx - g_paper_dib_x;
    g_doc_offy = besty - g_paper_dib_y;
    if (g_doc_offx < 0 || g_doc_offy < 0) return 0;
    /* the padded image must stay inside the paper we detected */
    if (g_paper_dib_w > 32 && g_paper_dib_h > 32) {
        if (g_doc_offx + g_ui.dw > g_paper_dib_w + 4 ||
            g_doc_offy + g_ui.dh > g_paper_dib_h + 4) {
            ms_log("UI: offset %d,%d out of paper %dx%d", g_doc_offx, g_doc_offy,
                   g_paper_dib_w, g_paper_dib_h);
            return 0;
        }
    }
    return 1;
}

static void ui_draw_status(void) {
    if (!g_ui_bits) return;
    int bw = g_ui_w, bh = 16;
    RECT r = { 0, UI_BAR_H + g_ui_h - bh, bw, UI_BAR_H + g_ui_h };
    HBRUSH b = CreateSolidBrush(RGB(20, 20, 20));
    FillRect(g_ui_dc, &r, b);
    DeleteObject(b);
    SetBkMode(g_ui_dc, TRANSPARENT);
    SetTextColor(g_ui_dc, RGB(200, 220, 255));
    char t[64];
    int n = snprintf(t, sizeof t, "R=%d  %s  P=%d%%%s", g_ui.radius,
                     UI_MODES[g_ui.mode], g_last_press * 100 / 4096,
                     g_ui_merge ? "  MERGE" : "");
    TextOutA(g_ui_dc, 4, r.top + 1, t, n);
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x)
            g_ui_bits[((size_t)y * g_ui_w + x) * 4 + 3] = 0xFF;
}

/* brush ring drawn straight into the layered surface, alternating colours so
 * it stays visible on white and on the artwork */
static int g_cur_x = -1, g_cur_y = -1, g_cur_r = -1;

static void ui_draw_cursor(int cx, int cy, int rad) {
    if (!g_ui_bits) return;
    for (int rr = rad - 1; rr <= rad; ++rr) {
        for (int a = 0; a < 720; ++a) {
            double th = a * 3.14159265358979 / 360.0;
            int x = cx + (int)(rr * cos(th));
            int y = cy + (int)(rr * sin(th));
            if (x < 0 || y < UI_BAR_H || x >= g_ui_w || y >= UI_BAR_H + g_ui_h) continue;
            unsigned char *px = g_ui_bits + ((size_t)y * g_ui_w + x) * 4;
            int on = (a / 24) & 1;
            px[0] = on ? 255 : 0; px[1] = on ? 255 : 0; px[2] = on ? 255 : 0; px[3] = 0xFF;
        }
    }
}

static void ui_erase_ring(void);
static void ui_refresh_ring(int cx, int cy) {
    int rad = (int)(g_ui.radius * g_ui_sc);
    if (rad < 3) rad = 3;
    if (g_cur_r == rad && g_cur_x == cx && g_cur_y == cy) return;
    /* every so often repaint the whole surface: incremental erasing can leave
     * a faint trail on slow compositors like this one */
    static int ticks;
    if ((++ticks % 40) == 0) {
        ui_render_rect(0, 0, g_ui.dw - 1, g_ui.dh - 1);
        ui_draw_bar();
        ui_draw_status();
        g_cur_r = -1;
    }
    ui_erase_ring();
    ui_draw_cursor(cx, cy, rad);
    g_cur_x = cx; g_cur_y = cy; g_cur_r = rad;
}

static void ui_erase_ring(void) {
    if (g_cur_r <= 0) return;
    int sx0 = g_cur_x - g_cur_r - 2, sx1 = g_cur_x + g_cur_r + 2;
    int sy0 = g_cur_y - g_cur_r - 2, sy1 = g_cur_y + g_cur_r + 2;
    int dx0 = sx0 * g_ui.dw / g_ui_w, dx1 = sx1 * g_ui.dw / g_ui_w;
    int dy0 = (sy0 - UI_BAR_H) * g_ui.dh / g_ui_h;
    int dy1 = (sy1 - UI_BAR_H) * g_ui.dh / g_ui_h;
    if (dx1 > dx0 && dy1 > dy0) ui_render_rect(dx0, dy0, dx1, dy1);
}

static void ui_bar_rect(int i, RECT *r) {
    int bw = g_ui_w / UI_BTN_N;
    if (bw < 24) bw = 24;
    r->left = i * bw;
    r->right = r->left + bw - 2;
    r->top = 3;
    r->bottom = UI_BAR_H - 3;
}

static void ui_draw_bar(void) {
    HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
    RECT all = { 0, 0, g_ui_w, UI_BAR_H };
    FillRect(g_ui_dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(g_ui_dc, TRANSPARENT);
    for (int i = 0; i < UI_BTN_N; ++i) {
        RECT r; ui_bar_rect(i, &r);
        int active = (i < 7 && i == g_ui.mode) || (i == 7 && g_ui_merge);
        HBRUSH b = CreateSolidBrush(active ? RGB(0, 120, 215) : RGB(70, 70, 70));
        FillRect(g_ui_dc, &r, b);
        DeleteObject(b);
        SetTextColor(g_ui_dc, RGB(235, 235, 235));
        const char *label;
        char buf[8];
        if (i < 7) label = UI_MODES[i];
        else if (i == 7) label = "MRG";
        else if (i == 8) label = "UND";
        else if (i == 9) label = "R-";
        else if (i == 10) label = "R+";
        else if (i == 11) label = "OK";
        else label = "X";
        TextOutA(g_ui_dc, r.left + 4, r.top + 8, label, (int)strlen(label));
        (void)buf;
    }
    /* GDI writes into a 32bpp DIB with alpha 0; UpdateLayeredWindow would make
     * the whole bar invisible, so force it opaque. */
    for (int y = 0; y < UI_BAR_H && y < g_ui_h; ++y)
        for (int x = 0; x < g_ui_w; ++x)
            g_ui_bits[((size_t)y * g_ui_w + x) * 4 + 3] = 0xFF;
}

static void ui_bar_click(int ox, int oy) {
    (void)oy;
    for (int i = 0; i < UI_BTN_N; ++i) {
        RECT r; ui_bar_rect(i, &r);
        if (ox < r.left || ox > r.right) continue;
        if (i < 7) g_ui.mode = i;
        else if (i == 7) g_ui_merge = !g_ui_merge;
        else if (i == 8) {
            if (g_ui.ctx && lq_undo(g_ui.ctx)) {
                ui_render_rect(0, 0, g_ui.dw - 1, g_ui.dh - 1);
                ui_draw_bar();
                ui_draw_status();
                ui_update_window();
            }
            return;
        }
        else if (i == 9) { g_ui.radius = (int)(g_ui.radius * 3 / 4); if (g_ui.radius < 8) g_ui.radius = 8; }
        else if (i == 10) { g_ui.radius = (int)(g_ui.radius * 4 / 3); if (g_ui.radius > 1024) g_ui.radius = 1024; }
        else if (i == 11) { ui_commit(); ui_close(); return; }
        else { ui_close(); return; }
        if (g_ui.ctx) { lq_destroy(g_ui.ctx); g_ui.ctx = NULL; }
        ui_draw_bar();
        ui_draw_status();
        ui_update_window();
        ms_log("UI: button %d -> mode=%d radius=%d", i, g_ui.mode, g_ui.radius);
        return;
    }
}

static void ui_render_rect(int dx0, int dy0, int dx1, int dy1) {
    if (!g_ui_bits || !g_ui.doc) return;
    /* Work in surface pixels and clear whatever falls outside the document,
     * otherwise the brush ring leaves ghost pixels at the paper edges. */
    int sx0 = (int)((dx0 - g_ui_offx) * g_ui_sc);
    int sx1 = (int)((dx1 - g_ui_offx) * g_ui_sc);
    int sy0 = (int)((dy0 - g_ui_offy) * g_ui_sc);
    int sy1 = (int)((dy1 - g_ui_offy) * g_ui_sc);
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > g_ui_w - 1) sx1 = g_ui_w - 1;
    if (sy1 > g_ui_h - 1) sy1 = g_ui_h - 1;
    for (int oy = sy0; oy <= sy1; ++oy) {
        int sy = (int)(g_ui_offy + oy / g_ui_sc);
        for (int ox = sx0; ox <= sx1; ++ox) {
            int sx = (int)(g_ui_offx + ox / g_ui_sc);
            unsigned char *dst = g_ui_bits + ((size_t)(oy + UI_BAR_H) * g_ui_w + ox) * 4;
            if (sx < 0 || sx >= g_ui.dw || sy < 0 || sy >= g_ui.dh) {
                dst[3] = 0;
                continue;
            }
            const unsigned char *src = g_ui.doc + ((size_t)sy * g_ui.dw + sx) * 4;
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0xFF;
        }
    }
}

static void ui_update_window(void) {
    if (!g_ui_hwnd) return;
    HDC screen = GetDC(NULL);
    POINT pt = { g_ui_x, g_ui_y };
    SIZE sz = { g_ui_w, g_ui_h + UI_BAR_H };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_ui_hwnd, screen, &pt, &sz, g_ui_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

static void send_plain_key(char c);

/* Hold E while the pen button / eraser is active. SAI's "hold E = temporary
 * eraser" mode switches to the eraser on key-down and restores the previous
 * tool on key-up, so the user's brush is never replaced. */
static void send_key_state(char c, int down) {
    SHORT vk = (SHORT)VkKeyScanA(c);
    if (vk == -1) { ms_log("KEY: no vk for %c", c); return; }
    BYTE v = (BYTE)(vk & 0xFF);
    keybd_event(v, 0, down ? 0 : KEYEVENTF_KEYUP, 0);
    HWND t = g_hwnd ? g_hwnd : GetForegroundWindow();
    if (t) {
        if (down) PostMessageA(t, WM_KEYDOWN, v, 1);
        else      PostMessageA(t, WM_KEYUP, v, 1 | 0xC0000000);
    }
    ms_log("KEY: %c %s (vk=%d, target=%p)", c, down ? "down" : "up", v, (void*)t);
}

static void send_plain_key(char c) {
    send_key_state(c, 1);
    Sleep(30);
    send_key_state(c, 0);
}

static void ui_commit(void) {
    if (!g_ui.doc) return;
    int w = g_ui.dw, h = g_ui.dh;
    unsigned char *img = g_ui.doc, *pad = NULL;
    if (g_doc_offx > 0 || g_doc_offy > 0) {
        int pw = w + g_doc_offx, ph = h + g_doc_offy;
        pad = (unsigned char*)calloc((size_t)pw * 4 * ph, 1);
        if (pad) {
            for (int y = 0; y < h; ++y)
                memcpy(pad + ((size_t)(y + g_doc_offy) * pw + g_doc_offx) * 4,
                       g_ui.doc + (size_t)y * w * 4, (size_t)w * 4);
            img = pad; w = pw; h = ph;
        }
    }
    FILE *f = fopen("C:\\wt_paste.bin", "wb");
    if (f) { fwrite(img, 1, (size_t)w * 4 * h, f); fclose(f); }
    FILE *t = fopen("C:\\wt_paste.txt", "wb");
    if (t) { fprintf(t, "%d %d", w, h); fclose(t); }
    free(pad);
    doc_paste();
    /* the overlay owns the focus; hand it back to SAI or the Ctrl+V lands on
     * the (about to be destroyed) overlay window */
    if (g_hwnd) { SetForegroundWindow(g_hwnd); SetFocus(g_hwnd); }
    Sleep(250);
    send_ctrl_key('V');
    if (g_ui_merge) {
        /* 图层 → 合并图层 (Ctrl+E): replaces the layer below, which is what
         * "liquify in place" means for a single-layer document. */
        Sleep(600);
        send_ctrl_key('E');
        ms_log("UI: committed %dx%d and merged down", g_ui.dw, g_ui.dh);
    } else {
        ms_log("UI: committed %dx%d as a new layer", g_ui.dw, g_ui.dh);
    }
}

static void ui_close(void) {
    g_ui_starting = 0;
    if (g_ui.ctx) { lq_destroy(g_ui.ctx); g_ui.ctx = NULL; }
    if (g_ui.doc) { free(g_ui.doc); g_ui.doc = NULL; }
    if (g_ui_bmp) { DeleteObject(g_ui_bmp); g_ui_bmp = NULL; }
    if (g_ui_dc) { DeleteDC(g_ui_dc); g_ui_dc = NULL; }
    if (g_ui_hwnd) { DestroyWindow(g_ui_hwnd); g_ui_hwnd = NULL; }
}

static LRESULT CALLBACK ui_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_RBUTTONUP)
        ms_log("UI: msg 0x%x at %d,%d", (unsigned)m, (int)LOWORD(l), (int)HIWORD(l));
    switch (m) {
    case WM_LBUTTONDOWN: {
        int cy = (int)HIWORD(l);
        if (cy < UI_BAR_H) {
            ui_bar_click((int)LOWORD(l), cy);
            return 0;
        }
        float dx = g_ui_offx + (float)LOWORD(l) / g_ui_sc;
        float dy = g_ui_offy + (float)(cy - UI_BAR_H) / g_ui_sc;
        ms_log("UI: down at doc %.0f,%.0f", dx, dy);
        SetCapture(h);
        g_ui.down = 1;
        g_ui.lastx = dx; g_ui.lasty = dy;
        if (!g_ui.ctx) g_ui.ctx = lq_create(g_ui.doc, g_ui.dw, g_ui.dh, g_ui.dw * 4);
        if (g_ui.ctx) lq_stroke_begin(g_ui.ctx, (int)dx, (int)dy, g_ui.radius);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_ui.down || !g_ui.ctx) return 0;
        float dx = (float)LOWORD(l) * g_ui.dw / g_ui_w;
        float dy = (float)HIWORD(l) * g_ui.dh / g_ui_h;
        static int moves;
        if ((moves++ % 10) == 0)
            ms_log("UI: move doc %.0f,%.0f", dx, dy);
        lq_apply(g_ui.ctx, (lq_mode)g_ui.mode, g_ui.lastx, g_ui.lasty, dx, dy,
                 (float)g_ui.radius, g_ui.strength);
        g_ui.lastx = dx; g_ui.lasty = dy;
        int a[4];
        lq_flush(g_ui.ctx, &a[0], &a[1], &a[2], &a[3]);
        if (a[0] >= 0) {
            ui_render_rect(a[0], a[1], a[2], a[3]);
            ui_update_window();
        }
        return 0;
    }
    case WM_LBUTTONUP:
        ms_log("UI: up");
        g_ui.down = 0;
        ReleaseCapture();
        return 0;
    case WM_RBUTTONUP:
        ui_commit();
        ui_close();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { ui_close(); }
        else if (w == VK_RETURN) { ui_commit(); ui_close(); }
        return 0;
    case WM_CLOSE:
        ui_close();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI ui_thread(LPVOID p) {
    (void)p;
    /* The Android-side button takes the focus, so hand it back to SAI before
     * sending Ctrl+A/C - otherwise the copy never happens. */
    if (g_hwnd) {
        SetForegroundWindow(g_hwnd);
        SetFocus(g_hwnd);
        Sleep(400);
    }
    /* 1. capture the document through SAI's own clipboard copy.
     * Deselect first: with an active selection Ctrl+C copies just its bounding
     * box, which looks like a "wrong paper size" to the user. Ctrl+A alone
     * should replace it, but SAI keeps the old selection when the key never
     * reaches it, so drop it explicitly. */
    if (OpenClipboard(NULL)) { EmptyClipboard(); CloseClipboard(); }
    send_ctrl_key('D');
    Sleep(120);
    send_ctrl_key('A');
    Sleep(200);
    send_ctrl_key('C');
    int got = 0;
    for (int i = 0; i < 40; ++i) { Sleep(100); if (clipboard_has_dib()) { got = 1; break; } }
    if (!got) { ms_log("UI: no CF_DIB from SAI"); return 0; }
    clipboard_read();
    send_ctrl_key('D');

    int w = 0, h = 0, bpp = 0;
    FILE *cf = fopen("C:\\wt_clipin.txt", "r");
    if (cf) { if (fscanf(cf, "%d %d %d", &w, &h, &bpp) != 3) w = h = 0; fclose(cf); }
    if (w <= 0 || h <= 0) { ms_log("UI: no document image"); return 0; }
    g_ui.doc = (unsigned char*)malloc((size_t)w * 4 * h);
    FILE *pf = fopen("C:\\wt_clipin.bin", "rb");
    if (!g_ui.doc || !pf) { if (pf) fclose(pf); ui_close(); ms_log("UI: read failed"); return 0; }
    size_t got2 = fread(g_ui.doc, 1, (size_t)w * 4 * h, pf);
    fclose(pf);
    if (got2 != (size_t)w * 4 * h) { ui_close(); return 0; }
    g_ui.dw = w; g_ui.dh = h;

    /* 2. mode/radius/strength */
    g_ui.mode = 0; g_ui.radius = w / 8; g_ui.strength = 0.7f;
    char mspec[32] = "forward";
    cf = fopen("C:\\wt_ui.txt", "r");
    if (cf) {
        if (fscanf(cf, "%31s %d %f", mspec, &g_ui.radius, &g_ui.strength) < 1)
            strcpy(mspec, "forward");
        fclose(cf);
    }
    if (!strcmp(mspec, "bloat")) g_ui.mode = 1;
    else if (!strcmp(mspec, "pinch")) g_ui.mode = 2;
    else if (!strcmp(mspec, "twirl_cw")) g_ui.mode = 3;
    else if (!strcmp(mspec, "twirl_ccw")) g_ui.mode = 4;
    else if (!strcmp(mspec, "smooth")) g_ui.mode = 5;

    /* 3. paper rect: zoom from the title, origin from the last blit so a
     * scrolled / 100% document is covered correctly, clipped to the canvas. */
    HWND canvas = ui_canvas_hwnd();
    RECT rc;
    POINT o = { 0, 0 };
    if (canvas) { GetClientRect(canvas, &rc); ClientToScreen(canvas, &o); }
    else { rc.left = 0; rc.top = 0; rc.right = g_screenW; rc.bottom = g_screenH; }
    char title[128] = "";
    if (canvas) GetWindowTextA(canvas, title, (int)sizeof title - 1);
    int zoom = 100;
    /* the title only carries a zoom when it looks like "(150%)"; the document
     * name ends with "(*)" and atoi on that produced nonsense scales */
    const char *pc = strrchr(title, '(');
    if (pc && strchr(pc, '%') && strchr(pc, '%') < pc + 12) zoom = atoi(pc + 1);
    if (zoom <= 0 || zoom > 6400) zoom = 100;
    g_ui_sc = zoom / 100.0f;
    g_cv_screen_x = o.x;
    g_cv_screen_y = o.y;
    int pw, ph, pl, pt;
    if (0 && ui_detect_paper()) {
        /* the viewport knows the real paper rectangle even when the clipboard
         * copy was trimmed to the painted content */
        pw = g_paper_dib_w;
        ph = g_paper_dib_h;
        pl = o.x + g_paper_dib_x;
        pt = o.y + g_paper_dib_y;
        ms_log("UI: paper %dx%d at %d,%d (from viewport)", pw, ph, pl, pt);
    } else {
        pw = (int)(w * g_ui_sc);
        ph = (int)(h * g_ui_sc);
        pl = o.x + (rc.right - pw) / 2;
        pt = o.y + (rc.bottom - ph) / 2;
    }
    if (zoom == 100 && g_cv_cx >= 8 && g_cv_cy >= 8) {
        /* 1:1 blit: dest(x,y) shows document (sx,sy), so the paper origin is
         * exact. At other zooms the source coords are already viewport pixels
         * and scaling them again lands in the wrong place, so keep the centred
         * estimate there. */
        int ql = o.x + g_cv_x - g_cv_sx;
        int qt = o.y + g_cv_y - g_cv_sy;
        if (ql > o.x - pw && ql < o.x + rc.right && qt > o.y - ph && qt < o.y + rc.bottom) {
            pl = ql;
            pt = qt;
        }
    }
    g_paper_x = pl; g_paper_y = pt; g_paper_w = pw; g_paper_h = ph;
    g_doc_offx = 0; g_doc_offy = 0;
    int located = ui_locate_content();
    if (located) ms_log("UI: content at %d,%d in the document", g_doc_offx, g_doc_offy);

    int vl = pl, vt = pt, vr = pl + pw, vb = pt + ph;
    if (located) {
        /* cover the content itself, not the whole paper */
        vl = pl + (int)(g_doc_offx * g_ui_sc);
        vt = pt + (int)(g_doc_offy * g_ui_sc);
        vr = vl + (int)(w * g_ui_sc);
        vb = vt + (int)(h * g_ui_sc);
    }
    ms_log("UI: geom doc=%dx%d zoom=%d paper=%d,%d %dx%d", w, h, zoom, pl, pt, pw, ph);
    { int l0 = vl, t0 = vt;
      if (vl < o.x) vl = o.x;
      if (vt < o.y) vt = o.y;
      if (vr > o.x + rc.right) vr = o.x + rc.right;
      if (vb > o.y + rc.bottom) vb = o.y + rc.bottom;
      if (located) { g_doc_offx += (int)((vl - l0) / g_ui_sc); g_doc_offy += (int)((vt - t0) / g_ui_sc); }
    }
    if (vr - vl < 32 || vb - vt < 32) {
        /* implausible geometry: fall back to the centred estimate */
        pw = (int)(w * g_ui_sc);
        ph = (int)(h * g_ui_sc);
        pl = o.x + (rc.right - pw) / 2;
        pt = o.y + (rc.bottom - ph) / 2;
        vl = pl; vt = pt; vr = pl + pw; vb = pt + ph;
        if (vl < o.x) vl = o.x;
        if (vt < o.y) vt = o.y;
        if (vr > o.x + rc.right) vr = o.x + rc.right;
        if (vb > o.y + rc.bottom) vb = o.y + rc.bottom;
        ms_log("UI: geom fallback %d,%d %dx%d", vl, vt, vr - vl, vb - vt);
        if (vr - vl < 32 || vb - vt < 32) {
            ui_close();
            ms_log("UI: bad paper %dx%d", w, h);
            return 0;
        }
    }
    g_ui_w = vr - vl;
    g_ui_h = vb - vt;
    g_ui_x = vl;
    g_ui_y = vt - UI_BAR_H;
    g_ui_offx = (float)(vl - pl) / g_ui_sc;
    g_ui_offy = (float)(vt - pt) / g_ui_sc;
    /* A small document (or a high zoom) makes the paper narrower than the
     * toolbar, which shrinks the buttons to a few pixels. Keep the bar at a
     * usable width and let the paper sit inside the wider window. */
    if (g_ui_w < 620) {
        int wl = vl;
        if (wl + 620 > o.x + rc.right) wl = o.x + rc.right - 620;
        if (wl < o.x) wl = o.x;
        int wr = wl + 620;
        if (wr > o.x + rc.right) wr = o.x + rc.right;
        g_ui_w = wr - wl;
        g_ui_x = wl;
        g_ui_offx = (float)(wl - pl) / g_ui_sc;
    }

    /* 4. surface + window */
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_ui_w;
    bi.bmiHeader.biHeight = -(g_ui_h + UI_BAR_H);  /* top-down, bar included */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(NULL);
    g_ui_dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    g_ui_bmp = CreateDIBSection(g_ui_dc, &bi, DIB_RGB_COLORS, (void**)&g_ui_bits, NULL, 0);
    if (!g_ui_bmp || !g_ui_bits) { ui_close(); ms_log("UI: create surface failed"); return 0; }
    SelectObject(g_ui_dc, g_ui_bmp);
    ui_render_rect(0, 0, w - 1, h - 1);
    ui_draw_bar();
    ui_draw_status();

    WNDCLASSEXA wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ui_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LiquifyOverlay";
    RegisterClassExA(&wc);
    g_ui_hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                "LiquifyOverlay", "liquify", WS_POPUP,
                                g_ui_x, g_ui_y, g_ui_w, g_ui_h + UI_BAR_H,
                                NULL, NULL, wc.hInstance, NULL);
    if (!g_ui_hwnd) { ui_close(); ms_log("UI: create window failed"); return 0; }
    ui_update_window();
    ShowWindow(g_ui_hwnd, SW_SHOW);
    SetForegroundWindow(g_ui_hwnd);
    SetFocus(g_ui_hwnd);
    ms_log("UI: %dx%d doc, paper %dx%d at %d,%d, mode=%s radius=%d strength=%.2f",
           w, h, g_ui_w, g_ui_h, g_ui_x, g_ui_y, mspec, g_ui.radius, g_ui.strength);

    /* Winlator's touch path does not reliably deliver mouse messages to a
     * Wine popup (the drag just never arrived), so drive the brush from the
     * global cursor/button state instead. The window only renders. */
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_ui_hwnd) break;

        POINT p; GetCursorPos(&p);
        int in_x = p.x >= g_ui_x && p.x < g_ui_x + g_ui_w;
        int in_bar = in_x && p.y >= g_ui_y && p.y < g_ui_y + UI_BAR_H;
        int in_doc = in_x && p.y >= g_ui_y + UI_BAR_H &&
                     p.y < g_ui_y + UI_BAR_H + g_ui_h;
        int over = in_bar || in_doc;
        int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        float dx = g_ui_offx + (float)(p.x - g_ui_x) / g_ui_sc;
        float dy = g_ui_offy + (float)(p.y - g_ui_y - UI_BAR_H) / g_ui_sc;

        if (!down && in_doc) {
            int cx = (int)(p.x - g_ui_x);
            int cy = (int)(p.y - g_ui_y);
            int rad = (int)(g_ui.radius * g_ui_sc);
            if (rad < 3) rad = 3;
            (void)rad;
            if (abs(cx - g_cur_x) >= 2 || abs(cy - g_cur_y) >= 2) {
                ui_refresh_ring(cx, cy);
                ui_update_window();
            }
        } else if (g_cur_r > 0) {
            ui_erase_ring();
            ui_update_window();
            g_cur_r = -1;
        }
        if (down && in_bar) {
            /* clicks on the bar are handled by WM_LBUTTONDOWN; just make sure
             * the brush never starts there */
        } else if (down && in_doc) {
            if (!g_ui.down) {
                if (!g_ui.ctx)
                    g_ui.ctx = lq_create(g_ui.doc, g_ui.dw, g_ui.dh, g_ui.dw * 4);
                if (g_ui.ctx) lq_stroke_begin(g_ui.ctx, (int)dx, (int)dy, g_ui.radius);
                g_ui.down = 1;
                g_ui.lastx = dx; g_ui.lasty = dy;
                ms_log("UI: down at doc %.0f,%.0f", dx, dy);
            } else if (g_ui.ctx && (dx != g_ui.lastx || dy != g_ui.lasty)) {
                /* pen pressure drives the strength: light touch = subtle */
                float pmul = 1.0f;
                if (g_last_press > 0) {
                    float p = g_last_press / 4095.0f;
                    if (p > 1.0f) p = 1.0f;
                    pmul = 0.2f + 0.8f * p;
                }
                lq_apply(g_ui.ctx, (lq_mode)g_ui.mode, g_ui.lastx, g_ui.lasty, dx, dy,
                         (float)g_ui.radius, g_ui.strength * pmul);
                g_ui.lastx = dx; g_ui.lasty = dy;
                int a[4];
                lq_flush(g_ui.ctx, &a[0], &a[1], &a[2], &a[3]);
                if (a[0] >= 0) {
                    ui_render_rect(a[0], a[1], a[2], a[3]);
                    ui_refresh_ring(p.x - g_ui_x, p.y - g_ui_y);
                    ui_update_window();
                }
            }
        } else if (g_ui.down) {
            g_ui.down = 0;
            ms_log("UI: up at doc %.0f,%.0f (peak pressure %d/4095)",
                   dx, dy, g_ui_press_max);
            g_ui_press_max = 0;
        }

        /* The Winlator input path does not surface the keyboard or mouse
         * button state to Wine (GetAsyncKeyState stays 0), so the commit and
         * cancel actions are also exposed as files for now. */
        if (GetAsyncKeyState(VK_RETURN) & 0x8000) { ui_commit(); ui_close(); break; }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { ui_close(); break; }
        Sleep(16);
    }
    return 0;
}

/* ---- global hotkey entry point (Ctrl+Alt+L) ----------------------------
 * The plugin should not need a trigger file. A message-only window in its own
 * thread registers the hotkey; WM_HOTKEY starts the overlay on the current
 * canvas. Registration result is logged so a conflict is visible.
 */
static DWORD WINAPI ui_thread(LPVOID p);
static DWORD WINAPI ui_watcher_thread(LPVOID p);
static HWND g_hotkey_wnd;
static int g_hotkey_started;

static LRESULT CALLBACK hotkey_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_HOTKEY && w == 1) {
        if (!g_ui_hwnd && !g_ui_starting) {
            g_ui_starting = 1;
            CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
        }
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI hotkey_thread(LPVOID p) {
    (void)p;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = hotkey_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LiquifyHotkey";
    RegisterClassA(&wc);
    g_hotkey_wnd = CreateWindowA("LiquifyHotkey", "", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!g_hotkey_wnd) { ms_log("HOTKEY: no message window"); return 0; }
    if (!RegisterHotKey(g_hotkey_wnd, 1, MOD_CONTROL | MOD_ALT, 'L')) {
        ms_log("HOTKEY: Ctrl+Alt+L registration failed (%lu)", GetLastError());
        return 0;
    }
    ms_log("HOTKEY: Ctrl+Alt+L registered, plugin ready");
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    UnregisterHotKey(g_hotkey_wnd, 1);
    return 0;
}

/* The Android-side button drops C:\wt_ui into the container; this watcher
 * turns it into an overlay. It runs from the first WTOpen, independent of the
 * GDI capture hooks, so the plugin can be opened without any debug switches. */
/* Liquify is offline for this release: keep the entry points compiled but do
 * not register the hotkey or the app-button watcher, so no overlay can appear.
 * The pen-pressure bridge is untouched. */
static void ensure_plugin_threads(void) {
    static LONG once = 0;
    (void)once;
}

static DWORD WINAPI ui_watcher_thread(LPVOID p) {
    (void)p;
    for (;;) {
        Sleep(250);
        /* test hook: force the eraser cursor without eraser hardware */
        if (GetFileAttributesA("C:\\wt_eraser") != INVALID_FILE_ATTRIBUTES) {
            FILE *ef = fopen("C:\\wt_eraser", "r");
            int v = 0;
            if (ef) { if (fscanf(ef, "%d", &v) != 1) v = 0; fclose(ef); }
            DeleteFileA("C:\\wt_eraser");
            g_eraser = v ? 1 : 0;
            ms_log("ERASER forced %d", g_eraser);
            send_key_state('e', g_eraser);
        }
        if (!g_ui_hwnd && !g_ui_starting &&
            GetFileAttributesA("C:\\wt_ui") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_ui");
            g_ui_starting = 1;
            CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
            ms_log("UI: opened from the app button");
        }
    }
    return 0;
}

static DWORD WINAPI memscan_thread(LPVOID p) {
    (void)p;
    for (;;) {
        Sleep(300);
        if (GetFileAttributesA("C:\\wt_watch.txt") != INVALID_FILE_ATTRIBUTES) {
            FILE *cf = fopen("C:\\wt_watch.txt", "r");
            unsigned long long a = 0; unsigned int np = 1;
            if (cf) { if (fscanf(cf, "%llx %u", &a, &np) != 2) np = 1; fclose(cf); }
            DeleteFileA("C:\\wt_watch.txt");
            if (np > 64) np = 64;
            if (a) {
                if (!g_veh) g_veh = AddVectoredExceptionHandler(1, watch_veh);
                g_watch_addr = a & ~0xFFFull;
                g_watch_hits = 0;
                DWORD old; int armed = 0;
                for (unsigned int i = 0; i < np; ++i) {
                    if (VirtualProtect((LPVOID)(uintptr_t)(g_watch_addr + (unsigned __int64)i * 0x1000),
                                       0x1000ull, PAGE_READWRITE | PAGE_GUARD, &old))
                        ++armed;
                }
                ms_log("WATCH armed %d page(s) at %llx", armed, g_watch_addr);
            }
            continue;
        }
        if (GetFileAttributesA("C:\\wt_invalidate") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_invalidate");
            if (g_cv_dst) InvalidateRect(g_cv_dst, NULL, TRUE);
            ms_log("invalidated canvas hwnd %p", (void*)g_cv_dst);
            continue;
        }
        if (g_stack_ready) {
            g_stack_ready = 0;
            stack_report();
            gdi_flush();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_dump.txt") != INVALID_FILE_ATTRIBUTES) {
            FILE *cf = fopen("C:\\wt_dump.txt", "r");
            unsigned long long a = 0; unsigned int len = 0;
            if (cf) { if (fscanf(cf, "%llx %u", &a, &len) != 2) len = 0; fclose(cf); }
            DeleteFileA("C:\\wt_dump.txt");
            if (a && len && len <= (8u << 20)) {
                unsigned char *bf = (unsigned char*)malloc(len);
                SIZE_T got = 0;
                if (bf && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, bf, len, &got) && got == len) {
                    FILE *of = fopen("C:\\wt_dump.bin", "wb");
                    if (of) { fwrite(bf, 1, len, of); fclose(of); }
                    ms_log("dumped %u bytes from %llx", len, a);
                } else {
                    ms_log("dump failed at %llx len %u", a, len);
                }
                free(bf);
            }
            continue;
        }
        if (GetFileAttributesA("C:\\wt_ui") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_ui");
            CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
            continue;
        }
        if (GetFileAttributesA("C:\\wt_auto_sel") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_auto_sel");
            auto_flow_sel();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_auto") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_auto");
            auto_flow();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_lqnet") != INVALID_FILE_ATTRIBUTES) {
            lq_net_ex("C:\\wt_lqnet");
            DeleteFileA("C:\\wt_lqnet");
            continue;
        }
        if (GetFileAttributesA("C:\\wt_lq") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_lq");
            lq_run();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_clipread") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_clipread");
            clipboard_read();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_doccap") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_doccap");
            doc_capture();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_paste") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_paste");
            doc_paste();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_clip") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_clip");
            clipboard_test();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_guard_canvas") != INVALID_FILE_ATTRIBUTES) {
            FILE *cf = fopen("C:\\wt_guard_canvas", "r");
            unsigned int off = 0; int np = 1;
            if (cf) { if (fscanf(cf, "%x %d", &off, &np) != 2) np = 1; fclose(cf); }
            DeleteFileA("C:\\wt_guard_canvas");
            arm_canvas_guard(off, np);
            continue;
        }
        if (GetFileAttributesA("C:\\wt_vehtest") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_vehtest");
            veh_control_test();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_mouse.txt") != INVALID_FILE_ATTRIBUTES) {
            do_mouse_file();
            continue;
        }
        if (GetFileAttributesA("C:\\wt_write") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("C:\\wt_write");
            g_write_done = 0;
            g_want_write = 1;
            gdi_log("WRITE TEST armed");
            continue;
        }
        if (GetFileAttributesA("C:\\wt_memscan") == INVALID_FILE_ATTRIBUTES) continue;
        DeleteFileA("C:\\wt_memscan");
        DeleteFileA("C:\\wt_memscan.log");
        ms_log("==== requested ====");
        /* Ask the next canvas present to hand us a needle and a dump. */
        FILE *pf = fopen("C:\\wt_pattern.bin", "rb");
        if (pf) {
            fclose(pf);
            gdi_flush();
            ms_run();
            continue;
        }
        g_needle_ready = 0;
        g_want_capture = 1;
        for (int i = 0; i < 200 && !g_needle_ready; ++i) Sleep(50);
        if (!g_needle_ready) { ms_log("no needle: no canvas present seen"); continue; }
        gdi_flush();
        ms_run();
    }
    return 0;
}

static void install_canvas_hooks(void) {
    if (g_gdi_hooks_done) return;
    if (GetFileAttributesA("C:\\wt_canvas_capture") == INVALID_FILE_ATTRIBUTES) return;
    HMODULE mod = GetModuleHandleA(NULL);
    if (!mod) return;
    g_gdi_hooks_done = 1;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; ++imp) {
        const char *dll = (const char*)mod + imp->Name;
        int is_gdi = _stricmp(dll, "GDI32.dll") == 0;
        IMAGE_THUNK_DATA *it = (IMAGE_THUNK_DATA*)((BYTE*)mod + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA*)((BYTE*)mod + imp->FirstThunk);
        if (!it) continue;
        for (; it->u1.AddressOfData; ++it, ++ft) {
            if (it->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME *nm = (IMAGE_IMPORT_BY_NAME*)((BYTE*)mod + it->u1.AddressOfData);
            void *hook = NULL; void **orig = NULL;
            if (is_gdi) {
                if (!strcmp((char*)nm->Name, "BitBlt")) { hook = (void*)probe_BitBlt; orig = (void**)&g_orig_bitblt; }
                else if (!strcmp((char*)nm->Name, "StretchDIBits")) { hook = (void*)probe_StretchDIBits; orig = (void**)&g_orig_stretchdibits; }
                else if (!strcmp((char*)nm->Name, "SetDIBitsToDevice")) { hook = (void*)probe_SetDIBitsToDevice; orig = (void**)&g_orig_setdibits; }
                else if (!strcmp((char*)nm->Name, "CreateDIBSection")) { hook = (void*)probe_CreateDIBSection; orig = (void**)&g_orig_cdibs; }
                else if (!strcmp((char*)nm->Name, "CreateCompatibleBitmap")) { hook = (void*)probe_CreateCompatibleBitmap; orig = (void**)&g_orig_ccbmp; }
                else if (!strcmp((char*)nm->Name, "DeleteObject")) { hook = (void*)probe_DeleteObject; orig = (void**)&g_orig_delobj; }
            }
            if (!hook || !orig) continue;
            DWORD old;
            if (VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                if (!*orig) *orig = (void*)ft->u1.Function;
                ft->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            }
        }
    }
    gdi_log("==== GDI survey hooks installed ====");
    log_line("GDI survey hooks installed");
}

HANDLE WINAPI WTOpenW(HWND hwnd, LOGCONTEXTW *lc, BOOL enable) {
    log_line("WTOpenW hwnd=%p enable=%d", hwnd, enable);
    g_hwnd = hwnd;
    install_canvas_hooks();
    {
        char cls[64] = "";
        if (!hwnd || !GetClassNameA(hwnd, cls, (int)sizeof cls)) cls[0] = 0;
        g_ctx_rereads = wtc_sai_rereads_on_overlap(cls);
        log_line("WTOpenW window class=%s: %s", cls[0] ? cls : "?",
                 g_ctx_rereads ? "SAI re-reads its area when told (WT_CTXOVERLAP)"
                               : "SAI keeps its area until restarted");
    }
    g_open = enable ? TRUE : FALSE;
    ensure_plugin_threads();
    if (lc) {
        log_ctx("WTOpenW ctx from app", lc);
        EnterCriticalSection(&g_cs);
        g_host_sai = host_is_sai();
        g_app_pktdata = lc->lcPktData ? lc->lcPktData : OUR_PKTDATA;
        if (g_host_sai) {
            g_app_pktdata = OUR_PKTDATA;
            g_post_window = 3;
        } else {
            g_post_window = 96;
        }
        /* SAI asks exactly for OUR_PKTDATA; any other mask is a polling app
         * (FireAlpaca asks 0x1fe0). Do not apply SAI's click-hold / message
         * conflation to those: they wait for packets and would never start. */
        g_polling = (g_app_pktdata != OUR_PKTDATA) ? 1 : 0;
        g_hold_on = (g_app_pktdata == OUR_PKTDATA) ? 1 : 0;
        /* Keep the mask the app asked for. Krita requests 0x1fe4 (44 bytes);
         * returning SAI's 36-byte 0x15e2 made it read pressure from Z. */
        lc->lcPktData = g_app_pktdata;
        lc->lcMoveMask = g_app_pktdata;
        lc->lcMsgBase = WT_DEFBASE;
        g_ctx = *lc; g_have_ctx = TRUE;
        g_deliver_serial = g_serial;
        LeaveCriticalSection(&g_cs);
    }
    return (HANDLE)(ULONG_PTR)0xC0FFEE01;
}

BOOL WINAPI WTClose(HANDLE ctx) { (void)ctx; log_line("WTClose"); g_open=FALSE; g_hwnd=NULL; return TRUE; }

/* SAI polls this to fetch the packet a WT_PACKET message referenced —
 * return the packet MATCHING that serial (from the ring), not just the
 * latest, or bursts of queued messages all collapse to one point */
int WINAPI WTPacket(HANDLE ctx, UINT serial, LPVOID buf) {
    static LONG count;
    (void)ctx;
    if (!buf) return 0;
    OURPKT pk;
    WTPKT mask;
    EnterCriticalSection(&g_cs);
    pk = (g_ring_serial[serial % RING_SZ] == serial) ? g_ring[serial % RING_SZ] : g_last;
    mask = g_app_pktdata ? g_app_pktdata : OUR_PKTDATA;
    if (g_trace_start) wtc_st_fetch(&g_st, GetTickCount(), serial);
    LeaveCriticalSection(&g_cs);
    if (g_host_sai) {
        memcpy(buf, &pk, sizeof(pk));          /* exact 0.3.10 layout */
    } else {
        wt_build_packet(&pk, serial, mask, buf);
    }
    g_fetched = serial;                 /* track how current SAI is (backlog probe) */
    g_fetch_count++;                    /* points SAI actually drew (draw-rate probe) */
    LONG n = InterlockedIncrement(&count);
    if (n == 1 || (n & 63) == 0) {      /* log 1st + every 64th fetch */
        DWORD lat = GetTickCount() - g_ring_time[serial % RING_SZ];  /* post->fetch latency */
        log_line("WTPacket #%ld serial=%u x=%ld y=%ld press=%u lat=%lums", n, serial,
             (long)pk.x, (long)pk.y, pk.pressure, (unsigned long)lat);
    }
    return 1;
}

UINT WINAPI WTQueueSizeSet(HANDLE ctx, int n) { log_line("WTQueueSizeSet n=%d", n); (void)ctx; (void)n; return 1; }
BOOL WINAPI WTGetW(HANDLE ctx, LOGCONTEXTW *lc) {
    log_line("WTGetW"); (void)ctx;
    if (lc) {
        fill_default_context(lc);
        /* SAI now believes the area we just handed it, so place the pen in
         * THAT area from here on. This is how a screen added after launch can
         * get pressure without a restart (#37), if SAI re-reads its context
         * when told the screens changed. */
        EnterCriticalSection(&g_cs);
        if (g_have_ctx && (g_ctx.lcSysOrgX != lc->lcSysOrgX || g_ctx.lcSysOrgY != lc->lcSysOrgY ||
                           g_ctx.lcSysExtX != lc->lcSysExtX || g_ctx.lcSysExtY != lc->lcSysExtY)) {
            log_line("SCREENS: SAI re-read its context: area (%d,%d %dx%d) -> (%d,%d %dx%d)",
                     g_ctx.lcSysOrgX, g_ctx.lcSysOrgY, g_ctx.lcSysExtX, g_ctx.lcSysExtY,
                     lc->lcSysOrgX, lc->lcSysOrgY, lc->lcSysExtX, lc->lcSysExtY);
            g_ctx.lcSysOrgX = lc->lcSysOrgX; g_ctx.lcSysOrgY = lc->lcSysOrgY;
            g_ctx.lcSysExtX = lc->lcSysExtX; g_ctx.lcSysExtY = lc->lcSysExtY;
            g_ctx.lcOutOrgX = lc->lcOutOrgX; g_ctx.lcOutOrgY = lc->lcOutOrgY;
            g_ctx.lcOutExtX = lc->lcOutExtX; g_ctx.lcOutExtY = lc->lcOutExtY;
            g_newscreen = 0;
        }
        LeaveCriticalSection(&g_cs);
    }
    return TRUE;
}
BOOL WINAPI WTEnable(HANDLE ctx, BOOL enable) { log_line("WTEnable enable=%d", enable); (void)ctx; g_open = enable?TRUE:FALSE; return TRUE; }
static int wt_fetch_packets(int max, LPVOID buf, int advance) {
    if (max <= 0 || !buf) return 0;
    int n = 0;
    WTPKT mask;
    UINT start, end;
    int size;
    EnterCriticalSection(&g_cs);
    g_polling = 1;
    mask = g_app_pktdata ? g_app_pktdata : OUR_PKTDATA;
    size = wt_packet_size(mask);
    end = g_serial;
    start = g_deliver_serial;
    if (start > end) start = end;
    if ((UINT)(end - start) > RING_SZ) start = end - RING_SZ;
    while (size > 0 && n < max && start != end) {
        if (g_ring_serial[start % RING_SZ] != start) { start++; continue; }
        wt_build_packet(&g_ring[start % RING_SZ], start, mask,
                        (unsigned char *)buf + (size_t)n * size);
        start++;
        n++;
    }
    if (advance) {
        g_deliver_serial = start;
        if (n > 0) {
            g_fetched = start - 1;     /* watchdog: producer is being consumed */
            g_fetch_count += n;
        }
    }
    LeaveCriticalSection(&g_cs);
    if (n > 0) log_line("WTPacketsGet n=%d size=%d poll=%d", n, size, advance);
    return n;
}

int  WINAPI WTPacketsGet(HANDLE ctx, int max, LPVOID buf) {
    (void)ctx;
    return wt_fetch_packets(max, buf, 1);
}

UINT WINAPI WTInfoA(UINT cat, UINT idx, LPVOID out) {
    if (host_is_sai()) return wt_info_sai_a(cat, idx, out);
    log_line("WTInfoA cat=%u idx=%u out=%p", cat, idx, out);
    switch (cat) {
    case 0: return 200;
    case WTI_INTERFACE:
        switch (idx) {
        case 1: { static const char id[]="Wine OwnTab 1.1"; if(out) memcpy(out,id,sizeof(id)); return sizeof(id); }
        case 2: { WORD v=(1)|(1<<8); if(out)*(WORD*)out=v; return sizeof(WORD); }
        case 3: { WORD v=(1)|(1<<8); if(out)*(WORD*)out=v; return sizeof(WORD); }
        case 4: case 5: case 6: { UINT n=1; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case 7: case 8: case 9: case 10: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }
        default: return 0;
        }
    case WTI_DEFCONTEXT: case WTI_DEFSYSCTX: case WTI_DDCTXS:
        if (idx==0) { if(out) fill_default_context_a((LOGCONTEXTA*)out); return sizeof(LOGCONTEXTA); }
        return 0;
    case WTI_DEVICES:
        switch (idx) {
        case DVC_NAME: { static const char n[]="Wine OwnTab"; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        case DVC_X: case DVC_Y: case DVC_Z: { AXIS a={0,IN_EXT,0,0}; if(out)*(AXIS*)out=a; return sizeof(AXIS); }
        case DVC_NPRESSURE: case DVC_TPRESSURE: { AXIS a={0,MAX_PRESS,0,0}; if(out)*(AXIS*)out=a; return sizeof(AXIS); }
        case DVC_ORIENTATION: case DVC_ROTATION: { AXIS a[3]={{0,3600,0,0},{0,900,0,0},{0,0,0,0}}; if(out)memcpy(out,a,sizeof(a)); return sizeof(a); }
        case DVC_PKTRATE: { UINT n=133; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case DVC_PKTDATA: { WTPKT m=OUR_PKTDATA; if(out)*(WTPKT*)out=m; return sizeof(WTPKT); }
        case DVC_PKTMODE: { WTPKT m=0; if(out)*(WTPKT*)out=m; return sizeof(WTPKT); }
        case DVC_HARDWARE: { DWORD n=0; if(out)*(DWORD*)out=n; return sizeof(DWORD); }
        case DVC_NCSRTYPES: { UINT n=1; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case DVC_FIRSTCSR: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case DVC_DEFX: case DVC_DEFY: case DVC_DEFZ: { LONG n=0; if(out)*(LONG*)out=n; return sizeof(LONG); }
        default: return 0;
        }
    case WTI_CURSORS:
        switch (idx) {
        case CSR_NAME: { static const char n[]="Pen"; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        case CSR_ACTIVE: { BOOL b=TRUE; if(out)*(BOOL*)out=b; return sizeof(BOOL); }
        case CSR_PKTDATA: { WTPKT m=OUR_PKTDATA; if(out)*(WTPKT*)out=m; return sizeof(WTPKT); }
        case CSR_BUTTONS: { UINT n=3; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case CSR_BUTTONBITS: { UINT n=3; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case CSR_BTNNAMES: { static const char n[]="Left\0Middle\0Right\0"; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        case CSR_BUTTONMAP: { BYTE m[3]={0,1,2}; if(out)memcpy(out,m,sizeof(m)); return sizeof(m); }
        case CSR_SYSBTNMAP: { BYTE m[3]={0,1,2}; if(out)memcpy(out,m,sizeof(m)); return sizeof(m); }
        case CSR_NPBUTTON: { UINT n=3; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case CSR_TPBUTTON: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }
        case CSR_NPBTNMARKS: case CSR_NPBREPEAT: case CSR_NPBTNACTION: { UINT n[3]={0,0,0}; if(out)memcpy(out,n,sizeof(n)); return sizeof(n); }
        case CSR_PHYSID: case CSR_TYPE: case CSR_CAPABILITIES: { UINT n=0; if(out)*(UINT*)out=n; return sizeof(UINT); }
        default: { UINT z=0; if(out)*(UINT*)out=z; return sizeof(UINT); }
        }
    default: return 0;
    }
}

HANDLE WINAPI WTOpenA(HWND hwnd, LOGCONTEXTA *lc, BOOL enable) {
    log_line("WTOpenA hwnd=%p enable=%d", hwnd, enable);
    g_hwnd = hwnd;
    g_open = enable ? TRUE : FALSE;
    if (lc) {
        EnterCriticalSection(&g_cs);
        g_app_pktdata = lc->lcPktData ? lc->lcPktData : OUR_PKTDATA;
        g_polling = (g_app_pktdata != OUR_PKTDATA) ? 1 : 0;
        g_hold_on = (g_app_pktdata == OUR_PKTDATA) ? 1 : 0;
        lc->lcPktData = g_app_pktdata;
        lc->lcMoveMask = g_app_pktdata;
        lc->lcMsgBase = WT_DEFBASE;
        LOGCONTEXTW w; fill_default_context(&w);
        w.lcPktData = g_app_pktdata; w.lcMsgBase = WT_DEFBASE;
        g_ctx = w; g_have_ctx = TRUE;
        g_deliver_serial = g_serial;
        LeaveCriticalSection(&g_cs);
    }
    return (HANDLE)(ULONG_PTR)0xC0FFEE01;
}

BOOL WINAPI WTGetA(HANDLE ctx, LOGCONTEXTA *lc) { log_line("WTGetA"); (void)ctx; if(lc) fill_default_context_a(lc); return TRUE; }
BOOL WINAPI WTSetA(HANDLE ctx, LOGCONTEXTA *lc) { log_line("WTSetA"); (void)ctx; (void)lc; return TRUE; }
BOOL WINAPI WTSetW(HANDLE ctx, LOGCONTEXTW *lc) {
    log_line("WTSetW"); (void)ctx;
    if (lc) {
        log_ctx("WTSetW ctx from SAI", lc);
        EnterCriticalSection(&g_cs);
        g_ctx = *lc; g_have_ctx = TRUE;
        LeaveCriticalSection(&g_cs);
    }
    return TRUE;
}

/* ---- remaining WinTab surface: well-behaved stubs so GetProcAddress never
 * fails. SAI resolves the whole API up front and aborts with "Windows function
 * call failed" if ANY export is missing — this was the startup crash. -------- */

BOOL WINAPI WTOverlap(HANDLE ctx, BOOL toTop) { log_line("WTOverlap %d", toTop); (void)ctx; (void)toTop; return TRUE; }
BOOL WINAPI WTConfig(HANDLE ctx, HWND hwnd) { log_line("WTConfig"); (void)ctx; (void)hwnd; return FALSE; }
BOOL WINAPI WTExtGet(HANDLE ctx, UINT ext, LPVOID out) { log_line("WTExtGet ext=%u", ext); (void)ctx; (void)out; return FALSE; }
BOOL WINAPI WTExtSet(HANDLE ctx, UINT ext, LPVOID in) { log_line("WTExtSet ext=%u", ext); (void)ctx; (void)in; return FALSE; }
BOOL WINAPI WTSave(HANDLE ctx, LPVOID save) { log_line("WTSave"); (void)ctx; (void)save; return FALSE; }
HANDLE WINAPI WTRestore(HWND hwnd, LPVOID save, BOOL enable) { log_line("WTRestore"); (void)hwnd; (void)save; (void)enable; return NULL; }
int  WINAPI WTPacketsPeek(HANDLE ctx, int max, LPVOID buf) { (void)ctx; return wt_fetch_packets(max, buf, 0); }
int  WINAPI WTDataGet(HANDLE ctx, UINT b, UINT e, int max, LPVOID buf, LPINT n) { log_line("WTDataGet"); (void)ctx;(void)b;(void)e;(void)max;(void)buf; if(n)*n=0; return 0; }
int  WINAPI WTDataPeek(HANDLE ctx, UINT b, UINT e, int max, LPVOID buf, LPINT n) { log_line("WTDataPeek"); (void)ctx;(void)b;(void)e;(void)max;(void)buf; if(n)*n=0; return 0; }
BOOL WINAPI WTQueuePacketsEx(HANDLE ctx, UINT *o, UINT *n) { log_line("WTQueuePacketsEx"); (void)ctx; if(o)*o=0; if(n)*n=0; return FALSE; }
int  WINAPI WTQueueSizeGet(HANDLE ctx) { log_line("WTQueueSizeGet"); (void)ctx; return 32; }
HANDLE WINAPI WTMgrOpen(HWND hwnd, UINT msgBase) { log_line("WTMgrOpen"); (void)hwnd; (void)msgBase; return NULL; }
BOOL WINAPI WTMgrClose(HANDLE mgr) { log_line("WTMgrClose"); (void)mgr; return FALSE; }
BOOL WINAPI WTMgrContextEnum(HANDLE mgr, LPVOID fn, LPARAM lp) { log_line("WTMgrContextEnum"); (void)mgr;(void)fn;(void)lp; return FALSE; }
HWND WINAPI WTMgrContextOwner(HANDLE mgr, HANDLE ctx) { log_line("WTMgrContextOwner"); (void)mgr;(void)ctx; return NULL; }
HANDLE WINAPI WTMgrDefContext(HANDLE mgr, BOOL sys) { log_line("WTMgrDefContext"); (void)mgr;(void)sys; return NULL; }
HANDLE WINAPI WTMgrDefContextEx(HANDLE mgr, UINT dev, BOOL sys) { log_line("WTMgrDefContextEx"); (void)mgr;(void)dev;(void)sys; return NULL; }

/* ---- not WinTab: how a probe tells OUR wintab32 from Wine's ---------------
 * Wine's built-in wintab32 exports every name above, so from outside the
 * process there is no way to know which of the two a LoadLibrary just resolved
 * to — and that is the entire question behind #29, where the file on disk was
 * ours and the DLL in SAI was Wine's. GetProcAddress finds this name only in
 * ours, and it hands back the build stamp so a probe can say WHICH build it
 * found rather than just "a good one". Deliberately not a WT* name: nothing
 * that scans for WinTab entry points should ever pick it up. */
__declspec(dllexport) const char * WINAPI SAIPP_BridgeBuild(void) { return WT_BUILD_STAMP; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_DETACH && g_click_hook) {
        UnhookWindowsHookEx(g_click_hook);
        g_click_hook = NULL;
    }
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cs);
        g_screenW = GetSystemMetrics(SM_CXSCREEN);
        g_screenH = GetSystemMetrics(SM_CYSCREEN);
        /* full virtual desktop spanning all monitors (single screen: == primary) */
        g_virtW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g_virtH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (g_virtW <= 0) g_virtW = g_screenW;
        if (g_virtH <= 0) g_virtH = g_screenH;
        read_virtual(&g_virtX, &g_virtY, &g_virtW, &g_virtH);
        {
            char exe[MAX_PATH] = "";
            DWORD len = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
            if (len == 0 || len >= sizeof exe) exe[0] = '\0';   /* unknown: treated as SAI */
            g_host_is_probe = wtc_host_is_probe(exe);
        }
        if (getenv("WT_NO_CTX_NOTIFY")) g_notify_ctx = 0;
        g_no_pinch_zoom = getenv("WT_NO_PINCH_ZOOM") != NULL;
        g_no_win32_wake = getenv("WT_NO_WIN32_WAKE") != NULL;
        if (getenv("WT_NO_SELFCHECK")) g_selfcheck = 0;
        if (getenv("WT_LOG_POS")) g_log_pos = 1;
        /* logging is OFF unless WT_DEBUG is set: without a log file, log_line() is a
         * no-op (g_log stays NULL) so there's zero per-packet fflush overhead. */
        if (getenv("WT_DEBUG")) g_log = fopen("C:\\wtlog.txt", "w");
        g_trace_start = g_log && !getenv("WT_NO_START_TRACE");
        /* Must happen before SAI calls WTInfo/WTOpen — it reads the pressure
         * axis exactly once, at open time. */
        load_max_press();
        /* The build stamp is not decoration. A DLL-side fix reaches the user
         * only if the file in the PREFIX was replaced, and for several releases
         * nothing refreshed it on upgrade — so "is the fix even loaded?" was
         * unanswerable from the log, and a stale DLL looked exactly like a
         * broken fix. One line here settles it. */
        log_line("==== OwnTab wintab32.dll loaded; built " WT_BUILD_STAMP
                 "; screen %dx%d virtual %dx%d at (%d,%d) maxPress=%d host=%s ====",
             g_screenW, g_screenH, g_virtW, g_virtH, g_virtX, g_virtY, g_max_press,
             g_host_is_probe ? "Test pen probe (no wake)" : "SAI");
        install_canvas_hooks();
        CreateThread(NULL, 0, producer, NULL, 0, NULL);
        if (g_gdi_hooks_done) CreateThread(NULL, 0, memscan_thread, NULL, 0, NULL);
        /* Off unless WT_TIMELAPSE / WT_TIMELAPSE_PROBE is set. Started after
         * the producer so nothing here can delay pressure coming up. */
        tl_startup();
    }
    return TRUE;
}
