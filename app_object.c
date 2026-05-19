#include "app_object.h"
#include "window.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "myobject.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): ObjectLab depends on myOS diagnostic enumeration rather than
   public Win32. That is intentional: it is the Object Manager/handle-table
   microscope. It will break when diagnostics move out-of-process or when handle
   tables stop exposing raw slots. Keep this app privileged/internal and never
   treat its MyEnumObjects/MyGetObjectInfo calls as public MSDN surface. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

typedef struct ObjectLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    Capability cap;
    pthread_mutex_t lock;
    _ObjectectInfo rows[32];
    int rowCount;
    MyHandleInfo handles[48];
    int handleCount;
    HANDLE hScratch;
    LPVOID pScratch;
    DWORD refreshCount;
    DWORD makeCount;
    DWORD closeCount;
    DWORD totalObjects;
    DWORD sectionObjects;
    DWORD eventObjects;
    DWORD mutexObjects;
    DWORD semaphoreObjects;
    DWORD timerObjects;
    char status[180];
    MyAppResizeState resize;
} ObjectLabApp;

static ObjectLabApp g_obj;


static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,50,70));
    fb_rect_outline(fb, x, y, w, 20, COLOR(120,140,175));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}

static BOOL enum_obj_cb(const _ObjectectInfo* info, LPARAM lp)
{
    (void)lp;
    if (!info) return TRUE;
    if (g_obj.rowCount < (int)(sizeof(g_obj.rows)/sizeof(g_obj.rows[0]))) {
        g_obj.rows[g_obj.rowCount++] = *info;
    }
    return TRUE;
}

static BOOL enum_handle_cb(const MyHandleInfo* info, LPARAM lp)
{
    (void)lp;
    if (!info) return TRUE;
    if (g_obj.handleCount < (int)(sizeof(g_obj.handles)/sizeof(g_obj.handles[0])))
        g_obj.handles[g_obj.handleCount++] = *info;
    return TRUE;
}

static void ensure_runtime(void)
{
    MyWinBindRuntime(g_obj.mgr, &g_obj.cap);
}

static void object_refresh(void)
{
    ensure_runtime();
    pthread_mutex_lock(&g_obj.lock);
    g_obj.rowCount = 0;
    g_obj.handleCount = 0;
    g_obj.refreshCount++;
    g_obj.totalObjects = MyGetObjectCount();
    g_obj.sectionObjects = MyGetObjectCountByType(_OBJECT_TYPE_SECTION);
    g_obj.eventObjects = MyGetObjectCountByType(_OBJECT_TYPE_EVENT);
    g_obj.mutexObjects = MyGetObjectCountByType(_OBJECT_TYPE_MUTEX);
    g_obj.semaphoreObjects = MyGetObjectCountByType(_OBJECT_TYPE_SEMAPHORE);
    g_obj.timerObjects = MyGetObjectCountByType(_OBJECT_TYPE_TIMER);
    MyEnumObjects(enum_obj_cb, 0);
    MyEnumProcessHandles(0, enum_handle_cb, 0);
    snprintf(g_obj.status, sizeof(g_obj.status), "v31/32 snapshot #%u: objects=%u | handles=%d | SD+namespace visible", g_obj.refreshCount, g_obj.totalObjects, g_obj.handleCount);
    pthread_mutex_unlock(&g_obj.lock);
}

static void object_make_section(void)
{
    ensure_runtime();
    if (!g_obj.hScratch) {
        g_obj.hScratch = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 2048, "Local\\myos.objlab.scratch");
        if (g_obj.hScratch) g_obj.pScratch = MapViewOfFile(g_obj.hScratch, FILE_MAP_ALL_ACCESS, 0, 0, 2048);
    }
    pthread_mutex_lock(&g_obj.lock);
    g_obj.makeCount++;
    if (g_obj.hScratch && g_obj.pScratch) {
        snprintf((char*)g_obj.pScratch, 2048, "ObjectLab scratch section #%u HWND=%u", g_obj.makeCount, g_obj.hWnd);
        snprintf(g_obj.status, sizeof(g_obj.status), "CREATE SECTION handle=0x%x mapped=YES", g_obj.hScratch);
    } else {
        snprintf(g_obj.status, sizeof(g_obj.status), "CREATE SECTION failed - CAP_SECTION_MAP?");
    }
    pthread_mutex_unlock(&g_obj.lock);
    object_refresh();
}

