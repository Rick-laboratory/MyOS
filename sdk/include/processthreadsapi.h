#pragma once
/* myOS Win32 SDK - processthreadsapi.h */
#include "winnt.h"
#include "handleapi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define STARTF_USESHOWWINDOW 0x00000001u
#define STARTF_USESIZE       0x00000002u
#define STARTF_USEPOSITION   0x00000004u
#define STARTF_USESTDHANDLES 0x00000100u

typedef struct STARTUPINFOA {
    DWORD cb;
    LPSTR lpReserved;
    LPSTR lpDesktop;
    LPSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwFlags;
    WORD  wShowWindow;
    WORD  cbReserved2;
    LPVOID lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;

typedef struct PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD  dwProcessId;
    DWORD  dwThreadId;
} PROCESS_INFORMATION, *LPPROCESS_INFORMATION;

DWORD WINAPI GetCurrentProcessId(void);
DWORD WINAPI GetCurrentThreadId(void);
HANDLE WINAPI OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId);
HANDLE WINAPI OpenThread(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId);
BOOL   WINAPI OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, HANDLE* TokenHandle);
BOOL   WINAPI GetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, LPDWORD ReturnLength);
BOOL  WINAPI CreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine,
                            LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                            BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
                            LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
                            LPPROCESS_INFORMATION lpProcessInformation);
void  WINAPI ExitProcess(UINT uExitCode);
BOOL  WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode);
BOOL  WINAPI GetExitCodeProcess(HANDLE hProcess, DWORD* lpExitCode);
void  WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo);
LPSTR WINAPI GetCommandLineA(void);
#ifdef __cplusplus
}
#endif
#ifndef UNICODE
#define CreateProcess CreateProcessA
#define GetStartupInfo GetStartupInfoA
#define GetCommandLine GetCommandLineA
#endif
