#pragma once
/*
 * myOS Win32 SDK - windef.h
 *
 * Public WinSDK-compatible base type facade. v118 keeps the storage typedefs
 * in mytypes.h, but RECT and common calling-convention helpers now live here
 * so external app code can include <windows.h> without pulling mywin.h.
 */
/* AUDIT(v118): Transitional SDK leak. windef.h still pulls myOS storage typedefs
   from ../../mytypes.h. This keeps the v118 build stable, but strict external
   WinSDK headers should eventually define the public base types here directly. */
#include "../../mytypes.h"

#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef APIENTRY
#define APIENTRY WINAPI
#endif
#ifndef CONST
#define CONST const
#endif

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#ifndef MYOS_RECT_DEFINED
#define MYOS_RECT_DEFINED
typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT, *LPRECT;
#endif

#ifndef RGB
#define RGB(r,g,b) ((DWORD)((((BYTE)(r)) << 16) | (((BYTE)(g)) << 8) | ((BYTE)(b))))
#endif

typedef DWORD COLORREF;
