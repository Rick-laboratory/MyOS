#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "message.h"
#include "input_layer.h"
#include "fb.h"
#include "window.h"
#include "capability.h"
#include "ipc.h"
#include "hwnd.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "myobject.h"
#include "apphost.h"
#include "processhost.h"
#include "smoke.h"
#include <linux/input-event-codes.h>

#define MYOS_MAX_DAMAGE_RECTS 32

typedef struct {
    int x, y, w, h;
} DesktopDamageRect;

typedef struct {
    Framebuffer     fb;
    WindowManager   wm;
    IPCBus          bus;
    HWNDManager     mgr;
    int             cursor_x, cursor_y;
    int             dirty;
    unsigned long long render_sig;
    unsigned long long render_sig_nopointer;
    DesktopDamageRect damage[MYOS_MAX_DAMAGE_RECTS];
    int             damage_count;
    int             damage_full;

    /* v176: retained compositor damage cache.  The scene is still drawn with
       existing immediate-mode routines inside a clip, but render scheduling now
       understands top-level window/icon/menu deltas and marks only their old+new
       screen rectangles instead of falling back to full-screen damage. */
    unsigned long long cached_window_sig[MAX_WINDOWS];
    WindowRect         cached_window_rect[MAX_WINDOWS];
    int                cached_window_present[MAX_WINDOWS];
    unsigned long long cached_icon_sig[MAX_DESKTOP_ICONS];
    DesktopDamageRect  cached_icon_rect[MAX_DESKTOP_ICONS];
    int                cached_icon_present[MAX_DESKTOP_ICONS];
    unsigned long long cached_shell_bg_sig;
    DesktopDamageRect  cached_menu_rects[APP_MENU_MAX_LEVELS + 1];
    int                cached_menu_rect_count;

    int             mouse_left_down;
    int             mouse_right_down;
    pthread_mutex_t lock;
} Desktop;

static unsigned long long myos_hash_u64(unsigned long long h, unsigned long long v);
static unsigned long long myos_hash_bytes(unsigned long long h, const void* data, size_t n);
static unsigned long long myos_hash_cstr(unsigned long long h, const char* s);

static int desktop_rects_touch_or_overlap(const DesktopDamageRect* a, const DesktopDamageRect* b)
{
    if (!a || !b) return 0;
    int ar = a->x + a->w, ab = a->y + a->h;
    int br = b->x + b->w, bb = b->y + b->h;
    return !(ar < b->x || br < a->x || ab < b->y || bb < a->y);
}

static void desktop_damage_full(Desktop* d)
{
    if (!d) return;
    d->damage_count = 1;
    d->damage_full = 1;
    d->damage[0].x = 0;
    d->damage[0].y = 0;
    d->damage[0].w = d->fb.width;
    d->damage[0].h = d->fb.height;
    d->dirty = 1;
}

static void desktop_damage_rect(Desktop* d, int x, int y, int w, int h)
{
    if (!d || w <= 0 || h <= 0 || d->fb.width <= 0 || d->fb.height <= 0) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > d->fb.width) x2 = d->fb.width;
    if (y2 > d->fb.height) y2 = d->fb.height;
    if (x >= x2 || y >= y2) return;

    DesktopDamageRect nr = { x, y, x2 - x, y2 - y };
    if ((long long)nr.w * (long long)nr.h > ((long long)d->fb.width * (long long)d->fb.height) / 2) {
        desktop_damage_full(d);
        return;
    }
    if (d->damage_full) { d->dirty = 1; return; }

    for (int i = 0; i < d->damage_count; ++i) {
        if (desktop_rects_touch_or_overlap(&d->damage[i], &nr)) {
            int ax = d->damage[i].x;
            int ay = d->damage[i].y;
            int ar = d->damage[i].x + d->damage[i].w;
            int ab = d->damage[i].y + d->damage[i].h;
            int nrx2 = nr.x + nr.w;
            int nry2 = nr.y + nr.h;
            if (nr.x < ax) ax = nr.x;
            if (nr.y < ay) ay = nr.y;
            if (nrx2 > ar) ar = nrx2;
            if (nry2 > ab) ab = nry2;
            d->damage[i].x = ax;
            d->damage[i].y = ay;
            d->damage[i].w = ar - ax;
            d->damage[i].h = ab - ay;
            d->dirty = 1;
            return;
        }
    }

    if (d->damage_count >= MYOS_MAX_DAMAGE_RECTS) {
        desktop_damage_full(d);
        return;
    }
    d->damage[d->damage_count++] = nr;
    d->dirty = 1;
}

static void desktop_damage_cursor_at(Desktop* d, int x, int y)
{
    /* Cursor is a 13x13 cross plus a one-pixel safety halo so the old pixels
       underneath are repainted through the clipped compositor pass. */
    desktop_damage_rect(d, x - 8, y - 8, 17, 17);
}

static void desktop_damage_debug_badge(Desktop* d)
{
    if (!d) return;
    desktop_damage_rect(d, 0, 0, d->fb.width > 940 ? 940 : d->fb.width, 35);
}

static void desktop_damage_taskbar(Desktop* d)
{
    if (!d || d->fb.height <= 0) return;
    desktop_damage_rect(d, 0, d->fb.height - TASKBAR_H, d->fb.width, TASKBAR_H);
}

static void desktop_damage_window_rect(Desktop* d, const WindowRect* r)
{
    if (!d || !r || r->w <= 0 || r->h <= 0) return;
    /* Include a tiny halo: the frame outline, resize grip and cursor-overlap
       pixels are NC-owned by the compositor, not the child surface. */
    desktop_damage_rect(d, r->x - 2, r->y - 2, r->w + 4, r->h + 4);
}

static DesktopDamageRect desktop_icon_damage_rect_from_icon(const DesktopIcon* ic, int selected)
{
    DesktopDamageRect r = { 0, 0, 0, 0 };
    if (!ic) return r;
    r.x = ic->x - (selected ? 4 : 2);
    r.y = ic->y - (selected ? 4 : 2);
    r.w = ic->w + (selected ? 8 : 4);
    r.h = ic->h + (selected ? 8 : 4);
    return r;
}

static void desktop_damage_cached_rect(Desktop* d, const DesktopDamageRect* r)
{
    if (!d || !r || r->w <= 0 || r->h <= 0) return;
    desktop_damage_rect(d, r->x, r->y, r->w, r->h);
}

static unsigned long long desktop_window_visual_signature(Desktop* d, int i)
{
    if (!d || i < 0 || i >= MAX_WINDOWS || i >= d->wm.count) return 0;
    Window* w = &d->wm.wins[i];
    unsigned long long h = 1469598103934665603ull;
    h = myos_hash_u64(h, (unsigned)w->x);
    h = myos_hash_u64(h, (unsigned)w->y);
    h = myos_hash_u64(h, (unsigned)w->w);
    h = myos_hash_u64(h, (unsigned)w->h);
    h = myos_hash_u64(h, (unsigned)w->active);
    h = myos_hash_u64(h, (unsigned)w->closed);
    h = myos_hash_u64(h, (unsigned)w->minimized);
    h = myos_hash_u64(h, (unsigned)w->maximized);
    h = myos_hash_u64(h, (unsigned)w->app_type);
    h = myos_hash_u64(h, (unsigned)w->app_hwnd);
    h = myos_hash_u64(h, (unsigned)w->process_id);
    h = myos_hash_cstr(h, w->title);
    h = myos_hash_cstr(h, w->ipc_status);
    if (!w->closed) {
        DWORD ownerVisualPid = 0;
        HWND ownerVisualRoot = 0;
        if (w->app_type == APP_TERMINAL) {
            ownerVisualPid = w->term ? w->term->cap.id : 0;
            ownerVisualRoot = w->term ? w->term->hwnd : 0;
        } else if (w->app_type == APP_IPC_PROXY) {
            ownerVisualPid = w->process_id;
            ownerVisualRoot = w->app_hwnd;
        } else {
            ownerVisualPid = w->app_cap.id;
            ownerVisualRoot = w->app_hwnd;
        }
        h = myos_hash_u64(h, hwnd_get_owner_visual_signature(&d->mgr, ownerVisualPid, ownerVisualRoot));
    }

    if (!w->closed && w->app_type == APP_TERMINAL && w->term) {
        Terminal* t = w->term;
        h = myos_hash_u64(h, (unsigned)t->row);
        h = myos_hash_u64(h, (unsigned)t->blink);
        h = myos_hash_u64(h, (unsigned)t->input_len);
        h = myos_hash_u64(h, (unsigned)t->pipe_fd);
        h = myos_hash_bytes(h, t->input, sizeof(t->input));
        h = myos_hash_bytes(h, t->buf, sizeof(t->buf));
    } else if (!w->closed && w->app_type == APP_IPC_PROXY && w->process_id) {
        MyProcessHostInfo hi;
        memset(&hi, 0, sizeof(hi));
        if (MyProcessHostGetInfo(w->process_id, &hi)) {
            h = myos_hash_u64(h, (unsigned)hi.gui_msg_sent);
            h = myos_hash_u64(h, (unsigned)hi.gui_msg_received);
            h = myos_hash_u64(h, (unsigned)hi.gui_msg_dispatched);
            h = myos_hash_u64(h, (unsigned)hi.gui_post_request);
            h = myos_hash_u64(h, (unsigned)hi.gui_post_ack);
            h = myos_hash_u64(h, (unsigned)hi.child_hwnd_request);
            h = myos_hash_u64(h, (unsigned)hi.child_hwnd_ack);
            h = myos_hash_u64(h, (unsigned)hi.child_hwnd_count);
            h = myos_hash_u64(h, (unsigned)hi.child_hwnd_command_count);
            h = myos_hash_u64(h, (unsigned)hi.gdi_enabled);
            h = myos_hash_u64(h, (unsigned)hi.gdi_sequence);
            h = myos_hash_u64(h, (unsigned)hi.gdi_paint_count);
            h = myos_hash_u64(h, (unsigned)hi.gdi_command_count);
            h = myos_hash_u64(h, (unsigned)hi.gdi_client_w);
            h = myos_hash_u64(h, (unsigned)hi.gdi_client_h);
            h = myos_hash_u64(h, (unsigned)hi.surface_enabled);
            h = myos_hash_u64(h, (unsigned)hi.surface_mapped);
            h = myos_hash_u64(h, (unsigned)hi.surface_seq);
            h = myos_hash_u64(h, (unsigned)hi.surface_paint_count);
            h = myos_hash_u64(h, (unsigned)hi.surface_dirty_left);
            h = myos_hash_u64(h, (unsigned)hi.surface_dirty_top);
            h = myos_hash_u64(h, (unsigned)hi.surface_dirty_right);
            h = myos_hash_u64(h, (unsigned)hi.surface_dirty_bottom);
            h = myos_hash_cstr(h, hi.gdi_status);
            h = myos_hash_cstr(h, hi.surface_status);
        }
    }
    return h ? h : 1;
}

