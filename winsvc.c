#include <winsvc.h>
#include "myos_private.h"
#include "myobject.h"
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>

#ifndef SERVICE_CONTROL_STOP
#define SERVICE_CONTROL_STOP 1u
#endif

/* AUDIT(v118): SCM is a synchronous registry/status table, not a service process
   dispatcher yet. Public contract still needs SERVICE_STATUS and dispatcher APIs. */
#define SCM_HANDLE_VALUE ((SC_HANDLE)(_OBJECT_SLOT_HANDLE_TAG | ((_OBJECT_TYPE_SERVICE & 0xffu) << _OBJECT_SLOT_HANDLE_TYPE_SHIFT) | 1u))
#define MYSVC_NAME_HASH_BUCKETS 128
#define MYSVC_NAME_HASH_MASK (MYSVC_NAME_HASH_BUCKETS - 1)

typedef struct MyServiceEntry {
    int valid;
    MyServiceInfo info;
    DWORD refCount;
    DWORD grantedAccess;
    DWORD nameHash;
    int nameHashNext;
} MyServiceEntry;

static pthread_mutex_t g_svc_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_svc_init = 0;
static MyServiceEntry g_svc[MYSVC_MAX_SERVICES];
static int g_svc_name_hash[MYSVC_NAME_HASH_BUCKETS];
static int g_svc_free_stack[MYSVC_MAX_SERVICES];
static int g_svc_free_top = 0;
/* v216: SCM/service objects use _OBJECT_TYPE_SERVICE slot-coded handles. */
static DWORD g_scm_refs = 0;

static void svc_init_locked(void)
{
    if (g_svc_init) return;
    memset(g_svc, 0, sizeof(g_svc));
    memset(g_svc_name_hash, 0, sizeof(g_svc_name_hash));
    g_svc_free_top = 0;
    for (int i = MYSVC_MAX_SERVICES - 1; i >= 0; --i) g_svc_free_stack[g_svc_free_top++] = i;
    g_svc_init = 1;
    _ObjectRegister(SCM_HANDLE_VALUE, _OBJECT_TYPE_SERVICE, 4u, MYSVC_ACCESS_ALL, 0, "\\ServicesActive\\SCM");
    _ObjectSetSecurity(SCM_HANDLE_VALUE, _OBJECT_SD_ADMIN_ONLY|_OBJECT_SD_PUBLIC_READ, _OBJECT_NS_SESSION);
}

void MySvcInit(void)
{
    pthread_mutex_lock(&g_svc_lock);
    svc_init_locked();
    pthread_mutex_unlock(&g_svc_lock);
}

static DWORD svc_name_hash(const char* name)
{
    DWORD h = 2166136261u;
    if (!name) return h;
    while (*name) {
        unsigned char ch = (unsigned char)*name++;
        if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
        h ^= ch;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static inline int svc_name_bucket(DWORD hash)
{
    return (int)(hash & MYSVC_NAME_HASH_MASK);
}

static void svc_hash_insert_locked(int idx)
{
    if (idx < 0 || idx >= MYSVC_MAX_SERVICES || !g_svc[idx].valid || !g_svc[idx].info.name[0]) return;
    DWORD h = svc_name_hash(g_svc[idx].info.name);
    int b = svc_name_bucket(h);
    g_svc[idx].nameHash = h;
    g_svc[idx].nameHashNext = g_svc_name_hash[b];
    g_svc_name_hash[b] = idx + 1;
}

static void svc_hash_remove_locked(int idx)
{
    if (idx < 0 || idx >= MYSVC_MAX_SERVICES || !g_svc[idx].nameHash) return;
    int b = svc_name_bucket(g_svc[idx].nameHash);
    int* link = &g_svc_name_hash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_svc[cur].nameHashNext; break; }
        link = &g_svc[cur].nameHashNext;
    }
    g_svc[idx].nameHash = 0;
    g_svc[idx].nameHashNext = 0;
}

static int svc_pop_free_locked(void)
{
    svc_init_locked();
    if (g_svc_free_top <= 0) return -1;
    return g_svc_free_stack[--g_svc_free_top];
}

static void svc_push_free_locked(int idx)
{
    svc_init_locked();
    if (idx < 0 || idx >= MYSVC_MAX_SERVICES || g_svc_free_top >= MYSVC_MAX_SERVICES) return;
    g_svc_free_stack[g_svc_free_top++] = idx;
}

