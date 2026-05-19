#include "app_controllab.h"
#include "window.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"

/* AUDIT(v119-lab): ControlLab still contains local ControlLite state beside
   real registered control class names. This is useful while controls mature,
   but it is also where strict USER32 will expose old shortcuts: direct
   hwnd_post(), locally synthesized WM_COMMAND, and private focusChild state can
   diverge from real child HWND focus/order. Expect breakage when BUTTON/EDIT/
   LISTBOX become fully routed through winuser.c/mycontrols.c only. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define CTRL_LOG_LINES 11
#define CTRL_LOG_CHARS 220
#define CTRL_MAX_ITEMS 8

#define MY_BN_CLICKED     0
#define MY_EN_CHANGE      0x0300
#define MY_LBN_SELCHANGE  1

#define MY_LB_ADDSTRING     (WM_USER + 0x101)
#define MY_LB_RESETCONTENT  (WM_USER + 0x102)
#define MY_LB_GETCURSEL     (WM_USER + 0x103)
#define MY_LB_SETCURSEL     (WM_USER + 0x104)
#define MY_LB_GETTEXT       (WM_USER + 0x105)

#define MY_WM_SETTEXT       0x000C
#define MY_WM_GETTEXT       0x000D

typedef enum ControlKind {
    CTRLK_BUTTON = 1,
    CTRLK_EDIT   = 2,
    CTRLK_LISTBOX= 3
} ControlKind;

typedef struct ControlLite {
    HWND hwnd;
    HWND parent;
    UINT id;
    ControlKind kind;
    int x, y, w, h;
    int visible;
    int pressed;
    int focus;
    char text[256];
    char items[CTRL_MAX_ITEMS][48];
    int itemCount;
    int sel;
} ControlLite;

typedef struct ControlLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    Capability cap;
    pthread_mutex_t lock;
    ControlLite button;
    ControlLite edit;
    ControlLite list;
    HWND focusChild;
    int itemSeq;
    int commandCount;
    MyAppResizeState resize;
    char status[384];
    char log[CTRL_LOG_LINES][CTRL_LOG_CHARS];
    int logCount;
} ControlLabApp;

static ControlLabApp g_ctl;

static WORD my_loword(WPARAM v) { return (WORD)(v & 0xffffu); }
static WORD my_hiword(WPARAM v) { return (WORD)((v >> 16) & 0xffffu); }
static WPARAM my_makewparam(WORD lo, WORD hi) { return ((WPARAM)hi << 16) | lo; }

static void ctl_log_locked(const char* s)
{
    if (!s) return;
    if (g_ctl.logCount < CTRL_LOG_LINES) snprintf(g_ctl.log[g_ctl.logCount++], sizeof(g_ctl.log[0]), "%.219s", s);
    else {
        for (int i = 1; i < CTRL_LOG_LINES; i++) snprintf(g_ctl.log[i-1], sizeof(g_ctl.log[0]), "%.219s", g_ctl.log[i]);
        snprintf(g_ctl.log[CTRL_LOG_LINES-1], sizeof(g_ctl.log[0]), "%.219s", s);
    }
}

static void ensure_runtime(void) { MyWinBindRuntime(g_ctl.mgr, &g_ctl.cap); }

static char key_to_char(int key, int shift)
{
    static const char normal[128] = {
        [KEY_A]='a',[KEY_B]='b',[KEY_C]='c',[KEY_D]='d',[KEY_E]='e',[KEY_F]='f',[KEY_G]='g',[KEY_H]='h',[KEY_I]='i',[KEY_J]='j',
        [KEY_K]='k',[KEY_L]='l',[KEY_M]='m',[KEY_N]='n',[KEY_O]='o',[KEY_P]='p',[KEY_Q]='q',[KEY_R]='r',[KEY_S]='s',[KEY_T]='t',
        [KEY_U]='u',[KEY_V]='v',[KEY_W]='w',[KEY_X]='x',[KEY_Y]='y',[KEY_Z]='z',
        [KEY_1]='1',[KEY_2]='2',[KEY_3]='3',[KEY_4]='4',[KEY_5]='5',[KEY_6]='6',[KEY_7]='7',[KEY_8]='8',[KEY_9]='9',[KEY_0]='0',
        [KEY_SPACE]=' ',[KEY_DOT]='.',[KEY_COMMA]=',',[KEY_MINUS]='-',[KEY_EQUAL]='=',[KEY_SEMICOLON]=';',[KEY_SLASH]='/',
        [KEY_APOSTROPHE]='\'',[KEY_BACKSLASH]='\\',[KEY_GRAVE]='`',[KEY_LEFTBRACE]='[',[KEY_RIGHTBRACE]=']',
    };
    static const char shifted[128] = {
        [KEY_A]='A',[KEY_B]='B',[KEY_C]='C',[KEY_D]='D',[KEY_E]='E',[KEY_F]='F',[KEY_G]='G',[KEY_H]='H',[KEY_I]='I',[KEY_J]='J',
        [KEY_K]='K',[KEY_L]='L',[KEY_M]='M',[KEY_N]='N',[KEY_O]='O',[KEY_P]='P',[KEY_Q]='Q',[KEY_R]='R',[KEY_S]='S',[KEY_T]='T',
        [KEY_U]='U',[KEY_V]='V',[KEY_W]='W',[KEY_X]='X',[KEY_Y]='Y',[KEY_Z]='Z',
        [KEY_1]='!',[KEY_2]='"',[KEY_3]='#',[KEY_4]='$',[KEY_5]='%',[KEY_6]='&',[KEY_7]='/',[KEY_8]='(',[KEY_9]=')',[KEY_0]='=',
        [KEY_SPACE]=' ',[KEY_DOT]=':',[KEY_COMMA]=';',[KEY_MINUS]='_',[KEY_EQUAL]='+',[KEY_SEMICOLON]=':',[KEY_SLASH]='?',
        [KEY_GRAVE]='~',[KEY_LEFTBRACE]='{',[KEY_RIGHTBRACE]='}',
    };
    if (key < 0 || key >= 128) return 0;
    return shift ? shifted[key] : normal[key];
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int __attribute__((unused)) to_client(int sx, int sy, int* cx, int* cy)
{
    MyWindowState st;
    memset(&st, 0, sizeof(st));
    if (!hwnd_copy_window_state(g_ctl.mgr, g_ctl.hWnd, &st)) return 0;
    *cx = sx - (int)st.rcWindow.left;
    *cy = sy - (int)st.rcWindow.top - TITLEBAR_H;
    return 1;
}

static ControlLite* control_from_hwnd(HWND hwnd)
{
    if (g_ctl.button.visible && g_ctl.button.hwnd == hwnd) return &g_ctl.button;
    if (g_ctl.edit.visible && g_ctl.edit.hwnd == hwnd) return &g_ctl.edit;
    if (g_ctl.list.visible && g_ctl.list.hwnd == hwnd) return &g_ctl.list;
    return NULL;
}

static ControlLite* hit_child_locked(int cx, int cy)
{
    if (g_ctl.button.visible && inside(cx, cy, g_ctl.button.x, g_ctl.button.y, g_ctl.button.w, g_ctl.button.h)) return &g_ctl.button;
    if (g_ctl.edit.visible && inside(cx, cy, g_ctl.edit.x, g_ctl.edit.y, g_ctl.edit.w, g_ctl.edit.h)) return &g_ctl.edit;
    if (g_ctl.list.visible && inside(cx, cy, g_ctl.list.x, g_ctl.list.y, g_ctl.list.w, g_ctl.list.h)) return &g_ctl.list;
    return NULL;
}

static void post_parent_command(ControlLite* c, WORD notify)
{
    if (!c || !g_ctl.mgr) return;
    hwnd_post(g_ctl.mgr, &g_ctl.cap, c->parent, WM_COMMAND, my_makewparam((WORD)c->id, notify), (LPARAM)c->hwnd);
}

static LRESULT CALLBACK child_control_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) return DefWindowProcA(hwnd, msg, wp, lp);

    /* v37.1: hwnd_create() sends WM_CREATE synchronously before create_child_locked()
       can store the new HWND in the ControlLite slot. Returning here prevents a
       self-deadlock when the parent creates BUTTON/EDIT/LISTBOX while holding
       g_ctl.lock. */
    if (msg == WM_CREATE) return 0;

    pthread_mutex_lock(&g_ctl.lock);
    ControlLite* c = control_from_hwnd(hwnd);
    if (!c) { pthread_mutex_unlock(&g_ctl.lock); return 0; }

    switch (msg) {
    case WM_LBUTTONDOWN:
        g_ctl.focusChild = hwnd;
        g_ctl.button.focus = g_ctl.edit.focus = g_ctl.list.focus = 0;
        c->focus = 1;
        if (c->kind == CTRLK_BUTTON) {
            c->pressed = 1;
            snprintf(g_ctl.status, sizeof(g_ctl.status), "BUTTON hwnd=0x%x pressed", hwnd);
            ctl_log_locked(g_ctl.status);
        } else if (c->kind == CTRLK_LISTBOX) {
            int localY = GET_Y_LPARAM(lp);
            int idx = (localY - 6) / 18;
            if (idx >= 0 && idx < c->itemCount) {
                c->sel = idx;
                snprintf(g_ctl.status, sizeof(g_ctl.status), "LISTBOX sel=%d '%s' -> WM_COMMAND/LBN_SELCHANGE", idx, c->items[idx]);
                ctl_log_locked(g_ctl.status);
                pthread_mutex_unlock(&g_ctl.lock);
                post_parent_command(c, MY_LBN_SELCHANGE);
                return 0;
            }
        } else if (c->kind == CTRLK_EDIT) {
            snprintf(g_ctl.status, sizeof(g_ctl.status), "EDIT focus hwnd=0x%x", hwnd);
            ctl_log_locked(g_ctl.status);
        }
        break;
    case WM_LBUTTONUP:
        if (c->kind == CTRLK_BUTTON && c->pressed) {
            c->pressed = 0;
            snprintf(g_ctl.status, sizeof(g_ctl.status), "BUTTON click -> WM_COMMAND/BN_CLICKED id=%u", c->id);
            ctl_log_locked(g_ctl.status);
            pthread_mutex_unlock(&g_ctl.lock);
            post_parent_command(c, MY_BN_CLICKED);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (c->kind == CTRLK_EDIT) {
            int key = (int)wp;
            int shift = ((int)lp & MYOS_KEYSTATE_SHIFT) ? 1 : 0;
            int ctrl = ((int)lp & MYOS_KEYSTATE_CTRL) ? 1 : 0;
            if (ctrl && key == KEY_C) {
                pthread_mutex_unlock(&g_ctl.lock);
                ensure_runtime();
                if (OpenClipboard(g_ctl.hWnd)) {
                    EmptyClipboard();
                    HGLOBAL h = GlobalAlloc(0, (DWORD)strlen(c->text) + 1);
                    char* p = (char*)GlobalLock(h);
                    if (p) { strcpy(p, c->text); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
                    CloseClipboard();
                }
                pthread_mutex_lock(&g_ctl.lock);
                snprintf(g_ctl.status, sizeof(g_ctl.status), "EDIT Ctrl+C -> Clipboard CF_TEXT '%s'", c->text);
                ctl_log_locked(g_ctl.status);
                break;
            }
            if (ctrl && key == KEY_V) {
                pthread_mutex_unlock(&g_ctl.lock);
                ensure_runtime();
                char tmp[256] = {0};
                if (OpenClipboard(g_ctl.hWnd) && IsClipboardFormatAvailable(CF_TEXT)) {
                    HGLOBAL h = GetClipboardData(CF_TEXT);
                    char* p = (char*)GlobalLock(h);
                    if (p) { snprintf(tmp, sizeof(tmp), "%.255s", p); GlobalUnlock(h); }
                    CloseClipboard();
                }
                pthread_mutex_lock(&g_ctl.lock);
                if (tmp[0]) snprintf(c->text, sizeof(c->text), "%.255s", tmp);
                snprintf(g_ctl.status, sizeof(g_ctl.status), "EDIT Ctrl+V <- Clipboard '%s'", c->text);
                ctl_log_locked(g_ctl.status);
                pthread_mutex_unlock(&g_ctl.lock);
                post_parent_command(c, MY_EN_CHANGE);
                return 0;
            }
            if (key == KEY_BACKSPACE) {
                size_t n = strlen(c->text);
                if (n) c->text[n-1] = 0;
                snprintf(g_ctl.status, sizeof(g_ctl.status), "EDIT Backspace -> '%s'", c->text);
                ctl_log_locked(g_ctl.status);
                pthread_mutex_unlock(&g_ctl.lock);
                post_parent_command(c, MY_EN_CHANGE);
                return 0;
            }
            char ch = key_to_char(key, shift);
            if (ch) {
                size_t n = strlen(c->text);
                if (n + 1 < sizeof(c->text)) { c->text[n] = ch; c->text[n+1] = 0; }
                snprintf(g_ctl.status, sizeof(g_ctl.status), "EDIT char '%c' -> '%s'", ch, c->text);
                ctl_log_locked(g_ctl.status);
                pthread_mutex_unlock(&g_ctl.lock);
                post_parent_command(c, MY_EN_CHANGE);
                return 0;
            }
        }
        break;
    case MY_WM_SETTEXT:
        snprintf(c->text, sizeof(c->text), "%.255s", (const char*)lp);
        break;
    case MY_WM_GETTEXT:
        if ((char*)lp && wp > 0) snprintf((char*)lp, (size_t)wp, "%.255s", c->text);
        break;
    case MY_LB_ADDSTRING:
        if (c->kind == CTRLK_LISTBOX && c->itemCount < CTRL_MAX_ITEMS) {
            snprintf(c->items[c->itemCount++], sizeof(c->items[0]), "%.47s", (const char*)lp);
            if (c->sel < 0) c->sel = 0;
        }
        break;
    case MY_LB_RESETCONTENT:
        c->itemCount = 0; c->sel = -1;
        break;
    case MY_LB_SETCURSEL:
        if (c->kind == CTRLK_LISTBOX && (int)wp >= 0 && (int)wp < c->itemCount) c->sel = (int)wp;
        break;
    default: break;
    }
    pthread_mutex_unlock(&g_ctl.lock);
    return 0;
}

static void child_control_proc_hwnd(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, void* userdata)
{
    (void)userdata;
    child_control_proc(hwnd, msg, wp, lp);
}

static void create_child_locked(ControlLite* c, ControlKind kind, UINT id, int x, int y, int w, int h, const char* text)
{
    if (c->visible) return;
    memset(c, 0, sizeof(*c));
    c->kind = kind; c->id = id; c->x = x; c->y = y; c->w = w; c->h = h; c->visible = 1; c->parent = g_ctl.hWnd; c->sel = -1;
    snprintf(c->text, sizeof(c->text), "%s", text ? text : "");
    c->hwnd = hwnd_create(g_ctl.mgr, child_control_proc_hwnd, NULL, g_ctl.cap);
    const char* cls = (kind == CTRLK_BUTTON) ? "BUTTON" : (kind == CTRLK_EDIT) ? "EDIT" : "LISTBOX";
    snprintf(g_ctl.status, sizeof(g_ctl.status), "CreateWindowExA(\"%s\",...) -> child HWND=0x%x id=%u", cls, c->hwnd, id);
    ctl_log_locked(g_ctl.status);
}

static void do_copy_edit_locked(void)
{
    char text[256]; snprintf(text, sizeof(text), "%.255s", g_ctl.edit.text);
    pthread_mutex_unlock(&g_ctl.lock);
    ensure_runtime();
    if (OpenClipboard(g_ctl.hWnd)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(0, (DWORD)strlen(text) + 1);
        char* p = (char*)GlobalLock(h);
        if (p) { strcpy(p, text); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
        CloseClipboard();
    }
    pthread_mutex_lock(&g_ctl.lock);
    snprintf(g_ctl.status, sizeof(g_ctl.status), "Copy button: EDIT -> Clipboard '%s'", text);
    ctl_log_locked(g_ctl.status);
}

static void do_paste_edit_locked(void)
{
    pthread_mutex_unlock(&g_ctl.lock);
    ensure_runtime();
    char tmp[256] = {0};
    if (OpenClipboard(g_ctl.hWnd) && IsClipboardFormatAvailable(CF_TEXT)) {
        HGLOBAL h = GetClipboardData(CF_TEXT);
        char* p = (char*)GlobalLock(h);
        if (p) { snprintf(tmp, sizeof(tmp), "%.255s", p); GlobalUnlock(h); }
        CloseClipboard();
    }
    pthread_mutex_lock(&g_ctl.lock);
    if (tmp[0]) snprintf(g_ctl.edit.text, sizeof(g_ctl.edit.text), "%.255s", tmp);
    snprintf(g_ctl.status, sizeof(g_ctl.status), "Paste button: Clipboard -> EDIT '%s'", g_ctl.edit.text);
    ctl_log_locked(g_ctl.status);
}

static LRESULT CALLBACK controllab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_ctl.lock);
        g_ctl.hWnd = hwnd;
        MyAppResizeInit(&g_ctl.resize, CONTROLLAB_W, CONTROLLAB_H, TITLEBAR_H);
        snprintf(g_ctl.status, sizeof(g_ctl.status), "ControlLab v37 ready: child HWND BUTTON/EDIT/LISTBOX + WM_COMMAND");
        ctl_log_locked(g_ctl.status);
        pthread_mutex_unlock(&g_ctl.lock);
        break;
    case WM_LBUTTONDOWN: {
        int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        pthread_mutex_lock(&g_ctl.lock);
        if (cy >= 8 && cy < 28) {
            int cmd = 0;
            if (cx >= 8 && cx < 120) cmd = CTRL_CMD_CREATE_BUTTON;
            else if (cx >= 126 && cx < 238) cmd = CTRL_CMD_CREATE_EDIT;
            else if (cx >= 244 && cx < 356) cmd = CTRL_CMD_CREATE_LISTBOX;
            else if (cx >= 362 && cx < 474) cmd = CTRL_CMD_ADD_ITEM;
            else if (cx >= 480 && cx < 592) cmd = CTRL_CMD_CLEAR_LIST;
            if (cmd) { pthread_mutex_unlock(&g_ctl.lock); hwnd_post(g_ctl.mgr, &g_ctl.cap, hwnd, WM_COMMAND, (WPARAM)cmd, 0); break; }
        }
        if (cy >= 34 && cy < 54) {
            int cmd = 0;
            if (cx >= 8 && cx < 120) cmd = CTRL_CMD_READ_EDIT;
            else if (cx >= 126 && cx < 238) cmd = CTRL_CMD_SET_EDIT;
            else if (cx >= 244 && cx < 356) cmd = CTRL_CMD_COPY;
            else if (cx >= 362 && cx < 474) cmd = CTRL_CMD_PASTE;
            if (cmd) { pthread_mutex_unlock(&g_ctl.lock); hwnd_post(g_ctl.mgr, &g_ctl.cap, hwnd, WM_COMMAND, (WPARAM)cmd, 0); break; }
        }
        ControlLite* c = hit_child_locked(cx, cy);
        if (c) {
            HWND child = c->hwnd;
            pthread_mutex_unlock(&g_ctl.lock);
            hwnd_post(g_ctl.mgr, &g_ctl.cap, child, WM_LBUTTONDOWN, wp, MAKELPARAM((WORD)(cx - c->x), (WORD)(cy - c->y)));
            break;
        }
        g_ctl.button.focus = g_ctl.edit.focus = g_ctl.list.focus = 0; g_ctl.focusChild = 0;
        pthread_mutex_unlock(&g_ctl.lock);
        break;
    }
    case WM_LBUTTONUP: {
        int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        pthread_mutex_lock(&g_ctl.lock);
        ControlLite* c = hit_child_locked(cx, cy);
        if (!c && g_ctl.button.pressed) c = &g_ctl.button;
        HWND child = c ? c->hwnd : 0;
        pthread_mutex_unlock(&g_ctl.lock);
        if (child) { ControlLite* cc = control_from_hwnd(child); int lx = cc ? cx - cc->x : cx; int ly = cc ? cy - cc->y : cy; hwnd_post(g_ctl.mgr, &g_ctl.cap, child, WM_LBUTTONUP, wp, MAKELPARAM((WORD)lx, (WORD)ly)); }
        break;
    }
    case WM_KEYDOWN: {
        pthread_mutex_lock(&g_ctl.lock);
        HWND child = g_ctl.focusChild;
        pthread_mutex_unlock(&g_ctl.lock);
        if (child) hwnd_post(g_ctl.mgr, &g_ctl.cap, child, WM_KEYDOWN, wp, lp);
        break;
    }
    case WM_COMMAND: {
        UINT id = (UINT)my_loword(wp);
        UINT code = (UINT)my_hiword(wp);
        pthread_mutex_lock(&g_ctl.lock);
        g_ctl.commandCount++;
        if (id == CTRL_CMD_CREATE_BUTTON) create_child_locked(&g_ctl.button, CTRLK_BUTTON, 101, 38, 96, 130, 34, "Child Button");
        else if (id == CTRL_CMD_CREATE_EDIT) create_child_locked(&g_ctl.edit, CTRLK_EDIT, 102, 38, 148, 250, 28, "edit me");
        else if (id == CTRL_CMD_CREATE_LISTBOX) create_child_locked(&g_ctl.list, CTRLK_LISTBOX, 103, 330, 92, 250, 144, "");
        else if (id == CTRL_CMD_ADD_ITEM) {
            if (!g_ctl.list.visible) create_child_locked(&g_ctl.list, CTRLK_LISTBOX, 103, 330, 92, 250, 144, "");
            if (g_ctl.list.itemCount < CTRL_MAX_ITEMS) snprintf(g_ctl.list.items[g_ctl.list.itemCount++], sizeof(g_ctl.list.items[0]), "Item %d", ++g_ctl.itemSeq);
            if (g_ctl.list.sel < 0) g_ctl.list.sel = 0;
            snprintf(g_ctl.status, sizeof(g_ctl.status), "LB_ADDSTRING -> items=%d", g_ctl.list.itemCount); ctl_log_locked(g_ctl.status);
        } else if (id == CTRL_CMD_CLEAR_LIST) {
            g_ctl.list.itemCount = 0; g_ctl.list.sel = -1; snprintf(g_ctl.status, sizeof(g_ctl.status), "LB_RESETCONTENT"); ctl_log_locked(g_ctl.status);
        } else if (id == CTRL_CMD_READ_EDIT) {
            snprintf(g_ctl.status, sizeof(g_ctl.status), "GetWindowText(EDIT=0x%x) -> '%s'", g_ctl.edit.hwnd, g_ctl.edit.visible ? g_ctl.edit.text : "<missing>"); ctl_log_locked(g_ctl.status);
        } else if (id == CTRL_CMD_SET_EDIT) {
            if (!g_ctl.edit.visible) create_child_locked(&g_ctl.edit, CTRLK_EDIT, 102, 38, 148, 250, 28, "");
            snprintf(g_ctl.edit.text, sizeof(g_ctl.edit.text), "Hello Controls v37 #%d", ++g_ctl.itemSeq);
            snprintf(g_ctl.status, sizeof(g_ctl.status), "SetWindowText(EDIT) -> '%s'", g_ctl.edit.text); ctl_log_locked(g_ctl.status);
        } else if (id == CTRL_CMD_COPY) do_copy_edit_locked();
        else if (id == CTRL_CMD_PASTE) do_paste_edit_locked();
        else if (id == 101 && code == MY_BN_CLICKED) {
            snprintf(g_ctl.status, sizeof(g_ctl.status), "Parent got WM_COMMAND: id=101 code=BN_CLICKED lParam=0x%lx", (unsigned long)lp); ctl_log_locked(g_ctl.status);
        } else if (id == 102 && code == MY_EN_CHANGE) {
            snprintf(g_ctl.status, sizeof(g_ctl.status), "Parent got WM_COMMAND: id=102 code=EN_CHANGE text='%s'", g_ctl.edit.text); ctl_log_locked(g_ctl.status);
        } else if (id == 103 && code == MY_LBN_SELCHANGE) {
            const char* item = (g_ctl.list.sel >= 0 && g_ctl.list.sel < g_ctl.list.itemCount) ? g_ctl.list.items[g_ctl.list.sel] : "";
            snprintf(g_ctl.status, sizeof(g_ctl.status), "Parent got WM_COMMAND: id=103 code=LBN_SELCHANGE sel=%d '%s'", g_ctl.list.sel, item); ctl_log_locked(g_ctl.status);
        }
        pthread_mutex_unlock(&g_ctl.lock);
        break;
    }
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_ctl.resize, lp, CONTROLLAB_MIN_W, CONTROLLAB_MIN_H);
        return 0;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_ctl.resize, lp);
        return 0;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_ctl.resize, lp, TITLEBAR_H);
        return 0;
    case WM_MOVE:
        MyAppResizeOnMove(&g_ctl.resize, lp);
        return 0;
    case WM_SIZE:
        MyAppResizeOnSize(&g_ctl.resize, wp, lp);
        return 0;
    case WM_CLOSE:
        return DefWindowProcA(hwnd, msg, wp, lp);
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,50,70));
    fb_rect_outline(fb, x, y, w, 20, COLOR(120,140,175));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}


