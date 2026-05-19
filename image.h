#pragma once
#include "fb.h"

typedef struct Image {
    int width;
    int height;
    Color* pixels;
} Image;

void image_free(Image* img);
int  image_load_any(const char* path, Image* img);
void image_draw_scaled(Framebuffer* fb, const Image* img, int x, int y, int w, int h);
