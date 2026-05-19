#pragma once
/* myOS Win32 SDK - shellapi.h */
#include "windef.h"
#include "winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SEE_MASK_NOCLOSEPROCESS 0x00000040u
#define SE_ERR_FNF              2u
#define SE_ERR_ACCESSDENIED     5u
#define SE_ERR_NOASSOC          31u
#define SE_ERR_DLLNOTFOUND      32u
#define MYOS_SHELLEXECUTE_SUCCESS ((HINSTANCE)33u)

typedef struct _SHELLEXECUTEINFOA {
    DWORD     cbSize;
    DWORD     fMask;
    HWND      hwnd;
    LPCSTR    lpVerb;
    LPCSTR    lpFile;
    LPCSTR    lpParameters;
    LPCSTR    lpDirectory;
    int       nShow;
    HINSTANCE hInstApp;
    HANDLE    hProcess;
} SHELLEXECUTEINFOA, *LPSHELLEXECUTEINFOA;

HINSTANCE WINAPI ShellExecuteA(HWND hwnd, LPCSTR lpOperation, LPCSTR lpFile,
                               LPCSTR lpParameters, LPCSTR lpDirectory, int nShowCmd);
BOOL      WINAPI ShellExecuteExA(LPSHELLEXECUTEINFOA lpExecInfo);

#ifdef __cplusplus
}
#endif
#ifndef UNICODE
#define SHELLEXECUTEINFO SHELLEXECUTEINFOA
#define ShellExecute ShellExecuteA
#define ShellExecuteEx ShellExecuteExA
#endif
