#include "myobject.h"
#include <string.h>
#include <pthread.h>
#include <stdio.h>

static _ObjectectInfo g_Objects[_OBJECT_MAX_OBJECTS];
static _ObjectSecurity g_ObjectSecurity[_OBJECT_MAX_OBJECTS];
static DWORD g_ObjectGeneration[_OBJECT_MAX_OBJECTS];
static DWORD g_ObjectLiveCount;
static DWORD g_ObjectTypeLiveCount[256];
static pthread_mutex_t g_ObjLock = PTHREAD_MUTEX_INITIALIZER;
static int g_ObjInitDone = 0;


static DWORD obj_index_from_type_slot(DWORD type, DWORD slot)
{
    if (type == _OBJECT_TYPE_NONE || type > 0xffu || slot >= _OBJECT_SLOT_STRIDE) return 0xffffffffu;
    DWORD idx = type * _OBJECT_SLOT_STRIDE + slot;
    return (idx < _OBJECT_MAX_OBJECTS) ? idx : 0xffffffffu;
}

HANDLE _ObjectMakeSlotHandle(DWORD dwType, DWORD dwSlot)
{
    DWORD idx = obj_index_from_type_slot(dwType, dwSlot);
    if (idx == 0xffffffffu) return 0;
    DWORD generation = __atomic_load_n(&g_ObjectGeneration[idx], __ATOMIC_ACQUIRE) & _OBJECT_SLOT_GENERATION_MASK;
    DWORD slotBits = (dwSlot + 1u) & _OBJECT_SLOT_HANDLE_SLOT_MASK;
    return (HANDLE)(_OBJECT_SLOT_HANDLE_TAG |
                    ((dwType & 0xffu) << _OBJECT_SLOT_HANDLE_TYPE_SHIFT) |
                    ((generation & _OBJECT_SLOT_GENERATION_MASK) << _OBJECT_SLOT_HANDLE_GENERATION_SHIFT) |
                    slotBits);
}

BOOL _ObjectDecodeObjectId(HANDLE hObject, DWORD* lpType, DWORD* lpSlot, DWORD* lpGeneration)
{
    DWORD v = (DWORD)hObject;
    if ((v & 0xff000000u) != _OBJECT_SLOT_HANDLE_TAG) return FALSE;
    DWORD type = (v >> _OBJECT_SLOT_HANDLE_TYPE_SHIFT) & 0xffu;
    DWORD rawSlot = v & _OBJECT_SLOT_HANDLE_SLOT_MASK;
    DWORD generation = (v & _OBJECT_SLOT_HANDLE_GENERATION_MASK) >> _OBJECT_SLOT_HANDLE_GENERATION_SHIFT;
    if (type == _OBJECT_TYPE_NONE || rawSlot == 0 || rawSlot > _OBJECT_SLOT_STRIDE) return FALSE;
    if (lpType) *lpType = type;
    if (lpSlot) *lpSlot = rawSlot - 1u;
    if (lpGeneration) *lpGeneration = generation;
    return TRUE;
}

BOOL _ObjectDecodeSlotHandle(HANDLE hObject, DWORD* lpType, DWORD* lpSlot)
{
    return _ObjectDecodeObjectId(hObject, lpType, lpSlot, NULL);
}

static int obj_direct_index_from_handle(HANDLE hObject)
{
    DWORD type = 0, slot = 0;
    if (!_ObjectDecodeSlotHandle(hObject, &type, &slot)) return -1;
    DWORD idx = obj_index_from_type_slot(type, slot);
    if (idx == 0xffffffffu) return -1;
    return (int)idx;
}

