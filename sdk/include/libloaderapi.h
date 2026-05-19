#pragma once
/* myOS Win32 SDK - libloaderapi.h */
#include "winnt.h"
#ifdef __cplusplus
extern "C" {
#endif
HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName);
DWORD   WINAPI GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);
HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName);
BOOL    WINAPI FreeLibrary(HMODULE hLibModule);
FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);

HRSRC   WINAPI FindResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType);
HGLOBAL WINAPI LoadResource(HMODULE hModule, HRSRC hResInfo);
LPVOID  WINAPI LockResource(HGLOBAL hResData);
DWORD   WINAPI SizeofResource(HMODULE hModule, HRSRC hResInfo);
BOOL    WINAPI SetDllDirectoryA(LPCSTR lpPathName);
DWORD   WINAPI GetDllDirectoryA(DWORD nBufferLength, LPSTR lpBuffer);
#ifdef __cplusplus
}
#endif
#ifndef UNICODE
#define GetModuleHandle GetModuleHandleA
#define GetModuleFileName GetModuleFileNameA
#define LoadLibrary LoadLibraryA
#define SetDllDirectory SetDllDirectoryA
#define GetDllDirectory GetDllDirectoryA
#endif
