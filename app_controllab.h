#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define CONTROLLAB_W      680
#define CONTROLLAB_H      430
#define CONTROLLAB_MIN_W  560
#define CONTROLLAB_MIN_H  340

#define CTRL_CMD_CREATE_BUTTON   3701
#define CTRL_CMD_CREATE_EDIT     3702
#define CTRL_CMD_CREATE_LISTBOX  3703
#define CTRL_CMD_ADD_ITEM        3704
#define CTRL_CMD_CLEAR_LIST      3705
#define CTRL_CMD_READ_EDIT       3706
#define CTRL_CMD_SET_EDIT        3707
#define CTRL_CMD_COPY            3708
#define CTRL_CMD_PASTE           3709

HWND controllab_create(HWNDManager* mgr, int x, int y, Capability cap);
void controllab_destroy(void);
void controllab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