static void obj_clear_slot_locked(int i, DWORD finalState)
{
    if (i < 0 || i >= _OBJECT_MAX_OBJECTS) return;
    DWORD oldType = g_Objects[i].type;
    if (oldType != _OBJECT_TYPE_NONE) {
        if (g_ObjectLiveCount) g_ObjectLiveCount--;
        if (oldType < 256u && g_ObjectTypeLiveCount[oldType]) g_ObjectTypeLiveCount[oldType]--;
    }
    DWORD oldGeneration = g_Objects[i].object_generation;
    if (!oldGeneration) oldGeneration = __atomic_load_n(&g_ObjectGeneration[i], __ATOMIC_ACQUIRE) & _OBJECT_SLOT_GENERATION_MASK;
    DWORD nextGeneration = (oldGeneration + 1u) & _OBJECT_SLOT_GENERATION_MASK;
    __atomic_store_n(&g_ObjectGeneration[i], nextGeneration, __ATOMIC_RELEASE);
    memset(&g_Objects[i], 0, sizeof(g_Objects[i]));
    memset(&g_ObjectSecurity[i], 0, sizeof(g_ObjectSecurity[i]));
    g_Objects[i].object_slot = (DWORD)i;
    g_Objects[i].object_generation = nextGeneration;
    g_Objects[i].object_state = finalState;
}

static void obj_prepare_live_slot_locked(int i, HANDLE hObject, DWORD dwType, DWORD dwOwnerPid, DWORD dwAccessMask, DWORD dwSize, LPCSTR lpName)
{
    if (g_Objects[i].type == _OBJECT_TYPE_NONE) {
        g_ObjectLiveCount++;
        if (dwType < 256u) g_ObjectTypeLiveCount[dwType]++;
    }
    DWORD generation = __atomic_load_n(&g_ObjectGeneration[i], __ATOMIC_ACQUIRE) & _OBJECT_SLOT_GENERATION_MASK;
    memset(&g_Objects[i], 0, sizeof(g_Objects[i]));
    memset(&g_ObjectSecurity[i], 0, sizeof(g_ObjectSecurity[i]));
    g_Objects[i].handle = hObject;
    g_Objects[i].type = dwType;
    g_Objects[i].owner_pid = dwOwnerPid;
    g_Objects[i].access_mask = dwAccessMask;
    g_Objects[i].ref_count = 1;
    g_Objects[i].pointer_count = 1;
    g_Objects[i].handle_count = 0;
    g_Objects[i].object_slot = (DWORD)i;
    g_Objects[i].object_generation = generation;
    g_Objects[i].object_state = _OBJECT_OBJECT_STATE_LIVE;
    g_Objects[i].size = dwSize;
    g_Objects[i].sd_flags = _OBJECT_SD_DEFAULT;
    g_Objects[i].namespace_id = _OBJECT_NS_NONE;
    snprintf(g_Objects[i].name, sizeof(g_Objects[i].name), "%s", lpName ? lpName : "");
}

void _ObjectInit(void)
{
    pthread_mutex_lock(&g_ObjLock);
    if (!g_ObjInitDone) {
        memset(g_Objects, 0, sizeof(g_Objects));
        memset(g_ObjectSecurity, 0, sizeof(g_ObjectSecurity));
        memset(g_ObjectGeneration, 0, sizeof(g_ObjectGeneration));
        g_ObjectLiveCount = 0;
        memset(g_ObjectTypeLiveCount, 0, sizeof(g_ObjectTypeLiveCount));
        g_ObjInitDone = 1;
    }
    pthread_mutex_unlock(&g_ObjLock);
}

static int obj_find_locked(HANDLE hObject)
{
    if (!hObject) return -1;
    int direct = obj_direct_index_from_handle(hObject);
    if (direct >= 0) {
        if (g_Objects[direct].handle == hObject && g_Objects[direct].type != _OBJECT_TYPE_NONE) return direct;
        return -1;
    }
    for (int i = 0; i < _OBJECT_MAX_OBJECTS; i++) {
        if (g_Objects[i].handle == hObject && g_Objects[i].type != _OBJECT_TYPE_NONE)
            return i;
    }
    return -1;
}