static unsigned long long desktop_icon_visual_signature(WindowManager* wm, int i)
{
    if (!wm || i < 0 || i >= MAX_DESKTOP_ICONS || i >= wm->desktop_icon_count) return 0;
    const DesktopIcon* ic = &wm->desktop_icons[i];
    unsigned long long h = 1469598103934665603ull;
    h = myos_hash_u64(h, (unsigned)ic->x);
    h = myos_hash_u64(h, (unsigned)ic->y);
    h = myos_hash_u64(h, (unsigned)ic->w);
    h = myos_hash_u64(h, (unsigned)ic->h);
    h = myos_hash_u64(h, (unsigned)ic->is_dir);
    h = myos_hash_u64(h, (unsigned)(i == wm->desktop_selected));
    h = myos_hash_u64(h, (unsigned)(i == wm->desktop_drag_idx && wm->desktop_dragging));
    h = myos_hash_cstr(h, ic->name);
    return h ? h : 1;
}

static unsigned long long desktop_shell_background_signature(Desktop* d)
{
    if (!d) return 0;
    WindowManager* wm = &d->wm;
    unsigned long long h = 1469598103934665603ull;
    h = myos_hash_u64(h, (unsigned)d->fb.width);
    h = myos_hash_u64(h, (unsigned)d->fb.height);
    h = myos_hash_u64(h, (unsigned)wm->bg);
    h = myos_hash_u64(h, (unsigned)wm->wallpaper_enabled);
    h = myos_hash_cstr(h, wm->wallpaper_path);
    h = myos_hash_cstr(h, wm->wallpaper_status);
    h = myos_hash_u64(h, (unsigned)wm->desktop_layout_mode);
    return h ? h : 1;
}

static int desktop_collect_menu_rects(WindowManager* wm, DesktopDamageRect* out, int max_out)
{
    if (!wm || !out || max_out <= 0) return 0;
    int n = 0;
    if (wm->menu_open && wm->menu_kind != MENU_KIND_APP && wm->menu_kind != MENU_KIND_APPPOPUP) {
        int count = (wm->menu_kind == MENU_KIND_SYSTEM) ? 7 : 31;
        out[n].x = wm->menu_x - 2;
        out[n].y = wm->menu_y - 2;
        out[n].w = 180 + 4;
        out[n].h = count * 22 + 6;
        if (++n >= max_out) return n;
    }
    if (wm->app_menu_level_count > 0) {
        if (wm->app_menu_owner_idx >= 0 && wm->app_menu_owner_idx < wm->count) {
            Window* ow = &wm->wins[wm->app_menu_owner_idx];
            out[n].x = ow->x - 2;
            out[n].y = ow->y - 2;
            out[n].w = ow->w + 4;
            out[n].h = TITLEBAR_H + APP_MENUBAR_H + 4;
            if (++n >= max_out) return n;
        }
        for (int i = 0; i < wm->app_menu_level_count && i < APP_MENU_MAX_LEVELS && n < max_out; ++i) {
            out[n].x = wm->app_menu_x[i] - 2;
            out[n].y = wm->app_menu_y[i] - 2;
            out[n].w = wm->app_menu_w[i] + 4;
            out[n].h = wm->app_menu_h[i] + 4;
            ++n;
        }
    }
    return n;
}

static void desktop_update_visual_damage_cache(Desktop* d)
{
    if (!d) return;
    WindowManager* wm = &d->wm;
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        int present = (i < wm->count && !wm->wins[i].closed && !wm->wins[i].minimized);
        d->cached_window_present[i] = present;
        if (present) {
            Window* w = &wm->wins[i];
            d->cached_window_rect[i].x = w->x;
            d->cached_window_rect[i].y = w->y;
            d->cached_window_rect[i].w = w->w;
            d->cached_window_rect[i].h = w->h;
            d->cached_window_sig[i] = desktop_window_visual_signature(d, i);
        } else {
            memset(&d->cached_window_rect[i], 0, sizeof(d->cached_window_rect[i]));
            d->cached_window_sig[i] = 0;
        }
    }
    for (int i = 0; i < MAX_DESKTOP_ICONS; ++i) {
        int present = (i < wm->desktop_icon_count);
        d->cached_icon_present[i] = present;
        if (present) {
            const DesktopIcon* ic = &wm->desktop_icons[i];
            d->cached_icon_sig[i] = desktop_icon_visual_signature(wm, i);
            d->cached_icon_rect[i] = desktop_icon_damage_rect_from_icon(ic, i == wm->desktop_selected);
        } else {
            d->cached_icon_sig[i] = 0;
            memset(&d->cached_icon_rect[i], 0, sizeof(d->cached_icon_rect[i]));
        }
    }
    d->cached_shell_bg_sig = desktop_shell_background_signature(d);
    d->cached_menu_rect_count = desktop_collect_menu_rects(wm, d->cached_menu_rects, APP_MENU_MAX_LEVELS + 1);
}

static int desktop_damage_from_visual_delta(Desktop* d)
{
    if (!d) return 0;
    WindowManager* wm = &d->wm;
    int damaged = 0;

    /* Background/layout/wallpaper changes alter every uncovered desktop pixel.
       Keep those conservative; window/control/menu changes below are scoped. */
    unsigned long long bg_sig = desktop_shell_background_signature(d);
    if (d->cached_shell_bg_sig && bg_sig != d->cached_shell_bg_sig)
        return 0;

    for (int i = 0; i < MAX_DESKTOP_ICONS; ++i) {
        int present = (i < wm->desktop_icon_count);
        unsigned long long sig = present ? desktop_icon_visual_signature(wm, i) : 0;
        if (present != d->cached_icon_present[i] || sig != d->cached_icon_sig[i]) {
            desktop_damage_cached_rect(d, &d->cached_icon_rect[i]);
            if (present) {
                DesktopDamageRect cr = desktop_icon_damage_rect_from_icon(&wm->desktop_icons[i], i == wm->desktop_selected);
                desktop_damage_cached_rect(d, &cr);
            }
            damaged = 1;
        }
    }

    for (int i = 0; i < MAX_WINDOWS; ++i) {
        int present = (i < wm->count && !wm->wins[i].closed && !wm->wins[i].minimized);
        unsigned long long sig = present ? desktop_window_visual_signature(d, i) : 0;
        if (present != d->cached_window_present[i] || sig != d->cached_window_sig[i]) {
            desktop_damage_window_rect(d, &d->cached_window_rect[i]);
            if (present) {
                WindowRect nr = { wm->wins[i].x, wm->wins[i].y, wm->wins[i].w, wm->wins[i].h };
                desktop_damage_window_rect(d, &nr);
            }
            desktop_damage_taskbar(d);
            damaged = 1;
        }
    }

    DesktopDamageRect current_menu[APP_MENU_MAX_LEVELS + 1];
    int current_menu_count = desktop_collect_menu_rects(wm, current_menu, APP_MENU_MAX_LEVELS + 1);
    if (current_menu_count != d->cached_menu_rect_count) {
        for (int i = 0; i < d->cached_menu_rect_count; ++i) desktop_damage_cached_rect(d, &d->cached_menu_rects[i]);
        for (int i = 0; i < current_menu_count; ++i) desktop_damage_cached_rect(d, &current_menu[i]);
        damaged = 1;
    } else {
        for (int i = 0; i < current_menu_count; ++i) {
            DesktopDamageRect* a = &d->cached_menu_rects[i];
            DesktopDamageRect* b = &current_menu[i];
            if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h) {
                desktop_damage_cached_rect(d, a);
                desktop_damage_cached_rect(d, b);
                damaged = 1;
            }
        }
    }
    if (wm->menu_open || current_menu_count || d->cached_menu_rect_count) {
        for (int i = 0; i < d->cached_menu_rect_count; ++i) desktop_damage_cached_rect(d, &d->cached_menu_rects[i]);
        for (int i = 0; i < current_menu_count; ++i) desktop_damage_cached_rect(d, &current_menu[i]);
        if (current_menu_count || d->cached_menu_rect_count) damaged = 1;
    }

    /* The top diagnostics intentionally reflect queue/object counters.  Those
       counters are not part of any app surface, so keep their damage local. */
    desktop_damage_debug_badge(d);
    damaged = 1;
    return damaged;
}

static void desktop_damage_clear(Desktop* d)
{
    if (!d) return;
    d->damage_count = 0;
    d->damage_full = 0;
}

static void desktop_init_hwnd_state_section(Desktop* d)
{
    if (!d) return;
    Capability cap = cap_create(73, "session-state", CAP_ADMIN);
    cap_add_target(&cap, 0);
    MyWinBindRuntime(&d->mgr, &cap);
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                     (DWORD)sizeof(MyWindowStateSection),
                                     MYOS_WINDOWSTATE_SECTION_NAME);
    if (!hMap) {
        printf("[v75] HWND shared state CreateFileMapping failed err=%u\n", (unsigned)GetLastError());
        return;
    }
    MyWindowStateSection* view = (MyWindowStateSection*)MapViewOfFile(hMap, FILE_MAP_READ|FILE_MAP_WRITE, 0, 0,
                                                                      (DWORD)sizeof(MyWindowStateSection));
    if (!view) {
        printf("[v75] HWND shared state MapViewOfFile failed err=%u\n", (unsigned)GetLastError());
        return;
    }
    hwnd_attach_window_state_section(&d->mgr, view);
    printf("[v75] HWND shared WindowState section mapped: %s size=%u view=%p\n",
           MYOS_WINDOWSTATE_SECTION_NAME, (unsigned)sizeof(MyWindowStateSection), (void*)view);
}


