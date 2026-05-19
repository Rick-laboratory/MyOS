#pragma once
#include "fb.h"
#include "hwnd.h"
#include "capability.h"

#define SPY_W      560
#define SPY_H      360
#define SPY_MIN_W  360
#define SPY_MIN_H  220

HWND spy_create(HWNDManager* mgr, int x, int y, Capability cap);
void spy_blit(HWND hwnd, int x, int y, int w, int h, Framebuffer* fb);