BOOL _ObjectRegister(HANDLE hObject, DWORD dwType, DWORD dwOwnerPid, DWORD dwAccessMask, DWORD dwSize, LPCSTR lpName)
{
    if (!hObject || dwType == _OBJECT_TYPE_NONE) return FALSE;
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx >= 0) {
        if (g_Objects[idx].type != dwType) {
            fprintf(stderr, "[OBJ] Register collision: handle=0x%lx oldType=%lu newType=%lu\n",
                    (unsigned long)hObject,
                    (unsigned long)g_Objects[idx].type,
                    (unsigned long)dwType);
            pthread_mutex_unlock(&g_ObjLock);
            return FALSE;
        }
        DWORD refs = __atomic_add_fetch(&g_Objects[idx].ref_count, 1u, __ATOMIC_RELAXED);
        g_Objects[idx].pointer_count = refs;
        if (dwAccessMask) g_Objects[idx].access_mask |= dwAccessMask;
        if (dwSize) g_Objects[idx].size = dwSize;
        if (lpName && lpName[0]) snprintf(g_Objects[idx].name, sizeof(g_Objects[idx].name), "%s", lpName);
        pthread_mutex_unlock(&g_ObjLock);
        return TRUE;
    }
    int preferred = obj_direct_index_from_handle(hObject);
    int start = 0, end = _OBJECT_MAX_OBJECTS;
    if (preferred >= 0) { start = preferred; end = preferred + 1; }
    for (int i = start; i < end; i++) {
        if (g_Objects[i].type == _OBJECT_TYPE_NONE) {
            obj_prepare_live_slot_locked(i, hObject, dwType, dwOwnerPid, dwAccessMask, dwSize, lpName);
            pthread_mutex_unlock(&g_ObjLock);
            return TRUE;
        }
    }
    if (preferred >= 0) {
        pthread_mutex_unlock(&g_ObjLock);
        return FALSE;
    }
    for (int i = 0; i < _OBJECT_MAX_OBJECTS; i++) {
        if (g_Objects[i].type == _OBJECT_TYPE_NONE) {
            obj_prepare_live_slot_locked(i, hObject, dwType, dwOwnerPid, dwAccessMask, dwSize, lpName);
            pthread_mutex_unlock(&g_ObjLock);
            return TRUE;
        }
    }
    pthread_mutex_unlock(&g_ObjLock);
    return FALSE;
}

BOOL _ObjectUnregister(HANDLE hObject)
{
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx < 0) { pthread_mutex_unlock(&g_ObjLock); return FALSE; }
    obj_clear_slot_locked(idx, _OBJECT_OBJECT_STATE_ZOMBIE);
    pthread_mutex_unlock(&g_ObjLock);
    return TRUE;
}

