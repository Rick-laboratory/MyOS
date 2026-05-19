#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "fb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

// v110: sdk/include/wingdi.h -> wingdi.c
// Public GDI32/MSDN entrypoints live here directly.

// ─────────────────────────────────────────────
// v110: GDI32/DC/WM_PAINT
// A tiny command-buffered drawing model: InvalidateRect queues WM_PAINT,
// BeginPaint allocates an HDC, GDI calls append commands, PaintLab blits them.
// ─────────────────────────────────────────────
/* AUDIT(v163): GDI now has compatible memory DCs, HBITMAP objects,
   SRCCOPY BitBlt/StretchBlt, USER32 paint/erase integration, HRGN/clip,
   directly writable 32-bpp BI_RGB CreateDIBSection, DIB transfer bridge
   APIs, per-DC stretch-mode state, and a first PatBlt brush/destination ROP
   path.  PatBlt covers the documented first pattern ROP subset
   (PATCOPY/PATINVERT/DSTINVERT/BLACKNESS/WHITENESS) for memory DCs and for
   window/paint command replay.  StretchBlt remains intentionally narrow:
   SRCCOPY, memory-source DCs, nearest/COLORONCOLOR-style sampling, clip-aware
   memory destinations, and snapshot commands for window/paint DC targets.
   HALFTONE is accepted as DC state but not yet a high-quality filter; broader
   ROPs, palettes, BI_BITFIELDS masks, ICM, alpha/composition and real driver
   clipping remain future compliance work. */
#define MYGDI_MAX_BRUSHES 64
#define MYGDI_MAX_BITMAPS 96
#define MYGDI_MAX_REGIONS 96
#define MYGDI_REGION_MAX_RECTS 256
#define MYGDI_MAX_DCS 48
#define MYGDI_MAX_WINDOWS 64
#define MYGDI_MAX_COMMANDS 768
#define MYGDI_DEFAULT_STRETCH_MODE BLACKONWHITE
#define MYGDI_HANDLE_HASH_BUCKETS 128
#define MYGDI_HANDLE_HASH_MASK (MYGDI_HANDLE_HASH_BUCKETS - 1)

typedef struct MyGdiRegionData {
    int count;
    RECT rects[MYGDI_REGION_MAX_RECTS];
} MyGdiRegionData;

typedef struct MyGdiBrushObj {
    int valid;
    HBRUSH handle;
    DWORD handleHash;
    int handleHashNext;
    COLORREF color;
    DWORD ownerPid;
    DWORD selectedCount;
} MyGdiBrushObj;

typedef struct MyGdiBitmapObj {
    int valid;
    HBITMAP handle;
    DWORD handleHash;
    int handleHashNext;
    int width;
    int height;
    int bpp;
    int widthBytes;
    DWORD ownerPid;
    DWORD selectedCount;
    int internalDefault;
    int dibSection;
    int dibTopDown;
    BITMAPINFOHEADER dibHeader;
    HANDLE dibSectionHandle;
    DWORD dibOffset;
    COLORREF* pixels;
} MyGdiBitmapObj;

typedef struct MyGdiRegionObj {
    int valid;
    HRGN handle;
    DWORD handleHash;
    int handleHashNext;
    DWORD ownerPid;
    MyGdiRegionData data;
} MyGdiRegionObj;

typedef struct MyGdiDCObj {
    int valid;
    HDC handle;
    DWORD handleHash;
    int handleHashNext;
    HWND hwnd;
    HBRUSH selectedBrush;
    HBITMAP selectedBitmap;
    HBITMAP defaultBitmap;
    RECT rcPaint;
    int paintDc;
    int memoryDc;
    int stretchMode;
    int hasClip;
    MyGdiRegionData clip;
} MyGdiDCObj;

typedef struct MyGdiWindowState {
    int valid;
    HWND hwnd;
    DWORD hwndHash;
    int hwndHashNext;
    int dirty;
    int paintPending;
    int erasePending;
    int internalPaint;
    DWORD invalidateSerial;
    DWORD postedPaints;
    DWORD coalescedInvalidates;
    RECT dirtyRect;
    int firstCommand;
    int lastCommand;
    int commandCount;
    MyGdiRegionData updateRegion;
} MyGdiWindowState;

typedef enum MyGdiCommandType {
    MYGDI_CMD_FILL = 1,
    MYGDI_CMD_RECT = 2,
    MYGDI_CMD_TEXT = 3,
    MYGDI_CMD_BLIT = 4,
    MYGDI_CMD_PATBLT = 5
} MyGdiCommandType;

typedef struct MyGdiCommand {
    int valid;
    HWND hwnd;
    MyGdiCommandType type;
    RECT rc;
    COLORREF color;
    char text[160];
    int srcX;
    int srcY;
    int blitW;
    int blitH;
    DWORD rop;
    COLORREF* blitPixels;
    int windowNext;
} MyGdiCommand;

static pthread_mutex_t g_GdiLock = PTHREAD_MUTEX_INITIALIZER;
static MyGdiBrushObj g_GdiBrushes[MYGDI_MAX_BRUSHES];
static MyGdiBitmapObj g_GdiBitmaps[MYGDI_MAX_BITMAPS];
static MyGdiRegionObj g_GdiRegions[MYGDI_MAX_REGIONS];
static MyGdiDCObj g_GdiDcs[MYGDI_MAX_DCS];
static MyGdiWindowState g_GdiWindows[MYGDI_MAX_WINDOWS];
static MyGdiCommand g_GdiCommands[MYGDI_MAX_COMMANDS];
static int g_GdiBrushHash[MYGDI_HANDLE_HASH_BUCKETS];
static int g_GdiBitmapHash[MYGDI_HANDLE_HASH_BUCKETS];
static int g_GdiRegionHash[MYGDI_HANDLE_HASH_BUCKETS];
static int g_GdiDcHash[MYGDI_HANDLE_HASH_BUCKETS];
static int g_GdiWindowHash[MYGDI_HANDLE_HASH_BUCKETS];
static HBRUSH g_NextBrushHandle = 0xA000;
static HBITMAP g_NextBitmapHandle = 0xB000;
static HRGN g_NextRegionHandle = 0xC000;
static HDC g_NextDcHandle = 0xD000;
static int g_GdiFreeInit = 0;
static int g_GdiBrushFree[MYGDI_MAX_BRUSHES];
static int g_GdiBitmapFree[MYGDI_MAX_BITMAPS];
static int g_GdiRegionFree[MYGDI_MAX_REGIONS];
static int g_GdiDcFree[MYGDI_MAX_DCS];
static int g_GdiWindowFree[MYGDI_MAX_WINDOWS];
static int g_GdiCommandFree[MYGDI_MAX_COMMANDS];
static int g_GdiBrushFreeTop = 0;
static int g_GdiBitmapFreeTop = 0;
static int g_GdiRegionFreeTop = 0;
static int g_GdiDcFreeTop = 0;
static int g_GdiWindowFreeTop = 0;
static int g_GdiCommandFreeTop = 0;

static Color mygdi_color(COLORREF cr)
{
    return COLOR((cr >> 16) & 0xff, (cr >> 8) & 0xff, cr & 0xff);
}

/* v239 quality-hotpath: GDI handles are now slot-coded where the backing
   fixed table allows it.  The hash table remains as ABI/fallback support for
   legacy/raw handles, but normal lookup becomes decode -> generation/state ->
   direct array access. */
static int mygdi_decode_slot_handle(HANDLE h, DWORD expectedType, int maxSlots)
{
    DWORD type = 0, slot = 0;
    if (!_ObjectDecodeSlotHandle(h, &type, &slot)) return -1;
    if (type != expectedType || slot >= (DWORD)maxSlots) return -1;
    return (int)slot;
}

static inline DWORD mygdi_handle_hash(HANDLE h)
{
    DWORD v = (DWORD)h;
    v *= 2654435761u;
    v ^= v >> 16;
    return v ? v : 1u;
}

static inline int mygdi_handle_bucket(DWORD hash)
{
    return (int)(hash & MYGDI_HANDLE_HASH_MASK);
}

static void mygdi_free_init_locked(void)
{
    if (g_GdiFreeInit) return;
    g_GdiBrushFreeTop = 0;
    for (int i = MYGDI_MAX_BRUSHES - 1; i >= 0; --i) g_GdiBrushFree[g_GdiBrushFreeTop++] = i;
    g_GdiBitmapFreeTop = 0;
    for (int i = MYGDI_MAX_BITMAPS - 1; i >= 0; --i) g_GdiBitmapFree[g_GdiBitmapFreeTop++] = i;
    g_GdiRegionFreeTop = 0;
    for (int i = MYGDI_MAX_REGIONS - 1; i >= 0; --i) g_GdiRegionFree[g_GdiRegionFreeTop++] = i;
    g_GdiDcFreeTop = 0;
    for (int i = MYGDI_MAX_DCS - 1; i >= 0; --i) g_GdiDcFree[g_GdiDcFreeTop++] = i;
    g_GdiWindowFreeTop = 0;
    for (int i = MYGDI_MAX_WINDOWS - 1; i >= 0; --i) g_GdiWindowFree[g_GdiWindowFreeTop++] = i;
    g_GdiCommandFreeTop = 0;
    for (int i = MYGDI_MAX_COMMANDS - 1; i >= 0; --i) g_GdiCommandFree[g_GdiCommandFreeTop++] = i;
    g_GdiFreeInit = 1;
}

static int mygdi_pop_free_locked(int* stack, int* top)
{
    mygdi_free_init_locked();
    if (!stack || !top || *top <= 0) return -1;
    return stack[--(*top)];
}

static int mygdi_free_stack_contains_locked(const int* stack, int top, int slot)
{
    if (!stack || top <= 0) return 0;
    for (int i = 0; i < top; ++i) if (stack[i] == slot) return 1;
    return 0;
}

static void mygdi_remove_free_slot_locked(int* stack, int* top, int slot)
{
    mygdi_free_init_locked();
    if (!stack || !top || *top <= 0) return;
    for (int i = 0; i < *top; ++i) {
        if (stack[i] == slot) {
            stack[i] = stack[--(*top)];
            return;
        }
    }
}

static void mygdi_push_free_locked(int* stack, int* top, int max, int slot)
{
    mygdi_free_init_locked();
    if (!stack || !top || slot < 0 || slot >= max || *top >= max) return;
    if (mygdi_free_stack_contains_locked(stack, *top, slot)) return;
    stack[(*top)++] = slot;
}

static int mygdi_pop_free_valid_locked(int* stack, int* top, int max, const void* table, size_t stride)
{
    mygdi_free_init_locked();
    while (stack && top && *top > 0) {
        int slot = stack[--(*top)];
        if (slot >= 0 && slot < max) {
            const int* valid = (const int*)((const unsigned char*)table + (size_t)slot * stride);
            if (!*valid) return slot;
        }
    }
    return -1;
}

static void mygdi_hash_insert_brush_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_BRUSHES || !g_GdiBrushes[idx].valid || !g_GdiBrushes[idx].handle) return;
    DWORD h = mygdi_handle_hash(g_GdiBrushes[idx].handle);
    int b = mygdi_handle_bucket(h);
    g_GdiBrushes[idx].handleHash = h;
    g_GdiBrushes[idx].handleHashNext = g_GdiBrushHash[b];
    g_GdiBrushHash[b] = idx + 1;
}

static void mygdi_hash_remove_brush_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_BRUSHES || !g_GdiBrushes[idx].handleHash) return;
    int b = mygdi_handle_bucket(g_GdiBrushes[idx].handleHash);
    int* link = &g_GdiBrushHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_GdiBrushes[cur].handleHashNext; break; }
        link = &g_GdiBrushes[cur].handleHashNext;
    }
    g_GdiBrushes[idx].handleHash = 0;
    g_GdiBrushes[idx].handleHashNext = 0;
}

static void mygdi_hash_insert_bitmap_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_BITMAPS || !g_GdiBitmaps[idx].valid || !g_GdiBitmaps[idx].handle) return;
    DWORD h = mygdi_handle_hash(g_GdiBitmaps[idx].handle);
    int b = mygdi_handle_bucket(h);
    g_GdiBitmaps[idx].handleHash = h;
    g_GdiBitmaps[idx].handleHashNext = g_GdiBitmapHash[b];
    g_GdiBitmapHash[b] = idx + 1;
}

static void mygdi_hash_remove_bitmap_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_BITMAPS || !g_GdiBitmaps[idx].handleHash) return;
    int b = mygdi_handle_bucket(g_GdiBitmaps[idx].handleHash);
    int* link = &g_GdiBitmapHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_GdiBitmaps[cur].handleHashNext; break; }
        link = &g_GdiBitmaps[cur].handleHashNext;
    }
    g_GdiBitmaps[idx].handleHash = 0;
    g_GdiBitmaps[idx].handleHashNext = 0;
}

static void mygdi_hash_insert_region_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_REGIONS || !g_GdiRegions[idx].valid || !g_GdiRegions[idx].handle) return;
    DWORD h = mygdi_handle_hash(g_GdiRegions[idx].handle);
    int b = mygdi_handle_bucket(h);
    g_GdiRegions[idx].handleHash = h;
    g_GdiRegions[idx].handleHashNext = g_GdiRegionHash[b];
    g_GdiRegionHash[b] = idx + 1;
}

static void mygdi_hash_remove_region_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_REGIONS || !g_GdiRegions[idx].handleHash) return;
    int b = mygdi_handle_bucket(g_GdiRegions[idx].handleHash);
    int* link = &g_GdiRegionHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_GdiRegions[cur].handleHashNext; break; }
        link = &g_GdiRegions[cur].handleHashNext;
    }
    g_GdiRegions[idx].handleHash = 0;
    g_GdiRegions[idx].handleHashNext = 0;
}

static void mygdi_hash_insert_dc_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_DCS || !g_GdiDcs[idx].valid || !g_GdiDcs[idx].handle) return;
    DWORD h = mygdi_handle_hash(g_GdiDcs[idx].handle);
    int b = mygdi_handle_bucket(h);
    g_GdiDcs[idx].handleHash = h;
    g_GdiDcs[idx].handleHashNext = g_GdiDcHash[b];
    g_GdiDcHash[b] = idx + 1;
}

static void mygdi_hash_remove_dc_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_DCS || !g_GdiDcs[idx].handleHash) return;
    int b = mygdi_handle_bucket(g_GdiDcs[idx].handleHash);
    int* link = &g_GdiDcHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_GdiDcs[cur].handleHashNext; break; }
        link = &g_GdiDcs[cur].handleHashNext;
    }
    g_GdiDcs[idx].handleHash = 0;
    g_GdiDcs[idx].handleHashNext = 0;
}

