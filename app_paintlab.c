#include "app_paintlab.h"
#include "window.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): PaintLab is the GDI invalidation/paint-contract canary.
   It mixes BeginPaint/EndPaint, GetDC/ReleaseDC, invalid regions and brush
   lifetime. It will expose bugs when HDCs become stricter objects or when paint
   is clipped per-window surface instead of framebuffer command shortcuts. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define PAINT_LOG_LINES 10
#define PAINT_LOG_CHARS 192

typedef struct PaintLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    Capability cap;
    pthread_mutex_t lock;
    HBRUSH hBrushBack;
    HBRUSH hBrushRect;
    HBRUSH hExtraBrush;
    DWORD paintCount;
    DWORD invalidateCount;
    DWORD beginCount;
    DWORD endCount;
    DWORD getDcCount;
    DWORD stressCount;
    DWORD validateCount;
    DWORD brushCreateCount;
    DWORD brushDeleteCount;
    int drawText;
    int drawRect;
    char status[192];
    MyAppResizeState resize;
    char log[PAINT_LOG_LINES][PAINT_LOG_CHARS];
    int logCount;
} PaintLabApp;

static PaintLabApp g_paint;

static void paint_log_locked(const char* s)
{
    if (!s) return;
    if (g_paint.logCount < PAINT_LOG_LINES) snprintf(g_paint.log[g_paint.logCount++], sizeof(g_paint.log[0]), "%s", s);
    else {
        for (int i = 1; i < PAINT_LOG_LINES; i++) snprintf(g_paint.log[i-1], sizeof(g_paint.log[0]), "%s", g_paint.log[i]);
        snprintf(g_paint.log[PAINT_LOG_LINES-1], sizeof(g_paint.log[0]), "%s", s);
    }
}


static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,50,70));
    fb_rect_outline(fb, x, y, w, 20, COLOR(120,140,175));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}

static void ensure_runtime(void)
{
    MyWinBindRuntime(g_paint.mgr, &g_paint.cap);
}

static void post_self(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_paint.mgr && g_paint.hWnd) hwnd_post(g_paint.mgr, &g_paint.cap, g_paint.hWnd, msg, wp, lp);
}

static void do_invalidate(void)
{
    ensure_runtime();
    RECT r = { 8, 66, PAINTLAB_W - 18, PAINTLAB_H - TITLEBAR_H - 78 };
    BOOL ok = InvalidateRect(g_paint.hWnd, &r, TRUE);
    pthread_mutex_lock(&g_paint.lock);
    if (ok) g_paint.invalidateCount++;
    snprintf(g_paint.status, sizeof(g_paint.status), "InvalidateRect(hwnd,&rc,TRUE) -> %s; WM_PAINT queued", ok ? "TRUE" : "FALSE");
    paint_log_locked(g_paint.status);
    pthread_mutex_unlock(&g_paint.lock);
}

static void do_getdc_draw(void)
{
    ensure_runtime();
    HDC hdc = GetDC(g_paint.hWnd);
    if (hdc) {
        HGDIOBJ old = SelectObject(hdc, g_paint.hBrushRect);
        Rectangle(hdc, 390, 92, 630, 158);
        TextOutA(hdc, 400, 112, "GetDC path: immediate command", 29);
        SelectObject(hdc, old);
        ReleaseDC(g_paint.hWnd, hdc);
    }
    pthread_mutex_lock(&g_paint.lock);
    g_paint.getDcCount++;
    snprintf(g_paint.status, sizeof(g_paint.status), "GetDC/Rectangle/TextOut/ReleaseDC -> hdc=0x%x", hdc);
    paint_log_locked(g_paint.status);
    pthread_mutex_unlock(&g_paint.lock);
}

static void do_validate(void)
{
    ensure_runtime();
    RECT r = { 8, 66, PAINTLAB_W - 18, PAINTLAB_H - TITLEBAR_H - 78 };
    BOOL ok = ValidateRect(g_paint.hWnd, &r);
    pthread_mutex_lock(&g_paint.lock);
    if (ok) g_paint.validateCount++;
    snprintf(g_paint.status, sizeof(g_paint.status), "ValidateRect(hwnd,&rc) -> %s; dirty/pending cleared", ok ? "TRUE" : "FALSE");
    paint_log_locked(g_paint.status);
    pthread_mutex_unlock(&g_paint.lock);
}

