#include <linux/input-event-codes.h>
#include "window.h"
#include "apphost.h"
#include "processhost.h"
#include "myos_diag.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <ctype.h>

// v43.3: shell close fallback uses USER32 DestroyWindow to keep MyWin metadata in sync.
extern BOOL DestroyWindow(HWND hWnd);
extern BOOL MyWinBindRuntime(HWNDManager* lpHwndManager, const Capability* lpCapability);
extern BOOL MyWinBindDesktop(WindowManager* lpWindowManager);
extern const Capability* MyWinGetCurrentCapability(void);
extern void MyWinUnbindRuntime(void);
extern HWNDManager* MyWinGetHwndManager(void);
extern HWND SetCapture(HWND hWnd);
extern BOOL ReleaseCapture(void);
extern HWND GetCapture(void);

// v77 shell HWND/WndProc pipeline.  Shell UI is no longer an invisible
// compositor side-channel: Desktop (#32769), Taskbar (Shell_TrayWnd) and
// Start button (BUTTON) are real HWNDs with normal WndProcs.
#define ID_TASKBAR_START 4001u
#define WM_MDI_VISUAL_CAPTION_H 22
static LRESULT CALLBACK DesktopWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK TaskbarWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
static void wm_open_system_menu(WindowManager* wm, int idx, int x, int y);
static void wm_post_system_command(WindowManager* wm, int idx, UINT cmd, int x, int y);
static void wm_post_desktop_command(WindowManager* wm, UINT cmd, int x, int y);
static void wm_create_shell_hwnds(WindowManager* wm);
static void wm_publish_shell_state(WindowManager* wm, HWND hwnd, const char* title,
                                   int x, int y, int w, int h, UINT lastMsg, DWORD zOrder);
static void wm_publish_all_shell_state(WindowManager* wm);
static int wm_shell_post_mouse(WindowManager* wm, HWND hwnd, UINT msg, int x, int y, WPARAM keys);
static void wm_send_app_menu_command(WindowManager* wm, HWND hwnd, UINT id);
static HWND wm_deep_child_from_screen(HWND hParent, int screen_x, int screen_y);
static HWND wm_mdi_child_from_screen_nc(HWND hApp, int screen_x, int screen_y, HWND* outClient);
static int wm_mdi_visual_caption_hit(HWND hChild, int screen_x, int screen_y);
static int wm_try_mdi_caption_mouse_down(WindowManager* wm, int x, int y);
static int wm_deliver_app_client_mouse_sync(WindowManager* wm, UINT msg, int x, int y, WPARAM keys);
static void wm_mdi_raw_drag_begin(HWND hChild, int screenX, int screenY);
static int wm_try_mdi_caption_mouse_move(WindowManager* wm, int x, int y);
static int wm_try_mdi_caption_mouse_up(WindowManager* wm, int x, int y);

static LRESULT CALLBACK ipcproxy_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    // v61: this WndProc is the parent-side HWND endpoint for a real GUI child.
    // It forwards scalar Win32 messages over the ProcessHost IPC socket; the
    // child queues them and runs its GetMessage/DispatchMessage loop there.
    if (Msg == WM_NCCREATE) return TRUE;

    HWNDManager* mgr = MyWinGetHwndManager();
    DWORD ownerPid = mgr ? hwnd_get_owner_pid(mgr, hWnd) : 0;
    if (ownerPid) {
        /* v166: preserve Win32 capture/drop ordering for OOP proxy windows.
           The proxy owns the parent-side SetCapture so raw mouse moves keep
           flowing to the child while dragging.  Releasing capture before
           forwarding WM_LBUTTONUP sent WM_CAPTURECHANGED to the child first;
           DragLab then cleared its drag state and reported MouseUp/drop=no even
           while the box was visibly inside the target.  Real Win32 code sees
           WM_LBUTTONUP while capture is still active and only then calls
           ReleaseCapture, so forward the button-up first and restore/release
           the parent proxy capture afterwards. */
        int releaseAfterDispatch = 0;
        if (Msg == WM_LBUTTONDOWN) SetCapture(hWnd);
        if (Msg == WM_LBUTTONUP && GetCapture() == hWnd) releaseAfterDispatch = 1;
        char text[64];
        snprintf(text, sizeof(text), "parent Dispatch msg=0x%04x", (unsigned)Msg);
        MyProcessHostSendWindowMessage(ownerPid, hWnd, Msg, wParam, lParam, text);
        if (releaseAfterDispatch && GetCapture() == hWnd) ReleaseCapture();
    }

    if (Msg == WM_CLOSE)
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    if (Msg == WM_DESTROY || Msg == WM_NCDESTROY || Msg == WM_CREATE)
        return 0;
    return 0;
}


static void desktop_make_default_path(WindowManager* wm);
static int  desktop_is_editable_file(const char* name);
static void desktop_make_label_lines(const char* name, char* line1, int line1_len, char* line2, int line2_len);
static void shorten_middle(const char* in, char* out, int out_len);
static void desktop_make_layout_path(WindowManager* wm);
static int  desktop_ensure_dir(WindowManager* wm);
static void desktop_save_layout(WindowManager* wm);
static int  desktop_load_saved_pos(WindowManager* wm, const char* name, int* x, int* y);
static void desktop_clamp_icon(WindowManager* wm, DesktopIcon* ic);
static void desktop_snap_icon(WindowManager* wm, DesktopIcon* ic);
static long long desktop_now_ms(void);
static void draw_desktop_icons(WindowManager* wm, Framebuffer* fb);
static int  desktop_icon_at(WindowManager* wm, int x, int y);
static int  desktop_is_wallpaper_file(const char* name);

static int valid_window_index(WindowManager* wm, int index)
{
    return wm && index >= 0 && index < wm->count && !wm->wins[index].closed;
}

/* v240: keep heavy terminal scrollback/state out of the hot Window slot.
   Terminal payloads are allocated only for APP_TERMINAL windows; common
   compositor paths now walk a compact Window table and touch terminal memory
   only when actually drawing/polling/typing into a terminal. */
static void wm_release_terminal_payload(WindowManager* wm, Window* w)
{
    if (!w || !w->term) return;
    if (wm && wm->bus && w->term->cap.id)
        ipc_unregister(wm->bus, w->term->cap.id);
    if (w->term->pipe_fd >= 0) {
        close(w->term->pipe_fd);
        w->term->pipe_fd = -1;
    }
    free(w->term);
    w->term = NULL;
}

static Terminal* wm_alloc_terminal_payload(Window* w)
{
    if (!w) return NULL;
    if (!w->term) w->term = (Terminal*)calloc(1, sizeof(Terminal));
    return w->term;
}

static HWND window_hwnd(Window* w)
{
    if (!w || w->closed) return 0;
    return (w->app_type == APP_TERMINAL) ? (w->term ? w->term->hwnd : 0) : w->app_hwnd;
}

static void wm_fill_window_state(WindowManager* wm, int index, MyWindowState* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->cbSize = sizeof(*out);
    if (!valid_window_index(wm, index)) return;

    Window* w = &wm->wins[index];
    HWND hwnd = window_hwnd(w);
    out->hWnd = hwnd;
    out->ownerPid = w->process_id ? w->process_id : (w->app_type == APP_TERMINAL && w->term ? w->term->cap.id : w->app_cap.id);
    out->ownerTid = w->thread_id ? w->thread_id : out->ownerPid;
    out->rcWindow.left = w->x;
    out->rcWindow.top = w->y;
    out->rcWindow.right = w->x + w->w;
    out->rcWindow.bottom = w->y + w->h;
    out->rcClient.left = w->x + 1;
    out->rcClient.top = w->y + TITLEBAR_H;
    out->rcClient.right = w->x + w->w - 1;
    out->rcClient.bottom = w->y + w->h - 1;
    out->visible = !w->closed && !w->minimized;
    out->minimized = w->minimized;
    out->active = (index == wm->focused);
    out->focused = out->active;
    out->enabled = TRUE;
    out->hasCapture = (GetCapture() == hwnd) ? TRUE : FALSE;
    out->destroyed = w->closed;
    out->flags = 0;
    if (out->visible)   out->flags |= MYWSF_VISIBLE;
    if (out->minimized) out->flags |= MYWSF_MINIMIZED;
    if (out->active)    out->flags |= MYWSF_ACTIVE;
    if (out->destroyed) out->flags |= MYWSF_DESTROYED;
    out->dirtyFlags = MYWS_DIRTY_RECT|MYWS_DIRTY_VISIBLE|MYWS_DIRTY_FOCUS|MYWS_DIRTY_ZORDER|MYWS_DIRTY_TEXT|MYWS_DIRTY_OWNER;
    out->zOrder = (DWORD)index;
    out->style = w->style;
    out->exStyle = 0;
    out->stateVersion = wm->state_version;
    out->updateSerial = wm->state_version;
    snprintf(out->szTitle, sizeof(out->szTitle), "%s", w->title);
}

static int wm_should_signal_window_state_event(UINT msg)
{
    /* v80.1: WM_NCHITTEST/WM_NCLBUTTON*/
    /* and WM_SYSCOMMAND are control-flow/query messages, not payload-free
       state notifications.  Publishing them through the Spy/subscription
       path reused wParam/lParam as signal payload (wParam=source HWND), and
       a normal DefWindowProc could later reinterpret that as HT or SC data.
       Result: with Spy++ open, titlebar moves could recurse/minimize/move the
       wrong window and appear as an OS hang.  Keep WSTS updated for diagnostics,
       but only signal stable state changes. */
    switch (msg) {
    case WM_NCHITTEST:
    case WM_NCMOUSEMOVE:
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP:
    case WM_SYSCOMMAND:
    case WM_WINDOWPOSCHANGING:
        return 0;
    default:
        return 1;
    }
}

static void wm_publish_window_state_event(WindowManager* wm, int index, UINT msg)
{
    if (!valid_window_index(wm, index) || !wm->mgr) return;
    HWND hwnd = window_hwnd(&wm->wins[index]);
    if (!hwnd) return;

    // v17.2/v80.1: update shared/read-only state first.  Only stable state
    // changes are posted as queue signals; transient NC/query messages stay in
    // WSTS as lastMessage but do not re-enter WndProcs as fake posted messages.
    wm->state_version++;
    MyWindowState st;
    wm_fill_window_state(wm, index, &st);
    st.lastMessage = msg;
    if (wm->wins[index].app_type == APP_IPC_PROXY && wm->wins[index].process_id)
        MyProcessHostUpdateGuiRect(wm->wins[index].process_id, wm->wins[index].x, wm->wins[index].y, wm->wins[index].w, wm->wins[index].h);
    hwnd_update_window_state(wm->mgr, &st);
    if (wm_should_signal_window_state_event(msg)) {
        hwnd_publish_from_window(wm->mgr, hwnd, msg,
                                 (WPARAM)hwnd, (LPARAM)st.stateVersion);
    }
}