static void mygdi_hash_insert_window_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_WINDOWS || !g_GdiWindows[idx].valid || !g_GdiWindows[idx].hwnd) return;
    DWORD h = mygdi_handle_hash(g_GdiWindows[idx].hwnd);
    int b = mygdi_handle_bucket(h);
    g_GdiWindows[idx].hwndHash = h;
    g_GdiWindows[idx].hwndHashNext = g_GdiWindowHash[b];
    g_GdiWindowHash[b] = idx + 1;
}

static void mygdi_hash_remove_window_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_WINDOWS || !g_GdiWindows[idx].hwndHash) return;
    int b = mygdi_handle_bucket(g_GdiWindows[idx].hwndHash);
    int* link = &g_GdiWindowHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_GdiWindows[cur].hwndHashNext; break; }
        link = &g_GdiWindows[cur].hwndHashNext;
    }
    g_GdiWindows[idx].hwndHash = 0;
    g_GdiWindows[idx].hwndHashNext = 0;
}

static int mygdi_rect_empty(const RECT* rc);
static COLORREF mygdi_canonical_color(COLORREF color);

static MyGdiWindowState* mygdi_window_locked(HWND hwnd, int create)
{
    DWORD hwndSlot = 0, hwndGen = 0;
    if (hwnd_decode(hwnd, &hwndSlot, &hwndGen) && hwndSlot < MYGDI_MAX_WINDOWS) {
        MyGdiWindowState* ws = &g_GdiWindows[hwndSlot];
        if (ws->valid && ws->hwnd == hwnd) return ws;
        if (create && !ws->valid) {
            mygdi_remove_free_slot_locked(g_GdiWindowFree, &g_GdiWindowFreeTop, (int)hwndSlot);
            memset(ws, 0, sizeof(*ws));
            ws->valid = 1;
            ws->hwnd = hwnd;
            mygdi_hash_insert_window_locked((int)hwndSlot);
            return ws;
        }
    }

    DWORD h = mygdi_handle_hash(hwnd);
    int b = mygdi_handle_bucket(h);
    for (int link = g_GdiWindowHash[b]; link; link = g_GdiWindows[link - 1].hwndHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYGDI_MAX_WINDOWS && g_GdiWindows[idx].valid &&
            g_GdiWindows[idx].hwndHash == h && g_GdiWindows[idx].hwnd == hwnd)
            return &g_GdiWindows[idx];
    }
    if (!create) return NULL;
    int i = mygdi_pop_free_valid_locked(g_GdiWindowFree, &g_GdiWindowFreeTop,
                                        MYGDI_MAX_WINDOWS, g_GdiWindows, sizeof(g_GdiWindows[0]));
    if (i < 0) return NULL;
    memset(&g_GdiWindows[i], 0, sizeof(g_GdiWindows[i]));
    g_GdiWindows[i].valid = 1;
    g_GdiWindows[i].hwnd = hwnd;
    mygdi_hash_insert_window_locked(i);
    return &g_GdiWindows[i];
}

static MyGdiBrushObj* mygdi_find_brush_locked(HBRUSH h)
{
    int slot = mygdi_decode_slot_handle((HANDLE)h, _OBJECT_TYPE_BRUSH, MYGDI_MAX_BRUSHES);
    if (slot >= 0 && g_GdiBrushes[slot].valid && g_GdiBrushes[slot].handle == h)
        return &g_GdiBrushes[slot];
    DWORD hh = mygdi_handle_hash(h);
    int b = mygdi_handle_bucket(hh);
    for (int link = g_GdiBrushHash[b]; link; link = g_GdiBrushes[link - 1].handleHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYGDI_MAX_BRUSHES && g_GdiBrushes[idx].valid &&
            g_GdiBrushes[idx].handleHash == hh && g_GdiBrushes[idx].handle == h) return &g_GdiBrushes[idx];
    }
    for (int i = 0; i < MYGDI_MAX_BRUSHES; i++)
        if (g_GdiBrushes[i].valid && g_GdiBrushes[i].handle == h) return &g_GdiBrushes[i];
    return NULL;
}

static MyGdiBitmapObj* mygdi_find_bitmap_locked(HBITMAP h)
{
    int slot = mygdi_decode_slot_handle((HANDLE)h, _OBJECT_TYPE_BITMAP, MYGDI_MAX_BITMAPS);
    if (slot >= 0 && g_GdiBitmaps[slot].valid && g_GdiBitmaps[slot].handle == h)
        return &g_GdiBitmaps[slot];
    DWORD hh = mygdi_handle_hash(h);
    int b = mygdi_handle_bucket(hh);
    for (int link = g_GdiBitmapHash[b]; link; link = g_GdiBitmaps[link - 1].handleHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYGDI_MAX_BITMAPS && g_GdiBitmaps[idx].valid &&
            g_GdiBitmaps[idx].handleHash == hh && g_GdiBitmaps[idx].handle == h) return &g_GdiBitmaps[idx];
    }
    for (int i = 0; i < MYGDI_MAX_BITMAPS; i++)
        if (g_GdiBitmaps[i].valid && g_GdiBitmaps[i].handle == h) return &g_GdiBitmaps[i];
    return NULL;
}

static void mygdi_update_bitmap_object_locked(const MyGdiBitmapObj* bmp)
{
    if (!bmp || !bmp->valid) return;
    DWORD flags = bmp->selectedCount ? _OBJECT_FLAG_GDI_SELECTED : 0;
    if (bmp->dibSection) flags |= _OBJECT_FLAG_GDI_DIBSECTION;
    _ObjectSetInfo(bmp->handle, flags, (DWORD)(bmp->widthBytes * bmp->height), NULL);
}

static void mygdi_bitmap_init_bmi(MyGdiBitmapObj* bmp, LONG signedHeight, DWORD compression)
{
    if (!bmp) return;
    memset(&bmp->dibHeader, 0, sizeof(bmp->dibHeader));
    bmp->dibHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmp->dibHeader.biWidth = bmp->width;
    bmp->dibHeader.biHeight = signedHeight;
    bmp->dibHeader.biPlanes = 1;
    bmp->dibHeader.biBitCount = (WORD)bmp->bpp;
    bmp->dibHeader.biCompression = compression;
    bmp->dibHeader.biSizeImage = (DWORD)(bmp->widthBytes * bmp->height);
}

static int mygdi_bitmap_index(const MyGdiBitmapObj* bmp, int x, int y)
{
    if (!bmp) return -1;
    int physicalY = bmp->dibTopDown ? y : (bmp->height - 1 - y);
    return physicalY * bmp->width + x;
}

static void mygdi_fill_bitmap_struct(const MyGdiBitmapObj* bmp, BITMAP* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!bmp) return;
    out->bmType = 0;
    out->bmWidth = bmp->width;
    out->bmHeight = bmp->height;
    out->bmWidthBytes = bmp->widthBytes;
    out->bmPlanes = 1;
    out->bmBitsPixel = (WORD)bmp->bpp;
    out->bmBits = bmp->pixels;
}

static MyGdiBitmapObj* mygdi_alloc_bitmap_locked(int width, int height, int bpp, const void* bits, int internalDefault)
{
    if (width <= 0 || height <= 0) return NULL;
    if (width > 8192 || height > 8192) return NULL;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) bpp = 32;

    int i = mygdi_pop_free_locked(g_GdiBitmapFree, &g_GdiBitmapFreeTop);
    if (i < 0) return NULL;

    MyGdiBitmapObj* bmp = &g_GdiBitmaps[i];
    memset(bmp, 0, sizeof(*bmp));
    bmp->pixels = (COLORREF*)calloc((size_t)width * (size_t)height, sizeof(COLORREF));
    if (!bmp->pixels) {
        memset(bmp, 0, sizeof(*bmp));
        mygdi_push_free_locked(g_GdiBitmapFree, &g_GdiBitmapFreeTop, MYGDI_MAX_BITMAPS, i);
        return NULL;
    }
    bmp->valid = 1;
    bmp->handle = _ObjectMakeSlotHandle(_OBJECT_TYPE_BITMAP, (DWORD)i);
    if (!bmp->handle) bmp->handle = g_NextBitmapHandle++;
    bmp->width = width;
    bmp->height = height;
    bmp->bpp = 32;
    bmp->widthBytes = width * 4;
    bmp->ownerPid = GetCurrentProcessId();
    bmp->internalDefault = internalDefault ? 1 : 0;
    bmp->dibSection = 0;
    bmp->dibTopDown = 1;
    mygdi_bitmap_init_bmi(bmp, height, BI_RGB);
    if (bits) {
        if (bpp == 32) {
            memcpy(bmp->pixels, bits, (size_t)width * (size_t)height * sizeof(COLORREF));
        } else {
            /* v156 keeps non-32-bit CreateBitmap alive but canonicalizes storage to 32-bit. */
            const BYTE* src = (const BYTE*)bits;
            int srcStride = ((width * bpp + 31) / 32) * 4;
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    if (bpp == 24) {
                        const BYTE* px = src + y * srcStride + x * 3;
                        bmp->pixels[y * width + x] = RGB(px[2], px[1], px[0]);
                    } else if (bpp == 16) {
                        WORD v = *(const WORD*)(const void*)(src + y * srcStride + x * 2);
                        BYTE r = (BYTE)(((v >> 10) & 0x1f) * 255 / 31);
                        BYTE g = (BYTE)(((v >> 5) & 0x1f) * 255 / 31);
                        BYTE b = (BYTE)((v & 0x1f) * 255 / 31);
                        bmp->pixels[y * width + x] = RGB(r,g,b);
                    }
                }
            }
        }
    }
    char name[96];
    snprintf(name, sizeof(name), "GDI\\BITMAP %dx%d%s", width, height, internalDefault ? " default" : "");
    mygdi_hash_insert_bitmap_locked(i);
    _ObjectRegister(bmp->handle, _OBJECT_TYPE_BITMAP, bmp->ownerPid,
                  _OBJECT_ACCESS_READ|_OBJECT_ACCESS_WRITE,
                  (DWORD)(width * height * 4), name);
    mygdi_update_bitmap_object_locked(bmp);
    return bmp;
}

static void mygdi_free_bitmap_locked(MyGdiBitmapObj* bmp)
{
    if (!bmp || !bmp->valid) return;
    int idx = (int)(bmp - g_GdiBitmaps);
    mygdi_hash_remove_bitmap_locked(idx);
    _ObjectUnregister(bmp->handle);
    free(bmp->pixels);
    memset(bmp, 0, sizeof(*bmp));
    mygdi_push_free_locked(g_GdiBitmapFree, &g_GdiBitmapFreeTop, MYGDI_MAX_BITMAPS, idx);
}

static MyGdiDCObj* mygdi_find_dc_locked(HDC h)
{
    int slot = mygdi_decode_slot_handle((HANDLE)h, _OBJECT_TYPE_DC, MYGDI_MAX_DCS);
    if (slot >= 0 && g_GdiDcs[slot].valid && g_GdiDcs[slot].handle == h)
        return &g_GdiDcs[slot];
    DWORD hh = mygdi_handle_hash(h);
    int b = mygdi_handle_bucket(hh);
    for (int link = g_GdiDcHash[b]; link; link = g_GdiDcs[link - 1].handleHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYGDI_MAX_DCS && g_GdiDcs[idx].valid &&
            g_GdiDcs[idx].handleHash == hh && g_GdiDcs[idx].handle == h) return &g_GdiDcs[idx];
    }
    for (int i = 0; i < MYGDI_MAX_DCS; i++)
        if (g_GdiDcs[i].valid && g_GdiDcs[i].handle == h) return &g_GdiDcs[i];
    return NULL;
}

static void mygdi_update_brush_object_locked(const MyGdiBrushObj* br)
{
    if (!br || !br->valid) return;
    DWORD flags = br->selectedCount ? _OBJECT_FLAG_GDI_SELECTED : 0;
    _ObjectSetInfo(br->handle, flags, br->color, NULL);
}

static void mygdi_register_dc_locked(const MyGdiDCObj* dc)
{
    if (!dc || !dc->valid) return;
    mygdi_hash_insert_dc_locked((int)(dc - g_GdiDcs));
    char name[96];
    snprintf(name, sizeof(name), "GDI\\DC hwnd=%u %s", dc->hwnd, dc->paintDc ? "paint" : "window");
    _ObjectRegister(dc->handle, _OBJECT_TYPE_DC, GetCurrentProcessId(),
                  _OBJECT_ACCESS_READ|_OBJECT_ACCESS_WRITE, (DWORD)dc->hwnd, name);
    _ObjectSetInfo(dc->handle, dc->paintDc ? _OBJECT_FLAG_DC_PAINT : 0, (DWORD)dc->hwnd, NULL);
}

static MyGdiDCObj* mygdi_alloc_dc_locked(void)
{
    int idx = mygdi_pop_free_locked(g_GdiDcFree, &g_GdiDcFreeTop);
    if (idx < 0) return NULL;
    MyGdiDCObj* dc = &g_GdiDcs[idx];
    memset(dc, 0, sizeof(*dc));
    dc->valid = 1;
    dc->handle = _ObjectMakeSlotHandle(_OBJECT_TYPE_DC, (DWORD)idx);
    if (!dc->handle) dc->handle = g_NextDcHandle++;
    return dc;
}

static void mygdi_free_dc_locked(MyGdiDCObj* dc)
{
    if (!dc || !dc->valid) return;
    int idx = (int)(dc - g_GdiDcs);
    mygdi_hash_remove_dc_locked(idx);
    _ObjectUnregister(dc->handle);
    memset(dc, 0, sizeof(*dc));
    mygdi_push_free_locked(g_GdiDcFree, &g_GdiDcFreeTop, MYGDI_MAX_DCS, idx);
}

static void mygdi_clear_command_locked(MyGdiCommand* c)
{
    if (!c) return;
    if (c->blitPixels) free(c->blitPixels);
    memset(c, 0, sizeof(*c));
}

static int mygdi_alloc_command_slot_locked(void)
{
    mygdi_free_init_locked();
    if (g_GdiCommandFreeTop <= 0) return -1;
    return g_GdiCommandFree[--g_GdiCommandFreeTop];
}

static void mygdi_free_command_slot_locked(int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_COMMANDS) return;
    mygdi_clear_command_locked(&g_GdiCommands[idx]);
    if (g_GdiCommandFreeTop < MYGDI_MAX_COMMANDS)
        g_GdiCommandFree[g_GdiCommandFreeTop++] = idx;
}