static void svc_clear_entry_locked(int idx)
{
    if (idx < 0 || idx >= MYSVC_MAX_SERVICES || !g_svc[idx].valid) return;
    svc_hash_remove_locked(idx);
    memset(&g_svc[idx], 0, sizeof(g_svc[idx]));
    svc_push_free_locked(idx);
}

static int find_by_handle_locked(SC_HANDLE h)
{
    DWORD type = 0, slot = 0;
    if (_ObjectDecodeSlotHandle((HANDLE)h, &type, &slot) && type == _OBJECT_TYPE_SERVICE) {
        /* slot 0 is the SCM handle; real service entries start at slot 1. */
        if (slot >= 1 && (slot - 1u) < MYSVC_MAX_SERVICES) {
            int idx = (int)(slot - 1u);
            if (g_svc[idx].valid && g_svc[idx].info.hService == h) return idx;
        }
        return -1;
    }
    for (int i = 0; i < MYSVC_MAX_SERVICES; ++i)
        if (g_svc[i].valid && g_svc[i].info.hService == h) return i;
    return -1;
}

static int find_by_name_locked(const char* name)
{
    if (!name) return -1;
    DWORD h = svc_name_hash(name);
    int b = svc_name_bucket(h);
    for (int link = g_svc_name_hash[b]; link; link = g_svc[link - 1].nameHashNext) {
        int idx = link - 1;
        if (MYOS_UNLIKELY(idx < 0 || idx >= MYSVC_MAX_SERVICES)) break;
        MyServiceEntry* e = &g_svc[idx];
        if (MYOS_UNLIKELY(!e->valid)) continue;
        if (e->nameHash == h && strcasecmp(e->info.name, name) == 0) return idx;
    }
    /* Compatibility fallback for older/raw entries should never be hot. */
    for (int i = 0; i < MYSVC_MAX_SERVICES; ++i)
        if (g_svc[i].valid && strcasecmp(g_svc[i].info.name, name) == 0) return i;
    return -1;
}

SC_HANDLE OpenSCManagerA(LPCSTR lpMachineName, LPCSTR lpDatabaseName, DWORD dwDesiredAccess)
{
    (void)lpMachineName; (void)lpDatabaseName; (void)dwDesiredAccess;
    MySvcInit();
    pthread_mutex_lock(&g_svc_lock);
    g_scm_refs++;
    _ObjectAddRef(SCM_HANDLE_VALUE);
    pthread_mutex_unlock(&g_svc_lock);
    return SCM_HANDLE_VALUE;
}

SC_HANDLE CreateServiceA(SC_HANDLE hSCManager, LPCSTR lpServiceName, LPCSTR lpDisplayName,
                         DWORD dwDesiredAccess, DWORD dwServiceType, DWORD dwStartType,
                         DWORD dwErrorControl, LPCSTR lpBinaryPathName, LPCSTR lpLoadOrderGroup,
                         DWORD* lpdwTagId, LPCSTR lpDependencies, LPCSTR lpServiceStartName,
                         LPCSTR lpPassword)
{
    (void)dwServiceType; (void)dwErrorControl; (void)lpLoadOrderGroup; (void)lpdwTagId; (void)lpDependencies; (void)lpServiceStartName; (void)lpPassword;
    if (hSCManager != SCM_HANDLE_VALUE || !lpServiceName || !lpServiceName[0]) return 0;
    MySvcInit();
    pthread_mutex_lock(&g_svc_lock);
    int idx = find_by_name_locked(lpServiceName);
    if (idx >= 0) {
        g_svc[idx].refCount++;
        g_svc[idx].grantedAccess |= dwDesiredAccess ? dwDesiredAccess : MYSVC_ACCESS_ALL;
        _ObjectAddRef(g_svc[idx].info.hService);
        SC_HANDLE h = g_svc[idx].info.hService;
        pthread_mutex_unlock(&g_svc_lock);
        return h;
    }
    int i = svc_pop_free_locked();
    if (i < 0) {
        pthread_mutex_unlock(&g_svc_lock);
        return 0;
    }
    memset(&g_svc[i], 0, sizeof(g_svc[i]));
    g_svc[i].valid = 1;
    g_svc[i].refCount = 1;
    g_svc[i].grantedAccess = dwDesiredAccess ? dwDesiredAccess : MYSVC_ACCESS_ALL;
    g_svc[i].info.hService = (SC_HANDLE)_ObjectMakeSlotHandle(_OBJECT_TYPE_SERVICE, (DWORD)(i + 1));
    g_svc[i].info.ownerPid = 4;
    g_svc[i].info.state = MYSVC_STOPPED;
    g_svc[i].info.startType = dwStartType ? dwStartType : MYSVC_START_TYPE_DEMAND;
    g_svc[i].info.acceptedControls = MYSVC_ACCEPT_STOP;
    g_svc[i].info.flags = (g_svc[i].info.startType == MYSVC_START_TYPE_AUTO) ? MYSVC_FLAG_AUTO_START : 0;
    snprintf(g_svc[i].info.name, sizeof(g_svc[i].info.name), "%s", lpServiceName);
    snprintf(g_svc[i].info.displayName, sizeof(g_svc[i].info.displayName), "%s", lpDisplayName ? lpDisplayName : lpServiceName);
    snprintf(g_svc[i].info.binaryPath, sizeof(g_svc[i].info.binaryPath), "%s", lpBinaryPathName ? lpBinaryPathName : "myos-service-lite");
    svc_hash_insert_locked(i);
    char objName[128];
    snprintf(objName, sizeof(objName), "\\ServicesActive\\%s", g_svc[i].info.name);
    _ObjectRegister(g_svc[i].info.hService, _OBJECT_TYPE_SERVICE, 4u, g_svc[i].grantedAccess, 0, objName);
    _ObjectSetSecurity(g_svc[i].info.hService, _OBJECT_SD_ADMIN_ONLY|_OBJECT_SD_PUBLIC_READ, _OBJECT_NS_SESSION);
    SC_HANDLE h = g_svc[i].info.hService;
    pthread_mutex_unlock(&g_svc_lock);
    return h;
}

