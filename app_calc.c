#include "app_calc.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "app_msdn_resize.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* AUDIT(v119-app): Calc is a canary for USER32 hit-test, mouse capture,
   resize and WM_COMMAND assumptions. It still draws its own buttons instead of
   real BUTTON child HWNDs, so a future strict-control pass can make visual
   clicks continue to draw while no BN_CLICKED-style command is generated. If
   "first click does nothing" returns, check focus/capture routing and the
   coordinate conversion path before touching calculator math. */


// ─────────────────────────────────────────────
//  Skalierender Rechner
//  - Layout wird aus aktueller Fenstergröße berechnet
//  - Buttons/Display/History skalieren/reflowen beim Resize
//  - Verlauf als kleiner Ringbuffer
// ─────────────────────────────────────────────

#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif
#ifndef RESIZE_GRIP
#define RESIZE_GRIP 6
#endif

typedef struct {
    int x, y, w, h;
} RectI;

typedef struct {
    RectI display;
    RectI history;
    RectI buttons[5][4];
    int   cw, ch;
    int   pad, gap;
    int   history_visible;
} CalcLayout;

typedef struct {
    char    display[64];
    double  operand;
    double  result;
    char    op;
    int     fresh;
    int     error;

    char    history[10][80];
    int     hist_count;
    int     hist_next;

    int      win_x, win_y, win_w, win_h;
    CalcLayout layout;
    MyAppResizeState resize;
} Calc;

static Calc g_calc;

// ── kleine Zeichenhelfer ─────────────────────

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
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

static void draw_str_right(Framebuffer* fb, RectI r, const char* s, Color c)
{
    int len = s ? (int)strlen(s) : 0;
    int max_chars = (r.w - 10) / 8;
    if (max_chars < 1) max_chars = 1;

    const char* out = s ? s : "";
    if (len > max_chars) {
        out = s + (len - max_chars);
        len = max_chars;
    }

    int x = r.x + r.w - 6 - len * 8;
    int y = r.y + (r.h - 8) / 2;
    draw_str_clip(fb, x, y, out, c, r.x + 2, r.y + 2, r.w - 4, r.h - 4);
}

static void draw_centered(Framebuffer* fb, RectI r, const char* s, Color c)
{
    int lw = s ? (int)strlen(s) * 8 : 0;
    int x = r.x + (r.w - lw) / 2;
    int y = r.y + (r.h - 8) / 2;
    draw_str_clip(fb, x, y, s, c, r.x + 2, r.y + 2, r.w - 4, r.h - 4);
}

// ── Layout ───────────────────────────────────

static void calc_make_layout(CalcLayout* l, int cw, int ch)
{
    memset(l, 0, sizeof(*l));
    l->cw = cw;
    l->ch = ch;

    int base = cw < ch ? cw : ch;
    l->pad = clamp_i(base / 28, 6, 14);
    l->gap = clamp_i(base / 55, 4, 10);

    int pad = l->pad;
    int gap = l->gap;

    int display_h = clamp_i(ch / 7, 38, 62);
    l->display.x = pad;
    l->display.y = pad;
    l->display.w = cw - pad * 2;
    l->display.h = display_h;

    int y = l->display.y + l->display.h + gap;
    int bottom_pad = pad + RESIZE_GRIP;
    int avail_after_display = ch - y - bottom_pad;

    int button_gap_total = gap * 4;
    int ideal_button_h = (avail_after_display - button_gap_total) / 5;

    int history_h = 0;
    if (ch >= 330) {
        history_h = clamp_i(ch / 5, 48, 104);
        ideal_button_h = (avail_after_display - history_h - gap - button_gap_total) / 5;
        if (ideal_button_h < 30) {
            history_h = clamp_i(ch / 8, 32, 56);
            ideal_button_h = (avail_after_display - history_h - gap - button_gap_total) / 5;
        }
    }

    if (history_h >= 28 && ideal_button_h >= 26) {
        l->history_visible = 1;
        l->history.x = pad;
        l->history.y = y;
        l->history.w = cw - pad * 2;
        l->history.h = history_h;
        y += history_h + gap;
    } else {
        l->history_visible = 0;
    }

    int bw = (cw - pad * 2 - gap * 3) / 4;
    int bh = (ch - y - bottom_pad - gap * 4) / 5;
    if (bh < 22) bh = 22;
    if (bw < 34) bw = 34;

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            l->buttons[row][col].x = pad + col * (bw + gap);
            l->buttons[row][col].y = y + row * (bh + gap);
            l->buttons[row][col].w = bw;
            l->buttons[row][col].h = bh;
        }
    }
}

