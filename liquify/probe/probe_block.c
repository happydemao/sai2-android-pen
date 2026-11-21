/* ---- GDI canvas survey probe (development only; C:\wt_canvas_capture) ----
 *
 * Goal: find the DIB that holds real document pixels (a layer buffer or the
 * canvas composite) and decide whether writing into it is persistent.
 *
 * What it does, all inside sai2.exe via the exe's IAT:
 *   CreateDIBSection        every DIB the app allocates, with size / bpp
 *   CreateCompatibleBitmap  bitmap allocations that are not DIBs
 *   BitBlt / StretchDIBits / SetDIBitsToDevice
 *                           which surface is composed where, and content
 *   DeleteObject            keep the registry from pointing at freed memory
 *
 * Blits are the only place pixels are dumped: at that moment the surface is
 * certainly alive and has content, so there is no freed-memory race.
 * Everything is written to C:\wt_gdi.log and C:\wt_gdi_NN.bmp.
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

static FILE *g_gdi;
static int   g_gdi_lines;
static int   g_gdi_dumps;
static int   g_gdi_hooks_done;

/* Registry of 32bpp surfaces the app allocated, so a blit can name them. */
#define MAX_DIB 96
static struct { HGDIOBJ obj; int w, h, bpp; int dumped; } g_dib[MAX_DIB];
static int g_dib_n;

static void gdi_log(const char *fmt, ...) {
    if (!g_gdi || g_gdi_lines > 6000) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_gdi, fmt, ap); va_end(ap);
    fputc('\n', g_gdi); fflush(g_gdi);
    g_gdi_lines++;
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

static int gdi_reg_find(HGDIOBJ obj) {
    for (int i = 0; i < g_dib_n; ++i) if (g_dib[i].obj == obj) return i;
    return -1;
}

static void gdi_reg_add(HGDIOBJ obj, int w, int h, int bpp) {
    if (g_dib_n >= MAX_DIB) return;
    g_dib[g_dib_n].obj = obj;
    g_dib[g_dib_n].w = w;
    g_dib[g_dib_n].h = h;
    g_dib[g_dib_n].bpp = bpp;
    g_dib[g_dib_n].dumped = 0;
    g_dib_n++;
}

static void gdi_dump(const char *tag, const void *bits, int w, int h, int stride, int bpp) {
    if (!g_gdi || !bits || w < 64 || h < 64 || stride <= 0) return;
    if (bpp != 32 && bpp != 24) return;
    if ((long long)w * h < 200000) return;
    if (g_gdi_dumps >= 10) return;
    char path[64];
    _snprintf(path, sizeof path - 1, "C:\\wt_gdi_%02d.bmp", g_gdi_dumps);
    path[sizeof path - 1] = 0;
    BITMAPFILEHEADER fh; memset(&fh, 0, sizeof fh);
    BITMAPINFOHEADER ih; memset(&ih, 0, sizeof ih);
    DWORD dataSize = (DWORD)(stride * h);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + dataSize;
    ih.biSize = sizeof ih;
    ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1;
    ih.biBitCount = (WORD)bpp; ih.biCompression = BI_RGB;
    ih.biSizeImage = dataSize;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(&fh, 1, sizeof fh, f);
    fwrite(&ih, 1, sizeof ih, f);
    const unsigned char *p = (const unsigned char*)bits;
    for (int y = h - 1; y >= 0; --y) fwrite(p + (size_t)y * stride, 1, (size_t)stride, f);
    fclose(f);
    g_gdi_dumps++;
    gdi_log("DUMP %s -> %s  %dx%d stride=%d bpp=%d", tag, path, w, h, stride, bpp);
}

/* Describe a DC's selected bitmap: size + a registry index when known. */
static void gdi_describe(const char *role, HDC hdc) {
    if (!hdc) { gdi_log("  %s dc=(null)", role); return; }
    HGDIOBJ obj = GetCurrentObject(hdc, OBJ_BITMAP);
    DIBSECTION ds;
    if (!obj || GetObjectA(obj, sizeof ds, &ds) != sizeof ds) {
        gdi_log("  %s %s obj=%p (no bitmap info)", role, gdi_class_of(hdc), (void*)obj);
        return;
    }
    int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
    if (h < 0) h = -h;
    int idx = gdi_reg_find(obj);
    gdi_log("  %s %s obj=%p %dx%d bpp=%d bits=%p reg=%d",
            role, gdi_class_of(hdc), (void*)obj, w, h,
            ds.dsBm.bmBitsPixel, ds.dsBm.bmBits, idx);
}

static BOOL WINAPI probe_BitBlt(HDC dst, int x, int y, int cx, int cy,
                                HDC src, int sx, int sy, DWORD rop) {
    if (g_gdi && !(cx * cy < 40000)) {
        gdi_log("BitBlt %dx%d at (%d,%d) rop=%08lx", cx, cy, x, y, (unsigned long)rop);
        gdi_describe("dst", dst);
        gdi_describe("src", src);
        if (src) {
            HGDIOBJ o = GetCurrentObject(src, OBJ_BITMAP);
            int idx = gdi_reg_find(o);
            if (idx >= 0 && !g_dib[idx].dumped) {
                DIBSECTION ds;
                if (GetObjectA(o, sizeof ds, &ds) == sizeof ds && ds.dsBm.bmBits) {
                    int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
                    if (h < 0) h = -h;
                    g_dib[idx].dumped = 1;
                    gdi_dump("BitBlt.src", ds.dsBm.bmBits, w, h, ds.dsBm.bmWidthBytes,
                             ds.dsBm.bmBitsPixel);
                }
            }
        }
    }
    return g_orig_bitblt(dst, x, y, cx, cy, src, sx, sy, rop);
}

