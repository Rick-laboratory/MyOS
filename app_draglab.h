#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define DRAGLAB_W      640
#define DRAGLAB_H      390
#define DRAGLAB_MIN_W  520
#define DRAGLAB_MIN_H  310

#define DRAG_CMD_CAPTURE  3601
#define DRAG_CMD_RELEASE  3602
#define DRAG_CMD_RESET    3603
#define DRAG_CMD_CANCEL   3604

HWND draglab_create(HWNDManager* mgr, int x, int y, Capability cap);
void draglab_destroy(void);
void draglab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