static void do_brush_plus(void)
{
    ensure_runtime();
    static unsigned seed = 0;
    seed++;
    COLORREF c = RGB((40 + seed*53) & 255, (100 + seed*37) & 255, (170 + seed*19) & 255);
    HBRUSH h = CreateSolidBrush(c);
    pthread_mutex_lock(&g_paint.lock);
    g_paint.hExtraBrush = h;
    if (h) g_paint.brushCreateCount++;
    snprintf(g_paint.status, sizeof(g_paint.status), "CreateSolidBrush(extra) -> HBRUSH=0x%x; ObjectLab should show BRUSH", h);
    paint_log_locked(g_paint.status);
    pthread_mutex_unlock(&g_paint.lock);
}

static void do_delete_extra_brush(void)
{
    ensure_runtime();
    HBRUSH h;
    pthread_mutex_lock(&g_paint.lock);
    h = g_paint.hExtraBrush;
    pthread_mutex_unlock(&g_paint.lock);
    BOOL ok = h ? DeleteObject(h) : FALSE;
    pthread_mutex_lock(&g_paint.lock);
    if (ok) { g_paint.brushDeleteCount++; g_paint.hExtraBrush = 0; }
    snprintf(g_paint.status, sizeof(g_paint.status), "DeleteObject(extraBrush=0x%x) -> %s", h, ok ? "TRUE" : "FALSE");
    paint_log_locked(g_paint.status);
    pthread_mutex_unlock(&g_paint.lock);
}

static void paint_hit_test(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8   && cx < 120) { post_self(WM_COMMAND, PAINT_CMD_INVALIDATE, 0); return; }
        if (cx >= 128 && cx < 240) { post_self(WM_COMMAND, PAINT_CMD_TEXT, 0); return; }
        if (cx >= 248 && cx < 360) { post_self(WM_COMMAND, PAINT_CMD_RECT, 0); return; }
        if (cx >= 368 && cx < 480) { post_self(WM_COMMAND, PAINT_CMD_GETDC, 0); return; }
        if (cx >= 488 && cx < 600) { post_self(WM_COMMAND, PAINT_CMD_CLEAR, 0); return; }
    }
    if (cy >= 34 && cy < 54) {
        if (cx >= 8   && cx < 120) { post_self(WM_COMMAND, PAINT_CMD_STRESS, 0); return; }
        if (cx >= 128 && cx < 240) { post_self(WM_COMMAND, PAINT_CMD_VALIDATE, 0); return; }
        if (cx >= 248 && cx < 360) { post_self(WM_COMMAND, PAINT_CMD_BRUSHPLUS, 0); return; }
        if (cx >= 368 && cx < 480) { post_self(WM_COMMAND, PAINT_CMD_DELBRUSH, 0); return; }
    }
}