static int WINAPI probe_StretchDIBits(HDC hdc, int xd, int yd, int wd, int hd,
                                      int xs, int ys, int ws, int hs,
                                      const void *bits, const BITMAPINFO *bi,
                                      UINT usage, DWORD rop) {
    if (g_gdi) {
        gdi_log("StretchDIBits dst %dx%d at (%d,%d) src %dx%d at (%d,%d) usage=%u rop=%08lx",
                wd, hd, xd, yd, ws, hs, xs, ys, usage, (unsigned long)rop);
        gdi_describe("dst", hdc);
        if (bits && bi && bi->bmiHeader.biBitCount > 0) {
            int w = bi->bmiHeader.biWidth, h = bi->bmiHeader.biHeight;
            if (h < 0) h = -h;
            int stride = ((w * bi->bmiHeader.biBitCount + 31) / 32) * 4;
            gdi_dump("StretchDIBits.src", bits, w, h, stride, bi->bmiHeader.biBitCount);
        }
    }
    return g_orig_stretchdibits(hdc, xd, yd, wd, hd, xs, ys, ws, hs, bits, bi, usage, rop);
}

static int WINAPI probe_SetDIBitsToDevice(HDC hdc, int x, int y, DWORD cx, DWORD cy,
                                          int sx, int sy, UINT start, UINT lines,
                                          const void *bits, const BITMAPINFO *bi, UINT usage) {
    if (g_gdi) {
        gdi_log("SetDIBitsToDevice %lux%lu at (%d,%d) start=%u lines=%u usage=%u",
                (unsigned long)cx, (unsigned long)cy, x, y, start, lines, usage);
        gdi_describe("dst", hdc);
        if (bits && bi && bi->bmiHeader.biBitCount > 0) {
            int w = bi->bmiHeader.biWidth, h = bi->bmiHeader.biHeight;
            if (h < 0) h = -h;
            int stride = ((w * bi->bmiHeader.biBitCount + 31) / 32) * 4;
            gdi_dump("SetDIBitsToDevice.src", bits, w, h, stride, bi->bmiHeader.biBitCount);
        }
    }
    return g_orig_setdibits(hdc, x, y, cx, cy, sx, sy, start, lines, bits, bi, usage);
}

static HBITMAP WINAPI probe_CreateDIBSection(HDC hdc, const BITMAPINFO *bi, UINT usage,
                                             void **bits, HANDLE section, DWORD offset) {
    HBITMAP h = g_orig_cdibs(hdc, bi, usage, bits, section, offset);
    if (g_gdi && h && bi && bits) {
        int w = bi->bmiHeader.biWidth;
        int hs = bi->bmiHeader.biHeight;
        int topdown = hs < 0;
        int hh = hs < 0 ? -hs : hs;
        /* bpp is taken from the real object: biBitCount can be 0 when the
         * caller passes a packed header. */
        DIBSECTION ds;
        int bpp = bi->bmiHeader.biBitCount;
        int stride = bi->bmiHeader.biSizeImage;
        if (GetObjectA(h, sizeof ds, &ds) == sizeof ds) {
            bpp = ds.dsBm.bmBitsPixel;
            stride = ds.dsBm.bmWidthBytes;
        }
        if (!stride) stride = ((w * bpp + 31) / 32) * 4;
        gdi_log("CreateDIBSection %dx%d bpp=%d usage=%u topdown=%d stride=%d bits=%p",
                w, hh, bpp, usage, topdown, stride, *bits);
        gdi_reg_add((HGDIOBJ)h, w, hh, bpp);
        gdi_reg_add((HGDIOBJ)h, w, hh, bpp) ; /* no-op guard on duplicate is below */
    }
    return h;
}

static HBITMAP WINAPI probe_CreateCompatibleBitmap(HDC hdc, int w, int h) {
    HBITMAP b = g_orig_ccbmp(hdc, w, h);
    if (g_gdi && b && (long long)w * h >= 200000)
        gdi_log("CreateCompatibleBitmap %dx%d -> %p", w, h, (void*)b);
    return b;
}

static BOOL WINAPI probe_DeleteObject(HGDIOBJ obj) {
    if (g_gdi) {
        int idx = gdi_reg_find(obj);
        if (idx >= 0) {
            gdi_log("DeleteObject frees %dx%d bpp=%d",
                    g_dib[idx].w, g_dib[idx].h, g_dib[idx].bpp);
            g_dib[idx] = g_dib[--g_dib_n];
        }
    }
    return g_orig_delobj(obj);
}

static void install_canvas_hooks(void) {
    if (g_gdi_hooks_done) return;
    if (GetFileAttributesA("C:\\wt_canvas_capture") == INVALID_FILE_ATTRIBUTES) return;
    HMODULE mod = GetModuleHandleA(NULL);
    if (!mod) return;
    g_gdi = fopen("C:\\wt_gdi.log", "w");
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
