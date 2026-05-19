#pragma once
#include "mytypes.h"
#include <stddef.h>

/* AUDIT(v118): Global object registry quota. When strict handles/object namespace
   grow, this should become a dynamic object table with explicit quota errors. */
#define _OBJECT_MAX_OBJECTS 2048

/* v239: 96 slots/type keeps slot-coded direct lookup for the current
   largest fixed USER/GDI tables (bitmaps/regions) while staying inside
   the existing 7-bit public object-slot field. */
#define _OBJECT_SLOT_STRIDE 96u
#define _OBJECT_SLOT_HANDLE_TAG 0x4f000000u
#define _OBJECT_SLOT_HANDLE_TYPE_SHIFT 16u
#define _OBJECT_SLOT_HANDLE_SLOT_MASK 0x0000007fu
#define _OBJECT_SLOT_HANDLE_GENERATION_SHIFT 7u
#define _OBJECT_SLOT_HANDLE_GENERATION_MASK 0x0000ff80u
#define _OBJECT_SLOT_GENERATION_MASK 0x000001ffu

#define _OBJECT_OBJECT_STATE_FREE           0u
#define _OBJECT_OBJECT_STATE_RESERVED       1u
#define _OBJECT_OBJECT_STATE_PENDING_CREATE 2u
#define _OBJECT_OBJECT_STATE_LIVE           3u
#define _OBJECT_OBJECT_STATE_CLOSING        4u
#define _OBJECT_OBJECT_STATE_ZOMBIE         5u

#define _OBJECT_TYPE_NONE     0u
#define _OBJECT_TYPE_HWND     1u
#define _OBJECT_TYPE_SECTION  2u
#define _OBJECT_TYPE_THREAD   3u
#define _OBJECT_TYPE_PROCESS  4u
#define _OBJECT_TYPE_EVENT    5u
#define _OBJECT_TYPE_TIMER    6u
#define _OBJECT_TYPE_MUTEX    7u
#define _OBJECT_TYPE_SEMAPHORE 8u
#define _OBJECT_TYPE_DC       9u
#define _OBJECT_TYPE_BRUSH    10u
#define _OBJECT_TYPE_BITMAP   11u
#define _OBJECT_TYPE_PEN      12u
#define _OBJECT_TYPE_FONT     13u
#define _OBJECT_TYPE_SERVICE  14u
#define _OBJECT_TYPE_SESSION  15u
#define _OBJECT_TYPE_REGION   16u
#define _OBJECT_TYPE_TOKEN    17u

#define _OBJECT_SLOT_TABLE_FITS ((_OBJECT_TYPE_TOKEN * _OBJECT_SLOT_STRIDE + (_OBJECT_SLOT_STRIDE - 1u)) < _OBJECT_MAX_OBJECTS)
#if !_OBJECT_SLOT_TABLE_FITS
#error "_OBJECT_MAX_OBJECTS too small for slot-coded object table"
#endif

#define _OBJECT_ACCESS_READ       0x00000001u
#define _OBJECT_ACCESS_WRITE      0x00000002u
#define _OBJECT_ACCESS_CONTROL    0x00000004u
#define _OBJECT_ACCESS_MAP        0x00000008u
#define _OBJECT_ACCESS_SIGNAL     0x00000010u
#define _OBJECT_ACCESS_ALL        0xffffffffu

#define _OBJECT_FLAG_EVENT_SIGNALED     0x00000001u
#define _OBJECT_FLAG_EVENT_MANUAL_RESET 0x00000002u
#define _OBJECT_FLAG_MUTEX_OWNED       0x00000001u
#define _OBJECT_FLAG_MUTEX_ABANDONED   0x00000002u
#define _OBJECT_FLAG_TIMER_SIGNALED    0x00000001u
#define _OBJECT_FLAG_TIMER_MANUAL      0x00000002u
#define _OBJECT_FLAG_TIMER_PERIODIC    0x00000004u
#define _OBJECT_FLAG_GDI_SELECTED      0x00000001u
#define _OBJECT_FLAG_GDI_DIBSECTION    0x00000002u
#define _OBJECT_FLAG_DC_PAINT          0x00000001u
#define _OBJECT_FLAG_PROCESS_EXITED     0x00000001u
#define _OBJECT_FLAG_THREAD_EXITED      0x00000001u

#define _OBJECT_SD_OWNER_ONLY 0x00000001u
#define _OBJECT_SD_PUBLIC_READ 0x00000002u
#define _OBJECT_SD_PUBLIC_WRITE 0x00000004u
#define _OBJECT_SD_ADMIN_ONLY 0x00000008u
#define _OBJECT_SD_DEFAULT (_OBJECT_SD_PUBLIC_READ|_OBJECT_SD_PUBLIC_WRITE)

