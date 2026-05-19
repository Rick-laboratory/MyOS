#pragma once
/*
 * myOS diagnostics/inspection surface.
 *
 * These APIs are intentionally NOT part of the public Win32/MSDN SDK facade.
 * They back Spy/Object/Wait/Service labs and internal smoke coverage.
 */
#include <windows.h>
#include "myobject.h"
#include "hwnd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MYWIN_MAX_ENV_VARS        24u
#define MYWIN_MAX_ENV_NAME        32u
#define MYWIN_MAX_ENV_VALUE       96u
#define MYWIN_MAX_MODULE_PATH     160u
#define MYWIN_MAX_MODULE_NAME     64u
#define MYWIN_MAX_LOADED_MODULES  16u
#define MYWIN_MAX_LOADER_PREVIEW  160u
#define MYWIN_MAX_LOADER_ENTRY    64u
#define MYWIN_MAX_ARGV_PREVIEW    160u
#define MYWIN_MAX_SUBSYSTEM       16u

#define MYWIN_MAX_HANDLE_ROWS     128
#define MYWIN_HANDLE_FLAG_INHERIT 0x00000001u
#define MYWIN_HANDLE_FLAG_PROTECT_FROM_CLOSE 0x00000002u

#define MYWIN_PROCESS_LIVE        0x00000001u
#define MYWIN_PROCESS_EXITED      0x00000002u

#define MYWIN_NS_LOCAL_SESSION    1u
#define MYWIN_NS_GLOBAL           2u
#define MYWIN_NS_PRIVATE          3u

typedef struct MyHandleInfo {
    DWORD  pid;
    HANDLE handle;
    HANDLE object_handle;
    DWORD  object_type;
    DWORD  granted_access;
    DWORD  flags;
    DWORD  slot;
    DWORD  object_slot;
    DWORD  object_ref;
    DWORD  object_generation;
    DWORD  object_state;
    DWORD  object_handle_count;
    char   object_name[96];
} MyHandleInfo;

typedef BOOL (*MYHANDLEENUMPROC)(const MyHandleInfo* lpInfo, LPARAM lParam);


typedef struct MyHandleTableAudit {
    DWORD total_handles;
    DWORD owner_pid_count;
    DWORD live_owner_handles;
    DWORD exited_owner_handles;
    DWORD orphan_owner_handles;
    DWORD inherited_handles;
    DWORD process_handles;
    DWORD thread_handles;
    DWORD waitable_handles;
    DWORD dead_pid_tables;
    DWORD max_handles_per_pid;
    DWORD sweep_calls;
    DWORD swept_handles;
    DWORD sweep_failures;
    DWORD last_sweep_pid;
    DWORD duplicate_success;
    DWORD duplicate_cross_process;
    DWORD duplicate_close_source;
    DWORD duplicate_failures;
    DWORD duplicate_access_denied;
    DWORD duplicate_invalid_process;
    DWORD handle_cache_hits;
    DWORD handle_cache_misses;
    DWORD handle_cache_stores;
    DWORD handle_cache_invalidations;
    DWORD handle_cache_entry_validated;
    DWORD handle_cache_entry_stale;
    DWORD handle_cache_slot_probes;
    DWORD handle_cache_slot_collisions;
    DWORD handle_free_hint_hits;
    DWORD handle_free_hint_misses;
    DWORD handle_free_mark_duplicate_skips;
    DWORD handle_free_stale_pops;
    DWORD handle_free_batch_hits;
    DWORD handle_free_batch_stores;
    DWORD handle_free_batch_flushes;
    DWORD handle_free_batch_flushed_slots;
    DWORD handle_free_batch_overflow;
    DWORD handle_free_batch_misses;
    DWORD handle_free_batch_lane_allocs;
    DWORD handle_free_batch_lane_matches;
    DWORD handle_free_batch_table_switch_avoided;
    DWORD pushlock_shared_fast;
    DWORD pushlock_shared_slow;
    DWORD pushlock_exclusive_fast;
    DWORD pushlock_exclusive_slow;
    DWORD pushlock_wakeups;
    DWORD pushlock_contentions;
} MyHandleTableAudit;

