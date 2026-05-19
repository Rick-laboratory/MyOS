#include "app_clipmenu.h"
#include "window.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): ClipMenuLab depends on three contracts at once: Clipboard,
   Accelerators and popup menus. TrackPopupMenu is still not a real modal menu
   loop, so submenu/escape/focus regressions here are expected during USER32 menu
   hardening. Clipboard ownership/global-memory lifetime is also transitional;
   later strict GlobalAlloc/SetClipboardData ownership may require app changes. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define CLIP_LOG_LINES 10
#define CLIP_LOG_CHARS 256

typedef struct ClipMenuLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    Capability cap;
    pthread_mutex_t lock;
    HMENU hMenu;
    HMENU hPopup;
    HACCEL hAccel;
    DWORD serial;
    DWORD setCount, getCount, clearCount, menuCount, popupCount, accelCount, commandCount;
    char localText[128];
    char clipboardText[160];
    char status[192];
    MyAppResizeState resize;
    char log[CLIP_LOG_LINES][CLIP_LOG_CHARS];
    int logCount;
} ClipMenuLabApp;

static ClipMenuLabApp g_clip;

static void clip_log_locked(const char* s)
{
    if (!s) return;
    if (g_clip.logCount < CLIP_LOG_LINES) snprintf(g_clip.log[g_clip.logCount++], sizeof(g_clip.log[0]), "%s", s);
    else {
        for (int i = 1; i < CLIP_LOG_LINES; i++) snprintf(g_clip.log[i-1], sizeof(g_clip.log[0]), "%s", g_clip.log[i]);
        snprintf(g_clip.log[CLIP_LOG_LINES-1], sizeof(g_clip.log[0]), "%s", s);
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
    MyWinBindRuntime(g_clip.mgr, &g_clip.cap);
}

static void post_self(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_clip.mgr && g_clip.hWnd) hwnd_post(g_clip.mgr, &g_clip.cap, g_clip.hWnd, msg, wp, lp);
}

static void clip_set_text(void)
{
    ensure_runtime();
    char text[128];
    DWORD n = ++g_clip.serial;
    snprintf(text, sizeof(text), "myOS clipboard text #%u from HWND=%u", n, g_clip.hWnd);

    BOOL ok = FALSE;
    if (OpenClipboard(g_clip.hWnd)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (DWORD)strlen(text) + 1);
        char* p = (char*)GlobalLock(h);
        if (p) {
            strcpy(p, text);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_TEXT, h) ? TRUE : FALSE;
        }
        CloseClipboard();
    }

    pthread_mutex_lock(&g_clip.lock);
    if (ok) {
        g_clip.setCount++;
        snprintf(g_clip.localText, sizeof(g_clip.localText), "%s", text);
        snprintf(g_clip.status, sizeof(g_clip.status), "SetClipboardData(CF_TEXT) -> TRUE text='%s'", text);
    } else {
        snprintf(g_clip.status, sizeof(g_clip.status), "SetClipboardData(CF_TEXT) -> FALSE");
    }
    clip_log_locked(g_clip.status);
    pthread_mutex_unlock(&g_clip.lock);
}

static void clip_get_text(void)
{
    ensure_runtime();
    char out[160] = "";
    BOOL ok = FALSE;
    if (OpenClipboard(g_clip.hWnd)) {
        if (IsClipboardFormatAvailable(CF_TEXT)) {
            HGLOBAL h = GetClipboardData(CF_TEXT);
            char* p = (char*)GlobalLock(h);
            if (p) {
                snprintf(out, sizeof(out), "%s", p);
                GlobalUnlock(h);
                ok = TRUE;
            }
        }
        CloseClipboard();
    }

    pthread_mutex_lock(&g_clip.lock);
    g_clip.getCount++;
    if (ok) {
        snprintf(g_clip.clipboardText, sizeof(g_clip.clipboardText), "%s", out);
        snprintf(g_clip.status, sizeof(g_clip.status), "GetClipboardData(CF_TEXT) -> '%s'", out);
    } else {
        snprintf(g_clip.clipboardText, sizeof(g_clip.clipboardText), "<empty or unavailable>");
        snprintf(g_clip.status, sizeof(g_clip.status), "GetClipboardData(CF_TEXT) -> EMPTY/FALSE");
    }
    clip_log_locked(g_clip.status);
    pthread_mutex_unlock(&g_clip.lock);
}