// ── Buttons ──────────────────────────────────

typedef struct { const char* label; char action; Color color; } Btn;

static const Btn btns[5][4] = {
    {{"C", 'C', COLOR(80,40,40)}, {"+-",'N', COLOR(50,50,65)}, {"%", 'P', COLOR(50,50,65)}, {"/", '/', COLOR(40,65,130)}},
    {{"7", '7', COLOR(50,50,65)}, {"8", '8', COLOR(50,50,65)}, {"9", '9', COLOR(50,50,65)}, {"*", '*', COLOR(40,65,130)}},
    {{"4", '4', COLOR(50,50,65)}, {"5", '5', COLOR(50,50,65)}, {"6", '6', COLOR(50,50,65)}, {"-", '-', COLOR(40,65,130)}},
    {{"1", '1', COLOR(50,50,65)}, {"2", '2', COLOR(50,50,65)}, {"3", '3', COLOR(50,50,65)}, {"+", '+', COLOR(40,65,130)}},
    {{"0", '0', COLOR(50,50,65)}, {".", '.', COLOR(50,50,65)}, {"<-",'B', COLOR(50,50,65)}, {"=", '=', COLOR(40,95,55)}},
};

static void history_push(const char* line)
{
    if (!line || !*line) return;
    snprintf(g_calc.history[g_calc.hist_next], sizeof(g_calc.history[g_calc.hist_next]), "%s", line);
    g_calc.hist_next = (g_calc.hist_next + 1) % 10;
    if (g_calc.hist_count < 10) g_calc.hist_count++;
}

static const char* history_get_newest_first(int n)
{
    if (n < 0 || n >= g_calc.hist_count) return NULL;
    int idx = g_calc.hist_next - 1 - n;
    while (idx < 0) idx += 10;
    return g_calc.history[idx % 10];
}

static void set_display_num(double v)
{
    if (!isfinite(v)) {
        snprintf(g_calc.display, sizeof(g_calc.display), "Error");
        g_calc.error = 1;
        g_calc.fresh = 1;
        return;
    }
    snprintf(g_calc.display, sizeof(g_calc.display), "%.12g", v);
    g_calc.error = 0;
}

// ── Berechnung ───────────────────────────────