typedef struct MyWaitAudit {
    DWORD wait_single_calls;
    DWORD wait_multiple_calls;
    DWORD wait_any_calls;
    DWORD wait_all_calls;
    DWORD wait_success;
    DWORD wait_timeouts;
    DWORD wait_failures;
    DWORD wait_access_denied;
    DWORD wait_invalid_handle;
    DWORD event_consumes;
    DWORD mutex_acquires;
    DWORD mutex_abandoned;
    DWORD semaphore_consumes;
    DWORD timer_consumes;
    DWORD wait_all_commits;
    DWORD wait_any_commits;
    DWORD wake_broadcasts;
    DWORD wake_skips;
    DWORD wait_single_targeted;
    DWORD wait_single_global_fallback;
    DWORD wait_multiple_targeted;
    DWORD wait_multiple_global_fallback;
    DWORD wait_multiple_targeted_wakes;
    DWORD wait_multiple_waitblock_links;
    DWORD wait_multiple_waitblock_unlinks;
    DWORD wait_multiple_waitblock_object_wakes;
    DWORD wait_multiple_resolved_probes;
    DWORD wait_multiple_immediate_hits;
    DWORD wait_multiple_deferred_links;
    DWORD wait_multiple_tls_gates;
    DWORD wait_multiple_prevalidated;
    DWORD wait_multiple_prevalidate_resolves;
    DWORD wait_multiple_prevalidate_fallbacks;
    DWORD wait_process_thread_targeted;
    DWORD wait_process_thread_immediate_hits;
    DWORD wait_process_thread_poll_slices;
    DWORD wait_process_thread_object_wakes;
    DWORD wait_dispatcher_header_inits;
    DWORD wait_dispatcher_header_head_hits;
    DWORD wait_dispatcher_header_state_stores;
    DWORD wait_dispatcher_header_fast_not_ready;
} MyWaitAudit;



typedef struct MyNamedDirectoryAudit {
    DWORD entries;
    DWORD free_slots;
    DWORD lookups;
    DWORD hits;
    DWORD misses;
    DWORD cross_type_conflicts;
    DWORD inserts;
    DWORD removes;
    DWORD fast_hits;
    DWORD fast_misses;
    DWORD fast_type_mismatches;
    DWORD stale_hits;
    DWORD free_reuse;
    DWORD free_duplicate_skips;
    DWORD epoch;
    DWORD tls_hits;
    DWORD tls_misses;
    DWORD tls_epoch_misses;
    DWORD tls_collisions;
    DWORD tls_stores;
    DWORD tls_stale_invalidations;
    DWORD slot_fast_hits;
    DWORD slot_fast_misses;
} MyNamedDirectoryAudit;

typedef struct MyProcessIndexAudit {
    DWORD pid_hash_hits;
    DWORD pid_hash_misses;
    DWORD tid_hash_hits;
    DWORD tid_hash_misses;
    DWORD pid_hash_inserts;
    DWORD tid_hash_inserts;
    DWORD alloc_fast;
    DWORD alloc_fallback;
    DWORD fallback_scans;
    DWORD live_records;
    DWORD exited_records;
} MyProcessIndexAudit;

