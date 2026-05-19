#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define MDILAB_W      760
#define MDILAB_H      480
#define MDILAB_MIN_W  560
#define MDILAB_MIN_H  360

#define MDILAB_CMD_NEW      0xA201u
#define MDILAB_CMD_TILE     0xA202u
#define MDILAB_CMD_CASCADE  0xA203u
#define MDILAB_CMD_NEXT     0xA204u
#define MDILAB_CMD_CLOSE    0xA205u

HWND mdilab_create(HWNDManager* mgr, int x, int y, Capability cap);
void mdilab_destroy(void);
void mdilab_blit(HWND hwnd, int x, int y, int w, int h, Framebuffer* fb);
