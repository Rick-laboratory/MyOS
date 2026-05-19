#include "app_mdilab.h"
#include "window.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "mycontrols.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

typedef struct MdiLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    HWND hClient;
    HWND hBtnNew;
    HWND hBtnTile;
    HWND hBtnCascade;
    HWND hBtnNext;
    HWND hBtnClose;
    HWND children[16];
    int childCount;
    int seq;
    int commandSeq;
    Capability cap;
    pthread_mutex_t lock;
    char status[192];
} MdiLabApp;

static MdiLabApp g_mdi;
static ATOM g_mdi_frame_atom = 0;
static ATOM g_mdi_child_atom = 0;

static void mdilab_bind(void)
{
    MyWinBindRuntime(g_mdi.mgr, &g_mdi.cap);
}

static void mdilab_set_status(const char* msg)
{
    pthread_mutex_lock(&g_mdi.lock);
    snprintf(g_mdi.status, sizeof(g_mdi.status), "%s", msg ? msg : "");
    pthread_mutex_unlock(&g_mdi.lock);
}

#define MDILAB_TOOLBAR_Y       (APP_MENUBAR_H + 6)
#define MDILAB_TOOLBAR_H       28
#define MDILAB_CLIENT_X        8
#define MDILAB_CLIENT_GAP      8
#define MDILAB_STATUS_H        24

static int mdilab_client_y(void)
{
    return MDILAB_TOOLBAR_Y + MDILAB_TOOLBAR_H + MDILAB_CLIENT_GAP;
}

static void mdilab_layout_controls(int clientW, int clientH)
{
    if (!g_mdi.hWnd) return;
    if (clientW <= 0) clientW = MDILAB_W;
    if (clientH <= 0) clientH = MDILAB_H - TITLEBAR_H;

    int y = MDILAB_TOOLBAR_Y;
    int x = 8;
    int gap = 8;
    if (g_mdi.hBtnNew) MoveWindow(g_mdi.hBtnNew, x, y, 72, MDILAB_TOOLBAR_H, TRUE);
    x += 72 + gap;
    if (g_mdi.hBtnTile) MoveWindow(g_mdi.hBtnTile, x, y, 72, MDILAB_TOOLBAR_H, TRUE);
    x += 72 + gap;
    if (g_mdi.hBtnCascade) MoveWindow(g_mdi.hBtnCascade, x, y, 88, MDILAB_TOOLBAR_H, TRUE);
    x += 88 + gap;
    if (g_mdi.hBtnNext) MoveWindow(g_mdi.hBtnNext, x, y, 72, MDILAB_TOOLBAR_H, TRUE);
    x += 72 + gap;
    if (g_mdi.hBtnClose) MoveWindow(g_mdi.hBtnClose, x, y, 72, MDILAB_TOOLBAR_H, TRUE);

    int clientX = MDILAB_CLIENT_X;
    int clientY = mdilab_client_y();
    int clientW2 = clientW - clientX * 2;
    int clientH2 = clientH - clientY - MDILAB_STATUS_H - 8;
    if (clientW2 < 120) clientW2 = 120;
    if (clientH2 < 80) clientH2 = 80;
    if (g_mdi.hClient)
        MoveWindow(g_mdi.hClient, clientX, clientY, clientW2, clientH2, TRUE);
}

static void mdilab_track_child(HWND hChild)
{
    if (!hChild) return;
    pthread_mutex_lock(&g_mdi.lock);
    if (g_mdi.childCount < (int)(sizeof(g_mdi.children)/sizeof(g_mdi.children[0])))
        g_mdi.children[g_mdi.childCount++] = hChild;
    pthread_mutex_unlock(&g_mdi.lock);
}

static void mdilab_prune_children(void)
{
    pthread_mutex_lock(&g_mdi.lock);
    int w = 0;
    for (int i = 0; i < g_mdi.childCount; ++i) {
        if (g_mdi.children[i] && IsWindow(g_mdi.children[i]))
            g_mdi.children[w++] = g_mdi.children[i];
    }
    g_mdi.childCount = w;
    pthread_mutex_unlock(&g_mdi.lock);
}