static void clip_clear(void)
{
    ensure_runtime();
    BOOL ok = FALSE;
    if (OpenClipboard(g_clip.hWnd)) { ok = EmptyClipboard(); CloseClipboard(); }
    pthread_mutex_lock(&g_clip.lock);
    if (ok) g_clip.clearCount++;
    snprintf(g_clip.clipboardText, sizeof(g_clip.clipboardText), "<cleared>");
    snprintf(g_clip.status, sizeof(g_clip.status), "EmptyClipboard() -> %s", ok ? "TRUE" : "FALSE");
    clip_log_locked(g_clip.status);
    pthread_mutex_unlock(&g_clip.lock);
}

static void clip_attach_menu(void)
{
    ensure_runtime();
    if (!g_clip.hMenu) {
        g_clip.hMenu = CreateMenu();
        AppendMenuA(g_clip.hMenu, MF_STRING, CLIP_CMD_COPY,  "Copy\tCtrl+C");
        AppendMenuA(g_clip.hMenu, MF_STRING, CLIP_CMD_PASTE, "Paste\tCtrl+V");
        AppendMenuA(g_clip.hMenu, MF_STRING, CLIP_CMD_NEW,   "New\tCtrl+N");
    }
    if (!g_clip.hPopup) {
        g_clip.hPopup = CreatePopupMenu();
        AppendMenuA(g_clip.hPopup, MF_STRING, CLIP_CMD_PASTE, "Popup Paste");
        AppendMenuA(g_clip.hPopup, MF_STRING, CLIP_CMD_COPY,  "Popup Copy");
    }
    BOOL ok = SetMenu(g_clip.hWnd, g_clip.hMenu);
    pthread_mutex_lock(&g_clip.lock);
    if (ok) g_clip.menuCount++;
    snprintf(g_clip.status, sizeof(g_clip.status), "CreateMenu/AppendMenuA/SetMenu -> %s menu=0x%x popup=0x%x", ok ? "TRUE" : "FALSE", g_clip.hMenu, g_clip.hPopup);
    clip_log_locked(g_clip.status);
    pthread_mutex_unlock(&g_clip.lock);
}

static void clip_popup(void)
{
    ensure_runtime();
    if (!g_clip.hPopup) clip_attach_menu();
    BOOL r = TrackPopupMenu(g_clip.hPopup, 0, 0, 0, 0, g_clip.hWnd, NULL);
    pthread_mutex_lock(&g_clip.lock);
    g_clip.popupCount++;
    snprintf(g_clip.status, sizeof(g_clip.status), "TrackPopupMenu modal -> %s", r ? "TRUE" : "FALSE");
    clip_log_locked(g_clip.status);
    pthread_mutex_unlock(&g_clip.lock);
}

static void clip_new_doc(void)
{
    pthread_mutex_lock(&g_clip.lock);
    snprintf(g_clip.localText, sizeof(g_clip.localText), "<new local buffer #%u>", ++g_clip.serial);
    snprintf(g_clip.status, sizeof(g_clip.status), "WM_COMMAND New/Ctrl+N -> local buffer reset");
    clip_log_locked(g_clip.status);
    pthread_mutex_unlock(&g_clip.lock);
}

static void clip_handle_command(UINT cmd, const char* via)
{
    pthread_mutex_lock(&g_clip.lock);
    g_clip.commandCount++;
    pthread_mutex_unlock(&g_clip.lock);
    if (cmd == CLIP_CMD_SET) { clip_set_text(); return; }
    if (cmd == CLIP_CMD_GET) { clip_get_text(); return; }
    if (cmd == CLIP_CMD_CLEAR) { clip_clear(); return; }
    if (cmd == CLIP_CMD_ATTACH) { clip_attach_menu(); return; }
    if (cmd == CLIP_CMD_POPUP) { clip_popup(); return; }
    if (cmd == CLIP_CMD_COPY) { clip_set_text(); pthread_mutex_lock(&g_clip.lock); g_clip.accelCount++; snprintf(g_clip.status, sizeof(g_clip.status), "%s Copy/Ctrl+C -> clipboard set", via); clip_log_locked(g_clip.status); pthread_mutex_unlock(&g_clip.lock); return; }
    if (cmd == CLIP_CMD_PASTE) { clip_get_text(); pthread_mutex_lock(&g_clip.lock); g_clip.accelCount++; snprintf(g_clip.status, sizeof(g_clip.status), "%s Paste/Ctrl+V -> clipboard read", via); clip_log_locked(g_clip.status); pthread_mutex_unlock(&g_clip.lock); return; }
    if (cmd == CLIP_CMD_NEW) { g_clip.accelCount++; clip_new_doc(); return; }
}

