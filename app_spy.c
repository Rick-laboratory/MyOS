#include "app_spy.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "window.h"
#include "font.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): Spy is a privileged diagnostic app, not normal Win32 user
   code. It reads WSTS/Object/Window state through myOS diagnostic APIs and will
   break when desktop/session isolation tightens. That is acceptable; keep it as
   admin/internal tooling and do not let Spy-only requirements leak into public
   USER32 behavior. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif
#ifndef RESIZE_GRIP
#define RESIZE_GRIP 6
#endif

typedef struct SpyRow {
    HWND hWnd;
    MyWindowState state;
    int subscribed;
    int hung;
    int slot;
} SpyRow;

typedef struct SpyApp {
    HWND hWnd;
    Capability cap;
    SpyRow rows[32];
    int count;
    HWND selected;
    HWND lastForeground;
    int lastClickIndex;
    long long lastClickMs;
    uint64_t updateCount;
    uint64_t enumCount;
    MyAppResizeState resize;
    char status[96];
} SpyApp;

static SpyApp g_spy;
static int spy_find_wsts_slot(HWND hWnd);

static void spy_bind_runtime(void)
{
    HWNDManager* mgr = MyWinGetHwndManager();
    if (mgr) MyWinBindRuntime(mgr, &g_spy.cap);
}

typedef struct EnumCtx {
    HWND self;
    SpyRow* rows;
    int count;
    int maxCount;
} EnumCtx;

static BOOL CALLBACK spy_enum_proc(HWND hWnd, LPARAM lParam)
{
    EnumCtx* ctx = (EnumCtx*)lParam;
    if (!ctx || !hWnd || ctx->count >= ctx->maxCount) return FALSE;

    MyWindowState st;
    memset(&st, 0, sizeof(st));
    st.cbSize = sizeof(st);
    if (!MyGetWindowState(hWnd, &st)) return TRUE;

    ctx->rows[ctx->count].hWnd = hWnd;
    ctx->rows[ctx->count].state = st;
    ctx->rows[ctx->count].subscribed = 0;
    ctx->rows[ctx->count].hung = IsHungAppWindow(hWnd);
    ctx->rows[ctx->count].slot = spy_find_wsts_slot(hWnd);
    ctx->count++;
    return TRUE;
}

static int spy_known(HWND hWnd)
{
    for (int i = 0; i < g_spy.count; i++) {
        if (g_spy.rows[i].hWnd == hWnd) return 1;
    }
    return 0;
}
static int spy_find_wsts_slot(HWND hWnd)
{
    const MyWindowStateSection* sec = MyGetWindowStateSection();
    if (!sec || sec->magic != MYOS_WINDOWSTATE_MAGIC) return -1;
    for (DWORD i = 0; i < sec->capacity && i < 32; i++) {
        if (sec->states[i].hWnd == hWnd) return (int)i;
    }
    return -1;
}


static long long spy_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void spy_refresh(void)
{
    spy_bind_runtime();
    SpyRow old[32];
    int oldCount = g_spy.count;
    memcpy(old, g_spy.rows, sizeof(old));

    memset(g_spy.rows, 0, sizeof(g_spy.rows));
    EnumCtx ctx = { g_spy.hWnd, g_spy.rows, 0, 32 };
    EnumWindows(spy_enum_proc, (LPARAM)&ctx);
    g_spy.count = ctx.count;
    g_spy.enumCount++;
    g_spy.lastForeground = GetForegroundWindow();

    for (int i = 0; i < g_spy.count; i++) {
        for (int j = 0; j < oldCount; j++) {
            if (old[j].hWnd == g_spy.rows[i].hWnd && old[j].subscribed) {
                g_spy.rows[i].subscribed = 1;
                break;
            }
        }
    }

    if (!g_spy.selected && g_spy.count > 0)
        g_spy.selected = g_spy.rows[0].hWnd;
    if (g_spy.selected && !spy_known(g_spy.selected))
        g_spy.selected = g_spy.count > 0 ? g_spy.rows[0].hWnd : 0;
}


static int spy_update_row(HWND hWnd)
{
    spy_bind_runtime();
    if (!hWnd) return 0;
    MyWindowState st;
    memset(&st, 0, sizeof(st));
    st.cbSize = sizeof(st);
    if (!MyGetWindowState(hWnd, &st)) return 0;

    for (int i = 0; i < g_spy.count; i++) {
        if (g_spy.rows[i].hWnd == hWnd) {
            g_spy.rows[i].state = st;
            g_spy.rows[i].hung = IsHungAppWindow(hWnd);
            g_spy.rows[i].slot = spy_find_wsts_slot(hWnd);
            return 1;
        }
    }

    if (g_spy.count < 32) {
        g_spy.rows[g_spy.count].hWnd = hWnd;
        g_spy.rows[g_spy.count].state = st;
        g_spy.rows[g_spy.count].subscribed = 1;
        g_spy.rows[g_spy.count].hung = IsHungAppWindow(hWnd);
        g_spy.rows[g_spy.count].slot = spy_find_wsts_slot(hWnd);
        g_spy.count++;
        return 1;
    }
    return 0;
}

