#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

#define EDITOR_W      560
#define EDITOR_H      380
#define EDITOR_MIN_W  300
#define EDITOR_MIN_H  220

HWND editor_create(HWNDManager* mgr, const char* path, Capability cap);
void editor_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);
void editor_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, void* userdata);
