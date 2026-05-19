#pragma once

#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MYWIN_MAX_RUNTIME_STACK 16

typedef struct MyWinRuntimeFrame {
    HWNDManager* hwndManager;
    WindowManager* windowManager;
    Capability capability;
    int hasCapability;
} MyWinRuntimeFrame;

extern HWNDManager* g_lpHwndManager;
extern WindowManager* g_lpWindowManager;
extern __thread Capability g_CurrentCapability;
extern __thread int g_HasCapability;
extern __thread MyWinRuntimeFrame g_RuntimeStack[MYWIN_MAX_RUNTIME_STACK];
extern __thread int g_RuntimeDepth;

DWORD mywin_current_pid(void);
BOOL mywin_ensure_runtime_process_objects(const Capability* cap, HANDLE* processObject, HANDLE* threadObject);
void mywin_note_runtime_process(const Capability* cap, HANDLE processObject, HANDLE threadObject, const char* imageName);

#ifdef __cplusplus
}
#endif