static void spy_update_foreground_flags(void)
{
    spy_bind_runtime();
    g_spy.lastForeground = GetForegroundWindow();
    for (int i = 0; i < g_spy.count; i++) {
        g_spy.rows[i].state.active = (g_spy.rows[i].hWnd == g_spy.lastForeground) ? TRUE : FALSE;
        g_spy.rows[i].hung = IsHungAppWindow(g_spy.rows[i].hWnd);
    }
}

static void spy_compact_dead_rows(void)
{
    int w = 0;
    for (int r = 0; r < g_spy.count; r++) {
        // v17.2: keep destroyed rows visible as tombstones if the WSTS slot
        // still has a stable snapshot. This makes close/destroy observable.
        if (g_spy.rows[r].hWnd && (IsWindow(g_spy.rows[r].hWnd) || g_spy.rows[r].state.destroyed)) {
            if (w != r) g_spy.rows[w] = g_spy.rows[r];
            w++;
        }
    }
    g_spy.count = w;
    if (g_spy.selected && !spy_known(g_spy.selected))
        g_spy.selected = g_spy.count > 0 ? g_spy.rows[0].hWnd : 0;
}

static void spy_subscribe_all(void)
{
    spy_bind_runtime();
    for (int i = 0; i < g_spy.count; i++) {
        HWND target = g_spy.rows[i].hWnd;
        if (!target || target == g_spy.hWnd) continue;
        // 0..0 means all source-window signals in the current HWND router.
        if (MySubscribeWindowMessage(target, g_spy.hWnd, 0, 0))
            g_spy.rows[i].subscribed = 1;
    }
    snprintf(g_spy.status, sizeof(g_spy.status), "Subscribed: POS|ACT|SHOW|TEXT|DESTROY via WSTS");
}


static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,55,75));
    fb_rect_outline(fb, x, y, w, 20, COLOR(115,135,170));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}

static void spy_select_from_point(int cx, int cy)
{
    spy_bind_runtime();

    if (cy >= 8 && cy < 28) {
        if (cx >= 8 && cx < 82) {
            spy_refresh();
            snprintf(g_spy.status, sizeof(g_spy.status), "Refresh: %d HWNDs", g_spy.count);
            return;
        }
        if (cx >= 88 && cx < 188) {
            spy_refresh();
            spy_subscribe_all();
            return;
        }
        if (cx >= 194 && cx < 302) {
            if (g_spy.selected) {
                if (SetForegroundWindow(g_spy.selected)) {
                    snprintf(g_spy.status, sizeof(g_spy.status), "Foreground HWND=%u", g_spy.selected);
                    spy_refresh();
                } else {
                    snprintf(g_spy.status, sizeof(g_spy.status), "SetForegroundWindow(HWND=%u) failed", g_spy.selected);
                }
            }
            return;
        }
        if (cx >= 308 && cx < 430) {
            if (g_spy.selected && g_spy.selected != g_spy.hWnd) {
                SetWindowTextA(g_spy.selected, "[myOS Spy touched]");
                snprintf(g_spy.status, sizeof(g_spy.status), "SetWindowTextA(HWND=%u)", g_spy.selected);
                spy_refresh();
            }
            return;
        }
    }

    // Keep hit-test coordinates in lockstep with spy_blit():
    // buttons at y=8..28, stats at 36, header at 56, first row at 72.
    // The previous v16.5/v16.6 draw layout moved rows down, but the
    // hit-test still used 54, so clicks selected the wrong row or no row.
    int list_y = 72;
    int row_h = 18;
    int idx = (cy - list_y) / row_h;
    if (idx >= 0 && idx < g_spy.count) {
        long long now = spy_now_ms();
        int is_double = (g_spy.lastClickIndex == idx && (now - g_spy.lastClickMs) <= 420);
        g_spy.lastClickIndex = idx;
        g_spy.lastClickMs = now;
        g_spy.selected = g_spy.rows[idx].hWnd;
        if (is_double && g_spy.selected) {
            if (SetForegroundWindow(g_spy.selected)) {
                snprintf(g_spy.status, sizeof(g_spy.status), "DblClick -> Foreground HWND=%u", g_spy.selected);
                spy_refresh();
            } else {
                snprintf(g_spy.status, sizeof(g_spy.status), "DblClick -> SetForegroundWindow(HWND=%u) failed", g_spy.selected);
            }
        } else {
            snprintf(g_spy.status, sizeof(g_spy.status), "Selected HWND=%u", g_spy.selected);
        }
    }
}