#define _OBJECT_NS_NONE    0u
#define _OBJECT_NS_LOCAL   1u
#define _OBJECT_NS_GLOBAL  2u
#define _OBJECT_NS_SESSION 3u

#define _OBJECT_SECURITY_MAX_SUBAUTH 8u
#define _OBJECT_SECURITY_MAX_ACES    16u
#define _OBJECT_ACE_ALLOW            0u
#define _OBJECT_ACE_DENY             1u
#define _OBJECT_ACE_FLAG_INHERIT_ONLY 0x08u
#define _OBJECT_ACE_FLAG_INHERITED    0x10u

typedef struct _ObjectSid {
    BYTE  revision;
    BYTE  subauth_count;
    BYTE  authority[6];
    DWORD subauth[_OBJECT_SECURITY_MAX_SUBAUTH];
} _ObjectSid;

typedef struct _ObjectAce {
    DWORD type;
    DWORD flags;
    DWORD mask;
    _ObjectSid sid;
} _ObjectAce;

typedef struct _ObjectSecurity {
    DWORD valid;
    DWORD control;
    DWORD namespace_id;
    BOOL  dacl_present;
    BOOL  dacl_null;
    DWORD ace_count;
    _ObjectSid owner;
    _ObjectAce aces[_OBJECT_SECURITY_MAX_ACES];
} _ObjectSecurity;

typedef struct _ObjectToken {
    _ObjectSid user;
    _ObjectSid groups[8];
    DWORD group_count;
    BOOL is_admin;
} _ObjectToken;

typedef struct _ObjectectHeader {
    HANDLE object_id;
    DWORD  object_type;
    DWORD  object_slot;
    DWORD  object_generation;
    DWORD  object_state;
    DWORD  pointer_count;
    DWORD  handle_count;
    DWORD  granted_access;
} _ObjectectHeader;

typedef struct _ObjectectInfo {
    HANDLE handle;
    DWORD  type;
    DWORD  owner_pid;
    DWORD  access_mask;
    DWORD  ref_count;
    DWORD  object_slot;
    DWORD  object_generation;
    DWORD  object_state;
    DWORD  pointer_count;
    DWORD  handle_count;
    DWORD  flags;
    DWORD  size;
    char   name[96];
    DWORD  sd_flags;
    DWORD  namespace_id;
    DWORD  sd_control;
    DWORD  dacl_present;
    DWORD  dacl_null;
    DWORD  ace_count;
} _ObjectectInfo;

typedef BOOL (*MYOBJECTENUMPROC)(const _ObjectectInfo* lpInfo, LPARAM lParam);

void  _ObjectInit(void);

HANDLE _ObjectMakeSlotHandle(DWORD dwType, DWORD dwSlot);
BOOL   _ObjectDecodeSlotHandle(HANDLE hObject, DWORD* lpType, DWORD* lpSlot);
BOOL   _ObjectDecodeObjectId(HANDLE hObject, DWORD* lpType, DWORD* lpSlot, DWORD* lpGeneration);
BOOL  _ObjectRegister(HANDLE hObject, DWORD dwType, DWORD dwOwnerPid, DWORD dwAccessMask, DWORD dwSize, LPCSTR lpName);
BOOL  _ObjectUnregister(HANDLE hObject);
BOOL  _ObjectAddRef(HANDLE hObject);
BOOL  _ObjectRelease(HANDLE hObject);
BOOL  _ObjectSetInfo(HANDLE hObject, DWORD dwFlags, DWORD dwSize, LPCSTR lpName);
BOOL  _ObjectSetSecurity(HANDLE hObject, DWORD dwSdFlags, DWORD dwNamespaceId);
BOOL  _ObjectSetSecurityDescriptor(HANDLE hObject, const _ObjectSecurity* lpSecurity);
BOOL  _ObjectGetSecurityDescriptor(HANDLE hObject, _ObjectSecurity* lpSecurity);
BOOL  _ObjectAccessCheck(HANDLE hObject, const _ObjectToken* lpToken, DWORD dwDesiredAccess, DWORD dwOwnerImplicitAccess);
BOOL  _ObjectGetInfo(HANDLE hObject, _ObjectectInfo* lpInfo);
BOOL  _ObjectQueryObjectHeader(HANDLE hObject, _ObjectectHeader* lpHeader);
BOOL  _ObjectReferenceHandle(HANDLE hObject);
BOOL  _ObjectDereferenceHandle(HANDLE hObject);
DWORD _ObjectGetCount(void);
DWORD _ObjectGetCountByType(DWORD dwType);
BOOL  _ObjectEnumObjects(MYOBJECTENUMPROC lpEnumFunc, LPARAM lParam);
const char* _ObjectTypeName(DWORD dwType);