static LRESULT CALLBACK paintlab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_paint.lock);
        g_paint.hWnd = hwnd;
        MyAppResizeInit(&g_paint.resize, PAINTLAB_W, PAINTLAB_H, TITLEBAR_H);
        snprintf(g_paint.status, sizeof(g_paint.status), "PaintLab v35 ready: GDI objects + dirty region coalescing");
        paint_log_locked(g_paint.status);
        pthread_mutex_unlock(&g_paint.lock);
        ensure_runtime();
        g_paint.hBrushBack = CreateSolidBrush(RGB(12, 14, 28));
        g_paint.hBrushRect = CreateSolidBrush(RGB(40, 96, 180));
        do_invalidate();
        break;
    case WM_LBUTTONDOWN:
        paint_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_COMMAND:
        if ((UINT)wp == PAINT_CMD_INVALIDATE) do_invalidate();
        else if ((UINT)wp == PAINT_CMD_TEXT) {
            pthread_mutex_lock(&g_paint.lock); g_paint.drawText = !g_paint.drawText; snprintf(g_paint.status, sizeof(g_paint.status), "Draw Text toggled -> %s", g_paint.drawText ? "ON" : "OFF"); paint_log_locked(g_paint.status); pthread_mutex_unlock(&g_paint.lock); do_invalidate();
        } else if ((UINT)wp == PAINT_CMD_RECT) {
            pthread_mutex_lock(&g_paint.lock); g_paint.drawRect = !g_paint.drawRect; snprintf(g_paint.status, sizeof(g_paint.status), "Draw Rect toggled -> %s", g_paint.drawRect ? "ON" : "OFF"); paint_log_locked(g_paint.status); pthread_mutex_unlock(&g_paint.lock); do_invalidate();
        } else if ((UINT)wp == PAINT_CMD_GETDC) do_getdc_draw();
        else if ((UINT)wp == PAINT_CMD_CLEAR) {
            pthread_mutex_lock(&g_paint.lock); g_paint.drawText = 0; g_paint.drawRect = 0; snprintf(g_paint.status, sizeof(g_paint.status), "Clear -> reset flags and invalidate"); paint_log_locked(g_paint.status); pthread_mutex_unlock(&g_paint.lock); do_invalidate();
        } else if ((UINT)wp == PAINT_CMD_STRESS) {
            pthread_mutex_lock(&g_paint.lock); g_paint.stressCount++; snprintf(g_paint.status, sizeof(g_paint.status), "Stress Paint -> 6 invalidates; v35 should coalesce pending WM_PAINT"); paint_log_locked(g_paint.status); pthread_mutex_unlock(&g_paint.lock);
            for (int i=0;i<6;i++) do_invalidate();
        } else if ((UINT)wp == PAINT_CMD_VALIDATE) do_validate();
        else if ((UINT)wp == PAINT_CMD_BRUSHPLUS) do_brush_plus();
        else if ((UINT)wp == PAINT_CMD_DELBRUSH) do_delete_extra_brush();
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        pthread_mutex_lock(&g_paint.lock);
        g_paint.paintCount++;
        if (hdc) g_paint.beginCount++;
        int drawText = g_paint.drawText;
        int drawRect = g_paint.drawRect;
        DWORD pc = g_paint.paintCount;
        pthread_mutex_unlock(&g_paint.lock);

        if (hdc) {
            RECT bg = { 8, 66, PAINTLAB_W - 20, 210 };
            FillRect(hdc, &bg, g_paint.hBrushBack);
            char line[160];
            snprintf(line, sizeof(line), "WM_PAINT #%u  rcPaint={%d,%d,%d,%d}  HDC=0x%x", pc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom, hdc);
            TextOutA(hdc, 18, 78, line, (int)strlen(line));
            TextOutA(hdc, 18, 96, "BeginPaint cleared dirty state; EndPaint validates.", 47);
            if (drawText) TextOutA(hdc, 18, 124, "DrawText/TextOut path is now command-buffered GDI-lite.", 54);
            if (drawRect) {
                HGDIOBJ old = SelectObject(hdc, g_paint.hBrushRect);
                Rectangle(hdc, 24, 150, 350, 198);
                SelectObject(hdc, old);
                TextOutA(hdc, 36, 168, "Rectangle via selected brush", 28);
            }
            EndPaint(hwnd, &ps);
            pthread_mutex_lock(&g_paint.lock);
            g_paint.endCount++;
            snprintf(g_paint.status, sizeof(g_paint.status), "WM_PAINT #%u -> BeginPaint hdc=0x%x, EndPaint TRUE", pc, hdc);
            paint_log_locked(g_paint.status);
            pthread_mutex_unlock(&g_paint.lock);
        }
        break;
    }
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_paint.resize, lp, PAINTLAB_MIN_W, PAINTLAB_MIN_H);
        return 0;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_paint.resize, lp);
        return 0;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_paint.resize, lp, TITLEBAR_H);
        do_invalidate();
        return 0;
    case WM_MOVE:
        MyAppResizeOnMove(&g_paint.resize, lp);
        return 0;
    case WM_SIZE:
        MyAppResizeOnSize(&g_paint.resize, wp, lp);
        do_invalidate();
        return 0;
    case WM_DESTROY:
        if (g_paint.hExtraBrush) { DeleteObject(g_paint.hExtraBrush); g_paint.hExtraBrush = 0; }
        if (g_paint.hBrushBack) { DeleteObject(g_paint.hBrushBack); g_paint.hBrushBack = 0; }
        if (g_paint.hBrushRect) { DeleteObject(g_paint.hBrushRect); g_paint.hBrushRect = 0; }
        break;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