static void object_close_section(void)
{
    ensure_runtime();
    if (g_obj.pScratch) { UnmapViewOfFile(g_obj.pScratch); g_obj.pScratch = NULL; }
    if (g_obj.hScratch) { CloseHandle(g_obj.hScratch); g_obj.hScratch = 0; }
    pthread_mutex_lock(&g_obj.lock);
    g_obj.closeCount++;
    snprintf(g_obj.status, sizeof(g_obj.status), "CLOSE scratch section/views count=%u", g_obj.closeCount);
    pthread_mutex_unlock(&g_obj.lock);
    object_refresh();
}

static void object_post_command(UINT cmd)
{
    if (g_obj.mgr && g_obj.hWnd)
        hwnd_post(g_obj.mgr, &g_obj.cap, g_obj.hWnd, WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
}

static void object_hit_test(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8 && cx < 110) { object_post_command(OBJLAB_REFRESH); return; }
        if (cx >= 118 && cx < 270) { object_post_command(OBJLAB_MAKE_SEC); return; }
        if (cx >= 278 && cx < 430) { object_post_command(OBJLAB_CLOSE_SEC); return; }
    }
}

static void object_handle_command(UINT cmd)
{
    switch (cmd) {
    case OBJLAB_REFRESH: object_refresh(); break;
    case OBJLAB_MAKE_SEC: object_make_section(); break;
    case OBJLAB_CLOSE_SEC: object_close_section(); break;
    default: break;
    }
}

static void objectlab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, void* userdata)
{
    (void)userdata;
    (void)wp;
    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_obj.lock);
        g_obj.hWnd = hwnd;
        MyAppResizeInit(&g_obj.resize, OBJECT_W, OBJECT_H, TITLEBAR_H);
        snprintf(g_obj.status, sizeof(g_obj.status), "ObjectLab ready - v31 access checks + v32 namespace");
        pthread_mutex_unlock(&g_obj.lock);
        object_refresh();
        break;
    case WM_LBUTTONDOWN:
        object_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        break;
    case WM_COMMAND:
        object_handle_command((UINT)LOWORD(wp));
        break;
    case OBJLAB_REFRESH:
        object_handle_command(OBJLAB_REFRESH);
        break;
    case OBJLAB_MAKE_SEC:
        object_handle_command(OBJLAB_MAKE_SEC);
        break;
    case OBJLAB_CLOSE_SEC:
        object_handle_command(OBJLAB_CLOSE_SEC);
        break;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_obj.resize, lp, OBJECT_MIN_W, OBJECT_MIN_H);
        break;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_obj.resize, lp);
        break;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_obj.resize, lp, TITLEBAR_H);
        break;
    case WM_MOVE:
        MyAppResizeOnMove(&g_obj.resize, lp);
        break;
    case WM_SIZE:
        MyAppResizeOnSize(&g_obj.resize, wp, lp);
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        object_close_section();
        break;
    default:
        break;
    }
}

HWND objectlab_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    memset(&g_obj, 0, sizeof(g_obj));
    pthread_mutex_init(&g_obj.lock, NULL);
    g_obj.mgr = mgr;
    g_obj.cap = cap;
    HWND h = hwnd_create(mgr, objectlab_wndproc, NULL, cap);
    g_obj.hWnd = h;
    return h;
}

void objectlab_destroy(void)
{
    object_close_section();
    pthread_mutex_destroy(&g_obj.lock);
}

