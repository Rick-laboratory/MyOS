#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define DEADLOCK_W      600
#define DEADLOCK_H      340
#define DEADLOCK_MIN_W  460
#define DEADLOCK_MIN_H  260

HWND deadlock_create(HWNDManager* mgr, int x, int y, Capability cap);
void deadlock_destroy(void);
void deadlock_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
