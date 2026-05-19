#pragma once
#include "fb.h"
#include "font.h"
#include "hwnd.h"
#include "capability.h"

// Default-Fenstergröße. Der Rechner rendert jetzt dynamisch in die aktuelle
// Clientgröße, diese Werte sind nur noch Spawn-/Startwerte.
#define CALC_W      320
#define CALC_H      420
#define CALC_MIN_W  240
#define CALC_MIN_H  280

HWND calc_create(HWNDManager* mgr, Framebuffer* fb,
                 int x, int y, Capability cap);

// Vom Compositor aufgerufen. Zeichnet den Rechner passend zur aktuellen
// Fenstergröße. wx/wy sind Fensterkoordinaten inkl. Titelleiste.
void calc_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb);

void calc_wndproc(HWND hwnd, UINT msg,
                  WPARAM wparam, LPARAM lparam,
                  void* userdata);
