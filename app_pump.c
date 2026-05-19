#include "app_pump.h"
#include "window.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): PumpLab is deliberately sensitive to message-queue changes.
   It has its own pump thread and uses direct hwnd_post() for stress/timer tests.
   A future strict per-thread queue model may make this fail before normal apps
   do. If it stalls, check UIQ owner tid, GetMessage filtering, WM_QUIT handling
   and whether cross-thread PostMessage is still legal for the target HWND. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define PMPLAB_CMD_POST_SELF 0x0B01u
#define PMPLAB_CMD_STRESS    0x0B02u
#define PMPLAB_CMD_HANG      0x0B03u
#define PMPLAB_CMD_TIMER     0x0B04u

// v20: Eine normale App mit eigener UI-Pump-ThreadQueue.
typedef struct PumpLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    Capability cap;
    pthread_t thread;
    pthread_mutex_t lock;
    int threadStarted;
    int running;
    int timerEnabled;
    uint64_t timerCount;
    uint64_t selfPosts;
    uint64_t stressPosts;
    uint64_t dispatchCount;
    uint64_t threadMessages;
    UINT lastMsg;
    MyAppResizeState resize;
    DWORD lastQueueDepth;
    DWORD tid;
    char status[128];
    char log[8][96];
    int logCount;
} PumpLabApp;

static PumpLabApp g_pump;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void pump_log_locked(const char* s)
{
    if (!s) return;
    if (g_pump.logCount < 8) {
        snprintf(g_pump.log[g_pump.logCount++], sizeof(g_pump.log[0]), "%s", s);
    } else {
        for (int i = 1; i < 8; i++) snprintf(g_pump.log[i-1], sizeof(g_pump.log[0]), "%s", g_pump.log[i]);
        snprintf(g_pump.log[7], sizeof(g_pump.log[0]), "%s", s);
    }
}


static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,55,75));
    fb_rect_outline(fb, x, y, w, 20, COLOR(115,135,170));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}

static void post_self(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_pump.mgr && g_pump.hWnd)
        hwnd_post(g_pump.mgr, &g_pump.cap, g_pump.hWnd, msg, wp, lp);
}

static void pump_post_command(UINT cmd)
{
    post_self(WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
}

static void pump_hit_test(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8 && cx < 96) { pump_post_command(PMPLAB_CMD_POST_SELF); return; }
        if (cx >= 104 && cx < 214) { pump_post_command(PMPLAB_CMD_STRESS); return; }
        if (cx >= 222 && cx < 316) { pump_post_command(PMPLAB_CMD_HANG); return; }
        if (cx >= 324 && cx < 426) { pump_post_command(PMPLAB_CMD_TIMER); return; }
    }
}

static void pump_handle_ui_command(UINT cmd)
{
    switch (cmd) {
    case PMPLAB_CMD_POST_SELF:
        post_self(PMPLAB_POST_SELF, 1, 0);
        pthread_mutex_lock(&g_pump.lock);
        snprintf(g_pump.status, sizeof(g_pump.status), "WM_COMMAND -> Post Self queued");
        pthread_mutex_unlock(&g_pump.lock);
        return;
    case PMPLAB_CMD_STRESS:
        for (int i = 0; i < 1000; i++) post_self(PMPLAB_STRESS, (WPARAM)i, 0);
        pthread_mutex_lock(&g_pump.lock);
        g_pump.stressPosts += 1000;
        snprintf(g_pump.status, sizeof(g_pump.status), "WM_COMMAND -> Stress 1000 queued");
        pump_log_locked("WM_COMMAND Stress 1000 -> queued into own UI thread");
        pthread_mutex_unlock(&g_pump.lock);
        return;
    case PMPLAB_CMD_HANG:
        post_self(PMPLAB_POST_SELF, 2, 0);
        pthread_mutex_lock(&g_pump.lock);
        snprintf(g_pump.status, sizeof(g_pump.status), "WM_COMMAND -> Hang 2s command queued");
        pthread_mutex_unlock(&g_pump.lock);
        return;
    case PMPLAB_CMD_TIMER:
        pthread_mutex_lock(&g_pump.lock);
        g_pump.timerEnabled = !g_pump.timerEnabled;
        snprintf(g_pump.status, sizeof(g_pump.status), "WM_COMMAND -> Timer %s", g_pump.timerEnabled ? "started" : "stopped");
        pump_log_locked(g_pump.timerEnabled ? "Timer started" : "Timer stopped");
        pthread_mutex_unlock(&g_pump.lock);
        return;
    default:
        return;
    }
}

