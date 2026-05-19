#include "app_draglab.h"
#include "window.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): DragLab is the capture/input-routing canary. It will break
   first when capture changes from a global HWND shortcut to queue/thread/desktop
   scoped behavior, or when ScreenToClient is moved fully into receiver-specific
   dispatch. If outside-window drag stops, inspect SetCapture/ReleaseCapture and
   lParam coordinate packing before editing the lab. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define DRAG_LOG_LINES 10
#define DRAG_LOG_CHARS 192

typedef struct DragLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    Capability cap;
    pthread_mutex_t lock;
    int boxX, boxY;
    int boxW, boxH;
    int targetX, targetY, targetW, targetH;
    int dragging;
    int capActive;
    int dragOffX, dragOffY;
    int mouseClientX, mouseClientY;
    int mouseScreenX, mouseScreenY;
    DWORD captureCount;
    DWORD releaseCount;
    DWORD moveCount;
    DWORD dropCount;
    DWORD cancelCount;
    HWND captureHwnd;
    HWND lastCaptureChangedTo;
    MyAppResizeState resize;
    char status[192];
    char log[DRAG_LOG_LINES][DRAG_LOG_CHARS];
    int logCount;
} DragLabApp;

static DragLabApp g_drag;

static void drag_log_locked(const char* s)
{
    if (!s) return;
    if (g_drag.logCount < DRAG_LOG_LINES) snprintf(g_drag.log[g_drag.logCount++], sizeof(g_drag.log[0]), "%s", s);
    else {
        for (int i = 1; i < DRAG_LOG_LINES; i++) snprintf(g_drag.log[i-1], sizeof(g_drag.log[0]), "%s", g_drag.log[i]);
        snprintf(g_drag.log[DRAG_LOG_LINES-1], sizeof(g_drag.log[0]), "%s", s);
    }
}

static void ensure_runtime(void)
{
    MyWinBindRuntime(g_drag.mgr, &g_drag.cap);
}

static void post_self(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_drag.mgr && g_drag.hWnd) hwnd_post(g_drag.mgr, &g_drag.cap, g_drag.hWnd, msg, wp, lp);
}

static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,50,70));
    fb_rect_outline(fb, x, y, w, 20, COLOR(120,140,175));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}


static int __attribute__((unused)) to_client(int sx, int sy, int* cx, int* cy)
{
    MyWindowState st;
    memset(&st, 0, sizeof(st));
    if (!hwnd_copy_window_state(g_drag.mgr, g_drag.hWnd, &st)) return 0;
    *cx = sx - (int)st.rcWindow.left;
    *cy = sy - (int)st.rcWindow.top - TITLEBAR_H;
    return 1;
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void do_capture_button(void)
{
    ensure_runtime();
    HWND oldOrSelf = SetCapture(g_drag.hWnd);
    pthread_mutex_lock(&g_drag.lock);
    g_drag.capActive = (GetCapture() == g_drag.hWnd);
    g_drag.captureHwnd = GetCapture();
    g_drag.captureCount++;
    snprintf(g_drag.status, sizeof(g_drag.status), "SetCapture(hwnd=0x%x) -> 0x%x; capture now=0x%x", g_drag.hWnd, oldOrSelf, g_drag.captureHwnd);
    drag_log_locked(g_drag.status);
    pthread_mutex_unlock(&g_drag.lock);
}

static void do_release_button(void)
{
    ensure_runtime();
    BOOL ok = ReleaseCapture();
    pthread_mutex_lock(&g_drag.lock);
    g_drag.capActive = (GetCapture() == g_drag.hWnd);
    g_drag.captureHwnd = GetCapture();
    if (ok) g_drag.releaseCount++;
    g_drag.dragging = 0;
    snprintf(g_drag.status, sizeof(g_drag.status), "ReleaseCapture() -> %s; capture now=0x%x", ok ? "TRUE" : "FALSE", g_drag.captureHwnd);
    drag_log_locked(g_drag.status);
    pthread_mutex_unlock(&g_drag.lock);
}

static void reset_box_locked(void)
{
    g_drag.boxX = 48; g_drag.boxY = 120; g_drag.boxW = 90; g_drag.boxH = 54;
    g_drag.targetX = 410; g_drag.targetY = 104; g_drag.targetW = 170; g_drag.targetH = 110;
}

static void drag_hit_test(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8   && cx < 120) { post_self(WM_COMMAND, DRAG_CMD_CAPTURE, 0); return; }
        if (cx >= 128 && cx < 240) { post_self(WM_COMMAND, DRAG_CMD_RELEASE, 0); return; }
        if (cx >= 248 && cx < 360) { post_self(WM_COMMAND, DRAG_CMD_RESET, 0); return; }
        if (cx >= 368 && cx < 480) { post_self(WM_COMMAND, DRAG_CMD_CANCEL, 0); return; }
    }

    pthread_mutex_lock(&g_drag.lock);
    g_drag.mouseScreenX = cx; g_drag.mouseScreenY = cy;
    g_drag.mouseClientX = cx; g_drag.mouseClientY = cy;
    if (inside(cx, cy, g_drag.boxX, g_drag.boxY, g_drag.boxW, g_drag.boxH)) {
        g_drag.dragging = 1;
        g_drag.dragOffX = cx - g_drag.boxX;
        g_drag.dragOffY = cy - g_drag.boxY;
        pthread_mutex_unlock(&g_drag.lock);
        ensure_runtime();
        SetCapture(g_drag.hWnd);
        pthread_mutex_lock(&g_drag.lock);
        g_drag.capActive = (GetCapture() == g_drag.hWnd);
        g_drag.captureHwnd = GetCapture();
        g_drag.captureCount++;
        snprintf(g_drag.status, sizeof(g_drag.status), "Begin drag: SetCapture -> hwnd=0x%x; moving outside client keeps WM_MOUSEMOVE", g_drag.hWnd);
        drag_log_locked(g_drag.status);
    }
    pthread_mutex_unlock(&g_drag.lock);
}

