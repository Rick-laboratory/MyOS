#include "app_servicelab.h"
#include "window.h"
#include "myobject.h"
#include "mycontrols.h"
#include "app_msdn_resize.h"  // PushLogLineA only; controls are real HWND children now
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* AUDIT(v119-lab): ServiceLab is tied to the transitional SCM contract. It
   creates real BUTTON child HWNDs, but service objects are still registry/status
   entries rather than service processes with StartServiceCtrlDispatcherA. Expect
   this lab to break or need rewriting when SCM moves to process-backed services,
   service accounts, dependencies and real SERVICE_STATUS semantics. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif
#ifndef SERVICE_CONTROL_STOP
#define SERVICE_CONTROL_STOP 1u
#endif

#define SVC_LOG_LINES 10
#define SVC_LOG_CHARS 220
#define SVC_MAX_SNAP 16
#define SVC_CLIENT_PAD 8
#define SVC_HIT_SLOP 2

typedef struct SVCBUTTONDEF {
    int x, y, w, h;
    UINT id;
    LPCSTR text;
} SVCBUTTONDEF;

static const SVCBUTTONDEF g_btnDefs[] = {
    {  10,  10,  92, 20, SVC_CMD_OPEN_SCM, "Open SCM"},
    { 108,  10,  92, 20, SVC_CMD_CREATE,   "Create"},
    { 206,  10,  92, 20, SVC_CMD_OPEN,     "Open"},
    { 304,  10,  92, 20, SVC_CMD_START,    "Start"},
    { 402,  10,  92, 20, SVC_CMD_STOP,     "Stop"},
    { 500,  10,  92, 20, SVC_CMD_QUERY,    "Query"},
    { 598,  10,  92, 20, SVC_CMD_REFRESH,  "Refresh"},
    {  10,  36,  92, 20, SVC_CMD_DELETE,   "Delete"},
    { 108,  36,  92, 20, SVC_CMD_CLOSE,    "Close"},
    { 206,  36,  92, 20, SVC_CMD_AUTO,     "AutoSvc"},
};

typedef struct ServiceLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    HWND hButtons[sizeof(g_btnDefs)/sizeof(g_btnDefs[0])];
    Capability cap;
    pthread_mutex_t lock;
    SC_HANDLE hScm;
    SC_HANDLE hSvc;
    int createSeq;
    int commands;
    int selected;
    MyAppResizeState resize;
    MyServiceInfo snap[SVC_MAX_SNAP];
    int snapCount;
    char latestName[48];
    char status[256];
    char log[SVC_LOG_LINES][SVC_LOG_CHARS];
    int logCount;
} ServiceLabApp;

static ServiceLabApp g_svcapp;

void servicelab_destroy(void);

static void log_locked(const char* s)
{
    PushLogLineA(&g_svcapp.log[0][0], SVC_LOG_LINES, SVC_LOG_CHARS, &g_svcapp.logCount, s);
}

static BOOL enum_cb(const MyServiceInfo* info, LPARAM lp)
{
    ServiceLabApp* app = (ServiceLabApp*)lp;
    if (app->snapCount < SVC_MAX_SNAP) app->snap[app->snapCount++] = *info;
    return TRUE;
}

static void refresh_locked(void)
{
    g_svcapp.snapCount = 0;
    MySvcEnumServices(enum_cb, (LPARAM)&g_svcapp);
    if (g_svcapp.selected >= g_svcapp.snapCount) g_svcapp.selected = g_svcapp.snapCount - 1;
    if (g_svcapp.selected < 0 && g_svcapp.snapCount > 0) g_svcapp.selected = 0;
    snprintf(g_svcapp.status, sizeof(g_svcapp.status),
             "SCM=0x%lx SVC=0x%lx services=%u running=%u selected=%d commands=%d",
             (unsigned long)g_svcapp.hScm, (unsigned long)g_svcapp.hSvc,
             MySvcGetCount(), MySvcGetRunningCount(), g_svcapp.selected, g_svcapp.commands);
}

static void ensure_scm_locked(void)
{
    if (!g_svcapp.hScm) {
        g_svcapp.hScm = OpenSCManagerA(NULL, NULL, MYSVC_ACCESS_ALL);
        char b[160]; snprintf(b, sizeof(b), "OpenSCManagerA(...) -> 0x%lx", (unsigned long)g_svcapp.hScm);
        log_locked(b);
    }
}