static void mygdi_link_command_to_window_locked(HWND hwnd, int idx)
{
    if (idx < 0 || idx >= MYGDI_MAX_COMMANDS) return;
    MyGdiWindowState* ws = mygdi_window_locked(hwnd, 1);
    g_GdiCommands[idx].windowNext = 0;
    if (!ws) return;
    if (ws->lastCommand) g_GdiCommands[ws->lastCommand - 1].windowNext = idx + 1;
    else ws->firstCommand = idx + 1;
    ws->lastCommand = idx + 1;
    ws->commandCount++;
}

static void mygdi_clear_commands_locked(HWND hwnd)
{
    MyGdiWindowState* ws = mygdi_window_locked(hwnd, 0);
    if (ws && ws->firstCommand) {
        int link = ws->firstCommand;
        int guard = 0;
        ws->firstCommand = 0;
        ws->lastCommand = 0;
        ws->commandCount = 0;
        while (link && guard++ < MYGDI_MAX_COMMANDS) {
            int idx = link - 1;
            if (idx < 0 || idx >= MYGDI_MAX_COMMANDS) break;
            int next = g_GdiCommands[idx].windowNext;
            mygdi_free_command_slot_locked(idx);
            link = next;
        }
        return;
    }

    for (int i = 0; i < MYGDI_MAX_COMMANDS; i++) {
        if (g_GdiCommands[i].valid && g_GdiCommands[i].hwnd == hwnd)
            mygdi_free_command_slot_locked(i);
    }
}

void MyGdiReleaseWindow(HWND hWnd)
{
    if (!hWnd) return;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, 0);
    if (ws) {
        int idx = (int)(ws - g_GdiWindows);
        mygdi_clear_commands_locked(hWnd);
        mygdi_hash_remove_window_locked(idx);
        memset(ws, 0, sizeof(*ws));
        mygdi_push_free_locked(g_GdiWindowFree, &g_GdiWindowFreeTop, MYGDI_MAX_WINDOWS, idx);
    }
    pthread_mutex_unlock(&g_GdiLock);
}

static int mygdi_collect_command_indices_locked(HWND hwnd, int* out, int maxOut)
{
    int n = 0;
    if (!out || maxOut <= 0) return 0;
    MyGdiWindowState* ws = mygdi_window_locked(hwnd, 0);
    if (ws && ws->firstCommand) {
        int link = ws->firstCommand;
        int guard = 0;
        while (link && n < maxOut && guard++ < MYGDI_MAX_COMMANDS) {
            int idx = link - 1;
            if (idx < 0 || idx >= MYGDI_MAX_COMMANDS) break;
            MyGdiCommand* c = &g_GdiCommands[idx];
            if (!c->valid || c->hwnd != hwnd) break;
            out[n++] = idx;
            link = c->windowNext;
        }
        if (!link && guard <= MYGDI_MAX_COMMANDS) return n;
        n = 0; /* corrupted list edge: fall back to table scan */
    }
    for (int i = 0; i < MYGDI_MAX_COMMANDS && n < maxOut; ++i)
        if (g_GdiCommands[i].valid && g_GdiCommands[i].hwnd == hwnd) out[n++] = i;
    return n;
}

static int mygdi_append_locked(HWND hwnd, MyGdiCommandType type, const RECT* rc, COLORREF color, LPCSTR text, int textLen)
{
    int idx = mygdi_alloc_command_slot_locked();
    if (idx < 0) return 0;
    MyGdiCommand* c = &g_GdiCommands[idx];
    memset(c, 0, sizeof(*c));
    c->valid = 1;
    c->hwnd = hwnd;
    c->type = type;
    if (rc) c->rc = *rc;
    c->color = color;
    if (text) {
        int n = textLen >= 0 ? textLen : (int)strlen(text);
        if (n < 0) n = 0;
        if (n > (int)sizeof(c->text)-1) n = (int)sizeof(c->text)-1;
        memcpy(c->text, text, (size_t)n);
        c->text[n] = 0;
    }
    mygdi_link_command_to_window_locked(hwnd, idx);
    return 1;
}

static int mygdi_append_blit_locked(HWND hwnd, const RECT* dst, int srcX, int srcY, int w, int h, const COLORREF* pixels)
{
    if (!pixels || !dst || w <= 0 || h <= 0) return 0;
    int idx = mygdi_alloc_command_slot_locked();
    if (idx < 0) return 0;
    MyGdiCommand* c = &g_GdiCommands[idx];
    memset(c, 0, sizeof(*c));
    c->valid = 1;
    c->hwnd = hwnd;
    c->type = MYGDI_CMD_BLIT;
    c->rc = *dst;
    c->srcX = srcX;
    c->srcY = srcY;
    c->blitW = w;
    c->blitH = h;
    c->blitPixels = (COLORREF*)malloc((size_t)w * (size_t)h * sizeof(COLORREF));
    if (!c->blitPixels) { mygdi_free_command_slot_locked(idx); return 0; }
    memcpy(c->blitPixels, pixels, (size_t)w * (size_t)h * sizeof(COLORREF));
    mygdi_link_command_to_window_locked(hwnd, idx);
    return 1;
}

static int mygdi_valid_patblt_rop(DWORD rop)
{
    return rop == PATCOPY || rop == PATINVERT || rop == DSTINVERT ||
           rop == BLACKNESS || rop == WHITENESS;
}

static int mygdi_patblt_rop_needs_brush(DWORD rop)
{
    return rop == PATCOPY || rop == PATINVERT;
}

static COLORREF mygdi_apply_patblt_rop(DWORD rop, COLORREF dst, COLORREF pattern)
{
    dst &= 0x00FFFFFFu;
    pattern &= 0x00FFFFFFu;
    switch (rop) {
    case PATCOPY:   return pattern;
    case PATINVERT: return (dst ^ pattern) & 0x00FFFFFFu;
    case DSTINVERT: return (~dst) & 0x00FFFFFFu;
    case BLACKNESS: return RGB(0,0,0);
    case WHITENESS: return RGB(255,255,255);
    default:        return dst;
    }
}

static int mygdi_append_patblt_locked(HWND hwnd, const RECT* rc, COLORREF brushColor, DWORD rop)
{
    if (!rc || mygdi_rect_empty(rc) || !mygdi_valid_patblt_rop(rop)) return 0;
    int idx = mygdi_alloc_command_slot_locked();
    if (idx < 0) return 0;
    MyGdiCommand* c = &g_GdiCommands[idx];
    memset(c, 0, sizeof(*c));
    c->valid = 1;
    c->hwnd = hwnd;
    c->type = MYGDI_CMD_PATBLT;
    c->rc = *rc;
    c->color = mygdi_canonical_color(brushColor);
    c->rop = rop;
    mygdi_link_command_to_window_locked(hwnd, idx);
    return 1;
}

static int mygdi_rect_empty(const RECT* rc)
{
    return !rc || rc->right <= rc->left || rc->bottom <= rc->top;
}

static RECT mygdi_default_update_rect(void)
{
    RECT rc;
    rc.left = 0; rc.top = 0; rc.right = 4096; rc.bottom = 4096;
    return rc;
}

static void mygdi_union_rect(RECT* dst, const RECT* src)
{
    if (!dst || !src) return;
    if (src->left < dst->left) dst->left = src->left;
    if (src->top < dst->top) dst->top = src->top;
    if (src->right > dst->right) dst->right = src->right;
    if (src->bottom > dst->bottom) dst->bottom = src->bottom;
}

static int mygdi_rects_equal(const RECT* a, const RECT* b)
{
    return a && b && a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

static int mygdi_rect_intersect(RECT* out, const RECT* a, const RECT* b)
{
    if (!out || !a || !b) return 0;
    out->left = a->left > b->left ? a->left : b->left;
    out->top = a->top > b->top ? a->top : b->top;
    out->right = a->right < b->right ? a->right : b->right;
    out->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (mygdi_rect_empty(out)) { memset(out, 0, sizeof(*out)); return 0; }
    return 1;
}

static int mygdi_rect_cmp(const void* av, const void* bv)
{
    const RECT* a = (const RECT*)av;
    const RECT* b = (const RECT*)bv;
    if (a->top != b->top) return (a->top < b->top) ? -1 : 1;
    if (a->left != b->left) return (a->left < b->left) ? -1 : 1;
    if (a->bottom != b->bottom) return (a->bottom < b->bottom) ? -1 : 1;
    if (a->right != b->right) return (a->right < b->right) ? -1 : 1;
    return 0;
}

static void mygdi_region_clear(MyGdiRegionData* r)
{
    if (r) memset(r, 0, sizeof(*r));
}

static void mygdi_region_copy(MyGdiRegionData* dst, const MyGdiRegionData* src)
{
    if (!dst) return;
    if (!src) { mygdi_region_clear(dst); return; }
    *dst = *src;
}

static int mygdi_region_type(const MyGdiRegionData* r)
{
    if (!r || r->count <= 0) return NULLREGION;
    return (r->count == 1) ? SIMPLEREGION : COMPLEXREGION;
}

static int mygdi_region_bounds(const MyGdiRegionData* r, RECT* out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!r || r->count <= 0) return NULLREGION;
    RECT b = r->rects[0];
    for (int i = 1; i < r->count; ++i) mygdi_union_rect(&b, &r->rects[i]);
    if (out) *out = b;
    return mygdi_region_type(r);
}

static int mygdi_region_add_raw(MyGdiRegionData* r, const RECT* rc)
{
    if (!r || mygdi_rect_empty(rc)) return 1;
    if (r->count >= MYGDI_REGION_MAX_RECTS) return 0;
    r->rects[r->count++] = *rc;
    return 1;
}

static int mygdi_region_can_merge(const RECT* a, const RECT* b, RECT* out)
{
    if (!a || !b || !out) return 0;
    if (mygdi_rects_equal(a, b)) { *out = *a; return 1; }
    if (a->top == b->top && a->bottom == b->bottom &&
        !(a->right < b->left || b->right < a->left)) {
        out->left = a->left < b->left ? a->left : b->left;
        out->top = a->top;
        out->right = a->right > b->right ? a->right : b->right;
        out->bottom = a->bottom;
        return 1;
    }
    if (a->left == b->left && a->right == b->right &&
        !(a->bottom < b->top || b->bottom < a->top)) {
        out->left = a->left;
        out->top = a->top < b->top ? a->top : b->top;
        out->right = a->right;
        out->bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
        return 1;
    }
    return 0;
}

static void mygdi_region_canonicalize(MyGdiRegionData* r)
{
    if (!r) return;
    int w = 0;
    for (int i = 0; i < r->count; ++i) if (!mygdi_rect_empty(&r->rects[i])) r->rects[w++] = r->rects[i];
    r->count = w;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < r->count && !changed; ++i) {
            for (int j = i + 1; j < r->count; ++j) {
                RECT m;
                if (mygdi_region_can_merge(&r->rects[i], &r->rects[j], &m)) {
                    r->rects[i] = m;
                    memmove(&r->rects[j], &r->rects[j + 1], (size_t)(r->count - j - 1) * sizeof(RECT));
                    r->count--;
                    changed = 1;
                    break;
                }
            }
        }
    }
    if (r->count > 1) qsort(r->rects, (size_t)r->count, sizeof(RECT), mygdi_rect_cmp);
}

static void mygdi_region_from_rect(MyGdiRegionData* r, const RECT* rc)
{
    mygdi_region_clear(r);
    if (!mygdi_rect_empty(rc)) {
        r->rects[0] = *rc;
        r->count = 1;
    }
}

static int mygdi_region_contains_point(const MyGdiRegionData* r, int x, int y)
{
    if (!r) return 0;
    for (int i = 0; i < r->count; ++i) {
        const RECT* a = &r->rects[i];
        if (x >= a->left && x < a->right && y >= a->top && y < a->bottom) return 1;
    }
    return 0;
}

static int mygdi_region_rect_intersects(const MyGdiRegionData* r, const RECT* rc)
{
    if (!r || mygdi_rect_empty(rc)) return 0;
    RECT hit;
    for (int i = 0; i < r->count; ++i) if (mygdi_rect_intersect(&hit, &r->rects[i], rc)) return 1;
    return 0;
}

static int mygdi_add_unique_long(LONG* vals, int* n, LONG v)
{
    if (!vals || !n) return 0;
    for (int i = 0; i < *n; ++i) if (vals[i] == v) return 1;
    if (*n >= MYGDI_REGION_MAX_RECTS * 4) return 0;
    vals[(*n)++] = v;
    return 1;
}

static int mygdi_long_cmp(const void* av, const void* bv)
{
    LONG a = *(const LONG*)av;
    LONG b = *(const LONG*)bv;
    return (a > b) - (a < b);
}

static int mygdi_region_bool_pred(int inA, int inB, int mode)
{
    switch (mode) {
    case RGN_AND: return inA && inB;
    case RGN_OR:  return inA || inB;
    case RGN_XOR: return (!!inA) != (!!inB);
    case RGN_DIFF:return inA && !inB;
    case RGN_COPY:return inA;
    default: return 0;
    }
}

static int mygdi_region_combine_data(MyGdiRegionData* dst, const MyGdiRegionData* a, const MyGdiRegionData* b, int mode)
{
    if (!dst || !a) return ERROR;
    if (mode == RGN_COPY) {
        mygdi_region_copy(dst, a);
        mygdi_region_canonicalize(dst);
        return mygdi_region_type(dst);
    }
    if (!b) return ERROR;

    LONG xs[MYGDI_REGION_MAX_RECTS * 4];
    LONG ys[MYGDI_REGION_MAX_RECTS * 4];
    int nx = 0, ny = 0;
    for (int k = 0; k < 2; ++k) {
        const MyGdiRegionData* r = k == 0 ? a : b;
        for (int i = 0; r && i < r->count; ++i) {
            const RECT* rc = &r->rects[i];
            if (mygdi_rect_empty(rc)) continue;
            if (!mygdi_add_unique_long(xs, &nx, rc->left) || !mygdi_add_unique_long(xs, &nx, rc->right) ||
                !mygdi_add_unique_long(ys, &ny, rc->top) || !mygdi_add_unique_long(ys, &ny, rc->bottom)) {
                return ERROR;
            }
        }
    }
    mygdi_region_clear(dst);
    if (nx < 2 || ny < 2) return NULLREGION;
    qsort(xs, (size_t)nx, sizeof(LONG), mygdi_long_cmp);
    qsort(ys, (size_t)ny, sizeof(LONG), mygdi_long_cmp);
    for (int yi = 0; yi < ny - 1; ++yi) {
        for (int xi = 0; xi < nx - 1; ++xi) {
            RECT cell = { xs[xi], ys[yi], xs[xi + 1], ys[yi + 1] };
            if (mygdi_rect_empty(&cell)) continue;
            int inA = mygdi_region_contains_point(a, (int)cell.left, (int)cell.top);
            int inB = mygdi_region_contains_point(b, (int)cell.left, (int)cell.top);
            if (mygdi_region_bool_pred(inA, inB, mode)) {
                if (!mygdi_region_add_raw(dst, &cell)) return ERROR;
            }
        }
    }
    mygdi_region_canonicalize(dst);
    return mygdi_region_type(dst);
}

