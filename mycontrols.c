#include "mycontrols.h"
#include "font.h"
#include <stdio.h>
#include <string.h>

void DrawClipTextA(Framebuffer* fb, int x, int y, const char* s, Color c,
                        int clip_x, int clip_y, int clip_w, int clip_h)
{
    if (!fb || !s || clip_w <= 0 || clip_h <= 0) return;
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

int PtInRectXY(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

int PtInRectSlopXY(int x, int y, int rx, int ry, int rw, int rh, int slop)
{
    return x >= rx - slop && x < rx + rw + slop &&
           y >= ry - slop && y < ry + rh + slop;
}

int HitTestButtonControl(const MYBUTTONCONTROL* buttons, int count, int x, int y, int slop)
{
    if (!buttons || count <= 0) return -1;
    for (int i = 0; i < count; ++i) {
        if (PtInRectSlopXY(x, y, buttons[i].x, buttons[i].y,
                                   buttons[i].w, buttons[i].h, slop))
            return buttons[i].id;
    }
    return -1;
}

void DrawButtonControl(Framebuffer* fb, int x, int y, int w, int h, const char* text)
{
    if (!fb) return;
    fb_rect(fb, x, y, w, h, COLOR(45,49,70));
    fb_rect_outline(fb, x, y, w, h, COLOR(120,135,170));
    if (text) font_draw_str(fb, x + 7, y + 6, text, WHITE);
}

void PushLogLineA(char* storage, int line_count, int line_chars, int* used, const char* text)
{
    if (!storage || !used || line_count <= 0 || line_chars <= 1 || !text) return;
    if (*used < 0) *used = 0;
    if (*used < line_count) {
        snprintf(storage + (*used * line_chars), (size_t)line_chars, "%.*s", line_chars - 1, text);
        (*used)++;
        return;
    }
    memmove(storage, storage + line_chars, (size_t)(line_count - 1) * (size_t)line_chars);
    snprintf(storage + ((line_count - 1) * line_chars), (size_t)line_chars, "%.*s", line_chars - 1, text);
}