static void __attribute__((unused)) wm_publish_window_state(WindowManager* wm, int index)
{
    wm_publish_window_state_event(wm, index, WM_WINDOWPOSCHANGED);
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void wm_clamp_window_to_screen(WindowManager* wm, int* x, int* y, int w, int h)
{
    if (!wm || !x || !y) return;

    int sw = wm->screen_w > 0 ? wm->screen_w : 640;
    int sh = wm->screen_h > 0 ? wm->screen_h : 480;
    int work_h = sh - TASKBAR_H;
    if (work_h < TITLEBAR_H + 40) work_h = sh;

    int max_x = sw - w - 10;
    int max_y = work_h - h - 10;

    // Bei kleinen Framebuffern lieber teilweise sichtbar als komplett offscreen.
    if (max_x < 10) max_x = 10;
    if (max_y < 10) max_y = 10;

    *x = clamp_int(*x, 10, max_x);
    *y = clamp_int(*y, 10, max_y);
}

static void wm_focus_fallback(WindowManager* wm)
{
    if (!wm || wm->focused >= 0) return;
    for (int i = wm->count - 1; i >= 0; i--) {
        if (!wm->wins[i].closed && !wm->wins[i].minimized) {
            wm->focused = i;
            wm->wins[i].active = 1;
            wm_publish_window_state_event(wm, i, WM_ACTIVATE);
            break;
        }
    }
}

static MyWindowLifetimeAudit g_WmLifetimeAudit;

void wm_get_lifetime_audit_stats(MyWindowLifetimeAudit* out)
{
    if (!out) return;
    *out = g_WmLifetimeAudit;
}

static BOOL wm_handle_owner_pid(HANDLE h, DWORD* outPid)
{
    if (outPid) *outPid = 0;
    if (!h) return FALSE;
    MyHandleInfo hi;
    memset(&hi, 0, sizeof(hi));
    if (!MyGetHandleInfo(h, &hi) || !hi.pid) return FALSE;
    if (outPid) *outPid = hi.pid;
    return TRUE;
}

static BOOL wm_resolve_loader_owner(Window* w, DWORD* outOwnerPid)
{
    if (outOwnerPid) *outOwnerPid = 0;
    if (!w || (!w->process_handle && !w->thread_handle)) return FALSE;

    if (w->process_handle_owner_pid) {
        if (outOwnerPid) *outOwnerPid = w->process_handle_owner_pid;
        return TRUE;
    }

    DWORD procOwner = 0;
    DWORD threadOwner = 0;
    BOOL haveProc = wm_handle_owner_pid(w->process_handle, &procOwner);
    BOOL haveThread = wm_handle_owner_pid(w->thread_handle, &threadOwner);

    if (haveProc && haveThread && procOwner && threadOwner && procOwner != threadOwner) {
        g_WmLifetimeAudit.loader_owner_mismatch++;
        fprintf(stderr, "[LIFETIME] loader handle owner mismatch image='%s' pid=%lu hProcess=0x%lx owner=%lu hThread=0x%lx owner=%lu\n",
                w->image_name[0] ? w->image_name : "?",
                (unsigned long)w->process_id,
                (unsigned long)w->process_handle, (unsigned long)procOwner,
                (unsigned long)w->thread_handle, (unsigned long)threadOwner);
        return FALSE;
    }

    DWORD ownerPid = procOwner ? procOwner : threadOwner;
    if (!ownerPid) {
        g_WmLifetimeAudit.loader_missing_owner++;
        fprintf(stderr, "[LIFETIME] missing loader handle owner image='%s' pid=%lu hProcess=0x%lx hThread=0x%lx -- no magic PID fallback used\n",
                w->image_name[0] ? w->image_name : "?",
                (unsigned long)w->process_id,
                (unsigned long)w->process_handle,
                (unsigned long)w->thread_handle);
        return FALSE;
    }

    /* v183: No more magic PID fallback.  If the window forgot to persist the
       HANDLE-table owner, recover it from the real handle table only for this
       cleanup, and count it as an invariant violation for the debug badge. */
    g_WmLifetimeAudit.loader_recovered_owner++;
    w->process_handle_owner_pid = ownerPid;
    if (outOwnerPid) *outOwnerPid = ownerPid;
    fprintf(stderr, "[LIFETIME] recovered loader handle owner image='%s' pid=%lu ownerPid=%lu\n",
            w->image_name[0] ? w->image_name : "?",
            (unsigned long)w->process_id,
            (unsigned long)ownerPid);
    return TRUE;
}

static void wm_close_loader_handles(Window* w)
{
    if (!w || (!w->process_handle && !w->thread_handle)) {
        if (w) w->process_handle_owner_pid = 0;
        return;
    }

    // Parent-side hProcess/hThread handles are strict per-process HANDLE-table
    // entries.  They must be closed in the exact HANDLE-table context that
    // received them from CreateProcess.  v183 deliberately removed the old
    // PID-46 fallback: an unknown owner is a lifetime bug, not a cleanup policy.
    DWORD ownerPid = 0;
    if (!wm_resolve_loader_owner(w, &ownerPid) || !ownerPid) {
        g_WmLifetimeAudit.loader_close_fail++;
        return;
    }

    if (!MyWinEnterProcessContext(ownerPid)) {
        g_WmLifetimeAudit.loader_close_fail++;
        fprintf(stderr, "[LIFETIME] failed to enter loader owner context ownerPid=%lu image='%s' pid=%lu\n",
                (unsigned long)ownerPid,
                w->image_name[0] ? w->image_name : "?",
                (unsigned long)w->process_id);
        return;
    }

    BOOL allOk = TRUE;
    if (w->process_handle) {
        HANDLE h = w->process_handle;
        if (CloseHandle(h)) {
            w->process_handle = 0;
        } else {
            allOk = FALSE;
            fprintf(stderr, "[LIFETIME] CloseHandle(hProcess=0x%lx) failed ownerPid=%lu err=%lu\n",
                    (unsigned long)h, (unsigned long)ownerPid, (unsigned long)GetLastError());
        }
    }
    if (w->thread_handle) {
        HANDLE h = w->thread_handle;
        if (CloseHandle(h)) {
            w->thread_handle = 0;
        } else {
            allOk = FALSE;
            fprintf(stderr, "[LIFETIME] CloseHandle(hThread=0x%lx) failed ownerPid=%lu err=%lu\n",
                    (unsigned long)h, (unsigned long)ownerPid, (unsigned long)GetLastError());
        }
    }
    MyWinLeaveProcessContext();

    if (allOk) g_WmLifetimeAudit.loader_close_ok++;
    else g_WmLifetimeAudit.loader_close_fail++;

    if (!w->process_handle && !w->thread_handle) w->process_handle_owner_pid = 0;
}

static int wm_live_ipc_windows_for_process(WindowManager* wm, DWORD processId, int excludeIndex)
{
    if (!wm || !processId) return 0;
    int n = 0;
    for (int i = 0; i < wm->count; ++i) {
        if (i == excludeIndex) continue;
        Window* other = &wm->wins[i];
        if (other->closed) continue;
        if (other->app_type == APP_IPC_PROXY && other->process_id == processId) n++;
    }
    return n;
}

static void wm_terminate_window_process(WindowManager* wm, int index)
{
    if (!wm || index < 0 || index >= wm->count) return;
    Window* w = &wm->wins[index];
    if (!w->process_id) return;

    // v183: An OOP process may own several parent-side top-level proxy HWNDs
    // (DialogLab root + modeless/modal probes).  Destroying a secondary proxy
    // must not kill the process.  Destroying the last proxy is the commit edge:
    // send WM_CLOSE to the child over ProcessHost IPC, then close the loader's
    // parent-side hProcess/hThread handles in their real owner context.
    if (w->app_type == APP_IPC_PROXY) {
        if (wm_live_ipc_windows_for_process(wm, w->process_id, index) == 0) {
            (void)MyProcessHostSendWindowMessage(w->process_id, w->app_hwnd, WM_CLOSE, 0, 0,
                                                 "parent last proxy teardown WM_CLOSE");
            wm_close_loader_handles(w);
        }
        return;
    }

    // v46: AppHost-launched desktop frames own a PROCESS/THREAD-lite pair.
    // Closing the visible top-level window now also transitions the app process
    // out of STILL_ACTIVE, which makes ObjectLab/WaitLab see Windows-like
    // process lifetime instead of an immortal Process-Lite record.
    if (MyWinEnterProcessContext(w->process_id)) {
        HANDLE hp = OpenProcess(PROCESS_TERMINATE|PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE, FALSE, w->process_id);
        if (hp) {
            DWORD code = STILL_ACTIVE;
            if (!GetExitCodeProcess(hp, &code) || code == STILL_ACTIVE)
                TerminateProcess(hp, 0);
            CloseHandle(hp);
        }
        MyWinLeaveProcessContext();
    }

    wm_close_loader_handles(w);
}

static void wm_mark_slot_closed(WindowManager* wm, int index)
{
    if (!wm || index < 0 || index >= wm->count) return;

    Window* w = &wm->wins[index];
    if (w->closed) return;

    wm_terminate_window_process(wm, index);

    if (w->term) w->term->hwnd = 0;
    w->app_hwnd = 0;
    w->closed = 1;
    w->active = 0;
    w->minimized = 0;
    if (wm->free_top < MAX_WINDOWS) wm->free_stack[wm->free_top++] = index;

    if (w->app_type == APP_PUMP)
        pump_destroy();
    if (w->app_type == APP_DEADLOCK)
        deadlock_destroy();
    if (w->app_type == APP_TERMINAL)
        wm_release_terminal_payload(wm, w);

    if (wm->focused == index)
        wm->focused = -1;

    wm_focus_fallback(wm);
}

int wm_on_destroyed_hwnd(WindowManager* wm, HWND hwnd)
{
    if (!wm || !hwnd) return 0;
    int index = -1;
    for (int i = 0; i < wm->count; i++) {
        Window* w = &wm->wins[i];
        if (w->closed) continue;
        if (window_hwnd(w) == hwnd) { index = i; break; }
    }
    if (index < 0) return 0;

    // hwnd_destroy() has already updated the shared HWND state section to
    // destroyed.  The desktop shell now only mirrors that lifetime into its
    // visible Window slot.  Do not call wm_publish_window_state_event() after
    // setting closed=1; valid_window_index() intentionally rejects tombstones.
    wm_mark_slot_closed(wm, index);
    return 1;
}

static BOOL wm_destroy_window_in_owner_context(WindowManager* wm, HWND hwnd, const Capability* ownerCap)
{
    if (!wm || !wm->mgr || !hwnd || !ownerCap || !ownerCap->id) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* v189: shell close must not call DestroyWindow while USER32 is still
       bound to the shell/session capability.  DestroyWindow is owner-thread
       affine by design; calling it as shell made the API reject the request,
       after which the old wm_close_window fallback went straight to
       hwnd_destroy().  That bypasses USER32's child-tree teardown and leaks
       child HWNDs (DialogLab buttons, modal OK/Cancel, ServiceLab controls,
       etc.).  Temporarily enter the window owner's runtime context so the
       normal DestroyWindow(parent)->DestroyWindow(children)->WM_DESTROY/
       WM_NCDESTROY path remains the only successful close path. */
    const Capability* prev = MyWinGetCurrentCapability();
    Capability prevCap;
    int havePrev = 0;
    if (prev) { prevCap = *prev; havePrev = 1; }

    Capability cap = *ownerCap;
    cap.flags |= CAP_IPC | CAP_WINDOW_READ | CAP_WINDOW_CONTROL;
    cap_add_target(&cap, 0);

    BOOL bound = MyWinBindRuntime(wm->mgr, &cap);
    if (bound) MyWinBindDesktop(wm);
    BOOL ok = bound ? DestroyWindow(hwnd) : FALSE;
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();

    if (havePrev) {
        MyWinBindRuntime(wm->mgr, &prevCap);
        MyWinBindDesktop(wm);
    } else {
        MyWinUnbindRuntime();
    }

    if (!ok) SetLastError(err);
    return ok;
}

static void wm_close_window(WindowManager* wm, int index)
{
    if (!valid_window_index(wm, index)) return;

    Window* w = &wm->wins[index];
    HWND hwnd = window_hwnd(w);
    if (!wm->mgr || !hwnd) {
        // No live HWND means this is already a broken/legacy shell slot.
        // Close the visible desktop frame anyway so the user is never stuck
        // with an immortal app rectangle.
        wm_mark_slot_closed(wm, index);
        return;
    }

    // v43.3: shell close is a USER32 close request, but it must be delivered
    // synchronously here. Posting WM_CLOSE left some USER32-backed apps (notably
    // Calculator) visually alive until their app queue happened to drain; legacy
    // terminal windows closed because their older path was effectively direct.
    //
    // This matches the practical Win32 shell behavior better for our single-loop
    // PoC: the app still gets first refusal by handling WM_CLOSE and not calling
    // DestroyWindow(), while DefWindowProcA performs the normal destruction.
    Capability sender = w->app_cap;
    if (!sender.id && w->app_type == APP_TERMINAL && w->term) sender = w->term->cap;
    if (!sender.id) {
        hwnd_destroy(wm->mgr, hwnd);
        wm_mark_slot_closed(wm, index);
        return;
    }

    int close_r = hwnd_send_timeout(wm->mgr, &sender, hwnd, WM_CLOSE, 0, 0, 100);
    if (close_r != 0) {
        // Last-resort compatibility fallback: keep the desktop frame from
        // becoming immortal if a stale/legacy HWND can no longer receive.
        if (hwnd_is_window(wm->mgr, hwnd)) {
            if (!wm_destroy_window_in_owner_context(wm, hwnd, &sender)) {
                printf("[LIFETIME] shell close owner-context DestroyWindow failed hwnd=%u owner=%u err=%u; using last-resort raw HWND destroy\n",
                       (unsigned)hwnd, (unsigned)sender.id, (unsigned)GetLastError());
                hwnd_destroy(wm->mgr, hwnd);
                wm_mark_slot_closed(wm, index);
            }
        } else {
            wm_mark_slot_closed(wm, index);
        }
        return;
    }

    // v43.3: harden the shell close contract.  USER32-backed apps route
    // WM_CLOSE through a thunk/class WndProc.  Some older labs still return 0
    // for unknown messages instead of falling through to DefWindowProcA, which
    // made the shell frame immortal even though the user clicked X.
    //
    // Windows lets an app veto WM_CLOSE intentionally.  myOS has no unsaved-data
    // prompt yet, so a shell X is authoritative for now: first deliver WM_CLOSE,
    // then force USER32 DestroyWindow if the HWND survived.  DestroyWindow keeps
    // WindowInfo, child HWNDs, WM_DESTROY/WM_NCDESTROY and WindowManager sync in
    // one path; direct hwnd_destroy would leak USER32 metadata.
    if (hwnd_is_window(wm->mgr, hwnd)) {
        if (!wm_destroy_window_in_owner_context(wm, hwnd, &sender)) {
            printf("[LIFETIME] shell close owner-context DestroyWindow failed hwnd=%u owner=%u err=%u; using last-resort raw HWND destroy\n",
                   (unsigned)hwnd, (unsigned)sender.id, (unsigned)GetLastError());
            hwnd_destroy(wm->mgr, hwnd);
            wm_mark_slot_closed(wm, index);
        }
    } else {
        wm_mark_slot_closed(wm, index);
    }
}

int wm_close_hwnd(WindowManager* wm, HWND hwnd)
{
    if (!wm || !hwnd) return 0;
    int idx = wm_find_hwnd(wm, hwnd);
    if (idx < 0) return 0;
    wm_close_window(wm, idx);
    return (!wm->mgr || !hwnd_is_window(wm->mgr, hwnd)) ? 1 : 0;
}


void wm_init(WindowManager* wm, HWNDManager* mgr, IPCBus* bus)
{
    memset(wm, 0, sizeof(*wm));
    wm->free_top = 0;
    wm->focused  = -1;
    wm->drag_idx = -1;
    wm->bg       = BG_DARK;
    memset(&wm->wallpaper, 0, sizeof(wm->wallpaper));
    wm->wallpaper_enabled = 0;
    wm->wallpaper_path[0] = 0;
    snprintf(wm->wallpaper_status, sizeof(wm->wallpaper_status), "Wallpaper: none");
    wm->screen_w = 640;
    wm->screen_h = 480;
    wm->mgr      = mgr;
    wm->bus      = bus;
    wm->desktop_layout_mode = DESKTOP_LAYOUT_GRID;
    wm->desktop_selected = -1;
    wm->desktop_drag_idx = -1;
    wm->desktop_last_click_idx = -1;
    wm->menu_kind = MENU_KIND_START;
    wm->menu_target_idx = -1;
    wm->menu_target_hwnd = 0;
    desktop_make_default_path(wm);
    desktop_ensure_dir(wm);
    desktop_make_layout_path(wm);
    wm_desktop_reload(wm);
    wm_create_shell_hwnds(wm);
    MyAppHostBindShell(wm);
}

int wm_add(WindowManager* wm, int x, int y, int w, int h,
           const char* title, Capability cap)
{
    int i = -1;

    // v240: O(1) reuse of closed shell slots via a free stack.
    if (wm->free_top > 0) i = wm->free_stack[--wm->free_top];
    else {
        if (wm->count >= MAX_WINDOWS) return -1;
        i = wm->count++;
    }

    wm_clamp_window_to_screen(wm, &x, &y, w, h);

    // Slot komplett zurücksetzen
    wm_release_terminal_payload(wm, &wm->wins[i]);
    memset(&wm->wins[i], 0, sizeof(Window));
    wm->wins[i].x      = x;
    wm->wins[i].y      = y;
    wm->wins[i].w      = w;
    wm->wins[i].h      = h;
    wm->wins[i].active = 1;
    wm->wins[i].closed = 0;
    wm->wins[i].minimized = 0;
    wm->wins[i].maximized = 0;
    wm->wins[i].restore_x = x; wm->wins[i].restore_y = y; wm->wins[i].restore_w = w; wm->wins[i].restore_h = h;
    wm->wins[i].style = 0;
    wm->wins[i].app_type = APP_TERMINAL;
    wm->wins[i].app_hwnd = 0;
    wm->wins[i].app_cap = cap;
    strncpy(wm->wins[i].title, title, 63);
    wm->wins[i].title[63] = 0;

    // Terminal initialisieren mit Cap
    if (!wm_alloc_terminal_payload(&wm->wins[i])) {
        if (i == wm->count - 1) wm->count--;
        return -1;
    }
    term_init(wm->wins[i].term, cap, wm->bus, wm->mgr);

    // HWND automatisch erstellen - wie CreateWindow in WinAPI
    // Ab jetzt ist das Fenster im System adressierbar
    if (wm->mgr) {
        wm->wins[i].term->hwnd = hwnd_create(wm->mgr,
                                             term_hwnd_proc,
                                             wm->wins[i].term, cap);
    }

    // IPC registrieren
    if (wm->bus)
        ipc_register(wm->bus, cap.id, term_wndproc,
                     wm->wins[i].term, cap);

    int old_focus = wm->focused;
    if (old_focus >= 0) wm->wins[old_focus].active = 0;
    wm->focused = i;
    wm->wins[i].active = 1;
    if (old_focus >= 0 && old_focus != i) wm_publish_window_state_event(wm, old_focus, WM_ACTIVATE);
    wm_publish_window_state_event(wm, i, WM_ACTIVATE);
    return i;
}


static int wm_reserve_window_slot(WindowManager* wm)
{
    if (!wm) return -1;
    if (wm->free_top > 0) return wm->free_stack[--wm->free_top];
    if (wm->count >= MAX_WINDOWS) return -1;
    return wm->count++;
}

static int wm_prepare_app_window(WindowManager* wm, int* x, int* y, int w, int h,
                                 const char* title, const char* fallback_title,
                                 AppType type, Capability cap)
{
    int i = wm_reserve_window_slot(wm);
    if (i < 0) return -1;

    wm_clamp_window_to_screen(wm, x, y, w, h);

    Window* win = &wm->wins[i];
    memset(win, 0, sizeof(*win));
    win->x = *x;
    win->y = *y;
    win->w = w;
    win->h = h;
    win->closed = 0;
    win->minimized = 0;
    win->maximized = 0;
    win->restore_x = *x; win->restore_y = *y; win->restore_w = w; win->restore_h = h;
    win->style = 0;
    win->app_type = type;
    win->app_cap = cap;
    snprintf(win->title, sizeof(win->title), "%s", title ? title : fallback_title);
    return i;
}

static int wm_finish_app_window(WindowManager* wm, int i)
{
    if (!valid_window_index(wm, i)) return -1;

    // Refactor guard: never leave a visible shell frame around a failed
    // USER32/CreateWindowExA app HWND.  That was the immortal-X bug: the
    // desktop slot survived, but there was no HWND left to receive WM_CLOSE.
    if (wm->wins[i].app_type != APP_TERMINAL && !wm->wins[i].app_hwnd) {
        wm_mark_slot_closed(wm, i);
        return -1;
    }

    int old_focus = wm->focused;
    if (old_focus >= 0) wm->wins[old_focus].active = 0;
    wm->focused = i;
    wm->wins[i].active = 1;
    if (old_focus >= 0 && old_focus != i) wm_publish_window_state_event(wm, old_focus, WM_ACTIVATE);
    wm_publish_window_state_event(wm, i, WM_ACTIVATE);
    return i;
}

int wm_add_calc(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, CALC_W, CALC_H, title, "Rechner", APP_CALC, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = calc_create(wm->mgr, NULL, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_editor(WindowManager* wm, int x, int y, const char* path, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, EDITOR_W, EDITOR_H, title, "Texteditor", APP_EDITOR, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = editor_create(wm->mgr, path, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_spy(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, SPY_W, SPY_H, title, "myOS Spy++", APP_SPY, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = spy_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_access(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, ACCESS_W, ACCESS_H, title, "AccessLab", APP_ACCESS, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = access_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_pump(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, PUMP_W, PUMP_H, title, "PumpLab", APP_PUMP, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = pump_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_deadlock(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, DEADLOCK_W, DEADLOCK_H, title, "DeadlockLab", APP_DEADLOCK, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = deadlock_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_section(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, SECTION_W, SECTION_H, title, "SectionLab", APP_SECTION, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = section_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_objectlab(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, OBJECT_W, OBJECT_H, title, "ObjectLab", APP_OBJECT, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = objectlab_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_waitlab(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, WAITLAB_W, WAITLAB_H, title, "WaitLab", APP_WAITLAB, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = waitlab_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_clipmenulab(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, CLIPMENU_W, CLIPMENU_H, title, "ClipMenuLab", APP_CLIPMENU, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = clipmenu_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_paintlab(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, PAINTLAB_W, PAINTLAB_H, title, "myOS PaintLab", APP_PAINTLAB, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = paintlab_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_draglab(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, DRAGLAB_W, DRAGLAB_H, title, "myOS DragLab", APP_DRAGLAB, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = draglab_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_controllab(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, CONTROLLAB_W, CONTROLLAB_H, title, "myOS ControlLab", APP_CONTROLLAB, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = controllab_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_servicelab(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, SERVICELAB_W, SERVICELAB_H, title, "myOS ServiceLab", APP_SERVICELAB, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = servicelab_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_dialoglab(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, DIALOGLAB_W, DIALOGLAB_H, title, "myOS DialogLab", APP_DIALOGLAB, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = dialoglab_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}

int wm_add_mdilab(WindowManager* wm, int x, int y, const char* title, Capability cap)
{
    int i = wm_prepare_app_window(wm, &x, &y, MDILAB_W, MDILAB_H, title, "myOS MDILab", APP_MDILAB, cap);
    if (i < 0) return -1;
    if (wm->mgr) wm->wins[i].app_hwnd = mdilab_create(wm->mgr, x, y, cap);
    return wm_finish_app_window(wm, i);
}


int wm_add_ipc_proxy(WindowManager* wm, int x, int y, int w, int h, const char* title, const char* class_name, Capability cap, int linux_pid, const char* status, HWND owner_hwnd, DWORD style, DWORD ex_style)
{
    const char* cls = (class_name && class_name[0]) ? class_name : "myOS.IPCProxyWindow";
    if (w < 180) w = 180;
    if (h < 110) h = 110;
    int i = wm_prepare_app_window(wm, &x, &y, w, h, title, "myOS IPC GUI Child", APP_IPC_PROXY, cap);
    if (i < 0) return -1;

    Window* win = &wm->wins[i];
    win->ipc_linux_pid = linux_pid;
    snprintf(win->ipc_status, sizeof(win->ipc_status), "%s", status && status[0] ? status : "CREATE_WINDOW over IPC");
    snprintf(win->ipc_class, sizeof(win->ipc_class), "%s", cls);

    if (wm->mgr) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ipcproxy_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = cls;
        RegisterClassExA(&wc);
        DWORD createStyle = style ? style : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        DWORD createExStyle = ex_style;
        win->app_hwnd = CreateWindowExA(createExStyle, cls, win->title, createStyle | WS_VISIBLE,
                                        x, y, w, h, owner_hwnd, 0, wc.hInstance, NULL);
    }
    return wm_finish_app_window(wm, i);
}

static int wm_add_sharedbus_single(WindowManager* wm, int x, int y, const char* title, Capability cap, AppType type)
{
    int i = wm_prepare_app_window(wm, &x, &y, SHARED_BUS_W, SHARED_BUS_H, title, "SharedBus", type, cap);
    if (i < 0) return -1;
    if (wm->mgr) {
        if (type == APP_BUS_PRODUCER) wm->wins[i].app_hwnd = sharedbus_create_producer(wm->mgr, cap);
        else wm->wins[i].app_hwnd = sharedbus_create_consumer(wm->mgr, cap);
    }
    return wm_finish_app_window(wm, i);
}

int wm_add_sharedbus_pair(WindowManager* wm, int x, int y)
{
    Capability prod = cap_create(1600 + wm->count, "bus-producer", CAP_IPC|CAP_SECTION_MAP|CAP_WINDOW_READ);
    Capability cons = cap_create(1700 + wm->count, "bus-consumer", CAP_IPC|CAP_SECTION_MAP|CAP_WINDOW_READ);
    cap_add_target(&prod, 0);
    cap_add_target(&cons, 0);

    int ip = wm_add_sharedbus_single(wm, x, y, "SharedBus Producer", prod, APP_BUS_PRODUCER);
    int ic = wm_add_sharedbus_single(wm, x + 34, y + SHARED_BUS_H + 18, "SharedBus Consumer", cons, APP_BUS_CONSUMER);
    if (ip >= 0 && ic >= 0) {
        wm_publish_window_state_event(wm, ip, WM_WINDOWPOSCHANGED);
        wm_publish_window_state_event(wm, ic, WM_WINDOWPOSCHANGED);
    }
    return ip >= 0 && ic >= 0 ? ip : -1;
}


// ── Hit-Tests ────────────────────────────────

static int hit_close(Window* w, int x, int y)
{
    int bx = w->x + w->w - TITLEBAR_H;
    return x >= bx && x < bx+TITLEBAR_H && y >= w->y && y < w->y+TITLEBAR_H;
}

static int hit_minimize(Window* w, int x, int y)
{
    int bx = w->x + w->w - TITLEBAR_H*2;
    return x >= bx && x < bx+TITLEBAR_H && y >= w->y && y < w->y+TITLEBAR_H;
}

static int hit_titlebar(Window* w, int x, int y)
{
    return x >= w->x && x < w->x+w->w-TITLEBAR_H*2 &&
           y >= w->y && y < w->y+TITLEBAR_H;
}

static int hit_left_edge(Window* w, int x)
{
    return x >= w->x && x < w->x + RESIZE_GRIP;
}

static int hit_right_edge(Window* w, int x)
{
    return x >= w->x + w->w - RESIZE_GRIP && x < w->x + w->w;
}

static int hit_top_edge(Window* w, int y)
{
    return y >= w->y && y < w->y + RESIZE_GRIP;
}

static int hit_bottom_edge(Window* w, int y)
{
    return y >= w->y + w->h - RESIZE_GRIP && y < w->y + w->h;
}

static LRESULT wm_frame_hit_test(Window* w, int x, int y)
{
    if (!w || w->closed || w->minimized) return HTNOWHERE;
    if (x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h)
        return HTNOWHERE;

    // MSDN-style frame map for the current myOS classic frame:
    // - screen coords in lParam
    // - titlebar buttons are non-client
    // - v79: all edges/corners produce HTLEFT/HTTOP/... resize codes
    // - client starts at x+1 / y+TITLEBAR_H, except resize borders.
    if (hit_close(w, x, y))    return HTCLOSE;
    if (hit_minimize(w, x, y)) return HTMINBUTTON;

    int l = hit_left_edge(w, x);
    int r = hit_right_edge(w, x);
    int t = hit_top_edge(w, y);
    int b = hit_bottom_edge(w, y);

    if (t && l) return HTTOPLEFT;
    if (t && r) return HTTOPRIGHT;
    if (b && l) return HTBOTTOMLEFT;
    if (b && r) return HTBOTTOMRIGHT;
    if (l) return HTLEFT;
    if (r) return HTRIGHT;
    if (t) return HTTOP;
    if (b) return HTBOTTOM;

    if (hit_titlebar(w, x, y)) return HTCAPTION;
    return HTCLIENT;
}

static void wm_min_size_for_window(const Window* w, int* min_w, int* min_h)
{
    int mw = 120;
    int mh = 80;
    if (w) {
        switch (w->app_type) {
        case APP_CALC:       mw = CALC_MIN_W; mh = CALC_MIN_H; break;
        case APP_EDITOR:     mw = EDITOR_MIN_W; mh = EDITOR_MIN_H; break;
        case APP_SPY:        mw = SPY_MIN_W; mh = SPY_MIN_H; break;
        case APP_ACCESS:     mw = ACCESS_MIN_W; mh = ACCESS_MIN_H; break;
        case APP_PUMP:       mw = PUMP_MIN_W; mh = PUMP_MIN_H; break;
        case APP_DEADLOCK:   mw = DEADLOCK_MIN_W; mh = DEADLOCK_MIN_H; break;
        case APP_SECTION:    mw = SECTION_MIN_W; mh = SECTION_MIN_H; break;
        case APP_BUS_PRODUCER:
        case APP_BUS_CONSUMER: mw = SHARED_BUS_MIN_W; mh = SHARED_BUS_MIN_H; break;
        case APP_OBJECT:     mw = OBJECT_MIN_W; mh = OBJECT_MIN_H; break;
        case APP_WAITLAB:    mw = WAITLAB_MIN_W; mh = WAITLAB_MIN_H; break;
        case APP_CLIPMENU:   mw = CLIPMENU_MIN_W; mh = CLIPMENU_MIN_H; break;
        case APP_PAINTLAB:   mw = PAINTLAB_MIN_W; mh = PAINTLAB_MIN_H; break;
        case APP_DRAGLAB:    mw = DRAGLAB_MIN_W; mh = DRAGLAB_MIN_H; break;
        case APP_CONTROLLAB: mw = CONTROLLAB_MIN_W; mh = CONTROLLAB_MIN_H; break;
        case APP_SERVICELAB: mw = SERVICELAB_MIN_W; mh = SERVICELAB_MIN_H; break;
        case APP_DIALOGLAB:  mw = DIALOGLAB_MIN_W; mh = DIALOGLAB_MIN_H; break;
        case APP_MDILAB:     mw = MDILAB_MIN_W; mh = MDILAB_MIN_H; break;
        default: break;
        }
    }
    if (mw < 120) mw = 120;
    if (mh < 80) mh = 80;
    if (min_w) *min_w = mw;
    if (min_h) *min_h = mh;
}

static const Capability* wm_window_sender_cap(Window* w)
{
    if (!w) return NULL;
    return (w->app_type == APP_TERMINAL) ? (w->term ? &w->term->cap : NULL) : &w->app_cap;
}

static void wm_send_window_message(WindowManager* wm, Window* w, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!wm || !wm->mgr || !w || !hwnd) return;
    const Capability* cap = wm_window_sender_cap(w);
    if (!cap) return;
    hwnd_send(wm->mgr, cap, hwnd, msg, wp, lp);
}

static void wm_init_minmax_for_window(WindowManager* wm, const Window* w, MINMAXINFO* mmi)
{
    (void)wm;
    if (!mmi) return;
    memset(mmi, 0, sizeof(*mmi));
    int min_w = 120, min_h = 80;
    wm_min_size_for_window(w, &min_w, &min_h);
    mmi->ptMinTrackSize.x = min_w;
    mmi->ptMinTrackSize.y = min_h;
    mmi->ptMaxTrackSize.x = 32767;
    mmi->ptMaxTrackSize.y = 32767;
}

static DragMode wm_drag_mode_from_hit(LRESULT ht)
{
    switch (ht) {
    case HTLEFT:        return DRAG_RESIZE_L;
    case HTRIGHT:       return DRAG_RESIZE_R;
    case HTTOP:         return DRAG_RESIZE_T;
    case HTBOTTOM:      return DRAG_RESIZE_B;
    case HTTOPLEFT:     return DRAG_RESIZE_TL;
    case HTTOPRIGHT:    return DRAG_RESIZE_TR;
    case HTBOTTOMLEFT:  return DRAG_RESIZE_BL;
    case HTBOTTOMRIGHT: return DRAG_RESIZE_RB;
    default:            return DRAG_NONE;
    }
}

static void wm_begin_nc_move(WindowManager* wm, int idx, int x, int y)
{
    if (!valid_window_index(wm, idx)) return;
    Window* w = &wm->wins[idx];
    wm->drag_mode = DRAG_MOVE;
    wm->drag_idx = idx;
    wm->drag_ox = x - w->x;
    wm->drag_oy = y - w->y;
    wm->drag_orig_x = w->x;
    wm->drag_orig_y = w->y;
    wm->drag_orig_w = w->w;
    wm->drag_orig_h = w->h;
    SetCapture(window_hwnd(w));
    wm_publish_window_state_event(wm, idx, WM_ENTERSIZEMOVE);
}

static void wm_begin_nc_size(WindowManager* wm, int idx, LRESULT ht, int x, int y)
{
    if (!valid_window_index(wm, idx)) return;
    Window* w = &wm->wins[idx];
    DragMode mode = wm_drag_mode_from_hit(ht);
    if (mode == DRAG_NONE) return;

    wm->drag_mode = mode;
    wm->drag_idx = idx;
    wm->drag_ox = x;
    wm->drag_oy = y;
    wm->drag_orig_x = w->x;
    wm->drag_orig_y = w->y;
    wm->drag_orig_w = w->w;
    wm->drag_orig_h = w->h;
    SetCapture(window_hwnd(w));
    wm_publish_window_state_event(wm, idx, WM_ENTERSIZEMOVE);
}

static void wm_store_restore_rect(Window* w)
{
    if (!w) return;
    if (!w->maximized) {
        w->restore_x = w->x;
        w->restore_y = w->y;
        w->restore_w = w->w;
        w->restore_h = w->h;
    }
}

static void wm_restore_window(WindowManager* wm, int idx)
{
    if (!valid_window_index(wm, idx)) return;
    Window* w = &wm->wins[idx];
    int rx = w->restore_w > 0 ? w->restore_x : w->x;
    int ry = w->restore_h > 0 ? w->restore_y : w->y;
    int rw = w->restore_w > 0 ? w->restore_w : w->w;
    int rh = w->restore_h > 0 ? w->restore_h : w->h;
    w->maximized = 0;
    w->minimized = 0;
    wm_set_window_pos_ex(wm, idx, HWND_TOP, rx, ry, rw, rh, SWP_SHOWWINDOW);
}

static void wm_maximize_window(WindowManager* wm, int idx)
{
    if (!valid_window_index(wm, idx)) return;
    Window* w = &wm->wins[idx];
    wm_store_restore_rect(w);
    int sw = wm->screen_w > 0 ? wm->screen_w : 640;
    int sh = wm->screen_h > 0 ? wm->screen_h : 480;
    int work_h = sh - TASKBAR_H;
    if (work_h < TITLEBAR_H + 60) work_h = sh;
    w->maximized = 1;
    w->minimized = 0;
    wm_set_window_pos_ex(wm, idx, HWND_TOP, 0, 0, sw, work_h, SWP_SHOWWINDOW);
}

LRESULT wm_def_window_proc(WindowManager* wm, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!wm || !hwnd) return 0;
    int idx = wm_find_hwnd(wm, hwnd);
    if (idx < 0) return 0;
    Window* w = &wm->wins[idx];
    int sx = GET_X_LPARAM(lp);
    int sy = GET_Y_LPARAM(lp);

    switch (msg) {
    case WM_NCHITTEST:
        return wm_frame_hit_test(w, sx, sy);

    case WM_NCLBUTTONDOWN: {
        LRESULT ht = (LRESULT)wp;
        wm_publish_window_state_event(wm, idx, WM_NCLBUTTONDOWN);
        switch (ht) {
        case HTCLOSE:
            return wm_def_window_proc(wm, hwnd, WM_SYSCOMMAND, SC_CLOSE, lp);
        case HTMINBUTTON:
            return wm_def_window_proc(wm, hwnd, WM_SYSCOMMAND, SC_MINIMIZE, lp);
        case HTMAXBUTTON:
            return wm_def_window_proc(wm, hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, lp);
        case HTSYSMENU:
            wm_open_system_menu(wm, idx, sx, sy);
            return 0;
        case HTCAPTION:
            wm_begin_nc_move(wm, idx, sx, sy);
            return 0;
        case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
        case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
            wm_begin_nc_size(wm, idx, ht, sx, sy);
            return 0;
        default:
            return 0;
        }
    }

    case WM_NCLBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        wm_mouse_up(wm);
        wm_publish_window_state_event(wm, idx, WM_NCLBUTTONUP);
        return 0;

    case WM_NCRBUTTONDOWN:
        wm_publish_window_state_event(wm, idx, WM_NCRBUTTONDOWN);
        if ((LRESULT)wp != HTCLIENT) wm_open_system_menu(wm, idx, sx, sy);
        return 0;

    case WM_SYSCOMMAND: {
        UINT cmd = ((UINT)wp) & 0xFFF0u;
        wm_publish_window_state_event(wm, idx, WM_SYSCOMMAND);
        switch (cmd) {
        case SC_CLOSE:
            wm_close_window(wm, idx);
            return 0;
        case SC_MINIMIZE:
            wm_store_restore_rect(w);
            wm_show_window(wm, idx, SW_MINIMIZE);
            return 0;
        case SC_RESTORE:
            wm_restore_window(wm, idx);
            return 0;
        case SC_MAXIMIZE:
            wm_maximize_window(wm, idx);
            return 0;
        case SC_KEYMENU:
            wm_open_system_menu(wm, idx, w->x + 8, w->y + TITLEBAR_H);
            return 0;
        case SC_MOVE:
            wm_begin_nc_move(wm, idx, sx, sy);
            return 0;
        case SC_SIZE: {
            LRESULT ht = (LRESULT)(((UINT)wp) & 0x000Fu);
            if (ht < HTLEFT || ht > HTBOTTOMRIGHT) ht = HTRIGHT;
            wm_begin_nc_size(wm, idx, ht, sx, sy);
            return 0;
        }
        default:
            return 0;
        }
    }

    default:
        return 0;
    }
}

static int top_window_at(WindowManager* wm, int x, int y)
{
    if (wm->focused >= 0 && !wm->wins[wm->focused].closed
        && !wm->wins[wm->focused].minimized) {
        Window* w = &wm->wins[wm->focused];
        if (x>=w->x && x<w->x+w->w && y>=w->y && y<w->y+w->h)
            return wm->focused;
    }
    for (int i = wm->count-1; i >= 0; i--) {
        if (i==wm->focused || wm->wins[i].closed || wm->wins[i].minimized)
            continue;
        Window* w = &wm->wins[i];
        if (x>=w->x && x<w->x+w->w && y>=w->y && y<w->y+w->h)
            return i;
    }
    return -1;
}


// ── Desktop als echter Filesystem-Ort ─────────

static void desktop_make_default_path(WindowManager* wm)
{
    if (!wm) return;

    // v11: der Desktop ist erstmal bewusst das Build-/Startverzeichnis.
    // Dadurch sieht man .c/.h/Makefile direkt als echte Desktop-Dateien.
    // Wenn getcwd() schiefgeht, fallen wir auf den alten echten Desktop-Ort zurück.
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd)) && cwd[0]) {
        snprintf(wm->desktop_path, sizeof(wm->desktop_path), "%.*s", 511, cwd);
        return;
    }

    const char* home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(wm->desktop_path, sizeof(wm->desktop_path), "%s/myos_desktop", home);
}

static void desktop_make_layout_path(WindowManager* wm)
{
    if (!wm || !wm->desktop_path[0]) return;
    snprintf(wm->desktop_layout_path, sizeof(wm->desktop_layout_path),
             "%.*s/.myos_layout", 498, wm->desktop_path);
}

static int desktop_ensure_dir(WindowManager* wm)
{
    if (!wm || !wm->desktop_path[0]) return 0;
    if (mkdir(wm->desktop_path, 0755) == 0) return 1;
    if (errno == EEXIST) return 1;
    return 0;
}

static long long desktop_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void desktop_clamp_icon(WindowManager* wm, DesktopIcon* ic)
{
    if (!wm || !ic) return;
    int sw = wm->screen_w > 0 ? wm->screen_w : 640;
    int sh = wm->screen_h > 0 ? wm->screen_h : 480;
    int max_x = sw - ic->w - 4;
    int max_y = sh - TASKBAR_H - ic->h - 4;
    if (max_x < 4) max_x = 4;
    if (max_y < 32) max_y = 32;
    ic->x = clamp_int(ic->x, 4, max_x);
    ic->y = clamp_int(ic->y, 32, max_y);
}

// v16.7: Desktop-Grid wie Windows-artiges Auto-Arrange:
// erst vertikal fuellen, dann naechste Spalte. Wichtig: wm_init laeuft
// kurz mit 640x480-Fallback; nach fb_init wird nochmal neu geladen.
// Dadurch nutzt der Desktop die echte Framebuffer-Breite statt nur ~5 Spalten.
static void desktop_grid_metrics(WindowManager* wm, int* ox, int* oy, int* cw, int* ch, int* rows, int* cols)
{
    int sw = (wm && wm->screen_w > 0) ? wm->screen_w : 1280;
    int sh = (wm && wm->screen_h > 0) ? wm->screen_h : 720;

    *ox = 20;
    *oy = 48;

    // Labels brauchen in der Praxis mehr Abstand als nur die Dokumentgrafik.
    // 124x112 verhindert die alte Kollision von zweizeiligen Namen.
    *cw = 124;
    *ch = 112;

    int usable_w = sw - *ox - 20;
    int usable_h = sh - TASKBAR_H - *oy - 12;
    if (usable_w < *cw) usable_w = *cw;
    if (usable_h < *ch) usable_h = *ch;

    *cols = usable_w / *cw;
    *rows = usable_h / *ch;
    if (*cols < 1) *cols = 1;
    if (*rows < 1) *rows = 1;
}

static void desktop_snap_icon(WindowManager* wm, DesktopIcon* ic)
{
    if (!wm || !ic) return;
    int ox, oy, cw, ch, rows, cols;
    desktop_grid_metrics(wm, &ox, &oy, &cw, &ch, &rows, &cols);

    int col = (ic->x - ox + cw / 2) / cw;
    int row = (ic->y - oy + ch / 2) / ch;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (row >= rows) row = rows - 1;
    if (col >= cols) col = cols - 1;

    ic->x = ox + col * cw;
    ic->y = oy + row * ch;
    desktop_clamp_icon(wm, ic);
}

static void desktop_auto_arrange_icons(WindowManager* wm)
{
    if (!wm) return;

    int ox, oy, cw, ch, rows, cols;
    desktop_grid_metrics(wm, &ox, &oy, &cw, &ch, &rows, &cols);

    for (int i = 0; i < wm->desktop_icon_count; i++) {
        DesktopIcon* ic = &wm->desktop_icons[i];
        ic->w = 96;
        ic->h = 88;

        // Windows-artig: Spalte vollmachen, dann nach rechts weiter.
        int col = i / rows;
        int row = i % rows;

        // Falls absurd viele Icons da sind, nicht alle am unteren Rand stacken,
        // sondern nach der sichtbaren Breite wieder row-major weiterlaufen.
        if (col >= cols) {
            int overflow = i - (cols * rows);
            col = overflow % cols;
            row = rows - 1;
        }

        ic->x = ox + col * cw;
        ic->y = oy + row * ch;
        desktop_clamp_icon(wm, ic);
    }
}

static int desktop_load_saved_pos(WindowManager* wm, const char* name, int* x, int* y)
{
    if (!wm || !name || !x || !y || !wm->desktop_layout_path[0]) return 0;
    FILE* f = fopen(wm->desktop_layout_path, "r");
    if (!f) return 0;

    char line[768];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char saved_name[256];
        int sx = 0, sy = 0;
        if (sscanf(line, "%255[^\t]\t%d\t%d", saved_name, &sx, &sy) == 3) {
            if (strcmp(saved_name, name) == 0) {
                *x = sx; *y = sy; found = 1; break;
            }
        }
    }
    fclose(f);
    return found;
}

static void desktop_save_layout(WindowManager* wm)
{
    if (!wm || !wm->desktop_layout_path[0]) return;
    FILE* f = fopen(wm->desktop_layout_path, "w");
    if (!f) return;
    for (int i = 0; i < wm->desktop_icon_count; i++) {
        DesktopIcon* ic = &wm->desktop_icons[i];
        fprintf(f, "%s\t%d\t%d\n", ic->name, ic->x, ic->y);
    }
    fclose(f);
}

void wm_desktop_toggle_layout(WindowManager* wm)
{
    if (!wm) return;
    wm->desktop_layout_mode = (wm->desktop_layout_mode == DESKTOP_LAYOUT_GRID)
        ? DESKTOP_LAYOUT_FREE
        : DESKTOP_LAYOUT_GRID;
    if (wm->desktop_layout_mode == DESKTOP_LAYOUT_GRID) {
        desktop_auto_arrange_icons(wm);
        desktop_save_layout(wm);
    }
    printf("[DESKTOP] Icon-Modus: %s\n", wm_desktop_layout_mode_name(wm));
}

const char* wm_desktop_layout_mode_name(WindowManager* wm)
{
    if (!wm) return "?";
    return wm->desktop_layout_mode == DESKTOP_LAYOUT_GRID ? "GRID" : "FREE";
}

static int str_eq_ci(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int str_ends_ci(const char* s, const char* suffix)
{
    if (!s || !suffix) return 0;
    size_t ns = strlen(s), nx = strlen(suffix);
    if (nx > ns) return 0;
    return str_eq_ci(s + ns - nx, suffix);
}

static int desktop_is_editable_file(const char* name)
{
    if (!name || !name[0]) return 0;
    if (name[0] == '.') return 0;

    // Das Desktop-Root ist jetzt das Build-Verzeichnis. Damit es nicht mit
    // .o-Dateien/Binaries zugeschneit wird, zeigen wir erstmal bewusst nur
    // menschenlesbare Projekt-/Textdateien und einfache Wallpaper-Formate an.
    if (str_eq_ci(name, "Makefile")) return 1;
    if (str_ends_ci(name, ".c")) return 1;
    if (str_ends_ci(name, ".h")) return 1;
    if (str_ends_ci(name, ".txt")) return 1;
    if (str_ends_ci(name, ".md")) return 1;
    if (str_ends_ci(name, ".ini")) return 1;
    if (str_ends_ci(name, ".cfg")) return 1;
    if (str_ends_ci(name, ".log")) return 1;
    if (str_ends_ci(name, ".ppm")) return 1;
    if (str_ends_ci(name, ".pnm")) return 1;
    if (str_ends_ci(name, ".bmp")) return 1;
    return 0;
}

static int desktop_is_wallpaper_file(const char* name)
{
    if (!name) return 0;
    if (str_ends_ci(name, ".ppm")) return 1;
    if (str_ends_ci(name, ".pnm")) return 1;
    if (str_ends_ci(name, ".bmp")) return 1;
    return 0;
}

static void shorten_middle(const char* in, char* out, int out_len)
{
    if (!out || out_len <= 0) return;
    out[0] = 0;
    if (!in) return;

    int n = (int)strlen(in);
    if (n < out_len) {
        strncpy(out, in, (size_t)out_len - 1);
        out[out_len - 1] = 0;
        return;
    }
    if (out_len < 8) {
        strncpy(out, in, (size_t)out_len - 1);
        out[out_len - 1] = 0;
        return;
    }

    int cap = out_len - 1;
    int left = (cap - 3) / 2;
    int right = cap - 3 - left;
    if (right < 1) right = 1;

    memcpy(out, in, (size_t)left);
    memcpy(out + left, "...", 3);
    memcpy(out + left + 3, in + n - right, (size_t)right);
    out[cap] = 0;
}

static void desktop_make_label_lines(const char* name, char* line1, int line1_len, char* line2, int line2_len)
{
    if (line1 && line1_len > 0) line1[0] = 0;
    if (line2 && line2_len > 0) line2[0] = 0;
    if (!name) return;

    // Max. 12 Zeichen pro Zeile bei 8px-Font passt gut unter 96px Icons.
    // Erweiterung bleibt sichtbar, damit .c/.h/.txt unterscheidbar bleiben.
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", name);

    char* dot = strrchr(tmp, '.');
    if (dot && dot != tmp && strlen(dot) <= 6) {
        *dot = 0;
        char base[32];
        shorten_middle(tmp, base, sizeof(base));
        char ext[8];
        snprintf(ext, sizeof(ext), "%s", dot + 1);

        shorten_middle(base, line1, line1_len);
        if (line2 && line2_len > 0) {
            line2[0] = '.';
            snprintf(line2 + 1, (size_t)line2_len - 1, "%s", ext);
        }
        return;
    }

    shorten_middle(name, line1, line1_len);
}

void wm_desktop_reload(WindowManager* wm)
{
    if (!wm) return;
    wm->desktop_icon_count = 0;
    desktop_ensure_dir(wm);

    DIR* dir = opendir(wm->desktop_path);
    if (!dir) return;

    struct dirent* de;
    int idx = 0;
    while ((de = readdir(dir)) != NULL && idx < MAX_DESKTOP_ICONS) {
        if (de->d_name[0] == '.') continue;
        if (!desktop_is_editable_file(de->d_name)) continue;

        DesktopIcon* ic = &wm->desktop_icons[idx];
        memset(ic, 0, sizeof(*ic));
        snprintf(ic->name, sizeof(ic->name), "%s", de->d_name);

        char full[768];
        snprintf(full, sizeof(full), "%s/%s", wm->desktop_path, de->d_name);
        snprintf(ic->path, sizeof(ic->path), "%.511s", full);
        struct stat st;
        if (stat(full, &st) == 0)
            ic->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;

        int col = idx % 6;
        int row = idx / 6;
        ic->x = 20 + col * 108;
        ic->y = 48 + row * 92;
        ic->w = 96;
        ic->h = 82;

        if (wm->desktop_layout_mode == DESKTOP_LAYOUT_FREE) {
            int sx = ic->x, sy = ic->y;
            if (desktop_load_saved_pos(wm, ic->name, &sx, &sy)) {
                ic->x = sx;
                ic->y = sy;
            }
            desktop_clamp_icon(wm, ic);
        }

        idx++;
    }
    closedir(dir);
    wm->desktop_icon_count = idx;

    // v16.6: Im GRID-Modus wird beim Start/Reload stumpf sauber neu angeordnet.
    // Alte Layout-Dateien oder kleinere Zellbreiten koennen so keine Icons mehr
    // uebereinander stapeln. FREE bleibt bewusst frei und nutzt gespeicherte Positionen.
    if (wm->desktop_layout_mode == DESKTOP_LAYOUT_GRID) {
        desktop_auto_arrange_icons(wm);
        desktop_save_layout(wm);
    }
}

int wm_desktop_create_text_file(WindowManager* wm)
{
    if (!wm) return 0;
    desktop_ensure_dir(wm);

    char full[768];
    char name[128];
    for (int i = 0; i < 999; i++) {
        if (i == 0)
            snprintf(name, sizeof(name), "Neue Textdatei.txt");
        else
            snprintf(name, sizeof(name), "Neue Textdatei (%d).txt", i + 1);

        snprintf(full, sizeof(full), "%s/%s", wm->desktop_path, name);
        FILE* test = fopen(full, "r");
        if (test) { fclose(test); continue; }

        FILE* f = fopen(full, "w");
        if (!f) return 0;
        fprintf(f, "myos text file\n\nDieser Desktop liegt wirklich im Filesystem:\n%s\n", wm->desktop_path);
        fclose(f);
        wm_desktop_reload(wm);
        desktop_save_layout(wm);
        printf("[DESKTOP] erstellt: %s\n", full);
        return 1;
    }
    return 0;
}

static void draw_desktop_icons(WindowManager* wm, Framebuffer* fb)
{
    if (!wm || !fb) return;
    for (int i = 0; i < wm->desktop_icon_count; i++) {
        DesktopIcon* ic = &wm->desktop_icons[i];
        int x = ic->x;
        int y = ic->y;

        if (i == wm->desktop_selected) {
            fb_rect(fb, x - 3, y - 3, ic->w + 6, ic->h + 6, COLOR(35,55,90));
            fb_rect_outline(fb, x - 3, y - 3, ic->w + 6, ic->h + 6, COLOR(120,170,240));
        }

        // Icon-Kachel / Dokument-Symbol mit kleinem Dateityp-Marker.
        Color paper = COLOR(230,230,235);
        Color marker = COLOR(70,70,120);
        const char* tag = "TXT";
        if (str_ends_ci(ic->name, ".c"))   { marker = COLOR(45,80,150);  tag = "C"; }
        if (str_ends_ci(ic->name, ".h"))   { marker = COLOR(45,125,95);  tag = "H"; }
        if (str_ends_ci(ic->name, ".bmp")) { marker = COLOR(120,70,140); tag = "BMP"; }
        if (str_ends_ci(ic->name, ".ppm") || str_ends_ci(ic->name, ".pnm")) { marker = COLOR(120,70,140); tag = "IMG"; }
        if (str_eq_ci(ic->name, "Makefile")){ marker = COLOR(130,85,45); tag = "MK"; }

        fb_rect(fb, x + 31, y + 4, 34, 42, ic->is_dir ? COLOR(80,70,35) : paper);
        fb_rect_outline(fb, x + 31, y + 4, 34, 42, ic->is_dir ? COLOR(180,150,60) : COLOR(70,70,90));
        if (!ic->is_dir) {
            fb_rect(fb, x + 37, y + 14, 22, 1, COLOR(90,90,110));
            fb_rect(fb, x + 37, y + 22, 22, 1, COLOR(90,90,110));
            fb_rect(fb, x + 37, y + 30, 16, 1, COLOR(90,90,110));
            fb_rect(fb, x + 31, y + 36, 34, 10, marker);
            font_draw_str(fb, x + 36, y + 37, tag, WHITE);
        } else {
            fb_rect(fb, x + 35, y + 12, 22, 10, COLOR(120,95,35));
        }

        char l1[14], l2[14];
        desktop_make_label_lines(ic->name, l1, sizeof(l1), l2, sizeof(l2));
        fb_rect(fb, x, y + 52, ic->w, 26, COLOR(15,15,22));
        int l1x = x + (ic->w - (int)strlen(l1) * 8) / 2;
        if (l1x < x + 2) l1x = x + 2;
        font_draw_str(fb, l1x, y + 55, l1, WHITE);
        if (l2[0]) {
            int l2x = x + (ic->w - (int)strlen(l2) * 8) / 2;
            if (l2x < x + 2) l2x = x + 2;
            font_draw_str(fb, l2x, y + 67, l2, COLOR(210,220,245));
        }
    }
}


static int desktop_icon_at(WindowManager* wm, int x, int y)
{
    if (!wm) return -1;
    for (int i = wm->desktop_icon_count - 1; i >= 0; i--) {
        DesktopIcon* ic = &wm->desktop_icons[i];
        if (x >= ic->x && x < ic->x + ic->w && y >= ic->y && y < ic->y + ic->h)
            return i;
    }
    return -1;
}

// ── Hintergrund ──────────────────────────────

static void draw_background(WindowManager* wm, Framebuffer* fb)
{
    static const Color bg_colors[BG_COUNT] = {
        [BG_DARK]   = COLOR(25,  25,  25 ),
        [BG_BLUE]   = COLOR(10,  20,  60 ),
        [BG_PURPLE] = COLOR(30,  10,  50 ),
        [BG_TEAL]   = COLOR(10,  40,  40 ),
    };
    fb_clear(fb, bg_colors[wm->bg]);

    int desktop_h = fb->height - TASKBAR_H;
    if (wm->wallpaper_enabled && wm->wallpaper.pixels && wm->wallpaper.width > 0 && wm->wallpaper.height > 0) {
        image_draw_scaled(fb, &wm->wallpaper, 0, 0, fb->width, desktop_h);
        return;
    }

    // Subtiles Gittermuster
    Color grid = COLOR(
        ((bg_colors[wm->bg] >> 16) & 0xff) + 8,
        ((bg_colors[wm->bg] >>  8) & 0xff) + 8,
        ( bg_colors[wm->bg]        & 0xff) + 8
    );
    for (int y = 0; y < desktop_h; y += 40)
        fb_rect(fb, 0, y, fb->width, 1, grid);
    for (int x = 0; x < fb->width; x += 40)
        fb_rect(fb, x, 0, 1, desktop_h, grid);
}

// ── Rechtsklick-Menü ─────────────────────────

#define MENU_W       180
#define MENU_ITEM_H   22

/* v76: Start menu command IDs.
   The desktop menu is no longer a hard-coded iy==N action blob inside
   the old central mouse handler.  v77 routes shell input into #32769 /
   Shell_TrayWnd WndProcs; command dispatcher handles items through the same WM_COMMAND style path that
   buttons, accelerators, and real Win32 menus use. */
enum {
    ID_START_NEW_TERMINAL    = 1001,
    ID_START_CALC            = 1002,
    ID_START_NEW_TEXTFILE    = 1003,
    ID_START_EDITOR          = 1004,
    ID_START_SPY             = 1005,
    ID_START_ACCESSLAB       = 1006,
    ID_START_PUMPLAB         = 1007,
    ID_START_DEADLOCKLAB     = 1008,
    ID_START_SECTIONLAB      = 1009,
    ID_START_STATEBUSLAB     = 1010,
    ID_START_HWNDSTATEPROBE  = 1011,
    ID_START_SURFACELAB      = 1012,
    ID_START_OBJECTLAB       = 1013,
    ID_START_WAITLAB         = 1014,
    ID_START_CLIPMENULAB     = 1015,
    ID_START_PAINTLAB        = 1016,
    ID_START_DRAGLAB         = 1017,
    ID_START_CONTROLLAB      = 1018,
    ID_START_SERVICELAB      = 1019,
    ID_START_DIALOGLAB       = 1020,
    ID_START_MDILAB          = 1021,
    ID_START_DESKTOP_RELOAD  = 1022,
    ID_START_TOGGLE_LAYOUT   = 1023,
    ID_START_CLEAR_WALLPAPER = 1024,
    ID_START_BG_DARK         = 1025,
    ID_START_BG_BLUE         = 1026,
    ID_START_BG_PURPLE       = 1027,
    ID_START_BG_TEAL         = 1028,
    ID_START_CLOSE_ALL       = 1029,
};

typedef struct StartMenuItem {
    UINT id;
    const char* text;
    UINT flags;
} StartMenuItem;

#define START_MENU_SEPARATOR 0x0800u
static const StartMenuItem g_StartMenuItems[] = {
    { ID_START_NEW_TERMINAL,    "Neues Terminal",         0 },
    { ID_START_CALC,            "Neuer Rechner",          0 },
    { ID_START_NEW_TEXTFILE,    "Neue Textdatei",         0 },
    { ID_START_EDITOR,          "Neuer Texteditor",       0 },
    { ID_START_SPY,             "Window Spy++",           0 },
    { ID_START_ACCESSLAB,       "AccessLab",              0 },
    { ID_START_PUMPLAB,         "PumpLab",                0 },
    { ID_START_DEADLOCKLAB,     "DeadlockLab",            0 },
    { ID_START_SECTIONLAB,      "SectionLab",             0 },
    { ID_START_STATEBUSLAB,     "StateBusLab",            0 },
    { ID_START_HWNDSTATEPROBE,  "HWND StateProbe",        0 },
    { ID_START_SURFACELAB,      "SurfaceLab",             0 },
    { ID_START_OBJECTLAB,       "ObjectLab",              0 },
    { ID_START_WAITLAB,         "WaitLab",                0 },
    { ID_START_CLIPMENULAB,     "ClipMenuLab",            0 },
    { ID_START_PAINTLAB,        "PaintLab",               0 },
    { ID_START_DRAGLAB,         "DragLab",                0 },
    { ID_START_CONTROLLAB,      "ControlLab",             0 },
    { ID_START_SERVICELAB,      "ServiceLab",             0 },
    { ID_START_DIALOGLAB,       "DialogLab",              0 },
    { ID_START_MDILAB,          "MDILab",                 0 },
    { ID_START_DESKTOP_RELOAD,  "Desktop neu laden",      0 },
    { ID_START_TOGGLE_LAYOUT,   "Iconmodus: Free/Grid",   0 },
    { ID_START_CLEAR_WALLPAPER, "Wallpaper loeschen",     0 },
    { 0,                        "---",                    START_MENU_SEPARATOR },
    { ID_START_BG_DARK,         "Hintergrund: Dunkel",    0 },
    { ID_START_BG_BLUE,         "Hintergrund: Blau",      0 },
    { ID_START_BG_PURPLE,       "Hintergrund: Lila",      0 },
    { ID_START_BG_TEAL,         "Hintergrund: Teal",      0 },
    { 0,                        "---",                    START_MENU_SEPARATOR },
    { ID_START_CLOSE_ALL,       "Beenden",                0 },
};
#define MENU_COUNT ((int)(sizeof(g_StartMenuItems)/sizeof(g_StartMenuItems[0])))

/* v83: per-window system menu IDs are the real SC_* values.  Menu click ->
   WM_SYSCOMMAND, never direct close/minimize. */
typedef struct SystemMenuItem {
    UINT id;
    const char* text;
    UINT flags;
} SystemMenuItem;

#define SYSTEM_MENU_SEPARATOR 0x0800u
static const SystemMenuItem g_SystemMenuItems[] = {
    { SC_RESTORE,   "Restore",  0 },
    { SC_MOVE,      "Move",     0 },
    { SC_SIZE,      "Size",     0 },
    { SC_MINIMIZE,  "Minimize", 0 },
    { SC_MAXIMIZE,  "Maximize", 0 },
    { 0,            "---",      SYSTEM_MENU_SEPARATOR },
    { SC_CLOSE,     "Close",    0 },
};
#define SYSTEM_MENU_COUNT ((int)(sizeof(g_SystemMenuItems)/sizeof(g_SystemMenuItems[0])))



static int wm_menu_count(WindowManager* wm)
{
    if (!wm) return 0;
    return wm->menu_kind == MENU_KIND_SYSTEM ? SYSTEM_MENU_COUNT : MENU_COUNT;
}

static int wm_menu_item_is_separator(WindowManager* wm, int index)
{
    if (!wm) return 1;
    if (wm->menu_kind == MENU_KIND_SYSTEM) {
        if (index < 0 || index >= SYSTEM_MENU_COUNT) return 1;
        return (g_SystemMenuItems[index].flags & SYSTEM_MENU_SEPARATOR) != 0;
    }
    if (index < 0 || index >= MENU_COUNT) return 1;
    return (g_StartMenuItems[index].flags & START_MENU_SEPARATOR) != 0;
}

static UINT wm_menu_command_at_index(WindowManager* wm, int index)
{
    if (!wm || wm_menu_item_is_separator(wm, index)) return 0;
    if (wm->menu_kind == MENU_KIND_SYSTEM) return g_SystemMenuItems[index].id;
    return g_StartMenuItems[index].id;
}

static const char* wm_menu_text_at_index(WindowManager* wm, int index)
{
    if (!wm || wm_menu_item_is_separator(wm, index)) return NULL;
    if (wm->menu_kind == MENU_KIND_SYSTEM) return g_SystemMenuItems[index].text;
    return g_StartMenuItems[index].text;
}

static HWND wm_menu_owner_hwnd(WindowManager* wm)
{
    if (!wm) return 0;
    if (wm->menu_kind == MENU_KIND_SYSTEM && wm->menu_target_hwnd) return wm->menu_target_hwnd;
    return wm->hwnd_desktop;
}

static void wm_menu_notify(WindowManager* wm, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!wm || !wm->mgr) return;
    HWND owner = wm_menu_owner_hwnd(wm);
    if (!owner) return;
    hwnd_post(wm->mgr, &wm->shell_cap, owner, msg, wp, lp);
}

static int wm_menu_first_selectable(WindowManager* wm)
{
    int count = wm_menu_count(wm);
    for (int i = 0; i < count; ++i)
        if (!wm_menu_item_is_separator(wm, i)) return i;
    return -1;
}

static int wm_menu_next_selectable(WindowManager* wm, int start, int dir)
{
    int count = wm_menu_count(wm);
    if (count <= 0) return -1;
    int i = start;
    if (i < 0 || i >= count) i = (dir >= 0) ? -1 : count;
    for (int step = 0; step < count; ++step) {
        i += (dir >= 0) ? 1 : -1;
        if (i < 0) i = count - 1;
        if (i >= count) i = 0;
        if (!wm_menu_item_is_separator(wm, i)) return i;
    }
    return -1;
}

static void wm_menu_select_index(WindowManager* wm, int index)
{
    if (!wm || !wm->menu_open) return;
    if (index < 0 || index >= wm_menu_count(wm)) index = wm_menu_first_selectable(wm);
    if (index >= 0 && wm_menu_item_is_separator(wm, index)) index = wm_menu_next_selectable(wm, index, 1);
    if (index < 0) return;
    if (wm->menu_selected == index) return;
    wm->menu_selected = index;
    UINT cmd = wm_menu_command_at_index(wm, index);
    wm_menu_notify(wm, WM_MENUSELECT, MAKEWPARAM((WORD)cmd, 0), (LPARAM)(uintptr_t)wm->menu_kind);
}

static void wm_menu_enter_loop(WindowManager* wm)
{
    if (!wm || !wm->menu_open) return;
    wm->menu_loop_serial++;
    wm->menu_key_count = 0;
    wm->menu_selected = wm_menu_first_selectable(wm);
    wm_menu_notify(wm, WM_ENTERMENULOOP, 0, 0);
    wm_menu_notify(wm, WM_INITMENU, 0, 0);
    wm_menu_notify(wm, WM_INITMENUPOPUP, 0, MAKELPARAM((WORD)0, (WORD)wm->menu_kind));
    if (wm->menu_selected >= 0) {
        UINT cmd = wm_menu_command_at_index(wm, wm->menu_selected);
        wm_menu_notify(wm, WM_MENUSELECT, MAKEWPARAM((WORD)cmd, 0), (LPARAM)(uintptr_t)wm->menu_kind);
    }
}


static void wm_close_menu_loop(WindowManager* wm, int canceled);
static int wm_menu_key_to_hotletter(int keycode);
static void wm_position_menu(WindowManager* wm, int* x, int* y, int w, int h, int avoid_taskbar);

// ── v101: HMENU-backed application menu bar / popup navigation ─────────
#define APP_MENU_POPUP_ITEM_H 22
#define APP_MENU_MIN_W        150
#define APP_MENU_MAX_W        260
#define APP_MENU_CHECK_W       18
#define APP_MENU_ARROW_W       16

static int wm_app_menu_get_item(HMENU hMenu, int pos, MENUITEMINFOA* out, char* text, UINT cch)
{
    if (out) memset(out, 0, sizeof(*out));
    if (text && cch) text[0] = 0;
    if (!hMenu || pos < 0 || !out) return 0;
    out->cbSize = sizeof(*out);
    out->fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING | MIIM_DATA;
    out->dwTypeData = text;
    out->cch = cch ? cch : 0;
    return GetMenuItemInfoA(hMenu, (UINT)pos, TRUE, out) ? 1 : 0;
}

static void wm_menu_copy_visible_text(const char* src, char* dst, size_t cap, int keep_tab)
{
    if (!dst || cap == 0) return;
    size_t n = 0;
    if (!src) src = "";
    for (const char* p = src; *p && n + 1 < cap; ++p) {
        if (*p == '&') {
            if (p[1] == '&' && n + 1 < cap) { dst[n++] = '&'; ++p; }
            continue;
        }
        if (*p == '\t' && !keep_tab) break;
        dst[n++] = *p;
    }
    dst[n] = 0;
}

static int wm_menu_find_mnemonic_char(const char* text)
{
    if (!text) return 0;
    for (const char* p = text; *p; ++p) {
        if (*p == '&' && p[1]) {
            if (p[1] == '&') { ++p; continue; }
            return tolower((unsigned char)p[1]);
        }
    }
    return 0;
}

static int wm_app_menu_item_is_separator(const MENUITEMINFOA* mi)
{
    return mi && (mi->fType & MFT_SEPARATOR);
}

static int wm_app_menu_item_is_disabled(const MENUITEMINFOA* mi)
{
    return mi && (mi->fState & (MF_DISABLED | MF_GRAYED));
}

static UINT wm_app_menu_item_flags(const MENUITEMINFOA* mi)
{
    if (!mi) return 0;
    UINT f = mi->fType | mi->fState;
    if (mi->hSubMenu) f |= MF_POPUP;
    return f;
}

static int wm_app_menu_first_selectable(HMENU hMenu)
{
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOA mi; char text[96];
        if (!wm_app_menu_get_item(hMenu, i, &mi, text, sizeof(text))) continue;
        if (wm_app_menu_item_is_separator(&mi) || wm_app_menu_item_is_disabled(&mi)) continue;
        return i;
    }
    return -1;
}

static int wm_app_menu_next_selectable(HMENU hMenu, int start, int dir)
{
    int count = GetMenuItemCount(hMenu);
    if (count <= 0) return -1;
    int i = start;
    if (i < 0 || i >= count) i = (dir >= 0) ? -1 : count;
    for (int step = 0; step < count; ++step) {
        i += (dir >= 0) ? 1 : -1;
        if (i < 0) i = count - 1;
        if (i >= count) i = 0;
        MENUITEMINFOA mi; char text[96];
        if (!wm_app_menu_get_item(hMenu, i, &mi, text, sizeof(text))) continue;
        if (wm_app_menu_item_is_separator(&mi) || wm_app_menu_item_is_disabled(&mi)) continue;
        return i;
    }
    return -1;
}

static int wm_app_menu_popup_width(HMENU hMenu)
{
    int count = GetMenuItemCount(hMenu);
    int w = APP_MENU_MIN_W;
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOA mi; char raw[96], vis[96];
        if (!wm_app_menu_get_item(hMenu, i, &mi, raw, sizeof(raw))) continue;
        wm_menu_copy_visible_text(raw, vis, sizeof(vis), 1);
        int tw = (int)strlen(vis) * 8 + APP_MENU_CHECK_W + APP_MENU_ARROW_W + 24;
        if (tw > w) w = tw;
    }
    if (w > APP_MENU_MAX_W) w = APP_MENU_MAX_W;
    return w;
}

static int wm_app_menu_bar_item_rect(Window* w, int topIndex, int* rx, int* ry, int* rw, int* rh)
{
    if (!w || !w->app_hwnd) return 0;
    HMENU bar = GetMenu(w->app_hwnd);
    if (!bar) return 0;
    int count = GetMenuItemCount(bar);
    if (topIndex < 0 || topIndex >= count) return 0;
    int x = w->x + 5;
    int y = w->y + TITLEBAR_H;
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOA mi; char raw[96], vis[96];
        if (!wm_app_menu_get_item(bar, i, &mi, raw, sizeof(raw))) continue;
        wm_menu_copy_visible_text(raw, vis, sizeof(vis), 0);
        int ww = (int)strlen(vis) * 8 + 22;
        if (ww < 46) ww = 46;
        if (i == topIndex) {
            if (rx) *rx = x;
            if (ry) *ry = y;
            if (rw) *rw = ww;
            if (rh) *rh = APP_MENUBAR_H;
            return 1;
        }
        x += ww;
    }
    return 0;
}

static int wm_app_menu_bar_hit(WindowManager* wm, int x, int y, int* outWinIdx, int* outTopIndex)
{
    if (outWinIdx) *outWinIdx = -1;
    if (outTopIndex) *outTopIndex = -1;
    int idx = top_window_at(wm, x, y);
    if (!valid_window_index(wm, idx)) return 0;
    Window* w = &wm->wins[idx];
    if (!w->app_hwnd || !GetMenu(w->app_hwnd)) return 0;
    if (y < w->y + TITLEBAR_H || y >= w->y + TITLEBAR_H + APP_MENUBAR_H) return 0;
    int count = GetMenuItemCount(GetMenu(w->app_hwnd));
    for (int i = 0; i < count; ++i) {
        int rx, ry, rw, rh;
        if (wm_app_menu_bar_item_rect(w, i, &rx, &ry, &rw, &rh) && x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
            if (outWinIdx) *outWinIdx = idx;
            if (outTopIndex) *outTopIndex = i;
            return 1;
        }
    }
    return 0;
}

static void wm_app_menu_notify(WindowManager* wm, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!wm || !wm->app_menu_hwnd) return;
    SendMessageA(wm->app_menu_hwnd, msg, wp, lp);
}

static void wm_app_menu_notify_select(WindowManager* wm, int level, int pos)
{
    if (!wm || level < 0 || level >= wm->app_menu_level_count) return;
    HMENU hMenu = wm->app_menu_popup[level];
    MENUITEMINFOA mi; char text[96];
    if (!wm_app_menu_get_item(hMenu, pos, &mi, text, sizeof(text))) return;
    UINT flags = wm_app_menu_item_flags(&mi);
    UINT item = mi.hSubMenu ? (UINT)pos : mi.wID;
    wm_app_menu_notify(wm, WM_MENUSELECT, MAKEWPARAM((WORD)item, (WORD)(flags & 0xffffu)), (LPARAM)hMenu);
}

static void wm_app_menu_set_selected(WindowManager* wm, int level, int pos)
{
    if (!wm || level < 0 || level >= wm->app_menu_level_count) return;
    HMENU hMenu = wm->app_menu_popup[level];
    int count = GetMenuItemCount(hMenu);
    if (pos < 0 || pos >= count) pos = wm_app_menu_first_selectable(hMenu);
    if (pos < 0) return;
    MENUITEMINFOA mi; char text[96];
    if (!wm_app_menu_get_item(hMenu, pos, &mi, text, sizeof(text))) return;
    if (wm_app_menu_item_is_separator(&mi) || wm_app_menu_item_is_disabled(&mi)) return;
    if (wm->app_menu_sel[level] == pos) return;
    wm->app_menu_sel[level] = pos;
    wm_app_menu_notify_select(wm, level, pos);
}

static void wm_app_menu_close_deeper(WindowManager* wm, int keepLevels)
{
    if (!wm) return;
    if (keepLevels < 0) keepLevels = 0;
    if (keepLevels > wm->app_menu_level_count) keepLevels = wm->app_menu_level_count;
    for (int i = wm->app_menu_level_count - 1; i >= keepLevels; --i) {
        if (wm->app_menu_popup[i]) wm_app_menu_notify(wm, WM_UNINITMENUPOPUP, (WPARAM)wm->app_menu_popup[i], 0);
        wm->app_menu_popup[i] = 0;
        wm->app_menu_sel[i] = -1;
    }
    wm->app_menu_level_count = keepLevels;
}

static void wm_app_menu_open_level(WindowManager* wm, int level, HMENU hMenu, int x, int y, int fromTopIndex)
{
    if (!wm || !hMenu || level < 0 || level >= APP_MENU_MAX_LEVELS) return;
    wm_app_menu_close_deeper(wm, level);
    int w = wm_app_menu_popup_width(hMenu);
    int count = GetMenuItemCount(hMenu);
    int h = count * APP_MENU_POPUP_ITEM_H + 2;
    wm_position_menu(wm, &x, &y, w, h, 0);
    wm->app_menu_popup[level] = hMenu;
    wm->app_menu_sel[level] = -1;
    wm->app_menu_x[level] = x;
    wm->app_menu_y[level] = y;
    wm->app_menu_w[level] = w;
    wm->app_menu_h[level] = h;
    wm->app_menu_level_count = level + 1;
    wm_app_menu_notify(wm, WM_INITMENUPOPUP, (WPARAM)hMenu, MAKELPARAM((WORD)(fromTopIndex >= 0 ? fromTopIndex : 0), FALSE));
    wm_app_menu_set_selected(wm, level, wm_app_menu_first_selectable(hMenu));
}

static void wm_close_app_menu_loop(WindowManager* wm, int canceled)
{
    if (!wm || wm->menu_kind != MENU_KIND_APP) return;
    wm_app_menu_close_deeper(wm, 0);
    wm_app_menu_notify(wm, WM_EXITMENULOOP, canceled ? 1u : 0u, 0);
    wm->menu_open = 0;
    wm->menu_kind = MENU_KIND_START;
    wm->app_menu_hwnd = 0;
    wm->app_menu_bar = 0;
    wm->app_menu_owner_idx = -1;
    wm->app_menu_top_index = -1;
    wm->app_menu_bar_hot = -1;
    wm->menu_selected = -1;
    wm->menu_loop_serial++;
}

static void wm_open_app_menu(WindowManager* wm, int idx, int topIndex)
{
    if (!valid_window_index(wm, idx)) return;
    Window* w = &wm->wins[idx];
    HMENU bar = w->app_hwnd ? GetMenu(w->app_hwnd) : 0;
    if (!bar) return;
    MENUITEMINFOA mi; char text[96];
    if (!wm_app_menu_get_item(bar, topIndex, &mi, text, sizeof(text)) || !mi.hSubMenu) return;

    if (wm->menu_open && wm->menu_kind == MENU_KIND_APP && wm->app_menu_hwnd == w->app_hwnd) {
        if (wm->app_menu_top_index == topIndex) return;
        wm->app_menu_top_index = topIndex;
        wm->app_menu_bar_hot = topIndex;
        int rx, ry, rw, rh;
        if (!wm_app_menu_bar_item_rect(w, topIndex, &rx, &ry, &rw, &rh)) { rx = w->x + 5; ry = w->y + TITLEBAR_H; rw = 60; }
        wm_app_menu_open_level(wm, 0, mi.hSubMenu, rx, ry + APP_MENUBAR_H, topIndex);
        return;
    }
    if (wm->menu_open) {
        if (wm->menu_kind == MENU_KIND_APP) wm_close_app_menu_loop(wm, 1);
        else wm_close_menu_loop(wm, 1);
    }

    int old_focus = wm->focused;
    if (old_focus >= 0) wm->wins[old_focus].active = 0;
    wm->focused = idx;
    w->active = 1;
    if (old_focus >= 0 && old_focus != idx) wm_publish_window_state_event(wm, old_focus, WM_ACTIVATE);
    wm_publish_window_state_event(wm, idx, WM_ACTIVATE);

    wm->menu_open = 1;
    wm->menu_kind = MENU_KIND_APP;
    wm->menu_from_start = 0;
    wm->app_menu_hwnd = w->app_hwnd;
    wm->app_menu_bar = bar;
    wm->app_menu_owner_idx = idx;
    wm->app_menu_top_index = topIndex;
    wm->app_menu_bar_hot = topIndex;
    wm->menu_loop_serial++;
    wm->menu_key_count = 0;
    wm_app_menu_notify(wm, WM_ENTERMENULOOP, 0, 0);
    wm_app_menu_notify(wm, WM_INITMENU, (WPARAM)bar, 0);
    int rx, ry, rw, rh;
    if (!wm_app_menu_bar_item_rect(w, topIndex, &rx, &ry, &rw, &rh)) { rx = w->x + 5; ry = w->y + TITLEBAR_H; rw = 60; }
    wm_app_menu_open_level(wm, 0, mi.hSubMenu, rx, ry + APP_MENUBAR_H, topIndex);
}


/* v137: A top-level HMENU item may legally be a direct command, not only a
   popup owner.  v136 rendered those items but clicked them through a submenu-
   only path, so MDILab's "New child / Tile / Cascade" bar looked live while it
   never emitted WM_COMMAND.  Keep the contract explicit: a menubar click either
   opens a real submenu or sends exactly one top-level menu WM_COMMAND. */
static int wm_app_menu_click_top_item(WindowManager* wm, int idx, int topIndex)
{
    if (!valid_window_index(wm, idx)) return 0;
    Window* w = &wm->wins[idx];
    HMENU bar = w->app_hwnd ? GetMenu(w->app_hwnd) : 0;
    if (!bar) return 0;

    MENUITEMINFOA mi; char text[96];
    if (!wm_app_menu_get_item(bar, topIndex, &mi, text, sizeof(text))) return 0;
    if (wm_app_menu_item_is_separator(&mi) || wm_app_menu_item_is_disabled(&mi)) return 1;

    if (mi.hSubMenu) {
        wm_open_app_menu(wm, idx, topIndex);
        return 1;
    }

    HWND hwnd = w->app_hwnd;
    UINT id = mi.wID;

    if (wm->menu_open) {
        if (wm->menu_kind == MENU_KIND_APP) wm_close_app_menu_loop(wm, 0);
        else wm_close_menu_loop(wm, 0);
    }

    int old_focus = wm->focused;
    if (old_focus >= 0) wm->wins[old_focus].active = 0;
    wm->focused = idx;
    w->active = 1;
    if (old_focus >= 0 && old_focus != idx) wm_publish_window_state_event(wm, old_focus, WM_ACTIVATE);
    wm_publish_window_state_event(wm, idx, WM_ACTIVATE);

    wm_send_app_menu_command(wm, hwnd, id);
    return 1;
}

static int wm_app_menu_hit_popup(WindowManager* wm, int x, int y, int* outLevel, int* outPos)
{
    if (outLevel) *outLevel = -1;
    if (outPos) *outPos = -1;
    if (!wm || wm->menu_kind != MENU_KIND_APP) return 0;
    for (int level = wm->app_menu_level_count - 1; level >= 0; --level) {
        int px = wm->app_menu_x[level], py = wm->app_menu_y[level];
        int pw = wm->app_menu_w[level], ph = wm->app_menu_h[level];
        if (x >= px && x < px + pw && y >= py && y < py + ph) {
            int pos = (y - py - 1) / APP_MENU_POPUP_ITEM_H;
            if (pos < 0 || pos >= GetMenuItemCount(wm->app_menu_popup[level])) return 0;
            if (outLevel) *outLevel = level;
            if (outPos) *outPos = pos;
            return 1;
        }
    }
    return 0;
}

static void wm_app_menu_open_submenu_from_selection(WindowManager* wm, int level)
{
    if (!wm || level < 0 || level >= wm->app_menu_level_count) return;
    int pos = wm->app_menu_sel[level];
    HMENU hMenu = wm->app_menu_popup[level];
    MENUITEMINFOA mi; char text[96];
    if (!wm_app_menu_get_item(hMenu, pos, &mi, text, sizeof(text)) || !mi.hSubMenu) return;
    int x = wm->app_menu_x[level] + wm->app_menu_w[level] - 3;
    int y = wm->app_menu_y[level] + 1 + pos * APP_MENU_POPUP_ITEM_H;
    wm_app_menu_open_level(wm, level + 1, mi.hSubMenu, x, y, -1);
}

static void wm_app_menu_invoke(WindowManager* wm, int level, int pos)
{
    if (!wm || level < 0 || level >= wm->app_menu_level_count) return;
    HMENU hMenu = wm->app_menu_popup[level];
    MENUITEMINFOA mi; char text[96];
    if (!wm_app_menu_get_item(hMenu, pos, &mi, text, sizeof(text))) return;
    if (wm_app_menu_item_is_separator(&mi) || wm_app_menu_item_is_disabled(&mi)) return;
    if (mi.hSubMenu) { wm->app_menu_sel[level] = pos; wm_app_menu_open_submenu_from_selection(wm, level); return; }
    HWND hwnd = wm->app_menu_hwnd;
    UINT id = mi.wID;
    wm_close_app_menu_loop(wm, 0);
    wm_send_app_menu_command(wm, hwnd, id);
}

static int wm_app_menu_mouse_down(WindowManager* wm, int x, int y)
{
    if (!wm || wm->menu_kind != MENU_KIND_APP) return 0;
    int idx = -1, top = -1;
    if (wm_app_menu_bar_hit(wm, x, y, &idx, &top) && idx == wm->app_menu_owner_idx) {
        return wm_app_menu_click_top_item(wm, idx, top);
    }
    int level = -1, pos = -1;
    if (wm_app_menu_hit_popup(wm, x, y, &level, &pos)) {
        MENUITEMINFOA mi; char text[96];
        if (!wm_app_menu_get_item(wm->app_menu_popup[level], pos, &mi, text, sizeof(text))) return 1;
        if (wm_app_menu_item_is_separator(&mi) || wm_app_menu_item_is_disabled(&mi)) return 1;
        wm_app_menu_set_selected(wm, level, pos);
        wm_app_menu_invoke(wm, level, pos);
        return 1;
    }
    wm_close_app_menu_loop(wm, 1);
    return 1;
}

static int wm_app_menu_mouse_move(WindowManager* wm, int x, int y)
{
    if (!wm || wm->menu_kind != MENU_KIND_APP) return 0;
    int idx = -1, top = -1;
    if (wm_app_menu_bar_hit(wm, x, y, &idx, &top) && idx == wm->app_menu_owner_idx) {
        if (top != wm->app_menu_top_index) wm_open_app_menu(wm, idx, top);
        return 1;
    }
    int level = -1, pos = -1;
    if (wm_app_menu_hit_popup(wm, x, y, &level, &pos)) {
        MENUITEMINFOA mi; char text[96];
        if (!wm_app_menu_get_item(wm->app_menu_popup[level], pos, &mi, text, sizeof(text))) return 1;
        if (wm_app_menu_item_is_separator(&mi) || wm_app_menu_item_is_disabled(&mi)) return 1;
        wm_app_menu_set_selected(wm, level, pos);
        if (mi.hSubMenu) wm_app_menu_open_submenu_from_selection(wm, level);
        else wm_app_menu_close_deeper(wm, level + 1);
        return 1;
    }
    return 0;
}

static int wm_app_menu_find_top_mnemonic(HMENU bar, int keycode)
{
    int ch = wm_menu_key_to_hotletter(keycode);
    if (!bar || !ch) return -1;
    int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOA mi; char raw[96];
        if (!wm_app_menu_get_item(bar, i, &mi, raw, sizeof(raw))) continue;
        int m = wm_menu_find_mnemonic_char(raw);
        if (m && m == ch) return i;
    }
    return -1;
}

static int wm_app_menu_find_item_mnemonic(HMENU hMenu, int keycode)
{
    int ch = wm_menu_key_to_hotletter(keycode);
    if (!hMenu || !ch) return -1;
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOA mi; char raw[96];
        if (!wm_app_menu_get_item(hMenu, i, &mi, raw, sizeof(raw))) continue;
        if (wm_app_menu_item_is_separator(&mi) || wm_app_menu_item_is_disabled(&mi)) continue;
        int m = wm_menu_find_mnemonic_char(raw);
        if (m && m == ch) return i;
    }
    return -1;
}

static int wm_app_menu_handle_key(WindowManager* wm, int keycode)
{
    if (!wm || wm->menu_kind != MENU_KIND_APP) return 0;
    wm->menu_key_count++;
    int level = wm->app_menu_level_count > 0 ? wm->app_menu_level_count - 1 : 0;
    HMENU hMenu = wm->app_menu_popup[level];
    switch (keycode) {
    case KEY_ESC:
        if (wm->app_menu_level_count > 1) wm_app_menu_close_deeper(wm, wm->app_menu_level_count - 1);
        else wm_close_app_menu_loop(wm, 1);
        return 1;
    case KEY_DOWN:
        wm_app_menu_set_selected(wm, level, wm_app_menu_next_selectable(hMenu, wm->app_menu_sel[level], 1));
        return 1;
    case KEY_UP:
        wm_app_menu_set_selected(wm, level, wm_app_menu_next_selectable(hMenu, wm->app_menu_sel[level], -1));
        return 1;
    case KEY_HOME:
        wm_app_menu_set_selected(wm, level, wm_app_menu_first_selectable(hMenu));
        return 1;
    case KEY_END: {
        int first = wm_app_menu_first_selectable(hMenu);
        wm_app_menu_set_selected(wm, level, wm_app_menu_next_selectable(hMenu, first, -1));
        return 1;
    }
    case KEY_RIGHT: {
        MENUITEMINFOA mi; char text[96];
        if (wm_app_menu_get_item(hMenu, wm->app_menu_sel[level], &mi, text, sizeof(text)) && mi.hSubMenu) {
            wm_app_menu_open_submenu_from_selection(wm, level);
        } else {
            int count = GetMenuItemCount(wm->app_menu_bar);
            if (count > 0) wm_open_app_menu(wm, wm->app_menu_owner_idx, (wm->app_menu_top_index + 1) % count);
        }
        return 1;
    }
    case KEY_LEFT:
        if (wm->app_menu_level_count > 1) wm_app_menu_close_deeper(wm, wm->app_menu_level_count - 1);
        else {
            int count = GetMenuItemCount(wm->app_menu_bar);
            if (count > 0) wm_open_app_menu(wm, wm->app_menu_owner_idx, (wm->app_menu_top_index + count - 1) % count);
        }
        return 1;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_SPACE:
        wm_app_menu_invoke(wm, level, wm->app_menu_sel[level]);
        return 1;
    default: {
        int top = wm_app_menu_find_top_mnemonic(wm->app_menu_bar, keycode);
        if (top >= 0 && (wm->alt_held || wm->app_menu_level_count == 0)) { wm_open_app_menu(wm, wm->app_menu_owner_idx, top); return 1; }
        int item = wm_app_menu_find_item_mnemonic(hMenu, keycode);
        if (item >= 0) { wm_app_menu_set_selected(wm, level, item); wm_app_menu_invoke(wm, level, item); return 1; }
        return 1;
    }
    }
}

int wm_activate_app_menu(WindowManager* wm, int keycode)
{
    if (!wm || wm->focused < 0 || wm->focused >= wm->count) return 0;
    Window* w = &wm->wins[wm->focused];
    if (w->closed || w->minimized || !w->app_hwnd) return 0;
    HMENU bar = GetMenu(w->app_hwnd);
    int count = bar ? GetMenuItemCount(bar) : 0;
    if (!bar || count <= 0) return 0;

    int top = (keycode > 0) ? wm_app_menu_find_top_mnemonic(bar, keycode) : -1;
    if (top < 0) top = 0;

    MENUITEMINFOA mi;
    char text[96];
    memset(&mi, 0, sizeof(mi));
    memset(text, 0, sizeof(text));
    if (!wm_app_menu_get_item(bar, top, &mi, text, sizeof(text))) return 0;

    /* v139: bare Alt/F10 must arm the menu bar, not invoke the first direct
       top-level command.  MDILab intentionally has direct menubar commands
       before its Window submenu; v138 therefore created children on Alt release.
       For keyboard activation without a mnemonic, prefer the first popup so a
       visible menu loop opens; if the bar has direct commands only, consume the
       key and only mark the bar hot.  Alt+mnemonic still invokes a direct item. */
    if (keycode <= 0 && !mi.hSubMenu) {
        for (int i = 0; i < count; ++i) {
            MENUITEMINFOA scan; char scanText[96];
            if (wm_app_menu_get_item(bar, i, &scan, scanText, sizeof(scanText)) && scan.hSubMenu) {
                return wm_app_menu_click_top_item(wm, wm->focused, i);
            }
        }
        wm->app_menu_hwnd = w->app_hwnd;
        wm->app_menu_bar = bar;
        wm->app_menu_owner_idx = wm->focused;
        wm->app_menu_top_index = top;
        wm->app_menu_bar_hot = top;
        return 1;
    }

    return wm_app_menu_click_top_item(wm, wm->focused, top);
}

static void wm_close_menu_loop(WindowManager* wm, int canceled)
{
    if (wm && wm->menu_open && wm->menu_kind == MENU_KIND_APP) { wm_close_app_menu_loop(wm, canceled); return; }
    if (!wm || !wm->menu_open) return;
    wm_menu_notify(wm, WM_EXITMENULOOP, canceled ? 1u : 0u, 0);
    wm->menu_open = 0;
    wm->menu_from_start = 0;
    wm->menu_kind = MENU_KIND_START;
    wm->menu_target_idx = -1;
    wm->menu_target_hwnd = 0;
    wm->menu_selected = -1;
    wm->menu_loop_serial++;
}

static int wm_menu_key_to_hotletter(int keycode)
{
    if (keycode >= KEY_A && keycode <= KEY_Z) return 'a' + (keycode - KEY_A);
    if (keycode >= KEY_1 && keycode <= KEY_9) return '1' + (keycode - KEY_1);
    if (keycode == KEY_0) return '0';
    return 0;
}

static int wm_menu_find_hotletter(WindowManager* wm, int ch)
{
    if (!wm || !ch) return -1;
    ch = tolower(ch);
    int count = wm_menu_count(wm);
    int start = wm->menu_selected;
    for (int pass = 0; pass < count; ++pass) {
        int i = (start + 1 + pass) % count;
        const char* text = wm_menu_text_at_index(wm, i);
        if (!text) continue;
        for (const char* p = text; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c) && tolower(c) == ch) return i;
        }
    }
    return -1;
}

static void wm_menu_invoke_index(WindowManager* wm, int index, int x, int y)
{
    if (!wm || !wm->menu_open) return;
    int kind = wm->menu_kind;
    int target_idx = wm->menu_target_idx;
    UINT cmd = wm_menu_command_at_index(wm, index);
    wm_close_menu_loop(wm, cmd ? 0 : 1);
    if (!cmd) return;
    if (kind == MENU_KIND_SYSTEM) wm_post_system_command(wm, target_idx, cmd, x, y);
    else wm_post_desktop_command(wm, cmd, x, y);
}

#define START_W      72
#define TASK_BTN_W  140
#define TASK_BTN_GAP 8

static void wm_position_menu(WindowManager* wm, int* x, int* y, int w, int h, int avoid_taskbar)
{
    if (!wm || !x || !y) return;
    int sw = wm->screen_w > 0 ? wm->screen_w : 640;
    int sh = wm->screen_h > 0 ? wm->screen_h : 480;
    int bottom_limit = sh - (avoid_taskbar ? TASKBAR_H : 0) - 2;
    if (*x + w > sw - 2) *x = sw - w - 2;
    if (*y + h > bottom_limit) *y = bottom_limit - h;
    if (*x < 2) *x = 2;
    if (*y < 2) *y = 2;
}

static void wm_open_menu(WindowManager* wm, int x, int y, int from_start)
{
    if (!wm) return;

    int sh = wm->screen_h > 0 ? wm->screen_h : 480;
    int mh = MENU_COUNT * MENU_ITEM_H + 2;

    if (from_start) {
        x = 4;
        y = sh - TASKBAR_H - mh - 2;
    }

    wm_position_menu(wm, &x, &y, MENU_W, mh, 1);

    wm->menu_open = 1;
    wm->menu_kind = MENU_KIND_START;
    wm->menu_x = x;
    wm->menu_y = y;
    wm->menu_from_start = from_start;
    wm->menu_target_idx = -1;
    wm->menu_target_hwnd = 0;
    wm_menu_enter_loop(wm);
}

static void wm_open_system_menu(WindowManager* wm, int idx, int x, int y)
{
    if (!valid_window_index(wm, idx)) return;
    int mh = SYSTEM_MENU_COUNT * MENU_ITEM_H + 2;
    wm_position_menu(wm, &x, &y, MENU_W, mh, 0);
    wm->menu_open = 1;
    wm->menu_kind = MENU_KIND_SYSTEM;
    wm->menu_x = x;
    wm->menu_y = y;
    wm->menu_from_start = 0;
    wm->menu_target_idx = idx;
    wm->menu_target_hwnd = window_hwnd(&wm->wins[idx]);
    wm_menu_enter_loop(wm);
}


static void draw_app_menubar(Window* w, Framebuffer* fb, int focused, WindowManager* wm)
{
    if (!w || !fb || !w->app_hwnd) return;
    HMENU bar = GetMenu(w->app_hwnd);
    if (!bar) return;
    int x0 = w->x + 1;
    int y0 = w->y + TITLEBAR_H;
    int ww = w->w - 2;
    if (ww <= 4) return;
    Color bg = focused ? COLOR(34,38,56) : COLOR(30,30,42);
    fb_rect(fb, x0, y0, ww, APP_MENUBAR_H, bg);
    fb_rect(fb, x0, y0 + APP_MENUBAR_H - 1, ww, 1, COLOR(72,76,104));

    int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i) {
        int rx, ry, rw, rh;
        MENUITEMINFOA mi; char raw[96], vis[96];
        if (!wm_app_menu_get_item(bar, i, &mi, raw, sizeof(raw))) continue;
        if (!wm_app_menu_bar_item_rect(w, i, &rx, &ry, &rw, &rh)) continue;
        wm_menu_copy_visible_text(raw, vis, sizeof(vis), 0);
        int hot = wm && wm->menu_open && wm->menu_kind == MENU_KIND_APP && wm->app_menu_hwnd == w->app_hwnd && wm->app_menu_top_index == i;
        if (hot) {
            fb_rect(fb, rx + 1, ry + 2, rw - 2, rh - 3, COLOR(74,84,126));
            fb_rect_outline(fb, rx + 1, ry + 2, rw - 2, rh - 3, COLOR(148,170,220));
        }
        Color tc = wm_app_menu_item_is_disabled(&mi) ? COLOR(128,128,142) : WHITE;
        font_draw_str(fb, rx + 10, ry + 7, vis, hot ? WHITE : tc);
    }
}

static void wm_app_menu_draw_checkmark(Framebuffer* fb, int x, int y, Color c)
{
    if (!fb) return;
    /*
     * Do not draw a UTF-8 checkmark through font_draw_str(): the built-in
     * 8x8 font is ASCII-only, so every non-ASCII byte becomes '?'.  Win32
     * checked menu items have a separate mark column, not text bytes before
     * the item label, so draw the check mark as chrome pixels here.
     */
    const int pts[][2] = {
        {1,5}, {2,6}, {3,7}, {4,8},
        {5,7}, {6,6}, {7,5}, {8,4}, {9,3}, {10,2}
    };
    for (unsigned i = 0; i < sizeof(pts) / sizeof(pts[0]); ++i) {
        int px = x + pts[i][0];
        int py = y + pts[i][1];
        fb_pixel(fb, px,     py, c);
        fb_pixel(fb, px + 1, py, c);
        fb_pixel(fb, px,     py + 1, c);
    }
}

static void draw_app_menu_popups(WindowManager* wm, Framebuffer* fb)
{
    if (!wm || !fb || !wm->menu_open || wm->menu_kind != MENU_KIND_APP) return;
    for (int level = 0; level < wm->app_menu_level_count; ++level) {
        HMENU hMenu = wm->app_menu_popup[level];
        int count = GetMenuItemCount(hMenu);
        int x = wm->app_menu_x[level], y = wm->app_menu_y[level], w = wm->app_menu_w[level], h = wm->app_menu_h[level];
        fb_rect(fb, x, y, w, h, COLOR(38,40,56));
        fb_rect_outline(fb, x, y, w, h, COLOR(120,128,166));
        for (int i = 0; i < count; ++i) {
            MENUITEMINFOA mi; char raw[96], vis[96];
            if (!wm_app_menu_get_item(hMenu, i, &mi, raw, sizeof(raw))) continue;
            int iy = y + 1 + i * APP_MENU_POPUP_ITEM_H;
            if (wm_app_menu_item_is_separator(&mi)) {
                fb_rect(fb, x + 6, iy + APP_MENU_POPUP_ITEM_H / 2, w - 12, 1, COLOR(82,86,112));
                continue;
            }
            int sel = (i == wm->app_menu_sel[level]);
            if (sel) {
                fb_rect(fb, x + 3, iy + 2, w - 6, APP_MENU_POPUP_ITEM_H - 3, COLOR(76,88,132));
                fb_rect_outline(fb, x + 3, iy + 2, w - 6, APP_MENU_POPUP_ITEM_H - 3, COLOR(155,175,225));
            }
            Color tc = wm_app_menu_item_is_disabled(&mi) ? COLOR(125,125,138) : (sel ? WHITE : COLOR(232,232,240));
            if (mi.fState & MF_CHECKED) wm_app_menu_draw_checkmark(fb, x + 4, iy + 5, tc);
            wm_menu_copy_visible_text(raw, vis, sizeof(vis), 1);
            char* tab = strchr(vis, '\t');
            if (tab) {
                *tab = 0;
                font_draw_str(fb, x + APP_MENU_CHECK_W + 8, iy + 7, vis, tc);
                font_draw_str(fb, x + w - 68, iy + 7, tab + 1, tc);
            } else {
                font_draw_str(fb, x + APP_MENU_CHECK_W + 8, iy + 7, vis, tc);
            }
            if (mi.hSubMenu) font_draw_str(fb, x + w - 16, iy + 7, ">", tc);
        }
    }
}

static void draw_menu(WindowManager* wm, Framebuffer* fb)
{
    if (!wm || !fb || !wm->menu_open) return;
    if (wm->menu_kind == MENU_KIND_APP) return;
    int x = wm->menu_x, y = wm->menu_y;
    int is_sys = (wm->menu_kind == MENU_KIND_SYSTEM);
    int count = is_sys ? SYSTEM_MENU_COUNT : MENU_COUNT;
    int h = count * MENU_ITEM_H + 2;
    fb_rect        (fb, x, y, MENU_W, h, is_sys ? COLOR(38,42,58) : COLOR(40,40,55));
    fb_rect_outline(fb, x, y, MENU_W, h, is_sys ? COLOR(130,130,165) : COLOR(90,90,120));
    if (is_sys) {
        for (int i = 0; i < SYSTEM_MENU_COUNT; i++) {
            int iy = y + 1 + i*MENU_ITEM_H;
            const SystemMenuItem* item = &g_SystemMenuItems[i];
            if (item->flags & SYSTEM_MENU_SEPARATOR) {
                fb_rect(fb, x+4, iy+10, MENU_W-8, 1, COLOR(86,90,112));
            } else {
                int sel = (i == wm->menu_selected);
                if (sel) {
                    fb_rect(fb, x+3, iy+2, MENU_W-6, MENU_ITEM_H-3, COLOR(76,88,132));
                    fb_rect_outline(fb, x+3, iy+2, MENU_W-6, MENU_ITEM_H-3, COLOR(155,175,225));
                }
                font_draw_str(fb, x+10, iy+7, item->text, sel ? WHITE : COLOR(230,230,235));
                if (item->id == SC_CLOSE) font_draw_str(fb, x+MENU_W-44, iy+7, "Alt+F4", sel ? WHITE : COLOR(200,200,210));
            }
        }
    } else {
        for (int i = 0; i < MENU_COUNT; i++) {
            int iy = y + 1 + i*MENU_ITEM_H;
            const StartMenuItem* item = &g_StartMenuItems[i];
            if (item->flags & START_MENU_SEPARATOR) {
                fb_rect(fb, x+4, iy+10, MENU_W-8, 1, COLOR(70,70,90));
            } else {
                int sel = (i == wm->menu_selected);
                if (sel) {
                    fb_rect(fb, x+3, iy+2, MENU_W-6, MENU_ITEM_H-3, COLOR(78,84,130));
                    fb_rect_outline(fb, x+3, iy+2, MENU_W-6, MENU_ITEM_H-3, COLOR(150,170,220));
                }
                font_draw_str(fb, x+10, iy+7, item->text, sel ? WHITE : COLOR(230,230,235));
            }
        }
    }
}

// ── Taskleiste ───────────────────────────────

static void draw_taskbar(WindowManager* wm, Framebuffer* fb)
{
    int sw = fb->width, sh = fb->height;
    int ty = sh - TASKBAR_H;

    fb_rect(fb, 0, ty, sw, TASKBAR_H, COLOR(20,20,30));
    fb_rect(fb, 0, ty, sw, 1, COLOR(70,70,100));

    // Start-Button
    Color start_bg = wm->menu_open && wm->menu_from_start ? COLOR(70,70,115) : COLOR(45,45,70);
    fb_rect        (fb, 4, ty+4, START_W, TASKBAR_H-8, start_bg);
    fb_rect_outline(fb, 4, ty+4, START_W, TASKBAR_H-8,
                    wm->menu_open && wm->menu_from_start ? WHITE : COLOR(90,90,125));
    font_draw_str(fb, 10, ty+12, "START", WHITE);

    // Fenster-Buttons
    int bx = 4 + START_W + TASK_BTN_GAP;
    for (int i = 0; i < wm->count; i++) {
        if (wm->wins[i].closed) continue;
        int active = (i == wm->focused);
        int mini   = wm->wins[i].minimized;
        Color bg = active ? TITLEBAR_ACT : (mini ? COLOR(40,40,60) : COLOR(55,55,75));
        fb_rect        (fb, bx, ty+4, TASK_BTN_W, TASKBAR_H-8, bg);
        fb_rect_outline(fb, bx, ty+4, TASK_BTN_W, TASKBAR_H-8,
                        active ? WHITE : COLOR(70,70,100));
        char label[17];
        shorten_middle(wm->wins[i].title, label, sizeof(label));
        Color tc = mini ? COLOR(150,150,150) : WHITE;
        font_draw_str(fb, bx+6, ty+12, label, tc);
        bx += TASK_BTN_W + TASK_BTN_GAP;
    }

    // Uhrzeit rechts
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d",
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    font_draw_str(fb, sw - 76, ty + 12, clock, COLOR(200,200,220));
}


/* v64: generic GDI-IPC renderer for the first real out-of-process GUI app.
   Calculator/Editor WndProc state runs inside myos_apphost_child.  The parent
   reads a compact state snapshot from ProcessHost/shared memory and paints it
   with the same framebuffer primitives as legacy in-process apps. */
typedef struct RemoteCalcRect { int x, y, w, h; } RemoteCalcRect;
typedef struct RemoteCalcLayout { RemoteCalcRect display, history, buttons[5][4]; int pad, gap, history_visible; } RemoteCalcLayout;
static const char* g_remote_calc_labels[5][4] = {
    {"C", "+-", "%", "/"}, {"7", "8", "9", "*"}, {"4", "5", "6", "-"}, {"1", "2", "3", "+"}, {"0", ".", "<-", "="}
};
static int remote_calc_clamp(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }
static void remote_calc_layout(RemoteCalcLayout* l, int cw, int ch)
{
    memset(l, 0, sizeof(*l));
    int base = cw < ch ? cw : ch;
    l->pad = remote_calc_clamp(base / 28, 6, 14);
    l->gap = remote_calc_clamp(base / 55, 4, 10);
    int pad = l->pad, gap = l->gap;
    int display_h = remote_calc_clamp(ch / 7, 38, 62);
    l->display.x = pad; l->display.y = pad; l->display.w = cw - pad * 2; l->display.h = display_h;
    int y = l->display.y + l->display.h + gap;
    int bottom_pad = pad + RESIZE_GRIP;
    int avail_after_display = ch - y - bottom_pad;
    int button_gap_total = gap * 4;
    int ideal_button_h = (avail_after_display - button_gap_total) / 5;
    int history_h = 0;
    if (ch >= 330) {
        history_h = remote_calc_clamp(ch / 5, 48, 104);
        ideal_button_h = (avail_after_display - history_h - gap - button_gap_total) / 5;
        if (ideal_button_h < 30) {
            history_h = remote_calc_clamp(ch / 8, 32, 56);
            ideal_button_h = (avail_after_display - history_h - gap - button_gap_total) / 5;
        }
    }
    if (history_h >= 28 && ideal_button_h >= 26) {
        l->history_visible = 1;
        l->history.x = pad; l->history.y = y; l->history.w = cw - pad * 2; l->history.h = history_h;
        y += history_h + gap;
    }
    int bw = (cw - pad * 2 - gap * 3) / 4;
    int bh = (ch - y - bottom_pad - gap * 4) / 5;
    if (bh < 22) bh = 22;
    if (bw < 34) bw = 34;
    for (int row = 0; row < 5; row++) for (int col = 0; col < 4; col++) {
        l->buttons[row][col].x = pad + col * (bw + gap);
        l->buttons[row][col].y = y + row * (bh + gap);
        l->buttons[row][col].w = bw;
        l->buttons[row][col].h = bh;
    }
}
static void remote_draw_str_right(Framebuffer* fb, RemoteCalcRect r, const char* s, Color c)
{
    int len = s ? (int)strlen(s) : 0;
    int max_chars = (r.w - 10) / 8; if (max_chars < 1) max_chars = 1;
    const char* out = s ? s : "";
    if (len > max_chars) { out = s + (len - max_chars); len = max_chars; }
    int x = r.x + r.w - 6 - len * 8;
    int y = r.y + (r.h - 8) / 2;
    font_draw_str(fb, x, y, out, c);
}
static void remote_draw_centered(Framebuffer* fb, RemoteCalcRect r, const char* s, Color c)
{
    int lw = s ? (int)strlen(s) * 8 : 0;
    int x = r.x + (r.w - lw) / 2;
    int y = r.y + (r.h - 8) / 2;
    font_draw_str(fb, x, y, s ? s : "", c);
}

static void remote_gdi_line(Framebuffer* fb, int x0, int y0, int x1, int y1, Color c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb_pixel(fb, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}


/* v75: tiny DWM-lite surface cache/compositor for OOP windows.
   The child creates/maps a FileMapping through the kernel bridge and publishes
   its POSIX shm backing name in MyProcessIpcShared.  The parent maps that
   backing read-only and blits stable XRGB8888 frames into the framebuffer.
   This is intentionally small: no alpha, no GPU, no occlusion tree yet. */
typedef struct WmSurfaceCache {
    int valid;
    char name[MYOS_IPC_IMAGE_MAX];
    void* ptr;
    size_t size;
    uint32_t lastSeq;
} WmSurfaceCache;

static WmSurfaceCache g_WmSurfaceCache[MAX_WINDOWS];

static void wm_surface_cache_close(WmSurfaceCache* c)
{
    if (!c) return;
    if (c->valid && c->ptr && c->size) munmap(c->ptr, c->size);
    memset(c, 0, sizeof(*c));
}

static MySurfaceHeader* wm_surface_cache_map(int index, const MyProcessHostInfo* hi)
{
    if (index < 0 || index >= MAX_WINDOWS || !hi || !hi->surface_enabled || !hi->surface_map_name[0] || !hi->surface_map_size)
        return NULL;
    WmSurfaceCache* c = &g_WmSurfaceCache[index];
    if (c->valid && (strcmp(c->name, hi->surface_map_name) != 0 || c->size != hi->surface_map_size))
        wm_surface_cache_close(c);
    if (!c->valid) {
        int fd = shm_open(hi->surface_map_name, O_RDONLY, 0);
        if (fd < 0) return NULL;
        void* p = mmap(NULL, hi->surface_map_size, PROT_READ, MAP_SHARED, fd, 0);
        int saved = errno;
        close(fd);
        if (p == MAP_FAILED) { (void)saved; return NULL; }
        c->valid = 1;
        c->ptr = p;
        c->size = hi->surface_map_size;
        snprintf(c->name, sizeof(c->name), "%s", hi->surface_map_name);
    }
    if (c->size < sizeof(MySurfaceHeader)) return NULL;
    return (MySurfaceHeader*)c->ptr;
}

static int wm_draw_oop_surface(int index, Window* w, Framebuffer* fb, const MyProcessHostInfo* hi)
{
    if (!w || !fb || !hi || !hi->surface_enabled || !hi->surface_mapped) return 0;
    int client_x = w->x + 1;
    int client_y = w->y + TITLEBAR_H;
    int client_w = w->w - 2;
    int client_h = w->h - TITLEBAR_H - 1;
    if (client_w <= 0 || client_h <= 0) return 0;
    MySurfaceHeader* h = wm_surface_cache_map(index, hi);
    if (!h || h->magic != MYOS_SURFACE_MAGIC || h->version != MYOS_SURFACE_VERSION || h->format != MYOS_SURFACE_FORMAT_XRGB8888)
        return 0;
    uint32_t a = h->seqBegin;
    if (a & 1u) return 0;
    uint32_t width = h->width, height = h->height, stride = h->stride;
    if (!width || !height || stride < width * 4u) return 0;
    size_t needed = sizeof(MySurfaceHeader) + (size_t)stride * (size_t)height;
    if (needed > g_WmSurfaceCache[index].size) return 0;
    if (width > (uint32_t)client_w) width = (uint32_t)client_w;
    if (height > (uint32_t)client_h) height = (uint32_t)client_h;
    const uint8_t* pixels = ((const uint8_t*)h) + sizeof(MySurfaceHeader);
    for (uint32_t yy = 0; yy < height; ++yy) {
        const uint32_t* row = (const uint32_t*)(const void*)(pixels + (size_t)yy * stride);
        for (uint32_t xx = 0; xx < width; ++xx) {
            fb_pixel(fb, client_x + (int)xx, client_y + (int)yy, (Color)(row[xx] & 0x00ffffffu));
        }
    }
    uint32_t b = h->seqEnd;
    if (a != b || (b & 1u)) return 1; /* frame may be mid-update, but we at least kept previous visual path alive. */
    g_WmSurfaceCache[index].lastSeq = b;
    char dbg[160];
    snprintf(dbg, sizeof(dbg), "SURFACE v75.1 seq=%u %ux%u dirty=%d,%d-%d,%d %.28s",
             (unsigned)b, (unsigned)h->width, (unsigned)h->height,
             (int)h->dirtyLeft, (int)h->dirtyTop, (int)h->dirtyRight, (int)h->dirtyBottom,
             hi->surface_status);
    font_draw_str(fb, client_x + 10, client_y + client_h - 18, dbg, COLOR(120,255,180));
    return 1;
}

static int wm_clip_client_rect(int client_x, int client_y, int client_w, int client_h, int* x, int* y, int* w, int* h)
{
    if (!x || !y || !w || !h) return 0;
    int rx = *x, ry = *y, rw = *w, rh = *h;
    int cr = client_x + client_w;
    int cb = client_y + client_h;
    if (rw <= 0 || rh <= 0 || client_w <= 0 || client_h <= 0) return 0;
    if (rx < client_x) { rw -= (client_x - rx); rx = client_x; }
    if (ry < client_y) { rh -= (client_y - ry); ry = client_y; }
    if (rx + rw > cr) rw = cr - rx;
    if (ry + rh > cb) rh = cb - ry;
    if (rw <= 0 || rh <= 0) return 0;
    *x = rx; *y = ry; *w = rw; *h = rh;
    return 1;
}

static void draw_ipc_gdi_commands(Window* w, Framebuffer* fb, const MyProcessHostInfo* hi)
{
    if (!w || !fb || !hi) return;
    int client_x = w->x + 1;
    int client_y = w->y + TITLEBAR_H;
    int client_w = w->w - 2;
    int client_h = w->h - TITLEBAR_H - 1;
    if (client_w <= 0 || client_h <= 0) return;

    DWORD count = hi->gdi_command_count;
    if (count > MYOS_GDI_MAX_COMMANDS) count = MYOS_GDI_MAX_COMMANDS;

    uint32_t target = (uint32_t)w->app_hwnd;
    int taggedForThisWindow = 0;
    int hasTaggedCommands = 0;
    for (DWORD i = 0; i < count; i++) {
        const MyGdiIpcCommand* c = &hi->gdi_commands[i];
        if (c->opcode == MYOS_GDI_OP_NONE) continue;
        if (c->hwnd) {
            hasTaggedCommands = 1;
            if (c->hwnd == target) taggedForThisWindow = 1;
        }
    }

    for (DWORD i = 0; i < count; i++) {
        const MyGdiIpcCommand* c = &hi->gdi_commands[i];
        if (c->opcode == MYOS_GDI_OP_NONE) continue;
        if (hasTaggedCommands) {
            if (!c->hwnd || c->hwnd != target) continue;
        }
        int x = client_x + c->x;
        int y = client_y + c->y;
        int ww = c->w;
        int hh = c->h;
        if (ww < 0) ww = 0;
        if (hh < 0) hh = 0;
        if (c->opcode == MYOS_GDI_OP_FILLRECT) {
            int rx = x, ry = y, rw = ww, rh = hh;
            if (wm_clip_client_rect(client_x, client_y, client_w, client_h, &rx, &ry, &rw, &rh))
                fb_rect(fb, rx, ry, rw, rh, (Color)c->color);
        } else if (c->opcode == MYOS_GDI_OP_RECTANGLE) {
            /* v173.3: retained OOP-GDI may be one frame behind the live shell
               resize.  Never allow stale command extents to draw outside the
               current client box while waiting for the child WM_SIZE repaint. */
            int rx = x, ry = y, rw = ww, rh = hh;
            if (wm_clip_client_rect(client_x, client_y, client_w, client_h, &rx, &ry, &rw, &rh))
                fb_rect_outline(fb, rx, ry, rw, rh, (Color)c->color);
        } else if (c->opcode == MYOS_GDI_OP_LINE) {
            remote_gdi_line(fb, x, y, client_x + c->w, client_y + c->h, (Color)c->color);
        } else if (c->opcode == MYOS_GDI_OP_TEXTOUT || c->opcode == MYOS_GDI_OP_DRAWTEXT) {
            const char* text = c->text;
            int len = text ? (int)strlen(text) : 0;
            int max_chars = ww > 0 ? ww / 8 : len;
            if (max_chars < 1) max_chars = 1;
            if (len > max_chars) { text += (len - max_chars); len = max_chars; }
            int tx = x;
            if ((c->flags & MYOS_GDI_TEXT_RIGHT) != 0u) tx = x + ww - len * 8 - 2;
            else if ((c->flags & MYOS_GDI_TEXT_CENTER) != 0u) tx = x + (ww - len * 8) / 2;
            int ty = y;
            if ((c->flags & MYOS_GDI_TEXT_VCENTER) != 0u) ty = y + (hh - 8) / 2;
            font_draw_str(fb, tx, ty, text ? text : "", (Color)c->color);
        }
    }

    char dbg[128];
    snprintf(dbg, sizeof(dbg), "%s seq=%u paint=%u cmds=%u target=%u %.24s",
             hasTaggedCommands ? (taggedForThisWindow ? "GDI-HWND" : "GDI-HWND(empty)") : "GDI-IPC",
             (unsigned)hi->gdi_sequence, (unsigned)hi->gdi_paint_count,
             (unsigned)hi->gdi_command_count, (unsigned)target, hi->gdi_status);
    font_draw_str(fb, client_x + 10, client_y + client_h - 30, dbg, COLOR(130,210,255));
}

static void draw_remote_calc(Window* w, Framebuffer* fb, const MyProcessHostInfo* hi)
{
    int client_x = w->x + 1;
    int client_y = w->y + TITLEBAR_H;
    int cw = w->w - 2;
    int ch = w->h - TITLEBAR_H - 1;
    if (cw < 40 || ch < 40) return;
    fb_rect(fb, client_x, client_y, cw, ch, COLOR(23, 23, 34));
    RemoteCalcLayout l;
    remote_calc_layout(&l, cw, ch);
    RemoteCalcRect d = l.display; d.x += client_x; d.y += client_y;
    fb_rect(fb, d.x, d.y, d.w, d.h, COLOR(13,13,22));
    fb_rect_outline(fb, d.x, d.y, d.w, d.h, COLOR(85,85,120));
    if (hi->calc_opline[0]) font_draw_str(fb, d.x + 6, d.y + 6, hi->calc_opline, COLOR(135,135,160));
    remote_draw_str_right(fb, d, hi->calc_display[0] ? hi->calc_display : "0", WHITE);
    if (l.history_visible) {
        RemoteCalcRect hr = l.history; hr.x += client_x; hr.y += client_y;
        fb_rect(fb, hr.x, hr.y, hr.w, hr.h, COLOR(18,18,28));
        fb_rect_outline(fb, hr.x, hr.y, hr.w, hr.h, COLOR(70,70,95));
        font_draw_str(fb, hr.x + 6, hr.y + 5, "Verlauf [child]", COLOR(150,150,180));
        char hist[192]; snprintf(hist, sizeof(hist), "%s", hi->calc_history_preview);
        char* save = NULL;
        char* tok = strtok_r(hist, "|", &save);
        int line = 0;
        while (tok && line < 5) {
            while (*tok == ' ') tok++;
            font_draw_str(fb, hr.x + 6, hr.y + 18 + line * 10, tok, COLOR(210,210,225));
            tok = strtok_r(NULL, "|", &save);
            line++;
        }
    }
    for (int row = 0; row < 5; row++) for (int col = 0; col < 4; col++) {
        RemoteCalcRect b = l.buttons[row][col]; b.x += client_x; b.y += client_y;
        Color bc = (row == 0 && col == 0) ? COLOR(80,40,40) : (col == 3 ? COLOR(40,65,130) : COLOR(50,50,65));
        if (row == 4 && col == 3) bc = COLOR(40,95,55);
        fb_rect(fb, b.x, b.y, b.w, b.h, bc);
        fb_rect_outline(fb, b.x, b.y, b.w, b.h, COLOR(92,92,125));
        remote_draw_centered(fb, b, g_remote_calc_labels[row][col], WHITE);
    }
    char line[96];
    snprintf(line, sizeof(line), "OOP child pid=%d rev=%u hits=%u last=%s", hi->linux_pid, (unsigned)hi->calc_revision, (unsigned)hi->calc_button_hits, hi->calc_last_button);
    font_draw_str(fb, client_x + 10, client_y + ch - 16, line, COLOR(160,220,255));
}

// ── Fenster zeichnen ─────────────────────────

static void draw_window_uncached(WindowManager* wm, Framebuffer* fb, Window* w, int focused)
{
    if (w->minimized) return;

    /* v173.4: the compositor owns all non-client pixels during live OOP
       resize.  v173.3 filled the current client before retained child GDI,
       but the classic frame/titlebar buttons were still partially painted by
       later primitives and could expose a one-pixel edge while dragging.
       Start every window from a current-size frame erase, then paint app
       content/titlebar/buttons, and finally redraw the outer outline last. */
    fb_rect(fb, w->x, w->y, w->w, w->h, COLOR(12,14,22));

    // Client-Inhalt
    if (w->app_type == APP_TERMINAL) {
        if (w->term)
            term_draw(w->term, fb, w->x+1, w->y+TITLEBAR_H,
                      w->w-2, w->h-TITLEBAR_H-1);
    } else if (w->app_type == APP_CALC) {
        calc_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_EDITOR) {
        editor_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_SPY) {
        spy_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_ACCESS) {
        access_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_PUMP) {
        pump_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_DEADLOCK) {
        deadlock_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_SECTION) {
        section_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_BUS_PRODUCER || w->app_type == APP_BUS_CONSUMER) {
        sharedbus_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_OBJECT) {
        objectlab_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_WAITLAB) {
        waitlab_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_CLIPMENU) {
        clipmenu_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_PAINTLAB) {
        paintlab_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_DRAGLAB) {
        draglab_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_CONTROLLAB) {
        controllab_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_SERVICELAB) {
        servicelab_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_DIALOGLAB) {
        dialoglab_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_MDILAB) {
        mdilab_blit(w->app_hwnd, w->x, w->y, w->w, w->h, fb);
    } else if (w->app_type == APP_IPC_PROXY) {
        MyProcessHostInfo calcHi;
        int haveCalcHi = (w->process_id && MyProcessHostGetInfo(w->process_id, &calcHi));
        /* v173.3: the shell frame is resized immediately while the fork/exec
           child receives WM_SIZE asynchronously over ProcessHost IPC.  The
           compositor must still own the full current client background for this
           frame; otherwise newly exposed client pixels show the desktop until
           the child publishes its next retained GDI stream.  This is not an app
           repaint hack: it is the window-manager fallback erase for OOP HWNDs. */
        int ipc_cx = w->x + 1;
        int ipc_cy = w->y + TITLEBAR_H;
        int ipc_cw = w->w - 2;
        int ipc_ch = w->h - TITLEBAR_H - 1;
        if (ipc_cw > 0 && ipc_ch > 0)
            fb_rect(fb, ipc_cx, ipc_cy, ipc_cw, ipc_ch, COLOR(12,14,22));
        if (haveCalcHi && calcHi.surface_enabled && calcHi.surface_mapped &&
            wm_draw_oop_surface((int)(calcHi.my_pid % MAX_WINDOWS), w, fb, &calcHi)) {
            if (calcHi.gdi_enabled && calcHi.gdi_command_count) draw_ipc_gdi_commands(w, fb, &calcHi);
            MyDrawChildWindows(w->app_hwnd, fb, w->x + 1, w->y + TITLEBAR_H);
        } else if (haveCalcHi && calcHi.gdi_enabled && calcHi.gdi_command_count) {
            draw_ipc_gdi_commands(w, fb, &calcHi);
            MyDrawChildWindows(w->app_hwnd, fb, w->x + 1, w->y + TITLEBAR_H);
        } else if (haveCalcHi && calcHi.calc_enabled) {
            draw_remote_calc(w, fb, &calcHi);
        } else {
        int cx = w->x + 1;
        int cy = w->y + TITLEBAR_H;
        int cw = w->w - 2;
        int ch = w->h - TITLEBAR_H - 1;
        fb_rect(fb, cx, cy, cw, ch, COLOR(16,22,36));
        fb_rect_outline(fb, cx + 8, cy + 10, cw - 16, ch - 20, COLOR(80,120,180));
        font_draw_str(fb, cx + 18, cy + 24, "v70 OOP GUI IPC proxy", COLOR(180,230,255));
        font_draw_str(fb, cx + 18, cy + 44, "user32 stubs over IPC: CW/PM/GM/DM", COLOR(210,210,230));
        char line[128];
        snprintf(line, sizeof(line), "HWND=%u LinuxPID=%d", (unsigned)w->app_hwnd, w->ipc_linux_pid);
        font_draw_str(fb, cx + 18, cy + 64, line, COLOR(170,255,190));
        snprintf(line, sizeof(line), "class=%.46s", w->ipc_class[0] ? w->ipc_class : "-");
        font_draw_str(fb, cx + 18, cy + 84, line, COLOR(220,220,180));
        font_draw_str(fb, cx + 18, cy + 104, w->ipc_status[0] ? w->ipc_status : "parent-created-window", COLOR(230,230,250));
        MyProcessHostInfo hi;
        if (w->process_id && MyProcessHostGetInfo(w->process_id, &hi)) {
            snprintf(line, sizeof(line), "msg sent=%u recv=%u disp=%u post=%u ack=%u",
                     (unsigned)hi.gui_msg_sent, (unsigned)hi.gui_msg_received,
                     (unsigned)hi.gui_msg_dispatched, (unsigned)hi.gui_post_request,
                     (unsigned)hi.gui_post_ack);
            font_draw_str(fb, cx + 18, cy + 124, line, COLOR(170,220,255));
            snprintf(line, sizeof(line), "last hwnd=%u msg=0x%04x close=%u",
                     (unsigned)hi.gui_last_hwnd, (unsigned)hi.gui_last_msg,
                     (unsigned)hi.gui_close_seen);
            font_draw_str(fb, cx + 18, cy + 144, line, COLOR(255,220,170));
            snprintf(line, sizeof(line), "rt api=%u cls=%u cw=%u gm=%u dm=%u",
                     (unsigned)hi.gui_runtime_api_calls,
                     (unsigned)hi.gui_register_class_calls,
                     (unsigned)hi.gui_create_window_calls,
                     (unsigned)hi.gui_get_message_calls,
                     (unsigned)hi.gui_dispatch_message_calls);
            font_draw_str(fb, cx + 18, cy + 164, line, COLOR(190,255,220));
            snprintf(line, sizeof(line), "kreq op=%u req=%u ack=%u ok=%u res=0x%x",
                     (unsigned)hi.kernel_op, (unsigned)hi.kernel_request,
                     (unsigned)hi.kernel_ack, (unsigned)hi.kernel_ok,
                     (unsigned)hi.kernel_result);
            font_draw_str(fb, cx + 18, cy + 184, line, COLOR(255,210,160));
            snprintf(line, sizeof(line), "%.62s", hi.kernel_status[0] ? hi.kernel_status : "kernel bridge idle");
            font_draw_str(fb, cx + 18, cy + 204, line, COLOR(255,235,190));
        }
        }
    }

    /* v96: USER32 standard scrollbars attached via WS_VSCROLL/WS_HSCROLL +
       SetScrollInfo/GetScrollInfo are drawn as client-edge overlays. */
    MyDrawStandardScrollBars(w->app_hwnd, fb, w->x + 1, w->y + TITLEBAR_H, w->w - 2, w->h - TITLEBAR_H - 1);

    // Rahmen wird am Ende noch einmal final gezogen, damit Titelleiste,
    // Close/Minimize und Resize-Grip die Außenkante beim Live-Resize nicht
    // übermalen.
    Color border = focused ? WHITE : COLOR(60,60,80);

    // Resize-Ecke
    fb_rect(fb, w->x+w->w-RESIZE_GRIP, w->y+w->h-RESIZE_GRIP,
            RESIZE_GRIP, RESIZE_GRIP,
            focused ? COLOR(100,100,160) : COLOR(50,50,70));

    // Titelleiste
    Color tbg = focused ? TITLEBAR_ACT : TITLEBAR;
    fb_rect(fb, w->x, w->y, w->w, TITLEBAR_H, tbg);
    fb_rect(fb, w->x, w->y+TITLEBAR_H-1, w->w, 1, COLOR(20,20,70));

    draw_app_menubar(w, fb, focused, wm);

    // Titel mit Mittel-Kürzung, damit z.B. lange .c/.h-Dateinamen unterscheidbar bleiben.
    int avail  = w->w - TITLEBAR_H*2 - 8;
    int max_chars = avail / 8;
    if (max_chars < 4) max_chars = 4;
    if (max_chars > 63) max_chars = 63;
    char title_short[64];
    shorten_middle(w->title, title_short, max_chars + 1);
    int text_w = (int)strlen(title_short)*8;
    int text_x = w->x + (avail - text_w)/2;
    if (text_x < w->x+4) text_x = w->x+4;
    font_draw_str(fb, text_x, w->y+(TITLEBAR_H-8)/2, title_short, WHITE);

    // Minimieren-Button [_]
    int mx = w->x + w->w - TITLEBAR_H*2;
    fb_rect(fb, mx, w->y, TITLEBAR_H, TITLEBAR_H, COLOR(80,80,120));
    fb_rect(fb, mx+(TITLEBAR_H-8)/2, w->y+TITLEBAR_H-6, 8, 2, WHITE);

    // Schließen-Button [X]
    int cx = w->x + w->w - TITLEBAR_H;
    fb_rect(fb, cx, w->y, TITLEBAR_H, TITLEBAR_H, COLOR(180,40,40));
    font_draw_str(fb, cx+(TITLEBAR_H-8)/2, w->y+(TITLEBAR_H-8)/2, "X", WHITE);

    /* v173.4: final NC outline pass.  This is intentionally after the caption
       buttons and resize grip: the outer frame belongs to the compositor, not
       to the child GDI stream nor to individual titlebar widgets. */
    fb_rect_outline(fb, w->x, w->y, w->w, w->h, border);
}



/* v178: per-top-level backing cache for retained OOP and selected in-process windows.
   v175/v176 made the compositor damage-driven, and v177 retained OOP windows.
   v178 adds a conservative visual-sequence contract for selected in-process
   apps: messages advance the owning HWND tree serial, unchanged windows are
   blitted from cache, and only changed windows rerender into their backing. */
typedef struct WmBackingCache {
    int valid;
    HWND hwnd;
    DWORD process_id;
    AppType app_type;
    int w, h;
    int focused;
    unsigned long long sig;
    uint8_t* pixels;
    size_t stride;
    size_t size;
} WmBackingCache;

static WmBackingCache g_WmBackingCache[MAX_WINDOWS];

static unsigned long long wm_hash_u64(unsigned long long h, unsigned long long v)
{
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

static unsigned long long wm_hash_bytes(unsigned long long h, const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static unsigned long long wm_hash_cstr(unsigned long long h, const char* s)
{
    if (!s) return wm_hash_u64(h, 0);
    return wm_hash_bytes(h, s, strlen(s) + 1);
}

static int wm_window_backing_cacheable(const Window* w)
{
    if (!w || w->closed || w->minimized || w->w <= 0 || w->h <= 0) return 0;
    if (w->app_type == APP_IPC_PROXY) return 1;

    /* v178: safe in-process retained cache users.  These apps now participate
       through the HWND visual-sequence lane: their top-level and child HWNDs
       advance MyWindowState.updateSerial after dispatch, and this cache
       signature hashes all live HWNDs owned by the app capability.  Avoid apps
       whose blit path intentionally samples global/time-varying diagnostics
       without a local message (Spy/Object/Wait/Pump/Deadlock/SharedBus for now). */
    switch (w->app_type) {
    case APP_CALC:
    case APP_EDITOR:
    case APP_ACCESS:
    case APP_SECTION:
    case APP_CLIPMENU:
    case APP_PAINTLAB:
    case APP_DRAGLAB:
    case APP_CONTROLLAB:
    case APP_SERVICELAB:
    case APP_DIALOGLAB:
    case APP_MDILAB:
        return w->app_hwnd != 0;
    default:
        return 0;
    }
}

static unsigned long long wm_window_backing_signature(WindowManager* wm, const Window* w, int focused)
{
    if (!wm_window_backing_cacheable(w)) return 0;
    unsigned long long h = 1469598103934665603ull;
    h = wm_hash_u64(h, (unsigned)w->w);
    h = wm_hash_u64(h, (unsigned)w->h);
    h = wm_hash_u64(h, (unsigned)focused);
    h = wm_hash_u64(h, (unsigned)w->active);
    h = wm_hash_u64(h, (unsigned)w->closed);
    h = wm_hash_u64(h, (unsigned)w->minimized);
    h = wm_hash_u64(h, (unsigned)w->maximized);
    h = wm_hash_u64(h, (unsigned)w->app_type);
    h = wm_hash_u64(h, (unsigned)w->app_hwnd);
    h = wm_hash_u64(h, (unsigned)w->process_id);
    h = wm_hash_cstr(h, w->title);
    h = wm_hash_cstr(h, w->ipc_status);
    h = wm_hash_cstr(h, w->ipc_class);

    DWORD ownerVisualPid = 0;
    HWND ownerVisualRoot = 0;
    if (w->app_type == APP_IPC_PROXY) {
        ownerVisualPid = w->process_id;
        ownerVisualRoot = w->app_hwnd;
    } else {
        ownerVisualPid = w->app_cap.id;
        ownerVisualRoot = w->app_hwnd;
    }
    h = wm_hash_u64(h, hwnd_get_owner_visual_signature(wm ? wm->mgr : NULL, ownerVisualPid, ownerVisualRoot));

    MYGDI_WINDOW_SNAPSHOT gs;
    memset(&gs, 0, sizeof(gs));
    if (w->app_hwnd && MyGdiGetWindowState(w->app_hwnd, &gs)) {
        h = wm_hash_u64(h, (unsigned)gs.dirty);
        h = wm_hash_u64(h, (unsigned)gs.paintPending);
        h = wm_hash_u64(h, (unsigned)gs.erasePending);
        h = wm_hash_u64(h, (unsigned)gs.internalPaint);
        h = wm_hash_u64(h, (unsigned)gs.invalidateSerial);
        h = wm_hash_u64(h, (unsigned)gs.postedPaints);
        h = wm_hash_u64(h, (unsigned)gs.coalescedInvalidates);
        h = wm_hash_u64(h, (unsigned)gs.dirtyRect.left);
        h = wm_hash_u64(h, (unsigned)gs.dirtyRect.top);
        h = wm_hash_u64(h, (unsigned)gs.dirtyRect.right);
        h = wm_hash_u64(h, (unsigned)gs.dirtyRect.bottom);
    }

    MyProcessHostInfo hi;
    memset(&hi, 0, sizeof(hi));
    if (w->process_id && MyProcessHostGetInfo(w->process_id, &hi)) {
        h = wm_hash_u64(h, (unsigned)hi.gui_msg_sent);
        h = wm_hash_u64(h, (unsigned)hi.gui_msg_received);
        h = wm_hash_u64(h, (unsigned)hi.gui_msg_dispatched);
        h = wm_hash_u64(h, (unsigned)hi.gui_post_request);
        h = wm_hash_u64(h, (unsigned)hi.gui_post_ack);
        h = wm_hash_u64(h, (unsigned)hi.child_hwnd_request);
        h = wm_hash_u64(h, (unsigned)hi.child_hwnd_ack);
        h = wm_hash_u64(h, (unsigned)hi.child_hwnd_count);
        h = wm_hash_u64(h, (unsigned)hi.child_hwnd_command_count);
        h = wm_hash_u64(h, (unsigned)hi.gdi_enabled);
        h = wm_hash_u64(h, (unsigned)hi.gdi_sequence);
        h = wm_hash_u64(h, (unsigned)hi.gdi_paint_count);
        h = wm_hash_u64(h, (unsigned)hi.gdi_command_count);
        h = wm_hash_u64(h, (unsigned)hi.gdi_client_w);
        h = wm_hash_u64(h, (unsigned)hi.gdi_client_h);
        h = wm_hash_u64(h, (unsigned)hi.surface_enabled);
        h = wm_hash_u64(h, (unsigned)hi.surface_mapped);
        h = wm_hash_u64(h, (unsigned)hi.surface_seq);
        h = wm_hash_u64(h, (unsigned)hi.surface_paint_count);
        h = wm_hash_u64(h, (unsigned)hi.surface_dirty_left);
        h = wm_hash_u64(h, (unsigned)hi.surface_dirty_top);
        h = wm_hash_u64(h, (unsigned)hi.surface_dirty_right);
        h = wm_hash_u64(h, (unsigned)hi.surface_dirty_bottom);
        h = wm_hash_cstr(h, hi.gdi_status);
        h = wm_hash_cstr(h, hi.surface_status);
        h = wm_hash_cstr(h, hi.calc_display);
        h = wm_hash_cstr(h, hi.calc_opline);
        h = wm_hash_cstr(h, hi.calc_history_preview);
    }
    return h ? h : 1;
}

static void wm_backing_cache_reset(WmBackingCache* c)
{
    if (!c) return;
    if (c->pixels) free(c->pixels);
    memset(c, 0, sizeof(*c));
}

static int wm_backing_cache_ensure(WmBackingCache* c, int w, int h)
{
    if (!c || w <= 0 || h <= 0) return 0;
    size_t stride = (size_t)w * sizeof(uint32_t);
    size_t size = stride * (size_t)h;
    if (c->pixels && c->w == w && c->h == h && c->stride == stride && c->size == size)
        return 1;
    uint8_t* p = (uint8_t*)realloc(c->pixels, size);
    if (!p) {
        wm_backing_cache_reset(c);
        return 0;
    }
    c->pixels = p;
    c->w = w;
    c->h = h;
    c->stride = stride;
    c->size = size;
    c->valid = 0;
    return 1;
}

static void wm_backing_blit_to_fb(const WmBackingCache* c, Framebuffer* fb, int dst_x, int dst_y)
{
    if (!c || !c->valid || !c->pixels || !fb || !fb->backbuf || c->w <= 0 || c->h <= 0) return;
    int clip_x = 0, clip_y = 0, clip_w = fb->width, clip_h = fb->height;
    fb_current_clip(fb, &clip_x, &clip_y, &clip_w, &clip_h);
    int x1 = dst_x;
    int y1 = dst_y;
    int x2 = dst_x + c->w;
    int y2 = dst_y + c->h;
    int cx2 = clip_x + clip_w;
    int cy2 = clip_y + clip_h;
    if (x1 < clip_x) x1 = clip_x;
    if (y1 < clip_y) y1 = clip_y;
    if (x2 > cx2) x2 = cx2;
    if (y2 > cy2) y2 = cy2;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > fb->width) x2 = fb->width;
    if (y2 > fb->height) y2 = fb->height;
    if (x1 >= x2 || y1 >= y2) return;

    int src_x = x1 - dst_x;
    int src_y = y1 - dst_y;
    size_t bytes = (size_t)(x2 - x1) * sizeof(uint32_t);
    for (int yy = y1; yy < y2; ++yy) {
        const uint8_t* src = c->pixels + (size_t)(src_y + (yy - y1)) * c->stride + (size_t)src_x * sizeof(uint32_t);
        uint8_t* dst = fb->backbuf + (size_t)yy * (size_t)fb->stride + (size_t)x1 * sizeof(uint32_t);
        memcpy(dst, src, bytes);
    }
}

static int wm_render_window_to_backing(WindowManager* wm, Window* w, int focused, WmBackingCache* c)
{
    if (!wm || !w || !c || !c->pixels || c->w <= 0 || c->h <= 0) return 0;
    Framebuffer tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.backbuf = c->pixels;
    tmp.width = c->w;
    tmp.height = c->h;
    tmp.stride = (int)c->stride;
    tmp.fd = -1;
    fb_reset_clip(&tmp);
    fb_clear(&tmp, COLOR(12,14,22));

    Window local = *w;
    local.x = 0;
    local.y = 0;
    draw_window_uncached(wm, &tmp, &local, focused);
    return 1;
}

static void draw_window(WindowManager* wm, Framebuffer* fb, int index, int focused)
{
    if (!wm || !fb || index < 0 || index >= wm->count) return;
    Window* w = &wm->wins[index];
    if (!wm_window_backing_cacheable(w)) {
        draw_window_uncached(wm, fb, w, focused);
        return;
    }

    WmBackingCache* c = &g_WmBackingCache[index];
    unsigned long long sig = wm_window_backing_signature(wm, w, focused);
    if (!sig || !wm_backing_cache_ensure(c, w->w, w->h)) {
        draw_window_uncached(wm, fb, w, focused);
        return;
    }
    if (!c->valid || c->hwnd != w->app_hwnd || c->process_id != w->process_id ||
        c->app_type != w->app_type || c->focused != focused || c->sig != sig) {
        if (!wm_render_window_to_backing(wm, w, focused, c)) {
            draw_window_uncached(wm, fb, w, focused);
            return;
        }
        c->valid = 1;
        c->hwnd = w->app_hwnd;
        c->process_id = w->process_id;
        c->app_type = w->app_type;
        c->focused = focused;
        c->sig = sig;
    }
    wm_backing_blit_to_fb(c, fb, w->x, w->y);
}
void wm_draw(WindowManager* wm, Framebuffer* fb)
{
    if (wm->screen_w != fb->width || wm->screen_h != fb->height ||
        wm->shell_state_w != fb->width || wm->shell_state_h != fb->height) {
        wm->screen_w = fb->width;
        wm->screen_h = fb->height;
        wm_publish_all_shell_state(wm);
    } else {
        wm->screen_w = fb->width;
        wm->screen_h = fb->height;
    }
    draw_background(wm, fb);
    draw_desktop_icons(wm, fb);

    // Build-Version steht final im Debug-Badge von main.c.  Hier nur eine
    // kleine zweite Zeile für Desktopmodus/Wallpaper, damit nichts überläuft.
    char mode[256];
    if (wm->wallpaper_enabled && wm->wallpaper_path[0]) {
        const char* slash = strrchr(wm->wallpaper_path, '/');
        const char* base = slash ? slash + 1 : wm->wallpaper_path;
        char wall_short[48];
        shorten_middle(base, wall_short, sizeof(wall_short));
        snprintf(mode, sizeof(mode), "Icons: %s   Wallpaper: %s", wm_desktop_layout_mode_name(wm), wall_short);
    } else {
        snprintf(mode, sizeof(mode), "Icons: %s", wm_desktop_layout_mode_name(wm));
    }
    fb_rect(fb, 2, 34, fb->width > 500 ? 500 : fb->width - 4, 14, COLOR(0, 45, 65));
    font_draw_str(fb, 8, 37, mode, COLOR(210,230,255));

    for (int i = 0; i < wm->count; i++) {
        if (i==wm->focused || wm->wins[i].closed) continue;
        draw_window(wm, fb, i, 0);
    }
    if (wm->focused>=0 && !wm->wins[wm->focused].closed)
        draw_window(wm, fb, wm->focused, 1);
    MyDrawTopLevelDialogs(fb);
    draw_taskbar(wm, fb);
    draw_menu(wm, fb);
    draw_app_menu_popups(wm, fb);
}

// ── Desktop command routing / Start menu WM_COMMAND path ─────────────

static LPARAM wm_pack_cmd_point(int x, int y)
{
    return (LPARAM)(((uint32_t)(uint16_t)x) | (((uint32_t)(uint16_t)y) << 16));
}

static int wm_cmd_point_x(LPARAM lp) { return (int)(int16_t)((uint32_t)lp & 0xffffu); }
static int wm_cmd_point_y(LPARAM lp) { return (int)(int16_t)(((uint32_t)lp >> 16) & 0xffffu); }

static void wm_command_spawn_point(WindowManager* wm, int invoke_x, int invoke_y,
                                   int start_base_x, int start_base_y,
                                   int* out_x, int* out_y)
{
    if (!out_x || !out_y) return;
    if (wm && wm->menu_from_start) {
        *out_x = start_base_x + wm->count * 18;
        *out_y = start_base_y + wm->count * 18;
    } else {
        *out_x = invoke_x;
        *out_y = invoke_y;
    }
}

static UINT wm_menu_command_from_point(WindowManager* wm, int x, int y)
{
    if (!wm || !wm->menu_open) return 0;
    if (x < wm->menu_x || x >= wm->menu_x + MENU_W) return 0;
    if (y < wm->menu_y + 1) return 0;
    int iy = (y - wm->menu_y - 1) / MENU_ITEM_H;
    if (iy < 0 || iy >= wm_menu_count(wm)) return 0;
    if (wm_menu_item_is_separator(wm, iy)) return 0;
    wm_menu_select_index(wm, iy);
    return wm_menu_command_at_index(wm, iy);
}

static void wm_post_system_command(WindowManager* wm, int idx, UINT cmd, int x, int y)
{
    if (!valid_window_index(wm, idx) || !cmd) return;
    HWND hwnd = window_hwnd(&wm->wins[idx]);
    if (!hwnd) return;
    wm_def_window_proc(wm, hwnd, WM_SYSCOMMAND, (WPARAM)cmd, MAKELPARAM((WORD)x, (WORD)y));
}

static void wm_desktop_command(WindowManager* wm, UINT cmd, int invoke_x, int invoke_y)
{
    if (!wm) return;
    int sx = invoke_x, sy = invoke_y;

    switch (cmd) {
    case ID_START_NEW_TERMINAL:
        if (wm->on_new_window) {
            wm_command_spawn_point(wm, invoke_x, invoke_y, 80, 60, &sx, &sy);
            wm->on_new_window(sx, sy, wm->new_window_ctx);
        }
        break;

    case ID_START_CALC: {
        Capability cap = cap_create(100 + wm->count, "calculator", CAP_IPC);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 120, 80, &sx, &sy);
        MyAppHostLaunch(wm, "calc", sx, sy, "Rechner", NULL, NULL);
        break;
    }

    case ID_START_NEW_TEXTFILE:
        wm_desktop_create_text_file(wm);
        break;

    case ID_START_EDITOR: {
        char full[768];
        snprintf(full, sizeof(full), "%s/Unbenannt.txt", wm->desktop_path);
        Capability cap = cap_create(300 + wm->count, "editor", CAP_FS_READ|CAP_FS_WRITE|CAP_IPC);
        cap_add_path(&cap, wm->desktop_path);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 160, 100, &sx, &sy);
        MyAppHostLaunch(wm, "editor", sx, sy, "Texteditor [NEU]", full, NULL);
        break;
    }

    case ID_START_SPY: {
        Capability cap = cap_create(500 + wm->count, "spy", CAP_IPC|CAP_HOOK|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_WINDOW_SUBSCRIBE|CAP_PROCESS_ENUM);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 220, 120, &sx, &sy);
        MyAppHostLaunch(wm, "spy", sx, sy, "myOS Spy++", NULL, NULL);
        break;
    }

    case ID_START_ACCESSLAB: {
        Capability cap = cap_create(800 + wm->count, "access-lab", CAP_IPC|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_SUBSCRIBE|CAP_PROCESS_ENUM);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 260, 140, &sx, &sy);
        MyAppHostLaunch(wm, "access-lab", sx, sy, "myOS AccessLab", NULL, NULL);
        break;
    }

    case ID_START_PUMPLAB: {
        Capability cap = cap_create(1000 + wm->count, "pump-lab", CAP_IPC|CAP_WINDOW_READ);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 300, 160, &sx, &sy);
        MyAppHostLaunch(wm, "pump-lab", sx, sy, "myOS PumpLab", NULL, NULL);
        break;
    }

    case ID_START_DEADLOCKLAB: {
        Capability cap = cap_create(1200 + wm->count, "deadlock-lab", CAP_IPC|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_SUBSCRIBE);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 340, 180, &sx, &sy);
        MyAppHostLaunch(wm, "deadlock-lab", sx, sy, "myOS DeadlockLab", NULL, NULL);
        break;
    }

    case ID_START_SECTIONLAB: {
        Capability cap = cap_create(1400 + wm->count, "section-lab", CAP_IPC|CAP_SECTION_MAP|CAP_WINDOW_READ);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 380, 200, &sx, &sy);
        MyAppHostLaunch(wm, "section-lab", sx, sy, "myOS SectionLab", NULL, NULL);
        break;
    }

    case ID_START_STATEBUSLAB:
        wm_command_spawn_point(wm, invoke_x, invoke_y, 260, 150, &sx, &sy);
        MyAppHostLaunch(wm, "statebus-lab", sx, sy, "StateBus Publisher [OOP v72]", NULL, NULL);
        MyAppHostLaunch(wm, "statebus-lab", sx + 34, sy + 410, "StateBus Subscriber [OOP v72]", NULL, NULL);
        break;

    case ID_START_HWNDSTATEPROBE:
        wm_command_spawn_point(wm, invoke_x, invoke_y, 300, 170, &sx, &sy);
        MyAppHostLaunch(wm, "hwndstate-lab", sx, sy, "HWND StateProbe [OOP v74/v76 route]", NULL, NULL);
        break;

    case ID_START_SURFACELAB:
        wm_command_spawn_point(wm, invoke_x, invoke_y, 330, 190, &sx, &sy);
        MyAppHostLaunch(wm, "surface-lab", sx, sy, "SurfaceLab [OOP v75.1/v76 route]", NULL, NULL);
        break;

    case ID_START_OBJECTLAB: {
        Capability cap = cap_create(1800 + wm->count, "object-lab", CAP_IPC|CAP_SECTION_MAP|CAP_WINDOW_READ|CAP_PROCESS_ENUM);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 420, 220, &sx, &sy);
        MyAppHostLaunch(wm, "object-lab", sx, sy, "myOS ObjectLab", NULL, NULL);
        break;
    }

    case ID_START_WAITLAB: {
        Capability cap = cap_create(2000 + wm->count, "wait-lab", CAP_IPC|CAP_EXEC|CAP_WINDOW_READ|CAP_PROCESS_ENUM);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 460, 240, &sx, &sy);
        MyAppHostLaunch(wm, "wait-lab", sx, sy, "myOS WaitLab", NULL, NULL);
        break;
    }

    case ID_START_CLIPMENULAB: {
        Capability cap = cap_create(2300 + wm->count, "clip-menu-lab", CAP_IPC|CAP_WINDOW_READ);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 500, 260, &sx, &sy);
        MyAppHostLaunch(wm, "clip-menu-lab", sx, sy, "myOS ClipMenuLab", NULL, NULL);
        break;
    }

    case ID_START_PAINTLAB: {
        Capability cap = cap_create(2400 + wm->count, "paint-lab", CAP_IPC|CAP_WINDOW_READ);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 540, 280, &sx, &sy);
        MyAppHostLaunch(wm, "paint-lab", sx, sy, "myOS PaintLab", NULL, NULL);
        break;
    }

    case ID_START_DRAGLAB: {
        Capability cap = cap_create(2600 + wm->count, "drag-lab", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 580, 300, &sx, &sy);
        MyAppHostLaunch(wm, "drag-lab", sx, sy, "myOS DragLab", NULL, NULL);
        break;
    }

    case ID_START_CONTROLLAB: {
        Capability cap = cap_create(2800 + wm->count, "control-lab", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 620, 320, &sx, &sy);
        MyAppHostLaunch(wm, "control-lab", sx, sy, "myOS ControlLab", NULL, NULL);
        break;
    }

    case ID_START_SERVICELAB: {
        Capability cap = cap_create(3000 + wm->count, "service-lab", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 660, 340, &sx, &sy);
        MyAppHostLaunch(wm, "service-lab", sx, sy, "myOS ServiceLab", NULL, NULL);
        break;
    }

    case ID_START_DIALOGLAB: {
        wm_command_spawn_point(wm, invoke_x, invoke_y, 700, 360, &sx, &sy);
        MyAppHostLaunch(wm, "dialog-lab", sx, sy, "myOS DialogLab", NULL, NULL);
        break;
    }

    case ID_START_MDILAB: {
        Capability cap = cap_create(3400 + wm->count, "mdi-lab", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
        cap_add_target(&cap, 0);
        wm_command_spawn_point(wm, invoke_x, invoke_y, 180, 120, &sx, &sy);
        wm_add_mdilab(wm, sx, sy, "myOS MDILab", cap);
        break;
    }

    case ID_START_DESKTOP_RELOAD:
        wm_desktop_reload(wm);
        break;
    case ID_START_TOGGLE_LAYOUT:
        wm_desktop_toggle_layout(wm);
        break;
    case ID_START_CLEAR_WALLPAPER:
        wm_clear_wallpaper(wm);
        break;
    case ID_START_BG_DARK:
        wm_clear_wallpaper(wm); wm->bg = BG_DARK;
        break;
    case ID_START_BG_BLUE:
        wm_clear_wallpaper(wm); wm->bg = BG_BLUE;
        break;
    case ID_START_BG_PURPLE:
        wm_clear_wallpaper(wm); wm->bg = BG_PURPLE;
        break;
    case ID_START_BG_TEAL:
        wm_clear_wallpaper(wm); wm->bg = BG_TEAL;
        break;
    case ID_START_CLOSE_ALL:
        for (int i = 0; i < wm->count; i++)
            wm_close_window(wm, i);
        break;
    default:
        break;
    }
}