typedef struct MyProcessLiteInfo {
    DWORD  pid;
    DWORD  parent_pid;
    DWORD  thread_id;
    HANDLE process_object;
    HANDLE thread_object;
    DWORD  flags;
    DWORD  exit_code;
    DWORD  inherited_handles;
    DWORD  duplicated_in;
    DWORD  handle_count;
    DWORD  cap_flags;
    DWORD  runtime_enters;
    DWORD  runtime_depth;
    DWORD  startup_flags;
    DWORD  startup_x;
    DWORD  startup_y;
    DWORD  startup_w;
    DWORD  startup_h;
    WORD   show_window;
    HANDLE std_input;
    HANDLE std_output;
    HANDLE std_error;
    char   image_name[64];
    char   image_path[MYWIN_MAX_MODULE_PATH];
    char   module_name[MYWIN_MAX_MODULE_NAME];
    HMODULE main_module;
    char   cap_name[32];
    char   command_line[128];
    char   current_directory[128];
    char   window_title[64];
    DWORD  environment_count;
    char   environment_preview[160];
    DWORD  dll_count;
    char   dll_preview[160];
    char   dll_directory[MYWIN_MAX_MODULE_PATH];
    DWORD  last_error;
    DWORD  loader_import_count;
    DWORD  loader_resolved_count;
    DWORD  loader_entry_called;
    DWORD  loader_error;
    char   loader_entry[MYWIN_MAX_LOADER_ENTRY];
    char   loader_import_preview[MYWIN_MAX_LOADER_PREVIEW];
    char   subsystem[MYWIN_MAX_SUBSYSTEM];
    DWORD  argc;
    char   argv_preview[MYWIN_MAX_ARGV_PREVIEW];
    DWORD  console_exit_code;
    int    linux_pid;
    DWORD  linux_status;
    DWORD  fork_exec;
    DWORD  process_host_state;
    DWORD  process_host_polls;
    DWORD  process_host_reaps;
    DWORD  process_host_kills;
    DWORD  process_host_start_ms;
    DWORD  process_host_exit_ms;
    char   process_host_state_name[24];
    char   process_host_last_event[96];
    DWORD  ipc_enabled;
    DWORD  ipc_messages;
    DWORD  ipc_hello;
    DWORD  ipc_exit_report;
    DWORD  ipc_last_opcode;
    DWORD  ipc_last_value;
    char   ipc_last_text[96];
    char   ipc_shared_name[96];
    DWORD  ipc_shared_heartbeat;
    DWORD  ipc_shared_child_pid;
    DWORD  ipc_shared_argc;
    DWORD  ipc_shared_exit_code;
    char   ipc_shared_status[96];
    char   ipc_shared_argv_preview[160];
} MyProcessLiteInfo;

typedef BOOL (*MYPROCESSLITEENUMPROC)(const MyProcessLiteInfo* lpInfo, LPARAM lParam);

typedef struct MyRuntimeContextInfo {
    DWORD  pid;
    DWORD  parent_pid;
    DWORD  thread_id;
    DWORD  flags;
    DWORD  exit_code;
    DWORD  cap_flags;
    DWORD  handle_count;
    DWORD  runtime_enters;
    DWORD  runtime_depth;
    DWORD  startup_flags;
    WORD   show_window;
    HANDLE std_input;
    HANDLE std_output;
    HANDLE std_error;
    char   image_name[64];
    char   image_path[MYWIN_MAX_MODULE_PATH];
    char   module_name[MYWIN_MAX_MODULE_NAME];
    HMODULE main_module;
    char   cap_name[32];
    char   command_line[128];
    char   current_directory[128];
    char   window_title[64];
    DWORD  environment_count;
    char   environment_preview[160];
    DWORD  dll_count;
    char   dll_preview[160];
    char   dll_directory[MYWIN_MAX_MODULE_PATH];
    DWORD  last_error;
    DWORD  loader_import_count;
    DWORD  loader_resolved_count;
    DWORD  loader_entry_called;
    DWORD  loader_error;
    char   loader_entry[MYWIN_MAX_LOADER_ENTRY];
    char   loader_import_preview[MYWIN_MAX_LOADER_PREVIEW];
    char   subsystem[MYWIN_MAX_SUBSYSTEM];
    DWORD  argc;
    char   argv_preview[MYWIN_MAX_ARGV_PREVIEW];
    DWORD  console_exit_code;
    int    linux_pid;
    DWORD  linux_status;
    DWORD  fork_exec;
    DWORD  process_host_state;
    DWORD  process_host_polls;
    DWORD  process_host_reaps;
    DWORD  process_host_kills;
    DWORD  process_host_start_ms;
    DWORD  process_host_exit_ms;
    char   process_host_state_name[24];
    char   process_host_last_event[96];
    DWORD  ipc_enabled;
    DWORD  ipc_messages;
    DWORD  ipc_hello;
    DWORD  ipc_exit_report;
    DWORD  ipc_last_opcode;
    DWORD  ipc_last_value;
    char   ipc_last_text[96];
    char   ipc_shared_name[96];
    DWORD  ipc_shared_heartbeat;
    DWORD  ipc_shared_child_pid;
    DWORD  ipc_shared_argc;
    DWORD  ipc_shared_exit_code;
    char   ipc_shared_status[96];
    char   ipc_shared_argv_preview[160];
} MyRuntimeContextInfo;

