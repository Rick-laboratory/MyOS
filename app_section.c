#include "app_section.h"
#include "window.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): SectionLab is the file-mapping/shared-memory canary. It
   relies on CreateFileMappingA/OpenFileMappingA/MapViewOfFile and visible shared
   writes. It will need attention when section access masks, view protection,
   inheritability and close/unmap lifetime become stricter MSDN behavior. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

typedef struct SectionLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    Capability cap;
    pthread_mutex_t lock;
    HANDLE hMap;
    LPVOID viewA;
    LPVOID viewB;
    DWORD writeCount;
    DWORD readCount;
    DWORD failCount;
    UINT lastMsg;
    MyAppResizeState resize;
    char lastRead[128];
    char status[160];
    char log[8][112];
    int logCount;
} SectionLabApp;

static SectionLabApp g_sec;

static void sec_log_locked(const char* s)
{
    if (!s) return;
    if (g_sec.logCount < 8) snprintf(g_sec.log[g_sec.logCount++], sizeof(g_sec.log[0]), "%s", s);
    else {
        for (int i = 1; i < 8; i++) snprintf(g_sec.log[i-1], sizeof(g_sec.log[0]), "%s", g_sec.log[i]);
        snprintf(g_sec.log[7], sizeof(g_sec.log[0]), "%s", s);
    }
}


static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,50,70));
    fb_rect_outline(fb, x, y, w, 20, COLOR(120,140,175));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}

static void post_self(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_sec.mgr && g_sec.hWnd) hwnd_post(g_sec.mgr, &g_sec.cap, g_sec.hWnd, msg, wp, lp);
}

static void ensure_runtime(void)
{
    MyWinBindRuntime(g_sec.mgr, &g_sec.cap);
}

static void create_map(void)
{
    ensure_runtime();
    if (!g_sec.hMap) {
        g_sec.hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "Local\\myos.sectionlab.demo");
    }
    if (g_sec.hMap && !g_sec.viewA) g_sec.viewA = MapViewOfFile(g_sec.hMap, FILE_MAP_WRITE|FILE_MAP_READ, 0, 0, 4096);
    if (g_sec.hMap && !g_sec.viewB) g_sec.viewB = MapViewOfFile(g_sec.hMap, FILE_MAP_READ, 0, 0, 4096);

    pthread_mutex_lock(&g_sec.lock);
    if (g_sec.hMap && g_sec.viewA && g_sec.viewB) {
        snprintf(g_sec.status, sizeof(g_sec.status), "CreateFileMapping/MapViewOfFile OK handle=0x%x", g_sec.hMap);
        sec_log_locked("CREATE: one section, two views mapped");
    } else {
        g_sec.failCount++;
        snprintf(g_sec.status, sizeof(g_sec.status), "CREATE/MAP failed - missing CAP_SECTION_MAP?");
        sec_log_locked("CREATE: FAILED");
    }
    pthread_mutex_unlock(&g_sec.lock);
}

static void write_map(void)
{
    ensure_runtime();
    if (!g_sec.hMap || !g_sec.viewA) create_map();
    pthread_mutex_lock(&g_sec.lock);
    if (g_sec.viewA) {
        g_sec.writeCount++;
        snprintf((char*)g_sec.viewA, 4096, "SharedSection says hello #%u from PID=%u HWND=%u", g_sec.writeCount, g_sec.cap.id, g_sec.hWnd);
        FlushViewOfFile(g_sec.viewA, 0);
        snprintf(g_sec.status, sizeof(g_sec.status), "WRITE viewA -> shared section #%u", g_sec.writeCount);
        sec_log_locked(g_sec.status);
    } else {
        g_sec.failCount++;
        snprintf(g_sec.status, sizeof(g_sec.status), "WRITE failed: no mapped write view");
        sec_log_locked("WRITE: FAILED no viewA");
    }
    pthread_mutex_unlock(&g_sec.lock);
}