static LRESULT CALLBACK draglab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_drag.lock);
        g_drag.hWnd = hwnd;
        MyAppResizeInit(&g_drag.resize, DRAGLAB_W, DRAGLAB_H, TITLEBAR_H);
        reset_box_locked();
        snprintf(g_drag.status, sizeof(g_drag.status), "DragLab v36 ready: SetCapture/ReleaseCapture + drop target lab");
        drag_log_locked(g_drag.status);
        pthread_mutex_unlock(&g_drag.lock);
        break;
    case WM_LBUTTONDOWN:
        drag_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_MOUSEMOVE: {
        int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        pthread_mutex_lock(&g_drag.lock);
        g_drag.mouseScreenX = cx; g_drag.mouseScreenY = cy;
        g_drag.mouseClientX = cx; g_drag.mouseClientY = cy;
        if (g_drag.dragging) {
            g_drag.boxX = cx - g_drag.dragOffX;
            g_drag.boxY = cy - g_drag.dragOffY;
            g_drag.moveCount++;
            if (g_drag.boxX < 8) g_drag.boxX = 8;
            if (g_drag.boxY < 48) g_drag.boxY = 48;
            int maxBoxX = (g_drag.resize.clientW > 0 ? g_drag.resize.clientW : (DRAGLAB_W - 2)) - g_drag.boxW - 24;
            int maxBoxY = (g_drag.resize.clientH > 0 ? g_drag.resize.clientH : (DRAGLAB_H - TITLEBAR_H - 1)) - g_drag.boxH - 28;
            if (maxBoxX < 8) maxBoxX = 8;
            if (maxBoxY < 48) maxBoxY = 48;
            if (g_drag.boxX > maxBoxX) g_drag.boxX = maxBoxX;
            if (g_drag.boxY > maxBoxY) g_drag.boxY = maxBoxY;
            snprintf(g_drag.status, sizeof(g_drag.status), "WM_MOUSEMOVE captured: client=(%d,%d) box=(%d,%d)", cx, cy, g_drag.boxX, g_drag.boxY);
        }
        g_drag.captureHwnd = GetCapture();
        g_drag.capActive = (g_drag.captureHwnd == hwnd);
        pthread_mutex_unlock(&g_drag.lock);
        break;
    }
    case WM_LBUTTONUP: {
        int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        pthread_mutex_lock(&g_drag.lock);
        int wasDrag = g_drag.dragging;
        g_drag.dragging = 0;
        int dropped = inside(g_drag.boxX + g_drag.boxW/2, g_drag.boxY + g_drag.boxH/2, g_drag.targetX, g_drag.targetY, g_drag.targetW, g_drag.targetH);
        if (wasDrag && dropped) g_drag.dropCount++;
        snprintf(g_drag.status, sizeof(g_drag.status), "%s: WM_LBUTTONUP client=(%d,%d) drop=%s", wasDrag ? "Drop" : "MouseUp", cx, cy, (wasDrag && dropped) ? "TARGET" : "no");
        drag_log_locked(g_drag.status);
        pthread_mutex_unlock(&g_drag.lock);
        ensure_runtime();
        ReleaseCapture();
        pthread_mutex_lock(&g_drag.lock);
        g_drag.releaseCount++;
        g_drag.captureHwnd = GetCapture();
        g_drag.capActive = (g_drag.captureHwnd == hwnd);
        pthread_mutex_unlock(&g_drag.lock);
        break;
    }
    case WM_CAPTURECHANGED:
        pthread_mutex_lock(&g_drag.lock);
        g_drag.lastCaptureChangedTo = (HWND)wp;
        g_drag.captureHwnd = GetCapture();
        g_drag.capActive = (g_drag.captureHwnd == hwnd);
        int hadDrag = g_drag.dragging;
        if (!g_drag.capActive) g_drag.dragging = 0;
        if (hadDrag) {
            snprintf(g_drag.status, sizeof(g_drag.status), "WM_CAPTURECHANGED: new capture hwnd=0x%lx", (unsigned long)wp);
        } else {
            char capmsg[128];
            snprintf(capmsg, sizeof(capmsg), "WM_CAPTURECHANGED: new capture hwnd=0x%lx", (unsigned long)wp);
            drag_log_locked(capmsg);
            pthread_mutex_unlock(&g_drag.lock);
            break;
        }
        drag_log_locked(g_drag.status);
        pthread_mutex_unlock(&g_drag.lock);
        break;
    case WM_COMMAND:
        if ((UINT)wp == DRAG_CMD_CAPTURE) do_capture_button();
        else if ((UINT)wp == DRAG_CMD_RELEASE) do_release_button();
        else if ((UINT)wp == DRAG_CMD_RESET) {
            pthread_mutex_lock(&g_drag.lock); reset_box_locked(); snprintf(g_drag.status, sizeof(g_drag.status), "Reset: box returned to source position"); drag_log_locked(g_drag.status); pthread_mutex_unlock(&g_drag.lock);
        } else if ((UINT)wp == DRAG_CMD_CANCEL) {
            ensure_runtime(); ReleaseCapture();
            pthread_mutex_lock(&g_drag.lock); g_drag.dragging = 0; g_drag.cancelCount++; g_drag.captureHwnd = GetCapture(); g_drag.capActive = 0; snprintf(g_drag.status, sizeof(g_drag.status), "Cancel drag -> ReleaseCapture"); drag_log_locked(g_drag.status); pthread_mutex_unlock(&g_drag.lock);
        }
        break;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_drag.resize, lp, DRAGLAB_MIN_W, DRAGLAB_MIN_H);
        return 0;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_drag.resize, lp);
        return 0;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_drag.resize, lp, TITLEBAR_H);
        return 0;
    case WM_MOVE:
        MyAppResizeOnMove(&g_drag.resize, lp);
        return 0;
    case WM_SIZE:
        MyAppResizeOnSize(&g_drag.resize, wp, lp);
        return 0;
    case WM_CLOSE:
        return DefWindowProcA(hwnd, msg, wp, lp);
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