static unsigned long long myos_hash_u64(unsigned long long h, unsigned long long v)
{
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

static unsigned long long myos_hash_bytes(unsigned long long h, const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static unsigned long long myos_hash_cstr(unsigned long long h, const char* s)
{
    if (!s) return myos_hash_u64(h, 0);
    return myos_hash_bytes(h, s, strlen(s) + 1);
}

/* v238: transient menus are compositor overlays, not HWND-backed windows.
   Their geometry was already damage-tracked, but their *content* (selection,
   MDI Window-list text/check state, submenu stack) must also participate in
   the render signature.  Otherwise a menu can change while keeping the same
   rectangle and the damage scheduler may leave old frontbuffer pixels under
   a clipped redraw. */
static unsigned long long desktop_hash_menu_item(unsigned long long h, HMENU menu, int pos)
{
    MENUITEMINFOA mi;
    char text[128];
    memset(&mi, 0, sizeof(mi));
    memset(text, 0, sizeof(text));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING | MIIM_DATA;
    mi.dwTypeData = text;
    mi.cch = (UINT)sizeof(text);
    if (!menu || !GetMenuItemInfoA(menu, (UINT)pos, TRUE, &mi))
        return myos_hash_u64(h, 0x4d454e55424144ull ^ (unsigned long long)(unsigned)pos);
    h = myos_hash_u64(h, (unsigned)mi.fType);
    h = myos_hash_u64(h, (unsigned)mi.fState);
    h = myos_hash_u64(h, (unsigned)mi.wID);
    h = myos_hash_u64(h, (unsigned)(uintptr_t)mi.hSubMenu);
    h = myos_hash_u64(h, (unsigned)(uintptr_t)mi.dwItemData);
    h = myos_hash_cstr(h, text);
    return h;
}

static unsigned long long desktop_menu_visual_signature(WindowManager* wm)
{
    if (!wm) return 0;
    unsigned long long h = 1469598103934665603ull;
    h = myos_hash_u64(h, (unsigned)wm->menu_open);
    h = myos_hash_u64(h, (unsigned)wm->menu_kind);
    h = myos_hash_u64(h, (unsigned)wm->menu_x);
    h = myos_hash_u64(h, (unsigned)wm->menu_y);
    h = myos_hash_u64(h, (unsigned)wm->menu_from_start);
    h = myos_hash_u64(h, (unsigned)wm->menu_target_idx);
    h = myos_hash_u64(h, (unsigned)(uintptr_t)wm->menu_target_hwnd);
    h = myos_hash_u64(h, (unsigned)wm->menu_selected);
    h = myos_hash_u64(h, (unsigned)wm->menu_loop_serial);
    h = myos_hash_u64(h, (unsigned)wm->menu_key_count);

    h = myos_hash_u64(h, (unsigned)(uintptr_t)wm->app_menu_hwnd);
    h = myos_hash_u64(h, (unsigned)(uintptr_t)wm->app_menu_bar);
    h = myos_hash_u64(h, (unsigned)wm->app_menu_owner_idx);
    h = myos_hash_u64(h, (unsigned)wm->app_menu_top_index);
    h = myos_hash_u64(h, (unsigned)wm->app_menu_bar_hot);
    h = myos_hash_u64(h, (unsigned)wm->app_menu_level_count);

    if (wm->app_menu_bar) {
        int bar_count = GetMenuItemCount(wm->app_menu_bar);
        h = myos_hash_u64(h, (unsigned)bar_count);
        for (int i = 0; i < bar_count && i < 32; ++i)
            h = desktop_hash_menu_item(h, wm->app_menu_bar, i);
    }

    for (int level = 0; level < APP_MENU_MAX_LEVELS; ++level) {
        HMENU popup = wm->app_menu_popup[level];
        h = myos_hash_u64(h, (unsigned)(uintptr_t)popup);
        h = myos_hash_u64(h, (unsigned)wm->app_menu_sel[level]);
        h = myos_hash_u64(h, (unsigned)wm->app_menu_x[level]);
        h = myos_hash_u64(h, (unsigned)wm->app_menu_y[level]);
        h = myos_hash_u64(h, (unsigned)wm->app_menu_w[level]);
        h = myos_hash_u64(h, (unsigned)wm->app_menu_h[level]);
        if (popup) {
            int count = GetMenuItemCount(popup);
            h = myos_hash_u64(h, (unsigned)count);
            for (int i = 0; i < count && i < 64; ++i)
                h = desktop_hash_menu_item(h, popup, i);
        }
    }
    return h ? h : 1;
}

static unsigned long long desktop_render_signature_core(Desktop* d, int include_pointer)
{
    if (!d) return 0;
    unsigned long long h = 1469598103934665603ull;

    h = myos_hash_u64(h, (unsigned)d->fb.width);
    h = myos_hash_u64(h, (unsigned)d->fb.height);
    if (include_pointer) {
        h = myos_hash_u64(h, (unsigned)d->cursor_x);
        h = myos_hash_u64(h, (unsigned)d->cursor_y);
    }
    h = myos_hash_u64(h, (unsigned)d->mouse_left_down);
    h = myos_hash_u64(h, (unsigned)d->mouse_right_down);

    HWNDManagerStats hs;
    memset(&hs, 0, sizeof(hs));
    hwnd_get_stats(&d->mgr, &hs);
    h = myos_hash_bytes(h, &hs, sizeof(hs));

    WindowManager* wm = &d->wm;
    h = myos_hash_u64(h, (unsigned)wm->count);
    h = myos_hash_u64(h, (unsigned)wm->focused);
    h = myos_hash_u64(h, (unsigned)wm->drag_mode);
    h = myos_hash_u64(h, (unsigned)wm->drag_idx);
    h = myos_hash_u64(h, (unsigned)wm->menu_open);
    h = myos_hash_u64(h, (unsigned)wm->menu_kind);
    h = myos_hash_u64(h, (unsigned)wm->menu_selected);
    h = myos_hash_u64(h, (unsigned)wm->menu_loop_serial);
    h = myos_hash_u64(h, desktop_menu_visual_signature(wm));
    h = myos_hash_u64(h, (unsigned)wm->app_menu_hwnd);
    h = myos_hash_u64(h, (unsigned)wm->app_menu_bar_hot);
    h = myos_hash_u64(h, (unsigned)wm->desktop_icon_count);
    h = myos_hash_u64(h, (unsigned)wm->desktop_selected);
    h = myos_hash_u64(h, (unsigned)wm->desktop_drag_idx);
    h = myos_hash_u64(h, (unsigned)wm->desktop_dragging);
    h = myos_hash_u64(h, (unsigned)wm->shell_msg_count);
    h = myos_hash_u64(h, (unsigned)wm->shell_cmd_count);
    h = myos_hash_u64(h, (unsigned)wm->state_version);
    h = myos_hash_cstr(h, wm->wallpaper_status);
    h = myos_hash_cstr(h, wm->wallpaper_path);

    for (int i = 0; i < wm->desktop_icon_count && i < MAX_DESKTOP_ICONS; ++i) {
        const DesktopIcon* ic = &wm->desktop_icons[i];
        h = myos_hash_u64(h, (unsigned)ic->x);
        h = myos_hash_u64(h, (unsigned)ic->y);
        h = myos_hash_u64(h, (unsigned)ic->w);
        h = myos_hash_u64(h, (unsigned)ic->h);
        h = myos_hash_u64(h, (unsigned)ic->is_dir);
        h = myos_hash_cstr(h, ic->name);
    }

    for (int i = 0; i < wm->count && i < MAX_WINDOWS; ++i) {
        Window* w = &wm->wins[i];
        h = myos_hash_u64(h, (unsigned)w->x);
        h = myos_hash_u64(h, (unsigned)w->y);
        h = myos_hash_u64(h, (unsigned)w->w);
        h = myos_hash_u64(h, (unsigned)w->h);
        h = myos_hash_u64(h, (unsigned)w->active);
        h = myos_hash_u64(h, (unsigned)w->closed);
        h = myos_hash_u64(h, (unsigned)w->minimized);
        h = myos_hash_u64(h, (unsigned)w->maximized);
        h = myos_hash_u64(h, (unsigned)w->app_type);
        h = myos_hash_u64(h, (unsigned)w->app_hwnd);
        h = myos_hash_u64(h, (unsigned)w->process_id);
        h = myos_hash_cstr(h, w->title);
        h = myos_hash_cstr(h, w->ipc_status);
        if (!w->closed) {
            DWORD ownerVisualPid = 0;
            HWND ownerVisualRoot = 0;
            if (w->app_type == APP_TERMINAL) {
                ownerVisualPid = w->term ? w->term->cap.id : 0;
                ownerVisualRoot = w->term ? w->term->hwnd : 0;
            } else if (w->app_type == APP_IPC_PROXY) {
                ownerVisualPid = w->process_id;
                ownerVisualRoot = w->app_hwnd;
            } else {
                ownerVisualPid = w->app_cap.id;
                ownerVisualRoot = w->app_hwnd;
            }
            h = myos_hash_u64(h, hwnd_get_owner_visual_signature(&d->mgr, ownerVisualPid, ownerVisualRoot));
        }

        if (!w->closed && w->app_type == APP_TERMINAL && w->term) {
            Terminal* t = w->term;
            h = myos_hash_u64(h, (unsigned)t->row);
            h = myos_hash_u64(h, (unsigned)t->blink);
            h = myos_hash_u64(h, (unsigned)t->input_len);
            h = myos_hash_u64(h, (unsigned)t->pipe_fd);
            h = myos_hash_bytes(h, t->input, sizeof(t->input));
            h = myos_hash_bytes(h, t->buf, sizeof(t->buf));
        } else if (!w->closed && w->app_type == APP_IPC_PROXY && w->process_id) {
            MyProcessHostInfo hi;
            memset(&hi, 0, sizeof(hi));
            if (MyProcessHostGetInfo(w->process_id, &hi)) {
                h = myos_hash_u64(h, (unsigned)hi.gui_msg_sent);
                h = myos_hash_u64(h, (unsigned)hi.gui_msg_received);
                h = myos_hash_u64(h, (unsigned)hi.gui_msg_dispatched);
                h = myos_hash_u64(h, (unsigned)hi.gui_post_request);
                h = myos_hash_u64(h, (unsigned)hi.gui_post_ack);
                h = myos_hash_u64(h, (unsigned)hi.child_hwnd_request);
                h = myos_hash_u64(h, (unsigned)hi.child_hwnd_ack);
                h = myos_hash_u64(h, (unsigned)hi.child_hwnd_count);
                h = myos_hash_u64(h, (unsigned)hi.child_hwnd_command_count);
                h = myos_hash_u64(h, (unsigned)hi.gdi_enabled);
                h = myos_hash_u64(h, (unsigned)hi.gdi_sequence);
                h = myos_hash_u64(h, (unsigned)hi.gdi_paint_count);
                h = myos_hash_u64(h, (unsigned)hi.gdi_command_count);
                h = myos_hash_u64(h, (unsigned)hi.gdi_client_w);
                h = myos_hash_u64(h, (unsigned)hi.gdi_client_h);
                h = myos_hash_u64(h, (unsigned)hi.surface_enabled);
                h = myos_hash_u64(h, (unsigned)hi.surface_mapped);
                h = myos_hash_u64(h, (unsigned)hi.surface_seq);
                h = myos_hash_u64(h, (unsigned)hi.surface_paint_count);
                h = myos_hash_u64(h, (unsigned)hi.surface_dirty_left);
                h = myos_hash_u64(h, (unsigned)hi.surface_dirty_top);
                h = myos_hash_u64(h, (unsigned)hi.surface_dirty_right);
                h = myos_hash_u64(h, (unsigned)hi.surface_dirty_bottom);
                h = myos_hash_cstr(h, hi.gdi_status);
                h = myos_hash_cstr(h, hi.surface_status);
            }
        }
    }
    return h ? h : 1;
}

static unsigned long long desktop_render_signature(Desktop* d)
{
    return desktop_render_signature_core(d, 1);
}

static unsigned long long desktop_render_signature_nopointer(Desktop* d)
{
    return desktop_render_signature_core(d, 0);
}

static void draw_cursor(Desktop* d)
{
    int x = d->cursor_x, y = d->cursor_y;
    fb_rect(&d->fb, x-6, y,   13, 1, BLACK);
    fb_rect(&d->fb, x,   y-6, 1, 13, BLACK);
    fb_rect(&d->fb, x-5, y,   11, 1, WHITE);
    fb_rect(&d->fb, x,   y-5, 1, 11, WHITE);
}

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void draw_debug_badge(Desktop* d)
{
    // Absichtlich oben links, damit man sofort sieht: richtige Build-Version läuft.
    // v80: auf kleinen Framebuffern nicht mehr über den rechten Rand laufen lassen.
    int bw = d->fb.width - 4;
    if (bw > 900) bw = 900;
    if (bw < 320) bw = d->fb.width > 8 ? d->fb.width - 8 : d->fb.width;
    fb_rect(&d->fb, 2, 2, bw, 30, COLOR(10, 40, 60));
    fb_rect_outline(&d->fb, 2, 2, bw, 30, COLOR(120, 180, 220));
    HWNDManagerStats st;
    hwnd_get_stats(&d->mgr, &st);
    char line1[192];
    char line2[160];
    MyWindowLifetimeAudit la;
    memset(&la, 0, sizeof(la));
    wm_get_lifetime_audit_stats(&la);
    MyProcessHostAudit pha;
    memset(&pha, 0, sizeof(pha));
    MyProcessHostGetAuditStats(&pha);
    MyHandleTableAudit hta;
    memset(&hta, 0, sizeof(hta));
    MyWinGetHandleTableAudit(&hta);
    MyWaitAudit wa;
    memset(&wa, 0, sizeof(wa));
    MyWinGetWaitAudit(&wa);
    snprintf(line1, sizeof(line1),
             "v190 menu loop | HWND=%u OBJ=%u PROC=%u/%u WSTS=%u/%u PH=%u/%u ipc=%u/%u err=%u LDR=%u/%u",
             st.hwnd_count, MyGetObjectCount(),
             MyGetProcessLiveCount(), MyGetProcessExitedCount(), st.state_active, st.state_destroyed,
             pha.running_count, pha.final_count, pha.open_ipc_resources, pha.final_open_ipc_resources,
             pha.cleanup_error_count, la.loader_close_ok, la.loader_close_fail);
    snprintf(line2, sizeof(line2),
             "HT=%u pid=%u dead=%u orphan=%u dup=%u/%u W=%u/%u/%u abn=%u fail=%u HUNG=%u",
             hta.total_handles, hta.owner_pid_count, hta.exited_owner_handles, hta.orphan_owner_handles,
             hta.duplicate_success, hta.duplicate_cross_process,
             wa.wait_success, wa.wait_timeouts, wa.wait_all_commits + wa.wait_any_commits, wa.mutex_abandoned,
             wa.wait_failures, st.hung_windows);
    font_draw_str(&d->fb, 8, 7, line1, WHITE);
    font_draw_str(&d->fb, 8, 19, line2, COLOR(210,230,255));
}

static void redraw(Desktop* d)
{
    if (!d) return;
    if (d->damage_count <= 0) {
        d->damage_count = 1;
        d->damage_full = 1;
        d->damage[0].x = 0;
        d->damage[0].y = 0;
        d->damage[0].w = d->fb.width;
        d->damage[0].h = d->fb.height;
    }

    /* v176: redraw only marked damage rectangles.  The drawing code remains
       ordinary Win32-style immediate GDI for now, but the framebuffer layer
       clips all primitives to the active damage rect and only flips that rect
       to the physical framebuffer.  This makes the compositor damage-driven
       without rewriting every control/app renderer in one risky step. */
    for (int i = 0; i < d->damage_count; ++i) {
        DesktopDamageRect r = d->damage[i];
        if (r.w <= 0 || r.h <= 0) continue;
        fb_set_clip(&d->fb, r.x, r.y, r.w, r.h);
        wm_draw(&d->wm, &d->fb);
        draw_debug_badge(d);
        draw_cursor(d);
        fb_reset_clip(&d->fb);
        fb_flip_rect(&d->fb, r.x, r.y, r.w, r.h);
    }
    desktop_damage_clear(d);
}

static WPARAM desktop_mouse_key_state(const Desktop* d)
{
    WPARAM wp = 0;
    if (d->mouse_left_down)  wp |= MK_LBUTTON;
    if (d->mouse_right_down) wp |= MK_RBUTTON;
    if (d->wm.shift_held)    wp |= MK_SHIFT;
    if (d->wm.ctrl_held)     wp |= MK_CONTROL;
    return wp;
}

static Capability* desktop_cap_for_hwnd(Desktop* d, HWND hwnd)
{
    int idx = -1;
    HWND cur = hwnd;
    while (cur) {
        idx = wm_find_hwnd(&d->wm, cur);
        if (idx >= 0) break;
        cur = GetParent(cur);
    }
    if (idx < 0) {
        HWND owner = MyGetDialogOwner(hwnd);
        if (owner) idx = wm_find_hwnd(&d->wm, owner);
    }
    if (idx < 0 || idx >= d->wm.count || d->wm.wins[idx].closed) return NULL;
    Window* w = &d->wm.wins[idx];
    return (w->app_type == APP_TERMINAL) ? (w->term ? &w->term->cap : NULL) : &w->app_cap;
}

static LPARAM desktop_mouse_lparam_for_hwnd(HWND hwnd, int screen_x, int screen_y)
{
    POINT pt;
    pt.x = screen_x;
    pt.y = screen_y;
    if (!ScreenToClient(hwnd, &pt)) {
        pt.x = screen_x;
        pt.y = screen_y;
    }
    return MAKELPARAM((WORD)pt.x, (WORD)pt.y);
}

/* v77.3: for top-level app client delivery, do not depend solely on the
   generic ScreenToClient() path.  The shell HWND migration made Desktop/
   Taskbar real HWNDs, but the desktop frame is still a WindowManager frame
   with a classic non-client area.  App WndProcs expect coordinates relative
   to the *client* area: x - (frame.x+1), y - (frame.y+TITLEBAR_H).

   Child HWNDs still use ScreenToClient(child), because their origin is relative
   to their parent client. */
static LPARAM desktop_mouse_lparam_for_client_endpoint(Desktop* d, HWND hwnd, int screen_x, int screen_y)
{
    if (d && hwnd && GetCapture() == hwnd && MyIsDialogWindow(hwnd)) {
        HWND parent = GetParent(hwnd);
        if (parent) return desktop_mouse_lparam_for_client_endpoint(d, parent, screen_x, screen_y);
    }
    if (d && hwnd) {
        int idx = wm_find_hwnd(&d->wm, hwnd);
        HWND parent = 0;
        if (idx < 0) {
            parent = GetParent(hwnd);
            if (parent) idx = wm_find_hwnd(&d->wm, parent);
        }
        if (idx >= 0 && idx < d->wm.count) {
            Window* w = &d->wm.wins[idx];
            HWND top = (w->app_type == APP_TERMINAL) ? (w->term ? w->term->hwnd : 0) : w->app_hwnd;
            if (hwnd == top) {
                int cx = screen_x - (w->x + 1);
                int cy = screen_y - (w->y + TITLEBAR_H);
                return MAKELPARAM((WORD)cx, (WORD)cy);
            }
        }
    }
    return desktop_mouse_lparam_for_hwnd(hwnd, screen_x, screen_y);
}

static int post_client_mouse_message(Desktop* d, UINT msg, int screen_x, int screen_y, WPARAM key_state)
{
    HWND hwnd = GetCapture();
    HWND capture_hwnd = hwnd;
    Capability* cap = NULL;
    uint32_t route_reason = hwnd ? _MSG_ROUTE_REASON_CAPTURE : _MSG_ROUTE_REASON_HITTEST;
    uint32_t route_state = hwnd ? _MSG_ROUTE_CAPTURED : _MSG_ROUTE_TARGET_RESOLVED;

    if (hwnd) {
        cap = desktop_cap_for_hwnd(d, hwnd);
        if (!cap) {
            /* v77.3: a stale capture on a Shell HWND (START/Desktop/Taskbar)
               must not black-hole later app-client clicks.  Real Win32 capture
               belongs to the thread/window receiving mouse input; here the raw
               Linux router is global, so clear invalid/stale capture and fall
               back to normal WindowFromPoint/focused-client routing. */
            if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MOUSEMOVE) {
                ReleaseCapture();
                hwnd = 0;
                capture_hwnd = 0;
                route_reason = _MSG_ROUTE_REASON_HITTEST;
                route_state = _MSG_ROUTE_TARGET_RESOLVED;
            } else {
                return 0;
            }
        }
    }

    if (!hwnd) {
        if (!wm_client_endpoint_at_focus(&d->wm, screen_x, screen_y, &hwnd, &cap))
            return 0;
    }

    LPARAM lp = desktop_mouse_lparam_for_client_endpoint(d, hwnd, screen_x, screen_y);
    _MsgRouteDescriptor route;
    memset(&route, 0, sizeof(route));
    route.cbSize = sizeof(route);
    route.lane = _MSG_LANE_INPUT;
    route.input_kind = mymsg_default_input_kind(msg);
    route.route_state = route_state;
    route.route_reason = route_reason;
    route.target_hwnd = hwnd;
    route.capture_hwnd = capture_hwnd;
    route.hit_hwnd = hwnd;
    route.hwnd_action = mymsg_required_hwnd_action_for_route(route.lane, route.input_kind, route.route_reason);
    return hwnd_post_routed(&d->mgr, cap, hwnd, msg, key_state, lp, &route);
}

static int post_client_wheel_message(Desktop* d, int screen_x, int screen_y, int wheel_steps)
{
    HWND hwnd = 0;
    Capability* cap = NULL;
    /* v92.2: Mouse wheel is a hover hit-test path, not a focused-window path.
       This mirrors modern Windows behavior and, more importantly for myOS,
       lets LISTBOX/COMBOBOX/SCROLLBAR react when the cursor is physically over
       that control even if another top-level window owns focus. */
    if (!wm_client_endpoint_at_point(&d->wm, screen_x, screen_y, &hwnd, &cap))
        return 0;
    int delta = wheel_steps * WHEEL_DELTA;
    WPARAM wp = MAKEWPARAM((WORD)desktop_mouse_key_state(d), (WORD)delta);
    LPARAM lp = MAKELPARAM((WORD)screen_x, (WORD)screen_y);
    _MsgRouteDescriptor route;
    memset(&route, 0, sizeof(route));
    route.cbSize = sizeof(route);
    route.lane = _MSG_LANE_INPUT;
    route.input_kind = _MSG_INPUT_MOUSE_WHEEL;
    route.route_state = _MSG_ROUTE_TARGET_RESOLVED;
    route.route_reason = _MSG_ROUTE_REASON_HOVER | _MSG_ROUTE_REASON_HITTEST;
    route.target_hwnd = hwnd;
    route.hit_hwnd = hwnd;
    route.hwnd_action = mymsg_required_hwnd_action_for_route(route.lane, route.input_kind, route.route_reason);
    return hwnd_post_routed(&d->mgr, cap, hwnd, WM_MOUSEWHEEL, wp, lp, &route);
}

static void on_new_window(int x, int y, void* ctx);

// ── Input Handler ─────────────────────────────


static char desktop_keycode_to_char(int key, int shift)
{
    static const char normal[128] = {
        [KEY_A]='a',[KEY_B]='b',[KEY_C]='c',[KEY_D]='d',[KEY_E]='e',
        [KEY_F]='f',[KEY_G]='g',[KEY_H]='h',[KEY_I]='i',[KEY_J]='j',
        [KEY_K]='k',[KEY_L]='l',[KEY_M]='m',[KEY_N]='n',[KEY_O]='o',
        [KEY_P]='p',[KEY_Q]='q',[KEY_R]='r',[KEY_S]='s',[KEY_T]='t',
        [KEY_U]='u',[KEY_V]='v',[KEY_W]='w',[KEY_X]='x',[KEY_Y]='y',
        [KEY_Z]='z',[KEY_1]='1',[KEY_2]='2',[KEY_3]='3',[KEY_4]='4',[KEY_5]='5',
        [KEY_6]='6',[KEY_7]='7',[KEY_8]='8',[KEY_9]='9',[KEY_0]='0',
        [KEY_SPACE]=' ',[KEY_DOT]='.',[KEY_COMMA]=',',[KEY_MINUS]='-',
        [KEY_EQUAL]='=',[KEY_SEMICOLON]=';',[KEY_SLASH]='/',
        [KEY_APOSTROPHE]='\'',[KEY_BACKSLASH]='\\',[KEY_GRAVE]='`',
        [KEY_LEFTBRACE]='[',[KEY_RIGHTBRACE]=']'
    };
    static const char shifted[128] = {
        [KEY_A]='A',[KEY_B]='B',[KEY_C]='C',[KEY_D]='D',[KEY_E]='E',
        [KEY_F]='F',[KEY_G]='G',[KEY_H]='H',[KEY_I]='I',[KEY_J]='J',
        [KEY_K]='K',[KEY_L]='L',[KEY_M]='M',[KEY_N]='N',[KEY_O]='O',
        [KEY_P]='P',[KEY_Q]='Q',[KEY_R]='R',[KEY_S]='S',[KEY_T]='T',
        [KEY_U]='U',[KEY_V]='V',[KEY_W]='W',[KEY_X]='X',[KEY_Y]='Y',
        [KEY_Z]='Z',[KEY_1]='!',[KEY_2]='"',[KEY_3]='#',[KEY_4]='$',[KEY_5]='%',
        [KEY_6]='&',[KEY_7]='/',[KEY_8]='(',[KEY_9]=')',[KEY_0]='=',
        [KEY_SPACE]=' ',[KEY_DOT]=':',[KEY_COMMA]=';',[KEY_MINUS]='_',
        [KEY_EQUAL]='+',[KEY_SEMICOLON]=':',[KEY_SLASH]='?',
        [KEY_GRAVE]='~',[KEY_LEFTBRACE]='{',[KEY_RIGHTBRACE]='}'
    };
    if (key < 0 || key >= 128) return 0;
    return shift ? shifted[key] : normal[key];
}


static LPARAM desktop_key_state_lparam(const Desktop* d)
{
    LPARAM st = 0;
    if (!d) return 0;
    if (d->wm.shift_held) st |= MYOS_KEYSTATE_SHIFT;
    if (d->wm.ctrl_held)  st |= MYOS_KEYSTATE_CTRL;
    if (d->wm.alt_held)   st |= MYOS_KEYSTATE_ALT;
    return st;
}

static int desktop_focused_key_target(Desktop* d, HWND* outHwnd, Capability** outCap)
{
    if (outHwnd) *outHwnd = 0;
    if (outCap) *outCap = NULL;
    if (!d || d->wm.focused < 0 || d->wm.focused >= d->wm.count) return 0;
    Window* w = &d->wm.wins[d->wm.focused];
    if (w->closed || w->minimized) return 0;
    if (w->app_type == APP_TERMINAL) {
        if (!w->term || !w->term->hwnd) return 0;
        if (outHwnd) *outHwnd = w->term->hwnd;
        if (outCap) *outCap = &w->term->cap;
        return 1;
    }
    if (!w->app_hwnd) return 0;
    HWND focus = GetFocus();
    if (focus && (focus == w->app_hwnd || IsChild(w->app_hwnd, focus) || MyIsOwnedDialogChild(w->app_hwnd, focus)) && IsWindowEnabled(focus)) {
        if (outHwnd) *outHwnd = focus;
        if (outCap) *outCap = &w->app_cap;
        return 1;
    }
    if (!IsWindowEnabled(w->app_hwnd)) return 0;
    if (outHwnd) *outHwnd = w->app_hwnd;
    if (outCap) *outCap = &w->app_cap;
    return 1;
}

static int desktop_post_focused_key(Desktop* d, UINT msg, int keycode, LPARAM key_state)
{
    HWND hwnd = 0;
    Capability* cap = NULL;
    if (!desktop_focused_key_target(d, &hwnd, &cap)) return 0;
    uint32_t kind = mymsg_default_input_kind(msg);
    _MsgRouteDescriptor route;
    memset(&route, 0, sizeof(route));
    route.cbSize = sizeof(route);
    route.lane = _MSG_LANE_INPUT;
    route.input_kind = kind;
    route.route_state = _MSG_ROUTE_FOCUS;
    route.route_reason = _MSG_ROUTE_REASON_FOCUS;
    route.target_hwnd = hwnd;
    route.focus_hwnd = hwnd;
    route.hwnd_action = mymsg_required_hwnd_action_for_route(route.lane, route.input_kind, route.route_reason);
    return hwnd_post_routed(&d->mgr, cap, hwnd, msg, (WPARAM)keycode, key_state, &route);
}

static void desktop_translate_key_to_char(Desktop* d, int keycode, LPARAM key_state, int system_key)
{
    char ch = desktop_keycode_to_char(keycode, d ? d->wm.shift_held : 0);
    if (!ch) return;
    HWND hwnd = 0;
    Capability* cap = NULL;
    if (!desktop_focused_key_target(d, &hwnd, &cap)) return;
    UINT msg = system_key ? WM_SYSCHAR : WM_CHAR;
    _MsgRouteDescriptor route;
    memset(&route, 0, sizeof(route));
    route.cbSize = sizeof(route);
    route.lane = _MSG_LANE_INPUT;
    route.input_kind = _MSG_INPUT_CHAR;
    route.route_state = _MSG_ROUTE_FOCUS;
    route.route_reason = _MSG_ROUTE_REASON_FOCUS;
    route.target_hwnd = hwnd;
    route.focus_hwnd = hwnd;
    route.hwnd_action = mymsg_required_hwnd_action_for_route(route.lane, route.input_kind, route.route_reason);
    hwnd_post_routed(&d->mgr, cap, hwnd, msg,
                     (WPARAM)(unsigned char)ch, key_state, &route);
}

static int desktop_is_debug_function_key(int key)
{
    return key == KEY_F2 || key == KEY_F3 || key == KEY_F4 || key == KEY_F5 ||
           key == KEY_F6 || key == KEY_F7 || key == KEY_F8 || key == KEY_F9 ||
           key == KEY_F10 || key == KEY_F11 || key == KEY_F12 || key == KEY_F13 ||
           key == KEY_F14 || key == KEY_F15 || key == KEY_F16 || key == KEY_F17;
}
static HWND desktop_dialog_root_for_message(HWND hwnd)
{
    if (!hwnd) return 0;
    if (MyIsDialogWindow(hwnd)) return hwnd;

    HWND cur = hwnd;
    HWND parent = GetParent(cur);
    HWND nearestParent = parent;
    while (parent) {
        if (MyIsDialogWindow(parent)) return parent;
        cur = parent;
        parent = GetParent(cur);
    }

    /* For normal app child controls (ControlLab etc.) IsDialogMessage-style
       tab traversal uses the app HWND as the dialog root.  The v88 regression
       passed the focused child itself as hDlg after the first Tab, so the cycle
       got stuck on the first control. */
    return nearestParent ? nearestParent : hwnd;
}

static int desktop_dialog_message_key(Desktop* d, int key, LPARAM key_state, int system_key)
{
    if (!d) return 0;
    if (!system_key && !(key == KEY_TAB || key == KEY_SPACE || key == KEY_ENTER || key == KEY_ESC ||
                         key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
                         key == KEY_HOME || key == KEY_END || key == KEY_PAGEUP || key == KEY_PAGEDOWN)) return 0;
    HWND hwnd = 0;
    Capability* cap = NULL;
    if (!desktop_focused_key_target(d, &hwnd, &cap) || !hwnd || !cap) return 0;
    HWND root = desktop_dialog_root_for_message(hwnd);
    if (!root) root = hwnd;
    MSG m;
    memset(&m, 0, sizeof(m));
    m.hwnd = hwnd;
    m.message = system_key ? WM_SYSKEYDOWN : WM_KEYDOWN;
    m.wParam = (WPARAM)key;
    m.lParam = key_state;
    const Capability* prevPtr = MyWinGetCurrentCapability();
    Capability prevCap;
    int havePrev = 0;
    if (prevPtr) { prevCap = *prevPtr; havePrev = 1; }
    MyWinBindRuntime(&d->mgr, cap);
    MyWinBindDesktop(&d->wm);
    int handled = MyTranslateModelessDialogMessageA(&m) ? 1 : 0;
    if (!handled) handled = IsDialogMessageA(root, &m) ? 1 : 0;
    /* v167: Tab is a dialog-manager contract, not a default-button fallback.
       If a newer dialog/control family forgets one edge of the normal
       IsDialogMessage path, keep the key consumed and explicitly wrap over
       the parent/root tabstops.  This prevents the classic failure where Tab
       on the last control falls through into the raw key path and activates
       IDOK/IDCANCEL through DefDlgProc instead of cycling to the first item. */
    if (!handled && !system_key && key == KEY_TAB && root && IsWindow(root)) {
        HWND cur = GetFocus();
        int shift = (key_state & MYOS_KEYSTATE_SHIFT) ? 1 : 0;
        HWND next = GetNextDlgTabItem(root, (cur && cur != root) ? cur : 0, shift ? TRUE : FALSE);
        if (!next) next = GetNextDlgTabItem(root, 0, shift ? TRUE : FALSE);
        if (next && IsWindow(next)) { SetFocus(next); handled = 1; }
        else if (MyIsDialogWindow(root)) handled = 1;
    }
    if (havePrev) { MyWinBindRuntime(&d->mgr, &prevCap); MyWinBindDesktop(&d->wm); }
    return handled;
}


typedef struct DesktopMdiClientFindCtx {
    HWND hClient;
} DesktopMdiClientFindCtx;

static BOOL CALLBACK desktop_find_mdi_client_cb(HWND hWnd, LPARAM lParam)
{
    DesktopMdiClientFindCtx* ctx = (DesktopMdiClientFindCtx*)lParam;
    if (!ctx || ctx->hClient) return FALSE;
    char cls[64];
    cls[0] = 0;
    if (GetClassNameA(hWnd, cls, sizeof(cls)) > 0 && strcmp(cls, "MDICLIENT") == 0) {
        ctx->hClient = hWnd;
        return FALSE;
    }
    return TRUE;
}

static HWND desktop_find_mdi_client(HWND hFrame)
{
    if (!hFrame || !IsWindow(hFrame)) return 0;
    DesktopMdiClientFindCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    EnumChildWindows(hFrame, desktop_find_mdi_client_cb, (LPARAM)&ctx);
    return ctx.hClient;
}

static HWND desktop_mdi_get_active_child(HWND hFrame, HWND* lpClient)
{
    if (lpClient) *lpClient = 0;
    HWND hClient = desktop_find_mdi_client(hFrame);
    if (!hClient || !IsWindow(hClient)) return 0;
    if (lpClient) *lpClient = hClient;

    DWORD ownerPid = 0;
    GetWindowThreadProcessId(hClient, &ownerPid);
    BOOL entered = FALSE;
    if (ownerPid) entered = MyWinEnterProcessContext(ownerPid);
    HWND active = (HWND)SendMessageA(hClient, WM_MDIGETACTIVE, 0, 0);
    if (entered) MyWinLeaveProcessContext();
    return active;
}

static int desktop_close_active_mdi_child(Desktop* d)
{
    if (!d) return 0;
    HWND hFrame = wm_get_foreground_hwnd(&d->wm);
    if (!hFrame || !IsWindow(hFrame)) return 0;

    HWND hClient = 0;
    HWND active = desktop_mdi_get_active_child(hFrame, &hClient);
    if (!hClient || !active || !IsWindow(active)) return 0;

    DWORD ownerPid = 0;
    GetWindowThreadProcessId(active, &ownerPid);
    BOOL entered = FALSE;
    if (ownerPid) entered = MyWinEnterProcessContext(ownerPid);
    SendMessageA(active, WM_CLOSE, 0, 0);
    if (entered) MyWinLeaveProcessContext();
    return 1;
}


static void on_key_down(void* ctx, const Message* msg)
{
    Desktop* d = (Desktop*)ctx;
    pthread_mutex_lock(&d->lock);

    int key = (int)msg->val1;

    /* v85: an active popup/menu loop owns keyboard first.  ESC cancels the
       menu instead of terminating the OS; Up/Down/Enter/hotletters are handled
       before focus/accelerator/debug paths. */
    if (d->wm.menu_open && key != KEY_LEFTALT && key != KEY_RIGHTALT &&
        key != KEY_LEFTSHIFT && key != KEY_RIGHTSHIFT &&
        key != KEY_LEFTCTRL && key != KEY_RIGHTCTRL) {
        wm_menu_handle_key(&d->wm, key);
        d->dirty = 1;
        pthread_mutex_unlock(&d->lock);
        return;
    }

    MyWinSetKeyDown(key, TRUE);

    /* v84: Keyboard input now has an explicit Win32-style order:
       raw Linux key -> modifier state -> WM_KEYDOWN/WM_SYSKEYDOWN ->
       TranslateMessage-style WM_CHAR/WM_SYSCHAR.  Debug F-key launchers only
       run after system keys/accelerators had first chance. */
    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) d->wm.shift_held = 1;
    if (key == KEY_LEFTCTRL  || key == KEY_RIGHTCTRL)  d->wm.ctrl_held = 1;
    if (key == KEY_LEFTALT   || key == KEY_RIGHTALT) { d->wm.alt_held = 1; d->wm.alt_menu_armed = 1; }
    else if (d->wm.alt_held) d->wm.alt_menu_armed = 0;

    LPARAM key_state = desktop_key_state_lparam(d);
    int system_key = d->wm.alt_held ? 1 : 0;

    if (key == KEY_LEFTALT || key == KEY_RIGHTALT) {
        desktop_post_focused_key(d, WM_SYSKEYDOWN, key, key_state);
        d->dirty = 1;
        pthread_mutex_unlock(&d->lock);
        return;
    }
    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT ||
        key == KEY_LEFTCTRL  || key == KEY_RIGHTCTRL) {
        desktop_post_focused_key(d, system_key ? WM_SYSKEYDOWN : WM_KEYDOWN, key, key_state);
        d->dirty = 1;
        pthread_mutex_unlock(&d->lock);
        return;
    }

    /* System accelerators before legacy/debug launchers. */
    if (d->wm.alt_held && key == KEY_F4 && d->wm.focused >= 0 && !d->wm.wins[d->wm.focused].closed) {
        HWND hwnd = wm_get_foreground_hwnd(&d->wm);
        if (hwnd) {
            desktop_post_focused_key(d, WM_SYSKEYDOWN, key, key_state);
            wm_def_window_proc(&d->wm, hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            d->dirty = 1;
            pthread_mutex_unlock(&d->lock);
            return;
        }
    }

    if (d->wm.alt_held && key == KEY_SPACE && d->wm.focused >= 0 && !d->wm.wins[d->wm.focused].closed) {
        HWND hwnd = wm_get_foreground_hwnd(&d->wm);
        if (hwnd) {
            desktop_post_focused_key(d, WM_SYSKEYDOWN, key, key_state);
            wm_def_window_proc(&d->wm, hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0);
            d->dirty = 1;
            pthread_mutex_unlock(&d->lock);
            return;
        }
    }

    /* v101: Alt+mnemonic and F10 activate the real HMENU menubar before
       dialog translation/app accelerators.  Alt+Space remains system-menu. */
    if (((d->wm.alt_held && key != KEY_F4 && key != KEY_SPACE) || key == KEY_F10) &&
        wm_activate_app_menu(&d->wm, key == KEY_F10 ? 0 : key)) {
        desktop_post_focused_key(d, system_key ? WM_SYSKEYDOWN : WM_KEYDOWN, key, key_state);
        d->dirty = 1;
        pthread_mutex_unlock(&d->lock);
        return;
    }

    /* v86: dialog manager owns Tab/Shift+Tab/Space/Enter/Esc before
       app accelerators and debug F-keys.  This mirrors IsDialogMessage: controls
       get focus traversal and default/cancel behavior without every lab
       re-implementing keyboard navigation. */
    if (desktop_dialog_message_key(d, key, key_state, system_key)) {
        d->dirty = 1;
        pthread_mutex_unlock(&d->lock);
        return;
    }

    if (key == KEY_ESC) {
        /* v141: an active MDI child is the keyboard subfocus of the frame.
           Plain Escape must close that child first; only when no child remains
           does Escape fall through to the top-level frame close. */
        if (desktop_close_active_mdi_child(d)) {
            d->dirty = 1;
            pthread_mutex_unlock(&d->lock);
            return;
        }
        HWND hwnd = wm_get_foreground_hwnd(&d->wm);
        if (hwnd) {
            wm_def_window_proc(&d->wm, hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            d->dirty = 1;
            pthread_mutex_unlock(&d->lock);
            return;
        }
        pthread_mutex_unlock(&d->lock);
        fb_clear(&d->fb, BLACK); fb_flip(&d->fb); exit(0);
    }

    /* App accelerators / normal input.  Ctrl/Alt combinations are sent to the
       focused window first; plain debug F-keys below remain test launchers. */
    if (d->wm.ctrl_held || d->wm.alt_held) {
        desktop_post_focused_key(d, system_key ? WM_SYSKEYDOWN : WM_KEYDOWN, key, key_state);
        if (d->wm.alt_held) desktop_translate_key_to_char(d, key, key_state, 1);
        d->dirty = 1;
        pthread_mutex_unlock(&d->lock);
        return;
    }

    /* Debug-/Fallback-Hotkeys, damit Features auch ohne perfekte Maus-Device-Erkennung testbar sind.
       v84: only plain F-keys reach this table; Alt/Ctrl combos are real key messages. */
    if (desktop_is_debug_function_key(key)) {
        if (key == KEY_F2) {
            d->wm.menu_open = 1;
            d->wm.menu_from_start = 1;
            d->wm.menu_x = 4;
            d->wm.menu_y = d->fb.height - TASKBAR_H - (24 * 22 + 2) - 2;
            if (d->wm.menu_y < 2) d->wm.menu_y = 2;
            d->dirty = 1;
            pthread_mutex_unlock(&d->lock);
            return;
        }
        if (key == KEY_F3) {
            static uint32_t next_hotkey_cap_id = 200;
            (void)next_hotkey_cap_id++;
            ShellExecuteA(0, "open", "calc", NULL, NULL, SW_SHOW);
            d->dirty = 1;
            pthread_mutex_unlock(&d->lock);
            return;
        }
        if (key == KEY_F4) {
            on_new_window(100 + d->wm.count * 18, 100 + d->wm.count * 18, d);
            d->dirty = 1;
            pthread_mutex_unlock(&d->lock);
            return;
        }
        if (key == KEY_F5) { wm_desktop_reload(&d->wm); d->dirty = 1; pthread_mutex_unlock(&d->lock); return; }
        if (key == KEY_F6) { wm_desktop_toggle_layout(&d->wm); d->dirty = 1; pthread_mutex_unlock(&d->lock); return; }
        if (key == KEY_F7) {
            static uint32_t next_spy_cap_id = 700;
            Capability cap = cap_create(next_spy_cap_id++, "spy-hotkey", CAP_IPC|CAP_HOOK|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_WINDOW_SUBSCRIBE|CAP_PROCESS_ENUM);
            cap_add_target(&cap, 0);
            wm_add_spy(&d->wm, 220 + d->wm.count * 18, 120 + d->wm.count * 18, "myOS Spy++ [F7]", cap);
            d->dirty = 1; pthread_mutex_unlock(&d->lock); return;
        }
        if (key == KEY_F8) {
            static uint32_t next_lab_cap_id = 900;
            Capability cap = cap_create(next_lab_cap_id++, "access-lab-hotkey", CAP_IPC|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_SUBSCRIBE|CAP_PROCESS_ENUM);
            cap_add_target(&cap, 0);
            wm_add_access(&d->wm, 260 + d->wm.count * 18, 140 + d->wm.count * 18, "myOS AccessLab [F8]", cap);
            d->dirty = 1; pthread_mutex_unlock(&d->lock); return;
        }
        if (key == KEY_F9) {
            static uint32_t next_pump_cap_id = 1100;
            Capability cap = cap_create(next_pump_cap_id++, "pump-lab-hotkey", CAP_IPC|CAP_WINDOW_READ);
            cap_add_target(&cap, 0);
            wm_add_pump(&d->wm, 300 + d->wm.count * 18, 160 + d->wm.count * 18, "myOS PumpLab [F9]", cap);
            d->dirty = 1; pthread_mutex_unlock(&d->lock); return;
        }
        if (key == KEY_F10) {
            static uint32_t next_deadlock_cap_id = 1300;
            Capability cap = cap_create(next_deadlock_cap_id++, "deadlock-lab-hotkey", CAP_IPC|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_SUBSCRIBE);
            cap_add_target(&cap, 0);
            wm_add_deadlock(&d->wm, 340 + d->wm.count * 18, 180 + d->wm.count * 18, "myOS DeadlockLab [F10]", cap);
            d->dirty = 1; pthread_mutex_unlock(&d->lock); return;
        }
        if (key == KEY_F11) { MyAppHostLaunch(&d->wm, "section-lab", 380 + d->wm.count * 18, 200 + d->wm.count * 18, "SectionLab [OOP F11]", NULL, NULL); d->dirty = 1; pthread_mutex_unlock(&d->lock); return; }
        if (key == KEY_F12) { MyAppHostLaunch(&d->wm, "object-lab", 420 + d->wm.count * 18, 220 + d->wm.count * 18, "ObjectLab [OOP F12]", NULL, NULL); d->dirty = 1; pthread_mutex_unlock(&d->lock); return; }
        if (key == KEY_F13) { MyAppHostLaunch(&d->wm, "wait-lab", 460 + d->wm.count * 18, 240 + d->wm.count * 18, "WaitLab [OOP F13]", NULL, NULL); d->dirty = 1; pthread_mutex_unlock(&d->lock); return; }
        if (key == KEY_F14) { MyAppHostLaunch(&d->wm, "paint-lab", 540 + d->wm.count * 18, 280 + d->wm.count * 18, "PaintLab [OOP F14]", NULL, NULL); d->dirty = 1; pthread_mutex_unlock(&d->lock); return; }
        if (key == KEY_F15) { MyAppHostLaunch(&d->wm, "drag-lab", 580 + d->wm.count * 18, 300 + d->wm.count * 18, "DragLab [OOP F15]", NULL, NULL); d->dirty = 1; pthread_mutex_unlock(&d->lock); return; }
        if (key == KEY_F16) { MyAppHostLaunch(&d->wm, "control-lab", 620 + d->wm.count * 18, 320 + d->wm.count * 18, "ControlLab [OOP F16]", NULL, NULL); d->dirty = 1; pthread_mutex_unlock(&d->lock); return; }
        if (key == KEY_F17) {
            static uint32_t next_svc_cap_id = 3100;
            Capability cap = cap_create(next_svc_cap_id++, "service-lab-hotkey", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_PROCESS_ENUM);
            cap_add_target(&cap, 0);
            wm_add_servicelab(&d->wm, 660 + d->wm.count * 18, 340 + d->wm.count * 18, "myOS ServiceLab [F17]", cap);
            d->dirty = 1; pthread_mutex_unlock(&d->lock); return;
        }
    }

    /* Normal key path.  Parent-side apps and OOP apps still receive WM_CHAR
       from the desktop translator, because many of our child message loops do
       not call TranslateMessage yet; this mirrors TranslateMessage semantics at
       the session input layer without reintroducing direct raw-key app logic. */
    desktop_post_focused_key(d, system_key ? WM_SYSKEYDOWN : WM_KEYDOWN, key, key_state);
    if (!system_key) desktop_translate_key_to_char(d, key, key_state, 0);
    else desktop_translate_key_to_char(d, key, key_state, 1);

    d->dirty = 1;
    pthread_mutex_unlock(&d->lock);
}

static void on_key_up(void* ctx, const Message* msg)
{
    Desktop* d = (Desktop*)ctx;
    pthread_mutex_lock(&d->lock);
    int key = (int)msg->val1;
    LPARAM key_state = desktop_key_state_lparam(d);
    int was_sys = d->wm.alt_held ? 1 : 0;

    if (d->wm.menu_open) {
        MyWinSetKeyDown(key, FALSE);
        if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) d->wm.shift_held = 0;
        if (key == KEY_LEFTCTRL  || key == KEY_RIGHTCTRL)  d->wm.ctrl_held = 0;
        if (key == KEY_LEFTALT   || key == KEY_RIGHTALT) { d->wm.alt_held = 0; d->wm.alt_menu_armed = 0; }
        d->dirty = 1;
        pthread_mutex_unlock(&d->lock);
        return;
    }

    if ((key == KEY_LEFTALT || key == KEY_RIGHTALT) && d->wm.alt_menu_armed &&
        wm_activate_app_menu(&d->wm, 0)) {
        MyWinSetKeyDown(key, FALSE);
        d->wm.alt_held = 0;
        d->wm.alt_menu_armed = 0;
        d->dirty = 1;
        pthread_mutex_unlock(&d->lock);
        return;
    }

    desktop_post_focused_key(d, was_sys ? WM_SYSKEYUP : WM_KEYUP, key, key_state);

    MyWinSetKeyDown(key, FALSE);

    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) d->wm.shift_held = 0;
    if (key == KEY_LEFTCTRL  || key == KEY_RIGHTCTRL)  d->wm.ctrl_held = 0;
    if (key == KEY_LEFTALT   || key == KEY_RIGHTALT) { d->wm.alt_held = 0; d->wm.alt_menu_armed = 0; }

    d->dirty = 1;
    pthread_mutex_unlock(&d->lock);
}