static BOOL obj_addref_at_index(int i, HANDLE hObject)
{
    if (i < 0 || i >= _OBJECT_MAX_OBJECTS) return FALSE;
    if (g_Objects[i].handle != hObject || g_Objects[i].type == _OBJECT_TYPE_NONE) return FALSE;
    DWORD old = __atomic_load_n(&g_Objects[i].ref_count, __ATOMIC_ACQUIRE);
    while (old) {
        DWORD next = old + 1u;
        if (__atomic_compare_exchange_n(&g_Objects[i].ref_count, &old, next, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_Objects[i].pointer_count, next, __ATOMIC_RELEASE);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL _ObjectAddRef(HANDLE hObject)
{
    _ObjectInit();
    if (!hObject) return FALSE;
    int direct = obj_direct_index_from_handle(hObject);
    if (direct >= 0) return obj_addref_at_index(direct, hObject);
    for (int i = 0; i < _OBJECT_MAX_OBJECTS; i++) {
        if (obj_addref_at_index(i, hObject)) return TRUE;
    }
    return FALSE;
}

static BOOL obj_release_at_index(int i, HANDLE hObject)
{
    if (i < 0 || i >= _OBJECT_MAX_OBJECTS) return FALSE;
    if (g_Objects[i].handle != hObject || g_Objects[i].type == _OBJECT_TYPE_NONE) return FALSE;
    DWORD old = __atomic_load_n(&g_Objects[i].ref_count, __ATOMIC_ACQUIRE);
    while (old) {
        DWORD next = old - 1u;
        if (__atomic_compare_exchange_n(&g_Objects[i].ref_count, &old, next, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_Objects[i].pointer_count, next, __ATOMIC_RELEASE);
            if (next == 0) {
                pthread_mutex_lock(&g_ObjLock);
                if (g_Objects[i].handle == hObject && g_Objects[i].type != _OBJECT_TYPE_NONE &&
                    __atomic_load_n(&g_Objects[i].ref_count, __ATOMIC_ACQUIRE) == 0) {
                    g_Objects[i].object_state = _OBJECT_OBJECT_STATE_CLOSING;
                    obj_clear_slot_locked(i, _OBJECT_OBJECT_STATE_ZOMBIE);
                }
                pthread_mutex_unlock(&g_ObjLock);
            }
            return TRUE;
        }
    }
    return FALSE;
}

BOOL _ObjectRelease(HANDLE hObject)
{
    _ObjectInit();
    if (!hObject) return FALSE;
    int direct = obj_direct_index_from_handle(hObject);
    if (direct >= 0) return obj_release_at_index(direct, hObject);
    for (int i = 0; i < _OBJECT_MAX_OBJECTS; i++) {
        if (obj_release_at_index(i, hObject)) return TRUE;
    }
    return FALSE;
}

BOOL _ObjectReferenceHandle(HANDLE hObject)
{
    _ObjectInit();
    if (!hObject) return FALSE;
    int direct = obj_direct_index_from_handle(hObject);
    if (direct >= 0 && g_Objects[direct].handle == hObject && g_Objects[direct].type != _OBJECT_TYPE_NONE) {
        __atomic_add_fetch(&g_Objects[direct].handle_count, 1u, __ATOMIC_RELAXED);
        return TRUE;
    }
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx < 0) { pthread_mutex_unlock(&g_ObjLock); return FALSE; }
    g_Objects[idx].handle_count++;
    pthread_mutex_unlock(&g_ObjLock);
    return TRUE;
}

BOOL _ObjectDereferenceHandle(HANDLE hObject)
{
    _ObjectInit();
    if (!hObject) return FALSE;
    int direct = obj_direct_index_from_handle(hObject);
    if (direct >= 0 && g_Objects[direct].handle == hObject && g_Objects[direct].type != _OBJECT_TYPE_NONE) {
        DWORD old = __atomic_load_n(&g_Objects[direct].handle_count, __ATOMIC_ACQUIRE);
        while (old) {
            DWORD next = old - 1u;
            if (__atomic_compare_exchange_n(&g_Objects[direct].handle_count, &old, next, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return TRUE;
        }
        return TRUE;
    }
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx >= 0 && g_Objects[idx].handle_count) g_Objects[idx].handle_count--;
    pthread_mutex_unlock(&g_ObjLock);
    return idx >= 0;
}


BOOL _ObjectSetInfo(HANDLE hObject, DWORD dwFlags, DWORD dwSize, LPCSTR lpName)
{
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx < 0) { pthread_mutex_unlock(&g_ObjLock); return FALSE; }
    g_Objects[idx].flags = dwFlags;
    if (dwSize) g_Objects[idx].size = dwSize;
    if (lpName) snprintf(g_Objects[idx].name, sizeof(g_Objects[idx].name), "%s", lpName);
    pthread_mutex_unlock(&g_ObjLock);
    return TRUE;
}

BOOL _ObjectSetSecurity(HANDLE hObject, DWORD dwSdFlags, DWORD dwNamespaceId)
{
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx < 0) { pthread_mutex_unlock(&g_ObjLock); return FALSE; }
    g_Objects[idx].sd_flags = dwSdFlags;
    g_Objects[idx].namespace_id = dwNamespaceId;
    g_Objects[idx].sd_control = 0;
    g_Objects[idx].dacl_present = 0;
    g_Objects[idx].dacl_null = 0;
    g_Objects[idx].ace_count = 0;
    memset(&g_ObjectSecurity[idx], 0, sizeof(g_ObjectSecurity[idx]));
    pthread_mutex_unlock(&g_ObjLock);
    return TRUE;
}

static BOOL myobj_sid_equal(const _ObjectSid* a, const _ObjectSid* b)
{
    if (!a || !b) return FALSE;
    if (a->revision != b->revision || a->subauth_count != b->subauth_count) return FALSE;
    if (memcmp(a->authority, b->authority, sizeof(a->authority)) != 0) return FALSE;
    for (DWORD i = 0; i < a->subauth_count && i < _OBJECT_SECURITY_MAX_SUBAUTH; ++i)
        if (a->subauth[i] != b->subauth[i]) return FALSE;
    return TRUE;
}

static BOOL myobj_token_has_sid(const _ObjectToken* token, const _ObjectSid* sid)
{
    if (!token || !sid) return FALSE;
    if (myobj_sid_equal(&token->user, sid)) return TRUE;
    for (DWORD i = 0; i < token->group_count && i < 8; ++i)
        if (myobj_sid_equal(&token->groups[i], sid)) return TRUE;
    return FALSE;
}

BOOL _ObjectSetSecurityDescriptor(HANDLE hObject, const _ObjectSecurity* lpSecurity)
{
    if (!lpSecurity || !lpSecurity->valid || lpSecurity->ace_count > _OBJECT_SECURITY_MAX_ACES) return FALSE;
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx < 0) { pthread_mutex_unlock(&g_ObjLock); return FALSE; }
    g_ObjectSecurity[idx] = *lpSecurity;
    g_ObjectSecurity[idx].valid = 1;
    g_Objects[idx].sd_control = lpSecurity->control;
    g_Objects[idx].namespace_id = lpSecurity->namespace_id;
    g_Objects[idx].dacl_present = lpSecurity->dacl_present ? 1u : 0u;
    g_Objects[idx].dacl_null = lpSecurity->dacl_null ? 1u : 0u;
    g_Objects[idx].ace_count = lpSecurity->ace_count;
    g_Objects[idx].sd_flags = 0;
    pthread_mutex_unlock(&g_ObjLock);
    return TRUE;
}

