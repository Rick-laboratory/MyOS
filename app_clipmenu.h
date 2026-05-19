#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define CLIPMENU_W      640
#define CLIPMENU_H      360
#define CLIPMENU_MIN_W  560
#define CLIPMENU_MIN_H  300

#define CLIP_CMD_SET     3101
#define CLIP_CMD_GET     3102
#define CLIP_CMD_CLEAR   3103
#define CLIP_CMD_NEW     3104
#define CLIP_CMD_COPY    3105
#define CLIP_CMD_PASTE   3106
#define CLIP_CMD_POPUP   3107
#define CLIP_CMD_ATTACH  3108

HWND clipmenu_create(HWNDManager* mgr, int x, int y, Capability cap);
void clipmenu_destroy(void);
void clipmenu_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
