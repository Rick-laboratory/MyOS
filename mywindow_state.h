#pragma once
#include "mytypes.h"

// Shared, read-mostly visible WindowState layout.
// v74: the same layout is mirrored into a real named FileMapping section so
// out-of-process children can map it themselves instead of reading parent memory.
#ifndef MYOS_RECT_DEFINED
#define MYOS_RECT_DEFINED
typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT, *LPRECT;
#endif

#define MYOS_WINDOWSTATE_MAGIC 0x57535453u /* 'WSTS' */
#define MYOS_WINDOWSTATE_LAYOUT_VERSION 74u
#define MYOS_WINDOWSTATE_SECTION_NAME "Global\\myos.v74.hwnd.state.section"

#define MYWS_DIRTY_RECT      0x00000001u
#define MYWS_DIRTY_STYLE     0x00000002u
#define MYWS_DIRTY_TEXT      0x00000004u
#define MYWS_DIRTY_VISIBLE   0x00000008u
#define MYWS_DIRTY_FOCUS     0x00000010u
#define MYWS_DIRTY_ZORDER    0x00000020u
#define MYWS_DIRTY_DESTROY   0x00000040u
#define MYWS_DIRTY_OWNER     0x00000080u

#define MYWSF_VISIBLE     0x00000001u
#define MYWSF_MINIMIZED   0x00000002u
#define MYWSF_ACTIVE      0x00000004u
#define MYWSF_DESTROYED   0x00000008u

typedef struct MyWindowState {
    DWORD cbSize;

    // Even = stable, odd = writer in progress. Readers copy only if both match.
    DWORD seqBegin;

    HWND  hWnd;
    DWORD ownerPid;
    DWORD ownerTid;
    RECT  rcWindow;
    RECT  rcClient;
    BOOL  visible;
    BOOL  minimized;
    BOOL  active;
    BOOL  focused;
    BOOL  enabled;
    BOOL  hasCapture;
    BOOL  destroyed;
    DWORD flags;
    DWORD dirtyFlags;
    DWORD zOrder;
    DWORD style;
    DWORD exStyle;
    DWORD stateVersion;
    DWORD updateSerial;
    UINT  lastMessage;
    CHAR  szTitle[64];

    DWORD seqEnd;
} MyWindowState;

typedef struct MyWindowStateSection {
    DWORD cbSize;
    DWORD magic;
    DWORD version;
    DWORD capacity;
    DWORD activeCount;
    DWORD destroyedCount;
    DWORD updateSerial;
    CHAR  sectionName[96];
    MyWindowState states[64]; // mirrors MAX_HWNDS for this PoC
} MyWindowStateSection;
