#pragma once
/* myOS Win32 SDK - handleapi.h */
#include "winnt.h"
#include "winerror.h"
#ifdef __cplusplus
extern "C" {
#endif
BOOL   WINAPI CloseHandle(HANDLE hObject);
BOOL   WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle,
                              HANDLE hTargetProcessHandle, HANDLE* lpTargetHandle,
                              DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions);
BOOL   WINAPI GetHandleInformation(HANDLE hObject, LPDWORD lpdwFlags);
BOOL   WINAPI SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags);
HANDLE WINAPI GetCurrentProcess(void);
HANDLE WINAPI GetCurrentThread(void);
#ifdef __cplusplus
}
#endif
