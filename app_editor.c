#include "app_editor.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "app_msdn_resize.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/input-event-codes.h>

/* AUDIT(v119-app): Editor is mostly a custom client-area control, not a real
   EDIT child HWND. It will survive many USER32 refactors, but it will break
   once keyboard focus, caret ownership, WM_CHAR/WM_KEYDOWN translation or
   scroll semantics become strict MSDN behavior unless the text area becomes an
   actual EDIT/richedit-style control or an explicit owner-draw control. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif
#ifndef RESIZE_GRIP
#define RESIZE_GRIP 6
#endif

typedef struct { int x, y, w, h; } RectI;

typedef struct {
    HWND hwnd;
    char path[768];
    char name[256];
    char text[8192];
    int  len;
    int  cursor;
    int  scroll_line;
    int  dirty;
    int  load_ok;
    char status[128];

    int win_x, win_y, win_w, win_h;
    RectI toolbar;
    RectI save_btn;
    RectI text_area;
    MyAppResizeState resize;
} Editor;

#define MAX_EDITORS 16
static Editor g_editors[MAX_EDITORS];

static Editor* editor_by_hwnd(HWND hwnd)
{
    for (int i = 0; i < MAX_EDITORS; i++)
        if (g_editors[i].hwnd == hwnd) return &g_editors[i];
    return NULL;
}

static Editor* editor_alloc(void)
{
    for (int i = 0; i < MAX_EDITORS; i++) {
        if (!g_editors[i].hwnd) {
            memset(&g_editors[i], 0, sizeof(g_editors[i]));
            return &g_editors[i];
        }
    }
    return &g_editors[0]; // Notfall: Demo bleibt lauffaehig.
}

static const char* base_name(const char* p)
{
    if (!p) return "Unbenannt.txt";
    const char* s1 = strrchr(p, '/');
    return s1 ? s1 + 1 : p;
}

static void editor_load(Editor* e)
{
    if (!e) return;
    e->text[0] = 0;
    e->len = 0;
    e->cursor = 0;
    e->scroll_line = 0;
    e->dirty = 0;
    e->load_ok = 0;

    FILE* f = fopen(e->path, "rb");
    if (!f) {
        snprintf(e->status, sizeof(e->status), "Neue Datei: %.96s", e->name);
        return;
    }
    size_t n = fread(e->text, 1, sizeof(e->text) - 1, f);
    fclose(f);
    e->text[n] = 0;
    e->len = (int)n;
    e->cursor = e->len;
    e->load_ok = 1;
    snprintf(e->status, sizeof(e->status), "Geladen: %.80s (%d bytes)", e->name, e->len);
}

static void editor_save(Editor* e)
{
    if (!e) return;
    FILE* f = fopen(e->path, "wb");
    if (!f) {
        snprintf(e->status, sizeof(e->status), "Speichern fehlgeschlagen");
        return;
    }
    fwrite(e->text, 1, (size_t)e->len, f);
    fclose(f);
    e->dirty = 0;
    snprintf(e->status, sizeof(e->status), "Gespeichert: %.76s (%d bytes)", e->name, e->len);
}

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void editor_make_layout(Editor* e, int ww, int wh)
{
    int client_x = e->win_x + 1;
    int client_y = e->win_y + TITLEBAR_H;
    int cw = ww - 2;
    int ch = wh - TITLEBAR_H - 1;

    e->toolbar.x = client_x;
    e->toolbar.y = client_y;
    e->toolbar.w = cw;
    e->toolbar.h = 30;

    e->save_btn.x = client_x + 8;
    e->save_btn.y = client_y + 5;
    e->save_btn.w = 92;
    e->save_btn.h = 20;

    e->text_area.x = client_x + 8;
    e->text_area.y = client_y + e->toolbar.h + 8;
    e->text_area.w = cw - 16;
    e->text_area.h = ch - e->toolbar.h - 16 - RESIZE_GRIP;
    if (e->text_area.h < 40) e->text_area.h = 40;
}

static void draw_str_clip(Framebuffer* fb, int x, int y, const char* s, Color c,
                          int clip_x, int clip_y, int clip_w, int clip_h)
{
    if (!s || clip_w <= 0 || clip_h <= 0) return;
    extern const unsigned char font8x8[95][8];
    while (*s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch >= 32 && ch <= 126) {
            const unsigned char* glyph = font8x8[ch - 32];
            for (int row = 0; row < 8; row++) {
                int py = y + row;
                if (py < clip_y || py >= clip_y + clip_h) continue;
                for (int col = 0; col < 8; col++) {
                    int px = x + col;
                    if (px < clip_x || px >= clip_x + clip_w) continue;
                    if (glyph[row] & (1 << col)) fb_pixel(fb, px, py, c);
                }
            }
        }
        x += 8;
        if (x >= clip_x + clip_w) break;
    }
}

static int line_for_pos(const char* text, int pos)
{
    int line = 0;
    for (int i = 0; text[i] && i < pos; i++) if (text[i] == '\n') line++;
    return line;
}

static int line_start_pos(const char* text, int wanted_line)
{
    int line = 0;
    if (wanted_line <= 0) return 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            line++;
            if (line == wanted_line) return i + 1;
        }
    }
    return (int)strlen(text);
}

static void ensure_cursor_visible(Editor* e)
{
    int cur_line = line_for_pos(e->text, e->cursor);
    int visible = (e->text_area.h - 8) / 12;
    if (visible < 1) visible = 1;
    if (cur_line < e->scroll_line) e->scroll_line = cur_line;
    if (cur_line >= e->scroll_line + visible) e->scroll_line = cur_line - visible + 1;
    if (e->scroll_line < 0) e->scroll_line = 0;
}

static void insert_char(Editor* e, char ch)
{
    if (!e || e->len >= (int)sizeof(e->text) - 2) return;
    memmove(e->text + e->cursor + 1, e->text + e->cursor, (size_t)(e->len - e->cursor + 1));
    e->text[e->cursor] = ch;
    e->cursor++;
    e->len++;
    e->dirty = 1;
    ensure_cursor_visible(e);
}

static void backspace(Editor* e)
{
    if (!e || e->cursor <= 0) return;
    memmove(e->text + e->cursor - 1, e->text + e->cursor, (size_t)(e->len - e->cursor + 1));
    e->cursor--;
    e->len--;
    e->dirty = 1;
    ensure_cursor_visible(e);
}

static char key_to_char(int key, int shift)
{
    static const char normal[128] = {
        [KEY_A]='a',[KEY_B]='b',[KEY_C]='c',[KEY_D]='d',[KEY_E]='e',
        [KEY_F]='f',[KEY_G]='g',[KEY_H]='h',[KEY_I]='i',[KEY_J]='j',
        [KEY_K]='k',[KEY_L]='l',[KEY_M]='m',[KEY_N]='n',[KEY_O]='o',
        [KEY_P]='p',[KEY_Q]='q',[KEY_R]='r',[KEY_S]='s',[KEY_T]='t',
        [KEY_U]='u',[KEY_V]='v',[KEY_W]='w',[KEY_X]='x',[KEY_Y]='y',
        [KEY_Z]='z',
        [KEY_1]='1',[KEY_2]='2',[KEY_3]='3',[KEY_4]='4',[KEY_5]='5',
        [KEY_6]='6',[KEY_7]='7',[KEY_8]='8',[KEY_9]='9',[KEY_0]='0',
        [KEY_SPACE]=' ',[KEY_DOT]='.',[KEY_COMMA]=',',[KEY_MINUS]='-',
        [KEY_EQUAL]='=',[KEY_SEMICOLON]=';',[KEY_SLASH]='/',
        [KEY_APOSTROPHE]='\'',[KEY_BACKSLASH]='\\',[KEY_GRAVE]='`',
        [KEY_LEFTBRACE]='[',[KEY_RIGHTBRACE]=']',
    };
    static const char shifted[128] = {
        [KEY_A]='A',[KEY_B]='B',[KEY_C]='C',[KEY_D]='D',[KEY_E]='E',
        [KEY_F]='F',[KEY_G]='G',[KEY_H]='H',[KEY_I]='I',[KEY_J]='J',
        [KEY_K]='K',[KEY_L]='L',[KEY_M]='M',[KEY_N]='N',[KEY_O]='O',
        [KEY_P]='P',[KEY_Q]='Q',[KEY_R]='R',[KEY_S]='S',[KEY_T]='T',
        [KEY_U]='U',[KEY_V]='V',[KEY_W]='W',[KEY_X]='X',[KEY_Y]='Y',
        [KEY_Z]='Z',
        [KEY_1]='!',[KEY_2]='"',[KEY_3]='#',[KEY_4]='$',[KEY_5]='%',
        [KEY_6]='&',[KEY_7]='/',[KEY_8]='(',[KEY_9]=')',[KEY_0]='=',
        [KEY_SPACE]=' ',[KEY_DOT]=':',[KEY_COMMA]=';',[KEY_MINUS]='_',
        [KEY_EQUAL]='+',[KEY_SEMICOLON]=':',[KEY_SLASH]='?',
        [KEY_GRAVE]='~',[KEY_LEFTBRACE]='{',[KEY_RIGHTBRACE]='}',
    };
    if (key < 0 || key >= 128) return 0;
    return shift ? shifted[key] : normal[key];
}

void editor_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    Editor* e = editor_by_hwnd(hwnd);
    if (!e || !fb) return;
    e->win_x = wx; e->win_y = wy; e->win_w = ww; e->win_h = wh;
    editor_make_layout(e, ww, wh);

    int client_x = wx + 1;
    int client_y = wy + TITLEBAR_H;
    int cw = ww - 2;
    int ch = wh - TITLEBAR_H - 1;
    if (cw < 40 || ch < 40) return;

    fb_rect(fb, client_x, client_y, cw, ch, COLOR(18, 18, 28));

    fb_rect(fb, e->toolbar.x, e->toolbar.y, e->toolbar.w, e->toolbar.h, COLOR(32, 32, 48));
    fb_rect(fb, e->save_btn.x, e->save_btn.y, e->save_btn.w, e->save_btn.h,
            e->dirty ? COLOR(70, 70, 120) : COLOR(45, 65, 55));
    fb_rect_outline(fb, e->save_btn.x, e->save_btn.y, e->save_btn.w, e->save_btn.h, COLOR(110,110,150));
    font_draw_str(fb, e->save_btn.x + 8, e->save_btn.y + 6, "Speichern", WHITE);

    char title[360];
    snprintf(title, sizeof(title), "%s%s", e->dirty ? "* " : "", e->name);
    draw_str_clip(fb, e->save_btn.x + e->save_btn.w + 12, e->toolbar.y + 11,
                  title, COLOR(220,220,235), e->toolbar.x, e->toolbar.y, e->toolbar.w, e->toolbar.h);

    RectI a = e->text_area;
    fb_rect(fb, a.x, a.y, a.w, a.h, COLOR(8, 8, 13));
    fb_rect_outline(fb, a.x, a.y, a.w, a.h, COLOR(75,75,105));

    int visible_lines = (a.h - 8) / 12;
    if (visible_lines < 1) visible_lines = 1;
    int pos = line_start_pos(e->text, e->scroll_line);
    int line_no = e->scroll_line;
    int draw_y = a.y + 5;
    int cursor_drawn = 0;

    for (int ln = 0; ln < visible_lines && pos <= e->len; ln++, line_no++, draw_y += 12) {
        char line[512];
        int l = 0;
        int start = pos;
        while (e->text[pos] && e->text[pos] != '\n' && l < (int)sizeof(line) - 1) {
            line[l++] = e->text[pos++];
        }
        line[l] = 0;
        draw_str_clip(fb, a.x + 6, draw_y, line, COLOR(220,255,220), a.x + 2, a.y + 2, a.w - 4, a.h - 4);

        if (e->cursor >= start && e->cursor <= start + l) {
            int cx = a.x + 6 + (e->cursor - start) * 8;
            fb_rect(fb, cx, draw_y, 2, 10, COLOR(120,200,255));
            cursor_drawn = 1;
        }
        if (e->text[pos] == '\n') pos++;
        else if (!e->text[pos]) {
            if (e->cursor == e->len && !cursor_drawn) {
                int cx = a.x + 6 + l * 8;
                fb_rect(fb, cx, draw_y, 2, 10, COLOR(120,200,255));
            }
            break;
        }
    }

    fb_rect(fb, client_x + 8, client_y + ch - 16, cw - 16, 12, COLOR(18, 18, 28));
    draw_str_clip(fb, client_x + 10, client_y + ch - 14, e->status,
                  e->dirty ? COLOR(255,220,140) : COLOR(160,180,210),
                  client_x + 8, client_y + ch - 18, cw - 16, 16);
}

void editor_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, void* userdata)
{
    Editor* e = userdata ? (Editor*)userdata : editor_by_hwnd(hwnd);
    if (!e) return;

    switch (msg) {
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lparam);
        int my = GET_Y_LPARAM(lparam);
        if (mx >= e->save_btn.x && mx < e->save_btn.x + e->save_btn.w &&
            my >= e->save_btn.y && my < e->save_btn.y + e->save_btn.h) {
            editor_save(e);
            return;
        }
        if (mx >= e->text_area.x && mx < e->text_area.x + e->text_area.w &&
            my >= e->text_area.y && my < e->text_area.y + e->text_area.h) {
            int col = clamp_i((mx - e->text_area.x - 6) / 8, 0, 200);
            int row = clamp_i((my - e->text_area.y - 5) / 12, 0, 200);
            int line = e->scroll_line + row;
            int p = line_start_pos(e->text, line);
            int n = 0;
            while (e->text[p + n] && e->text[p + n] != '\n' && n < col) n++;
            e->cursor = p + n;
            ensure_cursor_visible(e);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        int delta = (SHORT)HIWORD(wparam);
        e->scroll_line += (delta < 0) ? 3 : -3;
        if (e->scroll_line < 0) e->scroll_line = 0;
        break;
    }
    case WM_KEYDOWN: {
        int key = (int)wparam;
        int shift = ((int)lparam) & 1;
        if (key == KEY_F10) { editor_save(e); return; }
        if (key == KEY_BACKSPACE) { backspace(e); return; }
        if (key == KEY_ENTER) { insert_char(e, '\n'); return; }
        if (key == KEY_LEFT) { if (e->cursor > 0) e->cursor--; ensure_cursor_visible(e); return; }
        if (key == KEY_RIGHT) { if (e->cursor < e->len) e->cursor++; ensure_cursor_visible(e); return; }
        if (key == KEY_HOME) { e->cursor = line_start_pos(e->text, line_for_pos(e->text, e->cursor)); ensure_cursor_visible(e); return; }
        if (key == KEY_END) {
            while (e->cursor < e->len && e->text[e->cursor] != '\n') e->cursor++;
            ensure_cursor_visible(e); return;
        }
        if (key == KEY_UP) {
            int line = line_for_pos(e->text, e->cursor);
            int start = line_start_pos(e->text, line);
            int col = e->cursor - start;
            if (line > 0) {
                int ps = line_start_pos(e->text, line - 1);
                int pe = ps;
                while (e->text[pe] && e->text[pe] != '\n') pe++;
                e->cursor = ps + clamp_i(col, 0, pe - ps);
            }
            ensure_cursor_visible(e); return;
        }
        if (key == KEY_DOWN) {
            int line = line_for_pos(e->text, e->cursor);
            int start = line_start_pos(e->text, line);
            int col = e->cursor - start;
            int ns = line_start_pos(e->text, line + 1);
            if (ns < e->len) {
                int ne = ns;
                while (e->text[ne] && e->text[ne] != '\n') ne++;
                e->cursor = ns + clamp_i(col, 0, ne - ns);
            }
            ensure_cursor_visible(e); return;
        }
        char ch = key_to_char(key, shift);
        if (ch) insert_char(e, ch);
        break;
    }
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&e->resize, lparam, EDITOR_MIN_W, EDITOR_MIN_H);
        break;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&e->resize, lparam);
        break;
    case WM_WINDOWPOSCHANGED: {
        MyAppResizeOnWindowPosChanged(&e->resize, lparam, TITLEBAR_H);
        WINDOWPOS* wp = (WINDOWPOS*)lparam;
        if (wp) { e->win_x = wp->x; e->win_y = wp->y; e->win_w = wp->cx; e->win_h = wp->cy; editor_make_layout(e, e->win_w, e->win_h); }
        break;
    }
    case WM_MOVE:
        MyAppResizeOnMove(&e->resize, lparam);
        break;
    case WM_SIZE:
        MyAppResizeOnSize(&e->resize, wparam, lparam);
        break;
    case WM_DESTROY:
        e->hwnd = 0;
        break;
    default: break;
    }
}


static LRESULT CALLBACK editor_winproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_CREATE:
    case WM_LBUTTONDOWN:
    case WM_KEYDOWN:
    case WM_GETMINMAXINFO:
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
    case WM_MOVE:
    case WM_SIZE:
    case WM_DESTROY:
        editor_wndproc(hWnd, Msg, wParam, lParam, NULL);
        return 0;
    case WM_CLOSE:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static ATOM editor_register_class(void)
{
    static ATOM s_atom = 0;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = editor_winproc;
    wc.lpszClassName = "myOS.Editor";
    s_atom = RegisterClassExA(&wc);
    return s_atom;
}

HWND editor_create(HWNDManager* mgr, const char* path, Capability cap)
{
    Editor* e = editor_alloc();
    snprintf(e->path, sizeof(e->path), "%s", path && path[0] ? path : "/tmp/myos_editor.txt");
    snprintf(e->name, sizeof(e->name), "%.255s", base_name(e->path));
    editor_load(e);
    e->win_w = EDITOR_W;
    e->win_h = EDITOR_H;
    MyAppResizeInit(&e->resize, EDITOR_W, EDITOR_H, TITLEBAR_H);
    MyWinBindRuntime(mgr, &cap);
    editor_register_class();
    HWND hwnd = CreateWindowExA(
        WS_EX_NONE,
        "myOS.Editor",
        e->name,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, EDITOR_W, EDITOR_H,
        0, 0, 0, e);
    e->hwnd = hwnd;
    return hwnd;
}
