#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define PAINTLAB_W      680
#define PAINTLAB_H      430
#define PAINTLAB_MIN_W  560
#define PAINTLAB_MIN_H  330

#define PAINT_CMD_INVALIDATE 3401
#define PAINT_CMD_TEXT       3402
#define PAINT_CMD_RECT       3403
#define PAINT_CMD_GETDC      3404
#define PAINT_CMD_CLEAR      3405
#define PAINT_CMD_STRESS     3406
#define PAINT_CMD_VALIDATE   3407
#define PAINT_CMD_BRUSHPLUS  3408
#define PAINT_CMD_DELBRUSH   3409

HWND paintlab_create(HWNDManager* mgr, int x, int y, Capability cap);
void paintlab_destroy(void);
void paintlab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