static void clip_hit_test(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8   && cx < 120) { post_self(WM_COMMAND, CLIP_CMD_SET, 0); return; }
        if (cx >= 128 && cx < 240) { post_self(WM_COMMAND, CLIP_CMD_GET, 0); return; }
        if (cx >= 248 && cx < 360) { post_self(WM_COMMAND, CLIP_CMD_CLEAR, 0); return; }
        if (cx >= 368 && cx < 480) { post_self(WM_COMMAND, CLIP_CMD_ATTACH, 0); return; }
        if (cx >= 488 && cx < 600) { post_self(WM_COMMAND, CLIP_CMD_POPUP, 0); return; }
    }
    if (cy >= 34 && cy < 54) {
        if (cx >= 8   && cx < 120) { post_self(WM_COMMAND, CLIP_CMD_COPY, 0); return; }
        if (cx >= 128 && cx < 240) { post_self(WM_COMMAND, CLIP_CMD_PASTE, 0); return; }
        if (cx >= 248 && cx < 360) { post_self(WM_COMMAND, CLIP_CMD_NEW, 0); return; }
    }
}

static LRESULT CALLBACK clipmenu_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        pthread_mutex_lock(&g_clip.lock);
        g_clip.hWnd = hwnd;
        MyAppResizeInit(&g_clip.resize, CLIPMENU_W, CLIPMENU_H, TITLEBAR_H);
        snprintf(g_clip.status, sizeof(g_clip.status), "ClipMenuLab ready: Clipboard + Menus + Accelerators");
        snprintf(g_clip.localText, sizeof(g_clip.localText), "<local buffer>");
        snprintf(g_clip.clipboardText, sizeof(g_clip.clipboardText), "<not read yet>");
        clip_log_locked(g_clip.status);
        pthread_mutex_unlock(&g_clip.lock);
        clip_attach_menu();
        ACCEL a[3];
        a[0].fVirt = FVIRTKEY|FCONTROL; a[0].key = KEY_C; a[0].cmd = CLIP_CMD_COPY;
        a[1].fVirt = FVIRTKEY|FCONTROL; a[1].key = KEY_V; a[1].cmd = CLIP_CMD_PASTE;
        a[2].fVirt = FVIRTKEY|FCONTROL; a[2].key = KEY_N; a[2].cmd = CLIP_CMD_NEW;
        g_clip.hAccel = CreateAcceleratorTableA(a, 3);
        break;
    }
    case WM_LBUTTONDOWN:
        clip_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_KEYDOWN: {
        MSG m;
        memset(&m, 0, sizeof(m));
        m.hwnd = hwnd;
        m.message = WM_KEYDOWN;
        m.wParam = wp;
        m.lParam = lp;
        if (TranslateAcceleratorA(hwnd, g_clip.hAccel, &m)) {
            pthread_mutex_lock(&g_clip.lock);
            snprintf(g_clip.status, sizeof(g_clip.status), "TranslateAcceleratorA key=%lu state=0x%lx -> WM_COMMAND", (unsigned long)wp, (unsigned long)lp);
            clip_log_locked(g_clip.status);
            pthread_mutex_unlock(&g_clip.lock);
        }
        break;
    }
    case WM_COMMAND:
        clip_handle_command((UINT)wp, lp ? "ACCEL/MENU" : "BUTTON/MENU");
        break;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_clip.resize, lp, CLIPMENU_MIN_W, CLIPMENU_MIN_H);
        return 0;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_clip.resize, lp);
        return 0;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_clip.resize, lp, TITLEBAR_H);
        return 0;
    case WM_MOVE:
        MyAppResizeOnMove(&g_clip.resize, lp);
        return 0;
    case WM_SIZE:
        MyAppResizeOnSize(&g_clip.resize, wp, lp);
        return 0;
    case WM_DESTROY:
        if (g_clip.hAccel) { DestroyAcceleratorTable(g_clip.hAccel); g_clip.hAccel = 0; }
        if (g_clip.hMenu) { DestroyMenu(g_clip.hMenu); g_clip.hMenu = 0; }
        if (g_clip.hPopup) { DestroyMenu(g_clip.hPopup); g_clip.hPopup = 0; }
        break;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

