#pragma once
#include <stdint.h>

typedef struct {
    uint8_t*  pixels;   // framebuffer (direkt auf bildschirm)
    uint8_t*  backbuf;  // double buffer (hier zeichnen wir)
    int       width;
    int       height;
    int       stride;
    int       fd;

    // v175: compositor clip rectangle for damage-rect redraw.
    // Drawing primitives silently reject pixels outside this rect; legacy callers
    // that do not set a clip still get the full framebuffer.
    int       clip_enabled;
    int       clip_x;
    int       clip_y;
    int       clip_w;
    int       clip_h;
} Framebuffer;

typedef uint32_t Color;

#define COLOR(r,g,b)  ((Color)((b) | ((g) << 8) | ((r) << 16)))
#define BLACK         COLOR(0,   0,   0  )
#define WHITE         COLOR(255, 255, 255)
#define GRAY          COLOR(50,  50,  50 )
#define DARKGRAY      COLOR(25,  25,  25 )
#define TITLEBAR      COLOR(60,  60,  140)
#define TITLEBAR_ACT  COLOR(80,  80,  200)

int  fb_init(Framebuffer* fb, const char* path);
void fb_destroy(Framebuffer* fb);
void fb_flip(Framebuffer* fb);                                        // backbuf → screen
void fb_flip_rect(Framebuffer* fb, int x, int y, int w, int h);        // v175: partial flip
void fb_set_clip(Framebuffer* fb, int x, int y, int w, int h);         // v175: damage clip
void fb_reset_clip(Framebuffer* fb);                                   // v175: full clip
int  fb_current_clip(Framebuffer* fb, int* x, int* y, int* w, int* h); // v175
void fb_clear(Framebuffer* fb, Color c);
void fb_pixel(Framebuffer* fb, int x, int y, Color c);
void fb_rect(Framebuffer* fb, int x, int y, int w, int h, Color c);
void fb_rect_outline(Framebuffer* fb, int x, int y, int w, int h, Color c);
