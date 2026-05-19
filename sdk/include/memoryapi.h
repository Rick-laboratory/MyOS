#pragma once
/* myOS Win32 SDK - memoryapi.h */
#include "winnt.h"
#include "handleapi.h"
#ifdef __cplusplus
extern "C" {
#endif
HANDLE WINAPI CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
                                 DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName);
HANDLE WINAPI OpenFileMappingA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName);
LPVOID WINAPI MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                            DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, DWORD dwNumberOfBytesToMap);
BOOL   WINAPI UnmapViewOfFile(LPCVOID lpBaseAddress);
BOOL   WINAPI FlushViewOfFile(LPCVOID lpBaseAddress, DWORD dwNumberOfBytesToFlush);
#ifdef __cplusplus
}
#endif
#ifndef UNICODE
#define CreateFileMapping CreateFileMappingA
#define OpenFileMapping OpenFileMappingA
#endif