HWND clipmenu_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    static int s_lock_init = 0;
    if (!s_lock_init) { pthread_mutex_init(&g_clip.lock, NULL); s_lock_init = 1; }
    pthread_mutex_lock(&g_clip.lock);
    HWNDManager* oldMgr = mgr;
    Capability oldCap = cap;
    memset(&g_clip, 0, sizeof(g_clip));
    g_clip.mgr = oldMgr;
    g_clip.cap = oldCap;
    pthread_mutex_unlock(&g_clip.lock);

    MyWinBindRuntime(mgr, &cap);
    {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = clipmenu_wndproc;
        wc.lpszClassName = "myOS.ClipMenuLab";
        (void)RegisterClassExA(&wc);
    }
    HWND hWnd = CreateWindowExA(WS_EX_NONE, "myOS.ClipMenuLab", "myOS ClipMenuLab",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                x, y, CLIPMENU_W, CLIPMENU_H, 0, 0, 0, NULL);
    g_clip.hWnd = hWnd;
    return hWnd;
}

void clipmenu_destroy(void)
{
    pthread_mutex_lock(&g_clip.lock);
    memset(&g_clip, 0, sizeof(g_clip));
    pthread_mutex_unlock(&g_clip.lock);
}

void clipmenu_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    (void)hwnd;
    int x = wx + 1;
    int y = wy + TITLEBAR_H;
    int w = ww - 2;
    int h = wh - TITLEBAR_H - 1;
    if (w <= 0 || h <= 0) return;

    pthread_mutex_lock(&g_clip.lock);
    ClipMenuLabApp s = g_clip;
    pthread_mutex_unlock(&g_clip.lock);

    fb_rect(fb, x, y, w, h, COLOR(8,8,18));
    button(fb, x+8,   y+8, 112, "Set Clip");
    button(fb, x+128, y+8, 112, "Get Clip");
    button(fb, x+248, y+8, 112, "Clear");
    button(fb, x+368, y+8, 112, "Attach Menu");
    button(fb, x+488, y+8, 112, "Popup");
    button(fb, x+8,   y+34, 112, "Ctrl+C");
    button(fb, x+128, y+34, 112, "Ctrl+V");
    button(fb, x+248, y+34, 112, "Ctrl+N");

    char line[256];
    snprintf(line, sizeof(line), "MENU=0x%x POPUP=0x%x ACCEL=0x%x  set=%u get=%u clear=%u menu=%u popup=%u accel=%u cmd=%u",
             s.hMenu, s.hPopup, s.hAccel, s.setCount, s.getCount, s.clearCount, s.menuCount, s.popupCount, s.accelCount, s.commandCount);
    draw_clip_text(fb, x+8, y+66, line, COLOR(120,255,170), x+8, y+60, w-16, 18);
    snprintf(line, sizeof(line), "Local: %s", s.localText);
    draw_clip_text(fb, x+8, y+86, line, COLOR(210,210,255), x+8, y+80, w-16, 18);
    snprintf(line, sizeof(line), "Clipboard: %s", s.clipboardText);
    draw_clip_text(fb, x+8, y+106, line, COLOR(210,210,255), x+8, y+100, w-16, 18);
    snprintf(line, sizeof(line), "Status: %s", s.status);
    draw_clip_text(fb, x+8, y+126, line, COLOR(255,230,160), x+8, y+120, w-16, 18);

    int log_y = y + 150;
    fb_rect_outline(fb, x+8, log_y, w-16, h - 158, COLOR(70,80,115));
    font_draw_str(fb, x+14, log_y+8, "Clipboard / Menu / Accelerator log", WHITE);
    int yy = log_y + 28;
    for (int i = 0; i < s.logCount && yy < y + h - 12; i++, yy += 15) {
        draw_clip_text(fb, x+14, yy, s.log[i], WHITE, x+12, log_y+24, w-24, h-184);
    }
}