SC_HANDLE OpenServiceA(SC_HANDLE hSCManager, LPCSTR lpServiceName, DWORD dwDesiredAccess)
{
    if (hSCManager != SCM_HANDLE_VALUE) return 0;
    MySvcInit();
    pthread_mutex_lock(&g_svc_lock);
    int idx = find_by_name_locked(lpServiceName);
    if (idx < 0) { pthread_mutex_unlock(&g_svc_lock); return 0; }
    g_svc[idx].refCount++;
    g_svc[idx].grantedAccess |= dwDesiredAccess ? dwDesiredAccess : MYSVC_ACCESS_QUERY;
    _ObjectAddRef(g_svc[idx].info.hService);
    SC_HANDLE h = g_svc[idx].info.hService;
    pthread_mutex_unlock(&g_svc_lock);
    return h;
}

BOOL StartServiceA(SC_HANDLE hService, DWORD dwNumServiceArgs, LPCSTR* lpServiceArgVectors)
{
    (void)dwNumServiceArgs; (void)lpServiceArgVectors;
    MySvcInit();
    pthread_mutex_lock(&g_svc_lock);
    int idx = find_by_handle_locked(hService);
    if (idx < 0 || !(g_svc[idx].grantedAccess & MYSVC_ACCESS_START)) { pthread_mutex_unlock(&g_svc_lock); return FALSE; }
    g_svc[idx].info.state = MYSVC_RUNNING;
    g_svc[idx].info.flags |= MYSVC_FLAG_RUNNING;
    g_svc[idx].info.checkpoint++;
    _ObjectSetInfo(hService, g_svc[idx].info.flags, 0, NULL);
    pthread_mutex_unlock(&g_svc_lock);
    return TRUE;
}

BOOL ControlService(SC_HANDLE hService, DWORD dwControl, MyServiceInfo* lpServiceStatus)
{
    MySvcInit();
    pthread_mutex_lock(&g_svc_lock);
    int idx = find_by_handle_locked(hService);
    if (idx < 0 || !(g_svc[idx].grantedAccess & MYSVC_ACCESS_STOP)) { pthread_mutex_unlock(&g_svc_lock); return FALSE; }
    if (dwControl == SERVICE_CONTROL_STOP) {
        g_svc[idx].info.state = MYSVC_STOPPED;
        g_svc[idx].info.flags &= ~MYSVC_FLAG_RUNNING;
        g_svc[idx].info.checkpoint++;
        _ObjectSetInfo(hService, g_svc[idx].info.flags, 0, NULL);
    }
    if (lpServiceStatus) *lpServiceStatus = g_svc[idx].info;
    pthread_mutex_unlock(&g_svc_lock);
    return TRUE;
}