static LRESULT wm_desktop_wndproc(WindowManager* wm, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!wm) return 0;
    if (msg == WM_COMMAND) {
        UINT cmd = (UINT)((DWORD_PTR)wParam & 0xffffu);
        wm_desktop_command(wm, cmd, wm_cmd_point_x(lParam), wm_cmd_point_y(lParam));
        return 0;
    }
    return 0;
}

static void wm_post_desktop_command(WindowManager* wm, UINT cmd, int x, int y)
{
    if (!wm || !cmd) return;
    if (wm->hwnd_desktop && wm->mgr) {
        hwnd_post(wm->mgr, &wm->shell_cap, wm->hwnd_desktop, WM_COMMAND,
                  MAKEWPARAM((WORD)cmd, 0), wm_pack_cmd_point(x, y));
    } else {
        wm_desktop_wndproc(wm, WM_COMMAND, (WPARAM)cmd, wm_pack_cmd_point(x, y));
    }
}

static WindowManager* wm_from_shell_hwnd(HWND hWnd, LPARAM lParam)
{
    if (lParam) {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lParam;
        if (cs && cs->lpCreateParams) {
            SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return (WindowManager*)cs->lpCreateParams;
        }
    }
    return (WindowManager*)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
}

static void wm_publish_shell_state(WindowManager* wm, HWND hwnd, const char* title,
                                   int x, int y, int w, int h, UINT lastMsg, DWORD zOrder)
{
    if (!wm || !wm->mgr || !hwnd) return;
    MyWindowState st;
    memset(&st, 0, sizeof(st));
    wm->state_version++;
    st.cbSize = sizeof(st);
    st.hWnd = hwnd;
    st.ownerPid = wm->shell_cap.id;
    st.ownerTid = wm->shell_cap.id;
    st.rcWindow.left = x;
    st.rcWindow.top = y;
    st.rcWindow.right = x + w;
    st.rcWindow.bottom = y + h;
    st.rcClient = st.rcWindow;
    st.visible = TRUE;
    st.enabled = TRUE;
    st.destroyed = FALSE;
    st.active = (hwnd == wm->hwnd_desktop || hwnd == wm->hwnd_taskbar) ? TRUE : FALSE;
    st.focused = FALSE;
    st.hasCapture = (GetCapture() == hwnd) ? TRUE : FALSE;
    st.flags = MYWSF_VISIBLE | (st.active ? MYWSF_ACTIVE : 0);
    st.dirtyFlags = MYWS_DIRTY_RECT|MYWS_DIRTY_VISIBLE|MYWS_DIRTY_TEXT|MYWS_DIRTY_OWNER|MYWS_DIRTY_ZORDER;
    st.zOrder = zOrder;
    st.style = WS_VISIBLE;
    st.exStyle = 0;
    st.stateVersion = wm->state_version;
    st.updateSerial = wm->state_version;
    st.lastMessage = lastMsg;
    snprintf(st.szTitle, sizeof(st.szTitle), "%s", title ? title : "");
    hwnd_update_window_state(wm->mgr, &st);
}