static void read_map(void)
{
    ensure_runtime();
    if (!g_sec.hMap || !g_sec.viewB) create_map();
    pthread_mutex_lock(&g_sec.lock);
    if (g_sec.viewB) {
        g_sec.readCount++;
        snprintf(g_sec.lastRead, sizeof(g_sec.lastRead), "%s", (const char*)g_sec.viewB);
        snprintf(g_sec.status, sizeof(g_sec.status), "READ viewB <- '%s'", g_sec.lastRead);
        sec_log_locked(g_sec.status);
    } else {
        g_sec.failCount++;
        snprintf(g_sec.status, sizeof(g_sec.status), "READ failed: no mapped read view");
        sec_log_locked("READ: FAILED no viewB");
    }
    pthread_mutex_unlock(&g_sec.lock);
}

static void unmap_all(void)
{
    ensure_runtime();
    if (g_sec.viewA) { UnmapViewOfFile(g_sec.viewA); g_sec.viewA = NULL; }
    if (g_sec.viewB) { UnmapViewOfFile(g_sec.viewB); g_sec.viewB = NULL; }
    if (g_sec.hMap) { CloseHandle(g_sec.hMap); g_sec.hMap = 0; }
    pthread_mutex_lock(&g_sec.lock);
    snprintf(g_sec.status, sizeof(g_sec.status), "UnmapViewOfFile + CloseHandle done");
    sec_log_locked("UNMAP: all views closed");
    pthread_mutex_unlock(&g_sec.lock);
}

static void section_post_command(UINT cmd)
{
    post_self(WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
}

static void section_hit_test(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8 && cx < 128) { section_post_command(SECLAB_CREATE_MAP); return; }
        if (cx >= 136 && cx < 238) { section_post_command(SECLAB_WRITE); return; }
        if (cx >= 246 && cx < 348) { section_post_command(SECLAB_READ); return; }
        if (cx >= 356 && cx < 468) { section_post_command(SECLAB_UNMAP); return; }
    }
}

static void section_handle_command(UINT cmd)
{
    switch (cmd) {
    case SECLAB_CREATE_MAP: create_map(); break;
    case SECLAB_WRITE: write_map(); break;
    case SECLAB_READ: read_map(); break;
    case SECLAB_UNMAP: unmap_all(); break;
    default: break;
    }
}

static void section_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, void* userdata)
{
    (void)wp; (void)lp; (void)userdata;
    pthread_mutex_lock(&g_sec.lock);
    g_sec.lastMsg = msg;
    pthread_mutex_unlock(&g_sec.lock);
    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_sec.lock);
        g_sec.hWnd = hwnd;
        MyAppResizeInit(&g_sec.resize, SECTION_W, SECTION_H, TITLEBAR_H);
        snprintf(g_sec.status, sizeof(g_sec.status), "SectionLab ready - Create then Write/Read");
        sec_log_locked("WM_CREATE: FileMapping/MapViewOfFile test app");
        pthread_mutex_unlock(&g_sec.lock);
        break;
    case WM_LBUTTONDOWN:
        section_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_COMMAND:
        section_handle_command((UINT)LOWORD(wp));
        break;
    case SECLAB_CREATE_MAP:
        section_handle_command(SECLAB_CREATE_MAP);
        break;
    case SECLAB_WRITE:
        section_handle_command(SECLAB_WRITE);
        break;
    case SECLAB_READ:
        section_handle_command(SECLAB_READ);
        break;
    case SECLAB_UNMAP:
        section_handle_command(SECLAB_UNMAP);
        break;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_sec.resize, lp, SECTION_MIN_W, SECTION_MIN_H);
        break;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_sec.resize, lp);
        break;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_sec.resize, lp, TITLEBAR_H);
        break;
    case WM_MOVE:
        MyAppResizeOnMove(&g_sec.resize, lp);
        break;
    case WM_SIZE:
        MyAppResizeOnSize(&g_sec.resize, wp, lp);
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        unmap_all();
        break;
    default:
        break;
    }
}

