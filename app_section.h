#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define SECTION_W      620
#define SECTION_H      340
#define SECTION_MIN_W  480
#define SECTION_MIN_H  260

#define SECLAB_CREATE_MAP  (WM_USER + 0x400)
#define SECLAB_WRITE       (WM_USER + 0x401)
#define SECLAB_READ        (WM_USER + 0x402)
#define SECLAB_UNMAP       (WM_USER + 0x403)

HWND section_create(HWNDManager* mgr, int x, int y, Capability cap);
void section_destroy(void);
void section_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