static void command_locked(int id)
{
    g_svcapp.commands++;
    char b[240];
    switch (id) {
    case SVC_CMD_OPEN_SCM:
        ensure_scm_locked();
        break;
    case SVC_CMD_CREATE: {
        ensure_scm_locked();
        g_svcapp.createSeq++;
        snprintf(g_svcapp.latestName, sizeof(g_svcapp.latestName), "myos.demo.%d", g_svcapp.createSeq);
        g_svcapp.hSvc = CreateServiceA(g_svcapp.hScm, g_svcapp.latestName, "myOS Demo Service",
                                       MYSVC_ACCESS_ALL, 0, MYSVC_START_TYPE_DEMAND, 0,
                                       "\\SystemRoot\\myos\\demo-service", NULL, NULL, NULL, NULL, NULL);
        snprintf(b, sizeof(b), "CreateServiceA('%s') -> 0x%lx", g_svcapp.latestName, (unsigned long)g_svcapp.hSvc);
        log_locked(b);
        break; }
    case SVC_CMD_AUTO: {
        ensure_scm_locked();
        g_svcapp.hSvc = CreateServiceA(g_svcapp.hScm, "myos.autostart", "myOS AutoStart Service",
                                       MYSVC_ACCESS_ALL, 0, MYSVC_START_TYPE_AUTO, 0,
                                       "\\SystemRoot\\myos\\auto-service", NULL, NULL, NULL, NULL, NULL);
        snprintf(g_svcapp.latestName, sizeof(g_svcapp.latestName), "myos.autostart");
        snprintf(b, sizeof(b), "CreateServiceA('%s', AUTO_START) -> 0x%lx", g_svcapp.latestName, (unsigned long)g_svcapp.hSvc);
        log_locked(b);
        break; }
    case SVC_CMD_OPEN:
        ensure_scm_locked();
        if (!g_svcapp.latestName[0]) snprintf(g_svcapp.latestName, sizeof(g_svcapp.latestName), "myos.demo.1");
        g_svcapp.hSvc = OpenServiceA(g_svcapp.hScm, g_svcapp.latestName, MYSVC_ACCESS_QUERY|MYSVC_ACCESS_START|MYSVC_ACCESS_STOP|MYSVC_ACCESS_CHANGE);
        snprintf(b, sizeof(b), "OpenServiceA('%s') -> 0x%lx", g_svcapp.latestName, (unsigned long)g_svcapp.hSvc);
        log_locked(b);
        break;
    case SVC_CMD_START:
        snprintf(b, sizeof(b), "StartServiceA(0x%lx) -> %s", (unsigned long)g_svcapp.hSvc, StartServiceA(g_svcapp.hSvc, 0, NULL) ? "TRUE/RUNNING" : "FALSE");
        log_locked(b);
        break;
    case SVC_CMD_STOP: {
        MyServiceInfo st; memset(&st,0,sizeof(st));
        snprintf(b, sizeof(b), "ControlService(STOP,0x%lx) -> %s", (unsigned long)g_svcapp.hSvc, ControlService(g_svcapp.hSvc, SERVICE_CONTROL_STOP, &st) ? "TRUE/STOPPED" : "FALSE");
        log_locked(b);
        break; }
    case SVC_CMD_QUERY: {
        MyServiceInfo st; memset(&st,0,sizeof(st));
        if (QueryServiceStatus(g_svcapp.hSvc, &st)) snprintf(b, sizeof(b), "QueryServiceStatus -> %s checkpoint=%u flags=0x%lx", MySvcStateName(st.state), st.checkpoint, (unsigned long)st.flags);
        else snprintf(b, sizeof(b), "QueryServiceStatus(0x%lx) -> FALSE", (unsigned long)g_svcapp.hSvc);
        log_locked(b);
        break; }
    case SVC_CMD_DELETE: {
        SC_HANDLE h = g_svcapp.hSvc;
        BOOL ok = DeleteService(h);
        if (ok && h) {
            CloseServiceHandle(h);
            g_svcapp.hSvc = 0;
            snprintf(b, sizeof(b), "DeleteService(0x%lx) -> TRUE; CloseServiceHandle -> purged when last ref closed", (unsigned long)h);
        } else {
            snprintf(b, sizeof(b), "DeleteService(0x%lx) -> FALSE", (unsigned long)h);
        }
        log_locked(b);
        break; }
    case SVC_CMD_CLOSE:
        if (g_svcapp.hSvc) { snprintf(b, sizeof(b), "CloseServiceHandle(service=0x%lx)", (unsigned long)g_svcapp.hSvc); CloseServiceHandle(g_svcapp.hSvc); g_svcapp.hSvc = 0; log_locked(b); }
        else if (g_svcapp.hScm) { snprintf(b, sizeof(b), "CloseServiceHandle(SCM=0x%lx)", (unsigned long)g_svcapp.hScm); CloseServiceHandle(g_svcapp.hScm); g_svcapp.hScm = 0; log_locked(b); }
        else log_locked("CloseServiceHandle -> no open handles");
        break;
    case SVC_CMD_REFRESH:
        log_locked("EnumServicesStatus-lite / Refresh snapshot");
        break;
    }
    refresh_locked();
}