BOOL _ObjectGetSecurityDescriptor(HANDLE hObject, _ObjectSecurity* lpSecurity)
{
    if (!lpSecurity) return FALSE;
    memset(lpSecurity, 0, sizeof(*lpSecurity));
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx < 0 || !g_ObjectSecurity[idx].valid) { pthread_mutex_unlock(&g_ObjLock); return FALSE; }
    *lpSecurity = g_ObjectSecurity[idx];
    pthread_mutex_unlock(&g_ObjLock);
    return TRUE;
}

BOOL _ObjectAccessCheck(HANDLE hObject, const _ObjectToken* lpToken, DWORD dwDesiredAccess, DWORD dwOwnerImplicitAccess)
{
    if (dwDesiredAccess == 0) return TRUE;
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx < 0 || !g_ObjectSecurity[idx].valid) { pthread_mutex_unlock(&g_ObjLock); return FALSE; }
    _ObjectSecurity sec = g_ObjectSecurity[idx];
    pthread_mutex_unlock(&g_ObjLock);

    if (!lpToken) return FALSE;
    if (lpToken->is_admin) return TRUE;

    DWORD wanted = dwDesiredAccess;
    DWORD granted = 0;
    if (dwOwnerImplicitAccess && myobj_sid_equal(&lpToken->user, &sec.owner))
        granted |= (wanted & dwOwnerImplicitAccess);

    if (!sec.dacl_present || sec.dacl_null) return TRUE;

    for (DWORD i = 0; i < sec.ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i) {
        const _ObjectAce* ace = &sec.aces[i];
        if (ace->flags & _OBJECT_ACE_FLAG_INHERIT_ONLY) continue;
        if (ace->type != _OBJECT_ACE_DENY) continue;
        if (!myobj_token_has_sid(lpToken, &ace->sid)) continue;
        DWORD stillWanted = wanted & ~granted;
        if ((ace->mask & stillWanted) != 0) return FALSE;
    }

    for (DWORD i = 0; i < sec.ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i) {
        const _ObjectAce* ace = &sec.aces[i];
        if (ace->flags & _OBJECT_ACE_FLAG_INHERIT_ONLY) continue;
        if (ace->type != _OBJECT_ACE_ALLOW) continue;
        if (!myobj_token_has_sid(lpToken, &ace->sid)) continue;
        granted |= (ace->mask & wanted);
    }

    return ((granted & wanted) == wanted) ? TRUE : FALSE;
}