static void apply(char action)
{
    Calc* c = &g_calc;

    if (c->error && action != 'C') {
        snprintf(c->display, sizeof(c->display), "0");
        c->error = 0;
        c->fresh = 1;
    }

    switch (action) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9': {
        if (c->fresh || strcmp(c->display, "0") == 0) {
            snprintf(c->display, sizeof(c->display), "%c", action);
            c->fresh = 0;
        } else {
            int l = (int)strlen(c->display);
            if (l < (int)sizeof(c->display) - 1 && l < 24) {
                c->display[l] = action;
                c->display[l + 1] = 0;
            }
        }
        break;
    }
    case '.': {
        if (c->fresh) {
            snprintf(c->display, sizeof(c->display), "0.");
            c->fresh = 0;
        } else if (!strchr(c->display, '.')) {
            int l = (int)strlen(c->display);
            if (l < (int)sizeof(c->display) - 1) {
                c->display[l] = '.';
                c->display[l + 1] = 0;
            }
        }
        break;
    }
    case 'B': {
        if (!c->fresh) {
            int l = (int)strlen(c->display);
            if (l > 1) c->display[l - 1] = 0;
            else snprintf(c->display, sizeof(c->display), "0");
        }
        break;
    }
    case 'C':
        c->result = 0;
        c->operand = 0;
        c->op = 0;
        c->fresh = 1;
        c->error = 0;
        snprintf(c->display, sizeof(c->display), "0");
        break;
    case 'N':
        set_display_num(-atof(c->display));
        break;
    case 'P':
        set_display_num(atof(c->display) / 100.0);
        break;
    case '+': case '-': case '*': case '/':
        c->operand = atof(c->display);
        c->op = action;
        c->fresh = 1;
        break;
    case '=':
        if (c->op) {
            double rhs = atof(c->display);
            double out = 0.0;
            int ok = 1;
            switch (c->op) {
            case '+': out = c->operand + rhs; break;
            case '-': out = c->operand - rhs; break;
            case '*': out = c->operand * rhs; break;
            case '/':
                if (rhs == 0.0) ok = 0;
                else out = c->operand / rhs;
                break;
            }

            char line[80];
            if (ok) {
                snprintf(line, sizeof(line), "%.12g %c %.12g = %.12g", c->operand, c->op, rhs, out);
                history_push(line);
                set_display_num(out);
                c->result = out;
            } else {
                snprintf(line, sizeof(line), "%.12g %c %.12g = Error", c->operand, c->op, rhs);
                history_push(line);
                snprintf(c->display, sizeof(c->display), "Error");
                c->error = 1;
            }
            c->op = 0;
            c->fresh = 1;
        }
        break;
    }
}

// ── Render direkt in den Desktop-Backbuffer ──

static void draw_history(Framebuffer* fb, RectI r)
{
    fb_rect(fb, r.x, r.y, r.w, r.h, COLOR(18, 18, 28));
    fb_rect_outline(fb, r.x, r.y, r.w, r.h, COLOR(70, 70, 95));
    draw_str_clip(fb, r.x + 6, r.y + 5, "Verlauf", COLOR(150, 150, 180), r.x, r.y, r.w, r.h);

    int max_lines = (r.h - 18) / 10;
    if (max_lines < 1) return;
    if (max_lines > g_calc.hist_count) max_lines = g_calc.hist_count;

    for (int i = 0; i < max_lines; i++) {
        const char* line = history_get_newest_first(i);
        if (!line) break;
        int y = r.y + 18 + i * 10;
        draw_str_clip(fb, r.x + 6, y, line, COLOR(210, 210, 225), r.x + 2, r.y + 2, r.w - 4, r.h - 4);
    }
}

void calc_blit(HWND hwnd __attribute__((unused)),
               int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    int client_x = wx + 1;
    int client_y = wy + TITLEBAR_H;
    int cw = ww - 2;
    int ch = wh - TITLEBAR_H - 1;
    if (cw < 40 || ch < 40) return;

    g_calc.win_x = wx;
    g_calc.win_y = wy;
    g_calc.win_w = ww;
    g_calc.win_h = wh;
    calc_make_layout(&g_calc.layout, cw, ch);

    fb_rect(fb, client_x, client_y, cw, ch, COLOR(23, 23, 34));

    CalcLayout* l = &g_calc.layout;
    RectI d = l->display;
    d.x += client_x; d.y += client_y;

    fb_rect(fb, d.x, d.y, d.w, d.h, COLOR(13, 13, 22));
    fb_rect_outline(fb, d.x, d.y, d.w, d.h, COLOR(85, 85, 120));

    if (g_calc.op) {
        char opbuf[24];
        snprintf(opbuf, sizeof(opbuf), "%.8g %c", g_calc.operand, g_calc.op);
        draw_str_clip(fb, d.x + 6, d.y + 6, opbuf, COLOR(135, 135, 160), d.x, d.y, d.w, d.h);
    }
    draw_str_right(fb, d, g_calc.display, WHITE);

    if (l->history_visible) {
        RectI h = l->history;
        h.x += client_x; h.y += client_y;
        draw_history(fb, h);
    }

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            RectI b = l->buttons[row][col];
            b.x += client_x; b.y += client_y;
            fb_rect(fb, b.x, b.y, b.w, b.h, btns[row][col].color);
            fb_rect_outline(fb, b.x, b.y, b.w, b.h, COLOR(92, 92, 125));
            draw_centered(fb, b, btns[row][col].label, WHITE);
        }
    }
}