static HWND mdilab_create_child(const char* prefix)
{
    if (!g_mdi.hClient) return 0;
    mdilab_prune_children();
    if (g_mdi.childCount >= (int)(sizeof(g_mdi.children)/sizeof(g_mdi.children[0]))) {
        mdilab_set_status("MDILab child limit reached for this visual lab");
        return 0;
    }
    char title[64];
    snprintf(title, sizeof(title), "%s %d", prefix ? prefix : "Child", ++g_mdi.seq);
    MDICREATESTRUCTA mcs;
    memset(&mcs, 0, sizeof(mcs));
    mcs.szClass = "myOS.MDILabChild";
    mcs.szTitle = title;
    mcs.x = CW_USEDEFAULT;
    mcs.y = CW_USEDEFAULT;
    mcs.cx = 230;
    mcs.cy = 150;
    mcs.style = WS_OVERLAPPEDWINDOW;
    mcs.lParam = (LPARAM)g_mdi.seq;
    HWND hChild = (HWND)SendMessageA(g_mdi.hClient, WM_MDICREATE, 0, (LPARAM)&mcs);
    mdilab_track_child(hChild);
    snprintf(g_mdi.status, sizeof(g_mdi.status), "WM_MDICREATE -> hwnd=0x%x title='%s'", (unsigned)hChild, title);
    return hChild;
}

static LRESULT CALLBACK mdilab_child_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_COMMAND:
        return 0x4D44; /* 'MD' canary for DefFrameProc routing. */
    case WM_CLOSE:
        return DefMDIChildProcA(hwnd, msg, wp, lp);
    case WM_DESTROY:
        mdilab_prune_children();
        return 0;
    default:
        return DefMDIChildProcA(hwnd, msg, wp, lp);
    }
}

static const char* mdilab_command_origin(WPARAM wp, LPARAM lp)
{
    UINT code = HIWORD(wp);
    HWND sender = (HWND)lp;
    if (sender == g_mdi.hBtnNew || sender == g_mdi.hBtnTile || sender == g_mdi.hBtnCascade ||
        sender == g_mdi.hBtnNext || sender == g_mdi.hBtnClose) return "toolbar";
    if (sender) return "control";
    if (code == 0) return "menu";
    if (code == 1) return "accelerator";
    return "synthetic";
}

static int mdilab_note_command(UINT id, WPARAM wp, LPARAM lp)
{
    int seq = ++g_mdi.commandSeq;
    char msg[192];
    snprintf(msg, sizeof(msg), "WM_COMMAND id=%u origin=%s code=%u sender=0x%x seq=%d",
             (unsigned)id, mdilab_command_origin(wp, lp), (unsigned)HIWORD(wp),
             (unsigned)(HWND)lp, seq);
    mdilab_set_status(msg);
    return seq;
}

static LRESULT CALLBACK mdilab_frame_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_mdi.hWnd = hwnd;
        snprintf(g_mdi.status, sizeof(g_mdi.status), "MDILab ready: MDICLIENT/WM_MDITILE/WM_MDICASCADE/WM_MDISETMENU");
        return 0;
    case WM_SIZE:
        mdilab_layout_controls((int)LOWORD(lp), (int)HIWORD(lp));
        return 0;
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        UINT code = HIWORD(wp);
        HWND sender = (HWND)lp;
        int cmdSeq = mdilab_note_command(id, wp, lp);

        /* v139: Win32 controls send more WM_COMMAND notifications than the
           actual click.  BUTTON focus emits BN_SETFOCUS/BN_KILLFOCUS with the
           same LOWORD(id).  Treat toolbar HWNDs as actions only on BN_CLICKED;
           menu and accelerator commands have lParam == NULL and remain accepted.
           This is the real source of the manual "New creates two children" and
           "Next skips every second child" symptom: focus notification first,
           click notification second. */
        if (sender && sender != g_mdi.hBtnNew && sender != g_mdi.hBtnTile &&
            sender != g_mdi.hBtnCascade && sender != g_mdi.hBtnNext &&
            sender != g_mdi.hBtnClose) {
            return DefFrameProcA(hwnd, g_mdi.hClient, msg, wp, lp);
        }
        if (sender && code != BN_CLICKED) return 0;

        if (id == MDILAB_CMD_NEW) {
            HWND created = mdilab_create_child("Child");
            char msg[192];
            snprintf(msg, sizeof(msg), "WM_COMMAND New origin=%s seq=%d -> WM_MDICREATE hwnd=0x%x",
                     mdilab_command_origin(wp, lp), cmdSeq, (unsigned)created);
            mdilab_set_status(msg);
            return 0;
        }
        if (id == MDILAB_CMD_TILE) {
            SendMessageA(g_mdi.hClient, WM_MDITILE, MDITILE_VERTICAL, 0);
            return 0;
        }
        if (id == MDILAB_CMD_CASCADE) {
            SendMessageA(g_mdi.hClient, WM_MDICASCADE, 0, 0);
            return 0;
        }
        if (id == MDILAB_CMD_NEXT) {
            SendMessageA(g_mdi.hClient, WM_MDINEXT, 0, FALSE);
            return 0;
        }
        if (id == MDILAB_CMD_CLOSE) {
            HWND active = (HWND)SendMessageA(g_mdi.hClient, WM_MDIGETACTIVE, 0, 0);
            if (active) SendMessageA(active, WM_CLOSE, 0, 0);
            return 0;
        }
        return DefFrameProcA(hwnd, g_mdi.hClient, msg, wp, lp);
    }
    case WM_SYSCOMMAND:
        return DefFrameProcA(hwnd, g_mdi.hClient, msg, wp, lp);
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_mdi.hWnd = 0;
        g_mdi.hClient = 0;
        g_mdi.childCount = 0;
        return 0;
    default:
        return DefFrameProcA(hwnd, g_mdi.hClient, msg, wp, lp);
    }
}