static MyGdiRegionObj* mygdi_find_region_locked(HRGN h)
{
    int slot = mygdi_decode_slot_handle((HANDLE)h, _OBJECT_TYPE_REGION, MYGDI_MAX_REGIONS);
    if (slot >= 0 && g_GdiRegions[slot].valid && g_GdiRegions[slot].handle == h)
        return &g_GdiRegions[slot];
    DWORD hh = mygdi_handle_hash(h);
    int b = mygdi_handle_bucket(hh);
    for (int link = g_GdiRegionHash[b]; link; link = g_GdiRegions[link - 1].handleHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYGDI_MAX_REGIONS && g_GdiRegions[idx].valid &&
            g_GdiRegions[idx].handleHash == hh && g_GdiRegions[idx].handle == h) return &g_GdiRegions[idx];
    }
    for (int i = 0; i < MYGDI_MAX_REGIONS; i++)
        if (g_GdiRegions[i].valid && g_GdiRegions[i].handle == h) return &g_GdiRegions[i];
    return NULL;
}

static MyGdiRegionObj* mygdi_alloc_region_locked(const MyGdiRegionData* initial)
{
    int i = mygdi_pop_free_locked(g_GdiRegionFree, &g_GdiRegionFreeTop);
    if (i < 0) return NULL;
    MyGdiRegionObj* r = &g_GdiRegions[i];
    memset(r, 0, sizeof(*r));
    r->valid = 1;
    r->handle = _ObjectMakeSlotHandle(_OBJECT_TYPE_REGION, (DWORD)i);
    if (!r->handle) r->handle = g_NextRegionHandle++;
    r->ownerPid = GetCurrentProcessId();
    if (initial) r->data = *initial;
    mygdi_region_canonicalize(&r->data);
    char name[96];
    RECT box;
    int kind = mygdi_region_bounds(&r->data, &box);
    snprintf(name, sizeof(name), "GDI\\RGN type=%d rects=%d", kind, r->data.count);
    mygdi_hash_insert_region_locked(i);
    _ObjectRegister(r->handle, _OBJECT_TYPE_REGION, r->ownerPid, _OBJECT_ACCESS_READ|_OBJECT_ACCESS_WRITE, (DWORD)r->data.count, name);
    return r;
}

static void mygdi_update_region_object_locked(const MyGdiRegionObj* r)
{
    if (!r || !r->valid) return;
    RECT box;
    int kind = mygdi_region_bounds(&r->data, &box);
    (void)box;
    char name[96];
    snprintf(name, sizeof(name), "GDI\\RGN type=%d rects=%d", kind, r->data.count);
    _ObjectSetInfo(r->handle, 0, (DWORD)r->data.count, name);
}

static void mygdi_free_region_locked(MyGdiRegionObj* r)
{
    if (!r || !r->valid) return;
    int idx = (int)(r - g_GdiRegions);
    mygdi_hash_remove_region_locked(idx);
    _ObjectUnregister(r->handle);
    memset(r, 0, sizeof(*r));
    mygdi_push_free_locked(g_GdiRegionFree, &g_GdiRegionFreeTop, MYGDI_MAX_REGIONS, idx);
}

static void mygdi_window_refresh_bounds_locked(MyGdiWindowState* ws)
{
    if (!ws) return;
    RECT box;
    int type = mygdi_region_bounds(&ws->updateRegion, &box);
    if (type == NULLREGION) {
        memset(&ws->dirtyRect, 0, sizeof(ws->dirtyRect));
        ws->dirty = 0;
    } else {
        ws->dirtyRect = box;
        ws->dirty = 1;
    }
}

static int mygdi_is_valid_window(HWND hWnd)
{
    /* v222: paint/DC entrypoints consume the USER32 HWND state machine.
       A HWND is paintable only if the resolved lifecycle state allows the
       private PAINT action; this keeps dead/zombie HWNDs out of GDI without
       every GDI API re-deriving USER32 lifecycle truth. */
    HWNDManager* hm = MyWinGetHwndManager();
    return (hWnd && hm && hwnd_query_action(hm, hWnd, _HWND_ACTION_PAINT, NULL)) ? 1 : 0;
}

static void mygdi_remove_pending_paint(HWND hWnd)
{
    HWNDManager* hm = MyWinGetHwndManager();
    if (hm && hWnd) hwnd_remove_queued_messages_for_hwnd(hm, hWnd, WM_PAINT, WM_PAINT);
}

static COLORREF mygdi_canonical_color(COLORREF color)
{
    return color & 0x00ffffffu;
}

static void mygdi_set_pixel_bitmap(MyGdiBitmapObj* bmp, int x, int y, COLORREF color)
{
    if (!bmp || !bmp->valid || !bmp->pixels) return;
    if (x < 0 || y < 0 || x >= bmp->width || y >= bmp->height) return;
    bmp->pixels[mygdi_bitmap_index(bmp, x, y)] = mygdi_canonical_color(color);
}

static COLORREF mygdi_get_pixel_bitmap(const MyGdiBitmapObj* bmp, int x, int y)
{
    if (!bmp || !bmp->valid || !bmp->pixels) return CLR_INVALID;
    if (x < 0 || y < 0 || x >= bmp->width || y >= bmp->height) return CLR_INVALID;
    return bmp->pixels[mygdi_bitmap_index(bmp, x, y)] & 0x00ffffffu;
}

static int mygdi_dib_abs_height(LONG h)
{
    return h < 0 ? (int)(-h) : (int)h;
}

static int mygdi_dib_stride_32(int width)
{
    if (width <= 0) return 0;
    return (int)((((unsigned long long)width * 32ull + 31ull) / 32ull) * 4ull);
}

static int mygdi_validate_dib32_bmi(const BITMAPINFO* bmi, UINT usage, int requireHeight,
                                    int* widthOut, int* heightOut, int* topDownOut)
{
    if (widthOut) *widthOut = 0;
    if (heightOut) *heightOut = 0;
    if (topDownOut) *topDownOut = 1;
    if (!bmi) return 0;
    const BITMAPINFOHEADER* bih = &bmi->bmiHeader;
    if (bih->biSize != sizeof(BITMAPINFOHEADER)) return 0;
    if (usage != DIB_RGB_COLORS) return 0;
    if (bih->biWidth <= 0) return 0;
    if (requireHeight && bih->biHeight == 0) return 0;
    if (bih->biPlanes != 0 && bih->biPlanes != 1) return 0;
    if (bih->biBitCount != 0 && bih->biBitCount != 32) return 0;
    if (bih->biCompression != 0 && bih->biCompression != BI_RGB) return 0;
    int h = mygdi_dib_abs_height(bih->biHeight);
    if (requireHeight && h <= 0) return 0;
    if (bih->biWidth > 8192 || h > 8192) return 0;
    if (widthOut) *widthOut = (int)bih->biWidth;
    if (heightOut) *heightOut = h;
    if (topDownOut) *topDownOut = bih->biHeight < 0;
    return 1;
}

static COLORREF mygdi_dib32_read_color(const BYTE* row, int x)
{
    const BYTE* p = row + (size_t)x * 4u;
    return RGB(p[2], p[1], p[0]);
}

static void mygdi_dib32_write_color(BYTE* row, int x, COLORREF c)
{
    BYTE* p = row + (size_t)x * 4u;
    c = mygdi_canonical_color(c);
    p[0] = (BYTE)(c & 0xffu);
    p[1] = (BYTE)((c >> 8) & 0xffu);
    p[2] = (BYTE)((c >> 16) & 0xffu);
    p[3] = 0;
}

static int mygdi_dib32_physical_y(int dibHeight, int topDown, int logicalY)
{
    if (logicalY < 0 || logicalY >= dibHeight) return -1;
    return topDown ? logicalY : (dibHeight - 1 - logicalY);
}

static COLORREF mygdi_dib32_get_logical_pixel(const BYTE* bits, int width, int height, int topDown, int x, int y)
{
    if (!bits || x < 0 || y < 0 || x >= width || y >= height) return CLR_INVALID;
    int py = mygdi_dib32_physical_y(height, topDown, y);
    if (py < 0) return CLR_INVALID;
    const BYTE* row = bits + (size_t)py * (size_t)mygdi_dib_stride_32(width);
    return mygdi_dib32_read_color(row, x);
}

static int mygdi_fill_bitmap(MyGdiBitmapObj* bmp, const RECT* inRc, COLORREF color)
{
    if (!bmp || !bmp->valid || !bmp->pixels || !inRc) return 0;
    int left = inRc->left < 0 ? 0 : (int)inRc->left;
    int top = inRc->top < 0 ? 0 : (int)inRc->top;
    int right = inRc->right > bmp->width ? bmp->width : (int)inRc->right;
    int bottom = inRc->bottom > bmp->height ? bmp->height : (int)inRc->bottom;
    if (right <= left || bottom <= top) return 1;
    COLORREF c = mygdi_canonical_color(color);
    for (int y = top; y < bottom; y++)
        for (int x = left; x < right; x++)
            bmp->pixels[mygdi_bitmap_index(bmp, x, y)] = c;
    return 1;
}

static int mygdi_rect_bitmap(MyGdiBitmapObj* bmp, const RECT* rc, COLORREF color)
{
    if (!bmp || !rc) return 0;
    for (int x = rc->left; x < rc->right; x++) {
        mygdi_set_pixel_bitmap(bmp, x, rc->top, color);
        mygdi_set_pixel_bitmap(bmp, x, rc->bottom - 1, color);
    }
    for (int y = rc->top; y < rc->bottom; y++) {
        mygdi_set_pixel_bitmap(bmp, rc->left, y, color);
        mygdi_set_pixel_bitmap(bmp, rc->right - 1, y, color);
    }
    return 1;
}

static void mygdi_unselect_brush_locked(MyGdiDCObj* dc)
{
    if (!dc || !dc->selectedBrush) return;
    MyGdiBrushObj* br = mygdi_find_brush_locked(dc->selectedBrush);
    if (br && br->selectedCount) { br->selectedCount--; mygdi_update_brush_object_locked(br); }
    dc->selectedBrush = 0;
}

static void mygdi_unselect_bitmap_locked(MyGdiDCObj* dc)
{
    if (!dc || !dc->selectedBitmap) return;
    MyGdiBitmapObj* bmp = mygdi_find_bitmap_locked(dc->selectedBitmap);
    if (bmp && bmp->selectedCount) { bmp->selectedCount--; mygdi_update_bitmap_object_locked(bmp); }
    dc->selectedBitmap = 0;
}

static MyGdiBitmapObj* mygdi_selected_bitmap_locked(MyGdiDCObj* dc)
{
    if (!dc || !dc->memoryDc || !dc->selectedBitmap) return NULL;
    return mygdi_find_bitmap_locked(dc->selectedBitmap);
}

static BOOL mygdi_mark_dirty_region(HWND hWnd, const MyGdiRegionData* region, BOOL bErase, int internalPaint, int postPaint)
{
    if (!mygdi_is_valid_window(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    if ((!region || region->count <= 0) && !internalPaint) return TRUE;

    int shouldPost = 0;
    int ok = 0;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, 1);
    if (ws) {
        if (region && region->count > 0) {
            MyGdiRegionData combined;
            if (ws->dirty || ws->updateRegion.count > 0) {
                int r = mygdi_region_combine_data(&combined, &ws->updateRegion, region, RGN_OR);
                if (r == ERROR) { pthread_mutex_unlock(&g_GdiLock); return FALSE; }
                ws->updateRegion = combined;
            } else {
                ws->updateRegion = *region;
                mygdi_region_canonicalize(&ws->updateRegion);
            }
        } else if (internalPaint && ws->updateRegion.count == 0) {
            RECT rc = mygdi_default_update_rect();
            mygdi_region_from_rect(&ws->updateRegion, &rc);
        }
        mygdi_window_refresh_bounds_locked(ws);
        if (bErase) ws->erasePending = 1;
        if (internalPaint) ws->internalPaint = 1;
        ws->invalidateSerial++;
        if (postPaint && !ws->paintPending) {
            ws->paintPending = 1;
            ws->postedPaints++;
            shouldPost = 1;
        } else if (postPaint && ws->paintPending) {
            ws->coalescedInvalidates++;
        }
        ok = 1;
    }
    pthread_mutex_unlock(&g_GdiLock);

    if (shouldPost) {
        HWNDManager* hm = MyWinGetHwndManager();
        const Capability* cap = MyWinGetCurrentCapability();
        if (hm && cap) hwnd_post(hm, cap, hWnd, WM_PAINT, 0, 0);
    }
    return ok ? TRUE : FALSE;
}

static BOOL mygdi_mark_dirty(HWND hWnd, const RECT* lpRect, BOOL bErase, int internalPaint, int postPaint)
{
    RECT rc = lpRect ? *lpRect : mygdi_default_update_rect();
    MyGdiRegionData r;
    mygdi_region_from_rect(&r, &rc);
    return mygdi_mark_dirty_region(hWnd, &r, bErase, internalPaint, postPaint);
}

BOOL InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase)
{
    return mygdi_mark_dirty(hWnd, lpRect, bErase, 0, 1);
}

