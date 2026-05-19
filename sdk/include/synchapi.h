#pragma once
/* myOS Win32 SDK - synchapi.h */
#include "winnt.h"
#include "handleapi.h"
#ifdef __cplusplus
extern "C" {
#endif
HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName);
HANDLE WINAPI OpenEventA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName);
BOOL   WINAPI SetEvent(HANDLE hEvent);
BOOL   WINAPI ResetEvent(HANDLE hEvent);
HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
HANDLE WINAPI OpenMutexA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName);
BOOL   WINAPI ReleaseMutex(HANDLE hMutex);
HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount, LPCSTR lpName);
HANDLE WINAPI OpenSemaphoreA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName);
BOOL   WINAPI ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LONG* lpPreviousCount);
HANDLE WINAPI CreateWaitableTimerA(LPSECURITY_ATTRIBUTES lpTimerAttributes, BOOL bManualReset, LPCSTR lpTimerName);
BOOL   WINAPI SetWaitableTimer(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod,
                               LPVOID pfnCompletionRoutine, LPVOID lpArgToCompletionRoutine, BOOL fResume);
BOOL   WINAPI CancelWaitableTimer(HANDLE hTimer);
DWORD  WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD  WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
#ifdef __cplusplus
}
#endif
#ifndef UNICODE
#define CreateEvent CreateEventA
#define OpenEvent OpenEventA
#define CreateMutex CreateMutexA
#define OpenMutex OpenMutexA
#define CreateSemaphore CreateSemaphoreA
#define OpenSemaphore OpenSemaphoreA
#define CreateWaitableTimer CreateWaitableTimerA
#endif