static void mdilab_register_classes(void)
{
    {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = mdilab_frame_wndproc;
        wc.lpszClassName = "myOS.MDILab";
        g_mdi_frame_atom = RegisterClassExA(&wc);
    }
    {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = mdilab_child_wndproc;
        wc.lpszClassName = "myOS.MDILabChild";
        g_mdi_child_atom = RegisterClassExA(&wc);
    }
}

HWND mdilab_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    memset(&g_mdi, 0, sizeof(g_mdi));
    pthread_mutex_init(&g_mdi.lock, NULL);
    g_mdi.mgr = mgr;
    g_mdi.cap = cap;
    mdilab_bind();
    mdilab_register_classes();

    HWND hFrame = CreateWindowExA(0, "myOS.MDILab", "myOS MDILab",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  x, y, MDILAB_W, MDILAB_H, 0, 0, 0, NULL);
    g_mdi.hWnd = hFrame;
    if (!hFrame) return 0;

    g_mdi.hBtnNew     = CreateWindowExA(0, "BUTTON", "New",     WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 8, MDILAB_TOOLBAR_Y, 72, MDILAB_TOOLBAR_H, hFrame, (HMENU)MDILAB_CMD_NEW,     0, NULL);
    g_mdi.hBtnTile    = CreateWindowExA(0, "BUTTON", "Tile",    WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 88, MDILAB_TOOLBAR_Y, 72, MDILAB_TOOLBAR_H, hFrame, (HMENU)MDILAB_CMD_TILE,    0, NULL);
    g_mdi.hBtnCascade = CreateWindowExA(0, "BUTTON", "Cascade", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 168, MDILAB_TOOLBAR_Y, 88, MDILAB_TOOLBAR_H, hFrame, (HMENU)MDILAB_CMD_CASCADE, 0, NULL);
    g_mdi.hBtnNext    = CreateWindowExA(0, "BUTTON", "Next",    WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 264, MDILAB_TOOLBAR_Y, 72, MDILAB_TOOLBAR_H, hFrame, (HMENU)MDILAB_CMD_NEXT,    0, NULL);
    g_mdi.hBtnClose   = CreateWindowExA(0, "BUTTON", "Close",   WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 344, MDILAB_TOOLBAR_Y, 72, MDILAB_TOOLBAR_H, hFrame, (HMENU)MDILAB_CMD_CLOSE,   0, NULL);

    HMENU frameMenu = CreateMenu();
    HMENU windowMenu = CreatePopupMenu();
    AppendMenuA(frameMenu, MF_STRING, MDILAB_CMD_NEW, "&New child");
    AppendMenuA(frameMenu, MF_STRING, MDILAB_CMD_TILE, "&Tile");
    AppendMenuA(frameMenu, MF_STRING, MDILAB_CMD_CASCADE, "&Cascade");
    AppendMenuA(frameMenu, MF_POPUP, (UINT_PTR)windowMenu, "&Window");
    SetMenu(hFrame, frameMenu);

    CLIENTCREATESTRUCT ccs;
    memset(&ccs, 0, sizeof(ccs));
    ccs.hWindowMenu = windowMenu;
    ccs.idFirstChild = 0xB100u;
    g_mdi.hClient = CreateWindowExA(0, "MDICLIENT", "", WS_CHILD|WS_VISIBLE|MDIS_ALLCHILDSTYLES,
                                    MDILAB_CLIENT_X, mdilab_client_y(), MDILAB_W - MDILAB_CLIENT_X * 2,
                                    MDILAB_H - TITLEBAR_H - mdilab_client_y() - MDILAB_STATUS_H - 8,
                                    hFrame, (HMENU)0xA240u, 0, &ccs);
    mdilab_layout_controls(MDILAB_W, MDILAB_H - TITLEBAR_H);
    SendMessageA(g_mdi.hClient, WM_MDISETMENU, (WPARAM)frameMenu, (LPARAM)windowMenu);
    mdilab_create_child("Child");
    mdilab_create_child("Child");
    SendMessageA(g_mdi.hClient, WM_MDICASCADE, 0, 0);
    return hFrame;
}

void mdilab_destroy(void)
{
    if (g_mdi.hWnd && IsWindow(g_mdi.hWnd)) DestroyWindow(g_mdi.hWnd);
    memset(&g_mdi, 0, sizeof(g_mdi));
}