static void wm_publish_all_shell_state(WindowManager* wm)
{
    if (!wm) return;
    int sw = wm->screen_w > 0 ? wm->screen_w : 640;
    int sh = wm->screen_h > 0 ? wm->screen_h : 480;
    int ty = sh - TASKBAR_H;
    if (ty < 0) ty = 0;
    wm_publish_shell_state(wm, wm->hwnd_desktop, "Desktop", 0, 0, sw, sh, WM_WINDOWPOSCHANGED, 0);
    wm_publish_shell_state(wm, wm->hwnd_taskbar, "Taskbar", 0, ty, sw, TASKBAR_H, WM_WINDOWPOSCHANGED, 0xffffff00u);
    wm_publish_shell_state(wm, wm->hwnd_start_button, "START", 4, ty + 4, START_W, TASKBAR_H - 8, WM_WINDOWPOSCHANGED, 0xffffff10u);
    wm->shell_state_w = sw;
    wm->shell_state_h = sh;
}

static void wm_create_shell_hwnds(WindowManager* wm)
{
    if (!wm || !wm->mgr || wm->hwnd_desktop) return;

    wm->shell_cap = cap_create(77, "shell32/session", CAP_ADMIN|CAP_IPC|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_WINDOW_SUBSCRIBE);
    cap_add_target(&wm->shell_cap, 0);
    MyWinBindRuntime(wm->mgr, &wm->shell_cap);
    MyWinBindDesktop(wm);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = GetModuleHandleA(NULL);

    wc.lpfnWndProc = DesktopWndProc;
    wc.lpszClassName = "#32769";
    RegisterClassExA(&wc);

    wc.lpfnWndProc = TaskbarWndProc;
    wc.lpszClassName = "Shell_TrayWnd";
    RegisterClassExA(&wc);

    int sw = wm->screen_w > 0 ? wm->screen_w : 640;
    int sh = wm->screen_h > 0 ? wm->screen_h : 480;
    int ty = sh - TASKBAR_H;
    if (ty < 0) ty = 0;

    wm->hwnd_desktop = CreateWindowExA(0, "#32769", "Desktop", WS_VISIBLE,
                                       0, 0, sw, sh, 0, 0, wc.hInstance, wm);
    wm->hwnd_taskbar = CreateWindowExA(0, "Shell_TrayWnd", "Taskbar", WS_VISIBLE|WS_CHILD,
                                       0, ty, sw, TASKBAR_H, wm->hwnd_desktop, 0, wc.hInstance, wm);
    wm->hwnd_start_button = CreateWindowExA(0, "BUTTON", "START", WS_VISIBLE|WS_CHILD,
                                            4, 4, START_W, TASKBAR_H - 8,
                                            wm->hwnd_taskbar, (HMENU)(uintptr_t)ID_TASKBAR_START,
                                            wc.hInstance, NULL);

    wm_publish_all_shell_state(wm);
    printf("[SHELL v77] Desktop HWND=%u class=#32769 Taskbar HWND=%u class=Shell_TrayWnd StartButton HWND=%u\n",
           wm->hwnd_desktop, wm->hwnd_taskbar, wm->hwnd_start_button);
}