BOOL DeleteService(SC_HANDLE hService)
{
    MySvcInit();
    pthread_mutex_lock(&g_svc_lock);
    int idx = find_by_handle_locked(hService);
    if (idx < 0 || !(g_svc[idx].grantedAccess & MYSVC_ACCESS_CHANGE)) { pthread_mutex_unlock(&g_svc_lock); return FALSE; }
    g_svc[idx].info.flags |= MYSVC_FLAG_MARKED_DELETE;
    g_svc[idx].info.checkpoint++;
    _ObjectSetInfo(hService, g_svc[idx].info.flags, 0, NULL);
    pthread_mutex_unlock(&g_svc_lock);
    return TRUE;
}

BOOL QueryServiceStatus(SC_HANDLE hService, MyServiceInfo* lpServiceStatus)
{
    if (!lpServiceStatus) return FALSE;
    MySvcInit();
    pthread_mutex_lock(&g_svc_lock);
    int idx = find_by_handle_locked(hService);
    if (idx < 0 || !(g_svc[idx].grantedAccess & MYSVC_ACCESS_QUERY)) { pthread_mutex_unlock(&g_svc_lock); return FALSE; }
    *lpServiceStatus = g_svc[idx].info;
    pthread_mutex_unlock(&g_svc_lock);
    return TRUE;
}

BOOL CloseServiceHandle(SC_HANDLE hSCObject)
{
    if (!hSCObject) return FALSE;
    MySvcInit();
    pthread_mutex_lock(&g_svc_lock);
    if (hSCObject == SCM_HANDLE_VALUE) {
        if (g_scm_refs) g_scm_refs--;
        _ObjectRelease(SCM_HANDLE_VALUE);
        pthread_mutex_unlock(&g_svc_lock);
        return TRUE;
    }
    int idx = find_by_handle_locked(hSCObject);
    if (idx < 0) { pthread_mutex_unlock(&g_svc_lock); return FALSE; }
    if (g_svc[idx].refCount) g_svc[idx].refCount--;
    int purge = (g_svc[idx].refCount == 0) && (g_svc[idx].info.flags & MYSVC_FLAG_MARKED_DELETE);
    _ObjectRelease(hSCObject);
    if (purge) svc_clear_entry_locked(idx);
    pthread_mutex_unlock(&g_svc_lock);
    return TRUE;
}

BOOL MySvcEnumServices(MYSVCENUMPROC lpEnumFunc, LPARAM lParam)
{
    if (!lpEnumFunc) return FALSE;
    MySvcInit();
    MyServiceInfo snap[MYSVC_MAX_SERVICES];
    int n = 0;
    pthread_mutex_lock(&g_svc_lock);
    for (int i = 0; i < MYSVC_MAX_SERVICES; ++i) if (g_svc[i].valid && n < MYSVC_MAX_SERVICES) snap[n++] = g_svc[i].info;
    pthread_mutex_unlock(&g_svc_lock);
    for (int i = 0; i < n; ++i) if (!lpEnumFunc(&snap[i], lParam)) break;
    return TRUE;
}

DWORD MySvcGetCount(void)
{
    MySvcInit();
    DWORD n = 0;
    pthread_mutex_lock(&g_svc_lock);
    for (int i = 0; i < MYSVC_MAX_SERVICES; ++i) if (g_svc[i].valid) n++;
    pthread_mutex_unlock(&g_svc_lock);
    return n;
}

DWORD MySvcGetRunningCount(void)
{
    MySvcInit();
    DWORD n = 0;
    pthread_mutex_lock(&g_svc_lock);
    for (int i = 0; i < MYSVC_MAX_SERVICES; ++i) if (g_svc[i].valid && g_svc[i].info.state == MYSVC_RUNNING) n++;
    pthread_mutex_unlock(&g_svc_lock);
    return n;
}

const char* MySvcStateName(DWORD state)
{
    switch (state) {
    case MYSVC_STOPPED: return "STOPPED";
    case MYSVC_START_PENDING: return "START_PENDING";
    case MYSVC_STOP_PENDING: return "STOP_PENDING";
    case MYSVC_RUNNING: return "RUNNING";
    case MYSVC_PAUSED: return "PAUSED";
    default: return "UNKNOWN";
    }
}
