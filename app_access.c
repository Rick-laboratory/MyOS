#include "app_access.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "window.h"
#include <string.h>
#include <stdio.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): AccessLab is expected to fail loudly as SecurityDescriptor,
   token and ACL behavior becomes real. Current access checks are capability/
   flag based, not full SID/DACL AccessCheck. Keep failures here categorized as
   security-contract TODOs unless they crash the desktop or corrupt handles. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define ACC_CMD_PROBE     0x0A01u
#define ACC_CMD_SUBSCRIBE 0x0A02u
#define ACC_CMD_CONTROL   0x0A03u

typedef struct AccessApp {
    HWND hWnd;
    Capability cap;
    HWND target;
    int processCount;
    int windowCount;
    int signalCount;
    MyAppResizeState resize;
    int lastEnumOk;
    int lastProcOk;
    int lastReadOk;
    int lastSubOk;
    int lastControlOk;
    char status[128];
    char log[8][96];
    int logCount;
} AccessApp;

static AccessApp g_acc;

static void log_line(const char* s)
{
    if (!s) return;
    if (g_acc.logCount < 8) {
        snprintf(g_acc.log[g_acc.logCount++], sizeof(g_acc.log[0]), "%s", s);
    } else {
        for (int i = 1; i < 8; i++) snprintf(g_acc.log[i-1], sizeof(g_acc.log[0]), "%s", g_acc.log[i]);
        snprintf(g_acc.log[7], sizeof(g_acc.log[0]), "%s", s);
    }
}


static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,55,75));
    fb_rect_outline(fb, x, y, w, 20, COLOR(115,135,170));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}

static BOOL CALLBACK enum_win_proc(HWND hWnd, LPARAM lParam)
{
    (void)lParam;
    if (hWnd && hWnd != g_acc.hWnd && !g_acc.target) g_acc.target = hWnd;
    g_acc.windowCount++;
    return TRUE;
}

static BOOL enum_proc_proc(const MyProcessInfo* lpInfo, LPARAM lParam)
{
    (void)lParam;
    if (!lpInfo) return FALSE;
    g_acc.processCount++;
    return TRUE;
}

static void refresh_probe(void)
{
    HWNDManager* mgr = MyWinGetHwndManager();
    if (mgr) MyWinBindRuntime(mgr, &g_acc.cap);
    g_acc.target = 0;
    g_acc.windowCount = 0;
    g_acc.processCount = 0;
    g_acc.lastEnumOk = EnumWindows(enum_win_proc, 0);
    g_acc.lastProcOk = MyEnumProcesses(enum_proc_proc, 0);

    char line[128];
    snprintf(line, sizeof(line), "EnumWindows=%s windows=%d | MyEnumProcesses=%s procs=%d",
             g_acc.lastEnumOk ? "OK" : "DENIED", g_acc.windowCount,
             g_acc.lastProcOk ? "OK" : "DENIED", g_acc.processCount);
    log_line(line);

    g_acc.lastReadOk = 0;
    if (g_acc.target) {
        MyWindowState st;
        memset(&st, 0, sizeof(st));
        st.cbSize = sizeof(st);
        g_acc.lastReadOk = MyGetWindowState(g_acc.target, &st);
        snprintf(line, sizeof(line), "Read target HWND=%u -> %s title='%s'", g_acc.target,
                 g_acc.lastReadOk ? "OK" : "DENIED", g_acc.lastReadOk ? st.szTitle : "");
        log_line(line);
    }
    snprintf(g_acc.status, sizeof(g_acc.status), "Probe done: target HWND=%u", g_acc.target);
}

static void subscribe_probe(void)
{
    HWNDManager* mgr = MyWinGetHwndManager();
    if (mgr) MyWinBindRuntime(mgr, &g_acc.cap);
    if (!g_acc.target) refresh_probe();
    g_acc.lastSubOk = 0;
    if (g_acc.target) g_acc.lastSubOk = MySubscribeWindowMessage(g_acc.target, g_acc.hWnd, 0, 0);
    char line[96];
    snprintf(line, sizeof(line), "Subscribe target HWND=%u -> %s", g_acc.target, g_acc.lastSubOk ? "OK" : "DENIED");
    log_line(line);
    snprintf(g_acc.status, sizeof(g_acc.status), "%s", line);
}

static void control_probe(void)
{
    HWNDManager* mgr = MyWinGetHwndManager();
    if (mgr) MyWinBindRuntime(mgr, &g_acc.cap);
    if (!g_acc.target) refresh_probe();
    g_acc.lastControlOk = 0;
    if (g_acc.target) g_acc.lastControlOk = SetWindowTextA(g_acc.target, "[AccessLab touched]");
    char line[96];
    snprintf(line, sizeof(line), "SetWindowText target HWND=%u -> %s", g_acc.target, g_acc.lastControlOk ? "OK" : "DENIED");
    log_line(line);
    snprintf(g_acc.status, sizeof(g_acc.status), "%s", line);
}

static void access_post_command(UINT cmd)
{
    HWNDManager* mgr = MyWinGetHwndManager();
    if (mgr && g_acc.hWnd) {
        MyWinBindRuntime(mgr, &g_acc.cap);
        hwnd_post(mgr, &g_acc.cap, g_acc.hWnd, WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
    }
}

static void access_select_from_point(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8 && cx < 88) { access_post_command(ACC_CMD_PROBE); return; }
        if (cx >= 96 && cx < 206) { access_post_command(ACC_CMD_SUBSCRIBE); return; }
        if (cx >= 214 && cx < 324) { access_post_command(ACC_CMD_CONTROL); return; }
    }
}