BOOL InvalidateRgn(HWND hWnd, HRGN hRgn, BOOL bErase)
{
    if (!mygdi_is_valid_window(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    if (!hRgn) return InvalidateRect(hWnd, NULL, bErase);
    MyGdiRegionData r;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* obj = mygdi_find_region_locked(hRgn);
    if (!obj) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    r = obj->data;
    pthread_mutex_unlock(&g_GdiLock);
    return mygdi_mark_dirty_region(hWnd, &r, bErase, 0, 1);
}

BOOL ValidateRect(HWND hWnd, const RECT* lpRect)
{
    if (!mygdi_is_valid_window(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    int becameClean = 0;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, 0);
    if (ws && (ws->dirty || ws->internalPaint || ws->paintPending)) {
        if (!lpRect) {
            mygdi_region_clear(&ws->updateRegion);
        } else {
            MyGdiRegionData sub, out;
            mygdi_region_from_rect(&sub, lpRect);
            int r = mygdi_region_combine_data(&out, &ws->updateRegion, &sub, RGN_DIFF);
            if (r != ERROR) ws->updateRegion = out;
        }
        mygdi_window_refresh_bounds_locked(ws);
        if (ws->updateRegion.count == 0) {
            ws->paintPending = 0;
            ws->erasePending = 0;
            ws->internalPaint = 0;
            becameClean = 1;
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    if (becameClean) mygdi_remove_pending_paint(hWnd);
    return TRUE;
}

BOOL ValidateRgn(HWND hWnd, HRGN hRgn)
{
    if (!mygdi_is_valid_window(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    if (!hRgn) return ValidateRect(hWnd, NULL);
    int becameClean = 0;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* rgn = mygdi_find_region_locked(hRgn);
    if (!rgn) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, 0);
    if (ws && (ws->dirty || ws->internalPaint || ws->paintPending)) {
        MyGdiRegionData out;
        int r = mygdi_region_combine_data(&out, &ws->updateRegion, &rgn->data, RGN_DIFF);
        if (r != ERROR) ws->updateRegion = out;
        mygdi_window_refresh_bounds_locked(ws);
        if (ws->updateRegion.count == 0) {
            ws->paintPending = 0;
            ws->erasePending = 0;
            ws->internalPaint = 0;
            becameClean = 1;
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    if (becameClean) mygdi_remove_pending_paint(hWnd);
    return TRUE;
}

BOOL GetUpdateRect(HWND hWnd, LPRECT lpRect, BOOL bErase)
{
    if (!mygdi_is_valid_window(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    int dirty = 0;
    RECT rc = mygdi_default_update_rect();
    pthread_mutex_lock(&g_GdiLock);
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, 0);
    if (ws && (ws->dirty || ws->internalPaint)) {
        dirty = 1;
        rc = ws->dirtyRect;
        if (bErase) ws->erasePending = 1;
    }
    pthread_mutex_unlock(&g_GdiLock);
    if (dirty && lpRect) *lpRect = rc;
    return dirty ? TRUE : FALSE;
}

int GetUpdateRgn(HWND hWnd, HRGN hRgn, BOOL bErase)
{
    if (!mygdi_is_valid_window(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return ERROR; }
    if (!hRgn) { SetLastError(ERROR_INVALID_HANDLE); return ERROR; }
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* dst = mygdi_find_region_locked(hRgn);
    if (!dst) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return ERROR; }
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, 0);
    int result = NULLREGION;
    if (ws && (ws->dirty || ws->internalPaint)) {
        dst->data = ws->updateRegion;
        mygdi_region_canonicalize(&dst->data);
        mygdi_update_region_object_locked(dst);
        result = mygdi_region_type(&dst->data);
        if (bErase) ws->erasePending = 1;
    } else {
        mygdi_region_clear(&dst->data);
        mygdi_update_region_object_locked(dst);
    }
    pthread_mutex_unlock(&g_GdiLock);
    return result;
}

static BOOL mygdi_peek_update_state(HWND hWnd, BOOL* lpDirty, BOOL* lpErasePending)
{
    if (lpDirty) *lpDirty = FALSE;
    if (lpErasePending) *lpErasePending = FALSE;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, 0);
    if (ws) {
        if (lpDirty) *lpDirty = (ws->dirty || ws->internalPaint) ? TRUE : FALSE;
        if (lpErasePending) *lpErasePending = ws->erasePending ? TRUE : FALSE;
    }
    pthread_mutex_unlock(&g_GdiLock);
    return ws ? TRUE : FALSE;
}

static void mygdi_set_erase_pending(HWND hWnd, BOOL pending)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, pending ? 1 : 0);
    if (ws) ws->erasePending = pending ? 1 : 0;
    pthread_mutex_unlock(&g_GdiLock);
}

BOOL UpdateWindow(HWND hWnd)
{
    if (!mygdi_is_valid_window(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    BOOL dirty = FALSE;
    mygdi_peek_update_state(hWnd, &dirty, NULL);
    if (!dirty) return TRUE;
    mygdi_remove_pending_paint(hWnd);
    SendMessageA(hWnd, WM_PAINT, 0, 0);
    return TRUE;
}

static void mygdi_redraw_children(HWND hWnd, const RECT* rc, UINT flags)
{
    if (!(flags & RDW_ALLCHILDREN) || (flags & RDW_NOCHILDREN)) return;
    for (HWND child = GetWindow(hWnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (flags & RDW_INVALIDATE) InvalidateRect(child, rc, (flags & RDW_ERASE) != 0);
        if (flags & RDW_VALIDATE) ValidateRect(child, rc);
        if ((flags & (RDW_UPDATENOW|RDW_ERASENOW)) != 0) UpdateWindow(child);
    }
}

BOOL RedrawWindow(HWND hWnd, const RECT* lprcUpdate, HRGN hrgnUpdate, UINT flags)
{
    if (!mygdi_is_valid_window(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }

    if (flags == 0) flags = RDW_INVALIDATE;

    MyGdiRegionData updateRegion;
    MyGdiRegionData* pRegion = NULL;
    if (hrgnUpdate) {
        pthread_mutex_lock(&g_GdiLock);
        MyGdiRegionObj* rgn = mygdi_find_region_locked(hrgnUpdate);
        if (!rgn) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
        updateRegion = rgn->data;
        pthread_mutex_unlock(&g_GdiLock);
        pRegion = &updateRegion;
    }

    if (flags & RDW_VALIDATE) {
        if (pRegion) {
            if (!ValidateRgn(hWnd, hrgnUpdate)) return FALSE;
        } else {
            if (!ValidateRect(hWnd, lprcUpdate)) return FALSE;
        }
    }
    if (flags & RDW_NOERASE) {
        mygdi_set_erase_pending(hWnd, FALSE);
    }
    if (flags & (RDW_INVALIDATE|RDW_INTERNALPAINT)) {
        BOOL erase = (flags & RDW_ERASE) != 0 && (flags & RDW_NOERASE) == 0;
        if (pRegion) {
            if (!mygdi_mark_dirty_region(hWnd, pRegion, erase, (flags & RDW_INTERNALPAINT) != 0, 1)) return FALSE;
        } else {
            if (!mygdi_mark_dirty(hWnd, lprcUpdate, erase, (flags & RDW_INTERNALPAINT) != 0, 1)) return FALSE;
        }
    } else if ((flags & RDW_ERASE) && !(flags & RDW_NOERASE)) {
        BOOL dirty = FALSE;
        mygdi_peek_update_state(hWnd, &dirty, NULL);
        if (dirty) mygdi_set_erase_pending(hWnd, TRUE);
    }

    mygdi_redraw_children(hWnd, lprcUpdate, flags);

    if (flags & (RDW_UPDATENOW|RDW_ERASENOW)) {
        if (!UpdateWindow(hWnd)) return FALSE;
    }
    return TRUE;
}

HDC BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint)
{
    if (!hWnd || !lpPaint) return 0;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, 1);
    RECT rc = {0,0,4096,4096};
    BOOL erase = FALSE;
    if (ws && (ws->dirty || ws->internalPaint)) rc = ws->dirtyRect;
    if (ws) {
        erase = ws->erasePending ? TRUE : FALSE;
        ws->dirty = 0;
        ws->paintPending = 0;
        ws->erasePending = 0;
        ws->internalPaint = 0;
        mygdi_region_clear(&ws->updateRegion);
    }

    MyGdiDCObj* dc = mygdi_alloc_dc_locked();
    if (!dc) { pthread_mutex_unlock(&g_GdiLock); return 0; }
    dc->hwnd = hWnd;
    dc->paintDc = 1;
    dc->stretchMode = MYGDI_DEFAULT_STRETCH_MODE;
    dc->rcPaint = rc;
    dc->selectedBrush = 0;
    mygdi_register_dc_locked(dc);
    mygdi_clear_commands_locked(hWnd);
    memset(lpPaint, 0, sizeof(*lpPaint));
    lpPaint->hdc = dc->handle;
    lpPaint->fErase = erase;
    lpPaint->rcPaint = rc;
    pthread_mutex_unlock(&g_GdiLock);
    mygdi_remove_pending_paint(hWnd);

    if (erase) {
        /* v157: BeginPaint owns the erase-pending transition.  A custom
           WM_ERASEBKGND handler may return TRUE to suppress default erase;
           otherwise DefWindowProcA applies the class background brush. */
        LRESULT erased = SendMessageA(hWnd, WM_ERASEBKGND, (WPARAM)lpPaint->hdc, 0);
        if (!erased) (void)DefWindowProcA(hWnd, WM_ERASEBKGND, (WPARAM)lpPaint->hdc, 0);
    }
    return lpPaint->hdc;
}

BOOL EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint)
{
    (void)hWnd;
    if (!lpPaint || !lpPaint->hdc) return FALSE;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(lpPaint->hdc);
    if (dc) {
        mygdi_unselect_brush_locked(dc);
        mygdi_unselect_bitmap_locked(dc);
        mygdi_free_dc_locked(dc);
    }
    pthread_mutex_unlock(&g_GdiLock);
    return dc ? TRUE : FALSE;
}

HDC GetDC(HWND hWnd)
{
    if (!hWnd) return 0;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_alloc_dc_locked();
    if (!dc) { pthread_mutex_unlock(&g_GdiLock); return 0; }
    dc->hwnd = hWnd;
    dc->paintDc = 0;
    dc->stretchMode = MYGDI_DEFAULT_STRETCH_MODE;
    dc->rcPaint.left = 0; dc->rcPaint.top = 0; dc->rcPaint.right = 4096; dc->rcPaint.bottom = 4096;
    mygdi_register_dc_locked(dc);
    pthread_mutex_unlock(&g_GdiLock);
    return dc->handle;
}

int ReleaseDC(HWND hWnd, HDC hDC)
{
    (void)hWnd;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hDC);
    if (dc && dc->memoryDc) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (dc) {
        mygdi_unselect_brush_locked(dc);
        mygdi_unselect_bitmap_locked(dc);
        mygdi_free_dc_locked(dc);
    }
    pthread_mutex_unlock(&g_GdiLock);
    return dc ? 1 : 0;
}

HDC CreateCompatibleDC(HDC hdc)
{
    if (hdc) {
        pthread_mutex_lock(&g_GdiLock);
        MyGdiDCObj* src = mygdi_find_dc_locked(hdc);
        int valid = src != NULL;
        pthread_mutex_unlock(&g_GdiLock);
        if (!valid) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    }

    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_alloc_dc_locked();
    if (!dc) { pthread_mutex_unlock(&g_GdiLock); return 0; }
    MyGdiBitmapObj* def = mygdi_alloc_bitmap_locked(1, 1, 32, NULL, 1);
    if (!def) { mygdi_free_dc_locked(dc); pthread_mutex_unlock(&g_GdiLock); return 0; }
    def->selectedCount = 1;
    mygdi_update_bitmap_object_locked(def);
    dc->memoryDc = 1;
    dc->stretchMode = MYGDI_DEFAULT_STRETCH_MODE;
    dc->selectedBitmap = def->handle;
    dc->defaultBitmap = def->handle;
    dc->rcPaint.left = 0; dc->rcPaint.top = 0; dc->rcPaint.right = 1; dc->rcPaint.bottom = 1;
    mygdi_register_dc_locked(dc);
    HDC out = dc->handle;
    pthread_mutex_unlock(&g_GdiLock);
    return out;
}

BOOL DeleteDC(HDC hdc)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    if (!dc || !dc->memoryDc) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    HBITMAP defHandle = dc->defaultBitmap;
    mygdi_unselect_brush_locked(dc);
    mygdi_unselect_bitmap_locked(dc);
    mygdi_free_dc_locked(dc);
    MyGdiBitmapObj* def = mygdi_find_bitmap_locked(defHandle);
    if (def && def->internalDefault && def->selectedCount == 0) mygdi_free_bitmap_locked(def);
    pthread_mutex_unlock(&g_GdiLock);
    return TRUE;
}

static void mygdi_dc_visible_region_locked(MyGdiDCObj* dc, MyGdiRegionData* out)
{
    mygdi_region_clear(out);
    if (!dc || !out) return;
    if (dc->hasClip) { *out = dc->clip; return; }
    RECT rc = dc->rcPaint;
    if (dc->memoryDc) {
        MyGdiBitmapObj* bmp = mygdi_selected_bitmap_locked(dc);
        if (bmp) { rc.left = 0; rc.top = 0; rc.right = bmp->width; rc.bottom = bmp->height; }
    }
    mygdi_region_from_rect(out, &rc);
}

static int mygdi_dc_clip_rects_locked(MyGdiDCObj* dc, const RECT* in, RECT* outRects, int maxRects)
{
    if (!dc || !in || !outRects || maxRects <= 0) return 0;
    MyGdiRegionData base, input, clipped;
    mygdi_dc_visible_region_locked(dc, &base);
    mygdi_region_from_rect(&input, in);
    int kind = mygdi_region_combine_data(&clipped, &base, &input, RGN_AND);
    if (kind == ERROR || clipped.count <= 0) return 0;
    int n = clipped.count < maxRects ? clipped.count : maxRects;
    for (int i = 0; i < n; ++i) outRects[i] = clipped.rects[i];
    return n;
}

static int mygdi_valid_stretch_mode(int mode)
{
    return mode == BLACKONWHITE || mode == WHITEONBLACK || mode == COLORONCOLOR || mode == HALFTONE;
}

static int mygdi_abs_i(int v)
{
    return v < 0 ? -v : v;
}

static int mygdi_sgn_i(int v)
{
    return v < 0 ? -1 : 1;
}

static int mygdi_dc_allows_point_locked(MyGdiDCObj* dc, int x, int y)
{
    if (!dc) return 0;
    if (dc->hasClip) return mygdi_region_contains_point(&dc->clip, x, y);
    if (dc->memoryDc) {
        MyGdiBitmapObj* bmp = mygdi_selected_bitmap_locked(dc);
        return bmp && x >= 0 && y >= 0 && x < bmp->width && y < bmp->height;
    }
    return 1;
}

static int mygdi_map_stretch_axis(int dstIndex, int dstLen, int srcBase, int srcLen, int mirror)
{
    if (dstLen <= 0 || srcLen <= 0) return srcBase;
    int rel = (dstIndex * srcLen) / dstLen;
    if (rel < 0) rel = 0;
    if (rel >= srcLen) rel = srcLen - 1;
    return mirror ? (srcBase + srcLen - 1 - rel) : (srcBase + rel);
}

HRGN CreateRectRgn(int x1, int y1, int x2, int y2)
{
    RECT rc = { x1, y1, x2, y2 };
    MyGdiRegionData d;
    mygdi_region_from_rect(&d, &rc);
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* r = mygdi_alloc_region_locked(&d);
    HRGN h = r ? r->handle : 0;
    pthread_mutex_unlock(&g_GdiLock);
    return h;
}

HRGN CreateRectRgnIndirect(const RECT* lprc)
{
    if (!lprc) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return CreateRectRgn((int)lprc->left, (int)lprc->top, (int)lprc->right, (int)lprc->bottom);
}

BOOL SetRectRgn(HRGN hrgn, int left, int top, int right, int bottom)
{
    RECT rc = { left, top, right, bottom };
    MyGdiRegionData d;
    mygdi_region_from_rect(&d, &rc);
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* r = mygdi_find_region_locked(hrgn);
    if (!r) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    r->data = d;
    mygdi_update_region_object_locked(r);
    pthread_mutex_unlock(&g_GdiLock);
    return TRUE;
}

int OffsetRgn(HRGN hrgn, int x, int y)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* r = mygdi_find_region_locked(hrgn);
    if (!r) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return ERROR; }
    for (int i = 0; i < r->data.count; ++i) {
        r->data.rects[i].left += x;
        r->data.rects[i].right += x;
        r->data.rects[i].top += y;
        r->data.rects[i].bottom += y;
    }
    int kind = mygdi_region_type(&r->data);
    mygdi_update_region_object_locked(r);
    pthread_mutex_unlock(&g_GdiLock);
    return kind;
}

int GetRgnBox(HRGN hrgn, LPRECT lprc)
{
    if (!lprc) { SetLastError(ERROR_INVALID_PARAMETER); return ERROR; }
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* r = mygdi_find_region_locked(hrgn);
    if (!r) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return ERROR; }
    int kind = mygdi_region_bounds(&r->data, lprc);
    pthread_mutex_unlock(&g_GdiLock);
    return kind;
}

int CombineRgn(HRGN hrgnDst, HRGN hrgnSrc1, HRGN hrgnSrc2, int iMode)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* dst = mygdi_find_region_locked(hrgnDst);
    MyGdiRegionObj* src1 = mygdi_find_region_locked(hrgnSrc1);
    MyGdiRegionObj* src2 = (iMode == RGN_COPY) ? NULL : mygdi_find_region_locked(hrgnSrc2);
    if (!dst || !src1 || (iMode != RGN_COPY && !src2)) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_HANDLE);
        return ERROR;
    }
    MyGdiRegionData a = src1->data;
    MyGdiRegionData b;
    if (src2) b = src2->data;
    MyGdiRegionData out;
    int kind = mygdi_region_combine_data(&out, &a, src2 ? &b : NULL, iMode);
    if (kind != ERROR) {
        dst->data = out;
        mygdi_update_region_object_locked(dst);
    } else {
        SetLastError(ERROR_INVALID_PARAMETER);
    }
    pthread_mutex_unlock(&g_GdiLock);
    return kind;
}