static void wm_desktop_handle_lbutton(WindowManager* wm, int x, int y)
{
    if (!wm) return;

    if (wm->menu_open) {
        int kind = wm->menu_kind;
        int target_idx = wm->menu_target_idx;
        UINT cmd = wm_menu_command_from_point(wm, x, y);
        wm_close_menu_loop(wm, cmd ? 0 : 1);
        if (cmd) {
            if (kind == MENU_KIND_SYSTEM) wm_post_system_command(wm, target_idx, cmd, x, y);
            else wm_post_desktop_command(wm, cmd, x, y);
        }
        return;
    }

    int icon_idx = desktop_icon_at(wm, x, y);
    if (icon_idx >= 0) {
        DesktopIcon* ic = &wm->desktop_icons[icon_idx];
        long long now = desktop_now_ms();
        int is_double = (wm->desktop_last_click_idx == icon_idx &&
                         now - wm->desktop_last_click_ms <= 420);

        wm->desktop_selected = icon_idx;
        wm->desktop_last_click_idx = icon_idx;
        wm->desktop_last_click_ms = now;

        if (is_double && !ic->is_dir) {
            if (desktop_is_wallpaper_file(ic->name)) {
                wm_set_wallpaper(wm, ic->path);
                wm->desktop_drag_idx = -1;
                wm->desktop_dragging = 0;
                return;
            }
            char short_name[40];
            shorten_middle(ic->name, short_name, sizeof(short_name));
            char title[64];
            snprintf(title, sizeof(title), "Editor [%s]", short_name);
            MyAppHostLaunch(wm, "editor", ic->x + 96, ic->y + 10, title, ic->path, NULL);
            wm->desktop_drag_idx = -1;
            wm->desktop_dragging = 0;
            return;
        }

        wm->desktop_drag_idx = icon_idx;
        wm->desktop_dragging = 1;
        wm->desktop_drag_ox = x - ic->x;
        wm->desktop_drag_oy = y - ic->y;
        return;
    }

    wm->desktop_selected = -1;
    wm->desktop_drag_idx = -1;
    wm->desktop_dragging = 0;
}