void mdilab_blit(HWND hwnd, int x, int y, int w, int h, Framebuffer* fb)
{
    if (!fb) return;
    if (hwnd && g_mdi.hWnd != hwnd) g_mdi.hWnd = hwnd;
    int cx = x + 1;
    int cy = y + TITLEBAR_H;
    int cw = w - 2;
    int ch = h - TITLEBAR_H - 1;
    if (cw < 120 || ch < 100) return;

    fb_rect(fb, cx, cy, cw, ch, COLOR(11,13,22));
    fb_rect_outline(fb, cx, cy, cw, ch, COLOR(70,80,115));

    /* v134: the compositor draws the HMENU bar after app content, so MDILab
       must reserve APP_MENUBAR_H at the top of its client area.  The toolbar is
       real BUTTON child HWNDs only; do not hand-paint duplicate fake buttons
       underneath the menu bar. */
    int toolbarY = cy + MDILAB_TOOLBAR_Y - 3;
    fb_rect(fb, cx + 6, toolbarY, cw - 12, MDILAB_TOOLBAR_H + 6, COLOR(18,22,34));
    fb_rect_outline(fb, cx + 6, toolbarY, cw - 12, MDILAB_TOOLBAR_H + 6, COLOR(58,68,96));

    int clientX = cx + MDILAB_CLIENT_X;
    int clientY = cy + mdilab_client_y();
    int clientW = cw - MDILAB_CLIENT_X * 2;
    int clientH = ch - mdilab_client_y() - MDILAB_STATUS_H - 8;
    if (clientW < 120) clientW = 120;
    if (clientH < 80) clientH = 80;
    fb_rect(fb, clientX, clientY, clientW, clientH, COLOR(20,25,38));
    fb_rect_outline(fb, clientX, clientY, clientW, clientH, COLOR(100,115,155));

    pthread_mutex_lock(&g_mdi.lock);
    char status[192];
    snprintf(status, sizeof(status), "%s", g_mdi.status);
    pthread_mutex_unlock(&g_mdi.lock);

    BOOL enteredRenderContext = FALSE;
    if (g_mdi.cap.id)
        enteredRenderContext = MyWinEnterProcessContext((DWORD)g_mdi.cap.id);

    HWND activeChild = 0;
    if (g_mdi.hClient && IsWindow(g_mdi.hClient))
        activeChild = (HWND)SendMessageA(g_mdi.hClient, WM_MDIGETACTIVE, 0, 0);

    /* v142: render MDI children in USER32 child Z-order.  Activation raises a
       child in the MDICLIENT stack; drawing the old creation list made clicked
       background children look selected but remain visually behind siblings. */
    HWND zChildren[16];
    int zCount = 0;
    if (g_mdi.hClient && IsWindow(g_mdi.hClient)) {
        HWND top = GetWindow(g_mdi.hClient, GW_CHILD);
        while (top && zCount < 16) {
            zChildren[zCount++] = top;
            top = GetWindow(top, GW_HWNDNEXT);
        }
    }
    for (int zi = zCount - 1; zi >= 0; --zi) {
        HWND child = zChildren[zi];
        if (!child || !IsWindow(child)) continue;
        RECT rc;
        if (!GetWindowRect(child, &rc)) continue;
        RECT crc;
        if (!GetWindowRect(g_mdi.hClient, &crc)) continue;
        int rx = clientX + (int)(rc.left - crc.left);
        int ry = clientY + (int)(rc.top - crc.top);
        int rw = (int)(rc.right - rc.left);
        int rh = (int)(rc.bottom - rc.top);
        if (rw < 40) rw = 40;
        if (rh < 40) rh = 40;
        int active = (child == activeChild);
        fb_rect(fb, rx, ry, rw, rh, active ? COLOR(38,48,76) : COLOR(34,42,64));
        fb_rect_outline(fb, rx, ry, rw, rh, active ? COLOR(255,230,120) : COLOR(150,170,225));
        fb_rect(fb, rx, ry, rw, 20, active ? COLOR(76,92,140) : COLOR(60,74,108));
        char title[64];
        snprintf(title, sizeof(title), "%sMDI child z%d hwnd=0x%x", active ? "* " : "", zCount - zi, (unsigned)child);
        DrawClipTextA(fb, rx + 6, ry + 6, title, WHITE, rx + 2, ry + 2, rw - 4, 18);
    }

    MyDrawChildWindows(g_mdi.hWnd, fb, cx, cy);
    if (enteredRenderContext) MyWinLeaveProcessContext();
    DrawClipTextA(fb, cx + 10, cy + ch - 24, status, COLOR(185,220,255), cx + 8, cy + ch - 28, cw - 16, 18);
}