// ── WndProc ──────────────────────────────────

void calc_wndproc(HWND hwnd __attribute__((unused)), UINT msg,
                  WPARAM wparam __attribute__((unused)), LPARAM lparam, void* userdata __attribute__((unused)))
{
    switch (msg) {
    case WM_CREATE:
        snprintf(g_calc.display, sizeof(g_calc.display), "0");
        g_calc.fresh = 1;
        MyAppResizeInit(&g_calc.resize, CALC_W, CALC_H, TITLEBAR_H);
        break;

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lparam);
        int my = GET_Y_LPARAM(lparam);

        // Layout aus dem letzten Render passt zur aktuellen Fenstergröße.
        CalcLayout* l = &g_calc.layout;
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 4; col++) {
                RectI b = l->buttons[row][col];
                if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) {
                    apply(btns[row][col].action);
                    return;
                }
            }
        }
        break;
    }
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_calc.resize, lparam, CALC_MIN_W, CALC_MIN_H);
        break;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_calc.resize, lparam);
        break;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_calc.resize, lparam, TITLEBAR_H);
        if (g_calc.resize.clientW > 0 && g_calc.resize.clientH > 0)
            calc_make_layout(&g_calc.layout, g_calc.resize.clientW, g_calc.resize.clientH);
        break;
    case WM_MOVE:
        MyAppResizeOnMove(&g_calc.resize, lparam);
        break;
    case WM_SIZE:
        MyAppResizeOnSize(&g_calc.resize, wparam, lparam);
        if (g_calc.resize.clientW > 0 && g_calc.resize.clientH > 0)
            calc_make_layout(&g_calc.layout, g_calc.resize.clientW, g_calc.resize.clientH);
        break;
    default: break;
    }
}


static LRESULT CALLBACK calc_winproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_CREATE:
    case WM_LBUTTONDOWN:
    case WM_GETMINMAXINFO:
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
    case WM_MOVE:
    case WM_SIZE:
        calc_wndproc(hWnd, Msg, wParam, lParam, &g_calc);
        return 0;
    case WM_CLOSE:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static ATOM calc_register_class(void)
{
    static ATOM s_atom = 0;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = calc_winproc;
    wc.lpszClassName = "myOS.Calc";
    s_atom = RegisterClassExA(&wc);
    return s_atom;
}

HWND calc_create(HWNDManager* mgr, Framebuffer* fb __attribute__((unused)),
                 int x, int y, Capability cap)
{
    memset(&g_calc, 0, sizeof(g_calc));
    snprintf(g_calc.display, sizeof(g_calc.display), "0");
    g_calc.fresh = 1;
    g_calc.win_x = x;
    g_calc.win_y = y;
    g_calc.win_w = CALC_W;
    g_calc.win_h = CALC_H;
    MyAppResizeInit(&g_calc.resize, CALC_W, CALC_H, TITLEBAR_H);
    calc_make_layout(&g_calc.layout, CALC_W - 2, CALC_H - TITLEBAR_H - 1);

    MyWinBindRuntime(mgr, &cap);
    calc_register_class();
    return CreateWindowExA(
        WS_EX_NONE,
        "myOS.Calc",
        "Calculator",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        x, y, CALC_W, CALC_H,
        0, 0, 0, NULL);
}