static void pump_handle_command(WPARAM wParam)
{
    if (wParam == 2) {
        pthread_mutex_lock(&g_pump.lock);
        snprintf(g_pump.status, sizeof(g_pump.status), "UI thread intentionally hanging for 2s...");
        pump_log_locked("Hang 2s begin: Desktop should stay responsive");
        pthread_mutex_unlock(&g_pump.lock);
        sleep(2);
        pthread_mutex_lock(&g_pump.lock);
        snprintf(g_pump.status, sizeof(g_pump.status), "Recovered from 2s hang");
        pump_log_locked("Hang 2s end: pump recovered");
        pthread_mutex_unlock(&g_pump.lock);
        return;
    }

    pthread_mutex_lock(&g_pump.lock);
    g_pump.selfPosts++;
    snprintf(g_pump.status, sizeof(g_pump.status), "Self message handled #%llu", (unsigned long long)g_pump.selfPosts);
    pthread_mutex_unlock(&g_pump.lock);
}

static void pump_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, void* userdata)
{
    (void)userdata;
    pthread_mutex_lock(&g_pump.lock);
    g_pump.dispatchCount++;
    g_pump.lastMsg = msg;
    pthread_mutex_unlock(&g_pump.lock);

    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_pump.lock);
        g_pump.hWnd = hwnd;
        MyAppResizeInit(&g_pump.resize, PUMP_W, PUMP_H, TITLEBAR_H);
        snprintf(g_pump.status, sizeof(g_pump.status), "PumpLab HWND=%u created", hwnd);
        pump_log_locked("WM_CREATE: app owns a self-pumped queue");
        pthread_mutex_unlock(&g_pump.lock);
        break;
    case WM_LBUTTONDOWN:
        pump_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_COMMAND:
        pump_handle_ui_command((UINT)LOWORD(wp));
        break;
    case WM_TIMER:
        pthread_mutex_lock(&g_pump.lock);
        g_pump.timerCount++;
        snprintf(g_pump.status, sizeof(g_pump.status), "WM_TIMER #%llu", (unsigned long long)g_pump.timerCount);
        pthread_mutex_unlock(&g_pump.lock);
        break;
    case PMPLAB_POST_SELF:
        pump_handle_command(wp);
        break;
    case PMPLAB_STRESS:
        pthread_mutex_lock(&g_pump.lock);
        // cheap work, intentionally not logging every item
        if ((wp % 250) == 0) snprintf(g_pump.status, sizeof(g_pump.status), "Stress handling item %ld", (long)wp);
        pthread_mutex_unlock(&g_pump.lock);
        break;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_pump.resize, lp, PUMP_MIN_W, PUMP_MIN_H);
        break;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_pump.resize, lp);
        break;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_pump.resize, lp, TITLEBAR_H);
        break;
    case WM_MOVE:
        MyAppResizeOnMove(&g_pump.resize, lp);
        break;
    case WM_SIZE:
        MyAppResizeOnSize(&g_pump.resize, wp, lp);
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        pthread_mutex_lock(&g_pump.lock);
        g_pump.running = 0;
        snprintf(g_pump.status, sizeof(g_pump.status), "WM_DESTROY -> pump stops");
        pthread_mutex_unlock(&g_pump.lock);
        break;
    default:
        break;
    }
}

static void* pump_thread_main(void* arg)
{
    (void)arg;
    uint64_t nextTimer = now_ms() + 250;

    pthread_mutex_lock(&g_pump.lock);
    g_pump.tid = g_pump.cap.id;
    pump_log_locked("Pump thread online: GetMessage loop active");
    pthread_mutex_unlock(&g_pump.lock);

    while (1) {
        pthread_mutex_lock(&g_pump.lock);
        int running = g_pump.running;
        int timerEnabled = g_pump.timerEnabled;
        pthread_mutex_unlock(&g_pump.lock);
        if (!running) break;

        uint64_t t = now_ms();
        if (timerEnabled && t >= nextTimer) {
            post_self(WM_TIMER, 0, 0);
            nextTimer = t + 250;
        }

        MyMessage msg;
        if (hwnd_get_thread_message_wait(g_pump.mgr, g_pump.cap.id, g_pump.cap.id, 0, 0, 0, 1, 50, &msg)) {
            if (msg.hwnd == 0) {
                pthread_mutex_lock(&g_pump.lock);
                g_pump.threadMessages++;
                g_pump.lastMsg = msg.msg;
                pthread_mutex_unlock(&g_pump.lock);
            } else {
                hwnd_dispatch_message(g_pump.mgr, &msg);
            }
        }

        pthread_mutex_lock(&g_pump.lock);
        g_pump.lastQueueDepth = hwnd_get_thread_queue_status(g_pump.mgr, g_pump.cap.id, g_pump.cap.id);
        pthread_mutex_unlock(&g_pump.lock);
    }

    pthread_mutex_lock(&g_pump.lock);
    pump_log_locked("Pump thread stopped");
    pthread_mutex_unlock(&g_pump.lock);
    return NULL;
}