static void draw_control(Framebuffer* fb, int ox, int oy, const ControlLite* c)
{
    if (!c || !c->visible) return;
    int x = ox + c->x, y = oy + c->y;
    if (c->kind == CTRLK_BUTTON) {
        Color bg = c->pressed ? COLOR(75,95,145) : COLOR(50,62,95);
        fb_rect(fb, x, y, c->w, c->h, bg);
        fb_rect_outline(fb, x, y, c->w, c->h, c->focus ? COLOR(255,230,120) : COLOR(150,170,220));
        font_draw_str(fb, x + 16, y + 13, c->text, WHITE);
    } else if (c->kind == CTRLK_EDIT) {
        fb_rect(fb, x, y, c->w, c->h, COLOR(16,18,24));
        fb_rect_outline(fb, x, y, c->w, c->h, c->focus ? COLOR(255,230,120) : COLOR(120,130,155));
        draw_clip_text(fb, x+7, y+10, c->text, WHITE, x+4, y+4, c->w-8, c->h-8);
        if (c->focus) {
            int cx = x + 7 + (int)strlen(c->text) * 8;
            if (cx < x + c->w - 6) fb_rect(fb, cx, y+7, 1, c->h-14, COLOR(255,230,120));
        }
    } else if (c->kind == CTRLK_LISTBOX) {
        fb_rect(fb, x, y, c->w, c->h, COLOR(12,15,22));
        fb_rect_outline(fb, x, y, c->w, c->h, c->focus ? COLOR(255,230,120) : COLOR(100,115,145));
        for (int i = 0; i < c->itemCount; i++) {
            int iy = y + 6 + i * 18;
            if (i == c->sel) fb_rect(fb, x+4, iy-3, c->w-8, 16, COLOR(50,80,145));
            draw_clip_text(fb, x+10, iy, c->items[i], WHITE, x+4, y+4, c->w-8, c->h-8);
        }
    }
}

