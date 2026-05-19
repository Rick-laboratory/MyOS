#pragma once
/* myOS Win32 SDK - winbase.h */
#include "winnt.h"
#include "winerror.h"
#include "errhandlingapi.h"
#include "handleapi.h"
#include "synchapi.h"
#include "memoryapi.h"
#include "processthreadsapi.h"
#include "libloaderapi.h"
#ifdef __cplusplus
extern "C" {
#endif
DWORD WINAPI GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer);
BOOL  WINAPI SetCurrentDirectoryA(LPCSTR lpPathName);
DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
BOOL  WINAPI SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue);
DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize);
HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
BOOL   WINAPI SetStdHandle(DWORD nStdHandle, HANDLE hHandle);

BOOL  WINAPI InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR pSecurityDescriptor, DWORD dwRevision);
BOOL  WINAPI IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR pSecurityDescriptor);
DWORD WINAPI GetSecurityDescriptorLength(PSECURITY_DESCRIPTOR pSecurityDescriptor);
BOOL  WINAPI GetSecurityDescriptorControl(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSECURITY_DESCRIPTOR_CONTROL pControl, LPDWORD lpdwRevision);
BOOL  WINAPI SetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID pOwner, BOOL bOwnerDefaulted);
BOOL  WINAPI GetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID* pOwner, LPBOOL lpbOwnerDefaulted);
BOOL  WINAPI SetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID pGroup, BOOL bGroupDefaulted);
BOOL  WINAPI GetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID* pGroup, LPBOOL lpbGroupDefaulted);
BOOL  WINAPI SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, BOOL bDaclPresent, PACL pDacl, BOOL bDaclDefaulted);
BOOL  WINAPI GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, LPBOOL lpbDaclPresent, PACL* pDacl, LPBOOL lpbDaclDefaulted);
BOOL  WINAPI MakeSelfRelativeSD(PSECURITY_DESCRIPTOR pAbsoluteSecurityDescriptor, PSECURITY_DESCRIPTOR pSelfRelativeSecurityDescriptor, LPDWORD lpdwBufferLength);
BOOL  WINAPI MakeAbsoluteSD(PSECURITY_DESCRIPTOR pSelfRelativeSecurityDescriptor, PSECURITY_DESCRIPTOR pAbsoluteSecurityDescriptor, LPDWORD lpdwAbsoluteSecurityDescriptorSize, PACL pDacl, LPDWORD lpdwDaclSize, PACL pSacl, LPDWORD lpdwSaclSize, PSID pOwner, LPDWORD lpdwOwnerSize, PSID pPrimaryGroup, LPDWORD lpdwPrimaryGroupSize);
void  WINAPI MapGenericMask(PDWORD AccessMask, PGENERIC_MAPPING GenericMapping);
BOOL  WINAPI AccessCheck(PSECURITY_DESCRIPTOR pSecurityDescriptor, HANDLE ClientToken, DWORD DesiredAccess, PGENERIC_MAPPING GenericMapping, PPRIVILEGE_SET PrivilegeSet, LPDWORD PrivilegeSetLength, LPDWORD GrantedAccess, LPBOOL AccessStatus);
BOOL  WINAPI CheckTokenMembership(HANDLE TokenHandle, PSID SidToCheck, LPBOOL IsMember);
BOOL  WINAPI PrivilegeCheck(HANDLE ClientToken, PPRIVILEGE_SET RequiredPrivileges, LPBOOL pfResult);
BOOL  WINAPI GetKernelObjectSecurity(HANDLE Handle, SECURITY_INFORMATION RequestedInformation, PSECURITY_DESCRIPTOR pSecurityDescriptor, DWORD nLength, LPDWORD lpnLengthNeeded);
BOOL  WINAPI SetKernelObjectSecurity(HANDLE Handle, SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor);
BOOL  WINAPI InitializeAcl(PACL pAcl, DWORD nAclLength, DWORD dwAclRevision);
BOOL  WINAPI AddAccessAllowedAce(PACL pAcl, DWORD dwAceRevision, DWORD AccessMask, PSID pSid);
BOOL  WINAPI AddAccessDeniedAce(PACL pAcl, DWORD dwAceRevision, DWORD AccessMask, PSID pSid);
BOOL  WINAPI IsValidSid(PSID pSid);
DWORD WINAPI GetLengthSid(PSID pSid);
BOOL  WINAPI EqualSid(PSID pSid1, PSID pSid2);
BOOL  WINAPI CopySid(DWORD nDestinationSidLength, PSID pDestinationSid, PSID pSourceSid);
BOOL  WINAPI InitializeSid(PSID Sid, PSID_IDENTIFIER_AUTHORITY pIdentifierAuthority, BYTE nSubAuthorityCount);
PDWORD WINAPI GetSidSubAuthority(PSID pSid, DWORD nSubAuthority);

#define GMEM_MOVEABLE 0x0002u
HGLOBAL WINAPI GlobalAlloc(UINT uFlags, DWORD dwBytes);
LPVOID  WINAPI GlobalLock(HGLOBAL hMem);
BOOL    WINAPI GlobalUnlock(HGLOBAL hMem);
HGLOBAL WINAPI GlobalFree(HGLOBAL hMem);
#ifdef __cplusplus
}
#endif
#ifndef UNICODE
#define GetCurrentDirectory GetCurrentDirectoryA
#define SetCurrentDirectory SetCurrentDirectoryA
#define GetEnvironmentVariable GetEnvironmentVariableA
#define SetEnvironmentVariable SetEnvironmentVariableA
#define ExpandEnvironmentStrings ExpandEnvironmentStringsA
#endif
