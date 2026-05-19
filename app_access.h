#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define ACCESS_W      560
#define ACCESS_H      330
#define ACCESS_MIN_W  420
#define ACCESS_MIN_H  260

HWND access_create(HWNDManager* mgr, int x, int y, Capability cap);
void access_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