HWND controllab_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    static int s_lock_init = 0;
    if (!s_lock_init) { pthread_mutex_init(&g_ctl.lock, NULL); s_lock_init = 1; }
    pthread_mutex_lock(&g_ctl.lock);
    memset(&g_ctl, 0, sizeof(g_ctl));
    g_ctl.mgr = mgr; g_ctl.cap = cap;
    pthread_mutex_unlock(&g_ctl.lock);

    MyWinBindRuntime(mgr, &cap);
    {
        WNDCLASSEXA wc; memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = controllab_wndproc; wc.lpszClassName = "myOS.ControlLab";
        (void)RegisterClassExA(&wc);
    }
    // Register visible Win32-style control class names for the lab logs / future CreateWindowEx path.
    static int s_ctrl_classes = 0;
    if (!s_ctrl_classes) {
        WNDCLASSEXA wc; memset(&wc, 0, sizeof(wc)); wc.cbSize = sizeof(wc); wc.lpfnWndProc = child_control_proc;
        wc.lpszClassName = "BUTTON"; RegisterClassExA(&wc);
        wc.lpszClassName = "EDIT"; RegisterClassExA(&wc);
        wc.lpszClassName = "LISTBOX"; RegisterClassExA(&wc);
        s_ctrl_classes = 1;
    }
    HWND hWnd = CreateWindowExA(WS_EX_NONE, "myOS.ControlLab", "myOS ControlLab", WS_OVERLAPPEDWINDOW|WS_VISIBLE, x, y, CONTROLLAB_W, CONTROLLAB_H, 0, 0, 0, NULL);
    pthread_mutex_lock(&g_ctl.lock); g_ctl.hWnd = hWnd; pthread_mutex_unlock(&g_ctl.lock);
    return hWnd;
}