static void wm_taskbar_handle_lbutton(WindowManager* wm, int x, int y)
{
    (void)y;
    if (!wm) return;
    int bx = 4 + START_W + TASK_BTN_GAP;
    for (int i = 0; i < wm->count; i++) {
        if (wm->wins[i].closed) continue;
        if (x >= bx && x < bx + TASK_BTN_W) {
            if (i == wm->focused && !wm->wins[i].minimized) {
                wm->wins[i].minimized = 1;
                wm_publish_window_state_event(wm, i, WM_SHOWWINDOW);
            } else {
                int old_focus = wm->focused;
                if (old_focus >= 0) wm->wins[old_focus].active = 0;
                wm->wins[i].minimized = 0;
                wm->focused = i;
                wm->wins[i].active = 1;
                if (old_focus >= 0 && old_focus != i) wm_publish_window_state_event(wm, old_focus, WM_ACTIVATE);
                wm_publish_window_state_event(wm, i, WM_SHOWWINDOW);
                wm_publish_window_state_event(wm, i, WM_ACTIVATE);
            }
            return;
        }
        bx += TASK_BTN_W + TASK_BTN_GAP;
    }
    wm->menu_open = 0;
}

static LRESULT CALLBACK DesktopWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    WindowManager* wm = NULL;
    if (Msg == WM_NCCREATE) {
        wm = wm_from_shell_hwnd(hWnd, lParam);
        return TRUE;
    }
    wm = wm_from_shell_hwnd(hWnd, 0);
    if (!wm) return DefWindowProcA(hWnd, Msg, wParam, lParam);
    wm->shell_msg_count++;

    switch (Msg) {
    case WM_CREATE:
        return 0;
    case WM_RBUTTONDOWN:
        if (wm->menu_open) {
            int kind = wm->menu_kind;
            int target_idx = wm->menu_target_idx;
            UINT cmd = wm_menu_command_from_point(wm, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            wm_close_menu_loop(wm, cmd ? 0 : 1);
            if (cmd) {
                if (kind == MENU_KIND_SYSTEM) wm_post_system_command(wm, target_idx, cmd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                else wm_post_desktop_command(wm, cmd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
        } else {
            wm_open_menu(wm, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0);
        }
        return 0;
    case WM_LBUTTONDOWN:
        wm_desktop_handle_lbutton(wm, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_COMMAND:
        wm->shell_cmd_count++;
        wm_desktop_wndproc(wm, WM_COMMAND, wParam, lParam);
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static int wm_point_in_start_button_screen(WindowManager* wm, int x, int y)
{
    if (!wm) return 0;
    int sh = wm->screen_h > 0 ? wm->screen_h : 480;
    int ty = sh - TASKBAR_H;
    return (x >= 4 && x < 4 + START_W &&
            y >= ty + 4 && y < ty + 4 + TASKBAR_H - 8);
}

static void wm_taskbar_invoke_start(WindowManager* wm)
{
    if (!wm) return;
    wm->shell_cmd_count++;
    if (wm->menu_open && wm->menu_from_start) {
        wm_close_menu_loop(wm, 1);
    } else {
        wm_open_menu(wm, 4, (wm->screen_h > 0 ? wm->screen_h : 480) - TASKBAR_H, 1);
    }
}

static LRESULT CALLBACK TaskbarWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    WindowManager* wm = NULL;
    if (Msg == WM_NCCREATE) {
        wm = wm_from_shell_hwnd(hWnd, lParam);
        return TRUE;
    }
    wm = wm_from_shell_hwnd(hWnd, 0);
    if (!wm) return DefWindowProcA(hWnd, Msg, wParam, lParam);
    wm->shell_msg_count++;

    switch (Msg) {
    case WM_CREATE:
        return 0;
    case WM_COMMAND:
        wm->shell_cmd_count++;
        if (LOWORD(wParam) == ID_TASKBAR_START) {
            wm_taskbar_invoke_start(wm);
            return 0;
        }
        PostMessageA(wm->hwnd_desktop, WM_COMMAND, wParam, lParam);
        return 0;
    case WM_LBUTTONDOWN:
        /* v86.2 safety net: if START child hit-testing ever falls through to
           Shell_TrayWnd, still route through the same WM_COMMAND contract. */
        if (GET_X_LPARAM(lParam) >= 4 && GET_X_LPARAM(lParam) < 4 + START_W &&
            GET_Y_LPARAM(lParam) >= 4 && GET_Y_LPARAM(lParam) < 4 + TASKBAR_H - 8) {
            wm_taskbar_invoke_start(wm);
            return 0;
        }
        wm_taskbar_handle_lbutton(wm, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static LPARAM wm_shell_lparam_for_hwnd(WindowManager* wm, HWND hwnd, int x, int y)
{
    (void)wm;
    POINT pt;
    pt.x = x; pt.y = y;
    if (!ScreenToClient(hwnd, &pt)) { pt.x = x; pt.y = y; }
    return MAKELPARAM((WORD)pt.x, (WORD)pt.y);
}

static int wm_shell_post_mouse(WindowManager* wm, HWND hwnd, UINT msg, int x, int y, WPARAM keys)
{
    if (!wm || !wm->mgr || !hwnd) return 0;
    LPARAM lp = wm_shell_lparam_for_hwnd(wm, hwnd, x, y);
    return hwnd_post(wm->mgr, &wm->shell_cap, hwnd, msg, keys, lp) == 0;
}

static HWND wm_shell_hit_taskbar(WindowManager* wm, int x, int y)
{
    if (!wm || !wm->hwnd_taskbar) return 0;
    int sh = wm->screen_h > 0 ? wm->screen_h : 480;
    int ty = sh - TASKBAR_H;
    if (y < ty) return 0;

    /* v86.2: START had become fragile because Shell_TrayWnd is both a real
       HWND and still has compositor-drawn chrome.  ChildWindowFromPoint() is
       the desired Win32 path, but if the child-state mirror is stale or the
       shell child coords are temporarily out of sync, a click in the START
       rectangle fell back to Shell_TrayWnd and wm_taskbar_handle_lbutton() does
       not open the menu.  Keep the architecture: raw input still targets the
       BUTTON child, and the BUTTON still notifies the taskbar via
       WM_COMMAND(ID_TASKBAR_START).  This is only a deterministic shell-hit
       fallback for the real START child HWND. */
    POINT pt; pt.x = x; pt.y = y;
    if (!ScreenToClient(wm->hwnd_taskbar, &pt)) {
        pt.x = x;
        pt.y = y - ty;
    }

    HWND child = ChildWindowFromPoint(wm->hwnd_taskbar, pt);
    if (child) return child;

    if (wm->hwnd_start_button &&
        pt.x >= 4 && pt.x < 4 + START_W &&
        pt.y >= 4 && pt.y < 4 + TASKBAR_H - 8) {
        return wm->hwnd_start_button;
    }

    return wm->hwnd_taskbar;
}

static int wm_class_is(HWND hwnd, const char* expected)
{
    if (!hwnd || !expected) return 0;
    char cls[64];
    if (GetClassNameA(hwnd, cls, sizeof(cls)) <= 0) return 0;
    return strcmp(cls, expected) == 0;
}

/* v138: physical MDI caption route.
   v137 made DefMDIChildProcA understand the caption contract, but the real
   Linux-mouse path still relied on the generic client WM_LBUTTONDOWN fallback.
   That meant a manual click could be interpreted as ordinary client input before
   the non-client MDI caption path ever became observable.  Route the exact
   physical screen point through the MDI child WM_NCHITTEST contract first; when
   it says HTCAPTION, deliver WM_NCLBUTTONDOWN and consume the raw down so main.c
   cannot synthesize a second WM_LBUTTONDOWN for the same physical click. */
static int wm_try_mdi_caption_mouse_down_for_index(WindowManager* wm, int idx, int x, int y)
{
    if (!wm || !valid_window_index(wm, idx)) return 0;

    Window* w = &wm->wins[idx];
    if (!w->app_hwnd || w->closed || w->minimized) return 0;
    if (x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h) return 0;

    HWND mdiClient = 0;
    HWND target = wm_mdi_child_from_screen_nc(w->app_hwnd, x, y, &mdiClient);
    if (!target || target == w->app_hwnd) return 0;

    HWND parent = GetParent(target);
    if (!parent || parent != mdiClient || !wm_class_is(parent, "MDICLIENT")) return 0;

    LPARAM screen_lp = MAKELPARAM((WORD)x, (WORD)y);
    DWORD ownerPid = 0;
    GetWindowThreadProcessId(target, &ownerPid);

    /* v141: do not let the MDI caption drag contract depend solely on the
       current client/subfocus route.  The user sees the caption as the top band
       of the child window rectangle; use that same visual hit region as a
       compositor-side guard, then still deliver the canonical NC message to the
       child WndProc. */
    int visualCaption = wm_mdi_visual_caption_hit(target, x, y);

    BOOL entered = FALSE;
    if (ownerPid) entered = MyWinEnterProcessContext(ownerPid);
    LRESULT ht = SendMessageA(target, WM_NCHITTEST, 0, screen_lp);
    if (ht == HTCAPTION || visualCaption) {
        int old_focus = wm->focused;
        if (old_focus >= 0) wm->wins[old_focus].active = 0;
        wm->focused = idx;
        w->active = 1;
        if (old_focus >= 0 && old_focus != idx) wm_publish_window_state_event(wm, old_focus, WM_ACTIVATE);
        wm_publish_window_state_event(wm, idx, WM_ACTIVATE);

        /* v144: pin the compositor drag identity before calling into USER32.
           Manual first-launch testing showed a path that only became reliable
           after an Escape/WM_MDIDESTROY side-effect.  The raw drag latch must
           be derived solely from the physical hit we just resolved, not from a
           later capture/focus side effect inside DefMDIChildProcA. */
        wm_mdi_raw_drag_begin(target, x, y);
        SendMessageA(target, WM_NCLBUTTONDOWN, (WPARAM)HTCAPTION, screen_lp);
        if (entered) MyWinLeaveProcessContext();
        return 1;
    }
    if (entered) MyWinLeaveProcessContext();
    return 0;
}

static int wm_try_mdi_caption_mouse_down(WindowManager* wm, int x, int y)
{
    if (!wm) return 0;

    int idx = top_window_at(wm, x, y);
    if (wm_try_mdi_caption_mouse_down_for_index(wm, idx, x, y))
        return 1;

    /* v144: defensive first-launch route.  If focus/top_window_at is stale for
       the first MDILab instance, still resolve the MDI caption against visible
       app windows in front-to-back WindowManager order.  This mirrors what the
       user can see: the topmost visible MDI child caption under the cursor owns
       the drag, regardless of whether the frame focus was already primed. */
    for (int i = wm->count - 1; i >= 0; --i) {
        if (i == idx) continue;
        if (wm_try_mdi_caption_mouse_down_for_index(wm, i, x, y))
            return 1;
    }
    return 0;
}

// ── Maus/Input route ─────────────────────────

static void wm_ensure_session_input_runtime(WindowManager* wm)
{
    if (!wm || !wm->mgr) return;

    /* v146: keep the raw-input broker contract in one USER32 helper.  The v145
       bug was triggered by a completely empty TLS runtime; a weaker variant is
       an existing runtime without CAP_WINDOW_READ/CONTROL (for example a lab or
       app context calling a compositor endpoint).  Raw mouse routing is session
       broker work, so it must upgrade to the shell/session runtime instead of
       silently trusting whatever thread-local capability happened to be active. */
    if (!MyWinEnsureSessionInputRuntime(wm->mgr, wm, &wm->shell_cap)) {
        fprintf(stderr, "[v146][RUNTIME] raw-input broker runtime bootstrap failed\n");
    }
}

static WPARAM wm_input_keys_for_button(WindowManager* wm, int btn, int down)
{
    WPARAM wp = 0;
    if (down) {
        if (btn == 0) wp |= MK_LBUTTON;
        if (btn == 1) wp |= MK_RBUTTON;
    }
    if (wm && wm->shift_held) wp |= MK_SHIFT;
    if (wm && wm->ctrl_held)  wp |= MK_CONTROL;
    return wp;
}

static int wm_route_nonclient_down(WindowManager* wm, int x, int y)
{
    int idx = top_window_at(wm, x, y);
    if (idx < 0) return 0;

    wm->desktop_drag_idx = -1;
    wm->desktop_dragging = 0;

    int old_focus = wm->focused;
    if (old_focus >= 0) wm->wins[old_focus].active = 0;
    wm->focused = idx;
    wm->wins[idx].active = 1;
    if (old_focus >= 0 && old_focus != idx) wm_publish_window_state_event(wm, old_focus, WM_ACTIVATE);
    wm_publish_window_state_event(wm, idx, WM_ACTIVATE);

    Window* w = &wm->wins[idx];
    HWND hwnd = window_hwnd(w);
    if (!hwnd) return 0;

    LPARAM screen_lp = MAKELPARAM((WORD)x, (WORD)y);
    LRESULT ht = wm_def_window_proc(wm, hwnd, WM_NCHITTEST, 0, screen_lp);
    wm_publish_window_state_event(wm, idx, WM_NCHITTEST);

    if (ht == HTCLIENT || ht == HTNOWHERE || ht == HTTRANSPARENT) {
        // Client area: focus has been updated, caller/main.c posts the real
        // WM_LBUTTONDOWN to the app/control HWND with client coordinates.
        return 0;
    }

    // v78: non-client actions are now expressed as Win32-shaped messages:
    // WM_NCHITTEST -> WM_NCLBUTTONDOWN -> WM_SYSCOMMAND/DefWindowProc.
    // We execute the default handler on the parent USER/shell side because the
    // classic frame belongs to the compositor, not to an OOP child process yet.
    wm->nc_down_hwnd = hwnd;
    wm->nc_down_ht = (int)ht;
    wm_def_window_proc(wm, hwnd, WM_NCLBUTTONDOWN, (WPARAM)ht, screen_lp);
    return 1;
}

int wm_route_raw_mouse_button_down(WindowManager* wm, int x, int y, int btn)
{
    if (!wm) return 0;
    wm_ensure_session_input_runtime(wm);
    WPARAM keys = wm_input_keys_for_button(wm, btn, 1);

    if (btn == 1) {
        wm->desktop_drag_idx = -1;
        wm->desktop_dragging = 0;

        if (wm->menu_open) {
            if (wm->menu_kind == MENU_KIND_APP) { wm_close_app_menu_loop(wm, 1); return 1; }
            wm_shell_post_mouse(wm, wm->hwnd_desktop, WM_RBUTTONDOWN, x, y, keys);
            return 1;
        }

        {
            HWND owner = wm_get_foreground_hwnd(wm);
            HWND dlgHit = 0;
            if (owner && MyTopLevelDialogHitTest(owner, x, y, &dlgHit)) {
                wm->desktop_drag_idx = -1;
                wm->desktop_dragging = 0;
                return 0;
            }
        }

        int idx = top_window_at(wm, x, y);
        if (idx >= 0) {
            Window* w = &wm->wins[idx];
            HWND hwnd = window_hwnd(w);
            LPARAM screen_lp = MAKELPARAM((WORD)x, (WORD)y);
            LRESULT ht = wm_def_window_proc(wm, hwnd, WM_NCHITTEST, 0, screen_lp);
            if (ht != HTCLIENT && ht != HTNOWHERE && ht != HTTRANSPARENT) {
                wm_def_window_proc(wm, hwnd, WM_NCRBUTTONDOWN, (WPARAM)ht, screen_lp);
                return 1;
            }
            return 0; // let main.c deliver WM_RBUTTONDOWN to app-client later if needed
        }

        wm_shell_post_mouse(wm, wm->hwnd_desktop, WM_RBUTTONDOWN, x, y, keys);
        return 1;
    }

    // Popup/menu capture is owned by the Desktop WndProc in v77; v101 app menus
    // are owned by the foreground app HWND and use real HMENU snapshots.
    if (wm->menu_open) {
        if (wm->menu_kind == MENU_KIND_APP) return wm_app_menu_mouse_down(wm, x, y);
        wm_shell_post_mouse(wm, wm->hwnd_desktop, WM_LBUTTONDOWN, x, y, keys);
        return 1;
    }

    /* v88.2: owned top-level dialogs live outside the owner client rect.
       Let main.c deliver the normal client message to the #32770/child HWND
       instead of treating this as a desktop/taskbar click. */
    {
        HWND owner = wm_get_foreground_hwnd(wm);
        HWND dlgHit = 0;
        if (owner && MyTopLevelDialogHitTest(owner, x, y, &dlgHit)) {
            wm->desktop_drag_idx = -1;
            wm->desktop_dragging = 0;
            return 0;
        }
    }

    {
        int menuWin = -1, top = -1;
        if (wm_app_menu_bar_hit(wm, x, y, &menuWin, &top)) {
            wm->desktop_drag_idx = -1;
            wm->desktop_dragging = 0;
            return wm_app_menu_click_top_item(wm, menuWin, top);
        }
    }

    if (wm_point_in_start_button_screen(wm, x, y)) {
        /* v86.2: deterministic START command path.  The START HWND remains a
           real BUTTON child of Shell_TrayWnd, but raw input may arrive while the
           shell's child-state mirror/capture state is stale.  Use the same
           Taskbar WM_COMMAND handler synchronously instead of letting the click
           disappear into an async BUTTON path. */
        wm->desktop_drag_idx = -1;
        wm->desktop_dragging = 0;
        wm_taskbar_invoke_start(wm);
        return 1;
    }

    HWND taskTarget = wm_shell_hit_taskbar(wm, x, y);
    if (taskTarget) {
        wm->desktop_drag_idx = -1;
        wm->desktop_dragging = 0;
        wm_shell_post_mouse(wm, taskTarget, WM_LBUTTONDOWN, x, y, keys);
        return 1;
    }

    /* v138: MDI child captions are visually inside the app client, but Win32
       treats caption dragging as a non-client contract on the child itself.
       Try that exact path before the generic top-level frame/client split. */
    if (wm_try_mdi_caption_mouse_down(wm, x, y))
        return 1;

    // App frame/client hit.  Non-client is still a v78 target.  For real
    // app-client/control pixels, deliver the client mouse down synchronously so
    // USER32 controls establish focus/capture before the matching evdev up can
    // arrive.  The older queued fallback in main.c remains only as a safety net.
    int idx = top_window_at(wm, x, y);
    if (idx >= 0) {
        int nc = wm_route_nonclient_down(wm, x, y);
        if (nc) return 1;
        if (wm_deliver_app_client_mouse_sync(wm, WM_LBUTTONDOWN, x, y, keys)) return 1;
        return 0;
    }

    // Desktop icons/background now belong to DesktopWndProc (#32769).
    wm_shell_post_mouse(wm, wm->hwnd_desktop, WM_LBUTTONDOWN, x, y, keys);
    return 1;
}

int wm_route_raw_mouse_button_up(WindowManager* wm, int x, int y, int btn)
{
    if (!wm) return 0;
    wm_ensure_session_input_runtime(wm);
    WPARAM keys = wm_input_keys_for_button(wm, btn, 0);
    HWND cap = GetCapture();

    if (btn == 0 && wm_try_mdi_caption_mouse_up(wm, x, y))
        return 1;

    if (btn == 0 && wm_point_in_start_button_screen(wm, x, y)) {
        if (cap == wm->hwnd_start_button) ReleaseCapture();
        return 1;
    }

    if (cap && (cap == wm->hwnd_start_button || cap == wm->hwnd_taskbar ||
                cap == wm->hwnd_desktop || IsChild(wm->hwnd_taskbar, cap) ||
                IsChild(wm->hwnd_desktop, cap))) {
        wm_shell_post_mouse(wm, cap, btn == 1 ? WM_RBUTTONUP : WM_LBUTTONUP, x, y, keys);
        return 1;
    }

    if (wm->nc_down_hwnd) {
        HWND nc = wm->nc_down_hwnd;
        WPARAM ht = (WPARAM)(wm->nc_down_ht ? wm->nc_down_ht : HTCAPTION);
        wm->nc_down_hwnd = 0;
        wm->nc_down_ht = 0;
        if (wm_find_hwnd(wm, nc) >= 0)
            wm_def_window_proc(wm, nc, WM_NCLBUTTONUP, ht, MAKELPARAM((WORD)x, (WORD)y));
        else
            wm_mouse_up(wm);
        return 1;
    }

    int was_dragging = (wm->drag_mode != DRAG_NONE) || wm->desktop_dragging;
    if (was_dragging) {
        wm_mouse_up(wm);
        return 1;
    }

    HWND taskTarget = wm_shell_hit_taskbar(wm, x, y);
    if (taskTarget) {
        wm_shell_post_mouse(wm, taskTarget, btn == 1 ? WM_RBUTTONUP : WM_LBUTTONUP, x, y, keys);
        return 1;
    }

    if (btn == 0 && wm_deliver_app_client_mouse_sync(wm, WM_LBUTTONUP, x, y, keys))
        return 1;

    wm_mouse_up(wm);
    return 0;
}

int wm_mouse_move(WindowManager* wm, int x, int y)
{
    wm_ensure_session_input_runtime(wm);
    if (wm_try_mdi_caption_mouse_move(wm, x, y))
        return 1;

    if (wm && wm->menu_open) {
        if (wm->menu_kind == MENU_KIND_APP) {
            wm_app_menu_mouse_move(wm, x, y);
            return 1;
        } else if (x >= wm->menu_x && x < wm->menu_x + MENU_W && y >= wm->menu_y + 1) {
            int iy = (y - wm->menu_y - 1) / MENU_ITEM_H;
            if (iy >= 0 && iy < wm_menu_count(wm) && !wm_menu_item_is_separator(wm, iy)) {
                wm_menu_select_index(wm, iy);
                return 1;
            }
        }
    }
    if (wm->desktop_dragging && wm->desktop_drag_idx >= 0 &&
        wm->desktop_drag_idx < wm->desktop_icon_count) {
        DesktopIcon* ic = &wm->desktop_icons[wm->desktop_drag_idx];
        ic->x = x - wm->desktop_drag_ox;
        ic->y = y - wm->desktop_drag_oy;
        desktop_clamp_icon(wm, ic);
        return 1;
    }

    if (wm->drag_idx < 0) return 0;
    Window* w = &wm->wins[wm->drag_idx];

    if (wm->drag_mode == DRAG_MOVE) {
        int nx = x - wm->drag_ox;
        int ny = y - wm->drag_oy;
        wm_set_window_pos_ex(wm, wm->drag_idx, HWND_TOP, nx, ny, w->w, w->h,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 1;
    }

    int min_w = 120, min_h = 80;
    wm_min_size_for_window(w, &min_w, &min_h);

    int dx = x - wm->drag_ox;
    int dy = y - wm->drag_oy;
    int left   = wm->drag_orig_x;
    int top    = wm->drag_orig_y;
    int right  = wm->drag_orig_x + wm->drag_orig_w;
    int bottom = wm->drag_orig_y + wm->drag_orig_h;

    switch (wm->drag_mode) {
    case DRAG_RESIZE_L:
    case DRAG_RESIZE_TL:
    case DRAG_RESIZE_BL:
        left = wm->drag_orig_x + dx;
        if (right - left < min_w) left = right - min_w;
        break;
    case DRAG_RESIZE_R:
    case DRAG_RESIZE_TR:
    case DRAG_RESIZE_RB:
        right = wm->drag_orig_x + wm->drag_orig_w + dx;
        if (right - left < min_w) right = left + min_w;
        break;
    default:
        break;
    }

    switch (wm->drag_mode) {
    case DRAG_RESIZE_T:
    case DRAG_RESIZE_TL:
    case DRAG_RESIZE_TR:
        top = wm->drag_orig_y + dy;
        if (bottom - top < min_h) top = bottom - min_h;
        break;
    case DRAG_RESIZE_B:
    case DRAG_RESIZE_BL:
    case DRAG_RESIZE_RB:
        bottom = wm->drag_orig_y + wm->drag_orig_h + dy;
        if (bottom - top < min_h) bottom = top + min_h;
        break;
    default:
        break;
    }

    wm_set_window_pos_ex(wm, wm->drag_idx, HWND_TOP, left, top, right - left, bottom - top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    return 1;
}

void wm_mouse_up(WindowManager* wm)
{
    if (wm->desktop_dragging && wm->desktop_drag_idx >= 0 &&
        wm->desktop_drag_idx < wm->desktop_icon_count) {
        DesktopIcon* ic = &wm->desktop_icons[wm->desktop_drag_idx];
        if (wm->desktop_layout_mode == DESKTOP_LAYOUT_GRID)
            desktop_snap_icon(wm, ic);
        else
            desktop_clamp_icon(wm, ic);
        desktop_save_layout(wm);
    }
    int exit_idx = wm->drag_idx;
    if (valid_window_index(wm, exit_idx) && wm->drag_mode != DRAG_NONE) {
        HWND hwnd = window_hwnd(&wm->wins[exit_idx]);
        if (hwnd && GetCapture() == hwnd) ReleaseCapture();
        wm_publish_window_state_event(wm, exit_idx, WM_EXITSIZEMOVE);
    }

    wm->desktop_dragging = 0;
    wm->desktop_drag_idx = -1;
    wm->drag_mode = DRAG_NONE;
    wm->drag_idx  = -1;
}


int wm_menu_handle_key(WindowManager* wm, int keycode)
{
    if (!wm || !wm->menu_open) return 0;
    if (wm->menu_kind == MENU_KIND_APP) return wm_app_menu_handle_key(wm, keycode);
    wm->menu_key_count++;

    switch (keycode) {
    case KEY_UP:
        wm_menu_select_index(wm, wm_menu_next_selectable(wm, wm->menu_selected, -1));
        return 1;
    case KEY_DOWN:
        wm_menu_select_index(wm, wm_menu_next_selectable(wm, wm->menu_selected, 1));
        return 1;
    case KEY_HOME:
        wm_menu_select_index(wm, wm_menu_first_selectable(wm));
        return 1;
    case KEY_END: {
        int first = wm_menu_first_selectable(wm);
        wm_menu_select_index(wm, wm_menu_next_selectable(wm, first, -1));
        return 1;
    }
    case KEY_ESC:
        wm_close_menu_loop(wm, 1);
        return 1;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_SPACE: {
        int sel = wm->menu_selected;
        int x = wm->menu_x + 12;
        int y = wm->menu_y + 1 + (sel >= 0 ? sel : 0) * MENU_ITEM_H + MENU_ITEM_H / 2;
        wm_menu_invoke_index(wm, sel, x, y);
        return 1;
    }
    default: {
        int hot = wm_menu_key_to_hotletter(keycode);
        if (hot) {
            int idx = wm_menu_find_hotletter(wm, hot);
            if (idx >= 0) {
                wm_menu_select_index(wm, idx);
                int x = wm->menu_x + 12;
                int y = wm->menu_y + 1 + idx * MENU_ITEM_H + MENU_ITEM_H / 2;
                wm_menu_invoke_index(wm, idx, x, y);
                return 1;
            }
        }
        return 1; // menu loop owns keyboard while active
    }
    }
}

// ── Tastatur ─────────────────────────────────

void wm_key(WindowManager* wm, int keycode, int down)
{
    if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
        wm->shift_held = down; return;
    }
    if (!down) return;
    if (wm->focused < 0 || wm->wins[wm->focused].closed
        || wm->wins[wm->focused].minimized) return;
    if (wm->wins[wm->focused].app_type != APP_TERMINAL || !wm->wins[wm->focused].term) return;
    term_keycode(wm->wins[wm->focused].term, keycode, wm->shift_held);
}

void wm_blink(WindowManager* wm)
{
    if (wm->focused >= 0 && !wm->wins[wm->focused].closed &&
        wm->wins[wm->focused].app_type == APP_TERMINAL && wm->wins[wm->focused].term)
        wm->wins[wm->focused].term->blink ^= 1;
}


static void wm_pump_ipc_proxy_posts(WindowManager* wm)
{
    if (!wm || !wm->mgr) return;
    for (int i = 0; i < wm->count; i++) {
        Window* w = &wm->wins[i];
        if (w->closed || w->app_type != APP_IPC_PROXY || !w->process_id) continue;
        for (;;) {
            MyProcessHostCreateWindowRequest req;
            if (!MyProcessHostTakeCreateWindowRequest(w->process_id, &req)) break;
            Capability sender = w->app_cap;
            if (!sender.id) sender = cap_create(w->process_id, "ipc-gui-child", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_WINDOW_SUBSCRIBE);
            int entered = MyWinEnterProcessContext(w->process_id) ? 1 : 0;
            int index = wm_add_ipc_proxy(wm, req.x, req.y, req.w, req.h, req.title, req.class_name, sender, req.linux_pid, "parent created secondary CreateWindowExA from IPC", req.owner_hwnd, req.style, req.ex_style);
            if (entered) MyWinLeaveProcessContext();
            HWND createdHwnd = (index >= 0 && index < wm->count) ? wm->wins[index].app_hwnd : 0;
            if (index >= 0 && index < wm->count) {
                Window* nw = &wm->wins[index];
                nw->process_id = w->process_id;
                nw->thread_id = w->thread_id;
                nw->process_handle = 0;
                nw->thread_handle = 0;
                nw->process_handle_owner_pid = 0;
                snprintf(nw->image_name, sizeof(nw->image_name), "%s", w->image_name[0] ? w->image_name : "apphost-child");
            }
            MyProcessHostAckCreateWindow(w->process_id, createdHwnd, (DWORD)(index >= 0 ? index : 0), index >= 0,
                                         index >= 0 ? "parent CreateWindowExA secondary OK" : "parent CreateWindowExA secondary failed");
            if (index < 0) break;
        }
        for (;;) {
            MyProcessHostWindowMessage req;
            if (!MyProcessHostTakeEnableWindowRequest(w->process_id, &req)) break;
            HWND target = req.hwnd ? req.hwnd : w->app_hwnd;
            BOOL ok = FALSE;
            if (target && MyWinEnterProcessContext(w->process_id)) {
                (void)EnableWindow(target, req.wparam ? TRUE : FALSE);
                ok = TRUE;
                MyWinLeaveProcessContext();
            }
            MyProcessHostAckEnableWindow(w->process_id, ok ? TRUE : FALSE, ok ? "parent EnableWindow broker OK" : "parent EnableWindow broker failed");
            if (!ok) break;
        }
        for (;;) {
            MyProcessHostCreateChildWindowBatchRequest batch;
            if (!MyProcessHostTakeCreateChildWindowBatchRequest(w->process_id, &batch)) break;
            HWND hwnds[MYOS_IPC_MAX_CHILD_CONTROLS];
            UINT ids[MYOS_IPC_MAX_CHILD_CONTROLS];
            memset(hwnds, 0, sizeof(hwnds));
            memset(ids, 0, sizeof(ids));
            BOOL allOk = batch.count ? TRUE : FALSE;
            BOOL entered = FALSE;
            if (batch.count && batch.count <= MYOS_IPC_MAX_CHILD_CONTROLS && MyWinEnterProcessContext(w->process_id)) {
                entered = TRUE;
                for (UINT bi = 0; bi < batch.count; ++bi) {
                    MyProcessHostCreateChildWindowRequest* req = &batch.items[bi];
                    ids[bi] = req->id;
                    HWND child = 0;
                    if (req->parent_hwnd) {
                        WNDCLASSEXA wc;
                        memset(&wc, 0, sizeof(wc));
                        wc.cbSize = sizeof(wc);
                        wc.style = CS_HREDRAW | CS_VREDRAW;
                        wc.lpfnWndProc = ipcproxy_wndproc;
                        wc.hInstance = GetModuleHandleA(NULL);
                        wc.lpszClassName = req->class_name[0] ? req->class_name : "BUTTON";
                        RegisterClassExA(&wc);
                        child = CreateWindowExA(req->ex_style, wc.lpszClassName, req->title,
                                                req->style | WS_CHILD | WS_VISIBLE,
                                                req->x, req->y, req->w, req->h,
                                                req->parent_hwnd, (HMENU)(uintptr_t)req->id, wc.hInstance, NULL);
                    }
                    hwnds[bi] = child;
                    if (!child) allOk = FALSE;
                }
            } else {
                allOk = FALSE;
            }
            if (entered) MyWinLeaveProcessContext();
            MyProcessHostAckCreateChildWindowBatch(w->process_id, hwnds, ids, batch.count, allOk,
                                                   allOk ? "parent created child HWND batch" : "parent child HWND batch partially failed");
            if (!allOk) break;
        }
        for (;;) {
            MyProcessHostCreateChildWindowRequest req;
            if (!MyProcessHostTakeCreateChildWindowRequest(w->process_id, &req)) break;
            HWND child = 0;
            if (req.parent_hwnd && MyWinEnterProcessContext(w->process_id)) {
                WNDCLASSEXA wc;
                memset(&wc, 0, sizeof(wc));
                wc.cbSize = sizeof(wc);
                wc.style = CS_HREDRAW | CS_VREDRAW;
                wc.lpfnWndProc = ipcproxy_wndproc;
                wc.hInstance = GetModuleHandleA(NULL);
                wc.lpszClassName = req.class_name[0] ? req.class_name : "BUTTON";
                RegisterClassExA(&wc);
                child = CreateWindowExA(req.ex_style, wc.lpszClassName, req.title,
                                        req.style | WS_CHILD | WS_VISIBLE,
                                        req.x, req.y, req.w, req.h,
                                        req.parent_hwnd, (HMENU)(uintptr_t)req.id, wc.hInstance, NULL);
                MyWinLeaveProcessContext();
            }
            MyProcessHostAckCreateChildWindow(w->process_id, child, req.id, child ? TRUE : FALSE,
                                             child ? "parent created child HWND/control" : "parent failed child HWND/control");
            if (!child) break;
        }
        for (;;) {
            MyProcessHostWindowMessage req;
            if (!MyProcessHostTakeDestroyWindowRequest(w->process_id, &req)) break;
            HWND target = req.hwnd ? req.hwnd : w->app_hwnd;
            HWND exposeOwner = 0;
            int ok = target ? 1 : 0;
            BOOL destroyOk = FALSE;
            if (ok && MyWinEnterProcessContext(w->process_id)) {
                exposeOwner = GetWindow(target, GW_OWNER);
                if (!exposeOwner) exposeOwner = GetParent(target);
                destroyOk = DestroyWindow(target);
                if (destroyOk && exposeOwner && IsWindow(exposeOwner)) {
                    /* v172: Win32 exposes the owner/parent area after destroying an
                       owned top-level/dialog.  In the OOP bridge this must become a
                       real WM_PAINT request to the child process, not only a visual
                       compositor redraw. */
                    RedrawWindow(exposeOwner, NULL, 0, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
                }
                MyWinLeaveProcessContext();
            }
            MyProcessHostAckDestroyWindow(w->process_id, destroyOk ? TRUE : FALSE, destroyOk ? "parent DestroyWindow broker OK + exposed owner repaint" : "parent DestroyWindow broker failed");
            break;
        }
        if (w->closed) continue;
        for (;;) {
            MyProcessHostWindowMessage req;
            if (!MyProcessHostTakePostMessageRequest(w->process_id, &req)) break;
            Capability sender = w->app_cap;
            if (!sender.id) sender = cap_create(w->process_id, "ipc-gui-child", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_WINDOW_SUBSCRIBE);
            HWND target = req.hwnd ? req.hwnd : w->app_hwnd;
            int ok = 0;
            if (req.msg == WM_MYOS_HWND_STATE_SUBSCRIBE_REQ || req.msg == WM_MYOS_HWND_STATE_UNSUBSCRIBE_REQ) {
                if (req.msg == WM_MYOS_HWND_STATE_SUBSCRIBE_REQ)
                    ok = hwnd_subscribe(wm->mgr, &sender, 0, target, 0, 0) == 0;
                else
                    ok = hwnd_unsubscribe(wm->mgr, &sender, 0, target, 0, 0) == 0;
                if (ok) {
                    hwnd_post(wm->mgr, &sender, target, WM_MYOS_SUBSCRIBED,
                              req.msg == WM_MYOS_HWND_STATE_SUBSCRIBE_REQ ? 1u : 0u,
                              wm->mgr ? wm->mgr->state_section.updateSerial : 0u);
                }
                MyProcessHostAckPostMessage(w->process_id, ok ? TRUE : FALSE,
                                            ok ? (req.msg == WM_MYOS_HWND_STATE_SUBSCRIBE_REQ ? "v74 WSTS global subscribe OK" : "v74 WSTS global unsubscribe OK")
                                               : "v74 WSTS global subscribe/unsubscribe failed");
                continue;
            }
            ok = hwnd_post(wm->mgr, &sender, target, req.msg, req.wparam, req.lparam) == 0;
            MyProcessHostAckPostMessage(w->process_id, ok ? TRUE : FALSE, ok ? "parent queued child PostMessage" : "parent failed child PostMessage");
            if (!ok) break;
        }
    }
}

void wm_poll(WindowManager* wm)
{
    wm_pump_ipc_proxy_posts(wm);
    for (int i = 0; i < wm->count; i++)
        if (!wm->wins[i].closed && wm->wins[i].app_type == APP_TERMINAL && wm->wins[i].term)
            term_poll(wm->wins[i].term);
}


static HWND wm_deep_child_from_screen(HWND hParent, int screen_x, int screen_y)
{
    if (!hParent || !IsWindow(hParent)) return 0;

    HWND curParent = hParent;
    HWND deepest = 0;
    for (int depth = 0; depth < 8; ++depth) {
        POINT pt;
        pt.x = screen_x;
        pt.y = screen_y;
        if (!ScreenToClient(curParent, &pt)) break;
        HWND child = ChildWindowFromPoint(curParent, pt);
        if (!child || child == curParent || !IsWindowEnabled(child)) break;
        deepest = child;
        curParent = child;
    }
    return deepest;
}

typedef struct WmHwndCollect {
    HWND hwnds[64];
    int count;
} WmHwndCollect;

static BOOL CALLBACK wm_collect_child_hwnd_cb(HWND hWnd, LPARAM lParam)
{
    WmHwndCollect* c = (WmHwndCollect*)lParam;
    if (!c || c->count >= (int)(sizeof(c->hwnds) / sizeof(c->hwnds[0]))) return FALSE;
    c->hwnds[c->count++] = hWnd;
    return TRUE;
}

static int wm_pt_in_rect_screen(const RECT* rc, int x, int y)
{
    return rc && x >= (int)rc->left && x < (int)rc->right &&
           y >= (int)rc->top && y < (int)rc->bottom;
}


static int wm_mdi_visual_caption_hit(HWND hChild, int screen_x, int screen_y)
{
    if (!hChild || !IsWindow(hChild)) return 0;
    RECT rc;
    memset(&rc, 0, sizeof(rc));
    if (!GetWindowRect(hChild, &rc)) return 0;
    if (screen_x < (int)rc.left || screen_x >= (int)rc.right) return 0;
    if (screen_y < (int)rc.top || screen_y >= (int)rc.bottom) return 0;
    return (screen_y - (int)rc.top) < WM_MDI_VISUAL_CAPTION_H;
}

/* v140: MDI caption hit-testing must not depend solely on ChildWindowFromPoint
   finding a client child.  MDI captions are non-client pixels of the child; the
   visual rectangle we draw and the rectangle USER32 moves are the child's
   window rectangle.  Find the MDICLIENT first, then walk its direct children by
   GetWindowRect (reverse creation order ~= topmost in this lite Z model). */
static HWND wm_mdi_child_from_screen_nc(HWND hApp, int screen_x, int screen_y, HWND* outClient)
{
    if (outClient) *outClient = 0;
    if (!hApp || !IsWindow(hApp)) return 0;

    HWND mdiClient = 0;
    HWND deep = wm_deep_child_from_screen(hApp, screen_x, screen_y);
    if (deep) {
        if (wm_class_is(deep, "MDICLIENT")) mdiClient = deep;
        else {
            HWND parent = GetParent(deep);
            if (parent && wm_class_is(parent, "MDICLIENT")) {
                if (outClient) *outClient = parent;
                return deep;
            }
        }
    }

    /* v143: The first manual MDILab drag after launching from the shell exposed
       an init-order hole: the drawn MDI child rectangles were already correct,
       but the MDICLIENT client hit sometimes was not the first object found by
       ChildWindowFromPoint()/GetWindowRect(client).  Escape-closing one child
       happened to normalize the active child/Z state, which made later drags
       appear fixed.

       Do not make the compositor-side MDI NC hit depend on the MDICLIENT rect
       being the first successful hit.  The real drag region is the direct MDI
       child's own window rectangle.  Find the app's MDICLIENT, then walk its
       direct children in USER32 Z-order (top -> bottom) and test those rects. */
    if (!mdiClient) {
        WmHwndCollect appChildren;
        memset(&appChildren, 0, sizeof(appChildren));
        EnumChildWindows(hApp, wm_collect_child_hwnd_cb, (LPARAM)&appChildren);
        for (int i = appChildren.count - 1; i >= 0; --i) {
            HWND h = appChildren.hwnds[i];
            if (!h || GetParent(h) != hApp || !wm_class_is(h, "MDICLIENT")) continue;
            if (!mdiClient) mdiClient = h;
            RECT rc;
            memset(&rc, 0, sizeof(rc));
            if (GetWindowRect(h, &rc) && wm_pt_in_rect_screen(&rc, screen_x, screen_y)) {
                mdiClient = h;
                break;
            }
        }
    }

    if (!mdiClient || !IsWindow(mdiClient)) return 0;
    if (outClient) *outClient = mdiClient;

    for (HWND child = GetWindow(mdiClient, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (GetParent(child) != mdiClient || !IsWindowEnabled(child)) continue;
        RECT rc;
        memset(&rc, 0, sizeof(rc));
        if (GetWindowRect(child, &rc) && wm_pt_in_rect_screen(&rc, screen_x, screen_y))
            return child;
    }
    return 0;
}

static Capability* wm_capability_for_hwnd(WindowManager* wm, HWND hwnd)
{
    if (!wm || !hwnd) return NULL;
    HWND cur = hwnd;
    while (cur) {
        int idx = wm_find_hwnd(wm, cur);
        if (idx >= 0 && idx < wm->count && !wm->wins[idx].closed)
            return (wm->wins[idx].app_type == APP_TERMINAL) ? (wm->wins[idx].term ? &wm->wins[idx].term->cap : NULL) : &wm->wins[idx].app_cap;
        cur = GetParent(cur);
    }
    return NULL;
}

static int wm_deliver_app_client_mouse_sync(WindowManager* wm, UINT msg, int x, int y, WPARAM keys)
{
    if (!wm) return 0;

    HWND target = 0;
    Capability* cap = NULL;

    HWND captured = GetCapture();
    if (captured && IsWindow(captured)) {
        cap = wm_capability_for_hwnd(wm, captured);
        if (cap) target = captured;
        else if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MOUSEMOVE) {
            ReleaseCapture();
            captured = 0;
        }
    }

    if (!target) {
        if (!wm_client_endpoint_at_point(wm, x, y, &target, &cap) || !target || !cap)
            return 0;
    }

    LPARAM lp = wm_shell_lparam_for_hwnd(wm, target, x, y);
    DWORD ownerPid = 0;
    GetWindowThreadProcessId(target, &ownerPid);
    BOOL entered = FALSE;
    if (ownerPid) entered = MyWinEnterProcessContext(ownerPid);
    SendMessageA(target, msg, keys, lp);
    if (entered) MyWinLeaveProcessContext();
    return 1;
}

static void wm_send_app_menu_command(WindowManager* wm, HWND hwnd, UINT id)
{
    if (!wm || !hwnd || !id) return;
    DWORD ownerPid = 0;
    GetWindowThreadProcessId(hwnd, &ownerPid);
    if (ownerPid && MyWinEnterProcessContext(ownerPid)) {
        SendMessageA(hwnd, WM_COMMAND, MAKEWPARAM((WORD)id, 0), 0);
        MyWinLeaveProcessContext();
    } else {
        SendMessageA(hwnd, WM_COMMAND, MAKEWPARAM((WORD)id, 0), 0);
    }
}


typedef struct WmMdiRawDragState {
    HWND hChild;
    HWND hClient;
    int offX;
    int offY;
    int width;
    int height;
    DWORD ownerPid;
} WmMdiRawDragState;

static WmMdiRawDragState g_WmMdiRawDrag;

static void wm_mdi_raw_drag_clear(void)
{
    memset(&g_WmMdiRawDrag, 0, sizeof(g_WmMdiRawDrag));
}

static void wm_mdi_raw_drag_begin(HWND hChild, int screenX, int screenY)
{
    RECT rc;
    memset(&rc, 0, sizeof(rc));
    if (!hChild || !GetWindowRect(hChild, &rc)) return;
    memset(&g_WmMdiRawDrag, 0, sizeof(g_WmMdiRawDrag));
    g_WmMdiRawDrag.hChild = hChild;
    g_WmMdiRawDrag.hClient = GetParent(hChild);
    g_WmMdiRawDrag.offX = screenX - (int)rc.left;
    g_WmMdiRawDrag.offY = screenY - (int)rc.top;
    g_WmMdiRawDrag.width = (int)(rc.right - rc.left);
    g_WmMdiRawDrag.height = (int)(rc.bottom - rc.top);
    GetWindowThreadProcessId(hChild, &g_WmMdiRawDrag.ownerPid);
}

static int wm_mdi_raw_drag_active(void)
{
    return g_WmMdiRawDrag.hChild && IsWindow(g_WmMdiRawDrag.hChild) &&
           g_WmMdiRawDrag.hClient && IsWindow(g_WmMdiRawDrag.hClient);
}

static int wm_try_mdi_caption_mouse_move(WindowManager* wm, int x, int y)
{
    (void)wm;
    if (!wm_mdi_raw_drag_active()) { wm_mdi_raw_drag_clear(); return 0; }

    RECT pr;
    memset(&pr, 0, sizeof(pr));
    if (!GetWindowRect(g_WmMdiRawDrag.hClient, &pr)) { wm_mdi_raw_drag_clear(); return 0; }

    int parentW = (int)(pr.right - pr.left);
    int parentH = (int)(pr.bottom - pr.top);
    int childW = g_WmMdiRawDrag.width;
    int childH = g_WmMdiRawDrag.height;
    if (parentW < 1 || parentH < 1 || childW < 1 || childH < 1) return 1;

    int nx = x - (int)pr.left - g_WmMdiRawDrag.offX;
    int ny = y - (int)pr.top  - g_WmMdiRawDrag.offY;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx + childW > parentW) nx = (parentW > childW) ? (parentW - childW) : 0;
    if (ny + childH > parentH) ny = (parentH > childH) ? (parentH - childH) : 0;

    BOOL entered = FALSE;
    if (g_WmMdiRawDrag.ownerPid) entered = MyWinEnterProcessContext(g_WmMdiRawDrag.ownerPid);
    MoveWindow(g_WmMdiRawDrag.hChild, nx, ny, childW, childH, TRUE);
    if (entered) MyWinLeaveProcessContext();
    return 1;
}

static int wm_try_mdi_caption_mouse_up(WindowManager* wm, int x, int y)
{
    (void)wm;
    if (!wm_mdi_raw_drag_active()) { wm_mdi_raw_drag_clear(); return 0; }
    HWND child = g_WmMdiRawDrag.hChild;
    DWORD ownerPid = g_WmMdiRawDrag.ownerPid;
    wm_try_mdi_caption_mouse_move(wm, x, y);
    BOOL entered = FALSE;
    if (ownerPid) entered = MyWinEnterProcessContext(ownerPid);
    if (IsWindow(child)) {
        SendMessageA(child, WM_NCLBUTTONUP, (WPARAM)HTCAPTION, MAKELPARAM((WORD)x, (WORD)y));
        if (GetCapture() == child) ReleaseCapture();
    }
    if (entered) MyWinLeaveProcessContext();
    wm_mdi_raw_drag_clear();
    return 1;
}

static int wm_client_endpoint_for_window(WindowManager* wm, int idx, int x, int y, HWND* hwnd, Capability** cap)
{
    if (!wm || !hwnd || !cap || idx < 0 || idx >= wm->count) return 0;
    Window* w = &wm->wins[idx];
    if (w->closed || w->minimized) return 0;

    // Nur Client-Bereich: kein Titelbalken, keine Buttons, keine Resize-Ränder.
    // v79: also exclude the left resize border; v78 only excluded right/bottom.
    if (x < w->x + RESIZE_GRIP || x >= w->x + w->w - RESIZE_GRIP) return 0;
    if (y < w->y + TITLEBAR_H || y >= w->y + w->h - RESIZE_GRIP) return 0;

    /* v101: an attached HMENU menubar is compositor-owned chrome sitting
       between the title bar and client.  Do not route its pixels as client
       WM_LBUTTONDOWN/WM_MOUSEMOVE to the app. */
    if (w->app_hwnd && GetMenu(w->app_hwnd) &&
        y >= w->y + TITLEBAR_H && y < w->y + TITLEBAR_H + APP_MENUBAR_H) return 0;

    if (w->app_type == APP_TERMINAL) {
        if (!w->term || !w->term->hwnd || !IsWindowEnabled(w->term->hwnd)) return 0;
        *hwnd = w->term->hwnd;
        *cap = &w->term->cap;
        return 1;
    }

    if (!w->app_hwnd) return 0;
    HWND directChild = wm_deep_child_from_screen(w->app_hwnd, x, y);
    if (directChild && IsWindowEnabled(directChild)) {
        *hwnd = directChild;
        *cap = &w->app_cap;
        return 1;
    }
    if (!IsWindowEnabled(w->app_hwnd)) return 0;
    if (w->app_type == APP_IPC_PROXY && w->process_id) {
        MyProcessHostUpdateGuiRect(w->process_id, w->x, w->y, w->w, w->h);
        POINT pt;
        pt.x = x - (w->x + 1);
        pt.y = y - (w->y + TITLEBAR_H);
        HWND child = ChildWindowFromPoint(w->app_hwnd, pt);
        if (child) { *hwnd = child; *cap = &w->app_cap; return 1; }
    }
    *hwnd = w->app_hwnd;
    *cap = &w->app_cap;
    return 1;
}

static int wm_cap_for_dialog_hit(WindowManager* wm, HWND hit, Capability** cap)
{
    if (!wm || !hit || !cap) return 0;
    HWND owner = MyGetDialogOwner(hit);
    if (!owner) owner = hit;
    int idx = wm_find_hwnd(wm, owner);
    if (idx < 0 || idx >= wm->count) return 0;
    Window* w = &wm->wins[idx];
    if (w->closed || w->minimized) return 0;
    *cap = (w->app_type == APP_TERMINAL) ? (w->term ? &w->term->cap : NULL) : &w->app_cap;
    return *cap != NULL;
}

int wm_client_endpoint_at_focus(WindowManager* wm, int x, int y, HWND* hwnd, Capability** cap)
{
    if (!wm || !hwnd || !cap) return 0;
    wm_ensure_session_input_runtime(wm);
    *hwnd = 0;
    *cap = NULL;

    if (wm->focused < 0 || wm->focused >= wm->count) return 0;
    if (wm->menu_open) return 0;
    if (wm->drag_mode != DRAG_NONE) return 0;

    Window* w = &wm->wins[wm->focused];
    if (w->closed || w->minimized) return 0;

    {
        HWND owner = window_hwnd(w);
        HWND dlgHit = 0;
        if (owner && MyTopLevelDialogHitTest(owner, x, y, &dlgHit)) {
            *hwnd = dlgHit;
            *cap = (w->app_type == APP_TERMINAL) ? (w->term ? &w->term->cap : NULL) : &w->app_cap;
            return 1;
        }
    }

    return wm_client_endpoint_for_window(wm, wm->focused, x, y, hwnd, cap);
}

int wm_client_endpoint_at_point(WindowManager* wm, int x, int y, HWND* hwnd, Capability** cap)
{
    if (!wm || !hwnd || !cap) return 0;
    wm_ensure_session_input_runtime(wm);
    *hwnd = 0;
    *cap = NULL;
    if (wm->menu_open) return 0;
    if (wm->drag_mode != DRAG_NONE) return 0;

    /* v92.2: WM_MOUSEWHEEL uses hover routing.  First check owned top-level
       dialogs globally, not only the foreground owner.  This makes a LISTBOX
       in a modeless dialog scroll when the cursor is over it even if another
       app frame currently has focus. */
    {
        HWND dlgHit = 0;
        if (MyTopLevelDialogHitTest(0, x, y, &dlgHit) && dlgHit) {
            Capability* dlgCap = NULL;
            if (wm_cap_for_dialog_hit(wm, dlgHit, &dlgCap)) {
                *hwnd = dlgHit;
                *cap = dlgCap;
                return 1;
            }
        }
    }

    int idx = top_window_at(wm, x, y);
    if (idx < 0) return 0;
    return wm_client_endpoint_for_window(wm, idx, x, y, hwnd, cap);
}

// ── Kleine Window-API ─────────────────────────

int wm_get_window_rect(WindowManager* wm, int index, WindowRect* out)
{
    if (!out || !valid_window_index(wm, index)) return 0;
    Window* w = &wm->wins[index];
    out->x = w->x; out->y = w->y; out->w = w->w; out->h = w->h;
    return 1;
}

int wm_set_window_pos_ex(WindowManager* wm, int index, HWND hwndInsertAfter, int x, int y, int w, int h, UINT flags)
{
    if (!valid_window_index(wm, index)) return 0;
    Window* win = &wm->wins[index];
    HWND hwnd = window_hwnd(win);
    if (!hwnd) return 0;

    int old_x = win->x, old_y = win->y, old_w = win->w, old_h = win->h;

    WINDOWPOS wp;
    memset(&wp, 0, sizeof(wp));
    wp.hwnd = hwnd;
    wp.hwndInsertAfter = hwndInsertAfter;
    wp.x = (flags & SWP_NOMOVE) ? old_x : x;
    wp.y = (flags & SWP_NOMOVE) ? old_y : y;
    wp.cx = (flags & SWP_NOSIZE) ? old_w : w;
    wp.cy = (flags & SWP_NOSIZE) ? old_h : h;
    wp.flags = flags;

    MINMAXINFO mmi;
    wm_init_minmax_for_window(wm, win, &mmi);
    wm_send_window_message(wm, win, hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&mmi);
    if (mmi.ptMinTrackSize.x > 0 && wp.cx < mmi.ptMinTrackSize.x) wp.cx = mmi.ptMinTrackSize.x;
    if (mmi.ptMinTrackSize.y > 0 && wp.cy < mmi.ptMinTrackSize.y) wp.cy = mmi.ptMinTrackSize.y;
    if (wp.cx < 120) wp.cx = 120;
    if (wp.cy < 80)  wp.cy = 80;

    wm_clamp_window_to_screen(wm, &wp.x, &wp.y, wp.cx, wp.cy);

    // v80: real Win32-shaped position contract.  WndProcs may inspect/mutate
    // WINDOWPOS during CHANGING for in-process HWNDs.  OOP children still get
    // the message as a scalar diagnostic and use the shared HWND state as truth.
    wm_send_window_message(wm, win, hwnd, WM_WINDOWPOSCHANGING, 0, (LPARAM)&wp);

    if (wp.flags & SWP_NOSIZE) { wp.cx = old_w; wp.cy = old_h; }
    if (wp.flags & SWP_NOMOVE) { wp.x = old_x; wp.y = old_y; }
    if (wp.cx < 120) wp.cx = 120;
    if (wp.cy < 80)  wp.cy = 80;
    wm_clamp_window_to_screen(wm, &wp.x, &wp.y, wp.cx, wp.cy);

    int moved = (wp.x != old_x) || (wp.y != old_y);
    int sized = (wp.cx != old_w) || (wp.cy != old_h);
    int visibility_changed = 0;

    if (wp.flags & SWP_HIDEWINDOW) {
        if (!win->minimized) visibility_changed = 1;
        win->minimized = 1;
        if (wm->focused == index) wm->focused = -1;
    }
    if (wp.flags & SWP_SHOWWINDOW) {
        if (win->minimized) visibility_changed = 1;
        win->minimized = 0;
    }

    win->x = wp.x;
    win->y = wp.y;
    win->w = wp.cx;
    win->h = wp.cy;

    wm_publish_window_state_event(wm, index, WM_WINDOWPOSCHANGED);
    wm_send_window_message(wm, win, hwnd, WM_WINDOWPOSCHANGED, 0, (LPARAM)&wp);

    if (moved) {
        wm_publish_window_state_event(wm, index, WM_MOVE);
        /* v82: MSDN WM_MOVE lParam is the upper-left of the client area, not the outer frame. */
        int move_client_x = win->x + 1;
        int move_client_y = win->y + TITLEBAR_H;
        wm_send_window_message(wm, win, hwnd, WM_MOVE, 0, MAKELPARAM((WORD)move_client_x, (WORD)move_client_y));
    }
    if (sized || visibility_changed) {
        UINT sizeType = win->minimized ? SIZE_MINIMIZED : SIZE_RESTORED;
        /* v82: MSDN WM_SIZE lParam is client width/height.  The frame size stays in WINDOWPOS/WSTS. */
        int size_client_w = win->w - 2;
        int size_client_h = win->h - TITLEBAR_H - 1;
        if (size_client_w < 0) size_client_w = 0;
        if (size_client_h < 0) size_client_h = 0;
        wm_publish_window_state_event(wm, index, WM_SIZE);
        wm_send_window_message(wm, win, hwnd, WM_SIZE, sizeType, MAKELPARAM((WORD)size_client_w, (WORD)size_client_h));
    }
    if (visibility_changed) {
        wm_publish_window_state_event(wm, index, WM_SHOWWINDOW);
        wm_send_window_message(wm, win, hwnd, WM_SHOWWINDOW, win->minimized ? FALSE : TRUE, 0);
    }

    if (!(wp.flags & SWP_NOACTIVATE) && !(wp.flags & SWP_HIDEWINDOW)) {
        wm_set_foreground_hwnd(wm, hwnd);
    } else if (wm->focused < 0) {
        wm_focus_fallback(wm);
    }
    return 1;
}

int wm_set_window_pos(WindowManager* wm, int index, int x, int y, int w, int h)
{
    return wm_set_window_pos_ex(wm, index, HWND_TOP, x, y, w, h, SWP_NOZORDER);
}

int wm_show_window(WindowManager* wm, int index, int nCmdShow)
{
    if (!valid_window_index(wm, index)) return 0;
    Window* w = &wm->wins[index];
    UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER;
    switch (nCmdShow) {
    case SW_HIDE:
    case SW_MINIMIZE:
    case SW_SHOWMINIMIZED:
    case SW_SHOWMINNOACTIVE:
        flags |= SWP_HIDEWINDOW | SWP_NOACTIVATE;
        break;
    case SW_MAXIMIZE:
        wm_maximize_window(wm, index);
        return 1;
    case SW_SHOWNOACTIVATE:
    case SW_SHOWNA:
        flags |= SWP_SHOWWINDOW | SWP_NOACTIVATE;
        break;
    case SW_RESTORE:
        if (w->maximized || w->minimized) { wm_restore_window(wm, index); return 1; }
        flags |= SWP_SHOWWINDOW;
        break;
    case SW_SHOWNORMAL:
    case SW_SHOW:
    case SW_SHOWDEFAULT:
    default:
        flags |= SWP_SHOWWINDOW;
        break;
    }
    return wm_set_window_pos_ex(wm, index, HWND_TOP, w->x, w->y, w->w, w->h, flags);
}


int wm_get_window_title(WindowManager* wm, int index, char* out, int out_len)
{
    if (!out || out_len <= 0 || !valid_window_index(wm, index)) return 0;
    strncpy(out, wm->wins[index].title, (size_t)out_len - 1);
    out[out_len - 1] = 0;
    return 1;
}

int wm_find_window(WindowManager* wm, const char* title)
{
    if (!wm || !title) return -1;
    for (int i = wm->count - 1; i >= 0; i--) {
        if (!wm->wins[i].closed && strcmp(wm->wins[i].title, title) == 0)
            return i;
    }
    return -1;
}

void wm_enum_windows(WindowManager* wm, WindowEnumProc proc, void* userdata)
{
    if (!wm || !proc) return;
    for (int i = 0; i < wm->count; i++) {
        if (wm->wins[i].closed) continue;
        if (!proc(i, &wm->wins[i], userdata)) break;
    }
}

int wm_find_hwnd(WindowManager* wm, HWND hwnd)
{
    if (!wm || !hwnd) return -1;
    for (int i = 0; i < wm->count; i++) {
        if (!valid_window_index(wm, i)) continue;
        if (window_hwnd(&wm->wins[i]) == hwnd) return i;
    }
    return -1;
}

HWND wm_get_window_hwnd(WindowManager* wm, int index)
{
    if (!valid_window_index(wm, index)) return 0;
    return window_hwnd(&wm->wins[index]);
}

HWND wm_get_foreground_hwnd(WindowManager* wm)
{
    if (!wm || wm->focused < 0) return 0;
    return wm_get_window_hwnd(wm, wm->focused);
}

int wm_set_foreground_hwnd(WindowManager* wm, HWND hwnd)
{
    if (!wm || !hwnd) return 0;
    int idx = wm_find_hwnd(wm, hwnd);
    if (idx < 0) return 0;

    int old = wm->focused;
    if (old == idx && wm->wins[idx].active && !wm->wins[idx].minimized)
        return 1;

    if (old >= 0 && old < wm->count && !wm->wins[old].closed) {
        wm->wins[old].active = 0;
        if (old != idx) wm_publish_window_state_event(wm, old, WM_ACTIVATE);
    }

    int was_minimized = wm->wins[idx].minimized;
    wm->wins[idx].minimized = 0;
    wm->wins[idx].active = 1;
    wm->focused = idx;
    if (was_minimized) wm_publish_window_state_event(wm, idx, WM_SHOWWINDOW);
    wm_publish_window_state_event(wm, idx, WM_ACTIVATE);
    return 1;
}

int wm_get_window_state(WindowManager* wm, HWND hwnd, MyWindowState* out)
{
    if (!wm || !out) return 0;

    // v17 primary path: copy current snapshot from shared state section.
    if (wm->mgr && hwnd_copy_window_state(wm->mgr, hwnd, out))
        return 1;

    // Fallback for early boot / very first creation before first publish.
    int idx = wm_find_hwnd(wm, hwnd);
    if (idx < 0) return 0;
    wm_fill_window_state(wm, idx, out);
    return out->hWnd == hwnd;
}

int wm_set_window_title_by_hwnd(WindowManager* wm, HWND hwnd, const char* title)
{
    if (!wm || !title) return 0;
    int idx = wm_find_hwnd(wm, hwnd);
    if (idx < 0) return 0;
    snprintf(wm->wins[idx].title, sizeof(wm->wins[idx].title), "%s", title);
    wm_publish_window_state_event(wm, idx, WM_WINDOWTEXTCHANGED);
    return 1;
}


int wm_set_wallpaper(WindowManager* wm, const char* path)
{
    if (!wm || !path || !path[0]) return 0;
    Image img;
    memset(&img, 0, sizeof(img));
    if (!image_load_any(path, &img)) {
        snprintf(wm->wallpaper_status, sizeof(wm->wallpaper_status), "Wallpaper load failed: %s", path);
        return 0;
    }
    image_free(&wm->wallpaper);
    wm->wallpaper = img;
    wm->wallpaper_enabled = 1;
    snprintf(wm->wallpaper_path, sizeof(wm->wallpaper_path), "%s", path);
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    snprintf(wm->wallpaper_status, sizeof(wm->wallpaper_status), "Wallpaper set: %s (%dx%d)", base, img.width, img.height);
    return 1;
}

void wm_clear_wallpaper(WindowManager* wm)
{
    if (!wm) return;
    image_free(&wm->wallpaper);
    wm->wallpaper_enabled = 0;
    wm->wallpaper_path[0] = 0;
    snprintf(wm->wallpaper_status, sizeof(wm->wallpaper_status), "Wallpaper: none");
}

const char* wm_get_wallpaper_status(WindowManager* wm)
{
    if (!wm) return "Wallpaper: ?";
    return wm->wallpaper_status;
}

HWND wm_get_desktop_hwnd(WindowManager* wm)
{
    return wm ? wm->hwnd_desktop : 0;
}

HWND wm_get_taskbar_hwnd(WindowManager* wm)
{
    return wm ? wm->hwnd_taskbar : 0;
}

HWND wm_get_start_button_hwnd(WindowManager* wm)
{
    return wm ? wm->hwnd_start_button : 0;
}
