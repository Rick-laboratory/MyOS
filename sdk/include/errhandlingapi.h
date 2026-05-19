#pragma once
/* myOS Win32 SDK - errhandlingapi.h */
#include "winnt.h"
#include "winerror.h"
#ifdef __cplusplus
extern "C" {
#endif
DWORD WINAPI GetLastError(void);
void  WINAPI SetLastError(DWORD dwErrCode);
#ifdef __cplusplus
}
#endif
