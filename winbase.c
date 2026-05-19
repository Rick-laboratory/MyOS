#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "mywin_pendant_internal.h"
#include "processhost.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>

// v109: sdk/include/winbase.h -> winbase.c
// Public KERNEL32/WinBase/MSDN entrypoints live here directly.


// ─────────────────────────────────────────────
// v109: KERNEL32/WinBase implementation cluster.
// This is deliberately process-local for now, but handle/view semantics mirror
// the future out-of-process shared memory path.
// ─────────────────────────────────────────────
/* AUDIT(v118): Fixed-size kernel/object tables. Current labs fit, but repeated
   open/close, MDI, or foreign apps will hit these quotas. Capacity failures
   must eventually return precise LastError values instead of silent FALSE/NULL. */
#define MYWIN_MAX_SECTIONS 32
#define MYWIN_MAX_VIEWS    64
#define MYWIN_MAX_EVENTS   64
#define MYWIN_MAX_MUTEXES  64
#define MYWIN_MAX_SEMAPHORES 64
#define MYWIN_MAX_TIMERS   64
#define MYWIN_NAMED_OBJECT_HASH_BUCKETS 128
#define MYWIN_NAMED_OBJECT_HASH_MASK (MYWIN_NAMED_OBJECT_HASH_BUCKETS - 1)

/* v246/v247: WFMO waiters attach stack WaitBlocks directly to
   dispatcher-capable objects.  v247 folds Process/Thread objects into the
   same object-linked path used by Event/Mutex/Semaphore/Timer. */
typedef struct MyWinMultiWaiter MyWinMultiWaiter;
typedef struct MyWinWaitBlock {
    struct MyWinWaitBlock* next;
    struct MyWinWaitBlock** headLink;
    MyWinMultiWaiter* waiter;
    HANDLE objectHandle;
    DWORD objectType;
} MyWinWaitBlock;

#define MYWIN_DISPATCHER_HEADER_MAGIC 0x44534831u /* "DSH1" */
typedef struct MyWinDispatcherHeader {
    DWORD magic;
    DWORD objectType;
    HANDLE objectHandle;
    volatile LONG signalState;
    DWORD flags;
    MyWinWaitBlock* waitHead;
} MyWinDispatcherHeader;

typedef struct MyWinSectionObj {
    int valid;
    HANDLE handle;
    DWORD owner_pid;
    DWORD protect;
    DWORD size;
    DWORD refCount;
    char  name[96];
    DWORD nameHash;
    int   nameHashNext;
    char  shmName[96];
    int   shmFd;
    BYTE* data;
} MyWinSectionObj;

typedef struct MyWinMappedView {
    int valid;
    LPVOID ptr;
    HANDLE section;
    DWORD access;
    DWORD size;
} MyWinMappedView;

static MyWinSectionObj g_Sections[MYWIN_MAX_SECTIONS];
static MyWinMappedView g_Views[MYWIN_MAX_VIEWS];
static int g_SectionNameHash[MYWIN_NAMED_OBJECT_HASH_BUCKETS];
/* v216: sections use slot-coded Object Manager handles, not monotonic raw handles. */

typedef struct MyWinEventObj {
    int valid;
    HANDLE handle;
    DWORD owner_pid;
    DWORD access;
    DWORD refCount;
    BOOL manualReset;
    BOOL signaled;
    char name[96];
    DWORD nameHash;
    int nameHashNext;
    MyWinDispatcherHeader dispatcher;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} MyWinEventObj;

static MyWinEventObj g_Events[MYWIN_MAX_EVENTS];
static pthread_mutex_t g_EventTableLock = PTHREAD_MUTEX_INITIALIZER;
static int g_EventNameHash[MYWIN_NAMED_OBJECT_HASH_BUCKETS];


typedef struct MyWinMutexObj {
    int valid;
    HANDLE handle;
    DWORD owner_pid;
    DWORD access;
    DWORD refCount;
    BOOL owned;
    DWORD owner_thread;
    BOOL abandoned;
    char name[96];
    DWORD nameHash;
    int nameHashNext;
    MyWinDispatcherHeader dispatcher;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} MyWinMutexObj;

static MyWinMutexObj g_Mutexes[MYWIN_MAX_MUTEXES];
static pthread_mutex_t g_MutexTableLock = PTHREAD_MUTEX_INITIALIZER;
static int g_MutexNameHash[MYWIN_NAMED_OBJECT_HASH_BUCKETS];

typedef struct MyWinSemaphoreObj {
    int valid;
    HANDLE handle;
    DWORD owner_pid;
    DWORD access;
    DWORD refCount;
    LONG count;
    LONG maxCount;
    char name[96];
    DWORD nameHash;
    int nameHashNext;
    MyWinDispatcherHeader dispatcher;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} MyWinSemaphoreObj;

static MyWinSemaphoreObj g_Semaphores[MYWIN_MAX_SEMAPHORES];
static pthread_mutex_t g_SemaphoreTableLock = PTHREAD_MUTEX_INITIALIZER;
static int g_SemaphoreNameHash[MYWIN_NAMED_OBJECT_HASH_BUCKETS];

typedef struct MyWinTimerObj {
    int valid;
    HANDLE handle;
    DWORD owner_pid;
    DWORD access;
    DWORD refCount;
    BOOL manualReset;
    BOOL signaled;
    BOOL active;
    unsigned long long dueMs;
    LONG periodMs;
    char name[96];
    DWORD nameHash;
    int nameHashNext;
    MyWinDispatcherHeader dispatcher;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} MyWinTimerObj;

static MyWinTimerObj g_Timers[MYWIN_MAX_TIMERS];
static int g_TimerNameHash[MYWIN_NAMED_OBJECT_HASH_BUCKETS];

/* v250: central named-kernel-object directory.  The per-type hash tables remain
   as the fast typed payload lookup, but a shared name directory now catches
   cross-type collisions (Event vs Mutex vs Section, etc.) the way the NT
   Object Manager namespace does. */
#define MYWIN_NAMED_DIRECTORY_MAX 256
#define MYWIN_NAMED_DIRECTORY_BUCKETS 256u
#define MYWIN_NAMED_DIRECTORY_MASK (MYWIN_NAMED_DIRECTORY_BUCKETS - 1u)
typedef struct MyWinNamedDirectoryEntry {
    int valid;
    char name[96];
    DWORD nameHash;
    int hashNext;
    HANDLE objectHandle;
    DWORD objectType;
    DWORD objectSlot;       /* v253: decoded typed payload slot for direct typed-table open */
    DWORD objectGeneration; /* v253: Object Manager generation observed at publish time */
} MyWinNamedDirectoryEntry;
static MyWinNamedDirectoryEntry g_NamedDirectory[MYWIN_NAMED_DIRECTORY_MAX];
static int g_NamedDirectoryHash[MYWIN_NAMED_DIRECTORY_BUCKETS];
static int g_NamedDirectoryFree[MYWIN_NAMED_DIRECTORY_MAX];
static unsigned char g_NamedDirectoryFreeMark[MYWIN_NAMED_DIRECTORY_MAX];
static int g_NamedDirectoryFreeTop = 0;
static int g_NamedDirectoryFreeInit = 0;
static pthread_mutex_t g_NamedDirectoryLock = PTHREAD_MUTEX_INITIALIZER;
static DWORD g_NamedDirectoryLookups = 0;
static DWORD g_NamedDirectoryHits = 0;
static DWORD g_NamedDirectoryMisses = 0;
static DWORD g_NamedDirectoryCrossTypeConflicts = 0;
static DWORD g_NamedDirectoryInserts = 0;
static DWORD g_NamedDirectoryRemoves = 0;
static DWORD g_NamedDirectoryFastHits = 0;
static DWORD g_NamedDirectoryFastMisses = 0;
static DWORD g_NamedDirectoryFastTypeMismatches = 0;
static DWORD g_NamedDirectoryStaleHits = 0;
static DWORD g_NamedDirectoryFreeReuse = 0;
static DWORD g_NamedDirectoryFreeDuplicateSkips = 0;
static DWORD g_NamedDirectoryEpoch = 1;
static DWORD g_NamedDirectoryTlsHits = 0;
static DWORD g_NamedDirectoryTlsMisses = 0;
static DWORD g_NamedDirectoryTlsEpochMisses = 0;
static DWORD g_NamedDirectoryTlsCollisions = 0;
static DWORD g_NamedDirectoryTlsStores = 0;
static DWORD g_NamedDirectoryTlsStaleInvalidations = 0;
static DWORD g_NamedDirectorySlotFastHits = 0;
static DWORD g_NamedDirectorySlotFastMisses = 0;
#define MYWIN_NAMED_DIRECTORY_TLS_SETS 32u
#define MYWIN_NAMED_DIRECTORY_TLS_WAYS 4u
#define MYWIN_NAMED_DIRECTORY_TLS_SLOTS (MYWIN_NAMED_DIRECTORY_TLS_SETS * MYWIN_NAMED_DIRECTORY_TLS_WAYS)
#define MYWIN_NAMED_DIRECTORY_TLS_MASK (MYWIN_NAMED_DIRECTORY_TLS_SETS - 1u)
typedef struct MyWinNamedDirectoryTlsEntry {
    DWORD valid;
    DWORD epoch;
    DWORD nameHash;
    DWORD wantedType;
    DWORD objectType;
    DWORD objectSlot;
    DWORD objectGeneration;
    HANDLE objectHandle;
    char name[96];
} MyWinNamedDirectoryTlsEntry;
static __thread MyWinNamedDirectoryTlsEntry g_NamedDirectoryTls[MYWIN_NAMED_DIRECTORY_TLS_SLOTS];
static __thread unsigned char g_NamedDirectoryTlsNext[MYWIN_NAMED_DIRECTORY_TLS_SETS];
#define MYWIN_NAMED_DIRECTORY_MISS 0
#define MYWIN_NAMED_DIRECTORY_HIT 1
#define MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH 2
static void mywin_named_directory_free_init_locked(void);
static BOOL __attribute__((unused)) mywin_named_directory_preflight_type(LPCSTR canon, DWORD wantedType, DWORD* existingTypeOut);
static int mywin_named_directory_fast_lookup_payload(LPCSTR canon, DWORD wantedType, HANDLE* handleOut, DWORD* existingTypeOut, DWORD* slotOut, DWORD* generationOut);
static int __attribute__((unused)) mywin_named_directory_fast_lookup_type(LPCSTR canon, DWORD wantedType, HANDLE* handleOut, DWORD* existingTypeOut);
static BOOL mywin_named_directory_insert(LPCSTR canon, DWORD objectType, HANDLE objectHandle);
static void mywin_named_directory_remove(LPCSTR canon, DWORD objectType, HANDLE objectHandle);
static DWORD mywin_named_directory_epoch_load(void);
static void mywin_named_directory_note_stale(void);

static unsigned long long g_TimerNextDueCacheMs = 0;
static BOOL g_TimerNextDueCacheValid = FALSE;
/* v216: waitable timers use slot-coded Object Manager handles. */

#define MYWIN_MAX_TOKENS 64
typedef struct MyWinTokenObj {
    int valid;
    HANDLE handle;
    DWORD owner_pid;
    DWORD source_pid;
    DWORD access;
    DWORD refCount;
    _ObjectToken token;
    char name[96];
} MyWinTokenObj;

static MyWinTokenObj g_Tokens[MYWIN_MAX_TOKENS];
/* v216: tokens use slot-coded Object Manager handles. */

/* v240: fixed kernel tables now allocate from LIFO free stacks instead of
   scanning every table on each create/open-token/map-view path.  Name lookup
   was already hashed in v239; this completes the O(1)-average allocate side
   for the hot waitable/section/token objects. */
static int g_KernelFreeInit = 0;
static int g_SectionFree[MYWIN_MAX_SECTIONS];
static int g_ViewFree[MYWIN_MAX_VIEWS];
static int g_EventFree[MYWIN_MAX_EVENTS];
static int g_MutexFree[MYWIN_MAX_MUTEXES];
static int g_SemaphoreFree[MYWIN_MAX_SEMAPHORES];
static int g_TimerFree[MYWIN_MAX_TIMERS];
static int g_TokenFree[MYWIN_MAX_TOKENS];
static unsigned char g_SectionFreeMark[MYWIN_MAX_SECTIONS];
static unsigned char g_ViewFreeMark[MYWIN_MAX_VIEWS];
static unsigned char g_EventFreeMark[MYWIN_MAX_EVENTS];
static unsigned char g_MutexFreeMark[MYWIN_MAX_MUTEXES];
static unsigned char g_SemaphoreFreeMark[MYWIN_MAX_SEMAPHORES];
static unsigned char g_TimerFreeMark[MYWIN_MAX_TIMERS];
static unsigned char g_TokenFreeMark[MYWIN_MAX_TOKENS];
static int g_SectionFreeTop = 0;
static int g_ViewFreeTop = 0;
static int g_EventFreeTop = 0;
static int g_MutexFreeTop = 0;
static int g_SemaphoreFreeTop = 0;
static int g_TimerFreeTop = 0;
static int g_TokenFreeTop = 0;

static void mywin_kernel_free_init(void)
{
    if (g_KernelFreeInit) return;
    g_SectionFreeTop = 0;
    for (int i = MYWIN_MAX_SECTIONS - 1; i >= 0; --i) { g_SectionFree[g_SectionFreeTop++] = i; g_SectionFreeMark[i] = 1; }
    g_ViewFreeTop = 0;
    for (int i = MYWIN_MAX_VIEWS - 1; i >= 0; --i) { g_ViewFree[g_ViewFreeTop++] = i; g_ViewFreeMark[i] = 1; }
    g_EventFreeTop = 0;
    for (int i = MYWIN_MAX_EVENTS - 1; i >= 0; --i) { g_EventFree[g_EventFreeTop++] = i; g_EventFreeMark[i] = 1; }
    g_MutexFreeTop = 0;
    for (int i = MYWIN_MAX_MUTEXES - 1; i >= 0; --i) { g_MutexFree[g_MutexFreeTop++] = i; g_MutexFreeMark[i] = 1; }
    g_SemaphoreFreeTop = 0;
    for (int i = MYWIN_MAX_SEMAPHORES - 1; i >= 0; --i) { g_SemaphoreFree[g_SemaphoreFreeTop++] = i; g_SemaphoreFreeMark[i] = 1; }
    g_TimerFreeTop = 0;
    for (int i = MYWIN_MAX_TIMERS - 1; i >= 0; --i) { g_TimerFree[g_TimerFreeTop++] = i; g_TimerFreeMark[i] = 1; }
    g_TokenFreeTop = 0;
    for (int i = MYWIN_MAX_TOKENS - 1; i >= 0; --i) { g_TokenFree[g_TokenFreeTop++] = i; g_TokenFreeMark[i] = 1; }
    g_KernelFreeInit = 1;
}

static void mywin_kernel_push_free(int* stack, unsigned char* mark, int* top, int max, int slot)
{
    mywin_kernel_free_init();
    if (!stack || !mark || !top || slot < 0 || slot >= max || *top >= max) return;
    if (mark[slot]) return;
    mark[slot] = 1;
    stack[(*top)++] = slot;
}

static int mywin_kernel_pop_free_valid(int* stack, unsigned char* mark, int* top, int max, const void* table, size_t stride)
{
    mywin_kernel_free_init();
    while (stack && mark && top && *top > 0) {
        int slot = stack[--(*top)];
        if (slot >= 0 && slot < max) {
            mark[slot] = 0;
            const int* valid = (const int*)((const unsigned char*)table + (size_t)slot * stride);
            if (!*valid) return slot;
        }
    }
    return -1;
}

static int mywin_pop_section_slot(void) { return mywin_kernel_pop_free_valid(g_SectionFree, g_SectionFreeMark, &g_SectionFreeTop, MYWIN_MAX_SECTIONS, g_Sections, sizeof(g_Sections[0])); }
static int mywin_pop_view_slot(void) { return mywin_kernel_pop_free_valid(g_ViewFree, g_ViewFreeMark, &g_ViewFreeTop, MYWIN_MAX_VIEWS, g_Views, sizeof(g_Views[0])); }
static int mywin_pop_event_slot(void) { return mywin_kernel_pop_free_valid(g_EventFree, g_EventFreeMark, &g_EventFreeTop, MYWIN_MAX_EVENTS, g_Events, sizeof(g_Events[0])); }
static int mywin_pop_mutex_slot(void) { return mywin_kernel_pop_free_valid(g_MutexFree, g_MutexFreeMark, &g_MutexFreeTop, MYWIN_MAX_MUTEXES, g_Mutexes, sizeof(g_Mutexes[0])); }
static int mywin_pop_semaphore_slot(void) { return mywin_kernel_pop_free_valid(g_SemaphoreFree, g_SemaphoreFreeMark, &g_SemaphoreFreeTop, MYWIN_MAX_SEMAPHORES, g_Semaphores, sizeof(g_Semaphores[0])); }
static int mywin_pop_timer_slot(void) { return mywin_kernel_pop_free_valid(g_TimerFree, g_TimerFreeMark, &g_TimerFreeTop, MYWIN_MAX_TIMERS, g_Timers, sizeof(g_Timers[0])); }
static int mywin_pop_token_slot(void) { return mywin_kernel_pop_free_valid(g_TokenFree, g_TokenFreeMark, &g_TokenFreeTop, MYWIN_MAX_TOKENS, g_Tokens, sizeof(g_Tokens[0])); }
static void mywin_push_section_slot(int slot) { mywin_kernel_push_free(g_SectionFree, g_SectionFreeMark, &g_SectionFreeTop, MYWIN_MAX_SECTIONS, slot); }
static void mywin_push_view_slot(int slot) { mywin_kernel_push_free(g_ViewFree, g_ViewFreeMark, &g_ViewFreeTop, MYWIN_MAX_VIEWS, slot); }
static void mywin_push_event_slot(int slot) { mywin_kernel_push_free(g_EventFree, g_EventFreeMark, &g_EventFreeTop, MYWIN_MAX_EVENTS, slot); }
static void mywin_push_mutex_slot(int slot) { mywin_kernel_push_free(g_MutexFree, g_MutexFreeMark, &g_MutexFreeTop, MYWIN_MAX_MUTEXES, slot); }
static void mywin_push_semaphore_slot(int slot) { mywin_kernel_push_free(g_SemaphoreFree, g_SemaphoreFreeMark, &g_SemaphoreFreeTop, MYWIN_MAX_SEMAPHORES, slot); }
static void mywin_push_timer_slot(int slot) { mywin_kernel_push_free(g_TimerFree, g_TimerFreeMark, &g_TimerFreeTop, MYWIN_MAX_TIMERS, slot); }
static void mywin_push_token_slot(int slot) { mywin_kernel_push_free(g_TokenFree, g_TokenFreeMark, &g_TokenFreeTop, MYWIN_MAX_TOKENS, slot); }

/* v202: first privilege-bearing token model.  These LUID values are stable
   inside myOS; they intentionally mirror the Win32 shape (LUID + attributes)
   without pretending we have LSASS policy lookup yet. */
#define MYWIN_LUID_SE_SECURITY        8u
#define MYWIN_LUID_SE_TAKE_OWNERSHIP  9u
#define MYWIN_LUID_SE_BACKUP          17u
#define MYWIN_LUID_SE_RESTORE         18u

static BOOL mywin_luid_equal_local(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static void mywin_make_luid_local(DWORD low, LUID* out)
{
    if (!out) return;
    out->LowPart = low;
    out->HighPart = 0;
}

/* v148: central dispatcher wait primitive.  Public wait APIs no longer spin
   with usleep; all supported waitable state transitions wake this condvar. */
static pthread_once_t g_DispatcherOnce = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_DispatcherLock;
static pthread_cond_t  g_DispatcherCond;
static unsigned long long g_DispatcherSeq = 0;
static DWORD g_DispatcherWaiters = 0;

/* v187: waitable-object diagnostics. These are debug counters, not public Win32 ABI.
   They let StateProbe/smokes distinguish "wait returned" from "object exists" and
   catch stale signal/ref semantics after DuplicateHandle/inherit/exit sweeps. */
static DWORD g_WaitSingleCalls = 0;
static DWORD g_WaitMultipleCalls = 0;
static DWORD g_WaitAnyCalls = 0;
static DWORD g_WaitAllCalls = 0;
static DWORD g_WaitSuccess = 0;
static DWORD g_WaitTimeouts = 0;
static DWORD g_WaitFailures = 0;
static DWORD g_WaitAccessDenied = 0;
static DWORD g_WaitInvalidHandle = 0;
static DWORD g_WaitEventConsumes = 0;
static DWORD g_WaitMutexAcquires = 0;
static DWORD g_WaitMutexAbandoned = 0;
static DWORD g_WaitSemaphoreConsumes = 0;
static DWORD g_WaitTimerConsumes = 0;
static DWORD g_WaitAllCommits = 0;
static DWORD g_WaitAnyCommits = 0;
static DWORD g_WaitWakeBroadcasts = 0;
static DWORD g_WaitWakeSkips = 0;
static DWORD g_WaitSingleTargeted = 0;
static DWORD g_WaitSingleGlobalFallback = 0;
static DWORD g_WaitMultipleTargeted = 0;
static DWORD g_WaitMultipleGlobalFallback = 0;
static DWORD g_WaitMultipleTargetedWakes = 0;
static DWORD g_WaitMultipleWaitBlockLinks = 0;
static DWORD g_WaitMultipleWaitBlockUnlinks = 0;
static DWORD g_WaitMultipleWaitBlockObjectWakes = 0;
static DWORD g_WaitMultipleResolvedProbes = 0;
static DWORD g_WaitMultipleImmediateHits = 0;
static DWORD g_WaitMultipleDeferredLinks = 0;
static DWORD g_WaitMultipleTlsGates = 0;
static DWORD g_WaitMultiplePrevalidated = 0;
static DWORD g_WaitMultiplePrevalidateResolves = 0;
static DWORD g_WaitMultiplePrevalidateFallbacks = 0;
static DWORD g_WaitProcessThreadTargeted = 0;
static DWORD g_WaitProcessThreadImmediateHits = 0;
static DWORD g_WaitProcessThreadPollSlices = 0;
static DWORD g_WaitProcessThreadObjectWakes = 0;
static DWORD g_WaitDispatcherHeaderInits = 0;
static DWORD g_WaitDispatcherHeaderHeadHits = 0;
static DWORD g_WaitDispatcherHeaderStateStores = 0;
static DWORD g_WaitDispatcherHeaderFastNotReady = 0;

static void mywin_dispatcher_init_once(void)
{
    pthread_mutex_init(&g_DispatcherLock, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#ifdef CLOCK_MONOTONIC
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&g_DispatcherCond, &attr);
    pthread_condattr_destroy(&attr);
}

static void mywin_dispatcher_ensure(void)
{
    pthread_once(&g_DispatcherOnce, mywin_dispatcher_init_once);
}

static void mywin_waitable_cond_init(pthread_cond_t* cond)
{
    if (!cond) return;
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#ifdef CLOCK_MONOTONIC
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

static void mywin_dispatcher_header_bind(MyWinDispatcherHeader* h, HANDLE objectHandle, DWORD objectType, LONG signalState)
{
    if (!h) return;
    if (h->magic != MYWIN_DISPATCHER_HEADER_MAGIC || h->objectHandle != objectHandle || h->objectType != objectType) {
        h->waitHead = NULL;
        h->magic = MYWIN_DISPATCHER_HEADER_MAGIC;
        h->objectHandle = objectHandle;
        h->objectType = objectType;
        h->flags = 0;
        g_WaitDispatcherHeaderInits++;
    }
    __atomic_store_n(&h->signalState, signalState, __ATOMIC_RELEASE);
    g_WaitDispatcherHeaderStateStores++;
}

static MyWinWaitBlock** mywin_dispatcher_header_wait_head(MyWinDispatcherHeader* h, HANDLE objectHandle, DWORD objectType)
{
    if (!h || h->magic != MYWIN_DISPATCHER_HEADER_MAGIC) return NULL;
    if (h->objectHandle != objectHandle || h->objectType != objectType) return NULL;
    g_WaitDispatcherHeaderHeadHits++;
    return &h->waitHead;
}

static BOOL mywin_dispatcher_header_fast_not_ready(MyWinDispatcherHeader* h)
{
    if (!h || h->magic != MYWIN_DISPATCHER_HEADER_MAGIC) return FALSE;
    LONG state = __atomic_load_n(&h->signalState, __ATOMIC_ACQUIRE);
    if (state <= 0) {
        g_WaitDispatcherHeaderFastNotReady++;
        return TRUE;
    }
    return FALSE;
}


/* Forward declarations needed by the v246 WaitBlock list helpers below. */
static MyWinEventObj* mywin_find_event(HANDLE h);
static MyWinMutexObj* mywin_find_mutex(HANDLE h);
static MyWinSemaphoreObj* mywin_find_semaphore(HANDLE h);
static MyWinTimerObj* mywin_find_timer(HANDLE h);
static MyWinWaitBlock** mywin_process_thread_wait_head_for_object_locked(HANDLE objectHandle, DWORD objectType);

/* v246/v247: targeted WaitForMultipleObjects uses per-object intrusive
   WaitBlock lists instead of the v245 central waiter registry.  Pthreads still
   cannot wait on N independent condvars directly, so a WFMO call owns one
   stack waiter plus up to MAXIMUM_WAIT_OBJECTS stack blocks.  A signal on a
   dispatcher-capable object walks only that object's waitHead list. */
struct MyWinMultiWaiter {
    pthread_cond_t* cond;
    DWORD nCount;
    HANDLE objects[MAXIMUM_WAIT_OBJECTS];
    DWORD types[MAXIMUM_WAIT_OBJECTS];
    MyWinWaitBlock blocks[MAXIMUM_WAIT_OBJECTS];
    DWORD blockCount;
    DWORD wakeCount;
    BOOL registered;
};

static MyWinWaitBlock** mywin_wait_head_for_object_locked(HANDLE objectHandle, DWORD objectType)
{
    if (!objectHandle) return NULL;
    if (objectType == _OBJECT_TYPE_EVENT) {
        MyWinEventObj* ev = mywin_find_event(objectHandle);
        return (ev && ev->valid) ? mywin_dispatcher_header_wait_head(&ev->dispatcher, objectHandle, objectType) : NULL;
    }
    if (objectType == _OBJECT_TYPE_MUTEX) {
        MyWinMutexObj* m = mywin_find_mutex(objectHandle);
        return (m && m->valid) ? mywin_dispatcher_header_wait_head(&m->dispatcher, objectHandle, objectType) : NULL;
    }
    if (objectType == _OBJECT_TYPE_SEMAPHORE) {
        MyWinSemaphoreObj* sem = mywin_find_semaphore(objectHandle);
        return (sem && sem->valid) ? mywin_dispatcher_header_wait_head(&sem->dispatcher, objectHandle, objectType) : NULL;
    }
    if (objectType == _OBJECT_TYPE_TIMER) {
        MyWinTimerObj* t = mywin_find_timer(objectHandle);
        return (t && t->valid) ? mywin_dispatcher_header_wait_head(&t->dispatcher, objectHandle, objectType) : NULL;
    }
    if (objectType == _OBJECT_TYPE_PROCESS || objectType == _OBJECT_TYPE_THREAD) {
        return mywin_process_thread_wait_head_for_object_locked(objectHandle, objectType);
    }
    return NULL;
}

static void mywin_waitblock_unlink_locked(MyWinWaitBlock* block)
{
    if (!block || !block->headLink) return;
    MyWinWaitBlock** link = block->headLink;
    while (*link) {
        if (*link == block) {
            *link = block->next;
            g_WaitMultipleWaitBlockUnlinks++;
            break;
        }
        link = &(*link)->next;
    }
    block->next = NULL;
    block->headLink = NULL;
    block->waiter = NULL;
    block->objectHandle = 0;
    block->objectType = 0;
}

static __thread pthread_cond_t g_MultiWaitTlsCond;
static __thread int g_MultiWaitTlsCondInit = 0;

static pthread_cond_t* mywin_multi_wait_tls_cond(void)
{
    if (!g_MultiWaitTlsCondInit) {
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
#ifdef CLOCK_MONOTONIC
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
        pthread_cond_init(&g_MultiWaitTlsCond, &attr);
        pthread_condattr_destroy(&attr);
        g_MultiWaitTlsCondInit = 1;
        g_WaitMultipleTlsGates++;
    }
    return &g_MultiWaitTlsCond;
}

static DWORD mywin_multi_waiter_signal_locked(HANDLE objectHandle, DWORD objectType)
{
    MyWinWaitBlock** head = mywin_wait_head_for_object_locked(objectHandle, objectType);
    if (!head || !*head) return 0;

    DWORD signaled = 0;
    for (MyWinWaitBlock* block = *head; block; block = block->next) {
        MyWinMultiWaiter* w = block->waiter;
        if (w && w->registered && w->cond) {
            w->wakeCount++;
            pthread_cond_signal(w->cond);
            signaled++;
        }
    }
    if (signaled) {
        g_WaitMultipleTargetedWakes += signaled;
        g_WaitMultipleWaitBlockObjectWakes += signaled;
        if (objectType == _OBJECT_TYPE_PROCESS || objectType == _OBJECT_TYPE_THREAD)
            g_WaitProcessThreadObjectWakes += signaled;
    }
    return signaled;
}

static void mywin_multi_waiter_register_locked(MyWinMultiWaiter* w, DWORD nCount, const HANDLE* objects, const DWORD* types)
{
    if (!w || !objects || !types || nCount == 0 || nCount > MAXIMUM_WAIT_OBJECTS) return;
    memset(w, 0, sizeof(*w));
    w->cond = mywin_multi_wait_tls_cond();
    w->nCount = nCount;
    w->registered = TRUE;
    for (DWORD i = 0; i < nCount; ++i) {
        w->objects[i] = objects[i];
        w->types[i] = types[i];
        MyWinWaitBlock* block = &w->blocks[i];
        memset(block, 0, sizeof(*block));
        block->waiter = w;
        block->objectHandle = objects[i];
        block->objectType = types[i];
        MyWinWaitBlock** head = mywin_wait_head_for_object_locked(objects[i], types[i]);
        if (head) {
            block->headLink = head;
            block->next = *head;
            *head = block;
            g_WaitMultipleWaitBlockLinks++;
        }
        w->blockCount++;
    }
}

static void mywin_multi_waiter_unregister_locked(MyWinMultiWaiter* w)
{
    if (!w) return;
    w->registered = FALSE;
    for (DWORD i = 0; i < w->blockCount && i < MAXIMUM_WAIT_OBJECTS; ++i) {
        mywin_waitblock_unlink_locked(&w->blocks[i]);
    }
    w->blockCount = 0;
}

static void mywin_multi_waiter_destroy(MyWinMultiWaiter* w)
{
    if (!w) return;
    /* v248: the gate condvar is a per-thread reusable dispatcher gate. */
    w->cond = NULL;
}

static void mywin_waitblocks_detach_object_locked(HANDLE objectHandle, DWORD objectType)
{
    MyWinWaitBlock** head = mywin_wait_head_for_object_locked(objectHandle, objectType);
    if (!head || !*head) return;
    MyWinWaitBlock* block = *head;
    *head = NULL;
    while (block) {
        MyWinWaitBlock* next = block->next;
        MyWinMultiWaiter* w = block->waiter;
        if (w && w->registered && w->cond) {
            w->wakeCount++;
            pthread_cond_signal(w->cond);
            g_WaitMultipleTargetedWakes++;
            g_WaitMultipleWaitBlockObjectWakes++;
        }
        block->next = NULL;
        block->headLink = NULL;
        block->waiter = NULL;
        block->objectHandle = 0;
        block->objectType = 0;
        g_WaitMultipleWaitBlockUnlinks++;
        block = next;
    }
}

static void mywin_dispatcher_signal_object_locked(HANDLE objectHandle, DWORD objectType)
{
    g_DispatcherSeq++;
    (void)mywin_multi_waiter_signal_locked(objectHandle, objectType);
    if (g_DispatcherWaiters) {
        g_WaitWakeBroadcasts++;
        pthread_cond_broadcast(&g_DispatcherCond);
    } else {
        /* v244/v245: single-object waits block on object condvars and native
           multi-object waits use stack waiters, so skip the global condvar when
           no process/thread fallback waiter is parked there. */
        g_WaitWakeSkips++;
    }
}

static void mywin_dispatcher_signal_locked(void)
{
    mywin_dispatcher_signal_object_locked(0, _OBJECT_TYPE_NONE);
}

static void __attribute__((unused)) mywin_dispatcher_wake_all(void)
{
    mywin_dispatcher_ensure();
    if (pthread_mutex_trylock(&g_DispatcherLock) == 0) {
        mywin_dispatcher_signal_locked();
        pthread_mutex_unlock(&g_DispatcherLock);
    }
}

static void mywin_dispatcher_wake_object(HANDLE objectHandle, DWORD objectType)
{
    mywin_dispatcher_ensure();
    if (pthread_mutex_trylock(&g_DispatcherLock) == 0) {
        mywin_dispatcher_signal_object_locked(objectHandle, objectType);
        pthread_mutex_unlock(&g_DispatcherLock);
    }
}

static void mywin_abs_mono_timespec_from_ms(unsigned long long targetMs, struct timespec* out)
{
    if (!out) return;
    out->tv_sec = (time_t)(targetMs / 1000ull);
    out->tv_nsec = (long)((targetMs % 1000ull) * 1000000ull);
}


// v205: scalable process-local handle tables. Public HANDLE values are
// still process-local table handles, but the slot portion is now a sparse
// 24-bit index instead of a fixed 256-entry global array. This mirrors the
// NT shape much more closely: a small process allocates only a few leaf pages,
// while the architectural slot space can grow to 16,777,215 handles/process.
#define MYWIN_HANDLE_TAG             0x80000000u
#define MYWIN_HANDLE_GENERATION_BITS 7u
#define MYWIN_HANDLE_SLOT_BITS       24u
#define MYWIN_HANDLE_GENERATION_MASK 0x7fu
#define MYWIN_HANDLE_SLOT_MASK       0x00ffffffu
#define MYWIN_HANDLE_INDEX_COUNT     256u
#define MYWIN_HANDLE_MAX_SLOTS       MYWIN_HANDLE_SLOT_MASK
#define MYWIN_DEFAULT_HANDLE_QUOTA    1048576u
#define MYWIN_PSEUDO_CURRENT_PROCESS  ((HANDLE)0xffffffffu)
#define MYWIN_PSEUDO_CURRENT_THREAD   ((HANDLE)0xfffffffeu)

/* v212: userspace EX_PUSH_LOCK-style handle-table lock.
   The v211 wrapper used pthread_rwlock_t, which was functionally correct but
   not the shape Windows NT uses. This lock now has an atomic fast path:

     state == 0                         unlocked
     state & MYWIN_PUSHLOCK_EXCLUSIVE   exclusive owner present
     state >> 2                         shared-reader count

   Shared/exclusive uncontended acquire is one CAS. Contended paths park on a
   pthread condition variable. Waiting writers are counted separately and make
   new readers leave the fast path, which avoids unbounded writer starvation.
   This is still Linux userspace, not a kernel EX_PUSH_LOCK, but the data path
   is now the same idea: tiny word, interlocked fast path, parked waiters only
   when contention actually exists. */
#define MYWIN_PUSHLOCK_EXCLUSIVE  0x00000001u
#define MYWIN_PUSHLOCK_READER_INC 0x00000004u
#define MYWIN_PUSHLOCK_SPINS      64u

typedef struct MyWinPushLock {
    volatile DWORD state;
    volatile DWORD writer_waiters;
    volatile DWORD shared_waiters;
    pthread_mutex_t wait_lock;
    pthread_cond_t  wait_cond;
} MyWinPushLock;

static volatile DWORD g_PushLockSharedFast = 0;
static volatile DWORD g_PushLockSharedSlow = 0;
static volatile DWORD g_PushLockExclusiveFast = 0;
static volatile DWORD g_PushLockExclusiveSlow = 0;
static volatile DWORD g_PushLockWakeups = 0;
static volatile DWORD g_PushLockContentions = 0;

static void mywin_pushlock_cpu_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static void mywin_pushlock_init(MyWinPushLock* l)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    pthread_mutex_init(&l->wait_lock, NULL);
    pthread_cond_init(&l->wait_cond, NULL);
}

static void mywin_pushlock_destroy(MyWinPushLock* l)
{
    if (!l) return;
    pthread_cond_destroy(&l->wait_cond);
    pthread_mutex_destroy(&l->wait_lock);
}

static BOOL mywin_pushlock_try_acquire_shared(MyWinPushLock* l)
{
    if (!l) return TRUE;
    if (__atomic_load_n(&l->writer_waiters, __ATOMIC_ACQUIRE) != 0) return FALSE;
    DWORD s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
    while ((s & MYWIN_PUSHLOCK_EXCLUSIVE) == 0) {
        if (__atomic_load_n(&l->writer_waiters, __ATOMIC_ACQUIRE) != 0) return FALSE;
        if (__atomic_compare_exchange_n(&l->state, &s, s + MYWIN_PUSHLOCK_READER_INC, FALSE, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return TRUE;
    }
    return FALSE;
}

static BOOL mywin_pushlock_try_acquire_exclusive(MyWinPushLock* l)
{
    if (!l) return TRUE;
    DWORD expected = 0;
    return __atomic_compare_exchange_n(&l->state, &expected, MYWIN_PUSHLOCK_EXCLUSIVE, FALSE, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? TRUE : FALSE;
}

static void mywin_pushlock_acquire_shared(MyWinPushLock* l)
{
    if (!l) return;
    for (DWORD i = 0; i < MYWIN_PUSHLOCK_SPINS; ++i) {
        if (mywin_pushlock_try_acquire_shared(l)) { __atomic_add_fetch(&g_PushLockSharedFast, 1u, __ATOMIC_RELAXED); return; }
        mywin_pushlock_cpu_pause();
    }

    __atomic_add_fetch(&g_PushLockContentions, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_PushLockSharedSlow, 1u, __ATOMIC_RELAXED);
    pthread_mutex_lock(&l->wait_lock);
    __atomic_add_fetch(&l->shared_waiters, 1u, __ATOMIC_RELEASE);
    for (;;) {
        if (mywin_pushlock_try_acquire_shared(l)) break;
        pthread_cond_wait(&l->wait_cond, &l->wait_lock);
    }
    __atomic_sub_fetch(&l->shared_waiters, 1u, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&l->wait_lock);
}

static void mywin_pushlock_acquire_exclusive(MyWinPushLock* l)
{
    if (!l) return;
    for (DWORD i = 0; i < MYWIN_PUSHLOCK_SPINS; ++i) {
        if (mywin_pushlock_try_acquire_exclusive(l)) { __atomic_add_fetch(&g_PushLockExclusiveFast, 1u, __ATOMIC_RELAXED); return; }
        mywin_pushlock_cpu_pause();
    }

    __atomic_add_fetch(&g_PushLockContentions, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_PushLockExclusiveSlow, 1u, __ATOMIC_RELAXED);
    pthread_mutex_lock(&l->wait_lock);
    __atomic_add_fetch(&l->writer_waiters, 1u, __ATOMIC_RELEASE);
    for (;;) {
        if (mywin_pushlock_try_acquire_exclusive(l)) break;
        pthread_cond_wait(&l->wait_cond, &l->wait_lock);
    }
    __atomic_sub_fetch(&l->writer_waiters, 1u, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&l->wait_lock);
}

static void mywin_pushlock_release(MyWinPushLock* l)
{
    if (!l) return;
    DWORD prev = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
    BOOL last = FALSE;
    if (prev & MYWIN_PUSHLOCK_EXCLUSIVE) {
        __atomic_store_n(&l->state, 0u, __ATOMIC_RELEASE);
        last = TRUE;
    } else {
        DWORD old = __atomic_sub_fetch(&l->state, MYWIN_PUSHLOCK_READER_INC, __ATOMIC_RELEASE);
        last = (old == 0);
    }
    if (last && (__atomic_load_n(&l->writer_waiters, __ATOMIC_ACQUIRE) || __atomic_load_n(&l->shared_waiters, __ATOMIC_ACQUIRE))) {
        pthread_mutex_lock(&l->wait_lock);
        pthread_cond_broadcast(&l->wait_cond);
        pthread_mutex_unlock(&l->wait_lock);
        __atomic_add_fetch(&g_PushLockWakeups, 1u, __ATOMIC_RELAXED);
    }
}

typedef struct MyWinHandleEntry {
    int    valid;
    DWORD  pid;
    DWORD  slot;
    DWORD  generation;
    HANDLE handle;
    HANDLE objectHandle;
    DWORD  type;
    DWORD  object_slot;
    DWORD  access;
    BOOL   inherit;
    BOOL   protect_from_close;
} MyWinHandleEntry;

typedef struct MyWinHandleLeafPage {
    MyWinHandleEntry entry[MYWIN_HANDLE_INDEX_COUNT];
    /* v250: one bit per slot tracks whether the slot is already present on
       the per-process free stack.  That keeps duplicate-free protection O(1)
       and lets a TLS close->alloc hint consume the top free slot without
       leaving unbounded stale stack entries behind. */
    uint64_t free_mark[MYWIN_HANDLE_INDEX_COUNT / 64u];
} MyWinHandleLeafPage;

typedef struct MyWinHandleMidPage {
    MyWinHandleLeafPage* leaf[MYWIN_HANDLE_INDEX_COUNT];
} MyWinHandleMidPage;

typedef struct MyWinHandlePidTable {
    DWORD pid;
    DWORD count;
    DWORD peak_count;
    DWORD quota;
    DWORD quota_failures;
    DWORD alloc_hint;
    DWORD* free_stack;
    DWORD free_count;
    DWORD free_capacity;
    DWORD free_reuse_count;
    DWORD free_stack_grow_failures;
    volatile DWORD epoch;
    MyWinPushLock lock;
    struct MyWinHandlePidTable* next;
    struct MyWinHandlePidTable* hash_next;
    MyWinHandleMidPage* mid[MYWIN_HANDLE_INDEX_COUNT];
} MyWinHandlePidTable;

#define MYWIN_HANDLE_PID_HASH_BUCKETS 256u
#define MYWIN_HANDLE_PID_HASH_MASK (MYWIN_HANDLE_PID_HASH_BUCKETS - 1u)
#define MYWIN_HANDLE_FREE_STACK_INITIAL 4096u
static MyWinHandlePidTable* g_HandleTables = NULL;
static MyWinHandlePidTable* g_HandleTablePidHash[MYWIN_HANDLE_PID_HASH_BUCKETS];
static pthread_mutex_t g_HandleLock = PTHREAD_MUTEX_INITIALIZER;

/* v242: the table list/hash is process-global, but the hottest Win32 paths
   repeatedly resolve the current PID's table.  Tables are long-lived in the
   current runtime, so a one-entry TLS table cache removes the global mutex
   from DuplicateHandle/CloseHandle/Wait hot paths after the first lookup. */
static __thread DWORD g_HandleTableLookupCachePid = 0;
static __thread MyWinHandlePidTable* g_HandleTableLookupCache = NULL;

static DWORD g_HandleSweepCalls = 0;
static DWORD g_HandleSweepClosed = 0;
static DWORD g_HandleSweepFailures = 0;
static DWORD g_HandleSweepLastPid = 0;
static DWORD g_HandleDuplicateSuccess = 0;
static DWORD g_HandleDuplicateCrossProcess = 0;
static DWORD g_HandleDuplicateCloseSource = 0;
static DWORD g_HandleDuplicateFailures = 0;
static DWORD g_HandleDuplicateAccessDenied = 0;
static DWORD g_HandleDuplicateInvalidProcess = 0;

/* v213: thread-local last-handle cache now uses a per-process handle-table
   epoch instead of a single global epoch. Mutating one PID no longer invalidates
   another PID's cached handle lookup. */
static DWORD g_HandleCacheHits = 0;
static DWORD g_HandleCacheMisses = 0;
static DWORD g_HandleCacheStores = 0;
static DWORD g_HandleCacheInvalidations = 0;
/* v249: validate the TLS handle cache against the cached slot itself.
   An unrelated CloseHandle() still bumps the table epoch for diagnostics, but
   no longer destroys a hot source handle used by duplicate/wait loops. */
static DWORD g_HandleCacheEntryValidated = 0;
static DWORD g_HandleCacheEntryStale = 0;
/* v250: small direct-mapped TLS cache keeps several stable public handles hot.
   A one-entry cache works for DuplicateHandle(source) loops, but WFMO, message
   pumps and mixed USER/KERNEL paths often bounce between a handful of handles. */
#define MYWIN_HANDLE_TLS_CACHE_SETS 64u
#define MYWIN_HANDLE_TLS_CACHE_WAYS 4u
#define MYWIN_HANDLE_TLS_CACHE_SLOTS (MYWIN_HANDLE_TLS_CACHE_SETS * MYWIN_HANDLE_TLS_CACHE_WAYS)
#define MYWIN_HANDLE_TLS_CACHE_MASK  (MYWIN_HANDLE_TLS_CACHE_SETS - 1u)
static DWORD g_HandleCacheSlotProbes = 0;
static DWORD g_HandleCacheSlotCollisions = 0;
static DWORD g_HandleFreeHintHits = 0;
static DWORD g_HandleFreeHintMisses = 0;
static DWORD g_HandleFreeMarkDuplicateSkips = 0;
static DWORD g_HandleFreeStalePops = 0;
/* v255: one-slot TLS reuse is great for close->alloc pairs, but batchy GUI and
   wait code often closes several duplicates and then allocates several more.
   v256 widens that idea from one same-table batch to a tiny set of per-thread
   table lanes.  Alternating duplicates across current/child/foreign process
   handle tables can now keep several local free-slot stacks warm without
   flushing on every table switch.  Slots are still consumed while the owning
   process handle-table lock is held, so this is a locality/cache win rather
   than a semantic shortcut. */
#define MYWIN_HANDLE_FREE_BATCH_SLOTS 64u
#define MYWIN_HANDLE_FREE_BATCH_LANES 4u
static DWORD g_HandleFreeBatchHits = 0;
static DWORD g_HandleFreeBatchStores = 0;
static DWORD g_HandleFreeBatchFlushes = 0;
static DWORD g_HandleFreeBatchFlushedSlots = 0;
static DWORD g_HandleFreeBatchOverflow = 0;
static DWORD g_HandleFreeBatchMisses = 0;
static DWORD g_HandleFreeBatchLaneAllocs = 0;
static DWORD g_HandleFreeBatchLaneMatches = 0;
static DWORD g_HandleFreeBatchTableSwitchAvoided = 0;

static __thread MyWinHandlePidTable* g_HandleFreeHintTable = NULL;
static __thread DWORD g_HandleFreeHintSlot = 0;

typedef struct MyWinHandleFreeBatchLane {
    MyWinHandlePidTable* table;
    DWORD count;
    DWORD slots[MYWIN_HANDLE_FREE_BATCH_SLOTS];
} MyWinHandleFreeBatchLane;

typedef struct MyWinHandleFreeBatch {
    MyWinHandleFreeBatchLane lane[MYWIN_HANDLE_FREE_BATCH_LANES];
} MyWinHandleFreeBatch;

static pthread_key_t g_HandleFreeBatchKey;
static pthread_once_t g_HandleFreeBatchKeyOnce = PTHREAD_ONCE_INIT;
static __thread MyWinHandleFreeBatch* g_HandleFreeBatchTls = NULL;

typedef struct MyWinHandleLookupCache {
    BOOL valid;
    DWORD epoch;
    DWORD pid;
    HANDLE handle;
    HANDLE objectHandle;
    DWORD type;
    DWORD object_slot;
    DWORD access;
    struct MyWinHandlePidTable* table;
} MyWinHandleLookupCache;

static __thread MyWinHandleLookupCache g_HandleLookupCache[MYWIN_HANDLE_TLS_CACHE_SLOTS];
static __thread unsigned char g_HandleLookupCacheNext[MYWIN_HANDLE_TLS_CACHE_SETS];

static inline DWORD mywin_handle_cache_tls_index(DWORD pid, HANDLE h)
{
    DWORD v = ((DWORD)h & MYWIN_HANDLE_SLOT_MASK) ^ (((DWORD)h >> 16) & 0xffffu) ^ (pid * 33u);
    v ^= v >> 8;
    return v & MYWIN_HANDLE_TLS_CACHE_MASK;
}

static DWORD mywin_handle_table_epoch_load(MyWinHandlePidTable* table)
{
    if (!table) return 0;
    return __atomic_load_n(&table->epoch, __ATOMIC_ACQUIRE);
}

static void mywin_handle_cache_invalidate_table(MyWinHandlePidTable* table)
{
    if (!table) return;
    DWORD v = __atomic_add_fetch(&table->epoch, 1u, __ATOMIC_RELEASE);
    if (!v) __atomic_store_n(&table->epoch, 1u, __ATOMIC_RELEASE);
    /* v249: do not blindly clear this thread's TLS cache on every table
       mutation.  Lookup validates the cached slot/generation/object tuple, so
       closing a different handle no longer evicts a still-live source handle. */
    __atomic_add_fetch(&g_HandleCacheInvalidations, 1u, __ATOMIC_RELAXED);
}

static DWORD mywin_cached_object_slot(HANDLE objectHandle, DWORD type)
{
    DWORD decodedType = 0, decodedSlot = 0;
    if (objectHandle && _ObjectDecodeSlotHandle(objectHandle, &decodedType, &decodedSlot) && decodedType == type)
        return decodedSlot;
    return 0xffffffffu;
}

static MyWinHandleEntry* mywin_handle_cache_entry_fast(MyWinHandlePidTable* table, DWORD slot)
{
    if (!table || slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS) return NULL;
    DWORD z = slot - 1u;
    DWORD top = (z >> 16) & 0xffu;
    DWORD midIdx = (z >> 8) & 0xffu;
    DWORD leafIdx = z & 0xffu;
    MyWinHandleMidPage* mid = __atomic_load_n(&table->mid[top], __ATOMIC_ACQUIRE);
    if (!mid) return NULL;
    MyWinHandleLeafPage* leaf = __atomic_load_n(&mid->leaf[midIdx], __ATOMIC_ACQUIRE);
    if (!leaf) return NULL;
    return &leaf->entry[leafIdx];
}

static BOOL mywin_handle_cache_validate_entry(MyWinHandleLookupCache* c, DWORD pid, HANDLE h,
                                              DWORD* type, DWORD* objectSlot, DWORD* access, HANDLE* objOut)
{
    if (!c || !c->valid || c->pid != pid || c->handle != h || !c->table || !c->objectHandle) return FALSE;
    DWORD slot = ((DWORD)h & MYWIN_HANDLE_SLOT_MASK);
    DWORD gen = (((DWORD)h >> MYWIN_HANDLE_SLOT_BITS) & MYWIN_HANDLE_GENERATION_MASK);
    if (!slot || !gen) return FALSE;
    MyWinHandleEntry* e = mywin_handle_cache_entry_fast(c->table, slot);
    if (!e) return FALSE;

    int valid = __atomic_load_n(&e->valid, __ATOMIC_ACQUIRE);
    DWORD epid = __atomic_load_n(&e->pid, __ATOMIC_ACQUIRE);
    DWORD egen = __atomic_load_n(&e->generation, __ATOMIC_ACQUIRE);
    HANDLE eh = __atomic_load_n(&e->handle, __ATOMIC_ACQUIRE);
    HANDLE obj = __atomic_load_n(&e->objectHandle, __ATOMIC_ACQUIRE);
    DWORD etype = __atomic_load_n(&e->type, __ATOMIC_ACQUIRE);
    DWORD eslot = __atomic_load_n(&e->object_slot, __ATOMIC_ACQUIRE);
    DWORD eaccess = __atomic_load_n(&e->access, __ATOMIC_ACQUIRE);

    if (MYOS_LIKELY(valid && epid == pid && egen == gen && eh == h && obj == c->objectHandle &&
                    etype == c->type && eslot == c->object_slot)) {
        if (type) *type = etype;
        if (objectSlot) *objectSlot = eslot;
        if (access) *access = eaccess;
        if (objOut) *objOut = obj;
        c->access = eaccess;
        __atomic_add_fetch(&g_HandleCacheEntryValidated, 1u, __ATOMIC_RELAXED);
        return TRUE;
    }

    c->valid = FALSE;
    __atomic_add_fetch(&g_HandleCacheEntryStale, 1u, __ATOMIC_RELAXED);
    return FALSE;
}

static BOOL mywin_handle_cache_lookup_ex(DWORD pid, HANDLE h, DWORD* type, DWORD* objectSlot, DWORD* access, HANDLE* objOut)
{
    DWORD set = mywin_handle_cache_tls_index(pid, h);
    DWORD base = set * MYWIN_HANDLE_TLS_CACHE_WAYS;
    BOOL sawValid = FALSE;
    __atomic_add_fetch(&g_HandleCacheSlotProbes, 1u, __ATOMIC_RELAXED);
    for (DWORD way = 0; way < MYWIN_HANDLE_TLS_CACHE_WAYS; ++way) {
        MyWinHandleLookupCache* c = &g_HandleLookupCache[base + way];
        if (!c->valid) continue;
        sawValid = TRUE;
        if (MYOS_LIKELY(c->pid == pid && c->handle == h)) {
            if (mywin_handle_cache_validate_entry(c, pid, h, type, objectSlot, access, objOut)) {
                __atomic_add_fetch(&g_HandleCacheHits, 1u, __ATOMIC_RELAXED);
                return TRUE;
            }
        }
    }
    if (sawValid) __atomic_add_fetch(&g_HandleCacheSlotCollisions, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_HandleCacheMisses, 1u, __ATOMIC_RELAXED);
    return FALSE;
}
static BOOL mywin_handle_cache_lookup(DWORD pid, HANDLE h, DWORD* type, DWORD* access, HANDLE* objOut)
{
    return mywin_handle_cache_lookup_ex(pid, h, type, NULL, access, objOut);
}

static void mywin_handle_cache_store_ex(MyWinHandlePidTable* table, DWORD pid, HANDLE h, HANDLE objectHandle, DWORD type, DWORD objectSlot, DWORD access)
{
    if (!table || !pid || !h || !objectHandle) return;
    DWORD set = mywin_handle_cache_tls_index(pid, h);
    DWORD base = set * MYWIN_HANDLE_TLS_CACHE_WAYS;
    MyWinHandleLookupCache* c = NULL;
    DWORD slot = ((DWORD)h & MYWIN_HANDLE_SLOT_MASK);
    for (DWORD way = 0; way < MYWIN_HANDLE_TLS_CACHE_WAYS; ++way) {
        MyWinHandleLookupCache* probe = &g_HandleLookupCache[base + way];
        if (!probe->valid) { c = probe; break; }
        if (probe->pid == pid && probe->handle == h) { c = probe; break; }
        if (!c && probe->table == table && probe->handle) {
            DWORD pslot = ((DWORD)probe->handle & MYWIN_HANDLE_SLOT_MASK);
            if (pslot == slot) c = probe;
        }
    }
    if (!c) {
        DWORD way = (DWORD)(g_HandleLookupCacheNext[set]++ & (MYWIN_HANDLE_TLS_CACHE_WAYS - 1u));
        c = &g_HandleLookupCache[base + way];
        if (c->valid && (c->pid != pid || c->handle != h))
            __atomic_add_fetch(&g_HandleCacheSlotCollisions, 1u, __ATOMIC_RELAXED);
    }
    c->valid = TRUE;
    c->epoch = mywin_handle_table_epoch_load(table);
    c->pid = pid;
    c->handle = h;
    c->objectHandle = objectHandle;
    c->type = type;
    c->object_slot = objectSlot;
    c->access = access;
    c->table = table;
    __atomic_add_fetch(&g_HandleCacheStores, 1u, __ATOMIC_RELAXED);
}



#define MYWIN_MAX_LITE_PROCESSES 64
typedef struct MyWinLoadedModule {
    int valid;
    HMODULE module;
    DWORD refCount;
    char moduleName[MYWIN_MAX_MODULE_NAME];
    char imagePath[MYWIN_MAX_MODULE_PATH];
} MyWinLoadedModule;

typedef struct MyWinProcessLite {
    int valid;
    DWORD pid;
    DWORD parentPid;
    DWORD tid;
    /* v254: PID/TID index metadata.  ProcessLite records are now resolved
       through power-of-two hash buckets instead of table scans on hot
       OpenProcess/OpenThread/GetExitCode/Wait probes. */
    DWORD pidHash;
    DWORD tidHash;
    int   pidHashNext;
    int   tidHashNext;
    int   pidIndexed;
    int   tidIndexed;
    HANDLE processObject;
    HANDLE threadObject;
    DWORD flags;
    DWORD exitCode;
    DWORD inheritedHandles;
    DWORD duplicatedIn;
    DWORD runtimeEnters;
    Capability cap;
    int hasCap;
    DWORD startupFlags;
    DWORD startupX;
    DWORD startupY;
    DWORD startupW;
    DWORD startupH;
    WORD  showWindow;
    HANDLE stdInput;
    HANDLE stdOutput;
    HANDLE stdError;
    char imageName[64];
    char imagePath[MYWIN_MAX_MODULE_PATH];
    char moduleName[MYWIN_MAX_MODULE_NAME];
    HMODULE mainModule;
    char commandLine[128];
    char currentDirectory[128];
    char windowTitle[64];
    DWORD environmentCount;
    char envName[MYWIN_MAX_ENV_VARS][MYWIN_MAX_ENV_NAME];
    char envValue[MYWIN_MAX_ENV_VARS][MYWIN_MAX_ENV_VALUE];
    MyWinLoadedModule loadedModules[MYWIN_MAX_LOADED_MODULES];
    DWORD moduleSerial;
    char dllDirectory[MYWIN_MAX_MODULE_PATH];
    DWORD lastError;
    DWORD loaderImportCount;
    DWORD loaderResolvedCount;
    DWORD loaderEntryCalled;
    DWORD loaderError;
    char loaderEntry[MYWIN_MAX_LOADER_ENTRY];
    char loaderImportPreview[MYWIN_MAX_LOADER_PREVIEW];
    char subsystem[MYWIN_MAX_SUBSYSTEM];
    DWORD argc;
    char argvPreview[MYWIN_MAX_ARGV_PREVIEW];
    DWORD consoleExitCode;
    int   linuxPid;          // v58: ProcessHost/IPc-backed Linux child pid for console subsystem
    DWORD linuxStatus;       // raw waitpid status for diagnostics
    DWORD forkExec;          // 1 when backed by a real Linux child process
    DWORD objectLifetimeReleased; // v181: kernel live ref released once on process exit
    MyWinDispatcherHeader processDispatcher; // v248: common dispatcher header for PROCESS handles
    MyWinDispatcherHeader threadDispatcher;  // v248: common dispatcher header for THREAD handles
} MyWinProcessLite;

static MyWinProcessLite g_LiteProcesses[MYWIN_MAX_LITE_PROCESSES];
static pthread_mutex_t g_ProcessLock = PTHREAD_MUTEX_INITIALIZER;
static DWORD g_NextLitePid = 3000;

#define MYWIN_PROCESS_INDEX_BUCKETS 128u
#define MYWIN_PROCESS_INDEX_MASK (MYWIN_PROCESS_INDEX_BUCKETS - 1u)
static int g_LiteProcessPidHash[MYWIN_PROCESS_INDEX_BUCKETS];
static int g_LiteProcessTidHash[MYWIN_PROCESS_INDEX_BUCKETS];
static DWORD g_LiteProcessAllocHint = 0;
static DWORD g_ProcessIndexPidHits = 0;
static DWORD g_ProcessIndexPidMisses = 0;
static DWORD g_ProcessIndexTidHits = 0;
static DWORD g_ProcessIndexTidMisses = 0;
static DWORD g_ProcessIndexPidInserts = 0;
static DWORD g_ProcessIndexTidInserts = 0;
static DWORD g_ProcessIndexAllocFast = 0;
static DWORD g_ProcessIndexAllocFallback = 0;
static DWORD g_ProcessIndexFallbackScans = 0;

static void mywin_process_dispatcher_refresh_locked(MyWinProcessLite* p)
{
    if (!p || !p->valid) return;
    LONG state = (p->flags & MYWIN_PROCESS_EXITED) ? 1 : 0;
    if (p->processObject) mywin_dispatcher_header_bind(&p->processDispatcher, p->processObject, _OBJECT_TYPE_PROCESS, state);
    if (p->threadObject) mywin_dispatcher_header_bind(&p->threadDispatcher, p->threadObject, _OBJECT_TYPE_THREAD, state);
}

static MyWinProcessLite* mywin_lite_process_from_object_unlocked(HANDLE objectHandle, DWORD objectType)
{
    DWORD type = 0, slot = 0;
    if (_ObjectDecodeSlotHandle(objectHandle, &type, &slot) && type == objectType && slot < MYWIN_MAX_LITE_PROCESSES) {
        MyWinProcessLite* p = &g_LiteProcesses[slot];
        if (p->valid) {
            if (objectType == _OBJECT_TYPE_PROCESS && p->processObject == objectHandle) return p;
            if (objectType == _OBJECT_TYPE_THREAD && p->threadObject == objectHandle) return p;
        }
        return NULL;
    }
    for (int i = 0; i < MYWIN_MAX_LITE_PROCESSES; ++i) {
        MyWinProcessLite* p = &g_LiteProcesses[i];
        if (!p->valid) continue;
        if (objectType == _OBJECT_TYPE_PROCESS && p->processObject == objectHandle) return p;
        if (objectType == _OBJECT_TYPE_THREAD && p->threadObject == objectHandle) return p;
    }
    return NULL;
}

static MyWinWaitBlock** mywin_process_thread_wait_head_for_object_locked(HANDLE objectHandle, DWORD objectType)
{
    MyWinProcessLite* p = mywin_lite_process_from_object_unlocked(objectHandle, objectType);
    if (!p) return NULL;
    return (objectType == _OBJECT_TYPE_PROCESS)
        ? mywin_dispatcher_header_wait_head(&p->processDispatcher, objectHandle, objectType)
        : mywin_dispatcher_header_wait_head(&p->threadDispatcher, objectHandle, objectType);
}

static BOOL mywin_wait_type_is_process_or_thread(DWORD type)
{
    return (type == _OBJECT_TYPE_PROCESS || type == _OBJECT_TYPE_THREAD) ? TRUE : FALSE;
}

DWORD mywin_current_pid(void) { return g_HasCapability ? g_CurrentCapability.id : 0; }
static HANDLE mywin_make_user_handle(DWORD slot, DWORD generation)
{
    if (slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS) return 0;
    generation &= MYWIN_HANDLE_GENERATION_MASK;
    if (!generation) generation = 1;
    return (HANDLE)(MYWIN_HANDLE_TAG | (generation << MYWIN_HANDLE_SLOT_BITS) | (slot & MYWIN_HANDLE_SLOT_MASK));
}
static DWORD mywin_handle_slot(HANDLE h)
{
    return (((DWORD)h & MYWIN_HANDLE_TAG) == MYWIN_HANDLE_TAG) ? ((DWORD)h & MYWIN_HANDLE_SLOT_MASK) : 0;
}
static DWORD mywin_handle_generation(HANDLE h)
{
    return (((DWORD)h & MYWIN_HANDLE_TAG) == MYWIN_HANDLE_TAG) ? (((DWORD)h >> MYWIN_HANDLE_SLOT_BITS) & MYWIN_HANDLE_GENERATION_MASK) : 0;
}
static HMODULE mywin_make_module_handle(DWORD pid) { return (HMODULE)(0x52000000u | (pid & 0x00ffffffu)); }
static HMODULE mywin_make_dll_module_handle(DWORD pid, DWORD slot) { return (HMODULE)(0x53000000u | ((pid & 0x0fffu) << 12) | (slot & 0x0fffu)); }
static DWORD mywin_pid_from_dll_module_handle(HMODULE hModule) { return ((hModule & 0xff000000u) == 0x53000000u) ? ((hModule >> 12) & 0x0fffu) : 0; }
static DWORD mywin_slot_from_dll_module_handle(HMODULE hModule) { return ((hModule & 0xff000000u) == 0x53000000u) ? (hModule & 0x0fffu) : 0; }
/* AUDIT(v118): Fallback LastError is global for calls outside a process context.
   MSDN semantics are per-thread; strict mode should make this TLS and ensure
   every public failure path sets an error. */
static DWORD g_LastErrorFallback = ERROR_SUCCESS;
static DWORD mywin_process_handle_count(DWORD pid);
static HANDLE mywin_duplicate_handle_to_pid(DWORD srcPid, DWORD dstPid, HANDLE hSourceHandle, BOOL inherit, DWORD* errorOut);
static void mywin_env_defaults_locked(MyWinProcessLite* p);
static void mywin_env_init_for_child_locked(MyWinProcessLite* child, DWORD parentPid, LPVOID lpEnvironment);
static void mywin_env_preview_locked(const MyWinProcessLite* p, char* out, size_t cb);
static void mywin_dll_preview_locked(const MyWinProcessLite* p, char* out, size_t cb);
static void mywin_module_init_locked(MyWinProcessLite* p, LPCSTR imageName);

static inline DWORD mywin_process_index_hash(DWORD id)
{
    DWORD v = id * 2654435761u;
    v ^= v >> 16;
    return v;
}

static inline int mywin_process_index_bucket(DWORD hash)
{
    return (int)(hash & MYWIN_PROCESS_INDEX_MASK);
}

static void mywin_process_index_unlink_pid_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_LITE_PROCESSES) return;
    MyWinProcessLite* p = &g_LiteProcesses[idx];
    if (!p->pidIndexed) return;
    int b = mywin_process_index_bucket(p->pidHash);
    int* link = &g_LiteProcessPidHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = p->pidHashNext; break; }
        if (cur < 0 || cur >= MYWIN_MAX_LITE_PROCESSES) break;
        link = &g_LiteProcesses[cur].pidHashNext;
    }
    p->pidHashNext = 0;
    p->pidIndexed = 0;
    p->pidHash = 0;
}

static void mywin_process_index_unlink_tid_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_LITE_PROCESSES) return;
    MyWinProcessLite* p = &g_LiteProcesses[idx];
    if (!p->tidIndexed) return;
    int b = mywin_process_index_bucket(p->tidHash);
    int* link = &g_LiteProcessTidHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = p->tidHashNext; break; }
        if (cur < 0 || cur >= MYWIN_MAX_LITE_PROCESSES) break;
        link = &g_LiteProcesses[cur].tidHashNext;
    }
    p->tidHashNext = 0;
    p->tidIndexed = 0;
    p->tidHash = 0;
}

static void mywin_process_index_insert_pid_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_LITE_PROCESSES) return;
    MyWinProcessLite* p = &g_LiteProcesses[idx];
    if (!p->valid || !p->pid) return;
    DWORD h = mywin_process_index_hash(p->pid);
    if (p->pidIndexed && p->pidHash == h) return;
    mywin_process_index_unlink_pid_locked(idx);
    int b = mywin_process_index_bucket(h);
    p->pidHash = h;
    p->pidHashNext = g_LiteProcessPidHash[b];
    p->pidIndexed = 1;
    g_LiteProcessPidHash[b] = idx + 1;
    g_ProcessIndexPidInserts++;
}

static void mywin_process_index_insert_tid_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_LITE_PROCESSES) return;
    MyWinProcessLite* p = &g_LiteProcesses[idx];
    if (!p->valid || !p->tid) return;
    DWORD h = mywin_process_index_hash(p->tid);
    if (p->tidIndexed && p->tidHash == h) return;
    mywin_process_index_unlink_tid_locked(idx);
    int b = mywin_process_index_bucket(h);
    p->tidHash = h;
    p->tidHashNext = g_LiteProcessTidHash[b];
    p->tidIndexed = 1;
    g_LiteProcessTidHash[b] = idx + 1;
    g_ProcessIndexTidInserts++;
}

static void mywin_process_index_refresh_locked(MyWinProcessLite* p)
{
    if (!p) return;
    int idx = (int)(p - g_LiteProcesses);
    mywin_process_index_insert_pid_locked(idx);
    mywin_process_index_insert_tid_locked(idx);
}

static MyWinProcessLite* mywin_find_lite_process_locked(DWORD pid)
{
    if (!pid) return NULL;
    DWORD h = mywin_process_index_hash(pid);
    int b = mywin_process_index_bucket(h);
    for (int link = g_LiteProcessPidHash[b]; link; link = g_LiteProcesses[link - 1].pidHashNext) {
        int idx = link - 1;
        if (MYOS_LIKELY(idx >= 0 && idx < MYWIN_MAX_LITE_PROCESSES)) {
            MyWinProcessLite* p = &g_LiteProcesses[idx];
            if (MYOS_LIKELY(p->valid && p->pidIndexed) && p->pidHash == h && p->pid == pid) {
                g_ProcessIndexPidHits++;
                return p;
            }
        }
    }

    /* Conservative repair path for records created before the v254 index is
       warmed, or for future diagnostic table surgery.  Normal runtime lookup is
       the hash path above. */
    g_ProcessIndexPidMisses++;
    g_ProcessIndexFallbackScans++;
    for (int i = 0; i < MYWIN_MAX_LITE_PROCESSES; i++) {
        if (g_LiteProcesses[i].valid && g_LiteProcesses[i].pid == pid) {
            mywin_process_index_refresh_locked(&g_LiteProcesses[i]);
            return &g_LiteProcesses[i];
        }
    }
    return NULL;
}

static MyWinProcessLite* mywin_find_lite_thread_locked(DWORD tid)
{
    if (!tid) return NULL;
    DWORD h = mywin_process_index_hash(tid);
    int b = mywin_process_index_bucket(h);
    for (int link = g_LiteProcessTidHash[b]; link; link = g_LiteProcesses[link - 1].tidHashNext) {
        int idx = link - 1;
        if (MYOS_LIKELY(idx >= 0 && idx < MYWIN_MAX_LITE_PROCESSES)) {
            MyWinProcessLite* p = &g_LiteProcesses[idx];
            if (MYOS_LIKELY(p->valid && p->tidIndexed) && p->tidHash == h && p->tid == tid) {
                g_ProcessIndexTidHits++;
                return p;
            }
        }
    }

    g_ProcessIndexTidMisses++;
    g_ProcessIndexFallbackScans++;
    for (int i = 0; i < MYWIN_MAX_LITE_PROCESSES; i++) {
        if (g_LiteProcesses[i].valid && g_LiteProcesses[i].tid == tid) {
            mywin_process_index_refresh_locked(&g_LiteProcesses[i]);
            return &g_LiteProcesses[i];
        }
    }
    return NULL;
}

static MyWinProcessLite* mywin_alloc_lite_process_locked(DWORD pid)
{
    MyWinProcessLite* existing = mywin_find_lite_process_locked(pid);
    if (existing) return existing;

    for (int pass = 0; pass < 2; ++pass) {
        DWORD start = pass == 0 ? g_LiteProcessAllocHint : 0;
        DWORD end = pass == 0 ? MYWIN_MAX_LITE_PROCESSES : g_LiteProcessAllocHint;
        for (DWORD i = start; i < end; ++i) {
            if (!g_LiteProcesses[i].valid) {
                memset(&g_LiteProcesses[i], 0, sizeof(g_LiteProcesses[i]));
                g_LiteProcesses[i].valid = 1;
                g_LiteProcesses[i].pid = pid;
                g_LiteProcesses[i].tid = pid;
                g_LiteProcesses[i].flags = MYWIN_PROCESS_LIVE;
                g_LiteProcesses[i].exitCode = STILL_ACTIVE;
                g_LiteProcesses[i].lastError = ERROR_SUCCESS;
                snprintf(g_LiteProcesses[i].subsystem, sizeof(g_LiteProcesses[i].subsystem), "process");
                g_LiteProcesses[i].consoleExitCode = STILL_ACTIVE;
                g_LiteProcesses[i].linuxPid = 0;
                g_LiteProcesses[i].linuxStatus = 0;
                g_LiteProcesses[i].forkExec = 0;
                g_LiteProcesses[i].objectLifetimeReleased = 0;
                snprintf(g_LiteProcesses[i].imageName, sizeof(g_LiteProcesses[i].imageName), "process-lite");
                mywin_module_init_locked(&g_LiteProcesses[i], "process-lite");
                g_LiteProcessAllocHint = i + 1u;
                if (g_LiteProcessAllocHint >= MYWIN_MAX_LITE_PROCESSES) g_LiteProcessAllocHint = MYWIN_MAX_LITE_PROCESSES;
                if (pass == 0) g_ProcessIndexAllocFast++;
                else g_ProcessIndexAllocFallback++;
                mywin_process_index_refresh_locked(&g_LiteProcesses[i]);
                return &g_LiteProcesses[i];
            }
        }
    }
    g_ProcessIndexAllocFallback++;
    return NULL;
}

static HANDLE mywin_process_object_for_pid_locked(DWORD pid)
{
    MyWinProcessLite* p = mywin_find_lite_process_locked(pid);
    return p ? p->processObject : 0;
}

static HANDLE mywin_thread_object_for_tid_locked(DWORD tid)
{
    MyWinProcessLite* p = mywin_find_lite_thread_locked(tid);
    return p ? p->threadObject : 0;
}

static HANDLE mywin_process_object_for_pid(DWORD pid)
{
    HANDLE h = 0;
    pthread_mutex_lock(&g_ProcessLock);
    h = mywin_process_object_for_pid_locked(pid);
    pthread_mutex_unlock(&g_ProcessLock);
    return h;
}

static HANDLE mywin_thread_object_for_tid(DWORD tid)
{
    HANDLE h = 0;
    pthread_mutex_lock(&g_ProcessLock);
    h = mywin_thread_object_for_tid_locked(tid);
    pthread_mutex_unlock(&g_ProcessLock);
    return h;
}

BOOL mywin_ensure_runtime_process_objects(const Capability* cap, HANDLE* processObject, HANDLE* threadObject)
{
    if (processObject) *processObject = 0;
    if (threadObject) *threadObject = 0;
    if (!cap || !cap->id) return FALSE;

    HANDLE procObj = 0, threadObj = 0;
    DWORD slot = 0xffffffffu;

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_alloc_lite_process_locked(cap->id);
    if (p) {
        slot = (DWORD)(p - g_LiteProcesses);
        if (slot < _OBJECT_SLOT_STRIDE) {
            procObj = _ObjectMakeSlotHandle(_OBJECT_TYPE_PROCESS, slot);
            threadObj = _ObjectMakeSlotHandle(_OBJECT_TYPE_THREAD, slot);
            p->processObject = procObj;
            p->threadObject = threadObj;
            p->pid = cap->id;
            if (!p->tid) p->tid = cap->id;
            mywin_process_index_refresh_locked(p);
            mywin_process_dispatcher_refresh_locked(p);
        }
    }
    pthread_mutex_unlock(&g_ProcessLock);

    if (!procObj || !threadObj) return FALSE;

    _ObjectectInfo oi;
    if (!_ObjectGetInfo(procObj, &oi)) {
        _ObjectRegister(procObj, _OBJECT_TYPE_PROCESS, cap->id,
                      _OBJECT_ACCESS_READ|_OBJECT_ACCESS_CONTROL|_OBJECT_ACCESS_SIGNAL, 0, cap->name);
        _ObjectSetInfo(procObj, 0, STILL_ACTIVE, cap->name);
    }

    char threadName[96];
    snprintf(threadName, sizeof(threadName), "%s!ui", cap->name[0] ? cap->name : "runtime");
    if (!_ObjectGetInfo(threadObj, &oi)) {
        _ObjectRegister(threadObj, _OBJECT_TYPE_THREAD, cap->id,
                      _OBJECT_ACCESS_READ|_OBJECT_ACCESS_CONTROL|_OBJECT_ACCESS_SIGNAL, 0, threadName);
        _ObjectSetInfo(threadObj, 0, STILL_ACTIVE, threadName);
    }

    if (processObject) *processObject = procObj;
    if (threadObject) *threadObject = threadObj;
    return TRUE;
}

void mywin_note_runtime_process(const Capability* cap, HANDLE processObject, HANDLE threadObject, const char* imageName)
{
    if (!cap || !cap->id) return;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_alloc_lite_process_locked(cap->id);
    if (p) {
        p->pid = cap->id;
        if (!p->tid) p->tid = cap->id;
        mywin_process_index_refresh_locked(p);
        if (!p->processObject) p->processObject = processObject;
        if (!p->threadObject) p->threadObject = threadObject;
        if (!(p->flags & MYWIN_PROCESS_EXITED)) p->flags = MYWIN_PROCESS_LIVE;
        if (p->exitCode == 0) p->exitCode = STILL_ACTIVE;
        mywin_process_dispatcher_refresh_locked(p);
        p->cap = *cap;
        p->hasCap = 1;
        snprintf(p->imageName, sizeof(p->imageName), "%s", imageName && imageName[0] ? imageName : cap->name);
        mywin_module_init_locked(p, p->imageName);
        mywin_env_defaults_locked(p);
    }
    pthread_mutex_unlock(&g_ProcessLock);
}

static BOOL mywin_copy_process_capability(DWORD pid, Capability* out)
{
    BOOL ok = FALSE;
    if (out) memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(pid);
    if (p && p->hasCap) {
        if (out) *out = p->cap;
        ok = TRUE;
    }
    pthread_mutex_unlock(&g_ProcessLock);
    return ok;
}

BOOL MyWinEnterProcessContext(DWORD dwProcessId)
{
    if (!dwProcessId || g_RuntimeDepth >= MYWIN_MAX_RUNTIME_STACK) return FALSE;
    Capability cap;
    if (!mywin_copy_process_capability(dwProcessId, &cap)) return FALSE;

    g_RuntimeStack[g_RuntimeDepth].hwndManager = g_lpHwndManager;
    g_RuntimeStack[g_RuntimeDepth].windowManager = g_lpWindowManager;
    g_RuntimeStack[g_RuntimeDepth].capability = g_CurrentCapability;
    g_RuntimeStack[g_RuntimeDepth].hasCapability = g_HasCapability;
    g_RuntimeDepth++;

    g_CurrentCapability = cap;
    g_HasCapability = 1;

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(dwProcessId);
    if (p) p->runtimeEnters++;
    pthread_mutex_unlock(&g_ProcessLock);
    return TRUE;
}

BOOL MyWinLeaveProcessContext(void)
{
    if (g_RuntimeDepth <= 0) return FALSE;
    g_RuntimeDepth--;
    g_lpHwndManager = g_RuntimeStack[g_RuntimeDepth].hwndManager;
    g_lpWindowManager = g_RuntimeStack[g_RuntimeDepth].windowManager;
    g_CurrentCapability = g_RuntimeStack[g_RuntimeDepth].capability;
    g_HasCapability = g_RuntimeStack[g_RuntimeDepth].hasCapability;
    memset(&g_RuntimeStack[g_RuntimeDepth], 0, sizeof(g_RuntimeStack[g_RuntimeDepth]));
    return TRUE;
}

DWORD MyWinGetRuntimeContextDepth(void)
{
    return (DWORD)g_RuntimeDepth;
}


static MyWinProcessLite* mywin_current_lite_process_locked(void)
{
    DWORD pid = mywin_current_pid();
    if (!pid) return NULL;
    return mywin_find_lite_process_locked(pid);
}

static void mywin_set_last_error_locked(MyWinProcessLite* p, DWORD dwErrCode)
{
    if (p) p->lastError = dwErrCode;
    else g_LastErrorFallback = dwErrCode;
}

static void mywin_set_last_error(DWORD dwErrCode)
{
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    mywin_set_last_error_locked(p, dwErrCode);
    pthread_mutex_unlock(&g_ProcessLock);
}

DWORD GetLastError(void)
{
    DWORD ret;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    ret = p ? p->lastError : g_LastErrorFallback;
    pthread_mutex_unlock(&g_ProcessLock);
    return ret;
}


void SetLastError(DWORD dwErrCode)
{
    mywin_set_last_error(dwErrCode);
}


BOOL IsValidSid(PSID pSid)
{
    if (!pSid) return FALSE;
    if (pSid->Revision != SID_REVISION) return FALSE;
    if (pSid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES) return FALSE;
    return TRUE;
}

DWORD GetLengthSid(PSID pSid)
{
    if (!IsValidSid(pSid)) return 0;
    return (DWORD)(offsetof(SID, SubAuthority) + ((size_t)pSid->SubAuthorityCount * sizeof(DWORD)));
}

BOOL EqualSid(PSID pSid1, PSID pSid2)
{
    DWORD cb1 = GetLengthSid(pSid1);
    DWORD cb2 = GetLengthSid(pSid2);
    if (!cb1 || cb1 != cb2) return FALSE;
    return memcmp(pSid1, pSid2, cb1) == 0 ? TRUE : FALSE;
}

BOOL CopySid(DWORD nDestinationSidLength, PSID pDestinationSid, PSID pSourceSid)
{
    DWORD cb = GetLengthSid(pSourceSid);
    if (!pDestinationSid || !cb) { mywin_set_last_error(ERROR_INVALID_SID); return FALSE; }
    if (nDestinationSidLength < cb) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset(pDestinationSid, 0, nDestinationSidLength);
    memcpy(pDestinationSid, pSourceSid, cb);
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL InitializeSid(PSID Sid, PSID_IDENTIFIER_AUTHORITY pIdentifierAuthority, BYTE nSubAuthorityCount)
{
    if (!Sid || !pIdentifierAuthority || nSubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
        mywin_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(Sid, 0, sizeof(SID));
    Sid->Revision = SID_REVISION;
    Sid->SubAuthorityCount = nSubAuthorityCount;
    Sid->IdentifierAuthority = *pIdentifierAuthority;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

PDWORD GetSidSubAuthority(PSID pSid, DWORD nSubAuthority)
{
    if (!IsValidSid(pSid) || nSubAuthority >= pSid->SubAuthorityCount) {
        mywin_set_last_error(ERROR_INVALID_SID);
        return NULL;
    }
    return &pSid->SubAuthority[nSubAuthority];
}

static BOOL mywin_sd_valid_header(PSECURITY_DESCRIPTOR sd);
static PSID mywin_sd_sid_field(PSECURITY_DESCRIPTOR sd, PSID field);
static PACL mywin_sd_acl_field(PSECURITY_DESCRIPTOR sd, PACL field);
static DWORD mywin_acl_used_size(PACL acl);

BOOL InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR pSecurityDescriptor, DWORD dwRevision)
{
    if (!pSecurityDescriptor || dwRevision != SECURITY_DESCRIPTOR_REVISION) {
        mywin_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(pSecurityDescriptor, 0, sizeof(*pSecurityDescriptor));
    pSecurityDescriptor->Revision = SECURITY_DESCRIPTOR_REVISION;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, BOOL bDaclPresent, PACL pDacl, BOOL bDaclDefaulted)
{
    if (!pSecurityDescriptor || pSecurityDescriptor->Revision != SECURITY_DESCRIPTOR_REVISION) {
        mywin_set_last_error(ERROR_INVALID_SECURITY_DESCR);
        return FALSE;
    }
    if (pDacl && (pDacl->AclRevision < ACL_REVISION || pDacl->AclSize < sizeof(ACL))) {
        mywin_set_last_error(ERROR_INVALID_ACL);
        return FALSE;
    }
    if (bDaclPresent) pSecurityDescriptor->Control |= SE_DACL_PRESENT;
    else pSecurityDescriptor->Control &= (SECURITY_DESCRIPTOR_CONTROL)~SE_DACL_PRESENT;
    if (bDaclDefaulted) pSecurityDescriptor->Control |= SE_DACL_DEFAULTED;
    else pSecurityDescriptor->Control &= (SECURITY_DESCRIPTOR_CONTROL)~SE_DACL_DEFAULTED;
    pSecurityDescriptor->Dacl = pDacl;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}


BOOL IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR pSecurityDescriptor)
{
    if (!mywin_sd_valid_header(pSecurityDescriptor)) return FALSE;
    if ((pSecurityDescriptor->Control & SE_DACL_PRESENT) && pSecurityDescriptor->Dacl) {
        PACL dacl = mywin_sd_acl_field(pSecurityDescriptor, pSecurityDescriptor->Dacl);
        if (!dacl || dacl->AclRevision < ACL_REVISION || dacl->AclSize < sizeof(ACL)) return FALSE;
        if (!mywin_acl_used_size(dacl)) return FALSE;
    }
    PSID owner = mywin_sd_sid_field(pSecurityDescriptor, pSecurityDescriptor->Owner);
    if (owner && !IsValidSid(owner)) return FALSE;
    PSID group = mywin_sd_sid_field(pSecurityDescriptor, pSecurityDescriptor->Group);
    if (group && !IsValidSid(group)) return FALSE;
    return TRUE;
}

DWORD GetSecurityDescriptorLength(PSECURITY_DESCRIPTOR pSecurityDescriptor)
{
    if (!IsValidSecurityDescriptor(pSecurityDescriptor)) return 0;
    DWORD len = (DWORD)sizeof(SECURITY_DESCRIPTOR);
    PSID owner = mywin_sd_sid_field(pSecurityDescriptor, pSecurityDescriptor->Owner);
    PSID group = mywin_sd_sid_field(pSecurityDescriptor, pSecurityDescriptor->Group);
    PACL dacl = mywin_sd_acl_field(pSecurityDescriptor, pSecurityDescriptor->Dacl);
    PACL sacl = mywin_sd_acl_field(pSecurityDescriptor, pSecurityDescriptor->Sacl);
    if (owner) len += GetLengthSid(owner);
    if (group) len += GetLengthSid(group);
    if ((pSecurityDescriptor->Control & SE_DACL_PRESENT) && dacl) len += mywin_acl_used_size(dacl);
    if ((pSecurityDescriptor->Control & SE_SACL_PRESENT) && sacl) len += mywin_acl_used_size(sacl);
    return len;
}

BOOL GetSecurityDescriptorControl(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSECURITY_DESCRIPTOR_CONTROL pControl, LPDWORD lpdwRevision)
{
    if (!mywin_sd_valid_header(pSecurityDescriptor) || !pControl || !lpdwRevision) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *pControl = pSecurityDescriptor->Control;
    *lpdwRevision = pSecurityDescriptor->Revision;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL SetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID pOwner, BOOL bOwnerDefaulted)
{
    if (!mywin_sd_valid_header(pSecurityDescriptor) || (pSecurityDescriptor->Control & SE_SELF_RELATIVE)) { mywin_set_last_error(ERROR_INVALID_SECURITY_DESCR); return FALSE; }
    if (pOwner && !IsValidSid(pOwner)) { mywin_set_last_error(ERROR_INVALID_SID); return FALSE; }
    pSecurityDescriptor->Owner = pOwner;
    if (bOwnerDefaulted) pSecurityDescriptor->Control |= SE_OWNER_DEFAULTED;
    else pSecurityDescriptor->Control &= (SECURITY_DESCRIPTOR_CONTROL)~SE_OWNER_DEFAULTED;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL GetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID* pOwner, LPBOOL lpbOwnerDefaulted)
{
    if (!mywin_sd_valid_header(pSecurityDescriptor) || !pOwner) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *pOwner = mywin_sd_sid_field(pSecurityDescriptor, pSecurityDescriptor->Owner);
    if (lpbOwnerDefaulted) *lpbOwnerDefaulted = (pSecurityDescriptor->Control & SE_OWNER_DEFAULTED) ? TRUE : FALSE;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL SetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID pGroup, BOOL bGroupDefaulted)
{
    if (!mywin_sd_valid_header(pSecurityDescriptor) || (pSecurityDescriptor->Control & SE_SELF_RELATIVE)) { mywin_set_last_error(ERROR_INVALID_SECURITY_DESCR); return FALSE; }
    if (pGroup && !IsValidSid(pGroup)) { mywin_set_last_error(ERROR_INVALID_SID); return FALSE; }
    pSecurityDescriptor->Group = pGroup;
    if (bGroupDefaulted) pSecurityDescriptor->Control |= SE_GROUP_DEFAULTED;
    else pSecurityDescriptor->Control &= (SECURITY_DESCRIPTOR_CONTROL)~SE_GROUP_DEFAULTED;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL GetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID* pGroup, LPBOOL lpbGroupDefaulted)
{
    if (!mywin_sd_valid_header(pSecurityDescriptor) || !pGroup) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *pGroup = mywin_sd_sid_field(pSecurityDescriptor, pSecurityDescriptor->Group);
    if (lpbGroupDefaulted) *lpbGroupDefaulted = (pSecurityDescriptor->Control & SE_GROUP_DEFAULTED) ? TRUE : FALSE;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, LPBOOL lpbDaclPresent, PACL* pDacl, LPBOOL lpbDaclDefaulted)
{
    if (!mywin_sd_valid_header(pSecurityDescriptor) || !lpbDaclPresent || !pDacl) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *lpbDaclPresent = (pSecurityDescriptor->Control & SE_DACL_PRESENT) ? TRUE : FALSE;
    *pDacl = *lpbDaclPresent ? mywin_sd_acl_field(pSecurityDescriptor, pSecurityDescriptor->Dacl) : NULL;
    if (lpbDaclDefaulted) *lpbDaclDefaulted = (pSecurityDescriptor->Control & SE_DACL_DEFAULTED) ? TRUE : FALSE;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL MakeSelfRelativeSD(PSECURITY_DESCRIPTOR pAbsoluteSecurityDescriptor, PSECURITY_DESCRIPTOR pSelfRelativeSecurityDescriptor, LPDWORD lpdwBufferLength)
{
    if (!lpdwBufferLength || !mywin_sd_valid_header(pAbsoluteSecurityDescriptor)) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (pAbsoluteSecurityDescriptor->Control & SE_SELF_RELATIVE) { mywin_set_last_error(ERROR_INVALID_SECURITY_DESCR); return FALSE; }
    PSID owner = pAbsoluteSecurityDescriptor->Owner;
    PSID group = pAbsoluteSecurityDescriptor->Group;
    PACL dacl = (pAbsoluteSecurityDescriptor->Control & SE_DACL_PRESENT) ? pAbsoluteSecurityDescriptor->Dacl : NULL;
    PACL sacl = (pAbsoluteSecurityDescriptor->Control & SE_SACL_PRESENT) ? pAbsoluteSecurityDescriptor->Sacl : NULL;
    DWORD ownerLen = owner ? GetLengthSid(owner) : 0;
    DWORD groupLen = group ? GetLengthSid(group) : 0;
    DWORD daclLen = dacl ? mywin_acl_used_size(dacl) : 0;
    DWORD saclLen = sacl ? mywin_acl_used_size(sacl) : 0;
    if ((owner && !ownerLen) || (group && !groupLen) || (dacl && !daclLen) || (sacl && !saclLen)) { mywin_set_last_error(ERROR_INVALID_SECURITY_DESCR); return FALSE; }
    DWORD need = (DWORD)sizeof(SECURITY_DESCRIPTOR) + ownerLen + groupLen + daclLen + saclLen;
    if (!pSelfRelativeSecurityDescriptor || *lpdwBufferLength < need) {
        *lpdwBufferLength = need;
        mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(pSelfRelativeSecurityDescriptor, 0, *lpdwBufferLength);
    pSelfRelativeSecurityDescriptor->Revision = SECURITY_DESCRIPTOR_REVISION;
    pSelfRelativeSecurityDescriptor->Control = pAbsoluteSecurityDescriptor->Control | SE_SELF_RELATIVE;
    BYTE* base = (BYTE*)pSelfRelativeSecurityDescriptor;
    DWORD off = (DWORD)sizeof(SECURITY_DESCRIPTOR);
#define MYWIN_COPY_SD_PART(field, src, cb) do { if ((src) && (cb)) { memcpy(base + off, (src), (cb)); pSelfRelativeSecurityDescriptor->field = (void*)(uintptr_t)off; off += (cb); } } while (0)
    MYWIN_COPY_SD_PART(Owner, owner, ownerLen);
    MYWIN_COPY_SD_PART(Group, group, groupLen);
    MYWIN_COPY_SD_PART(Sacl, sacl, saclLen);
    MYWIN_COPY_SD_PART(Dacl, dacl, daclLen);
#undef MYWIN_COPY_SD_PART
    *lpdwBufferLength = need;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL MakeAbsoluteSD(PSECURITY_DESCRIPTOR pSelfRelativeSecurityDescriptor, PSECURITY_DESCRIPTOR pAbsoluteSecurityDescriptor, LPDWORD lpdwAbsoluteSecurityDescriptorSize, PACL pDacl, LPDWORD lpdwDaclSize, PACL pSacl, LPDWORD lpdwSaclSize, PSID pOwner, LPDWORD lpdwOwnerSize, PSID pPrimaryGroup, LPDWORD lpdwPrimaryGroupSize)
{
    if (!mywin_sd_valid_header(pSelfRelativeSecurityDescriptor) || !lpdwAbsoluteSecurityDescriptorSize || !lpdwDaclSize || !lpdwSaclSize || !lpdwOwnerSize || !lpdwPrimaryGroupSize) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    PSID owner = mywin_sd_sid_field(pSelfRelativeSecurityDescriptor, pSelfRelativeSecurityDescriptor->Owner);
    PSID group = mywin_sd_sid_field(pSelfRelativeSecurityDescriptor, pSelfRelativeSecurityDescriptor->Group);
    PACL dacl = (pSelfRelativeSecurityDescriptor->Control & SE_DACL_PRESENT) ? mywin_sd_acl_field(pSelfRelativeSecurityDescriptor, pSelfRelativeSecurityDescriptor->Dacl) : NULL;
    PACL sacl = (pSelfRelativeSecurityDescriptor->Control & SE_SACL_PRESENT) ? mywin_sd_acl_field(pSelfRelativeSecurityDescriptor, pSelfRelativeSecurityDescriptor->Sacl) : NULL;
    DWORD absNeed = sizeof(SECURITY_DESCRIPTOR);
    DWORD ownerNeed = owner ? GetLengthSid(owner) : 0;
    DWORD groupNeed = group ? GetLengthSid(group) : 0;
    DWORD daclNeed = dacl ? mywin_acl_used_size(dacl) : 0;
    DWORD saclNeed = sacl ? mywin_acl_used_size(sacl) : 0;
    BOOL small = (!pAbsoluteSecurityDescriptor || *lpdwAbsoluteSecurityDescriptorSize < absNeed ||
                  (ownerNeed && (!pOwner || *lpdwOwnerSize < ownerNeed)) ||
                  (groupNeed && (!pPrimaryGroup || *lpdwPrimaryGroupSize < groupNeed)) ||
                  (daclNeed && (!pDacl || *lpdwDaclSize < daclNeed)) ||
                  (saclNeed && (!pSacl || *lpdwSaclSize < saclNeed)));
    *lpdwAbsoluteSecurityDescriptorSize = absNeed;
    *lpdwOwnerSize = ownerNeed;
    *lpdwPrimaryGroupSize = groupNeed;
    *lpdwDaclSize = daclNeed;
    *lpdwSaclSize = saclNeed;
    if (small) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset(pAbsoluteSecurityDescriptor, 0, sizeof(*pAbsoluteSecurityDescriptor));
    pAbsoluteSecurityDescriptor->Revision = SECURITY_DESCRIPTOR_REVISION;
    pAbsoluteSecurityDescriptor->Control = (SECURITY_DESCRIPTOR_CONTROL)(pSelfRelativeSecurityDescriptor->Control & ~SE_SELF_RELATIVE);
    if (ownerNeed) { memcpy(pOwner, owner, ownerNeed); pAbsoluteSecurityDescriptor->Owner = pOwner; }
    if (groupNeed) { memcpy(pPrimaryGroup, group, groupNeed); pAbsoluteSecurityDescriptor->Group = pPrimaryGroup; }
    if (daclNeed) { memcpy(pDacl, dacl, daclNeed); pAbsoluteSecurityDescriptor->Dacl = pDacl; }
    if (saclNeed) { memcpy(pSacl, sacl, saclNeed); pAbsoluteSecurityDescriptor->Sacl = pSacl; }
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL InitializeAcl(PACL pAcl, DWORD nAclLength, DWORD dwAclRevision)
{
    if (!pAcl || nAclLength < sizeof(ACL) || nAclLength > 0xffffu || dwAclRevision < ACL_REVISION) {
        mywin_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(pAcl, 0, nAclLength);
    pAcl->AclRevision = (BYTE)dwAclRevision;
    pAcl->AclSize = (WORD)nAclLength;
    pAcl->AceCount = 0;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

static BYTE* mywin_acl_end(PACL pAcl)
{
    if (!pAcl || pAcl->AclSize < sizeof(ACL)) return NULL;
    BYTE* base = (BYTE*)pAcl;
    BYTE* p = base + sizeof(ACL);
    BYTE* limit = base + pAcl->AclSize;
    for (WORD i = 0; i < pAcl->AceCount; ++i) {
        if (p + sizeof(ACE_HEADER) > limit) return NULL;
        ACE_HEADER* h = (ACE_HEADER*)p;
        if (h->AceSize < sizeof(ACE_HEADER) || p + h->AceSize > limit) return NULL;
        p += h->AceSize;
    }
    return p;
}

static BOOL mywin_add_access_ace(PACL pAcl, DWORD dwAceRevision, DWORD AccessMask, PSID pSid, BYTE aceType, BYTE aceFlags)
{
    if (!pAcl || dwAceRevision < ACL_REVISION) { mywin_set_last_error(ERROR_INVALID_ACL); return FALSE; }
    DWORD sidLen = GetLengthSid(pSid);
    if (!sidLen) { mywin_set_last_error(ERROR_INVALID_SID); return FALSE; }
    BYTE* end = mywin_acl_end(pAcl);
    if (!end) { mywin_set_last_error(ERROR_INVALID_ACL); return FALSE; }
    DWORD aceSize = (DWORD)(offsetof(ACCESS_ALLOWED_ACE, SidStart) + sidLen);
    BYTE* limit = ((BYTE*)pAcl) + pAcl->AclSize;
    if (aceSize > 0xffffu || end + aceSize > limit) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    ACCESS_ALLOWED_ACE* ace = (ACCESS_ALLOWED_ACE*)end;
    memset(ace, 0, aceSize);
    ace->Header.AceType = aceType;
    ace->Header.AceFlags = aceFlags;
    ace->Header.AceSize = (WORD)aceSize;
    ace->Mask = AccessMask;
    memcpy(&ace->SidStart, pSid, sidLen);
    pAcl->AceCount++;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL AddAccessAllowedAce(PACL pAcl, DWORD dwAceRevision, DWORD AccessMask, PSID pSid)
{
    return mywin_add_access_ace(pAcl, dwAceRevision, AccessMask, pSid, ACCESS_ALLOWED_ACE_TYPE, 0);
}

BOOL AddAccessDeniedAce(PACL pAcl, DWORD dwAceRevision, DWORD AccessMask, PSID pSid)
{
    return mywin_add_access_ace(pAcl, dwAceRevision, AccessMask, pSid, ACCESS_DENIED_ACE_TYPE, 0);
}

static int mywin_public_sid_to_obj(PSID sid, _ObjectSid* out)
{
    if (!IsValidSid(sid) || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->revision = sid->Revision;
    out->subauth_count = sid->SubAuthorityCount;
    memcpy(out->authority, sid->IdentifierAuthority.Value, sizeof(out->authority));
    for (DWORD i = 0; i < sid->SubAuthorityCount && i < _OBJECT_SECURITY_MAX_SUBAUTH; ++i)
        out->subauth[i] = sid->SubAuthority[i];
    return 1;
}

static void mywin_obj_sid_init(_ObjectSid* out, const BYTE auth[6], DWORD c0, DWORD c1, DWORD count)
{
    memset(out, 0, sizeof(*out));
    out->revision = SID_REVISION;
    out->subauth_count = (BYTE)count;
    memcpy(out->authority, auth, 6);
    if (count > 0) out->subauth[0] = c0;
    if (count > 1) out->subauth[1] = c1;
}

static void mywin_process_sid_for_pid(DWORD pid, _ObjectSid* out)
{
    static const BYTE ntAuth[6] = {0,0,0,0,0,5};
    mywin_obj_sid_init(out, ntAuth, 21u, pid, 2u);
}

static void mywin_everyone_sid(_ObjectSid* out)
{
    static const BYTE worldAuth[6] = {0,0,0,0,0,1};
    mywin_obj_sid_init(out, worldAuth, SECURITY_WORLD_RID, 0, 1u);
}

static void mywin_admins_sid(_ObjectSid* out)
{
    static const BYTE ntAuth[6] = {0,0,0,0,0,5};
    memset(out, 0, sizeof(*out));
    out->revision = SID_REVISION;
    out->subauth_count = 2;
    memcpy(out->authority, ntAuth, 6);
    out->subauth[0] = SECURITY_BUILTIN_DOMAIN_RID;
    out->subauth[1] = DOMAIN_ALIAS_RID_ADMINS;
}

static void mywin_current_token(_ObjectToken* out)
{
    memset(out, 0, sizeof(*out));
    mywin_process_sid_for_pid(mywin_current_pid(), &out->user);
    mywin_everyone_sid(&out->groups[out->group_count++]);
    if (g_HasCapability && ((g_CurrentCapability.flags & CAP_ADMIN) == CAP_ADMIN)) {
        mywin_admins_sid(&out->groups[out->group_count++]);
        out->is_admin = TRUE;
    }
}

static DWORD mywin_object_generic_all(DWORD type)
{
    switch (type) {
    case _OBJECT_TYPE_EVENT: return EVENT_ALL_ACCESS;
    case _OBJECT_TYPE_MUTEX: return MUTEX_ALL_ACCESS;
    case _OBJECT_TYPE_SEMAPHORE: return SEMAPHORE_ALL_ACCESS;
    case _OBJECT_TYPE_TIMER: return TIMER_ALL_ACCESS;
    case _OBJECT_TYPE_SECTION: return FILE_MAP_ALL_ACCESS | READ_CONTROL | WRITE_DAC | WRITE_OWNER | DELETE;
    case _OBJECT_TYPE_PROCESS: return PROCESS_ALL_ACCESS;
    case _OBJECT_TYPE_THREAD: return THREAD_ALL_ACCESS;
    default: return 0xffffffffu;
    }
}

static DWORD mywin_object_generic_read(DWORD type)
{
    switch (type) {
    case _OBJECT_TYPE_SECTION: return FILE_MAP_READ | READ_CONTROL;
    case _OBJECT_TYPE_PROCESS: return PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | READ_CONTROL | SYNCHRONIZE;
    case _OBJECT_TYPE_THREAD: return THREAD_QUERY_INFORMATION | READ_CONTROL | SYNCHRONIZE;
    default: return SYNCHRONIZE | READ_CONTROL;
    }
}

static DWORD mywin_object_generic_write(DWORD type)
{
    switch (type) {
    case _OBJECT_TYPE_EVENT: return EVENT_MODIFY_STATE | READ_CONTROL;
    case _OBJECT_TYPE_MUTEX: return MUTEX_MODIFY_STATE | READ_CONTROL;
    case _OBJECT_TYPE_SEMAPHORE: return SEMAPHORE_MODIFY_STATE | READ_CONTROL;
    case _OBJECT_TYPE_TIMER: return TIMER_MODIFY_STATE | READ_CONTROL;
    case _OBJECT_TYPE_SECTION: return FILE_MAP_WRITE | READ_CONTROL;
    case _OBJECT_TYPE_PROCESS: return PROCESS_CREATE_PROCESS | PROCESS_DUP_HANDLE | READ_CONTROL;
    default: return READ_CONTROL | WRITE_DAC;
    }
}

static DWORD mywin_expand_generic_access(DWORD type, DWORD access)
{
    DWORD out = access;
    if (out & GENERIC_ALL) { out &= ~GENERIC_ALL; out |= mywin_object_generic_all(type); }
    if (out & GENERIC_READ) { out &= ~GENERIC_READ; out |= mywin_object_generic_read(type); }
    if (out & GENERIC_WRITE) { out &= ~GENERIC_WRITE; out |= mywin_object_generic_write(type); }
    if (out & GENERIC_EXECUTE) { out &= ~GENERIC_EXECUTE; out |= SYNCHRONIZE | READ_CONTROL; }
    return out;
}


static void mywin_map_mask_with_mapping(PDWORD AccessMask, PGENERIC_MAPPING GenericMapping)
{
    if (!AccessMask || !GenericMapping) return;
    DWORD out = *AccessMask;
    if (out & GENERIC_READ)    { out &= ~GENERIC_READ;    out |= GenericMapping->GenericRead; }
    if (out & GENERIC_WRITE)   { out &= ~GENERIC_WRITE;   out |= GenericMapping->GenericWrite; }
    if (out & GENERIC_EXECUTE) { out &= ~GENERIC_EXECUTE; out |= GenericMapping->GenericExecute; }
    if (out & GENERIC_ALL)     { out &= ~GENERIC_ALL;     out |= GenericMapping->GenericAll; }
    *AccessMask = out;
}

void MapGenericMask(PDWORD AccessMask, PGENERIC_MAPPING GenericMapping)
{
    mywin_map_mask_with_mapping(AccessMask, GenericMapping);
}


static BOOL mywin_sd_valid_header(PSECURITY_DESCRIPTOR sd)
{
    return sd && sd->Revision == SECURITY_DESCRIPTOR_REVISION;
}

static PSID mywin_sd_sid_field(PSECURITY_DESCRIPTOR sd, PSID field)
{
    if (!sd || !field) return NULL;
    if (sd->Control & SE_SELF_RELATIVE) {
        uintptr_t off = (uintptr_t)field;
        if (off == 0) return NULL;
        return (PSID)((BYTE*)sd + off);
    }
    return field;
}

static PACL mywin_sd_acl_field(PSECURITY_DESCRIPTOR sd, PACL field)
{
    if (!sd || !field) return NULL;
    if (sd->Control & SE_SELF_RELATIVE) {
        uintptr_t off = (uintptr_t)field;
        if (off == 0) return NULL;
        return (PACL)((BYTE*)sd + off);
    }
    return field;
}

static DWORD mywin_acl_used_size(PACL acl)
{
    if (!acl || acl->AclRevision < ACL_REVISION || acl->AclSize < sizeof(ACL)) return 0;
    BYTE* base = (BYTE*)acl;
    BYTE* p = base + sizeof(ACL);
    BYTE* limit = base + acl->AclSize;
    for (WORD i = 0; i < acl->AceCount; ++i) {
        if (p + sizeof(ACE_HEADER) > limit) return 0;
        ACE_HEADER* h = (ACE_HEADER*)p;
        if (h->AceSize < sizeof(ACE_HEADER) || p + h->AceSize > limit) return 0;
        p += h->AceSize;
    }
    return (DWORD)(p - base);
}

static BOOL mywin_obj_sid_to_public(const _ObjectSid* in, SID* out)
{
    if (!in || !out || in->revision != SID_REVISION || in->subauth_count > SID_MAX_SUB_AUTHORITIES) return FALSE;
    memset(out, 0, sizeof(*out));
    out->Revision = SID_REVISION;
    out->SubAuthorityCount = in->subauth_count;
    memcpy(out->IdentifierAuthority.Value, in->authority, sizeof(out->IdentifierAuthority.Value));
    for (DWORD i = 0; i < in->subauth_count && i < SID_MAX_SUB_AUTHORITIES; ++i)
        out->SubAuthority[i] = in->subauth[i];
    return TRUE;
}

static BOOL mywin_security_from_descriptor(PSECURITY_DESCRIPTOR sd, _ObjectSecurity* out, DWORD objectType)
{
    if (!out) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(out, 0, sizeof(*out));
    if (!mywin_sd_valid_header(sd)) { mywin_set_last_error(ERROR_INVALID_SECURITY_DESCR); return FALSE; }

    out->valid = 1;
    out->control = sd->Control;
    PSID owner = mywin_sd_sid_field(sd, sd->Owner);
    if (owner) {
        if (!mywin_public_sid_to_obj(owner, &out->owner)) { mywin_set_last_error(ERROR_INVALID_SID); return FALSE; }
    } else {
        mywin_process_sid_for_pid(mywin_current_pid(), &out->owner);
    }

    out->dacl_present = (sd->Control & SE_DACL_PRESENT) ? TRUE : FALSE;
    if (!out->dacl_present) {
        out->dacl_null = TRUE;
        mywin_set_last_error(ERROR_SUCCESS);
        return TRUE;
    }

    PACL acl = mywin_sd_acl_field(sd, sd->Dacl);
    if (!acl) {
        out->dacl_null = TRUE;
        mywin_set_last_error(ERROR_SUCCESS);
        return TRUE;
    }
    if (acl->AclRevision < ACL_REVISION || acl->AclSize < sizeof(ACL) || !mywin_acl_used_size(acl)) {
        mywin_set_last_error(ERROR_INVALID_ACL);
        return FALSE;
    }

    BYTE* base = (BYTE*)acl;
    BYTE* p = base + sizeof(ACL);
    BYTE* limit = base + acl->AclSize;
    for (WORD i = 0; i < acl->AceCount; ++i) {
        if (p + sizeof(ACE_HEADER) > limit || out->ace_count >= _OBJECT_SECURITY_MAX_ACES) {
            mywin_set_last_error(ERROR_INVALID_ACL);
            return FALSE;
        }
        ACE_HEADER* hdr = (ACE_HEADER*)p;
        if (hdr->AceSize < offsetof(ACCESS_ALLOWED_ACE, SidStart) || p + hdr->AceSize > limit) {
            mywin_set_last_error(ERROR_INVALID_ACL);
            return FALSE;
        }
        if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE && hdr->AceType != ACCESS_DENIED_ACE_TYPE) {
            mywin_set_last_error(ERROR_INVALID_ACL);
            return FALSE;
        }
        ACCESS_ALLOWED_ACE* ace = (ACCESS_ALLOWED_ACE*)p;
        PSID sid = (PSID)&ace->SidStart;
        if (!mywin_public_sid_to_obj(sid, &out->aces[out->ace_count].sid)) {
            mywin_set_last_error(ERROR_INVALID_SID);
            return FALSE;
        }
        out->aces[out->ace_count].type = (hdr->AceType == ACCESS_DENIED_ACE_TYPE) ? _OBJECT_ACE_DENY : _OBJECT_ACE_ALLOW;
        out->aces[out->ace_count].flags = hdr->AceFlags;
        out->aces[out->ace_count].mask = objectType ? mywin_expand_generic_access(objectType, ace->Mask) : ace->Mask;
        out->ace_count++;
        p += hdr->AceSize;
    }
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

static BOOL mywin_security_from_attributes(LPSECURITY_ATTRIBUTES sa, _ObjectSecurity* out, BOOL* hasExplicit)
{
    if (hasExplicit) *hasExplicit = FALSE;
    if (out) memset(out, 0, sizeof(*out));
    if (!sa || !sa->lpSecurityDescriptor) return TRUE;
    if (!out || !hasExplicit) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!mywin_security_from_descriptor((PSECURITY_DESCRIPTOR)sa->lpSecurityDescriptor, out, 0)) return FALSE;
    *hasExplicit = TRUE;
    return TRUE;
}


static DWORD mywin_default_sd_for_name(const char* canonical);

static BOOL mywin_objsec_add_ace(_ObjectSecurity* sec, DWORD type, DWORD flags, DWORD mask, const _ObjectSid* sid)
{
    if (!sec || !sid || sec->ace_count >= _OBJECT_SECURITY_MAX_ACES) return FALSE;
    _ObjectAce* ace = &sec->aces[sec->ace_count++];
    memset(ace, 0, sizeof(*ace));
    ace->type = type;
    ace->flags = flags;
    ace->mask = mask;
    ace->sid = *sid;
    return TRUE;
}

static BOOL mywin_build_default_object_security(const char* canonicalName, DWORD nsId, DWORD objectType, _ObjectSecurity* out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    out->valid = 1;
    out->namespace_id = nsId;
    out->control = SE_DACL_PRESENT | SE_DACL_AUTO_INHERITED;
    out->dacl_present = TRUE;
    out->dacl_null = FALSE;
    mywin_process_sid_for_pid(mywin_current_pid(), &out->owner);

    DWORD ownerAll = mywin_object_generic_all(objectType);
    if (!mywin_objsec_add_ace(out, _OBJECT_ACE_ALLOW, _OBJECT_ACE_FLAG_INHERITED, ownerAll, &out->owner)) return FALSE;

    DWORD legacy = mywin_default_sd_for_name(canonicalName);
    if (legacy & (_OBJECT_SD_PUBLIC_READ | _OBJECT_SD_PUBLIC_WRITE)) {
        _ObjectSid everyone;
        mywin_everyone_sid(&everyone);
        DWORD mask = 0;
        if (legacy & _OBJECT_SD_PUBLIC_READ)  mask |= mywin_object_generic_read(objectType);
        if (legacy & _OBJECT_SD_PUBLIC_WRITE) mask |= mywin_object_generic_write(objectType);
        if (mask && !mywin_objsec_add_ace(out, _OBJECT_ACE_ALLOW, _OBJECT_ACE_FLAG_INHERITED, mask, &everyone)) return FALSE;
    }
    return TRUE;
}

static BOOL mywin_apply_object_security(HANDLE hObject, DWORD nsId, const char* canonicalName, const _ObjectSecurity* sec, BOOL hasExplicit)
{
    _ObjectectInfo oi;
    DWORD type = _ObjectGetInfo(hObject, &oi) ? oi.type : _OBJECT_TYPE_NONE;
    if (hasExplicit) {
        if (!sec) return FALSE;
        _ObjectSecurity mapped = *sec;
        mapped.namespace_id = nsId;
        for (DWORD i = 0; i < mapped.ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i)
            mapped.aces[i].mask = mywin_expand_generic_access(type, mapped.aces[i].mask);
        return _ObjectSetSecurityDescriptor(hObject, &mapped);
    }

    _ObjectSecurity defSec;
    if (!mywin_build_default_object_security(canonicalName, nsId, type, &defSec))
        return _ObjectSetSecurity(hObject, mywin_default_sd_for_name(canonicalName), nsId);
    return _ObjectSetSecurityDescriptor(hObject, &defSec);
}

/* v204: Process/Thread objects are not namespace objects.  Their default
   security comes from the creator token's default DACL shape, not from the
   public Global/Local named-object defaults used by Events/Sections. */
static BOOL mywin_apply_token_default_object_security(HANDLE hObject, const _ObjectSecurity* sec, BOOL hasExplicit)
{
    _ObjectectInfo oi;
    DWORD type = _ObjectGetInfo(hObject, &oi) ? oi.type : _OBJECT_TYPE_NONE;
    if (hasExplicit) {
        if (!sec) return FALSE;
        _ObjectSecurity mapped = *sec;
        mapped.namespace_id = _OBJECT_NS_NONE;
        for (DWORD i = 0; i < mapped.ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i)
            mapped.aces[i].mask = mywin_expand_generic_access(type, mapped.aces[i].mask);
        return _ObjectSetSecurityDescriptor(hObject, &mapped);
    }

    _ObjectSecurity defSec;
    memset(&defSec, 0, sizeof(defSec));
    defSec.valid = 1;
    defSec.namespace_id = _OBJECT_NS_NONE;
    defSec.control = SE_DACL_PRESENT | SE_DACL_DEFAULTED;
    defSec.dacl_present = TRUE;
    defSec.dacl_null = FALSE;
    mywin_process_sid_for_pid(mywin_current_pid(), &defSec.owner);

    if (!mywin_objsec_add_ace(&defSec, _OBJECT_ACE_ALLOW, 0, mywin_object_generic_all(type), &defSec.owner))
        return FALSE;
    if (g_HasCapability && ((g_CurrentCapability.flags & CAP_ADMIN) == CAP_ADMIN)) {
        _ObjectSid admins;
        mywin_admins_sid(&admins);
        if (!mywin_objsec_add_ace(&defSec, _OBJECT_ACE_ALLOW, 0, mywin_object_generic_all(type), &admins))
            return FALSE;
    }
    return _ObjectSetSecurityDescriptor(hObject, &defSec);
}

static int mywin_ascii_ieq(LPCSTR a, LPCSTR b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static const char* mywin_path_basename(LPCSTR path)
{
    if (!path) return "";
    const char* base = path;
    for (const char* q = path; *q; ++q) {
        if (*q == '/' || *q == '\\') base = q + 1;
    }
    return base;
}

static int mywin_has_path_or_ext(LPCSTR s)
{
    if (!s) return 0;
    for (const char* q = s; *q; ++q) {
        if (*q == '/' || *q == '\\' || *q == ':') return 1;
    }
    return 0;
}

static int mywin_ends_with_exe_i(LPCSTR s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    return n >= 4 && mywin_ascii_ieq(s + n - 4, ".exe");
}

static void mywin_module_init_locked(MyWinProcessLite* p, LPCSTR imageName)
{
    if (!p) return;
    const char* img = (imageName && imageName[0]) ? imageName : (p->imageName[0] ? p->imageName : "process-lite");

    p->mainModule = mywin_make_module_handle(p->pid);
    snprintf(p->moduleName, sizeof(p->moduleName), "%s", mywin_path_basename(img));
    if (!mywin_ends_with_exe_i(p->moduleName) && strlen(p->moduleName) + 4 < sizeof(p->moduleName))
        strncat(p->moduleName, ".exe", sizeof(p->moduleName) - strlen(p->moduleName) - 1);

    if (mywin_has_path_or_ext(img)) {
        snprintf(p->imagePath, sizeof(p->imagePath), "%s", img);
    } else {
        snprintf(p->imagePath, sizeof(p->imagePath), "C:\\myOS\\System32\\%s", p->moduleName);
    }
}

static int mywin_module_matches_locked(const MyWinProcessLite* p, LPCSTR name)
{
    if (!p || !name || !name[0]) return 0;
    const char* base = mywin_path_basename(name);
    if (mywin_ascii_ieq(name, p->imagePath)) return 1;
    if (mywin_ascii_ieq(name, p->imageName)) return 1;
    if (mywin_ascii_ieq(name, p->moduleName)) return 1;
    if (base && mywin_ascii_ieq(base, p->moduleName)) return 1;

    char tmp[MYWIN_MAX_MODULE_NAME];
    snprintf(tmp, sizeof(tmp), "%s", base && base[0] ? base : name);
    if (!mywin_ends_with_exe_i(tmp) && strlen(tmp) + 4 < sizeof(tmp))
        strncat(tmp, ".exe", sizeof(tmp) - strlen(tmp) - 1);
    return mywin_ascii_ieq(tmp, p->moduleName);
}

static int mywin_env_find_locked(MyWinProcessLite* p, LPCSTR name)
{
    if (!p || !name || !name[0]) return -1;
    for (DWORD i = 0; i < p->environmentCount && i < MYWIN_MAX_ENV_VARS; i++)
        if (mywin_ascii_ieq(p->envName[i], name)) return (int)i;
    return -1;
}

static BOOL mywin_env_set_locked(MyWinProcessLite* p, LPCSTR name, LPCSTR value)
{
    if (!p || !name || !name[0] || strchr(name, '=')) return FALSE;
    if (strlen(name) >= MYWIN_MAX_ENV_NAME) return FALSE;

    int idx = mywin_env_find_locked(p, name);
    if (!value) {
        if (idx < 0) return TRUE;
        for (DWORD i = (DWORD)idx + 1; i < p->environmentCount; i++) {
            snprintf(p->envName[i - 1], sizeof(p->envName[i - 1]), "%s", p->envName[i]);
            snprintf(p->envValue[i - 1], sizeof(p->envValue[i - 1]), "%s", p->envValue[i]);
        }
        if (p->environmentCount > 0) p->environmentCount--;
        if (p->environmentCount < MYWIN_MAX_ENV_VARS) {
            p->envName[p->environmentCount][0] = 0;
            p->envValue[p->environmentCount][0] = 0;
        }
        return TRUE;
    }

    if (strlen(value) >= MYWIN_MAX_ENV_VALUE) return FALSE;
    if (idx < 0) {
        if (p->environmentCount >= MYWIN_MAX_ENV_VARS) return FALSE;
        idx = (int)p->environmentCount++;
    }
    snprintf(p->envName[idx], sizeof(p->envName[idx]), "%s", name);
    snprintf(p->envValue[idx], sizeof(p->envValue[idx]), "%s", value);
    return TRUE;
}

static void mywin_env_clear_locked(MyWinProcessLite* p)
{
    if (!p) return;
    p->environmentCount = 0;
    memset(p->envName, 0, sizeof(p->envName));
    memset(p->envValue, 0, sizeof(p->envValue));
}

static void mywin_env_copy_locked(MyWinProcessLite* dst, const MyWinProcessLite* src)
{
    if (!dst || !src) return;
    mywin_env_clear_locked(dst);
    DWORD n = src->environmentCount;
    if (n > MYWIN_MAX_ENV_VARS) n = MYWIN_MAX_ENV_VARS;
    for (DWORD i = 0; i < n; i++) {
        snprintf(dst->envName[i], sizeof(dst->envName[i]), "%s", src->envName[i]);
        snprintf(dst->envValue[i], sizeof(dst->envValue[i]), "%s", src->envValue[i]);
    }
    dst->environmentCount = n;
}

static void mywin_env_defaults_locked(MyWinProcessLite* p)
{
    if (!p || p->environmentCount) return;
    mywin_env_set_locked(p, "SystemRoot", "C:\\myOS");
    mywin_env_set_locked(p, "windir", "C:\\myOS");
    mywin_env_set_locked(p, "PATH", "C:\\myOS\\System32;C:\\myOS");
    mywin_env_set_locked(p, "TEMP", "C:\\Temp");
    mywin_env_set_locked(p, "TMP", "C:\\Temp");
    mywin_env_set_locked(p, "USERPROFILE", "C:\\Users\\Rick");
}

static void mywin_env_load_block_locked(MyWinProcessLite* p, LPVOID lpEnvironment)
{
    if (!p) return;
    mywin_env_clear_locked(p);
    if (!lpEnvironment) { mywin_env_defaults_locked(p); return; }

    const char* cur = (const char*)lpEnvironment;
    while (*cur && p->environmentCount < MYWIN_MAX_ENV_VARS) {
        const char* eq = strchr(cur, '=');
        size_t len = strlen(cur);
        if (eq && eq != cur) {
            char name[MYWIN_MAX_ENV_NAME];
            char value[MYWIN_MAX_ENV_VALUE];
            size_t nl = (size_t)(eq - cur);
            if (nl >= sizeof(name)) nl = sizeof(name) - 1;
            snprintf(name, sizeof(name), "%.*s", (int)nl, cur);
            snprintf(value, sizeof(value), "%s", eq + 1);
            mywin_env_set_locked(p, name, value);
        }
        cur += len + 1;
    }
    mywin_env_defaults_locked(p);
}

static void mywin_env_init_for_child_locked(MyWinProcessLite* child, DWORD parentPid, LPVOID lpEnvironment)
{
    if (!child) return;
    if (lpEnvironment) {
        mywin_env_load_block_locked(child, lpEnvironment);
        return;
    }

    MyWinProcessLite* parent = mywin_find_lite_process_locked(parentPid);
    if (parent && parent->environmentCount) mywin_env_copy_locked(child, parent);
    else { mywin_env_clear_locked(child); mywin_env_defaults_locked(child); }
}

static void mywin_env_preview_locked(const MyWinProcessLite* p, char* out, size_t cb)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    if (!p || p->environmentCount == 0) return;
    size_t used = 0;
    DWORD n = p->environmentCount < 3 ? p->environmentCount : 3;
    for (DWORD i = 0; i < n; i++) {
        int wrote = snprintf(out + used, cb - used, "%s%s=%s", used ? ";" : "", p->envName[i], p->envValue[i]);
        if (wrote < 0) break;
        if ((size_t)wrote >= cb - used) { out[cb - 1] = 0; break; }
        used += (size_t)wrote;
    }
}


static int mywin_has_dot_ext(LPCSTR s)
{
    if (!s) return 0;
    const char* base = mywin_path_basename(s);
    return base && strchr(base, '.') ? 1 : 0;
}

static void mywin_normalize_dll_name(LPCSTR in, char* outName, size_t cbName, char* outPath, size_t cbPath)
{
    if (outName && cbName) outName[0] = 0;
    if (outPath && cbPath) outPath[0] = 0;
    if (!in || !in[0]) return;

    const char* base = mywin_path_basename(in);
    char name[MYWIN_MAX_MODULE_NAME];
    snprintf(name, sizeof(name), "%s", (base && base[0]) ? base : in);
    for (char* pch = name; *pch; ++pch) *pch = (char)tolower((unsigned char)*pch);
    if (!mywin_has_dot_ext(name) && strlen(name) + 4 < sizeof(name))
        strncat(name, ".dll", sizeof(name) - strlen(name) - 1);

    if (outName && cbName) snprintf(outName, cbName, "%s", name);
    if (outPath && cbPath) {
        if (mywin_has_path_or_ext(in) && (strchr(in, '/') || strchr(in, '\\') || strchr(in, ':')))
            snprintf(outPath, cbPath, "%s", in);
        else
            snprintf(outPath, cbPath, "C:\\myOS\\System32\\%s", name);
    }
}

static int mywin_is_known_virtual_dll(LPCSTR normName)
{
    return mywin_ascii_ieq(normName, "kernel32.dll") ||
           mywin_ascii_ieq(normName, "user32.dll")   ||
           mywin_ascii_ieq(normName, "gdi32.dll")    ||
           mywin_ascii_ieq(normName, "shell32.dll")  ||
           mywin_ascii_ieq(normName, "advapi32.dll");
}

static void mywin_resolve_dll_path_locked(const MyWinProcessLite* p, LPCSTR input, LPCSTR normName, char* outPath, size_t cbPath)
{
    if (!outPath || cbPath == 0) return;
    outPath[0] = 0;
    if (input && input[0] && (strchr(input, '/') || strchr(input, '\\') || strchr(input, ':'))) {
        snprintf(outPath, cbPath, "%s", input);
        return;
    }
    if (p && p->dllDirectory[0]) {
        const char sep = strchr(p->dllDirectory, '/') ? '/' : '\\';
        size_t n = strlen(p->dllDirectory);
        snprintf(outPath, cbPath, "%s", p->dllDirectory);
        if (n && p->dllDirectory[n-1] != '/' && p->dllDirectory[n-1] != '\\')
            strncat(outPath, sep == '/' ? "/" : "\\", cbPath - strlen(outPath) - 1);
        strncat(outPath, normName ? normName : "", cbPath - strlen(outPath) - 1);
        return;
    }
    snprintf(outPath, cbPath, "C:\\myOS\\System32\\%s", normName ? normName : "");
}

static MyWinLoadedModule* mywin_find_loaded_module_by_name_locked(MyWinProcessLite* p, LPCSTR name)
{
    if (!p || !name || !name[0]) return NULL;
    char norm[MYWIN_MAX_MODULE_NAME];
    mywin_normalize_dll_name(name, norm, sizeof(norm), NULL, 0);
    for (DWORD i = 0; i < MYWIN_MAX_LOADED_MODULES; i++) {
        MyWinLoadedModule* m = &p->loadedModules[i];
        if (!m->valid) continue;
        if (mywin_ascii_ieq(m->moduleName, norm) || mywin_ascii_ieq(m->imagePath, name) || mywin_ascii_ieq(mywin_path_basename(name), m->moduleName))
            return m;
    }
    return NULL;
}

static MyWinLoadedModule* mywin_find_loaded_module_by_handle_locked(MyWinProcessLite* p, HMODULE hModule)
{
    if (!p || !hModule) return NULL;
    DWORD hp = mywin_pid_from_dll_module_handle(hModule);
    DWORD slot = mywin_slot_from_dll_module_handle(hModule);
    if (!hp || !slot || hp != (p->pid & 0x0fffu)) return NULL;
    if (slot >= MYWIN_MAX_LOADED_MODULES) return NULL;
    MyWinLoadedModule* m = &p->loadedModules[slot];
    return (m->valid && m->module == hModule) ? m : NULL;
}

static DWORD mywin_loaded_module_count_locked(const MyWinProcessLite* p)
{
    if (!p) return 0;
    DWORD count = 0;
    for (DWORD i = 0; i < MYWIN_MAX_LOADED_MODULES; i++)
        if (p->loadedModules[i].valid) count++;
    return count;
}

static void mywin_dll_preview_locked(const MyWinProcessLite* p, char* out, size_t cb)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    if (!p) return;
    size_t used = 0;
    DWORD shown = 0;
    for (DWORD i = 0; i < MYWIN_MAX_LOADED_MODULES && shown < 3; i++) {
        const MyWinLoadedModule* m = &p->loadedModules[i];
        if (!m->valid) continue;
        int wrote = snprintf(out + used, cb - used, "%s%s:%u", used ? ";" : "", m->moduleName, m->refCount);
        if (wrote < 0) break;
        if ((size_t)wrote >= cb - used) { out[cb - 1] = 0; break; }
        used += (size_t)wrote;
        shown++;
    }
}

static FARPROC mywin_resolve_virtual_export(LPCSTR moduleName, LPCSTR procName)
{
    (void)moduleName;
    if (!procName) return NULL;

    // Ordinal lookup is recognized but not mapped yet.  This mirrors the
    // loader surface without pretending we have a real export table parser.
    if (((uintptr_t)procName) < 0x10000u) return NULL;

#define MYWIN_EXPORT(name) if (mywin_ascii_ieq(procName, #name)) return (FARPROC)(void*)name
    MYWIN_EXPORT(GetCommandLineA);
    MYWIN_EXPORT(GetStartupInfoA);
    MYWIN_EXPORT(GetCurrentDirectoryA);
    MYWIN_EXPORT(SetCurrentDirectoryA);
    MYWIN_EXPORT(GetEnvironmentVariableA);
    MYWIN_EXPORT(SetEnvironmentVariableA);
    MYWIN_EXPORT(ExpandEnvironmentStringsA);
    MYWIN_EXPORT(GetModuleHandleA);
    MYWIN_EXPORT(GetModuleFileNameA);
    MYWIN_EXPORT(GetLastError);
    MYWIN_EXPORT(SetLastError);
    MYWIN_EXPORT(InitializeSecurityDescriptor);
    MYWIN_EXPORT(IsValidSecurityDescriptor);
    MYWIN_EXPORT(GetSecurityDescriptorLength);
    MYWIN_EXPORT(GetSecurityDescriptorControl);
    MYWIN_EXPORT(SetSecurityDescriptorOwner);
    MYWIN_EXPORT(GetSecurityDescriptorOwner);
    MYWIN_EXPORT(SetSecurityDescriptorGroup);
    MYWIN_EXPORT(GetSecurityDescriptorGroup);
    MYWIN_EXPORT(SetSecurityDescriptorDacl);
    MYWIN_EXPORT(GetSecurityDescriptorDacl);
    MYWIN_EXPORT(MakeSelfRelativeSD);
    MYWIN_EXPORT(MakeAbsoluteSD);
    MYWIN_EXPORT(MapGenericMask);
    MYWIN_EXPORT(AccessCheck);
    MYWIN_EXPORT(CheckTokenMembership);
    MYWIN_EXPORT(PrivilegeCheck);
    MYWIN_EXPORT(GetKernelObjectSecurity);
    MYWIN_EXPORT(SetKernelObjectSecurity);
    MYWIN_EXPORT(InitializeAcl);
    MYWIN_EXPORT(AddAccessAllowedAce);
    MYWIN_EXPORT(AddAccessDeniedAce);
    MYWIN_EXPORT(IsValidSid);
    MYWIN_EXPORT(GetLengthSid);
    MYWIN_EXPORT(EqualSid);
    MYWIN_EXPORT(CopySid);
    MYWIN_EXPORT(InitializeSid);
    MYWIN_EXPORT(GetSidSubAuthority);
    MYWIN_EXPORT(LoadLibraryA);
    MYWIN_EXPORT(FreeLibrary);
    MYWIN_EXPORT(GetProcAddress);
    MYWIN_EXPORT(MyWinRegisterDialogTemplateResourceA);
    MYWIN_EXPORT(MyWinRegisterResourceA);
    MYWIN_EXPORT(SizeofResource);
    MYWIN_EXPORT(LockResource);
    MYWIN_EXPORT(LoadResource);
    MYWIN_EXPORT(FindResourceA);
    MYWIN_EXPORT(SetDllDirectoryA);
    MYWIN_EXPORT(GetDllDirectoryA);
    MYWIN_EXPORT(CreateFileMappingA);
    MYWIN_EXPORT(OpenFileMappingA);
    MYWIN_EXPORT(MapViewOfFile);
    MYWIN_EXPORT(UnmapViewOfFile);
    MYWIN_EXPORT(FlushViewOfFile);
    MYWIN_EXPORT(CreateEventA);
    MYWIN_EXPORT(OpenEventA);
    MYWIN_EXPORT(SetEvent);
    MYWIN_EXPORT(ResetEvent);
    MYWIN_EXPORT(CreateMutexA);
    MYWIN_EXPORT(CreateSemaphoreA);
    MYWIN_EXPORT(CreateWaitableTimerA);
    MYWIN_EXPORT(WaitForSingleObject);
    MYWIN_EXPORT(WaitForMultipleObjects);
    MYWIN_EXPORT(MsgWaitForMultipleObjects);
    MYWIN_EXPORT(MsgWaitForMultipleObjectsEx);
    MYWIN_EXPORT(WaitMessage);
    MYWIN_EXPORT(CloseHandle);
    MYWIN_EXPORT(DuplicateHandle);
    MYWIN_EXPORT(GetHandleInformation);
    MYWIN_EXPORT(SetHandleInformation);
    MYWIN_EXPORT(GetStdHandle);
    MYWIN_EXPORT(SetStdHandle);
    MYWIN_EXPORT(CreateProcessA);
    MYWIN_EXPORT(OpenProcess);
    MYWIN_EXPORT(OpenThread);
    MYWIN_EXPORT(OpenProcessToken);
    MYWIN_EXPORT(GetTokenInformation);
    MYWIN_EXPORT(ExitProcess);
    MYWIN_EXPORT(TerminateProcess);
    MYWIN_EXPORT(GetExitCodeProcess);
    MYWIN_EXPORT(ShellExecuteA);
    MYWIN_EXPORT(ShellExecuteExA);
    MYWIN_EXPORT(RegisterClassExA);
    MYWIN_EXPORT(CreateWindowExA);
    MYWIN_EXPORT(RegisterDialogTemplateA);
    MYWIN_EXPORT(FindDialogTemplateA);
    MYWIN_EXPORT(DialogBoxIndirectParamA);
    MYWIN_EXPORT(CreateDialogIndirectParamA);
    MYWIN_EXPORT(DialogBoxParamA);
    MYWIN_EXPORT(CreateDialogParamA);
    MYWIN_EXPORT(EndDialog);
    MYWIN_EXPORT(IsDialogMessageA);
    MYWIN_EXPORT(IsDialogMessageW);
    MYWIN_EXPORT(MapDialogRect);
    MYWIN_EXPORT(MyIsDialogWindow);
    MYWIN_EXPORT(DefWindowProcA);
    MYWIN_EXPORT(DefDlgProcA);
    MYWIN_EXPORT(DestroyWindow);
    MYWIN_EXPORT(PostMessageA);
    MYWIN_EXPORT(SendMessageA);
    MYWIN_EXPORT(GetMessageA);
    MYWIN_EXPORT(PeekMessageA);
    MYWIN_EXPORT(SetTimer);
    MYWIN_EXPORT(KillTimer);
    MYWIN_EXPORT(DispatchMessageA);
    MYWIN_EXPORT(InvalidateRect);
    MYWIN_EXPORT(ValidateRect);
    MYWIN_EXPORT(GetUpdateRect);
    MYWIN_EXPORT(UpdateWindow);
    MYWIN_EXPORT(RedrawWindow);
    MYWIN_EXPORT(InvalidateRgn);
    MYWIN_EXPORT(ValidateRgn);
    MYWIN_EXPORT(GetUpdateRgn);
    MYWIN_EXPORT(CreateRectRgn);
    MYWIN_EXPORT(CreateRectRgnIndirect);
    MYWIN_EXPORT(SetRectRgn);
    MYWIN_EXPORT(OffsetRgn);
    MYWIN_EXPORT(GetRgnBox);
    MYWIN_EXPORT(CombineRgn);
    MYWIN_EXPORT(EqualRgn);
    MYWIN_EXPORT(PtInRegion);
    MYWIN_EXPORT(RectInRegion);
    MYWIN_EXPORT(SelectClipRgn);
    MYWIN_EXPORT(ExcludeClipRect);
    MYWIN_EXPORT(IntersectClipRect);
    MYWIN_EXPORT(GetClipBox);
    MYWIN_EXPORT(ScrollWindow);
    MYWIN_EXPORT(ScrollWindowEx);
    MYWIN_EXPORT(SetCapture);
    MYWIN_EXPORT(ReleaseCapture);
    MYWIN_EXPORT(GetCapture);
    MYWIN_EXPORT(GetFocus);
    MYWIN_EXPORT(SetFocus);
    MYWIN_EXPORT(SetForegroundWindow);
    MYWIN_EXPORT(GetClassNameA);
    MYWIN_EXPORT(GetClassLongA);
    MYWIN_EXPORT(SetClassLongA);
    MYWIN_EXPORT(GetClassLongPtrA);
    MYWIN_EXPORT(SetClassLongPtrA);
    MYWIN_EXPORT(GetWindowLongA);
    MYWIN_EXPORT(SetWindowLongA);
    MYWIN_EXPORT(GetWindowLongPtrA);
    MYWIN_EXPORT(SetWindowLongPtrA);
    MYWIN_EXPORT(CallWindowProcA);
    MYWIN_EXPORT(GetWindowTextA);
    MYWIN_EXPORT(SetWindowTextA);
    MYWIN_EXPORT(GetWindowTextLengthA);
    MYWIN_EXPORT(GetDlgItem);
    MYWIN_EXPORT(GetDlgCtrlID);
    MYWIN_EXPORT(GetNextDlgTabItem);
    MYWIN_EXPORT(GetNextDlgGroupItem);
    MYWIN_EXPORT(SendDlgItemMessageA);
    MYWIN_EXPORT(SetDlgItemTextA);
    MYWIN_EXPORT(GetDlgItemTextA);
    MYWIN_EXPORT(GetDlgItemInt);
    MYWIN_EXPORT(SetDlgItemInt);
    MYWIN_EXPORT(EnableWindow);
    MYWIN_EXPORT(IsWindowEnabled);
    MYWIN_EXPORT(CheckDlgButton);
    MYWIN_EXPORT(CheckRadioButton);
    MYWIN_EXPORT(IsDlgButtonChecked);
    MYWIN_EXPORT(SetScrollPos);
    MYWIN_EXPORT(GetScrollPos);
    MYWIN_EXPORT(SetScrollRange);
    MYWIN_EXPORT(GetScrollRange);
    MYWIN_EXPORT(SetScrollInfo);
    MYWIN_EXPORT(GetScrollInfo);
    MYWIN_EXPORT(ShowScrollBar);
    MYWIN_EXPORT(EnableScrollBar);
    MYWIN_EXPORT(GlobalAlloc);
    MYWIN_EXPORT(GlobalLock);
    MYWIN_EXPORT(GlobalUnlock);
    MYWIN_EXPORT(GlobalFree);
    MYWIN_EXPORT(OpenClipboard);
    MYWIN_EXPORT(CloseClipboard);
    MYWIN_EXPORT(EmptyClipboard);
    MYWIN_EXPORT(IsClipboardFormatAvailable);
    MYWIN_EXPORT(SetClipboardData);
    MYWIN_EXPORT(GetClipboardData);
    MYWIN_EXPORT(CreateMenu);
    MYWIN_EXPORT(CreatePopupMenu);
    MYWIN_EXPORT(AppendMenuA);
    MYWIN_EXPORT(InsertMenuA);
    MYWIN_EXPORT(ModifyMenuA);
    MYWIN_EXPORT(RemoveMenu);
    MYWIN_EXPORT(DeleteMenu);
    MYWIN_EXPORT(CheckMenuItem);
    MYWIN_EXPORT(EnableMenuItem);
    MYWIN_EXPORT(SetMenu);
    MYWIN_EXPORT(GetMenu);
    MYWIN_EXPORT(DrawMenuBar);
    MYWIN_EXPORT(DestroyMenu);
    MYWIN_EXPORT(GetSubMenu);
    MYWIN_EXPORT(GetMenuItemCount);
    MYWIN_EXPORT(GetMenuItemID);
    MYWIN_EXPORT(GetMenuItemInfoA);
    MYWIN_EXPORT(GetSystemMenu);
    MYWIN_EXPORT(TrackPopupMenu);
    MYWIN_EXPORT(TrackPopupMenuEx);
    MYWIN_EXPORT(GetOpenFileNameA);
    MYWIN_EXPORT(GetSaveFileNameA);
    MYWIN_EXPORT(ChooseFontA);
    MYWIN_EXPORT(CommDlgExtendedError);
    MYWIN_EXPORT(CreateAcceleratorTableA);
    MYWIN_EXPORT(TranslateAcceleratorA);
    MYWIN_EXPORT(DestroyAcceleratorTable);
    MYWIN_EXPORT(ShowWindow);
    MYWIN_EXPORT(IsWindow);
    MYWIN_EXPORT(IsWindowVisible);
    MYWIN_EXPORT(BeginDeferWindowPos);
    MYWIN_EXPORT(DeferWindowPos);
    MYWIN_EXPORT(EndDeferWindowPos);
    MYWIN_EXPORT(GetWindowPlacement);
    MYWIN_EXPORT(SetWindowPlacement);
    MYWIN_EXPORT(GetWindowRect);
    MYWIN_EXPORT(GetClientRect);
    MYWIN_EXPORT(ScreenToClient);
    MYWIN_EXPORT(ClientToScreen);
    MYWIN_EXPORT(MapWindowPoints);
    MYWIN_EXPORT(SetWindowPos);
    MYWIN_EXPORT(MoveWindow);
    MYWIN_EXPORT(GetParent);
    MYWIN_EXPORT(SetParent);
    MYWIN_EXPORT(GetAncestor);
    MYWIN_EXPORT(GetTopWindow);
    MYWIN_EXPORT(BringWindowToTop);
    MYWIN_EXPORT(GetWindow);
    MYWIN_EXPORT(IsChild);
    MYWIN_EXPORT(EnumChildWindows);
    MYWIN_EXPORT(ChildWindowFromPoint);
    MYWIN_EXPORT(WindowFromPoint);
    MYWIN_EXPORT(ChildWindowFromPointEx);
    MYWIN_EXPORT(RealChildWindowFromPoint);
    MYWIN_EXPORT(FindWindowA);
    MYWIN_EXPORT(FindWindowExA);
    MYWIN_EXPORT(EnumWindows);
    MYWIN_EXPORT(GetForegroundWindow);
    MYWIN_EXPORT(SetForegroundWindow);
    MYWIN_EXPORT(GetDesktopWindow);
    MYWIN_EXPORT(GetCurrentProcessId);
    MYWIN_EXPORT(GetCurrentThreadId);
    MYWIN_EXPORT(GetCurrentThread);
    MYWIN_EXPORT(BeginPaint);
    MYWIN_EXPORT(EndPaint);
    MYWIN_EXPORT(GetDC);
    MYWIN_EXPORT(ReleaseDC);
    MYWIN_EXPORT(CreateCompatibleDC);
    MYWIN_EXPORT(DeleteDC);
    MYWIN_EXPORT(CreateSolidBrush);
    MYWIN_EXPORT(CreateCompatibleBitmap);
    MYWIN_EXPORT(CreateBitmap);
    MYWIN_EXPORT(CreateDIBSection);
    MYWIN_EXPORT(DeleteObject);
    MYWIN_EXPORT(SelectObject);
    MYWIN_EXPORT(FillRect);
    MYWIN_EXPORT(Rectangle);
    MYWIN_EXPORT(TextOutA);
    MYWIN_EXPORT(DrawTextA);
    MYWIN_EXPORT(GetObjectA);
    MYWIN_EXPORT(SetPixel);
    MYWIN_EXPORT(GetPixel);
    MYWIN_EXPORT(BitBlt);
    MYWIN_EXPORT(PatBlt);
    MYWIN_EXPORT(StretchBlt);
    MYWIN_EXPORT(GetStretchBltMode);
    MYWIN_EXPORT(SetStretchBltMode);
    MYWIN_EXPORT(GetDIBits);
    MYWIN_EXPORT(SetDIBits);
    MYWIN_EXPORT(StretchDIBits);
    MYWIN_EXPORT(SetDIBitsToDevice);
#undef MYWIN_EXPORT
    return NULL;
}

LPSTR GetCommandLineA(void)
{
    // v49: expose the loader handoff command line through a WinAPI-shaped call.
    // The returned pointer is process-runtime storage, just like the current
    // PoC runtime context itself.  If no explicit command line was supplied,
    // fall back to the image name so simple apps still see something useful.
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    LPSTR ret = (LPSTR)"";
    if (p) ret = p->commandLine[0] ? p->commandLine : p->imageName;
    pthread_mutex_unlock(&g_ProcessLock);
    return ret;
}

void GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo)
{
    if (!lpStartupInfo) return;
    memset(lpStartupInfo, 0, sizeof(*lpStartupInfo));
    lpStartupInfo->cb = sizeof(*lpStartupInfo);

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (p) {
        lpStartupInfo->lpTitle = p->windowTitle[0] ? p->windowTitle : p->imageName;
        lpStartupInfo->dwX = p->startupX;
        lpStartupInfo->dwY = p->startupY;
        lpStartupInfo->dwXSize = p->startupW;
        lpStartupInfo->dwYSize = p->startupH;
        lpStartupInfo->dwFlags = p->startupFlags;
        lpStartupInfo->wShowWindow = p->showWindow;
        lpStartupInfo->hStdInput = p->stdInput;
        lpStartupInfo->hStdOutput = p->stdOutput;
        lpStartupInfo->hStdError = p->stdError;
    }
    pthread_mutex_unlock(&g_ProcessLock);
}

static BOOL mywin_std_handle_slot(DWORD nStdHandle, int* slot)
{
    if (!slot) return FALSE;
    if (nStdHandle == STD_INPUT_HANDLE)  { *slot = 0; return TRUE; }
    if (nStdHandle == STD_OUTPUT_HANDLE) { *slot = 1; return TRUE; }
    if (nStdHandle == STD_ERROR_HANDLE)  { *slot = 2; return TRUE; }
    return FALSE;
}

HANDLE GetStdHandle(DWORD nStdHandle)
{
    int slot = -1;
    if (!mywin_std_handle_slot(nStdHandle, &slot)) {
        mywin_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE ret = 0;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (p) {
        ret = (slot == 0) ? p->stdInput : (slot == 1 ? p->stdOutput : p->stdError);
        mywin_set_last_error_locked(p, ERROR_SUCCESS);
    } else {
        g_LastErrorFallback = ERROR_INVALID_HANDLE;
        ret = INVALID_HANDLE_VALUE;
    }
    pthread_mutex_unlock(&g_ProcessLock);
    return ret;
}

BOOL SetStdHandle(DWORD nStdHandle, HANDLE hHandle)
{
    int slot = -1;
    if (!mywin_std_handle_slot(nStdHandle, &slot)) {
        mywin_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (!p) {
        g_LastErrorFallback = ERROR_INVALID_HANDLE;
        pthread_mutex_unlock(&g_ProcessLock);
        return FALSE;
    }
    if (slot == 0) p->stdInput = hHandle;
    else if (slot == 1) p->stdOutput = hHandle;
    else p->stdError = hHandle;
    mywin_set_last_error_locked(p, ERROR_SUCCESS);
    pthread_mutex_unlock(&g_ProcessLock);
    return TRUE;
}

DWORD GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer)
{
    char dir[128];
    dir[0] = 0;
    DWORD err = ERROR_SUCCESS;

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (p && p->currentDirectory[0]) snprintf(dir, sizeof(dir), "%s", p->currentDirectory);
    else if (!p) err = ERROR_INVALID_HANDLE;
    pthread_mutex_unlock(&g_ProcessLock);

    if (!dir[0]) snprintf(dir, sizeof(dir), "/");
    DWORD len = (DWORD)strlen(dir);
    if (!lpBuffer || nBufferLength == 0 || nBufferLength <= len) {
        mywin_set_last_error(err == ERROR_SUCCESS ? ERROR_INSUFFICIENT_BUFFER : err);
        return len + 1; // Windows-style required size includes the trailing NUL when too small.
    }
    snprintf(lpBuffer, nBufferLength, "%s", dir);
    mywin_set_last_error(err);
    return len;
}

BOOL SetCurrentDirectoryA(LPCSTR lpPathName)
{
    if (!lpPathName || !lpPathName[0]) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    size_t len = strlen(lpPathName);
    if (len >= 128) { mywin_set_last_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (!p) { pthread_mutex_unlock(&g_ProcessLock); mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    snprintf(p->currentDirectory, sizeof(p->currentDirectory), "%s", lpPathName);
    mywin_set_last_error_locked(p, ERROR_SUCCESS);
    pthread_mutex_unlock(&g_ProcessLock);
    return TRUE;
}



// v118: COMDLG32 public entrypoints live in commdlg.c
//   CommDlgExtendedError / GetOpenFileNameA / GetSaveFileNameA / ChooseFontA


DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize)
{
    if (!lpName || !lpName[0]) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    char value[MYWIN_MAX_ENV_VALUE];
    value[0] = 0;
    BOOL found = FALSE;

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    int idx = mywin_env_find_locked(p, lpName);
    if (idx >= 0) {
        snprintf(value, sizeof(value), "%s", p->envValue[idx]);
        found = TRUE;
    }
    if (!p) mywin_set_last_error_locked(NULL, ERROR_INVALID_HANDLE);
    pthread_mutex_unlock(&g_ProcessLock);

    if (!found) { mywin_set_last_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
    DWORD len = (DWORD)strlen(value);
    if (!lpBuffer || nSize == 0) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return len + 1; }
    if (nSize <= len) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return len + 1; }
    snprintf(lpBuffer, nSize, "%s", value);
    mywin_set_last_error(ERROR_SUCCESS);
    return len;
}

BOOL SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue)
{
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    BOOL ok = p ? mywin_env_set_locked(p, lpName, lpValue) : FALSE;
    mywin_set_last_error_locked(p, ok ? ERROR_SUCCESS : (p ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE));
    pthread_mutex_unlock(&g_ProcessLock);
    return ok;
}

DWORD ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize)
{
    if (!lpSrc) return 0;

    char out[512];
    size_t used = 0;
    const char* s = lpSrc;

    while (*s && used + 1 < sizeof(out)) {
        if (*s == '%') {
            const char* end = strchr(s + 1, '%');
            if (end && end > s + 1) {
                char name[MYWIN_MAX_ENV_NAME];
                size_t nl = (size_t)(end - (s + 1));
                if (nl >= sizeof(name)) nl = sizeof(name) - 1;
                snprintf(name, sizeof(name), "%.*s", (int)nl, s + 1);

                char val[MYWIN_MAX_ENV_VALUE];
                val[0] = 0;
                BOOL found = FALSE;
                pthread_mutex_lock(&g_ProcessLock);
                MyWinProcessLite* p = mywin_current_lite_process_locked();
                int idx = mywin_env_find_locked(p, name);
                if (idx >= 0) { snprintf(val, sizeof(val), "%s", p->envValue[idx]); found = TRUE; }
                pthread_mutex_unlock(&g_ProcessLock);

                const char* repl = found ? val : NULL;
                if (repl) {
                    for (const char* r = repl; *r && used + 1 < sizeof(out); ++r) out[used++] = *r;
                } else {
                    for (const char* r = s; r <= end && used + 1 < sizeof(out); ++r) out[used++] = *r;
                }
                s = end + 1;
                continue;
            }
        }
        out[used++] = *s++;
    }
    out[used] = 0;

    DWORD required = (DWORD)used + 1;
    if (!lpDst || nSize == 0) return required;
    if (nSize < required) {
        if (nSize > 0) {
            size_t copy = nSize - 1;
            memcpy(lpDst, out, copy);
            lpDst[copy] = 0;
        }
        return required;
    }
    snprintf(lpDst, nSize, "%s", out);
    return required;
}


HMODULE GetModuleHandleA(LPCSTR lpModuleName)
{
    // v52/v53: main module plus per-process DLL table.  NULL still asks for
    // the current process image; non-NULL can resolve either the EXE image or
    // a loaded DLL token in the current Process-Lite context.
    HMODULE ret = 0;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (p) {
        if (!lpModuleName || !lpModuleName[0] || mywin_module_matches_locked(p, lpModuleName)) {
            ret = p->mainModule;
        } else {
            MyWinLoadedModule* m = mywin_find_loaded_module_by_name_locked(p, lpModuleName);
            if (m) ret = m->module;
        }
        mywin_set_last_error_locked(p, ret ? ERROR_SUCCESS : ERROR_MOD_NOT_FOUND);
    } else {
        mywin_set_last_error_locked(NULL, ERROR_INVALID_HANDLE);
    }
    pthread_mutex_unlock(&g_ProcessLock);
    return ret;
}

DWORD GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize)
{
    char path[MYWIN_MAX_MODULE_PATH];
    path[0] = 0;
    DWORD err = ERROR_SUCCESS;

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (p) {
        if (!hModule || hModule == p->mainModule) {
            snprintf(path, sizeof(path), "%s", p->imagePath[0] ? p->imagePath : p->imageName);
        } else {
            MyWinLoadedModule* m = mywin_find_loaded_module_by_handle_locked(p, hModule);
            if (m) snprintf(path, sizeof(path), "%s", m->imagePath[0] ? m->imagePath : m->moduleName);
            else err = ERROR_INVALID_HANDLE;
        }
    } else {
        err = ERROR_INVALID_HANDLE;
    }
    pthread_mutex_unlock(&g_ProcessLock);

    if (!path[0]) { mywin_set_last_error(err); return 0; }
    DWORD len = (DWORD)strlen(path);
    if (!lpFilename || nSize == 0) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return len; }
    if (nSize <= len) {
        DWORD copy = nSize > 0 ? nSize - 1 : 0;
        if (copy) memcpy(lpFilename, path, copy);
        lpFilename[copy] = 0;
        mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return nSize;
    }
    snprintf(lpFilename, nSize, "%s", path);
    mywin_set_last_error(ERROR_SUCCESS);
    return len;
}

HMODULE LoadLibraryA(LPCSTR lpLibFileName)
{
    if (!lpLibFileName || !lpLibFileName[0]) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return 0; }

    char norm[MYWIN_MAX_MODULE_NAME];
    char path[MYWIN_MAX_MODULE_PATH];
    mywin_normalize_dll_name(lpLibFileName, norm, sizeof(norm), path, sizeof(path));
    if (!norm[0]) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!mywin_is_known_virtual_dll(norm)) { mywin_set_last_error(ERROR_MOD_NOT_FOUND); return 0; }

    HMODULE ret = 0;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (p) {
        mywin_resolve_dll_path_locked(p, lpLibFileName, norm, path, sizeof(path));
        MyWinLoadedModule* existing = mywin_find_loaded_module_by_name_locked(p, norm);
        if (existing) {
            existing->refCount++;
            ret = existing->module;
        } else {
            for (DWORD i = 1; i < MYWIN_MAX_LOADED_MODULES; i++) {
                MyWinLoadedModule* m = &p->loadedModules[i];
                if (m->valid) continue;
                memset(m, 0, sizeof(*m));
                m->valid = 1;
                m->refCount = 1;
                m->module = mywin_make_dll_module_handle(p->pid, i);
                snprintf(m->moduleName, sizeof(m->moduleName), "%s", norm);
                snprintf(m->imagePath, sizeof(m->imagePath), "%s", path[0] ? path : norm);
                p->moduleSerial++;
                ret = m->module;
                break;
            }
        }
        mywin_set_last_error_locked(p, ret ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY);
    } else {
        mywin_set_last_error_locked(NULL, ERROR_INVALID_HANDLE);
    }
    pthread_mutex_unlock(&g_ProcessLock);
    return ret;
}

BOOL FreeLibrary(HMODULE hLibModule)
{
    if (!hLibModule) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }

    BOOL ok = FALSE;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (p && hLibModule != p->mainModule) {
        MyWinLoadedModule* m = mywin_find_loaded_module_by_handle_locked(p, hLibModule);
        if (m) {
            if (m->refCount > 1) m->refCount--;
            else memset(m, 0, sizeof(*m));
            p->moduleSerial++;
            ok = TRUE;
        }
    }
    mywin_set_last_error_locked(p, ok ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
    pthread_mutex_unlock(&g_ProcessLock);
    return ok;
}

FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    if (!hModule || !lpProcName) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }

    char moduleName[MYWIN_MAX_MODULE_NAME];
    moduleName[0] = 0;
    BOOL validModule = FALSE;

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (p) {
        if (hModule == p->mainModule) {
            snprintf(moduleName, sizeof(moduleName), "%s", p->moduleName[0] ? p->moduleName : p->imageName);
            validModule = TRUE;
        } else {
            MyWinLoadedModule* m = mywin_find_loaded_module_by_handle_locked(p, hModule);
            if (m) {
                snprintf(moduleName, sizeof(moduleName), "%s", m->moduleName);
                validModule = TRUE;
            }
        }
    }
    pthread_mutex_unlock(&g_ProcessLock);

    if (!validModule) { mywin_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    FARPROC fp = mywin_resolve_virtual_export(moduleName, lpProcName);
    mywin_set_last_error(fp ? ERROR_SUCCESS : ERROR_PROC_NOT_FOUND);
    return fp;
}


/* ──────────────────────────────────────────────────────────────────────
   v198 Resource/template handle model

   HINSTANCE/HMODULE/HGLOBAL/HRSRC are 32-bit opaque handles in myOS.  Common
   dialogs must therefore never treat hInstance as a raw pointer when
   OFN/CF_ENABLETEMPLATEHANDLE is set.  This small resource table gives the
   loader/resource APIs a real handle domain that later PE .rsrc loading can
   feed directly.  The table stores borrowed immutable bytes; ownership stays
   with the module/static blob or caller-provided HGLOBAL.
   ────────────────────────────────────────────────────────────────────── */
#define MYWIN_MAX_RESOURCES 64

typedef struct MyWinResourceKeyA {
    BOOL ordinal;
    WORD ord;
    char name[96];
} MyWinResourceKeyA;

typedef struct MyWinResourceEntry {
    int valid;
    HMODULE module;
    HRSRC resource;
    HGLOBAL dataHandle;
    MyWinResourceKeyA name;
    MyWinResourceKeyA type;
    const BYTE* data;
    DWORD size;
} MyWinResourceEntry;

static pthread_mutex_t g_ResourceLock = PTHREAD_MUTEX_INITIALIZER;
static MyWinResourceEntry g_Resources[MYWIN_MAX_RESOURCES];
static HRSRC   g_NextResourceHandle = 0xb1000001u;
static HGLOBAL g_NextResourceDataHandle = 0xb2000001u;

static BOOL mywin_resource_key_from_lpcstr(LPCSTR s, MyWinResourceKeyA* out)
{
    if (!s || !out) return FALSE;
    memset(out, 0, sizeof(*out));
    if (IS_INTRESOURCE(s)) {
        out->ordinal = TRUE;
        out->ord = (WORD)(ULONG_PTR)s;
        return TRUE;
    }
    if (!s[0]) return FALSE;
    snprintf(out->name, sizeof(out->name), "%s", s);
    return TRUE;
}

static BOOL mywin_resource_key_equal(const MyWinResourceKeyA* a, const MyWinResourceKeyA* b)
{
    if (!a || !b) return FALSE;
    if (a->ordinal != b->ordinal) return FALSE;
    if (a->ordinal) return a->ord == b->ord;
    return strcasecmp(a->name, b->name) == 0;
}

static HMODULE mywin_resource_default_module(HMODULE hModule)
{
    if (hModule) return hModule;
    HMODULE h = GetModuleHandleA(NULL);
    return h ? h : (HMODULE)1u;
}

BOOL MyWinRegisterResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType, LPCVOID lpData, DWORD cbData)
{
    MyWinResourceKeyA name, type;
    if (!mywin_resource_key_from_lpcstr(lpName, &name) ||
        !mywin_resource_key_from_lpcstr(lpType, &type) ||
        !lpData || cbData == 0) {
        mywin_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    HMODULE module = mywin_resource_default_module(hModule);

    pthread_mutex_lock(&g_ResourceLock);
    for (int i = 0; i < MYWIN_MAX_RESOURCES; ++i) {
        MyWinResourceEntry* e = &g_Resources[i];
        if (!e->valid) continue;
        if (e->module == module && mywin_resource_key_equal(&e->name, &name) && mywin_resource_key_equal(&e->type, &type)) {
            e->data = (const BYTE*)lpData;
            e->size = cbData;
            pthread_mutex_unlock(&g_ResourceLock);
            mywin_set_last_error(ERROR_SUCCESS);
            return TRUE;
        }
    }
    for (int i = 0; i < MYWIN_MAX_RESOURCES; ++i) {
        MyWinResourceEntry* e = &g_Resources[i];
        if (e->valid) continue;
        memset(e, 0, sizeof(*e));
        e->valid = 1;
        e->module = module;
        e->resource = g_NextResourceHandle++;
        e->dataHandle = g_NextResourceDataHandle++;
        e->name = name;
        e->type = type;
        e->data = (const BYTE*)lpData;
        e->size = cbData;
        pthread_mutex_unlock(&g_ResourceLock);
        mywin_set_last_error(ERROR_SUCCESS);
        return TRUE;
    }
    pthread_mutex_unlock(&g_ResourceLock);
    mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
}

BOOL MyWinRegisterDialogTemplateResourceA(HMODULE hModule, LPCSTR lpName, LPCDLGTEMPLATEA lpTemplate, DWORD cbTemplate)
{
    return MyWinRegisterResourceA(hModule, lpName, RT_DIALOG, lpTemplate, cbTemplate);
}

static MyWinResourceEntry* mywin_resource_find_by_hrsrc_locked(HRSRC hResInfo)
{
    if (!hResInfo) return NULL;
    for (int i = 0; i < MYWIN_MAX_RESOURCES; ++i)
        if (g_Resources[i].valid && g_Resources[i].resource == hResInfo) return &g_Resources[i];
    return NULL;
}

static MyWinResourceEntry* mywin_resource_find_by_data_locked(HGLOBAL hData)
{
    if (!hData) return NULL;
    for (int i = 0; i < MYWIN_MAX_RESOURCES; ++i)
        if (g_Resources[i].valid && g_Resources[i].dataHandle == hData) return &g_Resources[i];
    return NULL;
}

HRSRC FindResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType)
{
    MyWinResourceKeyA name, type;
    if (!mywin_resource_key_from_lpcstr(lpName, &name) || !mywin_resource_key_from_lpcstr(lpType, &type)) {
        mywin_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    HMODULE module = mywin_resource_default_module(hModule);
    HRSRC ret = 0;
    pthread_mutex_lock(&g_ResourceLock);
    for (int i = 0; i < MYWIN_MAX_RESOURCES; ++i) {
        MyWinResourceEntry* e = &g_Resources[i];
        if (!e->valid || e->module != module) continue;
        if (mywin_resource_key_equal(&e->name, &name) && mywin_resource_key_equal(&e->type, &type)) {
            ret = e->resource;
            break;
        }
    }
    pthread_mutex_unlock(&g_ResourceLock);
    mywin_set_last_error(ret ? ERROR_SUCCESS : ERROR_RESOURCE_NAME_NOT_FOUND);
    return ret;
}

HGLOBAL LoadResource(HMODULE hModule, HRSRC hResInfo)
{
    HMODULE module = mywin_resource_default_module(hModule);
    HGLOBAL ret = 0;
    pthread_mutex_lock(&g_ResourceLock);
    MyWinResourceEntry* e = mywin_resource_find_by_hrsrc_locked(hResInfo);
    if (e && (!hModule || e->module == module)) ret = e->dataHandle;
    pthread_mutex_unlock(&g_ResourceLock);
    mywin_set_last_error(ret ? ERROR_SUCCESS : ERROR_RESOURCE_DATA_NOT_FOUND);
    return ret;
}

LPVOID LockResource(HGLOBAL hResData)
{
    LPVOID ret = NULL;
    pthread_mutex_lock(&g_ResourceLock);
    MyWinResourceEntry* e = mywin_resource_find_by_data_locked(hResData);
    if (e) ret = (LPVOID)e->data;
    pthread_mutex_unlock(&g_ResourceLock);
    mywin_set_last_error(ret ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
    return ret;
}

DWORD SizeofResource(HMODULE hModule, HRSRC hResInfo)
{
    HMODULE module = mywin_resource_default_module(hModule);
    DWORD ret = 0;
    pthread_mutex_lock(&g_ResourceLock);
    MyWinResourceEntry* e = mywin_resource_find_by_hrsrc_locked(hResInfo);
    if (e && (!hModule || e->module == module)) ret = e->size;
    pthread_mutex_unlock(&g_ResourceLock);
    mywin_set_last_error(ret ? ERROR_SUCCESS : ERROR_RESOURCE_DATA_NOT_FOUND);
    return ret;
}

BOOL SetDllDirectoryA(LPCSTR lpPathName)
{
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (!p) { pthread_mutex_unlock(&g_ProcessLock); mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (lpPathName && strlen(lpPathName) >= sizeof(p->dllDirectory)) {
        mywin_set_last_error_locked(p, ERROR_FILENAME_EXCED_RANGE);
        pthread_mutex_unlock(&g_ProcessLock);
        return FALSE;
    }
    snprintf(p->dllDirectory, sizeof(p->dllDirectory), "%s", lpPathName ? lpPathName : "");
    mywin_set_last_error_locked(p, ERROR_SUCCESS);
    pthread_mutex_unlock(&g_ProcessLock);
    return TRUE;
}

DWORD GetDllDirectoryA(DWORD nBufferLength, LPSTR lpBuffer)
{
    char dir[MYWIN_MAX_MODULE_PATH];
    dir[0] = 0;
    DWORD err = ERROR_SUCCESS;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_current_lite_process_locked();
    if (p) snprintf(dir, sizeof(dir), "%s", p->dllDirectory);
    else err = ERROR_INVALID_HANDLE;
    pthread_mutex_unlock(&g_ProcessLock);

    DWORD len = (DWORD)strlen(dir);
    if (!lpBuffer || nBufferLength == 0 || nBufferLength <= len) {
        mywin_set_last_error(err == ERROR_SUCCESS ? ERROR_INSUFFICIENT_BUFFER : err);
        return len + 1;
    }
    snprintf(lpBuffer, nBufferLength, "%s", dir);
    mywin_set_last_error(err);
    return len;
}

BOOL MyWinGetRuntimeContextInfo(DWORD dwProcessId, MyRuntimeContextInfo* lpInfo)
{
    if (!lpInfo) return FALSE;
    memset(lpInfo, 0, sizeof(*lpInfo));
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(dwProcessId);
    if (!p) { pthread_mutex_unlock(&g_ProcessLock); return FALSE; }
    lpInfo->pid = p->pid;
    lpInfo->parent_pid = p->parentPid;
    lpInfo->thread_id = p->tid;
    lpInfo->flags = p->flags;
    lpInfo->exit_code = p->exitCode;
    lpInfo->cap_flags = p->hasCap ? p->cap.flags : 0;
    lpInfo->runtime_enters = p->runtimeEnters;
    lpInfo->runtime_depth = (DWORD)g_RuntimeDepth;
    lpInfo->startup_flags = p->startupFlags;
    lpInfo->show_window = p->showWindow;
    lpInfo->std_input = p->stdInput;
    lpInfo->std_output = p->stdOutput;
    lpInfo->std_error = p->stdError;
    snprintf(lpInfo->image_name, sizeof(lpInfo->image_name), "%s", p->imageName);
    snprintf(lpInfo->image_path, sizeof(lpInfo->image_path), "%s", p->imagePath);
    snprintf(lpInfo->module_name, sizeof(lpInfo->module_name), "%s", p->moduleName);
    lpInfo->main_module = p->mainModule;
    snprintf(lpInfo->cap_name, sizeof(lpInfo->cap_name), "%s", p->hasCap ? p->cap.name : "");
    snprintf(lpInfo->command_line, sizeof(lpInfo->command_line), "%s", p->commandLine);
    snprintf(lpInfo->current_directory, sizeof(lpInfo->current_directory), "%s", p->currentDirectory);
    snprintf(lpInfo->window_title, sizeof(lpInfo->window_title), "%s", p->windowTitle);
    lpInfo->environment_count = p->environmentCount;
    mywin_env_preview_locked(p, lpInfo->environment_preview, sizeof(lpInfo->environment_preview));
    lpInfo->dll_count = mywin_loaded_module_count_locked(p);
    mywin_dll_preview_locked(p, lpInfo->dll_preview, sizeof(lpInfo->dll_preview));
    snprintf(lpInfo->dll_directory, sizeof(lpInfo->dll_directory), "%s", p->dllDirectory);
    lpInfo->last_error = p->lastError;
    lpInfo->loader_import_count = p->loaderImportCount;
    lpInfo->loader_resolved_count = p->loaderResolvedCount;
    lpInfo->loader_entry_called = p->loaderEntryCalled;
    lpInfo->loader_error = p->loaderError;
    snprintf(lpInfo->loader_entry, sizeof(lpInfo->loader_entry), "%s", p->loaderEntry);
    snprintf(lpInfo->loader_import_preview, sizeof(lpInfo->loader_import_preview), "%s", p->loaderImportPreview);
    snprintf(lpInfo->subsystem, sizeof(lpInfo->subsystem), "%s", p->subsystem);
    lpInfo->argc = p->argc;
    snprintf(lpInfo->argv_preview, sizeof(lpInfo->argv_preview), "%s", p->argvPreview);
    lpInfo->console_exit_code = p->consoleExitCode;
    lpInfo->linux_pid = p->linuxPid;
    lpInfo->linux_status = p->linuxStatus;
    lpInfo->fork_exec = p->forkExec;
    DWORD pid = p->pid;
    pthread_mutex_unlock(&g_ProcessLock);
    lpInfo->handle_count = mywin_process_handle_count(pid);
    MyProcessHostInfo phi;
    if (MyProcessHostGetInfo(pid, &phi)) {
        lpInfo->process_host_state = phi.state;
        lpInfo->process_host_polls = phi.poll_count;
        lpInfo->process_host_reaps = phi.reap_count;
        lpInfo->process_host_kills = phi.kill_count;
        lpInfo->process_host_start_ms = phi.start_ms;
        lpInfo->process_host_exit_ms = phi.exit_ms;
        snprintf(lpInfo->process_host_state_name, sizeof(lpInfo->process_host_state_name), "%s", phi.state_name);
        snprintf(lpInfo->process_host_last_event, sizeof(lpInfo->process_host_last_event), "%s", phi.last_event);
        lpInfo->ipc_enabled = phi.ipc_enabled;
        lpInfo->ipc_messages = phi.ipc_messages;
        lpInfo->ipc_hello = phi.ipc_hello;
        lpInfo->ipc_exit_report = phi.ipc_exit_report;
        lpInfo->ipc_last_opcode = phi.ipc_last_opcode;
        lpInfo->ipc_last_value = phi.ipc_last_value;
        snprintf(lpInfo->ipc_last_text, sizeof(lpInfo->ipc_last_text), "%s", phi.ipc_last_text);
        snprintf(lpInfo->ipc_shared_name, sizeof(lpInfo->ipc_shared_name), "%s", phi.shared_name);
        lpInfo->ipc_shared_heartbeat = phi.shared_heartbeat;
        lpInfo->ipc_shared_child_pid = phi.shared_child_pid;
        lpInfo->ipc_shared_argc = phi.shared_argc;
        lpInfo->ipc_shared_exit_code = phi.shared_exit_code;
        snprintf(lpInfo->ipc_shared_status, sizeof(lpInfo->ipc_shared_status), "%s", phi.shared_status);
        snprintf(lpInfo->ipc_shared_argv_preview, sizeof(lpInfo->ipc_shared_argv_preview), "%s", phi.shared_argv_preview);
    }
    return TRUE;
}


BOOL MyWinSetProcessLoaderInfo(DWORD dwProcessId, DWORD dwImportCount, DWORD dwResolvedCount, DWORD dwLoaderError, LPCSTR lpEntryName, LPCSTR lpImportPreview, BOOL bEntryCalled)
{
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(dwProcessId);
    if (!p) { pthread_mutex_unlock(&g_ProcessLock); mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    p->loaderImportCount = dwImportCount;
    p->loaderResolvedCount = dwResolvedCount;
    p->loaderError = dwLoaderError;
    p->loaderEntryCalled = bEntryCalled ? 1u : 0u;
    snprintf(p->loaderEntry, sizeof(p->loaderEntry), "%s", lpEntryName ? lpEntryName : "");
    snprintf(p->loaderImportPreview, sizeof(p->loaderImportPreview), "%s", lpImportPreview ? lpImportPreview : "");
    p->lastError = dwLoaderError;
    pthread_mutex_unlock(&g_ProcessLock);
    return TRUE;
}

BOOL MyWinSetProcessSubsystemInfo(DWORD dwProcessId, LPCSTR lpSubsystem, DWORD dwArgc, LPCSTR lpArgvPreview, DWORD dwConsoleExitCode)
{
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(dwProcessId);
    if (!p) { pthread_mutex_unlock(&g_ProcessLock); mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    snprintf(p->subsystem, sizeof(p->subsystem), "%s", lpSubsystem && lpSubsystem[0] ? lpSubsystem : "windows");
    p->argc = dwArgc;
    snprintf(p->argvPreview, sizeof(p->argvPreview), "%s", lpArgvPreview ? lpArgvPreview : "");
    p->consoleExitCode = dwConsoleExitCode;
    pthread_mutex_unlock(&g_ProcessLock);
    return TRUE;
}

static void mywin_canonical_name(LPCSTR in, char* out, size_t cb, DWORD* nsOut)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    if (nsOut) *nsOut = _OBJECT_NS_NONE;
    if (!in || !in[0]) return;
    if (strncmp(in, "\\\\", 2) == 0 || in[0] == '\\') {
        snprintf(out, cb, "%s", in);
        if (strncmp(out, "\\BaseNamedObjects\\", 18) == 0) { if (nsOut) *nsOut = _OBJECT_NS_GLOBAL; }
        else if (strncmp(out, "\\Sessions\\", 10) == 0) { if (nsOut) *nsOut = _OBJECT_NS_SESSION; }
        else { if (nsOut) *nsOut = _OBJECT_NS_GLOBAL; }
        return;
    }
    if (strncmp(in, "Global\\", 7) == 0) {
        snprintf(out, cb, "\\BaseNamedObjects\\%s", in + 7);
        if (nsOut) *nsOut = _OBJECT_NS_GLOBAL;
        return;
    }
    if (strncmp(in, "Local\\", 6) == 0) {
        snprintf(out, cb, "\\Sessions\\1\\BaseNamedObjects\\%s", in + 6);
        if (nsOut) *nsOut = _OBJECT_NS_LOCAL;
        return;
    }
    snprintf(out, cb, "\\Sessions\\1\\BaseNamedObjects\\%s", in);
    if (nsOut) *nsOut = _OBJECT_NS_LOCAL;
}

static DWORD mywin_default_sd_for_name(const char* canonical)
{
    if (!canonical || !canonical[0]) return _OBJECT_SD_OWNER_ONLY;
    /* v121: Explicit private naming wins even inside Global\. This gives
       AccessLab/smoke a deterministic owner-only object without a full SD/ACL
       parser yet. */
    if (strstr(canonical, "\\private.") || strstr(canonical, ".private.")) return _OBJECT_SD_OWNER_ONLY;
    if (strncmp(canonical, "\\BaseNamedObjects\\", 18) == 0) return _OBJECT_SD_PUBLIC_READ | _OBJECT_SD_PUBLIC_WRITE;
    return _OBJECT_SD_PUBLIC_READ | _OBJECT_SD_PUBLIC_WRITE;
}

static void mywin_make_section_shm_name(char* out, size_t cb, HANDLE h, const char* canonical)
{
    if (!out || cb == 0) return;
    out[0] = 0;

    /* v73.2: keep POSIX shm backing names short and ABI-safe.
       v73.1 generated names like
         /myos_sec_00006000__BaseNamedObjects_myos_v73_hwnd_state_section
       which are 64 bytes without the NUL. process_ipc.h transports the backing
       name through kernel_map_name[MYOS_IPC_IMAGE_MAX] where MYOS_IPC_IMAGE_MAX
       is 64, so snprintf truncated the last byte before the child called
       shm_open(). The parent had created the full name, the child opened the
       truncated name, and shm_open returned ENOENT.

       Use a compact hash suffix instead. The object handle is already unique
       for the life of the object; the hash only keeps diagnostics stable and
       avoids collisions after handle reuse. */
    uint32_t hash = 2166136261u;
    const char* src = (canonical && canonical[0]) ? canonical : "anon";
    for (size_t i = 0; src[i]; ++i) {
        hash ^= (unsigned char)src[i];
        hash *= 16777619u;
    }
    snprintf(out, cb, "/myos_sec_%08x_%08x", (unsigned)h, (unsigned)hash);
}

static void mywin_destroy_section_storage(MyWinSectionObj* sec)
{
    if (!sec) return;
    if (sec->data && sec->size) munmap(sec->data, sec->size);
    sec->data = NULL;
    if (sec->shmFd >= 0) { close(sec->shmFd); sec->shmFd = -1; }
    if (sec->shmName[0]) shm_unlink(sec->shmName);
}

static BOOL mywin_object_allows_open(HANDLE objectHandle, DWORD desiredAccess)
{
    /* v199: SECURITY_ATTRIBUTES.lpSecurityDescriptor now becomes a real object
       DACL path.  Legacy sd_flags stay as the default descriptor fallback for
       old labs and unnamed bring-up objects. */
    _ObjectectInfo oi;
    if (!_ObjectGetInfo(objectHandle, &oi)) return FALSE;

    DWORD desired = mywin_expand_generic_access(oi.type, desiredAccess ? desiredAccess : SYNCHRONIZE);
    if (oi.sd_control || oi.dacl_present || oi.dacl_null || oi.ace_count) {
        _ObjectToken token;
        mywin_current_token(&token);
        return _ObjectAccessCheck(objectHandle, &token, desired, READ_CONTROL | WRITE_DAC);
    }

    if (oi.owner_pid == mywin_current_pid()) return TRUE;
    if (g_HasCapability && ((g_CurrentCapability.flags & CAP_ADMIN) == CAP_ADMIN)) return TRUE;

    BOOL needsRead = FALSE;
    BOOL needsWrite = FALSE;

    if (desired & (SYNCHRONIZE|READ_CONTROL|FILE_MAP_READ)) needsRead = TRUE;
    if (desired & (EVENT_MODIFY_STATE|MUTEX_MODIFY_STATE|SEMAPHORE_MODIFY_STATE|
                   TIMER_MODIFY_STATE|FILE_MAP_WRITE|PROCESS_DUP_HANDLE|
                   PROCESS_CREATE_PROCESS|WRITE_DAC|WRITE_OWNER|DELETE)) needsWrite = TRUE;

    DWORD known = SYNCHRONIZE|READ_CONTROL|FILE_MAP_READ|EVENT_MODIFY_STATE|
                  MUTEX_MODIFY_STATE|SEMAPHORE_MODIFY_STATE|TIMER_MODIFY_STATE|
                  FILE_MAP_WRITE|PROCESS_DUP_HANDLE|PROCESS_CREATE_PROCESS|
                  WRITE_DAC|WRITE_OWNER|DELETE|STANDARD_RIGHTS_REQUIRED|0x00000008u;
    if ((desired & ~known) != 0) needsWrite = TRUE;
    if (!needsRead && !needsWrite) needsRead = TRUE;

    if (needsRead && !(oi.sd_flags & _OBJECT_SD_PUBLIC_READ)) return FALSE;
    if (needsWrite && !(oi.sd_flags & _OBJECT_SD_PUBLIC_WRITE)) return FALSE;
    return TRUE;
}

static BOOL mywin_access_mask_allows(DWORD have, DWORD needed)
{
    if (needed == 0) return TRUE;
    if (have == _OBJECT_ACCESS_ALL || have == 0xffffffffu) return TRUE;
    return ((have & needed) == needed) ? TRUE : FALSE;
}

static BOOL mywin_is_legacy_raw_handle(HANDLE h, DWORD have)
{
    return (have == 0 && (((uintptr_t)h & 0x80000000u) == 0));
}

static DWORD mywin_map_view_access_to_section_access(DWORD viewAccess)
{
    DWORD need = 0;
    if (viewAccess & FILE_MAP_READ) need |= FILE_MAP_READ;
    if (viewAccess & FILE_MAP_WRITE) need |= FILE_MAP_WRITE;
    if (!need) need = FILE_MAP_READ;
    return need;
}

static inline DWORD mywin_pid_hash(DWORD pid)
{
    DWORD h = pid ? pid : 1u;
    h *= 2654435761u;
    h ^= h >> 16;
    return h ? h : 1u;
}

static inline DWORD mywin_pid_bucket(DWORD pid)
{
    return mywin_pid_hash(pid) & MYWIN_HANDLE_PID_HASH_MASK;
}


static MyWinHandlePidTable* mywin_find_handle_table_locked(DWORD pid, BOOL create)
{
    DWORD bucket = mywin_pid_bucket(pid);
    for (MyWinHandlePidTable* t = g_HandleTablePidHash[bucket]; t; t = t->hash_next) {
        if (t->pid == pid) return t;
    }
    if (!create) return NULL;
    MyWinHandlePidTable* t = (MyWinHandlePidTable*)calloc(1, sizeof(MyWinHandlePidTable));
    if (!t) return NULL;
    t->pid = pid;
    t->alloc_hint = 1;
    t->quota = MYWIN_DEFAULT_HANDLE_QUOTA;
    t->free_capacity = MYWIN_HANDLE_FREE_STACK_INITIAL;
    t->free_stack = (DWORD*)malloc((size_t)t->free_capacity * sizeof(DWORD));
    if (!t->free_stack) t->free_capacity = 0;
    t->epoch = 1;
    mywin_pushlock_init(&t->lock);
    t->next = g_HandleTables;
    g_HandleTables = t;
    t->hash_next = g_HandleTablePidHash[bucket];
    g_HandleTablePidHash[bucket] = t;
    return t;
}

static MyWinHandlePidTable* mywin_get_handle_table_ref(DWORD pid, BOOL create)
{
    if (!pid) return NULL;
    if (MYOS_LIKELY(g_HandleTableLookupCache &&
                    g_HandleTableLookupCachePid == pid &&
                    g_HandleTableLookupCache->pid == pid))
        return g_HandleTableLookupCache;

    MyWinHandlePidTable* t = NULL;
    pthread_mutex_lock(&g_HandleLock);
    t = mywin_find_handle_table_locked(pid, create);
    pthread_mutex_unlock(&g_HandleLock);
    if (t) {
        g_HandleTableLookupCachePid = pid;
        g_HandleTableLookupCache = t;
    }
    return t;
}

static void mywin_handle_slot_indices(DWORD slot, DWORD* topOut, DWORD* midOut, DWORD* leafOut)
{
    DWORD z = slot - 1u;
    if (topOut)  *topOut  = (z >> 16) & 0xffu;
    if (midOut)  *midOut  = (z >> 8) & 0xffu;
    if (leafOut) *leafOut = z & 0xffu;
}

static MyWinHandleLeafPage* mywin_handle_leaf_at_locked(MyWinHandlePidTable* table, DWORD slot, BOOL create)
{
    if (!table || slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS) return NULL;
    DWORD top = 0, mid = 0, leaf = 0;
    (void)leaf;
    mywin_handle_slot_indices(slot, &top, &mid, &leaf);
    if (!table->mid[top]) {
        if (!create) return NULL;
        table->mid[top] = (MyWinHandleMidPage*)calloc(1, sizeof(MyWinHandleMidPage));
        if (!table->mid[top]) return NULL;
    }
    if (!table->mid[top]->leaf[mid]) {
        if (!create) return NULL;
        table->mid[top]->leaf[mid] = (MyWinHandleLeafPage*)calloc(1, sizeof(MyWinHandleLeafPage));
        if (!table->mid[top]->leaf[mid]) return NULL;
    }
    return table->mid[top]->leaf[mid];
}

static MyWinHandleEntry* mywin_handle_entry_at_locked(MyWinHandlePidTable* table, DWORD slot, BOOL create)
{
    if (!table || slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS) return NULL;
    DWORD top = 0, mid = 0, leaf = 0;
    mywin_handle_slot_indices(slot, &top, &mid, &leaf);
    (void)top; (void)mid;
    MyWinHandleLeafPage* page = mywin_handle_leaf_at_locked(table, slot, create);
    return page ? &page->entry[leaf] : NULL;
}

static BOOL mywin_handle_free_mark_get_locked(MyWinHandlePidTable* table, DWORD slot)
{
    if (!table || slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS) return FALSE;
    DWORD top = 0, mid = 0, leaf = 0;
    mywin_handle_slot_indices(slot, &top, &mid, &leaf);
    (void)top; (void)mid;
    MyWinHandleLeafPage* page = mywin_handle_leaf_at_locked(table, slot, FALSE);
    if (!page) return FALSE;
    return (page->free_mark[leaf >> 6] & (1ull << (leaf & 63u))) ? TRUE : FALSE;
}

static void mywin_handle_free_mark_set_locked(MyWinHandlePidTable* table, DWORD slot)
{
    if (!table || slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS) return;
    DWORD top = 0, mid = 0, leaf = 0;
    mywin_handle_slot_indices(slot, &top, &mid, &leaf);
    (void)top; (void)mid;
    MyWinHandleLeafPage* page = mywin_handle_leaf_at_locked(table, slot, TRUE);
    if (page) page->free_mark[leaf >> 6] |= (1ull << (leaf & 63u));
}

static void mywin_handle_free_mark_clear_locked(MyWinHandlePidTable* table, DWORD slot)
{
    if (!table || slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS) return;
    DWORD top = 0, mid = 0, leaf = 0;
    mywin_handle_slot_indices(slot, &top, &mid, &leaf);
    (void)top; (void)mid;
    MyWinHandleLeafPage* page = mywin_handle_leaf_at_locked(table, slot, FALSE);
    if (page) page->free_mark[leaf >> 6] &= ~(1ull << (leaf & 63u));
}

static BOOL mywin_handle_freestack_push_global_locked(MyWinHandlePidTable* table, DWORD slot, BOOL noteHint)
{
    if (!table || slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS) return FALSE;
    if (mywin_handle_free_mark_get_locked(table, slot)) {
        __atomic_add_fetch(&g_HandleFreeMarkDuplicateSkips, 1u, __ATOMIC_RELAXED);
        if (noteHint) {
            g_HandleFreeHintTable = table;
            g_HandleFreeHintSlot = slot;
        }
        return TRUE;
    }
    if (table->free_count == table->free_capacity) {
        DWORD newCap = table->free_capacity ? (table->free_capacity * 2u) : MYWIN_HANDLE_FREE_STACK_INITIAL;
        DWORD* grown = (DWORD*)realloc(table->free_stack, (size_t)newCap * sizeof(DWORD));
        if (!grown) { table->free_stack_grow_failures++; return FALSE; }
        table->free_stack = grown;
        table->free_capacity = newCap;
    }
    table->free_stack[table->free_count++] = slot;
    mywin_handle_free_mark_set_locked(table, slot);
    if (noteHint) {
        g_HandleFreeHintTable = table;
        g_HandleFreeHintSlot = slot;
    }
    return TRUE;
}

static void mywin_handle_free_batch_flush_lane_locked(MyWinHandleFreeBatchLane* lane, MyWinHandlePidTable* table)
{
    if (!lane || !table || lane->table != table || !lane->count) return;
    DWORD flushed = 0;
    for (DWORD i = 0; i < lane->count; ++i) {
        DWORD slot = lane->slots[i];
        MyWinHandleEntry* e = mywin_handle_entry_at_locked(table, slot, FALSE);
        if (slot && slot <= MYWIN_HANDLE_MAX_SLOTS && e && !e->valid &&
            mywin_handle_freestack_push_global_locked(table, slot, FALSE)) {
            flushed++;
        }
    }
    lane->count = 0;
    lane->table = NULL;
    if (flushed) {
        __atomic_add_fetch(&g_HandleFreeBatchFlushes, 1u, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_HandleFreeBatchFlushedSlots, flushed, __ATOMIC_RELAXED);
    }
}

static MyWinHandleFreeBatchLane* mywin_handle_free_batch_find_lane(MyWinHandleFreeBatch* batch, MyWinHandlePidTable* table)
{
    if (!batch || !table) return NULL;
    for (DWORD i = 0; i < MYWIN_HANDLE_FREE_BATCH_LANES; ++i) {
        MyWinHandleFreeBatchLane* lane = &batch->lane[i];
        if (lane->table == table) {
            __atomic_add_fetch(&g_HandleFreeBatchLaneMatches, 1u, __ATOMIC_RELAXED);
            return lane;
        }
    }
    return NULL;
}

static MyWinHandleFreeBatchLane* mywin_handle_free_batch_get_lane_for_store(MyWinHandleFreeBatch* batch, MyWinHandlePidTable* table)
{
    MyWinHandleFreeBatchLane* lane = mywin_handle_free_batch_find_lane(batch, table);
    if (lane) return lane;

    for (DWORD i = 0; i < MYWIN_HANDLE_FREE_BATCH_LANES; ++i) {
        lane = &batch->lane[i];
        if (!lane->table || !lane->count) {
            lane->table = table;
            lane->count = 0;
            __atomic_add_fetch(&g_HandleFreeBatchLaneAllocs, 1u, __ATOMIC_RELAXED);
            return lane;
        }
    }

    /* All TLS lanes are occupied by other process handle tables.  Do not flush
       a foreign table while the current table lock is held; just fall back to
       the already-marked global free stack for this close. */
    __atomic_add_fetch(&g_HandleFreeBatchOverflow, 1u, __ATOMIC_RELAXED);
    return NULL;
}

static void mywin_handle_free_batch_destructor(void* p)
{
    MyWinHandleFreeBatch* batch = (MyWinHandleFreeBatch*)p;
    if (!batch) return;
    for (DWORD i = 0; i < MYWIN_HANDLE_FREE_BATCH_LANES; ++i) {
        MyWinHandleFreeBatchLane* lane = &batch->lane[i];
        if (!lane->table || !lane->count) continue;
        MyWinHandlePidTable* table = lane->table;
        mywin_pushlock_acquire_exclusive(&table->lock);
        mywin_handle_free_batch_flush_lane_locked(lane, table);
        mywin_pushlock_release(&table->lock);
    }
    free(batch);
    if (g_HandleFreeBatchTls == batch) g_HandleFreeBatchTls = NULL;
}

static void mywin_handle_free_batch_flush_tls_if_other(MyWinHandlePidTable* table)
{
    (void)table;
    /* v256: TLS free-batches are multi-lane.  Switching from the current process
       table to a child/foreign table no longer forces a flush; each lane is
       consumed under its own table lock, and destructor-time cleanup flushes the
       remaining lanes back to the marked global stack. */
}


static void mywin_handle_free_batch_drop_tls_table(MyWinHandlePidTable* table)
{
    MyWinHandleFreeBatch* batch = g_HandleFreeBatchTls;
    if (!batch || !table) return;
    for (DWORD i = 0; i < MYWIN_HANDLE_FREE_BATCH_LANES; ++i) {
        MyWinHandleFreeBatchLane* lane = &batch->lane[i];
        if (lane->table == table) {
            lane->table = NULL;
            lane->count = 0;
        }
    }
}

static void mywin_handle_free_batch_make_key(void)
{
    pthread_key_create(&g_HandleFreeBatchKey, mywin_handle_free_batch_destructor);
}

static MyWinHandleFreeBatch* mywin_handle_free_batch_get(void)
{
    if (MYOS_LIKELY(g_HandleFreeBatchTls)) return g_HandleFreeBatchTls;
    pthread_once(&g_HandleFreeBatchKeyOnce, mywin_handle_free_batch_make_key);
    MyWinHandleFreeBatch* batch = (MyWinHandleFreeBatch*)calloc(1, sizeof(MyWinHandleFreeBatch));
    if (!batch) return NULL;
    g_HandleFreeBatchTls = batch;
    pthread_setspecific(g_HandleFreeBatchKey, batch);
    return batch;
}

static BOOL mywin_handle_free_batch_store_locked(MyWinHandlePidTable* table, DWORD slot)
{
    if (!table || slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS) return FALSE;
    MyWinHandleFreeBatch* batch = mywin_handle_free_batch_get();
    if (!batch) return FALSE;

    MyWinHandleFreeBatchLane* lane = mywin_handle_free_batch_get_lane_for_store(batch, table);
    if (!lane) return FALSE;
    if (lane->count == MYWIN_HANDLE_FREE_BATCH_SLOTS) {
        __atomic_add_fetch(&g_HandleFreeBatchOverflow, 1u, __ATOMIC_RELAXED);
        mywin_handle_free_batch_flush_lane_locked(lane, table);
        lane->table = table;
    }
    if (lane->count < MYWIN_HANDLE_FREE_BATCH_SLOTS) {
        lane->slots[lane->count++] = slot;
        __atomic_add_fetch(&g_HandleFreeBatchStores, 1u, __ATOMIC_RELAXED);
        return TRUE;
    }
    return FALSE;
}

static DWORD mywin_handle_free_batch_pop_locked(MyWinHandlePidTable* table)
{
    MyWinHandleFreeBatch* batch = g_HandleFreeBatchTls;
    if (MYOS_UNLIKELY(!batch || !table)) { __atomic_add_fetch(&g_HandleFreeBatchMisses, 1u, __ATOMIC_RELAXED); return 0; }
    MyWinHandleFreeBatchLane* lane = mywin_handle_free_batch_find_lane(batch, table);
    if (MYOS_UNLIKELY(!lane || !lane->count)) { __atomic_add_fetch(&g_HandleFreeBatchMisses, 1u, __ATOMIC_RELAXED); return 0; }
    if (lane != &batch->lane[0]) __atomic_add_fetch(&g_HandleFreeBatchTableSwitchAvoided, 1u, __ATOMIC_RELAXED);
    while (lane->count) {
        DWORD slot = lane->slots[--lane->count];
        MyWinHandleEntry* e = mywin_handle_entry_at_locked(table, slot, FALSE);
        if (MYOS_LIKELY(slot && slot <= MYWIN_HANDLE_MAX_SLOTS && e && !e->valid)) {
            if (!lane->count) lane->table = NULL;
            table->free_reuse_count++;
            __atomic_add_fetch(&g_HandleFreeBatchHits, 1u, __ATOMIC_RELAXED);
            return slot;
        }
        __atomic_add_fetch(&g_HandleFreeStalePops, 1u, __ATOMIC_RELAXED);
    }
    lane->table = NULL;
    __atomic_add_fetch(&g_HandleFreeBatchMisses, 1u, __ATOMIC_RELAXED);
    return 0;
}

static DWORD mywin_handle_tls_free_hint_pop_locked(MyWinHandlePidTable* table)
{
    if (MYOS_UNLIKELY(!table || g_HandleFreeHintTable != table || !g_HandleFreeHintSlot)) return 0;
    DWORD slot = g_HandleFreeHintSlot;
    g_HandleFreeHintSlot = 0;
    g_HandleFreeHintTable = NULL;
    if (MYOS_UNLIKELY(slot == 0 || slot > MYWIN_HANDLE_MAX_SLOTS)) {
        __atomic_add_fetch(&g_HandleFreeHintMisses, 1u, __ATOMIC_RELAXED);
        return 0;
    }
    MyWinHandleEntry* e = mywin_handle_entry_at_locked(table, slot, FALSE);
    if (MYOS_UNLIKELY(!e || e->valid || !mywin_handle_free_mark_get_locked(table, slot))) {
        __atomic_add_fetch(&g_HandleFreeHintMisses, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    if (table->free_count && table->free_stack[table->free_count - 1u] == slot) {
        table->free_count--;
    } else {
        /* The slot was still free, but another close raced a newer entry above it.
           Consume the mark now; the old physical stack entry becomes one bounded
           stale pop instead of an unbounded duplicate-free growth source. */
        __atomic_add_fetch(&g_HandleFreeStalePops, 1u, __ATOMIC_RELAXED);
    }
    mywin_handle_free_mark_clear_locked(table, slot);
    table->free_reuse_count++;
    __atomic_add_fetch(&g_HandleFreeHintHits, 1u, __ATOMIC_RELAXED);
    return slot;
}

static BOOL mywin_handle_freestack_push_locked(MyWinHandlePidTable* table, DWORD slot)
{
    return mywin_handle_freestack_push_global_locked(table, slot, TRUE);
}

static DWORD mywin_handle_freestack_pop_locked(MyWinHandlePidTable* table)
{
    if (!table) return 0;
    while (table->free_count) {
        DWORD slot = table->free_stack[--table->free_count];
        mywin_handle_free_mark_clear_locked(table, slot);
        MyWinHandleEntry* e = mywin_handle_entry_at_locked(table, slot, FALSE);
        if (e && !e->valid) {
            table->free_reuse_count++;
            return slot;
        }
        __atomic_add_fetch(&g_HandleFreeStalePops, 1u, __ATOMIC_RELAXED);
    }
    return 0;
}

static void __attribute__((unused)) mywin_free_empty_handle_table_locked(MyWinHandlePidTable* table)
{
    if (!table || table->count != 0) return;
    MyWinHandlePidTable* prev = NULL;
    for (MyWinHandlePidTable* t = g_HandleTables; t; prev = t, t = t->next) {
        if (t != table) continue;
        if (prev) prev->next = t->next; else g_HandleTables = t->next;
        DWORD bucket = mywin_pid_bucket(t->pid);
        MyWinHandlePidTable* hprev = NULL;
        for (MyWinHandlePidTable* ht = g_HandleTablePidHash[bucket]; ht; hprev = ht, ht = ht->hash_next) {
            if (ht != t) continue;
            if (hprev) hprev->hash_next = ht->hash_next;
            else g_HandleTablePidHash[bucket] = ht->hash_next;
            break;
        }
        for (DWORD i = 0; i < MYWIN_HANDLE_INDEX_COUNT; ++i) {
            MyWinHandleMidPage* mid = t->mid[i];
            if (!mid) continue;
            for (DWORD j = 0; j < MYWIN_HANDLE_INDEX_COUNT; ++j) free(mid->leaf[j]);
            free(mid);
        }
        free(t->free_stack);
        mywin_pushlock_destroy(&t->lock);
        if (g_HandleTableLookupCache == t) {
            g_HandleTableLookupCache = NULL;
            g_HandleTableLookupCachePid = 0;
        }
        if (g_HandleFreeHintTable == t) {
            g_HandleFreeHintTable = NULL;
            g_HandleFreeHintSlot = 0;
        }
        mywin_handle_free_batch_drop_tls_table(t);
        for (DWORD ci = 0; ci < MYWIN_HANDLE_TLS_CACHE_SLOTS; ++ci) {
            if (g_HandleLookupCache[ci].table == t) memset(&g_HandleLookupCache[ci], 0, sizeof(g_HandleLookupCache[ci]));
        }
        free(t);
        return;
    }
}

typedef BOOL (*MYWIN_HANDLE_WALK_PROC)(MyWinHandleEntry* e, void* ctx);

static BOOL mywin_walk_handle_tables_locked(DWORD pidFilter, MYWIN_HANDLE_WALK_PROC proc, void* ctx)
{
    if (!proc) return TRUE;
    for (MyWinHandlePidTable* t = g_HandleTables; t; t = t->next) {
        if (pidFilter && t->pid != pidFilter) continue;
        for (DWORD a = 0; a < MYWIN_HANDLE_INDEX_COUNT; ++a) {
            MyWinHandleMidPage* mid = t->mid[a];
            if (!mid) continue;
            for (DWORD b = 0; b < MYWIN_HANDLE_INDEX_COUNT; ++b) {
                MyWinHandleLeafPage* leaf = mid->leaf[b];
                if (!leaf) continue;
                for (DWORD c = 0; c < MYWIN_HANDLE_INDEX_COUNT; ++c) {
                    MyWinHandleEntry* e = &leaf->entry[c];
                    if (!e->valid) continue;
                    if (!proc(e, ctx)) return FALSE;
                }
            }
        }
    }
    return TRUE;
}

typedef struct MyWinHandleSnapCtx {
    MyWinHandleEntry* entries;
    DWORD count;
    DWORD capacity;
    BOOL failed;
} MyWinHandleSnapCtx;

static BOOL mywin_handle_snapshot_cb(MyWinHandleEntry* e, void* ctxv)
{
    MyWinHandleSnapCtx* ctx = (MyWinHandleSnapCtx*)ctxv;
    if (!ctx || !e) return TRUE;
    if (ctx->count == ctx->capacity) {
        DWORD newCap = ctx->capacity ? (ctx->capacity * 2u) : 64u;
        MyWinHandleEntry* grown = (MyWinHandleEntry*)realloc(ctx->entries, (size_t)newCap * sizeof(MyWinHandleEntry));
        if (!grown) { ctx->failed = TRUE; return FALSE; }
        ctx->entries = grown;
        ctx->capacity = newCap;
    }
    ctx->entries[ctx->count++] = *e;
    return TRUE;
}

static MyWinHandleEntry* mywin_snapshot_handles_locked(DWORD pidFilter, DWORD* countOut)
{
    MyWinHandleSnapCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    mywin_walk_handle_tables_locked(pidFilter, mywin_handle_snapshot_cb, &ctx);
    if (ctx.failed) { free(ctx.entries); if (countOut) *countOut = 0; return NULL; }
    if (countOut) *countOut = ctx.count;
    return ctx.entries;
}

typedef struct MyWinPidSnapCtx {
    DWORD* pids;
    DWORD count;
    DWORD capacity;
    BOOL failed;
} MyWinPidSnapCtx;

static BOOL mywin_pid_snapshot_cb(MyWinHandleEntry* e, void* ctxv)
{
    MyWinPidSnapCtx* ctx = (MyWinPidSnapCtx*)ctxv;
    if (!ctx || !e || !e->pid) return TRUE;
    for (DWORD i = 0; i < ctx->count; ++i) if (ctx->pids[i] == e->pid) return TRUE;
    if (ctx->count == ctx->capacity) {
        DWORD newCap = ctx->capacity ? (ctx->capacity * 2u) : 32u;
        DWORD* grown = (DWORD*)realloc(ctx->pids, (size_t)newCap * sizeof(DWORD));
        if (!grown) { ctx->failed = TRUE; return FALSE; }
        ctx->pids = grown;
        ctx->capacity = newCap;
    }
    ctx->pids[ctx->count++] = e->pid;
    return TRUE;
}

static MyWinHandleEntry* mywin_find_handle_locked(DWORD pid, HANDLE h)
{
    DWORD slot = mywin_handle_slot(h);
    DWORD gen = mywin_handle_generation(h);
    if (!pid || !slot || !gen) return NULL;
    MyWinHandlePidTable* table = mywin_find_handle_table_locked(pid, FALSE);
    MyWinHandleEntry* e = mywin_handle_entry_at_locked(table, slot, FALSE);
    if (e && e->valid && e->pid == pid && e->generation == gen && e->handle == h) return e;
    return NULL;
}

static MyWinHandleEntry* mywin_find_handle_in_table_locked(MyWinHandlePidTable* table, DWORD pid, HANDLE h)
{
    DWORD slot = mywin_handle_slot(h);
    DWORD gen = mywin_handle_generation(h);
    if (!table || !pid || !slot || !gen) return NULL;
    MyWinHandleEntry* e = mywin_handle_entry_at_locked(table, slot, FALSE);
    if (e && e->valid && e->pid == pid && e->generation == gen && e->handle == h) return e;
    return NULL;
}

static BOOL g_StrictKernelHandles = TRUE;
static HANDLE mywin_resolve_handle(HANDLE h, DWORD* type, DWORD* access);
static HANDLE mywin_resolve_handle_public(HANDLE h, DWORD* type, DWORD* access);
static HANDLE mywin_resolve_handle_public_ex(HANDLE h, DWORD* type, DWORD* objectSlot, DWORD* access);
static BOOL mywin_public_access_allowed(HANDLE h, DWORD have, DWORD needed);
static BOOL mywin_has_handle_access(HANDLE h, DWORD needed, DWORD* haveOut);
static BOOL mywin_resolve_current_pseudo_handle(HANDLE h, HANDLE* objectHandle, DWORD* type, DWORD* access);
static void mywin_canonical_name(LPCSTR in, char* out, size_t cb, DWORD* nsOut);
static DWORD mywin_default_sd_for_name(const char* canonical);
static unsigned long long mywin_now_ms(void);
static void mywin_mutex_publish_state(MyWinMutexObj* m);
static void mywin_semaphore_publish_state(MyWinSemaphoreObj* sem);
static void mywin_timer_update_state_locked(MyWinTimerObj* t);
static void mywin_timer_publish_state(MyWinTimerObj* t);
static void mywin_dispatcher_wake_all(void);
static void mywin_abandon_mutexes_for_thread(DWORD ownerThread);
static BOOL mywin_waitables_have_process_or_thread(DWORD nCount, const HANDLE* lpHandles);
static BOOL mywin_next_timer_due_ms(unsigned long long* lpDueMs);
static MyWinSectionObj* mywin_find_section(HANDLE h);
static MyWinEventObj* mywin_find_event(HANDLE h);
static MyWinMutexObj* mywin_find_mutex(HANDLE h);
static MyWinSemaphoreObj* mywin_find_semaphore(HANDLE h);
static MyWinTimerObj* mywin_find_timer(HANDLE h);
static MyWinTokenObj* mywin_find_token(HANDLE h);
static void mywin_section_hash_insert_locked(int idx);
static void mywin_section_hash_remove_locked(int idx);
static MyWinSectionObj* mywin_find_section_by_canon_locked(LPCSTR canon);
static void mywin_event_hash_remove_locked(int idx);
static void mywin_mutex_hash_remove_locked(int idx);
static void mywin_semaphore_hash_remove_locked(int idx);
static void mywin_timer_hash_insert_locked(int idx);
static void mywin_timer_hash_remove_locked(int idx);
static MyWinTimerObj* mywin_find_timer_by_canon_locked(LPCSTR canon);
static void mywin_timer_due_cache_invalidate(void);
static void mywin_timer_due_cache_note_locked(const MyWinTimerObj* t);
static void mywin_release_token_object(HANDLE h);

typedef struct MyWinFindAnyHandleCtx {
    HANDLE h;
    MyWinHandleEntry* found;
} MyWinFindAnyHandleCtx;

static BOOL mywin_find_any_handle_cb(MyWinHandleEntry* e, void* ctxv)
{
    MyWinFindAnyHandleCtx* ctx = (MyWinFindAnyHandleCtx*)ctxv;
    if (e && e->valid && e->handle == ctx->h) { ctx->found = e; return FALSE; }
    return TRUE;
}

static MyWinHandleEntry* mywin_find_handle_any_locked(HANDLE h)
{
    MyWinFindAnyHandleCtx ctx;
    ctx.h = h;
    ctx.found = NULL;
    mywin_walk_handle_tables_locked(0, mywin_find_any_handle_cb, &ctx);
    return ctx.found;
}

static void mywin_copy_startup_info_to_process(MyWinProcessLite* p, LPCSTR lpCommandLine, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo)
{
    if (!p) return;
    p->startupFlags = 0;
    p->startupX = p->startupY = p->startupW = p->startupH = 0;
    p->showWindow = SW_SHOWNORMAL;
    p->stdInput = 0;
    p->stdOutput = 0;
    p->stdError = 0;
    p->commandLine[0] = 0;
    p->currentDirectory[0] = 0;
    p->windowTitle[0] = 0;

    if (lpCommandLine && lpCommandLine[0])
        snprintf(p->commandLine, sizeof(p->commandLine), "%s", lpCommandLine);
    if (lpCurrentDirectory && lpCurrentDirectory[0])
        snprintf(p->currentDirectory, sizeof(p->currentDirectory), "%s", lpCurrentDirectory);

    if (lpStartupInfo) {
        p->startupFlags = lpStartupInfo->dwFlags;
        p->startupX = lpStartupInfo->dwX;
        p->startupY = lpStartupInfo->dwY;
        p->startupW = lpStartupInfo->dwXSize;
        p->startupH = lpStartupInfo->dwYSize;
        if ((lpStartupInfo->dwFlags & STARTF_USESHOWWINDOW) != 0)
            p->showWindow = lpStartupInfo->wShowWindow;
        if ((lpStartupInfo->dwFlags & STARTF_USESTDHANDLES) != 0) {
            p->stdInput = lpStartupInfo->hStdInput;
            p->stdOutput = lpStartupInfo->hStdOutput;
            p->stdError = lpStartupInfo->hStdError;
        }
        if (lpStartupInfo->lpTitle && lpStartupInfo->lpTitle[0])
            snprintf(p->windowTitle, sizeof(p->windowTitle), "%s", lpStartupInfo->lpTitle);
    }
}

static void mywin_note_lite_process_ex(DWORD pid, DWORD parentPid, DWORD tid,
                                       HANDLE processObject, HANDLE threadObject,
                                       const char* imageName, const Capability* childCapability,
                                       LPCSTR lpCommandLine, LPCSTR lpCurrentDirectory,
                                       LPSTARTUPINFOA lpStartupInfo, LPVOID lpEnvironment)
{

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_alloc_lite_process_locked(pid);
    if (p) {
        p->pid = pid;
        p->parentPid = parentPid;
        p->tid = tid;
        mywin_process_index_refresh_locked(p);
        p->processObject = processObject;
        p->threadObject = threadObject;
        p->flags = MYWIN_PROCESS_LIVE;
        p->exitCode = STILL_ACTIVE;
        mywin_process_dispatcher_refresh_locked(p);
        snprintf(p->imageName, sizeof(p->imageName), "%s", imageName && imageName[0] ? imageName : "process-lite");
        mywin_module_init_locked(p, p->imageName);

        // v46: AppHost can provide a real child-token template instead of
        // blindly inheriting the loader/parent capability.  This is the first
        // loader-like boundary: PID/TID are allocated by Process-Lite, while
        // flags/paths/targets come from the image manifest.
        const Capability* srcCap = childCapability ? childCapability : (g_HasCapability ? &g_CurrentCapability : NULL);
        if (srcCap) {
            p->cap = *srcCap;
            p->cap.id = pid;
            snprintf(p->cap.name, sizeof(p->cap.name), "%.31s", p->imageName);
            p->hasCap = 1;
        }
        mywin_copy_startup_info_to_process(p, lpCommandLine, lpCurrentDirectory, lpStartupInfo);
        mywin_env_init_for_child_locked(p, parentPid, lpEnvironment);
        snprintf(p->subsystem, sizeof(p->subsystem), "process");
        p->argc = 0;
        p->argvPreview[0] = 0;
        p->consoleExitCode = STILL_ACTIVE;
        p->linuxPid = 0;
        p->linuxStatus = 0;
        p->forkExec = 0;
        p->objectLifetimeReleased = 0;
    }
    pthread_mutex_unlock(&g_ProcessLock);
}

static DWORD mywin_close_all_handles_for_pid(DWORD pid);

BOOL MyWinAttachLinuxProcess(DWORD dwProcessId, int nLinuxPid)
{
    if (!dwProcessId || nLinuxPid <= 0) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }

    char imageName[64] = "process";
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(dwProcessId);
    if (!p) { pthread_mutex_unlock(&g_ProcessLock); mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    snprintf(imageName, sizeof(imageName), "%s", p->imageName[0] ? p->imageName : "process");
    p->linuxPid = nLinuxPid;
    p->linuxStatus = 0;
    p->forkExec = 1;
    p->flags = MYWIN_PROCESS_LIVE;
    p->exitCode = STILL_ACTIVE;
    p->objectLifetimeReleased = 0;
    p->consoleExitCode = STILL_ACTIVE;
    mywin_process_dispatcher_refresh_locked(p);
    pthread_mutex_unlock(&g_ProcessLock);

    MyProcessHostInfo phi;
    if (!MyProcessHostGetInfo(dwProcessId, &phi)) {
        (void)MyProcessHostTrack(dwProcessId, nLinuxPid, imageName);
    }
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

static void mywin_mark_process_exited(DWORD pid, DWORD exitCode, DWORD rawStatus)
{
    HANDLE procObj = 0;
    HANDLE threadObj = 0;
    char procName[96] = "process-lite";
    char threadName[96] = "process-lite!main";
    DWORD releaseLifetimeRefs = 0;

    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(pid);
    if (p) {
        p->flags = MYWIN_PROCESS_EXITED;
        p->exitCode = exitCode;
        mywin_process_dispatcher_refresh_locked(p);
        if (p->subsystem[0] && strcmp(p->subsystem, "console") == 0) p->consoleExitCode = exitCode;
        p->linuxStatus = rawStatus;
        memset(p->loadedModules, 0, sizeof(p->loadedModules));
        p->moduleSerial++;
        procObj = p->processObject;
        threadObj = p->threadObject;
        if (!p->objectLifetimeReleased) {
            p->objectLifetimeReleased = 1;
            releaseLifetimeRefs = 1;
        }
        snprintf(procName, sizeof(procName), "%s", p->imageName);
        snprintf(threadName, sizeof(threadName), "%s!main", p->imageName);
    }
    pthread_mutex_unlock(&g_ProcessLock);

    /* v187: if a process/thread exits while owning a mutex, the mutex becomes
       abandoned and waiters must be woken before the dying process' own handle
       table is swept.  Remaining duplicated/inherited handles then see
       WAIT_ABANDONED instead of a stale owned mutex. */
    mywin_abandon_mutexes_for_thread(pid);

    /* v181: an Object Manager PROCESS/THREAD entry is not an app-start
       success counter.  The live process owns one kernel lifetime ref, and
       every public HANDLE owns one handle ref.  On exit release the lifetime
       ref exactly once; remaining external handles keep the object alive until
       CloseHandle(), matching Win32 object lifetime semantics. */
    mywin_close_all_handles_for_pid(pid);
    MyUser32CleanupProcessClasses(pid);
    _ObjectSetInfo(procObj, _OBJECT_FLAG_PROCESS_EXITED, exitCode, procName);
    _ObjectSetInfo(threadObj, _OBJECT_FLAG_THREAD_EXITED, exitCode, threadName);
    mywin_dispatcher_wake_object(procObj, _OBJECT_TYPE_PROCESS);
    mywin_dispatcher_wake_object(threadObj, _OBJECT_TYPE_THREAD);
    if (releaseLifetimeRefs) {
        _ObjectRelease(procObj);
        _ObjectRelease(threadObj);
    }
}

void MyWinNotifyProcessHostExit(DWORD dwProcessId, DWORD dwExitCode, DWORD dwRawStatus)
{
    if (!dwProcessId) return;
    mywin_mark_process_exited(dwProcessId, dwExitCode, dwRawStatus);
}

BOOL MyWinPollProcess(DWORD dwProcessId)
{
    int osPid = 0;
    DWORD alreadyExited = 0;
    BOOL found = FALSE;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(dwProcessId);
    if (p) { found = TRUE; osPid = p->linuxPid; alreadyExited = (p->flags & MYWIN_PROCESS_EXITED) ? 1u : 0u; }
    pthread_mutex_unlock(&g_ProcessLock);
    if (!found || alreadyExited || osPid <= 0) return alreadyExited ? TRUE : FALSE;

    BOOL exited = FALSE;
    DWORD exitCode = STILL_ACTIVE;
    DWORD rawStatus = 0;
    if (MyProcessHostPoll(dwProcessId, &exited, &exitCode, &rawStatus) && exited) {
        mywin_mark_process_exited(dwProcessId, exitCode, rawStatus);
        return TRUE;
    }
    return FALSE;
}

DWORD MyWinPollAllProcesses(void)
{
    DWORD pids[MYWIN_MAX_LITE_PROCESSES];
    DWORD n = 0;
    pthread_mutex_lock(&g_ProcessLock);
    for (int i = 0; i < MYWIN_MAX_LITE_PROCESSES && n < MYWIN_MAX_LITE_PROCESSES; i++) {
        if (g_LiteProcesses[i].valid && g_LiteProcesses[i].forkExec && !(g_LiteProcesses[i].flags & MYWIN_PROCESS_EXITED))
            pids[n++] = g_LiteProcesses[i].pid;
    }
    pthread_mutex_unlock(&g_ProcessLock);
    DWORD exited = 0;
    for (DWORD i = 0; i < n; i++) if (MyWinPollProcess(pids[i])) exited++;
    return exited;
}


static void mywin_process_note_inherited(DWORD pid, DWORD n)
{
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(pid);
    if (p) p->inheritedHandles += n;
    pthread_mutex_unlock(&g_ProcessLock);
}

static void mywin_process_note_duplicated_in(DWORD pid)
{
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(pid);
    if (p) p->duplicatedIn++;
    pthread_mutex_unlock(&g_ProcessLock);
}

static DWORD mywin_process_handle_count(DWORD pid)
{
    DWORD n = 0;
    pthread_mutex_lock(&g_HandleLock);
    MyWinHandlePidTable* table = mywin_find_handle_table_locked(pid, FALSE);
    if (table) n = table->count;
    pthread_mutex_unlock(&g_HandleLock);
    return n;
}

static BOOL mywin_process_is_exited(DWORD pid, DWORD* exitCode)
{
    MyWinPollProcess(pid);
    BOOL exited = FALSE;
    if (exitCode) *exitCode = STILL_ACTIVE;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(pid);
    if (p) {
        exited = (p->flags & MYWIN_PROCESS_EXITED) ? TRUE : FALSE;
        if (exitCode) *exitCode = p->exitCode;
    }
    pthread_mutex_unlock(&g_ProcessLock);
    return exited;
}

static void mywin_abandon_mutexes_for_thread(DWORD ownerThread)
{
    if (!ownerThread) return;
    BOOL changed = FALSE;
    mywin_dispatcher_ensure();
    pthread_mutex_lock(&g_DispatcherLock);
    for (int i = 0; i < MYWIN_MAX_MUTEXES; i++) {
        MyWinMutexObj* m = &g_Mutexes[i];
        if (!m->valid) continue;
        pthread_mutex_lock(&m->lock);
        if (m->owned && m->owner_thread == ownerThread) {
            m->owned = FALSE;
            m->owner_thread = 0;
            m->abandoned = TRUE;
            mywin_mutex_publish_state(m);
            changed = TRUE;
            pthread_cond_broadcast(&m->cond);
            mywin_dispatcher_signal_object_locked(m->handle, _OBJECT_TYPE_MUTEX);
        }
        pthread_mutex_unlock(&m->lock);
    }
    if (!changed) g_WaitWakeSkips++;
    pthread_mutex_unlock(&g_DispatcherLock);
}

static inline DWORD mywin_atomic_ref_inc(DWORD* ref)
{
    if (!ref) return 0;
    return __atomic_add_fetch(ref, 1u, __ATOMIC_RELAXED);
}

static inline DWORD mywin_atomic_ref_dec(DWORD* ref)
{
    if (!ref) return 0;
    DWORD old = __atomic_load_n(ref, __ATOMIC_RELAXED);
    while (old) {
        DWORD next = old - 1u;
        if (__atomic_compare_exchange_n(ref, &old, next, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return next;
    }
    return 0;
}

static void mywin_add_object_ref_by_slot(HANDLE objectHandle, DWORD type, DWORD objectSlot)
{
    if (!objectHandle) return;
    if (objectSlot != 0xffffffffu) {
        BOOL bumped = FALSE;
        if (type == _OBJECT_TYPE_SECTION && objectSlot < MYWIN_MAX_SECTIONS && g_Sections[objectSlot].valid && g_Sections[objectSlot].handle == objectHandle) { mywin_atomic_ref_inc(&g_Sections[objectSlot].refCount); bumped = TRUE; }
        else if (type == _OBJECT_TYPE_EVENT && objectSlot < MYWIN_MAX_EVENTS && g_Events[objectSlot].valid && g_Events[objectSlot].handle == objectHandle) { mywin_atomic_ref_inc(&g_Events[objectSlot].refCount); bumped = TRUE; }
        else if (type == _OBJECT_TYPE_MUTEX && objectSlot < MYWIN_MAX_MUTEXES && g_Mutexes[objectSlot].valid && g_Mutexes[objectSlot].handle == objectHandle) { mywin_atomic_ref_inc(&g_Mutexes[objectSlot].refCount); bumped = TRUE; }
        else if (type == _OBJECT_TYPE_SEMAPHORE && objectSlot < MYWIN_MAX_SEMAPHORES && g_Semaphores[objectSlot].valid && g_Semaphores[objectSlot].handle == objectHandle) { mywin_atomic_ref_inc(&g_Semaphores[objectSlot].refCount); bumped = TRUE; }
        else if (type == _OBJECT_TYPE_TIMER && objectSlot < MYWIN_MAX_TIMERS && g_Timers[objectSlot].valid && g_Timers[objectSlot].handle == objectHandle) { mywin_atomic_ref_inc(&g_Timers[objectSlot].refCount); bumped = TRUE; }
        else if (type == _OBJECT_TYPE_TOKEN && objectSlot < MYWIN_MAX_TOKENS && g_Tokens[objectSlot].valid && g_Tokens[objectSlot].handle == objectHandle) { mywin_atomic_ref_inc(&g_Tokens[objectSlot].refCount); bumped = TRUE; }
        if (bumped) { _ObjectAddRef(objectHandle); return; }
    }
    if (type == _OBJECT_TYPE_SECTION) { MyWinSectionObj* o = mywin_find_section(objectHandle); if (o) mywin_atomic_ref_inc(&o->refCount); }
    else if (type == _OBJECT_TYPE_EVENT) { MyWinEventObj* o = mywin_find_event(objectHandle); if (o) mywin_atomic_ref_inc(&o->refCount); }
    else if (type == _OBJECT_TYPE_MUTEX) { MyWinMutexObj* o = mywin_find_mutex(objectHandle); if (o) mywin_atomic_ref_inc(&o->refCount); }
    else if (type == _OBJECT_TYPE_SEMAPHORE) { MyWinSemaphoreObj* o = mywin_find_semaphore(objectHandle); if (o) mywin_atomic_ref_inc(&o->refCount); }
    else if (type == _OBJECT_TYPE_TIMER) { MyWinTimerObj* o = mywin_find_timer(objectHandle); if (o) mywin_atomic_ref_inc(&o->refCount); }
    else if (type == _OBJECT_TYPE_TOKEN) { MyWinTokenObj* o = mywin_find_token(objectHandle); if (o) mywin_atomic_ref_inc(&o->refCount); }
    _ObjectAddRef(objectHandle);
}

static void mywin_add_object_ref_by_type(HANDLE objectHandle, DWORD type)
{
    mywin_add_object_ref_by_slot(objectHandle, type, mywin_cached_object_slot(objectHandle, type));
}

static void mywin_release_object_ref_by_type(HANDLE objectHandle, DWORD type)
{
    if (!objectHandle) return;
    if (type == _OBJECT_TYPE_SECTION) {
        MyWinSectionObj* o = mywin_find_section(objectHandle);
        if (o) {
            int idx = (int)(o - g_Sections);
            DWORD left = mywin_atomic_ref_dec(&o->refCount);
            _ObjectRelease(objectHandle);
            if (!left) {
                mywin_section_hash_remove_locked(idx);
                mywin_named_directory_remove(o->name, _OBJECT_TYPE_SECTION, objectHandle);
                _ObjectUnregister(objectHandle);
                mywin_destroy_section_storage(o);
                memset(o, 0, sizeof(*o));
                mywin_push_section_slot(idx);
            }
            return;
        }
    } else if (type == _OBJECT_TYPE_EVENT) {
        pthread_mutex_lock(&g_EventTableLock);
        MyWinEventObj* o = mywin_find_event(objectHandle);
        if (o) {
            int idx = (int)(o - g_Events);
            DWORD left = mywin_atomic_ref_dec(&o->refCount);
            _ObjectRelease(objectHandle);
            if (!left) {
                mywin_dispatcher_ensure();
                pthread_mutex_lock(&g_DispatcherLock);
                mywin_waitblocks_detach_object_locked(objectHandle, _OBJECT_TYPE_EVENT);
                pthread_mutex_unlock(&g_DispatcherLock);
                mywin_event_hash_remove_locked(idx);
                mywin_named_directory_remove(o->name, _OBJECT_TYPE_EVENT, objectHandle);
                _ObjectUnregister(objectHandle);
                pthread_cond_destroy(&o->cond);
                pthread_mutex_destroy(&o->lock);
                memset(o, 0, sizeof(*o));
                mywin_push_event_slot(idx);
            }
            pthread_mutex_unlock(&g_EventTableLock);
            return;
        }
        pthread_mutex_unlock(&g_EventTableLock);
    } else if (type == _OBJECT_TYPE_MUTEX) {
        pthread_mutex_lock(&g_MutexTableLock);
        MyWinMutexObj* o = mywin_find_mutex(objectHandle);
        if (o) {
            int idx = (int)(o - g_Mutexes);
            DWORD left = mywin_atomic_ref_dec(&o->refCount);
            _ObjectRelease(objectHandle);
            if (!left) {
                mywin_dispatcher_ensure();
                pthread_mutex_lock(&g_DispatcherLock);
                mywin_waitblocks_detach_object_locked(objectHandle, _OBJECT_TYPE_MUTEX);
                pthread_mutex_unlock(&g_DispatcherLock);
                mywin_mutex_hash_remove_locked(idx);
                mywin_named_directory_remove(o->name, _OBJECT_TYPE_MUTEX, objectHandle);
                _ObjectUnregister(objectHandle);
                pthread_cond_destroy(&o->cond);
                pthread_mutex_destroy(&o->lock);
                memset(o, 0, sizeof(*o));
                mywin_push_mutex_slot(idx);
            }
            pthread_mutex_unlock(&g_MutexTableLock);
            return;
        }
        pthread_mutex_unlock(&g_MutexTableLock);
    } else if (type == _OBJECT_TYPE_SEMAPHORE) {
        pthread_mutex_lock(&g_SemaphoreTableLock);
        MyWinSemaphoreObj* o = mywin_find_semaphore(objectHandle);
        if (o) {
            int idx = (int)(o - g_Semaphores);
            DWORD left = mywin_atomic_ref_dec(&o->refCount);
            _ObjectRelease(objectHandle);
            if (!left) {
                mywin_dispatcher_ensure();
                pthread_mutex_lock(&g_DispatcherLock);
                mywin_waitblocks_detach_object_locked(objectHandle, _OBJECT_TYPE_SEMAPHORE);
                pthread_mutex_unlock(&g_DispatcherLock);
                mywin_semaphore_hash_remove_locked(idx);
                mywin_named_directory_remove(o->name, _OBJECT_TYPE_SEMAPHORE, objectHandle);
                _ObjectUnregister(objectHandle);
                pthread_cond_destroy(&o->cond);
                pthread_mutex_destroy(&o->lock);
                memset(o, 0, sizeof(*o));
                mywin_push_semaphore_slot(idx);
            }
            pthread_mutex_unlock(&g_SemaphoreTableLock);
            return;
        }
        pthread_mutex_unlock(&g_SemaphoreTableLock);
    } else if (type == _OBJECT_TYPE_TIMER) {
        MyWinTimerObj* o = mywin_find_timer(objectHandle);
        if (o) {
            int idx = (int)(o - g_Timers);
            DWORD left = mywin_atomic_ref_dec(&o->refCount);
            _ObjectRelease(objectHandle);
            if (!left) {
                mywin_dispatcher_ensure();
                pthread_mutex_lock(&g_DispatcherLock);
                mywin_waitblocks_detach_object_locked(objectHandle, _OBJECT_TYPE_TIMER);
                pthread_mutex_unlock(&g_DispatcherLock);
                mywin_timer_hash_remove_locked(idx);
                mywin_named_directory_remove(o->name, _OBJECT_TYPE_TIMER, objectHandle);
                mywin_timer_due_cache_invalidate();
                _ObjectUnregister(objectHandle);
                pthread_cond_destroy(&o->cond);
                pthread_mutex_destroy(&o->lock);
                memset(o, 0, sizeof(*o));
                mywin_push_timer_slot(idx);
            }
            return;
        }
    } else if (type == _OBJECT_TYPE_TOKEN) {
        mywin_release_token_object(objectHandle);
        return;
    }
    _ObjectRelease(objectHandle);
}


BOOL MyWinSetStrictKernelHandles(BOOL bEnable)
{
    BOOL old = g_StrictKernelHandles;
    g_StrictKernelHandles = bEnable ? TRUE : FALSE;
    return old;
}

BOOL MyWinGetStrictKernelHandles(void)
{
    return g_StrictKernelHandles ? TRUE : FALSE;
}

static DWORD mywin_process_id_from_handle(HANDLE hProcess)
{
    if (!hProcess || hProcess == GetCurrentProcess()) return mywin_current_pid();

    DWORD type = 0;
    HANDLE obj = mywin_resolve_handle_public(hProcess, &type, NULL);
    if (type == _OBJECT_TYPE_PROCESS) {
        _ObjectectInfo oi;
        if (_ObjectGetInfo(obj, &oi) && oi.type == _OBJECT_TYPE_PROCESS) return oi.owner_pid;
    }

    /* v147/v216: legacy raw 0x50000000 process-object escapes stay available
       only when strict handles are explicitly disabled for diagnostics/legacy
       bring-up. New process objects are slot-coded Object Manager handles. */
    if (!g_StrictKernelHandles && (((uintptr_t)hProcess & 0xff000000u) == 0x50000000u))
        return (DWORD)((uintptr_t)hProcess & 0x00ffffffu);
    return 0;
}

static HANDLE mywin_alloc_process_handle(DWORD pid, HANDLE objectHandle, DWORD type, DWORD access, BOOL inherit)
{
    if (!pid || !objectHandle) return 0;
    MyWinHandlePidTable* table = mywin_get_handle_table_ref(pid, TRUE);
    if (!table) return 0;
    mywin_handle_free_batch_flush_tls_if_other(table);
    mywin_pushlock_acquire_exclusive(&table->lock);
    if (table->quota && table->count >= table->quota) {
        table->quota_failures++;
        mywin_pushlock_release(&table->lock);
        return 0;
    }

    DWORD slot = mywin_handle_free_batch_pop_locked(table);
    if (!slot) slot = mywin_handle_tls_free_hint_pop_locked(table);
    if (!slot) slot = mywin_handle_freestack_pop_locked(table);
    if (!slot) {
        slot = table->alloc_hint ? table->alloc_hint : 1u;
        if (slot > MYWIN_HANDLE_MAX_SLOTS) {
            mywin_pushlock_release(&table->lock);
            return 0;
        }
        table->alloc_hint = (slot == MYWIN_HANDLE_MAX_SLOTS) ? (MYWIN_HANDLE_MAX_SLOTS + 1u) : (slot + 1u);
    }

    MyWinHandleEntry* e = mywin_handle_entry_at_locked(table, slot, TRUE);
    if (!e || e->valid) {
        /* Should not happen in the normal path: free_stack + high-water slot
           gives O(1) allocation. Keep a slow repair path for corrupted/legacy
           tables without making it the common allocator. */
        e = NULL;
        for (DWORD probe = 1; probe <= MYWIN_HANDLE_MAX_SLOTS; ++probe) {
            MyWinHandleEntry* cand = mywin_handle_entry_at_locked(table, probe, FALSE);
            if (cand && !cand->valid) { slot = probe; e = cand; break; }
        }
        if (!e) { mywin_pushlock_release(&table->lock); return 0; }
    }

    DWORD generation = (e->generation + 1u) & MYWIN_HANDLE_GENERATION_MASK;
    if (!generation) generation = 1;
    memset(e, 0, sizeof(*e));
    e->valid = 1;
    e->pid = pid;
    e->slot = slot;
    e->generation = generation;
    e->objectHandle = objectHandle;
    e->type = type;
    e->object_slot = mywin_cached_object_slot(objectHandle, type);
    e->access = access;
    e->inherit = inherit ? TRUE : FALSE;
    e->protect_from_close = FALSE;
    e->handle = mywin_make_user_handle(slot, generation);
    table->count++;
    if (table->count > table->peak_count) table->peak_count = table->count;
    HANDLE out = e->handle;
    mywin_pushlock_release(&table->lock);
    _ObjectReferenceHandle(objectHandle);
    /* v241: allocating a new handle does not mutate any already-resolved
       handle entry, so keep the per-thread lookup cache warm.  Close/reuse
       paths still invalidate the table epoch before a stale generation can be
       observed. */
    return out;
}


static HANDLE mywin_duplicate_handle_to_pid(DWORD srcPid, DWORD dstPid, HANDLE hSourceHandle, BOOL inherit, DWORD* errorOut)
{
    if (errorOut) *errorOut = ERROR_SUCCESS;
    if (!srcPid || !dstPid || !hSourceHandle) { if (errorOut) *errorOut = ERROR_INVALID_HANDLE; return 0; }
    DWORD srcType = _OBJECT_TYPE_NONE;
    DWORD srcAccess = 0;
    DWORD srcObjectSlot = 0xffffffffu;
    HANDLE hObject = 0;
    MyWinHandlePidTable* srcTable = mywin_get_handle_table_ref(srcPid, FALSE);
    if (srcTable) {
        mywin_pushlock_acquire_shared(&srcTable->lock);
        MyWinHandleEntry* src = mywin_find_handle_in_table_locked(srcTable, srcPid, hSourceHandle);
        if (src) { hObject = src->objectHandle; srcType = src->type; srcObjectSlot = src->object_slot; srcAccess = src->access; }
        mywin_pushlock_release(&srcTable->lock);
    }
    if (!hObject || srcType == _OBJECT_TYPE_NONE) { if (errorOut) *errorOut = ERROR_INVALID_HANDLE; return 0; }
    HANDLE nh = mywin_alloc_process_handle(dstPid, hObject, srcType, srcAccess, inherit);
    if (!nh) { if (errorOut) *errorOut = ERROR_NOT_ENOUGH_MEMORY; return 0; }
    mywin_add_object_ref_by_slot(hObject, srcType, srcObjectSlot);
    if (dstPid != mywin_current_pid()) mywin_process_note_duplicated_in(dstPid);
    return nh;
}


static BOOL mywin_close_process_handle_ex(DWORD pid, HANDLE h, BOOL honorProtect, HANDLE* objectHandle, DWORD* type, DWORD* errorOut)
{
    if (objectHandle) *objectHandle = 0;
    if (type) *type = _OBJECT_TYPE_NONE;
    if (errorOut) *errorOut = ERROR_INVALID_HANDLE;
    MyWinHandlePidTable* table = mywin_get_handle_table_ref(pid, FALSE);
    if (!table) return FALSE;
    mywin_handle_free_batch_flush_tls_if_other(table);
    mywin_pushlock_acquire_exclusive(&table->lock);
    MyWinHandleEntry* e = mywin_find_handle_in_table_locked(table, pid, h);
    if (!e) { mywin_pushlock_release(&table->lock); return FALSE; }
    if (honorProtect && e->protect_from_close) {
        if (errorOut) *errorOut = ERROR_ACCESS_DENIED;
        mywin_pushlock_release(&table->lock);
        return FALSE;
    }
    DWORD slot = e->slot;
    DWORD generation = e->generation;
    HANDLE closedObjectHandle = e->objectHandle;
    if (objectHandle) *objectHandle = e->objectHandle;
    if (type) *type = e->type;
    memset(e, 0, sizeof(*e));
    e->slot = slot;
    e->generation = generation;
    if (table->count) table->count--;
    if (!mywin_handle_free_batch_store_locked(table, slot) && !mywin_handle_freestack_push_locked(table, slot)) {
        if (table->alloc_hint == 0 || slot < table->alloc_hint) table->alloc_hint = slot;
    }
    mywin_pushlock_release(&table->lock);
    _ObjectDereferenceHandle(closedObjectHandle);
    mywin_handle_cache_invalidate_table(table);
    if (errorOut) *errorOut = ERROR_SUCCESS;
    return TRUE;
}



static HANDLE mywin_resolve_handle(HANDLE h, DWORD* type, DWORD* access)
{
    if (type) *type = _OBJECT_TYPE_NONE;
    if (access) *access = 0;
    if (!h) return 0;
    DWORD pid = mywin_current_pid();
    HANDLE cached = 0;
    if (mywin_handle_cache_lookup(pid, h, type, access, &cached)) return cached;
    MyWinHandlePidTable* table = mywin_get_handle_table_ref(pid, FALSE);
    if (table) {
        mywin_pushlock_acquire_shared(&table->lock);
        MyWinHandleEntry* e = mywin_find_handle_in_table_locked(table, pid, h);
        if (e) {
            HANDLE obj = e->objectHandle;
            DWORD etype = e->type;
            DWORD eslot = e->object_slot;
            DWORD eaccess = e->access;
            if (type) *type = etype;
            if (access) *access = eaccess;
            mywin_pushlock_release(&table->lock);
            mywin_handle_cache_store_ex(table, pid, h, obj, etype, eslot, eaccess);
            return obj;
        }
        mywin_pushlock_release(&table->lock);
    }
    if (g_HasCapability && ((g_CurrentCapability.flags & CAP_ADMIN) == CAP_ADMIN)) {
        pthread_mutex_lock(&g_HandleLock);
        MyWinHandleEntry* e = mywin_find_handle_any_locked(h);
        if (e) {
            HANDLE obj = e->objectHandle;
            if (type) *type = e->type;
            if (access) *access = e->access;
            pthread_mutex_unlock(&g_HandleLock);
            return obj;
        }
        pthread_mutex_unlock(&g_HandleLock);
    }
    return h;
}


static HANDLE mywin_resolve_handle_public(HANDLE h, DWORD* type, DWORD* access)
{
    return mywin_resolve_handle_public_ex(h, type, NULL, access);
}

static HANDLE mywin_resolve_handle_public_ex(HANDLE h, DWORD* type, DWORD* objectSlot, DWORD* access)
{
    if (type) *type = _OBJECT_TYPE_NONE;
    if (objectSlot) *objectSlot = 0xffffffffu;
    if (access) *access = 0;
    if (!h) return 0;
    if (!g_StrictKernelHandles) {
        HANDLE obj = mywin_resolve_handle(h, type, access);
        if (objectSlot) *objectSlot = mywin_cached_object_slot(obj, type ? *type : _OBJECT_TYPE_NONE);
        return obj;
    }
    DWORD pid = mywin_current_pid();
    HANDLE cached = 0;
    if (mywin_handle_cache_lookup_ex(pid, h, type, objectSlot, access, &cached)) return cached;
    MyWinHandlePidTable* table = mywin_get_handle_table_ref(pid, FALSE);
    if (!table) return 0;
    mywin_pushlock_acquire_shared(&table->lock);
    MyWinHandleEntry* e = mywin_find_handle_in_table_locked(table, pid, h);
    if (e) {
        HANDLE obj = e->objectHandle;
        DWORD etype = e->type;
        DWORD eslot = e->object_slot;
        DWORD eaccess = e->access;
        if (type) *type = etype;
        if (objectSlot) *objectSlot = eslot;
        if (access) *access = eaccess;
        mywin_pushlock_release(&table->lock);
        mywin_handle_cache_store_ex(table, pid, h, obj, etype, eslot, eaccess);
        return obj;
    }
    mywin_pushlock_release(&table->lock);
    return 0;
}

static BOOL mywin_public_access_allowed(HANDLE h, DWORD have, DWORD needed)
{
    if (needed == 0) return TRUE;
    if (!g_StrictKernelHandles && mywin_is_legacy_raw_handle(h, have)) return TRUE;
    return mywin_access_mask_allows(have, needed);
}

static BOOL mywin_has_handle_access(HANDLE h, DWORD needed, DWORD* haveOut)
{
    DWORD have = 0;
    DWORD type = 0;
    HANDLE obj = mywin_resolve_handle_public(h, &type, &have);
    if (haveOut) *haveOut = have;
    if (!obj) return FALSE;
    return mywin_public_access_allowed(h, have, needed);
}


static BOOL mywin_obj_sid_equal_local(const _ObjectSid* a, const _ObjectSid* b)
{
    if (!a || !b) return FALSE;
    if (a->revision != b->revision || a->subauth_count != b->subauth_count) return FALSE;
    if (memcmp(a->authority, b->authority, sizeof(a->authority)) != 0) return FALSE;
    for (DWORD i = 0; i < a->subauth_count && i < _OBJECT_SECURITY_MAX_SUBAUTH; ++i)
        if (a->subauth[i] != b->subauth[i]) return FALSE;
    return TRUE;
}

static BOOL mywin_token_has_sid_local(const _ObjectToken* token, const _ObjectSid* sid)
{
    if (!token || !sid) return FALSE;
    if (mywin_obj_sid_equal_local(&token->user, sid)) return TRUE;
    for (DWORD i = 0; i < token->group_count && i < 8; ++i)
        if (mywin_obj_sid_equal_local(&token->groups[i], sid)) return TRUE;
    return FALSE;
}

static BOOL mywin_obj_security_access_check_local(const _ObjectSecurity* sec, const _ObjectToken* token, DWORD wanted, DWORD ownerImplicitAccess, DWORD* grantedOut)
{
    if (grantedOut) *grantedOut = 0;
    if (!sec || !sec->valid || !token) return FALSE;
    if (wanted == 0) return TRUE;
    if (token->is_admin) { if (grantedOut) *grantedOut = wanted; return TRUE; }
    DWORD granted = 0;
    if (ownerImplicitAccess && mywin_obj_sid_equal_local(&token->user, &sec->owner))
        granted |= (wanted & ownerImplicitAccess);
    if (!sec->dacl_present || sec->dacl_null) { if (grantedOut) *grantedOut = wanted; return TRUE; }
    for (DWORD i = 0; i < sec->ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i) {
        const _ObjectAce* ace = &sec->aces[i];
        if (ace->flags & _OBJECT_ACE_FLAG_INHERIT_ONLY) continue;
        if (ace->type != _OBJECT_ACE_DENY) continue;
        if (!mywin_token_has_sid_local(token, &ace->sid)) continue;
        DWORD stillWanted = wanted & ~granted;
        if ((ace->mask & stillWanted) != 0) { if (grantedOut) *grantedOut = granted; return FALSE; }
    }
    for (DWORD i = 0; i < sec->ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i) {
        const _ObjectAce* ace = &sec->aces[i];
        if (ace->flags & _OBJECT_ACE_FLAG_INHERIT_ONLY) continue;
        if (ace->type != _OBJECT_ACE_ALLOW) continue;
        if (!mywin_token_has_sid_local(token, &ace->sid)) continue;
        granted |= (ace->mask & wanted);
    }
    if (grantedOut) *grantedOut = granted;
    return ((granted & wanted) == wanted) ? TRUE : FALSE;
}

static DWORD mywin_obj_security_maximum_allowed_local(const _ObjectSecurity* sec, const _ObjectToken* token, DWORD universe, DWORD ownerImplicitAccess)
{
    if (!sec || !sec->valid || !token) return 0;
    if (universe == 0) universe = 0xffffffffu;
    if (token->is_admin) return universe;

    DWORD denied = 0;
    DWORD granted = 0;
    if (ownerImplicitAccess && mywin_obj_sid_equal_local(&token->user, &sec->owner))
        granted |= (ownerImplicitAccess & universe);
    if (!sec->dacl_present || sec->dacl_null) return universe;

    for (DWORD i = 0; i < sec->ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i) {
        const _ObjectAce* ace = &sec->aces[i];
        if (ace->flags & _OBJECT_ACE_FLAG_INHERIT_ONLY) continue;
        if (ace->type != _OBJECT_ACE_DENY) continue;
        if (!mywin_token_has_sid_local(token, &ace->sid)) continue;
        denied |= (ace->mask & universe);
    }
    for (DWORD i = 0; i < sec->ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i) {
        const _ObjectAce* ace = &sec->aces[i];
        if (ace->flags & _OBJECT_ACE_FLAG_INHERIT_ONLY) continue;
        if (ace->type != _OBJECT_ACE_ALLOW) continue;
        if (!mywin_token_has_sid_local(token, &ace->sid)) continue;
        granted |= (ace->mask & universe & ~denied);
    }
    return granted & universe;
}

static BOOL mywin_token_has_luid_privilege(const _ObjectToken* token, LUID luid)
{
    if (!token || !token->is_admin) return FALSE;
    LUID sec, take, backup, restore;
    mywin_make_luid_local(MYWIN_LUID_SE_SECURITY, &sec);
    mywin_make_luid_local(MYWIN_LUID_SE_TAKE_OWNERSHIP, &take);
    mywin_make_luid_local(MYWIN_LUID_SE_BACKUP, &backup);
    mywin_make_luid_local(MYWIN_LUID_SE_RESTORE, &restore);
    return mywin_luid_equal_local(luid, sec) || mywin_luid_equal_local(luid, take) ||
           mywin_luid_equal_local(luid, backup) || mywin_luid_equal_local(luid, restore);
}

static DWORD mywin_fill_token_privileges(const _ObjectToken* token, LUID_AND_ATTRIBUTES* out, DWORD maxCount)
{
    if (!token || !token->is_admin) return 0;
    DWORD n = 0;
#define MYWIN_ADD_PRIV(low_) do { \
        if (out && n < maxCount) { mywin_make_luid_local((low_), &out[n].Luid); out[n].Attributes = SE_PRIVILEGE_ENABLED_BY_DEFAULT | SE_PRIVILEGE_ENABLED; } \
        n++; \
    } while (0)
    MYWIN_ADD_PRIV(MYWIN_LUID_SE_SECURITY);
    MYWIN_ADD_PRIV(MYWIN_LUID_SE_TAKE_OWNERSHIP);
    MYWIN_ADD_PRIV(MYWIN_LUID_SE_BACKUP);
    MYWIN_ADD_PRIV(MYWIN_LUID_SE_RESTORE);
#undef MYWIN_ADD_PRIV
    return n;
}

static void mywin_token_for_pid(DWORD pid, _ObjectToken* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    mywin_process_sid_for_pid(pid ? pid : mywin_current_pid(), &out->user);
    mywin_everyone_sid(&out->groups[out->group_count++]);

    BOOL admin = FALSE;
    if (g_HasCapability && g_CurrentCapability.id == pid && ((g_CurrentCapability.flags & CAP_ADMIN) == CAP_ADMIN)) admin = TRUE;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(pid);
    if (p && p->hasCap && ((p->cap.flags & CAP_ADMIN) == CAP_ADMIN)) admin = TRUE;
    pthread_mutex_unlock(&g_ProcessLock);
    if (admin && out->group_count < 8) {
        mywin_admins_sid(&out->groups[out->group_count++]);
        out->is_admin = TRUE;
    }
}

static MyWinTokenObj* mywin_find_token(HANDLE h)
{
    DWORD type = 0, slot = 0;
    if (_ObjectDecodeSlotHandle(h, &type, &slot) && type == _OBJECT_TYPE_TOKEN) {
        if (slot < MYWIN_MAX_TOKENS && g_Tokens[slot].valid && g_Tokens[slot].handle == h) return &g_Tokens[slot];
        return NULL;
    }
    for (int i = 0; i < MYWIN_MAX_TOKENS; i++)
        if (g_Tokens[i].valid && g_Tokens[i].handle == h) return &g_Tokens[i];
    return NULL;
}

static void mywin_release_token_object(HANDLE h)
{
    MyWinTokenObj* tok = mywin_find_token(h);
    if (!tok) { _ObjectRelease(h); return; }
    if (tok->refCount) tok->refCount--;
    _ObjectRelease(h);
    if (tok->refCount == 0) {
        int idx = (int)(tok - g_Tokens);
        _ObjectUnregister(h);
        memset(tok, 0, sizeof(*tok));
        mywin_push_token_slot(idx);
    }
}

static BOOL mywin_object_get_or_synth_security(HANDLE objectHandle, _ObjectSecurity* out)
{
    if (!out) return FALSE;
    if (_ObjectGetSecurityDescriptor(objectHandle, out)) return TRUE;
    _ObjectectInfo oi;
    if (!_ObjectGetInfo(objectHandle, &oi)) return FALSE;
    memset(out, 0, sizeof(*out));
    out->valid = 1;
    out->control = SE_DACL_PRESENT;
    out->namespace_id = oi.namespace_id;
    out->dacl_present = TRUE;
    out->dacl_null = FALSE;
    mywin_process_sid_for_pid(oi.owner_pid ? oi.owner_pid : mywin_current_pid(), &out->owner);
    DWORD all = mywin_object_generic_all(oi.type);
    out->aces[out->ace_count].type = _OBJECT_ACE_ALLOW;
    out->aces[out->ace_count].flags = 0;
    out->aces[out->ace_count].mask = all;
    out->aces[out->ace_count].sid = out->owner;
    out->ace_count++;
    if (oi.sd_flags & (_OBJECT_SD_PUBLIC_READ|_OBJECT_SD_PUBLIC_WRITE)) {
        out->aces[out->ace_count].type = _OBJECT_ACE_ALLOW;
        out->aces[out->ace_count].flags = 0;
        out->aces[out->ace_count].mask = 0;
        if (oi.sd_flags & _OBJECT_SD_PUBLIC_READ) out->aces[out->ace_count].mask |= mywin_object_generic_read(oi.type);
        if (oi.sd_flags & _OBJECT_SD_PUBLIC_WRITE) out->aces[out->ace_count].mask |= mywin_object_generic_write(oi.type);
        mywin_everyone_sid(&out->aces[out->ace_count].sid);
        out->ace_count++;
    }
    return TRUE;
}

static BOOL mywin_build_absolute_sd_from_objsec(const _ObjectSecurity* sec, SECURITY_DESCRIPTOR* sd, SID* ownerSid, BYTE* aclBuf, DWORD aclBufSize)
{
    if (!sec || !sd || !ownerSid || !aclBuf) return FALSE;
    memset(sd, 0, sizeof(*sd));
    if (!mywin_obj_sid_to_public(&sec->owner, ownerSid)) return FALSE;
    sd->Revision = SECURITY_DESCRIPTOR_REVISION;
    sd->Owner = ownerSid;
    if (!sec->dacl_present) return TRUE;
    sd->Control |= SE_DACL_PRESENT;
    if (sec->dacl_null) { sd->Dacl = NULL; return TRUE; }
    PACL acl = (PACL)aclBuf;
    if (!InitializeAcl(acl, aclBufSize, ACL_REVISION)) return FALSE;
    for (DWORD i = 0; i < sec->ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i) {
        SID tmpSid;
        if (!mywin_obj_sid_to_public(&sec->aces[i].sid, &tmpSid)) return FALSE;
        BOOL ok = mywin_add_access_ace(acl, ACL_REVISION, sec->aces[i].mask, &tmpSid,
                                       sec->aces[i].type == _OBJECT_ACE_DENY ? ACCESS_DENIED_ACE_TYPE : ACCESS_ALLOWED_ACE_TYPE,
                                       (BYTE)(sec->aces[i].flags & 0xffu));
        if (!ok) return FALSE;
    }
    sd->Dacl = acl;
    return TRUE;
}

BOOL OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, HANDLE* TokenHandle)
{
    if (!TokenHandle) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *TokenHandle = 0;
    DWORD pid = mywin_process_id_from_handle(ProcessHandle);
    if (!pid) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (ProcessHandle && ProcessHandle != GetCurrentProcess()) {
        DWORD procHave = 0;
        if (!mywin_has_handle_access(ProcessHandle, 0, &procHave) ||
            ((procHave & (PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION)) == 0)) {
            mywin_set_last_error(ERROR_ACCESS_DENIED);
            return FALSE;
        }
    }

    DWORD access = DesiredAccess ? DesiredAccess : TOKEN_QUERY;
    int i = mywin_pop_token_slot();
    if (i < 0) { mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }

    memset(&g_Tokens[i], 0, sizeof(g_Tokens[i]));
    HANDLE obj = _ObjectMakeSlotHandle(_OBJECT_TYPE_TOKEN, (DWORD)i);
    g_Tokens[i].valid = 1;
    g_Tokens[i].handle = obj;
    g_Tokens[i].owner_pid = mywin_current_pid();
    g_Tokens[i].source_pid = pid;
    g_Tokens[i].access = access;
    g_Tokens[i].refCount = 1;
    mywin_token_for_pid(pid, &g_Tokens[i].token);
    snprintf(g_Tokens[i].name, sizeof(g_Tokens[i].name), "Token(pid=%lu)", (unsigned long)pid);
    _ObjectRegister(obj, _OBJECT_TYPE_TOKEN, mywin_current_pid(), _OBJECT_ACCESS_READ|_OBJECT_ACCESS_CONTROL, 0, g_Tokens[i].name);
    _ObjectSetSecurity(obj, _OBJECT_SD_OWNER_ONLY, _OBJECT_NS_NONE);
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), obj, _OBJECT_TYPE_TOKEN, access, FALSE);
    if (!h) { mywin_release_token_object(obj); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    *TokenHandle = h;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}


BOOL GetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, LPDWORD ReturnLength)
{
    DWORD have = 0, type = 0;
    HANDLE obj = mywin_resolve_handle_public(TokenHandle, &type, &have);
    if (!obj || type != _OBJECT_TYPE_TOKEN) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(TokenHandle, have, TOKEN_QUERY)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    MyWinTokenObj* tok = mywin_find_token(obj);
    if (!tok) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }

    if (TokenInformationClass == TokenUser) {
        SID userSid;
        if (!mywin_obj_sid_to_public(&tok->token.user, &userSid)) { mywin_set_last_error(ERROR_INVALID_SID); return FALSE; }
        DWORD sidLen = GetLengthSid(&userSid);
        DWORD need = (DWORD)sizeof(TOKEN_USER) + sidLen;
        if (ReturnLength) *ReturnLength = need;
        if (!TokenInformation || TokenInformationLength < need) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        PTOKEN_USER tu = (PTOKEN_USER)TokenInformation;
        memset(tu, 0, TokenInformationLength);
        BYTE* p = ((BYTE*)TokenInformation) + sizeof(TOKEN_USER);
        memcpy(p, &userSid, sidLen);
        tu->User.Sid = (PSID)p;
        tu->User.Attributes = 0;
        mywin_set_last_error(ERROR_SUCCESS);
        return TRUE;
    }

    if (TokenInformationClass == TokenGroups) {
        DWORD n = tok->token.group_count;
        if (n > 8) n = 8;
        DWORD base = (DWORD)(sizeof(DWORD) + sizeof(SID_AND_ATTRIBUTES) * (n ? n : 1));
        DWORD sidBytes = 0;
        SID tmp[8];
        for (DWORD i = 0; i < n; ++i) { mywin_obj_sid_to_public(&tok->token.groups[i], &tmp[i]); sidBytes += GetLengthSid(&tmp[i]); }
        DWORD need = base + sidBytes;
        if (ReturnLength) *ReturnLength = need;
        if (!TokenInformation || TokenInformationLength < need) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        PTOKEN_GROUPS tg = (PTOKEN_GROUPS)TokenInformation;
        memset(tg, 0, TokenInformationLength);
        tg->GroupCount = n;
        BYTE* cursor = ((BYTE*)TokenInformation) + base;
        for (DWORD i = 0; i < n; ++i) {
            DWORD cb = GetLengthSid(&tmp[i]);
            memcpy(cursor, &tmp[i], cb);
            tg->Groups[i].Sid = (PSID)cursor;
            tg->Groups[i].Attributes = SE_GROUP_ENABLED;
            cursor += cb;
        }
        mywin_set_last_error(ERROR_SUCCESS);
        return TRUE;
    }

    if (TokenInformationClass == TokenPrivileges) {
        DWORD count = mywin_fill_token_privileges(&tok->token, NULL, 0);
        DWORD need = (DWORD)offsetof(TOKEN_PRIVILEGES, Privileges) + sizeof(LUID_AND_ATTRIBUTES) * (count ? count : 1);
        if (ReturnLength) *ReturnLength = need;
        if (!TokenInformation || TokenInformationLength < need) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        PTOKEN_PRIVILEGES tp = (PTOKEN_PRIVILEGES)TokenInformation;
        memset(tp, 0, TokenInformationLength);
        tp->PrivilegeCount = count;
        if (count) mywin_fill_token_privileges(&tok->token, tp->Privileges, count);
        mywin_set_last_error(ERROR_SUCCESS);
        return TRUE;
    }

    if (TokenInformationClass == TokenOwner || TokenInformationClass == TokenPrimaryGroup) {
        SID userSid;
        if (!mywin_obj_sid_to_public(&tok->token.user, &userSid)) { mywin_set_last_error(ERROR_INVALID_SID); return FALSE; }
        DWORD sidLen = GetLengthSid(&userSid);
        DWORD base = (TokenInformationClass == TokenOwner) ? (DWORD)sizeof(TOKEN_OWNER) : (DWORD)sizeof(TOKEN_PRIMARY_GROUP);
        DWORD need = base + sidLen;
        if (ReturnLength) *ReturnLength = need;
        if (!TokenInformation || TokenInformationLength < need) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        memset(TokenInformation, 0, TokenInformationLength);
        BYTE* p = ((BYTE*)TokenInformation) + base;
        memcpy(p, &userSid, sidLen);
        if (TokenInformationClass == TokenOwner) ((PTOKEN_OWNER)TokenInformation)->Owner = (PSID)p;
        else ((PTOKEN_PRIMARY_GROUP)TokenInformation)->PrimaryGroup = (PSID)p;
        mywin_set_last_error(ERROR_SUCCESS);
        return TRUE;
    }

    if (TokenInformationClass == TokenDefaultDacl) {
        SID userSid;
        if (!mywin_obj_sid_to_public(&tok->token.user, &userSid)) { mywin_set_last_error(ERROR_INVALID_SID); return FALSE; }
        DWORD sidLen = GetLengthSid(&userSid);
        DWORD aclNeed = (DWORD)(sizeof(ACL) + offsetof(ACCESS_ALLOWED_ACE, SidStart) + sidLen);
        DWORD need = (DWORD)sizeof(TOKEN_DEFAULT_DACL) + aclNeed;
        if (ReturnLength) *ReturnLength = need;
        if (!TokenInformation || TokenInformationLength < need) { mywin_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        memset(TokenInformation, 0, TokenInformationLength);
        PTOKEN_DEFAULT_DACL td = (PTOKEN_DEFAULT_DACL)TokenInformation;
        PACL acl = (PACL)(((BYTE*)TokenInformation) + sizeof(TOKEN_DEFAULT_DACL));
        if (!InitializeAcl(acl, aclNeed, ACL_REVISION) || !AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, &userSid)) return FALSE;
        td->DefaultDacl = acl;
        mywin_set_last_error(ERROR_SUCCESS);
        return TRUE;
    }

    mywin_set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL CheckTokenMembership(HANDLE TokenHandle, PSID SidToCheck, LPBOOL IsMember)
{
    if (!SidToCheck || !IsMember) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *IsMember = FALSE;
    _ObjectSid sid;
    if (!mywin_public_sid_to_obj(SidToCheck, &sid)) { mywin_set_last_error(ERROR_INVALID_SID); return FALSE; }

    _ObjectToken current;
    _ObjectToken* token = &current;
    MyWinTokenObj* tok = NULL;
    if (!TokenHandle) {
        mywin_current_token(&current);
    } else {
        DWORD have = 0, type = 0;
        HANDLE obj = mywin_resolve_handle_public(TokenHandle, &type, &have);
        if (!obj || type != _OBJECT_TYPE_TOKEN) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
        if (!mywin_public_access_allowed(TokenHandle, have, TOKEN_QUERY)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
        tok = mywin_find_token(obj);
        if (!tok) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
        token = &tok->token;
    }
    *IsMember = mywin_token_has_sid_local(token, &sid) ? TRUE : FALSE;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL PrivilegeCheck(HANDLE ClientToken, PPRIVILEGE_SET RequiredPrivileges, LPBOOL pfResult)
{
    if (!RequiredPrivileges || !pfResult) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *pfResult = FALSE;
    DWORD have = 0, type = 0;
    HANDLE obj = mywin_resolve_handle_public(ClientToken, &type, &have);
    if (!obj || type != _OBJECT_TYPE_TOKEN) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(ClientToken, have, TOKEN_QUERY)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    MyWinTokenObj* tok = mywin_find_token(obj);
    if (!tok) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }

    DWORD n = RequiredPrivileges->PrivilegeCount;
    BOOL allNecessary = (RequiredPrivileges->Control & PRIVILEGE_SET_ALL_NECESSARY) ? TRUE : FALSE;
    BOOL any = FALSE;
    BOOL all = TRUE;
    for (DWORD i = 0; i < n; ++i) {
        BOOL has = mywin_token_has_luid_privilege(&tok->token, RequiredPrivileges->Privilege[i].Luid);
        if (has) {
            any = TRUE;
            RequiredPrivileges->Privilege[i].Attributes |= SE_PRIVILEGE_USED_FOR_ACCESS;
        } else {
            all = FALSE;
        }
    }
    *pfResult = (n == 0) ? TRUE : (allNecessary ? all : any);
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL AccessCheck(PSECURITY_DESCRIPTOR pSecurityDescriptor, HANDLE ClientToken, DWORD DesiredAccess, PGENERIC_MAPPING GenericMapping, PPRIVILEGE_SET PrivilegeSet, LPDWORD PrivilegeSetLength, LPDWORD GrantedAccess, LPBOOL AccessStatus)
{
    if (!AccessStatus || !GrantedAccess) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *AccessStatus = FALSE;
    *GrantedAccess = 0;
    DWORD have = 0, type = 0;
    HANDLE obj = mywin_resolve_handle_public(ClientToken, &type, &have);
    if (!obj || type != _OBJECT_TYPE_TOKEN) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(ClientToken, have, TOKEN_QUERY)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    MyWinTokenObj* tok = mywin_find_token(obj);
    if (!tok) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    _ObjectSecurity sec;
    if (!mywin_security_from_descriptor(pSecurityDescriptor, &sec, 0)) return FALSE;

    DWORD desired = DesiredAccess;
    DWORD genericAllUniverse = 0xffffffffu & ~(MAXIMUM_ALLOWED | ACCESS_SYSTEM_SECURITY);
    if (GenericMapping) {
        genericAllUniverse = GenericMapping->GenericAll ? GenericMapping->GenericAll : genericAllUniverse;
        mywin_map_mask_with_mapping(&desired, GenericMapping);
        for (DWORD i = 0; i < sec.ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i)
            mywin_map_mask_with_mapping(&sec.aces[i].mask, GenericMapping);
    }

    DWORD usedPrivileges = 0;
    DWORD granted = 0;
    BOOL privilegeDenied = FALSE;
    if (desired & ACCESS_SYSTEM_SECURITY) {
        LUID secPriv;
        mywin_make_luid_local(MYWIN_LUID_SE_SECURITY, &secPriv);
        if (mywin_token_has_luid_privilege(&tok->token, secPriv)) {
            granted |= ACCESS_SYSTEM_SECURITY;
            usedPrivileges = 1;
        } else {
            privilegeDenied = TRUE;
        }
        desired &= ~ACCESS_SYSTEM_SECURITY;
    }

    DWORD daclDesired = desired & ~MAXIMUM_ALLOWED;
    DWORD daclGranted = 0;
    BOOL daclOk = TRUE;
    if (daclDesired)
        daclOk = mywin_obj_security_access_check_local(&sec, &tok->token, daclDesired, READ_CONTROL | WRITE_DAC, &daclGranted);
    granted |= daclGranted;

    if (desired & MAXIMUM_ALLOWED) {
        DWORD maxAllowed = mywin_obj_security_maximum_allowed_local(&sec, &tok->token, genericAllUniverse, READ_CONTROL | WRITE_DAC);
        granted |= maxAllowed;
        daclOk = daclOk && (maxAllowed != 0 || daclDesired == 0);
    }

    *GrantedAccess = granted;
    *AccessStatus = (!privilegeDenied && daclOk) ? TRUE : FALSE;

    if (PrivilegeSetLength) {
        DWORD need = (DWORD)offsetof(PRIVILEGE_SET, Privilege) + sizeof(LUID_AND_ATTRIBUTES) * (usedPrivileges ? usedPrivileges : 1);
        if (PrivilegeSet && *PrivilegeSetLength >= need) {
            memset(PrivilegeSet, 0, *PrivilegeSetLength);
            PrivilegeSet->PrivilegeCount = usedPrivileges;
            if (usedPrivileges) {
                mywin_make_luid_local(MYWIN_LUID_SE_SECURITY, &PrivilegeSet->Privilege[0].Luid);
                PrivilegeSet->Privilege[0].Attributes = SE_PRIVILEGE_USED_FOR_ACCESS;
            }
        }
        *PrivilegeSetLength = need;
    }
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL GetKernelObjectSecurity(HANDLE Handle, SECURITY_INFORMATION RequestedInformation, PSECURITY_DESCRIPTOR pSecurityDescriptor, DWORD nLength, LPDWORD lpnLengthNeeded)
{
    (void)RequestedInformation;
    if (!lpnLengthNeeded) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD have = 0, type = 0;
    HANDLE obj = mywin_resolve_handle_public(Handle, &type, &have);
    if (!obj) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(Handle, have, READ_CONTROL)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    _ObjectSecurity sec;
    if (!mywin_object_get_or_synth_security(obj, &sec)) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    SECURITY_DESCRIPTOR absSd;
    SID ownerSid;
    BYTE aclBuf[1024];
    if (!mywin_build_absolute_sd_from_objsec(&sec, &absSd, &ownerSid, aclBuf, sizeof(aclBuf))) { mywin_set_last_error(ERROR_INVALID_SECURITY_DESCR); return FALSE; }
    DWORD need = nLength;
    if (!MakeSelfRelativeSD(&absSd, pSecurityDescriptor, &need)) {
        *lpnLengthNeeded = need;
        return FALSE;
    }
    *lpnLengthNeeded = need;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL SetKernelObjectSecurity(HANDLE Handle, SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor)
{
    if (!SecurityDescriptor) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD have = 0, type = 0;
    HANDLE obj = mywin_resolve_handle_public(Handle, &type, &have);
    if (!obj) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if ((SecurityInformation & DACL_SECURITY_INFORMATION) && !mywin_public_access_allowed(Handle, have, WRITE_DAC)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    if ((SecurityInformation & OWNER_SECURITY_INFORMATION) && !mywin_public_access_allowed(Handle, have, WRITE_OWNER)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    _ObjectSecurity cur;
    if (!mywin_object_get_or_synth_security(obj, &cur)) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    _ObjectSecurity incoming;
    if (!mywin_security_from_descriptor(SecurityDescriptor, &incoming, type)) return FALSE;
    if (SecurityInformation & OWNER_SECURITY_INFORMATION) cur.owner = incoming.owner;
    if (SecurityInformation & DACL_SECURITY_INFORMATION) {
        cur.dacl_present = incoming.dacl_present;
        cur.dacl_null = incoming.dacl_null;
        cur.ace_count = incoming.ace_count;
        cur.control = (SECURITY_DESCRIPTOR_CONTROL)((cur.control & ~(SE_DACL_PRESENT|SE_DACL_DEFAULTED)) | (incoming.control & (SE_DACL_PRESENT|SE_DACL_DEFAULTED)));
        for (DWORD i = 0; i < incoming.ace_count && i < _OBJECT_SECURITY_MAX_ACES; ++i) cur.aces[i] = incoming.aces[i];
    }
    cur.valid = 1;
    if (!_ObjectSetSecurityDescriptor(obj, &cur)) { mywin_set_last_error(ERROR_INVALID_SECURITY_DESCR); return FALSE; }
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

static void mywin_fill_handle_info(const MyWinHandleEntry* e, MyHandleInfo* out)
{
    memset(out, 0, sizeof(*out));
    if (!e) return;
    out->pid = e->pid;
    out->handle = e->handle;
    out->object_handle = e->objectHandle;
    out->object_type = e->type;
    out->granted_access = e->access;
    out->flags = 0;
    if (e->inherit) out->flags |= MYWIN_HANDLE_FLAG_INHERIT;
    if (e->protect_from_close) out->flags |= MYWIN_HANDLE_FLAG_PROTECT_FROM_CLOSE;
    out->slot = e->slot;
    out->object_slot = e->object_slot;
    _ObjectectInfo oi;
    if (_ObjectGetInfo(e->objectHandle, &oi)) {
        out->object_ref = oi.pointer_count ? oi.pointer_count : oi.ref_count;
        out->object_generation = oi.object_generation;
        out->object_state = oi.object_state;
        out->object_handle_count = oi.handle_count;
        snprintf(out->object_name, sizeof(out->object_name), "%s", oi.name);
    }
}

BOOL MyEnumProcessHandles(DWORD dwProcessId, MYHANDLEENUMPROC lpEnumFunc, LPARAM lParam)
{
    if (!lpEnumFunc) return FALSE;
    DWORD n = 0;
    MyWinHandleEntry* snap = NULL;
    pthread_mutex_lock(&g_HandleLock);
    snap = mywin_snapshot_handles_locked(dwProcessId, &n);
    pthread_mutex_unlock(&g_HandleLock);
    if (!snap && n == 0) return TRUE;
    for (DWORD i=0;i<n;i++) {
        MyHandleInfo info;
        mywin_fill_handle_info(&snap[i], &info);
        if (!lpEnumFunc(&info, lParam)) break;
    }
    free(snap);
    return TRUE;
}

DWORD MyGetHandleCount(DWORD dwProcessId)
{
    DWORD n = 0;
    pthread_mutex_lock(&g_HandleLock);
    if (dwProcessId) {
        MyWinHandlePidTable* table = mywin_find_handle_table_locked(dwProcessId, FALSE);
        n = table ? table->count : 0;
    } else {
        for (MyWinHandlePidTable* t = g_HandleTables; t; t = t->next) n += t->count;
    }
    pthread_mutex_unlock(&g_HandleLock);
    return n;
}

static BOOL mywin_lite_process_flags_snapshot(DWORD pid, DWORD* flagsOut)
{
    BOOL found = FALSE;
    if (flagsOut) *flagsOut = 0;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(pid);
    if (p) {
        found = TRUE;
        if (flagsOut) *flagsOut = p->flags;
    }
    pthread_mutex_unlock(&g_ProcessLock);
    return found;
}

static BOOL mywin_lite_process_is_live_snapshot(DWORD pid, BOOL* existedOut)
{
    DWORD flags = 0;
    BOOL exists = mywin_lite_process_flags_snapshot(pid, &flags);
    if (existedOut) *existedOut = exists;
    if (!exists) return FALSE;
    return (flags & MYWIN_PROCESS_EXITED) ? FALSE : TRUE;
}

static void mywin_duplicate_note_failure(DWORD err)
{
    g_HandleDuplicateFailures++;
    if (err == ERROR_ACCESS_DENIED) g_HandleDuplicateAccessDenied++;
    else if (err == ERROR_INVALID_PARAMETER) g_HandleDuplicateInvalidProcess++;
    mywin_set_last_error(err);
}

BOOL MyWinGetProcessIndexAudit(MyProcessIndexAudit* lpAudit)
{
    if (!lpAudit) return FALSE;
    memset(lpAudit, 0, sizeof(*lpAudit));
    pthread_mutex_lock(&g_ProcessLock);
    lpAudit->pid_hash_hits = g_ProcessIndexPidHits;
    lpAudit->pid_hash_misses = g_ProcessIndexPidMisses;
    lpAudit->tid_hash_hits = g_ProcessIndexTidHits;
    lpAudit->tid_hash_misses = g_ProcessIndexTidMisses;
    lpAudit->pid_hash_inserts = g_ProcessIndexPidInserts;
    lpAudit->tid_hash_inserts = g_ProcessIndexTidInserts;
    lpAudit->alloc_fast = g_ProcessIndexAllocFast;
    lpAudit->alloc_fallback = g_ProcessIndexAllocFallback;
    lpAudit->fallback_scans = g_ProcessIndexFallbackScans;
    for (int i = 0; i < MYWIN_MAX_LITE_PROCESSES; ++i) {
        if (!g_LiteProcesses[i].valid) continue;
        if (g_LiteProcesses[i].flags & MYWIN_PROCESS_EXITED) lpAudit->exited_records++;
        else lpAudit->live_records++;
    }
    pthread_mutex_unlock(&g_ProcessLock);
    return TRUE;
}

BOOL MyWinGetHandleTableAudit(MyHandleTableAudit* lpAudit)
{
    if (!lpAudit) return FALSE;
    MyWinPollAllProcesses();
    memset(lpAudit, 0, sizeof(*lpAudit));

    MyWinHandleEntry* snap = NULL;
    DWORD n = 0;
    DWORD calls = 0, closed = 0, failures = 0, lastPid = 0;
    DWORD dupSuccess = 0, dupCross = 0, dupCloseSource = 0, dupFailures = 0, dupDenied = 0, dupBadProcess = 0;
    DWORD cacheHits = __atomic_load_n(&g_HandleCacheHits, __ATOMIC_RELAXED);
    DWORD cacheMisses = __atomic_load_n(&g_HandleCacheMisses, __ATOMIC_RELAXED);
    DWORD cacheStores = __atomic_load_n(&g_HandleCacheStores, __ATOMIC_RELAXED);
    DWORD cacheInvalidations = __atomic_load_n(&g_HandleCacheInvalidations, __ATOMIC_RELAXED);
    DWORD cacheEntryValidated = __atomic_load_n(&g_HandleCacheEntryValidated, __ATOMIC_RELAXED);
    DWORD cacheEntryStale = __atomic_load_n(&g_HandleCacheEntryStale, __ATOMIC_RELAXED);
    DWORD cacheSlotProbes = cacheHits + cacheMisses;
    DWORD cacheSlotCollisions = __atomic_load_n(&g_HandleCacheSlotCollisions, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_HandleLock);
    calls = g_HandleSweepCalls;
    closed = g_HandleSweepClosed;
    failures = g_HandleSweepFailures;
    lastPid = g_HandleSweepLastPid;
    dupSuccess = g_HandleDuplicateSuccess;
    dupCross = g_HandleDuplicateCrossProcess;
    dupCloseSource = g_HandleDuplicateCloseSource;
    dupFailures = g_HandleDuplicateFailures;
    dupDenied = g_HandleDuplicateAccessDenied;
    dupBadProcess = g_HandleDuplicateInvalidProcess;
    snap = mywin_snapshot_handles_locked(0, &n);
    pthread_mutex_unlock(&g_HandleLock);
    if (!snap && n != 0) return FALSE;

    DWORD ownerCapacity = n ? n : 1u;
    DWORD* ownerPids = (DWORD*)calloc(ownerCapacity, sizeof(DWORD));
    DWORD* ownerCounts = (DWORD*)calloc(ownerCapacity, sizeof(DWORD));
    DWORD* ownerExitedFlags = (DWORD*)calloc(ownerCapacity, sizeof(DWORD));
    if (!ownerPids || !ownerCounts || !ownerExitedFlags) {
        free(snap); free(ownerPids); free(ownerCounts); free(ownerExitedFlags);
        return FALSE;
    }
    DWORD ownerN = 0;

    lpAudit->total_handles = n;
    lpAudit->sweep_calls = calls;
    lpAudit->swept_handles = closed;
    lpAudit->sweep_failures = failures;
    lpAudit->last_sweep_pid = lastPid;
    lpAudit->duplicate_success = dupSuccess;
    lpAudit->duplicate_cross_process = dupCross;
    lpAudit->duplicate_close_source = dupCloseSource;
    lpAudit->duplicate_failures = dupFailures;
    lpAudit->duplicate_access_denied = dupDenied;
    lpAudit->duplicate_invalid_process = dupBadProcess;
    lpAudit->handle_cache_hits = cacheHits;
    lpAudit->handle_cache_misses = cacheMisses;
    lpAudit->handle_cache_stores = cacheStores;
    lpAudit->handle_cache_invalidations = cacheInvalidations;
    lpAudit->handle_cache_entry_validated = cacheEntryValidated;
    lpAudit->handle_cache_entry_stale = cacheEntryStale;
    lpAudit->handle_cache_slot_probes = cacheSlotProbes;
    lpAudit->handle_cache_slot_collisions = cacheSlotCollisions;
    lpAudit->handle_free_hint_hits = __atomic_load_n(&g_HandleFreeHintHits, __ATOMIC_RELAXED);
    lpAudit->handle_free_hint_misses = __atomic_load_n(&g_HandleFreeHintMisses, __ATOMIC_RELAXED);
    lpAudit->handle_free_mark_duplicate_skips = __atomic_load_n(&g_HandleFreeMarkDuplicateSkips, __ATOMIC_RELAXED);
    lpAudit->handle_free_stale_pops = __atomic_load_n(&g_HandleFreeStalePops, __ATOMIC_RELAXED);
    lpAudit->handle_free_batch_hits = __atomic_load_n(&g_HandleFreeBatchHits, __ATOMIC_RELAXED);
    lpAudit->handle_free_batch_stores = __atomic_load_n(&g_HandleFreeBatchStores, __ATOMIC_RELAXED);
    lpAudit->handle_free_batch_flushes = __atomic_load_n(&g_HandleFreeBatchFlushes, __ATOMIC_RELAXED);
    lpAudit->handle_free_batch_flushed_slots = __atomic_load_n(&g_HandleFreeBatchFlushedSlots, __ATOMIC_RELAXED);
    lpAudit->handle_free_batch_overflow = __atomic_load_n(&g_HandleFreeBatchOverflow, __ATOMIC_RELAXED);
    lpAudit->handle_free_batch_misses = __atomic_load_n(&g_HandleFreeBatchMisses, __ATOMIC_RELAXED);
    lpAudit->handle_free_batch_lane_allocs = __atomic_load_n(&g_HandleFreeBatchLaneAllocs, __ATOMIC_RELAXED);
    lpAudit->handle_free_batch_lane_matches = __atomic_load_n(&g_HandleFreeBatchLaneMatches, __ATOMIC_RELAXED);
    lpAudit->handle_free_batch_table_switch_avoided = __atomic_load_n(&g_HandleFreeBatchTableSwitchAvoided, __ATOMIC_RELAXED);
    lpAudit->pushlock_shared_fast = __atomic_load_n(&g_PushLockSharedFast, __ATOMIC_RELAXED);
    lpAudit->pushlock_shared_slow = __atomic_load_n(&g_PushLockSharedSlow, __ATOMIC_RELAXED);
    lpAudit->pushlock_exclusive_fast = __atomic_load_n(&g_PushLockExclusiveFast, __ATOMIC_RELAXED);
    lpAudit->pushlock_exclusive_slow = __atomic_load_n(&g_PushLockExclusiveSlow, __ATOMIC_RELAXED);
    lpAudit->pushlock_wakeups = __atomic_load_n(&g_PushLockWakeups, __ATOMIC_RELAXED);
    lpAudit->pushlock_contentions = __atomic_load_n(&g_PushLockContentions, __ATOMIC_RELAXED);

    for (DWORD i=0;i<n;i++) {
        DWORD flags = 0;
        BOOL haveOwnerProcess = mywin_lite_process_flags_snapshot(snap[i].pid, &flags);
        BOOL ownerExited = (haveOwnerProcess && (flags & MYWIN_PROCESS_EXITED)) ? TRUE : FALSE;

        if (snap[i].inherit) lpAudit->inherited_handles++;
        if (snap[i].type == _OBJECT_TYPE_PROCESS) lpAudit->process_handles++;
        else if (snap[i].type == _OBJECT_TYPE_THREAD) lpAudit->thread_handles++;
        else if (snap[i].type == _OBJECT_TYPE_EVENT || snap[i].type == _OBJECT_TYPE_MUTEX ||
                 snap[i].type == _OBJECT_TYPE_SEMAPHORE || snap[i].type == _OBJECT_TYPE_TIMER)
            lpAudit->waitable_handles++;

        if (!haveOwnerProcess) lpAudit->orphan_owner_handles++;
        else if (ownerExited) lpAudit->exited_owner_handles++;
        else lpAudit->live_owner_handles++;

        DWORD idx = ownerN;
        for (DWORD j=0;j<ownerN;j++) { if (ownerPids[j] == snap[i].pid) { idx = j; break; } }
        if (idx == ownerN && ownerN < ownerCapacity) {
            ownerPids[ownerN] = snap[i].pid;
            ownerCounts[ownerN] = 0;
            ownerExitedFlags[ownerN] = ownerExited ? 1u : 0u;
            ownerN++;
        }
        if (idx < ownerCapacity) {
            ownerCounts[idx]++;
            if (ownerExited) ownerExitedFlags[idx] = 1u;
            if (ownerCounts[idx] > lpAudit->max_handles_per_pid) lpAudit->max_handles_per_pid = ownerCounts[idx];
        }
    }

    lpAudit->owner_pid_count = ownerN;
    for (DWORD i=0;i<ownerN;i++) if (ownerExitedFlags[i] && ownerCounts[i] > 0) lpAudit->dead_pid_tables++;
    free(snap); free(ownerPids); free(ownerCounts); free(ownerExitedFlags);
    return TRUE;
}

BOOL MyWinGetNamedDirectoryAudit(MyNamedDirectoryAudit* lpAudit)
{
    if (!lpAudit) return FALSE;
    memset(lpAudit, 0, sizeof(*lpAudit));
    pthread_mutex_lock(&g_NamedDirectoryLock);
    mywin_named_directory_free_init_locked();
    DWORD entries = 0;
    for (int i = 0; i < MYWIN_NAMED_DIRECTORY_MAX; ++i) {
        if (g_NamedDirectory[i].valid) entries++;
    }
    lpAudit->entries = entries;
    lpAudit->free_slots = (DWORD)((g_NamedDirectoryFreeTop >= 0) ? g_NamedDirectoryFreeTop : 0);
    lpAudit->lookups = g_NamedDirectoryLookups;
    lpAudit->hits = g_NamedDirectoryHits;
    lpAudit->misses = g_NamedDirectoryMisses;
    lpAudit->cross_type_conflicts = g_NamedDirectoryCrossTypeConflicts;
    lpAudit->inserts = g_NamedDirectoryInserts;
    lpAudit->removes = g_NamedDirectoryRemoves;
    lpAudit->fast_hits = g_NamedDirectoryFastHits;
    lpAudit->fast_misses = g_NamedDirectoryFastMisses;
    lpAudit->fast_type_mismatches = g_NamedDirectoryFastTypeMismatches;
    lpAudit->stale_hits = __atomic_load_n(&g_NamedDirectoryStaleHits, __ATOMIC_RELAXED);
    lpAudit->free_reuse = g_NamedDirectoryFreeReuse;
    lpAudit->free_duplicate_skips = g_NamedDirectoryFreeDuplicateSkips;
    lpAudit->epoch = mywin_named_directory_epoch_load();
    lpAudit->tls_hits = __atomic_load_n(&g_NamedDirectoryTlsHits, __ATOMIC_RELAXED);
    lpAudit->tls_misses = __atomic_load_n(&g_NamedDirectoryTlsMisses, __ATOMIC_RELAXED);
    lpAudit->tls_epoch_misses = __atomic_load_n(&g_NamedDirectoryTlsEpochMisses, __ATOMIC_RELAXED);
    lpAudit->tls_collisions = __atomic_load_n(&g_NamedDirectoryTlsCollisions, __ATOMIC_RELAXED);
    lpAudit->tls_stores = __atomic_load_n(&g_NamedDirectoryTlsStores, __ATOMIC_RELAXED);
    lpAudit->tls_stale_invalidations = __atomic_load_n(&g_NamedDirectoryTlsStaleInvalidations, __ATOMIC_RELAXED);
    lpAudit->slot_fast_hits = __atomic_load_n(&g_NamedDirectorySlotFastHits, __ATOMIC_RELAXED);
    lpAudit->slot_fast_misses = __atomic_load_n(&g_NamedDirectorySlotFastMisses, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_NamedDirectoryLock);
    return TRUE;
}

BOOL MyWinGetWaitAudit(MyWaitAudit* lpAudit)
{
    if (!lpAudit) return FALSE;
    memset(lpAudit, 0, sizeof(*lpAudit));
    lpAudit->wait_single_calls = g_WaitSingleCalls;
    lpAudit->wait_multiple_calls = g_WaitMultipleCalls;
    lpAudit->wait_any_calls = g_WaitAnyCalls;
    lpAudit->wait_all_calls = g_WaitAllCalls;
    lpAudit->wait_success = g_WaitSuccess;
    lpAudit->wait_timeouts = g_WaitTimeouts;
    lpAudit->wait_failures = g_WaitFailures;
    lpAudit->wait_access_denied = g_WaitAccessDenied;
    lpAudit->wait_invalid_handle = g_WaitInvalidHandle;
    lpAudit->event_consumes = g_WaitEventConsumes;
    lpAudit->mutex_acquires = g_WaitMutexAcquires;
    lpAudit->mutex_abandoned = g_WaitMutexAbandoned;
    lpAudit->semaphore_consumes = g_WaitSemaphoreConsumes;
    lpAudit->timer_consumes = g_WaitTimerConsumes;
    lpAudit->wait_all_commits = g_WaitAllCommits;
    lpAudit->wait_any_commits = g_WaitAnyCommits;
    lpAudit->wake_broadcasts = g_WaitWakeBroadcasts;
    lpAudit->wake_skips = g_WaitWakeSkips;
    lpAudit->wait_single_targeted = g_WaitSingleTargeted;
    lpAudit->wait_single_global_fallback = g_WaitSingleGlobalFallback;
    lpAudit->wait_multiple_targeted = g_WaitMultipleTargeted;
    lpAudit->wait_multiple_global_fallback = g_WaitMultipleGlobalFallback;
    lpAudit->wait_multiple_targeted_wakes = g_WaitMultipleTargetedWakes;
    lpAudit->wait_multiple_waitblock_links = g_WaitMultipleWaitBlockLinks;
    lpAudit->wait_multiple_waitblock_unlinks = g_WaitMultipleWaitBlockUnlinks;
    lpAudit->wait_multiple_waitblock_object_wakes = g_WaitMultipleWaitBlockObjectWakes;
    lpAudit->wait_multiple_resolved_probes = g_WaitMultipleResolvedProbes;
    lpAudit->wait_multiple_immediate_hits = g_WaitMultipleImmediateHits;
    lpAudit->wait_multiple_deferred_links = g_WaitMultipleDeferredLinks;
    lpAudit->wait_multiple_tls_gates = g_WaitMultipleTlsGates;
    lpAudit->wait_multiple_prevalidated = g_WaitMultiplePrevalidated;
    lpAudit->wait_multiple_prevalidate_resolves = g_WaitMultiplePrevalidateResolves;
    lpAudit->wait_multiple_prevalidate_fallbacks = g_WaitMultiplePrevalidateFallbacks;
    lpAudit->wait_process_thread_targeted = g_WaitProcessThreadTargeted;
    lpAudit->wait_process_thread_immediate_hits = g_WaitProcessThreadImmediateHits;
    lpAudit->wait_process_thread_poll_slices = g_WaitProcessThreadPollSlices;
    lpAudit->wait_process_thread_object_wakes = g_WaitProcessThreadObjectWakes;
    lpAudit->wait_dispatcher_header_inits = g_WaitDispatcherHeaderInits;
    lpAudit->wait_dispatcher_header_head_hits = g_WaitDispatcherHeaderHeadHits;
    lpAudit->wait_dispatcher_header_state_stores = g_WaitDispatcherHeaderStateStores;
    lpAudit->wait_dispatcher_header_fast_not_ready = g_WaitDispatcherHeaderFastNotReady;
    return TRUE;
}

DWORD MyWinSweepExitedHandleTables(void)
{
    MyWinPollAllProcesses();
    MyWinPidSnapCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    pthread_mutex_lock(&g_HandleLock);
    mywin_walk_handle_tables_locked(0, mywin_pid_snapshot_cb, &ctx);
    pthread_mutex_unlock(&g_HandleLock);
    if (ctx.failed) { free(ctx.pids); return 0; }

    DWORD closed = 0;
    for (DWORD i=0;i<ctx.count;i++) {
        DWORD flags = 0;
        if (!mywin_lite_process_flags_snapshot(ctx.pids[i], &flags)) continue;
        if (flags & MYWIN_PROCESS_EXITED) closed += mywin_close_all_handles_for_pid(ctx.pids[i]);
    }
    free(ctx.pids);
    return closed;
}

BOOL MyGetHandleInfo(HANDLE hHandle, MyHandleInfo* lpInfo)
{
    if (!lpInfo) return FALSE;
    pthread_mutex_lock(&g_HandleLock);
    MyWinHandleEntry* e = mywin_find_handle_locked(mywin_current_pid(), hHandle);
    if (!e) e = mywin_find_handle_any_locked(hHandle);
    if (!e) { pthread_mutex_unlock(&g_HandleLock); memset(lpInfo,0,sizeof(*lpInfo)); return FALSE; }
    mywin_fill_handle_info(e, lpInfo);
    pthread_mutex_unlock(&g_HandleLock);
    return TRUE;
}

HANDLE GetCurrentProcess(void) { return MYWIN_PSEUDO_CURRENT_PROCESS; }
HANDLE GetCurrentThread(void) { return MYWIN_PSEUDO_CURRENT_THREAD; }

static unsigned long long mywin_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000ull);
}

static void mywin_event_publish_state(MyWinEventObj* ev)
{
    if (!ev || !ev->valid) return;
    DWORD flags = 0;
    if (ev->signaled) flags |= _OBJECT_FLAG_EVENT_SIGNALED;
    if (ev->manualReset) flags |= _OBJECT_FLAG_EVENT_MANUAL_RESET;
    mywin_dispatcher_header_bind(&ev->dispatcher, ev->handle, _OBJECT_TYPE_EVENT, ev->signaled ? 1 : 0);
    _ObjectSetInfo(ev->handle, flags, 0, ev->name);
}


static void mywin_mutex_publish_state(MyWinMutexObj* m)
{
    if (!m || !m->valid) return;
    DWORD flags = 0;
    if (m->owned) flags |= _OBJECT_FLAG_MUTEX_OWNED;
    if (m->abandoned) flags |= _OBJECT_FLAG_MUTEX_ABANDONED;
    mywin_dispatcher_header_bind(&m->dispatcher, m->handle, _OBJECT_TYPE_MUTEX, (!m->owned || m->abandoned) ? 1 : 0);
    _ObjectSetInfo(m->handle, flags, m->owner_thread, m->name);
}

static void mywin_semaphore_publish_state(MyWinSemaphoreObj* sem)
{
    if (!sem || !sem->valid) return;
    mywin_dispatcher_header_bind(&sem->dispatcher, sem->handle, _OBJECT_TYPE_SEMAPHORE, sem->count > 0 ? sem->count : 0);
    _ObjectSetInfo(sem->handle, (DWORD)(sem->count < 0 ? 0 : sem->count), (DWORD)(sem->maxCount < 0 ? 0 : sem->maxCount), sem->name);
}

static void mywin_timer_update_state_locked(MyWinTimerObj* t)
{
    if (!t || !t->valid) return;
    if (t->active && !t->signaled && mywin_now_ms() >= t->dueMs) {
        t->signaled = TRUE;
        if (t->periodMs <= 0) t->active = FALSE;
    }
}

static void mywin_timer_publish_state(MyWinTimerObj* t)
{
    if (!t || !t->valid) return;
    DWORD flags = 0;
    pthread_mutex_lock(&t->lock);
    mywin_timer_update_state_locked(t);
    if (t->signaled) flags |= _OBJECT_FLAG_TIMER_SIGNALED;
    if (t->manualReset) flags |= _OBJECT_FLAG_TIMER_MANUAL;
    if (t->periodMs > 0) flags |= _OBJECT_FLAG_TIMER_PERIODIC;
    DWORD size = (DWORD)t->periodMs;
    LONG signalState = t->signaled ? 1 : 0;
    pthread_mutex_unlock(&t->lock);
    mywin_dispatcher_header_bind(&t->dispatcher, t->handle, _OBJECT_TYPE_TIMER, signalState);
    _ObjectSetInfo(t->handle, flags, size, t->name);
}

static MyWinSectionObj* mywin_find_section(HANDLE h)
{
    DWORD type = 0, slot = 0;
    if (_ObjectDecodeSlotHandle(h, &type, &slot) && type == _OBJECT_TYPE_SECTION) {
        if (slot < MYWIN_MAX_SECTIONS && g_Sections[slot].valid && g_Sections[slot].handle == h) return &g_Sections[slot];
        return NULL;
    }
    for (int i = 0; i < MYWIN_MAX_SECTIONS; i++)
        if (g_Sections[i].valid && g_Sections[i].handle == h) return &g_Sections[i];
    return NULL;
}

static MyWinSectionObj* mywin_find_section_slot_payload(HANDLE h, DWORD slot)
{
    if (MYOS_LIKELY(slot < MYWIN_MAX_SECTIONS && g_Sections[slot].valid && g_Sections[slot].handle == h)) {
        __atomic_add_fetch(&g_NamedDirectorySlotFastHits, 1u, __ATOMIC_RELAXED);
        return &g_Sections[slot];
    }
    __atomic_add_fetch(&g_NamedDirectorySlotFastMisses, 1u, __ATOMIC_RELAXED);
    return NULL;
}


HANDLE CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
                          DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow,
                          LPCSTR lpName)
{
    (void)hFile; (void)dwMaximumSizeHigh;
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_SECTION_MAP)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    if (dwMaximumSizeLow == 0 || dwMaximumSizeLow > (1024u * 1024u)) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return 0; }

    char canonName[96]; DWORD nsId = 0;
    mywin_canonical_name(lpName, canonName, sizeof(canonName), &nsId);
    DWORD dirType = _OBJECT_TYPE_NONE;
    HANDLE dirObj = 0;
    DWORD dirSlot = 0xffffffffu, dirGeneration = 0;
    int dirLookup = mywin_named_directory_fast_lookup_payload(canonName, _OBJECT_TYPE_SECTION, &dirObj, &dirType, &dirSlot, &dirGeneration);
    (void)dirGeneration;
    if (dirLookup == MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    MyWinSectionObj* existing = NULL;
    if (dirObj) {
        existing = mywin_find_section_slot_payload(dirObj, dirSlot);
        if (!existing) existing = mywin_find_section(dirObj);
        if (!existing) mywin_named_directory_note_stale();
    }
    if (!existing) existing = mywin_find_section_by_canon_locked(canonName);
    if (existing) {
        if (!mywin_object_allows_open(existing->handle, FILE_MAP_ALL_ACCESS)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
        existing->refCount++;
        _ObjectAddRef(existing->handle);
        return mywin_alloc_process_handle(mywin_current_pid(), existing->handle, _OBJECT_TYPE_SECTION,
                                          FILE_MAP_ALL_ACCESS,
                                          lpFileMappingAttributes ? lpFileMappingAttributes->bInheritHandle : FALSE);
    }

    _ObjectSecurity objSec; BOOL hasObjSec = FALSE;
    if (!mywin_security_from_attributes(lpFileMappingAttributes, &objSec, &hasObjSec)) return 0;

    int i = mywin_pop_section_slot();
    if (i < 0) { mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }

    HANDLE objHandle = _ObjectMakeSlotHandle(_OBJECT_TYPE_SECTION, (DWORD)i);
    char shmName[96];
    mywin_make_section_shm_name(shmName, sizeof(shmName), objHandle, canonName);
    int fd = shm_open(shmName, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        shm_unlink(shmName);
        fd = shm_open(shmName, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd < 0) { mywin_push_section_slot(i); mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    if (ftruncate(fd, (off_t)dwMaximumSizeLow) != 0) { close(fd); shm_unlink(shmName); mywin_push_section_slot(i); mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    BYTE* data = (BYTE*)mmap(NULL, dwMaximumSizeLow, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); shm_unlink(shmName); mywin_push_section_slot(i); mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    memset(data, 0, dwMaximumSizeLow);

    memset(&g_Sections[i], 0, sizeof(g_Sections[i]));
    g_Sections[i].valid = 1;
    g_Sections[i].handle = objHandle;
    g_Sections[i].owner_pid = g_CurrentCapability.id;
    g_Sections[i].protect = flProtect;
    g_Sections[i].size = dwMaximumSizeLow;
    g_Sections[i].refCount = 1;
    g_Sections[i].shmFd = fd;
    snprintf(g_Sections[i].name, sizeof(g_Sections[i].name), "%s", canonName[0] ? canonName : "");
    snprintf(g_Sections[i].shmName, sizeof(g_Sections[i].shmName), "%s", shmName);
    g_Sections[i].data = data;
    mywin_section_hash_insert_locked(i);
    if (!mywin_named_directory_insert(g_Sections[i].name, _OBJECT_TYPE_SECTION, g_Sections[i].handle)) {
        mywin_section_hash_remove_locked(i);
        munmap(g_Sections[i].data, g_Sections[i].size);
        close(g_Sections[i].shmFd);
        shm_unlink(g_Sections[i].shmName);
        memset(&g_Sections[i], 0, sizeof(g_Sections[i]));
        mywin_push_section_slot(i);
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    _ObjectRegister(g_Sections[i].handle, _OBJECT_TYPE_SECTION, g_Sections[i].owner_pid,
                  _OBJECT_ACCESS_READ|_OBJECT_ACCESS_WRITE|_OBJECT_ACCESS_MAP,
                  g_Sections[i].size, g_Sections[i].name);
    mywin_apply_object_security(g_Sections[i].handle, nsId, g_Sections[i].name, &objSec, hasObjSec);

    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), g_Sections[i].handle, _OBJECT_TYPE_SECTION,
                                          FILE_MAP_ALL_ACCESS,
                                          lpFileMappingAttributes ? lpFileMappingAttributes->bInheritHandle : FALSE);
    if (!h) { mywin_release_object_ref_by_type(g_Sections[i].handle, _OBJECT_TYPE_SECTION); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    return h;
}


HANDLE OpenFileMappingA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName)
{
    (void)bInheritHandle;
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_SECTION_MAP)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    char canonName[96]; DWORD nsId = 0;
    mywin_canonical_name(lpName, canonName, sizeof(canonName), &nsId);
    (void)nsId;
    DWORD dirType = _OBJECT_TYPE_NONE;
    HANDLE dirObj = 0;
    DWORD dirSlot = 0xffffffffu, dirGeneration = 0;
    int dirLookup = mywin_named_directory_fast_lookup_payload(canonName, _OBJECT_TYPE_SECTION, &dirObj, &dirType, &dirSlot, &dirGeneration);
    (void)dirGeneration;
    if (dirLookup == MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    MyWinSectionObj* existing = NULL;
    if (dirObj) {
        existing = mywin_find_section_slot_payload(dirObj, dirSlot);
        if (!existing) existing = mywin_find_section(dirObj);
        if (!existing) mywin_named_directory_note_stale();
    }
    if (!existing) existing = mywin_find_section_by_canon_locked(canonName);
    if (!existing) { mywin_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    DWORD desired = dwDesiredAccess ? dwDesiredAccess : (FILE_MAP_READ|_OBJECT_ACCESS_MAP);
    if (!mywin_object_allows_open(existing->handle, desired)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    existing->refCount++;
    _ObjectAddRef(existing->handle);
    if (dwDesiredAccess) _ObjectSetInfo(existing->handle, 0, existing->size, existing->name);
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), existing->handle, _OBJECT_TYPE_SECTION,
                                          desired,
                                          bInheritHandle);
    if (!h) { existing->refCount--; _ObjectRelease(existing->handle); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); }
    return h;
}

LPVOID MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                     DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow,
                     DWORD dwNumberOfBytesToMap)
{
    (void)dwFileOffsetHigh;
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_SECTION_MAP)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return NULL; }
    DWORD type = 0, have = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hFileMappingObject, &type, &have);
    MyWinSectionObj* sec = mywin_find_section(hResolved);
    if (!sec || !sec->data || (type != _OBJECT_TYPE_NONE && type != _OBJECT_TYPE_SECTION)) { mywin_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    DWORD need = mywin_map_view_access_to_section_access(dwDesiredAccess);
    if (!mywin_public_access_allowed(hFileMappingObject, have, need)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return NULL; }
    if (dwFileOffsetLow >= sec->size) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    DWORD maxBytes = sec->size - dwFileOffsetLow;
    DWORD bytes = dwNumberOfBytesToMap ? dwNumberOfBytesToMap : maxBytes;
    if (bytes > maxBytes) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    if ((dwDesiredAccess & FILE_MAP_WRITE) && sec->protect != PAGE_READWRITE) { mywin_set_last_error(ERROR_ACCESS_DENIED); return NULL; }

    int i = mywin_pop_view_slot();
    if (i < 0) { mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    memset(&g_Views[i], 0, sizeof(g_Views[i]));
    g_Views[i].valid = 1;
    g_Views[i].ptr = (LPVOID)(sec->data + dwFileOffsetLow);
    g_Views[i].section = sec->handle;
    g_Views[i].access = dwDesiredAccess;
    g_Views[i].size = bytes;
    sec->refCount++;
    _ObjectAddRef(sec->handle);
    return g_Views[i].ptr;
}


BOOL UnmapViewOfFile(LPCVOID lpBaseAddress)
{
    if (!lpBaseAddress) return FALSE;
    for (int i = 0; i < MYWIN_MAX_VIEWS; i++) {
        if (g_Views[i].valid && g_Views[i].ptr == lpBaseAddress) {
            MyWinSectionObj* sec = mywin_find_section(g_Views[i].section);
            if (sec && sec->refCount) sec->refCount--;
            if (sec) _ObjectRelease(sec->handle);
            memset(&g_Views[i], 0, sizeof(g_Views[i]));
            mywin_push_view_slot(i);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL FlushViewOfFile(LPCVOID lpBaseAddress, DWORD dwNumberOfBytesToFlush)
{
    (void)dwNumberOfBytesToFlush;
    return lpBaseAddress ? TRUE : FALSE;
}

BOOL MyWinGetSectionBackingInfo(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                                DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, DWORD dwNumberOfBytesToMap,
                                LPSTR lpShmName, DWORD cchShmName, DWORD* lpMapBytes, DWORD* lpSectionSize, DWORD* lpProtect)
{
    if (lpShmName && cchShmName) lpShmName[0] = 0;
    if (lpMapBytes) *lpMapBytes = 0;
    if (lpSectionSize) *lpSectionSize = 0;
    if (lpProtect) *lpProtect = 0;
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_SECTION_MAP)) return FALSE;
    if (dwFileOffsetHigh != 0) return FALSE;

    DWORD type = 0, have = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hFileMappingObject, &type, &have);
    if (!hResolved || type != _OBJECT_TYPE_SECTION) return FALSE;
    if ((dwDesiredAccess & FILE_MAP_WRITE) && !(have & FILE_MAP_WRITE)) return FALSE;
    if ((dwDesiredAccess & FILE_MAP_READ) && !(have & FILE_MAP_READ) && !(have & FILE_MAP_WRITE)) return FALSE;

    MyWinSectionObj* sec = mywin_find_section(hResolved);
    if (!sec || !sec->shmName[0] || dwFileOffsetLow >= sec->size) return FALSE;
    DWORD maxBytes = sec->size - dwFileOffsetLow;
    DWORD bytes = dwNumberOfBytesToMap ? dwNumberOfBytesToMap : maxBytes;
    if (bytes == 0 || bytes > maxBytes) return FALSE;
    if ((dwDesiredAccess & FILE_MAP_WRITE) && sec->protect != PAGE_READWRITE) return FALSE;

    if (lpShmName && cchShmName) snprintf(lpShmName, cchShmName, "%s", sec->shmName);
    if (lpMapBytes) *lpMapBytes = bytes;
    if (lpSectionSize) *lpSectionSize = sec->size;
    if (lpProtect) *lpProtect = sec->protect;
    sec->refCount++;
    _ObjectAddRef(sec->handle);
    return TRUE;
}

BOOL MyWinReleaseSectionViewHandle(HANDLE hFileMappingObject)
{
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_SECTION_MAP)) return FALSE;
    DWORD type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hFileMappingObject, &type, NULL);
    if (!hResolved || type != _OBJECT_TYPE_SECTION) return FALSE;
    MyWinSectionObj* sec = mywin_find_section(hResolved);
    if (!sec) return FALSE;
    if (sec->refCount) sec->refCount--;
    _ObjectRelease(sec->handle);
    if (sec->refCount == 0) {
        int idx = (int)(sec - g_Sections);
        HANDLE obj = sec->handle;
        mywin_section_hash_remove_locked(idx);
        mywin_named_directory_remove(sec->name, _OBJECT_TYPE_SECTION, obj);
        _ObjectUnregister(obj);
        mywin_destroy_section_storage(sec);
        memset(sec, 0, sizeof(*sec));
        mywin_push_section_slot(idx);
    }
    return TRUE;
}


static DWORD mywin_named_hash(LPCSTR s)
{
    DWORD h = 2166136261u;
    if (!s) return h;
    while (*s) {
        h ^= (BYTE)(*s++);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static inline int mywin_named_bucket(DWORD hash)
{
    return (int)(hash & MYWIN_NAMED_OBJECT_HASH_MASK);
}

static inline int mywin_named_directory_bucket(DWORD hash)
{
    return (int)(hash & MYWIN_NAMED_DIRECTORY_MASK);
}

static DWORD mywin_named_directory_epoch_load(void)
{
    DWORD e = __atomic_load_n(&g_NamedDirectoryEpoch, __ATOMIC_ACQUIRE);
    return e ? e : 1u;
}

static void mywin_named_directory_epoch_bump_locked(void)
{
    DWORD e = __atomic_add_fetch(&g_NamedDirectoryEpoch, 1u, __ATOMIC_RELEASE);
    if (!e) __atomic_store_n(&g_NamedDirectoryEpoch, 1u, __ATOMIC_RELEASE);
}

static unsigned mywin_named_directory_tls_bucket(DWORD hash, DWORD wantedType)
{
    DWORD x = hash ^ (wantedType * 2654435761u);
    x ^= x >> 16;
    return (unsigned)(x & MYWIN_NAMED_DIRECTORY_TLS_MASK);
}

static BOOL mywin_named_directory_tls_probe(LPCSTR canon, DWORD hash, DWORD wantedType, HANDLE* handleOut, DWORD* existingTypeOut, DWORD* slotOut, DWORD* generationOut)
{
    if (handleOut) *handleOut = 0;
    if (existingTypeOut) *existingTypeOut = _OBJECT_TYPE_NONE;
    if (slotOut) *slotOut = 0xffffffffu;
    if (generationOut) *generationOut = 0;
    if (!canon || !canon[0] || wantedType == _OBJECT_TYPE_NONE) return FALSE;
    unsigned set = mywin_named_directory_tls_bucket(hash, wantedType);
    unsigned base = set * MYWIN_NAMED_DIRECTORY_TLS_WAYS;
    DWORD epoch = mywin_named_directory_epoch_load();
    BOOL sawValid = FALSE;
    for (unsigned way = 0; way < MYWIN_NAMED_DIRECTORY_TLS_WAYS; ++way) {
        MyWinNamedDirectoryTlsEntry* e = &g_NamedDirectoryTls[base + way];
        if (!e->valid) continue;
        sawValid = TRUE;
        if (MYOS_UNLIKELY(e->epoch != epoch)) {
            __atomic_add_fetch(&g_NamedDirectoryTlsEpochMisses, 1u, __ATOMIC_RELAXED);
            e->valid = 0;
            continue;
        }
        if (MYOS_LIKELY(e->nameHash == hash && e->wantedType == wantedType && e->name[0] && strcmp(e->name, canon) == 0)) {
            if (handleOut) *handleOut = e->objectHandle;
            if (existingTypeOut) *existingTypeOut = e->objectType;
            if (slotOut) *slotOut = e->objectSlot;
            if (generationOut) *generationOut = e->objectGeneration;
            __atomic_add_fetch(&g_NamedDirectoryTlsHits, 1u, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_NamedDirectoryLookups, 1u, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_NamedDirectoryHits, 1u, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_NamedDirectoryFastHits, 1u, __ATOMIC_RELAXED);
            return TRUE;
        }
    }
    if (sawValid) __atomic_add_fetch(&g_NamedDirectoryTlsCollisions, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_NamedDirectoryTlsMisses, 1u, __ATOMIC_RELAXED);
    return FALSE;
}

static void mywin_named_directory_slot_from_handle(HANDLE objectHandle, DWORD objectType, DWORD* slotOut, DWORD* generationOut)
{
    DWORD type = 0, slot = 0, gen = 0;
    if (_ObjectDecodeObjectId(objectHandle, &type, &slot, &gen) && type == objectType) {
        if (slotOut) *slotOut = slot;
        if (generationOut) *generationOut = gen;
    } else {
        if (slotOut) *slotOut = 0xffffffffu;
        if (generationOut) *generationOut = 0;
    }
}

static void mywin_named_directory_tls_store(LPCSTR canon, DWORD hash, DWORD wantedType, DWORD objectType, HANDLE objectHandle, DWORD objectSlot, DWORD objectGeneration)
{
    if (!canon || !canon[0] || !objectHandle || wantedType == _OBJECT_TYPE_NONE) return;
    unsigned set = mywin_named_directory_tls_bucket(hash, wantedType);
    unsigned base = set * MYWIN_NAMED_DIRECTORY_TLS_WAYS;
    DWORD epoch = mywin_named_directory_epoch_load();
    MyWinNamedDirectoryTlsEntry* victim = NULL;
    for (unsigned way = 0; way < MYWIN_NAMED_DIRECTORY_TLS_WAYS; ++way) {
        MyWinNamedDirectoryTlsEntry* e = &g_NamedDirectoryTls[base + way];
        if (!e->valid || e->epoch != epoch) { victim = e; break; }
        if (e->nameHash == hash && e->wantedType == wantedType && strcmp(e->name, canon) == 0) { victim = e; break; }
    }
    if (!victim) {
        unsigned way = (unsigned)(g_NamedDirectoryTlsNext[set]++ & (MYWIN_NAMED_DIRECTORY_TLS_WAYS - 1u));
        victim = &g_NamedDirectoryTls[base + way];
    }
    MyWinNamedDirectoryTlsEntry* e = victim;
    memset(e, 0, sizeof(*e));
    e->valid = 1;
    e->epoch = epoch;
    e->nameHash = hash;
    e->wantedType = wantedType;
    e->objectType = objectType;
    e->objectHandle = objectHandle;
    e->objectSlot = objectSlot;
    e->objectGeneration = objectGeneration;
    snprintf(e->name, sizeof(e->name), "%s", canon);
    __atomic_add_fetch(&g_NamedDirectoryTlsStores, 1u, __ATOMIC_RELAXED);
}

static void mywin_named_directory_note_stale(void)
{
    __atomic_add_fetch(&g_NamedDirectoryStaleHits, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_NamedDirectoryTlsStaleInvalidations, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_NamedDirectoryEpoch, 1u, __ATOMIC_RELEASE);
}

static void mywin_named_directory_free_init_locked(void)
{
    if (g_NamedDirectoryFreeInit) return;
    g_NamedDirectoryFreeTop = 0;
    for (int i = MYWIN_NAMED_DIRECTORY_MAX - 1; i >= 0; --i) {
        g_NamedDirectoryFree[g_NamedDirectoryFreeTop++] = i;
        g_NamedDirectoryFreeMark[i] = 1;
    }
    g_NamedDirectoryFreeInit = 1;
}

static int mywin_named_directory_pop_free_locked(void)
{
    mywin_named_directory_free_init_locked();
    while (g_NamedDirectoryFreeTop > 0) {
        int idx = g_NamedDirectoryFree[--g_NamedDirectoryFreeTop];
        if (idx >= 0 && idx < MYWIN_NAMED_DIRECTORY_MAX) {
            g_NamedDirectoryFreeMark[idx] = 0;
            if (!g_NamedDirectory[idx].valid) {
                g_NamedDirectoryFreeReuse++;
                return idx;
            }
        }
    }
    return -1;
}

static void mywin_named_directory_push_free_locked(int idx)
{
    mywin_named_directory_free_init_locked();
    if (idx < 0 || idx >= MYWIN_NAMED_DIRECTORY_MAX) return;
    if (g_NamedDirectoryFreeMark[idx]) { g_NamedDirectoryFreeDuplicateSkips++; return; }
    if (g_NamedDirectoryFreeTop >= MYWIN_NAMED_DIRECTORY_MAX) return;
    g_NamedDirectoryFreeMark[idx] = 1;
    g_NamedDirectoryFree[g_NamedDirectoryFreeTop++] = idx;
}

static int mywin_named_directory_find_locked(const char* canon, DWORD hash)
{
    if (!canon || !canon[0]) return -1;
    int b = mywin_named_directory_bucket(hash);
    for (int link = g_NamedDirectoryHash[b]; link; link = g_NamedDirectory[link - 1].hashNext) {
        int idx = link - 1;
        if (MYOS_LIKELY(idx >= 0 && idx < MYWIN_NAMED_DIRECTORY_MAX)) {
            MyWinNamedDirectoryEntry* e = &g_NamedDirectory[idx];
            if (MYOS_LIKELY(e->valid) && e->nameHash == hash && strcmp(e->name, canon) == 0) return idx;
        }
    }
    return -1;
}

static BOOL mywin_named_directory_lookup(LPCSTR canon, DWORD* typeOut, HANDLE* handleOut)
{
    if (typeOut) *typeOut = _OBJECT_TYPE_NONE;
    if (handleOut) *handleOut = 0;
    if (!canon || !canon[0]) return FALSE;
    DWORD h = mywin_named_hash(canon);
    pthread_mutex_lock(&g_NamedDirectoryLock);
    g_NamedDirectoryLookups++;
    int idx = mywin_named_directory_find_locked(canon, h);
    if (idx >= 0) {
        if (typeOut) *typeOut = g_NamedDirectory[idx].objectType;
        if (handleOut) *handleOut = g_NamedDirectory[idx].objectHandle;
        g_NamedDirectoryHits++;
        pthread_mutex_unlock(&g_NamedDirectoryLock);
        return TRUE;
    }
    g_NamedDirectoryMisses++;
    pthread_mutex_unlock(&g_NamedDirectoryLock);
    return FALSE;
}

static int mywin_named_directory_fast_lookup_payload(LPCSTR canon, DWORD wantedType, HANDLE* handleOut, DWORD* existingTypeOut, DWORD* slotOut, DWORD* generationOut)
{
    if (handleOut) *handleOut = 0;
    if (existingTypeOut) *existingTypeOut = _OBJECT_TYPE_NONE;
    if (slotOut) *slotOut = 0xffffffffu;
    if (generationOut) *generationOut = 0;
    if (!canon || !canon[0] || wantedType == _OBJECT_TYPE_NONE) return MYWIN_NAMED_DIRECTORY_MISS;
    DWORD h = mywin_named_hash(canon);
    if (mywin_named_directory_tls_probe(canon, h, wantedType, handleOut, existingTypeOut, slotOut, generationOut)) {
        return MYWIN_NAMED_DIRECTORY_HIT;
    }
    pthread_mutex_lock(&g_NamedDirectoryLock);
    g_NamedDirectoryLookups++;
    int idx = mywin_named_directory_find_locked(canon, h);
    if (idx >= 0) {
        MyWinNamedDirectoryEntry* e = &g_NamedDirectory[idx];
        if (existingTypeOut) *existingTypeOut = e->objectType;
        if (e->objectType != wantedType) {
            g_NamedDirectoryCrossTypeConflicts++;
            g_NamedDirectoryFastTypeMismatches++;
            pthread_mutex_unlock(&g_NamedDirectoryLock);
            return MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH;
        }
        if (handleOut) *handleOut = e->objectHandle;
        if (slotOut) *slotOut = e->objectSlot;
        if (generationOut) *generationOut = e->objectGeneration;
        g_NamedDirectoryHits++;
        g_NamedDirectoryFastHits++;
        HANDLE obj = e->objectHandle;
        DWORD typ = e->objectType;
        DWORD slot = e->objectSlot;
        DWORD gen = e->objectGeneration;
        pthread_mutex_unlock(&g_NamedDirectoryLock);
        mywin_named_directory_tls_store(canon, h, wantedType, typ, obj, slot, gen);
        return MYWIN_NAMED_DIRECTORY_HIT;
    }
    g_NamedDirectoryMisses++;
    g_NamedDirectoryFastMisses++;
    pthread_mutex_unlock(&g_NamedDirectoryLock);
    return MYWIN_NAMED_DIRECTORY_MISS;
}

static int __attribute__((unused)) mywin_named_directory_fast_lookup_type(LPCSTR canon, DWORD wantedType, HANDLE* handleOut, DWORD* existingTypeOut)
{
    return mywin_named_directory_fast_lookup_payload(canon, wantedType, handleOut, existingTypeOut, NULL, NULL);
}

static BOOL __attribute__((unused)) mywin_named_directory_preflight_type(LPCSTR canon, DWORD wantedType, DWORD* existingTypeOut)
{
    if (existingTypeOut) *existingTypeOut = _OBJECT_TYPE_NONE;
    if (!canon || !canon[0]) return TRUE;
    DWORD type = _OBJECT_TYPE_NONE;
    HANDLE ignored = 0;
    if (!mywin_named_directory_lookup(canon, &type, &ignored)) return TRUE;
    if (existingTypeOut) *existingTypeOut = type;
    if (type == wantedType) return TRUE;
    pthread_mutex_lock(&g_NamedDirectoryLock);
    g_NamedDirectoryCrossTypeConflicts++;
    pthread_mutex_unlock(&g_NamedDirectoryLock);
    return FALSE;
}

static BOOL mywin_named_directory_insert(LPCSTR canon, DWORD objectType, HANDLE objectHandle)
{
    if (!canon || !canon[0] || !objectHandle || objectType == _OBJECT_TYPE_NONE) return TRUE;
    DWORD h = mywin_named_hash(canon);
    pthread_mutex_lock(&g_NamedDirectoryLock);
    int existing = mywin_named_directory_find_locked(canon, h);
    if (existing >= 0) {
        MyWinNamedDirectoryEntry* e = &g_NamedDirectory[existing];
        if (e->objectType != objectType) {
            g_NamedDirectoryCrossTypeConflicts++;
            pthread_mutex_unlock(&g_NamedDirectoryLock);
            return FALSE;
        }
        e->objectHandle = objectHandle;
        mywin_named_directory_slot_from_handle(objectHandle, objectType, &e->objectSlot, &e->objectGeneration);
        mywin_named_directory_epoch_bump_locked();
        pthread_mutex_unlock(&g_NamedDirectoryLock);
        return TRUE;
    }
    int idx = mywin_named_directory_pop_free_locked();
    if (idx < 0) {
        pthread_mutex_unlock(&g_NamedDirectoryLock);
        return FALSE;
    }
    MyWinNamedDirectoryEntry* e = &g_NamedDirectory[idx];
    memset(e, 0, sizeof(*e));
    e->valid = 1;
    e->nameHash = h;
    e->objectHandle = objectHandle;
    e->objectType = objectType;
    mywin_named_directory_slot_from_handle(objectHandle, objectType, &e->objectSlot, &e->objectGeneration);
    snprintf(e->name, sizeof(e->name), "%s", canon);
    int b = mywin_named_directory_bucket(h);
    e->hashNext = g_NamedDirectoryHash[b];
    g_NamedDirectoryHash[b] = idx + 1;
    g_NamedDirectoryInserts++;
    mywin_named_directory_epoch_bump_locked();
    pthread_mutex_unlock(&g_NamedDirectoryLock);
    return TRUE;
}

static void mywin_named_directory_remove(LPCSTR canon, DWORD objectType, HANDLE objectHandle)
{
    if (!canon || !canon[0]) return;
    DWORD h = mywin_named_hash(canon);
    pthread_mutex_lock(&g_NamedDirectoryLock);
    int b = mywin_named_directory_bucket(h);
    int* link = &g_NamedDirectoryHash[b];
    while (*link) {
        int idx = *link - 1;
        if (idx >= 0 && idx < MYWIN_NAMED_DIRECTORY_MAX) {
            MyWinNamedDirectoryEntry* e = &g_NamedDirectory[idx];
            if (e->valid && e->nameHash == h && e->objectType == objectType &&
                e->objectHandle == objectHandle && strcmp(e->name, canon) == 0) {
                *link = e->hashNext;
                memset(e, 0, sizeof(*e));
                mywin_named_directory_push_free_locked(idx);
                g_NamedDirectoryRemoves++;
                mywin_named_directory_epoch_bump_locked();
                break;
            }
            link = &e->hashNext;
        } else {
            break;
        }
    }
    pthread_mutex_unlock(&g_NamedDirectoryLock);
}

static void mywin_section_hash_insert_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_SECTIONS || !g_Sections[idx].valid || !g_Sections[idx].name[0]) return;
    DWORD h = mywin_named_hash(g_Sections[idx].name);
    int b = mywin_named_bucket(h);
    g_Sections[idx].nameHash = h;
    g_Sections[idx].nameHashNext = g_SectionNameHash[b];
    g_SectionNameHash[b] = idx + 1;
}

static void mywin_section_hash_remove_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_SECTIONS || !g_Sections[idx].nameHash) return;
    int b = mywin_named_bucket(g_Sections[idx].nameHash);
    int* link = &g_SectionNameHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_Sections[cur].nameHashNext; break; }
        link = &g_Sections[cur].nameHashNext;
    }
    g_Sections[idx].nameHash = 0;
    g_Sections[idx].nameHashNext = 0;
}

static MyWinSectionObj* mywin_find_section_by_canon_locked(LPCSTR canon)
{
    if (!canon || !canon[0]) return NULL;
    DWORD h = mywin_named_hash(canon);
    int b = mywin_named_bucket(h);
    for (int link = g_SectionNameHash[b]; link; link = g_Sections[link - 1].nameHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYWIN_MAX_SECTIONS && g_Sections[idx].valid &&
            g_Sections[idx].nameHash == h && strcmp(g_Sections[idx].name, canon) == 0) return &g_Sections[idx];
    }
    return NULL;
}

static void mywin_timer_due_cache_invalidate(void)
{
    g_TimerNextDueCacheValid = FALSE;
    g_TimerNextDueCacheMs = 0;
}

static void mywin_timer_due_cache_note_locked(const MyWinTimerObj* t)
{
    if (!t || !t->valid || !t->active || t->signaled) return;
    if (!g_TimerNextDueCacheValid || t->dueMs < g_TimerNextDueCacheMs) {
        g_TimerNextDueCacheMs = t->dueMs;
        g_TimerNextDueCacheValid = TRUE;
    }
}

static void mywin_timer_hash_insert_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_TIMERS || !g_Timers[idx].valid || !g_Timers[idx].name[0]) return;
    DWORD h = mywin_named_hash(g_Timers[idx].name);
    int b = mywin_named_bucket(h);
    g_Timers[idx].nameHash = h;
    g_Timers[idx].nameHashNext = g_TimerNameHash[b];
    g_TimerNameHash[b] = idx + 1;
}

static void mywin_timer_hash_remove_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_TIMERS || !g_Timers[idx].nameHash) return;
    int b = mywin_named_bucket(g_Timers[idx].nameHash);
    int* link = &g_TimerNameHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_Timers[cur].nameHashNext; break; }
        link = &g_Timers[cur].nameHashNext;
    }
    g_Timers[idx].nameHash = 0;
    g_Timers[idx].nameHashNext = 0;
}

static MyWinTimerObj* mywin_find_timer_by_canon_locked(LPCSTR canon)
{
    if (!canon || !canon[0]) return NULL;
    DWORD h = mywin_named_hash(canon);
    int b = mywin_named_bucket(h);
    for (int link = g_TimerNameHash[b]; link; link = g_Timers[link - 1].nameHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYWIN_MAX_TIMERS && g_Timers[idx].valid &&
            g_Timers[idx].nameHash == h && strcmp(g_Timers[idx].name, canon) == 0) return &g_Timers[idx];
    }
    return NULL;
}

static void mywin_event_hash_insert_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_EVENTS || !g_Events[idx].valid || !g_Events[idx].name[0]) return;
    DWORD h = mywin_named_hash(g_Events[idx].name);
    int b = mywin_named_bucket(h);
    g_Events[idx].nameHash = h;
    g_Events[idx].nameHashNext = g_EventNameHash[b];
    g_EventNameHash[b] = idx + 1;
}

static void mywin_event_hash_remove_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_EVENTS || !g_Events[idx].nameHash) return;
    int b = mywin_named_bucket(g_Events[idx].nameHash);
    int* link = &g_EventNameHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_Events[cur].nameHashNext; break; }
        link = &g_Events[cur].nameHashNext;
    }
    g_Events[idx].nameHash = 0;
    g_Events[idx].nameHashNext = 0;
}

static MyWinEventObj* mywin_find_event_by_canon_locked(LPCSTR canon)
{
    if (!canon || !canon[0]) return NULL;
    DWORD h = mywin_named_hash(canon);
    int b = mywin_named_bucket(h);
    for (int link = g_EventNameHash[b]; link; link = g_Events[link - 1].nameHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYWIN_MAX_EVENTS && g_Events[idx].valid &&
            g_Events[idx].nameHash == h && strcmp(g_Events[idx].name, canon) == 0) return &g_Events[idx];
    }
    return NULL;
}

static void mywin_mutex_hash_insert_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_MUTEXES || !g_Mutexes[idx].valid || !g_Mutexes[idx].name[0]) return;
    DWORD h = mywin_named_hash(g_Mutexes[idx].name);
    int b = mywin_named_bucket(h);
    g_Mutexes[idx].nameHash = h;
    g_Mutexes[idx].nameHashNext = g_MutexNameHash[b];
    g_MutexNameHash[b] = idx + 1;
}

static void mywin_mutex_hash_remove_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_MUTEXES || !g_Mutexes[idx].nameHash) return;
    int b = mywin_named_bucket(g_Mutexes[idx].nameHash);
    int* link = &g_MutexNameHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_Mutexes[cur].nameHashNext; break; }
        link = &g_Mutexes[cur].nameHashNext;
    }
    g_Mutexes[idx].nameHash = 0;
    g_Mutexes[idx].nameHashNext = 0;
}

static MyWinMutexObj* mywin_find_mutex_by_canon_locked(LPCSTR canon)
{
    if (!canon || !canon[0]) return NULL;
    DWORD h = mywin_named_hash(canon);
    int b = mywin_named_bucket(h);
    for (int link = g_MutexNameHash[b]; link; link = g_Mutexes[link - 1].nameHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYWIN_MAX_MUTEXES && g_Mutexes[idx].valid &&
            g_Mutexes[idx].nameHash == h && strcmp(g_Mutexes[idx].name, canon) == 0) return &g_Mutexes[idx];
    }
    return NULL;
}

static void mywin_semaphore_hash_insert_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_SEMAPHORES || !g_Semaphores[idx].valid || !g_Semaphores[idx].name[0]) return;
    DWORD h = mywin_named_hash(g_Semaphores[idx].name);
    int b = mywin_named_bucket(h);
    g_Semaphores[idx].nameHash = h;
    g_Semaphores[idx].nameHashNext = g_SemaphoreNameHash[b];
    g_SemaphoreNameHash[b] = idx + 1;
}

static void mywin_semaphore_hash_remove_locked(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_SEMAPHORES || !g_Semaphores[idx].nameHash) return;
    int b = mywin_named_bucket(g_Semaphores[idx].nameHash);
    int* link = &g_SemaphoreNameHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_Semaphores[cur].nameHashNext; break; }
        link = &g_Semaphores[cur].nameHashNext;
    }
    g_Semaphores[idx].nameHash = 0;
    g_Semaphores[idx].nameHashNext = 0;
}

static MyWinSemaphoreObj* mywin_find_semaphore_by_canon_locked(LPCSTR canon)
{
    if (!canon || !canon[0]) return NULL;
    DWORD h = mywin_named_hash(canon);
    int b = mywin_named_bucket(h);
    for (int link = g_SemaphoreNameHash[b]; link; link = g_Semaphores[link - 1].nameHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYWIN_MAX_SEMAPHORES && g_Semaphores[idx].valid &&
            g_Semaphores[idx].nameHash == h && strcmp(g_Semaphores[idx].name, canon) == 0) return &g_Semaphores[idx];
    }
    return NULL;
}

static int mywin_type_slot_from_handle(HANDLE h, DWORD expectedType, DWORD maxSlots)
{
    DWORD type = 0, slot = 0;
    if (!_ObjectDecodeSlotHandle(h, &type, &slot)) return -1;
    if (type != expectedType || slot >= maxSlots) return -1;
    return (int)slot;
}

static MyWinEventObj* mywin_find_event(HANDLE h)
{
    int idx = mywin_type_slot_from_handle(h, _OBJECT_TYPE_EVENT, MYWIN_MAX_EVENTS);
    if (idx >= 0) return (g_Events[idx].valid && g_Events[idx].handle == h) ? &g_Events[idx] : NULL;
    for (int i = 0; i < MYWIN_MAX_EVENTS; i++)
        if (g_Events[i].valid && g_Events[i].handle == h) return &g_Events[i];
    return NULL;
}

static MyWinEventObj* mywin_find_event_slot_payload(HANDLE h, DWORD slot)
{
    if (MYOS_LIKELY(slot < MYWIN_MAX_EVENTS && g_Events[slot].valid && g_Events[slot].handle == h)) {
        __atomic_add_fetch(&g_NamedDirectorySlotFastHits, 1u, __ATOMIC_RELAXED);
        return &g_Events[slot];
    }
    __atomic_add_fetch(&g_NamedDirectorySlotFastMisses, 1u, __ATOMIC_RELAXED);
    return NULL;
}

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName)
{
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_IPC)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }

    char canonName[96]; DWORD nsId = 0;
    mywin_canonical_name(lpName, canonName, sizeof(canonName), &nsId);
    DWORD dirType = _OBJECT_TYPE_NONE;
    HANDLE dirObj = 0;
    DWORD dirSlot = 0xffffffffu, dirGeneration = 0;
    int dirLookup = mywin_named_directory_fast_lookup_payload(canonName, _OBJECT_TYPE_EVENT, &dirObj, &dirType, &dirSlot, &dirGeneration);
    (void)dirGeneration;
    if (dirLookup == MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }

    _ObjectSecurity objSec; BOOL hasObjSec = FALSE;
    if (!mywin_security_from_attributes(lpEventAttributes, &objSec, &hasObjSec)) return 0;

    HANDLE obj = 0;
    DWORD desired = EVENT_ALL_ACCESS;
    BOOL created = FALSE;
    pthread_mutex_lock(&g_EventTableLock);
    MyWinEventObj* existing = NULL;
    if (dirObj) {
        existing = mywin_find_event_slot_payload(dirObj, dirSlot);
        if (!existing) existing = mywin_find_event(dirObj);
        if (!existing) mywin_named_directory_note_stale();
    }
    if (!existing) existing = mywin_find_event_by_canon_locked(canonName);
    if (existing) {
        if (!mywin_object_allows_open(existing->handle, desired)) {
            pthread_mutex_unlock(&g_EventTableLock);
            mywin_set_last_error(ERROR_ACCESS_DENIED);
            return 0;
        }
        obj = existing->handle;
        mywin_atomic_ref_inc(&existing->refCount);
        _ObjectAddRef(obj);
    } else {
        int i = mywin_pop_event_slot();
        if (i >= 0) {
            memset(&g_Events[i], 0, sizeof(g_Events[i]));
            g_Events[i].valid = 1;
            g_Events[i].handle = _ObjectMakeSlotHandle(_OBJECT_TYPE_EVENT, (DWORD)i);
            g_Events[i].owner_pid = g_CurrentCapability.id;
            g_Events[i].access = EVENT_ALL_ACCESS;
            g_Events[i].refCount = 1;
            g_Events[i].manualReset = bManualReset ? TRUE : FALSE;
            g_Events[i].signaled = bInitialState ? TRUE : FALSE;
            mywin_dispatcher_header_bind(&g_Events[i].dispatcher, g_Events[i].handle, _OBJECT_TYPE_EVENT, g_Events[i].signaled ? 1 : 0);
            snprintf(g_Events[i].name, sizeof(g_Events[i].name), "%s", canonName[0] ? canonName : "");
            pthread_mutex_init(&g_Events[i].lock, NULL);
            mywin_waitable_cond_init(&g_Events[i].cond);
            DWORD flags = 0;
            if (g_Events[i].signaled) flags |= _OBJECT_FLAG_EVENT_SIGNALED;
            if (g_Events[i].manualReset) flags |= _OBJECT_FLAG_EVENT_MANUAL_RESET;
            _ObjectRegister(g_Events[i].handle, _OBJECT_TYPE_EVENT, g_Events[i].owner_pid,
                          _OBJECT_ACCESS_READ|_OBJECT_ACCESS_SIGNAL|_OBJECT_ACCESS_CONTROL,
                          0, g_Events[i].name);
            _ObjectSetInfo(g_Events[i].handle, flags, 0, g_Events[i].name);
            mywin_apply_object_security(g_Events[i].handle, nsId, g_Events[i].name, &objSec, hasObjSec);
            mywin_event_hash_insert_locked(i);
            mywin_named_directory_insert(g_Events[i].name, _OBJECT_TYPE_EVENT, g_Events[i].handle);
            obj = g_Events[i].handle;
            created = TRUE;
        }
    }
    pthread_mutex_unlock(&g_EventTableLock);
    if (!obj) { mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), obj, _OBJECT_TYPE_EVENT,
                                          desired,
                                          lpEventAttributes ? lpEventAttributes->bInheritHandle : FALSE);
    if (!h) { mywin_release_object_ref_by_type(obj, _OBJECT_TYPE_EVENT); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    (void)created;
    return h;
}


HANDLE OpenEventA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName)
{
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_IPC)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    char canonName[96]; DWORD nsId = 0;
    mywin_canonical_name(lpName, canonName, sizeof(canonName), &nsId);
    (void)nsId;
    DWORD dirType = _OBJECT_TYPE_NONE;
    HANDLE dirObj = 0;
    DWORD dirSlot = 0xffffffffu, dirGeneration = 0;
    int dirLookup = mywin_named_directory_fast_lookup_payload(canonName, _OBJECT_TYPE_EVENT, &dirObj, &dirType, &dirSlot, &dirGeneration);
    (void)dirGeneration;
    if (dirLookup == MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    DWORD desired = dwDesiredAccess ? dwDesiredAccess : EVENT_ALL_ACCESS;
    HANDLE obj = 0;
    pthread_mutex_lock(&g_EventTableLock);
    MyWinEventObj* ev = NULL;
    if (dirObj) {
        ev = mywin_find_event_slot_payload(dirObj, dirSlot);
        if (!ev) ev = mywin_find_event(dirObj);
        if (!ev) mywin_named_directory_note_stale();
    }
    if (!ev) ev = mywin_find_event_by_canon_locked(canonName);
    if (!ev) { pthread_mutex_unlock(&g_EventTableLock); mywin_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    if (!mywin_object_allows_open(ev->handle, desired)) { pthread_mutex_unlock(&g_EventTableLock); mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    obj = ev->handle;
    mywin_atomic_ref_inc(&ev->refCount);
    _ObjectAddRef(obj);
    pthread_mutex_unlock(&g_EventTableLock);
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), obj, _OBJECT_TYPE_EVENT, desired, bInheritHandle);
    if (!h) { mywin_release_object_ref_by_type(obj, _OBJECT_TYPE_EVENT); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); }
    return h;
}

BOOL SetEvent(HANDLE hEvent)
{
    DWORD have = 0, type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hEvent, &type, &have);
    MyWinEventObj* ev = mywin_find_event(hResolved);
    if (!ev || (type != _OBJECT_TYPE_NONE && type != _OBJECT_TYPE_EVENT)) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(hEvent, have, EVENT_MODIFY_STATE)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }

    mywin_dispatcher_ensure();
    pthread_mutex_lock(&g_DispatcherLock);
    pthread_mutex_lock(&ev->lock);
    ev->signaled = TRUE;
    mywin_event_publish_state(ev);
    if (ev->manualReset) pthread_cond_broadcast(&ev->cond);
    else pthread_cond_signal(&ev->cond);
    pthread_mutex_unlock(&ev->lock);
    mywin_dispatcher_signal_object_locked(ev->handle, _OBJECT_TYPE_EVENT);
    pthread_mutex_unlock(&g_DispatcherLock);
    return TRUE;
}

BOOL ResetEvent(HANDLE hEvent)
{
    DWORD have = 0, type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hEvent, &type, &have);
    MyWinEventObj* ev = mywin_find_event(hResolved);
    if (!ev || (type != _OBJECT_TYPE_NONE && type != _OBJECT_TYPE_EVENT)) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(hEvent, have, EVENT_MODIFY_STATE)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }

    mywin_dispatcher_ensure();
    pthread_mutex_lock(&g_DispatcherLock);
    pthread_mutex_lock(&ev->lock);
    ev->signaled = FALSE;
    mywin_event_publish_state(ev);
    pthread_mutex_unlock(&ev->lock);
    mywin_dispatcher_signal_object_locked(ev->handle, _OBJECT_TYPE_EVENT);
    pthread_mutex_unlock(&g_DispatcherLock);
    return TRUE;
}


static MyWinMutexObj* mywin_find_mutex(HANDLE h)
{
    int idx = mywin_type_slot_from_handle(h, _OBJECT_TYPE_MUTEX, MYWIN_MAX_MUTEXES);
    if (idx >= 0) return (g_Mutexes[idx].valid && g_Mutexes[idx].handle == h) ? &g_Mutexes[idx] : NULL;
    for (int i = 0; i < MYWIN_MAX_MUTEXES; i++)
        if (g_Mutexes[i].valid && g_Mutexes[i].handle == h) return &g_Mutexes[i];
    return NULL;
}

static MyWinMutexObj* mywin_find_mutex_slot_payload(HANDLE h, DWORD slot)
{
    if (MYOS_LIKELY(slot < MYWIN_MAX_MUTEXES && g_Mutexes[slot].valid && g_Mutexes[slot].handle == h)) {
        __atomic_add_fetch(&g_NamedDirectorySlotFastHits, 1u, __ATOMIC_RELAXED);
        return &g_Mutexes[slot];
    }
    __atomic_add_fetch(&g_NamedDirectorySlotFastMisses, 1u, __ATOMIC_RELAXED);
    return NULL;
}

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName)
{
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_IPC)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    char canonName[96]; DWORD nsId = 0;
    mywin_canonical_name(lpName, canonName, sizeof(canonName), &nsId);
    DWORD dirType = _OBJECT_TYPE_NONE;
    HANDLE dirObj = 0;
    DWORD dirSlot = 0xffffffffu, dirGeneration = 0;
    int dirLookup = mywin_named_directory_fast_lookup_payload(canonName, _OBJECT_TYPE_MUTEX, &dirObj, &dirType, &dirSlot, &dirGeneration);
    (void)dirGeneration;
    if (dirLookup == MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    _ObjectSecurity objSec; BOOL hasObjSec = FALSE;
    if (!mywin_security_from_attributes(lpMutexAttributes, &objSec, &hasObjSec)) return 0;

    HANDLE obj = 0;
    DWORD desired = MUTEX_ALL_ACCESS;
    pthread_mutex_lock(&g_MutexTableLock);
    MyWinMutexObj* existing = NULL;
    if (dirObj) {
        existing = mywin_find_mutex_slot_payload(dirObj, dirSlot);
        if (!existing) existing = mywin_find_mutex(dirObj);
        if (!existing) mywin_named_directory_note_stale();
    }
    if (!existing) existing = mywin_find_mutex_by_canon_locked(canonName);
    if (existing) {
        if (!mywin_object_allows_open(existing->handle, desired)) {
            pthread_mutex_unlock(&g_MutexTableLock);
            mywin_set_last_error(ERROR_ACCESS_DENIED);
            return 0;
        }
        obj = existing->handle;
        mywin_atomic_ref_inc(&existing->refCount);
        _ObjectAddRef(obj);
    } else {
        int i = mywin_pop_mutex_slot();
        if (i >= 0) {
            memset(&g_Mutexes[i], 0, sizeof(g_Mutexes[i]));
            g_Mutexes[i].valid = 1;
            g_Mutexes[i].handle = _ObjectMakeSlotHandle(_OBJECT_TYPE_MUTEX, (DWORD)i);
            g_Mutexes[i].owner_pid = mywin_current_pid();
            g_Mutexes[i].access = MUTEX_ALL_ACCESS;
            g_Mutexes[i].refCount = 1;
            g_Mutexes[i].owned = bInitialOwner ? TRUE : FALSE;
            g_Mutexes[i].owner_thread = bInitialOwner ? GetCurrentThreadId() : 0;
            mywin_dispatcher_header_bind(&g_Mutexes[i].dispatcher, g_Mutexes[i].handle, _OBJECT_TYPE_MUTEX, g_Mutexes[i].owned ? 0 : 1);
            snprintf(g_Mutexes[i].name, sizeof(g_Mutexes[i].name), "%s", canonName[0] ? canonName : "");
            pthread_mutex_init(&g_Mutexes[i].lock, NULL);
            mywin_waitable_cond_init(&g_Mutexes[i].cond);
            _ObjectRegister(g_Mutexes[i].handle, _OBJECT_TYPE_MUTEX, g_Mutexes[i].owner_pid,
                          _OBJECT_ACCESS_READ|_OBJECT_ACCESS_SIGNAL|_OBJECT_ACCESS_CONTROL, 0, g_Mutexes[i].name);
            mywin_apply_object_security(g_Mutexes[i].handle, nsId, g_Mutexes[i].name, &objSec, hasObjSec);
            mywin_mutex_publish_state(&g_Mutexes[i]);
            mywin_mutex_hash_insert_locked(i);
            mywin_named_directory_insert(g_Mutexes[i].name, _OBJECT_TYPE_MUTEX, g_Mutexes[i].handle);
            obj = g_Mutexes[i].handle;
        }
    }
    pthread_mutex_unlock(&g_MutexTableLock);
    if (!obj) { mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), obj, _OBJECT_TYPE_MUTEX,
                                          desired,
                                          lpMutexAttributes ? lpMutexAttributes->bInheritHandle : FALSE);
    if (!h) { mywin_release_object_ref_by_type(obj, _OBJECT_TYPE_MUTEX); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); }
    return h;
}


HANDLE OpenMutexA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName)
{
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_IPC)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    char canonName[96]; DWORD nsId = 0;
    mywin_canonical_name(lpName, canonName, sizeof(canonName), &nsId);
    (void)nsId;
    DWORD dirType = _OBJECT_TYPE_NONE;
    HANDLE dirObj = 0;
    DWORD dirSlot = 0xffffffffu, dirGeneration = 0;
    int dirLookup = mywin_named_directory_fast_lookup_payload(canonName, _OBJECT_TYPE_MUTEX, &dirObj, &dirType, &dirSlot, &dirGeneration);
    (void)dirGeneration;
    if (dirLookup == MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    DWORD desired = dwDesiredAccess ? dwDesiredAccess : MUTEX_ALL_ACCESS;
    HANDLE obj = 0;
    pthread_mutex_lock(&g_MutexTableLock);
    MyWinMutexObj* m = NULL;
    if (dirObj) {
        m = mywin_find_mutex_slot_payload(dirObj, dirSlot);
        if (!m) m = mywin_find_mutex(dirObj);
        if (!m) mywin_named_directory_note_stale();
    }
    if (!m) m = mywin_find_mutex_by_canon_locked(canonName);
    if (!m) { pthread_mutex_unlock(&g_MutexTableLock); mywin_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    if (!mywin_object_allows_open(m->handle, desired)) { pthread_mutex_unlock(&g_MutexTableLock); mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    obj = m->handle;
    mywin_atomic_ref_inc(&m->refCount);
    _ObjectAddRef(obj);
    pthread_mutex_unlock(&g_MutexTableLock);
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), obj, _OBJECT_TYPE_MUTEX, desired, bInheritHandle);
    if (!h) { mywin_release_object_ref_by_type(obj, _OBJECT_TYPE_MUTEX); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); }
    return h;
}

BOOL ReleaseMutex(HANDLE hMutex)
{
    DWORD have = 0, type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hMutex, &type, &have);
    MyWinMutexObj* m = mywin_find_mutex(hResolved);
    if (!m || (type != _OBJECT_TYPE_NONE && type != _OBJECT_TYPE_MUTEX)) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(hMutex, have, MUTEX_MODIFY_STATE)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }

    mywin_dispatcher_ensure();
    pthread_mutex_lock(&g_DispatcherLock);
    pthread_mutex_lock(&m->lock);
    if (!m->owned) {
        pthread_mutex_unlock(&m->lock);
        pthread_mutex_unlock(&g_DispatcherLock);
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    m->owned = FALSE;
    m->owner_thread = 0;
    m->abandoned = FALSE;
    mywin_mutex_publish_state(m);
    pthread_cond_signal(&m->cond);
    pthread_mutex_unlock(&m->lock);
    mywin_dispatcher_signal_object_locked(m->handle, _OBJECT_TYPE_MUTEX);
    pthread_mutex_unlock(&g_DispatcherLock);
    return TRUE;
}

static MyWinSemaphoreObj* mywin_find_semaphore(HANDLE h)
{
    int idx = mywin_type_slot_from_handle(h, _OBJECT_TYPE_SEMAPHORE, MYWIN_MAX_SEMAPHORES);
    if (idx >= 0) return (g_Semaphores[idx].valid && g_Semaphores[idx].handle == h) ? &g_Semaphores[idx] : NULL;
    for (int i = 0; i < MYWIN_MAX_SEMAPHORES; i++)
        if (g_Semaphores[i].valid && g_Semaphores[i].handle == h) return &g_Semaphores[i];
    return NULL;
}

static MyWinSemaphoreObj* mywin_find_semaphore_slot_payload(HANDLE h, DWORD slot)
{
    if (MYOS_LIKELY(slot < MYWIN_MAX_SEMAPHORES && g_Semaphores[slot].valid && g_Semaphores[slot].handle == h)) {
        __atomic_add_fetch(&g_NamedDirectorySlotFastHits, 1u, __ATOMIC_RELAXED);
        return &g_Semaphores[slot];
    }
    __atomic_add_fetch(&g_NamedDirectorySlotFastMisses, 1u, __ATOMIC_RELAXED);
    return NULL;
}

HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount, LPCSTR lpName)
{
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_IPC)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    if (lMaximumCount <= 0 || lInitialCount < 0 || lInitialCount > lMaximumCount) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    char canonName[96]; DWORD nsId = 0;
    mywin_canonical_name(lpName, canonName, sizeof(canonName), &nsId);
    DWORD dirType = _OBJECT_TYPE_NONE;
    HANDLE dirObj = 0;
    DWORD dirSlot = 0xffffffffu, dirGeneration = 0;
    int dirLookup = mywin_named_directory_fast_lookup_payload(canonName, _OBJECT_TYPE_SEMAPHORE, &dirObj, &dirType, &dirSlot, &dirGeneration);
    (void)dirGeneration;
    if (dirLookup == MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    _ObjectSecurity objSec; BOOL hasObjSec = FALSE;
    if (!mywin_security_from_attributes(lpSemaphoreAttributes, &objSec, &hasObjSec)) return 0;

    HANDLE obj = 0;
    DWORD desired = SEMAPHORE_ALL_ACCESS;
    pthread_mutex_lock(&g_SemaphoreTableLock);
    MyWinSemaphoreObj* existing = NULL;
    if (dirObj) {
        existing = mywin_find_semaphore_slot_payload(dirObj, dirSlot);
        if (!existing) existing = mywin_find_semaphore(dirObj);
        if (!existing) mywin_named_directory_note_stale();
    }
    if (!existing) existing = mywin_find_semaphore_by_canon_locked(canonName);
    if (existing) {
        if (!mywin_object_allows_open(existing->handle, desired)) {
            pthread_mutex_unlock(&g_SemaphoreTableLock);
            mywin_set_last_error(ERROR_ACCESS_DENIED);
            return 0;
        }
        obj = existing->handle;
        mywin_atomic_ref_inc(&existing->refCount);
        _ObjectAddRef(obj);
    } else {
        int i = mywin_pop_semaphore_slot();
        if (i >= 0) {
            memset(&g_Semaphores[i], 0, sizeof(g_Semaphores[i]));
            g_Semaphores[i].valid = 1;
            g_Semaphores[i].handle = _ObjectMakeSlotHandle(_OBJECT_TYPE_SEMAPHORE, (DWORD)i);
            g_Semaphores[i].owner_pid = mywin_current_pid();
            g_Semaphores[i].access = SEMAPHORE_ALL_ACCESS;
            g_Semaphores[i].refCount = 1;
            g_Semaphores[i].count = lInitialCount;
            g_Semaphores[i].maxCount = lMaximumCount;
            mywin_dispatcher_header_bind(&g_Semaphores[i].dispatcher, g_Semaphores[i].handle, _OBJECT_TYPE_SEMAPHORE, lInitialCount);
            snprintf(g_Semaphores[i].name, sizeof(g_Semaphores[i].name), "%s", canonName[0] ? canonName : "");
            pthread_mutex_init(&g_Semaphores[i].lock, NULL);
            mywin_waitable_cond_init(&g_Semaphores[i].cond);
            _ObjectRegister(g_Semaphores[i].handle, _OBJECT_TYPE_SEMAPHORE, g_Semaphores[i].owner_pid,
                          _OBJECT_ACCESS_READ|_OBJECT_ACCESS_SIGNAL|_OBJECT_ACCESS_CONTROL, (DWORD)lMaximumCount, g_Semaphores[i].name);
            mywin_apply_object_security(g_Semaphores[i].handle, nsId, g_Semaphores[i].name, &objSec, hasObjSec);
            mywin_semaphore_publish_state(&g_Semaphores[i]);
            mywin_semaphore_hash_insert_locked(i);
            mywin_named_directory_insert(g_Semaphores[i].name, _OBJECT_TYPE_SEMAPHORE, g_Semaphores[i].handle);
            obj = g_Semaphores[i].handle;
        }
    }
    pthread_mutex_unlock(&g_SemaphoreTableLock);
    if (!obj) { mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), obj, _OBJECT_TYPE_SEMAPHORE,
                                          desired,
                                          lpSemaphoreAttributes ? lpSemaphoreAttributes->bInheritHandle : FALSE);
    if (!h) { mywin_release_object_ref_by_type(obj, _OBJECT_TYPE_SEMAPHORE); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); }
    return h;
}


HANDLE OpenSemaphoreA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName)
{
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_IPC)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    char canonName[96]; DWORD nsId = 0;
    mywin_canonical_name(lpName, canonName, sizeof(canonName), &nsId);
    (void)nsId;
    DWORD dirType = _OBJECT_TYPE_NONE;
    HANDLE dirObj = 0;
    DWORD dirSlot = 0xffffffffu, dirGeneration = 0;
    int dirLookup = mywin_named_directory_fast_lookup_payload(canonName, _OBJECT_TYPE_SEMAPHORE, &dirObj, &dirType, &dirSlot, &dirGeneration);
    (void)dirGeneration;
    if (dirLookup == MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    DWORD desired = dwDesiredAccess ? dwDesiredAccess : SEMAPHORE_ALL_ACCESS;
    HANDLE obj = 0;
    pthread_mutex_lock(&g_SemaphoreTableLock);
    MyWinSemaphoreObj* sem = NULL;
    if (dirObj) {
        sem = mywin_find_semaphore_slot_payload(dirObj, dirSlot);
        if (!sem) sem = mywin_find_semaphore(dirObj);
        if (!sem) mywin_named_directory_note_stale();
    }
    if (!sem) sem = mywin_find_semaphore_by_canon_locked(canonName);
    if (!sem) { pthread_mutex_unlock(&g_SemaphoreTableLock); mywin_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    if (!mywin_object_allows_open(sem->handle, desired)) { pthread_mutex_unlock(&g_SemaphoreTableLock); mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    obj = sem->handle;
    mywin_atomic_ref_inc(&sem->refCount);
    _ObjectAddRef(obj);
    pthread_mutex_unlock(&g_SemaphoreTableLock);
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), obj, _OBJECT_TYPE_SEMAPHORE, desired, bInheritHandle);
    if (!h) { mywin_release_object_ref_by_type(obj, _OBJECT_TYPE_SEMAPHORE); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); }
    return h;
}

BOOL ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LONG* lpPreviousCount)
{
    if (lReleaseCount <= 0) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD have = 0, type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hSemaphore, &type, &have);
    MyWinSemaphoreObj* sem = mywin_find_semaphore(hResolved);
    if (!sem || (type != _OBJECT_TYPE_NONE && type != _OBJECT_TYPE_SEMAPHORE)) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(hSemaphore, have, SEMAPHORE_MODIFY_STATE)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }

    mywin_dispatcher_ensure();
    pthread_mutex_lock(&g_DispatcherLock);
    pthread_mutex_lock(&sem->lock);
    LONG prev = sem->count;
    if (sem->count + lReleaseCount > sem->maxCount) {
        pthread_mutex_unlock(&sem->lock);
        pthread_mutex_unlock(&g_DispatcherLock);
        mywin_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    sem->count += lReleaseCount;
    if (lpPreviousCount) *lpPreviousCount = prev;
    mywin_semaphore_publish_state(sem);
    if (lReleaseCount > 1) pthread_cond_broadcast(&sem->cond);
    else pthread_cond_signal(&sem->cond);
    pthread_mutex_unlock(&sem->lock);
    mywin_dispatcher_signal_object_locked(sem->handle, _OBJECT_TYPE_SEMAPHORE);
    pthread_mutex_unlock(&g_DispatcherLock);
    return TRUE;
}

static MyWinTimerObj* mywin_find_timer(HANDLE h)
{
    DWORD type = 0, slot = 0;
    if (_ObjectDecodeSlotHandle(h, &type, &slot) && type == _OBJECT_TYPE_TIMER) {
        if (slot < MYWIN_MAX_TIMERS && g_Timers[slot].valid && g_Timers[slot].handle == h) return &g_Timers[slot];
        return NULL;
    }
    for (int i = 0; i < MYWIN_MAX_TIMERS; i++)
        if (g_Timers[i].valid && g_Timers[i].handle == h) return &g_Timers[i];
    return NULL;
}

static MyWinTimerObj* mywin_find_timer_slot_payload(HANDLE h, DWORD slot)
{
    if (MYOS_LIKELY(slot < MYWIN_MAX_TIMERS && g_Timers[slot].valid && g_Timers[slot].handle == h)) {
        __atomic_add_fetch(&g_NamedDirectorySlotFastHits, 1u, __ATOMIC_RELAXED);
        return &g_Timers[slot];
    }
    __atomic_add_fetch(&g_NamedDirectorySlotFastMisses, 1u, __ATOMIC_RELAXED);
    return NULL;
}


HANDLE CreateWaitableTimerA(LPSECURITY_ATTRIBUTES lpTimerAttributes, BOOL bManualReset, LPCSTR lpTimerName)
{
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_IPC)) return 0;
    char canonName[96]; DWORD nsId = 0;
    mywin_canonical_name(lpTimerName, canonName, sizeof(canonName), &nsId);
    DWORD dirType = _OBJECT_TYPE_NONE;
    HANDLE dirObj = 0;
    DWORD dirSlot = 0xffffffffu, dirGeneration = 0;
    int dirLookup = mywin_named_directory_fast_lookup_payload(canonName, _OBJECT_TYPE_TIMER, &dirObj, &dirType, &dirSlot, &dirGeneration);
    (void)dirGeneration;
    if (dirLookup == MYWIN_NAMED_DIRECTORY_TYPE_MISMATCH) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    MyWinTimerObj* existing = NULL;
    if (dirObj) {
        existing = mywin_find_timer_slot_payload(dirObj, dirSlot);
        if (!existing) existing = mywin_find_timer(dirObj);
        if (!existing) mywin_named_directory_note_stale();
    }
    if (!existing) existing = mywin_find_timer_by_canon_locked(canonName);
    if (existing) {
        if (!mywin_object_allows_open(existing->handle, TIMER_ALL_ACCESS)) return 0;
        existing->refCount++;
        _ObjectAddRef(existing->handle);
        return mywin_alloc_process_handle(mywin_current_pid(), existing->handle, _OBJECT_TYPE_TIMER,
                                          TIMER_ALL_ACCESS,
                                          lpTimerAttributes ? lpTimerAttributes->bInheritHandle : FALSE);
    }
    _ObjectSecurity objSec; BOOL hasObjSec = FALSE;
    if (!mywin_security_from_attributes(lpTimerAttributes, &objSec, &hasObjSec)) return 0;

    int i = mywin_pop_timer_slot();
    if (i < 0) { mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memset(&g_Timers[i], 0, sizeof(g_Timers[i]));
    g_Timers[i].valid = 1;
    g_Timers[i].handle = _ObjectMakeSlotHandle(_OBJECT_TYPE_TIMER, (DWORD)i);
    g_Timers[i].owner_pid = mywin_current_pid();
    g_Timers[i].access = TIMER_ALL_ACCESS;
    g_Timers[i].refCount = 1;
    g_Timers[i].manualReset = bManualReset ? TRUE : FALSE;
    mywin_dispatcher_header_bind(&g_Timers[i].dispatcher, g_Timers[i].handle, _OBJECT_TYPE_TIMER, 0);
    snprintf(g_Timers[i].name, sizeof(g_Timers[i].name), "%s", canonName[0] ? canonName : "");
    pthread_mutex_init(&g_Timers[i].lock, NULL);
    mywin_waitable_cond_init(&g_Timers[i].cond);
    mywin_timer_hash_insert_locked(i);
    mywin_named_directory_insert(g_Timers[i].name, _OBJECT_TYPE_TIMER, g_Timers[i].handle);
    _ObjectRegister(g_Timers[i].handle, _OBJECT_TYPE_TIMER, g_Timers[i].owner_pid,
                  _OBJECT_ACCESS_READ|_OBJECT_ACCESS_SIGNAL|_OBJECT_ACCESS_CONTROL, 0, g_Timers[i].name);
    mywin_apply_object_security(g_Timers[i].handle, nsId, g_Timers[i].name, &objSec, hasObjSec);
    mywin_timer_publish_state(&g_Timers[i]);
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), g_Timers[i].handle, _OBJECT_TYPE_TIMER,
                                          TIMER_ALL_ACCESS,
                                          lpTimerAttributes ? lpTimerAttributes->bInheritHandle : FALSE);
    if (!h) { mywin_release_object_ref_by_type(g_Timers[i].handle, _OBJECT_TYPE_TIMER); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    return h;
}


BOOL SetWaitableTimer(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod, LPVOID pfnCompletionRoutine, LPVOID lpArgToCompletionRoutine, BOOL fResume)
{
    (void)pfnCompletionRoutine; (void)lpArgToCompletionRoutine; (void)fResume;
    if (!lpDueTime) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD have = 0, type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hTimer, &type, &have);
    MyWinTimerObj* t = mywin_find_timer(hResolved);
    if (!t || (type != _OBJECT_TYPE_NONE && type != _OBJECT_TYPE_TIMER)) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(hTimer, have, TIMER_MODIFY_STATE)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    long long q = lpDueTime->QuadPart;
    unsigned long long now = mywin_now_ms();
    unsigned long long delayMs;
    if (q < 0) {
        long long pos = -q;
        delayMs = (unsigned long long)(pos / 10000ll);
        if (delayMs == 0) delayMs = (unsigned long long)pos;
    } else {
        delayMs = (unsigned long long)q;
    }

    mywin_dispatcher_ensure();
    pthread_mutex_lock(&g_DispatcherLock);
    pthread_mutex_lock(&t->lock);
    t->active = TRUE;
    t->signaled = FALSE;
    t->dueMs = now + delayMs;
    t->periodMs = lPeriod;
    mywin_timer_due_cache_invalidate();
    mywin_timer_due_cache_note_locked(t);
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    mywin_timer_publish_state(t);
    mywin_dispatcher_signal_object_locked(t->handle, _OBJECT_TYPE_TIMER);
    pthread_mutex_unlock(&g_DispatcherLock);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE hTimer)
{
    DWORD have = 0, type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hTimer, &type, &have);
    MyWinTimerObj* t = mywin_find_timer(hResolved);
    if (!t || (type != _OBJECT_TYPE_NONE && type != _OBJECT_TYPE_TIMER)) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_public_access_allowed(hTimer, have, TIMER_MODIFY_STATE)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }

    mywin_dispatcher_ensure();
    pthread_mutex_lock(&g_DispatcherLock);
    pthread_mutex_lock(&t->lock);
    t->active = FALSE;
    t->signaled = FALSE;
    t->periodMs = 0;
    mywin_timer_due_cache_invalidate();
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    mywin_timer_publish_state(t);
    mywin_dispatcher_signal_object_locked(t->handle, _OBJECT_TYPE_TIMER);
    pthread_mutex_unlock(&g_DispatcherLock);
    return TRUE;
}


#define MYWIN_WAIT_PROBE_NOT_READY 0u
#define MYWIN_WAIT_PROBE_READY     1u
#define MYWIN_WAIT_PROBE_ABANDONED 2u

static DWORD mywin_probe_waitable_object_status(HANDLE hResolved, DWORD type, BOOL consume)
{
    if (!hResolved) return MYWIN_WAIT_PROBE_NOT_READY;

    if (type == _OBJECT_TYPE_NONE) {
        if (mywin_find_event(hResolved)) type = _OBJECT_TYPE_EVENT;
        else if (mywin_find_mutex(hResolved)) type = _OBJECT_TYPE_MUTEX;
        else if (mywin_find_semaphore(hResolved)) type = _OBJECT_TYPE_SEMAPHORE;
        else if (mywin_find_timer(hResolved)) type = _OBJECT_TYPE_TIMER;
    }

    if (type == _OBJECT_TYPE_EVENT) {
        MyWinEventObj* ev = mywin_find_event(hResolved);
        if (!ev) return MYWIN_WAIT_PROBE_NOT_READY;
        if (!consume && mywin_dispatcher_header_fast_not_ready(&ev->dispatcher)) return MYWIN_WAIT_PROBE_NOT_READY;
        pthread_mutex_lock(&ev->lock);
        BOOL ready = ev->signaled ? TRUE : FALSE;
        if (ready && consume && !ev->manualReset) {
            ev->signaled = FALSE;
            g_WaitEventConsumes++;
        }
        mywin_event_publish_state(ev);
        pthread_mutex_unlock(&ev->lock);
        return ready ? MYWIN_WAIT_PROBE_READY : MYWIN_WAIT_PROBE_NOT_READY;
    }
    if (type == _OBJECT_TYPE_MUTEX) {
        MyWinMutexObj* m = mywin_find_mutex(hResolved);
        if (!m) return MYWIN_WAIT_PROBE_NOT_READY;
        pthread_mutex_lock(&m->lock);
        BOOL abandoned = m->abandoned ? TRUE : FALSE;
        BOOL ready = (!m->owned || m->owner_thread == GetCurrentThreadId()) ? TRUE : FALSE;
        if (ready && consume) {
            m->owned = TRUE;
            m->owner_thread = GetCurrentThreadId();
            if (abandoned) g_WaitMutexAbandoned++;
            g_WaitMutexAcquires++;
            m->abandoned = FALSE;
        }
        mywin_mutex_publish_state(m);
        pthread_mutex_unlock(&m->lock);
        if (!ready) return MYWIN_WAIT_PROBE_NOT_READY;
        return abandoned ? MYWIN_WAIT_PROBE_ABANDONED : MYWIN_WAIT_PROBE_READY;
    }
    if (type == _OBJECT_TYPE_SEMAPHORE) {
        MyWinSemaphoreObj* sem = mywin_find_semaphore(hResolved);
        if (!sem) return MYWIN_WAIT_PROBE_NOT_READY;
        if (!consume && mywin_dispatcher_header_fast_not_ready(&sem->dispatcher)) return MYWIN_WAIT_PROBE_NOT_READY;
        pthread_mutex_lock(&sem->lock);
        BOOL ready = (sem->count > 0) ? TRUE : FALSE;
        if (ready && consume) {
            sem->count--;
            g_WaitSemaphoreConsumes++;
        }
        mywin_semaphore_publish_state(sem);
        pthread_mutex_unlock(&sem->lock);
        return ready ? MYWIN_WAIT_PROBE_READY : MYWIN_WAIT_PROBE_NOT_READY;
    }
    if (type == _OBJECT_TYPE_TIMER) {
        MyWinTimerObj* t = mywin_find_timer(hResolved);
        if (!t) return MYWIN_WAIT_PROBE_NOT_READY;
        pthread_mutex_lock(&t->lock);
        mywin_timer_update_state_locked(t);
        BOOL ready = t->signaled ? TRUE : FALSE;
        if (ready && consume) {
            g_WaitTimerConsumes++;
            if (t->periodMs > 0) {
                t->signaled = FALSE;
                t->active = TRUE;
                t->dueMs = mywin_now_ms() + (unsigned long long)t->periodMs;
                mywin_timer_due_cache_invalidate();
                mywin_timer_due_cache_note_locked(t);
            } else if (!t->manualReset) {
                t->signaled = FALSE;
            }
        }
        pthread_mutex_unlock(&t->lock);
        mywin_timer_publish_state(t);
        return ready ? MYWIN_WAIT_PROBE_READY : MYWIN_WAIT_PROBE_NOT_READY;
    }
    if (type == _OBJECT_TYPE_PROCESS) {
        _ObjectectInfo oi;
        DWORD pid = (_ObjectGetInfo(hResolved, &oi) && oi.type == _OBJECT_TYPE_PROCESS) ? oi.owner_pid : 0;
        return (pid && mywin_process_is_exited(pid, NULL)) ? MYWIN_WAIT_PROBE_READY : MYWIN_WAIT_PROBE_NOT_READY;
    }
    if (type == _OBJECT_TYPE_THREAD) {
        _ObjectectInfo oi;
        DWORD tid = (_ObjectGetInfo(hResolved, &oi) && oi.type == _OBJECT_TYPE_THREAD) ? oi.owner_pid : 0;
        return (tid && mywin_process_is_exited(tid, NULL)) ? MYWIN_WAIT_PROBE_READY : MYWIN_WAIT_PROBE_NOT_READY;
    }
    return MYWIN_WAIT_PROBE_NOT_READY;
}

static DWORD mywin_probe_waitable_status(HANDLE hHandle, BOOL consume)
{
    DWORD type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hHandle, &type, NULL);
    if (!hResolved) return MYWIN_WAIT_PROBE_NOT_READY;
    return mywin_probe_waitable_object_status(hResolved, type, consume);
}

static BOOL mywin_validate_waitable_handle(HANDLE hHandle, DWORD* lpError)
{
    if (lpError) *lpError = ERROR_INVALID_HANDLE;
    DWORD type = 0, have = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hHandle, &type, &have);
    if (!hResolved) return FALSE;

    BOOL waitable = (type == _OBJECT_TYPE_PROCESS || type == _OBJECT_TYPE_THREAD ||
                     type == _OBJECT_TYPE_EVENT || type == _OBJECT_TYPE_MUTEX ||
                     type == _OBJECT_TYPE_SEMAPHORE || type == _OBJECT_TYPE_TIMER ||
                     mywin_find_event(hResolved) || mywin_find_mutex(hResolved) ||
                     mywin_find_semaphore(hResolved) || mywin_find_timer(hResolved));
    if (!waitable) return FALSE;

    if (!mywin_public_access_allowed(hHandle, have, SYNCHRONIZE)) {
        if (lpError) *lpError = ERROR_ACCESS_DENIED;
        return FALSE;
    }
    if (lpError) *lpError = ERROR_SUCCESS;
    return TRUE;
}



static unsigned long long mywin_wait_target_ms(DWORD nCount, const HANDLE* lpHandles, DWORD dwMilliseconds, unsigned long long startMs);
static unsigned long long mywin_wait_target_ms_resolved(DWORD nCount, const DWORD* targetTypes, DWORD dwMilliseconds, unsigned long long startMs);

typedef struct MyWinWaitTargetInfo {
    HANDLE objectHandle;
    DWORD type;
} MyWinWaitTargetInfo;

static BOOL __attribute__((unused)) mywin_wait_target_resolve_one(HANDLE publicHandle, MyWinWaitTargetInfo* out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    DWORD type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(publicHandle, &type, NULL);
    if (!hResolved) return FALSE;
    if (type == _OBJECT_TYPE_NONE) {
        if (mywin_find_event(hResolved)) type = _OBJECT_TYPE_EVENT;
        else if (mywin_find_mutex(hResolved)) type = _OBJECT_TYPE_MUTEX;
        else if (mywin_find_semaphore(hResolved)) type = _OBJECT_TYPE_SEMAPHORE;
        else if (mywin_find_timer(hResolved)) type = _OBJECT_TYPE_TIMER;
        else {
            _ObjectectInfo oi;
            if (_ObjectGetInfo(hResolved, &oi)) type = oi.type;
        }
    }
    out->objectHandle = hResolved;
    out->type = type;
    return TRUE;
}

static BOOL mywin_wait_type_is_targetable(DWORD type)
{
    return (type == _OBJECT_TYPE_EVENT || type == _OBJECT_TYPE_MUTEX ||
            type == _OBJECT_TYPE_SEMAPHORE || type == _OBJECT_TYPE_TIMER ||
            type == _OBJECT_TYPE_PROCESS || type == _OBJECT_TYPE_THREAD) ? TRUE : FALSE;
}

static BOOL __attribute__((unused)) mywin_wait_multi_can_target(DWORD nCount, const HANDLE* lpHandles,
                                        HANDLE* objects, DWORD* types)
{
    if (!lpHandles || !objects || !types || nCount == 0 || nCount > MAXIMUM_WAIT_OBJECTS) return FALSE;
    for (DWORD i = 0; i < nCount; ++i) {
        MyWinWaitTargetInfo ti;
        if (!mywin_wait_target_resolve_one(lpHandles[i], &ti)) return FALSE;
        if (!mywin_wait_type_is_targetable(ti.type)) return FALSE;
        objects[i] = ti.objectHandle;
        types[i] = ti.type;
    }
    return TRUE;
}

static BOOL mywin_wait_multi_prevalidate_resolve(DWORD nCount, const HANDLE* lpHandles,
                                                 HANDLE* objects, DWORD* types,
                                                 BOOL* useTargetedOut, DWORD* errorOut)
{
    if (useTargetedOut) *useTargetedOut = FALSE;
    if (errorOut) *errorOut = ERROR_INVALID_PARAMETER;
    if (!lpHandles || !objects || !types || nCount == 0 || nCount > MAXIMUM_WAIT_OBJECTS) return FALSE;

    BOOL allTargetable = TRUE;
    for (DWORD i = 0; i < nCount; ++i) {
        DWORD type = 0, have = 0;
        HANDLE hResolved = mywin_resolve_handle_public(lpHandles[i], &type, &have);
        g_WaitMultiplePrevalidateResolves++;
        if (!hResolved) { if (errorOut) *errorOut = ERROR_INVALID_HANDLE; return FALSE; }

        if (type == _OBJECT_TYPE_NONE) {
            if (mywin_find_event(hResolved)) type = _OBJECT_TYPE_EVENT;
            else if (mywin_find_mutex(hResolved)) type = _OBJECT_TYPE_MUTEX;
            else if (mywin_find_semaphore(hResolved)) type = _OBJECT_TYPE_SEMAPHORE;
            else if (mywin_find_timer(hResolved)) type = _OBJECT_TYPE_TIMER;
            else {
                _ObjectectInfo oi;
                if (_ObjectGetInfo(hResolved, &oi)) type = oi.type;
            }
            g_WaitMultiplePrevalidateFallbacks++;
        }

        if (!mywin_wait_type_is_targetable(type)) { if (errorOut) *errorOut = ERROR_INVALID_HANDLE; return FALSE; }
        if (!mywin_public_access_allowed(lpHandles[i], have, SYNCHRONIZE)) {
            if (errorOut) *errorOut = ERROR_ACCESS_DENIED;
            return FALSE;
        }
        objects[i] = hResolved;
        types[i] = type;
        if (!mywin_wait_type_is_targetable(type)) allTargetable = FALSE;
    }
    if (useTargetedOut) *useTargetedOut = allTargetable;
    if (errorOut) *errorOut = ERROR_SUCCESS;
    g_WaitMultiplePrevalidated++;
    return TRUE;
}

static inline DWORD mywin_probe_waitable_index_status(DWORD i, BOOL useTargeted,
                                                     const HANDLE* lpHandles,
                                                     const HANDLE* targetObjects,
                                                     const DWORD* targetTypes,
                                                     BOOL consume)
{
    if (useTargeted) {
        g_WaitMultipleResolvedProbes++;
        return mywin_probe_waitable_object_status(targetObjects[i], targetTypes[i], consume);
    }
    return mywin_probe_waitable_status(lpHandles[i], consume);
}

static void mywin_dispatcher_wait_multi_targeted_locked(MyWinMultiWaiter* waiter,
                                                        DWORD nCount,
                                                        const DWORD* targetTypes,
                                                        DWORD dwMilliseconds,
                                                        unsigned long long startMs)
{
    (void)waiter;
    unsigned long long target = mywin_wait_target_ms_resolved(nCount, targetTypes, dwMilliseconds, startMs);
    if (target == 0) {
        pthread_cond_wait(waiter->cond, &g_DispatcherLock);
    } else {
        struct timespec ts;
        mywin_abs_mono_timespec_from_ms(target, &ts);
        (void)pthread_cond_timedwait(waiter->cond, &g_DispatcherLock, &ts);
    }
}

static BOOL mywin_next_timer_due_ms(unsigned long long* lpDueMs)
{
    unsigned long long now = mywin_now_ms();
    if (g_TimerNextDueCacheValid && g_TimerNextDueCacheMs > now) {
        if (lpDueMs) *lpDueMs = g_TimerNextDueCacheMs;
        return TRUE;
    }

    BOOL found = FALSE;
    unsigned long long best = 0;
    mywin_timer_due_cache_invalidate();
    for (int i = 0; i < MYWIN_MAX_TIMERS; i++) {
        MyWinTimerObj* t = &g_Timers[i];
        if (!t->valid) continue;
        pthread_mutex_lock(&t->lock);
        mywin_timer_update_state_locked(t);
        if (t->valid && t->active && !t->signaled) {
            if (!found || t->dueMs < best) { best = t->dueMs; found = TRUE; }
            mywin_timer_due_cache_note_locked(t);
        }
        pthread_mutex_unlock(&t->lock);
    }
    if (found && lpDueMs) *lpDueMs = best;
    return found;
}

static BOOL mywin_wait_targets_have_process_or_thread(DWORD nCount, const DWORD* targetTypes)
{
    if (!targetTypes) return FALSE;
    for (DWORD i = 0; i < nCount; ++i)
        if (mywin_wait_type_is_process_or_thread(targetTypes[i])) return TRUE;
    return FALSE;
}

static BOOL __attribute__((unused)) mywin_wait_targets_are_native_only(DWORD nCount, const DWORD* targetTypes)
{
    if (!targetTypes || nCount == 0) return FALSE;
    for (DWORD i = 0; i < nCount; ++i) {
        if (targetTypes[i] != _OBJECT_TYPE_EVENT && targetTypes[i] != _OBJECT_TYPE_MUTEX &&
            targetTypes[i] != _OBJECT_TYPE_SEMAPHORE && targetTypes[i] != _OBJECT_TYPE_TIMER)
            return FALSE;
    }
    return TRUE;
}

static BOOL mywin_waitables_have_process_or_thread(DWORD nCount, const HANDLE* lpHandles)
{
    if (!lpHandles) return FALSE;
    for (DWORD i = 0; i < nCount; i++) {
        DWORD type = 0;
        HANDLE hResolved = mywin_resolve_handle_public(lpHandles[i], &type, NULL);
        if (!hResolved) continue;
        if (mywin_wait_type_is_process_or_thread(type)) return TRUE;
    }
    return FALSE;
}

static unsigned long long mywin_wait_target_ms_ex(DWORD nCount, const HANDLE* lpHandles, const DWORD* targetTypes, DWORD dwMilliseconds, unsigned long long startMs)
{
    unsigned long long now = mywin_now_ms();
    unsigned long long target = 0;
    BOOL haveTarget = FALSE;

    if (dwMilliseconds != INFINITE) {
        target = startMs + (unsigned long long)dwMilliseconds;
        haveTarget = TRUE;
    }

    unsigned long long timerDue = 0;
    if (mywin_next_timer_due_ms(&timerDue)) {
        if (!haveTarget || timerDue < target) { target = timerDue; haveTarget = TRUE; }
    }

    /* ProcessHost/Linux child exit currently has no async kernel callback into
       this facade.  Use a bounded condvar timed wait for process/thread handles
       so WaitForSingleObject(hProcess, INFINITE) still reaps without usleep spin. */
    BOOL hasProcessOrThread = targetTypes ? mywin_wait_targets_have_process_or_thread(nCount, targetTypes)
                                          : mywin_waitables_have_process_or_thread(nCount, lpHandles);
    if (hasProcessOrThread && !MyProcessHostAsyncReaperActive()) {
        unsigned long long pollDue = now + 25ull;
        if (!haveTarget || pollDue < target) { target = pollDue; haveTarget = TRUE; }
        g_WaitProcessThreadPollSlices++;
    }

    if (!haveTarget) return 0;
    if (target < now) target = now;
    return target;
}

static unsigned long long mywin_wait_target_ms(DWORD nCount, const HANDLE* lpHandles, DWORD dwMilliseconds, unsigned long long startMs)
{
    return mywin_wait_target_ms_ex(nCount, lpHandles, NULL, dwMilliseconds, startMs);
}

static unsigned long long mywin_wait_target_ms_resolved(DWORD nCount, const DWORD* targetTypes, DWORD dwMilliseconds, unsigned long long startMs)
{
    return mywin_wait_target_ms_ex(nCount, NULL, targetTypes, dwMilliseconds, startMs);
}

static void mywin_dispatcher_wait_locked(DWORD nCount, const HANDLE* lpHandles, DWORD dwMilliseconds, unsigned long long startMs)
{
    unsigned long long target = mywin_wait_target_ms(nCount, lpHandles, dwMilliseconds, startMs);
    g_DispatcherWaiters++;
    if (target == 0) {
        pthread_cond_wait(&g_DispatcherCond, &g_DispatcherLock);
    } else {
        struct timespec ts;
        mywin_abs_mono_timespec_from_ms(target, &ts);
        (void)pthread_cond_timedwait(&g_DispatcherCond, &g_DispatcherLock, &ts);
    }
    if (g_DispatcherWaiters) g_DispatcherWaiters--;
}

static void mywin_wait_note_validation_failure(DWORD waitError)
{
    g_WaitFailures++;
    if (waitError == ERROR_ACCESS_DENIED) g_WaitAccessDenied++;
    else g_WaitInvalidHandle++;
}

static BOOL mywin_wait_timeout_elapsed(unsigned long long startMs, DWORD dwMilliseconds)
{
    return (dwMilliseconds != INFINITE && mywin_now_ms() - startMs >= (unsigned long long)dwMilliseconds) ? TRUE : FALSE;
}

static void mywin_wait_deadline_timespec(unsigned long long startMs, DWORD dwMilliseconds, struct timespec* ts)
{
    unsigned long long target = startMs + (unsigned long long)dwMilliseconds;
    mywin_abs_mono_timespec_from_ms(target, ts);
}

static DWORD mywin_wait_single_event_targeted(MyWinEventObj* ev, DWORD dwMilliseconds, unsigned long long startMs)
{
    if (!ev) return WAIT_FAILED;
    g_WaitSingleTargeted++;
    pthread_mutex_lock(&ev->lock);
    for (;;) {
        if (ev->signaled) {
            if (!ev->manualReset) {
                ev->signaled = FALSE;
                g_WaitEventConsumes++;
            }
            mywin_event_publish_state(ev);
            pthread_mutex_unlock(&ev->lock);
            g_WaitSuccess++;
            return WAIT_OBJECT_0;
        }
        if (dwMilliseconds == 0 || mywin_wait_timeout_elapsed(startMs, dwMilliseconds)) {
            pthread_mutex_unlock(&ev->lock);
            g_WaitTimeouts++;
            return WAIT_TIMEOUT;
        }
        if (dwMilliseconds == INFINITE) {
            pthread_cond_wait(&ev->cond, &ev->lock);
        } else {
            struct timespec ts;
            mywin_wait_deadline_timespec(startMs, dwMilliseconds, &ts);
            (void)pthread_cond_timedwait(&ev->cond, &ev->lock, &ts);
        }
    }
}

static DWORD mywin_wait_single_mutex_targeted(MyWinMutexObj* m, DWORD dwMilliseconds, unsigned long long startMs)
{
    if (!m) return WAIT_FAILED;
    g_WaitSingleTargeted++;
    DWORD self = GetCurrentThreadId();
    pthread_mutex_lock(&m->lock);
    for (;;) {
        BOOL abandoned = m->abandoned ? TRUE : FALSE;
        BOOL ready = (!m->owned || m->owner_thread == self) ? TRUE : FALSE;
        if (ready) {
            m->owned = TRUE;
            m->owner_thread = self;
            if (abandoned) g_WaitMutexAbandoned++;
            g_WaitMutexAcquires++;
            m->abandoned = FALSE;
            mywin_mutex_publish_state(m);
            pthread_mutex_unlock(&m->lock);
            g_WaitSuccess++;
            return abandoned ? WAIT_ABANDONED : WAIT_OBJECT_0;
        }
        if (dwMilliseconds == 0 || mywin_wait_timeout_elapsed(startMs, dwMilliseconds)) {
            pthread_mutex_unlock(&m->lock);
            g_WaitTimeouts++;
            return WAIT_TIMEOUT;
        }
        if (dwMilliseconds == INFINITE) {
            pthread_cond_wait(&m->cond, &m->lock);
        } else {
            struct timespec ts;
            mywin_wait_deadline_timespec(startMs, dwMilliseconds, &ts);
            (void)pthread_cond_timedwait(&m->cond, &m->lock, &ts);
        }
    }
}

static DWORD mywin_wait_single_semaphore_targeted(MyWinSemaphoreObj* sem, DWORD dwMilliseconds, unsigned long long startMs)
{
    if (!sem) return WAIT_FAILED;
    g_WaitSingleTargeted++;
    pthread_mutex_lock(&sem->lock);
    for (;;) {
        if (sem->count > 0) {
            sem->count--;
            g_WaitSemaphoreConsumes++;
            mywin_semaphore_publish_state(sem);
            pthread_mutex_unlock(&sem->lock);
            g_WaitSuccess++;
            return WAIT_OBJECT_0;
        }
        if (dwMilliseconds == 0 || mywin_wait_timeout_elapsed(startMs, dwMilliseconds)) {
            pthread_mutex_unlock(&sem->lock);
            g_WaitTimeouts++;
            return WAIT_TIMEOUT;
        }
        if (dwMilliseconds == INFINITE) {
            pthread_cond_wait(&sem->cond, &sem->lock);
        } else {
            struct timespec ts;
            mywin_wait_deadline_timespec(startMs, dwMilliseconds, &ts);
            (void)pthread_cond_timedwait(&sem->cond, &sem->lock, &ts);
        }
    }
}

static DWORD mywin_wait_single_timer_targeted(MyWinTimerObj* t, DWORD dwMilliseconds, unsigned long long startMs)
{
    if (!t) return WAIT_FAILED;
    g_WaitSingleTargeted++;
    pthread_mutex_lock(&t->lock);
    for (;;) {
        mywin_timer_update_state_locked(t);
        if (t->signaled) {
            g_WaitTimerConsumes++;
            if (t->periodMs > 0) {
                t->signaled = FALSE;
                t->active = TRUE;
                t->dueMs = mywin_now_ms() + (unsigned long long)t->periodMs;
                mywin_timer_due_cache_invalidate();
                mywin_timer_due_cache_note_locked(t);
            } else if (!t->manualReset) {
                t->signaled = FALSE;
            }
            pthread_mutex_unlock(&t->lock);
            mywin_timer_publish_state(t);
            g_WaitSuccess++;
            return WAIT_OBJECT_0;
        }
        if (dwMilliseconds == 0 || mywin_wait_timeout_elapsed(startMs, dwMilliseconds)) {
            pthread_mutex_unlock(&t->lock);
            g_WaitTimeouts++;
            return WAIT_TIMEOUT;
        }

        unsigned long long target = 0;
        if (dwMilliseconds != INFINITE) target = startMs + (unsigned long long)dwMilliseconds;
        if (t->active && !t->signaled) {
            if (target == 0 || t->dueMs < target) target = t->dueMs;
        }
        if (target == 0) {
            pthread_cond_wait(&t->cond, &t->lock);
        } else {
            struct timespec ts;
            mywin_abs_mono_timespec_from_ms(target, &ts);
            (void)pthread_cond_timedwait(&t->cond, &t->lock, &ts);
        }
    }
}

static DWORD mywin_wait_single_process_thread_targeted(HANDLE objectHandle, DWORD type, DWORD dwMilliseconds, unsigned long long startMs)
{
    g_WaitSingleTargeted++;
    g_WaitProcessThreadTargeted++;

    /* v248: if a PROCESS/THREAD object is already signaled, return before
       allocating/initializing a stack WaitBlock gate.  Non-ready process waits
       still register eagerly to avoid races with the current try-lock wake path. */
    DWORD immediate = mywin_probe_waitable_object_status(objectHandle, type, TRUE);
    g_WaitMultipleResolvedProbes++;
    if (immediate != MYWIN_WAIT_PROBE_NOT_READY) {
        g_WaitProcessThreadImmediateHits++;
        g_WaitSuccess++;
        return (immediate == MYWIN_WAIT_PROBE_ABANDONED) ? WAIT_ABANDONED : WAIT_OBJECT_0;
    }
    if (dwMilliseconds == 0 || mywin_wait_timeout_elapsed(startMs, dwMilliseconds)) {
        g_WaitTimeouts++;
        return WAIT_TIMEOUT;
    }

    const HANDLE objects[1] = { objectHandle };
    const DWORD types[1] = { type };
    MyWinMultiWaiter waiter;
    BOOL waiterRegistered = FALSE;
    BOOL waiterInitialized = FALSE;
    DWORD result = WAIT_FAILED;

    mywin_dispatcher_ensure();
    pthread_mutex_lock(&g_DispatcherLock);
    mywin_multi_waiter_register_locked(&waiter, 1, objects, types);
    waiterRegistered = TRUE;
    waiterInitialized = TRUE;
    for (;;) {
        DWORD st = mywin_probe_waitable_object_status(objectHandle, type, TRUE);
        g_WaitMultipleResolvedProbes++;
        if (st != MYWIN_WAIT_PROBE_NOT_READY) {
            g_WaitSuccess++;
            result = (st == MYWIN_WAIT_PROBE_ABANDONED) ? WAIT_ABANDONED : WAIT_OBJECT_0;
            break;
        }
        if (dwMilliseconds == 0 || mywin_wait_timeout_elapsed(startMs, dwMilliseconds)) {
            g_WaitTimeouts++;
            result = WAIT_TIMEOUT;
            break;
        }
        mywin_dispatcher_wait_multi_targeted_locked(&waiter, 1, types, dwMilliseconds, startMs);
    }
    if (waiterRegistered) mywin_multi_waiter_unregister_locked(&waiter);
    pthread_mutex_unlock(&g_DispatcherLock);
    if (waiterInitialized) mywin_multi_waiter_destroy(&waiter);
    return result;
}

static DWORD mywin_wait_single_targeted_if_possible(HANDLE hHandle, DWORD dwMilliseconds, unsigned long long startMs, BOOL* used)
{
    if (used) *used = FALSE;
    DWORD type = 0;
    HANDLE hResolved = mywin_resolve_handle_public(hHandle, &type, NULL);
    if (!hResolved) return WAIT_FAILED;
    if (type == _OBJECT_TYPE_EVENT || type == _OBJECT_TYPE_NONE) {
        MyWinEventObj* ev = mywin_find_event(hResolved);
        if (ev) { if (used) *used = TRUE; return mywin_wait_single_event_targeted(ev, dwMilliseconds, startMs); }
    }
    if (type == _OBJECT_TYPE_MUTEX || type == _OBJECT_TYPE_NONE) {
        MyWinMutexObj* m = mywin_find_mutex(hResolved);
        if (m) { if (used) *used = TRUE; return mywin_wait_single_mutex_targeted(m, dwMilliseconds, startMs); }
    }
    if (type == _OBJECT_TYPE_SEMAPHORE || type == _OBJECT_TYPE_NONE) {
        MyWinSemaphoreObj* sem = mywin_find_semaphore(hResolved);
        if (sem) { if (used) *used = TRUE; return mywin_wait_single_semaphore_targeted(sem, dwMilliseconds, startMs); }
    }
    if (type == _OBJECT_TYPE_TIMER || type == _OBJECT_TYPE_NONE) {
        MyWinTimerObj* t = mywin_find_timer(hResolved);
        if (t) { if (used) *used = TRUE; return mywin_wait_single_timer_targeted(t, dwMilliseconds, startMs); }
    }
    if (type == _OBJECT_TYPE_PROCESS || type == _OBJECT_TYPE_THREAD) {
        if (mywin_process_thread_wait_head_for_object_locked(hResolved, type)) {
            if (used) *used = TRUE;
            return mywin_wait_single_process_thread_targeted(hResolved, type, dwMilliseconds, startMs);
        }
    }
    return WAIT_FAILED;
}

DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    g_WaitSingleCalls++;
    DWORD waitError = ERROR_SUCCESS;
    if (!mywin_validate_waitable_handle(hHandle, &waitError)) {
        mywin_wait_note_validation_failure(waitError ? waitError : ERROR_INVALID_HANDLE);
        mywin_set_last_error(waitError ? waitError : ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }

    unsigned long long start = mywin_now_ms();
    BOOL usedTargeted = FALSE;
    DWORD targeted = mywin_wait_single_targeted_if_possible(hHandle, dwMilliseconds, start, &usedTargeted);
    if (usedTargeted) return targeted;

    /* PROCESS/THREAD handles and any future dispatcher-only waitable keep the
       conservative global dispatcher path. */
    g_WaitSingleGlobalFallback++;
    const HANDLE handles[1] = { hHandle };
    mywin_dispatcher_ensure();
    pthread_mutex_lock(&g_DispatcherLock);
    for (;;) {
        DWORD st = mywin_probe_waitable_status(hHandle, TRUE);
        if (st != MYWIN_WAIT_PROBE_NOT_READY) {
            pthread_mutex_unlock(&g_DispatcherLock);
            g_WaitSuccess++;
            return (st == MYWIN_WAIT_PROBE_ABANDONED) ? WAIT_ABANDONED : WAIT_OBJECT_0;
        }
        if (dwMilliseconds == 0) {
            pthread_mutex_unlock(&g_DispatcherLock);
            g_WaitTimeouts++;
            return WAIT_TIMEOUT;
        }
        if (dwMilliseconds != INFINITE && mywin_now_ms() - start >= dwMilliseconds) {
            pthread_mutex_unlock(&g_DispatcherLock);
            g_WaitTimeouts++;
            return WAIT_TIMEOUT;
        }
        mywin_dispatcher_wait_locked(1, handles, dwMilliseconds, start);
    }
}

DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds)
{
    /* v247: native waitables plus process/thread handles now park stack
       waiters through intrusive per-object WaitBlocks.  Linux-backed process
       waits still keep a short poll deadline because ProcessHost has no async
       kernel callback into this userspace facade yet. */
    g_WaitMultipleCalls++;
    if (bWaitAll) g_WaitAllCalls++; else g_WaitAnyCalls++;
    if (!lpHandles || nCount == 0 || nCount > MAXIMUM_WAIT_OBJECTS) {
        g_WaitFailures++;
        g_WaitInvalidHandle++;
        mywin_set_last_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    HANDLE targetObjects[MAXIMUM_WAIT_OBJECTS];
    DWORD targetTypes[MAXIMUM_WAIT_OBJECTS];
    DWORD waitError = ERROR_SUCCESS;
    BOOL useTargeted = FALSE;
    if (!mywin_wait_multi_prevalidate_resolve(nCount, lpHandles, targetObjects, targetTypes, &useTargeted, &waitError)) {
        mywin_wait_note_validation_failure(waitError ? waitError : ERROR_INVALID_HANDLE);
        mywin_set_last_error(waitError ? waitError : ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    BOOL targetHasProcessThread = useTargeted ? mywin_wait_targets_have_process_or_thread(nCount, targetTypes) : FALSE;
    if (useTargeted) {
        g_WaitMultipleTargeted++;
        if (targetHasProcessThread) g_WaitProcessThreadTargeted++;
    } else {
        g_WaitMultipleGlobalFallback++;
    }

    unsigned long long start = mywin_now_ms();
    mywin_dispatcher_ensure();
    MyWinMultiWaiter waiter;
    BOOL waiterRegistered = FALSE;
    BOOL waiterInitialized = FALSE;
    DWORD result = WAIT_FAILED;

    /* v250: delay WaitBlock linking for every targeted set, not only native
       Event/Mutex/Semaphore/Timer sets.  While g_DispatcherLock is held, a
       process/thread signal can set object state, but its object wake cannot
       walk the wait list until we either commit immediately or link blocks. */
    BOOL delayTargetRegistration = (useTargeted && dwMilliseconds != 0);

    pthread_mutex_lock(&g_DispatcherLock);
    if (useTargeted && dwMilliseconds != 0 && !delayTargetRegistration) {
        mywin_multi_waiter_register_locked(&waiter, nCount, targetObjects, targetTypes);
        waiterRegistered = TRUE;
        waiterInitialized = TRUE;
    }

    for (;;) {
        if (bWaitAll) {
            DWORD ready = 0;
            DWORD firstAbandoned = 0xffffffffu;
            for (DWORD i = 0; i < nCount; i++) {
                DWORD st = mywin_probe_waitable_index_status(i, useTargeted, lpHandles, targetObjects, targetTypes, FALSE);
                if (st != MYWIN_WAIT_PROBE_NOT_READY) {
                    ready++;
                    if (st == MYWIN_WAIT_PROBE_ABANDONED && firstAbandoned == 0xffffffffu) firstAbandoned = i;
                }
            }
            if (ready == nCount) {
                for (DWORD i = 0; i < nCount; i++) {
                    DWORD st = mywin_probe_waitable_index_status(i, useTargeted, lpHandles, targetObjects, targetTypes, TRUE);
                    if (st == MYWIN_WAIT_PROBE_ABANDONED && firstAbandoned == 0xffffffffu) firstAbandoned = i;
                }
                if (useTargeted && !waiterRegistered) {
                    g_WaitMultipleImmediateHits++;
                    if (targetHasProcessThread) g_WaitProcessThreadImmediateHits++;
                }
                g_WaitSuccess++;
                g_WaitAllCommits++;
                result = (firstAbandoned != 0xffffffffu) ? (WAIT_ABANDONED + firstAbandoned) : WAIT_OBJECT_0;
                break;
            }
        } else {
            for (DWORD i = 0; i < nCount; i++) {
                DWORD st = mywin_probe_waitable_index_status(i, useTargeted, lpHandles, targetObjects, targetTypes, FALSE);
                if (st != MYWIN_WAIT_PROBE_NOT_READY) {
                    st = mywin_probe_waitable_index_status(i, useTargeted, lpHandles, targetObjects, targetTypes, TRUE);
                    if (useTargeted && !waiterRegistered) {
                        g_WaitMultipleImmediateHits++;
                        if (mywin_wait_type_is_process_or_thread(targetTypes[i])) g_WaitProcessThreadImmediateHits++;
                    }
                    g_WaitSuccess++;
                    g_WaitAnyCommits++;
                    result = (st == MYWIN_WAIT_PROBE_ABANDONED) ? (WAIT_ABANDONED + i) : (WAIT_OBJECT_0 + i);
                    goto done_locked;
                }
            }
        }

        if (dwMilliseconds == 0) {
            g_WaitTimeouts++;
            result = WAIT_TIMEOUT;
            break;
        }
        if (dwMilliseconds != INFINITE && mywin_now_ms() - start >= dwMilliseconds) {
            g_WaitTimeouts++;
            result = WAIT_TIMEOUT;
            break;
        }
        if (useTargeted) {
            if (!waiterRegistered) {
                mywin_multi_waiter_register_locked(&waiter, nCount, targetObjects, targetTypes);
                waiterRegistered = TRUE;
                waiterInitialized = TRUE;
                g_WaitMultipleDeferredLinks++;
            }
            mywin_dispatcher_wait_multi_targeted_locked(&waiter, nCount, targetTypes, dwMilliseconds, start);
        } else {
            mywin_dispatcher_wait_locked(nCount, lpHandles, dwMilliseconds, start);
        }
    }

done_locked:
    if (waiterRegistered) {
        mywin_multi_waiter_unregister_locked(&waiter);
        waiterRegistered = FALSE;
    }
    pthread_mutex_unlock(&g_DispatcherLock);
    if (waiterInitialized) mywin_multi_waiter_destroy(&waiter);
    return result;
}

HANDLE OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId)
{
    if (!dwProcessId) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    HANDLE obj = mywin_process_object_for_pid(dwProcessId);
    _ObjectectInfo oi;
    if (!_ObjectGetInfo(obj, &oi) || oi.type != _OBJECT_TYPE_PROCESS) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD desired = dwDesiredAccess ? dwDesiredAccess : PROCESS_QUERY_LIMITED_INFORMATION;
    if (!mywin_object_allows_open(obj, desired)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    _ObjectAddRef(obj);
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), obj, _OBJECT_TYPE_PROCESS,
                                          mywin_expand_generic_access(_OBJECT_TYPE_PROCESS, desired),
                                          bInheritHandle);
    if (!h) { _ObjectRelease(obj); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    mywin_set_last_error(ERROR_SUCCESS);
    return h;
}

HANDLE OpenThread(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId)
{
    if (!dwThreadId) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    HANDLE obj = mywin_thread_object_for_tid(dwThreadId);
    _ObjectectInfo oi;
    if (!_ObjectGetInfo(obj, &oi) || oi.type != _OBJECT_TYPE_THREAD) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD desired = dwDesiredAccess ? dwDesiredAccess : THREAD_QUERY_INFORMATION;
    if (!mywin_object_allows_open(obj, desired)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    _ObjectAddRef(obj);
    HANDLE h = mywin_alloc_process_handle(mywin_current_pid(), obj, _OBJECT_TYPE_THREAD,
                                          mywin_expand_generic_access(_OBJECT_TYPE_THREAD, desired),
                                          bInheritHandle);
    if (!h) { _ObjectRelease(obj); mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    mywin_set_last_error(ERROR_SUCCESS);
    return h;
}


static BOOL mywin_create_process_core(LPCSTR lpApplicationName, LPSTR lpCommandLine,
                                      const Capability* lpChildCapability,
                                      BOOL bInheritHandles,
                                      LPCSTR lpCurrentDirectory,
                                      LPSTARTUPINFOA lpStartupInfo,
                                      LPVOID lpEnvironment,
                                      LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                      LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                      LPPROCESS_INFORMATION lpProcessInformation)
{
    if (!g_HasCapability || !(g_CurrentCapability.flags & CAP_EXEC)) { mywin_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    if (!lpProcessInformation) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(lpProcessInformation, 0, sizeof(*lpProcessInformation));

    _ObjectSecurity procSec; BOOL hasProcSec = FALSE;
    _ObjectSecurity threadSec; BOOL hasThreadSec = FALSE;
    if (!mywin_security_from_attributes(lpProcessAttributes, &procSec, &hasProcSec)) return FALSE;
    if (!mywin_security_from_attributes(lpThreadAttributes, &threadSec, &hasThreadSec)) return FALSE;

    BOOL bProcessHandleInherit = lpProcessAttributes ? lpProcessAttributes->bInheritHandle : FALSE;
    BOOL bThreadHandleInherit = lpThreadAttributes ? lpThreadAttributes->bInheritHandle : FALSE;

    DWORD parentPid = mywin_current_pid();
    DWORD childPid = 0;
    DWORD processSlot = 0xffffffffu;
    pthread_mutex_lock(&g_ProcessLock);
    childPid = g_NextLitePid++;
    MyWinProcessLite* reservedProcess = mywin_alloc_lite_process_locked(childPid);
    if (reservedProcess) processSlot = (DWORD)(reservedProcess - g_LiteProcesses);
    pthread_mutex_unlock(&g_ProcessLock);
    if (processSlot >= MYWIN_MAX_LITE_PROCESSES || processSlot >= _OBJECT_SLOT_STRIDE) {
        mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    const char* img = lpApplicationName && lpApplicationName[0] ? lpApplicationName :
                      (lpCommandLine && lpCommandLine[0] ? lpCommandLine : "myos-child-lite");
    DWORD childTid = childPid;
    HANDLE procObj = _ObjectMakeSlotHandle(_OBJECT_TYPE_PROCESS, processSlot);
    HANDLE threadObj = _ObjectMakeSlotHandle(_OBJECT_TYPE_THREAD, processSlot);
    char name[96];
    snprintf(name, sizeof(name), "%s", img);
    _ObjectRegister(procObj, _OBJECT_TYPE_PROCESS, childPid, _OBJECT_ACCESS_READ|_OBJECT_ACCESS_CONTROL|_OBJECT_ACCESS_SIGNAL, 0, name);
    _ObjectSetInfo(procObj, 0, STILL_ACTIVE, name);
    char threadName[96];
    snprintf(threadName, sizeof(threadName), "%s!main", img);
    _ObjectRegister(threadObj, _OBJECT_TYPE_THREAD, childPid, _OBJECT_ACCESS_READ|_OBJECT_ACCESS_CONTROL|_OBJECT_ACCESS_SIGNAL, 0, threadName);
    _ObjectSetInfo(threadObj, 0, STILL_ACTIVE, threadName);

    if (!mywin_apply_token_default_object_security(procObj, &procSec, hasProcSec) ||
        !mywin_apply_token_default_object_security(threadObj, &threadSec, hasThreadSec)) {
        _ObjectUnregister(procObj);
        _ObjectUnregister(threadObj);
        mywin_set_last_error(ERROR_INVALID_SECURITY_DESCR);
        return FALSE;
    }

    // v50: Process-Lite keeps CreateProcess/AppHost startup metadata plus a
    // per-process environment block.  lpEnvironment now behaves like a small
    // Win32 MULTI_SZ environment handoff; NULL inherits from the parent.
    mywin_note_lite_process_ex(childPid, parentPid, childTid, procObj, threadObj,
                               img, lpChildCapability, lpCommandLine,
                               lpCurrentDirectory, lpStartupInfo, lpEnvironment);

    if (lpStartupInfo && (lpStartupInfo->dwFlags & STARTF_USESTDHANDLES)) {
        DWORD stdErr = ERROR_SUCCESS;
        HANDLE childStdIn = 0, childStdOut = 0, childStdErr = 0;
        if (lpStartupInfo->hStdInput) {
            childStdIn = mywin_duplicate_handle_to_pid(parentPid, childPid, lpStartupInfo->hStdInput, TRUE, &stdErr);
            if (!childStdIn) { _ObjectUnregister(procObj); _ObjectUnregister(threadObj); mywin_set_last_error(stdErr); return FALSE; }
        }
        if (lpStartupInfo->hStdOutput) {
            childStdOut = mywin_duplicate_handle_to_pid(parentPid, childPid, lpStartupInfo->hStdOutput, TRUE, &stdErr);
            if (!childStdOut) { if (childStdIn) { HANDLE co=0; DWORD ct=0, ce=0; mywin_close_process_handle_ex(childPid, childStdIn, FALSE, &co, &ct, &ce); mywin_release_object_ref_by_type(co, ct); } _ObjectUnregister(procObj); _ObjectUnregister(threadObj); mywin_set_last_error(stdErr); return FALSE; }
        }
        if (lpStartupInfo->hStdError) {
            childStdErr = mywin_duplicate_handle_to_pid(parentPid, childPid, lpStartupInfo->hStdError, TRUE, &stdErr);
            if (!childStdErr) {
                if (childStdIn) { HANDLE co=0; DWORD ct=0, ce=0; mywin_close_process_handle_ex(childPid, childStdIn, FALSE, &co, &ct, &ce); mywin_release_object_ref_by_type(co, ct); }
                if (childStdOut) { HANDLE co=0; DWORD ct=0, ce=0; mywin_close_process_handle_ex(childPid, childStdOut, FALSE, &co, &ct, &ce); mywin_release_object_ref_by_type(co, ct); }
                _ObjectUnregister(procObj); _ObjectUnregister(threadObj); mywin_set_last_error(stdErr); return FALSE;
            }
        }
        pthread_mutex_lock(&g_ProcessLock);
        MyWinProcessLite* cp = mywin_find_lite_process_locked(childPid);
        if (cp) { cp->stdInput = childStdIn; cp->stdOutput = childStdOut; cp->stdError = childStdErr; }
        pthread_mutex_unlock(&g_ProcessLock);
    }

    DWORD inherited = 0;
    if (bInheritHandles) {
        DWORD n = 0;
        MyWinHandleEntry* todo = NULL;
        pthread_mutex_lock(&g_HandleLock);
        todo = mywin_snapshot_handles_locked(parentPid, &n);
        pthread_mutex_unlock(&g_HandleLock);
        for (DWORD i=0; todo && i<n; i++) {
            if (!todo[i].inherit) continue;
            HANDLE ch = mywin_alloc_process_handle(childPid, todo[i].objectHandle, todo[i].type, todo[i].access, todo[i].inherit);
            if (ch) { mywin_add_object_ref_by_type(todo[i].objectHandle, todo[i].type); inherited++; }
        }
        free(todo);
        mywin_process_note_inherited(childPid, inherited);
    }

    _ObjectAddRef(procObj);
    HANDLE parentHandle = mywin_alloc_process_handle(parentPid, procObj, _OBJECT_TYPE_PROCESS,
                                                     PROCESS_ALL_ACCESS,
                                                     bProcessHandleInherit);
    _ObjectAddRef(threadObj);
    HANDLE parentThreadHandle = mywin_alloc_process_handle(parentPid, threadObj, _OBJECT_TYPE_THREAD,
                                                           THREAD_ALL_ACCESS,
                                                           bThreadHandleInherit);
    if (!parentHandle || !parentThreadHandle) {
        if (parentHandle) CloseHandle(parentHandle);
        if (parentThreadHandle) CloseHandle(parentThreadHandle);
        mywin_mark_process_exited(childPid, ERROR_NOT_ENOUGH_MEMORY, 0);
        mywin_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    mywin_set_last_error(ERROR_SUCCESS);
    lpProcessInformation->hProcess = parentHandle;
    lpProcessInformation->hThread = parentThreadHandle;
    lpProcessInformation->dwProcessId = childPid;
    lpProcessInformation->dwThreadId = childTid;
    return TRUE;
}

BOOL MyWinCreateProcessWithStartupCapability(LPCSTR lpApplicationName, LPSTR lpCommandLine,
                                             const Capability* lpChildCapability,
                                             BOOL bInheritHandles,
                                             LPCSTR lpCurrentDirectory,
                                             LPSTARTUPINFOA lpStartupInfo,
                                             LPPROCESS_INFORMATION lpProcessInformation)
{
    if (!lpChildCapability) return FALSE;
    return mywin_create_process_core(lpApplicationName, lpCommandLine, lpChildCapability,
                                     bInheritHandles, lpCurrentDirectory, lpStartupInfo,
                                     NULL, NULL, NULL, lpProcessInformation);
}

BOOL MyWinCreateProcessWithCapability(LPCSTR lpApplicationName, LPSTR lpCommandLine,
                                      const Capability* lpChildCapability,
                                      BOOL bInheritHandles,
                                      LPPROCESS_INFORMATION lpProcessInformation)
{
    return MyWinCreateProcessWithStartupCapability(lpApplicationName, lpCommandLine,
                                                   lpChildCapability, bInheritHandles,
                                                   NULL, NULL, lpProcessInformation);
}

BOOL CreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine,
                    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
                    LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
                    LPPROCESS_INFORMATION lpProcessInformation)
{
    (void)dwCreationFlags;
    return mywin_create_process_core(lpApplicationName, lpCommandLine, NULL,
                                     bInheritHandles, lpCurrentDirectory, lpStartupInfo,
                                     lpEnvironment, lpProcessAttributes, lpThreadAttributes,
                                     lpProcessInformation);
}

BOOL MyGetProcessLiteInfo(DWORD dwProcessId, MyProcessLiteInfo* lpInfo)
{
    if (!lpInfo) return FALSE;
    memset(lpInfo, 0, sizeof(*lpInfo));
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(dwProcessId);
    if (p) {
        lpInfo->pid = p->pid;
        lpInfo->parent_pid = p->parentPid;
        lpInfo->thread_id = p->tid;
        lpInfo->process_object = p->processObject;
        lpInfo->thread_object = p->threadObject;
        lpInfo->flags = p->flags;
        lpInfo->exit_code = p->exitCode;
        lpInfo->inherited_handles = p->inheritedHandles;
        lpInfo->duplicated_in = p->duplicatedIn;
        lpInfo->cap_flags = p->hasCap ? p->cap.flags : 0;
        lpInfo->runtime_enters = p->runtimeEnters;
        lpInfo->runtime_depth = (DWORD)g_RuntimeDepth;
        lpInfo->startup_flags = p->startupFlags;
        lpInfo->startup_x = p->startupX;
        lpInfo->startup_y = p->startupY;
        lpInfo->startup_w = p->startupW;
        lpInfo->startup_h = p->startupH;
        lpInfo->show_window = p->showWindow;
        lpInfo->std_input = p->stdInput;
        lpInfo->std_output = p->stdOutput;
        lpInfo->std_error = p->stdError;
        lpInfo->handle_count = mywin_process_handle_count(p->pid);
        snprintf(lpInfo->image_name, sizeof(lpInfo->image_name), "%s", p->imageName);
        snprintf(lpInfo->image_path, sizeof(lpInfo->image_path), "%s", p->imagePath);
        snprintf(lpInfo->module_name, sizeof(lpInfo->module_name), "%s", p->moduleName);
        lpInfo->main_module = p->mainModule;
        snprintf(lpInfo->cap_name, sizeof(lpInfo->cap_name), "%s", p->hasCap ? p->cap.name : "");
        snprintf(lpInfo->command_line, sizeof(lpInfo->command_line), "%s", p->commandLine);
        snprintf(lpInfo->current_directory, sizeof(lpInfo->current_directory), "%s", p->currentDirectory);
        snprintf(lpInfo->window_title, sizeof(lpInfo->window_title), "%s", p->windowTitle);
        lpInfo->environment_count = p->environmentCount;
        mywin_env_preview_locked(p, lpInfo->environment_preview, sizeof(lpInfo->environment_preview));
        lpInfo->dll_count = mywin_loaded_module_count_locked(p);
        mywin_dll_preview_locked(p, lpInfo->dll_preview, sizeof(lpInfo->dll_preview));
        snprintf(lpInfo->dll_directory, sizeof(lpInfo->dll_directory), "%s", p->dllDirectory);
        lpInfo->last_error = p->lastError;
        lpInfo->loader_import_count = p->loaderImportCount;
        lpInfo->loader_resolved_count = p->loaderResolvedCount;
        lpInfo->loader_entry_called = p->loaderEntryCalled;
        lpInfo->loader_error = p->loaderError;
        snprintf(lpInfo->loader_entry, sizeof(lpInfo->loader_entry), "%s", p->loaderEntry);
        snprintf(lpInfo->loader_import_preview, sizeof(lpInfo->loader_import_preview), "%s", p->loaderImportPreview);
        snprintf(lpInfo->subsystem, sizeof(lpInfo->subsystem), "%s", p->subsystem);
        lpInfo->argc = p->argc;
        snprintf(lpInfo->argv_preview, sizeof(lpInfo->argv_preview), "%s", p->argvPreview);
        lpInfo->console_exit_code = p->consoleExitCode;
        lpInfo->linux_pid = p->linuxPid;
        lpInfo->linux_status = p->linuxStatus;
        lpInfo->fork_exec = p->forkExec;
        DWORD pid = p->pid;
        pthread_mutex_unlock(&g_ProcessLock);
        MyProcessHostInfo phi;
        if (MyProcessHostGetInfo(pid, &phi)) {
            lpInfo->process_host_state = phi.state;
            lpInfo->process_host_polls = phi.poll_count;
            lpInfo->process_host_reaps = phi.reap_count;
            lpInfo->process_host_kills = phi.kill_count;
            lpInfo->process_host_start_ms = phi.start_ms;
            lpInfo->process_host_exit_ms = phi.exit_ms;
            snprintf(lpInfo->process_host_state_name, sizeof(lpInfo->process_host_state_name), "%s", phi.state_name);
            snprintf(lpInfo->process_host_last_event, sizeof(lpInfo->process_host_last_event), "%s", phi.last_event);
            lpInfo->ipc_enabled = phi.ipc_enabled;
            lpInfo->ipc_messages = phi.ipc_messages;
            lpInfo->ipc_hello = phi.ipc_hello;
            lpInfo->ipc_exit_report = phi.ipc_exit_report;
            lpInfo->ipc_last_opcode = phi.ipc_last_opcode;
            lpInfo->ipc_last_value = phi.ipc_last_value;
            snprintf(lpInfo->ipc_last_text, sizeof(lpInfo->ipc_last_text), "%s", phi.ipc_last_text);
            snprintf(lpInfo->ipc_shared_name, sizeof(lpInfo->ipc_shared_name), "%s", phi.shared_name);
            lpInfo->ipc_shared_heartbeat = phi.shared_heartbeat;
            lpInfo->ipc_shared_child_pid = phi.shared_child_pid;
            lpInfo->ipc_shared_argc = phi.shared_argc;
            lpInfo->ipc_shared_exit_code = phi.shared_exit_code;
            snprintf(lpInfo->ipc_shared_status, sizeof(lpInfo->ipc_shared_status), "%s", phi.shared_status);
            snprintf(lpInfo->ipc_shared_argv_preview, sizeof(lpInfo->ipc_shared_argv_preview), "%s", phi.shared_argv_preview);
        }
        return TRUE;
    }
    pthread_mutex_unlock(&g_ProcessLock);
    return FALSE;
}

BOOL MyEnumProcessLite(MYPROCESSLITEENUMPROC lpEnumFunc, LPARAM lParam)
{
    if (!lpEnumFunc) return FALSE;
    MyProcessLiteInfo snap[MYWIN_MAX_LITE_PROCESSES];
    int n = 0;
    pthread_mutex_lock(&g_ProcessLock);
    for (int i=0;i<MYWIN_MAX_LITE_PROCESSES && n<MYWIN_MAX_LITE_PROCESSES;i++) {
        if (!g_LiteProcesses[i].valid) continue;
        memset(&snap[n], 0, sizeof(snap[n]));
        snap[n].pid = g_LiteProcesses[i].pid;
        snap[n].parent_pid = g_LiteProcesses[i].parentPid;
        snap[n].thread_id = g_LiteProcesses[i].tid;
        snap[n].process_object = g_LiteProcesses[i].processObject;
        snap[n].thread_object = g_LiteProcesses[i].threadObject;
        snap[n].flags = g_LiteProcesses[i].flags;
        snap[n].exit_code = g_LiteProcesses[i].exitCode;
        snap[n].inherited_handles = g_LiteProcesses[i].inheritedHandles;
        snap[n].duplicated_in = g_LiteProcesses[i].duplicatedIn;
        snap[n].cap_flags = g_LiteProcesses[i].hasCap ? g_LiteProcesses[i].cap.flags : 0;
        snap[n].runtime_enters = g_LiteProcesses[i].runtimeEnters;
        snap[n].runtime_depth = (DWORD)g_RuntimeDepth;
        snap[n].startup_flags = g_LiteProcesses[i].startupFlags;
        snap[n].startup_x = g_LiteProcesses[i].startupX;
        snap[n].startup_y = g_LiteProcesses[i].startupY;
        snap[n].startup_w = g_LiteProcesses[i].startupW;
        snap[n].startup_h = g_LiteProcesses[i].startupH;
        snap[n].show_window = g_LiteProcesses[i].showWindow;
        snap[n].std_input = g_LiteProcesses[i].stdInput;
        snap[n].std_output = g_LiteProcesses[i].stdOutput;
        snap[n].std_error = g_LiteProcesses[i].stdError;
        snprintf(snap[n].image_name, sizeof(snap[n].image_name), "%s", g_LiteProcesses[i].imageName);
        snprintf(snap[n].image_path, sizeof(snap[n].image_path), "%s", g_LiteProcesses[i].imagePath);
        snprintf(snap[n].module_name, sizeof(snap[n].module_name), "%s", g_LiteProcesses[i].moduleName);
        snap[n].main_module = g_LiteProcesses[i].mainModule;
        snprintf(snap[n].cap_name, sizeof(snap[n].cap_name), "%s", g_LiteProcesses[i].hasCap ? g_LiteProcesses[i].cap.name : "");
        snprintf(snap[n].command_line, sizeof(snap[n].command_line), "%s", g_LiteProcesses[i].commandLine);
        snprintf(snap[n].current_directory, sizeof(snap[n].current_directory), "%s", g_LiteProcesses[i].currentDirectory);
        snprintf(snap[n].window_title, sizeof(snap[n].window_title), "%s", g_LiteProcesses[i].windowTitle);
        snap[n].environment_count = g_LiteProcesses[i].environmentCount;
        mywin_env_preview_locked(&g_LiteProcesses[i], snap[n].environment_preview, sizeof(snap[n].environment_preview));
        snap[n].dll_count = mywin_loaded_module_count_locked(&g_LiteProcesses[i]);
        mywin_dll_preview_locked(&g_LiteProcesses[i], snap[n].dll_preview, sizeof(snap[n].dll_preview));
        snprintf(snap[n].dll_directory, sizeof(snap[n].dll_directory), "%s", g_LiteProcesses[i].dllDirectory);
        snap[n].last_error = g_LiteProcesses[i].lastError;
        snap[n].loader_import_count = g_LiteProcesses[i].loaderImportCount;
        snap[n].loader_resolved_count = g_LiteProcesses[i].loaderResolvedCount;
        snap[n].loader_entry_called = g_LiteProcesses[i].loaderEntryCalled;
        snap[n].loader_error = g_LiteProcesses[i].loaderError;
        snprintf(snap[n].loader_entry, sizeof(snap[n].loader_entry), "%s", g_LiteProcesses[i].loaderEntry);
        snprintf(snap[n].loader_import_preview, sizeof(snap[n].loader_import_preview), "%s", g_LiteProcesses[i].loaderImportPreview);
        snprintf(snap[n].subsystem, sizeof(snap[n].subsystem), "%s", g_LiteProcesses[i].subsystem);
        snap[n].argc = g_LiteProcesses[i].argc;
        snprintf(snap[n].argv_preview, sizeof(snap[n].argv_preview), "%s", g_LiteProcesses[i].argvPreview);
        snap[n].console_exit_code = g_LiteProcesses[i].consoleExitCode;
        snap[n].linux_pid = g_LiteProcesses[i].linuxPid;
        snap[n].linux_status = g_LiteProcesses[i].linuxStatus;
        snap[n].fork_exec = g_LiteProcesses[i].forkExec;
        n++;
    }
    pthread_mutex_unlock(&g_ProcessLock);
    for (int i=0;i<n;i++) {
        snap[i].handle_count = mywin_process_handle_count(snap[i].pid);
        MyProcessHostInfo phi;
        if (MyProcessHostGetInfo(snap[i].pid, &phi)) {
            snap[i].process_host_state = phi.state;
            snap[i].process_host_polls = phi.poll_count;
            snap[i].process_host_reaps = phi.reap_count;
            snap[i].process_host_kills = phi.kill_count;
            snap[i].process_host_start_ms = phi.start_ms;
            snap[i].process_host_exit_ms = phi.exit_ms;
            snprintf(snap[i].process_host_state_name, sizeof(snap[i].process_host_state_name), "%s", phi.state_name);
            snprintf(snap[i].process_host_last_event, sizeof(snap[i].process_host_last_event), "%s", phi.last_event);
            snap[i].ipc_enabled = phi.ipc_enabled;
            snap[i].ipc_messages = phi.ipc_messages;
            snap[i].ipc_hello = phi.ipc_hello;
            snap[i].ipc_exit_report = phi.ipc_exit_report;
            snap[i].ipc_last_opcode = phi.ipc_last_opcode;
            snap[i].ipc_last_value = phi.ipc_last_value;
            snprintf(snap[i].ipc_last_text, sizeof(snap[i].ipc_last_text), "%s", phi.ipc_last_text);
            snprintf(snap[i].ipc_shared_name, sizeof(snap[i].ipc_shared_name), "%s", phi.shared_name);
            snap[i].ipc_shared_heartbeat = phi.shared_heartbeat;
            snap[i].ipc_shared_child_pid = phi.shared_child_pid;
            snap[i].ipc_shared_argc = phi.shared_argc;
            snap[i].ipc_shared_exit_code = phi.shared_exit_code;
            snprintf(snap[i].ipc_shared_status, sizeof(snap[i].ipc_shared_status), "%s", phi.shared_status);
            snprintf(snap[i].ipc_shared_argv_preview, sizeof(snap[i].ipc_shared_argv_preview), "%s", phi.shared_argv_preview);
        }
        if (!lpEnumFunc(&snap[i], lParam)) break;
    }
    return TRUE;
}


static BOOL mywin_resolve_current_pseudo_handle(HANDLE h, HANDLE* objectHandle, DWORD* type, DWORD* access)
{
    if (objectHandle) *objectHandle = 0;
    if (type) *type = _OBJECT_TYPE_NONE;
    if (access) *access = 0;

    DWORD objType = _OBJECT_TYPE_NONE;
    DWORD pid = mywin_current_pid();
    DWORD tid = GetCurrentThreadId();
    HANDLE obj = 0;

    if (h == GetCurrentProcess()) {
        if (!pid) return FALSE;
        objType = _OBJECT_TYPE_PROCESS;
        obj = mywin_process_object_for_pid(pid);
    } else if (h == GetCurrentThread()) {
        if (!tid) return FALSE;
        objType = _OBJECT_TYPE_THREAD;
        obj = mywin_thread_object_for_tid(tid);
    } else {
        return FALSE;
    }

    _ObjectectInfo oi;
    if (!_ObjectGetInfo(obj, &oi) || oi.type != objType) return FALSE;
    if (objectHandle) *objectHandle = obj;
    if (type) *type = objType;
    if (access) *access = mywin_object_generic_all(objType);
    return TRUE;
}


BOOL GetHandleInformation(HANDLE hObject, LPDWORD lpdwFlags)
{
    if (!lpdwFlags) { mywin_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *lpdwFlags = 0;
    if (!hObject || hObject == GetCurrentProcess() || hObject == GetCurrentThread()) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    DWORD pid = mywin_current_pid();
    MyWinHandlePidTable* table = mywin_get_handle_table_ref(pid, FALSE);
    if (!table) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    mywin_pushlock_acquire_shared(&table->lock);
    MyWinHandleEntry* e = mywin_find_handle_in_table_locked(table, pid, hObject);
    if (!e) { mywin_pushlock_release(&table->lock); mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    DWORD flags = 0;
    if (e->inherit) flags |= HANDLE_FLAG_INHERIT;
    if (e->protect_from_close) flags |= HANDLE_FLAG_PROTECT_FROM_CLOSE;
    mywin_pushlock_release(&table->lock);

    *lpdwFlags = flags;
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags)
{
    if (!hObject || hObject == GetCurrentProcess() || hObject == GetCurrentThread()) {
        mywin_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    const DWORD supported = HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE;
    DWORD mask = dwMask & supported;
    DWORD flags = dwFlags & supported;

    DWORD pid = mywin_current_pid();
    MyWinHandlePidTable* table = mywin_get_handle_table_ref(pid, FALSE);
    if (!table) { mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    mywin_pushlock_acquire_exclusive(&table->lock);
    MyWinHandleEntry* e = mywin_find_handle_in_table_locked(table, pid, hObject);
    if (!e) { mywin_pushlock_release(&table->lock); mywin_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }

    if (mask & HANDLE_FLAG_INHERIT)
        e->inherit = (flags & HANDLE_FLAG_INHERIT) ? TRUE : FALSE;
    if (mask & HANDLE_FLAG_PROTECT_FROM_CLOSE)
        e->protect_from_close = (flags & HANDLE_FLAG_PROTECT_FROM_CLOSE) ? TRUE : FALSE;

    mywin_pushlock_release(&table->lock);
    mywin_handle_cache_invalidate_table(table);
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

/* v186: DuplicateHandle is now strict cross-process table semantics:
   source/target process handles must name real live process handle tables,
   access masks are subset-checked, DUPLICATE_CLOSE_SOURCE closes the source
   table entry even when source and target are different processes, and no raw
   Object Manager handles are accepted while strict public handles are enabled. */
BOOL DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                     HANDLE* lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions)
{
    if (!lpTargetHandle) { mywin_duplicate_note_failure(ERROR_INVALID_PARAMETER); return FALSE; }
    *lpTargetHandle = 0;
    if (!hSourceHandle) { mywin_duplicate_note_failure(ERROR_INVALID_HANDLE); return FALSE; }

    DWORD currentPid = mywin_current_pid();
    DWORD srcPid = mywin_process_id_from_handle(hSourceProcessHandle);
    DWORD dstPid = mywin_process_id_from_handle(hTargetProcessHandle);
    if (!srcPid) srcPid = currentPid;
    if (!dstPid) dstPid = currentPid;

    if (hSourceProcessHandle && hSourceProcessHandle != GetCurrentProcess() &&
        !mywin_has_handle_access(hSourceProcessHandle, PROCESS_DUP_HANDLE, NULL)) {
        mywin_duplicate_note_failure(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (hTargetProcessHandle && hTargetProcessHandle != GetCurrentProcess() &&
        !mywin_has_handle_access(hTargetProcessHandle, PROCESS_DUP_HANDLE, NULL)) {
        mywin_duplicate_note_failure(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    if (srcPid != currentPid) {
        BOOL srcExists = FALSE;
        BOOL srcLive = mywin_lite_process_is_live_snapshot(srcPid, &srcExists);
        if (!srcExists) { mywin_duplicate_note_failure(ERROR_INVALID_PARAMETER); return FALSE; }
        if (!srcLive) { mywin_duplicate_note_failure(ERROR_INVALID_HANDLE); return FALSE; }
    }
    if (dstPid != currentPid) {
        BOOL dstExists = FALSE;
        BOOL dstLive = mywin_lite_process_is_live_snapshot(dstPid, &dstExists);
        if (!dstExists || !dstLive) { mywin_duplicate_note_failure(ERROR_INVALID_PARAMETER); return FALSE; }
    }

    DWORD srcType = 0, srcAccess = 0, srcObjectSlot = 0xffffffffu;
    HANDLE hObject = 0;
    BOOL srcIsPseudo = FALSE;
    BOOL srcProtected = FALSE;
    if (srcPid == currentPid && mywin_resolve_current_pseudo_handle(hSourceHandle, &hObject, &srcType, &srcAccess)) {
        /* v206/v207: Win32 pseudo handles are not table entries; DuplicateHandle
           materializes them into a real per-process handle with full access.
           DUPLICATE_CLOSE_SOURCE has nothing to close for a pseudo source. */
        srcIsPseudo = TRUE;
    } else {
        MyWinHandlePidTable* srcTable = NULL;
        /* v242: repeated same-process DuplicateHandle() calls usually duplicate
           one stable source handle into many targets.  When DUPLICATE_CLOSE_SOURCE
           is not requested, protect-from-close is irrelevant, so the existing
           per-thread handle lookup cache can satisfy the source preflight without
           taking the table pushlock on every iteration. */
        if (srcPid == currentPid && !(dwOptions & DUPLICATE_CLOSE_SOURCE)) {
            if (mywin_handle_cache_lookup_ex(srcPid, hSourceHandle, &srcType, &srcObjectSlot, &srcAccess, &hObject))
                srcProtected = FALSE;
        }
        if (!hObject) {
            srcTable = mywin_get_handle_table_ref(srcPid, FALSE);
            if (srcTable) {
                mywin_pushlock_acquire_shared(&srcTable->lock);
                MyWinHandleEntry* src = mywin_find_handle_in_table_locked(srcTable, srcPid, hSourceHandle);
                if (src) {
                    hObject = src->objectHandle;
                    srcType = src->type;
                    srcObjectSlot = src->object_slot;
                    srcAccess = src->access;
                    srcProtected = src->protect_from_close ? TRUE : FALSE;
                }
                mywin_pushlock_release(&srcTable->lock);
                if (hObject) mywin_handle_cache_store_ex(srcTable, srcPid, hSourceHandle, hObject, srcType, srcObjectSlot, srcAccess);
            }
        }
        if (!hObject && !g_StrictKernelHandles) { hObject = mywin_resolve_handle(hSourceHandle, &srcType, &srcAccess); srcObjectSlot = mywin_cached_object_slot(hObject, srcType); }
    }
    if (!hObject) { mywin_duplicate_note_failure(ERROR_INVALID_HANDLE); return FALSE; }
    if (srcObjectSlot == 0xffffffffu) srcObjectSlot = mywin_cached_object_slot(hObject, srcType);
    if ((dwOptions & DUPLICATE_CLOSE_SOURCE) && srcProtected) {
        mywin_duplicate_note_failure(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    DWORD targetAccess = (dwOptions & DUPLICATE_SAME_ACCESS) ? srcAccess : dwDesiredAccess;
    if (!targetAccess) targetAccess = srcAccess;
    if (!(dwOptions & DUPLICATE_SAME_ACCESS) && !mywin_public_access_allowed(hSourceHandle, srcAccess, targetAccess)) {
        mywin_duplicate_note_failure(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    if (srcType != _OBJECT_TYPE_SECTION && srcType != _OBJECT_TYPE_EVENT &&
        srcType != _OBJECT_TYPE_PROCESS && srcType != _OBJECT_TYPE_THREAD &&
        srcType != _OBJECT_TYPE_MUTEX && srcType != _OBJECT_TYPE_SEMAPHORE &&
        srcType != _OBJECT_TYPE_TIMER && srcType != _OBJECT_TYPE_TOKEN) {
        mywin_duplicate_note_failure(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    HANDLE nh = mywin_alloc_process_handle(dstPid, hObject, srcType, targetAccess, bInheritHandle);
    if (!nh) { mywin_duplicate_note_failure(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    mywin_add_object_ref_by_slot(hObject, srcType, srcObjectSlot);
    if (dstPid != currentPid) mywin_process_note_duplicated_in(dstPid);
    *lpTargetHandle = nh;

    g_HandleDuplicateSuccess++;
    if (srcPid != dstPid) g_HandleDuplicateCrossProcess++;

    if ((dwOptions & DUPLICATE_CLOSE_SOURCE) && !srcIsPseudo) {
        HANDLE closedObject = 0;
        DWORD closedType = _OBJECT_TYPE_NONE;
        DWORD closeErr = ERROR_SUCCESS;
        if (!mywin_close_process_handle_ex(srcPid, hSourceHandle, TRUE, &closedObject, &closedType, &closeErr)) {
            /* Should be unreachable because the source was preflighted. Keep
               the target handle valid, but audit the invariant violation. */
            g_HandleDuplicateFailures++;
            if (closeErr == ERROR_ACCESS_DENIED) g_HandleDuplicateAccessDenied++;
            g_HandleSweepFailures++;
        } else {
            mywin_release_object_ref_by_type(closedObject, closedType);
            g_HandleDuplicateCloseSource++;
        }
    }

    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

static DWORD mywin_close_all_handles_for_pid(DWORD pid)
{
    DWORD n = 0;
    MyWinHandleEntry* snap = NULL;
    if (!pid) return 0;
    pthread_mutex_lock(&g_HandleLock);
    g_HandleSweepCalls++;
    g_HandleSweepLastPid = pid;
    MyWinHandlePidTable* table = mywin_find_handle_table_locked(pid, FALSE);
    if (table) mywin_pushlock_acquire_exclusive(&table->lock);
    snap = mywin_snapshot_handles_locked(pid, &n);
    if (table) {
        for (DWORD a = 0; a < MYWIN_HANDLE_INDEX_COUNT; ++a) {
            MyWinHandleMidPage* mid = table->mid[a];
            if (!mid) continue;
            for (DWORD b = 0; b < MYWIN_HANDLE_INDEX_COUNT; ++b) {
                MyWinHandleLeafPage* leaf = mid->leaf[b];
                if (!leaf) continue;
                memset(leaf->free_mark, 0, sizeof(leaf->free_mark));
                for (DWORD c = 0; c < MYWIN_HANDLE_INDEX_COUNT; ++c) {
                    MyWinHandleEntry* e = &leaf->entry[c];
                    if (!e->valid || e->pid != pid) continue;
                    DWORD slot = e->slot;
                    DWORD generation = e->generation;
                    memset(e, 0, sizeof(*e));
                    e->slot = slot;
                    e->generation = generation;
                }
            }
        }
        table->count = 0;
        table->alloc_hint = 1;
        table->free_count = 0;
        if (g_HandleFreeHintTable == table) { g_HandleFreeHintTable = NULL; g_HandleFreeHintSlot = 0; }
        mywin_handle_free_batch_drop_tls_table(table);
        mywin_pushlock_release(&table->lock);
    }
    g_HandleSweepClosed += n;
    pthread_mutex_unlock(&g_HandleLock);
    if (n && table) mywin_handle_cache_invalidate_table(table);

    /* v185/v205: process exit owns the whole per-process handle table. Sweep
       the table first, then release object references outside the table lock so
       destructors may take their own locks without lock-order inversions. */
    for (DWORD i=0; snap && i<n; i++) {
        if (!snap[i].objectHandle || snap[i].type == _OBJECT_TYPE_NONE) {
            g_HandleSweepFailures++;
            continue;
        }
        mywin_release_object_ref_by_type(snap[i].objectHandle, snap[i].type);
    }
    free(snap);
    return n;
}

void ExitProcess(UINT uExitCode)
{
    (void)TerminateProcess(GetCurrentProcess(), uExitCode);
}

BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode)
{
    DWORD pid = mywin_process_id_from_handle(hProcess);
    if (!pid) return FALSE;
    if (hProcess && hProcess != GetCurrentProcess() && !mywin_has_handle_access(hProcess, PROCESS_TERMINATE, NULL)) return FALSE;

    int osPid = 0;
    DWORD alreadyExited = 0;
    pthread_mutex_lock(&g_ProcessLock);
    MyWinProcessLite* p = mywin_find_lite_process_locked(pid);
    if (p) { osPid = p->linuxPid; alreadyExited = (p->flags & MYWIN_PROCESS_EXITED) ? 1u : 0u; }
    pthread_mutex_unlock(&g_ProcessLock);

    DWORD rawStatus = 0;
    if (osPid > 0 && !alreadyExited) {
        (void)MyProcessHostTerminate(pid, &rawStatus);
    }

    mywin_mark_process_exited(pid, (DWORD)uExitCode, rawStatus);
    return TRUE;
}

BOOL GetExitCodeProcess(HANDLE hProcess, DWORD* lpExitCode)
{
    if (!lpExitCode) return FALSE;
    DWORD pid = mywin_process_id_from_handle(hProcess);
    if (!pid) return FALSE;
    if (hProcess && hProcess != GetCurrentProcess() && !mywin_has_handle_access(hProcess, PROCESS_QUERY_LIMITED_INFORMATION, NULL) && !mywin_has_handle_access(hProcess, PROCESS_QUERY_INFORMATION, NULL)) return FALSE;
    DWORD code = STILL_ACTIVE;
    mywin_process_is_exited(pid, &code);
    *lpExitCode = code;
    return TRUE;
}

BOOL CloseHandle(HANDLE hObject)
{
    /* Win32 current-thread pseudo handle is not a table entry.  The current-process
       pseudo handle shares the INVALID_HANDLE_VALUE bit pattern, so CloseHandle
       deliberately keeps the invalid-handle failure path for 0xffffffff. */
    if (hObject == GetCurrentThread()) { mywin_set_last_error(ERROR_SUCCESS); return TRUE; }

    /* v147: public CloseHandle is strict-table only.  Raw Object Manager handles
       are no longer accepted by the public KERNEL32 facade. */
    HANDLE objectHandle = 0;
    DWORD type = 0;
    DWORD closeErr = ERROR_INVALID_HANDLE;
    BOOL hadProcessHandle = mywin_close_process_handle_ex(mywin_current_pid(), hObject, TRUE, &objectHandle, &type, &closeErr);
    if (!hadProcessHandle && closeErr == ERROR_INVALID_HANDLE && !g_StrictKernelHandles) objectHandle = mywin_resolve_handle(hObject, &type, NULL);
    if (!objectHandle) { mywin_set_last_error(closeErr); return FALSE; }

    mywin_release_object_ref_by_type(objectHandle, type);
    mywin_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

DWORD MyGetSectionCount(void)
{
    DWORD n = 0;
    for (int i = 0; i < MYWIN_MAX_SECTIONS; i++) if (g_Sections[i].valid) n++;
    return n;
}

DWORD MyGetMappedViewCount(void)
{
    DWORD n = 0;
    for (int i = 0; i < MYWIN_MAX_VIEWS; i++) if (g_Views[i].valid) n++;
    return n;
}


DWORD MyGetObjectCount(void)
{
    return _ObjectGetCount();
}

DWORD MyGetObjectCountByType(DWORD dwType)
{
    return _ObjectGetCountByType(dwType);
}

DWORD MyGetProcessLiveCount(void)
{
    DWORD n = 0;
    MyWinPollAllProcesses();
    pthread_mutex_lock(&g_ProcessLock);
    for (int i = 0; i < MYWIN_MAX_LITE_PROCESSES; i++)
        if (g_LiteProcesses[i].valid && !(g_LiteProcesses[i].flags & MYWIN_PROCESS_EXITED)) n++;
    pthread_mutex_unlock(&g_ProcessLock);
    return n;
}

DWORD MyGetProcessExitedCount(void)
{
    DWORD n = 0;
    MyWinPollAllProcesses();
    pthread_mutex_lock(&g_ProcessLock);
    for (int i = 0; i < MYWIN_MAX_LITE_PROCESSES; i++)
        if (g_LiteProcesses[i].valid && (g_LiteProcesses[i].flags & MYWIN_PROCESS_EXITED)) n++;
    pthread_mutex_unlock(&g_ProcessLock);
    return n;
}

BOOL MyEnumObjects(MYOBJECTENUMPROC lpEnumFunc, LPARAM lParam)
{
    return _ObjectEnumObjects(lpEnumFunc, lParam);
}

BOOL MyGetObjectInfo(HANDLE hObject, _ObjectectInfo* lpInfo)
{
    HANDLE hResolved = mywin_resolve_handle(hObject, NULL, NULL);
    return _ObjectGetInfo(hResolved, lpInfo);
}

const char* MyGetObjectTypeName(DWORD dwType)
{
    return _ObjectTypeName(dwType);
}

// ─────────────────────────────────────────────
// v33 USER32-lite: HGLOBAL + Clipboard + Menu + Accelerator
// ─────────────────────────────────────────────