void controllab_destroy(void)
{
    pthread_mutex_lock(&g_ctl.lock);
    memset(&g_ctl, 0, sizeof(g_ctl));
    pthread_mutex_unlock(&g_ctl.lock);
}

void controllab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    (void)hwnd;
    int x = wx + 1;
    int y = wy + TITLEBAR_H;
    int w = ww - 2;
    int h = wh - TITLEBAR_H - 1;
    if (w <= 0 || h <= 0) return;
    fb_rect(fb, x, y, w, h, COLOR(8,9,18));
    button(fb, x+8,   y+8, 112, "Button");
    button(fb, x+126, y+8, 112, "Edit");
    button(fb, x+244, y+8, 112, "ListBox");
    button(fb, x+362, y+8, 112, "Add Item");
    button(fb, x+480, y+8, 112, "Clear List");
    button(fb, x+8,   y+34, 112, "Read Edit");
    button(fb, x+126, y+34, 112, "Set Edit");
    button(fb, x+244, y+34, 112, "Copy");
    button(fb, x+362, y+34, 112, "Paste");

    pthread_mutex_lock(&g_ctl.lock);
    ControlLabApp s = g_ctl;
    pthread_mutex_unlock(&g_ctl.lock);

    font_draw_str(fb, x+38, y+76, "Child HWND controls:", COLOR(190,220,255));
    draw_control(fb, x, y, &s.button);
    draw_control(fb, x, y, &s.edit);
    draw_control(fb, x, y, &s.list);

    char line[512];
    snprintf(line, sizeof(line), "parent=0x%x focusChild=0x%x commands=%d  BUTTON=0x%x EDIT=0x%x LISTBOX=0x%x",
             s.hWnd, s.focusChild, s.commandCount, s.button.hwnd, s.edit.hwnd, s.list.hwnd);
    draw_clip_text(fb, x+8, y+250, line, COLOR(120,255,170), x+8, y+244, w-16, 18);
    snprintf(line, sizeof(line), "edit='%s'  listItems=%d sel=%d", s.edit.visible ? s.edit.text : "<not created>", s.list.itemCount, s.list.sel);
    draw_clip_text(fb, x+8, y+270, line, COLOR(160,210,255), x+8, y+264, w-16, 18);
    snprintf(line, sizeof(line), "Status: %s", s.status);
    draw_clip_text(fb, x+8, y+290, line, COLOR(255,230,160), x+8, y+284, w-16, 18);

    int log_y = y + 316;
    fb_rect_outline(fb, x+8, log_y, w-16, h - 324, COLOR(70,80,115));
    font_draw_str(fb, x+14, log_y+8, "ControlLab log", WHITE);
    int yy = log_y + 28;
    for (int i = 0; i < s.logCount && yy < y + h - 12; i++, yy += 15)
        draw_clip_text(fb, x+14, yy, s.log[i], WHITE, x+12, log_y+24, w-24, h-348);
}