static void on_mouse_move(void* ctx, const Message* msg)
{
    Desktop* d = (Desktop*)ctx;
    pthread_mutex_lock(&d->lock);

    // ABS-Geräte: Wert ist absolute Position, typischerweise 0..65535.
    int old_x = d->cursor_x;
    int old_y = d->cursor_y;
    d->cursor_x = (int)(msg->val1 * d->fb.width  / 65535);
    d->cursor_y = (int)(msg->val2 * d->fb.height / 65535);
    d->cursor_x = clamp_i(d->cursor_x, 0, d->fb.width  - 1);
    d->cursor_y = clamp_i(d->cursor_y, 0, d->fb.height - 1);

    if (old_x != d->cursor_x || old_y != d->cursor_y) {
        desktop_damage_cursor_at(d, old_x, old_y);
        desktop_damage_cursor_at(d, d->cursor_x, d->cursor_y);
    }

    if (!wm_mouse_move(&d->wm, d->cursor_x, d->cursor_y))
        post_client_mouse_message(d, WM_MOUSEMOVE, d->cursor_x, d->cursor_y, desktop_mouse_key_state(d));
    pthread_mutex_unlock(&d->lock);
}

static void on_mouse_delta(void* ctx, const Message* msg)
{
    Desktop* d = (Desktop*)ctx;
    pthread_mutex_lock(&d->lock);

    // REL-/VM-Maus: Wert ist dx/dy. Nicht skalieren, sondern addieren.
    int old_x = d->cursor_x;
    int old_y = d->cursor_y;
    d->cursor_x += (int)msg->val1;
    d->cursor_y += (int)msg->val2;
    d->cursor_x = clamp_i(d->cursor_x, 0, d->fb.width  - 1);
    d->cursor_y = clamp_i(d->cursor_y, 0, d->fb.height - 1);

    if (old_x != d->cursor_x || old_y != d->cursor_y) {
        desktop_damage_cursor_at(d, old_x, old_y);
        desktop_damage_cursor_at(d, d->cursor_x, d->cursor_y);
    }

    if (!wm_mouse_move(&d->wm, d->cursor_x, d->cursor_y))
        post_client_mouse_message(d, WM_MOUSEMOVE, d->cursor_x, d->cursor_y, desktop_mouse_key_state(d));
    pthread_mutex_unlock(&d->lock);
}

