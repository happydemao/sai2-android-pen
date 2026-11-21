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
static volatile HWND g_cv_dst;
static volatile int g_want_capture;   /* set by the scan thread, consumed by a blit */
static volatile int g_needle_ready;
static unsigned char g_needle[256];
static volatile unsigned long g_blit_count;

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
            g_cv_bits = (void*)bits; g_cv_w = w; g_cv_h = h;
            g_cv_stride = bi->bmiHeader.biSizeImage ? bi->bmiHeader.biSizeImage
                                                    : ((w * bi->bmiHeader.biBitCount + 31) / 32) * 4;
            g_cv_bpp = bi->bmiHeader.biBitCount;
            g_cv_x = x; g_cv_y = y; g_cv_cx = (int)cx; g_cv_cy = (int)cy;
            g_cv_dst = WindowFromDC(hdc);
            if (g_want_capture) {
                g_want_capture = 0;
                gdi_log("canvas present %dx%d stride=%d bpp=%d dst=%s rect=(%d,%d %dx%d) start=%u lines=%u",
                        w, h, g_cv_stride, g_cv_bpp, gdi_class_of(hdc), x, y, (int)cx, (int)cy, start, lines);
                gdi_dump_canvas();
                g_needle_ready = gdi_make_needle() ? 1 : 0;
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

static void ms_run(void) {
    ms_log("==== scan ====");
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
                if (ms_search(g_needle, (int)sizeof g_needle, hay, got, &at)) {
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

static DWORD WINAPI memscan_thread(LPVOID p) {
    (void)p;
    for (;;) {
        Sleep(300);
        if (GetFileAttributesA("C:\\wt_memscan") == INVALID_FILE_ATTRIBUTES) continue;
        DeleteFileA("C:\\wt_memscan");
        DeleteFileA("C:\\wt_memscan.log");
        ms_log("==== requested ====");
        /* Ask the next canvas present to hand us a needle and a dump. */
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
