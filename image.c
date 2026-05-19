#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

static void skip_ws_comments(FILE* f)
{
    int c;
    for (;;) {
        c = fgetc(f);
        if (c == '#') {
            while (c != EOF && c != '\n') c = fgetc(f);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c != EOF) ungetc(c, f);
        return;
    }
}

static int read_int_token(FILE* f, int* out)
{
    skip_ws_comments(f);
    char buf[64];
    int n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') {
            while (c != EOF && c != '\n') c = fgetc(f);
            if (n > 0) break;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (n > 0) break;
            continue;
        }
        if (n < (int)sizeof(buf) - 1) buf[n++] = (char)c;
    }
    if (n <= 0) return 0;
    buf[n] = 0;
    *out = atoi(buf);
    return 1;
}

static int image_load_ppm(const char* path, Image* img)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char sig[3] = {0};
    if (fread(sig, 1, 2, f) != 2) { fclose(f); return 0; }
    sig[2] = 0;
    if (strcmp(sig, "P6") != 0) { fclose(f); return 0; }

    int w = 0, h = 0, maxv = 0;
    if (!read_int_token(f, &w) || !read_int_token(f, &h) || !read_int_token(f, &maxv) || w <= 0 || h <= 0 || maxv <= 0) {
        fclose(f); return 0;
    }

    int c = fgetc(f);
    if (c == EOF) { fclose(f); return 0; }

    Color* px = (Color*)calloc((size_t)w * (size_t)h, sizeof(Color));
    if (!px) { fclose(f); return 0; }

    for (int i = 0; i < w * h; i++) {
        unsigned char rgb[3];
        if (fread(rgb, 1, 3, f) != 3) {
            free(px); fclose(f); return 0;
        }
        int r = rgb[0], g = rgb[1], b = rgb[2];
        if (maxv != 255) {
            r = (r * 255) / maxv;
            g = (g * 255) / maxv;
            b = (b * 255) / maxv;
        }
        px[i] = COLOR(r, g, b);
    }

    fclose(f);
    img->width = w;
    img->height = h;
    img->pixels = px;
    return 1;
}

#pragma pack(push,1)
typedef struct BMPFILEHDR {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BMPFILEHDR;

typedef struct BMPINFOHDR {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BMPINFOHDR;
#pragma pack(pop)

static int image_load_bmp(const char* path, Image* img)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    BMPFILEHDR fh;
    BMPINFOHDR ih;
    if (fread(&fh, sizeof(fh), 1, f) != 1 || fread(&ih, sizeof(ih), 1, f) != 1) { fclose(f); return 0; }
    if (fh.bfType != 0x4D42 || ih.biPlanes != 1 || (ih.biBitCount != 24 && ih.biBitCount != 32) || ih.biCompression != 0) {
        fclose(f); return 0;
    }
    int w = ih.biWidth;
    int h = ih.biHeight < 0 ? -ih.biHeight : ih.biHeight;
    int topdown = ih.biHeight < 0;
    if (w <= 0 || h <= 0) { fclose(f); return 0; }

    Color* px = (Color*)calloc((size_t)w * (size_t)h, sizeof(Color));
    if (!px) { fclose(f); return 0; }

    int bpp = ih.biBitCount / 8;
    int row_bytes = ((w * bpp + 3) / 4) * 4;
    unsigned char* row = (unsigned char*)malloc((size_t)row_bytes);
    if (!row) { free(px); fclose(f); return 0; }

    fseek(f, fh.bfOffBits, SEEK_SET);
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, (size_t)row_bytes, f) != (size_t)row_bytes) {
            free(row); free(px); fclose(f); return 0;
        }
        int dy = topdown ? y : (h - 1 - y);
        for (int x = 0; x < w; x++) {
            unsigned char* p = row + x * bpp;
            unsigned char b = p[0], g = p[1], r = p[2];
            px[dy * w + x] = COLOR(r, g, b);
        }
    }

    free(row);
    fclose(f);
    img->width = w;
    img->height = h;
    img->pixels = px;
    return 1;
}

void image_free(Image* img)
{
    if (!img) return;
    free(img->pixels);
    img->pixels = NULL;
    img->width = img->height = 0;
}

int image_load_any(const char* path, Image* img)
{
    if (!img) return 0;
    image_free(img);
    if (!path) return 0;
    const char* dot = strrchr(path, '.');
    if (dot) {
        if (!strcasecmp(dot, ".ppm") || !strcasecmp(dot, ".pnm")) return image_load_ppm(path, img);
        if (!strcasecmp(dot, ".bmp")) return image_load_bmp(path, img);
    }
    if (image_load_ppm(path, img)) return 1;
    return image_load_bmp(path, img);
}

void image_draw_scaled(Framebuffer* fb, const Image* img, int x, int y, int w, int h)
{
    if (!fb || !img || !img->pixels || img->width <= 0 || img->height <= 0 || w <= 0 || h <= 0) return;
    for (int dy = 0; dy < h; dy++) {
        int sy = (dy * img->height) / h;
        if (y + dy < 0 || y + dy >= fb->height) continue;
        for (int dx = 0; dx < w; dx++) {
            int sx = (dx * img->width) / w;
            if (x + dx < 0 || x + dx >= fb->width) continue;
            fb_pixel(fb, x + dx, y + dy, img->pixels[sy * img->width + sx]);
        }
    }
}