static void on_mouse_btn(void* ctx, const Message* msg)
{
    Desktop* d = (Desktop*)ctx;
    pthread_mutex_lock(&d->lock);

    int right = (msg->val1 == BTN_RIGHT);

    if (msg->val2) {
        int btn = right ? 1 : 0;
        int was_menu_open = d->wm.menu_open;
        int handled = wm_route_raw_mouse_button_down(&d->wm, d->cursor_x, d->cursor_y, btn);

        if (right) d->mouse_right_down = 1; else d->mouse_left_down = 1;

        /* v77.3: shell routing and app-client delivery are deliberately
           separated.  v77 turned the shell into real HWNDs, but a too-broad
           "handled" result could swallow WM_LBUTTONDOWN for OOP app client
           areas.  Win32-wise the raw router may handle shell/non-client, but
           app-client hits must still become WM_LBUTTONDOWN for the target
           HWND/WndProc. */
        if (!right && !was_menu_open && !handled) {
            /* v138: the raw router may now deliberately consume specific
               client-looking pixels such as an MDI child caption by sending the
               canonical WM_NCHITTEST/WM_NCLBUTTONDOWN path itself.  Do not
               re-post a second WM_LBUTTONDOWN merely because the same screen
               point also has a client endpoint; that was the remaining
               physical double-dispatch gap after the v137 command tripwire. */
            post_client_mouse_message(d, WM_LBUTTONDOWN, d->cursor_x, d->cursor_y, desktop_mouse_key_state(d));
        }
    } else {
        int btn = right ? 1 : 0;
        int handled = wm_route_raw_mouse_button_up(&d->wm, d->cursor_x, d->cursor_y, btn);

        if (right) d->mouse_right_down = 0; else d->mouse_left_down = 0;
        if (!right && !handled) {
            post_client_mouse_message(d, WM_LBUTTONUP, d->cursor_x, d->cursor_y, desktop_mouse_key_state(d));
        }
    }

    d->dirty = 1;
    pthread_mutex_unlock(&d->lock);
}

