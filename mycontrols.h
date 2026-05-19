#ifndef MYCONTROLS_H
#define MYCONTROLS_H

#include "fb.h"
#include "mytypes.h"

/* myOS owner-draw control helpers.
   Intentional naming rule: public/helper surface follows WinAPI-style verbs
   and A-suffix string functions where practical. These are not Win32 APIs,
   but they should read like small USER/GDI helpers, not app-local utilities. */
void DrawClipTextA(Framebuffer* fb, int x, int y, const char* s, Color c,
                        int clip_x, int clip_y, int clip_w, int clip_h);

/* Compatibility for old duplicated app code. New code should call DrawClipTextA. */
#define draw_clip_text DrawClipTextA

typedef struct MYBUTTONCONTROL {
    int x, y, w, h;
    int id;
    const char* text;
} MYBUTTONCONTROL;

int  PtInRectXY(int x, int y, int rx, int ry, int rw, int rh);
int  PtInRectSlopXY(int x, int y, int rx, int ry, int rw, int rh, int slop);
int  HitTestButtonControl(const MYBUTTONCONTROL* buttons, int count, int x, int y, int slop);
void DrawButtonControl(Framebuffer* fb, int x, int y, int w, int h, const char* text);

/* Tiny generic fixed-line log ring.  The app owns the storage; this helper only
   performs the common append/scroll operation. */
void PushLogLineA(char* storage, int line_count, int line_chars, int* used, const char* text);

#endif
