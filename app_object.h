#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define OBJECT_W      660
#define OBJECT_H      380
#define OBJECT_MIN_W  560
#define OBJECT_MIN_H  300

#define OBJLAB_REFRESH    (WM_USER + 0x700)
#define OBJLAB_MAKE_SEC   (WM_USER + 0x701)
#define OBJLAB_CLOSE_SEC  (WM_USER + 0x702)

HWND objectlab_create(HWNDManager* mgr, int x, int y, Capability cap);
void objectlab_destroy(void);
void objectlab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