static void on_mouse_wheel(void* ctx, const Message* msg)
{
    Desktop* d = (Desktop*)ctx;
    pthread_mutex_lock(&d->lock);
    post_client_wheel_message(d, d->cursor_x, d->cursor_y, (int)msg->val1);
    /* v92.1: wheel messages often change only USER32 control state
       (LISTBOX top index, COMBOBOX selection/dropdown, SCROLLBAR pos).
       No mouse-move/button event follows, so force a compositor refresh. */
    d->dirty = 1;
    pthread_mutex_unlock(&d->lock);
}

static void on_quit(void* ctx, const Message* msg __attribute__((unused)))
{
    Desktop* d = (Desktop*)ctx;
    fb_clear(&d->fb, BLACK); fb_flip(&d->fb); exit(0);
}

typedef void (*Handler)(void*, const Message*);
static const Handler handler_table[MSG_COUNT] = {
    [MSG_KEY_DOWN]    = on_key_down,
    [MSG_KEY_UP]      = on_key_up,
    [MSG_MOUSE_MOVE]  = on_mouse_move,
    [MSG_MOUSE_DELTA] = on_mouse_delta,
    [MSG_MOUSE_BTN]   = on_mouse_btn,
    [MSG_MOUSE_WHEEL] = on_mouse_wheel,
    [MSG_SYS_QUIT]    = on_quit,
};