static int __attribute__((unused)) to_client(int sx, int sy, int* cx, int* cy)
{
    MyWindowState st; memset(&st, 0, sizeof(st));
    if (!hwnd_copy_window_state(g_svcapp.mgr, g_svcapp.hWnd, &st)) return 0;
    /* Buttons are painted below an 8px client inset.  v39.1 hit-tested
       against raw client coords, so the visible right/bottom part of each
       button was not clickable.  Keep hit-test coords in the same coordinate
       space as servicelab_blit(). */
    *cx = sx - (int)st.rcWindow.left - SVC_CLIENT_PAD;
    *cy = sy - (int)st.rcWindow.top - TITLEBAR_H - SVC_CLIENT_PAD;
    return 1;
}


static void post_mouse_to_child_if_any(int cx, int cy, UINT msg)
{
    POINT pt; pt.x = cx; pt.y = cy;
    HWND hChild = ChildWindowFromPoint(g_svcapp.hWnd, pt);
    if (!hChild) return;

    /* Win32 semantics: child receives client-relative mouse coordinates.
       The tiny SVC_HIT_SLOP remains only as compatibility forgiveness for the
       framebuffer PoC, not as the app's own button hit-test. */
    for (unsigned i = 0; i < sizeof(g_btnDefs)/sizeof(g_btnDefs[0]); ++i) {
        if (g_svcapp.hButtons[i] == hChild) {
            int lx = cx - g_btnDefs[i].x;
            int ly = cy - g_btnDefs[i].y;
            if (lx >= -SVC_HIT_SLOP && ly >= -SVC_HIT_SLOP && lx < g_btnDefs[i].w + SVC_HIT_SLOP && ly < g_btnDefs[i].h + SVC_HIT_SLOP) {
                PostMessageA(hChild, msg, 0, MAKELPARAM((WORD)lx, (WORD)ly));
            }
            return;
        }
    }
}

static LRESULT CALLBACK servicelab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCREATE:
        return TRUE;
    case WM_CREATE:
        g_svcapp.hWnd = hwnd;
        return 0;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_svcapp.resize, lp, SERVICELAB_MIN_W, SERVICELAB_MIN_H);
        return 0;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_svcapp.resize, lp);
        return 0;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_svcapp.resize, lp, TITLEBAR_H);
        return 0;
    case WM_MOVE:
        MyAppResizeOnMove(&g_svcapp.resize, lp);
        return 0;
    case WM_SIZE:
        MyAppResizeOnSize(&g_svcapp.resize, wp, lp);
        return 0;
    case WM_CLOSE:
        return DefWindowProcA(hwnd, msg, wp, lp);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        int cx = GET_X_LPARAM(lp) - SVC_CLIENT_PAD;
        int cy = GET_Y_LPARAM(lp) - SVC_CLIENT_PAD;
        post_mouse_to_child_if_any(cx, cy, msg);
        if (msg == WM_LBUTTONDOWN) {
            pthread_mutex_lock(&g_svcapp.lock);
            int listX = 330, listY = 76, rowH = 18;
            if (PtInRectXY(cx, cy, listX, listY, 350, 150)) {
                int s = (cy - listY) / rowH;
                if (s >= 0 && s < g_svcapp.snapCount) {
                    g_svcapp.selected = s;
                    snprintf(g_svcapp.latestName, sizeof(g_svcapp.latestName), "%s", g_svcapp.snap[s].name);
                    g_svcapp.hSvc = g_svcapp.snap[s].hService;
                    log_locked("selected service from snapshot");
                    refresh_locked();
                }
            }
            pthread_mutex_unlock(&g_svcapp.lock);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = (int)LOWORD(wp);
        int code = (int)HIWORD(wp);
        if (code == BN_CLICKED) {
            pthread_mutex_lock(&g_svcapp.lock);
            command_locked(id);
            pthread_mutex_unlock(&g_svcapp.lock);
        }
        return 0;
    }
    case WM_DESTROY:
        servicelab_destroy();
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