BOOL EqualRgn(HRGN hrgn1, HRGN hrgn2)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* a = mygdi_find_region_locked(hrgn1);
    MyGdiRegionObj* b = mygdi_find_region_locked(hrgn2);
    if (!a || !b) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    MyGdiRegionData da = a->data, db = b->data;
    mygdi_region_canonicalize(&da);
    mygdi_region_canonicalize(&db);
    BOOL eq = (da.count == db.count) ? TRUE : FALSE;
    for (int i = 0; eq && i < da.count; ++i) if (!mygdi_rects_equal(&da.rects[i], &db.rects[i])) eq = FALSE;
    pthread_mutex_unlock(&g_GdiLock);
    return eq;
}

BOOL PtInRegion(HRGN hrgn, int x, int y)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* r = mygdi_find_region_locked(hrgn);
    if (!r) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    BOOL hit = mygdi_region_contains_point(&r->data, x, y) ? TRUE : FALSE;
    pthread_mutex_unlock(&g_GdiLock);
    return hit;
}

BOOL RectInRegion(HRGN hrgn, const RECT* lprc)
{
    if (!lprc) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_GdiLock);
    MyGdiRegionObj* r = mygdi_find_region_locked(hrgn);
    if (!r) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    BOOL hit = mygdi_region_rect_intersects(&r->data, lprc) ? TRUE : FALSE;
    pthread_mutex_unlock(&g_GdiLock);
    return hit;
}

int SelectClipRgn(HDC hdc, HRGN hrgn)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    if (!dc) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return ERROR; }
    if (!hrgn) {
        dc->hasClip = 0;
        mygdi_region_clear(&dc->clip);
        MyGdiRegionData vis;
        mygdi_dc_visible_region_locked(dc, &vis);
        int kind = mygdi_region_type(&vis);
        pthread_mutex_unlock(&g_GdiLock);
        return kind;
    }
    MyGdiRegionObj* r = mygdi_find_region_locked(hrgn);
    if (!r) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return ERROR; }
    dc->clip = r->data;
    dc->hasClip = 1;
    int kind = mygdi_region_type(&dc->clip);
    pthread_mutex_unlock(&g_GdiLock);
    return kind;
}

int GetClipBox(HDC hdc, LPRECT lprc)
{
    if (!lprc) { SetLastError(ERROR_INVALID_PARAMETER); return ERROR; }
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    if (!dc) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return ERROR; }
    MyGdiRegionData vis;
    mygdi_dc_visible_region_locked(dc, &vis);
    int kind = mygdi_region_bounds(&vis, lprc);
    pthread_mutex_unlock(&g_GdiLock);
    return kind;
}

int IntersectClipRect(HDC hdc, int left, int top, int right, int bottom)
{
    RECT rc = { left, top, right, bottom };
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    if (!dc) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return ERROR; }
    MyGdiRegionData cur, rectRgn, out;
    mygdi_dc_visible_region_locked(dc, &cur);
    mygdi_region_from_rect(&rectRgn, &rc);
    int kind = mygdi_region_combine_data(&out, &cur, &rectRgn, RGN_AND);
    if (kind == ERROR) { pthread_mutex_unlock(&g_GdiLock); return ERROR; }
    dc->clip = out;
    dc->hasClip = 1;
    pthread_mutex_unlock(&g_GdiLock);
    return kind;
}

int ExcludeClipRect(HDC hdc, int left, int top, int right, int bottom)
{
    RECT rc = { left, top, right, bottom };
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    if (!dc) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return ERROR; }
    MyGdiRegionData cur, rectRgn, out;
    mygdi_dc_visible_region_locked(dc, &cur);
    mygdi_region_from_rect(&rectRgn, &rc);
    int kind = mygdi_region_combine_data(&out, &cur, &rectRgn, RGN_DIFF);
    if (kind == ERROR) { pthread_mutex_unlock(&g_GdiLock); return ERROR; }
    dc->clip = out;
    dc->hasClip = 1;
    pthread_mutex_unlock(&g_GdiLock);
    return kind;
}

HBRUSH CreateSolidBrush(COLORREF color)
{
    pthread_mutex_lock(&g_GdiLock);
    int i = mygdi_pop_free_locked(g_GdiBrushFree, &g_GdiBrushFreeTop);
    if (i < 0) { pthread_mutex_unlock(&g_GdiLock); return 0; }
    memset(&g_GdiBrushes[i], 0, sizeof(g_GdiBrushes[i]));
    g_GdiBrushes[i].valid = 1;
    g_GdiBrushes[i].handle = _ObjectMakeSlotHandle(_OBJECT_TYPE_BRUSH, (DWORD)i);
    if (!g_GdiBrushes[i].handle) g_GdiBrushes[i].handle = g_NextBrushHandle++;
    g_GdiBrushes[i].color = color;
    g_GdiBrushes[i].ownerPid = GetCurrentProcessId();
    g_GdiBrushes[i].selectedCount = 0;
    HBRUSH h = g_GdiBrushes[i].handle;
    char name[96];
    snprintf(name, sizeof(name), "GDI\\BRUSH rgb=%02x%02x%02x", (unsigned)((color>>16)&0xff), (unsigned)((color>>8)&0xff), (unsigned)(color&0xff));
    mygdi_hash_insert_brush_locked(i);
    _ObjectRegister(h, _OBJECT_TYPE_BRUSH, g_GdiBrushes[i].ownerPid, _OBJECT_ACCESS_READ|_OBJECT_ACCESS_WRITE, color, name);
    pthread_mutex_unlock(&g_GdiLock);
    return h;
}

