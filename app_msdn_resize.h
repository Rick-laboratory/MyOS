#pragma once
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <stdio.h>

/* v82: tiny per-app cache for the Win32/MSDN WindowPos contract.
   The WindowManager owns the real WINDOWPOS commit.  Apps consume the same
   messages normal Win32 apps would: WM_GETMINMAXINFO, WM_WINDOWPOSCHANGING,
   WM_WINDOWPOSCHANGED, WM_MOVE and WM_SIZE. */
typedef struct MyAppResizeState {
    int initialized;
    int winX, winY, winW, winH;
    int clientX, clientY, clientW, clientH;
    UINT lastMsg;
    UINT lastSizeType;
    UINT lastPosFlags;
    DWORD minmaxCount;
    DWORD changingCount;
    DWORD changedCount;
    DWORD moveCount;
    DWORD sizeCount;
    DWORD enterSizeMoveCount;
    DWORD exitSizeMoveCount;
} MyAppResizeState;

static inline int myos_app_max_i(int a, int b) { return a > b ? a : b; }

static inline void MyAppResizeInit(MyAppResizeState* s, int winW, int winH, int titlebarH)
{
    if (!s) return;
    s->initialized = 1;
    s->winW = winW;
    s->winH = winH;
    s->clientW = myos_app_max_i(0, winW - 2);
    s->clientH = myos_app_max_i(0, winH - titlebarH - 1);
}

static inline void MyAppResizeOnGetMinMaxInfo(MyAppResizeState* s, LPARAM lp, int minW, int minH)
{
    if (s) { s->lastMsg = WM_GETMINMAXINFO; s->minmaxCount++; }
    MINMAXINFO* mmi = (MINMAXINFO*)lp;
    if (!mmi) return;
    if (mmi->ptMinTrackSize.x < minW) mmi->ptMinTrackSize.x = minW;
    if (mmi->ptMinTrackSize.y < minH) mmi->ptMinTrackSize.y = minH;
    if (mmi->ptMaxTrackSize.x <= 0) mmi->ptMaxTrackSize.x = 32767;
    if (mmi->ptMaxTrackSize.y <= 0) mmi->ptMaxTrackSize.y = 32767;
}

static inline void MyAppResizeOnWindowPosChanging(MyAppResizeState* s, LPARAM lp)
{
    if (!s) return;
    s->lastMsg = WM_WINDOWPOSCHANGING;
    s->changingCount++;
    WINDOWPOS* wp = (WINDOWPOS*)lp;
    if (wp) s->lastPosFlags = wp->flags;
}

static inline void MyAppResizeOnWindowPosChanged(MyAppResizeState* s, LPARAM lp, int titlebarH)
{
    if (!s) return;
    s->lastMsg = WM_WINDOWPOSCHANGED;
    s->changedCount++;
    WINDOWPOS* wp = (WINDOWPOS*)lp;
    if (wp) {
        s->winX = wp->x;
        s->winY = wp->y;
        s->winW = wp->cx;
        s->winH = wp->cy;
        s->clientX = wp->x + 1;
        s->clientY = wp->y + titlebarH;
        s->clientW = myos_app_max_i(0, wp->cx - 2);
        s->clientH = myos_app_max_i(0, wp->cy - titlebarH - 1);
        s->lastPosFlags = wp->flags;
        s->initialized = 1;
    }
}

static inline void MyAppResizeOnMove(MyAppResizeState* s, LPARAM lp)
{
    if (!s) return;
    s->lastMsg = WM_MOVE;
    s->moveCount++;
    s->clientX = GET_X_LPARAM(lp);
    s->clientY = GET_Y_LPARAM(lp);
}

static inline void MyAppResizeOnSize(MyAppResizeState* s, WPARAM wp, LPARAM lp)
{
    if (!s) return;
    s->lastMsg = WM_SIZE;
    s->sizeCount++;
    s->lastSizeType = (UINT)wp;
    s->clientW = GET_X_LPARAM(lp);
    s->clientH = GET_Y_LPARAM(lp);
}

static inline void MyAppResizeOnEnterSizeMove(MyAppResizeState* s)
{
    if (!s) return;
    s->lastMsg = WM_ENTERSIZEMOVE;
    s->enterSizeMoveCount++;
}

static inline void MyAppResizeOnExitSizeMove(MyAppResizeState* s)
{
    if (!s) return;
    s->lastMsg = WM_EXITSIZEMOVE;
    s->exitSizeMoveCount++;
}

static inline void MyAppResizeDescribe(const MyAppResizeState* s, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    if (!s || !s->initialized) {
        snprintf(out, outSize, "pos/size: <not initialized>");
        return;
    }
    snprintf(out, outSize,
             "MSDN pos: client=%dx%d move=%u size=%u wpchg=%u/%u flags=0x%x last=0x%04x",
             s->clientW, s->clientH,
             (unsigned)s->moveCount, (unsigned)s->sizeCount,
             (unsigned)s->changingCount, (unsigned)s->changedCount,
             s->lastPosFlags, s->lastMsg);
}
