#pragma once
#include "fb.h"
#include "font.h"
#include "terminal.h"
#include "app_calc.h"
#include "app_editor.h"
#include "app_spy.h"
#include "app_access.h"
#include "app_pump.h"
#include "app_deadlock.h"
#include "app_section.h"
#include "app_sharedbus.h"
#include "app_object.h"
#include "app_waitlab.h"
#include "app_clipmenu.h"
#include "app_paintlab.h"
#include "app_draglab.h"
#include "app_controllab.h"
#include "app_servicelab.h"
#include "app_dialoglab.h"
#include "app_mdilab.h"
#include "hwnd.h"
#include "ipc.h"
#include "image.h"
#include "mywindow_state.h"
#include <stddef.h>

/* AUDIT(v118): Shell compositor top-level window cap. Enough for current labs;
   MDI/stress/open-everything tests will hit this before USER32 HWND tables do. */
#define MAX_WINDOWS   16
#define MAX_DESKTOP_ICONS 64
#define TITLEBAR_H    24
#define TASKBAR_H     32
#define RESIZE_GRIP    6
#define MENU_KIND_START  0
#define MENU_KIND_SYSTEM 1
#define MENU_KIND_APP    2
#define MENU_KIND_APPPOPUP 3
#define APP_MENUBAR_H   20
#define APP_MENU_MAX_LEVELS 4

// Hintergrund-Stile
typedef enum {
    BG_DARK = 0,
    BG_BLUE,
    BG_PURPLE,
    BG_TEAL,
    BG_COUNT
} BgStyle;

typedef enum {
    APP_TERMINAL = 0,
    APP_CALC     = 1,
    APP_EDITOR   = 2,
    APP_SPY      = 3,
    APP_ACCESS   = 4,
    APP_PUMP     = 5,
    APP_DEADLOCK = 6,
    APP_SECTION  = 7,
    APP_BUS_PRODUCER = 8,
    APP_BUS_CONSUMER = 9,
    APP_OBJECT   = 10,
    APP_WAITLAB  = 11,
    APP_CLIPMENU = 12,
    APP_PAINTLAB = 13,
    APP_DRAGLAB  = 14,
    APP_CONTROLLAB = 15,
    APP_SERVICELAB = 16,
    APP_IPC_PROXY = 17,
    APP_DIALOGLAB = 18,
    APP_MDILAB = 19,
} AppType;

typedef struct Window {
    int        x, y, w, h;
    int        active;
    int        closed;
    int        minimized;
    int        maximized;
    int        restore_x, restore_y, restore_w, restore_h;
    uint32_t   style;      // spätere WS_* Flags / Attribute
    char       title[64];
    AppType    app_type;
    Terminal*  term;       // gültig für APP_TERMINAL
    HWND       app_hwnd;   // gültig für Nicht-Terminal-Apps
    Capability app_cap;    // Sender-/Owner-Token für Nicht-Terminal-Apps

    // v46: Loader/AppHost metadata.  The desktop frame is now tied to a
    // PROCESS/THREAD-lite pair when launched through AppHost.  Legacy direct
    // wm_add_* paths leave these fields zero.
    DWORD      process_id;
    DWORD      thread_id;
    HANDLE     process_handle;
    HANDLE     thread_handle;
    DWORD      process_handle_owner_pid; // v181: HANDLE table owner for loader-created hProcess/hThread
    char       image_name[64];

    // v59: parent-side proxy frame created for a real GUI child process.
    int        ipc_linux_pid;
    char       ipc_status[96];
    char       ipc_class[64];
} Window;

typedef struct {
    int x, y, w, h;
} WindowRect;

typedef int (*WindowEnumProc)(int index, const Window* win, void* userdata);

typedef struct MyWindowLifetimeAudit {
    DWORD loader_close_ok;
    DWORD loader_close_fail;
    DWORD loader_missing_owner;
    DWORD loader_recovered_owner;
    DWORD loader_owner_mismatch;
} MyWindowLifetimeAudit;

void wm_get_lifetime_audit_stats(MyWindowLifetimeAudit* out);

typedef enum {
    DRAG_NONE,
    DRAG_MOVE,
    DRAG_RESIZE_L,
    DRAG_RESIZE_R,
    DRAG_RESIZE_T,
    DRAG_RESIZE_B,
    DRAG_RESIZE_TL,
    DRAG_RESIZE_TR,
    DRAG_RESIZE_BL,
    DRAG_RESIZE_RB,
} DragMode;


typedef enum {
    DESKTOP_LAYOUT_FREE = 0,
    DESKTOP_LAYOUT_GRID = 1,
} DesktopLayoutMode;

