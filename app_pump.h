#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define PUMP_W      560
#define PUMP_H      330
#define PUMP_MIN_W  430
#define PUMP_MIN_H  260

#define PMPLAB_POST_SELF  (WM_USER + 0x200)
#define PMPLAB_STRESS     (WM_USER + 0x201)

HWND pump_create(HWNDManager* mgr, int x, int y, Capability cap);
void pump_destroy(void);
void pump_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