HWND draglab_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    static int s_lock_init = 0;
    if (!s_lock_init) { pthread_mutex_init(&g_drag.lock, NULL); s_lock_init = 1; }
    pthread_mutex_lock(&g_drag.lock);
    HWNDManager* oldMgr = mgr;
    Capability oldCap = cap;
    memset(&g_drag, 0, sizeof(g_drag));
    g_drag.mgr = oldMgr;
    g_drag.cap = oldCap;
    reset_box_locked();
    pthread_mutex_unlock(&g_drag.lock);

    MyWinBindRuntime(mgr, &cap);
    {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = draglab_wndproc;
        wc.lpszClassName = "myOS.DragLab";
        (void)RegisterClassExA(&wc);
    }
    HWND hWnd = CreateWindowExA(WS_EX_NONE, "myOS.DragLab", "myOS DragLab",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                x, y, DRAGLAB_W, DRAGLAB_H, 0, 0, 0, NULL);
    g_drag.hWnd = hWnd;
    return hWnd;
}

void draglab_destroy(void)
{
    pthread_mutex_lock(&g_drag.lock);
    memset(&g_drag, 0, sizeof(g_drag));
    pthread_mutex_unlock(&g_drag.lock);
}

void draglab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    (void)hwnd;
    int x = wx + 1;
    int y = wy + TITLEBAR_H;
    int w = ww - 2;
    int h = wh - TITLEBAR_H - 1;
    if (w <= 0 || h <= 0) return;

    fb_rect(fb, x, y, w, h, COLOR(8,8,18));
    button(fb, x+8,   y+8, 112, "Capture");
    button(fb, x+128, y+8, 112, "Release");
    button(fb, x+248, y+8, 112, "Reset");
    button(fb, x+368, y+8, 112, "Cancel");

    pthread_mutex_lock(&g_drag.lock);
    DragLabApp s = g_drag;
    pthread_mutex_unlock(&g_drag.lock);

    fb_rect(fb, x+s.targetX, y+s.targetY, s.targetW, s.targetH, COLOR(28,44,34));
    fb_rect_outline(fb, x+s.targetX, y+s.targetY, s.targetW, s.targetH, COLOR(90,190,120));
    font_draw_str(fb, x+s.targetX+14, y+s.targetY+14, "DROP TARGET", COLOR(160,255,180));
    font_draw_str(fb, x+s.targetX+14, y+s.targetY+34, "release box here", COLOR(190,220,190));

    Color boxColor = s.dragging ? COLOR(80,120,210) : COLOR(60,80,150);
    fb_rect(fb, x+s.boxX, y+s.boxY, s.boxW, s.boxH, boxColor);
    fb_rect_outline(fb, x+s.boxX, y+s.boxY, s.boxW, s.boxH, WHITE);
    font_draw_str(fb, x+s.boxX+14, y+s.boxY+18, "DRAG ME", WHITE);

    char line[256];
    snprintf(line, sizeof(line), "capture=%s hwnd=0x%x GetCapture=0x%x dragging=%s mouse=(%d,%d) screen=(%d,%d)",
             s.capActive ? "yes" : "no", s.hWnd, s.captureHwnd, s.dragging ? "yes" : "no", s.mouseClientX, s.mouseClientY, s.mouseScreenX, s.mouseScreenY);
    draw_clip_text(fb, x+8, y+238, line, COLOR(120,255,170), x+8, y+232, w-16, 18);
    snprintf(line, sizeof(line), "captures=%u releases=%u moves=%u drops=%u cancels=%u lastCaptureChangedTo=0x%x",
             s.captureCount, s.releaseCount, s.moveCount, s.dropCount, s.cancelCount, s.lastCaptureChangedTo);
    draw_clip_text(fb, x+8, y+258, line, COLOR(160,210,255), x+8, y+252, w-16, 18);
    snprintf(line, sizeof(line), "Status: %s", s.status);
    draw_clip_text(fb, x+8, y+278, line, COLOR(255,230,160), x+8, y+272, w-16, 18);

    int log_y = y + 304;
    fb_rect_outline(fb, x+8, log_y, w-16, h - 312, COLOR(70,80,115));
    font_draw_str(fb, x+14, log_y+8, "Capture / DragDrop log", WHITE);
    int yy = log_y + 28;
    for (int i = 0; i < s.logCount && yy < y + h - 12; i++, yy += 15)
        draw_clip_text(fb, x+14, yy, s.log[i], WHITE, x+12, log_y+24, w-24, h-336);
}