HWND servicelab_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    memset(&g_svcapp, 0, sizeof(g_svcapp));
    pthread_mutex_init(&g_svcapp.lock, NULL);
    g_svcapp.mgr = mgr;
    g_svcapp.cap = cap;
    g_svcapp.selected = -1;
    MyAppResizeInit(&g_svcapp.resize, SERVICELAB_W, SERVICELAB_H, TITLEBAR_H);
    MySvcInit();
    MyWinBindRuntime(mgr, &cap);

    {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = servicelab_wndproc;
        wc.lpszClassName = "myOS.ServiceLab";
        (void)RegisterClassExA(&wc);
    }

    g_svcapp.hWnd = CreateWindowExA(WS_EX_NONE, "myOS.ServiceLab", "myOS ServiceLab",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                    x, y, SERVICELAB_W, SERVICELAB_H,
                                    0, 0, 0, NULL);
    if (!g_svcapp.hWnd) return 0;

    for (unsigned i = 0; i < sizeof(g_btnDefs)/sizeof(g_btnDefs[0]); ++i) {
        g_svcapp.hButtons[i] = CreateWindowExA(WS_EX_NONE, "BUTTON", g_btnDefs[i].text,
                                              WS_CHILD|WS_VISIBLE,
                                              g_btnDefs[i].x, g_btnDefs[i].y, g_btnDefs[i].w, g_btnDefs[i].h,
                                              g_svcapp.hWnd, (HMENU)(DWORD_PTR)g_btnDefs[i].id, 0, NULL);
    }
    pthread_mutex_lock(&g_svcapp.lock);
    log_locked("WM_CREATE: ServiceLab / SCM-lite ready; BUTTON child HWNDs created via CreateWindowExA");
    refresh_locked();
    pthread_mutex_unlock(&g_svcapp.lock);
    return g_svcapp.hWnd;
}

void servicelab_destroy(void)
{
    pthread_mutex_lock(&g_svcapp.lock);
    if (g_svcapp.hSvc) CloseServiceHandle(g_svcapp.hSvc);
    if (g_svcapp.hScm) CloseServiceHandle(g_svcapp.hScm);
    g_svcapp.hSvc = 0; g_svcapp.hScm = 0;
    pthread_mutex_unlock(&g_svcapp.lock);
}

void servicelab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    (void)hwnd;
    pthread_mutex_lock(&g_svcapp.lock);
    /* v39.1: paint an opaque client background first. Without this the lab looked
       transparent because only widgets/log panels were redrawn. */
    fb_rect(fb, wx, wy + TITLEBAR_H, ww, wh - TITLEBAR_H, COLOR(8, 10, 18));
    fb_rect(fb, wx + 1, wy + TITLEBAR_H + 1, ww - 2, wh - TITLEBAR_H - 2, COLOR(10, 12, 22));
    int ox=wx+SVC_CLIENT_PAD, oy=wy+TITLEBAR_H+SVC_CLIENT_PAD;
    MyDrawChildWindows(g_svcapp.hWnd, fb, ox, oy);
    font_draw_str(fb, ox+10, oy+66, "SCM / Service Control Manager Lite", COLOR(210,230,255));
    font_draw_str(fb, ox+10, oy+86, g_svcapp.status, COLOR(120,255,170));
    char line[256];
    snprintf(line,sizeof(line),"latest='%s' ObjectLab should show SERVICE rows in \\ServicesActive", g_svcapp.latestName[0]?g_svcapp.latestName:"-");
    font_draw_str(fb, ox+10, oy+106, line, COLOR(255,245,160));

    int lx=ox+330, ly=oy+76, lw=360, lh=152;
    fb_rect(fb,lx,ly,lw,lh,COLOR(14,17,26)); fb_rect_outline(fb,lx,ly,lw,lh,COLOR(95,115,150));
    font_draw_str(fb,lx+8,ly-14,"Service snapshot",COLOR(220,220,230));
    for (int i=0;i<g_svcapp.snapCount && i<8;i++) {
        int ry=ly+i*18;
        if (i==g_svcapp.selected) fb_rect(fb,lx+2,ry+1,lw-4,16,COLOR(58,92,150));
        snprintf(line,sizeof(line),"0x%lx %-18s %-8s flags=0x%02lx", (unsigned long)g_svcapp.snap[i].hService, g_svcapp.snap[i].name, MySvcStateName(g_svcapp.snap[i].state), (unsigned long)g_svcapp.snap[i].flags);
        font_draw_str(fb,lx+8,ry+5,line,WHITE);
    }

    int logx=ox+10, logy=oy+244, logw=ww-36, logh=wh-TITLEBAR_H-260;
    if (logh < 80) logh=80;
    fb_rect(fb,logx,logy,logw,logh,COLOR(8,10,18)); fb_rect_outline(fb,logx,logy,logw,logh,COLOR(90,110,145));
    font_draw_str(fb,logx+6,logy+8,"ServiceLab log",WHITE);
    for (int i=0;i<g_svcapp.logCount && i<10;i++) font_draw_str(fb,logx+6,logy+26+i*15,g_svcapp.log[i],COLOR(235,235,235));
    pthread_mutex_unlock(&g_svcapp.lock);
}
