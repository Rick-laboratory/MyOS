#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define WAITLAB_W      780
#define WAITLAB_H      440
#define WAITLAB_MIN_W  620
#define WAITLAB_MIN_H  310

#define WAITLAB_CREATE_EVENT (WM_USER + 0x800)
#define WAITLAB_OPEN_EVENT   (WM_USER + 0x801)
#define WAITLAB_SET_EVENT    (WM_USER + 0x802)
#define WAITLAB_RESET_EVENT  (WM_USER + 0x803)
#define WAITLAB_WAIT_100     (WM_USER + 0x804)
#define WAITLAB_WAIT_1000    (WM_USER + 0x805)
#define WAITLAB_CREATE_3     (WM_USER + 0x806)
#define WAITLAB_WAIT_ANY     (WM_USER + 0x807)
#define WAITLAB_WAIT_ALL     (WM_USER + 0x808)
#define WAITLAB_DUP_HANDLE   (WM_USER + 0x809)
#define WAITLAB_CLOSE_EVENT  (WM_USER + 0x80a)
#define WAITLAB_SPAWN_CHILD  (WM_USER + 0x80b)
#define WAITLAB_DUP_TO_CHILD (WM_USER + 0x80c)
#define WAITLAB_CREATE_MUTEX (WM_USER + 0x80d)
#define WAITLAB_RELEASE_MUTEX (WM_USER + 0x80e)
#define WAITLAB_CREATE_SEM   (WM_USER + 0x80f)
#define WAITLAB_RELEASE_SEM  (WM_USER + 0x810)
#define WAITLAB_CREATE_TIMER (WM_USER + 0x811)
#define WAITLAB_WAIT_MIXED   (WM_USER + 0x812)
#define WAITLAB_RO_OPEN      (WM_USER + 0x813)
#define WAITLAB_RO_SET       (WM_USER + 0x814)
#define WAITLAB_RO_WAIT      (WM_USER + 0x815)
#define WAITLAB_WAIT_CHILD   (WM_USER + 0x816)
#define WAITLAB_TERM_CHILD   (WM_USER + 0x817)
#define WAITLAB_EXIT_CHILD   (WM_USER + 0x818)
#define WAITLAB_CHILD_CONTEXT (WM_USER + 0x819)
#define WAITLAB_ENV_API       (WM_USER + 0x81a)
#define WAITLAB_MODULE_API    (WM_USER + 0x81b)
#define WAITLAB_DLL_API       (WM_USER + 0x81c)
#define WAITLAB_LOADER_API    (WM_USER + 0x81d)
#define WAITLAB_CONSOLE_API   (WM_USER + 0x81e)
#define WAITLAB_GUI_IPC_API   (WM_USER + 0x81f)

HWND waitlab_create(HWNDManager* mgr, int x, int y, Capability cap);
void waitlab_destroy(void);
void waitlab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