HWND section_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    memset(&g_sec, 0, sizeof(g_sec));
    pthread_mutex_init(&g_sec.lock, NULL);
    g_sec.mgr = mgr;
    g_sec.cap = cap;
    snprintf(g_sec.status, sizeof(g_sec.status), "Starting SectionLab");
    HWND hWnd = hwnd_create(mgr, section_wndproc, NULL, cap);
    g_sec.hWnd = hWnd;
    return hWnd;
}

void section_destroy(void)
{
    unmap_all();
    pthread_mutex_destroy(&g_sec.lock);
}

void section_blit(HWND hwnd, int x, int y, int w, int h, Framebuffer* fb)
{
    if (!fb) return;
    if (hwnd && g_sec.hWnd != hwnd) g_sec.hWnd = hwnd;
    int cx = x + 1;
    int cy = y + TITLEBAR_H;
    int cw = w - 2;
    int ch = h - TITLEBAR_H - 1;
    if (cw < 80 || ch < 60) return;

    pthread_mutex_lock(&g_sec.lock);
    HANDLE hMap = g_sec.hMap;
    int hasA = g_sec.viewA != NULL;
    int hasB = g_sec.viewB != NULL;
    DWORD writes = g_sec.writeCount;
    DWORD reads = g_sec.readCount;
    DWORD fails = g_sec.failCount;
    UINT last = g_sec.lastMsg;
    char lastRead[128]; snprintf(lastRead, sizeof(lastRead), "%s", g_sec.lastRead[0] ? g_sec.lastRead : "<empty>");
    char status[160]; snprintf(status, sizeof(status), "%s", g_sec.status);
    char log[8][112]; int logCount = g_sec.logCount; memcpy(log, g_sec.log, sizeof(log));
    pthread_mutex_unlock(&g_sec.lock);

    ensure_runtime();
    DWORD secCount = MyGetSectionCount();
    DWORD viewCount = MyGetMappedViewCount();

    fb_rect(fb, cx, cy, cw, ch, COLOR(10,12,20));
    fb_rect_outline(fb, cx, cy, cw, ch, COLOR(70,90,130));
    button(fb, cx + 8,   cy + 8,  120, "Create Map");
    button(fb, cx + 136, cy + 8,  102, "Write");
    button(fb, cx + 246, cy + 8,  102, "Read");
    button(fb, cx + 356, cy + 8,  112, "Unmap");

    char line[256];
    snprintf(line, sizeof(line), "PID=%u HWND=%u handle=0x%x viewA=%s viewB=%s sections=%u views=%u last=0x%04x",
             g_sec.cap.id, g_sec.hWnd, hMap, hasA ? "YES" : "no", hasB ? "YES" : "no", secCount, viewCount, last);
    draw_clip_text(fb, cx + 8, cy + 42, line, COLOR(180,255,190), cx + 8, cy + 38, cw - 16, 12);

    snprintf(line, sizeof(line), "writes=%u reads=%u fails=%u lastRead='%s'", writes, reads, fails, lastRead);
    draw_clip_text(fb, cx + 8, cy + 58, line, COLOR(220,220,240), cx + 8, cy + 54, cw - 16, 12);

    fb_rect(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(18,18,28));
    fb_rect_outline(fb, cx + 8, cy + 78, cw - 16, ch - 112, COLOR(70,90,130));
    int yline = cy + 86;
    int start = logCount > 7 ? logCount - 7 : 0;
    for (int i = start; i < logCount; i++) {
        draw_clip_text(fb, cx + 14, yline, log[i], COLOR(210,210,225), cx + 10, cy + 80, cw - 20, ch - 116);
        yline += 14;
    }
    draw_clip_text(fb, cx + 14, cy + ch - 24, status, strstr(status, "failed") ? COLOR(255,165,140) : COLOR(180,255,190), cx + 10, cy + ch - 28, cw - 20, 14);
}