typedef struct {
    char name[256];
    int  x, y, w, h;
    int  is_dir;
    char path[512];
} DesktopIcon;

typedef struct WindowManager {
    Window   wins[MAX_WINDOWS];
    int      count;
    int      free_stack[MAX_WINDOWS];
    int      free_top;
    int      focused;
    DragMode drag_mode;
    int      drag_idx;
    int      drag_ox, drag_oy;
    int      drag_orig_x, drag_orig_y;
    int      drag_orig_w, drag_orig_h;
    HWND     nc_down_hwnd;   // v78: pending non-client mouse down target
    int      nc_down_ht;     // v78: HT* from WM_NCHITTEST
    int      shift_held;
    int      ctrl_held;
    int      alt_held;       // v83: Alt+Space -> SC_KEYMENU
    int      alt_menu_armed; // v101: Alt release activates app menubar if not used as combo
    int      screen_w, screen_h; // letzter bekannter Framebuffer, für Hit-Tests
    int      menu_open;
    int      menu_x, menu_y;
    int      menu_from_start;
    int      menu_kind;          // v83: 0=start/desktop, 1=system menu
    int      menu_target_idx;    // v83: target window for system menu
    HWND     menu_target_hwnd;
    int      menu_selected;      // v85: keyboard/hover selected item index
    unsigned menu_loop_serial;   // v85: increments on open/close for diagnostics
    unsigned menu_key_count;     // v85: keyboard navigation counter

    // v101: real HMENU-backed app menubar/popup state.  Start/system menus
    // still use the legacy MENU_KIND_START/SYSTEM tables above; MENU_KIND_APP
    // drives menu bars attached by SetMenu(hwnd, hMenu).
    HWND     app_menu_hwnd;
    HMENU    app_menu_bar;
    int      app_menu_owner_idx;
    int      app_menu_top_index;
    int      app_menu_bar_hot;
    int      app_menu_level_count;
    HMENU    app_menu_popup[APP_MENU_MAX_LEVELS];
    int      app_menu_sel[APP_MENU_MAX_LEVELS];
    int      app_menu_x[APP_MENU_MAX_LEVELS];
    int      app_menu_y[APP_MENU_MAX_LEVELS];
    int      app_menu_w[APP_MENU_MAX_LEVELS];
    int      app_menu_h[APP_MENU_MAX_LEVELS];
    BgStyle  bg;
    Image    wallpaper;
    int      wallpaper_enabled;
    char     wallpaper_path[512];
    char     wallpaper_status[128];
    char     desktop_path[512];
    char     desktop_layout_path[512];
    DesktopIcon desktop_icons[MAX_DESKTOP_ICONS];
    int      desktop_icon_count;
    DesktopLayoutMode desktop_layout_mode;
    int      desktop_selected;
    int      desktop_drag_idx;
    int      desktop_dragging;
    int      desktop_drag_ox, desktop_drag_oy;
    int      desktop_last_click_idx;
    long long desktop_last_click_ms;
    // callback wenn neues fenster per rechtsklick erstellt wird
    void (*on_new_window)(int x, int y, void* ctx);
    void*        new_window_ctx;
    HWNDManager* mgr;   // zeiger auf den hwnd manager
    IPCBus*      bus;   // zeiger auf den ipc bus
    DWORD        state_version;

    // v77: the shell itself is now represented by real HWNDs/classes.
    // Desktop -> #32769, Taskbar -> Shell_TrayWnd, Start button -> BUTTON child.
    HWND         hwnd_desktop;
    HWND         hwnd_taskbar;
    HWND         hwnd_start_button;
    Capability   shell_cap;
    DWORD        shell_msg_count;
    DWORD        shell_cmd_count;
    int          shell_state_w;
    int          shell_state_h;
} WindowManager;