void objectlab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    (void)hwnd;
    int cx = wx + 1;
    int cy = wy + TITLEBAR_H;
    int cw = ww - 2;
    int ch = wh - TITLEBAR_H - 1;
    fb_rect(fb, cx, cy, cw, ch, COLOR(12,12,22));

    button(fb, cx + 8, cy + 8, 94, "Refresh");
    button(fb, cx + 118, cy + 8, 150, "Create Section");
    button(fb, cx + 278, cy + 8, 150, "Close Section");

    pthread_mutex_lock(&g_obj.lock);
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "PID=%u HWND=%u obj=%u sec=%u ev=%u mut=%u sem=%u tim=%u listed=%d scratch=0x%x view=%s",
             g_obj.cap.id, g_obj.hWnd, g_obj.totalObjects, g_obj.sectionObjects, g_obj.eventObjects, g_obj.mutexObjects, g_obj.semaphoreObjects, g_obj.timerObjects, g_obj.rowCount,
             g_obj.hScratch, g_obj.pScratch ? "YES" : "no");
    draw_clip_text(fb, cx + 8, cy + 40, hdr, COLOR(160,255,180), cx+8, cy+36, cw-16, 14);
    draw_clip_text(fb, cx + 8, cy + 54, g_obj.status, COLOR(220,220,245), cx+8, cy+51, cw-16, 14);

    int obj_y = cy + 76;
    int obj_h = (ch - 94) / 2;
    if (obj_h < 90) obj_h = 90;
    int hnd_y = obj_y + obj_h + 10;
    int hnd_h = ch - (hnd_y - cy) - 12;

    fb_rect(fb, cx + 8, obj_y - 4, cw - 16, obj_h, COLOR(10,10,18));
    fb_rect_outline(fb, cx + 8, obj_y - 4, cw - 16, obj_h, COLOR(70,75,105));
    font_draw_str(fb, cx + 14, obj_y, "GLOBAL OBJECTS: OBJ_HANDLE TYPE OWNER REF ACCESS FLAGS SD NS SIZE NAME", COLOR(210,230,255));
    int maxObjRows = (obj_h - 24) / 16;
    if (maxObjRows > g_obj.rowCount) maxObjRows = g_obj.rowCount;
    for (int i = 0; i < maxObjRows; i++) {
        _ObjectectInfo* r = &g_obj.rows[i];
        char line[256];
        snprintf(line, sizeof(line), "0x%04x %-8s %5u %4u 0x%04x 0x%02x 0x%02x %u %5u %.36s",
                 r->handle, _ObjectTypeName(r->type), r->owner_pid, r->ref_count, r->access_mask, r->flags, r->sd_flags, r->namespace_id, r->size, r->name);
        draw_clip_text(fb, cx + 14, obj_y + 18 + i * 16, line,
                       (r->type == _OBJECT_TYPE_EVENT || r->type == _OBJECT_TYPE_MUTEX || r->type == _OBJECT_TYPE_SEMAPHORE || r->type == _OBJECT_TYPE_TIMER) ? COLOR(255,220,150) : (r->type == _OBJECT_TYPE_SECTION ? COLOR(180,255,190) : COLOR(220,220,230)),
                       cx + 12, obj_y + 14 + i * 16, cw - 24, 14);
    }

    fb_rect(fb, cx + 8, hnd_y - 4, cw - 16, hnd_h, COLOR(8,8,16));
    fb_rect_outline(fb, cx + 8, hnd_y - 4, cw - 16, hnd_h, COLOR(90,95,125));
    font_draw_str(fb, cx + 14, hnd_y, "PROCESS HANDLE TABLES: PID HANDLE -> OBJ TYPE GRANTED FLAGS OBJREF NAME", COLOR(210,230,255));
    int maxHandleRows = (hnd_h - 24) / 16;
    if (maxHandleRows > g_obj.handleCount) maxHandleRows = g_obj.handleCount;
    for (int i = 0; i < maxHandleRows; i++) {
        MyHandleInfo* h = &g_obj.handles[i];
        char line[256];
        snprintf(line, sizeof(line), "%4u 0x%08x -> 0x%04x %-8s 0x%04x 0x%02x %5u %.32s",
                 h->pid, h->handle, h->object_handle, _ObjectTypeName(h->object_type),
                 h->granted_access, h->flags, h->object_ref, h->object_name);
        draw_clip_text(fb, cx + 14, hnd_y + 18 + i * 16, line, COLOR(170,210,255),
                       cx + 12, hnd_y + 14 + i * 16, cw - 24, 14);
    }
    pthread_mutex_unlock(&g_obj.lock);
}