HBITMAP CreateCompatibleBitmap(HDC hdc, int cx, int cy)
{
    if (cx <= 0 || cy <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (hdc) {
        pthread_mutex_lock(&g_GdiLock);
        int valid = mygdi_find_dc_locked(hdc) != NULL;
        pthread_mutex_unlock(&g_GdiLock);
        if (!valid) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    }
    pthread_mutex_lock(&g_GdiLock);
    MyGdiBitmapObj* bmp = mygdi_alloc_bitmap_locked(cx, cy, 32, NULL, 0);
    HBITMAP h = bmp ? bmp->handle : 0;
    pthread_mutex_unlock(&g_GdiLock);
    return h;
}

HBITMAP CreateBitmap(int nWidth, int nHeight, UINT nPlanes, UINT nBitCount, const void* lpBits)
{
    if (nWidth <= 0 || nHeight <= 0 || nPlanes == 0 || nBitCount == 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    pthread_mutex_lock(&g_GdiLock);
    UINT bpp = nPlanes * nBitCount;
    MyGdiBitmapObj* bmp = mygdi_alloc_bitmap_locked(nWidth, nHeight, (int)bpp, lpBits, 0);
    HBITMAP h = bmp ? bmp->handle : 0;
    pthread_mutex_unlock(&g_GdiLock);
    return h;
}

HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO* pbmi, UINT iUsage, void** ppvBits, HANDLE hSection, DWORD dwOffset)
{
    if (!pbmi || !ppvBits) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    *ppvBits = NULL;

    if (hdc) {
        pthread_mutex_lock(&g_GdiLock);
        int validDc = mygdi_find_dc_locked(hdc) != NULL;
        pthread_mutex_unlock(&g_GdiLock);
        if (!validDc) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    }

    const BITMAPINFOHEADER* bih = &pbmi->bmiHeader;
    if (bih->biSize != sizeof(BITMAPINFOHEADER) || bih->biWidth <= 0 || bih->biHeight == 0 ||
        bih->biPlanes != 1 || bih->biBitCount != 32 || bih->biCompression != BI_RGB ||
        iUsage != DIB_RGB_COLORS || hSection != 0 || dwOffset != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    LONG signedHeight = bih->biHeight;
    int topDown = signedHeight < 0;
    int height = topDown ? (int)(-signedHeight) : (int)signedHeight;
    int width = (int)bih->biWidth;
    if (height <= 0 || width <= 0 || width > 8192 || height > 8192) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    pthread_mutex_lock(&g_GdiLock);
    MyGdiBitmapObj* bmp = mygdi_alloc_bitmap_locked(width, height, 32, NULL, 0);
    if (!bmp) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    bmp->dibSection = 1;
    bmp->dibTopDown = topDown ? 1 : 0;
    bmp->dibSectionHandle = 0;
    bmp->dibOffset = 0;
    bmp->dibHeader = *bih;
    bmp->dibHeader.biSizeImage = (DWORD)(bmp->widthBytes * bmp->height);
    *ppvBits = bmp->pixels;
    mygdi_update_bitmap_object_locked(bmp);
    char name[96];
    snprintf(name, sizeof(name), "GDI\\DIBSECTION %dx%d 32bpp %s", width, height, topDown ? "top-down" : "bottom-up");
    _ObjectSetInfo(bmp->handle, _OBJECT_FLAG_GDI_DIBSECTION, (DWORD)(bmp->widthBytes * bmp->height), name);
    HBITMAP h = bmp->handle;
    pthread_mutex_unlock(&g_GdiLock);
    return h;
}

BOOL DeleteObject(HGDIOBJ hObject)
{
    pthread_mutex_lock(&g_GdiLock);

    MyGdiBrushObj* br = mygdi_find_brush_locked((HBRUSH)hObject);
    if (br) {
        if (br->selectedCount) { pthread_mutex_unlock(&g_GdiLock); return FALSE; }
        int idx = (int)(br - g_GdiBrushes);
        mygdi_hash_remove_brush_locked(idx);
        _ObjectUnregister(br->handle);
        memset(br, 0, sizeof(*br));
        mygdi_push_free_locked(g_GdiBrushFree, &g_GdiBrushFreeTop, MYGDI_MAX_BRUSHES, idx);
        pthread_mutex_unlock(&g_GdiLock);
        return TRUE;
    }

    MyGdiBitmapObj* bmp = mygdi_find_bitmap_locked((HBITMAP)hObject);
    if (bmp) {
        if (bmp->selectedCount || bmp->internalDefault) { pthread_mutex_unlock(&g_GdiLock); return FALSE; }
        mygdi_free_bitmap_locked(bmp);
        pthread_mutex_unlock(&g_GdiLock);
        return TRUE;
    }

    MyGdiRegionObj* r = mygdi_find_region_locked((HRGN)hObject);
    if (r) {
        mygdi_free_region_locked(r);
        pthread_mutex_unlock(&g_GdiLock);
        return TRUE;
    }

    pthread_mutex_unlock(&g_GdiLock);
    return FALSE;
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ hObject)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    HGDIOBJ old = 0;
    MyGdiBrushObj* brush = mygdi_find_brush_locked((HBRUSH)hObject);
    MyGdiBitmapObj* bmp = mygdi_find_bitmap_locked((HBITMAP)hObject);
    if (dc && brush) {
        old = dc->selectedBrush;
        mygdi_unselect_brush_locked(dc);
        dc->selectedBrush = (HBRUSH)hObject;
        brush->selectedCount++;
        mygdi_update_brush_object_locked(brush);
    } else if (dc && dc->memoryDc && bmp) {
        if (bmp->selectedCount && dc->selectedBitmap != bmp->handle) {
            old = 0;
        } else {
            old = dc->selectedBitmap;
            mygdi_unselect_bitmap_locked(dc);
            dc->selectedBitmap = bmp->handle;
            bmp->selectedCount++;
            mygdi_update_bitmap_object_locked(bmp);
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    return old;
}

BOOL PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop)
{
    if (!mygdi_valid_patblt_rop(rop) || w <= 0 || h <= 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RECT rc = { x, y, x + w, y + h };
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    if (!dc) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    COLORREF brushColor = RGB(0,0,0);
    if (mygdi_patblt_rop_needs_brush(rop)) {
        MyGdiBrushObj* br = mygdi_find_brush_locked(dc->selectedBrush);
        if (!br) {
            pthread_mutex_unlock(&g_GdiLock);
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        brushColor = br->color;
    }

    RECT parts[MYGDI_REGION_MAX_RECTS];
    int n = mygdi_dc_clip_rects_locked(dc, &rc, parts, MYGDI_REGION_MAX_RECTS);
    int ok = 1;
    if (dc->memoryDc) {
        MyGdiBitmapObj* bmp = mygdi_selected_bitmap_locked(dc);
        if (!bmp || !bmp->pixels) {
            pthread_mutex_unlock(&g_GdiLock);
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        for (int pi = 0; pi < n; ++pi) {
            RECT p = parts[pi];
            if (p.left < 0) p.left = 0;
            if (p.top < 0) p.top = 0;
            if (p.right > bmp->width) p.right = bmp->width;
            if (p.bottom > bmp->height) p.bottom = bmp->height;
            for (int yy = (int)p.top; yy < (int)p.bottom; ++yy) {
                for (int xx = (int)p.left; xx < (int)p.right; ++xx) {
                    COLORREF dst = mygdi_get_pixel_bitmap(bmp, xx, yy);
                    mygdi_set_pixel_bitmap(bmp, xx, yy, mygdi_apply_patblt_rop(rop, dst, brushColor));
                }
            }
        }
    } else {
        for (int pi = 0; pi < n; ++pi) {
            if (!mygdi_append_patblt_locked(dc->hwnd, &parts[pi], brushColor, rop)) ok = 0;
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    if (!ok) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return ok ? TRUE : FALSE;
}

int FillRect(HDC hDC, const RECT* lprc, HBRUSH hbr)
{
    if (!lprc) return 0;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hDC);
    MyGdiBrushObj* br = mygdi_find_brush_locked(hbr);
    int ok = 0;
    if (dc && br) {
        RECT parts[MYGDI_REGION_MAX_RECTS];
        int n = mygdi_dc_clip_rects_locked(dc, lprc, parts, MYGDI_REGION_MAX_RECTS);
        ok = 1;
        if (n <= 0) ok = 1;
        for (int i = 0; i < n; ++i) {
            int one = dc->memoryDc ? mygdi_fill_bitmap(mygdi_selected_bitmap_locked(dc), &parts[i], br->color)
                                   : mygdi_append_locked(dc->hwnd, MYGDI_CMD_FILL, &parts[i], br->color, NULL, 0);
            if (!one) ok = 0;
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    return ok;
}

BOOL Rectangle(HDC hDC, int left, int top, int right, int bottom)
{
    RECT rc = { left, top, right, bottom };
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hDC);
    COLORREF color = RGB(120,160,255);
    if (dc && dc->selectedBrush) {
        MyGdiBrushObj* br = mygdi_find_brush_locked(dc->selectedBrush);
        if (br) color = br->color;
    }
    int ok = 0;
    if (dc) {
        if (dc->memoryDc) ok = mygdi_rect_bitmap(mygdi_selected_bitmap_locked(dc), &rc, color);
        else ok = mygdi_append_locked(dc->hwnd, MYGDI_CMD_RECT, &rc, color, NULL, 0);
    }
    pthread_mutex_unlock(&g_GdiLock);
    return ok ? TRUE : FALSE;
}

BOOL TextOutA(HDC hDC, int x, int y, LPCSTR lpString, int c)
{
    if (!lpString) return FALSE;
    RECT rc = { x, y, x, y };
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hDC);
    int ok = 0;
    if (dc) {
        if (dc->memoryDc) {
            /* v156 bitmap DC has no font engine yet; mark a tiny white pixel canary. */
            MyGdiBitmapObj* bmp = mygdi_selected_bitmap_locked(dc);
            mygdi_set_pixel_bitmap(bmp, x, y, RGB(255,255,255));
            ok = bmp != NULL;
        } else {
            ok = mygdi_append_locked(dc->hwnd, MYGDI_CMD_TEXT, &rc, RGB(255,255,255), lpString, c);
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    return ok ? TRUE : FALSE;
}

int DrawTextA(HDC hDC, LPCSTR lpchText, int cchText, LPRECT lprc, UINT format)
{
    (void)format;
    if (!lprc || !lpchText) return 0;
    return TextOutA(hDC, lprc->left, lprc->top, lpchText, cchText) ? 1 : 0;
}

int GetObjectA(HGDIOBJ hgdiobj, int cbBuffer, LPVOID lpvObject)
{
    if (!lpvObject || cbBuffer <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    pthread_mutex_lock(&g_GdiLock);
    MyGdiBitmapObj* bmp = mygdi_find_bitmap_locked((HBITMAP)hgdiobj);
    if (bmp) {
        BITMAP bm;
        mygdi_fill_bitmap_struct(bmp, &bm);

        if (bmp->dibSection && cbBuffer >= (int)sizeof(DIBSECTION)) {
            DIBSECTION ds;
            memset(&ds, 0, sizeof(ds));
            ds.dsBm = bm;
            ds.dsBmih = bmp->dibHeader;
            ds.dshSection = bmp->dibSectionHandle;
            ds.dsOffset = bmp->dibOffset;
            memcpy(lpvObject, &ds, sizeof(ds));
            pthread_mutex_unlock(&g_GdiLock);
            return (int)sizeof(ds);
        }

        int n = cbBuffer < (int)sizeof(bm) ? cbBuffer : (int)sizeof(bm);
        memcpy(lpvObject, &bm, (size_t)n);
        pthread_mutex_unlock(&g_GdiLock);
        return n;
    }
    pthread_mutex_unlock(&g_GdiLock);
    SetLastError(ERROR_INVALID_HANDLE);
    return 0;
}


int GetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT cLines, void* lpvBits, BITMAPINFO* lpbmi, UINT usage)
{
    if (!lpbmi) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (usage != DIB_RGB_COLORS) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }

    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    MyGdiBitmapObj* bmp = mygdi_find_bitmap_locked(hbm);
    if (!dc || !bmp || !bmp->valid) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(!dc ? ERROR_INVALID_HANDLE : ERROR_INVALID_HANDLE);
        return 0;
    }
    if (bmp->selectedCount) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    BITMAPINFOHEADER* bih = &lpbmi->bmiHeader;
    if (bih->biSize != sizeof(BITMAPINFOHEADER)) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!lpvBits) {
        memset(bih, 0, sizeof(*bih));
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = bmp->width;
        bih->biHeight = bmp->dibSection ? bmp->dibHeader.biHeight : bmp->height;
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = (DWORD)(bmp->widthBytes * bmp->height);
        pthread_mutex_unlock(&g_GdiLock);
        return 1;
    }

    int outWidth = 0, outHeight = 0, outTopDown = 0;
    if (!mygdi_validate_dib32_bmi(lpbmi, usage, 1, &outWidth, &outHeight, &outTopDown) ||
        outWidth != bmp->width || outHeight != bmp->height) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (start >= (UINT)bmp->height || cLines == 0) { pthread_mutex_unlock(&g_GdiLock); return 0; }
    UINT avail = (UINT)bmp->height - start;
    UINT lines = cLines < avail ? cLines : avail;
    BYTE* outBits = (BYTE*)lpvBits;
    int stride = mygdi_dib_stride_32(outWidth);
    for (UINT line = 0; line < lines; ++line) {
        int physicalOutLine = (int)(start + line);
        int logicalY = outTopDown ? physicalOutLine : (bmp->height - 1 - physicalOutLine);
        BYTE* row = outBits + (size_t)line * (size_t)stride;
        for (int x = 0; x < bmp->width; ++x) {
            COLORREF c = mygdi_get_pixel_bitmap(bmp, x, logicalY);
            mygdi_dib32_write_color(row, x, c);
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    return (int)lines;
}

int SetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT cLines, const void* lpBits, const BITMAPINFO* lpbmi, UINT ColorUse)
{
    if (!lpBits || !lpbmi) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (ColorUse != DIB_RGB_COLORS) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    int inWidth = 0, inHeight = 0, inTopDown = 0;
    if (!mygdi_validate_dib32_bmi(lpbmi, ColorUse, 1, &inWidth, &inHeight, &inTopDown)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    MyGdiBitmapObj* bmp = mygdi_find_bitmap_locked(hbm);
    if (!dc || !bmp || !bmp->valid) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (bmp->selectedCount || inWidth != bmp->width || inHeight != bmp->height) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (start >= (UINT)bmp->height || cLines == 0) { pthread_mutex_unlock(&g_GdiLock); return 0; }
    UINT avail = (UINT)bmp->height - start;
    UINT lines = cLines < avail ? cLines : avail;
    const BYTE* inBits = (const BYTE*)lpBits;
    int stride = mygdi_dib_stride_32(inWidth);
    for (UINT line = 0; line < lines; ++line) {
        int physicalInLine = (int)(start + line);
        int logicalY = inTopDown ? physicalInLine : (bmp->height - 1 - physicalInLine);
        const BYTE* row = inBits + (size_t)line * (size_t)stride;
        for (int x = 0; x < bmp->width; ++x) {
            COLORREF c = mygdi_dib32_read_color(row, x);
            mygdi_set_pixel_bitmap(bmp, x, logicalY, c);
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    return (int)lines;
}

static int mygdi_stretch_dibbits_locked(MyGdiDCObj* dst, int xDest, int yDest, int DestWidth, int DestHeight,
                                        int xSrc, int ySrc, int SrcWidth, int SrcHeight,
                                        const BYTE* bits, int dibWidth, int dibHeight, int dibTopDown)
{
    if (!dst || !bits || DestWidth <= 0 || DestHeight <= 0 || SrcWidth <= 0 || SrcHeight <= 0) return 0;
    if (dst->memoryDc) {
        MyGdiBitmapObj* db = mygdi_selected_bitmap_locked(dst);
        if (!db || !db->pixels) return 0;
        for (int dy = 0; dy < DestHeight; ++dy) {
            int ty = yDest + dy;
            if (ty < 0 || ty >= db->height) continue;
            int sy = ySrc + (dy * SrcHeight) / DestHeight;
            for (int dx = 0; dx < DestWidth; ++dx) {
                int tx = xDest + dx;
                if (tx < 0 || tx >= db->width) continue;
                int sx = xSrc + (dx * SrcWidth) / DestWidth;
                COLORREF c = mygdi_dib32_get_logical_pixel(bits, dibWidth, dibHeight, dibTopDown, sx, sy);
                if (c != CLR_INVALID) mygdi_set_pixel_bitmap(db, tx, ty, c);
            }
        }
        return 1;
    }

    COLORREF* snap = (COLORREF*)malloc((size_t)DestWidth * (size_t)DestHeight * sizeof(COLORREF));
    if (!snap) return 0;
    for (int dy = 0; dy < DestHeight; ++dy) {
        int sy = ySrc + (dy * SrcHeight) / DestHeight;
        for (int dx = 0; dx < DestWidth; ++dx) {
            int sx = xSrc + (dx * SrcWidth) / DestWidth;
            snap[dy * DestWidth + dx] = mygdi_dib32_get_logical_pixel(bits, dibWidth, dibHeight, dibTopDown, sx, sy);
        }
    }
    RECT dstRc = { xDest, yDest, xDest + DestWidth, yDest + DestHeight };
    int ok = mygdi_append_blit_locked(dst->hwnd, &dstRc, 0, 0, DestWidth, DestHeight, snap);
    free(snap);
    return ok;
}

int GetStretchBltMode(HDC hdc)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    int mode = dc ? dc->stretchMode : 0;
    pthread_mutex_unlock(&g_GdiLock);
    if (!mode) SetLastError(ERROR_INVALID_HANDLE);
    return mode;
}

int SetStretchBltMode(HDC hdc, int mode)
{
    if (!mygdi_valid_stretch_mode(mode)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    if (!dc) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return 0; }
    int old = dc->stretchMode ? dc->stretchMode : MYGDI_DEFAULT_STRETCH_MODE;
    dc->stretchMode = mode;
    pthread_mutex_unlock(&g_GdiLock);
    return old;
}

BOOL StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
                HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop)
{
    if (rop != SRCCOPY || wDest == 0 || hDest == 0 || wSrc == 0 || hSrc == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    int dstW = mygdi_abs_i(wDest);
    int dstH = mygdi_abs_i(hDest);
    int srcW = mygdi_abs_i(wSrc);
    int srcH = mygdi_abs_i(hSrc);
    if (dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    int dstLeft = wDest < 0 ? xDest + wDest : xDest;
    int dstTop  = hDest < 0 ? yDest + hDest : yDest;
    int srcLeft = wSrc < 0 ? xSrc + wSrc : xSrc;
    int srcTop  = hSrc < 0 ? ySrc + hSrc : ySrc;
    int mirrorX = mygdi_sgn_i(wDest) != mygdi_sgn_i(wSrc);
    int mirrorY = mygdi_sgn_i(hDest) != mygdi_sgn_i(hSrc);

    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dst = mygdi_find_dc_locked(hdcDest);
    MyGdiDCObj* src = mygdi_find_dc_locked(hdcSrc);
    MyGdiBitmapObj* sb = src ? mygdi_selected_bitmap_locked(src) : NULL;
    if (!dst || !src || !src->memoryDc || !sb || !sb->pixels) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (srcLeft < 0 || srcTop < 0 || srcLeft + srcW > sb->width || srcTop + srcH > sb->height) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (dst->memoryDc) {
        MyGdiBitmapObj* db = mygdi_selected_bitmap_locked(dst);
        if (!db || !db->pixels) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
        for (int dy = 0; dy < dstH; ++dy) {
            int ty = dstTop + dy;
            if (ty < 0 || ty >= db->height) continue;
            int sy = mygdi_map_stretch_axis(dy, dstH, srcTop, srcH, mirrorY);
            for (int dx = 0; dx < dstW; ++dx) {
                int tx = dstLeft + dx;
                if (tx < 0 || tx >= db->width) continue;
                if (!mygdi_dc_allows_point_locked(dst, tx, ty)) continue;
                int sx = mygdi_map_stretch_axis(dx, dstW, srcLeft, srcW, mirrorX);
                mygdi_set_pixel_bitmap(db, tx, ty, mygdi_get_pixel_bitmap(sb, sx, sy));
            }
        }
        pthread_mutex_unlock(&g_GdiLock);
        return TRUE;
    }

    COLORREF* snap = (COLORREF*)malloc((size_t)dstW * (size_t)dstH * sizeof(COLORREF));
    if (!snap) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (int dy = 0; dy < dstH; ++dy) {
        int sy = mygdi_map_stretch_axis(dy, dstH, srcTop, srcH, mirrorY);
        for (int dx = 0; dx < dstW; ++dx) {
            int sx = mygdi_map_stretch_axis(dx, dstW, srcLeft, srcW, mirrorX);
            snap[dy * dstW + dx] = mygdi_get_pixel_bitmap(sb, sx, sy);
        }
    }
    RECT dstRc = { dstLeft, dstTop, dstLeft + dstW, dstTop + dstH };
    int ok = mygdi_append_blit_locked(dst->hwnd, &dstRc, 0, 0, dstW, dstH, snap);
    free(snap);
    pthread_mutex_unlock(&g_GdiLock);
    if (!ok) SetLastError(ERROR_INVALID_HANDLE);
    return ok ? TRUE : FALSE;
}

int StretchDIBits(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight,
                  int xSrc, int ySrc, int SrcWidth, int SrcHeight,
                  const void* lpBits, const BITMAPINFO* lpbmi, UINT iUsage, DWORD rop)
{
    if (rop != SRCCOPY || !lpBits || !lpbmi || DestWidth <= 0 || DestHeight <= 0 || SrcWidth <= 0 || SrcHeight <= 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (int)GDI_ERROR;
    }
    int dibWidth = 0, dibHeight = 0, dibTopDown = 0;
    if (!mygdi_validate_dib32_bmi(lpbmi, iUsage, 1, &dibWidth, &dibHeight, &dibTopDown)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (int)GDI_ERROR;
    }
    if (xSrc < 0 || ySrc < 0 || xSrc + SrcWidth > dibWidth || ySrc + SrcHeight > dibHeight) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (int)GDI_ERROR;
    }

    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    if (!dc) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return (int)GDI_ERROR; }
    int ok = mygdi_stretch_dibbits_locked(dc, xDest, yDest, DestWidth, DestHeight,
                                          xSrc, ySrc, SrcWidth, SrcHeight,
                                          (const BYTE*)lpBits, dibWidth, dibHeight, dibTopDown);
    pthread_mutex_unlock(&g_GdiLock);
    if (!ok) { SetLastError(ERROR_INVALID_HANDLE); return (int)GDI_ERROR; }
    return SrcHeight;
}

int SetDIBitsToDevice(HDC hdc, int xDest, int yDest, DWORD dwWidth, DWORD dwHeight,
                      int xSrc, int ySrc, UINT uStartScan, UINT cScanLines,
                      const void* lpvBits, const BITMAPINFO* lpbmi, UINT fuColorUse)
{
    if (dwWidth > 0x7fffffffu || dwHeight > 0x7fffffffu || cScanLines == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (int)GDI_ERROR;
    }
    (void)uStartScan;
    return StretchDIBits(hdc, xDest, yDest, (int)dwWidth, (int)dwHeight,
                         xSrc, ySrc, (int)dwWidth, (int)cScanLines,
                         lpvBits, lpbmi, fuColorUse, SRCCOPY);
}


COLORREF SetPixel(HDC hdc, int x, int y, COLORREF color)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    COLORREF out = CLR_INVALID;
    if (dc && dc->memoryDc) {
        MyGdiBitmapObj* bmp = mygdi_selected_bitmap_locked(dc);
        if (bmp && x >= 0 && y >= 0 && x < bmp->width && y < bmp->height) {
            mygdi_set_pixel_bitmap(bmp, x, y, color);
            out = mygdi_canonical_color(color);
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    return out;
}

COLORREF GetPixel(HDC hdc, int x, int y)
{
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dc = mygdi_find_dc_locked(hdc);
    COLORREF out = CLR_INVALID;
    if (dc && dc->memoryDc) out = mygdi_get_pixel_bitmap(mygdi_selected_bitmap_locked(dc), x, y);
    pthread_mutex_unlock(&g_GdiLock);
    return out;
}

BOOL BitBlt(HDC hdcDest, int xDest, int yDest, int w, int h, HDC hdcSrc, int xSrc, int ySrc, DWORD rop)
{
    if (rop != SRCCOPY || w <= 0 || h <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_GdiLock);
    MyGdiDCObj* dst = mygdi_find_dc_locked(hdcDest);
    MyGdiDCObj* src = mygdi_find_dc_locked(hdcSrc);
    MyGdiBitmapObj* sb = src ? mygdi_selected_bitmap_locked(src) : NULL;
    if (!dst || !src || !src->memoryDc || !sb || !sb->pixels) {
        pthread_mutex_unlock(&g_GdiLock);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    int copyW = w;
    int copyH = h;
    if (xSrc < 0) { xDest -= xSrc; copyW += xSrc; xSrc = 0; }
    if (ySrc < 0) { yDest -= ySrc; copyH += ySrc; ySrc = 0; }
    if (xSrc + copyW > sb->width) copyW = sb->width - xSrc;
    if (ySrc + copyH > sb->height) copyH = sb->height - ySrc;
    if (copyW <= 0 || copyH <= 0) { pthread_mutex_unlock(&g_GdiLock); return TRUE; }

    if (dst->memoryDc) {
        MyGdiBitmapObj* db = mygdi_selected_bitmap_locked(dst);
        if (!db || !db->pixels) { pthread_mutex_unlock(&g_GdiLock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
        for (int yy = 0; yy < copyH; yy++) {
            int sy = ySrc + yy;
            int dy = yDest + yy;
            if (dy < 0 || dy >= db->height) continue;
            for (int xx = 0; xx < copyW; xx++) {
                int sx = xSrc + xx;
                int dx = xDest + xx;
                if (dx < 0 || dx >= db->width) continue;
                db->pixels[mygdi_bitmap_index(db, dx, dy)] = sb->pixels[mygdi_bitmap_index(sb, sx, sy)];
            }
        }
        pthread_mutex_unlock(&g_GdiLock);
        return TRUE;
    }

    RECT dstRc = { xDest, yDest, xDest + copyW, yDest + copyH };
    COLORREF* snap = (COLORREF*)malloc((size_t)copyW * (size_t)copyH * sizeof(COLORREF));
    if (!snap) { pthread_mutex_unlock(&g_GdiLock); return FALSE; }
    for (int yy = 0; yy < copyH; yy++)
        for (int xx = 0; xx < copyW; xx++)
            snap[yy * copyW + xx] = sb->pixels[mygdi_bitmap_index(sb, xSrc + xx, ySrc + yy)];
    int ok = mygdi_append_blit_locked(dst->hwnd, &dstRc, 0, 0, copyW, copyH, snap);
    free(snap);
    pthread_mutex_unlock(&g_GdiLock);
    return ok ? TRUE : FALSE;
}



static int mygdi_intersect_rect_local(RECT* out, const RECT* a, const RECT* b)
{
    if (!out || !a || !b) return 0;
    out->left = a->left > b->left ? a->left : b->left;
    out->top = a->top > b->top ? a->top : b->top;
    out->right = a->right < b->right ? a->right : b->right;
    out->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (out->right <= out->left || out->bottom <= out->top) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return 1;
}

BOOL MyGdiScrollWindowContent(HWND hWnd, int dx, int dy, const RECT* prcScroll, const RECT* prcClip)
{
    if (!hWnd) return FALSE;
    if (dx == 0 && dy == 0) return TRUE;

    RECT scroll = prcScroll ? *prcScroll : mygdi_default_update_rect();
    RECT clip = prcClip ? *prcClip : scroll;
    RECT effective;
    if (!mygdi_intersect_rect_local(&effective, &scroll, &clip)) return TRUE;

    pthread_mutex_lock(&g_GdiLock);
    int cmdIdx[MYGDI_MAX_COMMANDS];
    int cmdCount = mygdi_collect_command_indices_locked(hWnd, cmdIdx, MYGDI_MAX_COMMANDS);
    for (int ci = 0; ci < cmdCount; ++ci) {
        MyGdiCommand* c = &g_GdiCommands[cmdIdx[ci]];

        /* v158 command-buffer approximation of ScrollWindowEx's client bits:
           commands wholly inside the scroll source are translated by dx/dy.
           Partial commands are intentionally left for the invalidated uncovered
           region/WM_PAINT path until true retained surfaces/clipping regions land. */
        if (c->rc.left >= effective.left && c->rc.top >= effective.top &&
            c->rc.right <= effective.right && c->rc.bottom <= effective.bottom) {
            c->rc.left += dx;
            c->rc.right += dx;
            c->rc.top += dy;
            c->rc.bottom += dy;
        }
    }
    pthread_mutex_unlock(&g_GdiLock);
    return TRUE;
}

BOOL MyGdiGetWindowState(HWND hWnd, MYGDI_WINDOW_SNAPSHOT* lpOut)
{
    if (!lpOut) return FALSE;
    memset(lpOut, 0, sizeof(*lpOut));
    pthread_mutex_lock(&g_GdiLock);
    MyGdiWindowState* ws = mygdi_window_locked(hWnd, 0);
    if (ws) {
        lpOut->dirty = ws->dirty;
        lpOut->paintPending = ws->paintPending;
        lpOut->erasePending = ws->erasePending;
        lpOut->internalPaint = ws->internalPaint;
        lpOut->dirtyRect = ws->dirtyRect;
        lpOut->invalidateSerial = ws->invalidateSerial;
        lpOut->postedPaints = ws->postedPaints;
        lpOut->coalescedInvalidates = ws->coalescedInvalidates;
    }
    pthread_mutex_unlock(&g_GdiLock);
    return ws ? TRUE : FALSE;
}

DWORD MyGdiGetBrushSelectedCount(HBRUSH hBrush)
{
    DWORD n = 0;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiBrushObj* br = mygdi_find_brush_locked(hBrush);
    if (br) n = br->selectedCount;
    pthread_mutex_unlock(&g_GdiLock);
    return n;
}

void MyGdiBlitWindow(HWND hWnd, int clientX, int clientY, int clientW, int clientH, Framebuffer* fb)
{
    if (!fb || !hWnd) return;
    pthread_mutex_lock(&g_GdiLock);
    MyGdiCommand cmds[MYGDI_MAX_COMMANDS];
    int n = 0;
    int cmdIdx[MYGDI_MAX_COMMANDS];
    int cmdCount = mygdi_collect_command_indices_locked(hWnd, cmdIdx, MYGDI_MAX_COMMANDS);
    for (int ci = 0; ci < cmdCount && n < MYGDI_MAX_COMMANDS; ++ci) {
        int i = cmdIdx[ci];
        cmds[n] = g_GdiCommands[i];
        if (g_GdiCommands[i].type == MYGDI_CMD_BLIT && g_GdiCommands[i].blitPixels &&
            g_GdiCommands[i].blitW > 0 && g_GdiCommands[i].blitH > 0) {
            size_t bytes = (size_t)g_GdiCommands[i].blitW * (size_t)g_GdiCommands[i].blitH * sizeof(COLORREF);
            cmds[n].blitPixels = (COLORREF*)malloc(bytes);
            if (cmds[n].blitPixels) memcpy(cmds[n].blitPixels, g_GdiCommands[i].blitPixels, bytes);
        }
        n++;
    }
    pthread_mutex_unlock(&g_GdiLock);

    for (int i = 0; i < n; i++) {
        MyGdiCommand* c = &cmds[i];
        int x = clientX + (int)c->rc.left;
        int y = clientY + (int)c->rc.top;
        if (c->type == MYGDI_CMD_FILL) {
            int w = (int)(c->rc.right - c->rc.left);
            int h = (int)(c->rc.bottom - c->rc.top);
            if (x < clientX) { w -= (clientX - x); x = clientX; }
            if (y < clientY) { h -= (clientY - y); y = clientY; }
            if (x + w > clientX + clientW) w = clientX + clientW - x;
            if (y + h > clientY + clientH) h = clientY + clientH - y;
            if (w > 0 && h > 0) fb_rect(fb, x, y, w, h, mygdi_color(c->color));
        } else if (c->type == MYGDI_CMD_RECT) {
            int w = (int)(c->rc.right - c->rc.left);
            int h = (int)(c->rc.bottom - c->rc.top);
            if (w > 0 && h > 0) fb_rect_outline(fb, x, y, w, h, mygdi_color(c->color));
        } else if (c->type == MYGDI_CMD_TEXT) {
            if (x >= clientX && y >= clientY && x < clientX + clientW && y < clientY + clientH)
                font_draw_str(fb, x, y, c->text, mygdi_color(c->color));
        } else if (c->type == MYGDI_CMD_PATBLT) {
            int w = (int)(c->rc.right - c->rc.left);
            int h = (int)(c->rc.bottom - c->rc.top);
            int sx0 = 0, sy0 = 0;
            if (x < clientX) { sx0 = clientX - x; w -= sx0; x = clientX; }
            if (y < clientY) { sy0 = clientY - y; h -= sy0; y = clientY; }
            if (x + w > clientX + clientW) w = clientX + clientW - x;
            if (y + h > clientY + clientH) h = clientY + clientH - y;
            if (w > 0 && h > 0) {
                int clip_x = 0, clip_y = 0, clip_w = fb->width, clip_h = fb->height;
                fb_current_clip(fb, &clip_x, &clip_y, &clip_w, &clip_h);
                int clip_r = clip_x + clip_w;
                int clip_b = clip_y + clip_h;
                for (int yy = 0; yy < h; ++yy) {
                    int fy = y + yy;
                    if (fy < 0 || fy >= fb->height || fy < clip_y || fy >= clip_b) continue;
                    uint32_t* row = (uint32_t*)(fb->backbuf + fy * fb->stride);
                    (void)sy0;
                    for (int xx = 0; xx < w; ++xx) {
                        int fx = x + xx;
                        if (fx < 0 || fx >= fb->width || fx < clip_x || fx >= clip_r) continue;
                        (void)sx0;
                        row[fx] = mygdi_apply_patblt_rop(c->rop, row[fx], c->color);
                    }
                }
            }
        } else if (c->type == MYGDI_CMD_BLIT && c->blitPixels) {
            for (int yy = 0; yy < c->blitH; yy++) {
                int dy = y + yy;
                if (dy < clientY || dy >= clientY + clientH) continue;
                for (int xx = 0; xx < c->blitW; xx++) {
                    int dx = x + xx;
                    if (dx < clientX || dx >= clientX + clientW) continue;
                    fb_pixel(fb, dx, dy, mygdi_color(c->blitPixels[yy * c->blitW + xx]));
                }
            }
        }
        if (c->type == MYGDI_CMD_BLIT && c->blitPixels) free(c->blitPixels);
    }
}
