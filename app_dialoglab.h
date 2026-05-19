#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define DIALOGLAB_W      920
#define DIALOGLAB_H      300
#define DIALOGLAB_MIN_W  650
#define DIALOGLAB_MIN_H  260

HWND dialoglab_create(HWNDManager* mgr, int x, int y, Capability cap);
void dialoglab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