BOOL _ObjectGetInfo(HANDLE hObject, _ObjectectInfo* lpInfo)
{
    if (!lpInfo) return FALSE;
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    int idx = obj_find_locked(hObject);
    if (idx < 0) { pthread_mutex_unlock(&g_ObjLock); memset(lpInfo, 0, sizeof(*lpInfo)); return FALSE; }
    *lpInfo = g_Objects[idx];
    if (g_ObjectSecurity[idx].valid) {
        lpInfo->sd_control = g_ObjectSecurity[idx].control;
        lpInfo->dacl_present = g_ObjectSecurity[idx].dacl_present ? 1u : 0u;
        lpInfo->dacl_null = g_ObjectSecurity[idx].dacl_null ? 1u : 0u;
        lpInfo->ace_count = g_ObjectSecurity[idx].ace_count;
    }
    pthread_mutex_unlock(&g_ObjLock);
    return TRUE;
}

BOOL _ObjectQueryObjectHeader(HANDLE hObject, _ObjectectHeader* lpHeader)
{
    if (!lpHeader) return FALSE;
    memset(lpHeader, 0, sizeof(*lpHeader));
    _ObjectectInfo oi;
    if (!_ObjectGetInfo(hObject, &oi)) return FALSE;
    lpHeader->object_id = oi.handle;
    lpHeader->object_type = oi.type;
    lpHeader->object_slot = oi.object_slot;
    lpHeader->object_generation = oi.object_generation;
    lpHeader->object_state = oi.object_state;
    lpHeader->pointer_count = oi.pointer_count ? oi.pointer_count : oi.ref_count;
    lpHeader->handle_count = oi.handle_count;
    lpHeader->granted_access = oi.access_mask;
    return TRUE;
}

DWORD _ObjectGetCount(void)
{
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    DWORD n = g_ObjectLiveCount;
    pthread_mutex_unlock(&g_ObjLock);
    return n;
}

DWORD _ObjectGetCountByType(DWORD dwType)
{
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    DWORD n = (dwType < 256u) ? g_ObjectTypeLiveCount[dwType] : 0;
    pthread_mutex_unlock(&g_ObjLock);
    return n;
}

BOOL _ObjectEnumObjects(MYOBJECTENUMPROC lpEnumFunc, LPARAM lParam)
{
    if (!lpEnumFunc) return FALSE;
    _ObjectectInfo snap[_OBJECT_MAX_OBJECTS];
    int n = 0;
    _ObjectInit();
    pthread_mutex_lock(&g_ObjLock);
    for (int i = 0; i < _OBJECT_MAX_OBJECTS; i++) if (g_Objects[i].type != _OBJECT_TYPE_NONE && n < _OBJECT_MAX_OBJECTS) snap[n++] = g_Objects[i];
    pthread_mutex_unlock(&g_ObjLock);
    for (int i = 0; i < n; i++) {
        if (!lpEnumFunc(&snap[i], lParam)) break;
    }
    return TRUE;
}

const char* _ObjectTypeName(DWORD dwType)
{
    switch (dwType) {
    case _OBJECT_TYPE_HWND: return "HWND";
    case _OBJECT_TYPE_SECTION: return "SECTION";
    case _OBJECT_TYPE_THREAD: return "THREAD";
    case _OBJECT_TYPE_PROCESS: return "PROCESS";
    case _OBJECT_TYPE_EVENT: return "EVENT";
    case _OBJECT_TYPE_TIMER: return "TIMER";
    case _OBJECT_TYPE_MUTEX: return "MUTEX";
    case _OBJECT_TYPE_SEMAPHORE: return "SEMAPHORE";
    case _OBJECT_TYPE_DC: return "DC";
    case _OBJECT_TYPE_BRUSH: return "BRUSH";
    case _OBJECT_TYPE_BITMAP: return "BITMAP";
    case _OBJECT_TYPE_PEN: return "PEN";
    case _OBJECT_TYPE_FONT: return "FONT";
    case _OBJECT_TYPE_SERVICE: return "SERVICE";
    case _OBJECT_TYPE_SESSION: return "SESSION";
    case _OBJECT_TYPE_REGION: return "REGION";
    case _OBJECT_TYPE_TOKEN: return "TOKEN";
    default: return "?";
    }
}