static void access_handle_command(UINT cmd)
{
    switch (cmd) {
    case ACC_CMD_PROBE: refresh_probe(); break;
    case ACC_CMD_SUBSCRIBE: subscribe_probe(); break;
    case ACC_CMD_CONTROL: control_probe(); break;
    default: break;
    }
}

static LRESULT CALLBACK access_winproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_CREATE:
        g_acc.hWnd = hWnd;
        MyAppResizeInit(&g_acc.resize, ACCESS_W, ACCESS_H, TITLEBAR_H);
        snprintf(g_acc.status, sizeof(g_acc.status), "AccessLab ready");
        return 0;
    case WM_LBUTTONDOWN:
        access_select_from_point(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_COMMAND:
        access_handle_command((UINT)LOWORD(wParam));
        return 0;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_acc.resize, lParam, ACCESS_MIN_W, ACCESS_MIN_H);
        return 0;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_acc.resize, lParam);
        return 0;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_acc.resize, lParam, TITLEBAR_H);
        g_acc.signalCount++;
        snprintf(g_acc.status, sizeof(g_acc.status), "WM_WINDOWPOSCHANGED #%d client=%dx%d flags=0x%x",
                 g_acc.signalCount, g_acc.resize.clientW, g_acc.resize.clientH, g_acc.resize.lastPosFlags);
        return 0;
    case WM_MOVE:
        MyAppResizeOnMove(&g_acc.resize, lParam);
        return 0;
    case WM_SIZE:
        MyAppResizeOnSize(&g_acc.resize, wParam, lParam);
        g_acc.signalCount++;
        snprintf(g_acc.status, sizeof(g_acc.status), "WM_SIZE #%d type=%u client=%dx%d",
                 g_acc.signalCount, (unsigned)wParam, g_acc.resize.clientW, g_acc.resize.clientH);
        return 0;
    case WM_ACTIVATE:
    case WM_SHOWWINDOW:
    case WM_WINDOWTEXTCHANGED:
    case WM_DESTROY:
        g_acc.signalCount++;
        snprintf(g_acc.status, sizeof(g_acc.status), "Signal #%d msg=0x%04x source=%u ver=%ld",
                 g_acc.signalCount, Msg, (unsigned)wParam, (long)lParam);
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static ATOM access_register_class(void)
{
    static ATOM s_atom = 0;
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = access_winproc;
    wc.lpszClassName = "myOS.AccessLab";
    s_atom = RegisterClassExA(&wc);
    return s_atom;
}

HWND access_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    memset(&g_acc, 0, sizeof(g_acc));
    g_acc.cap = cap;
    snprintf(g_acc.status, sizeof(g_acc.status), "Starting AccessLab");
    MyWinBindRuntime(mgr, &cap);
    access_register_class();
    HWND hWnd = CreateWindowExA(WS_EX_NONE, "myOS.AccessLab", "myOS AccessLab",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                x, y, ACCESS_W, ACCESS_H, 0, 0, 0, NULL);
    g_acc.hWnd = hWnd;
    refresh_probe();
    return hWnd;
}

void access_blit(HWND hwnd, int x, int y, int w, int h, Framebuffer* fb)
{
    if (!fb) return;
    if (hwnd && g_acc.hWnd != hwnd) g_acc.hWnd = hwnd;
    int cx = x + 1;
    int cy = y + TITLEBAR_H;
    int cw = w - 2;
    int ch = h - TITLEBAR_H - 1;
    if (cw < 80 || ch < 60) return;

    fb_rect(fb, cx, cy, cw, ch, COLOR(10,10,16));
    fb_rect_outline(fb, cx, cy, cw, ch, COLOR(65,65,95));
    button(fb, cx + 8, cy + 8, 80, "Probe");
    button(fb, cx + 96, cy + 8, 110, "Subscribe");
    button(fb, cx + 214, cy + 8, 110, "Control");

    char caps[180];
    snprintf(caps, sizeof(caps), "PID=%u cap=0x%05x HWND=%u target=%u", g_acc.cap.id, g_acc.cap.flags, g_acc.hWnd, g_acc.target);
    draw_clip_text(fb, cx + 8, cy + 42, caps, COLOR(150,255,170), cx + 8, cy + 38, cw - 16, 12);

    char res[220];
    snprintf(res, sizeof(res), "ENUM=%s PROC=%s READ=%s SUB=%s CONTROL=%s signals=%d",
             g_acc.lastEnumOk ? "OK" : "DENY",
             g_acc.lastProcOk ? "OK" : "DENY",
             g_acc.lastReadOk ? "OK" : "DENY",
             g_acc.lastSubOk ? "OK" : "DENY",
             g_acc.lastControlOk ? "OK" : "DENY",
             g_acc.signalCount);
    draw_clip_text(fb, cx + 8, cy + 58, res, COLOR(220,220,240), cx + 8, cy + 54, cw - 16, 12);

    fb_rect(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(18,18,28));
    fb_rect_outline(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(65,65,95));
    int yline = cy + 86;
    int start = g_acc.logCount > 7 ? g_acc.logCount - 7 : 0;
    for (int i = start; i < g_acc.logCount; i++) {
        draw_clip_text(fb, cx + 14, yline, g_acc.log[i], COLOR(210,210,225), cx + 10, cy + 80, cw - 20, ch - 116);
        yline += 14;
    }

    draw_clip_text(fb, cx + 14, cy + ch - 24, g_acc.status, COLOR(180,255,190), cx + 10, cy + ch - 28, cw - 20, 14);
}
