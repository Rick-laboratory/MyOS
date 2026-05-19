#pragma once
#include "fb.h"
#include "font.h"
#include "capability.h"
#include "os_calls.h"
#include "ipc.h"
#include "hwnd.h"

#define TERM_COLS     240
#define TERM_ROWS     256
#define TERM_PADDING   6

typedef struct {
    char       buf[TERM_ROWS][TERM_COLS + 1];
    char       input[TERM_COLS + 1];
    int        input_len;
    int        row;
    int        blink;
    int        pipe_fd;
    int        last_visible_cols; // v16.6: fuer Shell-COLUMNS und sauberen Soft-Wrap
    Capability cap;
    IPCBus*      bus;
    HWNDManager* mgr;
    HWND         hwnd;   // das token dieses terminals im OS
} Terminal;

void term_init(Terminal* t, Capability cap, IPCBus* bus, HWNDManager* mgr);
void term_keycode(Terminal* t, int keycode, int shift);
void term_draw(Terminal* t, Framebuffer* fb, int x, int y, int w, int h);
void term_poll(Terminal* t);
void term_print(Terminal* t, const char* s);

// IPC WndProc
void term_wndproc(uint32_t sender, uint16_t msg,
                  intptr_t wparam, intptr_t lparam,
                  void* userdata);

// HWND WndProc - empfängt alle OS Messages
void term_hwnd_proc(HWND hwnd, UINT msg,
                    WPARAM wparam, LPARAM lparam,
                    void* userdata);
