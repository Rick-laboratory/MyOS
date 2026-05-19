#pragma once
#include <stdint.h>
#include <stddef.h>

// ─────────────────────────────────────────────
//  myOS public base types - WinAPI-style
//  Diese Typnamen sind absichtlich nah an MSDN/WinAPI gehalten.
// ─────────────────────────────────────────────

typedef char        CHAR;
typedef uint8_t     BYTE;
typedef int16_t     SHORT;
typedef uint16_t    WORD;
typedef int32_t     LONG;
typedef uint32_t    DWORD;
typedef int         BOOL;
typedef uint32_t    UINT;
typedef uintptr_t   UINT_PTR;
typedef uintptr_t   ULONG_PTR;
typedef intptr_t    INT_PTR;
typedef uintptr_t   DWORD_PTR;
typedef intptr_t    LONG_PTR;
typedef uintptr_t   WPARAM;
typedef intptr_t    LPARAM;
typedef intptr_t    LRESULT;
typedef uint16_t    ATOM;

// ─────────────────────────────────────────────
//  v40 semantic id aliases
//  Heute sind PID/TID/Capability-ID noch numerisch kompatibel. Die Typnamen
//  trennen aber ab jetzt die Bedeutung im Code, damit echte Prozess-/Thread-
//  Identitäten später nicht mehr versehentlich mit Capability-IDs vermischt
//  werden.
// ─────────────────────────────────────────────
typedef uint32_t    MyPid;
typedef uint32_t    MyTid;
typedef uint32_t    MyCapId;

typedef const char* LPCSTR;
typedef char*       LPSTR;
typedef void*       LPVOID;
typedef const void* LPCVOID;
typedef DWORD*      LPDWORD;
typedef BOOL*       LPBOOL;
typedef DWORD*      PDWORD;
typedef int*        LPINT;
typedef DWORD_PTR*  PDWORD_PTR;

#ifndef MYOS_POINT_DEFINED
#define MYOS_POINT_DEFINED
typedef struct tagPOINT {
    LONG x;
    LONG y;
} POINT, *PPOINT, *LPPOINT;
#endif


typedef uint32_t    HANDLE;
typedef HANDLE      HWND;
typedef HANDLE      HINSTANCE;
typedef HANDLE      HMODULE;
typedef void (*FARPROC)(void);
typedef HANDLE      HMENU;
typedef HANDLE      HICON;
typedef HANDLE      HCURSOR;
typedef HANDLE      HBRUSH;
typedef HANDLE      HBITMAP;
typedef HANDLE      HGLOBAL;
typedef HANDLE      HRSRC;
typedef HANDLE      HACCEL;
typedef HANDLE      HDC;
typedef HANDLE      HGDIOBJ;
typedef HANDLE      HRGN;
typedef HANDLE      HDWP;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif
