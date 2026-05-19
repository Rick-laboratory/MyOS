#include "fb.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int fb_init(Framebuffer* fb, const char* path)
{
    memset(fb, 0, sizeof(*fb));

    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) { perror("fb: open"); return -1; }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo);

    fb->width  = vinfo.xres;
    fb->height = vinfo.yres;
    fb->stride = finfo.line_length;

    size_t size = fb->stride * fb->height;

    fb->pixels  = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fb->fd, 0);
    fb->backbuf = malloc(size);   // double buffer im RAM

    if (fb->pixels == MAP_FAILED || !fb->backbuf) {
        perror("fb: mmap/malloc"); return -1;
    }

    fb_reset_clip(fb);
    printf("fb: %dx%d stride=%d\n", fb->width, fb->height, fb->stride);
    return 0;
}

void fb_destroy(Framebuffer* fb)
{
    if (fb->backbuf) free(fb->backbuf);
    if (fb->pixels)  munmap(fb->pixels, fb->stride * fb->height);
    if (fb->fd >= 0) close(fb->fd);
}

// Backbuf → Bildschirm - ein einziger memcpy
// Das ist der Trick: kein Flimmern, kein Tearing
void fb_flip(Framebuffer* fb)
{
    if (!fb || !fb->pixels || !fb->backbuf) return;
    memcpy(fb->pixels, fb->backbuf, fb->stride * fb->height);
}

void fb_flip_rect(Framebuffer* fb, int x, int y, int w, int h)
{
    if (!fb || !fb->pixels || !fb->backbuf || w <= 0 || h <= 0) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > fb->width) x2 = fb->width;
    if (y2 > fb->height) y2 = fb->height;
    if (x >= x2 || y >= y2) return;

    size_t bytes = (size_t)(x2 - x) * sizeof(uint32_t);
    for (int py = y; py < y2; ++py) {
        memcpy(fb->pixels + (size_t)py * (size_t)fb->stride + (size_t)x * sizeof(uint32_t),
               fb->backbuf + (size_t)py * (size_t)fb->stride + (size_t)x * sizeof(uint32_t),
               bytes);
    }
}

void fb_set_clip(Framebuffer* fb, int x, int y, int w, int h)
{
    if (!fb) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > fb->width) x2 = fb->width;
    if (y2 > fb->height) y2 = fb->height;
    if (x >= x2 || y >= y2) {
        fb->clip_enabled = 1;
        fb->clip_x = fb->clip_y = fb->clip_w = fb->clip_h = 0;
        return;
    }
    fb->clip_enabled = 1;
    fb->clip_x = x;
    fb->clip_y = y;
    fb->clip_w = x2 - x;
    fb->clip_h = y2 - y;
}

void fb_reset_clip(Framebuffer* fb)
{
    if (!fb) return;
    fb->clip_enabled = 0;
    fb->clip_x = 0;
    fb->clip_y = 0;
    fb->clip_w = fb->width;
    fb->clip_h = fb->height;
}

int fb_current_clip(Framebuffer* fb, int* x, int* y, int* w, int* h)
{
    if (!fb) return 0;
    if (fb->clip_enabled) {
        if (x) *x = fb->clip_x;
        if (y) *y = fb->clip_y;
        if (w) *w = fb->clip_w;
        if (h) *h = fb->clip_h;
    } else {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = fb->width;
        if (h) *h = fb->height;
    }
    return 1;
}

// Ab hier alles in den backbuf zeichnen
void fb_clear(Framebuffer* fb, Color c)
{
    if (!fb || !fb->backbuf) return;
    if (fb->clip_enabled) {
        fb_rect(fb, fb->clip_x, fb->clip_y, fb->clip_w, fb->clip_h, c);
        return;
    }
    uint32_t* p = (uint32_t*)fb->backbuf;
    int total = (fb->stride / 4) * fb->height;
    for (int i = 0; i < total; i++) p[i] = c;
}

void fb_pixel(Framebuffer* fb, int x, int y, Color c)
{
    if (!fb || x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    if (fb->clip_enabled) {
        if (x < fb->clip_x || y < fb->clip_y ||
            x >= fb->clip_x + fb->clip_w || y >= fb->clip_y + fb->clip_h) return;
    }
    uint32_t* row = (uint32_t*)(fb->backbuf + y * fb->stride);
    row[x] = c;
}

void fb_rect(Framebuffer* fb, int x, int y, int w, int h, Color c)
{
    if (!fb || w <= 0 || h <= 0) return;
    int x2 = x + w; if (x2 > fb->width)  x2 = fb->width;
    int y2 = y + h; if (y2 > fb->height) y2 = fb->height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (fb->clip_enabled) {
        int cx2 = fb->clip_x + fb->clip_w;
        int cy2 = fb->clip_y + fb->clip_h;
        if (x < fb->clip_x) x = fb->clip_x;
        if (y < fb->clip_y) y = fb->clip_y;
        if (x2 > cx2) x2 = cx2;
        if (y2 > cy2) y2 = cy2;
    }
    if (x >= x2 || y >= y2) return;
    for (int py = y; py < y2; py++) {
        uint32_t* row = (uint32_t*)(fb->backbuf + py * fb->stride);
        for (int px = x; px < x2; px++) row[px] = c;
    }
}

void fb_rect_outline(Framebuffer* fb, int x, int y, int w, int h, Color c)
{
    fb_rect(fb, x,     y,     w, 1, c);
    fb_rect(fb, x,     y+h-1, w, 1, c);
    fb_rect(fb, x,     y,     1, h, c);
    fb_rect(fb, x+w-1, y,     1, h, c);
}