HWND pump_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    memset(&g_pump, 0, sizeof(g_pump));
    pthread_mutex_init(&g_pump.lock, NULL);
    g_pump.mgr = mgr;
    g_pump.cap = cap;
    g_pump.running = 1;
    snprintf(g_pump.status, sizeof(g_pump.status), "Starting PumpLab");

    HWND hWnd = hwnd_create(mgr, pump_wndproc, NULL, cap);
    g_pump.hWnd = hWnd;
    hwnd_set_thread_external_pump(mgr, cap.id, cap.id, 1, "PumpLab");
    pthread_create(&g_pump.thread, NULL, pump_thread_main, NULL);
    g_pump.threadStarted = 1;
    return hWnd;
}

void pump_destroy(void)
{
    pthread_mutex_lock(&g_pump.lock);
    g_pump.running = 0;
    pthread_mutex_unlock(&g_pump.lock);
    if (g_pump.threadStarted) pthread_join(g_pump.thread, NULL);
    if (g_pump.mgr && g_pump.cap.id) hwnd_set_thread_external_pump(g_pump.mgr, g_pump.cap.id, g_pump.cap.id, 0, NULL);
    pthread_mutex_destroy(&g_pump.lock);
}

void pump_blit(HWND hwnd, int x, int y, int w, int h, Framebuffer* fb)
{
    if (!fb) return;
    if (hwnd && g_pump.hWnd != hwnd) g_pump.hWnd = hwnd;
    int cx = x + 1;
    int cy = y + TITLEBAR_H;
    int cw = w - 2;
    int ch = h - TITLEBAR_H - 1;
    if (cw < 80 || ch < 60) return;

    pthread_mutex_lock(&g_pump.lock);
    uint64_t timerCount = g_pump.timerCount;
    uint64_t selfPosts = g_pump.selfPosts;
    uint64_t stressPosts = g_pump.stressPosts;
    uint64_t dispatchCount = g_pump.dispatchCount;
    uint64_t threadMessages = g_pump.threadMessages;
    UINT lastMsg = g_pump.lastMsg;
    DWORD qdepth = hwnd_get_thread_queue_status(g_pump.mgr, g_pump.cap.id, g_pump.cap.id);
    BOOL hung = hwnd_is_window_hung(g_pump.mgr, g_pump.hWnd, 750) ? TRUE : FALSE;
    int timerEnabled = g_pump.timerEnabled;
    char status[128]; snprintf(status, sizeof(status), "%s", g_pump.status);
    char log[8][96]; int logCount = g_pump.logCount; memcpy(log, g_pump.log, sizeof(log));
    DWORD pid = g_pump.cap.id;
    DWORD tid = g_pump.cap.id;
    pthread_mutex_unlock(&g_pump.lock);

    fb_rect(fb, cx, cy, cw, ch, COLOR(10,10,16));
    fb_rect_outline(fb, cx, cy, cw, ch, COLOR(65,65,95));
    button(fb, cx + 8,   cy + 8,  88,  "Post Self");
    button(fb, cx + 104, cy + 8,  110, "Stress 1000");
    button(fb, cx + 222, cy + 8,  94,  "Hang 2s");
    button(fb, cx + 324, cy + 8,  102, timerEnabled ? "Stop Timer" : "Start Timer");

    char line1[220];
    snprintf(line1, sizeof(line1), "PID=%u TID=%u HWND=%u queue=%u dispatch=%llu last=0x%04x HUNG=%s",
             pid, tid, g_pump.hWnd, qdepth, (unsigned long long)dispatchCount, lastMsg, hung ? "YES" : "no");
    draw_clip_text(fb, cx + 8, cy + 42, line1, hung ? COLOR(255,140,140) : COLOR(150,255,170), cx + 8, cy + 38, cw - 16, 12);

    char line2[220];
    snprintf(line2, sizeof(line2), "self=%llu stressQueued=%llu timer=%llu threadMsgs=%llu  [external pump: desktop does not dispatch this queue]",
             (unsigned long long)selfPosts, (unsigned long long)stressPosts, (unsigned long long)timerCount,
             (unsigned long long)threadMessages);
    draw_clip_text(fb, cx + 8, cy + 58, line2, COLOR(220,220,240), cx + 8, cy + 54, cw - 16, 12);

    fb_rect(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(18,18,28));
    fb_rect_outline(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(65,65,95));
    int yline = cy + 86;
    int start = logCount > 7 ? logCount - 7 : 0;
    for (int i = start; i < logCount; i++) {
        draw_clip_text(fb, cx + 14, yline, log[i], COLOR(210,210,225), cx + 10, cy + 80, cw - 20, ch - 116);
        yline += 14;
    }

    draw_clip_text(fb, cx + 14, cy + ch - 24, status, hung ? COLOR(255,160,160) : COLOR(180,255,190), cx + 10, cy + ch - 28, cw - 20, 14);
}