static LRESULT CALLBACK spy_winproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    spy_bind_runtime();
    switch (Msg) {
    case WM_CREATE:
        g_spy.hWnd = hWnd;
        MyAppResizeInit(&g_spy.resize, SPY_W, SPY_H, TITLEBAR_H);
        snprintf(g_spy.status, sizeof(g_spy.status), "Spy ready: shared WindowState reads");
        return 0;
    case WM_LBUTTONDOWN:
        spy_select_from_point(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_spy.resize, lParam, SPY_MIN_W, SPY_MIN_H);
        return 0;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_spy.resize, lParam);
        return 0;
    case WM_MOVE:
        MyAppResizeOnMove(&g_spy.resize, lParam);
        return 0;
    case WM_SIZE:
        MyAppResizeOnSize(&g_spy.resize, wParam, lParam);
        snprintf(g_spy.status, sizeof(g_spy.status), "Spy WM_SIZE client=%dx%d", g_spy.resize.clientW, g_spy.resize.clientH);
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (!wParam && lParam) {
            MyAppResizeOnWindowPosChanged(&g_spy.resize, lParam, TITLEBAR_H);
            snprintf(g_spy.status, sizeof(g_spy.status), "Spy WM_WINDOWPOSCHANGED client=%dx%d flags=0x%x",
                     g_spy.resize.clientW, g_spy.resize.clientH, g_spy.resize.lastPosFlags);
            return 0;
        }
        __attribute__((fallthrough));
    case WM_ACTIVATE:
    case WM_SHOWWINDOW:
    case WM_WINDOWTEXTCHANGED:
    case WM_DESTROY: {
        // v17.2: queue carries only the signal. wParam tells us which source HWND
        // changed; current data is read from the shared WindowState section.
        HWND hSource = (HWND)wParam;
        g_spy.updateCount++;
        if (hSource) {
            spy_update_row(hSource);
            spy_update_foreground_flags();
            spy_compact_dead_rows();
            snprintf(g_spy.status, sizeof(g_spy.status), "Signal #%llu msg=0x%04x source HWND=%u WSTS ver=%ld",
                     (unsigned long long)g_spy.updateCount, Msg, hSource, (long)lParam);
        } else {
            snprintf(g_spy.status, sizeof(g_spy.status), "Signal #%llu msg=0x%04x -> no source",
                     (unsigned long long)g_spy.updateCount, Msg);
        }
        return 0;
    }
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static ATOM spy_register_class(void)
{
    static ATOM s_atom = 0;
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = spy_winproc;
    wc.lpszClassName = "myOS.Spy";
    s_atom = RegisterClassExA(&wc);
    return s_atom;
}

HWND spy_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    memset(&g_spy, 0, sizeof(g_spy));
    g_spy.cap = cap;
    snprintf(g_spy.status, sizeof(g_spy.status), "Starting Spy++ clone");
    MyWinBindRuntime(mgr, &cap);
    spy_register_class();
    HWND hWnd = CreateWindowExA(
        WS_EX_NONE,
        "myOS.Spy",
        "myOS Spy++",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        x, y, SPY_W, SPY_H,
        0, 0, 0, NULL);
    g_spy.hWnd = hWnd;
    spy_refresh();
    spy_subscribe_all();
    return hWnd;
}