static void on_input(const Message* msg, void* userdata)
{
    DISPATCH(userdata, handler_table, *msg);
}

// ── Render-Thread / modal compositor idle ─────

static void desktop_modal_idle(void* arg)
{
    Desktop* d = (Desktop*)arg;
    if (!d) return;
    /* Called from USER32 modal loops while hwnd_dispatch() is intentionally
       parked inside DialogBoxIndirectParamA / GetOpenFileNameA. The current
       app queue has external_pump=1, so hwnd_dispatch() below continues
       servicing other queues but will not recursively steal the modal
       dialog queue. */
    MyWinPollAllProcesses();
    hwnd_dispatch(&d->mgr);
    pthread_mutex_lock(&d->lock);
    ipc_dispatch(&d->bus);
    wm_poll(&d->wm);
    unsigned long long sig_np = desktop_render_signature_nopointer(d);
    if (sig_np != d->render_sig_nopointer) {
        if (!desktop_damage_from_visual_delta(d))
            desktop_damage_full(d);
    }
    if (d->dirty) {
        redraw(d);
        d->dirty = 0;
        d->render_sig = desktop_render_signature(d);
        d->render_sig_nopointer = desktop_render_signature_nopointer(d);
        desktop_update_visual_damage_cache(d);
    }
    pthread_mutex_unlock(&d->lock);
}

