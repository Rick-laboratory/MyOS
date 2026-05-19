#pragma once
#include "fb.h"

// ─────────────────────────────────────────────
//  Bitmap Font 8x8
//  Jedes Zeichen = 8 bytes = 8 Zeilen à 8 Pixel
//  Bit gesetzt = Pixel zeichnen
//  Kein FreeType, keine Datei, keine Library.
//  Die Daten sind einfach im Code.
// ─────────────────────────────────────────────

extern const unsigned char font8x8[95][8];

void font_draw_char(Framebuffer* fb, int x, int y, char c, Color fg);
void font_draw_str(Framebuffer* fb, int x, int y, const char* s, Color fg);
