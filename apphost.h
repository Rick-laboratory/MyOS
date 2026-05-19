#pragma once

#include "mytypes.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "capability.h"

struct WindowManager;
typedef struct WindowManager WindowManager;

typedef struct MyAppLaunchResult {
    int    window_index;
    DWORD  process_id;
    DWORD  thread_id;
    HANDLE hProcess;
    HANDLE hThread;
    char   image_name[64];
    char   title[64];
} MyAppLaunchResult;

typedef struct MyAppLaunchOptions {
    LPCSTR lpImageName;
    LPCSTR lpTitle;
    LPCSTR lpPath;
    LPCSTR lpParameters;
    LPCSTR lpDirectory;
    int    x;
    int    y;
    int    nShowCmd;
    LPSTARTUPINFOA lpStartupInfo;
} MyAppLaunchOptions;

// v46: Loader/AppHost-lite.  Starts a registered GUI image through the
// PROCESS/THREAD-lite path, enters the child runtime context, creates its
// top-level HWND, and attaches process metadata to the desktop Window slot.
BOOL MyAppHostLaunch(WindowManager* wm,
                     LPCSTR lpImageName,
                     int x,
                     int y,
                     LPCSTR lpTitle,
                     LPCSTR lpPath,
                     MyAppLaunchResult* lpResult);

// v48: extended internal loader entry.  Keeps MyAppHostLaunch() stable, but
// carries public WinAPI startup metadata from ShellExecuteEx/CreateProcess-style
// callers into Process-Lite and the desktop frame.
BOOL MyAppHostLaunchEx(WindowManager* wm,
                       const MyAppLaunchOptions* lpOptions,
                       MyAppLaunchResult* lpResult);

BOOL MyAppHostIsRegistered(LPCSTR lpImageName);

// v47+: binds the internal desktop shell used by public ShellExecuteA/Ex.
BOOL MyAppHostBindShell(WindowManager* wm);