static void* render_thread(void* arg)
{
    Desktop* d = (Desktop*)arg;
    struct timespec ts = { .tv_sec=0, .tv_nsec=16666666 };
    int blink = 0;
    while (1) {
        nanosleep(&ts, NULL);
        MyWinPollAllProcesses();
        hwnd_dispatch(&d->mgr);
        pthread_mutex_lock(&d->lock);
        ipc_dispatch(&d->bus);
        wm_poll(&d->wm);
        if (++blink >= 30) { wm_blink(&d->wm); blink=0; }
        unsigned long long sig_np = desktop_render_signature_nopointer(d);
        if (sig_np != d->render_sig_nopointer) {
            if (!desktop_damage_from_visual_delta(d))
                desktop_damage_full(d);
        }
        if (d->dirty) {
            redraw(d);
            d->dirty = 0;
            d->render_sig = desktop_render_signature(d);
            d->render_sig_nopointer = desktop_render_signature_nopointer(d);
            desktop_update_visual_damage_cache(d);
        }
        pthread_mutex_unlock(&d->lock);
    }
    return NULL;
}

static void* input_thread(void* arg)
{
    InputLayer* il = (InputLayer*)arg;
    Desktop* d = il ? (Desktop*)il->userdata : NULL;
    if (d) {
        /* v146: bind through the same centralized session-input guard used by
           raw compositor endpoints.  This covers both empty TLS and an already
           present but insufficient app capability. */
        MyWinEnsureSessionInputRuntime(&d->mgr, &d->wm, &d->wm.shell_cap);
    }
    input_layer_run(il);
    return NULL;
}

// ── Neues Terminal per Rechtsklick ────────────

static void on_new_window(int x, int y, void* ctx)
{
    Desktop* d = (Desktop*)ctx;
    static int nc = 3;
    char title[32];
    snprintf(title, sizeof(title), "Terminal [%d]", nc);

    // Jedes neue Fenster bekommt automatisch HWND + Queue
    // wm_add macht alles - kein manuelles registrieren
    Capability cap = cap_create(nc, title,
        CAP_FS_READ|CAP_FS_WRITE|CAP_EXEC|CAP_IPC);
    cap_add_path(&cap, "/home");
    cap_add_path(&cap, "/tmp");
    cap_add_target(&cap, 0);  // darf mit allen reden

    // Fenster nicht unter Taskleiste erstellen
    int safe_y = y;
    if (safe_y + 320 > d->fb.height - TASKBAR_H)
        safe_y = d->fb.height - TASKBAR_H - 320 - 10;
    if (safe_y < 0) safe_y = 10;
    wm_add(&d->wm, x, safe_y, 500, 320, title, cap);
    nc++;
}

int main(int argc, char* argv[])
{
    if (argc >= 2 && strcmp(argv[1], "--smoke") == 0) {
        return MyOSRunSmokeTests(argc - 2, argv + 2);
    }
    if (argc < 2) {
        fprintf(stderr, "sudo %s <evdev0> [evdev1] [evdev2] ...\n", argv[0]);
        fprintf(stderr, "Beispiel VMware: sudo %s /dev/input/event1 /dev/input/event2 /dev/input/event3\n", argv[0]);
        fprintf(stderr, "Smoke: %s --smoke [all|kernel32|user32|apphost|comdlg|services]\n", argv[0]);
        return 1;
    }

    Desktop d = {0};
    d.cursor_x = 80; d.cursor_y = 80; d.dirty = 1;
    pthread_mutex_init(&d.lock, NULL);

    if (fb_init(&d.fb, "/dev/fb0") != 0) return 1;

    // OS initialisieren
    _ObjectInit();
    MySvcInit();
    hwnd_manager_init(&d.mgr);
    ipc_init(&d.bus);

    // WindowManager kennt den HWND Manager und IPC Bus
    // Jedes wm_add erstellt ab jetzt automatisch ein HWND
    wm_init(&d.wm, &d.mgr, &d.bus);
    MyWinBindDesktop(&d.wm);
    MyWinSetModalIdleProc(desktop_modal_idle, &d);
    desktop_init_hwnd_state_section(&d);
    d.wm.screen_w = d.fb.width;
    d.wm.screen_h = d.fb.height;
    // v16.7: wm_init() laedt Icons noch mit 640x480-Fallback.
    // Nach fb_init kennen wir die echte Desktopgroesse, also einmal neu
    // arrangieren/laden, damit GRID den gesamten Desktop benutzen kann.
    wm_desktop_reload(&d.wm);
    d.wm.on_new_window  = on_new_window;
    d.wm.new_window_ctx = &d;

    // ── Fenster erstellen - wm_add macht alles ─
    Capability cap1 = cap_create(1, "admin-terminal",
        CAP_ADMIN);
    cap_add_path(&cap1, "/home");
    cap_add_path(&cap1, "/tmp");
    cap_add_path(&cap1, "/etc");
    cap_add_target(&cap1, 0);  // darf mit allen reden
    wm_add(&d.wm, 40, 40, 560, 360, "Terminal [ADMIN]", cap1);

    Capability cap2 = cap_create(2, "readonly-terminal",
        CAP_FS_READ|CAP_IPC);
    cap_add_path(&cap2, "/tmp");
    cap_add_target(&cap2, 0);
    wm_add(&d.wm, 380, 200, 560, 360, "Terminal [READ-ONLY]", cap2);

    int calc_x = 120;
    int calc_y = 80;
    MyAppHostLaunch(&d.wm, "calc", calc_x, calc_y, "Rechner [STARTUP]", NULL, NULL);

    InputLayer il;
    if (input_layer_init_many(&il, argc - 1, (const char* const*)&argv[1], on_input, &d) != 0) {
        fb_destroy(&d.fb); return 1;
    }

    redraw(&d);
    d.render_sig = desktop_render_signature(&d);
    d.render_sig_nopointer = desktop_render_signature_nopointer(&d);
    desktop_update_visual_damage_cache(&d);
    printf("myos - v206: pseudo handle materialization\n");
    printf("BUILD: myos_v238_menu_overlay_damage_signature - menu overlay damage/signature fix\n");
    printf("Demo hooks disabled in normal desktop runtime; hook API remains available.\n\n");

    pthread_t rtid, itid;
    pthread_create(&rtid, NULL, render_thread, &d);
    pthread_create(&itid, NULL, input_thread,  &il);
    pthread_join(itid, NULL);

    input_layer_destroy(&il);
    fb_destroy(&d.fb);
    hwnd_manager_destroy(&d.mgr);
    pthread_mutex_destroy(&d.lock);
    return 0;
}