BOOL   MyWinGetRuntimeContextInfo(DWORD dwProcessId, MyRuntimeContextInfo* lpInfo);
BOOL   MyWinSetProcessLoaderInfo(DWORD dwProcessId, DWORD dwImportCount, DWORD dwResolvedCount, DWORD dwLoaderError, LPCSTR lpEntryName, LPCSTR lpImportPreview, BOOL bEntryCalled);
BOOL   MyWinSetProcessSubsystemInfo(DWORD dwProcessId, LPCSTR lpSubsystem, DWORD dwArgc, LPCSTR lpArgvPreview, DWORD dwConsoleExitCode);
BOOL   MyWinAttachLinuxProcess(DWORD dwProcessId, int nLinuxPid);
BOOL   MyWinPollProcess(DWORD dwProcessId);
DWORD  MyWinPollAllProcesses(void);

BOOL   MyGetWindowState(HWND hWnd, MyWindowState* lpState);
const MyWindowStateSection* MyGetWindowStateSection(void);
BOOL   MyEnumProcesses(MYPROCESSENUMPROC lpEnumFunc, LPARAM lParam);
BOOL   MyGetProcessInfo(DWORD dwProcessId, MyProcessInfo* lpInfo);
BOOL   MyGetProcessLiteInfo(DWORD dwProcessId, MyProcessLiteInfo* lpInfo);
BOOL   MyEnumProcessLite(MYPROCESSLITEENUMPROC lpEnumFunc, LPARAM lParam);
DWORD  MyGetSectionCount(void);
DWORD  MyGetMappedViewCount(void);
DWORD  MyGetObjectCount(void);
DWORD  MyGetObjectCountByType(DWORD dwType);
DWORD  MyGetProcessLiveCount(void);
DWORD  MyGetProcessExitedCount(void);
BOOL   MyEnumObjects(MYOBJECTENUMPROC lpEnumFunc, LPARAM lParam);
BOOL   MyGetObjectInfo(HANDLE hObject, _ObjectectInfo* lpInfo);
const char* MyGetObjectTypeName(DWORD dwType);
BOOL   MyEnumProcessHandles(DWORD dwProcessId, MYHANDLEENUMPROC lpEnumFunc, LPARAM lParam);
DWORD  MyGetHandleCount(DWORD dwProcessId);
BOOL   MyWinGetHandleTableAudit(MyHandleTableAudit* lpAudit);
BOOL   MyWinGetWaitAudit(MyWaitAudit* lpAudit);
BOOL   MyWinGetNamedDirectoryAudit(MyNamedDirectoryAudit* lpAudit);
BOOL   MyWinGetProcessIndexAudit(MyProcessIndexAudit* lpAudit);
DWORD  MyWinSweepExitedHandleTables(void);
BOOL   MyGetHandleInfo(HANDLE hHandle, MyHandleInfo* lpInfo);
BOOL   MyWinSetStrictKernelHandles(BOOL bEnable);
BOOL   MyWinGetStrictKernelHandles(void);

#ifdef __cplusplus
}
#endif
