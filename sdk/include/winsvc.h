#pragma once
/* myOS Win32 SDK - winsvc.h */
#include "windef.h"
#include "winnt.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef HANDLE SC_HANDLE;

#define SERVICE_STOPPED          1u
#define SERVICE_START_PENDING    2u
#define SERVICE_STOP_PENDING     3u
#define SERVICE_RUNNING          4u
#define SERVICE_PAUSED           7u

#define SERVICE_DEMAND_START     3u
#define SERVICE_AUTO_START       2u
#define SERVICE_ACCEPT_STOP      0x00000001u
#define SERVICE_ACCEPT_PAUSE_CONTINUE 0x00000002u
#define SERVICE_QUERY_STATUS     0x0004u
#define SERVICE_START            0x0010u
#define SERVICE_STOP             0x0020u
#define SERVICE_CHANGE_CONFIG    0x0002u
#define SERVICE_ALL_ACCESS       0x00ffu
#define SC_MANAGER_ALL_ACCESS    0x00ffu
#define SERVICE_CONTROL_STOP     1u
#define SERVICE_WIN32_OWN_PROCESS 0x00000010u
#define SERVICE_ERROR_NORMAL     1u

/* AUDIT(v118): Public SCM contract is still transitional. MSDN wants
   SERVICE_STATUS/SERVICE_STATUS_PROCESS here; MyServiceInfo must move back to
   diagnostics/private headers before foreign service code can compile cleanly. */
typedef struct MyServiceInfo {
    SC_HANDLE hService;
    DWORD ownerPid;
    DWORD state;
    DWORD startType;
    DWORD acceptedControls;
    DWORD flags;
    DWORD checkpoint;
    DWORD win32ExitCode;
    char  name[48];
    char  displayName[72];
    char  binaryPath[96];
} MyServiceInfo;

SC_HANDLE WINAPI OpenSCManagerA(LPCSTR lpMachineName, LPCSTR lpDatabaseName, DWORD dwDesiredAccess);
SC_HANDLE WINAPI CreateServiceA(SC_HANDLE hSCManager, LPCSTR lpServiceName, LPCSTR lpDisplayName,
                                DWORD dwDesiredAccess, DWORD dwServiceType, DWORD dwStartType,
                                DWORD dwErrorControl, LPCSTR lpBinaryPathName, LPCSTR lpLoadOrderGroup,
                                DWORD* lpdwTagId, LPCSTR lpDependencies, LPCSTR lpServiceStartName,
                                LPCSTR lpPassword);
SC_HANDLE WINAPI OpenServiceA(SC_HANDLE hSCManager, LPCSTR lpServiceName, DWORD dwDesiredAccess);
BOOL      WINAPI StartServiceA(SC_HANDLE hService, DWORD dwNumServiceArgs, LPCSTR* lpServiceArgVectors);
BOOL      WINAPI ControlService(SC_HANDLE hService, DWORD dwControl, MyServiceInfo* lpServiceStatus);
BOOL      WINAPI DeleteService(SC_HANDLE hService);
BOOL      WINAPI QueryServiceStatus(SC_HANDLE hService, MyServiceInfo* lpServiceStatus);
BOOL      WINAPI CloseServiceHandle(SC_HANDLE hSCObject);

#ifdef __cplusplus
}
#endif
#ifndef UNICODE
#define OpenSCManager OpenSCManagerA
#define CreateService CreateServiceA
#define OpenService OpenServiceA
#define StartService StartServiceA
#endif