HWND paintlab_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    static int s_lock_init = 0;
    if (!s_lock_init) { pthread_mutex_init(&g_paint.lock, NULL); s_lock_init = 1; }
    pthread_mutex_lock(&g_paint.lock);
    HWNDManager* oldMgr = mgr;
    Capability oldCap = cap;
    memset(&g_paint, 0, sizeof(g_paint));
    g_paint.mgr = oldMgr;
    g_paint.cap = oldCap;
    pthread_mutex_unlock(&g_paint.lock);

    MyWinBindRuntime(mgr, &cap);
    {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = paintlab_wndproc;
        wc.lpszClassName = "myOS.PaintLab";
        (void)RegisterClassExA(&wc);
    }
    HWND hWnd = CreateWindowExA(WS_EX_NONE, "myOS.PaintLab", "myOS PaintLab",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                x, y, PAINTLAB_W, PAINTLAB_H, 0, 0, 0, NULL);
    g_paint.hWnd = hWnd;
    return hWnd;
}

void paintlab_destroy(void)
{
    pthread_mutex_lock(&g_paint.lock);
    memset(&g_paint, 0, sizeof(g_paint));
    pthread_mutex_unlock(&g_paint.lock);
}

void paintlab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    int x = wx + 1;
    int y = wy + TITLEBAR_H;
    int w = ww - 2;
    int h = wh - TITLEBAR_H - 1;
    if (w <= 0 || h <= 0) return;

    fb_rect(fb, x, y, w, h, COLOR(8,8,18));

    // Render the GDI-lite command surface that WM_PAINT/GetDC produced.
    MyGdiBlitWindow(hwnd, x, y, w, h, fb);

    button(fb, x+8,   y+8, 112, "Invalidate");
    button(fb, x+128, y+8, 112, "Draw Text");
    button(fb, x+248, y+8, 112, "Draw Rect");
    button(fb, x+368, y+8, 112, "GetDC Draw");
    button(fb, x+488, y+8, 112, "Clear");
    button(fb, x+8,   y+34, 112, "Stress");
    button(fb, x+128, y+34, 112, "Validate");
    button(fb, x+248, y+34, 112, "Brush+");
    button(fb, x+368, y+34, 112, "DelBrush");

    pthread_mutex_lock(&g_paint.lock);
    PaintLabApp s = g_paint;
    pthread_mutex_unlock(&g_paint.lock);

    MYGDI_WINDOW_SNAPSHOT gs;
    memset(&gs, 0, sizeof(gs));
    MyGdiGetWindowState(hwnd, &gs);
    DWORD rectSel = s.hBrushRect ? MyGdiGetBrushSelectedCount(s.hBrushRect) : 0;
    char line[256];
    snprintf(line, sizeof(line), "PAINT=%u invalid=%u posted=%u coal=%u dirty=%s pending=%s validate=%u getdc=%u stress=%u",
             s.paintCount, s.invalidateCount, gs.postedPaints, gs.coalescedInvalidates,
             gs.dirty ? "yes" : "no", gs.paintPending ? "yes" : "no", s.validateCount, s.getDcCount, s.stressCount);
    draw_clip_text(fb, x+8, y+222, line, COLOR(120,255,170), x+8, y+216, w-16, 18);
    snprintf(line, sizeof(line), "DirtyRect={%d,%d,%d,%d} BrushRect=0x%x sel=%u Extra=0x%x +%u -%u text=%s rect=%s",
             gs.dirtyRect.left, gs.dirtyRect.top, gs.dirtyRect.right, gs.dirtyRect.bottom,
             s.hBrushRect, rectSel, s.hExtraBrush, s.brushCreateCount, s.brushDeleteCount,
             s.drawText ? "ON" : "OFF", s.drawRect ? "ON" : "OFF");
    draw_clip_text(fb, x+8, y+242, line, COLOR(160,210,255), x+8, y+236, w-16, 18);
    snprintf(line, sizeof(line), "Status: %s", s.status);
    draw_clip_text(fb, x+8, y+262, line, COLOR(255,230,160), x+8, y+256, w-16, 18);

    int log_y = y + 286;
    fb_rect_outline(fb, x+8, log_y, w-16, h - 294, COLOR(70,80,115));
    font_draw_str(fb, x+14, log_y+8, "GDI / WM_PAINT log", WHITE);
    int yy = log_y + 28;
    for (int i = 0; i < s.logCount && yy < y + h - 12; i++, yy += 15) {
        draw_clip_text(fb, x+14, yy, s.log[i], WHITE, x+12, log_y+24, w-24, h-320);
    }
}
