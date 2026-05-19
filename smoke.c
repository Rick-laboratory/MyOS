#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <linux/input-event-codes.h>

#include "smoke.h"
#include <windows.h>
#include <winsvc.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "myobject.h"
#include "hwnd.h"
#include "ipc.h"
#include "window.h"
#include "apphost.h"
#include "processhost.h"

extern size_t MyWinDebugWindowInfoSize(void);
extern size_t MyWinDebugWindowInfoColdBufferBytes(void);

/*
   v139 MDI command dedupe/Alt/drag reset contract on top of v137 command-origin/caption-drag checks; v104-v137 smoke checks preserved.

   This is intentionally a smoke/regression runner, not a full conformance
   suite yet. PASS means the implemented contract path still works. WARN means
   a known MSDN-compliance gap is observable but must not block this baseline.
*/

typedef struct SmokeContext {
    int checks;
    int passed;
    int failed;
    int warnings;
    const char* group;
} SmokeContext;

typedef struct SmokeRuntime {
    HWNDManager mgr;
    IPCBus bus;
    WindowManager wm;
    Capability cap;
    int initialized;
} SmokeRuntime;

static unsigned long long smoke_perf_now_us(void);

static uint32_t smoke_v235_hwnd_bucket(HWND hwnd)
{
    uint32_t v = (uint32_t)hwnd;
    v ^= v >> MYQUEUE_HWND_BUCKET_BITS;
    v ^= v >> (MYQUEUE_HWND_BUCKET_BITS * 2u);
    return v & MYQUEUE_HWND_BUCKET_MASK;
}

static uint32_t smoke_v235_msg_bucket(UINT msg)
{
    return ((uint32_t)msg) & MYQUEUE_MSG_BUCKET_MASK;
}

static int smoke_v235_bucket_bit_is_set(uint64_t words[MYQUEUE_SLOT_WORDS], int slot)
{
    if (slot < 0 || slot >= MYQUEUE_CAP) return 0;
    return (words[slot >> 6] & (1ull << (uint32_t)(slot & 63))) ? 1 : 0;
}

static int smoke_streq(const char* a, const char* b)
{
    return a && b && strcmp(a, b) == 0;
}

static void smoke_line(const char* kind, const char* group, const char* name, const char* detail)
{
    printf("[%s] %-9s %s%s%s\n", kind, group ? group : "smoke", name ? name : "(unnamed)",
           (detail && detail[0]) ? " :: " : "", (detail && detail[0]) ? detail : "");
}

static int smoke_expect(SmokeContext* s, int cond, const char* name, const char* detail)
{
    if (!s) return cond;
    s->checks++;
    if (cond) {
        s->passed++;
        smoke_line("PASS", s->group, name, detail);
        return 1;
    }
    s->failed++;
    smoke_line("FAIL", s->group, name, detail);
    return 0;
}

static void __attribute__((unused)) smoke_warn(SmokeContext* s, const char* name, const char* detail)
{
    if (!s) return;
    s->warnings++;
    smoke_line("WARN", s->group, name, detail);
}

static void smoke_info(const char* group, const char* name, const char* detail)
{
    smoke_line("INFO", group, name, detail);
}

static void smoke_unique_name(char* out, size_t cb, const char* suffix)
{
    if (!out || cb == 0) return;
    unsigned long long ticks = (unsigned long long)time(NULL);
    snprintf(out, cb, "Global\\myos.v129.smoke.%lu.%llu.%s", (unsigned long)getpid(), ticks, suffix ? suffix : "object");
}

static void smoke_runtime_init(SmokeRuntime* rt)
{
    if (!rt || rt->initialized) return;
    memset(rt, 0, sizeof(*rt));

    _ObjectInit();
    MySvcInit();
    hwnd_manager_init(&rt->mgr);
    ipc_init(&rt->bus);
    wm_init(&rt->wm, &rt->mgr, &rt->bus);
    rt->wm.screen_w = 1280;
    rt->wm.screen_h = 800;

    rt->cap = cap_create(104, "smoke-admin", CAP_ADMIN);
    cap_add_path(&rt->cap, "/tmp");
    cap_add_path(&rt->cap, ".");
    cap_add_target(&rt->cap, 0);
    MyWinBindRuntime(&rt->mgr, &rt->cap);
    MyWinBindDesktop(&rt->wm);

    rt->initialized = 1;
}

static void smoke_runtime_destroy(SmokeRuntime* rt)
{
    if (!rt || !rt->initialized) return;
    MyWinUnbindRuntime();
    hwnd_manager_destroy(&rt->mgr);
    rt->initialized = 0;
}

static int smoke_expect_last_error(SmokeContext* s, DWORD expected, const char* api)
{
    DWORD got = GetLastError();
    char detail[128];
    snprintf(detail, sizeof(detail), "GetLastError expected %u, got %u", (unsigned)expected, (unsigned)got);
    return smoke_expect(s, got == expected, api, detail);
}


static LPARAM smoke_mouse_lparam_for_hwnd(HWND hwnd, int sx, int sy)
{
    POINT pt;
    pt.x = sx;
    pt.y = sy;
    if (!ScreenToClient(hwnd, &pt)) {
        pt.x = sx;
        pt.y = sy;
    }
    return MAKELPARAM((WORD)pt.x, (WORD)pt.y);
}

typedef struct SmokeRawDragThreadCtx {
    SmokeRuntime* rt;
    int sx;
    int sy;
    int mx;
    int my;
    int downOk;
    int moveOk;
    int upOk;
} SmokeRawDragThreadCtx;

static void* smoke_raw_drag_thread_no_runtime(void* arg)
{
    SmokeRawDragThreadCtx* ctx = (SmokeRawDragThreadCtx*)arg;
    if (!ctx || !ctx->rt) return NULL;
    /* Do not call MyWinBindRuntime() here.  This simulates the real evdev
       input thread before v145: fresh TLS, no USER32 runtime identity. */
    ctx->downOk = wm_route_raw_mouse_button_down(&ctx->rt->wm, ctx->sx, ctx->sy, 0);
    ctx->moveOk = wm_mouse_move(&ctx->rt->wm, ctx->mx, ctx->my);
    ctx->upOk = wm_route_raw_mouse_button_up(&ctx->rt->wm, ctx->mx, ctx->my, 0);
    return NULL;
}

static void* smoke_raw_drag_thread_weak_runtime(void* arg)
{
    SmokeRawDragThreadCtx* ctx = (SmokeRawDragThreadCtx*)arg;
    if (!ctx || !ctx->rt) return NULL;

    /* v146: the guard must not only handle empty TLS.  A thread may already be
       bound to a normal app/runtime capability that lacks CAP_WINDOW_READ or
       CAP_WINDOW_CONTROL.  Raw compositor input still needs the session broker
       contract and must upgrade before MDI hit-testing. */
    Capability weak = cap_create(79, "weak-input", CAP_IPC);
    cap_add_target(&weak, 0);
    MyWinBindRuntime(&ctx->rt->mgr, &weak);
    MyWinBindDesktop(&ctx->rt->wm);

    ctx->downOk = wm_route_raw_mouse_button_down(&ctx->rt->wm, ctx->sx, ctx->sy, 0);
    ctx->moveOk = wm_mouse_move(&ctx->rt->wm, ctx->mx, ctx->my);
    ctx->upOk = wm_route_raw_mouse_button_up(&ctx->rt->wm, ctx->mx, ctx->my, 0);
    return NULL;
}

static void smoke_raw_left_click(SmokeRuntime* rt, int sx, int sy)
{
    if (!rt) return;
    int handled = wm_route_raw_mouse_button_down(&rt->wm, sx, sy, 0);
    if (!handled) {
        HWND target = 0;
        Capability* cap = NULL;
        if (wm_client_endpoint_at_focus(&rt->wm, sx, sy, &target, &cap) && target && cap) {
            hwnd_post(&rt->mgr, cap, target, WM_LBUTTONDOWN, MK_LBUTTON,
                      smoke_mouse_lparam_for_hwnd(target, sx, sy));
        }
    }
    hwnd_dispatch(&rt->mgr);

    handled = wm_route_raw_mouse_button_up(&rt->wm, sx, sy, 0);
    if (!handled) {
        HWND target = 0;
        Capability* cap = NULL;
        if (wm_client_endpoint_at_focus(&rt->wm, sx, sy, &target, &cap) && target && cap) {
            hwnd_post(&rt->mgr, cap, target, WM_LBUTTONUP, 0,
                      smoke_mouse_lparam_for_hwnd(target, sx, sy));
        }
    }
    hwnd_dispatch(&rt->mgr);
}

static int smoke_kernel32(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "kernel32";
    smoke_runtime_init(rt);

    char nameEvent[128], nameSection[128], nameMutex[128], nameSem[128], nameTimer[128];
    smoke_unique_name(nameEvent, sizeof(nameEvent), "event");
    smoke_unique_name(nameSection, sizeof(nameSection), "section");
    smoke_unique_name(nameMutex, sizeof(nameMutex), "mutex");
    smoke_unique_name(nameSem, sizeof(nameSem), "sem");
    smoke_unique_name(nameTimer, sizeof(nameTimer), "timer");

    HANDLE hEvent = CreateEventA(NULL, TRUE, FALSE, nameEvent);
    smoke_expect(&s, hEvent != 0, "CreateEventA(manual, nonsignaled, named)", nameEvent);
    smoke_expect(&s, hEvent && WaitForSingleObject(hEvent, 0) == WAIT_TIMEOUT,
                 "WaitForSingleObject(event,0) timeout", "manual-reset event starts nonsignaled");
    smoke_expect(&s, hEvent && SetEvent(hEvent), "SetEvent", "signal named event");
    smoke_expect(&s, hEvent && WaitForSingleObject(hEvent, 0) == WAIT_OBJECT_0,
                 "WaitForSingleObject(event,0) signaled", "returns WAIT_OBJECT_0");
    smoke_expect(&s, hEvent && ResetEvent(hEvent), "ResetEvent", "manual-reset event reset");
    smoke_expect(&s, hEvent && WaitForSingleObject(hEvent, 0) == WAIT_TIMEOUT,
                 "WaitForSingleObject(event,0) after reset", "returns WAIT_TIMEOUT");

    HANDLE hOpen = OpenEventA(EVENT_ALL_ACCESS, FALSE, nameEvent);
    smoke_expect(&s, hOpen != 0, "OpenEventA(EVENT_ALL_ACCESS)", nameEvent);

    HANDLE hDup = 0;
    smoke_expect(&s, hOpen && DuplicateHandle(GetCurrentProcess(), hOpen, GetCurrentProcess(), &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS) && hDup != 0,
                 "DuplicateHandle(DUPLICATE_SAME_ACCESS)", "same-process event handle duplication");

    char nameCollision[128];
    smoke_unique_name(nameCollision, sizeof(nameCollision), "named_collision");
    HANDLE hCollisionEvent = CreateEventA(NULL, TRUE, FALSE, nameCollision);
    SetLastError(0x12345678u);
    HANDLE hCollisionMutex = CreateMutexA(NULL, FALSE, nameCollision);
    smoke_expect(&s, hCollisionEvent && !hCollisionMutex,
                 "v250 named directory rejects cross-type collision",
                 "Event name cannot be re-created as Mutex in one global Object Namespace");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "CreateMutexA(event-name collision) LastError");
    if (hCollisionMutex) CloseHandle(hCollisionMutex);
    if (hCollisionEvent) CloseHandle(hCollisionEvent);
    HANDLE hCollisionMutexAfterClose = CreateMutexA(NULL, FALSE, nameCollision);
    smoke_expect(&s, hCollisionMutexAfterClose != 0,
                 "v250 named directory releases name on last close",
                 "same name can be reused for another object type after the old object is destroyed");
    if (hCollisionMutexAfterClose) CloseHandle(hCollisionMutexAfterClose);

    HANDLE hMutex = CreateMutexA(NULL, FALSE, nameMutex);
    smoke_expect(&s, hMutex != 0, "CreateMutexA(named)", nameMutex);
    smoke_expect(&s, hMutex && WaitForSingleObject(hMutex, 0) == WAIT_OBJECT_0,
                 "WaitForSingleObject(mutex,0)", "acquires unowned mutex");
    smoke_expect(&s, hMutex && ReleaseMutex(hMutex), "ReleaseMutex", "release acquired mutex");

    HANDLE hSem = CreateSemaphoreA(NULL, 1, 2, nameSem);
    smoke_expect(&s, hSem != 0, "CreateSemaphoreA(named,1,2)", nameSem);
    smoke_expect(&s, hSem && WaitForSingleObject(hSem, 0) == WAIT_OBJECT_0,
                 "WaitForSingleObject(semaphore,0) first", "consumes count 1 -> 0");
    smoke_expect(&s, hSem && WaitForSingleObject(hSem, 0) == WAIT_TIMEOUT,
                 "WaitForSingleObject(semaphore,0) second", "count is now 0");
    LONG prev = -1;
    smoke_expect(&s, hSem && ReleaseSemaphore(hSem, 1, &prev) && prev == 0,
                 "ReleaseSemaphore(+1)", "previous count should be 0");

    HANDLE waitAny[2] = { hEvent, hSem };
    smoke_expect(&s, hEvent && hSem && WaitForMultipleObjects(2, waitAny, FALSE, 0) == WAIT_OBJECT_0 + 1,
                 "WaitForMultipleObjects(WaitAny)", "event reset, semaphore signaled => index 1");
    smoke_expect(&s, hEvent && hSem && WaitForMultipleObjects(2, waitAny, TRUE, 0) == WAIT_TIMEOUT,
                 "WaitForMultipleObjects(WaitAll) timeout", "one object still nonsignaled");
    SetEvent(hEvent);
    ReleaseSemaphore(hSem, 1, NULL);
    smoke_expect(&s, hEvent && hSem && WaitForMultipleObjects(2, waitAny, TRUE, 20) == WAIT_OBJECT_0,
                 "WaitForMultipleObjects(WaitAll)", "both objects signaled");

    HANDLE hTimer = CreateWaitableTimerA(NULL, TRUE, nameTimer);
    smoke_expect(&s, hTimer != 0, "CreateWaitableTimerA(manual,named)", nameTimer);
    LARGE_INTEGER due;
    due.QuadPart = -10000LL; /* approximately 1 ms relative in Win32 units; myOS treats it as due soon. */
    smoke_expect(&s, hTimer && SetWaitableTimer(hTimer, &due, 0, NULL, NULL, FALSE),
                 "SetWaitableTimer", "relative due time");
    smoke_expect(&s, hTimer && WaitForSingleObject(hTimer, 50) == WAIT_OBJECT_0,
                 "WaitForSingleObject(timer,50)", "timer becomes signaled");

    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, nameSection);
    smoke_expect(&s, hMap != 0, "CreateFileMappingA(PAGE_READWRITE,named)", nameSection);
    char* view = hMap ? (char*)MapViewOfFile(hMap, FILE_MAP_READ|FILE_MAP_WRITE, 0, 0, 4096) : NULL;
    smoke_expect(&s, view != NULL, "MapViewOfFile(READ|WRITE)", "map entire section");
    if (view) {
        strcpy(view, "v130 section smoke ok");
        smoke_expect(&s, strcmp(view, "v130 section smoke ok") == 0,
                     "section roundtrip", "write/read through mapped view");
    }
    HANDLE hOpenMap = OpenFileMappingA(FILE_MAP_READ, FALSE, nameSection);
    smoke_expect(&s, hOpenMap != 0, "OpenFileMappingA(FILE_MAP_READ)", nameSection);

    SetLastError(0x12345678u);
    BOOL badClose = CloseHandle(0);
    smoke_expect(&s, badClose == FALSE, "CloseHandle((HANDLE)0) returns FALSE", "MSDN invalid handle path");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "CloseHandle(NULL) LastError");

    SetLastError(0x12345678u);
    DWORD badWait = WaitForSingleObject(0, 0);
    smoke_expect(&s, badWait == WAIT_FAILED, "WaitForSingleObject(NULL) returns WAIT_FAILED", "MSDN invalid handle path");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "WaitForSingleObject(NULL) LastError");

    if (view) UnmapViewOfFile(view);
    if (hOpenMap) CloseHandle(hOpenMap);
    if (hMap) CloseHandle(hMap);
    if (hTimer) CloseHandle(hTimer);
    if (hSem) CloseHandle(hSem);
    if (hMutex) CloseHandle(hMutex);
    if (hDup) CloseHandle(hDup);
    if (hOpen) CloseHandle(hOpen);
    if (hEvent) CloseHandle(hEvent);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int g_user32_created = 0;
static int g_user32_destroyed = 0;
static int g_user32_command = 0;
static int g_user32_user = 0;
static int g_user32_capture_changed = 0;
static int g_user32_windowpos_changing = 0;
static int g_user32_windowpos_changed = 0;
static int g_user32_move = 0;
static int g_user32_size = 0;
static int g_user32_setfocus = 0;
static int g_user32_killfocus = 0;
static int g_user32_enable = 0;
static WPARAM g_user32_last_focus_from = 0;
static WPARAM g_user32_last_kill_to = 0;
static WPARAM g_user32_last_enable = 0;
static LPARAM g_user32_last_capture_lparam = 0;
static int g_user32_initmenu = 0;
static int g_user32_initmenu_popup = 0;
static int g_user32_uninitmenu_popup = 0;
static int g_user32_menuselect = 0;
static int g_user32_enter_menu_loop = 0;
static int g_user32_exit_menu_loop = 0;
static int g_user32_measureitem = 0;
static int g_user32_drawitem = 0;
static int g_user32_accel_command = 0;
static int g_user32_syscommand = 0;
static WPARAM g_user32_last_command = 0;
static WPARAM g_user32_last_syscommand = 0;
static WPARAM g_user32_last_menuselect_wparam = 0;
static LPARAM g_user32_last_menuselect_lparam = 0;
static WPARAM g_user32_last_initmenu_wparam = 0;
static WPARAM g_user32_last_initpopup_wparam = 0;
static WPARAM g_user32_last_uninitpopup_wparam = 0;
static WPARAM g_user32_last_exit_menu_wparam = 0;
static LPARAM g_user32_last_command_lparam = 0;
static int g_user32_hscroll = 0;
static int g_user32_vscroll = 0;
static WPARAM g_user32_last_scroll_wparam = 0;
static LPARAM g_user32_last_scroll_lparam = 0;
static int g_user32_timer = 0;
static int g_user32_paint = 0;
static WPARAM g_user32_last_timer_id = 0;
static LPARAM g_user32_last_timer_lparam = 0;
static int g_user32_timerproc = 0;
static UINT_PTR g_user32_last_timerproc_id = 0;
static HWND g_user32_last_timerproc_hwnd = 0;
static int g_user32_erase_true = 0;
static int g_user32_erase_false = 0;
static HDC g_user32_last_erase_hdc = 0;

static HWND g_mdi_smoke_client = 0;
static int g_mdi_child_create = 0;
static int g_mdi_child_destroy = 0;
static int g_mdi_child_activate = 0;
static int g_mdi_child_childactivate = 0;
static int g_mdi_child_command = 0;
static int g_mdi_frame_command = 0;
static WPARAM g_mdi_last_command = 0;

static LRESULT CALLBACK smoke_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_CREATE: g_user32_created++; return 0;
    case WM_WINDOWPOSCHANGING: g_user32_windowpos_changing++; return 0;
    case WM_WINDOWPOSCHANGED: g_user32_windowpos_changed++; return 0;
    case WM_MOVE: g_user32_move++; return 0;
    case WM_SIZE: g_user32_size++; return 0;
    case WM_SETFOCUS:
        g_user32_setfocus++;
        g_user32_last_focus_from = wParam;
        return 0;
    case WM_KILLFOCUS:
        g_user32_killfocus++;
        g_user32_last_kill_to = wParam;
        return 0;
    case WM_ENABLE:
        g_user32_enable++;
        g_user32_last_enable = wParam;
        return 0;
    case WM_COMMAND:
        g_user32_command++;
        g_user32_last_command = wParam;
        g_user32_last_command_lparam = lParam;
        if (wParam == 0x7701u || LOWORD(wParam) == 0x7701u) g_user32_accel_command++;
        return 0;
    case WM_SYSCOMMAND:
        g_user32_syscommand++;
        g_user32_last_syscommand = wParam;
        return 0;
    case WM_HSCROLL:
        g_user32_hscroll++;
        g_user32_last_scroll_wparam = wParam;
        g_user32_last_scroll_lparam = lParam;
        return 0;
    case WM_VSCROLL:
        g_user32_vscroll++;
        g_user32_last_scroll_wparam = wParam;
        g_user32_last_scroll_lparam = lParam;
        return 0;
    case WM_CAPTURECHANGED: g_user32_capture_changed++; g_user32_last_capture_lparam = lParam; return 0;
    case WM_ENTERMENULOOP: g_user32_enter_menu_loop++; return 0;
    case WM_EXITMENULOOP: g_user32_exit_menu_loop++; g_user32_last_exit_menu_wparam = wParam; return 0;
    case WM_INITMENU: g_user32_initmenu++; g_user32_last_initmenu_wparam = wParam; return 0;
    case WM_INITMENUPOPUP: g_user32_initmenu_popup++; g_user32_last_initpopup_wparam = wParam; return 0;
    case WM_UNINITMENUPOPUP: g_user32_uninitmenu_popup++; g_user32_last_uninitpopup_wparam = wParam; return 0;
    case WM_MENUSELECT:
        g_user32_menuselect++;
        g_user32_last_menuselect_wparam = wParam;
        g_user32_last_menuselect_lparam = lParam;
        return 0;
    case WM_MEASUREITEM: g_user32_measureitem++; return TRUE;
    case WM_DRAWITEM: g_user32_drawitem++; return TRUE;
    case WM_PAINT: {
        g_user32_paint++;
        PAINTSTRUCT ps;
        memset(&ps, 0, sizeof(ps));
        HDC hdc = BeginPaint(hWnd, &ps);
        if (hdc) EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_TIMER:
        g_user32_timer++;
        g_user32_last_timer_id = wParam;
        g_user32_last_timer_lparam = lParam;
        return 0;
    case WM_USER + 104: g_user32_user++; return 104;
    case WM_DESTROY: g_user32_destroyed++; return 0;
    default: return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static LRESULT CALLBACK smoke_erase_true_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_ERASEBKGND:
        g_user32_erase_true++;
        g_user32_last_erase_hdc = (HDC)wParam;
        return TRUE;
    case WM_PAINT: {
        g_user32_paint++;
        PAINTSTRUCT ps;
        memset(&ps, 0, sizeof(ps));
        HDC hdc = BeginPaint(hWnd, &ps);
        if (hdc) EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static LRESULT CALLBACK smoke_erase_false_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_ERASEBKGND:
        g_user32_erase_false++;
        g_user32_last_erase_hdc = (HDC)wParam;
        return FALSE;
    case WM_PAINT: {
        g_user32_paint++;
        PAINTSTRUCT ps;
        memset(&ps, 0, sizeof(ps));
        HDC hdc = BeginPaint(hWnd, &ps);
        if (hdc) EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static void CALLBACK smoke_timerproc(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    (void)dwTime;
    if (uMsg == WM_TIMER) {
        g_user32_timerproc++;
        g_user32_last_timerproc_id = idEvent;
        g_user32_last_timerproc_hwnd = hWnd;
    }
}

static LRESULT CALLBACK smoke_mdi_frame_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_COMMAND:
        g_mdi_frame_command++;
        return DefFrameProcA(hWnd, g_mdi_smoke_client, Msg, wParam, lParam);
    default:
        return DefFrameProcA(hWnd, g_mdi_smoke_client, Msg, wParam, lParam);
    }
}

static LRESULT CALLBACK smoke_mdi_child_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_CREATE:
        g_mdi_child_create++;
        return 0;
    case WM_CHILDACTIVATE:
        g_mdi_child_childactivate++;
        return 0;
    case WM_MDIACTIVATE:
        g_mdi_child_activate++;
        return 0;
    case WM_COMMAND:
        g_mdi_child_command++;
        g_mdi_last_command = wParam;
        if (LOWORD(wParam) == 0x7101u) return 0xBEEFu;
        return 0;
    case WM_DESTROY:
        g_mdi_child_destroy++;
        return 0;
    default:
        return DefMDIChildProcA(hWnd, Msg, wParam, lParam);
    }
}

static int smoke_user32(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "user32";
    smoke_runtime_init(rt);

    g_user32_created = g_user32_destroyed = g_user32_command = g_user32_user = 0;
    g_user32_capture_changed = g_user32_initmenu = g_user32_initmenu_popup = g_user32_uninitmenu_popup = 0;
    g_user32_menuselect = g_user32_enter_menu_loop = g_user32_exit_menu_loop = 0;
    g_user32_measureitem = g_user32_drawitem = g_user32_accel_command = g_user32_syscommand = 0;
    g_user32_windowpos_changing = g_user32_windowpos_changed = g_user32_move = g_user32_size = 0;
    g_user32_setfocus = g_user32_killfocus = g_user32_enable = 0;
    g_user32_last_focus_from = g_user32_last_kill_to = g_user32_last_enable = 0;
    g_user32_last_capture_lparam = 0;
    g_user32_last_command = 0;
    g_user32_last_command_lparam = 0;
    g_user32_hscroll = g_user32_vscroll = 0;
    g_user32_last_scroll_wparam = g_user32_last_scroll_lparam = 0;
    g_user32_timer = g_user32_timerproc = g_user32_paint = 0;
    g_user32_last_timer_id = 0;
    g_user32_last_timer_lparam = 0;
    g_user32_last_timerproc_id = 0;
    g_user32_last_timerproc_hwnd = 0;
    g_user32_last_syscommand = g_user32_last_menuselect_wparam = g_user32_last_menuselect_lparam = 0;
    g_user32_last_initmenu_wparam = g_user32_last_initpopup_wparam = g_user32_last_uninitpopup_wparam = 0;
    g_user32_last_exit_menu_wparam = 0;

    smoke_expect(&s, sizeof(MSG) <= 64, "MSG public ABI size", "public MSG no longer embeds MyMessage/private queue payload");

    size_t v243WindowInfoSize = MyWinDebugWindowInfoSize();
    size_t v243ColdBytes = MyWinDebugWindowInfoColdBufferBytes();
    smoke_expect(&s, v243WindowInfoSize <= 512,
                 "v243 WindowInfo hot/cold stride bound",
                 "pointer-backed text/listbox payload keeps USER32 WindowInfo traversal under eight cachelines");
    char v243Layout[192];
    snprintf(v243Layout, sizeof(v243Layout),
             "WindowInfo_stride=%zu out_of_line_cold_bytes=%zu cacheline=%u",
             v243WindowInfoSize, v243ColdBytes, (unsigned)MYOS_CACHELINE_BYTES);
    smoke_info(s.group, "v243 WindowInfo hot/cold layout", v243Layout);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_wndproc;
    wc.lpszClassName = "myOS.v129.SmokeWindow";
    ATOM atom = RegisterClassExA(&wc);
    smoke_expect(&s, atom != 0, "RegisterClassExA", wc.lpszClassName);

    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, "Smoke USER32", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                      40, 40, 240, 160, 0, 0, 0, NULL) : 0;
    smoke_expect(&s, hwnd != 0, "CreateWindowExA", "top-level smoke window");
    smoke_expect(&s, hwnd && IsWindow(hwnd), "IsWindow", "new HWND is valid");

    _HwndHeader hwndHdr;
    DWORD hwndSlot = 0, hwndGen = 0;
    memset(&hwndHdr, 0, sizeof(hwndHdr));
    BOOL hwndHeaderOk = hwnd && hwnd_decode(hwnd, &hwndSlot, &hwndGen) &&
                        hwnd_query_header(&rt->mgr, hwnd, &hwndHdr) &&
                        hwndHdr.hwnd_slot == hwndSlot &&
                        hwndHdr.hwnd_generation == hwndGen &&
                        hwndHdr.hwnd_state == _HWND_STATE_LIVE &&
                        hwndHdr.owner_pid == rt->cap.id;
    smoke_expect(&s, hwndHeaderOk,
                 "v219 HWND header uses slot/generation/state nomenclature",
                 "HWND resolves to a USER object header before later USER32 dispatch");

    int hwndDispatchOk = hwndHeaderOk;
    enum { V219_HWND_HEADER_PROBES = 4096 };
    unsigned long long v219Start = smoke_perf_now_us();
    for (int probe = 0; hwndDispatchOk && probe < V219_HWND_HEADER_PROBES; ++probe) {
        _HwndHeader h;
        if (!hwnd_query_header(&rt->mgr, hwnd, &h) ||
            h.hwnd_slot != hwndSlot || h.hwnd_generation != hwndGen ||
            h.hwnd_state != _HWND_STATE_LIVE)
            hwndDispatchOk = 0;
    }
    unsigned long long v219End = smoke_perf_now_us();
    smoke_expect(&s, hwndDispatchOk,
                 "v219 HWND -> WindowHeader direct dispatch",
                 "encoded HWND indexes the window slot and validates generation/state before action dispatch");
    char v219HwndBench[256];
    double v219Ms = (double)(v219End - v219Start) / 1000.0;
    double v219OpsS = v219Ms > 0.0 ? ((double)V219_HWND_HEADER_PROBES * 1000.0 / v219Ms) : 0.0;
    snprintf(v219HwndBench, sizeof(v219HwndBench),
             "probes=%d wall_ms=%.3f ops_s=%.0f hwnd=0x%x slot=%lu generation=%lu state=%lu",
             V219_HWND_HEADER_PROBES, v219Ms, v219OpsS, (unsigned)hwnd,
             (unsigned long)hwndSlot, (unsigned long)hwndGen,
             (unsigned long)_HWND_STATE_LIVE);
    smoke_info(s.group, "v219 HWND header dispatch benchmark", v219HwndBench);

    _HwndHeader v221ActionHeader;
    BOOL v221ActionOk = hwnd_query_action(&rt->mgr, hwnd, _HWND_ACTION_SHOW, &v221ActionHeader) &&
                        v221ActionHeader.hwnd_state == _HWND_STATE_LIVE &&
                        hwnd_state_allows(_HWND_STATE_LIVE, _HWND_ACTION_FOCUS) &&
                        hwnd_state_allows(_HWND_STATE_DESTROY_PENDING, _HWND_ACTION_MESSAGE) &&
                        !hwnd_state_allows(_HWND_STATE_DESTROY_PENDING, _HWND_ACTION_SHOW) &&
                        !hwnd_state_allows(_HWND_STATE_ZOMBIE, _HWND_ACTION_QUERY);
    smoke_expect(&s, v221ActionOk,
                 "v221 HWND state-machine action table",
                 "resolved HWND state selects legal query/message/show/focus/destroy action classes");

    int v221DispatchOk = v221ActionOk;
    enum { V221_HWND_ACTION_PROBES = 4096 };
    unsigned long long v221Start = smoke_perf_now_us();
    for (int probe = 0; v221DispatchOk && probe < V221_HWND_ACTION_PROBES; ++probe) {
        _HwndHeader h;
        if (!hwnd_query_action(&rt->mgr, hwnd, _HWND_ACTION_SHOW, &h) ||
            h.hwnd_slot != hwndSlot || h.hwnd_generation != hwndGen ||
            h.hwnd_state != _HWND_STATE_LIVE)
            v221DispatchOk = 0;
    }
    unsigned long long v221End = smoke_perf_now_us();
    smoke_expect(&s, v221DispatchOk,
                 "v221 HWND action resolve dispatch",
                 "HWND resolves once to state+action context before USER32 Show/Focus/Message paths");
    char v221HwndBench[256];
    double v221Ms = (double)(v221End - v221Start) / 1000.0;
    double v221OpsS = v221Ms > 0.0 ? ((double)V221_HWND_ACTION_PROBES * 1000.0 / v221Ms) : 0.0;
    snprintf(v221HwndBench, sizeof(v221HwndBench),
             "probes=%d wall_ms=%.3f ops_s=%.0f hwnd=0x%x state=%lu action=SHOW",
             V221_HWND_ACTION_PROBES, v221Ms, v221OpsS, (unsigned)hwnd,
             (unsigned long)_HWND_STATE_LIVE);
    smoke_info(s.group, "v221 HWND action resolve benchmark", v221HwndBench);

    _HwndHeader v222Header;
    BOOL v222TableOk = hwnd_state_allows(_HWND_STATE_LIVE, _HWND_ACTION_PAINT) &&
                       hwnd_state_allows(_HWND_STATE_LIVE, _HWND_ACTION_CAPTURE) &&
                       hwnd_state_allows(_HWND_STATE_LIVE, _HWND_ACTION_GEOMETRY) &&
                       hwnd_state_allows(_HWND_STATE_LIVE, _HWND_ACTION_HITTEST) &&
                       !hwnd_state_allows(_HWND_STATE_DESTROY_PENDING, _HWND_ACTION_PAINT) &&
                       hwnd_query_action(&rt->mgr, hwnd, _HWND_ACTION_GEOMETRY, &v222Header) &&
                       v222Header.hwnd_slot == hwndSlot &&
                       v222Header.hwnd_generation == hwndGen;
    smoke_expect(&s, v222TableOk,
                 "v222 HWND paint/input/geometry action table",
                 "LIVE HWNDs expose PAINT/CAPTURE/GEOMETRY/HITTEST as state-machine actions; destroying HWNDs do not");

    POINT v222Pt = { 100, 100 };
    BOOL v222CoordOk = ScreenToClient(hwnd, &v222Pt);
    smoke_expect(&s, v222CoordOk,
                 "v222 ScreenToClient uses geometry action resolve",
                 "coordinate conversion enters through HWND state-machine GEOMETRY action");

    RECT v222Dirty = { 1, 2, 32, 24 };
    PAINTSTRUCT v222Ps;
    memset(&v222Ps, 0, sizeof(v222Ps));
    BOOL v222PaintOk = InvalidateRect(hwnd, &v222Dirty, FALSE) &&
                       BeginPaint(hwnd, &v222Ps) != 0 &&
                       v222Ps.hdc != 0 &&
                       EndPaint(hwnd, &v222Ps);
    smoke_expect(&s, v222PaintOk,
                 "v222 GDI paint entrypoints use HWND PAINT action resolve",
                 "InvalidateRect/BeginPaint/EndPaint reject dead states through USER32 action state instead of ad-hoc IsWindow checks");

    g_user32_capture_changed = 0;
    HWND oldCap = SetCapture(hwnd);
    BOOL v222CaptureOk = (GetCapture() == hwnd) && ReleaseCapture() && (GetCapture() == 0);
    (void)oldCap;
    smoke_expect(&s, v222CaptureOk,
                 "v222 capture enters through HWND CAPTURE action resolve",
                 "SetCapture/ReleaseCapture consume resolved live HWND state and preserve MSDN capture behavior");

    HWND v222Child = CreateWindowExA(0, wc.lpszClassName, "child", WS_CHILD|WS_VISIBLE,
                                    10, 12, 40, 30, hwnd, 0, 0, NULL);
    POINT v222Hit = { 12, 14 };
    HWND v222HitHwnd = v222Child ? ChildWindowFromPoint(hwnd, v222Hit) : 0;
    smoke_expect(&s, v222Child && v222HitHwnd == v222Child,
                 "v222 hit-test enters through HWND HITTEST action resolve",
                 "ChildWindowFromPoint resolves parent and child state before visual hit dispatch");

    RECT v236Client;
    memset(&v236Client, 0, sizeof(v236Client));
    BOOL v236ClientOk = v222Child && GetClientRect(v222Child, &v236Client) &&
                        v236Client.left == 0 && v236Client.top == 0 &&
                        v236Client.right == 40 && v236Client.bottom == 30;
    smoke_expect(&s, v236ClientOk,
                 "v236 GetClientRect exposes USER32 client geometry",
                 "GetClientRect enters through HWND GEOMETRY action and returns client-origin size");

    POINT v236Round = { 5, 6 };
    BOOL v236RoundOk = v222Child && ClientToScreen(v222Child, &v236Round) &&
                       ScreenToClient(v222Child, &v236Round) &&
                       v236Round.x == 5 && v236Round.y == 6;
    smoke_expect(&s, v236RoundOk,
                 "v236 ClientToScreen/ScreenToClient roundtrip",
                 "both coordinate APIs share the same USER32 client-origin resolver");

    POINT v236Map = { 1, 2 };
    BOOL v236MapOk = v222Child && MapWindowPoints(v222Child, hwnd, &v236Map, 1) != 0 &&
                     v236Map.x == 11 && v236Map.y == 14;
    smoke_expect(&s, v236MapOk,
                 "v236 MapWindowPoints uses shared client-origin mapping",
                 "child client coordinates map into parent client coordinates without duplicate geometry branches");

    HWND v236Disabled = CreateWindowExA(0, wc.lpszClassName, "disabled-child", WS_CHILD|WS_VISIBLE|WS_DISABLED,
                                        10, 12, 40, 30, hwnd, 0, 0, NULL);
    POINT v236Hit = { 12, 14 };
    HWND v236All = v236Disabled ? ChildWindowFromPointEx(hwnd, v236Hit, CWP_ALL) : 0;
    HWND v236SkipDisabled = v236Disabled ? ChildWindowFromPointEx(hwnd, v236Hit, CWP_SKIPDISABLED|CWP_SKIPINVISIBLE) : 0;
    smoke_expect(&s, v236Disabled && v236All == v236Disabled && v236SkipDisabled == v222Child,
                 "v236 ChildWindowFromPointEx honors skip flags",
                 "hit-test API surface uses one table-like child scan path with exact CWP flag masks");

    POINT v236ScreenHit = v222Hit;
    HWND v236WindowFromPoint = 0;
    if (ClientToScreen(hwnd, &v236ScreenHit))
        v236WindowFromPoint = WindowFromPoint(v236ScreenHit);
    smoke_expect(&s, v236WindowFromPoint == v236Disabled || v236WindowFromPoint == v222Child,
                 "v236 WindowFromPoint enters HITTEST action path",
                 "screen-coordinate hit-test resolves top-level/client origin before child scan");

    HMODULE v237Module = GetModuleHandleA(NULL);
    BOOL v237ExportOk =
        v237Module &&
        GetProcAddress(v237Module, "GetWindowRect") &&
        GetProcAddress(v237Module, "ScreenToClient") &&
        GetProcAddress(v237Module, "SetWindowPos") &&
        GetProcAddress(v237Module, "GetParent") &&
        GetProcAddress(v237Module, "GetAncestor") &&
        GetProcAddress(v237Module, "GetWindow") &&
        GetProcAddress(v237Module, "FindWindowExA") &&
        GetProcAddress(v237Module, "EnumChildWindows") &&
        GetProcAddress(v237Module, "GetClassLongPtrA") &&
        GetProcAddress(v237Module, "GetWindowLongPtrA");
    smoke_expect(&s, v237ExportOk,
                 "v237 WinUser geometry/window APIs are virtual exports",
                 "loader/apphost GetProcAddress sees the USER32 surface already implemented in winuser.c");

    HWND v237Sibling = CreateWindowExA(0, wc.lpszClassName, "v237-sibling", WS_CHILD|WS_VISIBLE,
                                       70, 12, 38, 28, hwnd, 0, 0, NULL);
    BOOL v237FindOk = v222Child && v236Disabled && v237Sibling &&
                      FindWindowA(wc.lpszClassName, "Smoke USER32") == hwnd &&
                      FindWindowExA(hwnd, 0, wc.lpszClassName, "v237-sibling") == v237Sibling &&
                      FindWindowExA(hwnd, v237Sibling, wc.lpszClassName, "disabled-child") == v236Disabled &&
                      FindWindowExA(hwnd, 0, wc.lpszClassName, "missing-child") == 0;
    smoke_expect(&s, v237FindOk,
                 "v237 FindWindowExA honors parent/after/class/title selectors",
                 "window search uses USER32-local HWND metadata instead of title-only WindowManager lookup");

    BOOL v237AncestorOk = v222Child &&
                          GetAncestor(v222Child, GA_PARENT) == hwnd &&
                          GetAncestor(v222Child, GA_ROOT) == hwnd &&
                          GetAncestor(v222Child, GA_ROOTOWNER) == hwnd;
    smoke_expect(&s, v237AncestorOk,
                 "v237 GetAncestor supports root-owner surface",
                 "ancestor traversal is exposed through the same USER32 parent/owner metadata used by GetWindow");

    BOOL v237TopOk = v222Child && v237Sibling &&
                     GetTopWindow(hwnd) == v237Sibling &&
                     BringWindowToTop(v222Child) &&
                     GetTopWindow(hwnd) == v222Child;
    smoke_expect(&s, v237TopOk,
                 "v237 GetTopWindow/BringWindowToTop use local Z-order",
                 "top-child query and raise-to-top share the USER32 z-order state used by hit-test/paint");

    /* v241: make the v240 intrusive sibling list visible in the perf log.
       MAX_HWNDS is still intentionally small, so this exercises a realistic
       medium child tree without starving later smoke windows. */
    enum { V241_CHILD_COUNT = 40, V241_CHILD_PROBES = 4096 };
    HWND v241Children[V241_CHILD_COUNT];
    memset(v241Children, 0, sizeof(v241Children));
    int v241Created = 0;
    for (int i = 0; i < V241_CHILD_COUNT; ++i) {
        char title[32];
        snprintf(title, sizeof(title), "v241-child-%02d", i);
        HWND c = CreateWindowExA(0, wc.lpszClassName, title, WS_CHILD|WS_VISIBLE,
                                 4 + (i % 10) * 12, 48 + (i / 10) * 10, 10, 8,
                                 hwnd, 0, 0, NULL);
        if (!c) break;
        v241Children[v241Created++] = c;
    }
    BOOL v241TreeOk = (v241Created == V241_CHILD_COUNT) &&
                      GetTopWindow(hwnd) == v241Children[V241_CHILD_COUNT - 1] &&
                      FindWindowExA(hwnd, 0, wc.lpszClassName, "v241-child-00") == v241Children[0];
    smoke_expect(&s, v241TreeOk,
                 "v241 child sibling list preserves creation/Z traversal",
                 "medium child tree resolves through intrusive sibling links instead of global WindowInfo table order");

    int v241BenchOk = v241TreeOk;
    unsigned long long v241Start = smoke_perf_now_us();
    for (int probe = 0; v241BenchOk && probe < V241_CHILD_PROBES; ++probe) {
        HWND top = GetTopWindow(hwnd);
        HWND first = FindWindowExA(hwnd, 0, wc.lpszClassName, "v241-child-00");
        if (top != v241Children[V241_CHILD_COUNT - 1] || first != v241Children[0])
            v241BenchOk = 0;
    }
    unsigned long long v241End = smoke_perf_now_us();
    smoke_expect(&s, v241BenchOk,
                 "v241 child sibling list benchmark resolves stable children",
                 "GetTopWindow and FindWindowExA stay correct across repeated medium-tree probes");
    char v241Bench[256];
    double v241Ms = (double)(v241End - v241Start) / 1000.0;
    double v241OpsS = v241Ms > 0.0 ? ((double)(V241_CHILD_PROBES * 2) * 1000.0 / v241Ms) : 0.0;
    snprintf(v241Bench, sizeof(v241Bench),
             "children=%d probes=%d ops=%d wall_ms=%.3f ops_s=%.0f top=0x%x first=0x%x",
             v241Created, V241_CHILD_PROBES, V241_CHILD_PROBES * 2,
             v241Ms, v241OpsS,
             (unsigned)(v241Created ? v241Children[V241_CHILD_COUNT - 1] : 0),
             (unsigned)(v241Created ? v241Children[0] : 0));
    smoke_info(s.group, "v241 child sibling-list benchmark", v241Bench);

    for (int i = v241Created - 1; i >= 0; --i)
        if (v241Children[i]) DestroyWindow(v241Children[i]);

    /* v244: top-level USER32 windows now share the same intrusive list model as
       child windows.  Parent==NULL GetTopWindow/FindWindowA no longer needs a
       whole WindowInfo table scan in the normal case, and class/title hashes
       reject non-matches before touching cold string buffers. */
    enum { V244_ROOT_COUNT = 8, V244_ROOT_PROBES = 4096 };
    HWND v244Roots[V244_ROOT_COUNT];
    memset(v244Roots, 0, sizeof(v244Roots));
    int v244Created = 0;
    for (int i = 0; i < V244_ROOT_COUNT; ++i) {
        char title[32];
        snprintf(title, sizeof(title), "v244-root-%02d", i);
        HWND r = CreateWindowExA(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                                 120 + i * 4, 80 + i * 4, 100, 80, 0, 0, 0, NULL);
        if (!r) break;
        v244Roots[v244Created++] = r;
    }
    BOOL v244RootOk = (v244Created == V244_ROOT_COUNT) &&
                      GetTopWindow(0) == v244Roots[V244_ROOT_COUNT - 1] &&
                      FindWindowA(wc.lpszClassName, "v244-root-00") == v244Roots[0];
    smoke_expect(&s, v244RootOk,
                 "v244 root window list and title hash resolve top-level windows",
                 "parent==NULL GetTopWindow/FindWindowA uses the intrusive root list plus hot class/title hashes");

    int v244BenchOk = v244RootOk;
    unsigned long long v244Start = smoke_perf_now_us();
    for (int probe = 0; v244BenchOk && probe < V244_ROOT_PROBES; ++probe) {
        HWND top = GetTopWindow(0);
        HWND first = FindWindowA(wc.lpszClassName, "v244-root-00");
        if (top != v244Roots[V244_ROOT_COUNT - 1] || first != v244Roots[0])
            v244BenchOk = 0;
    }
    unsigned long long v244End = smoke_perf_now_us();
    smoke_expect(&s, v244BenchOk,
                 "v244 root list benchmark resolves stable top-level windows",
                 "GetTopWindow(0) and FindWindowA avoid the legacy whole WindowInfo scan for normal root windows");
    char v244Bench[256];
    double v244Ms = (double)(v244End - v244Start) / 1000.0;
    double v244OpsS = v244Ms > 0.0 ? ((double)(V244_ROOT_PROBES * 2) * 1000.0 / v244Ms) : 0.0;
    snprintf(v244Bench, sizeof(v244Bench),
             "roots=%d probes=%d ops=%d wall_ms=%.3f ops_s=%.0f top=0x%x first=0x%x",
             v244Created, V244_ROOT_PROBES, V244_ROOT_PROBES * 2,
             v244Ms, v244OpsS,
             (unsigned)(v244Created ? v244Roots[V244_ROOT_COUNT - 1] : 0),
             (unsigned)(v244Created ? v244Roots[0] : 0));
    smoke_info(s.group, "v244 root-list/title-hash benchmark", v244Bench);
    for (int i = v244Created - 1; i >= 0; --i)
        if (v244Roots[i]) DestroyWindow(v244Roots[i]);

    if (v237Sibling) DestroyWindow(v237Sibling);
    if (v236Disabled) DestroyWindow(v236Disabled);
    if (v222Child) DestroyWindow(v222Child);

    /* The v222 paint/hit-test probes intentionally exercise real USER32/GDI
       side effects.  Drain incidental paint/lifecycle messages and reset the
       shared smoke counters before the older message-order contract below. */
    MSG v222Drain;
    while (PeekMessageA(&v222Drain, 0, 0, 0, PM_REMOVE)) {
        if (v222Drain.message != WM_COMMAND) DispatchMessageA(&v222Drain);
    }
    g_user32_destroyed = 0;
    g_user32_command = 0;
    g_user32_user = 0;
    g_user32_windowpos_changing = 0;
    g_user32_windowpos_changed = 0;
    g_user32_move = 0;
    g_user32_size = 0;

    BOOL v223LaneOk = mymsg_default_lane(WM_KEYDOWN, 0) == _MSG_LANE_INPUT &&
                      mymsg_default_lane(WM_PAINT, 0) == _MSG_LANE_WINDOW &&
                      mymsg_default_lane(WM_TIMER, 0) == _MSG_LANE_TIMER &&
                      mymsg_default_lane(WM_COMMAND, MYMSG_FLAG_SYNC_REQ) == _MSG_LANE_SEND;
    smoke_expect(&s, v223LaneOk,
                 "v223 USER message lanes classify input/window/timer/send",
                 "private _MSG_LANE enum converts public WM_* messages into queue dispatch lanes once");

    MyMessageQueue v223Q;
    myqueue_init(&v223Q);
    MyMessage v223Posted;
    memset(&v223Posted, 0, sizeof(v223Posted));
    v223Posted.size = sizeof(v223Posted);
    v223Posted.hwnd = hwnd;
    v223Posted.msg = WM_COMMAND;
    v223Posted.wparam = 11;
    MyMessage v223Input = v223Posted;
    v223Input.msg = WM_KEYDOWN;
    v223Input.wparam = VK_TAB;
    myqueue_post(&v223Q, &v223Posted);
    myqueue_post(&v223Q, &v223Input);
    MyMessage v223Out;
    memset(&v223Out, 0, sizeof(v223Out));
    BOOL v223IterOk = myqueue_peek_match(&v223Q, &v223Out, 0, 0, 0, 1) &&
                      v223Out.msg == WM_KEYDOWN &&
                      v223Out.lane == _MSG_LANE_INPUT;
    smoke_expect(&s, v223IterOk,
                 "v223 queue selector iterates by message lane",
                 "input lane is selected before older posted messages without changing public MSG shape");

    _QueueSelect v223Select;
    myqueue_make_select(&v223Select, hwnd, WM_COMMAND, WM_COMMAND, 1);
    memset(&v223Out, 0, sizeof(v223Out));
    BOOL v223SelectOk = myqueue_peek_select(&v223Q, &v223Out, &v223Select) &&
                        v223Out.msg == WM_COMMAND &&
                        v223Out.lane == _MSG_LANE_POSTED &&
                        (v223Select.fields & _QUEUE_SELECT_MSG_RANGE) &&
                        (v223Select.fields & _QUEUE_SELECT_LANES);
    smoke_expect(&s, v223SelectOk,
                 "v223 queue selector uses explicit filter fields",
                 "_QUEUE_SELECT describes hwnd/message-range/lane/remove fields instead of open-coded iterator ifs");

    BOOL v224KindOk = mymsg_default_input_kind(WM_KEYDOWN) == _MSG_INPUT_KEY &&
                      mymsg_default_input_kind(WM_CHAR) == _MSG_INPUT_CHAR &&
                      mymsg_default_input_kind(WM_MOUSEMOVE) == _MSG_INPUT_MOUSE_MOVE &&
                      mymsg_default_input_kind(WM_LBUTTONDOWN) == _MSG_INPUT_MOUSE_BUTTON &&
                      mymsg_default_input_kind(WM_MOUSEWHEEL) == _MSG_INPUT_MOUSE_WHEEL &&
                      mymsg_default_input_kind(WM_COMMAND) == _MSG_INPUT_NONE;
    smoke_expect(&s, v224KindOk,
                 "v224 input messages classify by private input kind",
                 "_MSG_INPUT_KIND splits INPUT lane into key/char/mouse-move/button/wheel dispatch categories");

    MyMessageQueue v224Q;
    myqueue_init(&v224Q);
    MyMessage v224Key;
    memset(&v224Key, 0, sizeof(v224Key));
    v224Key.size = sizeof(v224Key);
    v224Key.hwnd = hwnd;
    v224Key.msg = WM_KEYDOWN;
    v224Key.wparam = VK_RETURN;
    MyMessage v224Mouse = v224Key;
    v224Mouse.msg = WM_LBUTTONDOWN;
    v224Mouse.wparam = MK_LBUTTON;
    myqueue_post(&v224Q, &v224Key);
    myqueue_post(&v224Q, &v224Mouse);
    _QueueSelect v224InputSelect;
    myqueue_make_input_select(&v224InputSelect, hwnd, _MSG_INPUT_KIND_BIT(_MSG_INPUT_MOUSE_BUTTON), 1);
    MyMessage v224Out;
    memset(&v224Out, 0, sizeof(v224Out));
    BOOL v224SelectOk = myqueue_peek_select(&v224Q, &v224Out, &v224InputSelect) &&
                        v224Out.msg == WM_LBUTTONDOWN &&
                        v224Out.lane == _MSG_LANE_INPUT &&
                        v224Out.input_kind == _MSG_INPUT_MOUSE_BUTTON &&
                        v224Out.route_state == _MSG_ROUTE_TARGET_RESOLVED &&
                        (v224InputSelect.fields & _QUEUE_SELECT_INPUT_KIND);
    smoke_expect(&s, v224SelectOk,
                 "v224 input queue selector filters by input-kind state",
                 "mouse-button input is selected through lane+input-kind metadata instead of another WM_* if ladder");

    BOOL v224ActionOk = hwnd_state_allows(_HWND_STATE_LIVE, _HWND_ACTION_MESSAGE | _HWND_ACTION_HITTEST) &&
                        hwnd_state_allows(_HWND_STATE_LIVE, _HWND_ACTION_MESSAGE | _HWND_ACTION_FOCUS) &&
                        !hwnd_state_allows(_HWND_STATE_DESTROY_PENDING, _HWND_ACTION_MESSAGE | _HWND_ACTION_HITTEST);
    smoke_expect(&s, v224ActionOk,
                 "v224 input dispatch maps input-kind to HWND action state",
                 "mouse input requires MESSAGE+HITTEST and keyboard input requires MESSAGE+FOCUS on the resolved HWND state");

    BOOL v225ReasonOk = mymsg_default_route_reason(WM_KEYDOWN, 0, _MSG_INPUT_KEY) == _MSG_ROUTE_REASON_FOCUS &&
                        mymsg_default_route_reason(WM_LBUTTONDOWN, 0, _MSG_INPUT_MOUSE_BUTTON) == _MSG_ROUTE_REASON_HITTEST &&
                        (mymsg_default_route_reason(WM_MOUSEWHEEL, 0, _MSG_INPUT_MOUSE_WHEEL) & _MSG_ROUTE_REASON_HOVER) &&
                        mymsg_default_route_reason(WM_TIMER, 0, _MSG_INPUT_NONE) == _MSG_ROUTE_REASON_TIMER;
    smoke_expect(&s, v225ReasonOk,
                 "v225 input route reasons classify focus/hittest/hover/timer",
                 "route-reason state records why a target HWND wins instead of rediscovering it from WM_* later");

    _MsgRouteDescriptor v225Route;
    memset(&v225Route, 0, sizeof(v225Route));
    v225Route.cbSize = sizeof(v225Route);
    v225Route.lane = _MSG_LANE_INPUT;
    v225Route.input_kind = _MSG_INPUT_MOUSE_BUTTON;
    v225Route.route_state = _MSG_ROUTE_CAPTURED;
    v225Route.route_reason = _MSG_ROUTE_REASON_CAPTURE;
    v225Route.target_hwnd = hwnd;
    v225Route.capture_hwnd = hwnd;
    v225Route.hwnd_action = mymsg_required_hwnd_action_for_route(v225Route.lane, v225Route.input_kind, v225Route.route_reason);
    MyMessage v225Msg;
    memset(&v225Msg, 0, sizeof(v225Msg));
    v225Msg.size = sizeof(v225Msg);
    v225Msg.hwnd = hwnd;
    v225Msg.msg = WM_LBUTTONDOWN;
    v225Msg.wparam = MK_LBUTTON;
    mymsg_apply_route_descriptor(&v225Msg, &v225Route);
    _MsgRouteDescriptor v225OutRoute;
    mymsg_make_route_descriptor(&v225Msg, &v225OutRoute);
    BOOL v225DescriptorOk = v225OutRoute.route_state == _MSG_ROUTE_CAPTURED &&
                            (v225OutRoute.route_reason & _MSG_ROUTE_REASON_CAPTURE) &&
                            v225OutRoute.capture_hwnd == hwnd &&
                            (v225OutRoute.hwnd_action & _HWND_ACTION_CAPTURE) &&
                            (v225OutRoute.hwnd_action & _HWND_ACTION_HITTEST);
    smoke_expect(&s, v225DescriptorOk,
                 "v225 route descriptor carries capture/focus/hittest state",
                 "_MSG_ROUTE_DESCRIPTOR makes the queue entry state-driven: reason -> HWND action -> dispatch path");

    MyMessageQueue v225Q;
    myqueue_init(&v225Q);
    myqueue_post(&v225Q, &v225Msg);
    MyMessage v225Queued;
    memset(&v225Queued, 0, sizeof(v225Queued));
    BOOL v225QueuedOk = myqueue_peek_match(&v225Q, &v225Queued, hwnd, WM_LBUTTONDOWN, WM_LBUTTONDOWN, 1) &&
                        v225Queued.route_state == _MSG_ROUTE_CAPTURED &&
                        (v225Queued.route_reason & _MSG_ROUTE_REASON_CAPTURE) &&
                        (v225Queued.route_action & _HWND_ACTION_CAPTURE);
    smoke_expect(&s, v225QueuedOk,
                 "v225 queued input preserves explicit route descriptor",
                 "the queued message keeps capture/focus/hit-test reason fields instead of degrading to raw MSG only");

    uint32_t v226KeyStages = mymsg_default_filter_stages(WM_KEYDOWN, _MSG_LANE_INPUT, _MSG_INPUT_KEY, _MSG_ROUTE_REASON_FOCUS);
    uint32_t v226MouseStages = mymsg_default_filter_stages(WM_LBUTTONDOWN, _MSG_LANE_INPUT, _MSG_INPUT_MOUSE_BUTTON, _MSG_ROUTE_REASON_HITTEST);
    BOOL v226StageOk = (v226KeyStages & _MSG_FILTER_HOOK) &&
                       (v226KeyStages & _MSG_FILTER_ACCELERATOR) &&
                       (v226KeyStages & _MSG_FILTER_DIALOG) &&
                       (v226KeyStages & _MSG_FILTER_MODELLESS) &&
                       (v226KeyStages & _MSG_FILTER_TRANSLATE) &&
                       (v226KeyStages & _MSG_FILTER_DISPATCH) &&
                       (v226MouseStages & _MSG_FILTER_MENU) &&
                       (v226MouseStages & _MSG_FILTER_DISPATCH);
    smoke_expect(&s, v226StageOk,
                 "v226 message filter stages classify accelerator/dialog/translate/dispatch",
                 "keyboard input now carries a pre-dispatch stage mask instead of every pump rediscovering the order");

    MyMessage v226Msg;
    memset(&v226Msg, 0, sizeof(v226Msg));
    v226Msg.size = sizeof(v226Msg);
    v226Msg.hwnd = hwnd;
    v226Msg.msg = WM_KEYDOWN;
    v226Msg.wparam = VK_TAB;
    myqueue_post(&v225Q, &v226Msg);
    MyMessage v226Queued;
    memset(&v226Queued, 0, sizeof(v226Queued));
    _QueueSelect v226FilterSelect;
    myqueue_make_filter_select(&v226FilterSelect, hwnd, _MSG_FILTER_ACCELERATOR, 1);
    BOOL v226SelectOk = myqueue_peek_select(&v225Q, &v226Queued, &v226FilterSelect) &&
                        v226Queued.msg == WM_KEYDOWN &&
                        (v226Queued.filter_stages & _MSG_FILTER_ACCELERATOR) &&
                        v226Queued.filter_state == _MSG_FILTER_STATE_PENDING &&
                        v226Queued.filter_stage == _MSG_FILTER_HOOK &&
                        (v226FilterSelect.fields & _QUEUE_SELECT_FILTER_STAGE);
    smoke_expect(&s, v226SelectOk,
                 "v226 queue selector filters by message filter stage",
                 "_QUEUE_SELECT can now iterate messages by pretranslate stage mask as well as lane/input-kind");

    _MsgRouteDescriptor v226Route;
    mymsg_make_route_descriptor(&v226Queued, &v226Route);
    BOOL v226DescriptorOk = (v226Route.filter_stages & _MSG_FILTER_ACCELERATOR) &&
                            (v226Route.filter_stages & _MSG_FILTER_DIALOG) &&
                            v226Route.filter_state == _MSG_FILTER_STATE_PENDING &&
                            v226Route.filter_stage == _MSG_FILTER_HOOK;
    smoke_expect(&s, v226DescriptorOk,
                 "v226 route descriptor carries USER pre-dispatch stage state",
                 "route descriptor now carries hook/accelerator/dialog/modeless/translate/dispatch state next to target HWND routing");

    enum { V226_FILTER_PROBES = 2048 };
    unsigned long long v226Start = smoke_perf_now_us();
    BOOL v226BenchOk = TRUE;
    for (int probe = 0; probe < V226_FILTER_PROBES; ++probe) {
        MyMessage a;
        memset(&a, 0, sizeof(a));
        a.size = sizeof(a);
        a.hwnd = hwnd;
        a.msg = (probe & 1) ? WM_KEYDOWN : WM_LBUTTONDOWN;
        a.lane = mymsg_default_lane(a.msg, 0);
        a.input_kind = mymsg_default_input_kind(a.msg);
        a.route_reason = mymsg_default_route_reason(a.msg, 0, a.input_kind);
        a.filter_stages = mymsg_default_filter_stages(a.msg, a.lane, a.input_kind, a.route_reason);
        a.filter_stage = mymsg_first_filter_stage(a.filter_stages);
        if (!a.filter_stage || !(a.filter_stages & _MSG_FILTER_DISPATCH)) v226BenchOk = FALSE;
    }
    unsigned long long v226End = smoke_perf_now_us();
    smoke_expect(&s, v226BenchOk,
                 "v226 filter-stage resolver stays data-driven",
                 "filter-stage masks resolve without falling back to scattered WM_* pretranslate checks");
    char v226Bench[256];
    double v226Ms = (double)(v226End - v226Start) / 1000.0;
    double v226OpsS = v226Ms > 0.0 ? ((double)V226_FILTER_PROBES * 1000.0 / v226Ms) : 0.0;
    snprintf(v226Bench, sizeof(v226Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f stages=hook/accelerator/dialog/modeless/translate/menu/dispatch",
             V226_FILTER_PROBES, v226Ms, v226OpsS);
    smoke_info(s.group, "v226 filter-stage descriptor benchmark", v226Bench);

    _MsgFilterPipeline v227Pipe;
    int v227Steps = mymsg_build_filter_pipeline(v226KeyStages, &v227Pipe);
    BOOL v227PipeOk = v227Steps >= 5 &&
                      v227Pipe.steps[0].stage == _MSG_FILTER_HOOK &&
                      v227Pipe.steps[1].stage == _MSG_FILTER_ACCELERATOR &&
                      v227Pipe.steps[2].stage == _MSG_FILTER_DIALOG &&
                      (v227Pipe.stages & _MSG_FILTER_DISPATCH);
    smoke_expect(&s, v227PipeOk,
                 "v227 message filter mask materializes into ordered pipeline",
                 "stage bits are now an ordered stage vector: hook -> accelerator -> dialog -> modeless/translate -> dispatch");

    MyMessage v227Adv;
    memset(&v227Adv, 0, sizeof(v227Adv));
    v227Adv.size = sizeof(v227Adv);
    v227Adv.hwnd = hwnd;
    v227Adv.msg = WM_KEYDOWN;
    v227Adv.lane = _MSG_LANE_INPUT;
    v227Adv.input_kind = _MSG_INPUT_KEY;
    v227Adv.route_reason = _MSG_ROUTE_REASON_FOCUS;
    v227Adv.filter_stages = v226KeyStages;
    v227Adv.filter_stage = mymsg_first_filter_stage(v227Adv.filter_stages);
    v227Adv.filter_state = _MSG_FILTER_STATE_PENDING;
    BOOL v227AdvanceOk = mymsg_advance_filter_stage(&v227Adv, _MSG_FILTER_HOOK, _MSG_FILTER_STATE_PASSTHROUGH) &&
                         v227Adv.filter_stage == _MSG_FILTER_ACCELERATOR &&
                         v227Adv.filter_state == _MSG_FILTER_STATE_PENDING &&
                         mymsg_advance_filter_stage(&v227Adv, _MSG_FILTER_ACCELERATOR, _MSG_FILTER_STATE_HANDLED) &&
                         v227Adv.filter_stage == _MSG_FILTER_ACCELERATOR &&
                         v227Adv.filter_state == _MSG_FILTER_STATE_HANDLED;
    smoke_expect(&s, v227AdvanceOk,
                 "v227 filter pipeline advances by stage result",
                 "PASSTHROUGH advances to the next stage, HANDLED/BLOCKED pins the consuming stage as dispatch state");

    enum { V227_PIPE_PROBES = 2048 };
    unsigned long long v227Start = smoke_perf_now_us();
    BOOL v227BenchOk = TRUE;
    for (int probe = 0; probe < V227_PIPE_PROBES; ++probe) {
        uint32_t stages = (probe & 1) ? v226KeyStages : v226MouseStages;
        _MsgFilterPipeline pipe;
        if (mymsg_build_filter_pipeline(stages, &pipe) <= 0) { v227BenchOk = FALSE; break; }
        for (uint32_t i = 0; i < pipe.count; ++i) {
            if (!pipe.steps[i].stage || pipe.steps[i].state != _MSG_FILTER_STATE_PENDING) { v227BenchOk = FALSE; break; }
        }
        if (!v227BenchOk) break;
    }
    unsigned long long v227End = smoke_perf_now_us();
    smoke_expect(&s, v227BenchOk,
                 "v227 filter pipeline runner stays data-driven",
                 "pipeline materialization keeps pre-dispatch order in data instead of per-loop condition ladders");
    char v227Bench[256];
    double v227Ms = (double)(v227End - v227Start) / 1000.0;
    double v227OpsS = v227Ms > 0.0 ? ((double)V227_PIPE_PROBES * 1000.0 / v227Ms) : 0.0;
    snprintf(v227Bench, sizeof(v227Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f pipeline=hook/accelerator/dialog/modeless/translate/menu/dispatch",
             V227_PIPE_PROBES, v227Ms, v227OpsS);
    smoke_info(s.group, "v227 filter pipeline benchmark", v227Bench);

    DWORD v228HookOrder = 99, v228AccelOrder = 99, v228DialogOrder = 99, v228DispatchOrder = 99;
    DWORD v228AccelCanHandle = 0, v228DialogCanHandle = 0, v228DispatchAction = 0;
    BOOL v228TableOk = MyWinQueryMessageFilterStage(_MSG_FILTER_HOOK, &v228HookOrder, NULL, NULL) &&
                       MyWinQueryMessageFilterStage(_MSG_FILTER_ACCELERATOR, &v228AccelOrder, &v228AccelCanHandle, NULL) &&
                       MyWinQueryMessageFilterStage(_MSG_FILTER_DIALOG, &v228DialogOrder, &v228DialogCanHandle, NULL) &&
                       MyWinQueryMessageFilterStage(_MSG_FILTER_DISPATCH, &v228DispatchOrder, NULL, &v228DispatchAction) &&
                       v228HookOrder < v228AccelOrder &&
                       v228AccelOrder < v228DialogOrder &&
                       v228DialogOrder < v228DispatchOrder &&
                       v228AccelCanHandle && v228DialogCanHandle &&
                       (v228DispatchAction & _HWND_ACTION_MESSAGE);
    smoke_expect(&s, v228TableOk,
                 "v228 filter stages dispatch through an op table",
                 "hook/accelerator/dialog/modeless/translate/menu/dispatch are now USER stage ops, not a switch ladder per pump");

    BOOL v228UnknownOk = !MyWinQueryMessageFilterStage(0x80000000u, NULL, NULL, NULL);
    smoke_expect(&s, v228UnknownOk,
                 "v228 filter op table rejects unknown stages",
                 "stage lookup validates private USER filter-stage enums before dispatching a handler");

    enum { V228_OP_PROBES = 2048 };
    unsigned long long v228Start = smoke_perf_now_us();
    BOOL v228BenchOk = TRUE;
    for (int probe = 0; probe < V228_OP_PROBES; ++probe) {
        DWORD order = 0, canHandle = 0, action = 0;
        DWORD stage = (probe % 7 == 0) ? _MSG_FILTER_HOOK :
                      (probe % 7 == 1) ? _MSG_FILTER_ACCELERATOR :
                      (probe % 7 == 2) ? _MSG_FILTER_DIALOG :
                      (probe % 7 == 3) ? _MSG_FILTER_MODELLESS :
                      (probe % 7 == 4) ? _MSG_FILTER_TRANSLATE :
                      (probe % 7 == 5) ? _MSG_FILTER_MENU : _MSG_FILTER_DISPATCH;
        if (!MyWinQueryMessageFilterStage(stage, &order, &canHandle, &action) ||
            order > 6 || !(action & _HWND_ACTION_MESSAGE)) {
            v228BenchOk = FALSE;
            break;
        }
    }
    unsigned long long v228End = smoke_perf_now_us();
    smoke_expect(&s, v228BenchOk,
                 "v228 filter op-table lookup stays data-driven",
                 "stage -> op metadata resolves through a compact table before the handler is invoked");
    char v228Bench[256];
    double v228Ms = (double)(v228End - v228Start) / 1000.0;
    double v228OpsS = v228Ms > 0.0 ? ((double)V228_OP_PROBES * 1000.0 / v228Ms) : 0.0;
    snprintf(v228Bench, sizeof(v228Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f ops=hook/accelerator/dialog/modeless/translate/menu/dispatch",
             V228_OP_PROBES, v228Ms, v228OpsS);
    smoke_info(s.group, "v228 filter op-table benchmark", v228Bench);

    BOOL v229LayoutOk = sizeof(_MsgRouteDescriptor) <= 44 &&
                        sizeof(MyMessage) <= 136 &&
                        sizeof(_MsgFilterPipeline) <= 28 &&
                        sizeof(_MsgFilterStep) == 2 &&
                        sizeof(_MsgRouteReason) == sizeof(uint32_t) &&
                        sizeof(_MsgFilterStage) == sizeof(uint32_t) &&
                        sizeof(_MsgLane) == 1 &&
                        sizeof(_MsgInputKind) == 1 &&
                        sizeof(_MsgFilterState) == 1;
    smoke_expect(&s, v229LayoutOk,
                 "v229 message sidecar layout is compact and cacheline friendly",
                 "state-like queue fields are bytes; Win32-style route/filter/action masks stay DWORD-sized defines");
    char v229Layout[256];
    snprintf(v229Layout, sizeof(v229Layout),
             "MsgRouteDescriptor=%zu MyMessage=%zu FilterPipeline=%zu FilterStep=%zu route_desc_cacheline=%s",
             sizeof(_MsgRouteDescriptor), sizeof(MyMessage), sizeof(_MsgFilterPipeline), sizeof(_MsgFilterStep),
             sizeof(_MsgRouteDescriptor) <= 64 ? "one" : "split");
    smoke_info(s.group, "v229 compact message sidecar layout", v229Layout);

    BOOL v230BitsOk = mymsg_qs_bits(WM_KEYDOWN, _MSG_LANE_INPUT, _MSG_INPUT_KEY, 0) == QS_KEY &&
                      mymsg_qs_bits(WM_MOUSEMOVE, _MSG_LANE_INPUT, _MSG_INPUT_MOUSE_MOVE, 0) == QS_MOUSEMOVE &&
                      mymsg_qs_bits(WM_LBUTTONDOWN, _MSG_LANE_INPUT, _MSG_INPUT_MOUSE_BUTTON, 0) == QS_MOUSEBUTTON &&
                      mymsg_qs_bits(WM_TIMER, _MSG_LANE_TIMER, _MSG_INPUT_NONE, 0) == QS_TIMER &&
                      mymsg_qs_bits(WM_PAINT, _MSG_LANE_WINDOW, _MSG_INPUT_NONE, 0) == QS_PAINT &&
                      mymsg_qs_bits(WM_COMMAND, _MSG_LANE_SEND, _MSG_INPUT_NONE, MYMSG_FLAG_SYNC_REQ) == QS_SENDMESSAGE;
    smoke_expect(&s, v230BitsOk,
                 "v230 QS bits classify queue-visible message state",
                 "public QS_KEY/QS_MOUSE/QS_TIMER/QS_PAINT/QS_SENDMESSAGE masks are derived once from lane/input-kind/message state");

    MyMessageQueue v230Q;
    myqueue_init(&v230Q);
    MyMessage v230Key;
    memset(&v230Key, 0, sizeof(v230Key));
    v230Key.size = sizeof(v230Key);
    v230Key.hwnd = hwnd;
    v230Key.msg = WM_KEYDOWN;
    v230Key.wparam = VK_TAB;
    MyMessage v230Timer = v230Key;
    v230Timer.msg = WM_TIMER;
    v230Timer.wparam = 77;
    myqueue_post(&v230Q, &v230Key);
    myqueue_post(&v230Q, &v230Timer);
    DWORD v230Status1 = myqueue_get_queue_status(&v230Q, QS_ALLINPUT);
    DWORD v230Status2 = myqueue_get_queue_status(&v230Q, QS_ALLINPUT);
    BOOL v230StatusOk = ((v230Status1 & QS_KEY) != 0) &&
                        ((v230Status1 & QS_TIMER) != 0) &&
                        (((v230Status1 >> 16) & QS_KEY) != 0) &&
                        (((v230Status1 >> 16) & QS_TIMER) != 0) &&
                        ((v230Status2 & QS_KEY) != 0) &&
                        ((v230Status2 & QS_TIMER) != 0) &&
                        (((v230Status2 >> 16) & (QS_KEY|QS_TIMER)) == 0);
    smoke_expect(&s, v230StatusOk,
                 "v230 GetQueueStatus-style current/changed QS masks",
                 "queue status returns LOWORD=current QS bits and HIWORD=changed bits, then clears queried changed state");

    _QueueSelect v230KeySelect;
    myqueue_make_qs_select(&v230KeySelect, hwnd, 0, 0, QS_KEY, 1);
    MyMessage v230Out;
    memset(&v230Out, 0, sizeof(v230Out));
    BOOL v230SelectOk = myqueue_peek_select(&v230Q, &v230Out, &v230KeySelect) &&
                        v230Out.msg == WM_KEYDOWN &&
                        (myqueue_peek_queue_status(&v230Q) & QS_TIMER) &&
                        !(myqueue_peek_queue_status(&v230Q) & QS_KEY);
    smoke_expect(&s, v230SelectOk,
                 "v230 queue selector uses QS mask as prefilter",
                 "QS_KEY selection consumes key messages and leaves unrelated timer queue state intact");
    myqueue_destroy(&v230Q);

    MyMessageQueue v230BenchQ;
    myqueue_init(&v230BenchQ);
    MyMessage v230BenchMsg;
    memset(&v230BenchMsg, 0, sizeof(v230BenchMsg));
    v230BenchMsg.size = sizeof(v230BenchMsg);
    v230BenchMsg.hwnd = hwnd;
    v230BenchMsg.msg = WM_COMMAND;
    myqueue_post(&v230BenchQ, &v230BenchMsg);
    _QueueSelect v230NoKey;
    myqueue_make_qs_select(&v230NoKey, hwnd, 0, 0, QS_KEY, 0);
    enum { V230_QS_PROBES = 2048 };
    unsigned long long v230Start = smoke_perf_now_us();
    MyMessage v230Dummy;
    for (int probe = 0; probe < V230_QS_PROBES; ++probe) {
        memset(&v230Dummy, 0, sizeof(v230Dummy));
        myqueue_peek_select(&v230BenchQ, &v230Dummy, &v230NoKey);
    }
    unsigned long long v230End = smoke_perf_now_us();
    BOOL v230PrefilterOk = v230BenchQ.qs_prefilter_skips >= V230_QS_PROBES;
    smoke_expect(&s, v230PrefilterOk,
                 "v230 QS prefilter skips non-matching queue scans",
                 "selector rejects QS_KEY before iterating a queue that only contains QS_POSTMESSAGE work");
    char v230Bench[256];
    double v230Ms = (double)(v230End - v230Start) / 1000.0;
    double v230OpsS = v230Ms > 0.0 ? ((double)V230_QS_PROBES * 1000.0 / v230Ms) : 0.0;
    snprintf(v230Bench, sizeof(v230Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f skips=%llu current_qs=0x%x changed_qs=0x%x",
             V230_QS_PROBES, v230Ms, v230OpsS,
             (unsigned long long)v230BenchQ.qs_prefilter_skips,
             (unsigned)v230BenchQ.current_qs,
             (unsigned)v230BenchQ.changed_qs);
    smoke_info(s.group, "v230 QS queue-status prefilter benchmark", v230Bench);
    myqueue_destroy(&v230BenchQ);

    BOOL v231LayoutOk = sizeof(_MessageHot) <= 64 &&
                        sizeof(_MessageCold) <= sizeof(MyMessage) &&
                        sizeof(((MyMessageQueue*)0)->hot) < sizeof(MyMessage) * MYQUEUE_CAP &&
                        sizeof(((MyMessageQueue*)0)->hot[0]) < sizeof(MyMessage);
    smoke_expect(&s, v231LayoutOk,
                 "v231 message queue uses hot/cold storage split",
                 "queue ring scans compact _MessageHot entries while cold IPC/sync/routing metadata lives in sidecar slots");

    MyMessageQueue v231Q;
    myqueue_init(&v231Q);
    MyMessage v231In;
    memset(&v231In, 0, sizeof(v231In));
    v231In.size = sizeof(v231In);
    v231In.type = 0x231;
    v231In.sender_pid = 11;
    v231In.sender_tid = 12;
    v231In.target_pid = 21;
    v231In.target_tid = 22;
    v231In.hwnd = hwnd;
    v231In.msg = WM_MOUSEWHEEL;
    v231In.wparam = 0x1234;
    v231In.lparam = 0x5678;
    v231In.route_reason = _MSG_ROUTE_REASON_HOVER | _MSG_ROUTE_REASON_HITTEST;
    v231In.capture_hwnd = hwnd;
    v231In.focus_hwnd = hwnd;
    v231In.hit_hwnd = hwnd;
    v231In.section_id = 77;
    v231In.payload_offset = 88;
    v231In.payload_size = 99;
    v231In.sync_ctx = (LPVOID)(uintptr_t)0x12345678u;
    myqueue_post(&v231Q, &v231In);
    MyMessage v231Out;
    memset(&v231Out, 0, sizeof(v231Out));
    BOOL v231RoundtripOk = myqueue_get(&v231Q, &v231Out) &&
                           v231Out.msg == v231In.msg &&
                           v231Out.wparam == v231In.wparam &&
                           v231Out.lparam == v231In.lparam &&
                           v231Out.sender_pid == v231In.sender_pid &&
                           v231Out.target_tid == v231In.target_tid &&
                           v231Out.section_id == v231In.section_id &&
                           v231Out.payload_offset == v231In.payload_offset &&
                           v231Out.payload_size == v231In.payload_size &&
                           v231Out.hit_hwnd == hwnd &&
                           v231Out.sync_ctx == v231In.sync_ctx;
    smoke_expect(&s, v231RoundtripOk,
                 "v231 hot/cold queue split preserves full MyMessage semantics",
                 "public dequeue reconstructs hot MSG payload plus cold sender/target/IPC/sync/routing metadata without drift");
    myqueue_destroy(&v231Q);

    MyMessageQueue v231BenchQ;
    myqueue_init(&v231BenchQ);
    MyMessage v231BenchMsg;
    memset(&v231BenchMsg, 0, sizeof(v231BenchMsg));
    v231BenchMsg.size = sizeof(v231BenchMsg);
    v231BenchMsg.hwnd = hwnd;
    v231BenchMsg.msg = WM_COMMAND;
    myqueue_post(&v231BenchQ, &v231BenchMsg);
    _QueueSelect v231BenchSelect;
    myqueue_make_qs_select(&v231BenchSelect, hwnd, 0, 0, QS_KEY, 0);
    enum { V231_HOT_PROBES = 2048 };
    unsigned long long v231Start = smoke_perf_now_us();
    MyMessage v231Dummy;
    for (int probe = 0; probe < V231_HOT_PROBES; ++probe) {
        memset(&v231Dummy, 0, sizeof(v231Dummy));
        myqueue_peek_select(&v231BenchQ, &v231Dummy, &v231BenchSelect);
    }
    unsigned long long v231End = smoke_perf_now_us();
    char v231Bench[320];
    double v231Ms = (double)(v231End - v231Start) / 1000.0;
    double v231OpsS = v231Ms > 0.0 ? ((double)V231_HOT_PROBES * 1000.0 / v231Ms) : 0.0;
    snprintf(v231Bench, sizeof(v231Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f MyMessage=%zu Hot=%zu Cold=%zu hot_ring=%zu old_ring=%zu",
             V231_HOT_PROBES, v231Ms, v231OpsS,
             sizeof(MyMessage), sizeof(_MessageHot), sizeof(_MessageCold),
             sizeof(v231BenchQ.hot), sizeof(MyMessage) * (size_t)MYQUEUE_CAP);
    smoke_info(s.group, "v231 hot queue scan benchmark", v231Bench);
    myqueue_destroy(&v231BenchQ);

    MyMessageQueue v232IndexQ;
    myqueue_init(&v232IndexQ);
    MyMessage v232Msg;
    memset(&v232Msg, 0, sizeof(v232Msg));
    v232Msg.size = sizeof(v232Msg);
    v232Msg.hwnd = hwnd;

    v232Msg.msg = WM_KEYDOWN;
    v232Msg.wparam = VK_TAB;
    myqueue_post(&v232IndexQ, &v232Msg);
    v232Msg.msg = WM_PAINT;
    v232Msg.wparam = 0;
    myqueue_post(&v232IndexQ, &v232Msg);
    v232Msg.msg = WM_TIMER;
    v232Msg.wparam = 23;
    myqueue_post(&v232IndexQ, &v232Msg);
    v232Msg.msg = WM_COMMAND;
    v232Msg.wparam = 0x2320;
    myqueue_post(&v232IndexQ, &v232Msg);
    v232Msg.flags = MYMSG_FLAG_SYNC_REQ;
    v232Msg.msg = WM_COMMAND;
    v232Msg.wparam = 0x2321;
    myqueue_post(&v232IndexQ, &v232Msg);
    v232Msg.flags = 0;
    v232Msg.lane = _MSG_LANE_BACKGROUND;
    v232Msg.msg = WM_USER + 0x232;
    v232Msg.wparam = 0x2322;
    myqueue_post(&v232IndexQ, &v232Msg);

    BOOL v232IndexOk =
        (v232IndexQ.lane_slot_bits[_MSG_LANE_INPUT][0]      & (1ull << 0)) &&
        (v232IndexQ.lane_slot_bits[_MSG_LANE_WINDOW][0]     & (1ull << 1)) &&
        (v232IndexQ.lane_slot_bits[_MSG_LANE_TIMER][0]      & (1ull << 2)) &&
        (v232IndexQ.lane_slot_bits[_MSG_LANE_POSTED][0]     & (1ull << 3)) &&
        (v232IndexQ.lane_slot_bits[_MSG_LANE_SEND][0]       & (1ull << 4)) &&
        (v232IndexQ.lane_slot_bits[_MSG_LANE_BACKGROUND][0] & (1ull << 5)) &&
        (v232IndexQ.qs_slot_bits[0][0] & (1ull << 0)) &&  /* QS_KEY */
        (v232IndexQ.qs_slot_bits[5][0] & (1ull << 1)) &&  /* QS_PAINT */
        (v232IndexQ.qs_slot_bits[4][0] & (1ull << 2)) &&  /* QS_TIMER */
        (v232IndexQ.qs_slot_bits[3][0] & (1ull << 3)) &&  /* QS_POSTMESSAGE */
        (v232IndexQ.qs_slot_bits[6][0] & (1ull << 4)) &&  /* QS_SENDMESSAGE */
        (v232IndexQ.qs_slot_bits[3][0] & (1ull << 5));    /* BACKGROUND also wakes posted selectors */

    _QueueSelect v232KeySelect;
    myqueue_make_qs_select(&v232KeySelect, hwnd, 0, 0, QS_KEY, 1);
    MyMessage v232IndexOut;
    memset(&v232IndexOut, 0, sizeof(v232IndexOut));
    BOOL v232RemoveUpdatesIndex = myqueue_peek_select(&v232IndexQ, &v232IndexOut, &v232KeySelect) &&
                                  v232IndexOut.msg == WM_KEYDOWN &&
                                  !(v232IndexQ.current_qs & QS_KEY) &&
                                  v232IndexQ.qs_slot_bits[0][0] == 0;
    smoke_expect(&s, v232IndexOk && v232RemoveUpdatesIndex,
                 "v232 queue maintains QS/lane slot indexes",
                 "per-thread queue updates lane and QS 256-bit slot indexes on post, coalesce, and remove without changing GetQueueStatus state");
    myqueue_destroy(&v232IndexQ);

    MyMessageQueue v232FifoQ;
    myqueue_init(&v232FifoQ);
    MyMessage v232Cmd;
    memset(&v232Cmd, 0, sizeof(v232Cmd));
    v232Cmd.size = sizeof(v232Cmd);
    v232Cmd.hwnd = hwnd;
    v232Cmd.msg = WM_COMMAND;
    for (int i = 0; i < 8; ++i) {
        v232Cmd.wparam = (WPARAM)i;
        myqueue_post(&v232FifoQ, &v232Cmd);
    }
    _QueueSelect v232CmdRemove;
    myqueue_make_select(&v232CmdRemove, hwnd, WM_COMMAND, WM_COMMAND, 1);
    MyMessage v232Drop;
    for (int i = 0; i < 6; ++i) {
        memset(&v232Drop, 0, sizeof(v232Drop));
        myqueue_peek_select(&v232FifoQ, &v232Drop, &v232CmdRemove);
    }
    MyMessage v232Timer1 = v232Cmd;
    v232Timer1.msg = WM_TIMER;
    v232Timer1.wparam = 1;
    myqueue_post(&v232FifoQ, &v232Timer1);
    for (int i = 0; i < 247; ++i) {
        v232Cmd.wparam = (WPARAM)(1000 + i);
        myqueue_post(&v232FifoQ, &v232Cmd);
    }
    MyMessage v232Timer2 = v232Cmd;
    v232Timer2.msg = WM_TIMER;
    v232Timer2.wparam = 2;
    myqueue_post(&v232FifoQ, &v232Timer2);
    _QueueSelect v232TimerRemove;
    myqueue_make_qs_select(&v232TimerRemove, hwnd, 0, 0, QS_TIMER, 1);
    MyMessage v232TimerOut1;
    MyMessage v232TimerOut2;
    memset(&v232TimerOut1, 0, sizeof(v232TimerOut1));
    memset(&v232TimerOut2, 0, sizeof(v232TimerOut2));
    BOOL v232FifoOk = myqueue_peek_select(&v232FifoQ, &v232TimerOut1, &v232TimerRemove) &&
                      myqueue_peek_select(&v232FifoQ, &v232TimerOut2, &v232TimerRemove) &&
                      v232TimerOut1.msg == WM_TIMER &&
                      v232TimerOut2.msg == WM_TIMER &&
                      v232TimerOut1.wparam == 1 &&
                      v232TimerOut2.wparam == 2;
    myqueue_destroy(&v232FifoQ);

    MyMessageQueue v232BenchQ;
    myqueue_init(&v232BenchQ);
    MyMessage v232BenchMsg;
    memset(&v232BenchMsg, 0, sizeof(v232BenchMsg));
    v232BenchMsg.size = sizeof(v232BenchMsg);
    v232BenchMsg.hwnd = hwnd;
    v232BenchMsg.msg = WM_COMMAND;
    for (int i = 0; i < 128; ++i) {
        v232BenchMsg.wparam = (WPARAM)i;
        myqueue_post(&v232BenchQ, &v232BenchMsg);
    }
    v232BenchMsg.msg = WM_TIMER;
    v232BenchMsg.wparam = 0x232;
    myqueue_post(&v232BenchQ, &v232BenchMsg);
    _QueueSelect v232BenchSelect;
    myqueue_make_qs_select(&v232BenchSelect, hwnd, 0, 0, QS_TIMER, 0);
    v232BenchSelect.laneMask = _MSG_LANE_BIT(_MSG_LANE_POSTED) | _MSG_LANE_BIT(_MSG_LANE_TIMER);
    enum { V232_INDEX_PROBES = 2048 };
    uint64_t v232CandidatesBefore = v232BenchQ.indexed_candidate_probes;
    uint64_t v232EmptyBefore = v232BenchQ.indexed_empty_skips;
    unsigned long long v232Start = smoke_perf_now_us();
    MyMessage v232Dummy;
    for (int probe = 0; probe < V232_INDEX_PROBES; ++probe) {
        memset(&v232Dummy, 0, sizeof(v232Dummy));
        myqueue_peek_select(&v232BenchQ, &v232Dummy, &v232BenchSelect);
    }
    unsigned long long v232End = smoke_perf_now_us();
    uint64_t v232CandidateDelta = v232BenchQ.indexed_candidate_probes - v232CandidatesBefore;
    uint64_t v232EmptyDelta = v232BenchQ.indexed_empty_skips - v232EmptyBefore;
    BOOL v232JumpOk = v232FifoOk &&
                      v232CandidateDelta == V232_INDEX_PROBES &&
                      v232EmptyDelta == V232_INDEX_PROBES &&
                      v232Dummy.msg == WM_TIMER;
    smoke_expect(&s, v232JumpOk,
                 "v232 selector jumps through indexed candidate slots",
                 "QS/lane bitset intersection probes one timer candidate per select and preserves FIFO relative to queue head across ring wrap");
    char v232Bench[320];
    double v232Ms = (double)(v232End - v232Start) / 1000.0;
    double v232OpsS = v232Ms > 0.0 ? ((double)V232_INDEX_PROBES * 1000.0 / v232Ms) : 0.0;
    snprintf(v232Bench, sizeof(v232Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f candidates=%llu empty_skips=%llu slot_words=%d lane_index=%zu qs_index=%zu",
             V232_INDEX_PROBES, v232Ms, v232OpsS,
             (unsigned long long)v232CandidateDelta,
             (unsigned long long)v232EmptyDelta,
             MYQUEUE_SLOT_WORDS,
             sizeof(v232BenchQ.lane_slot_bits),
             sizeof(v232BenchQ.qs_slot_bits));
    smoke_info(s.group, "v232 indexed queue slot-scan benchmark", v232Bench);
    myqueue_destroy(&v232BenchQ);

    MyMessageQueue v233IndexQ;
    myqueue_init(&v233IndexQ);
    MyMessage v233Msg;
    memset(&v233Msg, 0, sizeof(v233Msg));
    v233Msg.size = sizeof(v233Msg);
    v233Msg.hwnd = hwnd;
    v233Msg.msg = WM_MOUSEWHEEL;
    myqueue_post(&v233IndexQ, &v233Msg);
    memset(&v233Msg, 0, sizeof(v233Msg));
    v233Msg.size = sizeof(v233Msg);
    v233Msg.hwnd = hwnd;
    v233Msg.msg = WM_KEYDOWN;
    v233Msg.wparam = VK_TAB;
    myqueue_post(&v233IndexQ, &v233Msg);
    BOOL v233SecondLevelIndexOk =
        (v233IndexQ.input_kind_slot_bits[_MSG_INPUT_MOUSE_WHEEL][0] & (1ull << 0)) &&
        (v233IndexQ.input_kind_slot_bits[_MSG_INPUT_KEY][0] & (1ull << 1)) &&
        (v233IndexQ.filter_stage_slot_bits[5][0] & (1ull << 0)) && /* _MSG_FILTER_MENU */
        (v233IndexQ.filter_stage_slot_bits[3][0] & (1ull << 1));  /* _MSG_FILTER_TRANSLATE */
    smoke_expect(&s, v233SecondLevelIndexOk,
                 "v233 queue indexes input-kind and filter-stage slots",
                 "input-kind and predispatch filter stages are now 256-bit selector sources, not candidate-side WM_* branches");
    myqueue_destroy(&v233IndexQ);

    _QueueSelect v233InputSelect;
    myqueue_make_input_select(&v233InputSelect, hwnd, _MSG_INPUT_KIND_BIT(_MSG_INPUT_MOUSE_BUTTON), 0);
    _QueueSelectPlan v233Plan;
    memset(&v233Plan, 0, sizeof(v233Plan));
    int v233PlanOps = myqueue_debug_compile_select_plan(&v233InputSelect, &v233Plan);
    BOOL v233PlanOk = v233PlanOps == 2 &&
                      v233Plan.opCount == 2 &&
                      v233Plan.ops[0] == _QUEUE_SELECT_OP_QS &&
                      v233Plan.ops[1] == _QUEUE_SELECT_OP_HWND &&
                      (v233Plan.fields & _QUEUE_SELECT_INPUT_KIND) &&
                      (v233Plan.indexSourceMask & _QUEUE_SELECT_SOURCE_QS) &&
                      (v233Plan.indexSourceMask & _QUEUE_SELECT_SOURCE_INPUT_KIND) &&
                      !(v233Plan.indexSourceMask & _QUEUE_SELECT_SOURCE_FILTER_STAGE) &&
                      !(v233Plan.ops[0] == _QUEUE_SELECT_OP_INPUT_KIND) &&
                      !(v233Plan.ops[1] == _QUEUE_SELECT_OP_INPUT_KIND);
    smoke_expect(&s, v233PlanOk,
                 "v233 queue selector compiles a predicate op plan",
                 "QS/HWND/range remain compact predicate ops while lane/input/filter constraints are consumed by slot-index sources");

    MyMessageQueue v233CandidateQ;
    myqueue_init(&v233CandidateQ);
    MyMessage v233CandidateMsg;
    memset(&v233CandidateMsg, 0, sizeof(v233CandidateMsg));
    v233CandidateMsg.size = sizeof(v233CandidateMsg);
    v233CandidateMsg.hwnd = hwnd;
    v233CandidateMsg.msg = WM_MOUSEWHEEL;
    for (int i = 0; i < 64; ++i) {
        v233CandidateMsg.wparam = (WPARAM)i;
        myqueue_post(&v233CandidateQ, &v233CandidateMsg);
    }
    memset(&v233CandidateMsg, 0, sizeof(v233CandidateMsg));
    v233CandidateMsg.size = sizeof(v233CandidateMsg);
    v233CandidateMsg.hwnd = hwnd;
    v233CandidateMsg.msg = WM_LBUTTONDOWN;
    v233CandidateMsg.wparam = 0x233;
    myqueue_post(&v233CandidateQ, &v233CandidateMsg);
    uint64_t v233CandidateBefore = v233CandidateQ.indexed_candidate_probes;
    uint64_t v233PredicateBefore = v233CandidateQ.selector_plan_predicate_ops;
    uint64_t v233AcceptBefore = v233CandidateQ.selector_plan_fast_accepts;
    MyMessage v233CandidateOut;
    memset(&v233CandidateOut, 0, sizeof(v233CandidateOut));
    BOOL v233InputCandidateOk = myqueue_peek_select(&v233CandidateQ, &v233CandidateOut, &v233InputSelect) &&
                                v233CandidateOut.msg == WM_LBUTTONDOWN &&
                                (v233CandidateQ.indexed_candidate_probes - v233CandidateBefore) == 1 &&
                                (v233CandidateQ.selector_plan_predicate_ops - v233PredicateBefore) == 2 &&
                                (v233CandidateQ.selector_plan_fast_accepts - v233AcceptBefore) == 1;
    smoke_expect(&s, v233InputCandidateOk,
                 "v233 input-kind selector is index-backed",
                 "QS_MOUSEBUTTON no longer walks mouse-wheel candidates when the selector asks for mouse-button input-kind");
    myqueue_destroy(&v233CandidateQ);

    MyMessageQueue v233FilterQ;
    myqueue_init(&v233FilterQ);
    MyMessage v233FilterMsg;
    memset(&v233FilterMsg, 0, sizeof(v233FilterMsg));
    v233FilterMsg.size = sizeof(v233FilterMsg);
    v233FilterMsg.hwnd = hwnd;
    v233FilterMsg.msg = WM_MOUSEMOVE;
    for (int i = 0; i < 64; ++i) {
        v233FilterMsg.wparam = (WPARAM)i;
        myqueue_post(&v233FilterQ, &v233FilterMsg);
    }
    memset(&v233FilterMsg, 0, sizeof(v233FilterMsg));
    v233FilterMsg.size = sizeof(v233FilterMsg);
    v233FilterMsg.hwnd = hwnd;
    v233FilterMsg.msg = WM_KEYDOWN;
    v233FilterMsg.wparam = VK_TAB;
    myqueue_post(&v233FilterQ, &v233FilterMsg);
    _QueueSelect v233FilterSelect;
    myqueue_make_filter_select(&v233FilterSelect, 0, _MSG_FILTER_TRANSLATE, 0);
    uint64_t v233FilterCandidateBefore = v233FilterQ.indexed_candidate_probes;
    MyMessage v233FilterOut;
    memset(&v233FilterOut, 0, sizeof(v233FilterOut));
    BOOL v233FilterCandidateOk = myqueue_peek_select(&v233FilterQ, &v233FilterOut, &v233FilterSelect) &&
                                  v233FilterOut.msg == WM_KEYDOWN &&
                                  (v233FilterQ.indexed_candidate_probes - v233FilterCandidateBefore) == 1;
    smoke_expect(&s, v233FilterCandidateOk,
                 "v233 filter-stage selector is index-backed",
                 "_MSG_FILTER_TRANSLATE selection intersects filter-stage slot bits before probing hot queue entries");
    myqueue_destroy(&v233FilterQ);

    MyMessageQueue v233BenchQ;
    myqueue_init(&v233BenchQ);
    MyMessage v233BenchMsg;
    memset(&v233BenchMsg, 0, sizeof(v233BenchMsg));
    v233BenchMsg.size = sizeof(v233BenchMsg);
    v233BenchMsg.hwnd = hwnd;
    v233BenchMsg.msg = WM_MOUSEWHEEL;
    for (int i = 0; i < 64; ++i) {
        v233BenchMsg.wparam = (WPARAM)i;
        myqueue_post(&v233BenchQ, &v233BenchMsg);
    }
    memset(&v233BenchMsg, 0, sizeof(v233BenchMsg));
    v233BenchMsg.size = sizeof(v233BenchMsg);
    v233BenchMsg.hwnd = hwnd;
    v233BenchMsg.msg = WM_LBUTTONDOWN;
    myqueue_post(&v233BenchQ, &v233BenchMsg);
    enum { V233_PLAN_PROBES = 2048 };
    uint64_t v233BenchCandidatesBefore = v233BenchQ.indexed_candidate_probes;
    uint64_t v233BenchPlanBefore = v233BenchQ.selector_plan_builds;
    uint64_t v233BenchPredBefore = v233BenchQ.selector_plan_predicate_ops;
    unsigned long long v233Start = smoke_perf_now_us();
    MyMessage v233BenchDummy;
    for (int probe = 0; probe < V233_PLAN_PROBES; ++probe) {
        memset(&v233BenchDummy, 0, sizeof(v233BenchDummy));
        myqueue_peek_select(&v233BenchQ, &v233BenchDummy, &v233InputSelect);
    }
    unsigned long long v233End = smoke_perf_now_us();
    uint64_t v233BenchCandidateDelta = v233BenchQ.indexed_candidate_probes - v233BenchCandidatesBefore;
    uint64_t v233BenchPlanDelta = v233BenchQ.selector_plan_builds - v233BenchPlanBefore;
    uint64_t v233BenchPredDelta = v233BenchQ.selector_plan_predicate_ops - v233BenchPredBefore;
    char v233Bench[384];
    double v233Ms = (double)(v233End - v233Start) / 1000.0;
    double v233OpsS = v233Ms > 0.0 ? ((double)V233_PLAN_PROBES * 1000.0 / v233Ms) : 0.0;
    snprintf(v233Bench, sizeof(v233Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f candidates=%llu plans=%llu pred_ops=%llu input_index=%zu filter_index=%zu",
             V233_PLAN_PROBES, v233Ms, v233OpsS,
             (unsigned long long)v233BenchCandidateDelta,
             (unsigned long long)v233BenchPlanDelta,
             (unsigned long long)v233BenchPredDelta,
             sizeof(v233BenchQ.input_kind_slot_bits),
             sizeof(v233BenchQ.filter_stage_slot_bits));
    smoke_info(s.group, "v233 selector plan/index benchmark", v233Bench);
    myqueue_destroy(&v233BenchQ);

    MyMessageQueue v234CacheQ;
    myqueue_init(&v234CacheQ);
    MyMessage v234CacheMsg;
    memset(&v234CacheMsg, 0, sizeof(v234CacheMsg));
    v234CacheMsg.size = sizeof(v234CacheMsg);
    v234CacheMsg.hwnd = hwnd;
    v234CacheMsg.msg = WM_LBUTTONDOWN;
    v234CacheMsg.wparam = 0x234;
    myqueue_post(&v234CacheQ, &v234CacheMsg);

    enum { V234_CACHE_PROBES = 32 };
    uint64_t v234BuildBefore = v234CacheQ.selector_plan_builds;
    uint64_t v234HitBefore = v234CacheQ.selector_plan_cache_hits;
    BOOL v234CacheResultOk = TRUE;
    for (int probe = 0; probe < V234_CACHE_PROBES; ++probe) {
        MyMessage v234Out;
        memset(&v234Out, 0, sizeof(v234Out));
        if (!myqueue_peek_select(&v234CacheQ, &v234Out, &v233InputSelect) || v234Out.msg != WM_LBUTTONDOWN)
            v234CacheResultOk = FALSE;
    }
    uint64_t v234BuildDelta = v234CacheQ.selector_plan_builds - v234BuildBefore;
    uint64_t v234HitDelta = v234CacheQ.selector_plan_cache_hits - v234HitBefore;
    BOOL v234CacheOk = v234CacheResultOk &&
                       v234BuildDelta == 1 &&
                       v234HitDelta == (V234_CACHE_PROBES - 1);
    smoke_expect(&s, v234CacheOk,
                 "v234 queue selector caches compiled plans",
                 "repeated same-shape Peek/GetMessage selects build one plan and reuse it through the queue-local selector cache");

    _QueueSelect v234WheelSelect;
    myqueue_make_input_select(&v234WheelSelect, hwnd, _MSG_INPUT_KIND_BIT(_MSG_INPUT_MOUSE_WHEEL), 0);
    uint64_t v234SwitchBuildBefore = v234CacheQ.selector_plan_builds;
    uint64_t v234SwitchHitBefore = v234CacheQ.selector_plan_cache_hits;
    MyMessage v234SwitchOut;
    memset(&v234SwitchOut, 0, sizeof(v234SwitchOut));
    (void)myqueue_peek_select(&v234CacheQ, &v234SwitchOut, &v234WheelSelect);
    BOOL v234SwitchOk = (v234CacheQ.selector_plan_builds - v234SwitchBuildBefore) == 1 &&
                        (v234CacheQ.selector_plan_cache_hits - v234SwitchHitBefore) == 0;
    smoke_expect(&s, v234SwitchOk,
                 "v234 selector plan cache invalidates by select signature",
                 "changing input-kind/hwnd/range/QS selector shape recompiles the cached op/index plan instead of reusing stale predicates");
    myqueue_destroy(&v234CacheQ);

    MyMessageQueue v234BenchQ;
    myqueue_init(&v234BenchQ);
    MyMessage v234BenchMsg;
    memset(&v234BenchMsg, 0, sizeof(v234BenchMsg));
    v234BenchMsg.size = sizeof(v234BenchMsg);
    v234BenchMsg.hwnd = hwnd;
    v234BenchMsg.msg = WM_MOUSEWHEEL;
    for (int i = 0; i < 64; ++i) {
        v234BenchMsg.wparam = (WPARAM)i;
        myqueue_post(&v234BenchQ, &v234BenchMsg);
    }
    memset(&v234BenchMsg, 0, sizeof(v234BenchMsg));
    v234BenchMsg.size = sizeof(v234BenchMsg);
    v234BenchMsg.hwnd = hwnd;
    v234BenchMsg.msg = WM_LBUTTONDOWN;
    myqueue_post(&v234BenchQ, &v234BenchMsg);
    enum { V234_PLAN_CACHE_PROBES = 2048 };
    uint64_t v234BenchCandidatesBefore = v234BenchQ.indexed_candidate_probes;
    uint64_t v234BenchPlanBefore = v234BenchQ.selector_plan_builds;
    uint64_t v234BenchHitBefore = v234BenchQ.selector_plan_cache_hits;
    uint64_t v234BenchPredBefore = v234BenchQ.selector_plan_predicate_ops;
    unsigned long long v234Start = smoke_perf_now_us();
    MyMessage v234BenchDummy;
    for (int probe = 0; probe < V234_PLAN_CACHE_PROBES; ++probe) {
        memset(&v234BenchDummy, 0, sizeof(v234BenchDummy));
        myqueue_peek_select(&v234BenchQ, &v234BenchDummy, &v233InputSelect);
    }
    unsigned long long v234End = smoke_perf_now_us();
    uint64_t v234BenchCandidateDelta = v234BenchQ.indexed_candidate_probes - v234BenchCandidatesBefore;
    uint64_t v234BenchPlanDelta = v234BenchQ.selector_plan_builds - v234BenchPlanBefore;
    uint64_t v234BenchHitDelta = v234BenchQ.selector_plan_cache_hits - v234BenchHitBefore;
    uint64_t v234BenchPredDelta = v234BenchQ.selector_plan_predicate_ops - v234BenchPredBefore;
    char v234Bench[448];
    double v234Ms = (double)(v234End - v234Start) / 1000.0;
    double v234OpsS = v234Ms > 0.0 ? ((double)V234_PLAN_CACHE_PROBES * 1000.0 / v234Ms) : 0.0;
    snprintf(v234Bench, sizeof(v234Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f candidates=%llu plan_builds=%llu cache_hits=%llu pred_ops=%llu index_source_mask=0x%x",
             V234_PLAN_CACHE_PROBES, v234Ms, v234OpsS,
             (unsigned long long)v234BenchCandidateDelta,
             (unsigned long long)v234BenchPlanDelta,
             (unsigned long long)v234BenchHitDelta,
             (unsigned long long)v234BenchPredDelta,
             (unsigned)v234BenchQ.selector_plan_cache.indexSourceMask);
    smoke_info(s.group, "v234 cached selector-plan benchmark", v234Bench);
    myqueue_destroy(&v234BenchQ);

    MyMessageQueue v235IndexQ;
    myqueue_init(&v235IndexQ);
    HWND v235OtherHwnd = hwnd + 1u;
    while (smoke_v235_hwnd_bucket(v235OtherHwnd) == smoke_v235_hwnd_bucket(hwnd))
        v235OtherHwnd += 0x11u;
    MyMessage v235IdxMsg;
    memset(&v235IdxMsg, 0, sizeof(v235IdxMsg));
    v235IdxMsg.size = sizeof(v235IdxMsg);
    v235IdxMsg.hwnd = hwnd;
    v235IdxMsg.msg = WM_USER + 8u;
    myqueue_post(&v235IndexQ, &v235IdxMsg);
    memset(&v235IdxMsg, 0, sizeof(v235IdxMsg));
    v235IdxMsg.size = sizeof(v235IdxMsg);
    v235IdxMsg.hwnd = v235OtherHwnd;
    v235IdxMsg.msg = WM_USER + 7u;
    myqueue_post(&v235IndexQ, &v235IdxMsg);
    uint32_t v235HwndBucket = smoke_v235_hwnd_bucket(v235OtherHwnd);
    uint32_t v235MsgBucket = smoke_v235_msg_bucket(WM_USER + 7u);
    BOOL v235BucketMaintenanceOk =
        smoke_v235_bucket_bit_is_set(v235IndexQ.hwnd_bucket_slot_bits[v235HwndBucket], 1) &&
        smoke_v235_bucket_bit_is_set(v235IndexQ.msg_bucket_slot_bits[v235MsgBucket], 1);
    smoke_expect(&s, v235BucketMaintenanceOk,
                 "v235 queue indexes HWND/message buckets",
                 "HWND and exact/small message-range selectors now have bounded 64-bucket slot indexes before exact predicate validation");

    _QueueSelect v235ExactMsgSelect;
    myqueue_make_select(&v235ExactMsgSelect, 0, WM_USER + 7u, WM_USER + 7u, 0);
    _QueueSelectPlan v235MsgPlan;
    memset(&v235MsgPlan, 0, sizeof(v235MsgPlan));
    myqueue_debug_compile_select_plan(&v235ExactMsgSelect, &v235MsgPlan);
    _QueueSelect v235HwndSelect;
    myqueue_make_select(&v235HwndSelect, v235OtherHwnd, 0, 0, 0);
    _QueueSelectPlan v235HwndPlan;
    memset(&v235HwndPlan, 0, sizeof(v235HwndPlan));
    myqueue_debug_compile_select_plan(&v235HwndSelect, &v235HwndPlan);
    BOOL v235PlanSourcesOk =
        (v235MsgPlan.indexSourceMask & _QUEUE_SELECT_SOURCE_MSG_BUCKET) &&
        (v235MsgPlan.indexSourceMask & _QUEUE_SELECT_SOURCE_QS) &&
        (v235MsgPlan.ops[v235MsgPlan.opCount - 1] == _QUEUE_SELECT_OP_MSG_RANGE) &&
        (v235HwndPlan.indexSourceMask & _QUEUE_SELECT_SOURCE_HWND_BUCKET) &&
        v235HwndPlan.opCount >= 1 &&
        v235HwndPlan.ops[v235HwndPlan.opCount - 1] == _QUEUE_SELECT_OP_HWND;
    smoke_expect(&s, v235PlanSourcesOk,
                 "v235 selector plan uses HWND/message index sources",
                 "compiled selector plans intersect HWND/message buckets first while retaining HWND/range predicate ops as collision guards");

    MyMessage v235IndexOut;
    memset(&v235IndexOut, 0, sizeof(v235IndexOut));
    uint64_t v235MsgCandidateBefore = v235IndexQ.indexed_candidate_probes;
    BOOL v235MsgCandidateOk = myqueue_peek_select(&v235IndexQ, &v235IndexOut, &v235ExactMsgSelect) &&
                              v235IndexOut.msg == WM_USER + 7u &&
                              (v235IndexQ.indexed_candidate_probes - v235MsgCandidateBefore) == 1;
    smoke_expect(&s, v235MsgCandidateOk,
                 "v235 exact message selector is bucket-indexed",
                 "exact WM_USER+7 selection skips same-lane/QS WM_USER+8 candidates before hot-entry predicate checks");
    myqueue_destroy(&v235IndexQ);

    MyMessageQueue v235BenchQ;
    myqueue_init(&v235BenchQ);
    MyMessage v235BenchMsg;
    memset(&v235BenchMsg, 0, sizeof(v235BenchMsg));
    v235BenchMsg.size = sizeof(v235BenchMsg);
    v235BenchMsg.hwnd = hwnd;
    v235BenchMsg.msg = WM_USER + 8u;
    for (int i = 0; i < 64; ++i) {
        v235BenchMsg.wparam = (WPARAM)i;
        myqueue_post(&v235BenchQ, &v235BenchMsg);
    }
    memset(&v235BenchMsg, 0, sizeof(v235BenchMsg));
    v235BenchMsg.size = sizeof(v235BenchMsg);
    v235BenchMsg.hwnd = hwnd;
    v235BenchMsg.msg = WM_USER + 7u;
    myqueue_post(&v235BenchQ, &v235BenchMsg);
    enum { V235_BUCKET_PROBES = 2048 };
    uint64_t v235BenchCandidatesBefore = v235BenchQ.indexed_candidate_probes;
    uint64_t v235BenchPlanBefore = v235BenchQ.selector_plan_builds;
    uint64_t v235BenchHitBefore = v235BenchQ.selector_plan_cache_hits;
    uint64_t v235BenchPredBefore = v235BenchQ.selector_plan_predicate_ops;
    unsigned long long v235Start = smoke_perf_now_us();
    MyMessage v235BenchDummy;
    for (int probe = 0; probe < V235_BUCKET_PROBES; ++probe) {
        memset(&v235BenchDummy, 0, sizeof(v235BenchDummy));
        myqueue_peek_select(&v235BenchQ, &v235BenchDummy, &v235ExactMsgSelect);
    }
    unsigned long long v235End = smoke_perf_now_us();
    uint64_t v235BenchCandidateDelta = v235BenchQ.indexed_candidate_probes - v235BenchCandidatesBefore;
    uint64_t v235BenchPlanDelta = v235BenchQ.selector_plan_builds - v235BenchPlanBefore;
    uint64_t v235BenchHitDelta = v235BenchQ.selector_plan_cache_hits - v235BenchHitBefore;
    uint64_t v235BenchPredDelta = v235BenchQ.selector_plan_predicate_ops - v235BenchPredBefore;
    char v235Bench[512];
    double v235Ms = (double)(v235End - v235Start) / 1000.0;
    double v235OpsS = v235Ms > 0.0 ? ((double)V235_BUCKET_PROBES * 1000.0 / v235Ms) : 0.0;
    snprintf(v235Bench, sizeof(v235Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f candidates=%llu plan_builds=%llu cache_hits=%llu pred_ops=%llu index_source_mask=0x%x hwnd_index=%zu msg_index=%zu",
             V235_BUCKET_PROBES, v235Ms, v235OpsS,
             (unsigned long long)v235BenchCandidateDelta,
             (unsigned long long)v235BenchPlanDelta,
             (unsigned long long)v235BenchHitDelta,
             (unsigned long long)v235BenchPredDelta,
             (unsigned)v235BenchQ.selector_plan_cache.indexSourceMask,
             sizeof(v235BenchQ.hwnd_bucket_slot_bits),
             sizeof(v235BenchQ.msg_bucket_slot_bits));
    smoke_info(s.group, "v235 HWND/message bucket selector benchmark", v235Bench);
    myqueue_destroy(&v235BenchQ);

    enum { V225_ROUTE_PROBES = 2048 };
    unsigned long long v225Start = smoke_perf_now_us();
    for (int probe = 0; probe < V225_ROUTE_PROBES; ++probe) {
        MyMessage a;
        memset(&a, 0, sizeof(a));
        a.size = sizeof(a);
        a.hwnd = hwnd;
        a.msg = (probe & 1) ? WM_MOUSEWHEEL : WM_KEYDOWN;
        a.input_kind = mymsg_default_input_kind(a.msg);
        a.route_reason = mymsg_default_route_reason(a.msg, 0, a.input_kind);
        a.route_action = mymsg_required_hwnd_action_for_route(_MSG_LANE_INPUT, a.input_kind, a.route_reason);
        _MsgRouteDescriptor r;
        mymsg_make_route_descriptor(&a, &r);
        if (!r.hwnd_action) v225ReasonOk = FALSE;
    }
    unsigned long long v225End = smoke_perf_now_us();
    char v225Bench[256];
    double v225Ms = (double)(v225End - v225Start) / 1000.0;
    double v225OpsS = v225Ms > 0.0 ? ((double)V225_ROUTE_PROBES * 1000.0 / v225Ms) : 0.0;
    snprintf(v225Bench, sizeof(v225Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f reasons=focus/capture/hittest/hover/hook/dialog",
             V225_ROUTE_PROBES, v225Ms, v225OpsS);
    smoke_info(s.group, "v225 route descriptor benchmark", v225Bench);
    myqueue_destroy(&v225Q);

    enum { V224_INPUT_PROBES = 2048 };
    unsigned long long v224Start = smoke_perf_now_us();
    for (int probe = 0; probe < V224_INPUT_PROBES; ++probe) {
        MyMessage a;
        memset(&a, 0, sizeof(a));
        a.size = sizeof(a);
        a.hwnd = hwnd;
        a.msg = (probe & 1) ? WM_MOUSEMOVE : WM_KEYDOWN;
        myqueue_post(&v224Q, &a);
        _QueueSelect sel;
        myqueue_make_input_select(&sel, hwnd, _MSG_INPUT_KIND_MASK_ALL, 1);
        MyMessage b;
        memset(&b, 0, sizeof(b));
        myqueue_peek_select(&v224Q, &b, &sel);
    }
    unsigned long long v224End = smoke_perf_now_us();
    char v224Bench[256];
    double v224Ms = (double)(v224End - v224Start) / 1000.0;
    double v224OpsS = v224Ms > 0.0 ? ((double)V224_INPUT_PROBES * 1000.0 / v224Ms) : 0.0;
    snprintf(v224Bench, sizeof(v224Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f input_kinds=key/char/mouse-move/button/wheel",
             V224_INPUT_PROBES, v224Ms, v224OpsS);
    smoke_info(s.group, "v224 input-kind queue selector benchmark", v224Bench);
    myqueue_destroy(&v224Q);

    enum { V223_QUEUE_PROBES = 2048 };
    unsigned long long v223Start = smoke_perf_now_us();
    for (int probe = 0; probe < V223_QUEUE_PROBES; ++probe) {
        MyMessage a;
        memset(&a, 0, sizeof(a));
        a.size = sizeof(a);
        a.hwnd = hwnd;
        a.msg = (probe & 1) ? WM_MOUSEMOVE : WM_COMMAND;
        myqueue_post(&v223Q, &a);
        MyMessage b;
        memset(&b, 0, sizeof(b));
        myqueue_peek_match(&v223Q, &b, 0, 0, 0, 1);
    }
    unsigned long long v223End = smoke_perf_now_us();
    char v223Bench[256];
    double v223Ms = (double)(v223End - v223Start) / 1000.0;
    double v223OpsS = v223Ms > 0.0 ? ((double)V223_QUEUE_PROBES * 1000.0 / v223Ms) : 0.0;
    snprintf(v223Bench, sizeof(v223Bench),
             "probes=%d wall_ms=%.3f ops_s=%.0f lanes=input/window/posted/timer/send",
             V223_QUEUE_PROBES, v223Ms, v223OpsS);
    smoke_info(s.group, "v223 queue lane selector benchmark", v223Bench);
    myqueue_destroy(&v223Q);

    smoke_expect(&s, g_user32_created >= 1, "WM_CREATE delivery", "CreateWindowExA calls WndProc synchronously");

    smoke_expect(&s, hwnd && SendMessageA(hwnd, WM_USER + 104, 0, 0) == 104 && g_user32_user == 1,
                 "SendMessageA", "sync dispatch to WndProc");
    smoke_expect(&s, hwnd && PostMessageA(hwnd, WM_COMMAND, 0, 0), "PostMessageA", "queue WM_COMMAND");

    MSG msg;
    memset(&msg, 0, sizeof(msg));
    BOOL got = PeekMessageA(&msg, hwnd, 0, 0, PM_REMOVE);
    smoke_expect(&s, got && msg.message == WM_COMMAND && msg.hwnd == hwnd,
                 "PeekMessageA(PM_REMOVE)", "retrieves posted WM_COMMAND");
    if (got) DispatchMessageA(&msg);
    smoke_expect(&s, g_user32_command == 1, "DispatchMessageA", "posted WM_COMMAND dispatched once");

    g_user32_user = 0;
    MSG manual;
    memset(&manual, 0, sizeof(manual));
    manual.hwnd = hwnd;
    manual.message = WM_USER + 104;
    smoke_expect(&s, DispatchMessageA(&manual) == 104 && g_user32_user == 1,
                 "DispatchMessageA public MSG", "manual MSDN-shaped MSG dispatches without private sidecar");

    char text[80];
    memset(text, 0, sizeof(text));
    smoke_expect(&s, hwnd && SetWindowTextA(hwnd, "Smoke title"), "SetWindowTextA", "change title");
    smoke_expect(&s, hwnd && GetWindowTextA(hwnd, text, sizeof(text)) > 0 && strcmp(text, "Smoke title") == 0,
                 "GetWindowTextA", text);

    RECT rc;
    memset(&rc, 0, sizeof(rc));
    smoke_expect(&s, hwnd && GetWindowRect(hwnd, &rc) && rc.left == 40 && rc.top == 40 && rc.right == 280 && rc.bottom == 200,
                 "GetWindowRect", "standalone USER32 HWND initial rect is public-contract visible");

    smoke_expect(&s, hwnd && MoveWindow(hwnd, 80, 90, 300, 190, FALSE),
                 "MoveWindow", "standalone USER32 HWND moves without WindowManager slot");
    memset(&rc, 0, sizeof(rc));
    smoke_expect(&s, GetWindowRect(hwnd, &rc) && rc.left == 80 && rc.top == 90 && rc.right == 380 && rc.bottom == 280,
                 "MoveWindow/GetWindowRect", "left/top/size updated");
    smoke_expect(&s, g_user32_windowpos_changing == 1 && g_user32_windowpos_changed == 1,
                 "SetWindowPos messages", "WM_WINDOWPOSCHANGING/CHANGED delivered once");
    smoke_expect(&s, g_user32_move == 1 && g_user32_size == 1,
                 "MoveWindow messages", "WM_MOVE/WM_SIZE delivered for local geometry update");

    SetLastError(0x12345678u);
    smoke_expect(&s, !GetWindowRect(hwnd, NULL), "GetWindowRect(NULL)", "invalid lpRect rejected");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "GetWindowRect(NULL) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, !MoveWindow((HWND)0, 1, 2, 3, 4, TRUE), "MoveWindow(NULL)", "invalid HWND rejected");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "MoveWindow(NULL) LastError");

    smoke_expect(&s, hwnd && DestroyWindow(hwnd), "DestroyWindow", "top-level HWND destroyed");
    smoke_expect(&s, g_user32_destroyed == 1, "WM_DESTROY delivery", "DestroyWindow calls WndProc");
    smoke_expect(&s, hwnd && !IsWindow(hwnd), "IsWindow after DestroyWindow", "HWND no longer valid");

    HWND hwnd2 = atom ? CreateWindowExA(0, wc.lpszClassName, "Smoke USER32 Reuse", WS_OVERLAPPEDWINDOW,
                                       10, 10, 80, 60, 0, 0, 0, NULL) : 0;
    _HwndHeader hwndHdr2;
    DWORD hwndSlot2 = 0, hwndGen2 = 0;
    memset(&hwndHdr2, 0, sizeof(hwndHdr2));
    BOOL hwndGenerationOk = hwnd2 && hwnd_decode(hwnd2, &hwndSlot2, &hwndGen2) &&
                            hwnd_query_header(&rt->mgr, hwnd2, &hwndHdr2) &&
                            !IsWindow(hwnd) &&
                            (hwnd2 != hwnd) &&
                            (hwndSlot2 != hwndSlot || hwndGen2 != hwndGen);
    char v219GenDetail[256];
    snprintf(v219GenDetail, sizeof(v219GenDetail),
             "old_hwnd=0x%x old_slot=%lu old_gen=%lu new_hwnd=0x%x new_slot=%lu new_gen=%lu old_invalid=%d",
             (unsigned)hwnd, (unsigned long)hwndSlot, (unsigned long)hwndGen,
             (unsigned)hwnd2, (unsigned long)hwndSlot2, (unsigned long)hwndGen2,
             (!IsWindow(hwnd)) ? 1 : 0);
    smoke_expect(&s, hwndGenerationOk,
                 "v219 HWND generation invalidates stale window numbers",
                 v219GenDetail);
    if (hwnd2) DestroyWindow(hwnd2);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static HWND smoke_create_basic_window(SmokeRuntime* rt, const char* className, const char* title, SmokeContext* s)
{
    smoke_runtime_init(rt);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_wndproc;
    wc.lpszClassName = className;
    ATOM atom = RegisterClassExA(&wc);
    smoke_expect(s, atom != 0, "RegisterClassExA", className);
    HWND hwnd = atom ? CreateWindowExA(0, className, title, WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                      30, 30, 220, 140, 0, 0, 0, NULL) : 0;
    smoke_expect(s, hwnd != 0, "CreateWindowExA", title);
    return hwnd;
}

static int smoke_gdi(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "gdi";
    g_user32_created = g_user32_destroyed = 0;
    HWND hwnd = smoke_create_basic_window(rt, "myOS.v129.SmokeGdi", "Smoke GDI", &s);

    RECT dirty = {4, 5, 64, 48};
    smoke_expect(&s, hwnd && InvalidateRect(hwnd, &dirty, TRUE), "InvalidateRect", "marks HWND dirty and posts paint intent");
    MYGDI_WINDOW_SNAPSHOT snap;
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, hwnd && MyGdiGetWindowState(hwnd, &snap) && snap.dirty,
                 "MyGdiGetWindowState dirty", "GDI private snapshot is the paint tripwire");
    smoke_expect(&s, snap.dirtyRect.left == 4 && snap.dirtyRect.top == 5 && snap.dirtyRect.right == 64 && snap.dirtyRect.bottom == 48,
                 "dirty rect preserved", "InvalidateRect stores exact smoke rect");

    PAINTSTRUCT ps;
    memset(&ps, 0, sizeof(ps));
    HDC paintDc = hwnd ? BeginPaint(hwnd, &ps) : 0;
    smoke_expect(&s, paintDc != 0 && ps.hdc == paintDc, "BeginPaint", "allocates paint DC");
    smoke_expect(&s, ps.rcPaint.left == 4 && ps.rcPaint.top == 5 && ps.rcPaint.right == 64 && ps.rcPaint.bottom == 48,
                 "BeginPaint rcPaint", "uses invalidated rect");

    HBRUSH br = CreateSolidBrush(RGB(12,34,56));
    smoke_expect(&s, br != 0, "CreateSolidBrush", "brush object allocated");
    HGDIOBJ old = (paintDc && br) ? SelectObject(paintDc, (HGDIOBJ)br) : 0;
    smoke_expect(&s, br && MyGdiGetBrushSelectedCount(br) == 1, "SelectObject brush count", "brush selected into paint DC");
    (void)old;
    smoke_expect(&s, paintDc && FillRect(paintDc, &dirty, br) != 0, "FillRect", "records fill command");
    smoke_expect(&s, paintDc && Rectangle(paintDc, 6, 7, 60, 44), "Rectangle", "records rect command");
    smoke_expect(&s, paintDc && TextOutA(paintDc, 8, 9, "v128", 4), "TextOutA", "records text command");
    smoke_expect(&s, EndPaint(hwnd, &ps), "EndPaint", "releases paint DC and unselects brush");
    smoke_expect(&s, br && MyGdiGetBrushSelectedCount(br) == 0, "EndPaint unselects brush", "selected count returns to zero");
    smoke_expect(&s, br && DeleteObject((HGDIOBJ)br), "DeleteObject brush", "brush lifetime closed");

    HDC wndDc = hwnd ? GetDC(hwnd) : 0;
    smoke_expect(&s, wndDc != 0, "GetDC", "window DC allocated");
    smoke_expect(&s, wndDc && ReleaseDC(hwnd, wndDc) == 1, "ReleaseDC", "window DC released");
    smoke_expect(&s, ValidateRect(hwnd, NULL), "ValidateRect", "clears paint state");

    if (hwnd) DestroyWindow(hwnd);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static int smoke_gdi_bitmap_dc(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"gdi_bitmap_dc"};
    HWND hwnd = smoke_create_basic_window(rt, "myOS.v156.GdiBitmapSmoke", "Smoke GDI Bitmap", &s);

    HDC mem = CreateCompatibleDC(0);
    smoke_expect(&s, mem != 0, "CreateCompatibleDC(NULL)", "memory DC allocated");
    SetLastError(0x12345678u);
    smoke_expect(&s, mem && ReleaseDC(0, mem) == 0, "ReleaseDC(memory DC)", "memory DC is not released by ReleaseDC");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "ReleaseDC(memory DC) LastError");

    HBITMAP bmp = mem ? CreateCompatibleBitmap(mem, 8, 6) : 0;
    smoke_expect(&s, bmp != 0, "CreateCompatibleBitmap", "bitmap object allocated");
    BITMAP bm;
    memset(&bm, 0, sizeof(bm));
    smoke_expect(&s, bmp && GetObjectA((HGDIOBJ)bmp, sizeof(bm), &bm) == (int)sizeof(bm),
                 "GetObjectA(HBITMAP)", "returns BITMAP metadata");
    smoke_expect(&s, bm.bmWidth == 8 && bm.bmHeight == 6 && bm.bmBitsPixel == 32 && bm.bmWidthBytes == 32,
                 "BITMAP fields", "compatible bitmap is 8x6x32bpp");

    HGDIOBJ oldBmp = (mem && bmp) ? SelectObject(mem, (HGDIOBJ)bmp) : 0;
    smoke_expect(&s, oldBmp != 0, "SelectObject bitmap", "memory DC accepts HBITMAP and returns default bitmap");
    smoke_expect(&s, bmp && !DeleteObject((HGDIOBJ)bmp), "DeleteObject selected bitmap", "selected bitmap is protected");

    HDC wndDc = hwnd ? GetDC(hwnd) : 0;
    smoke_expect(&s, wndDc != 0, "GetDC fixture", "window DC for invalid bitmap select");
    smoke_expect(&s, wndDc && bmp && SelectObject(wndDc, (HGDIOBJ)bmp) == 0,
                 "SelectObject bitmap into window DC", "HBITMAP can only be selected into memory DC");
    if (wndDc) ReleaseDC(hwnd, wndDc);

    HBRUSH red = CreateSolidBrush(RGB(200,10,20));
    RECT fill = {1, 1, 5, 4};
    smoke_expect(&s, red && FillRect(mem, &fill, red) != 0, "FillRect(memory DC)", "writes selected bitmap pixels");
    smoke_expect(&s, GetPixel(mem, 2, 2) == RGB(200,10,20), "GetPixel after FillRect", "bitmap pixel changed by FillRect");
    smoke_expect(&s, SetPixel(mem, 7, 5, RGB(1,2,3)) == RGB(1,2,3), "SetPixel", "memory DC pixel write");
    smoke_expect(&s, GetPixel(mem, 7, 5) == RGB(1,2,3), "GetPixel", "memory DC pixel roundtrip");

    HDC mem2 = CreateCompatibleDC(mem);
    HBITMAP bmp2 = mem2 ? CreateCompatibleBitmap(mem2, 8, 6) : 0;
    HGDIOBJ oldBmp2 = (mem2 && bmp2) ? SelectObject(mem2, (HGDIOBJ)bmp2) : 0;
    smoke_expect(&s, mem2 && bmp2 && oldBmp2, "second memory DC", "destination bitmap fixture allocated");
    smoke_expect(&s, mem2 && BitBlt(mem2, 0, 0, 8, 6, mem, 0, 0, SRCCOPY),
                 "BitBlt memory->memory", "SRCCOPY copies pixels between bitmaps");
    smoke_expect(&s, GetPixel(mem2, 2, 2) == RGB(200,10,20) && GetPixel(mem2, 7, 5) == RGB(1,2,3),
                 "BitBlt copied pixels", "destination bitmap contains source pixels");
    SetLastError(0x12345678u);
    smoke_expect(&s, !BitBlt(mem2, 0, 0, 1, 1, mem, 0, 0, 0xDEADBEEFu),
                 "BitBlt unsupported ROP", "only SRCCOPY is supported in v156");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "BitBlt unsupported ROP LastError");

    PAINTSTRUCT ps;
    memset(&ps, 0, sizeof(ps));
    HDC paint = hwnd ? BeginPaint(hwnd, &ps) : 0;
    smoke_expect(&s, paint != 0, "BeginPaint fixture", "paint DC for memory->window blit");
    smoke_expect(&s, paint && BitBlt(paint, 3, 4, 4, 3, mem, 1, 1, SRCCOPY),
                 "BitBlt memory->paintDC", "appends bitmap blit command for window rendering");
    if (paint) EndPaint(hwnd, &ps);

    if (mem2 && oldBmp2) SelectObject(mem2, oldBmp2);
    smoke_expect(&s, bmp2 && DeleteObject((HGDIOBJ)bmp2), "DeleteObject unselected bitmap2", "bitmap deletes after unselect");
    smoke_expect(&s, mem2 && DeleteDC(mem2), "DeleteDC second memory DC", "memory DC lifetime closed");

    if (mem && oldBmp) SelectObject(mem, oldBmp);
    smoke_expect(&s, bmp && DeleteObject((HGDIOBJ)bmp), "DeleteObject unselected bitmap", "bitmap deletes after restoring default selection");
    smoke_expect(&s, mem && DeleteDC(mem), "DeleteDC memory DC", "memory DC deletes its default bitmap");
    smoke_expect(&s, red && DeleteObject((HGDIOBJ)red), "DeleteObject brush", "brush lifetime closed");

    SetLastError(0x12345678u);
    smoke_expect(&s, CreateCompatibleBitmap((HDC)0x777777u, 2, 2) == 0,
                 "CreateCompatibleBitmap invalid DC", "rejects bad source DC");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "CreateCompatibleBitmap invalid DC LastError");

    if (hwnd) DestroyWindow(hwnd);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static int smoke_gdi_dibsection(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"gdi_dibsection"};
    (void)rt;

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 4;
    bmi.bmiHeader.biHeight = -4;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP dib = CreateDIBSection(0, &bmi, DIB_RGB_COLORS, &bits, 0, 0);
    smoke_expect(&s, dib != 0 && bits != NULL, "CreateDIBSection top-down", "32bpp BI_RGB DIBSection returns writable bits");

    BITMAP bm;
    memset(&bm, 0, sizeof(bm));
    smoke_expect(&s, dib && GetObjectA((HGDIOBJ)dib, sizeof(bm), &bm) == (int)sizeof(bm),
                 "GetObjectA DIB as BITMAP", "sizeof(BITMAP) requests BITMAP payload");
    smoke_expect(&s, bm.bmWidth == 4 && bm.bmHeight == 4 && bm.bmBitsPixel == 32 && bm.bmBits == bits,
                 "DIB BITMAP fields", "bmBits points at the DIBSection storage");

    DIBSECTION ds;
    memset(&ds, 0, sizeof(ds));
    smoke_expect(&s, dib && GetObjectA((HGDIOBJ)dib, sizeof(ds), &ds) == (int)sizeof(ds),
                 "GetObjectA DIBSECTION", "large buffer returns DIBSECTION for CreateDIBSection bitmap");
    smoke_expect(&s, ds.dsBmih.biWidth == 4 && ds.dsBmih.biHeight == -4 && ds.dsBmih.biBitCount == 32 && ds.dsBm.bmBits == bits,
                 "DIBSECTION fields", "header sign and bits pointer are preserved");

    HDC mem = CreateCompatibleDC(0);
    HGDIOBJ old = (mem && dib) ? SelectObject(mem, (HGDIOBJ)dib) : 0;
    smoke_expect(&s, mem && old != 0, "SelectObject DIBSection", "memory DC accepts DIBSection HBITMAP");
    smoke_expect(&s, dib && !DeleteObject((HGDIOBJ)dib), "DeleteObject selected DIBSection", "selected bitmap lifetime is protected");

    DWORD* px = (DWORD*)bits;
    if (px) px[1 * 4 + 2] = RGB(11,22,33);
    smoke_expect(&s, mem && GetPixel(mem, 2, 1) == RGB(11,22,33),
                 "direct bits -> GetPixel", "top-down DIB memory writes are visible through selected DC");
    smoke_expect(&s, mem && SetPixel(mem, 3, 2, RGB(44,55,66)) == RGB(44,55,66),
                 "SetPixel DIBSection", "GDI writes selected DIBSection pixels");
    smoke_expect(&s, px && px[2 * 4 + 3] == RGB(44,55,66),
                 "SetPixel -> direct bits", "top-down DIB bits observe GDI writes");

    HDC mem2 = CreateCompatibleDC(0);
    HBITMAP compat = mem2 ? CreateCompatibleBitmap(mem2, 4, 4) : 0;
    HGDIOBJ old2 = (mem2 && compat) ? SelectObject(mem2, (HGDIOBJ)compat) : 0;
    smoke_expect(&s, mem2 && compat && old2, "compatible target", "BitBlt fixture allocated");
    smoke_expect(&s, mem2 && BitBlt(mem2, 0, 0, 4, 4, mem, 0, 0, SRCCOPY),
                 "BitBlt DIBSection -> compatible", "DIBSection participates as source bitmap");
    smoke_expect(&s, mem2 && GetPixel(mem2, 2, 1) == RGB(11,22,33) && GetPixel(mem2, 3, 2) == RGB(44,55,66),
                 "BitBlt DIB pixels copied", "destination gets directly-written and GDI-written pixels");

    if (old2 && mem2) SelectObject(mem2, old2);
    if (compat) DeleteObject((HGDIOBJ)compat);
    if (mem2) DeleteDC(mem2);
    if (old && mem) SelectObject(mem, old);
    if (mem) DeleteDC(mem);
    smoke_expect(&s, dib && DeleteObject((HGDIOBJ)dib), "DeleteObject DIBSection", "unselected DIBSection is released");

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 3;
    bmi.bmiHeader.biHeight = 3;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bottomBits = NULL;
    HBITMAP bottom = CreateDIBSection(0, &bmi, DIB_RGB_COLORS, &bottomBits, 0, 0);
    HDC bottomDc = CreateCompatibleDC(0);
    HGDIOBJ bottomOld = (bottomDc && bottom) ? SelectObject(bottomDc, (HGDIOBJ)bottom) : 0;
    smoke_expect(&s, bottom && bottomBits && bottomDc && bottomOld,
                 "CreateDIBSection bottom-up", "positive biHeight creates bottom-up storage mapping");
    DWORD* bp = (DWORD*)bottomBits;
    if (bp) bp[0] = RGB(77,88,99);
    smoke_expect(&s, bottomDc && GetPixel(bottomDc, 0, 2) == RGB(77,88,99),
                 "bottom-up bits -> GetPixel", "first DIB row maps to bottom scanline");
    smoke_expect(&s, bottomDc && SetPixel(bottomDc, 1, 0, RGB(12,21,30)) == RGB(12,21,30),
                 "SetPixel bottom-up", "top logical row maps to last physical scanline");
    smoke_expect(&s, bp && bp[(3 - 1) * 3 + 1] == RGB(12,21,30),
                 "bottom-up SetPixel bits", "DIB storage orientation is respected");
    if (bottomOld && bottomDc) SelectObject(bottomDc, bottomOld);
    if (bottomDc) DeleteDC(bottomDc);
    if (bottom) DeleteObject((HGDIOBJ)bottom);

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 2;
    bmi.bmiHeader.biHeight = -2;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    bits = (void*)0x1;
    SetLastError(0x12345678u);
    smoke_expect(&s, CreateDIBSection(0, &bmi, DIB_RGB_COLORS, &bits, 0, 0) == 0 && bits == NULL,
                 "CreateDIBSection unsupported bpp", "v160 rejects non-32bpp instead of fake-supporting it");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "CreateDIBSection 24bpp LastError");

    bmi.bmiHeader.biBitCount = 32;
    bits = (void*)0x1;
    SetLastError(0x12345678u);
    smoke_expect(&s, CreateDIBSection(0, &bmi, DIB_PAL_COLORS, &bits, 0, 0) == 0 && bits == NULL,
                 "CreateDIBSection DIB_PAL_COLORS", "paletted color table mode is explicitly unsupported in v160");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "CreateDIBSection DIB_PAL_COLORS LastError");

    bits = (void*)0x1;
    SetLastError(0x12345678u);
    smoke_expect(&s, CreateDIBSection(0, &bmi, DIB_RGB_COLORS, &bits, (HANDLE)0x6000, 0) == 0 && bits == NULL,
                 "CreateDIBSection hSection", "section-backed DIBs are rejected until real mapping support lands");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "CreateDIBSection hSection LastError");

    bits = (void*)0x1;
    SetLastError(0x12345678u);
    smoke_expect(&s, CreateDIBSection((HDC)0x777777u, &bmi, DIB_RGB_COLORS, &bits, 0, 0) == 0 && bits == NULL,
                 "CreateDIBSection invalid HDC", "bad DC handles fail with invalid handle");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "CreateDIBSection invalid HDC LastError");

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_gdi_dibbits(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"gdi_dibbits"};
    (void)rt;

    HDC mem = CreateCompatibleDC(0);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(mem, 3, 3) : 0;
    smoke_expect(&s, mem && bmp, "DIBbits fixture", "memory DC and unselected compatible bitmap allocated");

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 3;
    bmi.bmiHeader.biHeight = -3;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    DWORD topDown[9] = {
        RGB(10, 1, 1), RGB(20, 2, 2), RGB(30, 3, 3),
        RGB(40, 4, 4), RGB(50, 5, 5), RGB(60, 6, 6),
        RGB(70, 7, 7), RGB(80, 8, 8), RGB(90, 9, 9)
    };

    int setLines = bmp ? SetDIBits(mem, bmp, 0, 3, topDown, &bmi, DIB_RGB_COLORS) : 0;
    smoke_expect(&s, setLines == 3, "SetDIBits top-down", "unselected bitmap accepts 32bpp BI_RGB DIB rows");
    HGDIOBJ old = (mem && bmp) ? SelectObject(mem, (HGDIOBJ)bmp) : 0;
    smoke_expect(&s, old != 0, "SelectObject after SetDIBits", "select for pixel verification");
    smoke_expect(&s, mem && GetPixel(mem, 0, 0) == RGB(10,1,1) && GetPixel(mem, 2, 2) == RGB(90,9,9),
                 "SetDIBits pixels", "top-down source rows map to top-left GDI coordinates");

    DWORD failBuf[9];
    memset(failBuf, 0, sizeof(failBuf));
    SetLastError(0x12345678u);
    smoke_expect(&s, bmp && GetDIBits(mem, bmp, 0, 3, failBuf, &bmi, DIB_RGB_COLORS) == 0,
                 "GetDIBits selected bitmap", "MSDN contract rejects selected HBITMAP");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "GetDIBits selected LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, bmp && SetDIBits(mem, bmp, 0, 3, topDown, &bmi, DIB_RGB_COLORS) == 0,
                 "SetDIBits selected bitmap", "MSDN contract rejects selected HBITMAP");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "SetDIBits selected LastError");

    if (old && mem) SelectObject(mem, old);

    BITMAPINFO query;
    memset(&query, 0, sizeof(query));
    query.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    int q = bmp ? GetDIBits(mem, bmp, 0, 0, NULL, &query, DIB_RGB_COLORS) : 0;
    smoke_expect(&s, q == 1 && query.bmiHeader.biWidth == 3 && query.bmiHeader.biHeight == 3 &&
                 query.bmiHeader.biBitCount == 32 && query.bmiHeader.biCompression == BI_RGB,
                 "GetDIBits query", "lpvBits==NULL fills BITMAPINFOHEADER metadata");

    DWORD outTop[9];
    memset(outTop, 0, sizeof(outTop));
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 3;
    bmi.bmiHeader.biHeight = -3;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    int gotTop = bmp ? GetDIBits(mem, bmp, 0, 3, outTop, &bmi, DIB_RGB_COLORS) : 0;
    smoke_expect(&s, gotTop == 3 && outTop[0] == RGB(10,1,1) && outTop[8] == RGB(90,9,9),
                 "GetDIBits top-down", "negative biHeight writes top logical row first");

    DWORD outBottom[9];
    memset(outBottom, 0, sizeof(outBottom));
    bmi.bmiHeader.biHeight = 3;
    int gotBottom = bmp ? GetDIBits(mem, bmp, 0, 3, outBottom, &bmi, DIB_RGB_COLORS) : 0;
    smoke_expect(&s, gotBottom == 3 && outBottom[0] == RGB(70,7,7) && outBottom[8] == RGB(30,3,3),
                 "GetDIBits bottom-up", "positive biHeight writes bottom scanline first");

    void* dibBits = NULL;
    bmi.bmiHeader.biHeight = -3;
    HBITMAP dib = CreateDIBSection(0, &bmi, DIB_RGB_COLORS, &dibBits, 0, 0);
    smoke_expect(&s, dib && dibBits, "CreateDIBSection SetDIBits target", "DIBSection fixture allocated unselected");
    smoke_expect(&s, dib && SetDIBits(mem, dib, 0, 3, topDown, &bmi, DIB_RGB_COLORS) == 3,
                 "SetDIBits into DIBSection", "DIBSection storage is a valid target when unselected");
    smoke_expect(&s, dibBits && ((DWORD*)dibBits)[0] == RGB(10,1,1) && ((DWORD*)dibBits)[8] == RGB(90,9,9),
                 "SetDIBits DIBSection bits", "direct bits reflect SetDIBits writes");

    HDC dst = CreateCompatibleDC(0);
    HBITMAP dstBmp = dst ? CreateCompatibleBitmap(dst, 4, 4) : 0;
    HGDIOBJ dstOld = (dst && dstBmp) ? SelectObject(dst, (HGDIOBJ)dstBmp) : 0;
    smoke_expect(&s, dst && dstBmp && dstOld, "StretchDIBits target", "selected destination memory bitmap allocated");

    BITMAPINFO sbmi;
    memset(&sbmi, 0, sizeof(sbmi));
    sbmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    sbmi.bmiHeader.biWidth = 2;
    sbmi.bmiHeader.biHeight = -2;
    sbmi.bmiHeader.biPlanes = 1;
    sbmi.bmiHeader.biBitCount = 32;
    sbmi.bmiHeader.biCompression = BI_RGB;
    DWORD src2[4] = { RGB(1,10,10), RGB(2,20,20), RGB(3,30,30), RGB(4,40,40) };
    int stretch = dst ? StretchDIBits(dst, 0, 0, 4, 4, 0, 0, 2, 2, src2, &sbmi, DIB_RGB_COLORS, SRCCOPY) : (int)GDI_ERROR;
    smoke_expect(&s, stretch == 2, "StretchDIBits 2x scale", "returns source scanline count for SRCCOPY stretch");
    smoke_expect(&s, dst && GetPixel(dst, 0, 0) == RGB(1,10,10) && GetPixel(dst, 3, 0) == RGB(2,20,20) &&
                 GetPixel(dst, 0, 3) == RGB(3,30,30) && GetPixel(dst, 3, 3) == RGB(4,40,40),
                 "StretchDIBits nearest", "nearest-neighbor scale preserves source corners");

    HBITMAP dstBmp2 = CreateCompatibleBitmap(dst, 2, 2);
    HGDIOBJ oldDst2 = dstBmp2 ? SelectObject(dst, (HGDIOBJ)dstBmp2) : 0;
    sbmi.bmiHeader.biHeight = 2;
    DWORD bottomSrc[4] = { RGB(33,3,3), RGB(44,4,4), RGB(11,1,1), RGB(22,2,2) };
    int stretchBottom = dst ? StretchDIBits(dst, 0, 0, 2, 2, 0, 0, 2, 2, bottomSrc, &sbmi, DIB_RGB_COLORS, SRCCOPY) : (int)GDI_ERROR;
    smoke_expect(&s, stretchBottom == 2 && GetPixel(dst, 0, 0) == RGB(11,1,1) && GetPixel(dst, 0, 1) == RGB(33,3,3),
                 "StretchDIBits bottom-up", "positive biHeight physical bottom row is displayed at bottom");

    SetLastError(0x12345678u);
    smoke_expect(&s, StretchDIBits(dst, 0, 0, 2, 2, 0, 0, 2, 2, src2, &sbmi, DIB_RGB_COLORS, 0xDEADBEEFu) == (int)GDI_ERROR,
                 "StretchDIBits unsupported ROP", "unsupported raster op fails instead of fake rendering");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "StretchDIBits unsupported ROP LastError");

    if (oldDst2 && dst) SelectObject(dst, oldDst2);
    if (dstBmp2) DeleteObject((HGDIOBJ)dstBmp2);
    if (dstOld && dst) SelectObject(dst, dstOld);
    if (dstBmp) DeleteObject((HGDIOBJ)dstBmp);
    if (dst) DeleteDC(dst);
    if (dib) DeleteObject((HGDIOBJ)dib);
    if (bmp) DeleteObject((HGDIOBJ)bmp);
    if (mem) DeleteDC(mem);

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 2;
    bmi.bmiHeader.biHeight = -2;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC inv = CreateCompatibleDC(0);
    HBITMAP invBmp = inv ? CreateCompatibleBitmap(inv, 2, 2) : 0;
    SetLastError(0x12345678u);
    smoke_expect(&s, invBmp && SetDIBits(inv, invBmp, 0, 2, topDown, &bmi, DIB_RGB_COLORS) == 0,
                 "SetDIBits unsupported bpp", "v161 rejects non-32bpp instead of guessing format conversion");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "SetDIBits unsupported bpp LastError");
    if (invBmp) DeleteObject((HGDIOBJ)invBmp);
    if (inv) DeleteDC(inv);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_gdi_stretchblt(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"gdi_stretchblt"};
    smoke_runtime_init(rt);

    HDC src = CreateCompatibleDC(0);
    HBITMAP srcBmp = src ? CreateCompatibleBitmap(src, 2, 2) : 0;
    HGDIOBJ srcOld = (src && srcBmp) ? SelectObject(src, (HGDIOBJ)srcBmp) : 0;
    smoke_expect(&s, src && srcBmp && srcOld, "StretchBlt source fixture", "2x2 source memory DC allocated");
    if (src) {
        SetPixel(src, 0, 0, RGB(10,1,1));
        SetPixel(src, 1, 0, RGB(20,2,2));
        SetPixel(src, 0, 1, RGB(30,3,3));
        SetPixel(src, 1, 1, RGB(40,4,4));
    }

    HDC dst = CreateCompatibleDC(0);
    HBITMAP dstBmp = dst ? CreateCompatibleBitmap(dst, 4, 4) : 0;
    HGDIOBJ dstOld = (dst && dstBmp) ? SelectObject(dst, (HGDIOBJ)dstBmp) : 0;
    smoke_expect(&s, dst && dstBmp && dstOld, "StretchBlt destination fixture", "4x4 destination memory DC allocated");

    int defaultMode = dst ? GetStretchBltMode(dst) : 0;
    smoke_expect(&s, defaultMode == BLACKONWHITE, "GetStretchBltMode default", "new DC starts with BLACKONWHITE stretch mode");
    int oldMode = dst ? SetStretchBltMode(dst, COLORONCOLOR) : 0;
    smoke_expect(&s, oldMode == BLACKONWHITE && GetStretchBltMode(dst) == COLORONCOLOR,
                 "SetStretchBltMode COLORONCOLOR", "setter returns previous mode and stores new mode");
    smoke_expect(&s, dst && SetStretchBltMode(dst, HALFTONE) == COLORONCOLOR && GetStretchBltMode(dst) == HALFTONE,
                 "SetStretchBltMode HALFTONE state", "HALFTONE is accepted as DC state even before high-quality filtering");
    SetLastError(0x12345678u);
    smoke_expect(&s, dst && SetStretchBltMode(dst, 9999) == 0,
                 "SetStretchBltMode invalid", "invalid stretch mode fails");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "SetStretchBltMode invalid LastError");
    if (dst) SetStretchBltMode(dst, COLORONCOLOR);

    BOOL ok = (dst && src) ? StretchBlt(dst, 0, 0, 4, 4, src, 0, 0, 2, 2, SRCCOPY) : FALSE;
    smoke_expect(&s, ok, "StretchBlt 2x scale", "SRCCOPY DC-to-DC stretch succeeds");
    smoke_expect(&s, dst && GetPixel(dst, 0, 0) == RGB(10,1,1) && GetPixel(dst, 3, 0) == RGB(20,2,2) &&
                 GetPixel(dst, 0, 3) == RGB(30,3,3) && GetPixel(dst, 3, 3) == RGB(40,4,4),
                 "StretchBlt nearest 2x", "COLORONCOLOR-style nearest sampling preserves source corners");

    HBITMAP dst2 = dst ? CreateCompatibleBitmap(dst, 2, 2) : 0;
    HGDIOBJ old2 = (dst && dst2) ? SelectObject(dst, (HGDIOBJ)dst2) : 0;
    ok = (dst && src) ? StretchBlt(dst, 0, 0, 2, 2, src, 0, 0, 2, 2, SRCCOPY) : FALSE;
    smoke_expect(&s, ok && GetPixel(dst, 0, 0) == RGB(10,1,1) && GetPixel(dst, 1, 1) == RGB(40,4,4),
                 "StretchBlt 1:1", "1:1 stretch matches source pixels");

    ok = (dst && src) ? StretchBlt(dst, 0, 0, 2, 2, src, 2, 0, -2, 2, SRCCOPY) : FALSE;
    smoke_expect(&s, ok && GetPixel(dst, 0, 0) == RGB(20,2,2) && GetPixel(dst, 1, 0) == RGB(10,1,1),
                 "StretchBlt mirror X", "opposite source/destination width signs mirror horizontally");
    ok = (dst && src) ? StretchBlt(dst, 0, 0, 2, 2, src, 0, 2, 2, -2, SRCCOPY) : FALSE;
    smoke_expect(&s, ok && GetPixel(dst, 0, 0) == RGB(30,3,3) && GetPixel(dst, 0, 1) == RGB(10,1,1),
                 "StretchBlt mirror Y", "opposite source/destination height signs mirror vertically");

    HDC src4 = CreateCompatibleDC(0);
    HBITMAP src4Bmp = src4 ? CreateCompatibleBitmap(src4, 4, 4) : 0;
    HGDIOBJ src4Old = (src4 && src4Bmp) ? SelectObject(src4, (HGDIOBJ)src4Bmp) : 0;
    if (src4) {
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                SetPixel(src4, x, y, RGB((BYTE)(x + y * 10), (BYTE)x, (BYTE)y));
    }
    ok = (dst && src4) ? StretchBlt(dst, 0, 0, 2, 2, src4, 0, 0, 4, 4, SRCCOPY) : FALSE;
    smoke_expect(&s, ok && GetPixel(dst, 0, 0) == RGB(0,0,0) && GetPixel(dst, 1, 0) == RGB(2,2,0) &&
                 GetPixel(dst, 0, 1) == RGB(20,0,2) && GetPixel(dst, 1, 1) == RGB(22,2,2),
                 "StretchBlt shrink 4->2", "COLORONCOLOR shrink deletes rows/columns with nearest sampling");

    if (old2 && dst) SelectObject(dst, old2);
    if (dst2) DeleteObject((HGDIOBJ)dst2);

    for (int y = 0; dst && y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            SetPixel(dst, x, y, RGB(0,0,0));
    HRGN clip = CreateRectRgn(1, 1, 3, 3);
    int clipKind = (dst && clip) ? SelectClipRgn(dst, clip) : ERROR;
    ok = (dst && src) ? StretchBlt(dst, 0, 0, 4, 4, src, 0, 0, 2, 2, SRCCOPY) : FALSE;
    smoke_expect(&s, clipKind == SIMPLEREGION && ok && GetPixel(dst, 0, 0) == RGB(0,0,0) &&
                 GetPixel(dst, 1, 1) == RGB(10,1,1) && GetPixel(dst, 2, 2) == RGB(40,4,4) &&
                 GetPixel(dst, 3, 3) == RGB(0,0,0),
                 "StretchBlt destination clip", "target clip region limits memory-DC stretch writes");
    if (dst) SelectClipRgn(dst, 0);
    if (clip) DeleteObject((HGDIOBJ)clip);

    HWND win = CreateWindowExA(0, "STATIC", "stretch", WS_OVERLAPPEDWINDOW|WS_VISIBLE, 10, 10, 80, 60, 0, 0, 0, NULL);
    HDC winDc = win ? GetDC(win) : 0;
    smoke_expect(&s, winDc && src && StretchBlt(winDc, 1, 1, 4, 4, src, 0, 0, 2, 2, SRCCOPY),
                 "StretchBlt window snapshot", "memory-source StretchBlt can target window/paint command buffer");
    if (winDc) ReleaseDC(win, winDc);
    if (win) DestroyWindow(win);

    SetLastError(0x12345678u);
    smoke_expect(&s, dst && src && !StretchBlt(dst, 0, 0, 2, 2, src, 0, 0, 2, 2, 0xDEADBEEFu),
                 "StretchBlt unsupported ROP", "unsupported raster op fails instead of fake rendering");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "StretchBlt unsupported ROP LastError");

    if (src4Old && src4) SelectObject(src4, src4Old);
    if (src4Bmp) DeleteObject((HGDIOBJ)src4Bmp);
    if (src4) DeleteDC(src4);
    if (dstOld && dst) SelectObject(dst, dstOld);
    if (dstBmp) DeleteObject((HGDIOBJ)dstBmp);
    if (dst) DeleteDC(dst);
    if (srcOld && src) SelectObject(src, srcOld);
    if (srcBmp) DeleteObject((HGDIOBJ)srcBmp);
    if (src) DeleteDC(src);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static int smoke_gdi_patblt(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"gdi_patblt"};
    smoke_runtime_init(rt);

    HDC mem = CreateCompatibleDC(0);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(mem, 4, 4) : 0;
    HGDIOBJ oldBmp = (mem && bmp) ? SelectObject(mem, (HGDIOBJ)bmp) : 0;
    smoke_expect(&s, mem && bmp && oldBmp, "PatBlt memory fixture", "4x4 destination memory DC allocated");

    HBRUSH red = CreateSolidBrush(RGB(0x11,0x22,0x33));
    HBRUSH mask = CreateSolidBrush(RGB(0x01,0x02,0x03));
    smoke_expect(&s, red && mask, "CreateSolidBrush fixtures", "solid pattern brushes allocated");

    if (mem && red) SelectObject(mem, (HGDIOBJ)red);
    smoke_expect(&s, mem && PatBlt(mem, 0, 0, 2, 2, PATCOPY),
                 "PatBlt PATCOPY", "selected brush pattern copied into destination rectangle");
    smoke_expect(&s, GetPixel(mem, 0, 0) == RGB(0x11,0x22,0x33) && GetPixel(mem, 1, 1) == RGB(0x11,0x22,0x33) && GetPixel(mem, 2, 2) == RGB(0,0,0),
                 "PATCOPY pixels", "only requested rectangle receives brush color");

    if (mem && mask) SelectObject(mem, (HGDIOBJ)mask);
    COLORREF before = RGB(0x11,0x22,0x33);
    COLORREF pat = RGB(0x01,0x02,0x03);
    COLORREF expectedXor = (before ^ pat) & 0x00FFFFFFu;
    smoke_expect(&s, mem && PatBlt(mem, 0, 0, 1, 1, PATINVERT),
                 "PatBlt PATINVERT", "destination XOR selected pattern");
    smoke_expect(&s, GetPixel(mem, 0, 0) == expectedXor,
                 "PATINVERT pixel", "PATINVERT applies bitwise XOR over COLORREF bits");

    SetPixel(mem, 3, 0, RGB(0x10,0x20,0x30));
    smoke_expect(&s, mem && PatBlt(mem, 3, 0, 1, 1, DSTINVERT),
                 "PatBlt DSTINVERT", "destination inverted without requiring source DC");
    smoke_expect(&s, GetPixel(mem, 3, 0) == ((~RGB(0x10,0x20,0x30)) & 0x00FFFFFFu),
                 "DSTINVERT pixel", "destination COLORREF bits inverted");

    smoke_expect(&s, mem && PatBlt(mem, 0, 3, 1, 1, BLACKNESS),
                 "PatBlt BLACKNESS", "fills destination with palette index 0 black equivalent");
    smoke_expect(&s, GetPixel(mem, 0, 3) == RGB(0,0,0), "BLACKNESS pixel", "blackness writes black");
    smoke_expect(&s, mem && PatBlt(mem, 1, 3, 1, 1, WHITENESS),
                 "PatBlt WHITENESS", "fills destination with palette index 1 white equivalent");
    smoke_expect(&s, GetPixel(mem, 1, 3) == RGB(255,255,255), "WHITENESS pixel", "whiteness writes white");

    for (int yy = 0; yy < 4; ++yy)
        for (int xx = 0; xx < 4; ++xx)
            SetPixel(mem, xx, yy, RGB(0,0,0));
    HRGN clip = CreateRectRgn(1, 1, 3, 3);
    int clipKind = (mem && clip) ? SelectClipRgn(mem, clip) : ERROR;
    if (mem && red) SelectObject(mem, (HGDIOBJ)red);
    smoke_expect(&s, clipKind == SIMPLEREGION && PatBlt(mem, 0, 0, 4, 4, PATCOPY),
                 "PatBlt clip", "target clip region constrains memory PatBlt");
    smoke_expect(&s, GetPixel(mem, 0, 0) == RGB(0,0,0) && GetPixel(mem, 1, 1) == RGB(0x11,0x22,0x33) &&
                 GetPixel(mem, 2, 2) == RGB(0x11,0x22,0x33) && GetPixel(mem, 3, 3) == RGB(0,0,0),
                 "PatBlt clipped pixels", "outside clip remains untouched");
    if (mem) SelectClipRgn(mem, 0);
    if (clip) DeleteObject((HGDIOBJ)clip);

    HDC noBrush = CreateCompatibleDC(0);
    HBITMAP nbBmp = noBrush ? CreateCompatibleBitmap(noBrush, 1, 1) : 0;
    HGDIOBJ nbOld = (noBrush && nbBmp) ? SelectObject(noBrush, (HGDIOBJ)nbBmp) : 0;
    SetLastError(0x12345678u);
    smoke_expect(&s, noBrush && !PatBlt(noBrush, 0, 0, 1, 1, PATCOPY),
                 "PatBlt PATCOPY without brush", "pattern-dependent ROP fails without selected brush in current stock-object-lite model");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "PatBlt missing brush LastError");
    smoke_expect(&s, noBrush && PatBlt(noBrush, 0, 0, 1, 1, DSTINVERT),
                 "PatBlt DSTINVERT without brush", "destination-only ROP does not require selected brush");
    if (nbOld && noBrush) SelectObject(noBrush, nbOld);
    if (nbBmp) DeleteObject((HGDIOBJ)nbBmp);
    if (noBrush) DeleteDC(noBrush);

    HWND win = CreateWindowExA(0, "STATIC", "patblt", WS_OVERLAPPEDWINDOW|WS_VISIBLE, 10, 10, 80, 60, 0, 0, 0, NULL);
    HDC winDc = win ? GetDC(win) : 0;
    if (winDc && red) SelectObject(winDc, (HGDIOBJ)red);
    smoke_expect(&s, winDc && PatBlt(winDc, 1, 1, 5, 4, PATCOPY),
                 "PatBlt window command", "window/paint DC records pattern ROP command");
    if (winDc) ReleaseDC(win, winDc);
    if (win) DestroyWindow(win);

    SetLastError(0x12345678u);
    smoke_expect(&s, mem && !PatBlt(mem, 0, 0, 1, 1, 0xDEADBEEFu),
                 "PatBlt unsupported ROP", "unsupported PatBlt ROP fails instead of fake rendering");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "PatBlt unsupported ROP LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, !PatBlt((HDC)0x777777u, 0, 0, 1, 1, DSTINVERT),
                 "PatBlt invalid HDC", "invalid destination DC fails");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "PatBlt invalid HDC LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, mem && !PatBlt(mem, 0, 0, 0, 1, DSTINVERT),
                 "PatBlt zero width", "empty rectangle is rejected in the narrow PatBlt path");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "PatBlt zero width LastError");

    if (oldBmp && mem) SelectObject(mem, oldBmp);
    if (bmp) DeleteObject((HGDIOBJ)bmp);
    if (mem) DeleteDC(mem);
    if (red) DeleteObject((HGDIOBJ)red);
    if (mask) DeleteObject((HGDIOBJ)mask);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_user32_redraw(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"user32_redraw"};
    smoke_runtime_init(rt);
    g_user32_paint = 0;

    HWND hwnd = smoke_create_basic_window(rt, "myOS.v155.RedrawSmoke", "Smoke Redraw", &s);
    RECT out = {0,0,0,0};
    smoke_expect(&s, hwnd && !GetUpdateRect(hwnd, &out, FALSE), "GetUpdateRect clean", "fresh window has no update rect");

    RECT r1 = {4, 5, 40, 30};
    smoke_expect(&s, hwnd && InvalidateRect(hwnd, &r1, TRUE), "InvalidateRect", "sets dirty update rect");
    memset(&out, 0, sizeof(out));
    smoke_expect(&s, GetUpdateRect(hwnd, &out, FALSE) && out.left == 4 && out.top == 5 && out.right == 40 && out.bottom == 30,
                 "GetUpdateRect dirty", "returns invalidated rect without consuming it");

    MYGDI_WINDOW_SNAPSHOT snap;
    memset(&snap, 0, sizeof(snap));
    DWORD postedBefore = 0;
    smoke_expect(&s, MyGdiGetWindowState(hwnd, &snap) && snap.paintPending, "paint pending", "InvalidateRect posts one WM_PAINT intent");
    postedBefore = snap.postedPaints;

    RECT r2 = {20, 1, 70, 44};
    smoke_expect(&s, InvalidateRect(hwnd, &r2, FALSE), "InvalidateRect coalesce", "second invalidate merges into same pending paint");
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, MyGdiGetWindowState(hwnd, &snap) && snap.postedPaints == postedBefore && snap.coalescedInvalidates >= 1,
                 "WM_PAINT coalescing", "second invalidate does not post a second paint");
    smoke_expect(&s, snap.dirtyRect.left == 4 && snap.dirtyRect.top == 1 && snap.dirtyRect.right == 70 && snap.dirtyRect.bottom == 44,
                 "dirty rect union", "multiple invalidates union into one update rect");

    smoke_expect(&s, ValidateRect(hwnd, NULL), "ValidateRect", "clears dirty region and removes queued WM_PAINT");
    smoke_expect(&s, !GetUpdateRect(hwnd, NULL, FALSE), "GetUpdateRect after ValidateRect", "validation consumes update region");
    MSG msg;
    memset(&msg, 0, sizeof(msg));
    smoke_expect(&s, !PeekMessageA(&msg, hwnd, WM_PAINT, WM_PAINT, PM_REMOVE), "ValidateRect removes pending WM_PAINT", "no stale paint message remains");

    smoke_expect(&s, InvalidateRect(hwnd, &r1, TRUE), "Invalidate before BeginPaint", "marks dirty for paint DC");
    PAINTSTRUCT ps;
    memset(&ps, 0, sizeof(ps));
    HDC paintDc = BeginPaint(hwnd, &ps);
    smoke_expect(&s, paintDc != 0 && ps.rcPaint.left == 4 && ps.rcPaint.top == 5 && ps.rcPaint.right == 40 && ps.rcPaint.bottom == 30,
                 "BeginPaint validates", "BeginPaint returns rcPaint and clears dirty state");
    if (paintDc) EndPaint(hwnd, &ps);
    smoke_expect(&s, !GetUpdateRect(hwnd, NULL, FALSE), "GetUpdateRect after BeginPaint", "BeginPaint validates update region");

    g_user32_paint = 0;
    smoke_expect(&s, InvalidateRect(hwnd, &r1, TRUE), "Invalidate before UpdateWindow", "queues async paint intent");
    smoke_expect(&s, UpdateWindow(hwnd), "UpdateWindow", "sends WM_PAINT synchronously when dirty");
    smoke_expect(&s, g_user32_paint == 1 && !GetUpdateRect(hwnd, NULL, FALSE), "UpdateWindow sync paint", "WndProc painted once and update region is clean");
    memset(&msg, 0, sizeof(msg));
    smoke_expect(&s, !PeekMessageA(&msg, hwnd, WM_PAINT, WM_PAINT, PM_REMOVE), "UpdateWindow consumes queued paint", "synchronous paint removes stale async intent");

    smoke_expect(&s, RedrawWindow(hwnd, &r2, 0, RDW_INVALIDATE), "RedrawWindow RDW_INVALIDATE", "sets update region");
    smoke_expect(&s, GetUpdateRect(hwnd, &out, FALSE) && out.left == 20 && out.top == 1 && out.right == 70 && out.bottom == 44,
                 "RDW_INVALIDATE rect", "RedrawWindow stores requested dirty rect");
    smoke_expect(&s, RedrawWindow(hwnd, &r2, 0, RDW_VALIDATE), "RedrawWindow RDW_VALIDATE", "validates requested dirty rect");
    smoke_expect(&s, !GetUpdateRect(hwnd, NULL, FALSE), "RDW_VALIDATE clean", "dirty state cleared");

    g_user32_paint = 0;
    smoke_expect(&s, RedrawWindow(hwnd, &r1, 0, RDW_INVALIDATE|RDW_UPDATENOW), "RedrawWindow RDW_UPDATENOW", "invalidates and paints synchronously");
    smoke_expect(&s, g_user32_paint == 1 && !GetUpdateRect(hwnd, NULL, FALSE), "RDW_UPDATENOW sync", "synchronous redraw consumes update region");

    HWND child = CreateWindowExA(0, "myOS.v155.RedrawSmoke", "child", WS_CHILD|WS_VISIBLE,
                                5, 5, 40, 30, hwnd, 0, 0, NULL);
    smoke_expect(&s, child != 0, "CreateWindowExA child", "child visibility/dirty propagation fixture");
    if (child) ValidateRect(child, NULL);
    ValidateRect(hwnd, NULL);
    smoke_expect(&s, child && RedrawWindow(hwnd, &r1, 0, RDW_INVALIDATE|RDW_ALLCHILDREN),
                 "RedrawWindow RDW_ALLCHILDREN", "invalidates parent and child");
    smoke_expect(&s, child && GetUpdateRect(child, &out, FALSE), "RDW_ALLCHILDREN child dirty", "child gets update rect");
    if (child) ValidateRect(child, NULL);
    ValidateRect(hwnd, NULL);
    smoke_expect(&s, child && RedrawWindow(hwnd, &r1, 0, RDW_INVALIDATE|RDW_NOCHILDREN),
                 "RedrawWindow RDW_NOCHILDREN", "invalidates parent only");
    smoke_expect(&s, child && !GetUpdateRect(child, NULL, FALSE), "RDW_NOCHILDREN child clean", "child is not invalidated");

    SetLastError(0x12345678u);
    smoke_expect(&s, !GetUpdateRect((HWND)0, NULL, FALSE), "GetUpdateRect invalid HWND", "rejects null HWND");
    smoke_expect_last_error(&s, ERROR_INVALID_WINDOW_HANDLE, "GetUpdateRect invalid HWND LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, !RedrawWindow((HWND)0, NULL, 0, RDW_INVALIDATE), "RedrawWindow invalid HWND", "rejects null HWND");
    smoke_expect_last_error(&s, ERROR_INVALID_WINDOW_HANDLE, "RedrawWindow invalid HWND LastError");

    if (child) DestroyWindow(child);
    if (hwnd) DestroyWindow(hwnd);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static HWND smoke_create_erase_window(SmokeRuntime* rt, const char* clsName, WNDPROC proc, HBRUSH hbr, SmokeContext* s)
{
    smoke_runtime_init(rt);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.lpszClassName = clsName;
    wc.hbrBackground = hbr;
    ATOM atom = RegisterClassExA(&wc);
    smoke_expect(s, atom != 0, "RegisterClassExA erase", clsName);
    HWND hwnd = atom ? CreateWindowExA(0, clsName, "Smoke Erase", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                      40, 40, 160, 90, 0, 0, 0, NULL) : 0;
    smoke_expect(s, hwnd != 0, "CreateWindowExA erase", clsName);
    return hwnd;
}

static int smoke_user32_erase(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"user32_erase"};
    smoke_runtime_init(rt);
    g_user32_paint = 0;
    g_user32_erase_true = g_user32_erase_false = 0;
    g_user32_last_erase_hdc = 0;

    HBRUSH red = CreateSolidBrush(RGB(240, 10, 20));
    HBRUSH green = CreateSolidBrush(RGB(10, 220, 40));
    HBRUSH blue = CreateSolidBrush(RGB(20, 40, 240));
    smoke_expect(&s, red && green && blue, "CreateSolidBrush fixtures", "class background brushes");

    HWND hwnd = smoke_create_erase_window(rt, "myOS.v157.EraseFalse", smoke_erase_false_wndproc, red, &s);
    RECT rc = {1, 2, 30, 22};
    MYGDI_WINDOW_SNAPSHOT snap;
    memset(&snap, 0, sizeof(snap));

    smoke_expect(&s, hwnd && InvalidateRect(hwnd, &rc, FALSE), "InvalidateRect(FALSE)", "dirty without erase pending");
    smoke_expect(&s, MyGdiGetWindowState(hwnd, &snap) && snap.dirty && !snap.erasePending,
                 "erase pending false", "bErase=FALSE leaves PAINTSTRUCT.fErase false");
    PAINTSTRUCT ps;
    memset(&ps, 0, sizeof(ps));
    HDC hdc = BeginPaint(hwnd, &ps);
    smoke_expect(&s, hdc && !ps.fErase, "BeginPaint no erase", "fErase FALSE for non-erasing invalidate");
    if (hdc) EndPaint(hwnd, &ps);
    smoke_expect(&s, g_user32_erase_false == 0, "WM_ERASEBKGND not sent", "no erase message when erasePending is clear");

    smoke_expect(&s, InvalidateRect(hwnd, &rc, TRUE), "InvalidateRect(TRUE)", "dirty with erase pending");
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, MyGdiGetWindowState(hwnd, &snap) && snap.erasePending,
                 "erase pending true", "bErase=TRUE marks erase pending");
    memset(&ps, 0, sizeof(ps));
    hdc = BeginPaint(hwnd, &ps);
    smoke_expect(&s, hdc && ps.fErase, "BeginPaint fErase", "erasePending surfaces in PAINTSTRUCT.fErase");
    smoke_expect(&s, g_user32_erase_false == 1 && g_user32_last_erase_hdc == hdc,
                 "WM_ERASEBKGND sent", "BeginPaint sends erase with paint DC");
    if (hdc) EndPaint(hwnd, &ps);
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, MyGdiGetWindowState(hwnd, &snap) && !snap.dirty && !snap.erasePending,
                 "BeginPaint clears erase", "dirty and erase pending are consumed");

    smoke_expect(&s, InvalidateRect(hwnd, &rc, FALSE), "Invalidate before GetUpdateRect(TRUE)", "dirty fixture");
    RECT out = {0,0,0,0};
    smoke_expect(&s, GetUpdateRect(hwnd, &out, TRUE), "GetUpdateRect(TRUE)", "marks erase pending while reporting rect");
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, MyGdiGetWindowState(hwnd, &snap) && snap.erasePending,
                 "GetUpdateRect marks erase", "bErase TRUE converts dirty into erasePending");
    ValidateRect(hwnd, NULL);
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, MyGdiGetWindowState(hwnd, &snap) && !snap.erasePending,
                 "ValidateRect clears erase", "validation clears erase state too");

    smoke_expect(&s, RedrawWindow(hwnd, &rc, 0, RDW_INVALIDATE|RDW_ERASE),
                 "RedrawWindow RDW_ERASE", "sets erase pending");
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, MyGdiGetWindowState(hwnd, &snap) && snap.erasePending,
                 "RDW_ERASE pending", "RDW_ERASE marks erase pending");
    ValidateRect(hwnd, NULL);
    smoke_expect(&s, RedrawWindow(hwnd, &rc, 0, RDW_INVALIDATE|RDW_ERASE|RDW_NOERASE),
                 "RedrawWindow RDW_NOERASE", "invalidates but suppresses erase");
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, MyGdiGetWindowState(hwnd, &snap) && snap.dirty && !snap.erasePending,
                 "RDW_NOERASE suppresses", "dirty remains but erasePending is clear");

    g_user32_paint = 0;
    g_user32_erase_false = 0;
    smoke_expect(&s, RedrawWindow(hwnd, &rc, 0, RDW_ERASE|RDW_ERASENOW),
                 "RDW_ERASENOW existing dirty", "marks pending erase and updates now");
    smoke_expect(&s, g_user32_paint == 1 && g_user32_erase_false == 1,
                 "RDW_ERASENOW sync erase", "synchronous paint includes WM_ERASEBKGND");
    ValidateRect(hwnd, NULL);

    HDC mem = CreateCompatibleDC(0);
    HBITMAP bmp = CreateCompatibleBitmap(mem, 12, 12);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT fill = {0,0,12,12};
    FillRect(mem, &fill, blue);
    smoke_expect(&s, DefWindowProcA(hwnd, WM_ERASEBKGND, (WPARAM)mem, 0) == TRUE,
                 "DefWindowProcA WM_ERASEBKGND", "uses class hbrBackground");
    smoke_expect(&s, GetPixel(mem, 1, 1) == (RGB(240,10,20) & 0x00ffffffu),
                 "class background brush", "default erase fills with registered class brush");
    smoke_expect(&s, SetClassLongPtrA(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)green) == (LONG_PTR)red,
                 "SetClassLongPtr(GCLP_HBRBACKGROUND)", "changes app class erase brush");
    FillRect(mem, &fill, blue);
    smoke_expect(&s, DefWindowProcA(hwnd, WM_ERASEBKGND, (WPARAM)mem, 0) == TRUE &&
                    GetPixel(mem, 1, 1) == (RGB(10,220,40) & 0x00ffffffu),
                 "changed background brush", "DefWindowProc uses updated class brush");

    HWND trueHwnd = smoke_create_erase_window(rt, "myOS.v157.EraseTrue", smoke_erase_true_wndproc, green, &s);
    g_user32_erase_true = 0;
    smoke_expect(&s, trueHwnd && InvalidateRect(trueHwnd, &rc, TRUE),
                 "Invalidate handled TRUE", "fixture for handled erase");
    memset(&ps, 0, sizeof(ps));
    HDC truePaint = BeginPaint(trueHwnd, &ps);
    smoke_expect(&s, truePaint && ps.fErase && g_user32_erase_true == 1,
                 "WM_ERASEBKGND handled TRUE", "custom erase handler receives paint DC and suppresses default path");
    if (truePaint) EndPaint(trueHwnd, &ps);

    HWND child = hwnd ? CreateWindowExA(0, "myOS.v157.EraseFalse", "child", WS_CHILD|WS_VISIBLE,
                                       3, 3, 30, 20, hwnd, 0, 0, NULL) : 0;
    smoke_expect(&s, child != 0, "Create child erase fixture", "child erase state independent from parent");
    if (child) ValidateRect(child, NULL);
    ValidateRect(hwnd, NULL);
    smoke_expect(&s, child && InvalidateRect(child, &rc, TRUE),
                 "child InvalidateRect(TRUE)", "child erase pending only");
    MYGDI_WINDOW_SNAPSHOT parentSnap, childSnap;
    memset(&parentSnap, 0, sizeof(parentSnap));
    memset(&childSnap, 0, sizeof(childSnap));
    smoke_expect(&s, child && MyGdiGetWindowState(child, &childSnap) && childSnap.erasePending &&
                    MyGdiGetWindowState(hwnd, &parentSnap) && !parentSnap.erasePending,
                 "child erase isolated", "child erase does not mark parent erase pending");

    if (old) SelectObject(mem, old);
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    if (child) DestroyWindow(child);
    if (trueHwnd) DestroyWindow(trueHwnd);
    if (hwnd) DestroyWindow(hwnd);
    if (red) DeleteObject(red);
    if (green) DeleteObject(green);
    if (blue) DeleteObject(blue);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static int smoke_gdi_region(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"gdi_region"};
    HWND hwnd = smoke_create_basic_window(rt, "myOS.v159.GdiRegionSmoke", "Smoke GDI Region", &s);

    RECT box;
    HRGN empty = CreateRectRgn(0, 0, 0, 10);
    smoke_expect(&s, empty != 0, "CreateRectRgn empty", "empty region still returns an HRGN");
    memset(&box, 0x7f, sizeof(box));
    smoke_expect(&s, empty && GetRgnBox(empty, &box) == NULLREGION,
                 "GetRgnBox empty", "empty HRGN reports NULLREGION");
    smoke_expect(&s, box.left == 0 && box.top == 0 && box.right == 0 && box.bottom == 0,
                 "empty box", "NULLREGION returns an empty bounding box");

    HRGN r1 = CreateRectRgn(10, 10, 30, 30);
    smoke_expect(&s, r1 != 0, "CreateRectRgn simple", "simple rectangular HRGN allocated");
    memset(&box, 0, sizeof(box));
    smoke_expect(&s, r1 && GetRgnBox(r1, &box) == SIMPLEREGION,
                 "GetRgnBox simple", "simple region reports SIMPLEREGION");
    smoke_expect(&s, box.left == 10 && box.top == 10 && box.right == 30 && box.bottom == 30,
                 "simple bounds", "GetRgnBox returns exact bounds");
    smoke_expect(&s, r1 && PtInRegion(r1, 10, 10) && PtInRegion(r1, 29, 29) && !PtInRegion(r1, 30, 30),
                 "PtInRegion half-open", "right/bottom edge is outside the rectangular region");

    smoke_expect(&s, r1 && OffsetRgn(r1, 5, -3) == SIMPLEREGION,
                 "OffsetRgn", "offset keeps simple region classification");
    memset(&box, 0, sizeof(box));
    GetRgnBox(r1, &box);
    smoke_expect(&s, box.left == 15 && box.top == 7 && box.right == 35 && box.bottom == 27,
                 "OffsetRgn bounds", "region bounds move by x/y delta");

    smoke_expect(&s, r1 && SetRectRgn(r1, 0, 0, 20, 20),
                 "SetRectRgn", "existing HRGN receives a new rectangle");
    HRGN r2 = CreateRectRgn(10, 10, 30, 30);
    HRGN dst = CreateRectRgn(0, 0, 0, 0);
    smoke_expect(&s, r2 && dst, "region fixtures", "second source and destination HRGN allocated");

    int kind = (dst && r1 && r2) ? CombineRgn(dst, r1, r2, RGN_AND) : ERROR;
    smoke_expect(&s, kind == SIMPLEREGION, "CombineRgn RGN_AND", "overlap of two rectangles is simple");
    memset(&box, 0, sizeof(box));
    GetRgnBox(dst, &box);
    smoke_expect(&s, box.left == 10 && box.top == 10 && box.right == 20 && box.bottom == 20,
                 "RGN_AND bounds", "intersection bounds are exact");

    kind = (dst && r1 && r2) ? CombineRgn(dst, r1, r2, RGN_OR) : ERROR;
    smoke_expect(&s, kind == COMPLEXREGION, "CombineRgn RGN_OR", "overlapping offset rectangles form a non-rectangular union");
    memset(&box, 0, sizeof(box));
    GetRgnBox(dst, &box);
    smoke_expect(&s, box.left == 0 && box.top == 0 && box.right == 30 && box.bottom == 30,
                 "RGN_OR bounds", "complex union returns tight bounding box");

    HRGN copy = CreateRectRgn(0,0,0,0);
    kind = copy && dst ? CombineRgn(copy, dst, 0, RGN_COPY) : ERROR;
    smoke_expect(&s, kind == COMPLEXREGION && EqualRgn(copy, dst),
                 "CombineRgn RGN_COPY / EqualRgn", "copy preserves complex region identity");

    RECT probe = { 25, 25, 40, 40 };
    smoke_expect(&s, dst && RectInRegion(dst, &probe),
                 "RectInRegion hit", "rect intersects complex region");
    RECT miss = { 31, 31, 40, 40 };
    smoke_expect(&s, dst && !RectInRegion(dst, &miss),
                 "RectInRegion miss", "disjoint rect is outside complex region");

    HRGN big = CreateRectRgn(0, 0, 50, 20);
    HRGN cut = CreateRectRgn(0, 0, 10, 20);
    smoke_expect(&s, hwnd && big && InvalidateRgn(hwnd, big, TRUE),
                 "InvalidateRgn", "region joins the HWND update region");
    MYGDI_WINDOW_SNAPSHOT snap;
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, hwnd && MyGdiGetWindowState(hwnd, &snap) && snap.dirty && snap.erasePending,
                 "InvalidateRgn dirty/erase", "bErase participates in WM_ERASEBKGND state");
    HRGN update = CreateRectRgn(0,0,0,0);
    kind = (hwnd && update) ? GetUpdateRgn(hwnd, update, FALSE) : ERROR;
    smoke_expect(&s, kind == SIMPLEREGION && EqualRgn(update, big),
                 "GetUpdateRgn", "copies HWND update region into caller HRGN");
    smoke_expect(&s, hwnd && cut && ValidateRgn(hwnd, cut),
                 "ValidateRgn partial", "removes validated region from update region");
    memset(&box, 0, sizeof(box));
    kind = (hwnd && update) ? GetUpdateRgn(hwnd, update, FALSE) : ERROR;
    GetRgnBox(update, &box);
    smoke_expect(&s, kind == SIMPLEREGION && box.left == 10 && box.top == 0 && box.right == 50 && box.bottom == 20,
                 "ValidateRgn remainder", "remaining update region is exact after subtraction");
    if (hwnd) ValidateRect(hwnd, NULL);

    HRGN scrollRgn = CreateRectRgn(0,0,0,0);
    RECT scroll = {0,0,100,50};
    RECT out = {0,0,0,0};
    kind = hwnd && scrollRgn ? ScrollWindowEx(hwnd, 10, 5, &scroll, NULL, scrollRgn, &out, 0) : ERROR;
    memset(&box, 0, sizeof(box));
    int scrollBoxKind = scrollRgn ? GetRgnBox(scrollRgn, &box) : ERROR;
    smoke_expect(&s, kind == COMPLEXREGION && scrollBoxKind == COMPLEXREGION,
                 "ScrollWindowEx hrgnUpdate", "diagonal scroll writes a complex exposed HRGN");
    smoke_expect(&s, box.left == 0 && box.top == 0 && box.right == 100 && box.bottom == 50,
                 "ScrollWindowEx hrgn bounds", "hrgnUpdate bounding box matches prcUpdate");

    HDC mem = CreateCompatibleDC(0);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(mem, 8, 6) : 0;
    HGDIOBJ oldBmp = (mem && bmp) ? SelectObject(mem, (HGDIOBJ)bmp) : 0;
    HBRUSH red = CreateSolidBrush(RGB(250, 1, 2));
    HRGN clip = CreateRectRgn(1, 1, 4, 4);
    smoke_expect(&s, mem && clip && SelectClipRgn(mem, clip) == SIMPLEREGION,
                 "SelectClipRgn", "memory DC accepts app clipping region");
    memset(&box, 0, sizeof(box));
    smoke_expect(&s, mem && GetClipBox(mem, &box) == SIMPLEREGION && box.left == 1 && box.top == 1 && box.right == 4 && box.bottom == 4,
                 "GetClipBox", "clip box reflects selected HRGN");
    RECT full = {0,0,8,6};
    smoke_expect(&s, mem && red && FillRect(mem, &full, red) != 0,
                 "FillRect clipped", "drawing honors selected clip region for bitmap DCs");
    smoke_expect(&s, mem && GetPixel(mem, 2, 2) == RGB(250,1,2) && GetPixel(mem, 0, 0) != RGB(250,1,2),
                 "clip pixels", "inside clip is painted and outside remains untouched");
    smoke_expect(&s, mem && ExcludeClipRect(mem, 2, 2, 3, 3) == COMPLEXREGION,
                 "ExcludeClipRect", "subtracting a hole from clip produces a complex region");
    smoke_expect(&s, mem && IntersectClipRect(mem, 1, 1, 2, 2) == SIMPLEREGION,
                 "IntersectClipRect", "intersecting clip reduces to one rectangle");
    smoke_expect(&s, mem && SelectClipRgn(mem, 0) != ERROR,
                 "SelectClipRgn(NULL)", "NULL clip clears the application clip region");

    if (oldBmp && mem) SelectObject(mem, oldBmp);
    if (mem) DeleteDC(mem);
    if (bmp) DeleteObject((HGDIOBJ)bmp);
    if (red) DeleteObject((HGDIOBJ)red);
    if (empty) DeleteObject((HGDIOBJ)empty);
    if (r1) DeleteObject((HGDIOBJ)r1);
    if (r2) DeleteObject((HGDIOBJ)r2);
    if (dst) DeleteObject((HGDIOBJ)dst);
    if (copy) DeleteObject((HGDIOBJ)copy);
    if (big) DeleteObject((HGDIOBJ)big);
    if (cut) DeleteObject((HGDIOBJ)cut);
    if (update) DeleteObject((HGDIOBJ)update);
    if (scrollRgn) DeleteObject((HGDIOBJ)scrollRgn);
    if (clip) DeleteObject((HGDIOBJ)clip);
    if (hwnd) DestroyWindow(hwnd);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_user32_scroll(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"user32_scroll"};
    smoke_runtime_init(rt);
    g_user32_paint = 0;
    g_user32_move = 0;

    HWND hwnd = smoke_create_basic_window(rt, "myOS.v159.ScrollSmoke", "Smoke ScrollWindowEx", &s);
    RECT scroll = {0, 0, 100, 50};
    RECT out = {0,0,0,0};
    MYGDI_WINDOW_SNAPSHOT snap;

    if (hwnd) ValidateRect(hwnd, NULL);
    int region = hwnd ? ScrollWindowEx(hwnd, 10, 0, &scroll, NULL, 0, &out, SW_INVALIDATE) : ERROR;
    smoke_expect(&s, region == SIMPLEREGION, "ScrollWindowEx dx return", "right scroll exposes one left strip");
    smoke_expect(&s, out.left == 0 && out.top == 0 && out.right == 10 && out.bottom == 50,
                 "ScrollWindowEx dx update rect", "uncovered left strip is reported in client coordinates");
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, hwnd && MyGdiGetWindowState(hwnd, &snap) && snap.dirty && !snap.erasePending &&
                    snap.dirtyRect.left == 0 && snap.dirtyRect.top == 0 && snap.dirtyRect.right == 10 && snap.dirtyRect.bottom == 50,
                 "SW_INVALIDATE dirty", "ScrollWindowEx invalidates uncovered strip without erase pending");
    if (hwnd) ValidateRect(hwnd, NULL);

    memset(&out, 0, sizeof(out));
    region = hwnd ? ScrollWindowEx(hwnd, 0, -5, &scroll, NULL, 0, &out, SW_ERASE) : ERROR;
    smoke_expect(&s, region == SIMPLEREGION, "ScrollWindowEx dy return", "up scroll exposes one bottom strip");
    smoke_expect(&s, out.left == 0 && out.top == 45 && out.right == 100 && out.bottom == 50,
                 "ScrollWindowEx dy update rect", "uncovered bottom strip is reported");
    memset(&snap, 0, sizeof(snap));
    smoke_expect(&s, hwnd && MyGdiGetWindowState(hwnd, &snap) && snap.dirty && snap.erasePending,
                 "SW_ERASE dirty", "SW_ERASE participates in the update region and marks erase pending");
    if (hwnd) ValidateRect(hwnd, NULL);

    memset(&out, 0, sizeof(out));
    region = hwnd ? ScrollWindowEx(hwnd, 10, 5, &scroll, NULL, 0, &out, 0) : ERROR;
    smoke_expect(&s, region == COMPLEXREGION, "ScrollWindowEx complex return", "diagonal scroll exposes a non-rectangular region");
    smoke_expect(&s, out.left == 0 && out.top == 0 && out.right == 100 && out.bottom == 50,
                 "ScrollWindowEx complex bounds", "prcUpdate receives bounding rectangle for the exposed region");
    smoke_expect(&s, hwnd && !GetUpdateRect(hwnd, NULL, FALSE), "ScrollWindowEx no flags", "no SW_INVALIDATE/SW_ERASE leaves update region clean");

    HWND child = hwnd ? CreateWindowExA(0, "myOS.v159.ScrollSmoke", "child", WS_CHILD|WS_VISIBLE,
                                       20, 20, 30, 20, hwnd, 0, 0, NULL) : 0;
    smoke_expect(&s, child != 0, "Create scroll child", "SW_SCROLLCHILDREN fixture");
    RECT before = {0,0,0,0};
    RECT after = {0,0,0,0};
    if (child) GetWindowRect(child, &before);
    g_user32_move = 0;
    region = hwnd ? ScrollWindowEx(hwnd, 7, -3, &scroll, NULL, 0, &out, SW_SCROLLCHILDREN) : ERROR;
    if (child) GetWindowRect(child, &after);
    smoke_expect(&s, region != ERROR, "SW_SCROLLCHILDREN return", "child-scroll call succeeds");
    smoke_expect(&s, child && after.left == before.left + 7 && after.top == before.top - 3,
                 "SW_SCROLLCHILDREN moves child", "intersecting child HWND is offset by dx/dy");
    smoke_expect(&s, g_user32_move >= 1, "SW_SCROLLCHILDREN WM_MOVE", "intersecting child receives WM_MOVE");

    g_user32_move = 0;
    region = hwnd ? ScrollWindowEx(hwnd, 0, 0, &scroll, NULL, 0, &out, SW_SCROLLCHILDREN) : ERROR;
    smoke_expect(&s, region == NULLREGION, "ScrollWindowEx zero delta", "zero scroll has no exposed update region");
    smoke_expect(&s, g_user32_move >= 1, "SW_SCROLLCHILDREN zero WM_MOVE", "Win32 sends WM_MOVE to intersecting children even if coordinates are unchanged");

    if (child) GetWindowRect(child, &before);
    BOOL ok = hwnd ? ScrollWindow(hwnd, -4, 0, NULL, NULL) : FALSE;
    if (child) GetWindowRect(child, &after);
    smoke_expect(&s, ok, "ScrollWindow wrapper", "legacy ScrollWindow succeeds via ScrollWindowEx");
    smoke_expect(&s, child && after.left == before.left - 4,
                 "ScrollWindow child offset", "legacy NULL lpRect path offsets child windows");
    smoke_expect(&s, hwnd && GetUpdateRect(hwnd, NULL, FALSE), "ScrollWindow invalidates", "legacy ScrollWindow combines uncovered area into update region");
    if (hwnd) ValidateRect(hwnd, NULL);

    SetLastError(0x12345678u);
    smoke_expect(&s, ScrollWindowEx((HWND)0, 1, 1, NULL, NULL, 0, NULL, SW_INVALIDATE) == ERROR,
                 "ScrollWindowEx invalid HWND", "rejects null HWND");
    smoke_expect_last_error(&s, ERROR_INVALID_WINDOW_HANDLE, "ScrollWindowEx invalid HWND LastError");

    if (child) DestroyWindow(child);
    if (hwnd) DestroyWindow(hwnd);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_menu(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "menu";
    g_user32_command = g_user32_initmenu = g_user32_initmenu_popup = g_user32_uninitmenu_popup = 0;
    g_user32_menuselect = g_user32_enter_menu_loop = g_user32_exit_menu_loop = 0;
    g_user32_measureitem = g_user32_drawitem = g_user32_accel_command = g_user32_syscommand = 0;
    g_user32_capture_changed = 0;
    g_user32_last_command = g_user32_last_syscommand = 0;
    g_user32_last_menuselect_wparam = g_user32_last_menuselect_lparam = 0;
    g_user32_last_initmenu_wparam = g_user32_last_initpopup_wparam = g_user32_last_uninitpopup_wparam = 0;
    g_user32_last_exit_menu_wparam = 0;
    HWND hwnd = smoke_create_basic_window(rt, "myOS.v129.SmokeMenu", "Smoke Menu", &s);

    HMENU menu = CreateMenu();
    HMENU popup = CreatePopupMenu();
    smoke_expect(&s, menu != 0, "CreateMenu", "top-level menu");
    smoke_expect(&s, popup != 0, "CreatePopupMenu", "popup/submenu");
    smoke_expect(&s, popup && AppendMenuA(popup, MF_STRING, 0x7001u, "Open"), "AppendMenuA popup item", "command 0x7001");
    smoke_expect(&s, popup && AppendMenuA(popup, MF_STRING | MF_DISABLED, 0x7002u, "Disabled"), "AppendMenuA disabled item", "command 0x7002");
    smoke_expect(&s, popup && AppendMenuA(popup, MF_SEPARATOR, 0, NULL), "AppendMenuA separator", "separator is not invokable");
    smoke_expect(&s, menu && popup && AppendMenuA(menu, MF_POPUP | MF_STRING, (UINT_PTR)popup, "File"), "AppendMenuA submenu", "File -> popup");
    smoke_expect(&s, menu && GetMenuItemCount(menu) == 1, "GetMenuItemCount(menu)", "top-level has one submenu");
    smoke_expect(&s, popup && GetMenuItemCount(popup) == 3, "GetMenuItemCount(popup)", "popup has three entries");
    smoke_expect(&s, menu && GetSubMenu(menu, 0) == popup, "GetSubMenu", "submenu handle roundtrip");
    smoke_expect(&s, popup && GetMenuItemID(popup, 0) == 0x7001u, "GetMenuItemID", "first popup command id");

    MENUITEMINFOA mii;
    memset(&mii, 0, sizeof(mii));
    char text[32];
    memset(text, 0, sizeof(text));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_ID | MIIM_STATE | MIIM_STRING | MIIM_FTYPE;
    mii.dwTypeData = text;
    mii.cch = sizeof(text);
    smoke_expect(&s, popup && GetMenuItemInfoA(popup, 0, TRUE, &mii) && mii.wID == 0x7001u && strcmp(text, "Open") == 0,
                 "GetMenuItemInfoA", "position 0 returns Open/0x7001");
    smoke_expect(&s, popup && CheckMenuItem(popup, 0x7001u, MF_BYCOMMAND | MF_CHECKED) == MF_UNCHECKED,
                 "CheckMenuItem", "returns previous unchecked state");
    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STATE;
    smoke_expect(&s, popup && GetMenuItemInfoA(popup, 0x7001u, FALSE, &mii) && (mii.fState & MFS_CHECKED),
                 "GetMenuItemInfoA checked state", "CheckMenuItem state is visible");
    smoke_expect(&s, popup && EnableMenuItem(popup, 0x7002u, MF_BYCOMMAND | MF_ENABLED) != (UINT)-1,
                 "EnableMenuItem", "disabled command can be enabled");
    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STATE;
    smoke_expect(&s, popup && GetMenuItemInfoA(popup, 0x7002u, FALSE, &mii) && !(mii.fState & (MFS_DISABLED | MFS_GRAYED)),
                 "EnableMenuItem state", "enabled command no longer reports disabled state");
    smoke_expect(&s, hwnd && menu && SetMenu(hwnd, menu), "SetMenu", "attach menu to smoke window");
    smoke_expect(&s, hwnd && GetMenu(hwnd) == menu, "GetMenu", "attached menu roundtrip");
    smoke_expect(&s, hwnd && DrawMenuBar(hwnd), "DrawMenuBar", "menu invalidation path");

    /* v137 global command-origin tripwire: an app menubar direct top-level
       command must emit exactly one WM_COMMAND, and it must not require a fake
       hand-painted hit path.  This exercises the WindowManager/raw-input path
       that real MDILab uses for its top "New child / Tile / Cascade" items. */
    int directIdx = wm_add_calc(&rt->wm, 260, 120, "v137 direct-menu smoke", rt->cap);
    HWND directHwnd = (directIdx >= 0 && directIdx < rt->wm.count) ? rt->wm.wins[directIdx].app_hwnd : 0;
    HMENU directMenu = CreateMenu();
    smoke_expect(&s, directIdx >= 0 && directHwnd && IsWindow(directHwnd), "WindowManager app slot for direct menu", "raw app-menubar click target");
    smoke_expect(&s, directMenu && AppendMenuA(directMenu, MF_STRING, 0x7010u, "&Direct"),
                 "AppendMenuA direct top-level item", "non-popup menubar command");
    smoke_expect(&s, directHwnd && SetWindowLongPtrA(directHwnd, GWLP_WNDPROC, (LONG_PTR)smoke_wndproc) != 0,
                 "SetWindowLongPtrA menu tripwire proc", "smoke WndProc receives WM_COMMAND");
    smoke_expect(&s, directHwnd && directMenu && SetMenu(directHwnd, directMenu),
                 "SetMenu direct top-level", "bar contains direct command item");
    g_user32_command = 0;
    g_user32_last_command = 0;
    g_user32_last_command_lparam = 0;
    int menuHandled = (directIdx >= 0 && directIdx < rt->wm.count) ?
        wm_route_raw_mouse_button_down(&rt->wm, rt->wm.wins[directIdx].x + 12,
                                       rt->wm.wins[directIdx].y + TITLEBAR_H + APP_MENUBAR_H / 2, 0) : 0;
    smoke_expect(&s, menuHandled && g_user32_command == 1 && LOWORD(g_user32_last_command) == 0x7010u &&
                     HIWORD(g_user32_last_command) == 0 && g_user32_last_command_lparam == 0,
                 "menubar direct WM_COMMAND tripwire", "one raw click produces exactly one menu-origin WM_COMMAND");

    g_user32_command = 0;
    g_user32_last_command = 0;
    if (directIdx >= 0 && directIdx < rt->wm.count) {
        rt->wm.focused = directIdx;
        rt->wm.wins[directIdx].active = 1;
    }
    int altMenuHandled = wm_activate_app_menu(&rt->wm, 0);
    smoke_expect(&s, altMenuHandled && g_user32_command == 0 && g_user32_last_command == 0,
                 "bare Alt direct-menubar guard", "Alt/F10 arms menu bar without invoking first direct command");
    rt->wm.menu_open = 0;
    rt->wm.menu_kind = MENU_KIND_START;

    if (directHwnd) DestroyWindow(directHwnd);
    if (directMenu) DestroyMenu(directMenu);

    HWND capBefore = hwnd ? SetCapture(hwnd) : 0;
    HWND focusBefore = hwnd ? SetFocus(hwnd) : 0;
    (void)capBefore;
    (void)focusBefore;
    g_user32_command = g_user32_enter_menu_loop = g_user32_exit_menu_loop = 0;
    g_user32_initmenu = g_user32_initmenu_popup = g_user32_uninitmenu_popup = 0;
    g_user32_menuselect = 0;
    g_user32_last_command = 0;
    if (hwnd) PostMessageA(hwnd, WM_KEYDOWN, KEY_ENTER, 0);
    BOOL tpmRet = popup && (UINT_PTR)TrackPopupMenu(popup, TPM_RETURNCMD, 10, 10, 0, hwnd, NULL) == 0x7001u;
    smoke_expect(&s, tpmRet, "TrackPopupMenu(TPM_RETURNCMD)", "queued ENTER commits first invokable command");
    smoke_expect(&s, g_user32_command == 0, "TPM_RETURNCMD no WM_COMMAND", "return-command mode does not dispatch command");
    smoke_expect(&s, g_user32_enter_menu_loop == 1 && g_user32_exit_menu_loop == 1,
                 "TrackPopupMenu modal loop notifications", "WM_ENTERMENULOOP and WM_EXITMENULOOP sent");
    smoke_expect(&s, g_user32_initmenu == 1 && g_user32_initmenu_popup >= 1 && g_user32_uninitmenu_popup >= 1,
                 "TrackPopupMenu init/uninit messages", "WM_INITMENU/POPUP and WM_UNINITMENUPOPUP sent");
    smoke_expect(&s, g_user32_menuselect >= 2 && LOWORD(g_user32_last_menuselect_wparam) == 0 && HIWORD(g_user32_last_menuselect_wparam) == 0xffffu,
                 "TrackPopupMenu close MENUSELECT", "menu close notification uses HIWORD(wParam)=0xFFFF");
    smoke_expect(&s, GetCapture() == hwnd && GetFocus() == hwnd,
                 "TrackPopupMenu restores capture/focus", "pre-existing owner capture/focus survive modal loop");

    g_user32_last_command = 0;
    g_user32_command = 0;
    if (hwnd) PostMessageA(hwnd, WM_KEYDOWN, KEY_ENTER, 0);
    smoke_expect(&s, popup && TrackPopupMenu(popup, 0, 10, 10, 0, hwnd, NULL),
                 "TrackPopupMenu dispatch", "queued ENTER non-TPM_RETURNCMD sends WM_COMMAND");
    smoke_expect(&s, g_user32_last_command == 0x7001u && g_user32_command == 1,
                 "TrackPopupMenu WM_COMMAND", "owner received command 0x7001 exactly once");

    HMENU disabledOnly = CreatePopupMenu();
    smoke_expect(&s, disabledOnly != 0, "CreatePopupMenu disabled-only", "cancel canary menu");
    smoke_expect(&s, disabledOnly && AppendMenuA(disabledOnly, MF_STRING | MF_DISABLED, 0x7003u, "Nope"),
                 "Append disabled-only item", "disabled command must not be selected");
    g_user32_last_command = 0;
    g_user32_command = 0;
    SetLastError(0x12345678u);
    smoke_expect(&s, disabledOnly && !TrackPopupMenu(disabledOnly, 0, 10, 10, 0, hwnd, NULL),
                 "TrackPopupMenu disabled-only cancels", "no invokable command means no dispatch");
    smoke_expect(&s, g_user32_command == 0 && g_user32_last_command == 0,
                 "disabled menu no WM_COMMAND", "disabled items cannot fire owner command");
    smoke_expect_last_error(&s, ERROR_CANCELLED, "TrackPopupMenu disabled-only LastError");

    HMENU idleCancel = CreatePopupMenu();
    smoke_expect(&s, idleCancel && AppendMenuA(idleCancel, MF_STRING, 0x7005u, "Idle cancel"),
                 "Append idle-cancel item", "real popup loop must not auto-commit without input");
    g_user32_last_command = 0;
    g_user32_command = 0;
    SetLastError(0x12345678u);
    smoke_expect(&s, idleCancel && !TrackPopupMenu(idleCancel, 0, 10, 10, 0, hwnd, NULL),
                 "TrackPopupMenu no-input cancels", "modal loop does not synthesize first command");
    smoke_expect(&s, g_user32_command == 0 && g_user32_last_command == 0,
                 "no-input menu no WM_COMMAND", "idle popup cancellation is silent");
    smoke_expect_last_error(&s, ERROR_CANCELLED, "TrackPopupMenu no-input LastError");

    HMENU ownerDraw = CreatePopupMenu();
    smoke_expect(&s, ownerDraw != 0, "CreatePopupMenu ownerdraw", "owner-draw canary menu");
    smoke_expect(&s, ownerDraw && AppendMenuA(ownerDraw, MF_OWNERDRAW, 0x7004u, (LPCSTR)(uintptr_t)0x7004u),
                 "AppendMenuA ownerdraw item", "owner-draw command 0x7004");
    g_user32_measureitem = g_user32_drawitem = 0;
    if (hwnd) PostMessageA(hwnd, WM_KEYDOWN, KEY_ENTER, 0);
    smoke_expect(&s, ownerDraw && TrackPopupMenu(ownerDraw, TPM_RETURNCMD, 10, 10, 0, hwnd, NULL) == (BOOL)0x7004u,
                 "TrackPopupMenu ownerdraw", "queued ENTER selects owner-draw item");
    smoke_expect(&s, g_user32_measureitem == 1 && g_user32_drawitem == 1,
                 "owner-draw menu messages", "WM_MEASUREITEM and WM_DRAWITEM are sent for selected owner-draw item");

    ACCEL ac[3];
    memset(ac, 0, sizeof(ac));
    ac[0].fVirt = FVIRTKEY | FCONTROL;
    ac[0].key = 'N';
    ac[0].cmd = 0x7701u;
    ac[1].fVirt = FVIRTKEY | FSHIFT;
    ac[1].key = 'X';
    ac[1].cmd = 0x7702u;
    ac[2].fVirt = FVIRTKEY | FALT;
    ac[2].key = KEY_F4;
    ac[2].cmd = SC_CLOSE;
    HACCEL ha = CreateAcceleratorTableA(ac, 3);
    smoke_expect(&s, ha != 0, "CreateAcceleratorTableA", "Ctrl+N, Shift+X, Alt+F4 accelerators");
    MSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hwnd;
    msg.message = WM_KEYDOWN;
    msg.wParam = 'N';
    msg.lParam = MYOS_KEYSTATE_CTRL;
    g_user32_accel_command = 0;
    smoke_expect(&s, ha && TranslateAcceleratorA(hwnd, ha, &msg) == 1 && g_user32_accel_command == 1,
                 "TranslateAcceleratorA Ctrl+N", "dispatches WM_COMMAND for Ctrl+N");
    g_user32_last_command = 0;
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hwnd;
    msg.message = WM_KEYDOWN;
    msg.wParam = 'N';
    msg.lParam = 0;
    smoke_expect(&s, ha && TranslateAcceleratorA(hwnd, ha, &msg) == 0 && g_user32_last_command == 0,
                 "TranslateAcceleratorA no modifier miss", "Ctrl accelerator does not match bare N");
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hwnd;
    msg.message = WM_KEYDOWN;
    msg.wParam = 'X';
    msg.lParam = MYOS_KEYSTATE_SHIFT;
    smoke_expect(&s, ha && TranslateAcceleratorA(hwnd, ha, &msg) == 1 && LOWORD(g_user32_last_command) == 0x7702u,
                 "TranslateAcceleratorA Shift+X", "shift modifier participates in matching");
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hwnd;
    msg.message = WM_SYSKEYDOWN;
    msg.wParam = KEY_F4;
    msg.lParam = MYOS_KEYSTATE_ALT;
    g_user32_syscommand = 0;
    g_user32_last_syscommand = 0;
    smoke_expect(&s, ha && TranslateAcceleratorA(hwnd, ha, &msg) == 1 &&
                     g_user32_syscommand == 1 && LOWORD(g_user32_last_syscommand) == SC_CLOSE,
                 "TranslateAcceleratorA Alt+F4", "system accelerator dispatches WM_SYSCOMMAND/SC_CLOSE");

    g_user32_last_command = 0;
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hwnd;
    msg.message = WM_KEYDOWN;
    msg.wParam = KEY_N;
    msg.lParam = MYOS_KEYSTATE_CTRL;
    smoke_expect(&s, ha && TranslateAcceleratorA(hwnd, ha, &msg) == 1 && LOWORD(g_user32_last_command) == 0x7701u,
                 "TranslateAcceleratorA Linux keycode", "FVIRTKEY accepts desktop KEY_N as VK_N");

    g_user32_last_command = 0;
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hwnd;
    msg.message = WM_KEYDOWN;
    msg.wParam = 'N';
    msg.lParam = MYOS_KEYSTATE_CTRL | MYOS_KEYSTATE_SHIFT;
    smoke_expect(&s, ha && TranslateAcceleratorA(hwnd, ha, &msg) == 0 && g_user32_last_command == 0,
                 "TranslateAcceleratorA exact modifiers", "Ctrl accelerator does not fire with extra Shift");

    ACCEL ch[1];
    memset(ch, 0, sizeof(ch));
    ch[0].fVirt = 0;
    ch[0].key = 'm';
    ch[0].cmd = 0x7703u;
    HACCEL hChar = CreateAcceleratorTableA(ch, 1);
    g_user32_last_command = 0;
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hwnd;
    msg.message = WM_CHAR;
    msg.wParam = 'M';
    msg.lParam = 0;
    smoke_expect(&s, hChar && TranslateAcceleratorA(hwnd, hChar, &msg) == 1 && LOWORD(g_user32_last_command) == 0x7703u,
                 "TranslateAcceleratorA character accelerator", "non-FVIRTKEY accelerator handles WM_CHAR case-insensitively");
    smoke_expect(&s, hChar && DestroyAcceleratorTable(hChar), "DestroyAcceleratorTable char", "character accelerator lifetime closed");

    smoke_expect(&s, ha && DestroyAcceleratorTable(ha), "DestroyAcceleratorTable", "accelerator lifetime closed");
    smoke_expect(&s, !DestroyAcceleratorTable(ha), "DestroyAcceleratorTable double destroy", "destroyed accelerator is invalid");

    if (GetCapture() == hwnd) ReleaseCapture();
    if (ownerDraw) DestroyMenu(ownerDraw);
    if (idleCancel) DestroyMenu(idleCancel);
    if (disabledOnly) DestroyMenu(disabledOnly);
    if (popup) DestroyMenu(popup);
    if (menu) DestroyMenu(menu);
    if (hwnd) DestroyWindow(hwnd);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static HMENU g_v154_init_enable_menu = 0;
static int g_v154_cancelmode = 0;

static LRESULT CALLBACK smoke_v154_menu_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (Msg == WM_INITMENUPOPUP) {
        g_user32_initmenu_popup++;
        g_user32_last_initpopup_wparam = wParam;
        if ((HMENU)wParam == g_v154_init_enable_menu)
            EnableMenuItem((HMENU)wParam, 0x1541u, MF_BYCOMMAND | MF_ENABLED);
        return 0;
    }
    if (Msg == WM_CANCELMODE) {
        g_v154_cancelmode++;
        return 0;
    }
    return smoke_wndproc(hWnd, Msg, wParam, lParam);
}

static int smoke_user32_popup_modal(SmokeRuntime* rt)
{
    SmokeContext s = {0,0,0,0,"user32_popup_modal"};
    (void)rt;

    smoke_runtime_init(rt);
    Capability owner = rt->cap;
    Capability proc = cap_create(1541, "v154-popup-modal", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&proc, 0);
    MyWinBindRuntime(&rt->mgr, &proc);
    MyWinBindDesktop(&rt->wm);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_v154_menu_wndproc;
    wc.lpszClassName = "myOS.v154.PopupOwner";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, "myOS.v154.PopupOwner", "v154 popup owner",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                10, 10, 240, 120, 0, 0, 0, NULL);
    smoke_expect(&s, hwnd != 0, "CreateWindowExA popup owner", "v154 owner window");

    HMENU initMenu = CreatePopupMenu();
    g_v154_init_enable_menu = initMenu;
    smoke_expect(&s, initMenu && AppendMenuA(initMenu, MF_STRING | MF_DISABLED, 0x1541u, "Init-enables me"),
                 "Append disabled init item", "item starts disabled");
    g_user32_initmenu_popup = 0;
    SetLastError(0x154154u);
    if (hwnd) PostMessageA(hwnd, WM_KEYDOWN, KEY_ENTER, 0);
    UINT_PTR initRet = hwnd ? (UINT_PTR)TrackPopupMenu(initMenu, TPM_RETURNCMD, 4, 4, 0, hwnd, NULL) : 0;
    smoke_expect(&s, initRet == 0x1541u,
                 "TrackPopupMenu post-init selection", "WM_INITMENUPOPUP can enable item before selection scan");
    smoke_expect(&s, g_user32_initmenu_popup >= 1,
                 "TrackPopupMenu WM_INITMENUPOPUP", "popup init delivered before command resolution");

    HMENU keyMenu = CreatePopupMenu();
    smoke_expect(&s, keyMenu && AppendMenuA(keyMenu, MF_STRING, 0x1542u, "First"),
                 "Append keyboard first", "first item");
    smoke_expect(&s, keyMenu && AppendMenuA(keyMenu, MF_STRING, 0x1543u, "Second"),
                 "Append keyboard second", "second item");
    if (hwnd) {
        PostMessageA(hwnd, WM_KEYDOWN, KEY_DOWN, 0);
        PostMessageA(hwnd, WM_KEYDOWN, KEY_ENTER, 0);
    }
    UINT_PTR keyRet = hwnd ? (UINT_PTR)TrackPopupMenu(keyMenu, TPM_RETURNCMD, 8, 8, 0, hwnd, NULL) : 0;
    smoke_expect(&s, keyRet == 0x1543u,
                 "TrackPopupMenu keyboard navigation", "queued DOWN+ENTER commits second item");

    HMENU escMenu = CreatePopupMenu();
    smoke_expect(&s, escMenu && AppendMenuA(escMenu, MF_STRING, 0x1544u, "Cancel target"),
                 "Append cancel target", "ESC should cancel before command");
    g_user32_command = 0;
    if (hwnd) PostMessageA(hwnd, WM_KEYDOWN, KEY_ESC, 0);
    SetLastError(0x154154u);
    BOOL escRet = hwnd ? TrackPopupMenu(escMenu, 0, 8, 8, 0, hwnd, NULL) : FALSE;
    smoke_expect(&s, escRet == FALSE && g_user32_command == 0,
                 "TrackPopupMenu ESC cancel", "queued ESC cancels without WM_COMMAND");
    smoke_expect_last_error(&s, ERROR_CANCELLED, "TrackPopupMenu ESC LastError");

    HMENU noNotify = CreatePopupMenu();
    smoke_expect(&s, noNotify && AppendMenuA(noNotify, MF_STRING, 0x1545u, "No notify"),
                 "Append TPM_NONOTIFY item", "nonnotify target");
    g_user32_command = 0;
    g_user32_last_command = 0;
    if (hwnd) PostMessageA(hwnd, WM_KEYDOWN, KEY_ENTER, 0);
    BOOL nnRet = hwnd ? TrackPopupMenu(noNotify, TPM_NONOTIFY, 8, 8, 0, hwnd, NULL) : FALSE;
    smoke_expect(&s, nnRet == TRUE && g_user32_command == 0 && g_user32_last_command == 0,
                 "TrackPopupMenu TPM_NONOTIFY", "selection succeeds without WM_COMMAND dispatch");

    HMENU exMenu = CreatePopupMenu();
    TPMPARAMS tpm;
    memset(&tpm, 0, sizeof(tpm));
    tpm.cbSize = sizeof(tpm);
    tpm.rcExclude.left = 1;
    tpm.rcExclude.top = 2;
    tpm.rcExclude.right = 3;
    tpm.rcExclude.bottom = 4;
    smoke_expect(&s, exMenu && AppendMenuA(exMenu, MF_STRING, 0x1546u, "TrackEx"),
                 "Append TrackPopupMenuEx item", "TrackPopupMenuEx target");
    if (hwnd) PostMessageA(hwnd, WM_KEYDOWN, KEY_ENTER, 0);
    UINT_PTR exRet = hwnd ? (UINT_PTR)TrackPopupMenuEx(exMenu, TPM_RETURNCMD, 12, 12, hwnd, &tpm) : 0;
    smoke_expect(&s, exRet == 0x1546u,
                 "TrackPopupMenuEx", "TPMPARAMS path returns command");
    tpm.cbSize = sizeof(tpm) - 1u;
    SetLastError(0x154154u);
    smoke_expect(&s, !TrackPopupMenuEx(exMenu, TPM_RETURNCMD, 12, 12, hwnd, &tpm),
                 "TrackPopupMenuEx bad TPMPARAMS", "short TPMPARAMS rejected");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "TrackPopupMenuEx bad TPMPARAMS LastError");

    HMENU mouseMenu = CreatePopupMenu();
    smoke_expect(&s, mouseMenu && AppendMenuA(mouseMenu, MF_STRING, 0x1547u, "Mouse first"),
                 "Append mouse first", "mouse row 0");
    smoke_expect(&s, mouseMenu && AppendMenuA(mouseMenu, MF_STRING, 0x1548u, "Mouse second"),
                 "Append mouse second", "mouse row 1");
    if (hwnd) PostMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(14, 8 + 18 + 4));
    UINT_PTR mouseRet = hwnd ? (UINT_PTR)TrackPopupMenu(mouseMenu, TPM_RETURNCMD, 8, 8, 0, hwnd, NULL) : 0;
    smoke_expect(&s, mouseRet == 0x1548u,
                 "TrackPopupMenu mouse row commit", "queued mouse-up selects row under cursor");

    SetLastError(0x154154u);
    smoke_expect(&s, !TrackPopupMenu(mouseMenu, TPM_RETURNCMD, 8, 8, 1, hwnd, NULL),
                 "TrackPopupMenu reserved parameter", "nonzero reserved argument rejected");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "TrackPopupMenu reserved LastError");

    if (mouseMenu) DestroyMenu(mouseMenu);
    if (exMenu) DestroyMenu(exMenu);
    if (noNotify) DestroyMenu(noNotify);
    if (escMenu) DestroyMenu(escMenu);
    if (keyMenu) DestroyMenu(keyMenu);
    if (initMenu) DestroyMenu(initMenu);
    g_v154_init_enable_menu = 0;
    if (hwnd) DestroyWindow(hwnd);
    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static int smoke_capture(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "capture";
    g_user32_capture_changed = 0;
    g_user32_last_capture_lparam = 0;
    HWND a = smoke_create_basic_window(rt, "myOS.v129.SmokeCaptureA", "Capture A", &s);
    HWND b = smoke_create_basic_window(rt, "myOS.v129.SmokeCaptureB", "Capture B", &s);

    HWND ret = a ? SetCapture(a) : 0;
    smoke_expect(&s, ret == 0 && GetCapture() == a, "SetCapture(A)", "first capture returns NULL and owner becomes A");
    ret = b ? SetCapture(b) : 0;
    smoke_expect(&s, ret == a && GetCapture() == b, "SetCapture(B) replaces A", "old capture owner returned");
    smoke_expect(&s, g_user32_capture_changed == 1 && g_user32_last_capture_lparam == (LPARAM)b,
                 "WM_CAPTURECHANGED replacement", "synchronous notification uses lParam=new capture HWND");
    smoke_expect(&s, ReleaseCapture() && GetCapture() == 0, "ReleaseCapture", "capture cleared");
    smoke_expect(&s, g_user32_capture_changed == 2 && g_user32_last_capture_lparam == 0,
                 "WM_CAPTURECHANGED release", "released capture window is notified with lParam=NULL");
    smoke_expect(&s, ReleaseCapture() == FALSE, "ReleaseCapture without owner", "returns FALSE when no capture exists");
    smoke_expect(&s, SetCapture(0) == 0, "SetCapture(NULL)", "invalid capture request fails");

    if (b) DestroyWindow(b);
    if (a) DestroyWindow(a);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static ATOM smoke_register_proc_class(SmokeContext* s, const char* className)
{
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_wndproc;
    wc.lpszClassName = className;
    ATOM atom = RegisterClassExA(&wc);
    smoke_expect(s, atom != 0, "RegisterClassExA", className);
    return atom;
}

static INT_PTR CALLBACK smoke_v192_default_dlgproc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)hDlg; (void)uMsg; (void)wParam; (void)lParam;
    return FALSE;
}

typedef struct MYOS_PACKED SmokeV193DialogTemplate { DLGTEMPLATE dlg; WORD menu; WORD className; WORD title; } SmokeV193DialogTemplate;
static SmokeV193DialogTemplate g_v193_outer_template = { { WS_VISIBLE, 0, 0, 4, 4, 110, 56 }, 0, 0, 0 };
static SmokeV193DialogTemplate g_v193_inner_template = { { WS_VISIBLE, 0, 0, 6, 6, 88, 42 }, 0, 0, 0 };
static HWND g_v193_root = 0;
static HWND g_v193_root_child = 0;
static HWND g_v193_outer = 0;
static HWND g_v193_inner = 0;
static int g_v193_outer_saw_root_disabled = 0;
static int g_v193_outer_saw_focus_inside = 0;
static int g_v193_inner_saw_outer_disabled = 0;
static int g_v193_inner_saw_root_disabled = 0;
static int g_v193_inner_saw_capture_released = 0;
static int g_v193_after_inner_root_still_disabled = 0;
static int g_v193_after_inner_outer_reenabled = 0;
static HWND g_v193_focus_after_inner = 0;
static INT_PTR g_v193_inner_result = 0;

static INT_PTR CALLBACK smoke_v193_inner_dlgproc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    if (uMsg == WM_INITDIALOG) {
        g_v193_inner = hDlg;
        PostMessageA(hDlg, WM_USER + 194, 0, 0);
        return TRUE;
    }
    if (uMsg == WM_USER + 194) {
        g_v193_inner_saw_outer_disabled = g_v193_outer ? !IsWindowEnabled(g_v193_outer) : 0;
        g_v193_inner_saw_root_disabled = g_v193_root ? !IsWindowEnabled(g_v193_root) : 0;
        g_v193_inner_saw_capture_released = (GetCapture() == 0);
        EndDialog(hDlg, (INT_PTR)0x1931);
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK smoke_v193_outer_dlgproc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    if (uMsg == WM_INITDIALOG) {
        g_v193_outer = hDlg;
        PostMessageA(hDlg, WM_USER + 193, 0, 0);
        return TRUE;
    }
    if (uMsg == WM_USER + 193) {
        HWND f = GetFocus();
        g_v193_outer_saw_root_disabled = g_v193_root ? !IsWindowEnabled(g_v193_root) : 0;
        g_v193_outer_saw_focus_inside = (f == hDlg || IsChild(hDlg, f));
        g_v193_inner_result = DialogBoxIndirectParamA(0, (LPCDLGTEMPLATEA)&g_v193_inner_template,
                                                       hDlg, smoke_v193_inner_dlgproc, 0x193);
        g_v193_after_inner_root_still_disabled = g_v193_root ? !IsWindowEnabled(g_v193_root) : 0;
        g_v193_after_inner_outer_reenabled = IsWindowEnabled(hDlg);
        g_v193_focus_after_inner = GetFocus();
        EndDialog(hDlg, (INT_PTR)0x1930);
        return TRUE;
    }
    return FALSE;
}

static int smoke_focus_dialog(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "focusdlg";
    smoke_runtime_init(rt);

    g_user32_command = 0;
    g_user32_setfocus = g_user32_killfocus = g_user32_enable = 0;
    g_user32_last_focus_from = g_user32_last_kill_to = g_user32_last_enable = 0;
    g_user32_last_command = 0;
    SetFocus(0);
    if (GetCapture()) ReleaseCapture();

    smoke_register_proc_class(&s, "myOS.v129.SmokeFocusRoot");
    smoke_register_proc_class(&s, "myOS.v129.SmokeFocusChild");
    HWND root = CreateWindowExA(0, "myOS.v129.SmokeFocusRoot", "Focus Root", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                20, 20, 320, 180, 0, 0, 0, NULL);
    smoke_expect(&s, root != 0 && IsWindow(root), "Create focus root", "top-level owner for focus children");
    HWND c1 = root ? CreateWindowExA(0, "myOS.v129.SmokeFocusChild", "c1", WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                     4, 4, 80, 24, root, (HMENU)(UINT_PTR)101, 0, NULL) : 0;
    HWND c2 = root ? CreateWindowExA(0, "myOS.v129.SmokeFocusChild", "c2", WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                     4, 34, 80, 24, root, (HMENU)(UINT_PTR)102, 0, NULL) : 0;
    smoke_expect(&s, c1 && c2, "Create focus children", "two focusable WS_TABSTOP children");

    HWND old = c1 ? SetFocus(c1) : 0;
    smoke_expect(&s, old == 0 && GetFocus() == c1, "SetFocus first child", "first focus transition returns NULL");
    smoke_expect(&s, g_user32_setfocus == 1 && g_user32_last_focus_from == 0,
                 "WM_SETFOCUS first child", "focus notification delivered synchronously");
    old = c2 ? SetFocus(c2) : 0;
    smoke_expect(&s, old == c1 && GetFocus() == c2, "SetFocus second child", "old focus HWND returned");
    smoke_expect(&s, g_user32_killfocus == 1 && g_user32_last_kill_to == (WPARAM)c2 &&
                     g_user32_setfocus == 2 && g_user32_last_focus_from == (WPARAM)c1,
                 "WM_KILLFOCUS/WM_SETFOCUS order", "old loses focus and new gains focus synchronously");

    BOOL prevEnabled = c2 ? EnableWindow(c2, FALSE) : FALSE;
    smoke_expect(&s, prevEnabled == TRUE && !IsWindowEnabled(c2), "EnableWindow(FALSE)", "disables focused child");
    smoke_expect(&s, GetFocus() == 0 && g_user32_last_enable == FALSE,
                 "Disable clears focus", "focused disabled window loses focus before returning");
    smoke_expect(&s, c2 && SetFocus(c2) == 0 && GetFocus() == 0,
                 "SetFocus disabled child", "disabled HWND cannot receive focus");
    smoke_expect(&s, c2 && EnableWindow(c2, TRUE) == FALSE && IsWindowEnabled(c2) && g_user32_last_enable == TRUE,
                 "EnableWindow(TRUE)", "reenable returns previous disabled state and sends WM_ENABLE");

    smoke_register_proc_class(&s, "myOS.v129.SmokeDialogRoot");
    HWND dlg = CreateWindowExA(0, "myOS.v129.SmokeDialogRoot", "Dialog Core", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                               40, 60, 360, 220, 0, 0, 0, NULL);
    smoke_expect(&s, dlg != 0 && IsWindow(dlg), "Create dialog-core root", "dialog-style parent for IsDialogMessage smoke");
    HWND bFirst = dlg ? CreateWindowExA(0, "BUTTON", "First", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                                        8, 8, 70, 22, dlg, (HMENU)(UINT_PTR)3001, 0, NULL) : 0;
    HWND bDisabled = dlg ? CreateWindowExA(0, "BUTTON", "Disabled", WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_DISABLED|BS_PUSHBUTTON,
                                           8, 36, 80, 22, dlg, (HMENU)(UINT_PTR)3002, 0, NULL) : 0;
    HWND bHidden = dlg ? CreateWindowExA(0, "BUTTON", "Hidden", WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,
                                         8, 64, 80, 22, dlg, (HMENU)(UINT_PTR)3003, 0, NULL) : 0;
    HWND bOk = dlg ? CreateWindowExA(0, "BUTTON", "OK", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
                                     8, 92, 70, 22, dlg, (HMENU)(UINT_PTR)IDOK, 0, NULL) : 0;
    HWND bCancel = dlg ? CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                                         8, 120, 70, 22, dlg, (HMENU)(UINT_PTR)IDCANCEL, 0, NULL) : 0;
    HWND bMnemonic = dlg ? CreateWindowExA(0, "BUTTON", "&Help", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                                           8, 148, 70, 22, dlg, (HMENU)(UINT_PTR)3004, 0, NULL) : 0;
    smoke_expect(&s, bFirst && bDisabled && bHidden && bOk && bCancel && bMnemonic, "Create dialog buttons", "tab/default/cancel/mnemonic controls exist");
    smoke_expect(&s, GetNextDlgTabItem(dlg, 0, FALSE) == bFirst,
                 "GetNextDlgTabItem first", "first visible enabled tabstop is selected");
    smoke_expect(&s, GetNextDlgTabItem(dlg, bFirst, FALSE) == bOk,
                 "GetNextDlgTabItem skips disabled/hidden", "disabled and invisible tabstops are not returned");

    MSG m;
    memset(&m, 0, sizeof(m));
    SetFocus(bFirst);
    m.hwnd = bFirst; m.message = WM_KEYDOWN; m.wParam = KEY_TAB; m.lParam = 0;
    smoke_expect(&s, IsDialogMessageA(dlg, &m) && GetFocus() == bOk,
                 "IsDialogMessageA TAB", "TAB advances to next enabled tabstop");
    memset(&m, 0, sizeof(m));
    m.hwnd = bOk; m.message = WM_KEYDOWN; m.wParam = KEY_TAB; m.lParam = MYOS_KEYSTATE_SHIFT;
    smoke_expect(&s, IsDialogMessageA(dlg, &m) && GetFocus() == bFirst,
                 "IsDialogMessageA Shift+TAB", "reverse TAB wraps over disabled controls");

    memset(&m, 0, sizeof(m));
    SetFocus(bFirst);
    m.hwnd = bFirst; m.message = WM_KEYDOWN; m.wParam = VK_TAB; m.lParam = 0;
    smoke_expect(&s, IsDialogMessageA(dlg, &m) && GetFocus() == bOk,
                 "IsDialogMessageA VK_TAB", "Win32 VK_TAB advances just like the evdev KEY_TAB path");

    g_user32_command = 0;
    g_user32_last_command = 0;
    SetFocus(dlg);
    memset(&m, 0, sizeof(m));
    m.hwnd = dlg; m.message = WM_KEYDOWN; m.wParam = KEY_ENTER;
    smoke_expect(&s, IsDialogMessageA(dlg, &m) && LOWORD(g_user32_last_command) == IDOK,
                 "IsDialogMessageA ENTER", "default pushbutton dispatches IDOK WM_COMMAND when focus is on the dialog");

    g_user32_last_command = 0;
    memset(&m, 0, sizeof(m));
    SetFocus(dlg);
    m.hwnd = dlg; m.message = WM_KEYDOWN; m.wParam = VK_RETURN;
    smoke_expect(&s, IsDialogMessageA(dlg, &m) && LOWORD(g_user32_last_command) == IDOK,
                 "IsDialogMessageA VK_RETURN", "Win32 VK_RETURN dispatches the visible default pushbutton");

    g_user32_last_command = 0;
    memset(&m, 0, sizeof(m));
    m.hwnd = bFirst; m.message = WM_KEYDOWN; m.wParam = KEY_ESC;
    smoke_expect(&s, IsDialogMessageA(dlg, &m) && LOWORD(g_user32_last_command) == IDCANCEL,
                 "IsDialogMessageA ESC", "cancel button dispatches IDCANCEL WM_COMMAND");

    g_user32_last_command = 0;
    memset(&m, 0, sizeof(m));
    m.hwnd = bFirst; m.message = WM_KEYDOWN; m.wParam = VK_ESCAPE;
    smoke_expect(&s, IsDialogMessageW(dlg, &m) && LOWORD(g_user32_last_command) == IDCANCEL,
                 "IsDialogMessageW VK_ESCAPE", "W alias uses the same dialog-manager path and VK normalization");

    g_user32_last_command = 0;
    memset(&m, 0, sizeof(m));
    m.hwnd = dlg; m.message = WM_SYSCHAR; m.wParam = 'h';
    BOOL mnemonicHandled = IsDialogMessageA(dlg, &m);
    hwnd_dispatch(&rt->mgr);
    smoke_expect(&s, mnemonicHandled && LOWORD(g_user32_last_command) == 3004,
                 "IsDialogMessageA WM_SYSCHAR mnemonic", "Alt mnemonic activates the ampersand button without dispatching raw keydown");

    typedef struct MYOS_PACKED SmokeV192DialogTemplate { DLGTEMPLATE dlg; WORD menu; WORD className; WORD title; } SmokeV192DialogTemplate;
    SmokeV192DialogTemplate focusTpl = { { WS_VISIBLE, 0, 0, 4, 4, 96, 48 }, 0, 0, 0 };
    HWND realDlg = CreateDialogIndirectParamA(0, (LPCDLGTEMPLATEA)&focusTpl, dlg, smoke_v192_default_dlgproc, 0);
    HWND realOk = realDlg ? CreateWindowExA(0, "BUTTON", "OK", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
                                           8, 8, 70, 22, realDlg, (HMENU)(UINT_PTR)IDOK, 0, NULL) : 0;
    smoke_expect(&s, realDlg && realOk, "Create real #32770 dialog", "modeless dialog for DM_SETDEFID/IsDialogMessage VK_RETURN");
    if (realDlg) SendMessageA(realDlg, DM_SETDEFID, IDOK, 0);
    smoke_expect(&s, realDlg && LOWORD(SendMessageA(realDlg, DM_GETDEFID, 0, 0)) == IDOK,
                 "DM_SETDEFID/DM_GETDEFID", "DefDlgProc stores and reports the default pushbutton id");
    memset(&m, 0, sizeof(m));
    if (realDlg) SetFocus(realDlg);
    m.hwnd = realDlg; m.message = WM_KEYDOWN; m.wParam = VK_RETURN;
    smoke_expect(&s, realDlg && IsDialogMessageA(realDlg, &m) && !IsWindow(realDlg),
                 "IsDialogMessageA real dialog VK_RETURN", "VK_RETURN reaches DefDlgProc default button and EndDialog destroys the modeless dialog");

    HWND r1 = dlg ? CreateWindowExA(0, "BUTTON", "R1", WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_GROUP|BS_AUTORADIOBUTTON,
                                    140, 8, 70, 22, dlg, (HMENU)(UINT_PTR)3101, 0, NULL) : 0;
    HWND r2 = dlg ? CreateWindowExA(0, "BUTTON", "R2", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
                                    140, 36, 70, 22, dlg, (HMENU)(UINT_PTR)3102, 0, NULL) : 0;
    smoke_expect(&s, r1 && r2, "Create radio group", "two auto-radio buttons in one group");
    if (r1) SendMessageA(r1, BM_SETCHECK, BST_CHECKED, 0);
    SetFocus(r1);
    memset(&m, 0, sizeof(m));
    m.hwnd = r1; m.message = WM_KEYDOWN; m.wParam = KEY_RIGHT;
    smoke_expect(&s, IsDialogMessageA(dlg, &m) && GetFocus() == r2 && SendMessageA(r2, BM_GETCHECK, 0, 0) == BST_CHECKED,
                 "IsDialogMessageA radio arrow", "arrow moves focus and checked state within radio group");

    DWORD mdReg0 = 0, mdUnreg0 = 0, mdLive0 = 0, mdHit0 = 0, mdMiss0 = 0;
    MyGetModelessDialogAudit(&mdReg0, &mdUnreg0, &mdLive0, &mdHit0, &mdMiss0);
    SmokeV192DialogTemplate modelessTpl = { { WS_VISIBLE, 0, 0, 10, 10, 116, 52 }, 0, 0, 0 };
    HWND mdl = root ? CreateDialogIndirectParamA(0, (LPCDLGTEMPLATEA)&modelessTpl, root, smoke_v192_default_dlgproc, 0x194) : 0;
    HWND mdlOk = mdl ? CreateWindowExA(0, "BUTTON", "OK", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
                                       8, 8, 64, 22, mdl, (HMENU)(UINT_PTR)IDOK, 0, NULL) : 0;
    HWND mdlCancel = mdl ? CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                                           8, 34, 72, 22, mdl, (HMENU)(UINT_PTR)IDCANCEL, 0, NULL) : 0;
    DWORD mdReg1 = 0, mdUnreg1 = 0, mdLive1 = 0, mdHit1 = 0, mdMiss1 = 0;
    MyGetModelessDialogAudit(&mdReg1, &mdUnreg1, &mdLive1, &mdHit1, &mdMiss1);
    smoke_expect(&s, mdl && MyIsModelessDialog(mdl) && mdLive1 == mdLive0 + 1 && mdReg1 == mdReg0 + 1,
                 "modeless dialog registry", "CreateDialogIndirectParamA registers a live modeless dialog");
    smoke_expect(&s, root && IsWindowEnabled(root),
                 "modeless owner remains enabled", "CreateDialog does not apply DialogBox-style owner disable");

    memset(&m, 0, sizeof(m));
    if (mdl) SetFocus(mdl);
    m.hwnd = mdl; m.message = WM_KEYDOWN; m.wParam = VK_TAB;
    smoke_expect(&s, mdl && mdlOk && MyTranslateModelessDialogMessageA(&m) && GetFocus() == mdlOk,
                 "modeless pump VK_TAB", "shared modeless registry routes Tab through IsDialogMessageA");

    memset(&m, 0, sizeof(m));
    m.hwnd = mdlOk; m.message = WM_KEYDOWN; m.wParam = VK_TAB; m.lParam = MYOS_KEYSTATE_SHIFT;
    smoke_expect(&s, mdl && mdlCancel && MyTranslateModelessDialogMessageA(&m) && GetFocus() == mdlCancel,
                 "modeless pump Shift+TAB", "modeless dialog traversal wraps backward inside its child tree");

    if (mdl) SendMessageA(mdl, DM_SETDEFID, IDOK, 0);
    if (mdl) SetFocus(mdl);
    memset(&m, 0, sizeof(m));
    m.hwnd = mdl; m.message = WM_KEYDOWN; m.wParam = VK_RETURN;
    BOOL mdEnter = mdl ? MyTranslateModelessDialogMessageA(&m) : FALSE;
    DWORD mdReg2 = 0, mdUnreg2 = 0, mdLive2 = 0, mdHit2 = 0, mdMiss2 = 0;
    MyGetModelessDialogAudit(&mdReg2, &mdUnreg2, &mdLive2, &mdHit2, &mdMiss2);
    smoke_expect(&s, mdEnter && (!mdl || !IsWindow(mdl)) && mdLive2 == mdLive0 && mdUnreg2 == mdUnreg0 + 1,
                 "modeless Enter unregisters", "default-button EndDialog destroys and unregisters the modeless dialog");
    smoke_expect(&s, mdHit2 >= mdHit0 + 3,
                 "modeless pump audit hits", "modeless registry pump records handled dialog messages");

    HWND stale = root ? CreateDialogIndirectParamA(0, (LPCDLGTEMPLATEA)&modelessTpl, root, smoke_v192_default_dlgproc, 0x1941) : 0;
    if (stale) DestroyWindow(stale);
    memset(&m, 0, sizeof(m));
    m.hwnd = stale; m.message = WM_KEYDOWN; m.wParam = VK_TAB;
    smoke_expect(&s, !MyTranslateModelessDialogMessageA(&m) && MyWinGetModelessDialogCount() == mdLive0,
                 "stale modeless ignored", "destroyed modeless HWND is removed from the shared registry/pump");

    g_v193_root = root;
    g_v193_root_child = c1;
    g_v193_outer = g_v193_inner = 0;
    g_v193_outer_saw_root_disabled = g_v193_outer_saw_focus_inside = 0;
    g_v193_inner_saw_outer_disabled = g_v193_inner_saw_root_disabled = 0;
    g_v193_inner_saw_capture_released = 0;
    g_v193_after_inner_root_still_disabled = 0;
    g_v193_after_inner_outer_reenabled = 0;
    g_v193_focus_after_inner = 0;
    g_v193_inner_result = 0;
    if (c1) {
        EnableWindow(c1, TRUE);
        SetFocus(c1);
        SetCapture(c1);
    }
    INT_PTR nestedResult = root ? DialogBoxIndirectParamA(0, (LPCDLGTEMPLATEA)&g_v193_outer_template,
                                                          root, smoke_v193_outer_dlgproc, 0x193) : -1;
    smoke_expect(&s, nestedResult == (INT_PTR)0x1930 && g_v193_inner_result == (INT_PTR)0x1931,
                 "modal nested result stack", "outer and inner DialogBox loops return their own EndDialog result");
    smoke_expect(&s, g_v193_outer_saw_root_disabled && g_v193_outer_saw_focus_inside,
                 "modal disables owner and focuses dialog", "outer modal sees disabled owner and focus routed into dialog tree");
    smoke_expect(&s, g_v193_inner_saw_outer_disabled && g_v193_inner_saw_root_disabled && g_v193_inner_saw_capture_released,
                 "nested modal keeps owner chain disabled", "inner modal disables outer without reenabling the original owner or keeping stale capture");
    smoke_expect(&s, g_v193_after_inner_root_still_disabled && g_v193_after_inner_outer_reenabled,
                 "nested modal restore boundary", "closing inner reenables only the outer dialog while root owner remains modal-disabled");
    smoke_expect(&s, root && IsWindowEnabled(root) && GetFocus() == c1 && GetCapture() == c1,
                 "modal restores owner focus/capture", "closing outer reenables owner and restores previous focus/capture target");
    if (GetCapture() == c1) ReleaseCapture();
    g_v193_root = g_v193_root_child = g_v193_outer = g_v193_inner = 0;

    if (dlg) DestroyWindow(dlg);
    if (root) DestroyWindow(root);
    SetFocus(0);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}



static int smoke_pump_parent_commands(HWND hParent)
{
    int pumped = 0;
    MSG msg;
    memset(&msg, 0, sizeof(msg));
    while (PeekMessageA(&msg, hParent, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        pumped++;
        if (pumped > 64) break;
    }
    return pumped;
}

static void smoke_reset_control_notifications(void)
{
    g_user32_command = 0;
    g_user32_last_command = 0;
    g_user32_last_command_lparam = 0;
    g_user32_hscroll = 0;
    g_user32_vscroll = 0;
    g_user32_last_scroll_wparam = 0;
    g_user32_last_scroll_lparam = 0;
}

static int smoke_controls(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "controls";
    smoke_runtime_init(rt);

    smoke_register_proc_class(&s, "myOS.v129.SmokeControlsRoot");
    HWND root = CreateWindowExA(0, "myOS.v129.SmokeControlsRoot", "Controls Root", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                20, 20, 520, 420, 0, 0, 0, NULL);
    smoke_expect(&s, root != 0, "controls root", "parent WndProc captures WM_COMMAND/WM_*SCROLL notifications");

    HWND btn = root ? CreateWindowExA(0, "BUTTON", "Push", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                                      10, 10, 80, 24, root, (HMENU)1001, 0, NULL) : 0;
    smoke_expect(&s, btn != 0, "BUTTON CreateWindowExA", "standard pushbutton class registered");
    smoke_reset_control_notifications();
    SendMessageA(btn, BM_CLICK, 0, 0);
    smoke_pump_parent_commands(root);
    smoke_expect(&s, g_user32_command == 1 && LOWORD(g_user32_last_command) == 1001 && HIWORD(g_user32_last_command) == BN_CLICKED && (HWND)g_user32_last_command_lparam == btn,
                 "BUTTON BM_CLICK -> WM_COMMAND/BN_CLICKED", "notification uses LOWORD=id HIWORD=code lParam=HWND");

    HWND chk = root ? CreateWindowExA(0, "BUTTON", "Check", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                                      10, 40, 90, 24, root, (HMENU)1002, 0, NULL) : 0;
    smoke_expect(&s, chk != 0, "AUTOCHECKBOX CreateWindowExA", "checkbox control exists");
    SendMessageA(chk, BM_SETCHECK, BST_UNCHECKED, 0);
    smoke_expect(&s, SendMessageA(chk, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "BM_GETCHECK unchecked", "BM_SETCHECK stores unchecked state");
    SendMessageA(chk, BM_CLICK, 0, 0);
    smoke_pump_parent_commands(root);
    smoke_expect(&s, SendMessageA(chk, BM_GETCHECK, 0, 0) == BST_CHECKED, "AUTOCHECKBOX BM_CLICK toggles", "click changes check state");

    HWND r1 = root ? CreateWindowExA(0, "BUTTON", "R1", WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_GROUP|BS_AUTORADIOBUTTON,
                                     10, 70, 70, 24, root, (HMENU)1003, 0, NULL) : 0;
    HWND r2 = root ? CreateWindowExA(0, "BUTTON", "R2", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
                                     90, 70, 70, 24, root, (HMENU)1004, 0, NULL) : 0;
    smoke_expect(&s, r1 && r2, "AUTORADIOBUTTON pair", "radio group controls exist");
    SendMessageA(r1, BM_CLICK, 0, 0);
    smoke_pump_parent_commands(root);
    smoke_expect(&s, SendMessageA(r1, BM_GETCHECK, 0, 0) == BST_CHECKED && SendMessageA(r2, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
                 "radio first checked", "first auto-radio owns group state");
    SendMessageA(r2, BM_CLICK, 0, 0);
    smoke_pump_parent_commands(root);
    smoke_expect(&s, SendMessageA(r1, BM_GETCHECK, 0, 0) == BST_UNCHECKED && SendMessageA(r2, BM_GETCHECK, 0, 0) == BST_CHECKED,
                 "radio exclusivity", "checking second clears first");

    HWND st = root ? CreateWindowExA(0, "STATIC", "Static", WS_CHILD|WS_VISIBLE|SS_CENTER,
                                     10, 105, 140, 22, root, (HMENU)1005, 0, NULL) : 0;
    char buf[160];
    memset(buf, 0, sizeof(buf));
    smoke_expect(&s, st != 0, "STATIC CreateWindowExA", "static control exists");
    smoke_expect(&s, st && SetWindowTextA(st, "Static Text") && GetWindowTextA(st, buf, sizeof(buf)) > 0 && strcmp(buf, "Static Text") == 0,
                 "STATIC SetWindowText/GetWindowText", "text storage follows normal window text contract");
    smoke_expect(&s, st && SendMessageA(st, WM_GETDLGCODE, 0, 0) == DLGC_STATIC,
                 "STATIC WM_GETDLGCODE", "static controls identify as non-focus static controls");

    HWND edit = root ? CreateWindowExA(0, "EDIT", "abc", WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
                                       10, 135, 180, 24, root, (HMENU)1006, 0, NULL) : 0;
    memset(buf, 0, sizeof(buf));
    smoke_expect(&s, edit != 0, "EDIT CreateWindowExA", "edit control exists");
    smoke_expect(&s, edit && GetWindowTextA(edit, buf, sizeof(buf)) == 3 && strcmp(buf, "abc") == 0,
                 "EDIT initial WM_GETTEXT", "initial lpWindowName is edit text");
    smoke_reset_control_notifications();
    SendMessageA(edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageA(edit, WM_CHAR, '!', 0);
    smoke_pump_parent_commands(root);
    memset(buf, 0, sizeof(buf));
    smoke_expect(&s, GetWindowTextA(edit, buf, sizeof(buf)) == 4 && strcmp(buf, "abc!") == 0,
                 "EDIT WM_CHAR append", "typed character mutates edit text");
    smoke_expect(&s, g_user32_command >= 1 && HIWORD(g_user32_last_command) == EN_CHANGE,
                 "EDIT EN_CHANGE", "edit posts change notification to parent");
    SendMessageA(edit, WM_KEYDOWN, KEY_BACKSPACE, 0);
    smoke_pump_parent_commands(root);
    memset(buf, 0, sizeof(buf));
    smoke_expect(&s, GetWindowTextA(edit, buf, sizeof(buf)) == 3 && strcmp(buf, "abc") == 0,
                 "EDIT Backspace", "keyboard edit path deletes previous char");
    SendMessageA(edit, EM_SETREADONLY, TRUE, 0);
    SendMessageA(edit, WM_CHAR, '?', 0);
    memset(buf, 0, sizeof(buf));
    smoke_expect(&s, GetWindowTextA(edit, buf, sizeof(buf)) == 3 && strcmp(buf, "abc") == 0,
                 "EDIT ES_READONLY blocks mutation", "readonly edit ignores typed changes");

    HWND lb = root ? CreateWindowExA(0, "LISTBOX", "", WS_CHILD|WS_VISIBLE|WS_TABSTOP|LBS_NOTIFY,
                                     10, 170, 160, 80, root, (HMENU)1007, 0, NULL) : 0;
    smoke_expect(&s, lb != 0, "LISTBOX CreateWindowExA", "listbox control exists");
    int li0 = (int)SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"Alpha");
    int li1 = (int)SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"Beta");
    smoke_expect(&s, li0 == 0 && li1 == 1 && SendMessageA(lb, LB_GETCOUNT, 0, 0) == 2,
                 "LISTBOX LB_ADDSTRING/LB_GETCOUNT", "items append in order");
    memset(buf, 0, sizeof(buf));
    smoke_expect(&s, SendMessageA(lb, LB_GETTEXT, 1, (LPARAM)buf) == 4 && strcmp(buf, "Beta") == 0,
                 "LISTBOX LB_GETTEXT", "item text roundtrip");
    SendMessageA(lb, LB_SETCURSEL, 1, 0);
    smoke_expect(&s, SendMessageA(lb, LB_GETCURSEL, 0, 0) == 1,
                 "LISTBOX LB_SETCURSEL/LB_GETCURSEL", "programmatic selection stored");
    smoke_reset_control_notifications();
    SendMessageA(lb, WM_KEYDOWN, KEY_UP, 0);
    smoke_pump_parent_commands(root);
    smoke_expect(&s, SendMessageA(lb, LB_GETCURSEL, 0, 0) == 0 && g_user32_command == 1 && HIWORD(g_user32_last_command) == LBN_SELCHANGE,
                 "LISTBOX keyboard LBN_SELCHANGE", "keyboard selection posts notify when LBS_NOTIFY set");

    HWND cb = root ? CreateWindowExA(0, "COMBOBOX", "", WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,
                                     200, 170, 170, 110, root, (HMENU)1008, 0, NULL) : 0;
    smoke_expect(&s, cb != 0, "COMBOBOX CreateWindowExA", "combobox control exists");
    int ci0 = (int)SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"One");
    int ci1 = (int)SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"Two");
    smoke_expect(&s, ci0 == 0 && ci1 == 1 && SendMessageA(cb, CB_GETCOUNT, 0, 0) == 2,
                 "COMBOBOX CB_ADDSTRING/CB_GETCOUNT", "combo items append in order");
    memset(buf, 0, sizeof(buf));
    smoke_expect(&s, SendMessageA(cb, CB_GETLBTEXT, 1, (LPARAM)buf) == 3 && strcmp(buf, "Two") == 0,
                 "COMBOBOX CB_GETLBTEXT", "combo item text roundtrip");
    SendMessageA(cb, CB_SETCURSEL, 0, 0);
    smoke_expect(&s, SendMessageA(cb, CB_GETCURSEL, 0, 0) == 0,
                 "COMBOBOX CB_SETCURSEL/CB_GETCURSEL", "programmatic combo selection stored");
    smoke_reset_control_notifications();
    SendMessageA(cb, WM_KEYDOWN, KEY_DOWN, 0);
    smoke_pump_parent_commands(root);
    smoke_expect(&s, SendMessageA(cb, CB_GETCURSEL, 0, 0) == 1 && g_user32_command == 1 && HIWORD(g_user32_last_command) == CBN_SELCHANGE,
                 "COMBOBOX keyboard CBN_SELCHANGE", "keyboard selection posts combo notify");
    SendMessageA(cb, CB_SHOWDROPDOWN, TRUE, 0);
    smoke_expect(&s, SendMessageA(cb, CB_GETDROPPEDSTATE, 0, 0) == TRUE,
                 "COMBOBOX CB_SHOWDROPDOWN(TRUE)", "dropdown state opens");
    SendMessageA(cb, CB_SHOWDROPDOWN, FALSE, 0);
    smoke_expect(&s, SendMessageA(cb, CB_GETDROPPEDSTATE, 0, 0) == FALSE,
                 "COMBOBOX CB_SHOWDROPDOWN(FALSE)", "dropdown state closes");

    HWND sb = root ? CreateWindowExA(0, "SCROLLBAR", "", WS_CHILD|WS_VISIBLE|WS_TABSTOP|SBS_VERT,
                                     390, 30, 20, 160, root, (HMENU)1009, 0, NULL) : 0;
    smoke_expect(&s, sb != 0, "SCROLLBAR CreateWindowExA", "scrollbar control exists");
    int minv = -1, maxv = -1;
    smoke_expect(&s, SendMessageA(sb, SBM_SETRANGE, 0, 10) == TRUE && SendMessageA(sb, SBM_GETRANGE, (WPARAM)&minv, (LPARAM)&maxv) == TRUE && minv == 0 && maxv == 10,
                 "SCROLLBAR SBM_SETRANGE/SBM_GETRANGE", "range roundtrip");
    smoke_expect(&s, SendMessageA(sb, SBM_SETPOS, 5, TRUE) == 0 && SendMessageA(sb, SBM_GETPOS, 0, 0) == 5,
                 "SCROLLBAR SBM_SETPOS/SBM_GETPOS", "position roundtrip clamps through control state");
    smoke_reset_control_notifications();
    SendMessageA(sb, WM_KEYDOWN, KEY_DOWN, 0);
    smoke_pump_parent_commands(root);
    smoke_expect(&s, SendMessageA(sb, SBM_GETPOS, 0, 0) == 6 && g_user32_vscroll == 1 && LOWORD(g_user32_last_scroll_wparam) == SB_LINEDOWN && (HWND)g_user32_last_scroll_lparam == sb,
                 "SCROLLBAR keyboard WM_VSCROLL", "down arrow posts vertical scroll notify");

    if (root) DestroyWindow(root);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_ipc_section(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "ipc_section";
    smoke_runtime_init(rt);

    char nameSection[128], nameEvent[128];
    smoke_unique_name(nameSection, sizeof(nameSection), "ipc.section");
    smoke_unique_name(nameEvent, sizeof(nameEvent), "ipc.event");

    HANDLE hMapA = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, nameSection);
    smoke_expect(&s, hMapA != 0, "CreateFileMappingA", nameSection);
    char* viewA = hMapA ? (char*)MapViewOfFile(hMapA, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 4096) : NULL;
    smoke_expect(&s, viewA != NULL, "MapViewOfFile writer", "READ|WRITE view");
    HANDLE hMapB = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, nameSection);
    smoke_expect(&s, hMapB != 0, "OpenFileMappingA second handle", nameSection);
    char* viewB = hMapB ? (char*)MapViewOfFile(hMapB, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 4096) : NULL;
    smoke_expect(&s, viewB != NULL, "MapViewOfFile reader", "second view of same section");

    HANDLE hEvent = CreateEventA(NULL, TRUE, FALSE, nameEvent);
    smoke_expect(&s, hEvent != 0, "CreateEventA signal", nameEvent);
    HANDLE hEventOpen = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, nameEvent);
    smoke_expect(&s, hEventOpen != 0, "OpenEventA signal", "second signal handle");
    if (viewA) strcpy(viewA, "ipc-section-before-signal");
    smoke_expect(&s, viewA && viewB && strcmp(viewB, "ipc-section-before-signal") == 0,
                 "section visible before signal", "shared backing visible through second mapping");
    smoke_expect(&s, hEvent && ResetEvent(hEvent), "ResetEvent signal", "roundtrip starts unsignaled");
    smoke_expect(&s, hEventOpen && WaitForSingleObject(hEventOpen, 0) == WAIT_TIMEOUT,
                 "Wait signal timeout", "reader handle observes reset state");
    if (viewA) strcpy(viewA, "ipc-section-after-signal");
    smoke_expect(&s, hEvent && SetEvent(hEvent), "SetEvent signal", "writer signals reader");
    smoke_expect(&s, hEventOpen && WaitForSingleObject(hEventOpen, 0) == WAIT_OBJECT_0,
                 "Wait signal object", "reader handle observes signal");
    smoke_expect(&s, viewB && strcmp(viewB, "ipc-section-after-signal") == 0,
                 "section payload after signal", "signal + shared payload roundtrip");
    smoke_expect(&s, viewB && FlushViewOfFile(viewB, 0), "FlushViewOfFile", "current implementation accepts flush probe");

    if (viewB) UnmapViewOfFile(viewB);
    if (viewA) UnmapViewOfFile(viewA);
    if (hEventOpen) CloseHandle(hEventOpen);
    if (hEvent) CloseHandle(hEvent);
    if (hMapB) CloseHandle(hMapB);
    if (hMapA) CloseHandle(hMapA);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}



static int g_lifecycle_order[64];
static int g_lifecycle_count = 0;
static int g_lifecycle_mode = 0;
static int g_lifecycle_close_seen = 0;
static int g_lifecycle_manual_close_seen = 0;
static int g_lifecycle_last_create_x = 0;
static int g_lifecycle_last_create_y = 0;
static int g_lifecycle_last_create_cx = 0;
static int g_lifecycle_last_create_cy = 0;
static LPARAM g_lifecycle_last_create_param = 0;

enum {
    MYOS_LIFE_MODE_NORMAL = 0,
    MYOS_LIFE_MODE_FAIL_NCCREATE = 1,
    MYOS_LIFE_MODE_FAIL_CREATE = 2,
    MYOS_LIFE_MODE_MANUAL_CLOSE = 3,
    MYOS_LIFE_MODE_DEF_CLOSE = 4
};

static void lifecycle_reset(int mode)
{
    memset(g_lifecycle_order, 0, sizeof(g_lifecycle_order));
    g_lifecycle_count = 0;
    g_lifecycle_mode = mode;
    g_lifecycle_close_seen = 0;
    g_lifecycle_manual_close_seen = 0;
    g_lifecycle_last_create_x = g_lifecycle_last_create_y = 0;
    g_lifecycle_last_create_cx = g_lifecycle_last_create_cy = 0;
    g_lifecycle_last_create_param = 0;
}

static void lifecycle_push(UINT msg)
{
    if (g_lifecycle_count < (int)(sizeof(g_lifecycle_order) / sizeof(g_lifecycle_order[0])))
        g_lifecycle_order[g_lifecycle_count++] = (int)msg;
}

static int lifecycle_seen_order3(UINT a, UINT b, UINT c)
{
    for (int i = 0; i + 2 < g_lifecycle_count; ++i) {
        if ((UINT)g_lifecycle_order[i] == a &&
            (UINT)g_lifecycle_order[i + 1] == b &&
            (UINT)g_lifecycle_order[i + 2] == c)
            return 1;
    }
    return 0;
}

static int lifecycle_seen_order2(UINT a, UINT b)
{
    for (int i = 0; i + 1 < g_lifecycle_count; ++i) {
        if ((UINT)g_lifecycle_order[i] == a &&
            (UINT)g_lifecycle_order[i + 1] == b)
            return 1;
    }
    return 0;
}

static int lifecycle_count_msg(UINT msg)
{
    int n = 0;
    for (int i = 0; i < g_lifecycle_count; ++i)
        if ((UINT)g_lifecycle_order[i] == msg) n++;
    return n;
}

static LRESULT CALLBACK smoke_lifecycle_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    switch (Msg) {
    case WM_NCCREATE: {
        lifecycle_push(Msg);
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lParam;
        if (cs) {
            g_lifecycle_last_create_x = cs->x;
            g_lifecycle_last_create_y = cs->y;
            g_lifecycle_last_create_cx = cs->cx;
            g_lifecycle_last_create_cy = cs->cy;
            g_lifecycle_last_create_param = (LPARAM)cs->lpCreateParams;
        }
        if (g_lifecycle_mode == MYOS_LIFE_MODE_FAIL_NCCREATE) return FALSE;
        return TRUE;
    }
    case WM_CREATE:
        lifecycle_push(Msg);
        if (g_lifecycle_mode == MYOS_LIFE_MODE_FAIL_CREATE) return -1;
        return 0;
    case WM_SHOWWINDOW:
    case WM_DESTROY:
    case WM_NCDESTROY:
        lifecycle_push(Msg);
        return 0;
    case WM_CLOSE:
        lifecycle_push(Msg);
        g_lifecycle_close_seen++;
        if (g_lifecycle_mode == MYOS_LIFE_MODE_MANUAL_CLOSE) {
            g_lifecycle_manual_close_seen++;
            return 0;
        }
        if (g_lifecycle_mode == MYOS_LIFE_MODE_DEF_CLOSE)
            return DefWindowProcA(hWnd, Msg, wParam, lParam);
        return 0;
    case WM_USER + 210:
        lifecycle_push(Msg);
        return 210;
    case WM_USER + 211:
        lifecycle_push(Msg);
        return 211;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static HWND smoke_create_lifecycle_window(SmokeRuntime* rt, SmokeContext* s, const char* className,
                                          DWORD style, LPVOID lpParam)
{
    smoke_runtime_init(rt);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_lifecycle_wndproc;
    wc.lpszClassName = className;
    ATOM atom = RegisterClassExA(&wc);
    smoke_expect(s, atom != 0, "RegisterClassExA lifecycle", className);
    HWND hwnd = atom ? CreateWindowExA(0, className, "Lifecycle", style,
                                      101, 102, 203, 204, 0, 0, 0, lpParam) : 0;
    return hwnd;
}

static int smoke_lifecycle(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "lifecycle";
    smoke_runtime_init(rt);

    lifecycle_reset(MYOS_LIFE_MODE_NORMAL);
    HWND hwnd = smoke_create_lifecycle_window(rt, &s, "myOS.v129.SmokeLifecycle", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                              (LPVOID)(uintptr_t)0x1234u);
    smoke_expect(&s, hwnd != 0 && IsWindow(hwnd), "CreateWindowExA visible", "HWND returned after create lifecycle");
    smoke_expect(&s, lifecycle_seen_order3(WM_NCCREATE, WM_CREATE, WM_SHOWWINDOW),
                 "Create lifecycle order", "WM_NCCREATE -> WM_CREATE -> WM_SHOWWINDOW");
    smoke_expect(&s, g_lifecycle_last_create_x == 101 && g_lifecycle_last_create_y == 102 &&
                     g_lifecycle_last_create_cx == 203 && g_lifecycle_last_create_cy == 204 &&
                     g_lifecycle_last_create_param == (LPARAM)(uintptr_t)0x1234u,
                 "CREATESTRUCTA contents", "coords, size and lpCreateParams preserved");
    smoke_expect(&s, lifecycle_count_msg(WM_NCCREATE) == 1 && lifecycle_count_msg(WM_CREATE) == 1,
                 "Create messages once", "NCCREATE/CREATE are not replayed");

    smoke_expect(&s, hwnd && SendMessageA(hwnd, WM_USER + 210, 0, 0) == 210,
                 "SendMessageA return value", "synchronous WndProc result is returned");

    smoke_expect(&s, hwnd && PostMessageA(hwnd, WM_USER + 210, 1, 0), "PostMessage FIFO #1", "normal priority message 210");
    smoke_expect(&s, hwnd && PostMessageA(hwnd, WM_USER + 211, 2, 0), "PostMessage FIFO #2", "normal priority message 211");
    MSG msg1, msg2;
    memset(&msg1, 0, sizeof(msg1));
    memset(&msg2, 0, sizeof(msg2));
    BOOL got1 = PeekMessageA(&msg1, hwnd, WM_USER + 210, WM_USER + 211, PM_NOREMOVE);
    smoke_expect(&s, got1 && msg1.message == WM_USER + 210 && msg1.wParam == 1,
                 "PeekMessageA(PM_NOREMOVE)", "first FIFO message observed but not removed");
    memset(&msg2, 0, sizeof(msg2));
    BOOL got2 = PeekMessageA(&msg2, hwnd, WM_USER + 210, WM_USER + 211, PM_REMOVE);
    smoke_expect(&s, got2 && msg2.message == msg1.message && msg2.wParam == msg1.wParam,
                 "PeekMessageA(PM_REMOVE) after NOREMOVE", "same first message is removed");
    memset(&msg2, 0, sizeof(msg2));
    BOOL got3 = PeekMessageA(&msg2, hwnd, WM_USER + 210, WM_USER + 211, PM_REMOVE);
    smoke_expect(&s, got3 && msg2.message == WM_USER + 211 && msg2.wParam == 2,
                 "PostMessage FIFO order", "second posted message follows first");
    if (got2) DispatchMessageA(&msg1);
    if (got3) DispatchMessageA(&msg2);
    smoke_expect(&s, lifecycle_count_msg(WM_USER + 210) >= 2 && lifecycle_count_msg(WM_USER + 211) == 1,
                 "Dispatch queued FIFO messages", "queued public MSG values dispatch through WndProc");

    smoke_expect(&s, hwnd && DestroyWindow(hwnd), "DestroyWindow lifecycle", "explicit destroy succeeds");
    smoke_expect(&s, lifecycle_seen_order2(WM_DESTROY, WM_NCDESTROY),
                 "Destroy lifecycle order", "WM_DESTROY before WM_NCDESTROY");
    smoke_expect(&s, hwnd && !IsWindow(hwnd), "IsWindow after lifecycle destroy", "HWND invalid after NCDESTROY/unlink");

    lifecycle_reset(MYOS_LIFE_MODE_FAIL_NCCREATE);
    HWND hFailNc = smoke_create_lifecycle_window(rt, &s, "myOS.v129.SmokeFailNcCreate", WS_OVERLAPPEDWINDOW, NULL);
    smoke_expect(&s, hFailNc == 0, "WM_NCCREATE aborts CreateWindowExA", "FALSE from WM_NCCREATE rejects HWND");
    smoke_expect(&s, lifecycle_count_msg(WM_NCCREATE) == 1 && !IsWindow(hFailNc),
                 "WM_NCCREATE abort cleanup", "failed HWND is not externally valid");

    lifecycle_reset(MYOS_LIFE_MODE_FAIL_CREATE);
    HWND hFailCreate = smoke_create_lifecycle_window(rt, &s, "myOS.v129.SmokeFailCreate", WS_OVERLAPPEDWINDOW, NULL);
    smoke_expect(&s, hFailCreate == 0, "WM_CREATE aborts CreateWindowExA", "-1 from WM_CREATE rejects HWND");
    smoke_expect(&s, lifecycle_seen_order2(WM_CREATE, WM_DESTROY) && lifecycle_seen_order2(WM_DESTROY, WM_NCDESTROY),
                 "WM_CREATE abort teardown", "failed create destroys partially-created HWND");

    lifecycle_reset(MYOS_LIFE_MODE_MANUAL_CLOSE);
    HWND hManualClose = smoke_create_lifecycle_window(rt, &s, "myOS.v129.SmokeManualClose", WS_OVERLAPPEDWINDOW, NULL);
    smoke_expect(&s, hManualClose != 0 && IsWindow(hManualClose), "Create manual-close window", "custom WM_CLOSE handler");
    smoke_expect(&s, hManualClose && SendMessageA(hManualClose, WM_CLOSE, 0, 0) == 0 && g_lifecycle_close_seen == 1,
                 "WM_CLOSE delivered to WndProc", "application gets first chance to decide");
    smoke_expect(&s, hManualClose && IsWindow(hManualClose), "WM_CLOSE not automatic", "custom handler did not call DefWindowProc/DestroyWindow");
    if (hManualClose && IsWindow(hManualClose)) DestroyWindow(hManualClose);

    lifecycle_reset(MYOS_LIFE_MODE_DEF_CLOSE);
    HWND hDefClose = smoke_create_lifecycle_window(rt, &s, "myOS.v129.SmokeDefClose", WS_OVERLAPPEDWINDOW, NULL);
    smoke_expect(&s, hDefClose != 0 && IsWindow(hDefClose), "Create def-close window", "DefWindowProc WM_CLOSE handler");
    smoke_expect(&s, hDefClose && SendMessageA(hDefClose, WM_CLOSE, 0, 0) == 0 && g_lifecycle_close_seen == 1,
                 "DefWindowProc(WM_CLOSE)", "default close path invoked");
    smoke_expect(&s, hDefClose && !IsWindow(hDefClose) && lifecycle_seen_order2(WM_DESTROY, WM_NCDESTROY),
                 "DefWindowProc closes window", "WM_CLOSE default destroys HWND");

    lifecycle_reset(MYOS_LIFE_MODE_NORMAL);
    HWND hQuitCarrier = smoke_create_lifecycle_window(rt, &s, "myOS.v129.SmokeQuitCarrier", WS_OVERLAPPEDWINDOW, NULL);
    smoke_expect(&s, hQuitCarrier != 0 && IsWindow(hQuitCarrier), "Create quit-carrier window", "ensures current thread queue exists");
    PostQuitMessage(123);
    MSG quit;
    memset(&quit, 0, sizeof(quit));
    BOOL gm = GetMessageA(&quit, 0, 0, 0);
    smoke_expect(&s, gm == FALSE && quit.message == WM_QUIT && quit.wParam == 123,
                 "GetMessageA WM_QUIT", "returns 0 and preserves PostQuitMessage exit code");
    if (hQuitCarrier && IsWindow(hQuitCarrier)) DestroyWindow(hQuitCarrier);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static int smoke_access_rights(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "access";
    smoke_runtime_init(rt);

    char eventName[128], mutexName[128], semName[128], sectionName[128];
    char privEventName[128], privMutexName[128], privSemName[128], privSectionName[128];
    smoke_unique_name(eventName, sizeof(eventName), "v128.event");
    smoke_unique_name(mutexName, sizeof(mutexName), "v128.mutex");
    smoke_unique_name(semName, sizeof(semName), "v128.sem");
    smoke_unique_name(sectionName, sizeof(sectionName), "v128.section");
    smoke_unique_name(privEventName, sizeof(privEventName), "private.event");
    smoke_unique_name(privMutexName, sizeof(privMutexName), "private.mutex");
    smoke_unique_name(privSemName, sizeof(privSemName), "private.sem");
    smoke_unique_name(privSectionName, sizeof(privSectionName), "private.section");

    HANDLE hEvent = CreateEventA(NULL, TRUE, FALSE, eventName);
    smoke_expect(&s, hEvent != 0, "CreateEventA access target", eventName);
    HANDLE hEventSync = OpenEventA(SYNCHRONIZE, FALSE, eventName);
    smoke_expect(&s, hEventSync != 0, "OpenEventA(SYNCHRONIZE)", "wait-only event handle");
    SetLastError(0x12345678u);
    smoke_expect(&s, hEventSync && SetEvent(hEventSync) == FALSE,
                 "SetEvent denied on SYNCHRONIZE handle", "modify right is required");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "SetEvent(SYNCHRONIZE handle) LastError");
    HANDLE hEventMod = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName);
    smoke_expect(&s, hEventMod != 0, "OpenEventA(EVENT_MODIFY_STATE)", "signal-only event handle");
    smoke_expect(&s, hEventMod && SetEvent(hEventMod), "SetEvent allowed on modify handle", "EVENT_MODIFY_STATE grants signal");
    SetLastError(0x12345678u);
    smoke_expect(&s, hEventMod && WaitForSingleObject(hEventMod, 0) == WAIT_FAILED,
                 "WaitForSingleObject denied on modify-only event", "SYNCHRONIZE right is required");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "WaitForSingleObject(modify-only event) LastError");
    smoke_expect(&s, hEventSync && WaitForSingleObject(hEventSync, 0) == WAIT_OBJECT_0,
                 "WaitForSingleObject allowed on sync event", "signaled by modify handle");
    smoke_expect(&s, hEventMod && ResetEvent(hEventMod), "ResetEvent allowed on modify handle", "manual-reset event reset");

    HANDLE hDupAll = 0;
    SetLastError(0x12345678u);
    smoke_expect(&s, hEventSync && DuplicateHandle(GetCurrentProcess(), hEventSync, GetCurrentProcess(), &hDupAll,
                                                   EVENT_ALL_ACCESS, FALSE, 0) == FALSE,
                 "DuplicateHandle cannot amplify event rights", "SYNCHRONIZE -> EVENT_ALL_ACCESS denied");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "DuplicateHandle amplify LastError");
    HANDLE hDupSync = 0;
    smoke_expect(&s, hEventSync && DuplicateHandle(GetCurrentProcess(), hEventSync, GetCurrentProcess(), &hDupSync,
                                                   0, FALSE, DUPLICATE_SAME_ACCESS) && hDupSync != 0,
                 "DuplicateHandle(DUPLICATE_SAME_ACCESS) preserves rights", "sync-only duplicate created");
    SetLastError(0x12345678u);
    smoke_expect(&s, hDupSync && SetEvent(hDupSync) == FALSE,
                 "SetEvent denied on sync-only duplicate", "duplicated access mask preserved");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "SetEvent(sync duplicate) LastError");

    HANDLE hMutex = CreateMutexA(NULL, FALSE, mutexName);
    smoke_expect(&s, hMutex != 0, "CreateMutexA access target", mutexName);
    HANDLE hMutexSync = OpenMutexA(SYNCHRONIZE, FALSE, mutexName);
    smoke_expect(&s, hMutexSync != 0, "OpenMutexA(SYNCHRONIZE)", "wait-only mutex handle");
    smoke_expect(&s, hMutexSync && WaitForSingleObject(hMutexSync, 0) == WAIT_OBJECT_0,
                 "WaitForSingleObject mutex sync handle", "SYNCHRONIZE can acquire");
    SetLastError(0x12345678u);
    smoke_expect(&s, hMutexSync && ReleaseMutex(hMutexSync) == FALSE,
                 "ReleaseMutex denied on SYNCHRONIZE handle", "MUTEX_MODIFY_STATE required");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "ReleaseMutex(sync handle) LastError");
    smoke_expect(&s, hMutex && ReleaseMutex(hMutex), "ReleaseMutex owner/full handle", "cleanup after sync acquisition");

    HANDLE hSem = CreateSemaphoreA(NULL, 1, 3, semName);
    smoke_expect(&s, hSem != 0, "CreateSemaphoreA access target", semName);
    HANDLE hSemSync = OpenSemaphoreA(SYNCHRONIZE, FALSE, semName);
    smoke_expect(&s, hSemSync != 0, "OpenSemaphoreA(SYNCHRONIZE)", "wait-only semaphore handle");
    smoke_expect(&s, hSemSync && WaitForSingleObject(hSemSync, 0) == WAIT_OBJECT_0,
                 "WaitForSingleObject semaphore sync handle", "consumes one count");
    LONG prev = -1;
    SetLastError(0x12345678u);
    smoke_expect(&s, hSemSync && ReleaseSemaphore(hSemSync, 1, &prev) == FALSE,
                 "ReleaseSemaphore denied on SYNCHRONIZE handle", "SEMAPHORE_MODIFY_STATE required");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "ReleaseSemaphore(sync handle) LastError");
    smoke_expect(&s, hSem && ReleaseSemaphore(hSem, 1, &prev) && prev == 0,
                 "ReleaseSemaphore full handle cleanup", "count restored from 0");
    HANDLE hSemMod = OpenSemaphoreA(SEMAPHORE_MODIFY_STATE, FALSE, semName);
    smoke_expect(&s, hSemMod != 0, "OpenSemaphoreA(SEMAPHORE_MODIFY_STATE)", "release-only semaphore handle");
    smoke_expect(&s, hSemMod && ReleaseSemaphore(hSemMod, 1, &prev) && prev == 1,
                 "ReleaseSemaphore modify handle", "modify right grants release");
    SetLastError(0x12345678u);
    smoke_expect(&s, hSemMod && WaitForSingleObject(hSemMod, 0) == WAIT_FAILED,
                 "WaitForSingleObject denied on semaphore modify-only handle", "SYNCHRONIZE right is required");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "WaitForSingleObject(semaphore modify-only) LastError");

    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, sectionName);
    smoke_expect(&s, hMap != 0, "CreateFileMappingA access target", sectionName);
    char* ownerView = hMap ? (char*)MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 4096) : NULL;
    smoke_expect(&s, ownerView != NULL, "MapViewOfFile owner READ|WRITE", "full-access section handle");
    if (ownerView) strcpy(ownerView, "v130 owner section");
    HANDLE hMapRead = OpenFileMappingA(FILE_MAP_READ, FALSE, sectionName);
    smoke_expect(&s, hMapRead != 0, "OpenFileMappingA(FILE_MAP_READ)", "read-only section handle");
    char* readView = hMapRead ? (char*)MapViewOfFile(hMapRead, FILE_MAP_READ, 0, 0, 4096) : NULL;
    smoke_expect(&s, readView && strcmp(readView, "v130 owner section") == 0,
                 "MapViewOfFile read-only handle READ", "read mapping sees owner data");
    SetLastError(0x12345678u);
    smoke_expect(&s, hMapRead && MapViewOfFile(hMapRead, FILE_MAP_WRITE, 0, 0, 4096) == NULL,
                 "MapViewOfFile WRITE denied on read-only handle", "FILE_MAP_WRITE is required");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "MapViewOfFile(read handle/write view) LastError");
    HANDLE hMapWrite = OpenFileMappingA(FILE_MAP_WRITE, FALSE, sectionName);
    smoke_expect(&s, hMapWrite != 0, "OpenFileMappingA(FILE_MAP_WRITE)", "write-only section handle");
    char* writeView = hMapWrite ? (char*)MapViewOfFile(hMapWrite, FILE_MAP_WRITE, 0, 0, 4096) : NULL;
    smoke_expect(&s, writeView != NULL, "MapViewOfFile write handle WRITE", "write mapping allowed");
    if (writeView) strcpy(writeView, "v130 write handle ok");
    smoke_expect(&s, ownerView && strcmp(ownerView, "v130 write handle ok") == 0,
                 "section write-through", "write-only mapping updates backing storage");

    HANDLE hPrivEvent = CreateEventA(NULL, TRUE, FALSE, privEventName);
    HANDLE hPrivMutex = CreateMutexA(NULL, FALSE, privMutexName);
    HANDLE hPrivSem = CreateSemaphoreA(NULL, 1, 2, privSemName);
    HANDLE hPrivMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, privSectionName);
    smoke_expect(&s, hPrivEvent && hPrivMutex && hPrivSem && hPrivMap,
                 "create private named objects", "owner-only SD-lite via .private. name");

    Capability owner = rt->cap;
    Capability other = cap_create(205, "v130-nonowner", CAP_IPC | CAP_SECTION_MAP);
    cap_add_target(&other, 0);
    MyWinBindRuntime(&rt->mgr, &other);

    HANDLE hOtherPublic = OpenEventA(SYNCHRONIZE, FALSE, eventName);
    smoke_expect(&s, hOtherPublic != 0, "non-owner opens public event read", "Global object grants public read");
    if (hOtherPublic) CloseHandle(hOtherPublic);

    SetLastError(0x12345678u);
    smoke_expect(&s, OpenEventA(SYNCHRONIZE, FALSE, privEventName) == 0,
                 "non-owner denied private event", "owner-only event security");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "OpenEventA(private) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, OpenMutexA(SYNCHRONIZE, FALSE, privMutexName) == 0,
                 "non-owner denied private mutex", "owner-only mutex security");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "OpenMutexA(private) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, OpenSemaphoreA(SYNCHRONIZE, FALSE, privSemName) == 0,
                 "non-owner denied private semaphore", "owner-only semaphore security");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "OpenSemaphoreA(private) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, OpenFileMappingA(FILE_MAP_READ, FALSE, privSectionName) == 0,
                 "non-owner denied private section", "owner-only section security");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "OpenFileMappingA(private) LastError");

    MyWinBindRuntime(&rt->mgr, &owner);

    if (writeView) UnmapViewOfFile(writeView);
    if (readView) UnmapViewOfFile(readView);
    if (ownerView) UnmapViewOfFile(ownerView);
    if (hPrivMap) CloseHandle(hPrivMap);
    if (hPrivSem) CloseHandle(hPrivSem);
    if (hPrivMutex) CloseHandle(hPrivMutex);
    if (hPrivEvent) CloseHandle(hPrivEvent);
    if (hMapWrite) CloseHandle(hMapWrite);
    if (hMapRead) CloseHandle(hMapRead);
    if (hMap) CloseHandle(hMap);
    if (hSemMod) CloseHandle(hSemMod);
    if (hSemSync) CloseHandle(hSemSync);
    if (hSem) CloseHandle(hSem);
    if (hMutexSync) CloseHandle(hMutexSync);
    if (hMutex) CloseHandle(hMutex);
    if (hDupSync) CloseHandle(hDupSync);
    if (hEventMod) CloseHandle(hEventMod);
    if (hEventSync) CloseHandle(hEventSync);
    if (hEvent) CloseHandle(hEvent);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_handle_invalid(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "handle_invalid";
    smoke_runtime_init(rt);

    SetLastError(0x12345678u);
    smoke_expect(&s, CloseHandle((HANDLE)0) == FALSE, "CloseHandle(NULL)", "must fail");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "CloseHandle(NULL) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, CloseHandle(INVALID_HANDLE_VALUE) == FALSE, "CloseHandle(INVALID_HANDLE_VALUE)", "must fail");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "CloseHandle(INVALID_HANDLE_VALUE) LastError");

    char name[128];
    smoke_unique_name(name, sizeof(name), "doubleclose");
    HANDLE h = CreateEventA(NULL, TRUE, FALSE, name);
    smoke_expect(&s, h != 0, "CreateEventA for double close", name);
    smoke_expect(&s, h && CloseHandle(h), "CloseHandle(valid)", "first close succeeds");
    SetLastError(0x12345678u);
    smoke_expect(&s, h && CloseHandle(h) == FALSE, "CloseHandle(double)", "second close fails");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "CloseHandle(double) LastError");

    HANDLE dup = (HANDLE)0xfeed1234u;
    SetLastError(0x12345678u);
    smoke_expect(&s, DuplicateHandle(GetCurrentProcess(), (HANDLE)0, GetCurrentProcess(), &dup, 0, FALSE, 0) == FALSE && dup == 0,
                 "DuplicateHandle(NULL source)", "invalid source handle rejected");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "DuplicateHandle(NULL source) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, DuplicateHandle(GetCurrentProcess(), (HANDLE)0x00abcdefu, GetCurrentProcess(), &dup, 0, FALSE, 0) == FALSE,
                 "DuplicateHandle(random source)", "unknown source handle rejected");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "DuplicateHandle(random source) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), NULL, 0, FALSE, 0) == FALSE,
                 "DuplicateHandle(NULL target pointer)", "target pointer required");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "DuplicateHandle(NULL target pointer) LastError");

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_wait_invalid(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "wait_invalid";
    smoke_runtime_init(rt);

    SetLastError(0x12345678u);
    smoke_expect(&s, WaitForSingleObject((HANDLE)0, 0) == WAIT_FAILED, "WaitForSingleObject(NULL)", "must fail");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "WaitForSingleObject(NULL) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, WaitForSingleObject(INVALID_HANDLE_VALUE, 0) == WAIT_FAILED, "WaitForSingleObject(INVALID_HANDLE_VALUE)", "must fail");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "WaitForSingleObject(INVALID_HANDLE_VALUE) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, WaitForMultipleObjects(0, NULL, FALSE, 0) == WAIT_FAILED, "WaitForMultipleObjects(count=0)", "invalid parameter");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "WaitForMultipleObjects(count=0) LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, WaitForMultipleObjects(1, NULL, FALSE, 0) == WAIT_FAILED, "WaitForMultipleObjects(NULL handles)", "invalid parameter");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "WaitForMultipleObjects(NULL handles) LastError");
    HANDLE bad[2] = { (HANDLE)0, INVALID_HANDLE_VALUE };
    SetLastError(0x12345678u);
    smoke_expect(&s, WaitForMultipleObjects(2, bad, FALSE, 0) == WAIT_FAILED, "WaitForMultipleObjects(invalid handles)", "invalid handle in array");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "WaitForMultipleObjects(invalid handles) LastError");

    char name[128];
    smoke_unique_name(name, sizeof(name), "wait-valid");
    HANDLE h = CreateEventA(NULL, TRUE, TRUE, name);
    HANDLE arr[1] = { h };
    smoke_expect(&s, h != 0, "CreateEventA(valid wait)", name);
    SetLastError(0x12345678u);
    smoke_expect(&s, h && WaitForMultipleObjects(1, arr, FALSE, 0) == WAIT_OBJECT_0,
                 "WaitForMultipleObjects(valid single)", "valid path still works");
    if (h) CloseHandle(h);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static unsigned long long smoke_perf_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)(ts.tv_nsec / 1000ull);
}

typedef struct SmokeHandleThreadBench {
    HWNDManager* mgr;
    Capability cap;
    HANDLE source;
    int iterations;
    HANDLE* handles;
    pthread_barrier_t* start_barrier;
    int ok;
    int made;
    DWORD count_before;
    DWORD count_after;
    unsigned long long dup_us;
    unsigned long long close_us;
} SmokeHandleThreadBench;

static void* smoke_handle_thread_fanout(void* arg)
{
    SmokeHandleThreadBench* b = (SmokeHandleThreadBench*)arg;
    if (!b || !b->handles) return NULL;
    MyWinBindRuntime(b->mgr, &b->cap);
    b->ok = 1;
    if (b->start_barrier) pthread_barrier_wait(b->start_barrier);
    unsigned long long a = smoke_perf_now_us();
    for (int i = 0; i < b->iterations; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), b->source, GetCurrentProcess(), &b->handles[i], 0, FALSE, DUPLICATE_SAME_ACCESS) || !b->handles[i]) {
            b->ok = 0;
            break;
        }
        b->made++;
    }
    unsigned long long z = smoke_perf_now_us();
    b->dup_us = z - a;
    a = smoke_perf_now_us();
    for (int i = 0; i < b->made; ++i) {
        if (b->handles[i] && !CloseHandle(b->handles[i])) b->ok = 0;
        b->handles[i] = 0;
    }
    z = smoke_perf_now_us();
    b->close_us = z - a;
    return NULL;
}

static void* smoke_handle_thread_churn(void* arg)
{
    SmokeHandleThreadBench* b = (SmokeHandleThreadBench*)arg;
    if (!b) return NULL;
    MyWinBindRuntime(b->mgr, &b->cap);
    b->ok = 1;
    if (b->start_barrier) pthread_barrier_wait(b->start_barrier);
    unsigned long long a = smoke_perf_now_us();
    for (int i = 0; i < b->iterations; ++i) {
        HANDLE h = 0;
        if (!DuplicateHandle(GetCurrentProcess(), b->source, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS) || !h) {
            b->ok = 0;
            break;
        }
        b->made++;
        if (!CloseHandle(h)) {
            b->ok = 0;
            break;
        }
    }
    unsigned long long z = smoke_perf_now_us();
    b->dup_us = z - a;
    b->close_us = 0;
    return NULL;
}

static void* smoke_handle_thread_process_churn(void* arg)
{
    SmokeHandleThreadBench* b = (SmokeHandleThreadBench*)arg;
    if (!b) return NULL;
    MyWinBindRuntime(b->mgr, &b->cap);
    b->ok = 1;
    b->count_before = MyGetHandleCount(b->cap.id);
    char name[128];
    snprintf(name, sizeof(name), "Global\\myos.v212.mpchurn.%lu.%lu", (unsigned long)getpid(), (unsigned long)b->cap.id);
    HANDLE src = CreateEventA(NULL, TRUE, FALSE, name);
    if (!src) { b->ok = 0; b->count_after = MyGetHandleCount(b->cap.id); return NULL; }
    if (b->start_barrier) pthread_barrier_wait(b->start_barrier);
    unsigned long long a = smoke_perf_now_us();
    for (int i = 0; i < b->iterations; ++i) {
        HANDLE h = 0;
        if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS) || !h) {
            b->ok = 0;
            break;
        }
        b->made++;
        if (!CloseHandle(h)) {
            b->ok = 0;
            break;
        }
    }
    unsigned long long z = smoke_perf_now_us();
    b->dup_us = z - a;
    if (!CloseHandle(src)) b->ok = 0;
    b->count_after = MyGetHandleCount(b->cap.id);
    return NULL;
}

static int smoke_strict_handles(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "strict_handles";
    smoke_runtime_init(rt);

    BOOL oldStrict = MyWinSetStrictKernelHandles(TRUE);
    smoke_expect(&s, MyWinGetStrictKernelHandles() == TRUE,
                 "MyWinSetStrictKernelHandles(TRUE)", "public KERNEL32 raw-handle fallback disabled");

    char eventName[128], semName[128], mutexName[128], timerName[128], sectionName[128];
    smoke_unique_name(eventName, sizeof(eventName), "v147.strict.event");
    smoke_unique_name(semName, sizeof(semName), "v147.strict.sem");
    smoke_unique_name(mutexName, sizeof(mutexName), "v147.strict.mutex");
    smoke_unique_name(timerName, sizeof(timerName), "v147.strict.timer");
    smoke_unique_name(sectionName, sizeof(sectionName), "v147.strict.section");

    HANDLE hEvent = CreateEventA(NULL, TRUE, FALSE, eventName);
    MyHandleInfo eventInfo;
    memset(&eventInfo, 0, sizeof(eventInfo));
    smoke_expect(&s, hEvent && MyGetHandleInfo(hEvent, &eventInfo) && eventInfo.object_handle != 0,
                 "strict_handles event raw object discovered", "diagnostic handle table exposes backing object only to tests");
    HANDLE rawEvent = eventInfo.object_handle;

    SetLastError(0x12345678u);
    smoke_expect(&s, rawEvent && WaitForSingleObject(rawEvent, 0) == WAIT_FAILED,
                 "raw event rejected by WaitForSingleObject", "public wait requires process handle-table handle");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "WaitForSingleObject(raw event) LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, rawEvent && SetEvent(rawEvent) == FALSE,
                 "raw event rejected by SetEvent", "EVENT_MODIFY_STATE is not granted by raw object value");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "SetEvent(raw event) LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, rawEvent && ResetEvent(rawEvent) == FALSE,
                 "raw event rejected by ResetEvent", "public reset path is strict-table only");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "ResetEvent(raw event) LastError");

    HANDLE rawDup = (HANDLE)0xfeed1234u;
    SetLastError(0x12345678u);
    smoke_expect(&s, rawEvent && DuplicateHandle(GetCurrentProcess(), rawEvent, GetCurrentProcess(), &rawDup, 0, FALSE, DUPLICATE_SAME_ACCESS) == FALSE && rawDup == 0,
                 "raw event rejected by DuplicateHandle", "source must exist in source process handle table");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "DuplicateHandle(raw event) LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, rawEvent && CloseHandle(rawEvent) == FALSE,
                 "raw event rejected by CloseHandle", "CloseHandle cannot destroy object-manager handles directly");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "CloseHandle(raw event) LastError");

    smoke_expect(&s, hEvent && SetEvent(hEvent), "table event SetEvent still works", "valid process handle survives strict mode");
    smoke_expect(&s, hEvent && WaitForSingleObject(hEvent, 0) == WAIT_OBJECT_0,
                 "table event WaitForSingleObject still works", "strict mode does not break normal handles");

    HANDLE hDup = 0;
    smoke_expect(&s, hEvent && DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS) && hDup != 0,
                 "table event DuplicateHandle still works", "strict mode preserves legal duplication");
    smoke_expect(&s, hDup && CloseHandle(hDup), "CloseHandle(duplicated table handle)", "first close succeeds");
    SetLastError(0x12345678u);
    smoke_expect(&s, hDup && WaitForSingleObject(hDup, 0) == WAIT_FAILED,
                 "closed duplicated handle rejected", "closed slot no longer resolves");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "WaitForSingleObject(closed duplicate) LastError");

    smoke_expect(&s, GetCurrentProcess() == (HANDLE)0xffffffffu && GetCurrentThread() == (HANDLE)0xfffffffeu,
                 "v206 pseudo handle constants", "current process/thread return Win32 pseudo handle values");

    HANDLE hPseudoProc = 0;
    MyHandleInfo pseudoProcInfo;
    memset(&pseudoProcInfo, 0, sizeof(pseudoProcInfo));
    smoke_expect(&s, DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &hPseudoProc, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
                     hPseudoProc && hPseudoProc != GetCurrentProcess() &&
                     MyGetHandleInfo(hPseudoProc, &pseudoProcInfo) && pseudoProcInfo.object_type == _OBJECT_TYPE_PROCESS,
                 "v206 DuplicateHandle current-process pseudo", "pseudo process handle materializes into a real process handle table entry");

    HANDLE hPseudoThread = 0;
    MyHandleInfo pseudoThreadInfo;
    memset(&pseudoThreadInfo, 0, sizeof(pseudoThreadInfo));
    smoke_expect(&s, DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &hPseudoThread, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
                     hPseudoThread && hPseudoThread != GetCurrentThread() &&
                     MyGetHandleInfo(hPseudoThread, &pseudoThreadInfo) && pseudoThreadInfo.object_type == _OBJECT_TYPE_THREAD,
                 "v206 DuplicateHandle current-thread pseudo", "pseudo thread handle materializes into a real thread handle table entry");

    smoke_expect(&s, CloseHandle(GetCurrentThread()),
                 "v206 CloseHandle current-thread pseudo", "closing the thread pseudo handle is a successful no-op and does not consume table refs");
    SetLastError(0x12345678u);
    smoke_expect(&s, CloseHandle(GetCurrentProcess()) == FALSE,
                 "v206 CloseHandle current-process pseudo bitpattern", "current-process pseudo equals INVALID_HANDLE_VALUE, so CloseHandle keeps invalid-handle semantics");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "CloseHandle(current-process pseudo) LastError");

    DWORD hflags = 0;
    smoke_expect(&s, hEvent && GetHandleInformation(hEvent, &hflags) && hflags == 0,
                 "v207 GetHandleInformation initial", "fresh non-inheritable event handle has no public handle flags");
    smoke_expect(&s, hEvent && SetHandleInformation(hEvent, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT),
                 "v207 SetHandleInformation inherit on", "inherit flag is mutable per handle");
    hflags = 0;
    smoke_expect(&s, hEvent && GetHandleInformation(hEvent, &hflags) && (hflags & HANDLE_FLAG_INHERIT),
                 "v207 GetHandleInformation inherit on", "HANDLE_FLAG_INHERIT is reported by the public API");
    MyHandleInfo inheritDiag;
    memset(&inheritDiag, 0, sizeof(inheritDiag));
    smoke_expect(&s, hEvent && MyGetHandleInfo(hEvent, &inheritDiag) && (inheritDiag.flags & MYWIN_HANDLE_FLAG_INHERIT),
                 "v207 diagnostic inherit flag", "diagnostic handle table mirrors the public inherit flag");

    HANDLE hNonInheritDup = 0;
    smoke_expect(&s, hEvent && DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &hNonInheritDup, 0, FALSE, DUPLICATE_SAME_ACCESS),
                 "v207 DuplicateHandle bInherit FALSE", "new duplicate can explicitly clear inherit regardless of source flag");
    hflags = 0;
    smoke_expect(&s, hNonInheritDup && GetHandleInformation(hNonInheritDup, &hflags) && !(hflags & HANDLE_FLAG_INHERIT),
                 "v207 duplicate inherit cleared", "bInheritHandle controls the new handle's inherit bit");
    if (hNonInheritDup) CloseHandle(hNonInheritDup);

    HANDLE hInheritDup = 0;
    smoke_expect(&s, hEvent && DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &hInheritDup, 0, TRUE, DUPLICATE_SAME_ACCESS),
                 "v207 DuplicateHandle bInherit TRUE", "new duplicate can explicitly set inherit");
    hflags = 0;
    smoke_expect(&s, hInheritDup && GetHandleInformation(hInheritDup, &hflags) && (hflags & HANDLE_FLAG_INHERIT),
                 "v207 duplicate inherit set", "bInheritHandle TRUE sets HANDLE_FLAG_INHERIT on the new handle");
    if (hInheritDup) CloseHandle(hInheritDup);

    smoke_expect(&s, hEvent && SetHandleInformation(hEvent, HANDLE_FLAG_INHERIT, 0),
                 "v207 SetHandleInformation inherit off", "inherit flag can be cleared per handle");
    hflags = 0;
    smoke_expect(&s, hEvent && GetHandleInformation(hEvent, &hflags) && !(hflags & HANDLE_FLAG_INHERIT),
                 "v207 GetHandleInformation inherit off", "cleared inherit flag is visible");

    smoke_expect(&s, hEvent && SetHandleInformation(hEvent, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE),
                 "v207 SetHandleInformation protect on", "protect-from-close flag is mutable per handle");
    hflags = 0;
    smoke_expect(&s, hEvent && GetHandleInformation(hEvent, &hflags) && (hflags & HANDLE_FLAG_PROTECT_FROM_CLOSE),
                 "v207 GetHandleInformation protect on", "HANDLE_FLAG_PROTECT_FROM_CLOSE is reported by the public API");
    MyHandleInfo protectDiag;
    memset(&protectDiag, 0, sizeof(protectDiag));
    smoke_expect(&s, hEvent && MyGetHandleInfo(hEvent, &protectDiag) && (protectDiag.flags & MYWIN_HANDLE_FLAG_PROTECT_FROM_CLOSE),
                 "v207 diagnostic protect flag", "diagnostic handle table mirrors protect-from-close");
    SetLastError(0x12345678u);
    smoke_expect(&s, hEvent && CloseHandle(hEvent) == FALSE,
                 "v207 CloseHandle protected handle", "protected public handle is not closed");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "CloseHandle(protected) LastError");
    smoke_expect(&s, hEvent && WaitForSingleObject(hEvent, 0) == WAIT_OBJECT_0,
                 "v207 protected handle remains live", "failed protected close leaves the handle usable");
    HANDLE hCloseProtected = (HANDLE)0xfeed2222u;
    SetLastError(0x12345678u);
    smoke_expect(&s, hEvent && DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &hCloseProtected, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE) == FALSE && hCloseProtected == 0,
                 "v207 DuplicateHandle close protected source", "DUPLICATE_CLOSE_SOURCE honors HANDLE_FLAG_PROTECT_FROM_CLOSE before allocating target");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "DuplicateHandle(protected CLOSE_SOURCE) LastError");
    smoke_expect(&s, hEvent && SetHandleInformation(hEvent, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0),
                 "v207 SetHandleInformation protect off", "protected handle can be made closeable again");
    hflags = 0;
    smoke_expect(&s, hEvent && GetHandleInformation(hEvent, &hflags) && !(hflags & HANDLE_FLAG_PROTECT_FROM_CLOSE),
                 "v207 GetHandleInformation protect off", "cleared protect flag is visible");

    SetLastError(0x12345678u);
    smoke_expect(&s, GetHandleInformation(GetCurrentThread(), &hflags) == FALSE,
                 "v207 GetHandleInformation pseudo thread", "pseudo handles are not public table entries");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "GetHandleInformation(pseudo thread) LastError");

    HANDLE oldStdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE oldStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE oldStdErr = GetStdHandle(STD_ERROR_HANDLE);
    smoke_expect(&s, SetStdHandle(STD_INPUT_HANDLE, hEvent) && GetStdHandle(STD_INPUT_HANDLE) == hEvent,
                 "v208 SetStdHandle/GetStdHandle stdin", "standard handles are process-table HANDLE values stored per process");
    smoke_expect(&s, SetStdHandle(STD_OUTPUT_HANDLE, hEvent) && GetStdHandle(STD_OUTPUT_HANDLE) == hEvent &&
                     SetStdHandle(STD_ERROR_HANDLE, hEvent) && GetStdHandle(STD_ERROR_HANDLE) == hEvent,
                 "v208 SetStdHandle stdout/stderr", "stdout/stderr slots are independent standard-handle entries");
    SetLastError(0x12345678u);
    smoke_expect(&s, GetStdHandle((DWORD)0x1234u) == INVALID_HANDLE_VALUE,
                 "v208 GetStdHandle invalid selector", "unknown STD_* selector returns INVALID_HANDLE_VALUE");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "GetStdHandle(invalid) LastError");

    STARTUPINFOA stdSi; memset(&stdSi, 0, sizeof(stdSi));
    stdSi.cb = sizeof(stdSi);
    stdSi.dwFlags = STARTF_USESTDHANDLES;
    stdSi.hStdInput = hEvent;
    stdSi.hStdOutput = hEvent;
    stdSi.hStdError = hEvent;
    PROCESS_INFORMATION stdPi; memset(&stdPi, 0, sizeof(stdPi));
    BOOL stdProcOk = CreateProcessA("v208-stdhandles.exe", NULL, NULL, NULL, TRUE, 0, NULL, NULL, &stdSi, &stdPi);
    MyProcessLiteInfo stdInfo; memset(&stdInfo, 0, sizeof(stdInfo));
    BOOL stdInfoOk = stdProcOk && MyGetProcessLiteInfo(stdPi.dwProcessId, &stdInfo);
    smoke_expect(&s, stdInfoOk && (stdInfo.startup_flags & STARTF_USESTDHANDLES) &&
                     stdInfo.std_input && stdInfo.std_output && stdInfo.std_error && stdInfo.handle_count >= 3,
                 "v208 CreateProcess STARTF_USESTDHANDLES", "child process receives materialized process-local std handle table entries");
    if (stdProcOk) { TerminateProcess(stdPi.hProcess, 0); CloseHandle(stdPi.hThread); CloseHandle(stdPi.hProcess); }
    SetStdHandle(STD_INPUT_HANDLE, oldStdIn == INVALID_HANDLE_VALUE ? 0 : oldStdIn);
    SetStdHandle(STD_OUTPUT_HANDLE, oldStdOut == INVALID_HANDLE_VALUE ? 0 : oldStdOut);
    SetStdHandle(STD_ERROR_HANDLE, oldStdErr == INVALID_HANDLE_VALUE ? 0 : oldStdErr);

    if (hPseudoProc) CloseHandle(hPseudoProc);
    if (hPseudoThread) CloseHandle(hPseudoThread);

    enum { V205_HANDLE_BURST = 300 };
    HANDLE burst[V205_HANDLE_BURST];
    memset(burst, 0, sizeof(burst));
    DWORD burstBefore = MyGetHandleCount(0);
    int burstOk = 1;
    for (int i = 0; i < V205_HANDLE_BURST; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &burst[i], 0, FALSE, DUPLICATE_SAME_ACCESS) || !burst[i]) {
            burstOk = 0;
            break;
        }
    }
    DWORD burstAfter = MyGetHandleCount(0);
    smoke_expect(&s, burstOk && burstAfter >= burstBefore + V205_HANDLE_BURST,
                 "v205 sparse handle table exceeds 256 slots", "300 same-process duplicates prove table capacity is no longer the old static 256 wall");
    for (int i = 0; i < V205_HANDLE_BURST; ++i) if (burst[i]) CloseHandle(burst[i]);

    enum { V208_HANDLE_BENCH = 4096 };
    HANDLE* bench = (HANDLE*)calloc(V208_HANDLE_BENCH, sizeof(HANDLE));
    int benchOk = bench != NULL;
    unsigned long long benchDupStart = smoke_perf_now_us();
    int benchMade = 0;
    for (int i = 0; benchOk && i < V208_HANDLE_BENCH; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &bench[i], 0, FALSE, DUPLICATE_SAME_ACCESS) || !bench[i]) {
            benchOk = 0;
            break;
        }
        benchMade++;
    }
    unsigned long long benchDupEnd = smoke_perf_now_us();
    DWORD benchCount = MyGetHandleCount(0);
    unsigned long long benchCloseStart = smoke_perf_now_us();
    for (int i = 0; bench && i < benchMade; ++i) if (bench[i]) CloseHandle(bench[i]);
    unsigned long long benchCloseEnd = smoke_perf_now_us();
    DWORD benchAfterClose = MyGetHandleCount(0);
    smoke_expect(&s, benchOk && benchMade == V208_HANDLE_BENCH && benchAfterClose + V208_HANDLE_BENCH <= benchCount,
                 "v208 handle benchmark allocation/close", "4096 duplicate/close cycle exercises sparse-table page growth without defining architecture");
    char benchDetail[192];
    double dupMs = (double)(benchDupEnd - benchDupStart) / 1000.0;
    double closeMs = (double)(benchCloseEnd - benchCloseStart) / 1000.0;
    double dupOps = dupMs > 0.0 ? ((double)benchMade * 1000.0 / dupMs) : 0.0;
    double closeOps = closeMs > 0.0 ? ((double)benchMade * 1000.0 / closeMs) : 0.0;
    snprintf(benchDetail, sizeof(benchDetail), "handles=%d dup_ms=%.3f close_ms=%.3f dup_ops_s=%.0f close_ops_s=%.0f count_after=%lu",
             benchMade, dupMs, closeMs, dupOps, closeOps, (unsigned long)benchAfterClose);
    smoke_info(s.group, "v208 handle benchmark", benchDetail);
    free(bench);

    enum { V209_HANDLE_THREADS = 4, V209_HANDLES_PER_THREAD = 1024 };
    pthread_t fanThreads[V209_HANDLE_THREADS];
    SmokeHandleThreadBench fanBench[V209_HANDLE_THREADS];
    pthread_barrier_t fanBarrier;
    int fanBarrierOk = (pthread_barrier_init(&fanBarrier, NULL, V209_HANDLE_THREADS) == 0);
    int fanThreadsStarted = 0;
    DWORD fanBefore = MyGetHandleCount(0);
    memset(fanBench, 0, sizeof(fanBench));
    int fanSetupOk = fanBarrierOk;
    for (int i = 0; i < V209_HANDLE_THREADS; ++i) {
        fanBench[i].mgr = &rt->mgr;
        fanBench[i].cap = rt->cap;
        fanBench[i].source = hEvent;
        fanBench[i].iterations = V209_HANDLES_PER_THREAD;
        fanBench[i].handles = (HANDLE*)calloc(V209_HANDLES_PER_THREAD, sizeof(HANDLE));
        fanBench[i].start_barrier = &fanBarrier;
        if (!fanBench[i].handles) fanSetupOk = 0;
    }
    unsigned long long fanStart = smoke_perf_now_us();
    for (int i = 0; fanSetupOk && i < V209_HANDLE_THREADS; ++i) {
        if (pthread_create(&fanThreads[i], NULL, smoke_handle_thread_fanout, &fanBench[i]) != 0) { fanSetupOk = 0; break; }
        fanThreadsStarted++;
    }
    for (int i = 0; i < fanThreadsStarted; ++i) pthread_join(fanThreads[i], NULL);
    unsigned long long fanEnd = smoke_perf_now_us();
    if (fanBarrierOk) pthread_barrier_destroy(&fanBarrier);
    int fanMade = 0;
    int fanOk = fanSetupOk && fanThreadsStarted == V209_HANDLE_THREADS;
    unsigned long long fanDupUsSum = 0, fanCloseUsSum = 0;
    for (int i = 0; i < V209_HANDLE_THREADS; ++i) {
        fanOk = fanOk && fanBench[i].ok && fanBench[i].made == V209_HANDLES_PER_THREAD;
        fanMade += fanBench[i].made;
        fanDupUsSum += fanBench[i].dup_us;
        fanCloseUsSum += fanBench[i].close_us;
        free(fanBench[i].handles);
    }
    DWORD fanAfter = MyGetHandleCount(0);
    smoke_expect(&s, fanOk && fanAfter == fanBefore,
                 "v209 multi-thread same-process handle fanout", "4 worker threads duplicate and close 4096 handles without leaking table entries");
    char fanDetail[256];
    double fanWallMs = (double)(fanEnd - fanStart) / 1000.0;
    double fanOps = fanWallMs > 0.0 ? ((double)fanMade * 1000.0 / fanWallMs) : 0.0;
    snprintf(fanDetail, sizeof(fanDetail), "threads=%d handles=%d wall_ms=%.3f dup_thread_ms_sum=%.3f close_thread_ms_sum=%.3f ops_s=%.0f count_before=%lu count_after=%lu",
             V209_HANDLE_THREADS, fanMade, fanWallMs, (double)fanDupUsSum / 1000.0, (double)fanCloseUsSum / 1000.0, fanOps, (unsigned long)fanBefore, (unsigned long)fanAfter);
    smoke_info(s.group, "v209 handle fanout benchmark", fanDetail);

    enum { V209_CHURN_PER_THREAD = 2048 };
    pthread_t churnThreads[V209_HANDLE_THREADS];
    SmokeHandleThreadBench churnBench[V209_HANDLE_THREADS];
    pthread_barrier_t churnBarrier;
    int churnBarrierOk = (pthread_barrier_init(&churnBarrier, NULL, V209_HANDLE_THREADS) == 0);
    int churnThreadsStarted = 0;
    DWORD churnBefore = MyGetHandleCount(0);
    memset(churnBench, 0, sizeof(churnBench));
    int churnSetupOk = churnBarrierOk;
    for (int i = 0; i < V209_HANDLE_THREADS; ++i) {
        churnBench[i].mgr = &rt->mgr;
        churnBench[i].cap = rt->cap;
        churnBench[i].source = hEvent;
        churnBench[i].iterations = V209_CHURN_PER_THREAD;
        churnBench[i].start_barrier = &churnBarrier;
    }
    unsigned long long churnStart = smoke_perf_now_us();
    for (int i = 0; churnSetupOk && i < V209_HANDLE_THREADS; ++i) {
        if (pthread_create(&churnThreads[i], NULL, smoke_handle_thread_churn, &churnBench[i]) != 0) { churnSetupOk = 0; break; }
        churnThreadsStarted++;
    }
    for (int i = 0; i < churnThreadsStarted; ++i) pthread_join(churnThreads[i], NULL);
    unsigned long long churnEnd = smoke_perf_now_us();
    if (churnBarrierOk) pthread_barrier_destroy(&churnBarrier);
    int churnMade = 0;
    int churnOk = churnSetupOk && churnThreadsStarted == V209_HANDLE_THREADS;
    for (int i = 0; i < V209_HANDLE_THREADS; ++i) {
        churnOk = churnOk && churnBench[i].ok && churnBench[i].made == V209_CHURN_PER_THREAD;
        churnMade += churnBench[i].made;
    }
    DWORD churnAfter = MyGetHandleCount(0);
    smoke_expect(&s, churnOk && churnAfter == churnBefore,
                 "v209 multi-thread duplicate/close churn", "4 worker threads interleave duplicate+close loops to exercise handle-lock contention");
    char churnDetail[224];
    double churnWallMs = (double)(churnEnd - churnStart) / 1000.0;
    double churnOps = churnWallMs > 0.0 ? ((double)churnMade * 1000.0 / churnWallMs) : 0.0;
    snprintf(churnDetail, sizeof(churnDetail), "threads=%d operations=%d wall_ms=%.3f ops_s=%.0f count_before=%lu count_after=%lu",
             V209_HANDLE_THREADS, churnMade, churnWallMs, churnOps, (unsigned long)churnBefore, (unsigned long)churnAfter);
    smoke_info(s.group, "v209 handle churn benchmark", churnDetail);

    enum { V212_PROC_CHURN_PER_THREAD = 2048 };
    pthread_t procThreads[V209_HANDLE_THREADS];
    SmokeHandleThreadBench procBench[V209_HANDLE_THREADS];
    pthread_barrier_t procBarrier;
    int procBarrierOk = (pthread_barrier_init(&procBarrier, NULL, V209_HANDLE_THREADS) == 0);
    int procThreadsStarted = 0;
    memset(procBench, 0, sizeof(procBench));
    int procSetupOk = procBarrierOk;
    for (int i = 0; i < V209_HANDLE_THREADS; ++i) {
        char capName[32];
        snprintf(capName, sizeof(capName), "v212-hproc-%d", i);
        procBench[i].mgr = &rt->mgr;
        procBench[i].cap = cap_create(21200 + (DWORD)i, capName, CAP_ADMIN);
        cap_add_target(&procBench[i].cap, 0);
        cap_add_path(&procBench[i].cap, ".");
        procBench[i].iterations = V212_PROC_CHURN_PER_THREAD;
        procBench[i].start_barrier = &procBarrier;
    }
    unsigned long long procStart = smoke_perf_now_us();
    for (int i = 0; procSetupOk && i < V209_HANDLE_THREADS; ++i) {
        if (pthread_create(&procThreads[i], NULL, smoke_handle_thread_process_churn, &procBench[i]) != 0) { procSetupOk = 0; break; }
        procThreadsStarted++;
    }
    for (int i = 0; i < procThreadsStarted; ++i) pthread_join(procThreads[i], NULL);
    unsigned long long procEnd = smoke_perf_now_us();
    if (procBarrierOk) pthread_barrier_destroy(&procBarrier);
    int procMade = 0;
    int procOk = procSetupOk && procThreadsStarted == V209_HANDLE_THREADS;
    DWORD procBeforeSum = 0, procAfterSum = 0;
    unsigned long long procThreadUsSum = 0;
    for (int i = 0; i < V209_HANDLE_THREADS; ++i) {
        procOk = procOk && procBench[i].ok && procBench[i].made == V212_PROC_CHURN_PER_THREAD && procBench[i].count_after == procBench[i].count_before;
        procMade += procBench[i].made;
        procBeforeSum += procBench[i].count_before;
        procAfterSum += procBench[i].count_after;
        procThreadUsSum += procBench[i].dup_us;
    }
    smoke_expect(&s, procOk,
                 "v212 multi-process parallel handle churn", "4 distinct runtime PIDs churn separate per-process handle tables in parallel");
    char procDetail[256];
    double procWallMs = (double)(procEnd - procStart) / 1000.0;
    double procOps = procWallMs > 0.0 ? ((double)procMade * 1000.0 / procWallMs) : 0.0;
    snprintf(procDetail, sizeof(procDetail), "processes=%d operations=%d wall_ms=%.3f thread_ms_sum=%.3f ops_s=%.0f count_before_sum=%lu count_after_sum=%lu",
             V209_HANDLE_THREADS, procMade, procWallMs, (double)procThreadUsSum / 1000.0, procOps, (unsigned long)procBeforeSum, (unsigned long)procAfterSum);
    smoke_info(s.group, "v212 multi-process handle churn benchmark", procDetail);

    MyHandleTableAudit pushAudit;
    memset(&pushAudit, 0, sizeof(pushAudit));
    MyWinGetHandleTableAudit(&pushAudit);
    char pushDetail[256];
    snprintf(pushDetail, sizeof(pushDetail), "shared_fast=%lu shared_slow=%lu exclusive_fast=%lu exclusive_slow=%lu wakeups=%lu contentions=%lu",
             (unsigned long)pushAudit.pushlock_shared_fast, (unsigned long)pushAudit.pushlock_shared_slow,
             (unsigned long)pushAudit.pushlock_exclusive_fast, (unsigned long)pushAudit.pushlock_exclusive_slow,
             (unsigned long)pushAudit.pushlock_wakeups, (unsigned long)pushAudit.pushlock_contentions);
    smoke_info(s.group, "v212 ex-pushlock-style audit", pushDetail);

    MyHandleTableAudit cacheBefore;
    MyHandleTableAudit cacheAfter;
    memset(&cacheBefore, 0, sizeof(cacheBefore));
    memset(&cacheAfter, 0, sizeof(cacheAfter));
    MyWinGetHandleTableAudit(&cacheBefore);
    enum { V211_CACHE_PROBES = 4096 };
    int cacheProbeOk = 1;
    unsigned long long cacheStart = smoke_perf_now_us();
    for (int i = 0; i < V211_CACHE_PROBES; ++i) {
        DWORD wr = WaitForSingleObject(hEvent, 0);
        if (wr != WAIT_TIMEOUT && wr != WAIT_OBJECT_0) { cacheProbeOk = 0; break; }
    }
    unsigned long long cacheEnd = smoke_perf_now_us();
    MyWinGetHandleTableAudit(&cacheAfter);
    DWORD cacheHitsDelta = cacheAfter.handle_cache_hits - cacheBefore.handle_cache_hits;
    DWORD cacheMissDelta = cacheAfter.handle_cache_misses - cacheBefore.handle_cache_misses;
    smoke_expect(&s, cacheProbeOk && cacheHitsDelta > 0 && cacheHitsDelta >= (V211_CACHE_PROBES / 2),
                 "v211 TLS last-handle cache hit path", "repeated same-handle public waits hit the thread-local lookup cache without changing handle semantics");
    char cacheDetail[256];
    double cacheMs = (double)(cacheEnd - cacheStart) / 1000.0;
    double cacheOps = cacheMs > 0.0 ? ((double)V211_CACHE_PROBES * 1000.0 / cacheMs) : 0.0;
    snprintf(cacheDetail, sizeof(cacheDetail), "probes=%d wall_ms=%.3f ops_s=%.0f hits_delta=%lu misses_delta=%lu total_hits=%lu total_misses=%lu invalidations=%lu",
             V211_CACHE_PROBES, cacheMs, cacheOps, (unsigned long)cacheHitsDelta, (unsigned long)cacheMissDelta,
             (unsigned long)cacheAfter.handle_cache_hits, (unsigned long)cacheAfter.handle_cache_misses,
             (unsigned long)cacheAfter.handle_cache_invalidations);
    smoke_info(s.group, "v211 handle TLS cache benchmark", cacheDetail);

    MyHandleTableAudit v249Before;
    MyHandleTableAudit v249After;
    memset(&v249Before, 0, sizeof(v249Before));
    memset(&v249After, 0, sizeof(v249After));
    MyWinGetHandleTableAudit(&v249Before);
    enum { V249_DUP_CLOSE_PROBES = 2048 };
    int v249Ok = 1;
    int v249Made = 0;
    unsigned long long v249Start = smoke_perf_now_us();
    for (int i = 0; i < V249_DUP_CLOSE_PROBES; ++i) {
        HANDLE hd = 0;
        if (!DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &hd, 0, FALSE, DUPLICATE_SAME_ACCESS) || !hd) {
            v249Ok = 0;
            break;
        }
        v249Made++;
        if (!CloseHandle(hd)) { v249Ok = 0; break; }
    }
    unsigned long long v249End = smoke_perf_now_us();
    MyWinGetHandleTableAudit(&v249After);
    DWORD v249Hits = v249After.handle_cache_hits - v249Before.handle_cache_hits;
    DWORD v249Misses = v249After.handle_cache_misses - v249Before.handle_cache_misses;
    DWORD v249Validated = v249After.handle_cache_entry_validated - v249Before.handle_cache_entry_validated;
    DWORD v249Stale = v249After.handle_cache_entry_stale - v249Before.handle_cache_entry_stale;
    DWORD v250FreeHintHits = v249After.handle_free_hint_hits - v249Before.handle_free_hint_hits;
    DWORD v250FreeHintMisses = v249After.handle_free_hint_misses - v249Before.handle_free_hint_misses;
    DWORD v250FreeStalePops = v249After.handle_free_stale_pops - v249Before.handle_free_stale_pops;
    DWORD v255PairBatchHits = v249After.handle_free_batch_hits - v249Before.handle_free_batch_hits;
    DWORD v255PairBatchStores = v249After.handle_free_batch_stores - v249Before.handle_free_batch_stores;
    smoke_expect(&s, v249Ok && v249Made == V249_DUP_CLOSE_PROBES && v249Hits >= (V249_DUP_CLOSE_PROBES / 2) && v249Validated >= (V249_DUP_CLOSE_PROBES / 2),
                 "v249 source handle cache survives unrelated CloseHandle", "duplicate/close churn keeps the source handle in the TLS cache");
    smoke_expect(&s, v249Ok && (v250FreeHintHits + v255PairBatchHits) >= (DWORD)(V249_DUP_CLOSE_PROBES / 2),
                 "v250 handle free-slot TLS hint reuses close->alloc slots",
                 "same-thread DuplicateHandle/CloseHandle churn consumes the freshly closed slot without a global free-stack pop; v255 may satisfy the reuse through the wider TLS free-batch");
    char v249Detail[320];
    double v249Ms = (double)(v249End - v249Start) / 1000.0;
    double v249Ops = v249Ms > 0.0 ? ((double)v249Made * 1000.0 / v249Ms) : 0.0;
    snprintf(v249Detail, sizeof(v249Detail), "probes=%d wall_ms=%.3f ops_s=%.0f hits=%lu misses=%lu validated=%lu stale=%lu invalidations=%lu freeHintHit=%lu freeHintMiss=%lu batchHit=%lu batchStore=%lu stalePop=%lu",
             v249Made, v249Ms, v249Ops, (unsigned long)v249Hits, (unsigned long)v249Misses,
             (unsigned long)v249Validated, (unsigned long)v249Stale,
             (unsigned long)(v249After.handle_cache_invalidations - v249Before.handle_cache_invalidations),
             (unsigned long)v250FreeHintHits, (unsigned long)v250FreeHintMisses,
             (unsigned long)v255PairBatchHits, (unsigned long)v255PairBatchStores, (unsigned long)v250FreeStalePops);
    smoke_info(s.group, "v249/v250/v255 handle source-cache/free-reuse churn benchmark", v249Detail);

    enum { V255_BATCH_HANDLES = 48 };
    HANDLE v255Batch[V255_BATCH_HANDLES];
    memset(v255Batch, 0, sizeof(v255Batch));
    MyHandleTableAudit v255Before, v255After;
    memset(&v255Before, 0, sizeof(v255Before));
    memset(&v255After, 0, sizeof(v255After));
    int v255Ok = 1;
    for (int i = 0; i < V255_BATCH_HANDLES; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &v255Batch[i], 0, FALSE, DUPLICATE_SAME_ACCESS) || !v255Batch[i]) {
            v255Ok = 0;
            break;
        }
    }
    MyWinGetHandleTableAudit(&v255Before);
    unsigned long long v255Start = smoke_perf_now_us();
    for (int i = 0; v255Ok && i < V255_BATCH_HANDLES; ++i) {
        if (!CloseHandle(v255Batch[i])) v255Ok = 0;
        v255Batch[i] = 0;
    }
    for (int i = 0; v255Ok && i < V255_BATCH_HANDLES; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &v255Batch[i], 0, FALSE, DUPLICATE_SAME_ACCESS) || !v255Batch[i]) v255Ok = 0;
    }
    unsigned long long v255End = smoke_perf_now_us();
    for (int i = 0; i < V255_BATCH_HANDLES; ++i) {
        if (v255Batch[i]) CloseHandle(v255Batch[i]);
    }
    MyWinGetHandleTableAudit(&v255After);
    DWORD v255BatchStores = v255After.handle_free_batch_stores - v255Before.handle_free_batch_stores;
    DWORD v255BatchHits = v255After.handle_free_batch_hits - v255Before.handle_free_batch_hits;
    DWORD v255BatchFlushes = v255After.handle_free_batch_flushes - v255Before.handle_free_batch_flushes;
    DWORD v255BatchFlushed = v255After.handle_free_batch_flushed_slots - v255Before.handle_free_batch_flushed_slots;
    DWORD v255BatchOverflow = v255After.handle_free_batch_overflow - v255Before.handle_free_batch_overflow;
    smoke_expect(&s, v255Ok && v255BatchStores >= V255_BATCH_HANDLES && v255BatchHits >= V255_BATCH_HANDLES,
                 "v255 handle TLS free-batch reuses batchy close bursts",
                 "closing many same-thread duplicates and reallocating them consumes slots from the local free-batch before the global free stack");
    char v255Detail[320];
    double v255Ms = (double)(v255End - v255Start) / 1000.0;
    double v255Ops = v255Ms > 0.0 ? ((double)(V255_BATCH_HANDLES * 2) * 1000.0 / v255Ms) : 0.0;
    snprintf(v255Detail, sizeof(v255Detail), "handles=%d ops=%d wall_ms=%.3f ops_s=%.0f batchStore=%lu batchHit=%lu flush=%lu flushedSlots=%lu overflow=%lu",
             V255_BATCH_HANDLES, V255_BATCH_HANDLES * 2, v255Ms, v255Ops,
             (unsigned long)v255BatchStores, (unsigned long)v255BatchHits,
             (unsigned long)v255BatchFlushes, (unsigned long)v255BatchFlushed, (unsigned long)v255BatchOverflow);
    smoke_info(s.group, "v255 handle TLS free-batch benchmark", v255Detail);

    enum { V256_LANE_HANDLES = 24 };
    STARTUPINFOA v256Si; memset(&v256Si, 0, sizeof(v256Si));
    v256Si.cb = sizeof(v256Si);
    PROCESS_INFORMATION v256Pi; memset(&v256Pi, 0, sizeof(v256Pi));
    HANDLE v256Child[V256_LANE_HANDLES];
    HANDLE v256Current[V256_LANE_HANDLES];
    memset(v256Child, 0, sizeof(v256Child));
    memset(v256Current, 0, sizeof(v256Current));
    BOOL v256ProcOk = CreateProcessA("v256-freebatch-child.exe", NULL, NULL, NULL, FALSE, 0, NULL, NULL, &v256Si, &v256Pi);
    int v256Ok = v256ProcOk ? 1 : 0;
    int v256Made = 0;
    for (int i = 0; v256Ok && i < V256_LANE_HANDLES; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), hEvent, v256Pi.hProcess, &v256Child[i], 0, TRUE, DUPLICATE_SAME_ACCESS) || !v256Child[i]) v256Ok = 0;
        else v256Made++;
    }
    MyHandleTableAudit v256Before, v256After;
    memset(&v256Before, 0, sizeof(v256Before));
    memset(&v256After, 0, sizeof(v256After));
    MyWinGetHandleTableAudit(&v256Before);
    unsigned long long v256Start = smoke_perf_now_us();
    for (int i = 0; v256Ok && i < V256_LANE_HANDLES; ++i) {
        HANDLE back = 0;
        if (!DuplicateHandle(v256Pi.hProcess, v256Child[i], GetCurrentProcess(), &back, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE) || !back) v256Ok = 0;
        else {
            v256Child[i] = 0;
            if (!CloseHandle(back)) v256Ok = 0;
        }
    }
    for (int i = 0; v256Ok && i < V256_LANE_HANDLES; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), hEvent, v256Pi.hProcess, &v256Child[i], 0, TRUE, DUPLICATE_SAME_ACCESS) || !v256Child[i]) v256Ok = 0;
    }
    for (int i = 0; v256Ok && i < V256_LANE_HANDLES; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), hEvent, GetCurrentProcess(), &v256Current[i], 0, FALSE, DUPLICATE_SAME_ACCESS) || !v256Current[i]) v256Ok = 0;
    }
    unsigned long long v256End = smoke_perf_now_us();
    MyWinGetHandleTableAudit(&v256After);
    DWORD v256Stores = v256After.handle_free_batch_stores - v256Before.handle_free_batch_stores;
    DWORD v256Hits = v256After.handle_free_batch_hits - v256Before.handle_free_batch_hits;
    DWORD v256Flushes = v256After.handle_free_batch_flushes - v256Before.handle_free_batch_flushes;
    DWORD v256LaneAllocs = v256After.handle_free_batch_lane_allocs - v256Before.handle_free_batch_lane_allocs;
    DWORD v256SwitchAvoided = v256After.handle_free_batch_table_switch_avoided - v256Before.handle_free_batch_table_switch_avoided;
    smoke_expect(&s, v256Ok && v256Made == V256_LANE_HANDLES &&
                     v256Stores >= (DWORD)(V256_LANE_HANDLES * 2) &&
                     v256Hits >= (DWORD)(V256_LANE_HANDLES * 2) &&
                     v256Flushes == 0 && v256LaneAllocs >= 1 && v256SwitchAvoided >= 1,
                 "v256 handle TLS free-batch keeps multiple process tables warm",
                 "alternating current/child process handle-table close bursts are retained in separate TLS lanes instead of flushing on table switch");
    char v256Detail[384];
    double v256Ms = (double)(v256End - v256Start) / 1000.0;
    double v256Ops = v256Ms > 0.0 ? ((double)(V256_LANE_HANDLES * 4) * 1000.0 / v256Ms) : 0.0;
    snprintf(v256Detail, sizeof(v256Detail), "handles=%d ops=%d wall_ms=%.3f ops_s=%.0f stores=%lu hits=%lu flush=%lu laneAlloc=%lu switchAvoid=%lu",
             V256_LANE_HANDLES, V256_LANE_HANDLES * 4, v256Ms, v256Ops,
             (unsigned long)v256Stores, (unsigned long)v256Hits, (unsigned long)v256Flushes,
             (unsigned long)v256LaneAllocs, (unsigned long)v256SwitchAvoided);
    smoke_info(s.group, "v256 multi-table handle TLS free-batch benchmark", v256Detail);
    for (int i = 0; i < V256_LANE_HANDLES; ++i) {
        if (v256Current[i]) CloseHandle(v256Current[i]);
        if (v256Child[i] && v256ProcOk) {
            HANDLE back = 0;
            if (DuplicateHandle(v256Pi.hProcess, v256Child[i], GetCurrentProcess(), &back, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE) && back) CloseHandle(back);
        }
    }
    if (v256ProcOk) { TerminateProcess(v256Pi.hProcess, 0); CloseHandle(v256Pi.hThread); CloseHandle(v256Pi.hProcess); }

    HANDLE v250Events[4] = {0,0,0,0};
    int v250Ok = 1;
    for (int i = 0; i < 4; ++i) {
        char nm[128];
        smoke_unique_name(nm, sizeof(nm), "v250.cache.event");
        v250Events[i] = CreateEventA(NULL, TRUE, FALSE, nm);
        if (!v250Events[i]) v250Ok = 0;
    }
    MyHandleTableAudit v250HandleBefore, v250HandleAfter;
    MyWaitAudit v250WaitBefore, v250WaitAfter;
    memset(&v250HandleBefore, 0, sizeof(v250HandleBefore));
    memset(&v250HandleAfter, 0, sizeof(v250HandleAfter));
    memset(&v250WaitBefore, 0, sizeof(v250WaitBefore));
    memset(&v250WaitAfter, 0, sizeof(v250WaitAfter));
    MyWinGetHandleTableAudit(&v250HandleBefore);
    MyWinGetWaitAudit(&v250WaitBefore);
    enum { V250_WFMO_CACHE_PROBES = 1024 };
    int v250Made = 0;
    unsigned long long v250Start = smoke_perf_now_us();
    for (int i = 0; v250Ok && i < V250_WFMO_CACHE_PROBES; ++i) {
        DWORD wr = WaitForMultipleObjects(4, v250Events, FALSE, 0);
        if (wr != WAIT_TIMEOUT) { v250Ok = 0; break; }
        v250Made++;
    }
    unsigned long long v250End = smoke_perf_now_us();
    MyWinGetHandleTableAudit(&v250HandleAfter);
    MyWinGetWaitAudit(&v250WaitAfter);
    DWORD v250Hits = v250HandleAfter.handle_cache_hits - v250HandleBefore.handle_cache_hits;
    DWORD v250Misses = v250HandleAfter.handle_cache_misses - v250HandleBefore.handle_cache_misses;
    DWORD v250SlotProbes = v250HandleAfter.handle_cache_slot_probes - v250HandleBefore.handle_cache_slot_probes;
    DWORD v250Collisions = v250HandleAfter.handle_cache_slot_collisions - v250HandleBefore.handle_cache_slot_collisions;
    DWORD v250Prevalidated = v250WaitAfter.wait_multiple_prevalidated - v250WaitBefore.wait_multiple_prevalidated;
    DWORD v250PreResolves = v250WaitAfter.wait_multiple_prevalidate_resolves - v250WaitBefore.wait_multiple_prevalidate_resolves;
    smoke_expect(&s, v250Ok && v250Made == V250_WFMO_CACHE_PROBES &&
                     v250Prevalidated >= (DWORD)V250_WFMO_CACHE_PROBES &&
                     v250PreResolves >= (DWORD)(V250_WFMO_CACHE_PROBES * 4) &&
                     v250Hits >= (DWORD)(V250_WFMO_CACHE_PROBES * 2),
                 "v250 WFMO prevalidation and multi-entry handle TLS cache",
                 "multi-handle timeout waits resolve handles once per call and keep several public handles hot");
    char v250Detail[320];
    double v250Ms = (double)(v250End - v250Start) / 1000.0;
    double v250Ops = v250Ms > 0.0 ? ((double)v250Made * 1000.0 / v250Ms) : 0.0;
    snprintf(v250Detail, sizeof(v250Detail), "probes=%d wall_ms=%.3f ops_s=%.0f hits=%lu misses=%lu slotProbes=%lu collisions=%lu prevalidated=%lu preResolves=%lu",
             v250Made, v250Ms, v250Ops, (unsigned long)v250Hits, (unsigned long)v250Misses,
             (unsigned long)v250SlotProbes, (unsigned long)v250Collisions,
             (unsigned long)v250Prevalidated, (unsigned long)v250PreResolves);
    smoke_info(s.group, "v250 WFMO prevalidation/cache benchmark", v250Detail);
    for (int i = 0; i < 4; ++i) if (v250Events[i]) CloseHandle(v250Events[i]);

    char v214EventName[128], v214MutexName[128], v214SemName[128];
    smoke_unique_name(v214EventName, sizeof(v214EventName), "v214.hash.event");
    smoke_unique_name(v214MutexName, sizeof(v214MutexName), "v214.hash.mutex");
    smoke_unique_name(v214SemName, sizeof(v214SemName), "v214.hash.sem");
    HANDLE v214Event = CreateEventA(NULL, TRUE, FALSE, v214EventName);
    HANDLE v214Mutex = CreateMutexA(NULL, FALSE, v214MutexName);
    HANDLE v214Sem = CreateSemaphoreA(NULL, 0, 4, v214SemName);
    enum { V214_NAMED_OPEN_BENCH = 2048 };
    int namedOk = v214Event && v214Mutex && v214Sem;
    int namedOps = 0;
    MyNamedDirectoryAudit namedDirBefore, namedDirAfter;
    memset(&namedDirBefore, 0, sizeof(namedDirBefore));
    memset(&namedDirAfter, 0, sizeof(namedDirAfter));
    MyWinGetNamedDirectoryAudit(&namedDirBefore);
    unsigned long long namedStart = smoke_perf_now_us();
    for (int i = 0; namedOk && i < V214_NAMED_OPEN_BENCH; ++i) {
        HANDLE he = OpenEventA(EVENT_MODIFY_STATE, FALSE, v214EventName);
        HANDLE hm = OpenMutexA(SYNCHRONIZE, FALSE, v214MutexName);
        HANDLE hs = OpenSemaphoreA(SEMAPHORE_MODIFY_STATE, FALSE, v214SemName);
        if (!he || !hm || !hs) namedOk = 0;
        if (he) { CloseHandle(he); namedOps++; }
        if (hm) { CloseHandle(hm); namedOps++; }
        if (hs) { CloseHandle(hs); namedOps++; }
    }
    unsigned long long namedEnd = smoke_perf_now_us();
    MyWinGetNamedDirectoryAudit(&namedDirAfter);
    DWORD namedDirFastHits = namedDirAfter.fast_hits - namedDirBefore.fast_hits;
    DWORD namedDirFastMisses = namedDirAfter.fast_misses - namedDirBefore.fast_misses;
    DWORD namedDirStale = namedDirAfter.stale_hits - namedDirBefore.stale_hits;
    DWORD namedDirTlsHits = namedDirAfter.tls_hits - namedDirBefore.tls_hits;
    DWORD namedDirTlsMisses = namedDirAfter.tls_misses - namedDirBefore.tls_misses;
    DWORD namedDirTlsStores = namedDirAfter.tls_stores - namedDirBefore.tls_stores;
    DWORD namedDirTlsEpochMisses = namedDirAfter.tls_epoch_misses - namedDirBefore.tls_epoch_misses;
    DWORD namedDirSlotHits = namedDirAfter.slot_fast_hits - namedDirBefore.slot_fast_hits;
    DWORD namedDirSlotMisses = namedDirAfter.slot_fast_misses - namedDirBefore.slot_fast_misses;
    smoke_expect(&s, namedOk && namedOps == V214_NAMED_OPEN_BENCH * 3,
                 "v214 per-type named object hash lookup", "OpenEvent/OpenMutex/OpenSemaphore keep the per-type fallback path intact");
    smoke_expect(&s, namedOk && namedDirFastHits >= (DWORD)namedOps && namedDirStale == 0,
                 "v251 central named object directory fast-open", "OpenEvent/OpenMutex/OpenSemaphore resolve name -> object handle through the shared directory before typed fallback");
    smoke_expect(&s, namedOk && namedDirTlsHits + 32u >= (DWORD)namedOps && namedDirTlsStores >= 3u,
                 "v252 named directory TLS fast-open cache", "repeated same-thread named opens avoid the shared directory mutex after the first resolved lookup");
    smoke_expect(&s, namedOk && namedDirSlotHits >= (DWORD)namedOps && namedDirSlotMisses == 0,
                 "v253 named directory typed-slot fast-open",
                 "directory fast-open carries the typed payload slot so OpenEvent/OpenMutex/OpenSemaphore skip handle re-decode/fallback lookup");
    char namedDetail[512];
    double namedMs = (double)(namedEnd - namedStart) / 1000.0;
    double namedOpsS = namedMs > 0.0 ? ((double)namedOps * 1000.0 / namedMs) : 0.0;
    snprintf(namedDetail, sizeof(namedDetail), "iterations=%d opens=%d wall_ms=%.3f ops_s=%.0f event=%d mutex=%d semaphore=%d dirFastHit=%lu dirFastMiss=%lu stale=%lu tlsHit=%lu tlsMiss=%lu tlsStore=%lu tlsEpochMiss=%lu slotHit=%lu slotMiss=%lu entries=%lu free=%lu epoch=%lu",
             V214_NAMED_OPEN_BENCH, namedOps, namedMs, namedOpsS, v214Event ? 1 : 0, v214Mutex ? 1 : 0, v214Sem ? 1 : 0,
             (unsigned long)namedDirFastHits, (unsigned long)namedDirFastMisses, (unsigned long)namedDirStale,
             (unsigned long)namedDirTlsHits, (unsigned long)namedDirTlsMisses, (unsigned long)namedDirTlsStores, (unsigned long)namedDirTlsEpochMisses,
             (unsigned long)namedDirSlotHits, (unsigned long)namedDirSlotMisses,
             (unsigned long)namedDirAfter.entries, (unsigned long)namedDirAfter.free_slots, (unsigned long)namedDirAfter.epoch);
    smoke_info(s.group, "v252 named directory TLS fast-open benchmark", namedDetail);

    char v253SectionName[128], v253TimerName[128];
    smoke_unique_name(v253SectionName, sizeof(v253SectionName), "v253.dir.section");
    smoke_unique_name(v253TimerName, sizeof(v253TimerName), "v253.dir.timer");
    HANDLE v253Section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, v253SectionName);
    HANDLE v253Timer = CreateWaitableTimerA(NULL, TRUE, v253TimerName);
    MyNamedDirectoryAudit v253DirBefore, v253DirAfter;
    memset(&v253DirBefore, 0, sizeof(v253DirBefore));
    memset(&v253DirAfter, 0, sizeof(v253DirAfter));
    MyWinGetNamedDirectoryAudit(&v253DirBefore);
    enum { V253_SLOT_PROBES = 1024 };
    int v253SlotOk = v253Section && v253Timer;
    int v253SlotOps = 0;
    unsigned long long v253SlotStart = smoke_perf_now_us();
    for (int i = 0; v253SlotOk && i < V253_SLOT_PROBES; ++i) {
        HANDLE hs = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, v253SectionName);
        HANDLE ht = CreateWaitableTimerA(NULL, TRUE, v253TimerName);
        if (!hs || !ht) v253SlotOk = 0;
        if (hs) { CloseHandle(hs); v253SlotOps++; }
        if (ht) { CloseHandle(ht); v253SlotOps++; }
    }
    unsigned long long v253SlotEnd = smoke_perf_now_us();
    MyWinGetNamedDirectoryAudit(&v253DirAfter);
    DWORD v253SlotHits = v253DirAfter.slot_fast_hits - v253DirBefore.slot_fast_hits;
    DWORD v253SlotMisses = v253DirAfter.slot_fast_misses - v253DirBefore.slot_fast_misses;
    smoke_expect(&s, v253SlotOk && v253SlotOps == V253_SLOT_PROBES * 2 && v253SlotHits >= (DWORD)v253SlotOps && v253SlotMisses == 0,
                 "v253 named directory typed-slot covers sections/timers",
                 "Section and timer named opens/reopens use the directory payload slot, not only Event/Mutex/Semaphore paths");
    char v253SlotDetail[320];
    double v253SlotMs = (double)(v253SlotEnd - v253SlotStart) / 1000.0;
    double v253SlotOpsS = v253SlotMs > 0.0 ? ((double)v253SlotOps * 1000.0 / v253SlotMs) : 0.0;
    snprintf(v253SlotDetail, sizeof(v253SlotDetail), "iterations=%d ops=%d wall_ms=%.3f ops_s=%.0f slotHit=%lu slotMiss=%lu section=%d timer=%d",
             V253_SLOT_PROBES, v253SlotOps, v253SlotMs, v253SlotOpsS,
             (unsigned long)v253SlotHits, (unsigned long)v253SlotMisses, v253Section ? 1 : 0, v253Timer ? 1 : 0);
    smoke_info(s.group, "v253 named directory section/timer slot benchmark", v253SlotDetail);
    if (v253Section) CloseHandle(v253Section);
    if (v253Timer) CloseHandle(v253Timer);

    char v252EpochName[128];
    smoke_unique_name(v252EpochName, sizeof(v252EpochName), "v252.dir.epoch");
    HANDLE v252EpochEvent = CreateEventA(NULL, TRUE, FALSE, v252EpochName);
    HANDLE v252EpochEventOpen = v252EpochEvent ? OpenEventA(EVENT_ALL_ACCESS, FALSE, v252EpochName) : 0;
    MyNamedDirectoryAudit v252EpochBefore, v252EpochAfter;
    memset(&v252EpochBefore, 0, sizeof(v252EpochBefore));
    memset(&v252EpochAfter, 0, sizeof(v252EpochAfter));
    MyWinGetNamedDirectoryAudit(&v252EpochBefore);
    if (v252EpochEventOpen) CloseHandle(v252EpochEventOpen);
    if (v252EpochEvent) CloseHandle(v252EpochEvent);
    HANDLE v252EpochMutex = CreateMutexA(NULL, FALSE, v252EpochName);
    SetLastError(0x12345678u);
    HANDLE v252StaleEvent = OpenEventA(EVENT_ALL_ACCESS, FALSE, v252EpochName);
    MyWinGetNamedDirectoryAudit(&v252EpochAfter);
    smoke_expect(&s, v252EpochEvent && v252EpochEventOpen && v252EpochMutex && !v252StaleEvent,
                 "v252 named directory TLS cache invalidates on type reuse", "Event-name cache entry cannot survive remove+Mutex recreate of the same canonical name");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "OpenEventA(mutex-name after TLS epoch change) LastError");
    smoke_expect(&s, v252EpochAfter.tls_epoch_misses > v252EpochBefore.tls_epoch_misses && v252EpochAfter.epoch > v252EpochBefore.epoch,
                 "v252 named directory epoch protects cached entries", "directory remove/insert bumps epoch and forces stale TLS entries back through authoritative lookup");
    if (v252StaleEvent) CloseHandle(v252StaleEvent);
    if (v252EpochMutex) CloseHandle(v252EpochMutex);

    MyHandleInfo v215EventInfo, v215MutexInfo, v215SemInfo;
    memset(&v215EventInfo, 0, sizeof(v215EventInfo));
    memset(&v215MutexInfo, 0, sizeof(v215MutexInfo));
    memset(&v215SemInfo, 0, sizeof(v215SemInfo));
    BOOL v215DiagOk = v214Event && v214Mutex && v214Sem &&
                      MyGetHandleInfo(v214Event, &v215EventInfo) &&
                      MyGetHandleInfo(v214Mutex, &v215MutexInfo) &&
                      MyGetHandleInfo(v214Sem, &v215SemInfo);
    DWORD v215Type = 0, v215Slot = 0;
    BOOL v215SlotOk = v215DiagOk &&
                      _ObjectDecodeSlotHandle(v215EventInfo.object_handle, &v215Type, &v215Slot) && v215Type == _OBJECT_TYPE_EVENT && v215Slot < _OBJECT_SLOT_STRIDE &&
                      _ObjectDecodeSlotHandle(v215MutexInfo.object_handle, &v215Type, &v215Slot) && v215Type == _OBJECT_TYPE_MUTEX && v215Slot < _OBJECT_SLOT_STRIDE &&
                      _ObjectDecodeSlotHandle(v215SemInfo.object_handle, &v215Type, &v215Slot) && v215Type == _OBJECT_TYPE_SEMAPHORE && v215Slot < _OBJECT_SLOT_STRIDE;
    smoke_expect(&s, v215SlotOk,
                 "v215 object handles encode type-local slot", "Event/Mutex/Semaphore object handles decode directly to their object-slot array index");

    enum { V215_OBJECT_SLOT_PROBES = 8192 };
    int slotProbeOk = v215SlotOk ? 1 : 0;
    unsigned long long slotStart = smoke_perf_now_us();
    for (int i = 0; slotProbeOk && i < V215_OBJECT_SLOT_PROBES; ++i) {
        _ObjectectInfo eoi, moi, soi;
        if (!_ObjectGetInfo(v215EventInfo.object_handle, &eoi) || eoi.type != _OBJECT_TYPE_EVENT) slotProbeOk = 0;
        if (!_ObjectGetInfo(v215MutexInfo.object_handle, &moi) || moi.type != _OBJECT_TYPE_MUTEX) slotProbeOk = 0;
        if (!_ObjectGetInfo(v215SemInfo.object_handle, &soi) || soi.type != _OBJECT_TYPE_SEMAPHORE) slotProbeOk = 0;
    }
    unsigned long long slotEnd = smoke_perf_now_us();
    int slotOps = V215_OBJECT_SLOT_PROBES * 3;
    smoke_expect(&s, slotProbeOk,
                 "v215 direct object-slot lookup", "ObjectId/object handle resolves to array slot without scanning type tables");
    char slotDetail[224];
    double slotMs = (double)(slotEnd - slotStart) / 1000.0;
    double slotOpsS = slotMs > 0.0 ? ((double)slotOps * 1000.0 / slotMs) : 0.0;
    snprintf(slotDetail, sizeof(slotDetail), "probes=%d lookups=%d wall_ms=%.3f ops_s=%.0f event_obj=0x%x mutex_obj=0x%x semaphore_obj=0x%x",
             V215_OBJECT_SLOT_PROBES, slotOps, slotMs, slotOpsS,
             (unsigned)v215EventInfo.object_handle, (unsigned)v215MutexInfo.object_handle, (unsigned)v215SemInfo.object_handle);
    smoke_info(s.group, "v215 object-slot lookup benchmark", slotDetail);

    char v216SectionName[128], v216TimerName[128], v216SvcName[128];
    smoke_unique_name(v216SectionName, sizeof(v216SectionName), "v216.slot.section");
    smoke_unique_name(v216TimerName, sizeof(v216TimerName), "v216.slot.timer");
    smoke_unique_name(v216SvcName, sizeof(v216SvcName), "v216.slot.service");

    HANDLE v216Section = CreateFileMappingA(0, NULL, PAGE_READWRITE, 0, 4096, v216SectionName);
    HANDLE v216Timer = CreateWaitableTimerA(NULL, TRUE, v216TimerName);
    HANDLE v216Token = 0;
    BOOL v216TokenOk = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &v216Token);
    SC_HANDLE v216Scm = OpenSCManagerA(NULL, NULL, MYSVC_ACCESS_ALL);
    SC_HANDLE v216Svc = v216Scm ? CreateServiceA(v216Scm, v216SvcName, v216SvcName, MYSVC_ACCESS_ALL, 0, MYSVC_START_TYPE_DEMAND, 0, "myos-v216-service", NULL, NULL, NULL, NULL, NULL) : 0;

    MyHandleInfo v216SectionInfo, v216TimerInfo, v216TokenInfo;
    memset(&v216SectionInfo, 0, sizeof(v216SectionInfo));
    memset(&v216TimerInfo, 0, sizeof(v216TimerInfo));
    memset(&v216TokenInfo, 0, sizeof(v216TokenInfo));
    BOOL v216DiagOk = v216Section && MyGetHandleInfo(v216Section, &v216SectionInfo) &&
                      v216Timer && MyGetHandleInfo(v216Timer, &v216TimerInfo) &&
                      v216TokenOk && v216Token && MyGetHandleInfo(v216Token, &v216TokenInfo) &&
                      v216Svc && _ObjectGetInfo((HANDLE)v216Svc, &(_ObjectectInfo){0});

    HANDLE v216Objects[8];
    DWORD  v216Types[8] = { _OBJECT_TYPE_EVENT, _OBJECT_TYPE_MUTEX, _OBJECT_TYPE_SEMAPHORE, _OBJECT_TYPE_SECTION, _OBJECT_TYPE_TIMER, _OBJECT_TYPE_TOKEN, _OBJECT_TYPE_PROCESS, _OBJECT_TYPE_THREAD };
    v216Objects[0] = v215EventInfo.object_handle;
    v216Objects[1] = v215MutexInfo.object_handle;
    v216Objects[2] = v215SemInfo.object_handle;
    v216Objects[3] = v216SectionInfo.object_handle;
    v216Objects[4] = v216TimerInfo.object_handle;
    v216Objects[5] = v216TokenInfo.object_handle;
    v216Objects[6] = pseudoProcInfo.object_handle;
    v216Objects[7] = pseudoThreadInfo.object_handle;

    int v216SlotOk = v216DiagOk ? 1 : 0;
    for (int i = 0; v216SlotOk && i < 8; ++i) {
        DWORD ty = 0, sl = 0;
        _ObjectectInfo oi;
        memset(&oi, 0, sizeof(oi));
        if (!_ObjectDecodeSlotHandle(v216Objects[i], &ty, &sl) || ty != v216Types[i] || sl >= _OBJECT_SLOT_STRIDE ||
            !_ObjectGetInfo(v216Objects[i], &oi) || oi.type != v216Types[i]) v216SlotOk = 0;
    }
    DWORD svcType = 0, svcSlot = 0;
    _ObjectectInfo svcOi;
    memset(&svcOi, 0, sizeof(svcOi));
    BOOL v216ServiceSlotOk = v216Svc && _ObjectDecodeSlotHandle((HANDLE)v216Svc, &svcType, &svcSlot) &&
                             svcType == _OBJECT_TYPE_SERVICE && svcSlot < _OBJECT_SLOT_STRIDE &&
                             _ObjectGetInfo((HANDLE)v216Svc, &svcOi) && svcOi.type == _OBJECT_TYPE_SERVICE;
    smoke_expect(&s, v216SlotOk && v216ServiceSlotOk,
                 "v216 unified object-slot handles", "Section/Timer/Token/Process/Thread/Service join Event/Mutex/Semaphore on slot-coded ObjectHandles");

    enum { V216_OBJECT_SLOT_PROBES = 8192 };
    int v216LookupOk = v216SlotOk && v216ServiceSlotOk;
    unsigned long long v216SlotStart = smoke_perf_now_us();
    for (int probe = 0; v216LookupOk && probe < V216_OBJECT_SLOT_PROBES; ++probe) {
        for (int i = 0; i < 8; ++i) {
            _ObjectectInfo oi;
            if (!_ObjectGetInfo(v216Objects[i], &oi) || oi.type != v216Types[i]) { v216LookupOk = 0; break; }
        }
        _ObjectectInfo soi;
        if (!_ObjectGetInfo((HANDLE)v216Svc, &soi) || soi.type != _OBJECT_TYPE_SERVICE) v216LookupOk = 0;
    }
    unsigned long long v216SlotEnd = smoke_perf_now_us();
    int v216SlotOps = V216_OBJECT_SLOT_PROBES * 9;
    smoke_expect(&s, v216LookupOk,
                 "v216 unified direct object-slot lookup", "All major kernel-object families resolve through ObjectId -> array slot without type-table scans");
    char v216SlotDetail[320];
    double v216SlotMs = (double)(v216SlotEnd - v216SlotStart) / 1000.0;
    double v216SlotOpsS = v216SlotMs > 0.0 ? ((double)v216SlotOps * 1000.0 / v216SlotMs) : 0.0;
    snprintf(v216SlotDetail, sizeof(v216SlotDetail),
             "probes=%d lookups=%d wall_ms=%.3f ops_s=%.0f section_obj=0x%x timer_obj=0x%x token_obj=0x%x process_obj=0x%x thread_obj=0x%x service_obj=0x%x",
             V216_OBJECT_SLOT_PROBES, v216SlotOps, v216SlotMs, v216SlotOpsS,
             (unsigned)v216SectionInfo.object_handle, (unsigned)v216TimerInfo.object_handle,
             (unsigned)v216TokenInfo.object_handle, (unsigned)pseudoProcInfo.object_handle,
             (unsigned)pseudoThreadInfo.object_handle, (unsigned)(HANDLE)v216Svc);
    smoke_info(s.group, "v216 unified object-slot lookup benchmark", v216SlotDetail);

    HANDLE hV217PseudoProc = 0, hV217PseudoThread = 0;
    DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &hV217PseudoProc, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &hV217PseudoThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    HANDLE v217Handles[8] = { v214Event, v214Mutex, v214Sem, v216Section, v216Timer, v216Token, hV217PseudoProc, hV217PseudoThread };
    DWORD  v217Types[8] = { _OBJECT_TYPE_EVENT, _OBJECT_TYPE_MUTEX, _OBJECT_TYPE_SEMAPHORE, _OBJECT_TYPE_SECTION, _OBJECT_TYPE_TIMER, _OBJECT_TYPE_TOKEN, _OBJECT_TYPE_PROCESS, _OBJECT_TYPE_THREAD };
    int v217SlotCacheOk = 1;
    int v217FailIndex = -1;
    DWORD v217FailHaveType = 0, v217FailExpectedType = 0, v217FailSlot = 0, v217FailCachedSlot = 0, v217FailDecodedType = 0;
    for (int i = 0; v217SlotCacheOk && i < 8; ++i) {
        MyHandleInfo hi;
        _ObjectectInfo oi;
        DWORD ty = 0, sl = 0;
        memset(&hi, 0, sizeof(hi));
        memset(&oi, 0, sizeof(oi));
        BOOL diagOk = v217Handles[i] && MyGetHandleInfo(v217Handles[i], &hi);
        BOOL decOk = diagOk && _ObjectDecodeSlotHandle(hi.object_handle, &ty, &sl);
        BOOL objOk = diagOk && _ObjectGetInfo(hi.object_handle, &oi);
        if (!diagOk || hi.object_type != v217Types[i] || !decOk || ty != v217Types[i] || hi.object_slot != sl ||
            !objOk || oi.type != v217Types[i]) {
            v217SlotCacheOk = 0;
            v217FailIndex = i;
            v217FailHaveType = hi.object_type;
            v217FailExpectedType = v217Types[i];
            v217FailSlot = sl;
            v217FailCachedSlot = hi.object_slot;
            v217FailDecodedType = ty;
        }
    }
    char v217CheckDetail[256];
    if (v217SlotCacheOk) {
        snprintf(v217CheckDetail, sizeof(v217CheckDetail), "checked=8 event/mutex/semaphore/section/timer/token/process/thread public handles");
    } else {
        snprintf(v217CheckDetail, sizeof(v217CheckDetail), "idx=%d haveType=%lu expectedType=%lu decodedType=%lu decodedSlot=%lu cachedSlot=%lu",
                 v217FailIndex, (unsigned long)v217FailHaveType, (unsigned long)v217FailExpectedType,
                 (unsigned long)v217FailDecodedType, (unsigned long)v217FailSlot, (unsigned long)v217FailCachedSlot);
    }
    smoke_expect(&s, v217SlotCacheOk,
                 "v217 public HANDLE entries cache object slots", v217CheckDetail);

    enum { V217_HANDLE_SLOT_PROBES = 4096 };
    int v217LookupOk = v217SlotCacheOk;
    unsigned long long v217Start = smoke_perf_now_us();
    for (int probe = 0; v217LookupOk && probe < V217_HANDLE_SLOT_PROBES; ++probe) {
        for (int i = 0; i < 8; ++i) {
            MyHandleInfo hi;
            DWORD ty = 0, sl = 0;
            if (!MyGetHandleInfo(v217Handles[i], &hi) || hi.object_type != v217Types[i] ||
                !_ObjectDecodeSlotHandle(hi.object_handle, &ty, &sl) || hi.object_slot != sl) {
                v217LookupOk = 0;
                break;
            }
        }
    }
    unsigned long long v217End = smoke_perf_now_us();
    int v217Ops = V217_HANDLE_SLOT_PROBES * 8;
    smoke_expect(&s, v217LookupOk,
                 "v217 handle-slot diagnostic lookup", "diagnostic public HANDLE lookup resolves to cached object slot without semantic drift");
    MyHandleInfo v217EventDiag, v217ProcDiag, v217ThreadDiag;
    memset(&v217EventDiag, 0, sizeof(v217EventDiag));
    memset(&v217ProcDiag, 0, sizeof(v217ProcDiag));
    memset(&v217ThreadDiag, 0, sizeof(v217ThreadDiag));
    MyGetHandleInfo(v214Event, &v217EventDiag);
    MyGetHandleInfo(hV217PseudoProc, &v217ProcDiag);
    MyGetHandleInfo(hV217PseudoThread, &v217ThreadDiag);
    char v217Detail[256];
    double v217Ms = (double)(v217End - v217Start) / 1000.0;
    double v217OpsS = v217Ms > 0.0 ? ((double)v217Ops * 1000.0 / v217Ms) : 0.0;
    snprintf(v217Detail, sizeof(v217Detail), "probes=%d lookups=%d wall_ms=%.3f ops_s=%.0f event_slot=%lu process_slot=%lu thread_slot=%lu service_slot=%lu",
             V217_HANDLE_SLOT_PROBES, v217Ops, v217Ms, v217OpsS,
             (unsigned long)v217EventDiag.object_slot,
             (unsigned long)v217ProcDiag.object_slot,
             (unsigned long)v217ThreadDiag.object_slot,
             (unsigned long)svcSlot);
    smoke_info(s.group, "v217 public handle-slot cache benchmark", v217Detail);


    HANDLE v218Event = CreateEventA(NULL, TRUE, FALSE, NULL);
    MyHandleInfo v218Hi;
    _ObjectectHeader v218Hdr;
    DWORD v218Ty = 0, v218Slot = 0, v218Gen = 0;
    memset(&v218Hi, 0, sizeof(v218Hi));
    memset(&v218Hdr, 0, sizeof(v218Hdr));
    BOOL v218HeaderOk = v218Event && MyGetHandleInfo(v218Event, &v218Hi) &&
                        _ObjectDecodeObjectId(v218Hi.object_handle, &v218Ty, &v218Slot, &v218Gen) &&
                        _ObjectQueryObjectHeader(v218Hi.object_handle, &v218Hdr) &&
                        v218Hdr.object_type == _OBJECT_TYPE_EVENT &&
                        v218Hdr.object_slot == (_OBJECT_TYPE_EVENT * _OBJECT_SLOT_STRIDE + v218Slot) &&
                        v218Hdr.object_generation == v218Gen &&
                        v218Hdr.object_state == _OBJECT_OBJECT_STATE_LIVE &&
                        v218Hdr.pointer_count >= 1 && v218Hdr.handle_count >= 1;
    smoke_expect(&s, v218HeaderOk,
                 "v218 object header uses slot/generation/state nomenclature",
                 "ObjectId decodes to OBJECT_HEADER-style metadata: Type, Slot, Generation, State, PointerCount, HandleCount");

    HANDLE v218OldObject = v218Hi.object_handle;
    DWORD v218OldGen = v218Gen;
    DWORD v218OldSlot = v218Slot;
    if (v218Event) CloseHandle(v218Event);
    _ObjectectInfo v218OldInfo;
    memset(&v218OldInfo, 0, sizeof(v218OldInfo));
    BOOL v218OldInvalid = v218OldObject && !_ObjectGetInfo(v218OldObject, &v218OldInfo);
    HANDLE v218Event2 = CreateEventA(NULL, TRUE, FALSE, NULL);
    MyHandleInfo v218Hi2;
    DWORD v218Ty2 = 0, v218Slot2 = 0, v218Gen2 = 0;
    memset(&v218Hi2, 0, sizeof(v218Hi2));
    BOOL v218GenerationOk = v218OldInvalid && v218Event2 && MyGetHandleInfo(v218Event2, &v218Hi2) &&
                            _ObjectDecodeObjectId(v218Hi2.object_handle, &v218Ty2, &v218Slot2, &v218Gen2) &&
                            v218Ty2 == _OBJECT_TYPE_EVENT &&
                            (v218Hi2.object_handle != v218OldObject) &&
                            (v218Slot2 != v218OldSlot || v218Gen2 != v218OldGen);
    char v218GenDetail[256];
    snprintf(v218GenDetail, sizeof(v218GenDetail), "old_obj=0x%x old_slot=%lu old_gen=%lu new_obj=0x%x new_slot=%lu new_gen=%lu old_invalid=%d",
             (unsigned)v218OldObject, (unsigned long)v218OldSlot, (unsigned long)v218OldGen,
             (unsigned)v218Hi2.object_handle, (unsigned long)v218Slot2, (unsigned long)v218Gen2,
             v218OldInvalid ? 1 : 0);
    smoke_expect(&s, v218GenerationOk,
                 "v218 ObjectId generation invalidates stale object numbers", v218GenDetail);

    enum { V218_HEADER_PROBES = 4096 };
    int v218LookupOk = v218HeaderOk && v218GenerationOk;
    unsigned long long v218Start = smoke_perf_now_us();
    for (int probe = 0; v218LookupOk && probe < V218_HEADER_PROBES; ++probe) {
        _ObjectectHeader oh;
        if (!_ObjectQueryObjectHeader(v218Hi2.object_handle, &oh) ||
            oh.object_type != _OBJECT_TYPE_EVENT || oh.object_state != _OBJECT_OBJECT_STATE_LIVE) {
            v218LookupOk = 0;
            break;
        }
    }
    unsigned long long v218End = smoke_perf_now_us();
    smoke_expect(&s, v218LookupOk,
                 "v218 ObjectId -> ObjectHeader direct dispatch",
                 "ObjectId indexes an OBJECT_HEADER-style slot carrying state/meta/payload identity");
    char v218Detail[256];
    double v218Ms = (double)(v218End - v218Start) / 1000.0;
    double v218OpsS = v218Ms > 0.0 ? ((double)V218_HEADER_PROBES * 1000.0 / v218Ms) : 0.0;
    snprintf(v218Detail, sizeof(v218Detail), "probes=%d wall_ms=%.3f ops_s=%.0f object=0x%x slot=%lu generation=%lu state=%lu handle_count=%lu",
             V218_HEADER_PROBES, v218Ms, v218OpsS, (unsigned)v218Hi2.object_handle,
             (unsigned long)(_OBJECT_TYPE_EVENT * _OBJECT_SLOT_STRIDE + v218Slot2),
             (unsigned long)v218Gen2, (unsigned long)_OBJECT_OBJECT_STATE_LIVE,
             (unsigned long)v218Hi2.object_handle_count);
    smoke_info(s.group, "v218 object header dispatch benchmark", v218Detail);
    if (v218Event2) CloseHandle(v218Event2);

    if (hV217PseudoThread) CloseHandle(hV217PseudoThread);
    if (hV217PseudoProc) CloseHandle(hV217PseudoProc);
    if (v216Token) CloseHandle(v216Token);
    if (v216Timer) CloseHandle(v216Timer);
    if (v216Section) CloseHandle(v216Section);
    if (v216Svc) { DeleteService(v216Svc); CloseServiceHandle(v216Svc); }
    if (v216Scm) CloseServiceHandle(v216Scm);

    if (v214Event) CloseHandle(v214Event);
    if (v214Mutex) CloseHandle(v214Mutex);
    if (v214Sem) CloseHandle(v214Sem);

    HANDLE hSem = CreateSemaphoreA(NULL, 0, 2, semName);
    MyHandleInfo semInfo;
    memset(&semInfo, 0, sizeof(semInfo));
    smoke_expect(&s, hSem && MyGetHandleInfo(hSem, &semInfo),
                 "strict_handles semaphore raw object discovered", "diagnostic path only");
    SetLastError(0x12345678u);
    LONG prev = -1;
    smoke_expect(&s, semInfo.object_handle && ReleaseSemaphore(semInfo.object_handle, 1, &prev) == FALSE,
                 "raw semaphore rejected by ReleaseSemaphore", "public semaphore modify requires table handle");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "ReleaseSemaphore(raw semaphore) LastError");
    smoke_expect(&s, hSem && ReleaseSemaphore(hSem, 1, &prev) && prev == 0,
                 "table semaphore ReleaseSemaphore still works", "valid process handle can modify semaphore");
    smoke_expect(&s, hSem && WaitForSingleObject(hSem, 0) == WAIT_OBJECT_0,
                 "table semaphore WaitForSingleObject still works", "released count is consumable");

    HANDLE hMutex = CreateMutexA(NULL, TRUE, mutexName);
    MyHandleInfo mutexInfo;
    memset(&mutexInfo, 0, sizeof(mutexInfo));
    smoke_expect(&s, hMutex && MyGetHandleInfo(hMutex, &mutexInfo),
                 "strict_handles mutex raw object discovered", "diagnostic path only");
    SetLastError(0x12345678u);
    smoke_expect(&s, mutexInfo.object_handle && ReleaseMutex(mutexInfo.object_handle) == FALSE,
                 "raw mutex rejected by ReleaseMutex", "public mutex release requires table handle");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "ReleaseMutex(raw mutex) LastError");
    smoke_expect(&s, hMutex && ReleaseMutex(hMutex),
                 "table mutex ReleaseMutex still works", "owner can release through valid handle");

    HANDLE hTimer = CreateWaitableTimerA(NULL, FALSE, timerName);
    MyHandleInfo timerInfo;
    memset(&timerInfo, 0, sizeof(timerInfo));
    smoke_expect(&s, hTimer && MyGetHandleInfo(hTimer, &timerInfo),
                 "strict_handles timer raw object discovered", "diagnostic path only");
    LARGE_INTEGER due;
    due.QuadPart = -10000ll;
    SetLastError(0x12345678u);
    smoke_expect(&s, timerInfo.object_handle && SetWaitableTimer(timerInfo.object_handle, &due, 0, NULL, NULL, FALSE) == FALSE,
                 "raw timer rejected by SetWaitableTimer", "public timer modify requires table handle");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "SetWaitableTimer(raw timer) LastError");
    smoke_expect(&s, hTimer && SetWaitableTimer(hTimer, &due, 0, NULL, NULL, FALSE),
                 "table timer SetWaitableTimer still works", "valid waitable timer handle remains usable");
    smoke_expect(&s, hTimer && WaitForSingleObject(hTimer, 50) == WAIT_OBJECT_0,
                 "table timer WaitForSingleObject still works", "timer becomes signaled");

    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, sectionName);
    MyHandleInfo mapInfo;
    memset(&mapInfo, 0, sizeof(mapInfo));
    smoke_expect(&s, hMap && MyGetHandleInfo(hMap, &mapInfo),
                 "strict_handles section raw object discovered", "diagnostic path only");
    SetLastError(0x12345678u);
    smoke_expect(&s, mapInfo.object_handle && MapViewOfFile(mapInfo.object_handle, FILE_MAP_READ, 0, 0, 4096) == NULL,
                 "raw section rejected by MapViewOfFile", "public map path requires table handle");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "MapViewOfFile(raw section) LastError");
    char* view = hMap ? (char*)MapViewOfFile(hMap, FILE_MAP_READ|FILE_MAP_WRITE, 0, 0, 4096) : NULL;
    smoke_expect(&s, view != NULL, "table section MapViewOfFile still works", "valid process handle maps normally");
    if (view) {
        strcpy(view, "strict-handles-ok");
        UnmapViewOfFile(view);
    }

    if (hMap) CloseHandle(hMap);
    if (hTimer) CloseHandle(hTimer);
    if (hMutex) CloseHandle(hMutex);
    if (hSem) CloseHandle(hSem);
    if (hEvent) CloseHandle(hEvent);
    MyWinSetStrictKernelHandles(oldStrict);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

typedef struct SmokeWaitSignalCtx {
    SmokeRuntime* rt;
    HANDLE h;
    unsigned delay_ms;
    int op;
    int ok;
} SmokeWaitSignalCtx;

static unsigned long long smoke_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000ull);
}

static void* smoke_wait_signal_thread(void* arg)
{
    SmokeWaitSignalCtx* ctx = (SmokeWaitSignalCtx*)arg;
    if (!ctx || !ctx->rt) return NULL;
    MyWinBindRuntime(&ctx->rt->mgr, &ctx->rt->cap);
    MyWinBindDesktop(&ctx->rt->wm);
    if (ctx->delay_ms) usleep((useconds_t)ctx->delay_ms * 1000u);
    if (ctx->op == 1) {
        ctx->ok = SetEvent(ctx->h) ? 1 : 0;
    } else if (ctx->op == 2) {
        LONG prev = -1;
        ctx->ok = ReleaseSemaphore(ctx->h, 1, &prev) ? 1 : 0;
    } else if (ctx->op == 3) {
        ctx->ok = ReleaseMutex(ctx->h) ? 1 : 0;
    }
    MyWinUnbindRuntime();
    return NULL;
}

typedef struct SmokeWfmoWaitCtx {
    SmokeRuntime* rt;
    HANDLE handles[2];
    DWORD count;
    DWORD timeout_ms;
    DWORD result;
    int ok;
} SmokeWfmoWaitCtx;

static void* smoke_wfmo_wait_thread(void* arg)
{
    SmokeWfmoWaitCtx* ctx = (SmokeWfmoWaitCtx*)arg;
    if (!ctx || !ctx->rt || !ctx->count) return NULL;
    MyWinBindRuntime(&ctx->rt->mgr, &ctx->rt->cap);
    MyWinBindDesktop(&ctx->rt->wm);
    ctx->result = WaitForMultipleObjects(ctx->count, ctx->handles, FALSE, ctx->timeout_ms);
    ctx->ok = 1;
    MyWinUnbindRuntime();
    return NULL;
}

typedef struct SmokeTerminateProcessCtx {
    SmokeRuntime* rt;
    HANDLE hProcess;
    unsigned delay_ms;
    UINT exit_code;
    int ok;
} SmokeTerminateProcessCtx;

static void* smoke_terminate_process_thread(void* arg)
{
    SmokeTerminateProcessCtx* ctx = (SmokeTerminateProcessCtx*)arg;
    if (!ctx || !ctx->rt || !ctx->hProcess) return NULL;
    MyWinBindRuntime(&ctx->rt->mgr, &ctx->rt->cap);
    MyWinBindDesktop(&ctx->rt->wm);
    if (ctx->delay_ms) usleep((useconds_t)ctx->delay_ms * 1000u);
    ctx->ok = TerminateProcess(ctx->hProcess, ctx->exit_code) ? 1 : 0;
    MyWinUnbindRuntime();
    return NULL;
}

typedef struct SmokeMsgPostCtx {
    SmokeRuntime* rt;
    HWND hwnd;
    UINT msg;
    WPARAM wp;
    LPARAM lp;
    unsigned delay_ms;
    int ok;
} SmokeMsgPostCtx;

static void* smoke_msg_post_thread(void* arg)
{
    SmokeMsgPostCtx* ctx = (SmokeMsgPostCtx*)arg;
    if (!ctx || !ctx->rt) return NULL;
    MyWinBindRuntime(&ctx->rt->mgr, &ctx->rt->cap);
    MyWinBindDesktop(&ctx->rt->wm);
    if (ctx->delay_ms) usleep((useconds_t)ctx->delay_ms * 1000u);
    ctx->ok = PostMessageA(ctx->hwnd, ctx->msg, ctx->wp, ctx->lp) ? 1 : 0;
    /* Do not MyWinUnbindRuntime() here: the current USER32 runtime stores
       manager/desktop pointers globally while capability is TLS-ish.  Existing
       worker waits can rebind for their own operation, but unbinding after a
       PostMessage would race the main WaitMessage/PeekMessage smoke. */
    return NULL;
}

static void smoke_drain_current_queue(void)
{
    MSG msg;
    int guard = 256;
    while (guard-- > 0 && PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        /* drain only */
    }
}

static int smoke_wait_real(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "wait_real";
    smoke_runtime_init(rt);

    char eventName[96], semName[96], eventAllName[96], semAllName[96], timerName[96], autoName[96], semCommitName[96];
    smoke_unique_name(eventName, sizeof(eventName), "v148.wait.event");
    smoke_unique_name(semName, sizeof(semName), "v148.wait.sem");
    smoke_unique_name(eventAllName, sizeof(eventAllName), "v148.wait.all.event");
    smoke_unique_name(semAllName, sizeof(semAllName), "v148.wait.all.sem");
    smoke_unique_name(timerName, sizeof(timerName), "v148.wait.timer");
    smoke_unique_name(autoName, sizeof(autoName), "v148.wait.commit.event");
    smoke_unique_name(semCommitName, sizeof(semCommitName), "v148.wait.commit.sem");

    HANDLE hEvent = CreateEventA(NULL, FALSE, FALSE, eventName);
    smoke_expect(&s, hEvent != 0, "CreateEventA(auto,nonsignaled)", eventName);
    SmokeWaitSignalCtx evCtx = { rt, hEvent, 25, 1, 0 };
    pthread_t evThread;
    pthread_create(&evThread, NULL, smoke_wait_signal_thread, &evCtx);
    unsigned long long evStart = smoke_mono_ms();
    DWORD evWait = WaitForSingleObject(hEvent, 500);
    unsigned long long evElapsed = smoke_mono_ms() - evStart;
    pthread_join(evThread, NULL);
    smoke_expect(&s, evCtx.ok == 1, "SetEvent worker", "worker signaled through same process handle table");
    smoke_expect(&s, evWait == WAIT_OBJECT_0 && evElapsed >= 10 && evElapsed < 250,
                 "WaitForSingleObject(event) cond wake", "dispatcher condvar wakes blocked waiter");

    HANDLE hSem = CreateSemaphoreA(NULL, 0, 1, semName);
    smoke_expect(&s, hSem != 0, "CreateSemaphoreA(0,1)", semName);
    SmokeWaitSignalCtx semCtx = { rt, hSem, 25, 2, 0 };
    pthread_t semThread;
    pthread_create(&semThread, NULL, smoke_wait_signal_thread, &semCtx);
    unsigned long long semStart = smoke_mono_ms();
    DWORD semWait = WaitForSingleObject(hSem, 500);
    unsigned long long semElapsed = smoke_mono_ms() - semStart;
    pthread_join(semThread, NULL);
    smoke_expect(&s, semCtx.ok == 1, "ReleaseSemaphore worker", "release signals dispatcher condvar");
    smoke_expect(&s, semWait == WAIT_OBJECT_0 && semElapsed >= 10 && semElapsed < 250,
                 "WaitForSingleObject(semaphore) cond wake", "semaphore release wakes blocked waiter");

    HANDLE hAllEvent = CreateEventA(NULL, FALSE, FALSE, eventAllName);
    HANDLE hAllSem = CreateSemaphoreA(NULL, 0, 1, semAllName);
    HANDLE allHandles[2] = { hAllEvent, hAllSem };
    smoke_expect(&s, hAllEvent != 0 && hAllSem != 0, "create WAIT_ALL pair", "auto-reset event + zero-count semaphore");
    SmokeWaitSignalCtx allSemCtx = { rt, hAllSem, 20, 2, 0 };
    SmokeWaitSignalCtx allEvCtx = { rt, hAllEvent, 45, 1, 0 };
    pthread_t allSemThread, allEvThread;
    pthread_create(&allSemThread, NULL, smoke_wait_signal_thread, &allSemCtx);
    pthread_create(&allEvThread, NULL, smoke_wait_signal_thread, &allEvCtx);
    DWORD allWait = WaitForMultipleObjects(2, allHandles, TRUE, 500);
    pthread_join(allSemThread, NULL);
    pthread_join(allEvThread, NULL);
    smoke_expect(&s, allSemCtx.ok == 1 && allEvCtx.ok == 1, "WAIT_ALL workers", "both objects became signaled asynchronously");
    smoke_expect(&s, allWait == WAIT_OBJECT_0, "WaitForMultipleObjects(WAIT_ALL) cond wake", "returns only after both objects are ready");
    smoke_expect(&s, WaitForMultipleObjects(2, allHandles, FALSE, 0) == WAIT_TIMEOUT,
                 "WAIT_ALL commit consumed objects", "auto-reset event and semaphore count consumed atomically under dispatcher lock");

    HANDLE hCommitEvent = CreateEventA(NULL, FALSE, TRUE, autoName);
    HANDLE hCommitSem = CreateSemaphoreA(NULL, 1, 1, semCommitName);
    HANDLE commitHandles[2] = { hCommitEvent, hCommitSem };
    smoke_expect(&s, hCommitEvent != 0 && hCommitSem != 0, "create immediate WAIT_ALL pair", "both objects start ready");
    smoke_expect(&s, WaitForMultipleObjects(2, commitHandles, TRUE, 0) == WAIT_OBJECT_0,
                 "WAIT_ALL scan+commit immediate", "ready pair succeeds without polling loop");
    smoke_expect(&s, WaitForMultipleObjects(2, commitHandles, FALSE, 0) == WAIT_TIMEOUT,
                 "WAIT_ALL scan+commit drains consumables", "post-commit pair is no longer ready");

    HANDLE hTimer = CreateWaitableTimerA(NULL, FALSE, timerName);
    LARGE_INTEGER due;
    due.QuadPart = -300000ll; /* 30 ms relative */
    smoke_expect(&s, hTimer != 0 && SetWaitableTimer(hTimer, &due, 0, NULL, NULL, FALSE),
                 "SetWaitableTimer(relative 30ms)", "timer waiter uses condvar timedwait deadline");
    unsigned long long timerStart = smoke_mono_ms();
    DWORD timerWait = hTimer ? WaitForSingleObject(hTimer, 500) : WAIT_FAILED;
    unsigned long long timerElapsed = smoke_mono_ms() - timerStart;
    smoke_expect(&s, timerWait == WAIT_OBJECT_0 && timerElapsed >= 10 && timerElapsed < 250,
                 "WaitForSingleObject(timer) timed cond wake", "wait wakes at timer due time without usleep spin");

    char dupAutoName[96];
    smoke_unique_name(dupAutoName, sizeof(dupAutoName), "v187.wait.dup.auto");
    HANDLE hAuto = CreateEventA(NULL, FALSE, FALSE, dupAutoName);
    HANDLE hAutoDup = 0;
    smoke_expect(&s, hAuto && DuplicateHandle(GetCurrentProcess(), hAuto, GetCurrentProcess(), &hAutoDup, 0, FALSE, DUPLICATE_SAME_ACCESS),
                 "DuplicateHandle(auto-reset event for wait)", "two process handles reference one waitable object");
    HANDLE dupWaits[2] = { hAuto, hAutoDup };
    smoke_expect(&s, hAuto && SetEvent(hAuto), "SetEvent(auto-reset duplicate target)", "signal once through original handle");
    DWORD dupWait = (hAuto && hAutoDup) ? WaitForMultipleObjects(2, dupWaits, FALSE, 0) : WAIT_FAILED;
    DWORD dupWait2 = (hAuto && hAutoDup) ? WaitForMultipleObjects(2, dupWaits, FALSE, 0) : WAIT_FAILED;
    smoke_expect(&s, dupWait < WAIT_OBJECT_0 + 2 && dupWait2 == WAIT_TIMEOUT,
                 "auto-reset duplicated handles consume once", "same object visible through two handles but one signal commits once");

    /* Worker threads in the wait smoke bind/unbind runtime around KERNEL waits.
       Rebind the primary smoke capability before entering USER32 message-queue
       tests so CreateWindowExA/PostMessageA observe the intended UI owner. */
    MyWinBindRuntime(&rt->mgr, &rt->cap);
    MyWinBindDesktop(&rt->wm);

    WNDCLASSEXA msgWc;
    memset(&msgWc, 0, sizeof(msgWc));
    msgWc.cbSize = sizeof(msgWc);
    msgWc.lpfnWndProc = smoke_wndproc;
    msgWc.lpszClassName = "myOS.v188.MsgWaitWindow";
    ATOM msgAtom = RegisterClassExA(&msgWc);
    HWND hMsgWnd = msgAtom ? CreateWindowExA(0, msgWc.lpszClassName, "MsgWait", WS_OVERLAPPEDWINDOW,
                                             15, 15, 180, 100, 0, 0, 0, NULL) : 0;
    smoke_expect(&s, msgAtom != 0 && hMsgWnd != 0, "MsgWait smoke window", "message queue wait target created");
    smoke_drain_current_queue();

    DWORD msgTimeout = MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
    smoke_expect(&s, msgTimeout == WAIT_TIMEOUT,
                 "MsgWaitForMultipleObjects timeout", "empty current UI queue times out");

    smoke_expect(&s, hMsgWnd && PostMessageA(hMsgWnd, WM_USER + 188, 0x188u, 0x488u),
                 "PostMessageA for MsgWait", "posted message should wake queue wait");
    DWORD msgReady = MsgWaitForMultipleObjects(0, NULL, FALSE, 0, QS_ALLINPUT);
    smoke_expect(&s, msgReady == WAIT_OBJECT_0,
                 "MsgWaitForMultipleObjects posted message", "nCount=0 returns WAIT_OBJECT_0 for queue input");
    MSG msgWaitMsg;
    memset(&msgWaitMsg, 0, sizeof(msgWaitMsg));
    BOOL msgPeek = PeekMessageA(&msgWaitMsg, hMsgWnd, WM_USER + 188, WM_USER + 188, PM_REMOVE);
    smoke_expect(&s, msgPeek && msgWaitMsg.message == WM_USER + 188 && msgWaitMsg.wParam == 0x188u,
                 "MsgWait does not consume message", "PeekMessage removes the message after MsgWait returned");

    HANDLE hMsgEvent = CreateEventA(NULL, FALSE, FALSE, "v188.msgwait.event");
    smoke_drain_current_queue();
    smoke_expect(&s, hMsgEvent && SetEvent(hMsgEvent), "SetEvent for MsgWait", "handle ready before queue input");
    HANDLE msgHandles[1] = { hMsgEvent };
    DWORD msgHandleFirst = hMsgEvent ? MsgWaitForMultipleObjects(1, msgHandles, FALSE, 0, QS_ALLINPUT) : WAIT_FAILED;
    smoke_expect(&s, msgHandleFirst == WAIT_OBJECT_0,
                 "MsgWaitForMultipleObjects handle before message", "ready waitable keeps WAIT_OBJECT_0+index semantics");

    smoke_expect(&s, hMsgWnd && PostMessageA(hMsgWnd, WM_USER + 189, 0x189u, 0),
                 "PostMessageA queue-index case", "queue message without ready handle");
    DWORD msgIndex = hMsgEvent ? MsgWaitForMultipleObjects(1, msgHandles, FALSE, 0, QS_ALLINPUT) : WAIT_FAILED;
    smoke_expect(&s, msgIndex == WAIT_OBJECT_0 + 1,
                 "MsgWaitForMultipleObjects message index", "message readiness returns WAIT_OBJECT_0+nCount");
    memset(&msgWaitMsg, 0, sizeof(msgWaitMsg));
    smoke_expect(&s, PeekMessageA(&msgWaitMsg, hMsgWnd, WM_USER + 189, WM_USER + 189, PM_REMOVE) && msgWaitMsg.message == WM_USER + 189,
                 "MsgWait message index not consumed", "message remains queued after MsgWait return");

    smoke_drain_current_queue();
    SmokeMsgPostCtx postCtx = { rt, hMsgWnd, WM_USER + 190, 0x190u, 0, 25, 0 };
    pthread_t postThread;
    pthread_create(&postThread, NULL, smoke_msg_post_thread, &postCtx);
    unsigned long long wmStart = smoke_mono_ms();
    BOOL waitMsgOk = WaitMessage();
    unsigned long long wmElapsed = smoke_mono_ms() - wmStart;
    pthread_join(postThread, NULL);
    smoke_expect(&s, postCtx.ok == 1, "WaitMessage post worker", "worker posts into current UI-thread queue");
    smoke_expect(&s, waitMsgOk && wmElapsed >= 10 && wmElapsed < 250,
                 "WaitMessage blocks then wakes", "WaitMessage returns when a message becomes available");
    memset(&msgWaitMsg, 0, sizeof(msgWaitMsg));
    smoke_expect(&s, PeekMessageA(&msgWaitMsg, hMsgWnd, WM_USER + 190, WM_USER + 190, PM_REMOVE) && msgWaitMsg.message == WM_USER + 190,
                 "WaitMessage does not consume message", "message still retrieved by PeekMessage after WaitMessage");

    smoke_expect(&s, hMsgWnd && PostMessageA(hMsgWnd, WM_USER + 191, 0x191u, 0),
                 "PostMessageA MsgWaitEx", "posted message for Ex path");
    DWORD msgEx = MsgWaitForMultipleObjectsEx(0, NULL, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    smoke_expect(&s, msgEx == WAIT_OBJECT_0,
                 "MsgWaitForMultipleObjectsEx(MWMO_INPUTAVAILABLE)", "Ex path reports queued input without consuming it");
    memset(&msgWaitMsg, 0, sizeof(msgWaitMsg));
    smoke_expect(&s, PeekMessageA(&msgWaitMsg, hMsgWnd, WM_USER + 191, WM_USER + 191, PM_REMOVE) && msgWaitMsg.message == WM_USER + 191,
                 "MsgWaitForMultipleObjectsEx not consumed", "queued message remains for PeekMessage");

    if (hMsgEvent) CloseHandle(hMsgEvent);
    if (hMsgWnd) DestroyWindow(hMsgWnd);

    char abandonName[96];
    smoke_unique_name(abandonName, sizeof(abandonName), "v187.wait.abandoned.mutex");
    PROCESS_INFORMATION abandonInfo;
    memset(&abandonInfo, 0, sizeof(abandonInfo));
    Capability abandonChildTemplate = cap_create(0, "v187-abandon-child", CAP_IPC);
    BOOL abandonProc = MyWinCreateProcessWithStartupCapability("v187-abandon-child", NULL, &abandonChildTemplate, FALSE, ".", NULL, &abandonInfo);
    HANDLE hChildMutex = 0;
    HANDLE hParentMutex = 0;
    if (abandonProc) {
        Capability abandonChildRuntime = cap_create(abandonInfo.dwProcessId, "v187-abandon-child", CAP_IPC);
        MyWinBindRuntime(&rt->mgr, &abandonChildRuntime);
        MyWinBindDesktop(&rt->wm);
        hChildMutex = CreateMutexA(NULL, TRUE, abandonName);
        MyWinBindRuntime(&rt->mgr, &rt->cap);
        MyWinBindDesktop(&rt->wm);
        hParentMutex = OpenMutexA(SYNCHRONIZE|MUTEX_MODIFY_STATE, FALSE, abandonName);
    }
    DWORD abandonBefore = hParentMutex ? WaitForSingleObject(hParentMutex, 0) : WAIT_FAILED;
    if (abandonInfo.hProcess) TerminateProcess(abandonInfo.hProcess, 0x1870);
    MyWinSweepExitedHandleTables();
    DWORD abandonAfter = hParentMutex ? WaitForSingleObject(hParentMutex, 0) : WAIT_FAILED;
    smoke_expect(&s, abandonProc && hChildMutex && hParentMutex && abandonBefore == WAIT_TIMEOUT && abandonAfter == WAIT_ABANDONED,
                 "mutex abandoned on owner process exit", "exited owner releases mutex as abandoned and next waiter acquires it");
    if (hParentMutex && abandonAfter == WAIT_ABANDONED) ReleaseMutex(hParentMutex);
    if (hParentMutex) CloseHandle(hParentMutex);
    if (abandonInfo.hThread) CloseHandle(abandonInfo.hThread);
    if (abandonInfo.hProcess) CloseHandle(abandonInfo.hProcess);

    char benchEventName[96];
    smoke_unique_name(benchEventName, sizeof(benchEventName), "v244.wait.targeted.bench");
    HANDLE hBenchEvent = CreateEventA(NULL, TRUE, TRUE, benchEventName);
    unsigned long long targetedBenchStart = smoke_perf_now_us();
    DWORD targetedBenchOk = 0;
    for (DWORD i = 0; i < 8192; i++) {
        if (hBenchEvent && WaitForSingleObject(hBenchEvent, 0) == WAIT_OBJECT_0) targetedBenchOk++;
    }
    unsigned long long targetedBenchUs = smoke_perf_now_us() - targetedBenchStart;
    double targetedOps = targetedBenchUs ? (8192.0 * 1000000.0) / (double)targetedBenchUs : 0.0;
    char targetedBenchDetail[192];
    snprintf(targetedBenchDetail, sizeof(targetedBenchDetail),
             "probes=8192 ok=%u wall_us=%llu ops_s=%.0f",
             targetedBenchOk, targetedBenchUs, targetedOps);
    smoke_expect(&s, targetedBenchOk == 8192,
                 "v244 targeted WaitForSingleObject event benchmark", targetedBenchDetail);
    if (hBenchEvent) CloseHandle(hBenchEvent);

    char wfmoAnyEventName[96], wfmoAnySemName[96];
    smoke_unique_name(wfmoAnyEventName, sizeof(wfmoAnyEventName), "v245.wait.multi.any.event");
    smoke_unique_name(wfmoAnySemName, sizeof(wfmoAnySemName), "v245.wait.multi.any.sem");
    HANDLE hWfmoAnyEvent = CreateEventA(NULL, FALSE, FALSE, wfmoAnyEventName);
    HANDLE hWfmoAnySem = CreateSemaphoreA(NULL, 0, 1, wfmoAnySemName);
    HANDLE wfmoAnyHandles[2] = { hWfmoAnyEvent, hWfmoAnySem };
    SmokeWaitSignalCtx wfmoAnyCtx = { rt, hWfmoAnySem, 15, 2, 0 };
    pthread_t wfmoAnyThread;
    pthread_create(&wfmoAnyThread, NULL, smoke_wait_signal_thread, &wfmoAnyCtx);
    unsigned long long wfmoAnyStart = smoke_mono_ms();
    DWORD wfmoAny = (hWfmoAnyEvent && hWfmoAnySem) ? WaitForMultipleObjects(2, wfmoAnyHandles, FALSE, 500) : WAIT_FAILED;
    unsigned long long wfmoAnyElapsed = smoke_mono_ms() - wfmoAnyStart;
    pthread_join(wfmoAnyThread, NULL);
    char wfmoAnyDetail[192];
    snprintf(wfmoAnyDetail, sizeof(wfmoAnyDetail),
             "result=0x%x elapsed_ms=%llu worker=%d", wfmoAny, wfmoAnyElapsed, wfmoAnyCtx.ok);
    smoke_expect(&s, wfmoAny == WAIT_OBJECT_0 + 1 && wfmoAnyCtx.ok == 1 && wfmoAnyElapsed >= 5 && wfmoAnyElapsed < 250,
                 "v245 targeted WaitForMultipleObjects WAIT_ANY waiter", wfmoAnyDetail);
    if (hWfmoAnySem) CloseHandle(hWfmoAnySem);
    if (hWfmoAnyEvent) CloseHandle(hWfmoAnyEvent);

    enum { V246_WFMO_BUCKET_WAITERS = 6 };
    HANDLE v246Events[V246_WFMO_BUCKET_WAITERS];
    HANDLE v246Sems[V246_WFMO_BUCKET_WAITERS];
    pthread_t v246Threads[V246_WFMO_BUCKET_WAITERS];
    SmokeWfmoWaitCtx v246Ctx[V246_WFMO_BUCKET_WAITERS];
    memset(v246Events, 0, sizeof(v246Events));
    memset(v246Sems, 0, sizeof(v246Sems));
    memset(v246Threads, 0, sizeof(v246Threads));
    memset(v246Ctx, 0, sizeof(v246Ctx));
    MyWaitAudit v246Before;
    memset(&v246Before, 0, sizeof(v246Before));
    MyWinGetWaitAudit(&v246Before);
    int v246SetupOk = 1;
    for (int i = 0; i < V246_WFMO_BUCKET_WAITERS; ++i) {
        char en[96], sn[96], eventSuffix[48], semSuffix[48];
        snprintf(eventSuffix, sizeof(eventSuffix), "v246.wait.bucket.event.%d", i);
        snprintf(semSuffix, sizeof(semSuffix), "v246.wait.bucket.sem.%d", i);
        smoke_unique_name(en, sizeof(en), eventSuffix);
        smoke_unique_name(sn, sizeof(sn), semSuffix);
        v246Events[i] = CreateEventA(NULL, FALSE, FALSE, en);
        v246Sems[i] = CreateSemaphoreA(NULL, 0, 1, sn);
        v246Ctx[i].rt = rt;
        v246Ctx[i].handles[0] = v246Events[i];
        v246Ctx[i].handles[1] = v246Sems[i];
        v246Ctx[i].count = 2;
        v246Ctx[i].timeout_ms = 1000;
        v246Ctx[i].result = WAIT_FAILED;
        if (!v246Events[i] || !v246Sems[i] ||
            pthread_create(&v246Threads[i], NULL, smoke_wfmo_wait_thread, &v246Ctx[i]) != 0) {
            v246SetupOk = 0;
            break;
        }
    }
    if (v246SetupOk) usleep(25000u);
    LONG v246Prev = -1;
    if (v246SetupOk) v246SetupOk = ReleaseSemaphore(v246Sems[3], 1, &v246Prev) ? 1 : 0;
    if (v246SetupOk) usleep(25000u);
    for (int i = 0; i < V246_WFMO_BUCKET_WAITERS; ++i) {
        if (v246Sems[i] && i != 3) {
            LONG ignoredPrev = -1;
            ReleaseSemaphore(v246Sems[i], 1, &ignoredPrev);
        }
    }
    for (int i = 0; i < V246_WFMO_BUCKET_WAITERS; ++i) {
        if (v246Threads[i]) pthread_join(v246Threads[i], NULL);
    }
    MyWaitAudit v246After;
    memset(&v246After, 0, sizeof(v246After));
    MyWinGetWaitAudit(&v246After);
    int v246ResultsOk = v246SetupOk;
    for (int i = 0; i < V246_WFMO_BUCKET_WAITERS; ++i) {
        if (!v246Ctx[i].ok || v246Ctx[i].result != WAIT_OBJECT_0 + 1) v246ResultsOk = 0;
    }
    DWORD v246Wakes = v246After.wait_multiple_targeted_wakes - v246Before.wait_multiple_targeted_wakes;
    DWORD v246ObjectWakes = v246After.wait_multiple_waitblock_object_wakes - v246Before.wait_multiple_waitblock_object_wakes;
    DWORD v246Links = v246After.wait_multiple_waitblock_links - v246Before.wait_multiple_waitblock_links;
    DWORD v246Resolved = v246After.wait_multiple_resolved_probes - v246Before.wait_multiple_resolved_probes;
    char v246WaitBlockDetail[256];
    snprintf(v246WaitBlockDetail, sizeof(v246WaitBlockDetail),
             "waiters=%d wakes=%u objectWakes=%u links=%u resolvedProbes=%u prev=%ld",
             V246_WFMO_BUCKET_WAITERS, v246Wakes, v246ObjectWakes, v246Links,
             v246Resolved, (long)v246Prev);
    smoke_expect(&s, v246ResultsOk && v246Wakes >= V246_WFMO_BUCKET_WAITERS &&
                    v246ObjectWakes >= V246_WFMO_BUCKET_WAITERS && v246Links >= v246ObjectWakes &&
                    v246Resolved >= V246_WFMO_BUCKET_WAITERS,
                 "v246 targeted WFMO per-object WaitBlock fanout", v246WaitBlockDetail);
    for (int i = 0; i < V246_WFMO_BUCKET_WAITERS; ++i) {
        if (v246Sems[i]) CloseHandle(v246Sems[i]);
        if (v246Events[i]) CloseHandle(v246Events[i]);
    }

    MyWaitAudit v247Before;
    memset(&v247Before, 0, sizeof(v247Before));
    MyWinGetWaitAudit(&v247Before);

    PROCESS_INFORMATION v247SinglePi;
    memset(&v247SinglePi, 0, sizeof(v247SinglePi));
    Capability v247SingleCap = cap_create(0, "v247-single-wait-child", CAP_IPC);
    BOOL v247SingleProc = MyWinCreateProcessWithStartupCapability("v247-single-wait-child", NULL, &v247SingleCap, FALSE, ".", NULL, &v247SinglePi);
    SmokeTerminateProcessCtx v247SingleCtx = { rt, v247SinglePi.hProcess, 15, 247u, 0 };
    pthread_t v247SingleThread;
    int v247SingleThreadOk = v247SingleProc ? (pthread_create(&v247SingleThread, NULL, smoke_terminate_process_thread, &v247SingleCtx) == 0) : 0;
    unsigned long long v247SingleStart = smoke_mono_ms();
    DWORD v247SingleWait = (v247SingleProc && v247SingleThreadOk) ? WaitForSingleObject(v247SinglePi.hProcess, 500) : WAIT_FAILED;
    unsigned long long v247SingleElapsed = smoke_mono_ms() - v247SingleStart;
    if (v247SingleThreadOk) pthread_join(v247SingleThread, NULL);
    DWORD v247SingleExit = STILL_ACTIVE;
    if (v247SinglePi.hProcess) GetExitCodeProcess(v247SinglePi.hProcess, &v247SingleExit);
    MyWaitAudit v247AfterSingle;
    memset(&v247AfterSingle, 0, sizeof(v247AfterSingle));
    MyWinGetWaitAudit(&v247AfterSingle);
    DWORD v247SingleTargets = v247AfterSingle.wait_process_thread_targeted - v247Before.wait_process_thread_targeted;
    DWORD v247SingleWakes = v247AfterSingle.wait_process_thread_object_wakes - v247Before.wait_process_thread_object_wakes;
    char v247SingleDetail[256];
    snprintf(v247SingleDetail, sizeof(v247SingleDetail),
             "result=0x%x elapsed_ms=%llu worker=%d exit=%u ptTarget=%u ptWake=%u",
             v247SingleWait, v247SingleElapsed, v247SingleCtx.ok, v247SingleExit, v247SingleTargets, v247SingleWakes);
    smoke_expect(&s, v247SingleProc && v247SingleThreadOk && v247SingleWait == WAIT_OBJECT_0 &&
                    v247SingleCtx.ok == 1 && v247SingleExit == 247u &&
                    v247SingleTargets >= 1 && v247SingleWakes >= 1,
                 "v247 targeted WaitForSingleObject(process) WaitBlock", v247SingleDetail);
    if (v247SinglePi.hThread) CloseHandle(v247SinglePi.hThread);
    if (v247SinglePi.hProcess) CloseHandle(v247SinglePi.hProcess);

    PROCESS_INFORMATION v247MultiPi;
    memset(&v247MultiPi, 0, sizeof(v247MultiPi));
    Capability v247MultiCap = cap_create(0, "v247-multi-wait-child", CAP_IPC);
    BOOL v247MultiProc = MyWinCreateProcessWithStartupCapability("v247-multi-wait-child", NULL, &v247MultiCap, FALSE, ".", NULL, &v247MultiPi);
    char v247MultiEventName[96];
    smoke_unique_name(v247MultiEventName, sizeof(v247MultiEventName), "v247.wait.process.event");
    HANDLE v247MultiEvent = CreateEventA(NULL, FALSE, FALSE, v247MultiEventName);
    HANDLE v247MultiHandles[2] = { v247MultiEvent, v247MultiPi.hProcess };
    MyWaitAudit v247BeforeMulti;
    memset(&v247BeforeMulti, 0, sizeof(v247BeforeMulti));
    MyWinGetWaitAudit(&v247BeforeMulti);
    SmokeTerminateProcessCtx v247MultiCtx = { rt, v247MultiPi.hProcess, 15, 248u, 0 };
    pthread_t v247MultiThread;
    int v247MultiThreadOk = (v247MultiProc && v247MultiEvent) ? (pthread_create(&v247MultiThread, NULL, smoke_terminate_process_thread, &v247MultiCtx) == 0) : 0;
    unsigned long long v247MultiStart = smoke_mono_ms();
    DWORD v247MultiWait = (v247MultiProc && v247MultiEvent && v247MultiThreadOk) ? WaitForMultipleObjects(2, v247MultiHandles, FALSE, 500) : WAIT_FAILED;
    unsigned long long v247MultiElapsed = smoke_mono_ms() - v247MultiStart;
    if (v247MultiThreadOk) pthread_join(v247MultiThread, NULL);
    MyWaitAudit v247AfterMulti;
    memset(&v247AfterMulti, 0, sizeof(v247AfterMulti));
    MyWinGetWaitAudit(&v247AfterMulti);
    DWORD v247MultiTargets = v247AfterMulti.wait_process_thread_targeted - v247BeforeMulti.wait_process_thread_targeted;
    DWORD v247MultiWakes = v247AfterMulti.wait_process_thread_object_wakes - v247BeforeMulti.wait_process_thread_object_wakes;
    DWORD v247MultiLinks = v247AfterMulti.wait_multiple_waitblock_links - v247BeforeMulti.wait_multiple_waitblock_links;
    char v247MultiDetail[256];
    snprintf(v247MultiDetail, sizeof(v247MultiDetail),
             "result=0x%x elapsed_ms=%llu worker=%d ptTarget=%u ptWake=%u links=%u",
             v247MultiWait, v247MultiElapsed, v247MultiCtx.ok, v247MultiTargets, v247MultiWakes, v247MultiLinks);
    smoke_expect(&s, v247MultiProc && v247MultiEvent && v247MultiThreadOk &&
                    v247MultiWait == WAIT_OBJECT_0 + 1 && v247MultiCtx.ok == 1 &&
                    v247MultiTargets >= 1 && v247MultiWakes >= 1 && v247MultiLinks >= 2,
                 "v247 targeted WaitForMultipleObjects(process) WaitBlock", v247MultiDetail);
    if (v247MultiEvent) CloseHandle(v247MultiEvent);
    if (v247MultiPi.hThread) CloseHandle(v247MultiPi.hThread);
    if (v247MultiPi.hProcess) CloseHandle(v247MultiPi.hProcess);

    char v248ImmediateEventName[96];
    char v248ImmediateSemName[96];
    smoke_unique_name(v248ImmediateEventName, sizeof(v248ImmediateEventName), "v248.wait.immediate.event");
    smoke_unique_name(v248ImmediateSemName, sizeof(v248ImmediateSemName), "v248.wait.immediate.sem");
    HANDLE v248ImmediateEvent = CreateEventA(NULL, FALSE, TRUE, v248ImmediateEventName);
    HANDLE v248ImmediateSem = CreateSemaphoreA(NULL, 0, 1, v248ImmediateSemName);
    HANDLE v248ImmediateHandles[2] = { v248ImmediateEvent, v248ImmediateSem };
    MyWaitAudit v248BeforeImmediate;
    memset(&v248BeforeImmediate, 0, sizeof(v248BeforeImmediate));
    MyWinGetWaitAudit(&v248BeforeImmediate);
    DWORD v248ImmediateWait = (v248ImmediateEvent && v248ImmediateSem) ? WaitForMultipleObjects(2, v248ImmediateHandles, FALSE, 500) : WAIT_FAILED;
    MyWaitAudit v248AfterImmediate;
    memset(&v248AfterImmediate, 0, sizeof(v248AfterImmediate));
    MyWinGetWaitAudit(&v248AfterImmediate);
    DWORD v248ImmediateHits = v248AfterImmediate.wait_multiple_immediate_hits - v248BeforeImmediate.wait_multiple_immediate_hits;
    DWORD v248ImmediateLinks = v248AfterImmediate.wait_multiple_waitblock_links - v248BeforeImmediate.wait_multiple_waitblock_links;
    DWORD v248ImmediateDeferred = v248AfterImmediate.wait_multiple_deferred_links - v248BeforeImmediate.wait_multiple_deferred_links;
    char v248ImmediateDetail[256];
    snprintf(v248ImmediateDetail, sizeof(v248ImmediateDetail),
             "result=0x%x immediate=%u links=%u deferred=%u",
             v248ImmediateWait, v248ImmediateHits, v248ImmediateLinks, v248ImmediateDeferred);
    smoke_expect(&s, v248ImmediateEvent && v248ImmediateSem && v248ImmediateWait == WAIT_OBJECT_0 &&
                    v248ImmediateHits >= 1 && v248ImmediateLinks == 0 && v248ImmediateDeferred == 0,
                 "v248 targeted WFMO immediate native fast path skips WaitBlocks", v248ImmediateDetail);
    if (v248ImmediateSem) CloseHandle(v248ImmediateSem);
    if (v248ImmediateEvent) CloseHandle(v248ImmediateEvent);

    PROCESS_INFORMATION v250ImmediatePi;
    memset(&v250ImmediatePi, 0, sizeof(v250ImmediatePi));
    Capability v250ImmediateCap = cap_create(0, "v250-immediate-process-child", CAP_IPC);
    BOOL v250ImmediateProc = MyWinCreateProcessWithStartupCapability("v250-immediate-process-child", NULL, &v250ImmediateCap, FALSE, ".", NULL, &v250ImmediatePi);
    char v250ImmediateEventName[96];
    smoke_unique_name(v250ImmediateEventName, sizeof(v250ImmediateEventName), "v250.wait.immediate.process.event");
    HANDLE v250ImmediateEvent = CreateEventA(NULL, FALSE, FALSE, v250ImmediateEventName);
    if (v250ImmediateProc && v250ImmediatePi.hProcess) TerminateProcess(v250ImmediatePi.hProcess, 250u);
    HANDLE v250ImmediateHandles[2] = { v250ImmediateEvent, v250ImmediatePi.hProcess };
    MyWaitAudit v250BeforeImmediate;
    memset(&v250BeforeImmediate, 0, sizeof(v250BeforeImmediate));
    MyWinGetWaitAudit(&v250BeforeImmediate);
    DWORD v250ImmediateWait = (v250ImmediateProc && v250ImmediateEvent && v250ImmediatePi.hProcess) ? WaitForMultipleObjects(2, v250ImmediateHandles, FALSE, 500) : WAIT_FAILED;
    MyWaitAudit v250AfterImmediate;
    memset(&v250AfterImmediate, 0, sizeof(v250AfterImmediate));
    MyWinGetWaitAudit(&v250AfterImmediate);
    DWORD v250ImmediateHits = v250AfterImmediate.wait_multiple_immediate_hits - v250BeforeImmediate.wait_multiple_immediate_hits;
    DWORD v250ProcessImmediate = v250AfterImmediate.wait_process_thread_immediate_hits - v250BeforeImmediate.wait_process_thread_immediate_hits;
    DWORD v250ImmediateLinks = v250AfterImmediate.wait_multiple_waitblock_links - v250BeforeImmediate.wait_multiple_waitblock_links;
    DWORD v250ImmediateDeferred = v250AfterImmediate.wait_multiple_deferred_links - v250BeforeImmediate.wait_multiple_deferred_links;
    char v250ImmediateDetail[256];
    snprintf(v250ImmediateDetail, sizeof(v250ImmediateDetail),
             "result=0x%x immediate=%u ptImmediate=%u links=%u deferred=%u",
             v250ImmediateWait, v250ImmediateHits, v250ProcessImmediate, v250ImmediateLinks, v250ImmediateDeferred);
    smoke_expect(&s, v250ImmediateProc && v250ImmediateEvent && v250ImmediatePi.hProcess &&
                    v250ImmediateWait == WAIT_OBJECT_0 + 1 && v250ImmediateHits >= 1 &&
                    v250ProcessImmediate >= 1 && v250ImmediateLinks == 0 && v250ImmediateDeferred == 0,
                 "v250 targeted WFMO immediate process fast path skips WaitBlocks", v250ImmediateDetail);
    if (v250ImmediateEvent) CloseHandle(v250ImmediateEvent);
    if (v250ImmediatePi.hThread) CloseHandle(v250ImmediatePi.hThread);
    if (v250ImmediatePi.hProcess) CloseHandle(v250ImmediatePi.hProcess);

    MyProcessIndexAudit v254ProcBefore, v254ProcAfter;
    memset(&v254ProcBefore, 0, sizeof(v254ProcBefore));
    memset(&v254ProcAfter, 0, sizeof(v254ProcAfter));
    MyWinGetProcessIndexAudit(&v254ProcBefore);
    PROCESS_INFORMATION v254Pi;
    memset(&v254Pi, 0, sizeof(v254Pi));
    Capability v254Cap = cap_create(0, "v254-process-index-child", CAP_IPC);
    BOOL v254Created = MyWinCreateProcessWithStartupCapability("v254-process-index-child", NULL, &v254Cap, FALSE, ".", NULL, &v254Pi);
    enum { V254_PROCESS_INDEX_PROBES = 1024 };
    int v254Ok = v254Created ? 1 : 0;
    int v254Ops = 0;
    unsigned long long v254Start = smoke_perf_now_us();
    for (int i = 0; v254Ok && i < V254_PROCESS_INDEX_PROBES; ++i) {
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, v254Pi.dwProcessId);
        HANDLE ht = OpenThread(THREAD_QUERY_INFORMATION, FALSE, v254Pi.dwThreadId);
        MyProcessLiteInfo pli;
        DWORD code = 0;
        memset(&pli, 0, sizeof(pli));
        if (!hp || !ht || !MyGetProcessLiteInfo(v254Pi.dwProcessId, &pli) || !GetExitCodeProcess(v254Pi.hProcess, &code)) v254Ok = 0;
        if (hp) CloseHandle(hp);
        if (ht) CloseHandle(ht);
        v254Ops++;
    }
    unsigned long long v254End = smoke_perf_now_us();
    if (v254Pi.hProcess) TerminateProcess(v254Pi.hProcess, 254u);
    if (v254Pi.hThread) CloseHandle(v254Pi.hThread);
    if (v254Pi.hProcess) CloseHandle(v254Pi.hProcess);
    MyWinGetProcessIndexAudit(&v254ProcAfter);
    DWORD v254PidHits = v254ProcAfter.pid_hash_hits - v254ProcBefore.pid_hash_hits;
    DWORD v254TidHits = v254ProcAfter.tid_hash_hits - v254ProcBefore.tid_hash_hits;
    DWORD v254PidMisses = v254ProcAfter.pid_hash_misses - v254ProcBefore.pid_hash_misses;
    DWORD v254TidMisses = v254ProcAfter.tid_hash_misses - v254ProcBefore.tid_hash_misses;
    DWORD v254AllocFast = v254ProcAfter.alloc_fast - v254ProcBefore.alloc_fast;
    smoke_expect(&s, v254Ok && v254Ops == V254_PROCESS_INDEX_PROBES &&
                    v254PidHits >= (DWORD)(V254_PROCESS_INDEX_PROBES * 2) &&
                    v254TidHits >= (DWORD)V254_PROCESS_INDEX_PROBES &&
                    v254AllocFast >= 1,
                 "v254 ProcessLite PID/TID hash index",
                 "OpenProcess/OpenThread/GetExitCodeProcess resolve ProcessLite through hash-indexed PID/TID buckets");
    char v254ProcessDetail[320];
    double v254Ms = (double)(v254End - v254Start) / 1000.0;
    double v254OpsS = v254Ms > 0.0 ? ((double)v254Ops * 1000.0 / v254Ms) : 0.0;
    snprintf(v254ProcessDetail, sizeof(v254ProcessDetail),
             "probes=%d wall_ms=%.3f ops_s=%.0f pidHit=%lu tidHit=%lu pidMiss=%lu tidMiss=%lu allocFast=%lu fallbackScans=%lu live=%lu exited=%lu",
             v254Ops, v254Ms, v254OpsS,
             (unsigned long)v254PidHits, (unsigned long)v254TidHits,
             (unsigned long)v254PidMisses, (unsigned long)v254TidMisses,
             (unsigned long)v254AllocFast,
             (unsigned long)(v254ProcAfter.fallback_scans - v254ProcBefore.fallback_scans),
             (unsigned long)v254ProcAfter.live_records, (unsigned long)v254ProcAfter.exited_records);
    smoke_info(s.group, "v254 ProcessLite hash-index benchmark", v254ProcessDetail);

    MyWaitAudit waitAudit;
    memset(&waitAudit, 0, sizeof(waitAudit));
    MyWinGetWaitAudit(&waitAudit);
    char waitAuditDetail[704];
    snprintf(waitAuditDetail, sizeof(waitAuditDetail),
             "success=%u timeout=%u any=%u all=%u event=%u sem=%u timer=%u abandoned=%u singleTarget=%u singleFallback=%u multiTarget=%u multiFallback=%u multiWake=%u objectWake=%u links=%u immediate=%u deferred=%u tlsGate=%u dhInit=%u dhHead=%u dhStore=%u dhFast=%u resolvedProbe=%u ptTarget=%u ptImmediate=%u ptPoll=%u ptWake=%u bcast=%u skip=%u fail=%u",
             waitAudit.wait_success, waitAudit.wait_timeouts, waitAudit.wait_any_commits, waitAudit.wait_all_commits,
             waitAudit.event_consumes, waitAudit.semaphore_consumes, waitAudit.timer_consumes,
             waitAudit.mutex_abandoned, waitAudit.wait_single_targeted, waitAudit.wait_single_global_fallback,
             waitAudit.wait_multiple_targeted, waitAudit.wait_multiple_global_fallback, waitAudit.wait_multiple_targeted_wakes,
             waitAudit.wait_multiple_waitblock_object_wakes, waitAudit.wait_multiple_waitblock_links,
             waitAudit.wait_multiple_immediate_hits, waitAudit.wait_multiple_deferred_links,
             waitAudit.wait_multiple_tls_gates, waitAudit.wait_dispatcher_header_inits,
             waitAudit.wait_dispatcher_header_head_hits, waitAudit.wait_dispatcher_header_state_stores,
             waitAudit.wait_dispatcher_header_fast_not_ready,
             waitAudit.wait_multiple_resolved_probes, waitAudit.wait_process_thread_targeted,
             waitAudit.wait_process_thread_immediate_hits, waitAudit.wait_process_thread_poll_slices,
             waitAudit.wait_process_thread_object_wakes, waitAudit.wake_broadcasts,
             waitAudit.wake_skips, waitAudit.wait_failures);
    smoke_expect(&s, waitAudit.wait_all_commits > 0 && waitAudit.wait_any_commits > 0 &&
                     waitAudit.event_consumes > 0 && waitAudit.semaphore_consumes > 0 &&
                     waitAudit.timer_consumes > 0 && waitAudit.mutex_abandoned > 0 &&
                     waitAudit.wait_single_targeted > 0 && waitAudit.wait_multiple_targeted > 0 &&
                     waitAudit.wait_multiple_targeted_wakes > 0 &&
                     waitAudit.wait_multiple_waitblock_object_wakes > 0 &&
                     waitAudit.wait_multiple_waitblock_links >= waitAudit.wait_multiple_waitblock_object_wakes &&
                     waitAudit.wait_multiple_immediate_hits > 0 && waitAudit.wait_multiple_tls_gates > 0 &&
                     waitAudit.wait_dispatcher_header_inits > 0 && waitAudit.wait_dispatcher_header_head_hits > 0 &&
                     waitAudit.wait_dispatcher_header_state_stores > 0 && waitAudit.wait_dispatcher_header_fast_not_ready > 0 &&
                     waitAudit.wait_multiple_resolved_probes > 0 && waitAudit.wait_process_thread_targeted > 0 &&
                     waitAudit.wait_process_thread_object_wakes > 0 && waitAudit.wake_skips > 0,
                 "waitable audit counters reflect real commits and targeted waits", waitAuditDetail);

    if (hAutoDup) CloseHandle(hAutoDup);
    if (hAuto) CloseHandle(hAuto);

    if (hTimer) CloseHandle(hTimer);
    if (hCommitSem) CloseHandle(hCommitSem);
    if (hCommitEvent) CloseHandle(hCommitEvent);
    if (hAllSem) CloseHandle(hAllSem);
    if (hAllEvent) CloseHandle(hAllEvent);
    if (hSem) CloseHandle(hSem);
    if (hEvent) CloseHandle(hEvent);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_last_error(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "last_error";
    smoke_runtime_init(rt);

    SetLastError(0x13572468u);
    smoke_expect(&s, GetLastError() == 0x13572468u, "SetLastError/GetLastError", "sentinel roundtrip");
    SetLastError(0x12345678u);
    smoke_expect(&s, OpenEventA(EVENT_ALL_ACCESS, FALSE, "Global\\myos.v129.missing.event") == 0,
                 "OpenEventA missing", "missing named object fails");
    smoke_expect_last_error(&s, ERROR_FILE_NOT_FOUND, "OpenEventA missing LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\myos.v129.missing.section") == 0,
                 "OpenFileMappingA missing", "missing named section fails");
    smoke_expect_last_error(&s, ERROR_FILE_NOT_FOUND, "OpenFileMappingA missing LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0, "Global\\myos.v129.zero.section") == 0,
                 "CreateFileMappingA zero size", "invalid size fails");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "CreateFileMappingA zero size LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, MapViewOfFile((HANDLE)0, FILE_MAP_READ, 0, 0, 0) == NULL,
                 "MapViewOfFile(NULL)", "invalid handle fails");
    smoke_expect_last_error(&s, ERROR_INVALID_HANDLE, "MapViewOfFile(NULL) LastError");

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


#define SMOKE_V196_CDLG_ID_FILENAME 0x3E83u
#define SMOKE_V197_CUSTOM_ID        0x4CF1u

static int g_v196_ofn_hook_init = 0;
static int g_v196_cf_hook_init = 0;
static int g_v197_ofn_initdone = 0;
static int g_v197_ofn_fileok = 0;
static int g_v197_ofn_fileok_veto_once = 0;
static int g_v197_ofn_typechange = 0;
static int g_v197_cdm_folder_ok = 0;
static int g_v197_template_custom_seen = 0;

typedef struct SmokeV197ComdlgHookCtx {
    int mode;
    const char* spec;
    const char* defExt;
} SmokeV197ComdlgHookCtx;

static UINT_PTR CALLBACK smoke_v196_ofn_hook(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    if (uMsg == WM_INITDIALOG) {
        OPENFILENAMEA* ofn = (OPENFILENAMEA*)lParam;
        SmokeV197ComdlgHookCtx* ctx = ofn ? (SmokeV197ComdlgHookCtx*)ofn->lCustData : NULL;
        g_v196_ofn_hook_init++;
        if (ctx && ctx->defExt) SendMessageA(hDlg, CDM_SETDEFEXT, 0, (LPARAM)ctx->defExt);
        if (ctx && ctx->spec) SetDlgItemTextA(hDlg, SMOKE_V196_CDLG_ID_FILENAME, ctx->spec);
        if (GetDlgItem(hDlg, SMOKE_V197_CUSTOM_ID)) g_v197_template_custom_seen++;
        PostMessageA(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, 0), (LPARAM)GetDlgItem(hDlg, IDOK));
        return 0;
    }
    if (uMsg == WM_NOTIFY && lParam) {
        OFNOTIFYA* no = (OFNOTIFYA*)lParam;
        SmokeV197ComdlgHookCtx* ctx = (no && no->lpOFN) ? (SmokeV197ComdlgHookCtx*)no->lpOFN->lCustData : NULL;
        if (no->hdr.code == CDN_INITDONE) {
            char folder[MAX_PATH];
            g_v197_ofn_initdone++;
            folder[0] = 0;
            SendMessageA(hDlg, CDM_GETFOLDERPATH, sizeof(folder), (LPARAM)folder);
            if (folder[0]) g_v197_cdm_folder_ok++;
        } else if (no->hdr.code == CDN_FILEOK) {
            g_v197_ofn_fileok++;
            if (ctx && ctx->mode == 3 && !g_v197_ofn_fileok_veto_once) {
                g_v197_ofn_fileok_veto_once++;
                SetDlgItemTextA(hDlg, SMOKE_V196_CDLG_ID_FILENAME, ctx->spec ? ctx->spec : "sample");
                PostMessageA(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, 0), (LPARAM)GetDlgItem(hDlg, IDOK));
                return 1;
            }
        } else if (no->hdr.code == CDN_TYPECHANGE) {
            g_v197_ofn_typechange++;
        }
    }
    return 0;
}

static UINT_PTR CALLBACK smoke_v196_cf_hook(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    if (uMsg == WM_INITDIALOG) {
        g_v196_cf_hook_init++;
        if (GetDlgItem(hDlg, SMOKE_V197_CUSTOM_ID)) g_v197_template_custom_seen++;
        PostMessageA(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, 0), (LPARAM)GetDlgItem(hDlg, IDOK));
    }
    return 0;
}

static int smoke_v196_ends_with(const char* s, const char* suffix)
{
    size_t ns = s ? strlen(s) : 0;
    size_t nx = suffix ? strlen(suffix) : 0;
    return ns >= nx && strcmp(s + ns - nx, suffix) == 0;
}


#define SMOKE_V198_CFONT_ID_FONTLIST   0x3EA0u
#define SMOKE_V198_CFONT_ID_STYLELIST  0x3EA1u
#define SMOKE_V198_CFONT_ID_SIZELIST   0x3EA2u
#define SMOKE_V198_CFONT_ID_STRIKEOUT  0x3EA3u
#define SMOKE_V198_CFONT_ID_UNDERLINE  0x3EA4u
#define SMOKE_V198_CFONT_ID_COLOR      0x3EA5u
#define SMOKE_V198_CFONT_ID_SAMPLE     0x3EA6u
#define SMOKE_V198_CFONT_ID_STATUS     0x3EA7u

static void smoke_v198_align4(BYTE** pp)
{
    uintptr_t v = (uintptr_t)(*pp);
    v = (v + 3u) & ~(uintptr_t)3u;
    *pp = (BYTE*)v;
}
static void smoke_v198_w16(BYTE** pp, WORD v) { memcpy(*pp, &v, sizeof(v)); *pp += sizeof(v); }
static void smoke_v198_w32(BYTE** pp, DWORD v) { memcpy(*pp, &v, sizeof(v)); *pp += sizeof(v); }
static void smoke_v198_wstr(BYTE** pp, LPCSTR s)
{
    if (!s) s = "";
    while (*s) smoke_v198_w16(pp, (WORD)(unsigned char)*s++);
    smoke_v198_w16(pp, 0);
}
static void smoke_v198_ord(BYTE** pp, WORD atom)
{
    smoke_v198_w16(pp, 0xFFFFu);
    smoke_v198_w16(pp, atom);
}
static void smoke_v198_item(BYTE** pp, DWORD style, DWORD exStyle, short x, short y, short cx, short cy, WORD id, WORD clsAtom, LPCSTR title)
{
    smoke_v198_align4(pp);
    smoke_v198_w32(pp, style);
    smoke_v198_w32(pp, exStyle);
    smoke_v198_w16(pp, (WORD)x); smoke_v198_w16(pp, (WORD)y);
    smoke_v198_w16(pp, (WORD)cx); smoke_v198_w16(pp, (WORD)cy);
    smoke_v198_w16(pp, id);
    smoke_v198_ord(pp, clsAtom);
    smoke_v198_wstr(pp, title ? title : "");
    smoke_v198_w16(pp, 0);
}

static DWORD smoke_v198_build_ofn_template(BYTE* buf, DWORD cb)
{
    if (!buf || cb < 2048) return 0;
    memset(buf, 0, cb);
    BYTE* p = buf;
    smoke_v198_w32(&p, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    smoke_v198_w32(&p, 0);
    smoke_v198_w16(&p, 9);
    smoke_v198_w16(&p, 0); smoke_v198_w16(&p, 0);
    smoke_v198_w16(&p, 330); smoke_v198_w16(&p, 198);
    smoke_v198_w16(&p, 0); smoke_v198_w16(&p, 0);
    smoke_v198_wstr(&p, "v198 OFN custom template");
    smoke_v198_w16(&p, 8); smoke_v198_wstr(&p, "MS Shell Dlg");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|SS_LEFT, 0, 8, 8, 290, 10, SMOKE_V197_CUSTOM_ID, MYOS_DLG_CLASS_STATIC, "custom OFN template control");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY, 0, 8, 25, 308, 82, 0x3E82u, MYOS_DLG_CLASS_LISTBOX, "");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|SS_LEFT, 0, 8, 116, 56, 10, 0x3F11u, MYOS_DLG_CLASS_STATIC, "File &name:");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL|WS_BORDER, 0, 72, 113, 166, 14, SMOKE_V196_CDLG_ID_FILENAME, MYOS_DLG_CLASS_EDIT, "");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_GROUP|WS_TABSTOP|BS_DEFPUSHBUTTON, 0, 258, 112, 58, 16, IDOK, MYOS_DLG_CLASS_BUTTON, "&Open");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_BORDER, 0, 90, 134, 148, 14, 0x3E84u, MYOS_DLG_CLASS_COMBOBOX, "");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX, 0, 8, 162, 74, 12, 0x3E85u, MYOS_DLG_CLASS_BUTTON, "&Read only");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON, 0, 258, 134, 58, 16, IDCANCEL, MYOS_DLG_CLASS_BUTTON, "Cancel");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|SS_LEFT, 0, 8, 178, 308, 10, 0x3E86u, MYOS_DLG_CLASS_STATIC, "");
    return (DWORD)(p - buf);
}

static DWORD smoke_v198_build_font_template(BYTE* buf, DWORD cb)
{
    if (!buf || cb < 3072) return 0;
    memset(buf, 0, cb);
    BYTE* p = buf;
    smoke_v198_w32(&p, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    smoke_v198_w32(&p, 0);
    smoke_v198_w16(&p, 11);
    smoke_v198_w16(&p, 0); smoke_v198_w16(&p, 0);
    smoke_v198_w16(&p, 350); smoke_v198_w16(&p, 206);
    smoke_v198_w16(&p, 0); smoke_v198_w16(&p, 0);
    smoke_v198_wstr(&p, "v198 Font custom template");
    smoke_v198_w16(&p, 8); smoke_v198_wstr(&p, "MS Shell Dlg");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|SS_LEFT, 0, 8, 8, 150, 10, SMOKE_V197_CUSTOM_ID, MYOS_DLG_CLASS_STATIC, "custom font template control");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY, 0, 8, 20, 118, 86, SMOKE_V198_CFONT_ID_FONTLIST, MYOS_DLG_CLASS_LISTBOX, "");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|LBS_NOTIFY, 0, 134, 20, 74, 58, SMOKE_V198_CFONT_ID_STYLELIST, MYOS_DLG_CLASS_LISTBOX, "");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY, 0, 218, 20, 38, 58, SMOKE_V198_CFONT_ID_SIZELIST, MYOS_DLG_CLASS_LISTBOX, "");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_GROUP|WS_TABSTOP|BS_DEFPUSHBUTTON, 0, 272, 20, 58, 16, IDOK, MYOS_DLG_CLASS_BUTTON, "OK");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON, 0, 272, 42, 58, 16, IDCANCEL, MYOS_DLG_CLASS_BUTTON, "Cancel");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX, 0, 134, 102, 74, 12, SMOKE_V198_CFONT_ID_STRIKEOUT, MYOS_DLG_CLASS_BUTTON, "Stri&keout");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX, 0, 134, 118, 74, 12, SMOKE_V198_CFONT_ID_UNDERLINE, MYOS_DLG_CLASS_BUTTON, "&Underline");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_BORDER, 0, 218, 101, 74, 14, SMOKE_V198_CFONT_ID_COLOR, MYOS_DLG_CLASS_COMBOBOX, "");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|SS_CENTER|SS_CENTERIMAGE|WS_BORDER, 0, 8, 132, 250, 34, SMOKE_V198_CFONT_ID_SAMPLE, MYOS_DLG_CLASS_STATIC, "AaBbYyZz");
    smoke_v198_item(&p, WS_CHILD|WS_VISIBLE|SS_LEFT, 0, 8, 178, 322, 10, SMOKE_V198_CFONT_ID_STATUS, MYOS_DLG_CLASS_STATIC, "");
    return (DWORD)(p - buf);
}

static HGLOBAL smoke_v198_template_handle_from_blob(const BYTE* blob, DWORD cb)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, cb ? cb : 1);
    if (!h) return 0;
    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); return 0; }
    memcpy(p, blob, cb);
    GlobalUnlock(h);
    return h;
}

static int smoke_comdlg(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "comdlg";
    smoke_runtime_init(rt);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_wndproc;
    wc.lpszClassName = "myOS.v197.ComDlgOwner";
    RegisterClassExA(&wc);
    HWND owner = CreateWindowExA(0, "myOS.v197.ComDlgOwner", "v197 comdlg owner", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                 22, 22, 320, 180, 0, 0, 0, NULL);
    smoke_expect(&s, owner != 0, "Common dialog owner", "owner HWND for modal disable/restore and hook-driven dialogs");

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    SetLastError(0);
    BOOL ok = GetOpenFileNameA(&ofn);
    smoke_expect(&s, ok == FALSE, "GetOpenFileNameA invalid lStructSize", "must fail validation without entering modal loop");
    smoke_expect(&s, CommDlgExtendedError() == CDERR_STRUCTSIZE, "CommDlgExtendedError GetOpenFileNameA struct", "CDERR_STRUCTSIZE is reported exactly");

    char fileBuf[MAX_PATH];
    memset(fileBuf, 0, sizeof(fileBuf));
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.Flags = OFN_ENABLEHOOK;
    ok = GetOpenFileNameA(&ofn);
    smoke_expect(&s, ok == FALSE && CommDlgExtendedError() == CDERR_NOHOOK,
                 "GetOpenFileNameA OFN_ENABLEHOOK requires hook", "missing hook fails with CDERR_NOHOOK, not silent fallback");

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.Flags = OFN_ENABLETEMPLATE;
    ok = GetOpenFileNameA(&ofn);
    smoke_expect(&s, ok == FALSE && CommDlgExtendedError() == CDERR_NOHINSTANCE,
                 "GetOpenFileNameA template requires hInstance", "OFN_ENABLETEMPLATE uses module resources, not raw pointers or silent fallback");

    char tmpTemplate[] = "/tmp/myos_v196_comdlg_XXXXXX";
    char* tmpDir = mkdtemp(tmpTemplate);
    char samplePath[MAX_PATH];
    samplePath[0] = 0;
    if (tmpDir) {
        snprintf(samplePath, sizeof(samplePath), "%s/sample.txt", tmpDir);
        FILE* f = fopen(samplePath, "w");
        if (f) { fputs("v196", f); fclose(f); }
        char sample2[MAX_PATH];
        snprintf(sample2, sizeof(sample2), "%s/second.txt", tmpDir);
        f = fopen(sample2, "w");
        if (f) { fputs("v197", f); fclose(f); }
    }
    BYTE ofnTemplateBlob[3072];
    DWORD ofnTemplateSize = smoke_v198_build_ofn_template(ofnTemplateBlob, sizeof(ofnTemplateBlob));
    HMODULE hMainModule = GetModuleHandleA(NULL);
    BOOL regTpl = MyWinRegisterDialogTemplateResourceA(hMainModule, "SMOKE_V198_OFN_TEMPLATE", (LPCDLGTEMPLATEA)ofnTemplateBlob, ofnTemplateSize);
    HRSRC hOfnRes = FindResourceA(hMainModule, "SMOKE_V198_OFN_TEMPLATE", RT_DIALOG);
    HGLOBAL hOfnResData = hOfnRes ? LoadResource(hMainModule, hOfnRes) : 0;
    smoke_expect(&s, regTpl && hOfnRes && hOfnResData && LockResource(hOfnResData) == (LPVOID)ofnTemplateBlob && SizeofResource(hMainModule, hOfnRes) == ofnTemplateSize,
                 "resource dialog template handle model", "FindResource/LoadResource/LockResource/SizeofResource expose RT_DIALOG bytes by opaque handles");

    memset(fileBuf, 0, sizeof(fileBuf));
    snprintf(fileBuf, sizeof(fileBuf), "sample");
    char titleBuf[64];
    memset(titleBuf, 0, sizeof(titleBuf));
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrInitialDir = tmpDir ? tmpDir : ".";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrFileTitle = titleBuf;
    ofn.nMaxFileTitle = sizeof(titleBuf);
    ofn.lpstrDefExt = "txt";
    ofn.lpstrTitle = "v197 open smoke";
    ofn.Flags = OFN_ENABLEHOOK | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    SmokeV197ComdlgHookCtx openCtx = {1, "sample", NULL};
    ofn.lCustData = (LPARAM)&openCtx;
    ofn.lpfnHook = smoke_v196_ofn_hook;
    g_v196_ofn_hook_init = 0;
    g_v197_ofn_initdone = 0;
    g_v197_ofn_fileok = 0;
    g_v197_cdm_folder_ok = 0;
    ok = GetOpenFileNameA(&ofn);
    smoke_expect(&s, ok == TRUE && CommDlgExtendedError() == 0 && g_v196_ofn_hook_init == 1,
                 "GetOpenFileNameA hook-driven OK", "real #32770 runtime returns TRUE and clears extended error");
    smoke_expect(&s, g_v197_ofn_initdone == 1 && g_v197_ofn_fileok == 1 && g_v197_cdm_folder_ok >= 1,
                 "GetOpenFileNameA CDN_INITDONE/CDN_FILEOK/CDM_GETFOLDERPATH",
                 "Explorer-style hook sees WM_NOTIFY events and can query current folder via CDM message");
    smoke_expect(&s, smoke_v196_ends_with(fileBuf, "/sample.txt") && strcmp(titleBuf, "sample.txt") == 0,
                 "GetOpenFileNameA defext/title/offsets", "lpstrDefExt appends before FILEMUSTEXIST and lpstrFileTitle is filled");
    smoke_expect(&s, ofn.nFileOffset < strlen(fileBuf) && ofn.nFileExtension < strlen(fileBuf) &&
                     strcmp(fileBuf + ofn.nFileOffset, "sample.txt") == 0 && strcmp(fileBuf + ofn.nFileExtension, "txt") == 0,
                 "GetOpenFileNameA nFileOffset/nFileExtension", "offset fields point into returned lpstrFile");
    smoke_expect(&s, owner == 0 || IsWindowEnabled(owner),
                 "GetOpenFileNameA restores owner", "modal owner is re-enabled after OK");

    memset(fileBuf, 0, sizeof(fileBuf));
    snprintf(fileBuf, sizeof(fileBuf), "sample");
    memset(titleBuf, 0, sizeof(titleBuf));
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.hInstance = hMainModule;
    ofn.lpTemplateName = "SMOKE_V198_OFN_TEMPLATE";
    ofn.lpstrInitialDir = tmpDir ? tmpDir : ".";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrFileTitle = titleBuf;
    ofn.nMaxFileTitle = sizeof(titleBuf);
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    SmokeV197ComdlgHookCtx resTplCtx = {1, "sample", NULL};
    ofn.lCustData = (LPARAM)&resTplCtx;
    ofn.lpfnHook = smoke_v196_ofn_hook;
    g_v197_template_custom_seen = 0;
    ok = GetOpenFileNameA(&ofn);
    smoke_expect(&s, ok == TRUE && g_v197_template_custom_seen == 1 && smoke_v196_ends_with(fileBuf, "/sample.txt"),
                 "GetOpenFileNameA OFN_ENABLETEMPLATE resource", "module RT_DIALOG resource template creates custom controls and commits through common dialog runtime");

    HGLOBAL hOfnTemplate = smoke_v198_template_handle_from_blob(ofnTemplateBlob, ofnTemplateSize);
    memset(fileBuf, 0, sizeof(fileBuf));
    snprintf(fileBuf, sizeof(fileBuf), "sample");
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.hInstance = (HINSTANCE)hOfnTemplate;
    ofn.lpstrInitialDir = tmpDir ? tmpDir : ".";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_ENABLEHOOK | OFN_ENABLETEMPLATEHANDLE | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    SmokeV197ComdlgHookCtx handleTplCtx = {1, "sample", NULL};
    ofn.lCustData = (LPARAM)&handleTplCtx;
    ofn.lpfnHook = smoke_v196_ofn_hook;
    g_v197_template_custom_seen = 0;
    ok = GetOpenFileNameA(&ofn);
    smoke_expect(&s, ok == TRUE && g_v197_template_custom_seen == 1 && smoke_v196_ends_with(fileBuf, "/sample.txt"),
                 "GetOpenFileNameA OFN_ENABLETEMPLATEHANDLE", "HGLOBAL template handle is locked through the resource/template handle model, not cast as a pointer");
    if (hOfnTemplate) GlobalFree(hOfnTemplate);

    memset(fileBuf, 0, sizeof(fileBuf));
    snprintf(fileBuf, sizeof(fileBuf), "sample");
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrInitialDir = tmpDir ? tmpDir : ".";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_ENABLEHOOK | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    SmokeV197ComdlgHookCtx vetoCtx = {3, "sample", NULL};
    ofn.lCustData = (LPARAM)&vetoCtx;
    ofn.lpfnHook = smoke_v196_ofn_hook;
    g_v196_ofn_hook_init = 0;
    g_v197_ofn_fileok = 0;
    g_v197_ofn_fileok_veto_once = 0;
    ok = GetOpenFileNameA(&ofn);
    smoke_expect(&s, ok == TRUE && g_v197_ofn_fileok >= 2 && g_v197_ofn_fileok_veto_once == 1 && smoke_v196_ends_with(fileBuf, "/sample.txt"),
                 "GetOpenFileNameA CDN_FILEOK veto", "hook can veto first OK and allow a later valid OK");

    memset(fileBuf, 0, sizeof(fileBuf));
    snprintf(fileBuf, sizeof(fileBuf), "sample;second");
    memset(titleBuf, 0, sizeof(titleBuf));
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrInitialDir = tmpDir ? tmpDir : ".";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrFileTitle = titleBuf;
    ofn.nMaxFileTitle = sizeof(titleBuf);
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_ENABLEHOOK | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    SmokeV197ComdlgHookCtx multiCtx = {4, "sample;second", "txt"};
    ofn.lCustData = (LPARAM)&multiCtx;
    ofn.lpfnHook = smoke_v196_ofn_hook;
    ok = GetOpenFileNameA(&ofn);
    const char* multiFirst = fileBuf + strlen(fileBuf) + 1;
    const char* multiSecond = multiFirst + strlen(multiFirst) + 1;
    smoke_expect(&s, ok == TRUE && fileBuf[0] && strcmp(multiFirst, "sample") != 0 &&
                     strcmp(multiFirst, "sample.txt") == 0 && strcmp(multiSecond, "second.txt") == 0 && multiSecond[strlen(multiSecond)+1] == '\0',
                 "GetOpenFileNameA OFN_ALLOWMULTISELECT buffer", "multi-select writes dir\\0file1\\0file2\\0\\0 with default extension");
    smoke_expect(&s, ofn.nFileOffset == strlen(fileBuf) + 1 && strcmp(titleBuf, "sample.txt") == 0,
                 "GetOpenFileNameA multi-select offsets/title", "nFileOffset points at first returned file and file title is populated");

    CHOOSEFONTA cf;
    memset(&cf, 0, sizeof(cf));
    SetLastError(0);
    ok = ChooseFontA(&cf);
    smoke_expect(&s, ok == FALSE, "ChooseFontA invalid lStructSize", "must fail validation without entering modal loop");
    smoke_expect(&s, CommDlgExtendedError() == CDERR_STRUCTSIZE, "CommDlgExtendedError ChooseFontA struct", "CDERR_STRUCTSIZE is reported exactly");

    LOGFONTA lf;
    memset(&lf, 0, sizeof(lf));
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = owner;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_ENABLEHOOK;
    ok = ChooseFontA(&cf);
    smoke_expect(&s, ok == FALSE && CommDlgExtendedError() == CDERR_NOHOOK,
                 "ChooseFontA CF_ENABLEHOOK requires hook", "missing hook fails with CDERR_NOHOOK");

    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = owner;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_ENABLETEMPLATE;
    ok = ChooseFontA(&cf);
    smoke_expect(&s, ok == FALSE && CommDlgExtendedError() == CDERR_NOHINSTANCE,
                 "ChooseFontA template requires hInstance", "CF_ENABLETEMPLATE uses module resources, not raw pointers or silent fallback");

    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = owner;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_USESTYLE;
    ok = ChooseFontA(&cf);
    smoke_expect(&s, ok == FALSE && CommDlgExtendedError() == CDERR_INITIALIZATION,
                 "ChooseFontA CF_USESTYLE buffer validation", "CF_USESTYLE requires lpszStyle storage");

    memset(&lf, 0, sizeof(lf));
    snprintf(lf.lfFaceName, sizeof(lf.lfFaceName), "DefinitelyMissingFace");
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = owner;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST;
    ok = ChooseFontA(&cf);
    smoke_expect(&s, ok == FALSE && CommDlgExtendedError() == CFERR_NOFONTS,
                 "ChooseFontA CF_FORCEFONTEXIST", "invalid initial face is rejected before modal commit");

    char style[32];
    snprintf(style, sizeof(style), "Regular");
    memset(&lf, 0, sizeof(lf));
    snprintf(lf.lfFaceName, sizeof(lf.lfFaceName), "Courier New");
    lf.lfHeight = -22;
    lf.lfWeight = FW_BOLD;
    lf.lfItalic = TRUE;
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = owner;
    cf.lpLogFont = &lf;
    cf.iPointSize = 220;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_ENABLEHOOK | CF_USESTYLE |
               CF_NOSIZESEL | CF_NOSTYLESEL | CF_NOFACESEL;
    cf.lpfnHook = smoke_v196_cf_hook;
    cf.lpszStyle = style;
    g_v196_cf_hook_init = 0;
    ok = ChooseFontA(&cf);
    smoke_expect(&s, ok == TRUE && CommDlgExtendedError() == 0 && g_v196_cf_hook_init == 1,
                 "ChooseFontA hook-driven OK", "real #32770 font dialog returns TRUE and clears extended error");
    smoke_expect(&s, strcmp(lf.lfFaceName, "Courier New") == 0 && lf.lfHeight == -22 &&
                     lf.lfWeight == FW_BOLD && lf.lfItalic && strcmp(style, "Regular") == 0,
                 "ChooseFontA NO*SEL preserves caller fields", "CF_NOFACESEL/CF_NOSIZESEL/CF_NOSTYLESEL preserve LOGFONT/style fields");

    BYTE fontTemplateBlob[4096];
    DWORD fontTemplateSize = smoke_v198_build_font_template(fontTemplateBlob, sizeof(fontTemplateBlob));
    HGLOBAL hFontTemplate = smoke_v198_template_handle_from_blob(fontTemplateBlob, fontTemplateSize);
    memset(&lf, 0, sizeof(lf));
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = owner;
    cf.lpLogFont = &lf;
    cf.hInstance = (HINSTANCE)hFontTemplate;
    cf.Flags = CF_SCREENFONTS | CF_ENABLEHOOK | CF_ENABLETEMPLATEHANDLE;
    cf.lpfnHook = smoke_v196_cf_hook;
    g_v196_cf_hook_init = 0;
    g_v197_template_custom_seen = 0;
    ok = ChooseFontA(&cf);
    smoke_expect(&s, ok == TRUE && g_v196_cf_hook_init == 1 && g_v197_template_custom_seen == 1,
                 "ChooseFontA CF_ENABLETEMPLATEHANDLE", "font common dialog consumes HGLOBAL DLGTEMPLATE without pointer fallback");
    if (hFontTemplate) GlobalFree(hFontTemplate);
    smoke_expect(&s, owner == 0 || IsWindowEnabled(owner),
                 "ChooseFontA restores owner", "modal owner is re-enabled after OK");

    if (owner && IsWindow(owner)) DestroyWindow(owner);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_services(SmokeRuntime* rt)
{
    (void)rt;
    SmokeContext s = {0};
    s.group = "services";
    MySvcInit();

    HANDLE hScm = OpenSCManagerA(NULL, NULL, MYSVC_ACCESS_ALL);
    smoke_expect(&s, hScm != 0, "OpenSCManagerA", "MYSVC_ACCESS_ALL");

    char svcName[96];
    smoke_unique_name(svcName, sizeof(svcName), "service");
    for (char* p = svcName; *p; ++p) if (*p == '\\') *p = '.';

    HANDLE hSvc = hScm ? CreateServiceA(hScm, svcName, "v130 Smoke Service", MYSVC_ACCESS_ALL,
                                        0x00000010u, MYSVC_START_TYPE_DEMAND,
                                        1u, "smoke.exe", NULL, NULL, NULL, NULL, NULL) : 0;
    smoke_expect(&s, hSvc != 0, "CreateServiceA", svcName);
    smoke_expect(&s, hSvc && StartServiceA(hSvc, 0, NULL), "StartServiceA", "service enters RUNNING state");
    MyServiceInfo st;
    memset(&st, 0, sizeof(st));
    smoke_expect(&s, hSvc && QueryServiceStatus(hSvc, &st) && st.state == MYSVC_RUNNING,
                 "QueryServiceStatus", "SERVICE_RUNNING");
    smoke_expect(&s, hSvc && ControlService(hSvc, 1u, &st) && st.state == MYSVC_STOPPED,
                 "ControlService(STOP)", "SERVICE_STOPPED");
    smoke_expect(&s, hSvc && DeleteService(hSvc), "DeleteService", "mark service deleted/removed");
    if (hSvc) CloseServiceHandle(hSvc);
    if (hScm) CloseServiceHandle(hScm);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}



static int smoke_ownership(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "ownership";
    smoke_runtime_init(rt);

    Capability owner = rt->cap;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_wndproc;
    wc.lpszClassName = "myOS.v130.OwnerRoot";
    smoke_expect(&s, RegisterClassExA(&wc) != 0, "RegisterClassExA(owner root)", wc.lpszClassName);

    HWND root = CreateWindowExA(0, "myOS.v130.OwnerRoot", "Owner Root", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                20, 20, 300, 180, 0, 0, 0, NULL);
    smoke_expect(&s, root && IsWindow(root), "CreateWindowExA owner root", "root owned by smoke-admin UI thread");

    HWND child = root ? CreateWindowExA(0, "myOS.v130.OwnerRoot", "Child", WS_CHILD|WS_VISIBLE,
                                        4, 5, 90, 40, root, (HMENU)4001, 0, NULL) : 0;
    smoke_expect(&s, child && IsWindow(child), "CreateWindowExA child", "visual parent path");
    smoke_expect(&s, child && GetParent(child) == root, "GetParent(child)", "Parent is visual/clip parent");
    smoke_expect(&s, child && GetWindow(child, GW_OWNER) == 0, "GW_OWNER(child)", "child parent is not owner");
    smoke_expect(&s, child && (HWND)GetWindowLongPtrA(child, GWLP_HWNDPARENT) == root,
                 "GWLP_HWNDPARENT(child)", "child reports parent through Win32 legacy slot");

    HWND owned = root ? CreateWindowExA(0, "myOS.v130.OwnerRoot", "Owned Popup", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                        50, 50, 160, 100, root, 0, 0, NULL) : 0;
    smoke_expect(&s, owned && IsWindow(owned), "CreateWindowExA owned top-level", "hWndParent becomes owner without WS_CHILD");
    smoke_expect(&s, owned && GetParent(owned) == 0, "GetParent(owned top-level)", "owner is not visual parent");
    smoke_expect(&s, owned && GetWindow(owned, GW_OWNER) == root, "GW_OWNER(owned top-level)", "owner tracked separately");
    smoke_expect(&s, owned && (HWND)GetWindowLongPtrA(owned, GWLP_HWNDPARENT) == root,
                 "GWLP_HWNDPARENT(owned top-level)", "legacy owner slot preserved");

    DWORD pidRoot = 0, pidChild = 0, pidOwned = 0;
    DWORD tidRoot = root ? GetWindowThreadProcessId(root, &pidRoot) : 0;
    DWORD tidChild = child ? GetWindowThreadProcessId(child, &pidChild) : 0;
    DWORD tidOwned = owned ? GetWindowThreadProcessId(owned, &pidOwned) : 0;
    smoke_expect(&s, tidRoot == owner.id && pidRoot == owner.id, "GetWindowThreadProcessId(root)", "process/thread dimensions explicit");
    smoke_expect(&s, tidChild == tidRoot && pidChild == pidRoot, "GetWindowThreadProcessId(child)", "child shares owner thread");
    smoke_expect(&s, tidOwned == tidRoot && pidOwned == pidRoot, "GetWindowThreadProcessId(owned)", "owner relation does not change affinity");

    Capability foreign = cap_create(205, "v130-foreign-ui", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&foreign, 0);
    MyWinBindRuntime(&rt->mgr, &foreign);
    MyWinBindDesktop(&rt->wm);
    smoke_expect(&s, RegisterClassExA(&wc) != 0, "RegisterClassExA(foreign root)", "v150 requires each process to register its own app class");

    HWND foreignTop = CreateWindowExA(0, "myOS.v130.OwnerRoot", "Foreign Top", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                      80, 80, 140, 90, 0, 0, 0, NULL);
    DWORD foreignPid = 0;
    DWORD foreignTid = foreignTop ? GetWindowThreadProcessId(foreignTop, &foreignPid) : 0;
    smoke_expect(&s, foreignTop && IsWindow(foreignTop), "foreign own top-level", "foreign thread may create its own root");
    smoke_expect(&s, foreignTid == foreign.id && foreignPid == foreign.id, "GetWindowThreadProcessId(foreign)", "foreign affinity is independent");

    SetLastError(0x12345678u);
    HWND badChild = CreateWindowExA(0, "myOS.v130.OwnerRoot", "Bad Child", WS_CHILD|WS_VISIBLE,
                                    1, 1, 20, 20, root, (HMENU)4002, 0, NULL);
    smoke_expect(&s, badChild == 0, "foreign CreateWindowExA(child of owner)", "cross-owner child parent denied");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign CreateWindowExA(child) LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, MoveWindow(child, 30, 30, 100, 50, FALSE) == FALSE,
                 "foreign MoveWindow(owner child)", "CAP_WINDOW_CONTROL no longer bypasses thread ownership");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign MoveWindow LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, DestroyWindow(child) == FALSE && IsWindow(child),
                 "foreign DestroyWindow(owner child)", "foreign thread cannot tear down owner HWND");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign DestroyWindow LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, SetFocus(child) == 0, "foreign SetFocus before AttachThreadInput", "input queues are separate by default");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign SetFocus LastError");

    smoke_expect(&s, AttachThreadInput(foreign.id, owner.id, TRUE), "AttachThreadInput(TRUE)", "input queues can be explicitly coupled");
    SetLastError(0);
    smoke_expect(&s, SetFocus(child) == 0 && GetFocus() == child,
                 "foreign SetFocus after AttachThreadInput", "attached input queues allow focus transition");
    smoke_expect(&s, AttachThreadInput(foreign.id, owner.id, FALSE), "AttachThreadInput(FALSE)", "detach input queues");

    SetLastError(0x12345678u);
    smoke_expect(&s, SetCapture(child) == 0, "foreign SetCapture after detach", "capture follows thread/input ownership");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign SetCapture LastError");

    if (foreignTop) DestroyWindow(foreignTop);

    MyWinBindRuntime(&rt->mgr, &owner);
    if (GetFocus() == child) SetFocus(0);
    if (owned) DestroyWindow(owned);
    if (child) DestroyWindow(child);
    if (root) DestroyWindow(root);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_hwnd_access(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "hwnd_access";
    smoke_runtime_init(rt);

    Capability owner = rt->cap;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_wndproc;
    wc.lpszClassName = "myOS.v131.AccessRoot";
    smoke_expect(&s, RegisterClassExA(&wc) != 0, "RegisterClassExA(access root)", wc.lpszClassName);

    HWND root = CreateWindowExA(0, "myOS.v131.AccessRoot", "Access Root", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                24, 24, 300, 180, 0, 0, 0, NULL);
    HWND child = root ? CreateWindowExA(0, "myOS.v131.AccessRoot", "Access Child", WS_CHILD|WS_VISIBLE,
                                        6, 7, 100, 50, root, (HMENU)4101, 0, NULL) : 0;
    HWND owner2 = CreateWindowExA(0, "myOS.v131.AccessRoot", "Second Parent", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                  340, 24, 220, 140, 0, 0, 0, NULL);
    HWND owned = root ? CreateWindowExA(0, "myOS.v131.AccessRoot", "Owned Popup", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                        54, 54, 160, 90, root, 0, 0, NULL) : 0;

    smoke_expect(&s, root && child && owner2 && owned, "CreateWindowExA(access fixtures)", "root/child/owner/owned popup");
    smoke_expect(&s, IsWindow(root) && IsWindow(child), "IsWindow(owner fixtures)", "baseline valid HWNDs");

    Capability noRead = cap_create(301, "v131-foreign-no-read", CAP_IPC|CAP_WINDOW_CONTROL);
    cap_add_target(&noRead, 0);
    MyWinBindRuntime(&rt->mgr, &noRead);

    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(root, &pid);
    smoke_expect(&s, tid == owner.id && pid == owner.id, "foreign GetWindowThreadProcessId", "basic identity query remains readable");
    smoke_expect(&s, IsWindow(root), "foreign IsWindow", "validity query remains readable");

    char text[128];
    char cls[128];
    RECT rc;
    memset(text, 0, sizeof(text));
    memset(cls, 0, sizeof(cls));
    memset(&rc, 0, sizeof(rc));

    SetLastError(0x12345678u);
    smoke_expect(&s, GetWindowTextA(root, text, sizeof(text)) == 0, "foreign GetWindowText without READ", "text needs CAP_WINDOW_READ or ownership");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign GetWindowText LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, GetClassNameA(root, cls, sizeof(cls)) == 0, "foreign GetClassName without READ", "class needs CAP_WINDOW_READ or ownership");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign GetClassName LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, GetWindowRect(root, &rc) == FALSE, "foreign GetWindowRect without READ", "geometry needs CAP_WINDOW_READ or ownership");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign GetWindowRect LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, GetParent(child) == 0, "foreign GetParent without READ", "tree query denied without read capability");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign GetParent LastError");

    Capability reader = cap_create(302, "v131-foreign-reader", CAP_IPC|CAP_WINDOW_READ);
    cap_add_target(&reader, 0);
    MyWinBindRuntime(&rt->mgr, &reader);

    memset(text, 0, sizeof(text));
    memset(cls, 0, sizeof(cls));
    memset(&rc, 0, sizeof(rc));
    smoke_expect(&s, GetWindowTextA(root, text, sizeof(text)) > 0 && strcmp(text, "Access Root") == 0,
                 "foreign reader GetWindowText", "CAP_WINDOW_READ permits metadata read");
    smoke_expect(&s, GetClassNameA(root, cls, sizeof(cls)) > 0 && strcmp(cls, "myOS.v131.AccessRoot") == 0,
                 "foreign reader GetClassName", "class metadata read");
    smoke_expect(&s, GetWindowRect(root, &rc) && (rc.right - rc.left) == 300 && (rc.bottom - rc.top) == 180,
                 "foreign reader GetWindowRect", "geometry metadata read");
    smoke_expect(&s, GetParent(child) == root, "foreign reader GetParent", "visual tree metadata read");
    smoke_expect(&s, GetWindow(owned, GW_OWNER) == root, "foreign reader GW_OWNER", "owner relationship read");
    smoke_expect(&s, GetWindowLongPtrA(child, GWLP_ID) == 4101, "foreign reader GetWindowLongPtr", "read-only window long path");

    Capability controller = cap_create(303, "v131-foreign-controller", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&controller, 0);
    MyWinBindRuntime(&rt->mgr, &controller);

    SetLastError(0x12345678u);
    smoke_expect(&s, SetWindowTextA(root, "pwned") == FALSE, "foreign SetWindowText", "CAP_WINDOW_CONTROL does not mutate foreign USER32 metadata");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign SetWindowText LastError");
    memset(text, 0, sizeof(text));
    MyWinBindRuntime(&rt->mgr, &reader);
    smoke_expect(&s, GetWindowTextA(root, text, sizeof(text)) > 0 && strcmp(text, "Access Root") == 0,
                 "foreign SetWindowText no effect", "title unchanged");
    MyWinBindRuntime(&rt->mgr, &controller);

    SetLastError(0x12345678u);
    smoke_expect(&s, EnableWindow(root, FALSE) == FALSE, "foreign EnableWindow", "enable state is owner-only");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign EnableWindow LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, SetWindowLongPtrA(child, GWLP_ID, 9999) == 0, "foreign SetWindowLongPtr", "window long mutation is owner-only");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign SetWindowLongPtr LastError");
    MyWinBindRuntime(&rt->mgr, &reader);
    smoke_expect(&s, GetWindowLongPtrA(child, GWLP_ID) == 4101, "foreign SetWindowLongPtr no effect", "id unchanged");
    MyWinBindRuntime(&rt->mgr, &controller);

    SetLastError(0x12345678u);
    smoke_expect(&s, SetParent(child, owner2) == 0, "foreign SetParent", "foreign reparent denied");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign SetParent LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, PostMessageA(root, WM_SETTEXT, 0, (LPARAM)"inject") == FALSE,
                 "foreign PostMessage WM_SETTEXT", "arbitrary foreign mutation messages denied");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign PostMessage WM_SETTEXT LastError");

    DWORD_PTR result = 0x7777u;
    SetLastError(0x12345678u);
    smoke_expect(&s, SendMessageTimeoutA(root, WM_COMMAND, MAKEWPARAM(0x1234u, 0), 0, SMTO_ABORTIFHUNG, 10, &result) == FALSE,
                 "foreign SendMessage WM_COMMAND", "foreign command injection denied");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign SendMessage WM_COMMAND LastError");

    SetLastError(0x12345678u);
    smoke_expect(&s, PostMessageA(root, WM_CLOSE, 0, 0) == TRUE,
                 "foreign PostMessage WM_CLOSE", "close remains request-shaped and queued");

    MSG forged;
    memset(&forged, 0, sizeof(forged));
    forged.hwnd = root;
    forged.message = WM_COMMAND;
    forged.wParam = MAKEWPARAM(0xBADC, 0);
    int beforeCmd = g_user32_command;
    SetLastError(0x12345678u);
    smoke_expect(&s, DispatchMessageA(&forged) == 0 && g_user32_command == beforeCmd,
                 "foreign DispatchMessage forged MSG", "manual MSG cannot inject into foreign WndProc");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign DispatchMessage LastError");

    MyWinBindRuntime(&rt->mgr, &owner);
    smoke_expect(&s, SetParent(child, owner2) == root && GetParent(child) == owner2,
                 "owner SetParent same-owner", "owner may reparent within same UI owner");
    smoke_expect(&s, SetParent(child, root) == owner2 && GetParent(child) == root,
                 "owner SetParent restore", "restore visual parent");
    smoke_expect(&s, SetWindowTextA(root, "Access Root 2"), "owner SetWindowText", "owner mutation succeeds");
    memset(text, 0, sizeof(text));
    smoke_expect(&s, GetWindowTextA(root, text, sizeof(text)) > 0 && strcmp(text, "Access Root 2") == 0,
                 "owner GetWindowText after SetWindowText", "owner sees updated title");

    /* MDI-specific injection gates: foreign contexts may read geometry/class
       with CAP_WINDOW_READ, but cannot create/destroy/activate another MDI tree. */
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_mdi_frame_wndproc;
    wc.lpszClassName = "myOS.v131.AccessMDIFrame";
    smoke_expect(&s, RegisterClassExA(&wc) != 0, "RegisterClassExA(access MDI frame)", wc.lpszClassName);
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_mdi_child_wndproc;
    wc.lpszClassName = "myOS.v131.AccessMDIChild";
    smoke_expect(&s, RegisterClassExA(&wc) != 0, "RegisterClassExA(access MDI child)", wc.lpszClassName);

    HWND frame = CreateWindowExA(0, "myOS.v131.AccessMDIFrame", "Access MDI Frame", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                 80, 260, 420, 260, 0, 0, 0, NULL);
    CLIENTCREATESTRUCT ccs;
    memset(&ccs, 0, sizeof(ccs));
    ccs.idFirstChild = 61000u;
    HWND client = frame ? CreateWindowExA(0, "MDICLIENT", "", WS_CHILD|WS_VISIBLE|MDIS_ALLCHILDSTYLES,
                                          0, 0, 390, 220, frame, (HMENU)501, 0, &ccs) : 0;
    g_mdi_smoke_client = client;
    MDICREATESTRUCTA mcs;
    memset(&mcs, 0, sizeof(mcs));
    mcs.szClass = "myOS.v131.AccessMDIChild";
    mcs.szTitle = "MDI Owned";
    mcs.x = 4; mcs.y = 4; mcs.cx = 140; mcs.cy = 90;
    mcs.style = WS_OVERLAPPEDWINDOW;
    HWND mdiChild = client ? (HWND)SendMessageA(client, WM_MDICREATE, 0, (LPARAM)&mcs) : 0;
    smoke_expect(&s, frame && client && mdiChild && IsWindow(mdiChild), "MDI access fixtures", "owner-created MDI tree");

    HWND activeBefore = client ? (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0) : 0;
    MyWinBindRuntime(&rt->mgr, &controller);
    SetLastError(0x12345678u);
    smoke_expect(&s, SendMessageA(client, WM_MDICREATE, 0, (LPARAM)&mcs) == 0,
                 "foreign SendMessage WM_MDICREATE", "foreign MDI create injection denied");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign WM_MDICREATE LastError");
    SetLastError(0x12345678u);
    smoke_expect(&s, PostMessageA(client, WM_MDIDESTROY, (WPARAM)mdiChild, 0) == FALSE,
                 "foreign PostMessage WM_MDIDESTROY", "foreign MDI destroy injection denied");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign WM_MDIDESTROY LastError");
    memset(&forged, 0, sizeof(forged));
    forged.hwnd = client;
    forged.message = WM_MDIACTIVATE;
    forged.wParam = (WPARAM)mdiChild;
    SetLastError(0x12345678u);
    smoke_expect(&s, DispatchMessageA(&forged) == 0, "foreign DispatchMessage WM_MDIACTIVATE", "forged MDI activate denied");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "foreign DispatchMessage MDI LastError");

    MyWinBindRuntime(&rt->mgr, &owner);
    smoke_expect(&s, (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0) == activeBefore,
                 "MDI active unchanged after foreign injection", "active child survived denied foreign messages");
    smoke_expect(&s, IsWindow(mdiChild), "MDI child survived foreign destroy", "denied WM_MDIDESTROY had no effect");

    if (client) DestroyWindow(client);
    if (frame) DestroyWindow(frame);
    if (owned) DestroyWindow(owned);
    if (child) DestroyWindow(child);
    if (owner2) DestroyWindow(owner2);
    if (root) DestroyWindow(root);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_mdi(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "mdi";
    smoke_runtime_init(rt);

    g_mdi_smoke_client = 0;
    g_mdi_child_create = g_mdi_child_destroy = g_mdi_child_activate = 0;
    g_mdi_child_childactivate = g_mdi_child_command = g_mdi_frame_command = 0;
    g_mdi_last_command = 0;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_mdi_frame_wndproc;
    wc.lpszClassName = "myOS.v132.MDIFrame";
    smoke_expect(&s, RegisterClassExA(&wc) != 0, "RegisterClassExA(frame)", wc.lpszClassName);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_mdi_child_wndproc;
    wc.lpszClassName = "myOS.v132.MDIChild";
    smoke_expect(&s, RegisterClassExA(&wc) != 0, "RegisterClassExA(child)", wc.lpszClassName);

    HWND frame = CreateWindowExA(0, "myOS.v132.MDIFrame", "MDI Frame", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                 40, 40, 520, 360, 0, 0, 0, NULL);
    smoke_expect(&s, frame && IsWindow(frame), "CreateWindowExA(frame)", "top-level frame HWND");

    HMENU menu = CreateMenu();
    CLIENTCREATESTRUCT ccs;
    memset(&ccs, 0, sizeof(ccs));
    ccs.hWindowMenu = menu;
    ccs.idFirstChild = 50000u;
    HWND client = CreateWindowExA(0, "MDICLIENT", "", WS_CHILD|WS_VISIBLE|MDIS_ALLCHILDSTYLES,
                                  0, 0, 500, 300, frame, (HMENU)100, 0, &ccs);
    g_mdi_smoke_client = client;
    smoke_expect(&s, client && IsWindow(client), "CreateWindowExA(MDICLIENT)", "built-in MDI client class");
    smoke_expect(&s, client && GetParent(client) == frame, "MDICLIENT parent", "client is child of frame");
    char cls[64];
    memset(cls, 0, sizeof(cls));
    smoke_expect(&s, client && GetClassNameA(client, cls, sizeof(cls)) && strcmp(cls, "MDICLIENT") == 0,
                 "GetClassNameA(MDICLIENT)", cls);

    MDICREATESTRUCTA mcs1;
    memset(&mcs1, 0, sizeof(mcs1));
    mcs1.szClass = "myOS.v132.MDIChild";
    mcs1.szTitle = "Child One";
    mcs1.x = 10; mcs1.y = 12; mcs1.cx = 180; mcs1.cy = 120;
    mcs1.style = WS_OVERLAPPEDWINDOW;
    mcs1.lParam = 0x1111;
    HWND child1 = (HWND)SendMessageA(client, WM_MDICREATE, 0, (LPARAM)&mcs1);
    smoke_expect(&s, child1 && IsWindow(child1), "WM_MDICREATE child1", "returns valid MDI child HWND");
    smoke_expect(&s, child1 && GetParent(child1) == client, "MDI child parent", "parent is MDICLIENT");
    smoke_expect(&s, child1 && GetDlgCtrlID(child1) == 50000, "MDI child idFirstChild", "first id from CLIENTCREATESTRUCT");
    smoke_expect(&s, g_mdi_child_create == 1, "MDI child WM_CREATE", "child WndProc sees creation");
    smoke_expect(&s, (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0) == child1,
                 "WM_MDIGETACTIVE after child1", "new child is active");
    smoke_expect(&s, GetFocus() == child1, "MDI activation focus", "active child receives focus");

    MDICREATESTRUCTA mcs2 = mcs1;
    mcs2.szTitle = "Child Two";
    mcs2.x = 40; mcs2.y = 44; mcs2.lParam = 0x2222;
    HWND child2 = (HWND)SendMessageA(client, WM_MDICREATE, 0, (LPARAM)&mcs2);
    smoke_expect(&s, child2 && IsWindow(child2) && child2 != child1, "WM_MDICREATE child2", "second MDI child HWND");
    smoke_expect(&s, child2 && GetDlgCtrlID(child2) == 50001, "MDI child id increments", "second id from CLIENTCREATESTRUCT");
    smoke_expect(&s, (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0) == child2,
                 "WM_MDIGETACTIVE after child2", "second child becomes active");
    smoke_expect(&s, GetMenuItemCount(menu) == 2, "MDI window menu auto-list", "two child entries appended to hWindowMenu");
    smoke_expect(&s, GetMenuItemID(menu, 0) == 50000 && GetMenuItemID(menu, 1) == 50001,
                 "MDI window menu child IDs", "idFirstChild-backed IDs");
    smoke_expect(&s, g_mdi_child_activate >= 3 && g_mdi_child_childactivate >= 2,
                 "WM_MDIACTIVATE / WM_CHILDACTIVATE", "activation notifications sent");

    MDICREATESTRUCTA mcs3 = mcs1;
    mcs3.szTitle = "Child Three";
    mcs3.x = 70; mcs3.y = 76; mcs3.lParam = 0x3333;
    HWND child3 = (HWND)SendMessageA(client, WM_MDICREATE, 0, (LPARAM)&mcs3);
    smoke_expect(&s, child3 && IsWindow(child3) && child3 != child1 && child3 != child2,
                 "WM_MDICREATE child3", "third MDI child HWND");
    smoke_expect(&s, GetMenuItemCount(menu) == 3 && GetMenuItemID(menu, 2) == 50002,
                 "MDI window menu refresh after create", "third child entry added");

    HMENU frameMenu2 = CreateMenu();
    HMENU windowMenu2 = CreatePopupMenu();
    smoke_expect(&s, SendMessageA(client, WM_MDISETMENU, (WPARAM)frameMenu2, (LPARAM)windowMenu2) == 0,
                 "WM_MDISETMENU", "stores frame/window menu pair and returns old frame menu");
    smoke_expect(&s, GetMenuItemCount(windowMenu2) == 3 && GetMenuItemID(windowMenu2, 0) == 50000 && GetMenuItemID(windowMenu2, 2) == 50002,
                 "WM_MDISETMENU window list", "new hWindowMenu populated with children");

    SendMessageA(client, WM_MDICASCADE, 0, 0);
    RECT c1Cascade, c2Cascade, c3Cascade;
    memset(&c1Cascade, 0, sizeof(c1Cascade));
    memset(&c2Cascade, 0, sizeof(c2Cascade));
    memset(&c3Cascade, 0, sizeof(c3Cascade));
    GetWindowRect(child1, &c1Cascade); GetWindowRect(child2, &c2Cascade); GetWindowRect(child3, &c3Cascade);
    smoke_expect(&s, c2Cascade.left > c1Cascade.left && c3Cascade.left > c2Cascade.left &&
                     c2Cascade.top > c1Cascade.top && c3Cascade.top > c2Cascade.top,
                 "WM_MDICASCADE", "children receive stepped cascade positions");
    RECT clientRc;
    memset(&clientRc, 0, sizeof(clientRc));
    GetWindowRect(client, &clientRc);
    smoke_expect(&s, c1Cascade.left >= clientRc.left && c1Cascade.top >= clientRc.top &&
                     c3Cascade.right <= clientRc.right && c3Cascade.bottom <= clientRc.bottom,
                 "WM_MDICASCADE bounds", "cascade keeps visible children inside MDICLIENT without scrollbars");

    LPARAM cascadeCaptionPoint = MAKELPARAM((WORD)(c1Cascade.left + 10), (WORD)(c1Cascade.top + 6));
    smoke_expect(&s, SendMessageA(child1, WM_NCHITTEST, 0, cascadeCaptionPoint) == HTCAPTION,
                 "MDI cascade WM_NCHITTEST caption", "drawn child caption resolves to HTCAPTION");

    RECT cascadeDragBefore, cascadeDragAfter;
    memset(&cascadeDragBefore, 0, sizeof(cascadeDragBefore));
    memset(&cascadeDragAfter, 0, sizeof(cascadeDragAfter));
    GetWindowRect(child1, &cascadeDragBefore);
    SendMessageA(child1, WM_LBUTTONDOWN, 0, MAKELPARAM(10, (WORD)-5));
    SendMessageA(child1, WM_MOUSEMOVE, 0, MAKELPARAM(44, 18));
    SendMessageA(child1, WM_LBUTTONUP, 0, MAKELPARAM(44, 18));
    GetWindowRect(child1, &cascadeDragAfter);
    smoke_expect(&s, (cascadeDragAfter.left != cascadeDragBefore.left || cascadeDragAfter.top != cascadeDragBefore.top) &&
                     cascadeDragAfter.left >= clientRc.left && cascadeDragAfter.top >= clientRc.top &&
                     cascadeDragAfter.right <= clientRc.right && cascadeDragAfter.bottom <= clientRc.bottom,
                 "MDI cascade caption drag", "caption drag moves child and remains clipped to MDICLIENT");

    RECT fallbackDragBefore, fallbackDragAfter;
    memset(&fallbackDragBefore, 0, sizeof(fallbackDragBefore));
    memset(&fallbackDragAfter, 0, sizeof(fallbackDragAfter));
    GetWindowRect(child1, &fallbackDragBefore);
    SendMessageA(child1, WM_LBUTTONDOWN, 0, MAKELPARAM(12, 6));
    SendMessageA(child1, WM_MOUSEMOVE, 0, MAKELPARAM(40, 20));
    SendMessageA(child1, WM_LBUTTONUP, 0, MAKELPARAM(40, 20));
    GetWindowRect(child1, &fallbackDragAfter);
    smoke_expect(&s, fallbackDragAfter.left != fallbackDragBefore.left || fallbackDragAfter.top != fallbackDragBefore.top,
                 "MDI caption positive-Y compatibility drag", "stale outer-caption coords still start the same drag contract");

    /* v139 note: this smoke creates standalone USER32 HWNDs, not WindowManager
       slots.  The physical evdev/raw compositor route is therefore covered by
       the MDILab app-lab smoke below, where the frame has a real desktop slot. */

    SendMessageA(client, WM_MDITILE, MDITILE_HORIZONTAL, 0);
    RECT c1Tile, c2Tile, c3Tile;
    memset(&c1Tile, 0, sizeof(c1Tile));
    memset(&c2Tile, 0, sizeof(c2Tile));
    memset(&c3Tile, 0, sizeof(c3Tile));
    GetWindowRect(child1, &c1Tile); GetWindowRect(child2, &c2Tile); GetWindowRect(child3, &c3Tile);
    smoke_expect(&s, c1Tile.left < c2Tile.left && c2Tile.left < c3Tile.left &&
                     (c1Tile.right - c1Tile.left) > 20 && (c2Tile.right - c2Tile.left) > 20,
                 "WM_MDITILE", "children tiled across MDICLIENT");
    smoke_expect(&s, SendMessageA(client, WM_MDIICONARRANGE, 0, 0) == 1,
                 "WM_MDIICONARRANGE", "safe no-op until iconic MDI surfaces exist");

    SendMessageA(client, WM_MDINEXT, (WPARAM)child3, FALSE);
    smoke_expect(&s, (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0) == child1,
                 "WM_MDINEXT", "cycles active child forward");
    SendMessageA(frame, WM_COMMAND, MAKEWPARAM(50002u, 0), 0);
    smoke_expect(&s, (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0) == child3,
                 "MDI window-menu command", "idFirstChild menu item activates selected child");
    SendMessageA(child1, WM_LBUTTONDOWN, 0, MAKELPARAM(4, 30));
    smoke_expect(&s, (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0) == child1,
                 "MDI child mouse activation", "clicking child activates its MDI slot");
    smoke_expect(&s, GetWindow(client, GW_CHILD) == child1,
                 "MDI activation Z-order", "clicking an inactive child raises it to top of MDICLIENT stack");

    RECT dragBefore, dragAfter;
    memset(&dragBefore, 0, sizeof(dragBefore));
    memset(&dragAfter, 0, sizeof(dragAfter));
    GetWindowRect(child1, &dragBefore);
    SendMessageA(child1, WM_LBUTTONDOWN, 0, MAKELPARAM(10, (WORD)-5));
    SendMessageA(child1, WM_MOUSEMOVE, 0, MAKELPARAM(40, 10));
    SendMessageA(child1, WM_LBUTTONUP, 0, MAKELPARAM(40, 10));
    GetWindowRect(child1, &dragAfter);
    smoke_expect(&s, dragAfter.left > dragBefore.left || dragAfter.top > dragBefore.top,
                 "MDI child caption drag", "negative client-Y caption band moves child in MDICLIENT coords");

    int frameCmdBefore = g_mdi_frame_command;
    int childCmdBefore = g_mdi_child_command;
    LRESULT routed = SendMessageA(frame, WM_COMMAND, MAKEWPARAM(0x7101u, 0), 0);
    smoke_expect(&s, routed == 0xBEEF && g_mdi_frame_command == frameCmdBefore + 1 &&
                     g_mdi_child_command == childCmdBefore + 1,
                 "DefFrameProcA WM_COMMAND route", "frame routes command to active MDI child");
    smoke_expect(&s, LOWORD(g_mdi_last_command) == 0x7101u,
                 "MDI command payload", "LOWORD(wParam) preserved");

    SendMessageA(client, WM_MDIACTIVATE, (WPARAM)child2, 0);
    smoke_expect(&s, (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0) == child2,
                 "WM_MDIACTIVATE explicit", "explicit activation selects child2");

    RECT before, after;
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    smoke_expect(&s, GetWindowRect(child2, &before), "GetWindowRect(MDI child)", "MDI child has screen rect");
    smoke_expect(&s, MoveWindow(child2, 80, 82, 200, 140, FALSE) && GetWindowRect(child2, &after) &&
                     (after.right - after.left) == 200 && (after.bottom - after.top) == 140 &&
                     (after.left != before.left || after.top != before.top),
                 "MoveWindow(MDI child)", "child geometry remains USER32-visible");

    smoke_expect(&s, SendMessageA(client, WM_MDIDESTROY, (WPARAM)child2, 0) != 0,
                 "WM_MDIDESTROY", "destroy active child2");
    smoke_expect(&s, !IsWindow(child2), "MDI child destroyed", "child2 HWND invalid after WM_MDIDESTROY");
    HWND fallback = (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0);
    smoke_expect(&s, fallback == child1 || fallback == child3,
                 "MDI active fallback", "destroying active child falls back to remaining child");
    smoke_expect(&s, GetMenuItemCount(windowMenu2) == 2,
                 "MDI window menu refresh after destroy", "destroyed child removed from hWindowMenu");

    smoke_expect(&s, DestroyWindow(client), "DestroyWindow(MDICLIENT)", "client destroys remaining children first");
    smoke_expect(&s, !IsWindow(child1) && !IsWindow(child3), "MDICLIENT child teardown", "remaining children destroyed with client");
    smoke_expect(&s, g_mdi_child_destroy >= 3, "MDI child WM_DESTROY", "all children delivered WM_DESTROY");
    if (frame) DestroyWindow(frame);
    if (menu) DestroyMenu(menu);
    if (frameMenu2) DestroyMenu(frameMenu2);
    if (windowMenu2) DestroyMenu(windowMenu2);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_app_labs(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "app_labs";
    smoke_runtime_init(rt);

    typedef struct AppLabCase {
        const char* name;
        int (*create)(WindowManager*, int, int, const char*, Capability);
        AppType type;
        int expectChildren;
    } AppLabCase;

    AppLabCase cases[] = {
        { "calc",        wm_add_calc,       APP_CALC,       0 },
        { "spy",         wm_add_spy,        APP_SPY,        0 },
        { "access-lab",  wm_add_access,     APP_ACCESS,     0 },
        { "pump-lab",    wm_add_pump,       APP_PUMP,       0 },
        { "deadlock-lab",wm_add_deadlock,   APP_DEADLOCK,   0 },
        { "section-lab", wm_add_section,    APP_SECTION,    0 },
        { "object-lab",  wm_add_objectlab,  APP_OBJECT,     0 },
        { "wait-lab",    wm_add_waitlab,    APP_WAITLAB,    0 },
        { "clip-menu-lab", wm_add_clipmenulab, APP_CLIPMENU, 0 },
        { "paint-lab",   wm_add_paintlab,   APP_PAINTLAB,   0 },
        { "drag-lab",    wm_add_draglab,    APP_DRAGLAB,    0 },
        { "control-lab", wm_add_controllab, APP_CONTROLLAB, 0 },
        { "service-lab", wm_add_servicelab, APP_SERVICELAB, 1 },
        { "dialog-lab",  wm_add_dialoglab,  APP_DIALOGLAB,  0 },
        { "mdi-lab",     wm_add_mdilab,     APP_MDILAB,     1 },
    };

    int liveBefore = 0;
    for (int i = 0; i < rt->wm.count; ++i) if (!rt->wm.wins[i].closed) liveBefore++;

    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        int x = 40 + (int)i * 7;
        int y = 50 + (int)i * 5;
        char title[96];
        snprintf(title, sizeof(title), "v130 smoke %s", cases[i].name);
        int idx = cases[i].create(&rt->wm, x, y, title, rt->cap);
        char detail[128];
        snprintf(detail, sizeof(detail), "wm_add_%s returned desktop slot", cases[i].name);
        smoke_expect(&s, idx >= 0 && idx < rt->wm.count && !rt->wm.wins[idx].closed, cases[i].name, detail);
        HWND hwnd = (idx >= 0 && idx < rt->wm.count) ? rt->wm.wins[idx].app_hwnd : 0;
        snprintf(detail, sizeof(detail), "slot=%d hwnd=0x%x type=%d", idx, (unsigned)hwnd, (int)cases[i].type);
        smoke_expect(&s, hwnd && IsWindow(hwnd) && rt->wm.wins[idx].app_type == cases[i].type, "app HWND/type", detail);
        RECT rc;
        memset(&rc, 0, sizeof(rc));
        smoke_expect(&s, hwnd && GetWindowRect(hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top,
                     "app GetWindowRect", cases[i].name);
        smoke_expect(&s, hwnd && MoveWindow(hwnd, x + 3, y + 4, (int)(rc.right - rc.left), (int)(rc.bottom - rc.top), TRUE),
                     "app MoveWindow", cases[i].name);
        RECT rc2;
        memset(&rc2, 0, sizeof(rc2));
        smoke_expect(&s, hwnd && GetWindowRect(hwnd, &rc2) && rc2.left == x + 3 && rc2.top == y + 4,
                     "app MoveWindow visible", cases[i].name);
        if (cases[i].expectChildren) {
            POINT pt;
            pt.x = 20;
            pt.y = (cases[i].type == APP_MDILAB) ? (APP_MENUBAR_H + 14) : 18;
            HWND child = ChildWindowFromPoint(hwnd, pt);
            smoke_expect(&s, child && IsWindow(child), "app child HWND", cases[i].name);
        }
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
        smoke_expect(&s, !hwnd || !IsWindow(hwnd), "app DestroyWindow", cases[i].name);
    }

    /* v139 MDILab manual contract: toolbar New is a real BUTTON path and must
       create exactly one MDI child per physical click; after all children close,
       CW_USEDEFAULT cascade state must restart from the first slot. */
    int mdiLabIdx = wm_add_mdilab(&rt->wm, 120, 90, "v139 smoke mdilab manual", rt->cap);
    HWND mdiLab = (mdiLabIdx >= 0 && mdiLabIdx < rt->wm.count) ? rt->wm.wins[mdiLabIdx].app_hwnd : 0;
    HWND mdiClient = mdiLab ? GetDlgItem(mdiLab, 0xA240) : 0;
    HMENU mdiFrameMenu = mdiLab ? GetMenu(mdiLab) : 0;
    HMENU mdiWindowMenu = mdiFrameMenu ? GetSubMenu(mdiFrameMenu, 3) : 0;
    smoke_expect(&s, mdiLabIdx >= 0 && mdiLab && mdiClient && mdiWindowMenu,
                 "MDILab manual setup", "frame, MDICLIENT and Window submenu visible");
    int beforeChildren = mdiWindowMenu ? GetMenuItemCount(mdiWindowMenu) : -1;
    if (mdiLabIdx >= 0 && mdiLabIdx < rt->wm.count) {
        rt->wm.focused = mdiLabIdx;
        rt->wm.wins[mdiLabIdx].active = 1;
    }

    /* v143: cover the exact manual report: a freshly opened MDILab must drag
       before any Escape/Close side-effect has had a chance to normalize state.
       Test both the active top child and the visible background child caption in
       the initial cascade stack. */
    HWND initialTop = mdiClient ? GetWindow(mdiClient, GW_CHILD) : 0;
    HWND initialBack = initialTop ? GetWindow(initialTop, GW_HWNDNEXT) : 0;

    /* v145: reproduce the real manual bug.  The evdev input thread has fresh
       thread-local USER32 state, so before v145 the first MDI caption hit-test
       ran with no runtime capability and failed until Escape accidentally bound
       a runtime on that same input thread. */
    if (initialTop && IsWindow(initialTop)) {
        RECT before, after;
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        GetWindowRect(initialTop, &before);
        SmokeRawDragThreadCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.rt = rt;
        ctx.sx = before.left + 14;
        ctx.sy = before.top + 7;
        ctx.mx = ctx.sx + 19;
        ctx.my = ctx.sy + 13;
        pthread_t tid;
        int thOk = (pthread_create(&tid, NULL, smoke_raw_drag_thread_no_runtime, &ctx) == 0);
        if (thOk) pthread_join(tid, NULL);
        GetWindowRect(initialTop, &after);
        smoke_expect(&s, thOk && ctx.downOk && ctx.moveOk && ctx.upOk &&
                         (after.left != before.left || after.top != before.top),
                     "MDILab first input-thread caption drag",
                     "fresh TLS/no runtime raw input bootstraps session broker before Escape");
    } else {
        smoke_expect(&s, 0, "MDILab first input-thread caption drag", "initial top child missing");
    }

    if (initialTop && IsWindow(initialTop)) {
        RECT before, after;
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        GetWindowRect(initialTop, &before);
        SmokeRawDragThreadCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.rt = rt;
        ctx.sx = before.left + 13;
        ctx.sy = before.top + 7;
        ctx.mx = ctx.sx - 17;
        ctx.my = ctx.sy + 11;
        pthread_t tid;
        int thOk = (pthread_create(&tid, NULL, smoke_raw_drag_thread_weak_runtime, &ctx) == 0);
        if (thOk) pthread_join(tid, NULL);
        GetWindowRect(initialTop, &after);
        smoke_expect(&s, thOk && ctx.downOk && ctx.moveOk && ctx.upOk &&
                         (after.left != before.left || after.top != before.top),
                     "MDILab weak-runtime caption drag",
                     "raw input upgrades insufficient app capability to session broker");
    } else {
        smoke_expect(&s, 0, "MDILab weak-runtime caption drag", "initial top child missing");
    }

    if (initialTop && IsWindow(initialTop)) {
        RECT before, after;
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        GetWindowRect(initialTop, &before);
        int dx = before.left + 12;
        int dy = before.top + 6;
        int rawDownOk = wm_route_raw_mouse_button_down(&rt->wm, dx, dy, 0);
        int rawMoveConsumed = wm_mouse_move(&rt->wm, dx + 31, dy + 17);
        int rawUpOk = wm_route_raw_mouse_button_up(&rt->wm, dx + 31, dy + 17, 0);
        GetWindowRect(initialTop, &after);
        smoke_expect(&s, rawDownOk && rawMoveConsumed && rawUpOk &&
                         (after.left != before.left || after.top != before.top),
                     "MDILab initial active caption drag", "fresh MDILab top child drags before Escape/Close");
    } else {
        smoke_expect(&s, 0, "MDILab initial active caption drag", "initial top child missing");
    }
    if (initialBack && IsWindow(initialBack)) {
        RECT before, after;
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        GetWindowRect(initialBack, &before);
        int dx = before.left + 12;
        int dy = before.top + 6;
        int rawDownOk = wm_route_raw_mouse_button_down(&rt->wm, dx, dy, 0);
        int rawMoveConsumed = wm_mouse_move(&rt->wm, dx + 27, dy + 15);
        int rawUpOk = wm_route_raw_mouse_button_up(&rt->wm, dx + 27, dy + 15, 0);
        GetWindowRect(initialBack, &after);
        smoke_expect(&s, rawDownOk && rawMoveConsumed && rawUpOk &&
                         (after.left != before.left || after.top != before.top),
                     "MDILab initial background caption drag", "fresh MDILab visible background child drags before Escape/Close");
    } else {
        smoke_expect(&s, 0, "MDILab initial background caption drag", "initial background child missing");
    }

    /* v144: cover the manual first-launch miss that falls through to the
       MDICLIENT as ordinary client input instead of being caught by the
       compositor pre-route.  MDICLIENT must still promote its visual child
       caption hit-region to WM_NCLBUTTONDOWN/SetCapture so the drag starts. */
    {
        HWND fallbackChild = mdiClient ? GetWindow(mdiClient, GW_CHILD) : 0;
        RECT before, after, cr;
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        memset(&cr, 0, sizeof(cr));
        BOOL okRect = fallbackChild && GetWindowRect(fallbackChild, &before) && GetWindowRect(mdiClient, &cr);
        if (okRect) {
            POINT cpt;
            cpt.x = (int)(before.left - cr.left) + 12;
            cpt.y = (int)(before.top  - cr.top)  + 6;
            SendMessageA(mdiClient, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((WORD)cpt.x, (WORD)cpt.y));
            HWND cap = GetCapture();
            SendMessageA(fallbackChild, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM((WORD)54, (WORD)4));
            SendMessageA(fallbackChild, WM_LBUTTONUP, 0, MAKELPARAM((WORD)54, (WORD)4));
            GetWindowRect(fallbackChild, &after);
            smoke_expect(&s, cap == fallbackChild && (after.left != before.left || after.top != before.top),
                         "MDILab MDICLIENT fallback caption drag", "client fallback promotes visual caption to child NC drag");
        } else {
            smoke_expect(&s, 0, "MDILab MDICLIENT fallback caption drag", "fallback child fixture missing");
        }
    }

    int newX = rt->wm.wins[mdiLabIdx].x + 1 + 8 + 30;
    int newY = rt->wm.wins[mdiLabIdx].y + TITLEBAR_H + APP_MENUBAR_H + 6 + 14;
    smoke_raw_left_click(rt, newX, newY);
    int afterChildren = mdiWindowMenu ? GetMenuItemCount(mdiWindowMenu) : -1;
    smoke_expect(&s, beforeChildren == 2 && afterChildren == beforeChildren + 1,
                 "MDILab toolbar New one-shot", "one physical BUTTON click creates exactly one child");

    /* v140: the real evdev loop can receive down/up before the 60Hz render
       thread pumps the HWND queue.  The raw router now delivers app client
       mouse downs/ups synchronously, so a fast physical click must still be a
       deterministic one-shot BUTTON click, not a click-storm lottery. */
    beforeChildren = mdiWindowMenu ? GetMenuItemCount(mdiWindowMenu) : -1;
    wm_route_raw_mouse_button_down(&rt->wm, newX, newY, 0);
    wm_route_raw_mouse_button_up(&rt->wm, newX, newY, 0);
    hwnd_dispatch(&rt->mgr);
    afterChildren = mdiWindowMenu ? GetMenuItemCount(mdiWindowMenu) : -1;
    smoke_expect(&s, beforeChildren >= 0 && afterChildren == beforeChildren + 1,
                 "MDILab toolbar fast click no-pump", "raw down/up before dispatch still creates exactly one child");

    int guard = 0;
    while (mdiClient && mdiWindowMenu && GetMenuItemCount(mdiWindowMenu) > 0 && guard++ < 16) {
        HWND active = (HWND)SendMessageA(mdiClient, WM_MDIGETACTIVE, 0, 0);
        if (!active) break;
        SendMessageA(active, WM_CLOSE, 0, 0);
    }
    smoke_expect(&s, mdiWindowMenu && GetMenuItemCount(mdiWindowMenu) == 0,
                 "MDILab close-all empties Window menu", "all MDI children destroyed before reset check");
    smoke_raw_left_click(rt, newX, newY);
    HWND resetChild = mdiClient ? (HWND)SendMessageA(mdiClient, WM_MDIGETACTIVE, 0, 0) : 0;
    RECT resetRc, clientRc2;
    memset(&resetRc, 0, sizeof(resetRc));
    memset(&clientRc2, 0, sizeof(clientRc2));
    BOOL resetRectOk = resetChild && GetWindowRect(resetChild, &resetRc) && GetWindowRect(mdiClient, &clientRc2);
    smoke_expect(&s, mdiWindowMenu && GetMenuItemCount(mdiWindowMenu) == 1 && resetRectOk &&
                     resetRc.left == clientRc2.left + 8 && resetRc.top == clientRc2.top + 8,
                 "MDILab New after close-all resets cascade", "empty MDI client restarts CW_USEDEFAULT at first slot");

    if (resetChild && resetRectOk) {
        RECT dragBefore2, dragAfter2;
        memset(&dragBefore2, 0, sizeof(dragBefore2));
        memset(&dragAfter2, 0, sizeof(dragAfter2));
        GetWindowRect(resetChild, &dragBefore2);
        int dx = dragBefore2.left + 12;
        int dy = dragBefore2.top + 6;
        int rawDownOk = wm_route_raw_mouse_button_down(&rt->wm, dx, dy, 0);
        int rawMoveConsumed = wm_mouse_move(&rt->wm, dx + 42, dy + 26);
        int rawUpOk = wm_route_raw_mouse_button_up(&rt->wm, dx + 42, dy + 26, 0);
        GetWindowRect(resetChild, &dragAfter2);
        smoke_expect(&s, rawDownOk && rawMoveConsumed && rawUpOk &&
                         (dragAfter2.left != dragBefore2.left || dragAfter2.top != dragBefore2.top),
                     "MDILab physical caption drag", "WindowManager raw path drags MDI child visual caption and consumes move");
    } else {
        smoke_expect(&s, 0, "MDILab physical caption drag", "reset child fixture missing");
    }
    if (mdiLab && IsWindow(mdiLab)) DestroyWindow(mdiLab);

    int editorIdx = wm_add_editor(&rt->wm, 90, 110, NULL, "v130 smoke editor", rt->cap);
    smoke_expect(&s, editorIdx >= 0 && editorIdx < rt->wm.count && !rt->wm.wins[editorIdx].closed,
                 "editor", "wm_add_editor returned desktop slot");
    HWND editorHwnd = (editorIdx >= 0 && editorIdx < rt->wm.count) ? rt->wm.wins[editorIdx].app_hwnd : 0;
    smoke_expect(&s, editorHwnd && IsWindow(editorHwnd) && rt->wm.wins[editorIdx].app_type == APP_EDITOR,
                 "editor HWND/type", "editor direct app canary");
    RECT editorRc;
    memset(&editorRc, 0, sizeof(editorRc));
    smoke_expect(&s, editorHwnd && GetWindowRect(editorHwnd, &editorRc), "editor GetWindowRect", "editor geometry visible");
    smoke_expect(&s, editorHwnd && MoveWindow(editorHwnd, 94, 116, editorRc.right-editorRc.left, editorRc.bottom-editorRc.top, TRUE),
                 "editor MoveWindow", "editor follows USER32 geometry contract");
    if (editorHwnd && IsWindow(editorHwnd)) DestroyWindow(editorHwnd);
    smoke_expect(&s, !editorHwnd || !IsWindow(editorHwnd), "editor DestroyWindow", "editor closes through USER32 path");

    int liveAfter = 0;
    for (int i = 0; i < rt->wm.count; ++i) if (!rt->wm.wins[i].closed) liveAfter++;
    smoke_expect(&s, liveAfter == liveBefore, "desktop slot cleanup", "DestroyWindow syncs WindowManager slots for app canaries");

    /* Keep the heavy OOP GUI apps out of this smoke pass for now; v129 still
       gates registration/alias coverage here.  The existing apphost smoke still
       launches a real fork/exec console process. */
    const char* aliases[] = {
        "calc", "editor", "clip-menu-lab", "paint-lab", "control-lab",
        "dialog-lab", "service-lab", "mdi-lab", "drag-lab", "access-lab", "wait-lab",
        "object-lab", "section-lab", "argdump"
    };
    for (unsigned i = 0; i < sizeof(aliases)/sizeof(aliases[0]); ++i)
        smoke_expect(&s, MyAppHostIsRegistered(aliases[i]), "apphost alias registered", aliases[i]);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static int smoke_shell_broker(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "shell_broker";
    smoke_runtime_init(rt);

    /* v133 regression tripwire: v131 made HWND access strict.  The compositor
       thread may pump shell queues while its ambient Capability is not the
       shell owner.  hwnd_dispatch_message() must therefore enter the target
       window owner's runtime context before calling #32769/Shell_TrayWnd
       WndProcs, otherwise GetWindowLongPtrA(GWLP_USERDATA) fails and the
       shell ignores legitimate commands/clicks. */
    const UINT ID_START_DIALOGLAB_SMOKE = 1020u;

    int liveBefore = 0;
    for (int i = 0; i < rt->wm.count; ++i) if (!rt->wm.wins[i].closed) liveBefore++;
    Capability neutral = cap_create(9910, "neutral-dispatcher", CAP_IPC);
    cap_add_target(&neutral, 0);
    MyWinBindRuntime(&rt->mgr, &neutral);
    MyWinBindDesktop(&rt->wm);

    int postOk = hwnd_post(&rt->mgr, &rt->wm.shell_cap, rt->wm.hwnd_desktop,
                           WM_COMMAND, MAKEWPARAM((WORD)ID_START_DIALOGLAB_SMOKE, 0),
                           (LPARAM)(((uint32_t)(uint16_t)180) | (((uint32_t)(uint16_t)120) << 16))) == 0;
    smoke_expect(&s, postOk, "post desktop command as shell", "queued WM_COMMAND to #32769 while ambient cap is neutral");
    hwnd_dispatch(&rt->mgr);

    int liveAfter = 0;
    int dialogIdx = -1;
    for (int i = 0; i < rt->wm.count; ++i) {
        if (!rt->wm.wins[i].closed) liveAfter++;
        /* v167: DialogLab is intentionally no longer the legacy APP_DIALOGLAB
           in-process slot.  The shell command must create an AppHost IPC proxy
           owned by the dialog-lab child process. */
        if (!rt->wm.wins[i].closed &&
            rt->wm.wins[i].app_type == APP_IPC_PROXY &&
            strcmp(rt->wm.wins[i].image_name, "dialog-lab") == 0) dialogIdx = i;
    }
    smoke_expect(&s, liveAfter == liveBefore + 1 && dialogIdx >= 0, "desktop command executes", "DialogLab created as OOP IPC proxy from queued shell command");
    HWND dialogHwnd = (dialogIdx >= 0 && dialogIdx < rt->wm.count) ? rt->wm.wins[dialogIdx].app_hwnd : 0;
    smoke_expect(&s, dialogHwnd && IsWindow(dialogHwnd) && rt->wm.wins[dialogIdx].app_type == APP_IPC_PROXY && rt->wm.wins[dialogIdx].process_id != 0,
                 "desktop command HWND/type", "shell WndProc ran in shell owner context and launched isolated DialogLab");

    if (dialogHwnd && IsWindow(dialogHwnd)) {
        MyWinBindRuntime(&rt->mgr, &rt->wm.wins[dialogIdx].app_cap);
        MyWinBindDesktop(&rt->wm);
        DestroyWindow(dialogHwnd);
    }

    MyWinBindRuntime(&rt->mgr, &rt->cap);
    MyWinBindDesktop(&rt->wm);
    int appIdx = wm_add_calc(&rt->wm, 100, 100, "v133 taskbar resume", rt->cap);
    smoke_expect(&s, appIdx >= 0 && appIdx < rt->wm.count, "taskbar app canary", "calc created for taskbar resume check");
    if (appIdx >= 0 && appIdx < rt->wm.count) {
        rt->wm.wins[appIdx].minimized = 1;
        rt->wm.focused = -1;

        MyWinBindRuntime(&rt->mgr, &neutral);
        MyWinBindDesktop(&rt->wm);
        int taskX = 4 + 72 + 8 + 5;
        for (int i = 0; i < appIdx; ++i)
            if (!rt->wm.wins[i].closed) taskX += 140 + 8;
        LPARAM lp = (LPARAM)(((uint32_t)(uint16_t)taskX) | (((uint32_t)(uint16_t)8) << 16));
        int taskPost = hwnd_post(&rt->mgr, &rt->wm.shell_cap, rt->wm.hwnd_taskbar,
                                 WM_LBUTTONDOWN, 0, lp) == 0;
        smoke_expect(&s, taskPost, "post taskbar click as shell", "queued Shell_TrayWnd click while ambient cap is neutral");
        hwnd_dispatch(&rt->mgr);
        smoke_expect(&s, rt->wm.focused == appIdx && rt->wm.wins[appIdx].minimized == 0,
                     "taskbar resume executes", "Shell_TrayWnd WndProc restored minimized app");
        HWND appHwnd = rt->wm.wins[appIdx].app_hwnd;
        MyWinBindRuntime(&rt->mgr, &rt->cap);
        MyWinBindDesktop(&rt->wm);
        if (appHwnd && IsWindow(appHwnd)) DestroyWindow(appHwnd);
    }

    MyWinBindRuntime(&rt->mgr, &rt->cap);
    MyWinBindDesktop(&rt->wm);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_user32_timers(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "user32_timer";
    smoke_runtime_init(rt);
    MyWinBindRuntime(&rt->mgr, &rt->cap);
    MyWinBindDesktop(&rt->wm);

    g_user32_timer = g_user32_timerproc = g_user32_paint = 0;
    g_user32_last_timer_id = 0;
    g_user32_last_timer_lparam = 0;
    g_user32_last_timerproc_id = 0;
    g_user32_last_timerproc_hwnd = 0;
    g_user32_command = 0;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_wndproc;
    wc.lpszClassName = "myOS.v149.TimerSmoke";
    ATOM atom = RegisterClassExA(&wc);
    smoke_expect(&s, atom != 0, "RegisterClassExA(timer)", wc.lpszClassName);

    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, "Smoke USER32 timers", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                      55, 55, 240, 120, 0, 0, 0, NULL) : 0;
    smoke_expect(&s, hwnd != 0, "CreateWindowExA(timer)", "timer owner window");

    UINT_PTR id = hwnd ? SetTimer(hwnd, 0x149u, 15, NULL) : 0;
    smoke_expect(&s, id == 0x149u, "SetTimer(hwnd,id,NULL)", "window timer returns caller id");

    MSG msg;
    memset(&msg, 0, sizeof(msg));
    smoke_expect(&s, !PeekMessageA(&msg, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE),
                 "WM_TIMER not early", "timer is synthetic only after due time");
    usleep(25000);

    PostMessageA(hwnd, WM_COMMAND, 0x1490u, 0);
    memset(&msg, 0, sizeof(msg));
    BOOL gotCmd = PeekMessageA(&msg, hwnd, 0, 0, PM_REMOVE);
    smoke_expect(&s, gotCmd && msg.message == WM_COMMAND,
                 "WM_TIMER low priority", "queued WM_COMMAND wins before due synthetic timer");
    if (gotCmd) DispatchMessageA(&msg);

    memset(&msg, 0, sizeof(msg));
    BOOL gotTimer = PeekMessageA(&msg, hwnd, WM_TIMER, WM_TIMER, PM_NOREMOVE);
    smoke_expect(&s, gotTimer && msg.message == WM_TIMER && msg.hwnd == hwnd && msg.wParam == id,
                 "PeekMessageA(PM_NOREMOVE) WM_TIMER", "observes due timer without consuming it");
    memset(&msg, 0, sizeof(msg));
    gotTimer = PeekMessageA(&msg, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE);
    smoke_expect(&s, gotTimer && msg.message == WM_TIMER && msg.hwnd == hwnd && msg.wParam == id,
                 "PeekMessageA(PM_REMOVE) WM_TIMER", "consumes synthetic window timer");
    if (gotTimer) DispatchMessageA(&msg);
    smoke_expect(&s, g_user32_timer == 1 && g_user32_last_timer_id == id && g_user32_last_timer_lparam == 0,
                 "DispatchMessageA WM_TIMER WndProc", "NULL TIMERPROC routes to owner WndProc");

    smoke_expect(&s, KillTimer(hwnd, id), "KillTimer(hwnd,id)", "window timer removed");

    UINT_PTR cbid = hwnd ? SetTimer(hwnd, 0x150u, 10, smoke_timerproc) : 0;
    smoke_expect(&s, cbid == 0x150u, "SetTimer(hwnd,id,TIMERPROC)", "callback timer registered");
    usleep(18000);
    memset(&msg, 0, sizeof(msg));
    gotTimer = GetMessageA(&msg, hwnd, WM_TIMER, WM_TIMER);
    smoke_expect(&s, gotTimer && msg.message == WM_TIMER && msg.lParam != 0,
                 "GetMessageA WM_TIMER", "message pump synthesizes callback timer");
    if (gotTimer) DispatchMessageA(&msg);
    smoke_expect(&s, g_user32_timerproc == 1 && g_user32_last_timerproc_id == cbid && g_user32_last_timerproc_hwnd == hwnd,
                 "DispatchMessageA TIMERPROC", "callback invoked instead of WndProc");
    smoke_expect(&s, KillTimer(hwnd, cbid), "KillTimer callback", "callback timer removed");

    UINT_PTR threadId = SetTimer(0, 0, 10, NULL);
    smoke_expect(&s, threadId != 0, "SetTimer(NULL,0,NULL)", "thread timer generated id");
    usleep(18000);
    memset(&msg, 0, sizeof(msg));
    gotTimer = PeekMessageA(&msg, 0, WM_TIMER, WM_TIMER, PM_REMOVE);
    smoke_expect(&s, gotTimer && msg.message == WM_TIMER && msg.hwnd == 0 && msg.wParam == threadId,
                 "thread WM_TIMER", "hWnd NULL timer targets thread queue");
    smoke_expect(&s, KillTimer(0, threadId), "KillTimer(NULL,id)", "thread timer removed");

    UINT_PTR killed = hwnd ? SetTimer(hwnd, 0x151u, 10, NULL) : 0;
    smoke_expect(&s, killed == 0x151u && KillTimer(hwnd, killed), "KillTimer prevents pending timer", "timer killed before due");
    usleep(18000);
    memset(&msg, 0, sizeof(msg));
    smoke_expect(&s, !PeekMessageA(&msg, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE),
                 "killed timer silent", "no synthetic WM_TIMER after KillTimer");

    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_apphost(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "apphost";
    smoke_runtime_init(rt);

    smoke_expect(&s, MyAppHostIsRegistered("calc"), "MyAppHostIsRegistered(calc)", "registered GUI image");
    smoke_expect(&s, MyAppHostIsRegistered("argdump"), "MyAppHostIsRegistered(argdump)", "registered console image");
    smoke_expect(&s, MyAppHostIsRegistered("drag-lab"), "MyAppHostIsRegistered(drag-lab)", "registered OOP DragLab image");
    smoke_expect(&s, MyAppHostIsRegistered("dialog-lab"), "MyAppHostIsRegistered(dialog-lab)", "registered OOP DialogLab image");

    DWORD lifetimeOwnerPid = rt->cap.id;
    DWORD ownerHandlesBefore = MyGetHandleCount(lifetimeOwnerPid);
    DWORD procObjBefore = MyGetObjectCountByType(_OBJECT_TYPE_PROCESS);
    DWORD threadObjBefore = MyGetObjectCountByType(_OBJECT_TYPE_THREAD);
    MyWindowLifetimeAudit auditBefore;
    memset(&auditBefore, 0, sizeof(auditBefore));
    wm_get_lifetime_audit_stats(&auditBefore);
    MyProcessHostAudit phAuditBefore;
    memset(&phAuditBefore, 0, sizeof(phAuditBefore));
    MyProcessHostGetAuditStats(&phAuditBefore);
    MyHandleTableAudit htAuditBefore;
    memset(&htAuditBefore, 0, sizeof(htAuditBefore));
    MyWinGetHandleTableAudit(&htAuditBefore);
    HWNDManagerStats hwndAuditBefore;
    memset(&hwndAuditBefore, 0, sizeof(hwndAuditBefore));
    hwnd_get_stats(&rt->mgr, &hwndAuditBefore);

    /* v185: process-exit must own and sweep the exited process' private
       HANDLE table.  Parent-owned hProcess/hThread may remain until closed,
       but handles owned by the exited child PID must not survive exit. */
    SECURITY_ATTRIBUTES inheritSa;
    memset(&inheritSa, 0, sizeof(inheritSa));
    inheritSa.nLength = sizeof(inheritSa);
    inheritSa.bInheritHandle = TRUE;
    char inheritName[96];
    smoke_unique_name(inheritName, sizeof(inheritName), "v186.inherited.event");
    HANDLE hInheritedEvent = CreateEventA(&inheritSa, TRUE, FALSE, inheritName);
    PROCESS_INFORMATION hpInfo;
    memset(&hpInfo, 0, sizeof(hpInfo));
    Capability hpChild = cap_create(0, "v186-handle-sweep-child", CAP_IPC);
    BOOL hpCreated = hInheritedEvent && MyWinCreateProcessWithStartupCapability("v186-handle-sweep-child", NULL, &hpChild, TRUE, ".", NULL, &hpInfo);
    DWORD childHandlesBeforeExit = hpCreated ? MyGetHandleCount(hpInfo.dwProcessId) : 0;
    if (hpCreated && hpInfo.hProcess) TerminateProcess(hpInfo.hProcess, 0x1860);
    DWORD childHandlesAfterExit = hpCreated ? MyGetHandleCount(hpInfo.dwProcessId) : 0;
    if (hpInfo.hThread) CloseHandle(hpInfo.hThread);
    if (hpInfo.hProcess) CloseHandle(hpInfo.hProcess);
    if (hInheritedEvent) CloseHandle(hInheritedEvent);
    char htdetail[192];
    snprintf(htdetail, sizeof(htdetail), "created=%u childPid=%u child handles %u->%u",
             (unsigned)hpCreated, (unsigned)hpInfo.dwProcessId,
             (unsigned)childHandlesBeforeExit, (unsigned)childHandlesAfterExit);
    smoke_expect(&s, hpCreated && childHandlesBeforeExit > 0 && childHandlesAfterExit == 0,
                 "process exit sweeps owned handle table", htdetail);

    /* v186: cross-process DuplicateHandle must transfer one real object ref
       into the target process handle table. Closing the parent handle must not
       kill the child handle; child exit must release the inherited/duplicated
       ref and unregister the waitable if no other handles remain. */
    char dupName[96];
    smoke_unique_name(dupName, sizeof(dupName), "v186.dup.event");
    HANDLE hDupEvent = CreateEventA(NULL, TRUE, FALSE, dupName);
    PROCESS_INFORMATION dupInfo;
    memset(&dupInfo, 0, sizeof(dupInfo));
    Capability dupChild = cap_create(0, "v186-dup-child", CAP_IPC);
    BOOL dupProcCreated = hDupEvent && MyWinCreateProcessWithStartupCapability("v186-dup-child", NULL, &dupChild, FALSE, ".", NULL, &dupInfo);
    HANDLE hChildDup = 0;
    BOOL dupToChild = dupProcCreated && DuplicateHandle(GetCurrentProcess(), hDupEvent, dupInfo.hProcess, &hChildDup, 0, TRUE, DUPLICATE_SAME_ACCESS);
    DWORD childDupHandles = dupProcCreated ? MyGetHandleCount(dupInfo.dwProcessId) : 0;
    MyHandleInfo childDupHi;
    memset(&childDupHi, 0, sizeof(childDupHi));
    BOOL childDupInfo = hChildDup ? MyGetHandleInfo(hChildDup, &childDupHi) : FALSE;
    if (hDupEvent) { CloseHandle(hDupEvent); hDupEvent = 0; }
    _ObjectectInfo childDupObjAfterParentClose;
    memset(&childDupObjAfterParentClose, 0, sizeof(childDupObjAfterParentClose));
    BOOL objAliveAfterParentClose = childDupInfo ? MyGetObjectInfo(childDupHi.object_handle, &childDupObjAfterParentClose) : FALSE;
    if (dupProcCreated && dupInfo.hProcess) TerminateProcess(dupInfo.hProcess, 0x1861);
    MyWinSweepExitedHandleTables();
    _ObjectectInfo childDupObjAfterChildExit;
    memset(&childDupObjAfterChildExit, 0, sizeof(childDupObjAfterChildExit));
    BOOL objAliveAfterChildExit = childDupInfo ? MyGetObjectInfo(childDupHi.object_handle, &childDupObjAfterChildExit) : FALSE;
    snprintf(htdetail, sizeof(htdetail), "created=%u dup=%u childPid=%u h=0x%x count=%u info=%u alive parentClose=%u childExit=%u",
             (unsigned)dupProcCreated, (unsigned)dupToChild, (unsigned)dupInfo.dwProcessId,
             (unsigned)(uintptr_t)hChildDup, (unsigned)childDupHandles, (unsigned)childDupInfo,
             (unsigned)objAliveAfterParentClose, (unsigned)objAliveAfterChildExit);
    smoke_expect(&s, dupProcCreated && dupToChild && childDupHandles > 0 && childDupInfo && objAliveAfterParentClose && !objAliveAfterChildExit,
                 "DuplicateHandle parent->child preserves object until child exit", htdetail);
    if (dupInfo.hThread) CloseHandle(dupInfo.hThread);
    if (dupInfo.hProcess) CloseHandle(dupInfo.hProcess);

    /* DUPLICATE_CLOSE_SOURCE is not local-only: with PROCESS_DUP_HANDLE access
       it must close the source handle in the named source process table. */
    char closeSrcName[96];
    smoke_unique_name(closeSrcName, sizeof(closeSrcName), "v186.close_source.event");
    HANDLE hCloseSrcEvent = CreateEventA(NULL, TRUE, FALSE, closeSrcName);
    PROCESS_INFORMATION closeSrcInfo;
    memset(&closeSrcInfo, 0, sizeof(closeSrcInfo));
    Capability closeSrcChild = cap_create(0, "v186-close-source-child", CAP_IPC);
    BOOL closeSrcProcCreated = hCloseSrcEvent && MyWinCreateProcessWithStartupCapability("v186-close-source-child", NULL, &closeSrcChild, FALSE, ".", NULL, &closeSrcInfo);
    HANDLE hCloseSrcChild = 0;
    BOOL closeSrcDupToChild = closeSrcProcCreated && DuplicateHandle(GetCurrentProcess(), hCloseSrcEvent, closeSrcInfo.hProcess, &hCloseSrcChild, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DWORD childBeforeCloseSource = closeSrcProcCreated ? MyGetHandleCount(closeSrcInfo.dwProcessId) : 0;
    HANDLE hBackDup = 0;
    BOOL closeSourceOk = closeSrcDupToChild && DuplicateHandle(closeSrcInfo.hProcess, hCloseSrcChild, GetCurrentProcess(), &hBackDup, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    DWORD childAfterCloseSource = closeSrcProcCreated ? MyGetHandleCount(closeSrcInfo.dwProcessId) : 0;
    snprintf(htdetail, sizeof(htdetail), "created=%u dupToChild=%u closeSource=%u child handles %u->%u back=0x%x",
             (unsigned)closeSrcProcCreated, (unsigned)closeSrcDupToChild, (unsigned)closeSourceOk,
             (unsigned)childBeforeCloseSource, (unsigned)childAfterCloseSource, (unsigned)(uintptr_t)hBackDup);
    smoke_expect(&s, closeSrcProcCreated && closeSrcDupToChild && closeSourceOk && childBeforeCloseSource > childAfterCloseSource && hBackDup != 0,
                 "DUPLICATE_CLOSE_SOURCE closes cross-process source handle", htdetail);
    if (hBackDup) CloseHandle(hBackDup);
    if (hCloseSrcEvent) CloseHandle(hCloseSrcEvent);
    if (closeSrcInfo.hProcess) TerminateProcess(closeSrcInfo.hProcess, 0x1862);
    if (closeSrcInfo.hThread) CloseHandle(closeSrcInfo.hThread);
    if (closeSrcInfo.hProcess) CloseHandle(closeSrcInfo.hProcess);

    /* Access masks are meaningful: a SYNCHRONIZE-only duplicate cannot be used
       as the source for EVENT_MODIFY_STATE unless DUPLICATE_SAME_ACCESS is used. */
    char accessName[96];
    smoke_unique_name(accessName, sizeof(accessName), "v186.access.event");
    HANDLE hAccessEvent = CreateEventA(NULL, TRUE, FALSE, accessName);
    HANDLE hSyncOnly = 0;
    HANDLE hDenied = 0;
    BOOL syncDup = hAccessEvent && DuplicateHandle(GetCurrentProcess(), hAccessEvent, GetCurrentProcess(), &hSyncOnly, SYNCHRONIZE, FALSE, 0);
    BOOL deniedDup = hSyncOnly ? DuplicateHandle(GetCurrentProcess(), hSyncOnly, GetCurrentProcess(), &hDenied, EVENT_MODIFY_STATE, FALSE, 0) : TRUE;
    DWORD deniedErr = GetLastError();
    snprintf(htdetail, sizeof(htdetail), "syncDup=%u deniedDup=%u err=%u sync=0x%x denied=0x%x",
             (unsigned)syncDup, (unsigned)deniedDup, (unsigned)deniedErr,
             (unsigned)(uintptr_t)hSyncOnly, (unsigned)(uintptr_t)hDenied);
    smoke_expect(&s, syncDup && !deniedDup && hDenied == 0 && deniedErr == ERROR_ACCESS_DENIED,
                 "DuplicateHandle enforces requested access subset", htdetail);
    if (hDenied) CloseHandle(hDenied);
    if (hSyncOnly) CloseHandle(hSyncOnly);
    if (hAccessEvent) CloseHandle(hAccessEvent);

    /* A process handle to an exited process object is not a valid target handle
       table for DuplicateHandle. No dead-PID target fallback is allowed. */
    char deadTargetName[96];
    smoke_unique_name(deadTargetName, sizeof(deadTargetName), "v186.dead_target.event");
    HANDLE hDeadTargetEvent = CreateEventA(NULL, TRUE, FALSE, deadTargetName);
    PROCESS_INFORMATION deadTargetInfo;
    memset(&deadTargetInfo, 0, sizeof(deadTargetInfo));
    Capability deadTargetChild = cap_create(0, "v186-dead-target-child", CAP_IPC);
    BOOL deadTargetCreated = hDeadTargetEvent && MyWinCreateProcessWithStartupCapability("v186-dead-target-child", NULL, &deadTargetChild, FALSE, ".", NULL, &deadTargetInfo);
    if (deadTargetCreated && deadTargetInfo.hProcess) TerminateProcess(deadTargetInfo.hProcess, 0x1863);
    HANDLE hDeadDup = 0;
    BOOL deadTargetDup = deadTargetCreated && DuplicateHandle(GetCurrentProcess(), hDeadTargetEvent, deadTargetInfo.hProcess, &hDeadDup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DWORD deadTargetErr = GetLastError();
    snprintf(htdetail, sizeof(htdetail), "created=%u dup=%u err=%u deadPid=%u target=0x%x",
             (unsigned)deadTargetCreated, (unsigned)deadTargetDup, (unsigned)deadTargetErr,
             (unsigned)deadTargetInfo.dwProcessId, (unsigned)(uintptr_t)hDeadDup);
    smoke_expect(&s, deadTargetCreated && !deadTargetDup && hDeadDup == 0 && deadTargetErr == ERROR_INVALID_PARAMETER,
                 "DuplicateHandle rejects exited target process table", htdetail);
    if (hDeadDup) CloseHandle(hDeadDup);
    if (hDeadTargetEvent) CloseHandle(hDeadTargetEvent);
    if (deadTargetInfo.hThread) CloseHandle(deadTargetInfo.hThread);
    if (deadTargetInfo.hProcess) CloseHandle(deadTargetInfo.hProcess);

    MyAppLaunchOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.lpImageName = "argdump";
    opt.lpTitle = "v130 smoke argdump";
    opt.lpParameters = "alpha beta";
    opt.lpDirectory = ".";
    opt.x = 20;
    opt.y = 20;
    opt.nShowCmd = SW_SHOW;

    MyAppLaunchResult r;
    memset(&r, 0, sizeof(r));
    BOOL ok = MyAppHostLaunchEx(&rt->wm, &opt, &r);
    smoke_expect(&s, ok && r.process_id != 0 && r.hProcess != 0 && r.hThread != 0,
                 "MyAppHostLaunchEx(argdump)", "real child process launch through apphost");

    if (ok && r.process_id) {
        DWORD wait = WAIT_TIMEOUT;
        for (int spin = 0; spin < 200; ++spin) {
            MyWinPollProcess(r.process_id);
            wait = WaitForSingleObject(r.hProcess, 0);
            if (wait == WAIT_OBJECT_0) break;
            usleep(10000);
        }
        smoke_expect(&s, wait == WAIT_OBJECT_0, "process handle wait", "argdump process becomes signaled");
        MyProcessLiteInfo pi;
        memset(&pi, 0, sizeof(pi));
        smoke_expect(&s, MyGetProcessLiteInfo(r.process_id, &pi) && pi.fork_exec == 1,
                     "MyGetProcessLiteInfo", "fork_exec process metadata recorded");
        if (r.hThread) CloseHandle(r.hThread);
        if (r.hProcess) CloseHandle(r.hProcess);
    }

    /* v164 regression: OOP editor must launch through AppHost and accept text input. */
    MyAppLaunchOptions eopt;
    memset(&eopt, 0, sizeof(eopt));
    eopt.lpImageName = "editor";
    eopt.lpTitle = "v164 diagnostic editor";
    eopt.lpPath = "/tmp/myos_v164_diag_editor.txt";
    eopt.lpDirectory = ".";
    eopt.x = 80;
    eopt.y = 70;
    eopt.nShowCmd = SW_SHOW;

    MyAppLaunchResult er;
    memset(&er, 0, sizeof(er));
    BOOL eok = MyAppHostLaunchEx(&rt->wm, &eopt, &er);
    char edetail[160];
    snprintf(edetail, sizeof(edetail), "ok=%d err=%u idx=%d pid=%u hwnd=0x%x",
             eok, (unsigned)GetLastError(), er.window_index, (unsigned)er.process_id,
             (unsigned)(er.window_index >= 0 ? rt->wm.wins[er.window_index].app_hwnd : 0));
    smoke_expect(&s, eok && er.window_index >= 0 && er.process_id != 0,
                 "MyAppHostLaunchEx(editor)", edetail);

    MyProcessHostInfo ehi;
    memset(&ehi, 0, sizeof(ehi));
    int hasEditor = 0;
    for (int spin = 0; spin < 80; ++spin) {
        MyWinPollProcess(er.process_id);
        hwnd_dispatch(&rt->mgr);
        wm_poll(&rt->wm);
        memset(&ehi, 0, sizeof(ehi));
        if (er.process_id && MyProcessHostGetInfo(er.process_id, &ehi) &&
            ehi.editor_enabled && ehi.gdi_enabled && ehi.gdi_command_count > 0) {
            hasEditor = 1;
            break;
        }
        usleep(10000);
    }
    snprintf(edetail, sizeof(edetail), "editor_enabled=%u gdi=%u cmds=%u status=%.50s",
             (unsigned)ehi.editor_enabled, (unsigned)ehi.gdi_enabled,
             (unsigned)ehi.gdi_command_count, ehi.editor_status);
    smoke_expect(&s, hasEditor, "OOP editor publishes initial GDI", edetail);

    if (eok && er.window_index >= 0 && er.window_index < rt->wm.count) {
        HWND eh = rt->wm.wins[er.window_index].app_hwnd;
        hwnd_post(&rt->mgr, &rt->wm.wins[er.window_index].app_cap, eh, WM_CHAR, (WPARAM)'x', 0);
        for (int spin = 0; spin < 80; ++spin) {
            MyWinPollProcess(er.process_id);
            hwnd_dispatch(&rt->mgr);
            wm_poll(&rt->wm);
            usleep(10000);
        }
        memset(&ehi, 0, sizeof(ehi));
        MyProcessHostGetInfo(er.process_id, &ehi);
        snprintf(edetail, sizeof(edetail), "len=%u typed=%u preview=%.50s status=%.50s",
                 (unsigned)ehi.editor_length, (unsigned)ehi.editor_chars_typed,
                 ehi.editor_preview, ehi.editor_status);
        smoke_expect(&s, ehi.editor_length >= 1 && ehi.editor_chars_typed >= 1 &&
                     strstr(ehi.editor_preview, "x") != NULL,
                     "OOP editor accepts WM_CHAR", edetail);
        if (eh && IsWindow(eh))
            wm_close_hwnd(&rt->wm, eh);
        if (er.hProcess) CloseHandle(er.hProcess);
        if (er.hThread) CloseHandle(er.hThread);
    }



    /* v165: DragLab must no longer be a parent-process WinMain island.
       Launch it through AppHost and prove that the Linux child owns render and
       drag/capture state through ProcessHost shared diagnostics. */
    MyAppLaunchOptions dopt;
    memset(&dopt, 0, sizeof(dopt));
    dopt.lpImageName = "drag-lab";
    dopt.lpTitle = "v169 diagnostic draglab";
    dopt.lpDirectory = ".";
    dopt.x = 100;
    dopt.y = 90;
    dopt.nShowCmd = SW_SHOW;

    MyAppLaunchResult dr;
    memset(&dr, 0, sizeof(dr));
    BOOL dok = MyAppHostLaunchEx(&rt->wm, &dopt, &dr);
    char ddetail[192];
    snprintf(ddetail, sizeof(ddetail), "ok=%d err=%u idx=%d pid=%u hwnd=0x%x",
             dok, (unsigned)GetLastError(), dr.window_index, (unsigned)dr.process_id,
             (unsigned)(dr.window_index >= 0 ? rt->wm.wins[dr.window_index].app_hwnd : 0));
    smoke_expect(&s, dok && dr.window_index >= 0 && dr.process_id != 0,
                 "MyAppHostLaunchEx(drag-lab)", ddetail);

    MyProcessHostInfo dhi;
    memset(&dhi, 0, sizeof(dhi));
    int hasDragGdi = 0;
    for (int spin = 0; spin < 80; ++spin) {
        MyWinPollProcess(dr.process_id);
        hwnd_dispatch(&rt->mgr);
        wm_poll(&rt->wm);
        memset(&dhi, 0, sizeof(dhi));
        if (dr.process_id && MyProcessHostGetInfo(dr.process_id, &dhi) &&
            dhi.gdi_enabled && dhi.gdi_command_count > 0 && strstr(dhi.gdi_status, "DragLab") != NULL) {
            hasDragGdi = 1;
            break;
        }
        usleep(10000);
    }
    snprintf(ddetail, sizeof(ddetail), "gdi=%u cmds=%u status=%.70s",
             (unsigned)dhi.gdi_enabled, (unsigned)dhi.gdi_command_count, dhi.gdi_status);
    smoke_expect(&s, hasDragGdi, "OOP DragLab publishes child-owned GDI", ddetail);

    if (dok && dr.window_index >= 0 && dr.window_index < rt->wm.count) {
        HWND dh = rt->wm.wins[dr.window_index].app_hwnd;
        hwnd_post(&rt->mgr, &rt->wm.wins[dr.window_index].app_cap, dh, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(52, 132));
        hwnd_post(&rt->mgr, &rt->wm.wins[dr.window_index].app_cap, dh, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(455, 145));
        hwnd_post(&rt->mgr, &rt->wm.wins[dr.window_index].app_cap, dh, WM_LBUTTONUP, 0, MAKELPARAM(455, 145));
        for (int spin = 0; spin < 100; ++spin) {
            MyWinPollProcess(dr.process_id);
            hwnd_dispatch(&rt->mgr);
            wm_poll(&rt->wm);
            usleep(10000);
        }
        memset(&dhi, 0, sizeof(dhi));
        MyProcessHostGetInfo(dr.process_id, &dhi);
        snprintf(ddetail, sizeof(ddetail), "move=%u cap=%u rel=%u status=%.70s",
                 (unsigned)dhi.paint_move_count, (unsigned)dhi.paint_capture_count,
                 (unsigned)dhi.paint_release_count, dhi.paint_status);
        smoke_expect(&s, dhi.paint_move_count >= 1 && dhi.paint_capture_count >= 1 && dhi.paint_release_count >= 1,
                     "OOP DragLab handles drag/capture messages", ddetail);
        smoke_expect(&s, strstr(dhi.paint_status, "drop=TARGET") != NULL,
                     "OOP DragLab preserves drop verdict after parent capture release", ddetail);
        if (dh && IsWindow(dh))
            wm_close_hwnd(&rt->wm, dh);
        if (dr.hProcess) CloseHandle(dr.hProcess);
        if (dr.hThread) CloseHandle(dr.hThread);
    }

    /* v167: DialogLab moved onto the real GUI IPC child path.  Prove the child
       creates parent-brokered command HWNDs and that the root tab order is
       circular instead of falling through to a default/cancel command. */
    MyAppLaunchOptions dlgopt;
    memset(&dlgopt, 0, sizeof(dlgopt));
    dlgopt.lpImageName = "dialog-lab";
    dlgopt.lpTitle = "v173.1 diagnostic dialoglab";
    dlgopt.lpDirectory = ".";
    dlgopt.x = 120;
    dlgopt.y = 110;
    dlgopt.nShowCmd = SW_SHOW;

    MyAppLaunchResult dlgr;
    memset(&dlgr, 0, sizeof(dlgr));
    BOOL dlgok = MyAppHostLaunchEx(&rt->wm, &dlgopt, &dlgr);
    char dlgdetail[224];
    snprintf(dlgdetail, sizeof(dlgdetail), "ok=%d err=%u idx=%d pid=%u hwnd=0x%x",
             dlgok, (unsigned)GetLastError(), dlgr.window_index, (unsigned)dlgr.process_id,
             (unsigned)(dlgr.window_index >= 0 ? rt->wm.wins[dlgr.window_index].app_hwnd : 0));
    smoke_expect(&s, dlgok && dlgr.window_index >= 0 && dlgr.process_id != 0,
                 "MyAppHostLaunchEx(dialog-lab)", dlgdetail);

    MyProcessHostInfo dlghi;
    memset(&dlghi, 0, sizeof(dlghi));
    int hasDlgGdi = 0;
    for (int spin = 0; spin < 120; ++spin) {
        MyWinPollProcess(dlgr.process_id);
        hwnd_dispatch(&rt->mgr);
        wm_poll(&rt->wm);
        memset(&dlghi, 0, sizeof(dlghi));
        if (dlgr.process_id && MyProcessHostGetInfo(dlgr.process_id, &dlghi) &&
            dlghi.gdi_enabled && dlghi.gdi_command_count > 0 && dlghi.child_hwnd_count >= 13 &&
            strstr(dlghi.gdi_status, "DialogLab") != NULL) { hasDlgGdi = 1; break; }
        usleep(10000);
    }
    snprintf(dlgdetail, sizeof(dlgdetail), "gdi=%u cmds=%u child_count=%u status=%.80s child=%.70s",
             (unsigned)dlghi.gdi_enabled, (unsigned)dlghi.gdi_command_count,
             (unsigned)dlghi.child_hwnd_count, dlghi.gdi_status, dlghi.child_hwnd_status);
    smoke_expect(&s, hasDlgGdi, "OOP DialogLab publishes GDI and child HWNDs", dlgdetail);

    if (dlgok && dlgr.window_index >= 0 && dlgr.window_index < rt->wm.count) {
        HWND root = rt->wm.wins[dlgr.window_index].app_hwnd;
        /* v187: OOP DialogLab now creates command controls through batch IPC.
           The ProcessHost child counter is the request-side signal; the actual
           parent HWNDs can appear a dispatch tick later.  Wait for the real
           USER32 tab chain before asserting focus/order semantics. */
        HWND first = 0;
        for (int spin = 0; spin < 120; ++spin) {
            MyWinPollProcess(dlgr.process_id);
            hwnd_dispatch(&rt->mgr);
            wm_poll(&rt->wm);
            first = GetNextDlgTabItem(root, 0, FALSE);
            if (first) break;
            usleep(10000);
        }
        HWND cur = first;
        int wrapOk = 0;
        int steps = 0;
        for (steps = 0; cur && steps < 32; ++steps) {
            cur = GetNextDlgTabItem(root, cur, FALSE);
            if (cur == first && steps > 0) { wrapOk = 1; break; }
        }
        snprintf(dlgdetail, sizeof(dlgdetail), "root=0x%x first=0x%x cur=0x%x steps=%d child_count=%u",
                 (unsigned)root, (unsigned)first, (unsigned)cur, steps, (unsigned)dlghi.child_hwnd_count);
        smoke_expect(&s, first != 0 && wrapOk, "OOP DialogLab tab order wraps", dlgdetail);

        HWND modelessBtn = first ? GetNextDlgTabItem(root, first, FALSE) : 0;
        int modelessIdx = -1;
        int modelessCount = 0;
        if (modelessBtn) {
            /* v171: simulate the real physical BUTTON path, not a direct
               WM_COMMAND injection.  BUTTON sends BN_SETFOCUS on mouse-down and
               BN_CLICKED on mouse-up; DialogLab must activate only once. */
            hwnd_post(&rt->mgr, &rt->wm.wins[dlgr.window_index].app_cap, modelessBtn, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(6, 6));
            hwnd_post(&rt->mgr, &rt->wm.wins[dlgr.window_index].app_cap, modelessBtn, WM_LBUTTONUP, 0, MAKELPARAM(6, 6));
            for (int spin = 0; spin < 220; ++spin) {
                MyWinPollProcess(dlgr.process_id);
                hwnd_dispatch(&rt->mgr);
                wm_poll(&rt->wm);
                modelessIdx = -1;
                modelessCount = 0;
                for (int wi = 0; wi < rt->wm.count; ++wi) {
                    if (wi == dlgr.window_index) continue;
                    if (!rt->wm.wins[wi].closed && rt->wm.wins[wi].app_type == APP_IPC_PROXY &&
                        rt->wm.wins[wi].process_id == dlgr.process_id &&
                        strstr(rt->wm.wins[wi].title, "Modeless") != NULL) {
                        if (modelessIdx < 0) modelessIdx = wi;
                        modelessCount++;
                    }
                }
                if (modelessCount == 1 && spin > 30) break;
                usleep(10000);
            }
        }
        snprintf(dlgdetail, sizeof(dlgdetail), "modelessBtn=0x%x idx=%d modelessCount=%d total=%d", (unsigned)modelessBtn, modelessIdx, modelessCount, rt->wm.count);
        smoke_expect(&s, modelessBtn != 0 && modelessIdx >= 0 && modelessCount == 1, "OOP DialogLab physical BUTTON opens one modeless HWND", dlgdetail);

        /* v169: regression tripwire for the architectural fix.  Multiple
           top-level windows owned by the same OOP child must retain distinct
           GDI command streams.  A per-process stream would make root/modeless
           replay the same surface; tagged commands prove the parent compositor
           can filter by HWND. */
        int rootGdiCmds = 0;
        int modelessGdiCmds = 0;
        HWND modelessRoot = (modelessIdx >= 0 && modelessIdx < rt->wm.count) ? rt->wm.wins[modelessIdx].app_hwnd : 0;
        for (int spin = 0; spin < 120; ++spin) {
            MyWinPollProcess(dlgr.process_id);
            hwnd_dispatch(&rt->mgr);
            wm_poll(&rt->wm);
            memset(&dlghi, 0, sizeof(dlghi));
            if (MyProcessHostGetInfo(dlgr.process_id, &dlghi)) {
                rootGdiCmds = 0;
                modelessGdiCmds = 0;
                DWORD n = dlghi.gdi_command_count;
                if (n > MYOS_GDI_MAX_COMMANDS) n = MYOS_GDI_MAX_COMMANDS;
                for (DWORD gi = 0; gi < n; ++gi) {
                    if (dlghi.gdi_commands[gi].hwnd == (uint32_t)root) rootGdiCmds++;
                    if (modelessRoot && dlghi.gdi_commands[gi].hwnd == (uint32_t)modelessRoot) modelessGdiCmds++;
                }
                if (rootGdiCmds > 0 && modelessGdiCmds > 0) break;
            }
            usleep(10000);
        }
        snprintf(dlgdetail, sizeof(dlgdetail), "root=0x%x cmds=%d modeless=0x%x cmds=%d total=%u status=%.60s",
                 (unsigned)root, rootGdiCmds, (unsigned)modelessRoot, modelessGdiCmds,
                 (unsigned)dlghi.gdi_command_count, dlghi.gdi_status);
        smoke_expect(&s, rootGdiCmds > 0 && modelessGdiCmds > 0,
                     "OOP DialogLab GDI streams are per-HWND", dlgdetail);

        int modalIdx = -1;
        BOOL ownerWasEnabled = IsWindowEnabled(root);
        if (first) {
            hwnd_post(&rt->mgr, &rt->wm.wins[dlgr.window_index].app_cap, root, WM_COMMAND, MAKEWPARAM(0x8801u, BN_CLICKED), (LPARAM)first);
            for (int spin = 0; spin < 200; ++spin) {
                MyWinPollProcess(dlgr.process_id);
                hwnd_dispatch(&rt->mgr);
                wm_poll(&rt->wm);
                for (int wi = 0; wi < rt->wm.count; ++wi) {
                    if (wi == dlgr.window_index) continue;
                    if (!rt->wm.wins[wi].closed && rt->wm.wins[wi].app_type == APP_IPC_PROXY &&
                        rt->wm.wins[wi].process_id == dlgr.process_id &&
                        strstr(rt->wm.wins[wi].title, "Modal") != NULL) { modalIdx = wi; break; }
                }
                if (modalIdx >= 0 && !IsWindowEnabled(root)) break;
                usleep(10000);
            }
        }
        snprintf(dlgdetail, sizeof(dlgdetail), "modalBtn=0x%x idx=%d ownerBefore=%d ownerEnabled=%d", (unsigned)first, modalIdx, ownerWasEnabled, IsWindowEnabled(root));
        smoke_expect(&s, first != 0 && modalIdx >= 0 && ownerWasEnabled && !IsWindowEnabled(root),
                     "OOP DialogLab modal opens HWND and disables owner", dlgdetail);

        if (modalIdx >= 0) {
            HWND modalRoot = rt->wm.wins[modalIdx].app_hwnd;
            HWND okBtn = 0;
            for (int spin = 0; spin < 160; ++spin) {
                MyWinPollProcess(dlgr.process_id);
                hwnd_dispatch(&rt->mgr);
                wm_poll(&rt->wm);
                okBtn = GetNextDlgTabItem(modalRoot, 0, FALSE);
                if (okBtn) break;
                usleep(10000);
            }
            if (okBtn) hwnd_post(&rt->mgr, &rt->wm.wins[modalIdx].app_cap, modalRoot, WM_COMMAND, MAKEWPARAM(0x8A01u, BN_CLICKED), (LPARAM)okBtn);
            for (int spin = 0; spin < 200; ++spin) {
                MyWinPollProcess(dlgr.process_id);
                hwnd_dispatch(&rt->mgr);
                wm_poll(&rt->wm);
                if (IsWindowEnabled(root) && (!modalRoot || !IsWindow(modalRoot))) break;
                usleep(10000);
            }
            snprintf(dlgdetail, sizeof(dlgdetail), "modalRoot=0x%x ok=0x%x ownerEnabled=%d modalAlive=%d",
                     (unsigned)modalRoot, (unsigned)okBtn, IsWindowEnabled(root), modalRoot ? IsWindow(modalRoot) : 0);
            smoke_expect(&s, okBtn != 0 && IsWindowEnabled(root) && (!modalRoot || !IsWindow(modalRoot)), "OOP DialogLab modal OK re-enables owner and closes dialog", dlgdetail);
        }

        if (modelessIdx >= 0 && rt->wm.wins[modelessIdx].app_hwnd && IsWindow(rt->wm.wins[modelessIdx].app_hwnd))
            wm_close_hwnd(&rt->wm, rt->wm.wins[modelessIdx].app_hwnd);
        for (int spin = 0; spin < 80; ++spin) {
            MyWinPollProcess(dlgr.process_id);
            hwnd_dispatch(&rt->mgr);
            wm_poll(&rt->wm);
            if (modelessIdx < 0 || !IsWindow(rt->wm.wins[modelessIdx].app_hwnd)) break;
            usleep(5000);
        }
        if (root && IsWindow(root))
            wm_close_hwnd(&rt->wm, root);
        if (dlgr.hProcess) CloseHandle(dlgr.hProcess);
        if (dlgr.hThread) CloseHandle(dlgr.hThread);
    }

    /* v183: strict lifetime/leak audit.  AppHost's loader hProcess/hThread
       handles are table handles owned by the caller capability.  Closing an
       AppHost window must not depend on a magic PID fallback and must leave the
       loader owner's handle table at baseline. */
    for (int spin = 0; spin < 240; ++spin) {
        MyWinPollAllProcesses();
        hwnd_dispatch(&rt->mgr);
        wm_poll(&rt->wm);
        usleep(5000);
    }
    MyWindowLifetimeAudit auditAfter;
    memset(&auditAfter, 0, sizeof(auditAfter));
    wm_get_lifetime_audit_stats(&auditAfter);
    MyProcessHostSweepFinalizedResources();
    MyProcessHostAudit phAuditAfter;
    memset(&phAuditAfter, 0, sizeof(phAuditAfter));
    MyProcessHostGetAuditStats(&phAuditAfter);
    MyWinSweepExitedHandleTables();
    MyHandleTableAudit htAuditAfter;
    memset(&htAuditAfter, 0, sizeof(htAuditAfter));
    MyWinGetHandleTableAudit(&htAuditAfter);
    HWNDManagerStats hwndAuditAfter;
    memset(&hwndAuditAfter, 0, sizeof(hwndAuditAfter));
    hwnd_get_stats(&rt->mgr, &hwndAuditAfter);
    DWORD ownerHandlesAfter = MyGetHandleCount(lifetimeOwnerPid);
    DWORD procObjAfter = MyGetObjectCountByType(_OBJECT_TYPE_PROCESS);
    DWORD threadObjAfter = MyGetObjectCountByType(_OBJECT_TYPE_THREAD);
    char ldetail[192];
    snprintf(ldetail, sizeof(ldetail), "ownerPid=%u handles %u->%u audit ok=%u fail=%u miss=%u rec=%u mismatch=%u",
             (unsigned)lifetimeOwnerPid, (unsigned)ownerHandlesBefore, (unsigned)ownerHandlesAfter,
             (unsigned)auditAfter.loader_close_ok, (unsigned)auditAfter.loader_close_fail,
             (unsigned)auditAfter.loader_missing_owner, (unsigned)auditAfter.loader_recovered_owner,
             (unsigned)auditAfter.loader_owner_mismatch);
    smoke_expect(&s, ownerHandlesAfter == ownerHandlesBefore,
                 "AppHost loader handle table returns to baseline", ldetail);
    smoke_expect(&s, auditAfter.loader_close_fail == auditBefore.loader_close_fail &&
                     auditAfter.loader_missing_owner == auditBefore.loader_missing_owner &&
                     auditAfter.loader_owner_mismatch == auditBefore.loader_owner_mismatch,
                 "AppHost loader cleanup has no strict lifetime violations", ldetail);
    snprintf(ldetail, sizeof(ldetail), "PROCESS obj %u->%u THREAD obj %u->%u live=%u exited=%u",
             (unsigned)procObjBefore, (unsigned)procObjAfter,
             (unsigned)threadObjBefore, (unsigned)threadObjAfter,
             (unsigned)MyGetProcessLiveCount(), (unsigned)MyGetProcessExitedCount());
    smoke_expect(&s, procObjAfter <= procObjBefore + 1 && threadObjAfter <= threadObjBefore + 1,
                 "AppHost PROCESS/THREAD objects do not grow unbounded", ldetail);

    snprintf(ldetail, sizeof(ldetail),
             "HT total=%u owners=%u deadHandles=%u deadTables=%u orphan=%u swept=%u->%u fail=%u",
             (unsigned)htAuditAfter.total_handles, (unsigned)htAuditAfter.owner_pid_count,
             (unsigned)htAuditAfter.exited_owner_handles, (unsigned)htAuditAfter.dead_pid_tables,
             (unsigned)htAuditAfter.orphan_owner_handles,
             (unsigned)htAuditBefore.swept_handles, (unsigned)htAuditAfter.swept_handles,
             (unsigned)htAuditAfter.sweep_failures);
    smoke_expect(&s, htAuditAfter.exited_owner_handles == 0 && htAuditAfter.dead_pid_tables == 0,
                 "Handle table audit has no dead PID tables", ldetail);
    smoke_expect(&s, htAuditAfter.sweep_failures == htAuditBefore.sweep_failures,
                 "Handle table exit sweep has no ref-release failures", ldetail);

    char hwnddetail[224];
    snprintf(hwnddetail, sizeof(hwnddetail),
             "HWND count %u->%u WSTS active %u->%u dead %u->%u cap=%u",
             (unsigned)hwndAuditBefore.hwnd_count, (unsigned)hwndAuditAfter.hwnd_count,
             (unsigned)hwndAuditBefore.state_active, (unsigned)hwndAuditAfter.state_active,
             (unsigned)hwndAuditBefore.state_destroyed, (unsigned)hwndAuditAfter.state_destroyed,
             (unsigned)hwndAuditAfter.state_capacity);
    smoke_expect(&s, hwndAuditAfter.hwnd_count == hwndAuditBefore.hwnd_count &&
                     hwndAuditAfter.state_active == hwndAuditBefore.state_active,
                 "AppHost shell close destroys child HWND tree", hwnddetail);

    char phdetail[320];
    snprintf(phdetail, sizeof(phdetail),
             "PH valid=%u run=%u final=%u open=%u finalOpen=%u cleanup=%u err=%u reclaim=%u beforeFinalOpen=%u beforeErr=%u async=%u polls=%u reaps=%u notify=%u",
             (unsigned)phAuditAfter.valid_count, (unsigned)phAuditAfter.running_count,
             (unsigned)phAuditAfter.final_count, (unsigned)phAuditAfter.open_ipc_resources,
             (unsigned)phAuditAfter.final_open_ipc_resources, (unsigned)phAuditAfter.cleanup_count,
             (unsigned)phAuditAfter.cleanup_error_count, (unsigned)phAuditAfter.reclaimed_slots,
             (unsigned)phAuditBefore.final_open_ipc_resources, (unsigned)phAuditBefore.cleanup_error_count,
             (unsigned)phAuditAfter.async_reaper_started, (unsigned)phAuditAfter.async_reaper_polls,
             (unsigned)phAuditAfter.async_reaper_reaps, (unsigned)phAuditAfter.async_reaper_notifications);
    smoke_expect(&s, phAuditAfter.final_open_ipc_resources == phAuditBefore.final_open_ipc_resources,
                 "ProcessHost finalized children release IPC/shm resources", phdetail);
    smoke_expect(&s, phAuditAfter.cleanup_error_count == phAuditBefore.cleanup_error_count,
                 "ProcessHost reaper cleanup has no resource errors", phdetail);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static int g_v150_proc_a_creates = 0;
static int g_v150_proc_b_creates = 0;
static int g_v150_proc_c_creates = 0;

static LRESULT CALLBACK smoke_v150_proc_a(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    (void)hWnd; (void)wParam; (void)lParam;
    if (Msg == WM_CREATE) g_v150_proc_a_creates++;
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

static LRESULT CALLBACK smoke_v150_proc_b(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    (void)hWnd; (void)wParam; (void)lParam;
    if (Msg == WM_CREATE) g_v150_proc_b_creates++;
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

static LRESULT CALLBACK smoke_v150_proc_c(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    (void)hWnd; (void)wParam; (void)lParam;
    if (Msg == WM_CREATE) g_v150_proc_c_creates++;
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}



static int g_v151_base_user = 0;
static int g_v151_sub_user = 0;
static int g_v151_call_old = 0;
static int g_v151_class2_create = 0;
static WNDPROC g_v151_old_proc = NULL;

static LRESULT CALLBACK smoke_v151_base_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (Msg == WM_CREATE) return 0;
    if (Msg == WM_USER + 151) {
        (void)wParam; (void)lParam;
        g_v151_base_user++;
        return 151;
    }
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

static LRESULT CALLBACK smoke_v151_sub_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (Msg == WM_USER + 151) {
        g_v151_sub_user++;
        LRESULT old = CallWindowProcA(g_v151_old_proc, hWnd, Msg, wParam, lParam);
        if (old == 151) g_v151_call_old++;
        return old + 1;
    }
    return CallWindowProcA(g_v151_old_proc, hWnd, Msg, wParam, lParam);
}

static LRESULT CALLBACK smoke_v151_class2_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    if (Msg == WM_CREATE) {
        g_v151_class2_create++;
        return 0;
    }
    if (Msg == WM_USER + 151) return 2151;
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

static int smoke_user32_longptr(SmokeRuntime* rt)
{
    smoke_runtime_init(rt);
    SmokeContext s = {0,0,0,0,"user32_longptr"};
    Capability owner = rt->cap;
    Capability procA = cap_create(1511, "v151-longptr-a", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    Capability procB = cap_create(1512, "v151-longptr-b", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&procA, 0);
    cap_add_target(&procB, 0);

    g_v151_base_user = g_v151_sub_user = g_v151_call_old = g_v151_class2_create = 0;
    g_v151_old_proc = NULL;

    const char* clsName = "myOS.v151.LongPtrShared";
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_v151_base_proc;
    wc.lpszClassName = clsName;
    wc.style = 0x151u;
    wc.cbWndExtra = (int)(2 * sizeof(LONG_PTR));
    wc.cbClsExtra = (int)(2 * sizeof(LONG_PTR));

    MyWinBindRuntime(&rt->mgr, &procA);
    MyWinBindDesktop(&rt->wm);
    ATOM atomA = RegisterClassExA(&wc);
    smoke_expect(&s, atomA != 0, "RegisterClassExA cbExtra", "class owns cbWndExtra/cbClsExtra storage");
    HWND hA = CreateWindowExA(0x40, clsName, "v151 A", WS_OVERLAPPEDWINDOW, 10, 10, 160, 90, 0, 0, 0, NULL);
    smoke_expect(&s, hA != 0, "CreateWindowExA cbExtra", "window receives class-sized extra storage");

    smoke_expect(&s, hA && GetClassLongPtrA(hA, GCL_CBWNDEXTRA) == (LONG_PTR)(2 * sizeof(LONG_PTR)),
                 "GetClassLongPtrA(GCL_CBWNDEXTRA)", "class exposes window extra byte count");
    smoke_expect(&s, hA && GetClassLongPtrA(hA, GCL_CBCLSEXTRA) == (LONG_PTR)(2 * sizeof(LONG_PTR)),
                 "GetClassLongPtrA(GCL_CBCLSEXTRA)", "class exposes class extra byte count");
    smoke_expect(&s, hA && GetClassLongPtrA(hA, GCW_ATOM) == (LONG_PTR)atomA,
                 "GetClassLongPtrA(GCW_ATOM)", "class atom stored on HWND metadata");

    smoke_expect(&s, hA && SetWindowLongPtrA(hA, GWLP_USERDATA, (LONG_PTR)0x1510) == 0,
                 "SetWindowLongPtrA(GWLP_USERDATA)", "userdata initial zero -> value");
    smoke_expect(&s, hA && GetWindowLongPtrA(hA, GWLP_USERDATA) == (LONG_PTR)0x1510,
                 "GetWindowLongPtrA(GWLP_USERDATA)", "userdata roundtrip");

    LONG_PTR oldStyle = hA ? SetWindowLongPtrA(hA, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_DISABLED) : 0;
    smoke_expect(&s, hA && (oldStyle & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW,
                 "SetWindowLongPtrA(GWL_STYLE)", "returns previous style");
    smoke_expect(&s, hA && (GetWindowLongPtrA(hA, GWL_STYLE) & WS_DISABLED),
                 "GetWindowLongPtrA(GWL_STYLE)", "style mutation visible");
    smoke_expect(&s, hA && SetWindowLongPtrA(hA, GWL_EXSTYLE, 0x88) == 0x40,
                 "SetWindowLongPtrA(GWL_EXSTYLE)", "returns previous exstyle");
    smoke_expect(&s, hA && GetWindowLongPtrA(hA, GWL_EXSTYLE) == 0x88,
                 "GetWindowLongPtrA(GWL_EXSTYLE)", "exstyle mutation visible");

    smoke_expect(&s, hA && SetWindowLongPtrA(hA, 0, (LONG_PTR)0xAAA1) == 0,
                 "SetWindowLongPtrA(cbWndExtra[0])", "first extra slot starts zero");
    smoke_expect(&s, hA && SetWindowLongPtrA(hA, (int)sizeof(LONG_PTR), (LONG_PTR)0xAAA2) == 0,
                 "SetWindowLongPtrA(cbWndExtra[1])", "second extra slot starts zero");
    smoke_expect(&s, hA && GetWindowLongPtrA(hA, 0) == (LONG_PTR)0xAAA1 &&
                     GetWindowLongPtrA(hA, (int)sizeof(LONG_PTR)) == (LONG_PTR)0xAAA2,
                 "GetWindowLongPtrA(cbWndExtra)", "window extra slots roundtrip");
    SetLastError(0x51515151u);
    LONG_PTR badExtra = hA ? GetWindowLongPtrA(hA, (int)(2 * sizeof(LONG_PTR))) : 0;
    smoke_expect(&s, badExtra == 0, "GetWindowLongPtrA(cbWndExtra OOB)", "offset exactly past cbWndExtra fails");
    smoke_expect_last_error(&s, ERROR_INVALID_INDEX, "GetWindowLongPtrA(cbWndExtra OOB) LastError");

    smoke_expect(&s, hA && SetClassLongPtrA(hA, 0, (LONG_PTR)0xCA01) == 0,
                 "SetClassLongPtrA(cbClsExtra[0])", "first class extra starts zero");
    smoke_expect(&s, hA && SetClassLongPtrA(hA, (int)sizeof(LONG_PTR), (LONG_PTR)0xCA02) == 0,
                 "SetClassLongPtrA(cbClsExtra[1])", "second class extra starts zero");
    smoke_expect(&s, hA && GetClassLongPtrA(hA, 0) == (LONG_PTR)0xCA01 &&
                     GetClassLongPtrA(hA, (int)sizeof(LONG_PTR)) == (LONG_PTR)0xCA02,
                 "GetClassLongPtrA(cbClsExtra)", "class extra slots roundtrip");
    SetLastError(0x51515151u);
    LONG_PTR badClassExtra = hA ? SetClassLongPtrA(hA, (int)(2 * sizeof(LONG_PTR)), (LONG_PTR)0xBAD) : 0;
    smoke_expect(&s, badClassExtra == 0, "SetClassLongPtrA(cbClsExtra OOB)", "class extra bounds enforced");
    smoke_expect_last_error(&s, ERROR_INVALID_INDEX, "SetClassLongPtrA(cbClsExtra OOB) LastError");

    LONG_PTR oldClassStyle = hA ? SetClassLongPtrA(hA, GCL_STYLE, 0x5151) : 0;
    smoke_expect(&s, hA && oldClassStyle == 0x151,
                 "SetClassLongPtrA(GCL_STYLE)", "class style returns previous value");
    smoke_expect(&s, hA && GetClassLongPtrA(hA, GCL_STYLE) == 0x5151,
                 "GetClassLongPtrA(GCL_STYLE)", "class style mutation visible");

    g_v151_old_proc = (WNDPROC)SetWindowLongPtrA(hA, GWLP_WNDPROC, (LONG_PTR)smoke_v151_sub_proc);
    smoke_expect(&s, hA && g_v151_old_proc == smoke_v151_base_proc,
                 "SetWindowLongPtrA(GWLP_WNDPROC)", "subclass returns old per-HWND WndProc");
    LRESULT subResult = hA ? SendMessageA(hA, WM_USER + 151, 0, 0) : 0;
    smoke_expect(&s, subResult == 152 && g_v151_sub_user == 1 && g_v151_base_user == 1 && g_v151_call_old == 1,
                 "subclass dispatch + CallWindowProcA", "current HWND proc calls previous proc and returns chained result");

    WNDPROC oldClassProc = hA ? (WNDPROC)SetClassLongPtrA(hA, GCLP_WNDPROC, (LONG_PTR)smoke_v151_class2_proc) : NULL;
    smoke_expect(&s, oldClassProc == smoke_v151_base_proc,
                 "SetClassLongPtrA(GCLP_WNDPROC)", "class WndProc replacement returns old class proc");
    HWND hA2 = CreateWindowExA(0, clsName, "v151 A2", WS_OVERLAPPEDWINDOW, 20, 20, 160, 90, 0, 0, 0, NULL);
    smoke_expect(&s, hA2 != 0 && g_v151_class2_create == 1,
                 "CreateWindowExA after GCLP_WNDPROC", "new window uses replaced class WndProc");

    MyWinBindRuntime(&rt->mgr, &procB);
    MyWinBindDesktop(&rt->wm);
    wc.lpfnWndProc = smoke_v151_base_proc;
    ATOM atomB = RegisterClassExA(&wc);
    HWND hB = atomB ? CreateWindowExA(0, clsName, "v151 B", WS_OVERLAPPEDWINDOW, 30, 30, 160, 90, 0, 0, 0, NULL) : 0;
    smoke_expect(&s, atomB != 0 && atomB != atomA && hB != 0,
                 "per-process classlong fixture", "same class name gets isolated class storage");
    smoke_expect(&s, hB && SetClassLongPtrA(hB, 0, (LONG_PTR)0xCB01) == 0,
                 "proc B SetClassLongPtrA(cbClsExtra)", "B class extra starts independent zero");

    MyWinBindRuntime(&rt->mgr, &procA);
    MyWinBindDesktop(&rt->wm);
    smoke_expect(&s, hA && GetClassLongPtrA(hA, 0) == (LONG_PTR)0xCA01,
                 "proc A class extra isolated", "B mutation did not touch A class extra");
    MyWinBindRuntime(&rt->mgr, &procB);
    MyWinBindDesktop(&rt->wm);
    smoke_expect(&s, hB && GetClassLongPtrA(hB, 0) == (LONG_PTR)0xCB01,
                 "proc B class extra isolated", "B retains its own class extra value");

    SetLastError(0x51515151u);
    LONG_PTR invalidHwnd = GetWindowLongPtrA(0, GWLP_ID);
    smoke_expect(&s, invalidHwnd == 0, "GetWindowLongPtrA(NULL)", "invalid HWND rejected");
    smoke_expect_last_error(&s, ERROR_INVALID_WINDOW_HANDLE, "GetWindowLongPtrA(NULL) LastError");

    HWND hBtn = CreateWindowExA(0, "BUTTON", "B", WS_CHILD|WS_VISIBLE, 1, 1, 60, 24, 0, (HMENU)1, 0, NULL);
    SetLastError(0x51515151u);
    LONG_PTR sysMutate = hBtn ? SetClassLongPtrA(hBtn, GCL_STYLE, 0x2222) : 0;
    smoke_expect(&s, hBtn && sysMutate == 0, "SetClassLongPtrA(system BUTTON)", "system classes are protected from app mutation");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "SetClassLongPtrA(system BUTTON) LastError");
    if (hBtn) DestroyWindow(hBtn);

    MyWinBindRuntime(&rt->mgr, &procA);
    MyWinBindDesktop(&rt->wm);
    if (hA2) DestroyWindow(hA2);
    if (hA) DestroyWindow(hA);
    MyWinBindRuntime(&rt->mgr, &procB);
    MyWinBindDesktop(&rt->wm);
    if (hB) DestroyWindow(hB);

    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static int g_v152_changing = 0;
static int g_v152_changed = 0;
static int g_v152_move = 0;
static int g_v152_size = 0;
static int g_v152_show = 0;
static HWND g_v152_mutate_hwnd = 0;

static LRESULT CALLBACK smoke_v152_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    switch (Msg) {
    case WM_WINDOWPOSCHANGING:
        g_v152_changing++;
        if (hWnd == g_v152_mutate_hwnd && lParam) {
            WINDOWPOS* wp = (WINDOWPOS*)lParam;
            wp->x = 33;
            wp->y = 34;
            wp->cx = 111;
            wp->cy = 55;
            wp->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        }
        return 0;
    case WM_WINDOWPOSCHANGED:
        g_v152_changed++;
        return 0;
    case WM_MOVE:
        g_v152_move++;
        return 0;
    case WM_SIZE:
        g_v152_size++;
        return 0;
    case WM_SHOWWINDOW:
        g_v152_show++;
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static int smoke_user32_windowpos(SmokeRuntime* rt)
{
    smoke_runtime_init(rt);
    SmokeContext s = {0,0,0,0,"user32_windowpos"};
    Capability owner = rt->cap;
    Capability proc = cap_create(1521, "v152-windowpos", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&proc, 0);
    MyWinBindRuntime(&rt->mgr, &proc);
    MyWinBindDesktop(&rt->wm);

    g_v152_changing = g_v152_changed = g_v152_move = g_v152_size = g_v152_show = 0;
    g_v152_mutate_hwnd = 0;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_v152_proc;
    wc.lpszClassName = "myOS.v152.WindowPos";
    ATOM atom = RegisterClassExA(&wc);
    smoke_expect(&s, atom != 0, "RegisterClassExA", "v152 windowpos fixture class");

    HWND parent = CreateWindowExA(0, "myOS.v152.WindowPos", "v152 parent", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                  0, 0, 300, 200, 0, 0, 0, NULL);
    HWND child1 = parent ? CreateWindowExA(0, "myOS.v152.WindowPos", "child1", WS_CHILD|WS_VISIBLE,
                                           10, 10, 80, 30, parent, (HMENU)101, 0, NULL) : 0;
    HWND child2 = parent ? CreateWindowExA(0, "myOS.v152.WindowPos", "child2", WS_CHILD|WS_VISIBLE,
                                           20, 20, 80, 30, parent, (HMENU)102, 0, NULL) : 0;
    smoke_expect(&s, parent && child1 && child2, "CreateWindowExA hierarchy", "parent plus two visible children");
    smoke_expect(&s, IsWindowVisible(parent) && IsWindowVisible(child1), "IsWindowVisible initial", "visible chain is true");

    BOOL was = parent ? ShowWindow(parent, SW_HIDE) : FALSE;
    smoke_expect(&s, was == TRUE && !IsWindowVisible(parent) && !IsWindowVisible(child1),
                 "ShowWindow(SW_HIDE)", "returns previous visible state and hides parent chain");
    was = parent ? ShowWindow(parent, SW_SHOW) : TRUE;
    smoke_expect(&s, was == FALSE && IsWindowVisible(parent) && IsWindowVisible(child1),
                 "ShowWindow(SW_SHOW)", "returns previous hidden state and restores chain visibility");

    RECT r0, r1;
    memset(&r0, 0, sizeof(r0)); memset(&r1, 0, sizeof(r1));
    GetWindowRect(child1, &r0);
    smoke_expect(&s, child1 && SetWindowPos(child1, HWND_TOP, 99, 99, 120, 35, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
                 "SetWindowPos(SWP_NOMOVE)", "position preserved, size updated");
    GetWindowRect(child1, &r1);
    smoke_expect(&s, r1.left == r0.left && r1.top == r0.top && (r1.right-r1.left) == 120 && (r1.bottom-r1.top) == 35,
                 "SWP_NOMOVE result", "left/top unchanged while cx/cy commit");

    smoke_expect(&s, child1 && SetWindowPos(child1, HWND_TOP, 40, 42, 999, 888, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE),
                 "SetWindowPos(SWP_NOSIZE)", "position updated, size preserved");
    GetWindowRect(child1, &r1);
    smoke_expect(&s, r1.left == 40 && r1.top == 42 && (r1.right-r1.left) == 120 && (r1.bottom-r1.top) == 35,
                 "SWP_NOSIZE result", "cx/cy preserved while x/y commit");

    smoke_expect(&s, child1 && SetWindowPos(child1, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW | SWP_NOZORDER | SWP_NOACTIVATE),
                 "SetWindowPos(SWP_HIDEWINDOW)", "WS_VISIBLE cleared through USER32 metadata");
    smoke_expect(&s, child1 && !IsWindowVisible(child1), "IsWindowVisible child hidden", "own visibility bit is false");
    smoke_expect(&s, child1 && SetWindowPos(child1, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOZORDER | SWP_NOACTIVATE),
                 "SetWindowPos(SWP_SHOWWINDOW)", "WS_VISIBLE restored through USER32 metadata");
    smoke_expect(&s, child1 && IsWindowVisible(child1), "IsWindowVisible child shown", "own and parent chain visible");

    smoke_expect(&s, parent && GetWindow(parent, GW_CHILD) == child2, "initial child Z", "newer child starts above older sibling");
    smoke_expect(&s, child1 && SetWindowPos(child1, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
                 "SetWindowPos(HWND_TOP)", "child raised to top of local USER32 Z-order");
    smoke_expect(&s, parent && GetWindow(parent, GW_CHILD) == child1, "GW_CHILD after raise", "top child follows SetWindowPos");
    smoke_expect(&s, child1 && SetWindowPos(child1, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
                 "SetWindowPos(HWND_BOTTOM)", "child lowered to bottom of local USER32 Z-order");
    smoke_expect(&s, parent && GetWindow(parent, GW_CHILD) == child2, "GW_CHILD after lower", "lowered child no longer tops the sibling stack");

    g_v152_mutate_hwnd = child1;
    int beforeChanging = g_v152_changing;
    smoke_expect(&s, child1 && SetWindowPos(child1, HWND_TOP, 1, 2, 3, 4, SWP_NOZORDER | SWP_NOACTIVATE),
                 "WM_WINDOWPOSCHANGING mutation", "WndProc may mutate WINDOWPOS before commit");
    GetWindowRect(child1, &r1);
    smoke_expect(&s, r1.left == 33 && r1.top == 34 && (r1.right-r1.left) == 111 && (r1.bottom-r1.top) == 55 && g_v152_changing > beforeChanging,
                 "mutated WINDOWPOS committed", "x/y/cx/cy from CHANGING are authoritative");
    g_v152_mutate_hwnd = 0;

    HDWP hdwp = BeginDeferWindowPos(2);
    smoke_expect(&s, hdwp != 0, "BeginDeferWindowPos", "batch handle allocated");
    if (hdwp) hdwp = DeferWindowPos(hdwp, child1, HWND_TOP, 5, 6, 50, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    if (hdwp) hdwp = DeferWindowPos(hdwp, child2, HWND_TOP, 60, 6, 55, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    smoke_expect(&s, hdwp != 0 && EndDeferWindowPos(hdwp), "EndDeferWindowPos", "batched SetWindowPos operations applied");
    RECT r2;
    GetWindowRect(child1, &r1); GetWindowRect(child2, &r2);
    smoke_expect(&s, r1.left == 5 && r1.top == 6 && (r1.right-r1.left) == 50 &&
                     r2.left == 60 && r2.top == 6 && (r2.right-r2.left) == 55,
                 "defer batch geometry", "both children moved by EndDeferWindowPos");

    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    smoke_expect(&s, parent && GetWindowPlacement(parent, &wp), "GetWindowPlacement", "returns normal position and showCmd");
    wp.showCmd = SW_HIDE;
    wp.rcNormalPosition.left = 200;
    wp.rcNormalPosition.top = 210;
    wp.rcNormalPosition.right = 360;
    wp.rcNormalPosition.bottom = 300;
    smoke_expect(&s, parent && SetWindowPlacement(parent, &wp), "SetWindowPlacement(SW_HIDE)", "placement applies rect and show command");
    GetWindowRect(parent, &r1);
    smoke_expect(&s, r1.left == 200 && r1.top == 210 && (r1.right-r1.left) == 160 && !IsWindowVisible(parent),
                 "placement hidden result", "normal rect stored and visibility hidden");
    wp.showCmd = SW_SHOWNORMAL;
    smoke_expect(&s, parent && SetWindowPlacement(parent, &wp) && IsWindowVisible(parent),
                 "SetWindowPlacement(SW_SHOWNORMAL)", "placement restores visibility");

    WINDOWPLACEMENT bad;
    memset(&bad, 0, sizeof(bad));
    bad.length = sizeof(WINDOWPLACEMENT) - 1;
    SetLastError(0x152152u);
    smoke_expect(&s, !GetWindowPlacement(parent, &bad), "GetWindowPlacement bad length", "length validation enforced");
    smoke_expect_last_error(&s, ERROR_INVALID_PARAMETER, "GetWindowPlacement bad length LastError");

    if (child2) DestroyWindow(child2);
    if (child1) DestroyWindow(child1);
    if (parent) DestroyWindow(parent);
    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}



static int g_v153_init_count = 0;
static int g_v153_proc1_user = 0;
static int g_v153_proc2_user = 0;
static int g_v153_false_count = 0;

typedef struct MYOS_PACKED SmokeV153DialogTemplate {
    DLGTEMPLATE dlg;
    WORD menu;
    WORD className;
    WORD title;
} SmokeV153DialogTemplate;

static const SmokeV153DialogTemplate g_v153_dialog_template = {
    { WS_VISIBLE, 0, 0, 4, 4, 96, 48 },
    0, 0, 0
};

static INT_PTR CALLBACK smoke_v153_dlgproc1(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    if (uMsg == WM_INITDIALOG) {
        g_v153_init_count++;
        SetWindowLongPtrA(hDlg, DWLP_USER, (LONG_PTR)0x15300001);
        return TRUE;
    }
    if (uMsg == WM_USER + 153) {
        g_v153_proc1_user++;
        SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, (LONG_PTR)0x15310001);
        return TRUE;
    }
    if (uMsg == WM_USER + 154) {
        g_v153_false_count++;
        return FALSE;
    }
    return FALSE;
}

static INT_PTR CALLBACK smoke_v153_dlgproc2(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    if (uMsg == WM_USER + 153) {
        g_v153_proc2_user++;
        SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, (LONG_PTR)0x15320002);
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK smoke_v153_modal_end_proc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    if (uMsg == WM_INITDIALOG) {
        EndDialog(hDlg, (INT_PTR)0x1530BEEF);
        return TRUE;
    }
    return FALSE;
}


static void smoke_v195_w16(BYTE** pp, WORD v) { memcpy(*pp, &v, sizeof(v)); *pp += sizeof(v); }
static void smoke_v195_w32(BYTE** pp, DWORD v) { memcpy(*pp, &v, sizeof(v)); *pp += sizeof(v); }
static void smoke_v195_i16(BYTE** pp, short v) { memcpy(*pp, &v, sizeof(v)); *pp += sizeof(v); }
static void smoke_v195_align4(BYTE** pp, BYTE* base)
{
    uintptr_t off = (uintptr_t)(*pp - base);
    while (off & 3u) { *(*pp)++ = 0; off++; }
}
static void smoke_v195_wstr(BYTE** pp, const char* s)
{
    if (!s) s = "";
    while (*s) smoke_v195_w16(pp, (WORD)(unsigned char)*s++);
    smoke_v195_w16(pp, 0);
}
static void smoke_v195_ord(BYTE** pp, WORD atom)
{
    smoke_v195_w16(pp, 0xFFFFu);
    smoke_v195_w16(pp, atom);
}

static LPCDLGTEMPLATEA smoke_v195_build_old_template(BYTE* buf, size_t cb, WORD point, WORD cdit, const char* title)
{
    (void)cb;
    memset(buf, 0, 512);
    BYTE* p = buf;
    smoke_v195_w32(&p, WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VISIBLE);
    smoke_v195_w32(&p, 0);
    smoke_v195_w16(&p, cdit);
    smoke_v195_i16(&p, 4); smoke_v195_i16(&p, 4); smoke_v195_i16(&p, 150); smoke_v195_i16(&p, 72);
    smoke_v195_w16(&p, 0);       /* menu */
    smoke_v195_w16(&p, 0);       /* default #32770 */
    smoke_v195_wstr(&p, title ? title : "v195 old");
    smoke_v195_w16(&p, point);
    smoke_v195_wstr(&p, "MS Shell Dlg");
    smoke_v195_align4(&p, buf);

    /* STATIC id 19501 */
    smoke_v195_w32(&p, WS_CHILD | WS_VISIBLE);
    smoke_v195_w32(&p, 0);
    smoke_v195_i16(&p, 6); smoke_v195_i16(&p, 6); smoke_v195_i16(&p, 60); smoke_v195_i16(&p, 10);
    smoke_v195_w16(&p, 19501);
    smoke_v195_ord(&p, MYOS_DLG_CLASS_STATIC);
    smoke_v195_wstr(&p, "Static");
    smoke_v195_w16(&p, 0);
    smoke_v195_align4(&p, buf);

    /* EDIT id 19502 */
    smoke_v195_w32(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL);
    smoke_v195_w32(&p, 0);
    smoke_v195_i16(&p, 6); smoke_v195_i16(&p, 20); smoke_v195_i16(&p, 80); smoke_v195_i16(&p, 12);
    smoke_v195_w16(&p, 19502);
    smoke_v195_ord(&p, MYOS_DLG_CLASS_EDIT);
    smoke_v195_wstr(&p, "edit");
    smoke_v195_w16(&p, 0);
    smoke_v195_align4(&p, buf);

    /* OK button id IDOK */
    smoke_v195_w32(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON);
    smoke_v195_w32(&p, 0);
    smoke_v195_i16(&p, 92); smoke_v195_i16(&p, 20); smoke_v195_i16(&p, 42); smoke_v195_i16(&p, 14);
    smoke_v195_w16(&p, IDOK);
    smoke_v195_ord(&p, MYOS_DLG_CLASS_BUTTON);
    smoke_v195_wstr(&p, "OK");
    smoke_v195_w16(&p, 0);
    smoke_v195_align4(&p, buf);

    /* Cancel button id IDCANCEL */
    smoke_v195_w32(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON);
    smoke_v195_w32(&p, 0);
    smoke_v195_i16(&p, 92); smoke_v195_i16(&p, 38); smoke_v195_i16(&p, 42); smoke_v195_i16(&p, 14);
    smoke_v195_w16(&p, IDCANCEL);
    smoke_v195_ord(&p, MYOS_DLG_CLASS_BUTTON);
    smoke_v195_wstr(&p, "Cancel");
    smoke_v195_w16(&p, 0);
    smoke_v195_align4(&p, buf);

    return (LPCDLGTEMPLATEA)buf;
}

static LPCDLGTEMPLATEA smoke_v195_build_ex_template(BYTE* buf, size_t cb)
{
    (void)cb;
    memset(buf, 0, 512);
    BYTE* p = buf;
    smoke_v195_w16(&p, 1);           /* dlgVer */
    smoke_v195_w16(&p, 0xFFFFu);     /* signature */
    smoke_v195_w32(&p, 0x19500001u); /* helpID */
    smoke_v195_w32(&p, 0x00000200u); /* exStyle */
    smoke_v195_w32(&p, WS_CAPTION | WS_SYSMENU | DS_SHELLFONT | WS_VISIBLE);
    smoke_v195_w16(&p, 2);           /* items */
    smoke_v195_i16(&p, 8); smoke_v195_i16(&p, 8); smoke_v195_i16(&p, 120); smoke_v195_i16(&p, 60);
    smoke_v195_w16(&p, 0);           /* menu */
    smoke_v195_w16(&p, 0);           /* class */
    smoke_v195_wstr(&p, "v195 ex");
    smoke_v195_w16(&p, 9);           /* point */
    smoke_v195_w16(&p, 700);         /* weight */
    *p++ = 0;                        /* italic */
    *p++ = 1;                        /* charset */
    smoke_v195_wstr(&p, "MS Shell Dlg");
    smoke_v195_align4(&p, buf);

    smoke_v195_w32(&p, 0x19500010u); /* item helpID */
    smoke_v195_w32(&p, 0x00000400u); /* item exStyle */
    smoke_v195_w32(&p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON);
    smoke_v195_i16(&p, 8); smoke_v195_i16(&p, 8); smoke_v195_i16(&p, 50); smoke_v195_i16(&p, 14);
    smoke_v195_w32(&p, 19520);
    smoke_v195_ord(&p, MYOS_DLG_CLASS_BUTTON);
    smoke_v195_wstr(&p, "ExBtn");
    smoke_v195_w16(&p, 0);
    smoke_v195_align4(&p, buf);

    smoke_v195_w32(&p, 0x19500011u);
    smoke_v195_w32(&p, 0);
    smoke_v195_w32(&p, WS_CHILD | WS_VISIBLE);
    smoke_v195_i16(&p, 8); smoke_v195_i16(&p, 28); smoke_v195_i16(&p, 80); smoke_v195_i16(&p, 10);
    smoke_v195_w32(&p, 19521);
    smoke_v195_ord(&p, MYOS_DLG_CLASS_STATIC);
    smoke_v195_wstr(&p, "ExStatic");
    smoke_v195_w16(&p, 0);
    smoke_v195_align4(&p, buf);

    return (LPCDLGTEMPLATEA)buf;
}

static INT_PTR CALLBACK smoke_v195_dlgproc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)hDlg; (void)wParam; (void)lParam;
    if (uMsg == WM_INITDIALOG) return TRUE;
    return FALSE;
}

static int smoke_dialog_template_engine(SmokeRuntime* rt)
{
    smoke_runtime_init(rt);
    SmokeContext s = {0,0,0,0,"dialogtpl"};
    Capability owner = rt->cap;
    Capability proc = cap_create(1951, "v195-dialog-template", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&proc, 0);
    MyWinBindRuntime(&rt->mgr, &proc);
    MyWinBindDesktop(&rt->wm);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_wndproc;
    wc.lpszClassName = "myOS.v195.TemplateOwner";
    RegisterClassExA(&wc);
    HWND ownerWnd = CreateWindowExA(0, "myOS.v195.TemplateOwner", "v195 owner", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                    30, 30, 300, 180, 0, 0, 0, NULL);
    smoke_expect(&s, ownerWnd != 0, "template owner", "owner HWND for dialog template engine tests");

    BYTE oldBlob[768];
    BYTE bigBlob[768];
    BYTE exBlob[768];
    LPCDLGTEMPLATEA oldTpl = smoke_v195_build_old_template(oldBlob, sizeof(oldBlob), 8, 4, "v195 old");
    HWND oldDlg = ownerWnd ? CreateDialogIndirectParamA(0, oldTpl, ownerWnd, smoke_v195_dlgproc, 0x195) : 0;
    HWND st = oldDlg ? GetDlgItem(oldDlg, 19501) : 0;
    HWND edit = oldDlg ? GetDlgItem(oldDlg, 19502) : 0;
    HWND ok = oldDlg ? GetDlgItem(oldDlg, IDOK) : 0;
    HWND cancel = oldDlg ? GetDlgItem(oldDlg, IDCANCEL) : 0;
    smoke_expect(&s, oldDlg && st && edit && ok && cancel,
                 "DLGTEMPLATE creates controls", "old-format template creates STATIC/EDIT/OK/Cancel from ordinal classes");
    smoke_expect(&s, oldDlg && GetWindow(oldDlg, GW_OWNER) == ownerWnd && GetParent(oldDlg) == 0,
                 "DLGTEMPLATE dialog owner", "CreateDialog owner is tracked as GW_OWNER, not child parent");
    char txt[64];
    memset(txt, 0, sizeof(txt));
    if (ok) GetWindowTextA(ok, txt, sizeof(txt));
    smoke_expect(&s, strcmp(txt, "OK") == 0 && (GetWindowLongPtrA(ok, GWLP_ID) == IDOK),
                 "DLGITEM title/id", "item title and control id survive template creation");
    smoke_expect(&s, ok && ((DWORD)GetWindowLongPtrA(ok, GWL_STYLE) & WS_TABSTOP) &&
                     ((DWORD)GetWindowLongPtrA(ok, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON,
                 "DLGITEM style", "button style/WS_TABSTOP is preserved");
    smoke_expect(&s, oldDlg && GetNextDlgTabItem(oldDlg, 0, FALSE) == edit &&
                     GetNextDlgTabItem(oldDlg, edit, FALSE) == ok &&
                     GetNextDlgTabItem(oldDlg, ok, FALSE) == cancel,
                 "template tab order", "tab order follows template item order while skipping non-tabstop static");

    LPCDLGTEMPLATEA bigTpl = smoke_v195_build_old_template(bigBlob, sizeof(bigBlob), 12, 4, "v195 big");
    HWND bigDlg = ownerWnd ? CreateDialogIndirectParamA(0, bigTpl, ownerWnd, smoke_v195_dlgproc, 0x1952) : 0;
    RECT oldRc = {0}, bigRc = {0};
    if (oldDlg) GetWindowRect(oldDlg, &oldRc);
    if (bigDlg) GetWindowRect(bigDlg, &bigRc);
    smoke_expect(&s, oldDlg && bigDlg && (bigRc.right - bigRc.left) > (oldRc.right - oldRc.left) &&
                     (bigRc.bottom - bigRc.top) > (oldRc.bottom - oldRc.top),
                 "DS_SETFONT DLU mapping", "larger dialog font changes dialog-unit to pixel mapping");

    LPCDLGTEMPLATEA exTpl = smoke_v195_build_ex_template(exBlob, sizeof(exBlob));
    HWND exDlg = ownerWnd ? CreateDialogIndirectParamA(0, exTpl, ownerWnd, smoke_v195_dlgproc, 0x1953) : 0;
    HWND exBtn = exDlg ? GetDlgItem(exDlg, 19520) : 0;
    HWND exStatic = exDlg ? GetDlgItem(exDlg, 19521) : 0;
    smoke_expect(&s, exDlg && exBtn && exStatic,
                 "DLGTEMPLATEEX creates controls", "extended template parses header and DLGITEMTEMPLATEEX records");
    memset(txt, 0, sizeof(txt));
    if (exBtn) GetWindowTextA(exBtn, txt, sizeof(txt));
    smoke_expect(&s, strcmp(txt, "ExBtn") == 0 && GetWindowLongPtrA(exBtn, GWLP_ID) == 19520,
                 "DLGTEMPLATEEX title/id", "DWORD control id and title are preserved");
    smoke_expect(&s, exDlg && ((DWORD)GetWindowLongPtrA(exDlg, GWL_EXSTYLE) & 0x00000200u) &&
                     exBtn && ((DWORD)GetWindowLongPtrA(exBtn, GWL_EXSTYLE) & 0x00000400u),
                 "DLGTEMPLATEEX exStyle", "dialog and item extended styles survive parsing");
    smoke_expect(&s, exDlg && GetWindow(exDlg, GW_OWNER) == ownerWnd,
                 "DLGTEMPLATEEX owner", "extended dialog also receives owner relationship");

    if (exDlg && IsWindow(exDlg)) EndDialog(exDlg, 0x1953);
    if (bigDlg && IsWindow(bigDlg)) EndDialog(bigDlg, 0x1952);
    if (oldDlg && IsWindow(oldDlg)) EndDialog(oldDlg, 0x1951);
    if (ownerWnd && IsWindow(ownerWnd)) DestroyWindow(ownerWnd);
    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_user32_dialog_dwlp(SmokeRuntime* rt)
{
    smoke_runtime_init(rt);
    SmokeContext s = {0,0,0,0,"user32_dialog_dwlp"};
    Capability owner = rt->cap;
    Capability proc = cap_create(1531, "v153-dialog-dwlp", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&proc, 0);

    g_v153_init_count = g_v153_proc1_user = g_v153_proc2_user = g_v153_false_count = 0;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_wndproc;
    wc.lpszClassName = "myOS.v153.DialogParent";

    MyWinBindRuntime(&rt->mgr, &proc);
    MyWinBindDesktop(&rt->wm);
    RegisterClassExA(&wc);
    HWND parent = CreateWindowExA(0, "myOS.v153.DialogParent", "v153 parent", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  10, 10, 260, 160, 0, 0, 0, NULL);
    smoke_expect(&s, parent != 0, "dialog parent", "parent HWND for modal/modeless dialogs");

    HWND hDlg = parent ? CreateDialogIndirectParamA(0, (LPCDLGTEMPLATEA)&g_v153_dialog_template,
                                                    parent, smoke_v153_dlgproc1, 0x153) : 0;
    smoke_expect(&s, hDlg != 0 && g_v153_init_count == 1, "CreateDialogIndirectParamA", "modeless dialog created and WM_INITDIALOG sent");
    smoke_expect(&s, hDlg && GetClassLongPtrA(hDlg, GCL_CBWNDEXTRA) >= (LONG_PTR)DLGWINDOWEXTRA,
                 "#32770 DLGWINDOWEXTRA", "dialog class reserves DWLP storage bytes");
    smoke_expect(&s, hDlg && GetWindowLongPtrA(hDlg, DWLP_USER) == (LONG_PTR)0x15300001,
                 "DWLP_USER init roundtrip", "DialogProc can store state in DWLP_USER during WM_INITDIALOG");
    smoke_expect(&s, hDlg && SetWindowLongPtrA(hDlg, DWLP_USER, (LONG_PTR)0x1530CAFE) == (LONG_PTR)0x15300001,
                 "SetWindowLongPtrA(DWLP_USER)", "returns previous dialog user value");
    smoke_expect(&s, hDlg && GetWindowLongPtrA(hDlg, DWLP_USER) == (LONG_PTR)0x1530CAFE,
                 "GetWindowLongPtrA(DWLP_USER)", "DWLP_USER roundtrip");
    smoke_expect(&s, hDlg && SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, (LONG_PTR)0x15305555) == 0,
                 "SetWindowLongPtrA(DWLP_MSGRESULT)", "explicit message-result slot starts zero");
    smoke_expect(&s, hDlg && GetWindowLongPtrA(hDlg, DWLP_MSGRESULT) == (LONG_PTR)0x15305555,
                 "GetWindowLongPtrA(DWLP_MSGRESULT)", "DWLP_MSGRESULT roundtrip");
    smoke_expect(&s, hDlg && (DLGPROC)GetWindowLongPtrA(hDlg, DWLP_DLGPROC) == smoke_v153_dlgproc1,
                 "GetWindowLongPtrA(DWLP_DLGPROC)", "app dialog proc is stored separately from WndProc");
    smoke_expect(&s, hDlg && (WNDPROC)GetWindowLongPtrA(hDlg, GWLP_WNDPROC) == DefDlgProcA,
                 "GWLP_WNDPROC remains DefDlgProcA", "dialog window proc stays DefDlgProcA");
    smoke_expect(&s, hDlg && GetWindowLongPtrA(hDlg, DWLP_DLGPROC) != GetWindowLongPtrA(hDlg, GWLP_WNDPROC),
                 "DWLP_DLGPROC != GWLP_WNDPROC", "dialog callback is not the window proc");

    LRESULT r1 = hDlg ? SendMessageA(hDlg, WM_USER + 153, 0, 0) : 0;
    smoke_expect(&s, r1 == (LRESULT)0x15310001 && g_v153_proc1_user == 1,
                 "DialogProc TRUE uses DWLP_MSGRESULT", "DefDlgProc returns the DWLP_MSGRESULT value");
    LRESULT rFalse = hDlg ? SendMessageA(hDlg, WM_USER + 154, 0, 0) : -1;
    smoke_expect(&s, rFalse == 0 && g_v153_false_count == 1,
                 "DialogProc FALSE falls through", "unhandled dialog messages fall through to default handling");

    DLGPROC oldDlg = hDlg ? (DLGPROC)SetWindowLongPtrA(hDlg, DWLP_DLGPROC, (LONG_PTR)smoke_v153_dlgproc2) : NULL;
    smoke_expect(&s, oldDlg == smoke_v153_dlgproc1,
                 "SetWindowLongPtrA(DWLP_DLGPROC)", "replaces app DialogProc and returns the old callback");
    smoke_expect(&s, hDlg && (WNDPROC)GetWindowLongPtrA(hDlg, GWLP_WNDPROC) == DefDlgProcA,
                 "DWLP_DLGPROC does not subclass HWND", "GWLP_WNDPROC remains DefDlgProcA after dialog-proc replacement");
    LRESULT r2 = hDlg ? SendMessageA(hDlg, WM_USER + 153, 0, 0) : 0;
    smoke_expect(&s, r2 == (LRESULT)0x15320002 && g_v153_proc2_user == 1 && g_v153_proc1_user == 1,
                 "replaced DWLP_DLGPROC dispatch", "subsequent dialog messages use the replacement DialogProc");

    SetLastError(0x153153u);
    LONG_PTR bad = hDlg ? GetWindowLongPtrA(hDlg, DLGWINDOWEXTRA) : 0;
    smoke_expect(&s, bad == 0, "invalid DWLP offset", "offset exactly past DLGWINDOWEXTRA is rejected");
    smoke_expect_last_error(&s, ERROR_INVALID_INDEX, "invalid DWLP offset LastError");

    HWND hDlg2 = parent ? CreateDialogIndirectParamA(0, (LPCDLGTEMPLATEA)&g_v153_dialog_template,
                                                     parent, smoke_v153_dlgproc1, 0x154) : 0;
    smoke_expect(&s, hDlg2 != 0, "second modeless dialog", "second dialog proves per-HWND DWLP storage");
    if (hDlg2) SetWindowLongPtrA(hDlg2, DWLP_USER, (LONG_PTR)0x15322222);
    smoke_expect(&s, hDlg && hDlg2 && GetWindowLongPtrA(hDlg, DWLP_USER) == (LONG_PTR)0x1530CAFE &&
                     GetWindowLongPtrA(hDlg2, DWLP_USER) == (LONG_PTR)0x15322222,
                 "DWLP_USER isolated per HWND", "dialog extra bytes are per dialog window");
    smoke_expect(&s, hDlg2 && EndDialog(hDlg2, 0x154) && !IsWindow(hDlg2),
                 "modeless EndDialog", "modeless dialog is destroyed without a modal waiter");

    BOOL parentEnabledBefore = parent ? IsWindowEnabled(parent) : FALSE;
    INT_PTR modalResult = parent ? DialogBoxIndirectParamA(0, (LPCDLGTEMPLATEA)&g_v153_dialog_template,
                                                           parent, smoke_v153_modal_end_proc, 0) : -1;
    smoke_expect(&s, modalResult == (INT_PTR)0x1530BEEF,
                 "DialogBoxIndirectParamA modal result", "EndDialog result survives until modal loop returns");
    smoke_expect(&s, parent && parentEnabledBefore && IsWindowEnabled(parent),
                 "modal owner enabled symmetry", "owner is enabled again after modal dialog returns");

    if (hDlg && IsWindow(hDlg)) EndDialog(hDlg, 0x153);
    if (parent && IsWindow(parent)) DestroyWindow(parent);
    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static int smoke_user32_class_process(SmokeRuntime* rt)
{
    smoke_runtime_init(rt);
    SmokeContext s = {0,0,0,0,"user32_class"};
    Capability owner = rt->cap;
    Capability procA = cap_create(1501, "v150-class-proc-a", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    Capability procB = cap_create(1502, "v150-class-proc-b", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    Capability procC = cap_create(1503, "v150-class-proc-c", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&procA, 0); cap_add_target(&procB, 0); cap_add_target(&procC, 0);

    const char* sharedName = "myOS.v150.SharedClass";
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_v150_proc_a;
    wc.lpszClassName = sharedName;

    MyWinBindRuntime(&rt->mgr, &procA);
    MyWinBindDesktop(&rt->wm);
    ATOM atomA = RegisterClassExA(&wc);
    smoke_expect(&s, atomA != 0, "proc A RegisterClassExA", "shared name owned by pid 1501");
    HWND hA1 = CreateWindowExA(0, sharedName, "A1", WS_OVERLAPPEDWINDOW, 10, 10, 120, 80, 0, 0, 0, NULL);
    smoke_expect(&s, hA1 != 0 && g_v150_proc_a_creates == 1 && g_v150_proc_b_creates == 0,
                 "proc A CreateWindowExA", "caller resolves its own WndProc");
    if (hA1) DestroyWindow(hA1);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_v150_proc_b;
    wc.lpszClassName = sharedName;
    MyWinBindRuntime(&rt->mgr, &procB);
    MyWinBindDesktop(&rt->wm);
    ATOM atomB = RegisterClassExA(&wc);
    smoke_expect(&s, atomB != 0 && atomB != atomA, "proc B RegisterClassExA", "same class name gets a distinct process-owned class");
    HWND hB1 = CreateWindowExA(0, sharedName, "B1", WS_OVERLAPPEDWINDOW, 20, 20, 120, 80, 0, 0, 0, NULL);
    smoke_expect(&s, hB1 != 0 && g_v150_proc_b_creates == 1 && g_v150_proc_a_creates == 1,
                 "proc B CreateWindowExA", "same name resolves to proc B WndProc");
    if (hB1) DestroyWindow(hB1);

    MyWinBindRuntime(&rt->mgr, &procA);
    MyWinBindDesktop(&rt->wm);
    HWND hA2 = CreateWindowExA(0, sharedName, "A2", WS_OVERLAPPEDWINDOW, 30, 30, 120, 80, 0, 0, 0, NULL);
    smoke_expect(&s, hA2 != 0 && g_v150_proc_a_creates == 2 && g_v150_proc_b_creates == 1,
                 "proc A re-resolve", "switching back does not see proc B class");
    if (hA2) DestroyWindow(hA2);

    HWND hAtom = CreateWindowExA(0, (LPCSTR)(uintptr_t)atomA, "Atom A", WS_OVERLAPPEDWINDOW, 40, 40, 120, 80, 0, 0, 0, NULL);
    smoke_expect(&s, hAtom != 0 && g_v150_proc_a_creates == 3,
                 "atom class lookup", "RegisterClassExA atom still creates the registered class");
    if (hAtom) DestroyWindow(hAtom);

    MyWinBindRuntime(&rt->mgr, &procB);
    MyWinBindDesktop(&rt->wm);
    ATOM unregB = UnregisterClassA(sharedName, 0);
    smoke_expect(&s, unregB == atomB, "proc B UnregisterClassA", "only caller-owned class is removed");
    HWND hBFail = CreateWindowExA(0, sharedName, "B removed", WS_OVERLAPPEDWINDOW, 50, 50, 120, 80, 0, 0, 0, NULL);
    smoke_expect(&s, hBFail == 0, "proc B class removed", "CreateWindowExA no longer finds B-owned class");

    MyWinBindRuntime(&rt->mgr, &procA);
    MyWinBindDesktop(&rt->wm);
    HWND hA3 = CreateWindowExA(0, sharedName, "A survives", WS_OVERLAPPEDWINDOW, 60, 60, 120, 80, 0, 0, 0, NULL);
    smoke_expect(&s, hA3 != 0 && g_v150_proc_a_creates == 4,
                 "proc A class survives", "foreign unregister did not remove A class");
    if (hA3) DestroyWindow(hA3);

    ATOM badSys = UnregisterClassA("BUTTON", 0);
    smoke_expect(&s, badSys == 0, "system class protected", "apps cannot unregister BUTTON");
    smoke_expect_last_error(&s, ERROR_ACCESS_DENIED, "UnregisterClassA(BUTTON) LastError");
    HWND hBtn = CreateWindowExA(0, "BUTTON", "Button", WS_CHILD|WS_VISIBLE, 1, 1, 80, 24, 0, (HMENU)1, 0, NULL);
    smoke_expect(&s, hBtn != 0, "system BUTTON survives", "global control class remains registered");
    if (hBtn) DestroyWindow(hBtn);

    MyWinBindRuntime(&rt->mgr, &procC);
    MyWinBindDesktop(&rt->wm);
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = smoke_v150_proc_c;
    wc.lpszClassName = "myOS.v150.CleanupClass";
    ATOM atomC = RegisterClassExA(&wc);
    smoke_expect(&s, atomC != 0, "proc C RegisterClassExA", "cleanup fixture class registered");
    MyUser32CleanupProcessClasses(1503);
    HWND hC = CreateWindowExA(0, "myOS.v150.CleanupClass", "C", WS_OVERLAPPEDWINDOW, 70, 70, 120, 80, 0, 0, 0, NULL);
    smoke_expect(&s, hC == 0 && g_v150_proc_c_creates == 0,
                 "process class cleanup", "exited process classes are removed");

    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);
    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}


static void smoke_v199_process_sid(SID* sid, DWORD pid)
{
    SID_IDENTIFIER_AUTHORITY ntAuth = {{0,0,0,0,0,5}};
    memset(sid, 0, sizeof(*sid));
    InitializeSid(sid, &ntAuth, 2);
    *GetSidSubAuthority(sid, 0) = 21u;
    *GetSidSubAuthority(sid, 1) = pid;
}

static void smoke_v199_everyone_sid(SID* sid)
{
    SID_IDENTIFIER_AUTHORITY worldAuth = {{0,0,0,0,0,1}};
    memset(sid, 0, sizeof(*sid));
    InitializeSid(sid, &worldAuth, 1);
    *GetSidSubAuthority(sid, 0) = SECURITY_WORLD_RID;
}

static int smoke_security_descriptor(SmokeRuntime* rt)
{
    SmokeContext s = {0};
    s.group = "security";
    smoke_runtime_init(rt);

    Capability owner = cap_create(1991, "v199-sd-owner", CAP_IPC|CAP_SECTION_MAP|CAP_EXEC);
    Capability other = cap_create(1992, "v199-sd-other", CAP_IPC|CAP_SECTION_MAP);
    Capability admin = rt->cap;

    SID ownerSid, everyoneSid;
    smoke_v199_process_sid(&ownerSid, owner.id);
    smoke_v199_everyone_sid(&everyoneSid);
    smoke_expect(&s, IsValidSid(&ownerSid) && IsValidSid(&everyoneSid), "SID primitives", "S-1-5-21-pid and S-1-1-0 are valid");

    BYTE ownerAclBuf[256];
    PACL ownerAcl = (PACL)ownerAclBuf;
    SECURITY_DESCRIPTOR ownerSd;
    memset(&ownerSd, 0, sizeof(ownerSd));
    BOOL ownerSdOk = InitializeAcl(ownerAcl, sizeof(ownerAclBuf), ACL_REVISION) &&
                     AddAccessAllowedAce(ownerAcl, ACL_REVISION, EVENT_ALL_ACCESS, &ownerSid) &&
                     InitializeSecurityDescriptor(&ownerSd, SECURITY_DESCRIPTOR_REVISION) &&
                     SetSecurityDescriptorDacl(&ownerSd, TRUE, ownerAcl, FALSE);
    ownerSd.Owner = &ownerSid;
    SECURITY_ATTRIBUTES ownerSa;
    memset(&ownerSa, 0, sizeof(ownerSa));
    ownerSa.nLength = sizeof(ownerSa);
    ownerSa.lpSecurityDescriptor = &ownerSd;
    smoke_expect(&s, ownerSdOk && ownerAcl->AceCount == 1, "build owner DACL", "absolute SECURITY_DESCRIPTOR + ACCESS_ALLOWED_ACE");

    char ownerName[96];
    smoke_unique_name(ownerName, sizeof(ownerName), "v199.owner_dacl.event");
    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);
    HANDLE hOwnerCreate = CreateEventA(&ownerSa, TRUE, FALSE, ownerName);
    _ObjectectInfo oi;
    memset(&oi, 0, sizeof(oi));
    DWORD objHandle = 0;
    if (hOwnerCreate) {
        MyHandleInfo hi;
        memset(&hi, 0, sizeof(hi));
        if (MyGetHandleInfo(hOwnerCreate, &hi)) objHandle = hi.object_handle;
    }
    BOOL gotObj = objHandle ? MyGetObjectInfo(objHandle, &oi) : FALSE;
    smoke_expect(&s, hOwnerCreate && gotObj && oi.dacl_present == 1 && oi.ace_count == 1,
                 "CreateEventA stores DACL", "Object Manager has explicit SD metadata");

    MyWinBindRuntime(&rt->mgr, &other);
    MyWinBindDesktop(&rt->wm);
    HANDLE hOtherDenied = OpenEventA(SYNCHRONIZE, FALSE, ownerName);
    DWORD otherDeniedErr = GetLastError();
    smoke_expect(&s, hOtherDenied == 0 && otherDeniedErr == ERROR_ACCESS_DENIED,
                 "DACL denies foreign OpenEvent", "no matching ALLOW ACE for other process SID");
    if (hOtherDenied) CloseHandle(hOtherDenied);

    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);
    HANDLE hOwnerSync = OpenEventA(SYNCHRONIZE, FALSE, ownerName);
    BOOL ownerSyncSetDenied = hOwnerSync ? SetEvent(hOwnerSync) : TRUE;
    DWORD ownerSyncErr = GetLastError();
    smoke_expect(&s, hOwnerSync != 0 && !ownerSyncSetDenied && ownerSyncErr == ERROR_ACCESS_DENIED,
                 "granted handle access is enforced", "SYNCHRONIZE handle cannot SetEvent");
    if (hOwnerSync) CloseHandle(hOwnerSync);
    HANDLE hOwnerWrite = OpenEventA(EVENT_MODIFY_STATE, FALSE, ownerName);
    BOOL ownerCanSet = hOwnerWrite ? SetEvent(hOwnerWrite) : FALSE;
    smoke_expect(&s, hOwnerWrite != 0 && ownerCanSet, "owner ACE grants modify", "EVENT_MODIFY_STATE ALLOW ACE works");
    if (hOwnerWrite) CloseHandle(hOwnerWrite);
    if (hOwnerCreate) CloseHandle(hOwnerCreate);

    BYTE emptyAclBuf[sizeof(ACL)];
    PACL emptyAcl = (PACL)emptyAclBuf;
    SECURITY_DESCRIPTOR emptySd;
    memset(&emptySd, 0, sizeof(emptySd));
    BOOL emptyOk = InitializeAcl(emptyAcl, sizeof(emptyAclBuf), ACL_REVISION) &&
                   InitializeSecurityDescriptor(&emptySd, SECURITY_DESCRIPTOR_REVISION) &&
                   SetSecurityDescriptorDacl(&emptySd, TRUE, emptyAcl, FALSE);
    emptySd.Owner = &ownerSid;
    SECURITY_ATTRIBUTES emptySa;
    memset(&emptySa, 0, sizeof(emptySa));
    emptySa.nLength = sizeof(emptySa);
    emptySa.lpSecurityDescriptor = &emptySd;
    char emptyName[96];
    smoke_unique_name(emptyName, sizeof(emptyName), "v199.empty_dacl.event");
    MyWinBindRuntime(&rt->mgr, &owner);
    HANDLE hEmptyCreate = emptyOk ? CreateEventA(&emptySa, TRUE, FALSE, emptyName) : 0;
    MyWinBindRuntime(&rt->mgr, &other);
    HANDLE hEmptyOther = OpenEventA(SYNCHRONIZE, FALSE, emptyName);
    DWORD emptyOtherErr = GetLastError();
    MyWinBindRuntime(&rt->mgr, &owner);
    HANDLE hEmptyOwnerDac = OpenEventA(WRITE_DAC, FALSE, emptyName);
    HANDLE hEmptyOwnerSync = OpenEventA(SYNCHRONIZE, FALSE, emptyName);
    smoke_expect(&s, hEmptyCreate && !hEmptyOther && emptyOtherErr == ERROR_ACCESS_DENIED && hEmptyOwnerDac && !hEmptyOwnerSync,
                 "empty DACL semantics", "empty DACL blocks access; owner keeps WRITE_DAC only");
    if (hEmptyOwnerSync) CloseHandle(hEmptyOwnerSync);
    if (hEmptyOwnerDac) CloseHandle(hEmptyOwnerDac);
    if (hEmptyOther) CloseHandle(hEmptyOther);
    if (hEmptyCreate) CloseHandle(hEmptyCreate);

    SECURITY_DESCRIPTOR nullSd;
    memset(&nullSd, 0, sizeof(nullSd));
    BOOL nullOk = InitializeSecurityDescriptor(&nullSd, SECURITY_DESCRIPTOR_REVISION) &&
                  SetSecurityDescriptorDacl(&nullSd, TRUE, NULL, FALSE);
    nullSd.Owner = &ownerSid;
    SECURITY_ATTRIBUTES nullSa;
    memset(&nullSa, 0, sizeof(nullSa));
    nullSa.nLength = sizeof(nullSa);
    nullSa.lpSecurityDescriptor = &nullSd;
    char nullName[96];
    smoke_unique_name(nullName, sizeof(nullName), "v199.null_dacl.event");
    MyWinBindRuntime(&rt->mgr, &owner);
    HANDLE hNullCreate = nullOk ? CreateEventA(&nullSa, TRUE, FALSE, nullName) : 0;
    MyWinBindRuntime(&rt->mgr, &other);
    HANDLE hNullOther = OpenEventA(EVENT_MODIFY_STATE, FALSE, nullName);
    BOOL nullOtherSet = hNullOther ? SetEvent(hNullOther) : FALSE;
    smoke_expect(&s, hNullCreate && hNullOther && nullOtherSet,
                 "NULL DACL grants all", "present NULL DACL is intentionally open");
    if (hNullOther) CloseHandle(hNullOther);
    if (hNullCreate) { MyWinBindRuntime(&rt->mgr, &owner); CloseHandle(hNullCreate); }

    BYTE denyAclBuf[256];
    PACL denyAcl = (PACL)denyAclBuf;
    SECURITY_DESCRIPTOR denySd;
    memset(&denySd, 0, sizeof(denySd));
    BOOL denyOk = InitializeAcl(denyAcl, sizeof(denyAclBuf), ACL_REVISION) &&
                  AddAccessDeniedAce(denyAcl, ACL_REVISION, EVENT_MODIFY_STATE, &everyoneSid) &&
                  AddAccessAllowedAce(denyAcl, ACL_REVISION, EVENT_ALL_ACCESS, &everyoneSid) &&
                  InitializeSecurityDescriptor(&denySd, SECURITY_DESCRIPTOR_REVISION) &&
                  SetSecurityDescriptorDacl(&denySd, TRUE, denyAcl, FALSE);
    denySd.Owner = &ownerSid;
    SECURITY_ATTRIBUTES denySa;
    memset(&denySa, 0, sizeof(denySa));
    denySa.nLength = sizeof(denySa);
    denySa.lpSecurityDescriptor = &denySd;
    char denyName[96];
    smoke_unique_name(denyName, sizeof(denyName), "v199.deny_first.event");
    MyWinBindRuntime(&rt->mgr, &owner);
    HANDLE hDenyCreate = denyOk ? CreateEventA(&denySa, TRUE, FALSE, denyName) : 0;
    MyWinBindRuntime(&rt->mgr, &other);
    HANDLE hDenySync = OpenEventA(SYNCHRONIZE, FALSE, denyName);
    HANDLE hDenyModify = OpenEventA(EVENT_MODIFY_STATE, FALSE, denyName);
    DWORD denyModifyErr = GetLastError();
    smoke_expect(&s, hDenyCreate && hDenySync && !hDenyModify && denyModifyErr == ERROR_ACCESS_DENIED,
                 "DENY ACE precedes ALLOW ACE", "Everyone allow cannot override explicit deny");
    if (hDenyModify) CloseHandle(hDenyModify);
    if (hDenySync) CloseHandle(hDenySync);
    if (hDenyCreate) { MyWinBindRuntime(&rt->mgr, &owner); CloseHandle(hDenyCreate); }

    /* v200: SECURITY_DESCRIPTOR API surface: owner/DACL getters, self-relative
       conversion roundtrip, generic access mapping and public AccessCheck. */
    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);
    BYTE v200AclBuf[256];
    PACL v200Acl = (PACL)v200AclBuf;
    SECURITY_DESCRIPTOR v200Abs;
    memset(&v200Abs, 0, sizeof(v200Abs));
    BOOL v200Build = InitializeAcl(v200Acl, sizeof(v200AclBuf), ACL_REVISION) &&
                     AddAccessAllowedAce(v200Acl, ACL_REVISION, GENERIC_WRITE, &ownerSid) &&
                     InitializeSecurityDescriptor(&v200Abs, SECURITY_DESCRIPTOR_REVISION) &&
                     SetSecurityDescriptorOwner(&v200Abs, &ownerSid, FALSE) &&
                     SetSecurityDescriptorDacl(&v200Abs, TRUE, v200Acl, FALSE);
    PSID gotOwner = NULL;
    BOOL ownerDefaulted = TRUE;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = TRUE;
    PACL gotDacl = NULL;
    BOOL v200Getters = GetSecurityDescriptorOwner(&v200Abs, &gotOwner, &ownerDefaulted) &&
                       GetSecurityDescriptorDacl(&v200Abs, &daclPresent, &gotDacl, &daclDefaulted);
    smoke_expect(&s, v200Build && v200Getters && gotOwner && EqualSid(gotOwner, &ownerSid) && !ownerDefaulted && daclPresent && gotDacl == v200Acl && !daclDefaulted,
                 "v200 SD owner/DACL getters", "Set/GetSecurityDescriptorOwner and GetSecurityDescriptorDacl follow absolute SD semantics");

    BYTE relBuf[512];
    DWORD relNeed = 0;
    BOOL relSmall = !MakeSelfRelativeSD(&v200Abs, NULL, &relNeed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER;
    DWORD relLen = sizeof(relBuf);
    BOOL relOk = MakeSelfRelativeSD(&v200Abs, (PSECURITY_DESCRIPTOR)relBuf, &relLen);
    PSECURITY_DESCRIPTOR relSd = (PSECURITY_DESCRIPTOR)relBuf;
    PACL relDacl = NULL;
    BOOL relPresent = FALSE;
    BOOL relDefaulted = TRUE;
    BOOL relGet = relOk && GetSecurityDescriptorDacl(relSd, &relPresent, &relDacl, &relDefaulted);
    smoke_expect(&s, relSmall && relOk && (relSd->Control & SE_SELF_RELATIVE) && relNeed == relLen && relGet && relPresent && relDacl && relDacl->AceCount == 1,
                 "v200 self-relative SD", "MakeSelfRelativeSD creates offset-based SD accepted by DACL getter");

    SECURITY_DESCRIPTOR absRound;
    SID absOwner;
    BYTE absDaclBuf[256];
    DWORD absSize = sizeof(absRound), absDaclSize = sizeof(absDaclBuf), absSaclSize = 0, absOwnerSize = sizeof(absOwner), absGroupSize = 0;
    BOOL absOk = MakeAbsoluteSD(relSd, &absRound, &absSize, (PACL)absDaclBuf, &absDaclSize, NULL, &absSaclSize, &absOwner, &absOwnerSize, NULL, &absGroupSize);
    smoke_expect(&s, absOk && absRound.Owner && EqualSid(absRound.Owner, &ownerSid) && absRound.Dacl && absRound.Dacl->AceCount == 1,
                 "v200 absolute SD roundtrip", "MakeAbsoluteSD restores owner SID and DACL ACEs");

    GENERIC_MAPPING eventMap;
    eventMap.GenericRead = SYNCHRONIZE | READ_CONTROL;
    eventMap.GenericWrite = EVENT_MODIFY_STATE | READ_CONTROL;
    eventMap.GenericExecute = SYNCHRONIZE | READ_CONTROL;
    eventMap.GenericAll = EVENT_ALL_ACCESS;
    DWORD genericMask = GENERIC_WRITE;
    MapGenericMask(&genericMask, &eventMap);
    smoke_expect(&s, (genericMask & GENERIC_WRITE) == 0 && (genericMask & EVENT_MODIFY_STATE) && (genericMask & READ_CONTROL),
                 "v200 MapGenericMask", "GENERIC_WRITE maps through object-specific GENERIC_MAPPING");

    HANDLE hOwnerToken = 0;
    BOOL tokOpen = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hOwnerToken);
    PRIVILEGE_SET ps;
    DWORD psLen = sizeof(ps);
    DWORD granted = 0;
    BOOL accessStatus = FALSE;
    BOOL acOk = tokOpen && AccessCheck(&v200Abs, hOwnerToken, GENERIC_WRITE, &eventMap, &ps, &psLen, &granted, &accessStatus);
    smoke_expect(&s, acOk && accessStatus && (granted & EVENT_MODIFY_STATE),
                 "v200 AccessCheck allow", "public AccessCheck maps generic desired access and grants owner ACE");

    BYTE v200DenyAclBuf[256];
    PACL v200DenyAcl = (PACL)v200DenyAclBuf;
    SECURITY_DESCRIPTOR v200DenyAbs;
    memset(&v200DenyAbs, 0, sizeof(v200DenyAbs));
    BOOL v200DenyBuild = InitializeAcl(v200DenyAcl, sizeof(v200DenyAclBuf), ACL_REVISION) &&
                         AddAccessDeniedAce(v200DenyAcl, ACL_REVISION, EVENT_MODIFY_STATE, &everyoneSid) &&
                         AddAccessAllowedAce(v200DenyAcl, ACL_REVISION, EVENT_ALL_ACCESS, &everyoneSid) &&
                         InitializeSecurityDescriptor(&v200DenyAbs, SECURITY_DESCRIPTOR_REVISION) &&
                         SetSecurityDescriptorOwner(&v200DenyAbs, &ownerSid, FALSE) &&
                         SetSecurityDescriptorDacl(&v200DenyAbs, TRUE, v200DenyAcl, FALSE);
    granted = 0; accessStatus = TRUE; psLen = sizeof(ps);
    BOOL acDenyOk = v200DenyBuild && hOwnerToken && AccessCheck(&v200DenyAbs, hOwnerToken, EVENT_MODIFY_STATE, &eventMap, &ps, &psLen, &granted, &accessStatus);
    smoke_expect(&s, acDenyOk && !accessStatus && granted == 0,
                 "v200 AccessCheck deny", "DENY ACE wins over later ALLOW in public AccessCheck path");

    /* v201: real token handles and token information snapshots. */
    DWORD tokenNeed = 0;
    BOOL tokenSmall = hOwnerToken && !GetTokenInformation(hOwnerToken, TokenUser, NULL, 0, &tokenNeed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER;
    BYTE tokenUserBuf[256];
    BOOL tokenUserOk = hOwnerToken && GetTokenInformation(hOwnerToken, TokenUser, tokenUserBuf, sizeof(tokenUserBuf), &tokenNeed);
    PTOKEN_USER tu = (PTOKEN_USER)tokenUserBuf;
    smoke_expect(&s, tokenSmall && tokenUserOk && tu->User.Sid && EqualSid(tu->User.Sid, &ownerSid),
                 "v201 TokenUser", "OpenProcessToken + GetTokenInformation(TokenUser) returns process SID");

    BYTE tokenGroupBuf[512];
    DWORD groupNeed = sizeof(tokenGroupBuf);
    BOOL groupsOk = hOwnerToken && GetTokenInformation(hOwnerToken, TokenGroups, tokenGroupBuf, sizeof(tokenGroupBuf), &groupNeed);
    PTOKEN_GROUPS tg = (PTOKEN_GROUPS)tokenGroupBuf;
    BOOL hasEveryone = FALSE;
    if (groupsOk) {
        for (DWORD gi = 0; gi < tg->GroupCount; ++gi) {
            if (tg->Groups[gi].Sid && EqualSid(tg->Groups[gi].Sid, &everyoneSid)) hasEveryone = TRUE;
        }
    }
    smoke_expect(&s, groupsOk && tg->GroupCount >= 1 && hasEveryone,
                 "v201 TokenGroups", "process token carries Everyone group for DACL evaluation");

    /* v202: token membership/owner/default-DACL/privilege-bearing AccessCheck. */
    BOOL isEveryoneMember = FALSE;
    SID otherSid;
    smoke_v199_process_sid(&otherSid, other.id);
    BOOL isOtherMember = TRUE;
    BOOL membershipOk = hOwnerToken &&
                        CheckTokenMembership(hOwnerToken, &everyoneSid, &isEveryoneMember) &&
                        CheckTokenMembership(hOwnerToken, &otherSid, &isOtherMember);
    smoke_expect(&s, membershipOk && isEveryoneMember && !isOtherMember,
                 "v202 CheckTokenMembership", "token SID/group membership is queried through the public API");

    BYTE tokenOwnerBuf[256];
    DWORD ownerInfoNeed = sizeof(tokenOwnerBuf);
    BOOL tokenOwnerOk = hOwnerToken && GetTokenInformation(hOwnerToken, TokenOwner, tokenOwnerBuf, sizeof(tokenOwnerBuf), &ownerInfoNeed);
    PTOKEN_OWNER tokenOwner = (PTOKEN_OWNER)tokenOwnerBuf;
    BYTE tokenDaclBuf[512];
    DWORD defaultDaclNeed = sizeof(tokenDaclBuf);
    BOOL tokenDaclOk = hOwnerToken && GetTokenInformation(hOwnerToken, TokenDefaultDacl, tokenDaclBuf, sizeof(tokenDaclBuf), &defaultDaclNeed);
    PTOKEN_DEFAULT_DACL tokenDacl = (PTOKEN_DEFAULT_DACL)tokenDaclBuf;
    smoke_expect(&s, tokenOwnerOk && tokenOwner->Owner && EqualSid(tokenOwner->Owner, &ownerSid) &&
                     tokenDaclOk && tokenDacl->DefaultDacl && tokenDacl->DefaultDacl->AceCount == 1,
                 "v202 TokenOwner/DefaultDacl", "token exposes owner SID and default DACL snapshot");

    MyWinBindRuntime(&rt->mgr, &admin);
    MyWinBindDesktop(&rt->wm);
    HANDLE hAdminToken = 0;
    BOOL adminTokOpen = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hAdminToken);
    BYTE adminPrivBuf[256];
    DWORD adminPrivNeed = sizeof(adminPrivBuf);
    BOOL adminPrivOk = adminTokOpen && GetTokenInformation(hAdminToken, TokenPrivileges, adminPrivBuf, sizeof(adminPrivBuf), &adminPrivNeed);
    PTOKEN_PRIVILEGES adminPrivs = (PTOKEN_PRIVILEGES)adminPrivBuf;
    PRIVILEGE_SET reqPriv;
    memset(&reqPriv, 0, sizeof(reqPriv));
    reqPriv.PrivilegeCount = 1;
    reqPriv.Control = PRIVILEGE_SET_ALL_NECESSARY;
    reqPriv.Privilege[0].Luid.LowPart = 8; /* myOS stable SeSecurityPrivilege LUID */
    reqPriv.Privilege[0].Luid.HighPart = 0;
    BOOL privResult = FALSE;
    BOOL privCheckOk = adminTokOpen && PrivilegeCheck(hAdminToken, &reqPriv, &privResult);
    smoke_expect(&s, adminPrivOk && adminPrivs->PrivilegeCount >= 1 && privCheckOk && privResult &&
                     (reqPriv.Privilege[0].Attributes & SE_PRIVILEGE_USED_FOR_ACCESS),
                 "v202 token privileges", "admin token carries enabled privileges and PrivilegeCheck marks used access");

    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);
    granted = 0; accessStatus = TRUE; psLen = sizeof(ps);
    BOOL ownerSystemSecurity = hOwnerToken && AccessCheck(&v200Abs, hOwnerToken, ACCESS_SYSTEM_SECURITY, &eventMap, &ps, &psLen, &granted, &accessStatus);
    MyWinBindRuntime(&rt->mgr, &admin);
    MyWinBindDesktop(&rt->wm);
    DWORD adminGranted = 0; BOOL adminAccessStatus = FALSE; DWORD adminPsLen = sizeof(ps);
    BOOL adminSystemSecurity = adminTokOpen && AccessCheck(&v200Abs, hAdminToken, ACCESS_SYSTEM_SECURITY, &eventMap, &ps, &adminPsLen, &adminGranted, &adminAccessStatus);
    smoke_expect(&s, ownerSystemSecurity && !accessStatus && adminSystemSecurity && adminAccessStatus && (adminGranted & ACCESS_SYSTEM_SECURITY),
                 "v202 ACCESS_SYSTEM_SECURITY", "AccessCheck requires SeSecurityPrivilege for SACL/security access");

    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);
    granted = 0; accessStatus = FALSE; psLen = sizeof(ps);
    BOOL maxOk = hOwnerToken && AccessCheck(&v200Abs, hOwnerToken, MAXIMUM_ALLOWED, &eventMap, &ps, &psLen, &granted, &accessStatus);
    smoke_expect(&s, maxOk && accessStatus && (granted & EVENT_MODIFY_STATE) && (granted & WRITE_DAC),
                 "v202 MAXIMUM_ALLOWED", "AccessCheck computes maximum DACL+owner-implicit access instead of treating it as a raw bit");

    /* v203: named objects without explicit SECURITY_ATTRIBUTES now receive a
       real namespace-inherited DACL rather than legacy sd_flags-only metadata. */
    char nsDefaultName[96];
    smoke_unique_name(nsDefaultName, sizeof(nsDefaultName), "v203.default_namespace.event");
    HANDLE hNsDefault = CreateEventA(NULL, TRUE, FALSE, nsDefaultName);
    MyHandleInfo nsHi; memset(&nsHi, 0, sizeof(nsHi));
    _ObjectectInfo nsOi; memset(&nsOi, 0, sizeof(nsOi));
    BOOL nsInfoOk = hNsDefault && MyGetHandleInfo(hNsDefault, &nsHi) && MyGetObjectInfo(nsHi.object_handle, &nsOi);
    BYTE nsSecBuf[512]; DWORD nsSecNeed = sizeof(nsSecBuf);
    BOOL nsSecOk = hNsDefault && GetKernelObjectSecurity(hNsDefault, OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION, (PSECURITY_DESCRIPTOR)nsSecBuf, sizeof(nsSecBuf), &nsSecNeed);
    BOOL nsDaclPresent = FALSE; BOOL nsDaclDefaulted = TRUE; PACL nsDacl = NULL;
    BOOL nsDaclOk = nsSecOk && GetSecurityDescriptorDacl((PSECURITY_DESCRIPTOR)nsSecBuf, &nsDaclPresent, &nsDacl, &nsDaclDefaulted);
    BOOL inheritedAce = FALSE;
    if (nsDaclOk && nsDacl && nsDacl->AceCount > 0) {
        BYTE* pAce = ((BYTE*)nsDacl) + sizeof(ACL);
        for (WORD ai = 0; ai < nsDacl->AceCount; ++ai) {
            ACE_HEADER* ah = (ACE_HEADER*)pAce;
            if (ah->AceFlags & INHERITED_ACE) inheritedAce = TRUE;
            pAce += ah->AceSize;
        }
    }
    smoke_expect(&s, nsInfoOk && nsOi.dacl_present == 1 && (nsOi.sd_control & SE_DACL_AUTO_INHERITED) &&
                     nsOi.namespace_id == _OBJECT_NS_GLOBAL && nsDaclOk && nsDaclPresent && nsDacl && nsDacl->AceCount >= 2 && inheritedAce,
                 "v203 namespace default DACL", "Global\\ named object gets explicit auto-inherited owner/everyone DACL metadata");

    MyWinBindRuntime(&rt->mgr, &other);
    MyWinBindDesktop(&rt->wm);
    HANDLE hNsOther = OpenEventA(EVENT_MODIFY_STATE, FALSE, nsDefaultName);
    BOOL nsOtherSet = hNsOther ? SetEvent(hNsOther) : FALSE;
    smoke_expect(&s, hNsOther && nsOtherSet,
                 "v203 namespace allows public named open", "default namespace DACL, not smoke-only flags, grants Everyone write semantics");
    if (hNsOther) CloseHandle(hNsOther);

    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);
    char nsPrivateName[96];
    smoke_unique_name(nsPrivateName, sizeof(nsPrivateName), "v203.private.namespace.private.event");
    HANDLE hNsPrivate = CreateEventA(NULL, TRUE, FALSE, nsPrivateName);
    MyHandleInfo privHi; memset(&privHi, 0, sizeof(privHi));
    _ObjectectInfo privOi; memset(&privOi, 0, sizeof(privOi));
    BOOL privInfoOk = hNsPrivate && MyGetHandleInfo(hNsPrivate, &privHi) && MyGetObjectInfo(privHi.object_handle, &privOi);
    MyWinBindRuntime(&rt->mgr, &other);
    HANDLE hPrivOther = OpenEventA(SYNCHRONIZE, FALSE, nsPrivateName);
    DWORD hPrivOtherErr = GetLastError();
    MyWinBindRuntime(&rt->mgr, &owner);
    HANDLE hPrivOwnerDac = OpenEventA(WRITE_DAC, FALSE, nsPrivateName);
    smoke_expect(&s, privInfoOk && privOi.dacl_present == 1 && privOi.ace_count == 1 && !hPrivOther &&
                     hPrivOtherErr == ERROR_ACCESS_DENIED && hPrivOwnerDac,
                 "v203 private namespace default", "private named object inherits owner-only DACL and denies foreign open");
    if (hPrivOwnerDac) CloseHandle(hPrivOwnerDac);
    if (hPrivOther) CloseHandle(hPrivOther);
    if (hNsPrivate) CloseHandle(hNsPrivate);
    if (hNsDefault) CloseHandle(hNsDefault);

    char ksecName[96];
    smoke_unique_name(ksecName, sizeof(ksecName), "v200.kernel_security.event");
    HANDLE hKsec = CreateEventA(&ownerSa, TRUE, FALSE, ksecName);
    BYTE ksecBuf[512];
    DWORD ksecNeed = 0;
    BOOL ksecSmall = hKsec && !GetKernelObjectSecurity(hKsec, OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION, NULL, 0, &ksecNeed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER;
    BOOL ksecGet = hKsec && GetKernelObjectSecurity(hKsec, OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION, (PSECURITY_DESCRIPTOR)ksecBuf, sizeof(ksecBuf), &ksecNeed);
    BOOL ksecSelf = ksecGet && (((PSECURITY_DESCRIPTOR)ksecBuf)->Control & SE_SELF_RELATIVE);
    smoke_expect(&s, ksecSmall && ksecGet && ksecSelf,
                 "v200 GetKernelObjectSecurity", "kernel object security is exported as self-relative SECURITY_DESCRIPTOR");

    BYTE publicAclBuf[256];
    PACL publicAcl = (PACL)publicAclBuf;
    SECURITY_DESCRIPTOR publicSd;
    memset(&publicSd, 0, sizeof(publicSd));
    BOOL publicSdOk = InitializeAcl(publicAcl, sizeof(publicAclBuf), ACL_REVISION) &&
                      AddAccessAllowedAce(publicAcl, ACL_REVISION, EVENT_MODIFY_STATE, &everyoneSid) &&
                      InitializeSecurityDescriptor(&publicSd, SECURITY_DESCRIPTOR_REVISION) &&
                      SetSecurityDescriptorOwner(&publicSd, &ownerSid, FALSE) &&
                      SetSecurityDescriptorDacl(&publicSd, TRUE, publicAcl, FALSE);
    BOOL setKsec = hKsec && publicSdOk && SetKernelObjectSecurity(hKsec, DACL_SECURITY_INFORMATION, &publicSd);
    MyWinBindRuntime(&rt->mgr, &other);
    MyWinBindDesktop(&rt->wm);
    HANDLE hKsecOther = OpenEventA(EVENT_MODIFY_STATE, FALSE, ksecName);
    BOOL ksecOtherSet = hKsecOther ? SetEvent(hKsecOther) : FALSE;
    smoke_expect(&s, setKsec && hKsecOther && ksecOtherSet,
                 "v200 SetKernelObjectSecurity", "changing DACL changes later OpenEventA access decision");
    if (hKsecOther) CloseHandle(hKsecOther);
    MyWinBindRuntime(&rt->mgr, &owner);
    if (hKsec) CloseHandle(hKsec);

    /* v204: CreateProcessA lpProcessAttributes/lpThreadAttributes now feed
       the real Object Manager DACLs for PROCESS/THREAD objects, and later
       OpenProcess/OpenThread must go through those DACLs instead of only
       checking whether the raw object exists. */
    BYTE procAclBuf[256];
    BYTE threadAclBuf[256];
    PACL procAcl = (PACL)procAclBuf;
    PACL threadAcl = (PACL)threadAclBuf;
    SECURITY_DESCRIPTOR procSd;
    SECURITY_DESCRIPTOR threadSd;
    memset(&procSd, 0, sizeof(procSd));
    memset(&threadSd, 0, sizeof(threadSd));
    BOOL procThreadSdOk = InitializeAcl(procAcl, sizeof(procAclBuf), ACL_REVISION) &&
                          AddAccessAllowedAce(procAcl, ACL_REVISION, PROCESS_ALL_ACCESS, &ownerSid) &&
                          InitializeSecurityDescriptor(&procSd, SECURITY_DESCRIPTOR_REVISION) &&
                          SetSecurityDescriptorOwner(&procSd, &ownerSid, FALSE) &&
                          SetSecurityDescriptorDacl(&procSd, TRUE, procAcl, FALSE) &&
                          InitializeAcl(threadAcl, sizeof(threadAclBuf), ACL_REVISION) &&
                          AddAccessAllowedAce(threadAcl, ACL_REVISION, THREAD_ALL_ACCESS, &ownerSid) &&
                          InitializeSecurityDescriptor(&threadSd, SECURITY_DESCRIPTOR_REVISION) &&
                          SetSecurityDescriptorOwner(&threadSd, &ownerSid, FALSE) &&
                          SetSecurityDescriptorDacl(&threadSd, TRUE, threadAcl, FALSE);
    SECURITY_ATTRIBUTES procSa; memset(&procSa, 0, sizeof(procSa));
    SECURITY_ATTRIBUTES threadSa; memset(&threadSa, 0, sizeof(threadSa));
    procSa.nLength = sizeof(procSa); procSa.lpSecurityDescriptor = &procSd; procSa.bInheritHandle = TRUE;
    threadSa.nLength = sizeof(threadSa); threadSa.lpSecurityDescriptor = &threadSd; threadSa.bInheritHandle = TRUE;
    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    BOOL cpOk = procThreadSdOk && CreateProcessA("v204-process-security.exe", NULL, &procSa, &threadSa, FALSE, 0, NULL, NULL, &si, &pi);
    MyHandleInfo procHi; memset(&procHi, 0, sizeof(procHi));
    MyHandleInfo threadHi; memset(&threadHi, 0, sizeof(threadHi));
    _ObjectectInfo procOi; memset(&procOi, 0, sizeof(procOi));
    _ObjectectInfo threadOi; memset(&threadOi, 0, sizeof(threadOi));
    BOOL ptObjOk = cpOk && MyGetHandleInfo(pi.hProcess, &procHi) && MyGetHandleInfo(pi.hThread, &threadHi) &&
                   MyGetObjectInfo(procHi.object_handle, &procOi) && MyGetObjectInfo(threadHi.object_handle, &threadOi);
    smoke_expect(&s, cpOk && ptObjOk && procOi.type == _OBJECT_TYPE_PROCESS && threadOi.type == _OBJECT_TYPE_THREAD &&
                     procOi.dacl_present == 1 && threadOi.dacl_present == 1 &&
                     (procHi.flags & MYWIN_HANDLE_FLAG_INHERIT) && (threadHi.flags & MYWIN_HANDLE_FLAG_INHERIT),
                 "v204 CreateProcess SECURITY_ATTRIBUTES", "process/thread object DACLs and handle inherit flags come from the two SECURITY_ATTRIBUTES arguments");

    MyWinBindRuntime(&rt->mgr, &other);
    MyWinBindDesktop(&rt->wm);
    HANDLE hOtherProc = cpOk ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.dwProcessId) : 0;
    DWORD hOtherProcErr = GetLastError();
    HANDLE hOtherThread = cpOk ? OpenThread(THREAD_QUERY_INFORMATION, FALSE, pi.dwThreadId) : 0;
    DWORD hOtherThreadErr = GetLastError();
    smoke_expect(&s, cpOk && !hOtherProc && hOtherProcErr == ERROR_ACCESS_DENIED && !hOtherThread && hOtherThreadErr == ERROR_ACCESS_DENIED,
                 "v204 process/thread DACL denies foreign open", "OpenProcess/OpenThread now evaluate object security before granting a handle");
    if (hOtherProc) CloseHandle(hOtherProc);
    if (hOtherThread) CloseHandle(hOtherThread);

    MyWinBindRuntime(&rt->mgr, &owner);
    MyWinBindDesktop(&rt->wm);
    HANDLE hOwnerProc = cpOk ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE, FALSE, pi.dwProcessId) : 0;
    HANDLE hOwnerThread = cpOk ? OpenThread(THREAD_QUERY_INFORMATION|SYNCHRONIZE, FALSE, pi.dwThreadId) : 0;
    BYTE procKsecBuf[512]; DWORD procKsecNeed = sizeof(procKsecBuf);
    BOOL procKsecOk = pi.hProcess && GetKernelObjectSecurity(pi.hProcess, OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION, (PSECURITY_DESCRIPTOR)procKsecBuf, sizeof(procKsecBuf), &procKsecNeed);
    smoke_expect(&s, hOwnerProc && hOwnerThread && procKsecOk && (((PSECURITY_DESCRIPTOR)procKsecBuf)->Control & SE_SELF_RELATIVE),
                 "v204 owner opens secured process/thread", "owner SID receives later OpenProcess/OpenThread access and exported self-relative security");
    if (hOwnerProc) CloseHandle(hOwnerProc);
    if (hOwnerThread) CloseHandle(hOwnerThread);
    if (pi.hProcess) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); }
    if (pi.hThread) CloseHandle(pi.hThread);

    if (hAdminToken) { MyWinBindRuntime(&rt->mgr, &admin); CloseHandle(hAdminToken); MyWinBindRuntime(&rt->mgr, &owner); }
    if (hOwnerToken) CloseHandle(hOwnerToken);

    MyWinBindRuntime(&rt->mgr, &admin);
    MyWinBindDesktop(&rt->wm);

    char summary[128];
    snprintf(summary, sizeof(summary), "checks=%d pass=%d fail=%d warn=%d", s.checks, s.passed, s.failed, s.warnings);
    smoke_info(s.group, "summary", summary);
    return s.failed;
}

static void smoke_usage(void)
{
    printf("myOS v204 smoke runner\n");
    printf("usage: ./myos_input --smoke [all|kernel32|user32|lifecycle|gdi|menu|capture|focusdlg|dialogtpl|controls|ownership|hwnd_access|mdi|app_labs|shell_broker|ipc_section|access|handle_invalid|wait_invalid|strict_handles|wait_real|user32_timer|user32_class|user32_longptr|user32_windowpos|user32_dialog_dwlp|user32_popup_modal|user32_redraw|user32_erase|user32_scroll|gdi_bitmap_dc|gdi_region|gdi_dibsection|gdi_dibbits|gdi_stretchblt|gdi_patblt|last_error|apphost|comdlg|services|security]\n");
    printf("exit code: 0 = PASS, non-zero = regression FAIL. WARN lines are known MSDN-compliance gaps.\n");
}

int MyOSRunSmokeTests(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    const char* mode = (argc > 0 && argv && argv[0]) ? argv[0] : "all";
    if (smoke_streq(mode, "help") || smoke_streq(mode, "--help") || smoke_streq(mode, "-h")) {
        smoke_usage();
        return 0;
    }

    SmokeRuntime rt;
    memset(&rt, 0, sizeof(rt));
    int failures = 0;

    printf("myOS v238 menu overlay damage/signature fix\n");
    printf("mode=%s\n", mode);

    if (smoke_streq(mode, "all") || smoke_streq(mode, "kernel32"))       failures += smoke_kernel32(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32"))         failures += smoke_user32(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "lifecycle"))      failures += smoke_lifecycle(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "gdi"))            failures += smoke_gdi(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "gdi_bitmap_dc"))  failures += smoke_gdi_bitmap_dc(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "gdi_region"))     failures += smoke_gdi_region(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "gdi_dibsection")) failures += smoke_gdi_dibsection(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "gdi_dibbits"))    failures += smoke_gdi_dibbits(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "gdi_stretchblt")) failures += smoke_gdi_stretchblt(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "gdi_patblt"))    failures += smoke_gdi_patblt(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32_redraw"))  failures += smoke_user32_redraw(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32_erase"))  failures += smoke_user32_erase(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32_scroll"))  failures += smoke_user32_scroll(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "menu"))           failures += smoke_menu(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "capture"))        failures += smoke_capture(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "focusdlg"))       failures += smoke_focus_dialog(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "dialogtpl"))      failures += smoke_dialog_template_engine(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "controls"))       failures += smoke_controls(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "ownership"))      failures += smoke_ownership(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "hwnd_access"))     failures += smoke_hwnd_access(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "mdi"))            failures += smoke_mdi(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "app_labs"))      failures += smoke_app_labs(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "shell_broker"))  failures += smoke_shell_broker(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "ipc_section"))    failures += smoke_ipc_section(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "access"))         failures += smoke_access_rights(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "handle_invalid")) failures += smoke_handle_invalid(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "wait_invalid"))   failures += smoke_wait_invalid(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "strict_handles")) failures += smoke_strict_handles(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "wait_real"))      failures += smoke_wait_real(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32_timer"))   failures += smoke_user32_timers(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32_class"))   failures += smoke_user32_class_process(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32_longptr")) failures += smoke_user32_longptr(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32_windowpos")) failures += smoke_user32_windowpos(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32_dialog_dwlp")) failures += smoke_user32_dialog_dwlp(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "user32_popup_modal")) failures += smoke_user32_popup_modal(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "last_error"))     failures += smoke_last_error(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "comdlg"))         failures += smoke_comdlg(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "services"))       failures += smoke_services(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "security"))       failures += smoke_security_descriptor(&rt);
    if (smoke_streq(mode, "all") || smoke_streq(mode, "apphost"))        failures += smoke_apphost(&rt);

    if (!smoke_streq(mode, "all") && !smoke_streq(mode, "kernel32") && !smoke_streq(mode, "user32") &&
        !smoke_streq(mode, "lifecycle") && !smoke_streq(mode, "gdi") && !smoke_streq(mode, "gdi_bitmap_dc") && !smoke_streq(mode, "gdi_region") && !smoke_streq(mode, "gdi_dibsection") && !smoke_streq(mode, "gdi_dibbits") && !smoke_streq(mode, "gdi_stretchblt") && !smoke_streq(mode, "gdi_patblt") && !smoke_streq(mode, "menu") && !smoke_streq(mode, "capture") &&
        !smoke_streq(mode, "focusdlg") && !smoke_streq(mode, "dialogtpl") && !smoke_streq(mode, "controls") && !smoke_streq(mode, "ownership") && !smoke_streq(mode, "hwnd_access") && !smoke_streq(mode, "mdi") &&
        !smoke_streq(mode, "app_labs") && !smoke_streq(mode, "shell_broker") && !smoke_streq(mode, "ipc_section") && !smoke_streq(mode, "access") && !smoke_streq(mode, "handle_invalid") &&
        !smoke_streq(mode, "wait_invalid") && !smoke_streq(mode, "strict_handles") && !smoke_streq(mode, "wait_real") && !smoke_streq(mode, "user32_timer") && !smoke_streq(mode, "user32_class") && !smoke_streq(mode, "user32_longptr") && !smoke_streq(mode, "user32_windowpos") && !smoke_streq(mode, "user32_dialog_dwlp") && !smoke_streq(mode, "user32_popup_modal") && !smoke_streq(mode, "last_error") &&
        !smoke_streq(mode, "comdlg") && !smoke_streq(mode, "services") && !smoke_streq(mode, "security") && !smoke_streq(mode, "apphost") && !smoke_streq(mode, "user32_redraw") && !smoke_streq(mode, "user32_erase") && !smoke_streq(mode, "user32_scroll")) {
        smoke_usage();
        smoke_runtime_destroy(&rt);
        return 2;
    }

    smoke_runtime_destroy(&rt);
    printf("SMOKE RESULT: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
