#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define SHARED_BUS_W      560
#define SHARED_BUS_H      300
#define SHARED_BUS_MIN_W  420
#define SHARED_BUS_MIN_H  240

#define BUSLAB_CREATE_BUS    (WM_USER + 0x500)
#define BUSLAB_WRITE_NOTIFY  (WM_USER + 0x501)
#define BUSLAB_SPAM_100      (WM_USER + 0x502)
#define BUSLAB_SPAM_10K      (WM_USER + 0x507)
#define BUSLAB_MAP_BUS       (WM_USER + 0x503)
#define BUSLAB_READ_NOW      (WM_USER + 0x504)
#define BUSLAB_CLEAR         (WM_USER + 0x505)
#define BUSLAB_NOTIFY        (WM_USER + 0x506)

HWND sharedbus_create_producer(HWNDManager* mgr, Capability cap);
HWND sharedbus_create_consumer(HWNDManager* mgr, Capability cap);
void sharedbus_destroy(void);
void sharedbus_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
