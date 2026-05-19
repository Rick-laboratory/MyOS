#include "app_deadlock.h"
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

/* AUDIT(v119-lab): DeadlockLab probes SendMessageTimeout and hung-window
   behavior. It depends on PumpLab being discoverable through WSTS diagnostics.
   It should be expected to change when SendMessage becomes truly cross-thread/
   cross-process blocking with reentrancy rules. Failures here are often useful:
   they reveal modal send, timeout or queue-pump semantics that are still not
   MSDN-compatible. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define DLMSG_SEND_FAST     (WM_USER + 0x330)
#define DLMSG_SEND_SLOW     (WM_USER + 0x331)
#define DLMSG_HANG_SEND     (WM_USER + 0x332)
#define DLMSG_CROSS_SEND    (WM_USER + 0x333)

typedef struct DeadlockLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    Capability cap;
    pthread_t thread;
    pthread_mutex_t lock;
    int threadStarted;
    int running;
    HWND targetPump;
    uint64_t dispatchCount;
    uint64_t sendOk;
    uint64_t sendTimeout;
    uint64_t targetMiss;
    UINT lastMsg;
    MyAppResizeState resize;
    DWORD lastQueueDepth;
    int lastSendMs;
    char status[160];
    char log[8][112];
    int logCount;
} DeadlockLabApp;

static DeadlockLabApp g_dl;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void dl_log_locked(const char* s)
{
    if (!s) return;
    if (g_dl.logCount < 8) {
        snprintf(g_dl.log[g_dl.logCount++], sizeof(g_dl.log[0]), "%s", s);
    } else {
        for (int i = 1; i < 8; i++) snprintf(g_dl.log[i-1], sizeof(g_dl.log[0]), "%s", g_dl.log[i]);
        snprintf(g_dl.log[7], sizeof(g_dl.log[0]), "%s", s);
    }
}


static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(55,45,75));
    fb_rect_outline(fb, x, y, w, 20, COLOR(145,125,185));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}

static HWND find_pump_hwnd(void)
{
    if (!g_dl.mgr) return 0;
    const MyWindowStateSection* sec = hwnd_get_window_state_section(g_dl.mgr);
    if (!sec || sec->magic != MYOS_WINDOWSTATE_MAGIC) return 0;
    for (DWORD i = 0; i < sec->capacity; i++) {
        const MyWindowState* st = &sec->states[i];
        if (!st->hWnd || st->destroyed) continue;
        if (strstr(st->szTitle, "PumpLab")) return st->hWnd;
    }
    return 0;
}

static void post_self(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_dl.mgr && g_dl.hWnd)
        hwnd_post(g_dl.mgr, &g_dl.cap, g_dl.hWnd, msg, wp, lp);
}

static void run_send_test(const char* label, int timeout_ms, int make_hang_first, int send_hang_command)
{
    HWND target = find_pump_hwnd();
    uint64_t t0 = now_ms();
    pthread_mutex_lock(&g_dl.lock);
    g_dl.targetPump = target;
    if (!target) {
        g_dl.targetMiss++;
        snprintf(g_dl.status, sizeof(g_dl.status), "%s: no PumpLab found - open F9 first", label);
        dl_log_locked(g_dl.status);
        pthread_mutex_unlock(&g_dl.lock);
        return;
    }
    snprintf(g_dl.status, sizeof(g_dl.status), "%s: SendMessageTimeout -> PumpLab HWND=%u timeouts=%dms", label, target, timeout_ms);
    dl_log_locked(g_dl.status);
    pthread_mutex_unlock(&g_dl.lock);

    if (make_hang_first) {
        hwnd_post(g_dl.mgr, &g_dl.cap, target, PMPLAB_POST_SELF, 2, 0);
        usleep(50000);
    }

    int msg = send_hang_command ? PMPLAB_POST_SELF : PMPLAB_POST_SELF;
    WPARAM wp = send_hang_command ? 2 : 1;
    int target_was_hung = hwnd_is_window_hung(g_dl.mgr, target, 750);
    int r = hwnd_send_timeout(g_dl.mgr, &g_dl.cap, target, msg, wp, 0, timeout_ms);
    uint64_t dt = now_ms() - t0;

    pthread_mutex_lock(&g_dl.lock);
    g_dl.lastSendMs = (int)dt;
    if (r == 0) {
        g_dl.sendOk++;
        snprintf(g_dl.status, sizeof(g_dl.status), "%s: OK in %dms", label, (int)dt);
    } else if (r == -2) {
        g_dl.sendTimeout++;
        snprintf(g_dl.status, sizeof(g_dl.status), "%s: TIMEOUT in %dms%s", label, (int)dt, target_was_hung ? " target=HUNG" : "");
    } else {
        g_dl.sendTimeout++;
        snprintf(g_dl.status, sizeof(g_dl.status), "%s: DENY/FAIL in %dms", label, (int)dt);
    }
    dl_log_locked(g_dl.status);
    pthread_mutex_unlock(&g_dl.lock);
}

static void deadlock_post_command(UINT cmd)
{
    post_self(WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
}

static void deadlock_hit_test(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8 && cx < 122) { deadlock_post_command(DLMSG_SEND_FAST); return; }
        if (cx >= 130 && cx < 254) { deadlock_post_command(DLMSG_SEND_SLOW); return; }
        if (cx >= 262 && cx < 398) { deadlock_post_command(DLMSG_HANG_SEND); return; }
        if (cx >= 406 && cx < 546) { deadlock_post_command(DLMSG_CROSS_SEND); return; }
    }
}

static void deadlock_handle_command(UINT cmd)
{
    switch (cmd) {
    case DLMSG_SEND_FAST: run_send_test("fast 250ms", 250, 0, 0); break;
    case DLMSG_SEND_SLOW: run_send_test("slow 1000ms", 1000, 0, 0); break;
    case DLMSG_HANG_SEND: run_send_test("hang+send 250ms", 250, 1, 0); break;
    case DLMSG_CROSS_SEND: run_send_test("send hang-cmd 1000ms", 1000, 0, 1); break;
    default: break;
    }
}

static void deadlock_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, void* userdata)
{
    (void)userdata; (void)wp; (void)lp;
    pthread_mutex_lock(&g_dl.lock);
    g_dl.dispatchCount++;
    g_dl.lastMsg = msg;
    pthread_mutex_unlock(&g_dl.lock);

    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_dl.lock);
        g_dl.hWnd = hwnd;
        MyAppResizeInit(&g_dl.resize, DEADLOCK_W, DEADLOCK_H, TITLEBAR_H);
        snprintf(g_dl.status, sizeof(g_dl.status), "DeadlockLab ready - open PumpLab with F9");
        dl_log_locked("WM_CREATE: sync SendMessageTimeout tests are external-pumped");
        pthread_mutex_unlock(&g_dl.lock);
        break;
    case WM_LBUTTONDOWN:
        deadlock_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_COMMAND:
        deadlock_handle_command((UINT)LOWORD(wp));
        break;
    case DLMSG_SEND_FAST:
        deadlock_handle_command(DLMSG_SEND_FAST);
        break;
    case DLMSG_SEND_SLOW:
        deadlock_handle_command(DLMSG_SEND_SLOW);
        break;
    case DLMSG_HANG_SEND:
        deadlock_handle_command(DLMSG_HANG_SEND);
        break;
    case DLMSG_CROSS_SEND:
        deadlock_handle_command(DLMSG_CROSS_SEND);
        break;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_dl.resize, lp, DEADLOCK_MIN_W, DEADLOCK_MIN_H);
        break;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_dl.resize, lp);
        break;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_dl.resize, lp, TITLEBAR_H);
        break;
    case WM_MOVE:
        MyAppResizeOnMove(&g_dl.resize, lp);
        break;
    case WM_SIZE:
        MyAppResizeOnSize(&g_dl.resize, wp, lp);
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        pthread_mutex_lock(&g_dl.lock);
        g_dl.running = 0;
        snprintf(g_dl.status, sizeof(g_dl.status), "WM_DESTROY -> deadlock pump stops");
        pthread_mutex_unlock(&g_dl.lock);
        break;
    default:
        break;
    }
}

static void* deadlock_thread_main(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&g_dl.lock);
    dl_log_locked("Deadlock thread online: own GetMessage loop active");
    pthread_mutex_unlock(&g_dl.lock);

    while (1) {
        pthread_mutex_lock(&g_dl.lock);
        int running = g_dl.running;
        pthread_mutex_unlock(&g_dl.lock);
        if (!running) break;

        MyMessage msg;
        if (hwnd_get_thread_message_wait(g_dl.mgr, g_dl.cap.id, g_dl.cap.id, 0, 0, 0, 1, 50, &msg)) {
            if (msg.hwnd) hwnd_dispatch_message(g_dl.mgr, &msg);
        }
        pthread_mutex_lock(&g_dl.lock);
        g_dl.lastQueueDepth = hwnd_get_thread_queue_status(g_dl.mgr, g_dl.cap.id, g_dl.cap.id);
        pthread_mutex_unlock(&g_dl.lock);
    }
    return NULL;
}

HWND deadlock_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    memset(&g_dl, 0, sizeof(g_dl));
    pthread_mutex_init(&g_dl.lock, NULL);
    g_dl.mgr = mgr;
    g_dl.cap = cap;
    g_dl.running = 1;
    snprintf(g_dl.status, sizeof(g_dl.status), "Starting DeadlockLab");
    HWND hWnd = hwnd_create(mgr, deadlock_wndproc, NULL, cap);
    g_dl.hWnd = hWnd;
    hwnd_set_thread_external_pump(mgr, cap.id, cap.id, 1, "DeadlockLab");
    pthread_create(&g_dl.thread, NULL, deadlock_thread_main, NULL);
    g_dl.threadStarted = 1;
    return hWnd;
}

void deadlock_destroy(void)
{
    pthread_mutex_lock(&g_dl.lock);
    g_dl.running = 0;
    pthread_mutex_unlock(&g_dl.lock);
    if (g_dl.threadStarted) pthread_join(g_dl.thread, NULL);
    if (g_dl.mgr && g_dl.cap.id) hwnd_set_thread_external_pump(g_dl.mgr, g_dl.cap.id, g_dl.cap.id, 0, NULL);
    pthread_mutex_destroy(&g_dl.lock);
}

void deadlock_blit(HWND hwnd, int x, int y, int w, int h, Framebuffer* fb)
{
    if (!fb) return;
    if (hwnd && g_dl.hWnd != hwnd) g_dl.hWnd = hwnd;
    int cx = x + 1;
    int cy = y + TITLEBAR_H;
    int cw = w - 2;
    int ch = h - TITLEBAR_H - 1;
    if (cw < 80 || ch < 60) return;

    pthread_mutex_lock(&g_dl.lock);
    HWND target = g_dl.targetPump ? g_dl.targetPump : find_pump_hwnd();
    uint64_t dispatchCount = g_dl.dispatchCount;
    uint64_t sendOk = g_dl.sendOk;
    uint64_t sendTimeout = g_dl.sendTimeout;
    uint64_t targetMiss = g_dl.targetMiss;
    UINT lastMsg = g_dl.lastMsg;
    DWORD qdepth = hwnd_get_thread_queue_status(g_dl.mgr, g_dl.cap.id, g_dl.cap.id);
    uint64_t sends = hwnd_get_thread_send_count(g_dl.mgr, target ? hwnd_get_owner_pid(g_dl.mgr, target) : 0, target ? hwnd_get_owner_pid(g_dl.mgr, target) : 0);
    uint64_t timeouts = hwnd_get_thread_send_timeout_count(g_dl.mgr, target ? hwnd_get_owner_pid(g_dl.mgr, target) : 0, target ? hwnd_get_owner_pid(g_dl.mgr, target) : 0);
    BOOL selfHung = hwnd_is_window_hung(g_dl.mgr, g_dl.hWnd, 750) ? TRUE : FALSE;
    BOOL targetHung = target ? (hwnd_is_window_hung(g_dl.mgr, target, 750) ? TRUE : FALSE) : FALSE;
    int lastSendMs = g_dl.lastSendMs;
    char status[160]; snprintf(status, sizeof(status), "%s", g_dl.status);
    char log[8][112]; int logCount = g_dl.logCount; memcpy(log, g_dl.log, sizeof(log));
    DWORD pid = g_dl.cap.id;
    DWORD tid = g_dl.cap.id;
    pthread_mutex_unlock(&g_dl.lock);

    fb_rect(fb, cx, cy, cw, ch, COLOR(12,9,18));
    fb_rect_outline(fb, cx, cy, cw, ch, COLOR(85,65,115));
    button(fb, cx + 8,   cy + 8,  114, "Send 250ms");
    button(fb, cx + 130, cy + 8,  124, "Send 1000ms");
    button(fb, cx + 262, cy + 8,  136, "Hang+Send");
    button(fb, cx + 406, cy + 8,  140, "Send HangCmd");

    char line1[256];
    snprintf(line1, sizeof(line1), "PID=%u TID=%u HWND=%u targetPump=%u q=%u dispatch=%llu last=0x%04x selfH=%s targetH=%s",
             pid, tid, g_dl.hWnd, target, qdepth, (unsigned long long)dispatchCount, lastMsg,
             selfHung ? "YES" : "no", targetHung ? "YES" : "no");
    draw_clip_text(fb, cx + 8, cy + 42, line1, (selfHung || targetHung) ? COLOR(255,150,150) : COLOR(180,255,190), cx + 8, cy + 38, cw - 16, 12);

    char line2[256];
    snprintf(line2, sizeof(line2), "OK=%llu timeouts=%llu miss=%llu lastSend=%dms targetSend=%llu targetTimeout=%llu",
             (unsigned long long)sendOk, (unsigned long long)sendTimeout, (unsigned long long)targetMiss,
             lastSendMs, (unsigned long long)sends, (unsigned long long)timeouts);
    draw_clip_text(fb, cx + 8, cy + 58, line2, COLOR(220,220,240), cx + 8, cy + 54, cw - 16, 12);

    fb_rect(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(20,16,28));
    fb_rect_outline(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(85,65,115));
    int yline = cy + 86;
    int start = logCount > 7 ? logCount - 7 : 0;
    for (int i = start; i < logCount; i++) {
        draw_clip_text(fb, cx + 14, yline, log[i], COLOR(210,210,225), cx + 10, cy + 80, cw - 20, ch - 116);
        yline += 14;
    }

    draw_clip_text(fb, cx + 14, cy + ch - 24, status,
                   (strstr(status, "TIMEOUT") || strstr(status, "DENY")) ? COLOR(255,165,140) : COLOR(180,255,190),
                   cx + 10, cy + ch - 28, cw - 20, 14);
}