void spy_blit(HWND hwnd, int x, int y, int w, int h, Framebuffer* fb)
{
    spy_bind_runtime();
    if (!fb) return;
    if (hwnd && g_spy.hWnd != hwnd) g_spy.hWnd = hwnd;
    int cx = x + 1;
    int cy = y + TITLEBAR_H;
    int cw = w - 2;
    int ch = h - TITLEBAR_H - 1;
    if (cw < 80 || ch < 60) return;

    // v17: kein Polling im Draw-Pfad. Spy++ rendert den letzten Snapshot
    // aus der shared WindowState section; Updates kommen per Queue-Signal.
    spy_update_foreground_flags();

    fb_rect(fb, cx, cy, cw, ch, COLOR(10,10,16));
    fb_rect_outline(fb, cx, cy, cw, ch, COLOR(65,65,95));

    button(fb, cx + 8,   cy + 8, 74,  "Refresh");
    button(fb, cx + 88,  cy + 8, 100, "Subscribe");
    button(fb, cx + 194, cy + 8, 108, "Foreground");
    button(fb, cx + 308, cy + 8, 122, "Rename target");

    const MyWindowStateSection* sec = MyGetWindowStateSection();
    char hdr[180];
    snprintf(hdr, sizeof(hdr), "HWNDs=%d FG=%u signals=%llu enum=%llu WSTS ver=%u cap=%u live=%u dead=%u",
             g_spy.count, g_spy.lastForeground,
             (unsigned long long)g_spy.updateCount,
             (unsigned long long)g_spy.enumCount,
             sec ? sec->version : 0,
             sec ? sec->capacity : 0,
             sec ? sec->activeCount : 0,
             sec ? sec->destroyedCount : 0);
    // v16.5: mehr vertikale Luft. Vorher überlappte die Stats-Zeile
    // mit dem Tabellenkopf bei kleinen/normalen Spy-Fenstern.
    draw_clip_text(fb, cx + 8, cy + 36, hdr, COLOR(150,255,170),
                   cx + 6, cy + 32, cw - 12, 12);

    int list_y = cy + 72;
    int row_h = 18;
    int detail_y = cy + ch - 70;
    int max_rows = (detail_y - list_y - 6) / row_h;
    if (max_rows < 1) max_rows = 1;
    if (max_rows > g_spy.count) max_rows = g_spy.count;

    draw_clip_text(fb, cx + 8, list_y - 14, "HWND SLT A M S D H VER   RECT              TITLE",
                   COLOR(180,180,210), cx + 6, list_y - 16, cw - 12, 12);
    for (int i = 0; i < max_rows; i++) {
        SpyRow* r = &g_spy.rows[i];
        int ry = list_y + i * row_h;
        int selected = (r->hWnd == g_spy.selected);
        if (selected) {
            fb_rect(fb, cx + 4, ry - 2, cw - 8, row_h, COLOR(35,55,95));
            fb_rect_outline(fb, cx + 4, ry - 2, cw - 8, row_h, COLOR(120,170,240));
        }
        char line[256];
        RECT rc = r->state.rcWindow;
        snprintf(line, sizeof(line), "%4u %3d %c %c %c %c %c %5lu %4d,%4d %4dx%-4d %s",
                 r->hWnd,
                 r->slot,
                 r->state.active ? 'A' : '-',
                 r->state.minimized ? 'M' : '-',
                 r->subscribed ? 'S' : '-',
                 r->state.destroyed ? 'D' : '-',
                 r->hung ? 'H' : '-',
                 (unsigned long)r->state.stateVersion,
                 rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 r->state.szTitle);
        draw_clip_text(fb, cx + 8, ry + 3, line,
                       selected ? WHITE : COLOR(210,210,225),
                       cx + 6, list_y - 16, cw - 12, detail_y - (list_y - 16) - 4);
    }

    fb_rect(fb, cx + 8, detail_y, cw - 16, 58, COLOR(18,18,28));
    fb_rect_outline(fb, cx + 8, detail_y, cw - 16, 58, COLOR(65,65,95));

    MyWindowState st;
    if (g_spy.selected && MyGetWindowState(g_spy.selected, &st)) {
        char d1[180], d2[180];
        snprintf(d1, sizeof(d1), "Selected HWND=%u slot=%d seq=%lu active=%d visible=%d min=%d dead=%d hung=%d msg=0x%04x ver=%lu",
                 st.hWnd, spy_find_wsts_slot(st.hWnd), (unsigned long)st.seqEnd,
                 st.active, st.visible, st.minimized, st.destroyed, IsHungAppWindow(st.hWnd), st.lastMessage,
                 (unsigned long)st.stateVersion);
        snprintf(d2, sizeof(d2), "Title='%s'  RECT=(%d,%d)-(%d,%d)",
                 st.szTitle, st.rcWindow.left, st.rcWindow.top, st.rcWindow.right, st.rcWindow.bottom);
        draw_clip_text(fb, cx + 14, detail_y + 7, d1, COLOR(220,220,240), cx + 10, detail_y + 4, cw - 20, 14);
        draw_clip_text(fb, cx + 14, detail_y + 22, d2, COLOR(170,210,255), cx + 10, detail_y + 19, cw - 20, 14);
    } else {
        draw_clip_text(fb, cx + 14, detail_y + 8, "No selected window", COLOR(200,200,220), cx + 10, detail_y + 4, cw - 20, 14);
    }
    // Status bleibt jetzt wirklich innerhalb der Detailbox, nicht auf der Fensterkante.
    draw_clip_text(fb, cx + 14, detail_y + 42, g_spy.status, COLOR(180,255,190),
                   cx + 10, detail_y + 39, cw - 20, 14);
}