void wm_init(WindowManager* wm, HWNDManager* mgr, IPCBus* bus);
int  wm_add(WindowManager* wm, int x, int y, int w, int h, const char* title, Capability cap);
int  wm_add_calc(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_editor(WindowManager* wm, int x, int y, const char* path, const char* title, Capability cap);
int  wm_add_spy(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_access(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_pump(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_deadlock(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_section(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_sharedbus_pair(WindowManager* wm, int x, int y);
int  wm_add_objectlab(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_waitlab(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_clipmenulab(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_paintlab(WindowManager* wm, int x, int y, const char* title, Capability cap);
int  wm_add_draglab(WindowManager* wm, int x, int y, const char* title, Capability cap);
void wm_draw(WindowManager* wm, Framebuffer* fb);
int wm_mouse_move(WindowManager* wm, int x, int y);
// v77: Win32-ish raw-input routing.  These are not behavior handlers; they
// route to DesktopWndProc/TaskbarWndProc/App WndProcs or the v78 non-client path.
int  wm_route_raw_mouse_button_down(WindowManager* wm, int x, int y, int btn);
int  wm_route_raw_mouse_button_up(WindowManager* wm, int x, int y, int btn);
void wm_mouse_up(WindowManager* wm); // legacy drag-release helper, kept for internals/tests
void wm_key(WindowManager* wm, int keycode, int down);
void wm_blink(WindowManager* wm);
void wm_poll(WindowManager* wm);   // prozess-output pollen
void wm_desktop_reload(WindowManager* wm);
int  wm_desktop_create_text_file(WindowManager* wm);
void wm_desktop_toggle_layout(WindowManager* wm);
const char* wm_desktop_layout_mode_name(WindowManager* wm);

// Liefert nur dann ein App-HWND, wenn der Punkt wirklich im Client-Bereich liegt.
// Chrome/Taskbar/Menu/Drag werden dadurch nicht an Apps weitergereicht.
int  wm_client_endpoint_at_focus(WindowManager* wm, int x, int y, HWND* hwnd, Capability** cap);
int  wm_activate_app_menu(WindowManager* wm, int keycode);

// v92.2: Hover/WindowFromPoint-Variante für WM_MOUSEWHEEL.  Anders als
// wm_client_endpoint_at_focus() nimmt diese Funktion nicht das aktuell
// fokussierte Top-Level-Fenster als Ausgangspunkt, sondern sucht das HWND
// unter den Screen-Koordinaten.  Damit scrollt LISTBOX/COMBOBOX/SCROLLBAR
// unter dem Mauszeiger, auch wenn ein anderes App-Fenster den Fokus hat.
int  wm_client_endpoint_at_point(WindowManager* wm, int x, int y, HWND* hwnd, Capability** cap);

// Kleine User/WinAPI-artige Fensterabfragen.
int  wm_get_window_rect(WindowManager* wm, int index, WindowRect* out);
int  wm_set_window_pos(WindowManager* wm, int index, int x, int y, int w, int h);
int  wm_set_window_pos_ex(WindowManager* wm, int index, HWND hwndInsertAfter, int x, int y, int w, int h, UINT flags);
int  wm_show_window(WindowManager* wm, int index, int nCmdShow);
int  wm_get_window_title(WindowManager* wm, int index, char* out, int out_len);
int  wm_find_window(WindowManager* wm, const char* title);
void wm_enum_windows(WindowManager* wm, WindowEnumProc proc, void* userdata);
int  wm_find_hwnd(WindowManager* wm, HWND hwnd);
int  wm_close_hwnd(WindowManager* wm, HWND hwnd);
HWND wm_get_window_hwnd(WindowManager* wm, int index);
HWND wm_get_foreground_hwnd(WindowManager* wm);
int  wm_set_foreground_hwnd(WindowManager* wm, HWND hwnd);
int  wm_get_window_state(WindowManager* wm, HWND hwnd, MyWindowState* out);
int  wm_set_window_title_by_hwnd(WindowManager* wm, HWND hwnd, const char* title);
LRESULT wm_def_window_proc(WindowManager* wm, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp); // v78: frame/non-client DefWindowProc bridge
int  wm_menu_handle_key(WindowManager* wm, int keycode); // v85: active menu-loop keyboard handling
int  wm_on_destroyed_hwnd(WindowManager* wm, HWND hwnd); // v43: USER32 DestroyWindow -> desktop frame sync
int  wm_set_wallpaper(WindowManager* wm, const char* path);
void wm_clear_wallpaper(WindowManager* wm);
const char* wm_get_wallpaper_status(WindowManager* wm);

int wm_add_controllab(WindowManager* wm, int x, int y, const char* title, Capability cap);
int wm_add_servicelab(WindowManager* wm, int x, int y, const char* title, Capability cap);
int wm_add_dialoglab(WindowManager* wm, int x, int y, const char* title, Capability cap);
int wm_add_mdilab(WindowManager* wm, int x, int y, const char* title, Capability cap);
int wm_add_ipc_proxy(WindowManager* wm, int x, int y, int w, int h, const char* title, const char* class_name, Capability cap, int linux_pid, const char* status, HWND owner_hwnd, DWORD style, DWORD ex_style);

// v77 shell HWND accessors.
HWND wm_get_desktop_hwnd(WindowManager* wm);
HWND wm_get_taskbar_hwnd(WindowManager* wm);
HWND wm_get_start_button_hwnd(WindowManager* wm);
