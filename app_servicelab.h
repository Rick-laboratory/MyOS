#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define SERVICELAB_W      720
#define SERVICELAB_H      430
#define SERVICELAB_MIN_W  600
#define SERVICELAB_MIN_H  340

#define SVC_CMD_OPEN_SCM   3901
#define SVC_CMD_CREATE     3902
#define SVC_CMD_OPEN       3903
#define SVC_CMD_START      3904
#define SVC_CMD_STOP       3905
#define SVC_CMD_QUERY      3906
#define SVC_CMD_DELETE     3907
#define SVC_CMD_CLOSE      3908
#define SVC_CMD_AUTO       3909
#define SVC_CMD_REFRESH    3910

HWND servicelab_create(HWNDManager* mgr, int x, int y, Capability cap);
void servicelab_destroy(void);
void servicelab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
