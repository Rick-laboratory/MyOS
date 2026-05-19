#include "processhost.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>

/* AUDIT(v118): ProcessHost is fixed-table and datagram/shared-memory based.
   It is solid for current OOP labs, but high process counts, long messages,
   or richer child USER32 calls need queueing/backpressure/error semantics. */
#define MYPROCESSHOST_MAX 64
#define MYPROCESSHOST_MAX_ARGC 64
#define MYPROCESSHOST_PID_HASH_BUCKETS 128
#define MYPROCESSHOST_PID_HASH_MASK (MYPROCESSHOST_PID_HASH_BUCKETS - 1)

typedef struct MyProcessHostEntry {
    int valid;
    DWORD myPid;
    DWORD pidHash;
    int pidHashNext;
    int linuxPid;
    DWORD state;
    DWORD exitCode;
    DWORD rawStatus;
    DWORD pollCount;
    DWORD reapCount;
    DWORD killCount;
    DWORD cleanupCount;
    DWORD cleanupErrorCount;
    DWORD resourcesClosed;
    DWORD startMs;
    DWORD exitMs;
    char imageName[64];
    char lastEvent[96];

    // v58: per-child IPC channel + shared section.
    int ipcEnabled;
    int ipcFd;
    DWORD ipcMessages;
    DWORD ipcHello;
    DWORD ipcExitReport;
    DWORD ipcLastOpcode;
    DWORD ipcLastValue;
    char ipcLastText[MYOS_IPC_TEXT_MAX];
    char sharedName[96];
    int sharedFd;
    MyProcessIpcShared* shared;

    // v59: GUI child can request parent-side CreateWindowExA.
    DWORD guiCreateRequest;
    DWORD guiCreateConsumed;
    DWORD guiCreateAck;
    DWORD guiHwnd;
    DWORD guiWindowIndex;
    char guiClass[MYOS_IPC_IMAGE_MAX];
    char guiTitle[MYOS_IPC_TEXT_MAX];
    MyProcessHostCreateWindowRequest lastCreate; /* v172: carries owner/style for top-level CreateWindowExA. */

    // v61: GUI runtime-backed cross-process PostMessage/GetMessage/DispatchMessage bridge.
    DWORD guiMsgSent;
    DWORD guiMsgReceived;
    DWORD guiMsgDispatched;
    DWORD guiPostRequest;
    DWORD guiPostConsumed;
    DWORD guiPostAck;
    DWORD guiCloseSeen;
    DWORD guiDestroyRequest;
    DWORD guiDestroyConsumed;
    DWORD guiDestroyAck;
    DWORD guiEnableRequest;
    DWORD guiEnableConsumed;
    DWORD guiEnableAck;

    // v66/v180: pending child-HWND/control CreateWindowExA request from GUI child.
    DWORD childCreateRequest;
    DWORD childCreateConsumed;
    DWORD childCreateAck;
    DWORD childBatchCreateRequest;
    DWORD childBatchCreateConsumed;
    DWORD childBatchCreateAck;
    DWORD childCreated;
    MyProcessHostCreateChildWindowRequest lastChildCreate;
    MyProcessHostCreateChildWindowBatchRequest lastChildBatch;

    MyProcessHostWindowMessage lastPost;
    MyProcessHostWindowMessage lastDestroy;
    MyProcessHostWindowMessage lastEnable;
    MyProcessHostWindowMessage lastWindowMsg;
} MyProcessHostEntry;

static MyProcessHostEntry g_ProcessHost[MYPROCESSHOST_MAX];
static int g_ProcessHostPidHash[MYPROCESSHOST_PID_HASH_BUCKETS];
static int g_ProcessHostFreeStack[MYPROCESSHOST_MAX];
static int g_ProcessHostFreeTop = 0;
static int g_ProcessHostFreeInit = 0;
static pthread_mutex_t g_ProcessHostLock = PTHREAD_MUTEX_INITIALIZER;
static DWORD g_ProcessHostReclaimedSlots = 0;

/* v249: ProcessHost now has a tiny async reaper.  Wait paths no longer need
   to poll Linux-backed children every 25ms just to discover process exit; the
   reaper observes waitpid(WNOHANG), then notifies the KERNEL32 dispatcher. */
static pthread_mutex_t g_ProcessHostReaperStartLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_ProcessHostReaperThread;
static int g_ProcessHostReaperStarted = 0;
static DWORD g_ProcessHostReaperPolls = 0;
static DWORD g_ProcessHostReaperReaps = 0;
static DWORD g_ProcessHostReaperNotifications = 0;

static DWORD ph_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long ms = (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
    return (DWORD)(ms & 0xffffffffu);
}

const char* MyProcessHostStateName(DWORD state)
{
    switch (state) {
    case MYPROCESSHOST_STATE_RUNNING: return "running";
    case MYPROCESSHOST_STATE_EXITED:  return "exited";
    case MYPROCESSHOST_STATE_REAPED:  return "reaped";
    case MYPROCESSHOST_STATE_LOST:    return "lost";
    case MYPROCESSHOST_STATE_KILLED:  return "killed";
    default: return "empty";
    }
}

static DWORD ph_exit_code_from_wait_status(int status)
{
    if (WIFEXITED(status)) return (DWORD)WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return (DWORD)(128 + WTERMSIG(status));
    return 1u;
}


static void* ph_reaper_main(void* arg)
{
    (void)arg;
    for (;;) {
        DWORD pids[MYPROCESSHOST_MAX];
        int n = 0;
        pthread_mutex_lock(&g_ProcessHostLock);
        for (int i = 0; i < MYPROCESSHOST_MAX && n < MYPROCESSHOST_MAX; ++i) {
            if (g_ProcessHost[i].valid && g_ProcessHost[i].state == MYPROCESSHOST_STATE_RUNNING)
                pids[n++] = g_ProcessHost[i].myPid;
        }
        pthread_mutex_unlock(&g_ProcessHostLock);

        for (int i = 0; i < n; ++i) {
            BOOL exited = FALSE;
            DWORD exitCode = STILL_ACTIVE;
            DWORD rawStatus = 0;
            __atomic_add_fetch(&g_ProcessHostReaperPolls, 1u, __ATOMIC_RELAXED);
            if (MyProcessHostPoll(pids[i], &exited, &exitCode, &rawStatus) && exited) {
                __atomic_add_fetch(&g_ProcessHostReaperReaps, 1u, __ATOMIC_RELAXED);
                MyWinNotifyProcessHostExit(pids[i], exitCode, rawStatus);
                __atomic_add_fetch(&g_ProcessHostReaperNotifications, 1u, __ATOMIC_RELAXED);
            }
        }

        usleep(n ? 2000u : 25000u);
    }
    return NULL;
}

static void ph_reaper_ensure(void)
{
    pthread_mutex_lock(&g_ProcessHostReaperStartLock);
    if (!g_ProcessHostReaperStarted) {
        if (pthread_create(&g_ProcessHostReaperThread, NULL, ph_reaper_main, NULL) == 0) {
            pthread_detach(g_ProcessHostReaperThread);
            __atomic_store_n(&g_ProcessHostReaperStarted, 1, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&g_ProcessHostReaperStartLock);
}

BOOL MyProcessHostAsyncReaperActive(void)
{
    return __atomic_load_n(&g_ProcessHostReaperStarted, __ATOMIC_ACQUIRE) ? TRUE : FALSE;
}

static int ph_state_is_final(DWORD state)
{
    return state == MYPROCESSHOST_STATE_EXITED ||
           state == MYPROCESSHOST_STATE_REAPED ||
           state == MYPROCESSHOST_STATE_LOST ||
           state == MYPROCESSHOST_STATE_KILLED;
}

static int ph_entry_has_ipc_resources_locked(const MyProcessHostEntry* e)
{
    return e && (e->ipcFd >= 0 || e->sharedFd >= 0 || e->shared != NULL || e->sharedName[0] != 0);
}

static void ph_freestack_init_locked(void)
{
    if (g_ProcessHostFreeInit) return;
    g_ProcessHostFreeTop = 0;
    for (int i = MYPROCESSHOST_MAX - 1; i >= 0; --i) g_ProcessHostFreeStack[g_ProcessHostFreeTop++] = i;
    g_ProcessHostFreeInit = 1;
}

static int ph_pop_free_locked(void)
{
    ph_freestack_init_locked();
    if (g_ProcessHostFreeTop <= 0) return -1;
    return g_ProcessHostFreeStack[--g_ProcessHostFreeTop];
}

static inline DWORD ph_pid_hash(DWORD pid)
{
    DWORD h = pid ? pid : 1u;
    h *= 2654435761u;
    h ^= h >> 16;
    return h ? h : 1u;
}

static inline int ph_pid_bucket(DWORD hash)
{
    return (int)(hash & MYPROCESSHOST_PID_HASH_MASK);
}

static void ph_hash_insert_locked(int idx)
{
    if (idx < 0 || idx >= MYPROCESSHOST_MAX || !g_ProcessHost[idx].valid || !g_ProcessHost[idx].myPid) return;
    DWORD h = ph_pid_hash(g_ProcessHost[idx].myPid);
    int b = ph_pid_bucket(h);
    g_ProcessHost[idx].pidHash = h;
    g_ProcessHost[idx].pidHashNext = g_ProcessHostPidHash[b];
    g_ProcessHostPidHash[b] = idx + 1;
}

static void ph_hash_remove_locked(int idx)
{
    if (idx < 0 || idx >= MYPROCESSHOST_MAX || !g_ProcessHost[idx].pidHash) return;
    int b = ph_pid_bucket(g_ProcessHost[idx].pidHash);
    int* link = &g_ProcessHostPidHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) { *link = g_ProcessHost[cur].pidHashNext; break; }
        link = &g_ProcessHost[cur].pidHashNext;
    }
    g_ProcessHost[idx].pidHash = 0;
    g_ProcessHost[idx].pidHashNext = 0;
}

static void ph_init_slot_locked(int idx, DWORD myPid)
{
    if (idx < 0 || idx >= MYPROCESSHOST_MAX) return;
    memset(&g_ProcessHost[idx], 0, sizeof(g_ProcessHost[idx]));
    g_ProcessHost[idx].valid = 1;
    g_ProcessHost[idx].myPid = myPid;
    g_ProcessHost[idx].ipcFd = -1;
    g_ProcessHost[idx].sharedFd = -1;
    ph_hash_insert_locked(idx);
}

static void ph_cleanup_ipc_resources_locked(MyProcessHostEntry* e, const char* why)
{
    if (!e) return;

    int had = ph_entry_has_ipc_resources_locked(e);
    int errors = 0;

    if (e->ipcFd >= 0) {
        if (close(e->ipcFd) != 0) errors++;
        e->ipcFd = -1;
    }
    if (e->shared) {
        if (munmap(e->shared, sizeof(MyProcessIpcShared)) != 0) errors++;
        e->shared = NULL;
    }
    if (e->sharedFd >= 0) {
        if (close(e->sharedFd) != 0) errors++;
        e->sharedFd = -1;
    }
    if (e->sharedName[0]) {
        if (shm_unlink(e->sharedName) != 0 && errno != ENOENT) errors++;
        e->sharedName[0] = '\0';
    }

    if (had) {
        e->cleanupCount++;
        e->resourcesClosed = 1;
        if (errors) e->cleanupErrorCount += (DWORD)errors;
        snprintf(e->lastEvent, sizeof(e->lastEvent), "%s; ipc resources closed err=%d",
                 why && why[0] ? why : "finalized", errors);
    }
}

static MyProcessHostEntry* ph_find_locked(DWORD myPid)
{
    if (!myPid) return NULL;
    DWORD h = ph_pid_hash(myPid);
    int b = ph_pid_bucket(h);
    for (int link = g_ProcessHostPidHash[b]; link; link = g_ProcessHost[link - 1].pidHashNext) {
        int idx = link - 1;
        if (idx >= 0 && idx < MYPROCESSHOST_MAX && g_ProcessHost[idx].valid &&
            g_ProcessHost[idx].pidHash == h && g_ProcessHost[idx].myPid == myPid) return &g_ProcessHost[idx];
    }
    return NULL;
}

static MyProcessHostEntry* ph_alloc_locked(DWORD myPid)
{
    MyProcessHostEntry* e = ph_find_locked(myPid);
    if (e) return e;
    int freeIdx = ph_pop_free_locked();
    if (freeIdx >= 0) {
        ph_init_slot_locked(freeIdx, myPid);
        return &g_ProcessHost[freeIdx];
    }

    /* v184: ProcessHost is an internal fork/exec tracker, not a Win32
       object-lifetime owner.  Finalized entries with closed IPC resources may
       be reclaimed when the fixed table is exhausted.  Running/final-open
       entries are never recycled. */
    for (int i = 0; i < MYPROCESSHOST_MAX; i++) {
        if (g_ProcessHost[i].valid && ph_state_is_final(g_ProcessHost[i].state) &&
            !ph_entry_has_ipc_resources_locked(&g_ProcessHost[i])) {
            ph_hash_remove_locked(i);
            ph_init_slot_locked(i, myPid);
            g_ProcessHostReclaimedSlots++;
            return &g_ProcessHost[i];
        }
    }
    return NULL;
}

static void ph_shared_snapshot_locked(const MyProcessHostEntry* e, MyProcessHostInfo* out)
{
    if (!e || !out || !e->shared) return;
    out->shared_heartbeat = e->shared->heartbeat;
    out->shared_child_pid = e->shared->child_pid;
    out->shared_argc = e->shared->argc;
    out->shared_exit_code = e->shared->exit_code;
    snprintf(out->shared_status, sizeof(out->shared_status), "%s", e->shared->status);
    snprintf(out->shared_argv_preview, sizeof(out->shared_argv_preview), "%s", e->shared->argv_preview);
    out->gui_create_request = e->shared->gui_request;
    out->gui_create_ack = e->shared->gui_ack;
    out->gui_hwnd = e->shared->gui_hwnd;
    out->gui_window_index = e->shared->gui_window_index;
    out->gui_x = e->shared->gui_x;
    out->gui_y = e->shared->gui_y;
    out->gui_w = e->shared->gui_w;
    out->gui_h = e->shared->gui_h;
    snprintf(out->gui_class, sizeof(out->gui_class), "%s", e->shared->gui_class);
    snprintf(out->gui_title, sizeof(out->gui_title), "%s", e->shared->gui_title);
    out->gui_msg_sent = e->shared->gui_msg_sent;
    out->gui_msg_received = e->shared->gui_msg_received;
    out->gui_msg_dispatched = e->shared->gui_msg_dispatched;
    out->gui_post_request = e->shared->gui_post_request;
    out->gui_post_ack = e->shared->gui_post_ack;
    out->gui_close_seen = e->shared->gui_close_seen;
    out->gui_last_hwnd = e->shared->gui_last_hwnd;
    out->gui_last_msg = e->shared->gui_last_msg;
    out->gui_last_wparam_lo = (DWORD)(e->shared->gui_last_wparam & 0xffffffffu);
    out->gui_last_lparam_lo = (DWORD)(e->shared->gui_last_lparam & 0xffffffffu);
    snprintf(out->gui_last_text, sizeof(out->gui_last_text), "%s", e->shared->gui_last_text);
    out->gui_runtime_api_calls = e->shared->gui_runtime_api_calls;
    out->gui_register_class_calls = e->shared->gui_register_class_calls;
    out->gui_create_window_calls = e->shared->gui_create_window_calls;
    out->gui_get_message_calls = e->shared->gui_get_message_calls;
    out->gui_dispatch_message_calls = e->shared->gui_dispatch_message_calls;
    out->gui_destroy_window_calls = e->shared->gui_destroy_window_calls;
    out->gui_destroy_request = e->shared->gui_destroy_request;
    out->gui_destroy_ack = e->shared->gui_destroy_ack;
    snprintf(out->gui_runtime_status, sizeof(out->gui_runtime_status), "%s", e->shared->gui_runtime_status);
    out->calc_enabled = e->shared->calc_enabled;
    out->calc_revision = e->shared->calc_revision;
    out->calc_button_hits = e->shared->calc_button_hits;
    snprintf(out->calc_display, sizeof(out->calc_display), "%s", e->shared->calc_display);
    snprintf(out->calc_opline, sizeof(out->calc_opline), "%s", e->shared->calc_opline);
    snprintf(out->calc_last_button, sizeof(out->calc_last_button), "%s", e->shared->calc_last_button);
    snprintf(out->calc_history_preview, sizeof(out->calc_history_preview), "%s", e->shared->calc_history_preview);

    out->editor_enabled = e->shared->editor_enabled;
    out->editor_revision = e->shared->editor_revision;
    out->editor_chars_typed = e->shared->editor_chars_typed;
    out->editor_keydowns = e->shared->editor_keydowns;
    out->editor_cursor = e->shared->editor_cursor;
    out->editor_length = e->shared->editor_length;
    out->editor_dirty = e->shared->editor_dirty;
    out->editor_scroll_line = e->shared->editor_scroll_line;
    snprintf(out->editor_path, sizeof(out->editor_path), "%s", e->shared->editor_path);
    snprintf(out->editor_name, sizeof(out->editor_name), "%s", e->shared->editor_name);
    snprintf(out->editor_status, sizeof(out->editor_status), "%s", e->shared->editor_status);
    snprintf(out->editor_preview, sizeof(out->editor_preview), "%s", e->shared->editor_preview);

    out->paint_enabled = e->shared->paint_enabled;
    out->paint_revision = e->shared->paint_revision;
    out->paint_segments = e->shared->paint_segments;
    out->paint_mouse_down = e->shared->paint_mouse_down;
    out->paint_capture_count = e->shared->paint_capture_count;
    out->paint_release_count = e->shared->paint_release_count;
    out->paint_move_count = e->shared->paint_move_count;
    out->paint_clear_count = e->shared->paint_clear_count;
    out->paint_last_x = e->shared->paint_last_x;
    out->paint_last_y = e->shared->paint_last_y;
    snprintf(out->paint_status, sizeof(out->paint_status), "%s", e->shared->paint_status);

    out->clip_enabled = e->shared->clip_enabled;
    out->clip_request = e->shared->clip_request;
    out->clip_ack = e->shared->clip_ack;
    out->clip_op = e->shared->clip_op;
    out->clip_ok = e->shared->clip_ok;
    out->clip_set_count = e->shared->clip_set_count;
    out->clip_get_count = e->shared->clip_get_count;
    out->clip_clear_count = e->shared->clip_clear_count;
    out->clip_open_count = e->shared->clip_open_count;
    out->clip_close_count = e->shared->clip_close_count;
    snprintf(out->clip_text, sizeof(out->clip_text), "%s", e->shared->clip_text);
    snprintf(out->clip_local_text, sizeof(out->clip_local_text), "%s", e->shared->clip_local_text);
    snprintf(out->clip_status, sizeof(out->clip_status), "%s", e->shared->clip_status);

    out->menu_enabled = e->shared->menu_enabled;
    out->menu_request = e->shared->menu_request;
    out->menu_ack = e->shared->menu_ack;
    out->menu_op = e->shared->menu_op;
    out->menu_ok = e->shared->menu_ok;
    out->menu_create_count = e->shared->menu_create_count;
    out->menu_append_count = e->shared->menu_append_count;
    out->menu_set_count = e->shared->menu_set_count;
    out->menu_popup_count = e->shared->menu_popup_count;
    out->menu_command_count = e->shared->menu_command_count;
    out->accel_count = e->shared->accel_count;
    out->accel_translate_count = e->shared->accel_translate_count;
    out->menu_last_handle = e->shared->menu_last_handle;
    out->menu_last_command = e->shared->menu_last_command;
    snprintf(out->menu_status, sizeof(out->menu_status), "%s", e->shared->menu_status);

    out->kernel_enabled = e->shared->kernel_enabled;
    out->kernel_request = e->shared->kernel_request;
    out->kernel_ack = e->shared->kernel_ack;
    out->kernel_op = e->shared->kernel_op;
    out->kernel_ok = e->shared->kernel_ok;
    out->kernel_error = e->shared->kernel_error;
    out->kernel_handle = e->shared->kernel_handle;
    out->kernel_result = e->shared->kernel_result;
    out->kernel_access = e->shared->kernel_access;
    out->kernel_flags = e->shared->kernel_flags;
    out->kernel_timeout = e->shared->kernel_timeout;
    out->kernel_count = e->shared->kernel_count;
    out->kernel_wait_all = e->shared->kernel_wait_all;
    snprintf(out->kernel_name, sizeof(out->kernel_name), "%s", e->shared->kernel_name);
    snprintf(out->kernel_status, sizeof(out->kernel_status), "%s", e->shared->kernel_status);

    out->child_hwnd_request = e->shared->child_hwnd_request;
    out->child_hwnd_ack = e->shared->child_hwnd_ack;
    out->child_hwnd_created = e->shared->child_hwnd_created;
    out->child_hwnd_count = e->shared->child_hwnd_count;
    out->child_hwnd_parent = e->shared->child_hwnd_parent;
    out->child_hwnd_last = e->shared->child_hwnd_last;
    out->child_hwnd_last_id = e->shared->child_hwnd_last_id;
    out->child_hwnd_last_msg = e->shared->child_hwnd_last_msg;
    out->child_hwnd_command_count = e->shared->child_hwnd_command_count;
    out->child_hwnd_click_count = e->shared->child_hwnd_click_count;
    snprintf(out->child_hwnd_status, sizeof(out->child_hwnd_status), "%s", e->shared->child_hwnd_status);

    out->gdi_enabled = e->shared->gdi_enabled;
    out->gdi_sequence = e->shared->gdi_sequence;
    out->gdi_paint_count = e->shared->gdi_paint_count;
    out->gdi_command_count = e->shared->gdi_command_count;
    if (out->gdi_command_count > MYOS_GDI_MAX_COMMANDS) out->gdi_command_count = MYOS_GDI_MAX_COMMANDS;
    out->gdi_client_w = e->shared->gdi_client_w;
    out->gdi_client_h = e->shared->gdi_client_h;
    out->gdi_last_msg = e->shared->gdi_last_msg;
    snprintf(out->gdi_status, sizeof(out->gdi_status), "%s", e->shared->gdi_status);
    if (out->gdi_command_count)
        memcpy(out->gdi_commands, e->shared->gdi_commands, sizeof(MyGdiIpcCommand) * out->gdi_command_count);


    out->surface_enabled = e->shared->surface_enabled;
    out->surface_mapped = e->shared->surface_mapped;
    out->surface_width = e->shared->surface_width;
    out->surface_height = e->shared->surface_height;
    out->surface_stride = e->shared->surface_stride;
    out->surface_format = e->shared->surface_format;
    out->surface_seq = e->shared->surface_seq;
    out->surface_paint_count = e->shared->surface_paint_count;
    out->surface_dirty_left = e->shared->surface_dirty_left;
    out->surface_dirty_top = e->shared->surface_dirty_top;
    out->surface_dirty_right = e->shared->surface_dirty_right;
    out->surface_dirty_bottom = e->shared->surface_dirty_bottom;
    out->surface_map_size = e->shared->surface_map_size;
    snprintf(out->surface_map_name, sizeof(out->surface_map_name), "%s", e->shared->surface_map_name);
    snprintf(out->surface_status, sizeof(out->surface_status), "%s", e->shared->surface_status);
}

static void ph_fill_info_locked(const MyProcessHostEntry* e, MyProcessHostInfo* out)
{
    memset(out, 0, sizeof(*out));
    if (!e) return;
    out->my_pid = e->myPid;
    out->linux_pid = e->linuxPid;
    out->state = e->state;
    out->exit_code = e->exitCode;
    out->raw_status = e->rawStatus;
    out->poll_count = e->pollCount;
    out->reap_count = e->reapCount;
    out->kill_count = e->killCount;
    out->cleanup_count = e->cleanupCount;
    out->cleanup_error_count = e->cleanupErrorCount;
    out->resources_closed = e->resourcesClosed;
    out->start_ms = e->startMs;
    out->exit_ms = e->exitMs;
    snprintf(out->state_name, sizeof(out->state_name), "%s", MyProcessHostStateName(e->state));
    snprintf(out->image_name, sizeof(out->image_name), "%s", e->imageName);
    snprintf(out->last_event, sizeof(out->last_event), "%s", e->lastEvent);

    out->ipc_enabled = e->ipcEnabled ? 1u : 0u;
    out->ipc_fd = e->ipcFd >= 0 ? (DWORD)e->ipcFd : 0u;
    out->ipc_messages = e->ipcMessages;
    out->ipc_hello = e->ipcHello;
    out->ipc_exit_report = e->ipcExitReport;
    out->ipc_last_opcode = e->ipcLastOpcode;
    out->ipc_last_value = e->ipcLastValue;
    snprintf(out->ipc_last_text, sizeof(out->ipc_last_text), "%s", e->ipcLastText);
    snprintf(out->shared_name, sizeof(out->shared_name), "%s", e->sharedName);
    out->gui_create_request = e->guiCreateRequest;
    out->gui_create_consumed = e->guiCreateConsumed;
    out->gui_create_ack = e->guiCreateAck;
    out->gui_hwnd = e->guiHwnd;
    out->gui_window_index = e->guiWindowIndex;
    snprintf(out->gui_class, sizeof(out->gui_class), "%s", e->guiClass);
    snprintf(out->gui_title, sizeof(out->gui_title), "%s", e->guiTitle);
    out->gui_msg_sent = e->guiMsgSent;
    out->gui_msg_received = e->guiMsgReceived;
    out->gui_msg_dispatched = e->guiMsgDispatched;
    out->gui_post_request = e->guiPostRequest;
    out->gui_post_ack = e->guiPostAck;
    out->gui_close_seen = e->guiCloseSeen;
    out->gui_destroy_request = e->guiDestroyRequest;
    out->gui_destroy_ack = e->guiDestroyAck;
    out->child_hwnd_request = e->childCreateRequest + e->childBatchCreateRequest;
    out->child_hwnd_ack = e->childCreateAck ? e->childCreateAck : e->childBatchCreateAck;
    out->child_hwnd_created = e->childCreated;
    out->child_hwnd_parent = (DWORD)e->lastChildCreate.parent_hwnd;
    out->child_hwnd_last_id = e->lastChildCreate.id;
    snprintf(out->child_hwnd_status, sizeof(out->child_hwnd_status), "%s", e->shared ? e->shared->child_hwnd_status : "");
    if (e->shared) {
        out->kernel_enabled = e->shared->kernel_enabled;
        out->kernel_request = e->shared->kernel_request;
        out->kernel_ack = e->shared->kernel_ack;
        out->kernel_op = e->shared->kernel_op;
        out->kernel_ok = e->shared->kernel_ok;
        out->kernel_error = e->shared->kernel_error;
        out->kernel_handle = e->shared->kernel_handle;
        out->kernel_result = e->shared->kernel_result;
        snprintf(out->kernel_status, sizeof(out->kernel_status), "%s", e->shared->kernel_status);
    }
    out->gui_last_hwnd = (DWORD)e->lastWindowMsg.hwnd;
    out->gui_last_msg = (DWORD)e->lastWindowMsg.msg;
    out->gui_last_wparam_lo = (DWORD)((uintptr_t)e->lastWindowMsg.wparam & 0xffffffffu);
    out->gui_last_lparam_lo = (DWORD)((intptr_t)e->lastWindowMsg.lparam & 0xffffffffu);
    snprintf(out->gui_last_text, sizeof(out->gui_last_text), "%s", e->lastWindowMsg.text);
    ph_shared_snapshot_locked(e, out);
}

static int ph_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void ph_child_chdir_or_exit(LPCSTR lpDirectory)
{
    if (!lpDirectory || !lpDirectory[0]) return;
    if (chdir(lpDirectory) != 0) {
        fprintf(stderr, "[v73 processhost child] chdir('%s') failed: %s\n", lpDirectory, strerror(errno));
        _exit(125);
    }
}

static BOOL ph_directory_is_usable(LPCSTR lpDirectory)
{
    if (!lpDirectory || !lpDirectory[0]) return TRUE;
    struct stat st;
    if (stat(lpDirectory, &st) != 0) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    if (!S_ISDIR(st.st_mode)) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    if (access(lpDirectory, X_OK) != 0) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return TRUE;
}


static void ph_send_ipc_ack_locked(MyProcessHostEntry* e, DWORD opcode, DWORD value, LPCSTR text)
{
    if (!e || !e->ipcEnabled || e->ipcFd < 0) return;
    MyProcessIpcMessage ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic = MYOS_IPC_MAGIC;
    ack.version = MYOS_IPC_VERSION;
    ack.opcode = opcode;
    ack.my_pid = e->myPid;
    ack.child_pid = (uint32_t)e->linuxPid;
    ack.value = value;
    snprintf(ack.text, sizeof(ack.text), "%s", text && text[0] ? text : "ACK");
    (void)send(e->ipcFd, &ack, sizeof(ack), 0);
}

static void ph_handle_clipboard_request_locked(MyProcessHostEntry* e, const MyProcessIpcMessage* msg)
{
    if (!e || !e->shared || !msg) return;
    MyProcessIpcShared* sh = e->shared;
    DWORD op = sh->clip_op;
    BOOL ok = FALSE;
    const UINT fmt = sh->clip_format ? sh->clip_format : 1u;
    sh->clip_enabled = 1;

    if (op == MYOS_CLIP_OP_OPEN) {
        ok = OpenClipboard((HWND)msg->hwnd);
        if (ok) sh->clip_open_count++;
    } else if (op == MYOS_CLIP_OP_CLOSE) {
        ok = CloseClipboard();
        if (ok) sh->clip_close_count++;
    } else if (op == MYOS_CLIP_OP_EMPTY) {
        ok = EmptyClipboard();
        if (ok) sh->clip_clear_count++;
        if (ok) snprintf(sh->clip_text, sizeof(sh->clip_text), "%s", "<cleared>");
    } else if (op == MYOS_CLIP_OP_SET) {
        const char* text = sh->clip_text;
        size_t n = strnlen(text ? text : "", sizeof(sh->clip_text) - 1);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (DWORD)(n + 1));
        if (h) {
            char* p = (char*)GlobalLock(h);
            if (p) {
                memcpy(p, text ? text : "", n);
                p[n] = 0;
                (void)GlobalUnlock(h);
                ok = SetClipboardData(fmt, h) ? TRUE : FALSE;
            }
            if (!ok) (void)GlobalFree(h);
        }
        if (ok) sh->clip_set_count++;
    } else if (op == MYOS_CLIP_OP_GET) {
        HGLOBAL h = GetClipboardData(fmt);
        if (h) {
            const char* p = (const char*)GlobalLock(h);
            if (p) {
                snprintf(sh->clip_text, sizeof(sh->clip_text), "%s", p);
                (void)GlobalUnlock(h);
                ok = TRUE;
            }
        }
        if (ok) sh->clip_get_count++;
        else snprintf(sh->clip_text, sizeof(sh->clip_text), "%s", "<empty or unavailable>");
    } else if (op == MYOS_CLIP_OP_ISAVAIL) {
        ok = IsClipboardFormatAvailable(fmt);
        sh->clip_isavail_count++;
    }

    sh->clip_ok = ok ? 1u : 0u;
    sh->clip_ack = sh->clip_request;
    snprintf(sh->clip_status, sizeof(sh->clip_status), "parent clip op=%u %s text=%.46s", (unsigned)op, ok ? "OK" : "FAIL", sh->clip_text);
    ph_send_ipc_ack_locked(e, MYOS_IPC_OP_CLIPBOARD_ACK, ok ? 1u : 0u, sh->clip_status);
}

static void ph_handle_menu_request_locked(MyProcessHostEntry* e, const MyProcessIpcMessage* msg)
{
    if (!e || !e->shared || !msg) return;
    MyProcessIpcShared* sh = e->shared;
    sh->menu_enabled = 1;
    sh->menu_ok = 1;
    sh->menu_ack = sh->menu_request;
    snprintf(sh->menu_status, sizeof(sh->menu_status), "parent menu/accel op=%u handle=0x%x cmd=%u", (unsigned)sh->menu_op, (unsigned)sh->menu_last_handle, (unsigned)sh->menu_last_command);
    ph_send_ipc_ack_locked(e, MYOS_IPC_OP_MENU_ACK, 1u, sh->menu_status);
}

typedef struct MyKernelRequestJob {
    DWORD myPid;
    int linuxPid;
    int ipcFd;
    MyProcessIpcShared* shared;
    uint32_t seq;
    DWORD op;
    DWORD handle;
    DWORD access;
    DWORD flags;
    DWORD timeout;
    DWORD count;
    DWORD wait_all;
    DWORD options;
    DWORD target_pid;
    int initial;
    int maximum;
    int release_count;
    DWORD size;
    DWORD offset_low;
    DWORD offset_high;
    DWORD handles[8];
    char name[MYOS_IPC_IMAGE_MAX];
} MyKernelRequestJob;

typedef struct MyKernelRequestResult {
    DWORD ok;
    DWORD result;
    DWORD err;
    DWORD map_size;
    char map_name[MYOS_IPC_IMAGE_MAX];
    char status[MYOS_IPC_TEXT_MAX];
} MyKernelRequestResult;

static BOOL ph_kernel_job_still_current(const MyKernelRequestJob* job)
{
    if (!job || !job->shared || !job->seq) return FALSE;
    BOOL current = FALSE;
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(job->myPid);
    if (e && e->shared == job->shared && e->state == MYPROCESSHOST_STATE_RUNNING &&
        job->shared->kernel_request == job->seq && job->shared->kernel_ack != job->seq) {
        current = TRUE;
    }
    pthread_mutex_unlock(&g_ProcessHostLock);
    return current;
}

static DWORD ph_kernel_wait_slice_ms(const MyKernelRequestJob* job, unsigned long long start)
{
    const DWORD slice = 50u;
    if (!job) return 0u;
    if (job->timeout == 0u) return 0u;
    if (job->timeout == INFINITE) return slice;
    unsigned long long elapsed = (unsigned long long)ph_now_ms() - start;
    if (elapsed >= (unsigned long long)job->timeout) return 0u;
    unsigned long long remain = (unsigned long long)job->timeout - elapsed;
    return (DWORD)(remain < slice ? remain : slice);
}

static DWORD ph_execute_wait_one_job(const MyKernelRequestJob* job)
{
    if (!job) return WAIT_FAILED;
    if (job->timeout == 0u)
        return WaitForSingleObject((HANDLE)job->handle, 0u);

    unsigned long long start = (unsigned long long)ph_now_ms();
    for (;;) {
        if (!ph_kernel_job_still_current(job)) return WAIT_FAILED;
        DWORD slice = ph_kernel_wait_slice_ms(job, start);
        DWORD r = WaitForSingleObject((HANDLE)job->handle, slice);
        if (r != WAIT_TIMEOUT) return r;
        if (job->timeout != INFINITE && ((unsigned long long)ph_now_ms() - start) >= (unsigned long long)job->timeout)
            return WAIT_TIMEOUT;
    }
}

static DWORD ph_execute_wait_many_job(const MyKernelRequestJob* job)
{
    if (!job) return WAIT_FAILED;
    DWORD n = job->count;
    if (n > 8u) n = 8u;
    if (n == 0u) return WAIT_FAILED;
    HANDLE handles[8];
    for (DWORD i = 0; i < n; ++i) handles[i] = (HANDLE)job->handles[i];

    if (job->timeout == 0u)
        return WaitForMultipleObjects(n, handles, job->wait_all ? TRUE : FALSE, 0u);

    unsigned long long start = (unsigned long long)ph_now_ms();
    for (;;) {
        if (!ph_kernel_job_still_current(job)) return WAIT_FAILED;
        DWORD slice = ph_kernel_wait_slice_ms(job, start);
        DWORD r = WaitForMultipleObjects(n, handles, job->wait_all ? TRUE : FALSE, slice);
        if (r != WAIT_TIMEOUT) return r;
        if (job->timeout != INFINITE && ((unsigned long long)ph_now_ms() - start) >= (unsigned long long)job->timeout)
            return WAIT_TIMEOUT;
    }
}

static void ph_execute_kernel_job(const MyKernelRequestJob* job, MyKernelRequestResult* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->err = ERROR_SUCCESS;
    snprintf(out->status, sizeof(out->status), "kernel bridge ACK");
    if (!job) { out->err = ERROR_INVALID_PARAMETER; return; }

    BOOL entered = MyWinEnterProcessContext(job->myPid);
    if (!entered) {
        out->err = ERROR_INVALID_PARAMETER;
        snprintf(out->status, sizeof(out->status), "KREQ: no process context pid=%u", (unsigned)job->myPid);
        return;
    }

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = (job->flags & MYOS_KFLAG_INHERIT) ? TRUE : FALSE;

    switch (job->op) {
    case MYOS_KOP_CREATE_EVENT: {
        HANDLE h = CreateEventA(&sa,
                                (job->flags & MYOS_KFLAG_MANUAL_RESET) ? TRUE : FALSE,
                                (job->flags & MYOS_KFLAG_INITIAL_STATE) ? TRUE : FALSE,
                                job->name[0] ? job->name : NULL);
        out->ok = h ? 1u : 0u;
        out->result = (DWORD)h;
        snprintf(out->status, sizeof(out->status), "KREQ CreateEvent seq=%u name=%.28s -> h=0x%x", job->seq, job->name, (unsigned)h);
        break;
    }
    case MYOS_KOP_OPEN_EVENT: {
        HANDLE h = OpenEventA(job->access, sa.bInheritHandle, job->name);
        out->ok = h ? 1u : 0u;
        out->result = (DWORD)h;
        snprintf(out->status, sizeof(out->status), "KREQ OpenEvent seq=%u name=%.30s -> h=0x%x", job->seq, job->name, (unsigned)h);
        break;
    }
    case MYOS_KOP_SET_EVENT:
        out->ok = SetEvent((HANDLE)job->handle) ? 1u : 0u;
        out->result = out->ok;
        snprintf(out->status, sizeof(out->status), "KREQ SetEvent seq=%u h=0x%x -> %s", job->seq, job->handle, out->ok ? "OK" : "FAIL");
        break;
    case MYOS_KOP_RESET_EVENT:
        out->ok = ResetEvent((HANDLE)job->handle) ? 1u : 0u;
        out->result = out->ok;
        snprintf(out->status, sizeof(out->status), "KREQ ResetEvent seq=%u h=0x%x -> %s", job->seq, job->handle, out->ok ? "OK" : "FAIL");
        break;
    case MYOS_KOP_CREATE_MUTEX: {
        HANDLE h = CreateMutexA(&sa, (job->flags & MYOS_KFLAG_INITIAL_OWNER) ? TRUE : FALSE,
                                job->name[0] ? job->name : NULL);
        out->ok = h ? 1u : 0u;
        out->result = (DWORD)h;
        snprintf(out->status, sizeof(out->status), "KREQ CreateMutex seq=%u name=%.28s -> h=0x%x", job->seq, job->name, (unsigned)h);
        break;
    }
    case MYOS_KOP_RELEASE_MUTEX:
        out->ok = ReleaseMutex((HANDLE)job->handle) ? 1u : 0u;
        out->result = out->ok;
        snprintf(out->status, sizeof(out->status), "KREQ ReleaseMutex seq=%u h=0x%x -> %s", job->seq, job->handle, out->ok ? "OK" : "FAIL");
        break;
    case MYOS_KOP_CREATE_SEMAPHORE: {
        HANDLE h = CreateSemaphoreA(&sa, job->initial, job->maximum,
                                    job->name[0] ? job->name : NULL);
        out->ok = h ? 1u : 0u;
        out->result = (DWORD)h;
        snprintf(out->status, sizeof(out->status), "KREQ CreateSemaphore seq=%u name=%.24s -> h=0x%x", job->seq, job->name, (unsigned)h);
        break;
    }
    case MYOS_KOP_RELEASE_SEMAPHORE: {
        LONG prev = 0;
        out->ok = ReleaseSemaphore((HANDLE)job->handle, job->release_count ? job->release_count : 1, &prev) ? 1u : 0u;
        out->result = (DWORD)prev;
        snprintf(out->status, sizeof(out->status), "KREQ ReleaseSemaphore seq=%u h=0x%x prev=%ld -> %s", job->seq, job->handle, (long)prev, out->ok ? "OK" : "FAIL");
        break;
    }
    case MYOS_KOP_CLOSE_HANDLE:
        out->ok = CloseHandle((HANDLE)job->handle) ? 1u : 0u;
        out->result = out->ok;
        snprintf(out->status, sizeof(out->status), "KREQ CloseHandle seq=%u h=0x%x -> %s", job->seq, job->handle, out->ok ? "OK" : "FAIL");
        break;
    case MYOS_KOP_DUPLICATE_HANDLE: {
        HANDLE dup = 0;
        DWORD opts = job->options ? job->options : DUPLICATE_SAME_ACCESS;
        out->ok = DuplicateHandle(GetCurrentProcess(), (HANDLE)job->handle,
                                  GetCurrentProcess(), &dup,
                                  job->access,
                                  (job->flags & MYOS_KFLAG_INHERIT) ? TRUE : FALSE,
                                  opts) ? 1u : 0u;
        out->result = (DWORD)dup;
        snprintf(out->status, sizeof(out->status),
                 "KREQ DuplicateHandle seq=%u src=0x%x opts=0x%x -> 0x%x %s",
                 job->seq, job->handle, opts, (unsigned)dup, out->ok ? "OK" : "FAIL");
        break;
    }
    case MYOS_KOP_CREATE_FILE_MAPPING: {
        DWORD protect = job->options ? job->options : PAGE_READWRITE;
        DWORD size = job->size ? job->size : 4096u;
        HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, protect, 0, size, job->name[0] ? job->name : NULL);
        out->ok = h ? 1u : 0u;
        out->result = (DWORD)h;
        snprintf(out->status, sizeof(out->status), "KREQ CreateFileMapping seq=%u name=%.24s size=%u -> h=0x%x", job->seq, job->name, (unsigned)size, (unsigned)h);
        break;
    }
    case MYOS_KOP_OPEN_FILE_MAPPING: {
        HANDLE h = OpenFileMappingA(job->access ? job->access : FILE_MAP_ALL_ACCESS, (job->flags & MYOS_KFLAG_INHERIT) ? TRUE : FALSE, job->name);
        out->ok = h ? 1u : 0u;
        out->result = (DWORD)h;
        snprintf(out->status, sizeof(out->status), "KREQ OpenFileMapping seq=%u name=%.28s -> h=0x%x", job->seq, job->name, (unsigned)h);
        break;
    }
    case MYOS_KOP_MAP_VIEW_OF_FILE: {
        DWORD mapBytes = 0, sectionSize = 0, protect = 0;
        char shmName[MYOS_IPC_IMAGE_MAX];
        shmName[0] = 0;
        out->ok = MyWinGetSectionBackingInfo((HANDLE)job->handle, job->access, job->offset_high, job->offset_low, job->size,
                                             shmName, sizeof(shmName), &mapBytes, &sectionSize, &protect) ? 1u : 0u;
        out->result = out->ok ? mapBytes : 0u;
        out->map_size = out->ok ? mapBytes : 0u;
        snprintf(out->map_name, sizeof(out->map_name), "%s", shmName);
        snprintf(out->status, sizeof(out->status), "KREQ MapView seq=%u h=0x%x shm=%.24s bytes=%u -> %s",
                 job->seq, job->handle, shmName, (unsigned)mapBytes, out->ok ? "OK" : "FAIL");
        break;
    }
    case MYOS_KOP_UNMAP_VIEW_OF_FILE:
        out->ok = MyWinReleaseSectionViewHandle((HANDLE)job->handle) ? 1u : 0u;
        out->result = out->ok;
        snprintf(out->status, sizeof(out->status), "KREQ UnmapView seq=%u h=0x%x -> %s", job->seq, job->handle, out->ok ? "OK" : "FAIL");
        break;
    case MYOS_KOP_WAIT_ONE:
        out->result = ph_execute_wait_one_job(job);
        out->ok = (out->result != WAIT_FAILED) ? 1u : 0u;
        if (!out->ok) out->err = ERROR_OPERATION_ABORTED;
        snprintf(out->status, sizeof(out->status), "KREQ WaitOne seq=%u h=0x%x timeout=%u -> 0x%x", job->seq, job->handle, job->timeout, out->result);
        break;
    case MYOS_KOP_WAIT_MANY:
        out->result = ph_execute_wait_many_job(job);
        out->ok = (out->result != WAIT_FAILED) ? 1u : 0u;
        if (!out->ok) out->err = ERROR_OPERATION_ABORTED;
        snprintf(out->status, sizeof(out->status), "KREQ WaitMany seq=%u n=%u all=%u timeout=%u -> 0x%x", job->seq, (unsigned)job->count, job->wait_all, job->timeout, out->result);
        break;
    default:
        out->err = ERROR_INVALID_FUNCTION;
        snprintf(out->status, sizeof(out->status), "KREQ unsupported op=%u seq=%u", (unsigned)job->op, job->seq);
        break;
    }

    if (out->err == ERROR_SUCCESS && !out->ok && out->result == 0) out->err = GetLastError();
    MyWinLeaveProcessContext();
}

static void ph_complete_kernel_job(const MyKernelRequestJob* job, const MyKernelRequestResult* res)
{
    if (!job || !res) return;

    MyProcessIpcMessage ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic = MYOS_IPC_MAGIC;
    ack.version = MYOS_IPC_VERSION;
    ack.opcode = MYOS_IPC_OP_KERNEL_ACK;
    ack.my_pid = job->myPid;
    ack.child_pid = (uint32_t)job->linuxPid;
    ack.value = res->result;
    ack.wparam = res->ok;
    ack.lparam = res->err;
    snprintf(ack.text, sizeof(ack.text), "%s", res->status);

    int fd = -1;
    BOOL sendAck = FALSE;
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(job->myPid);
    if (e && e->shared == job->shared) {
        MyProcessIpcShared* sh = e->shared;
        if (sh && sh->kernel_request == job->seq && sh->kernel_ack != job->seq) {
            sh->kernel_ok = res->ok;
            sh->kernel_error = res->err;
            sh->kernel_result = res->result;
            if (job->op == MYOS_KOP_CREATE_EVENT || job->op == MYOS_KOP_OPEN_EVENT ||
                job->op == MYOS_KOP_CREATE_MUTEX || job->op == MYOS_KOP_CREATE_SEMAPHORE ||
                job->op == MYOS_KOP_DUPLICATE_HANDLE || job->op == MYOS_KOP_CREATE_FILE_MAPPING ||
                job->op == MYOS_KOP_OPEN_FILE_MAPPING)
                sh->kernel_handle = res->result;
            if (job->op == MYOS_KOP_MAP_VIEW_OF_FILE) {
                sh->kernel_map_size = res->map_size;
                snprintf(sh->kernel_map_name, sizeof(sh->kernel_map_name), "%s", res->map_name);
            }
            snprintf(sh->kernel_status, sizeof(sh->kernel_status), "%s", res->status[0] ? res->status : "kernel bridge ACK");
            sh->kernel_ack = job->seq;
            snprintf(e->lastEvent, sizeof(e->lastEvent), "KREQ ack op=%u seq=%u result=0x%x", (unsigned)job->op, job->seq, (unsigned)res->result);
            if (e->ipcEnabled && e->ipcFd >= 0) { fd = e->ipcFd; sendAck = TRUE; }
        } else {
            snprintf(e->lastEvent, sizeof(e->lastEvent), "KREQ stale op=%u seq=%u current=%u", (unsigned)job->op, job->seq, sh ? sh->kernel_request : 0u);
        }
    }
    pthread_mutex_unlock(&g_ProcessHostLock);

    if (sendAck && fd >= 0) (void)send(fd, &ack, sizeof(ack), 0);
}

static void* ph_kernel_request_thread(void* arg)
{
    MyKernelRequestJob* job = (MyKernelRequestJob*)arg;
    if (!job) return NULL;
    MyKernelRequestResult res;
    ph_execute_kernel_job(job, &res);
    ph_complete_kernel_job(job, &res);
    free(job);
    return NULL;
}

static void ph_kernel_ack_spawn_failure_locked(MyProcessHostEntry* e, MyProcessIpcShared* sh, uint32_t seq, DWORD op)
{
    if (!e || !sh) return;
    sh->kernel_ok = 0;
    sh->kernel_error = ERROR_NOT_ENOUGH_MEMORY;
    sh->kernel_result = WAIT_FAILED;
    snprintf(sh->kernel_status, sizeof(sh->kernel_status), "KREQ spawn failed op=%u seq=%u", (unsigned)op, seq);
    sh->kernel_ack = seq;

    MyProcessIpcMessage ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic = MYOS_IPC_MAGIC;
    ack.version = MYOS_IPC_VERSION;
    ack.opcode = MYOS_IPC_OP_KERNEL_ACK;
    ack.my_pid = e->myPid;
    ack.child_pid = (uint32_t)e->linuxPid;
    ack.value = WAIT_FAILED;
    ack.wparam = 0;
    ack.lparam = ERROR_NOT_ENOUGH_MEMORY;
    snprintf(ack.text, sizeof(ack.text), "%s", sh->kernel_status);
    if (e->ipcEnabled && e->ipcFd >= 0) (void)send(e->ipcFd, &ack, sizeof(ack), 0);
}

static void ph_queue_kernel_request_locked(MyProcessHostEntry* e, const MyProcessIpcMessage* msg)
{
    if (!e || !e->shared || !msg) return;
    MyProcessIpcShared* sh = e->shared;
    MyKernelRequestJob* job = (MyKernelRequestJob*)calloc(1, sizeof(*job));
    if (!job) {
        ph_kernel_ack_spawn_failure_locked(e, sh, sh->kernel_request, sh->kernel_op);
        return;
    }

    sh->kernel_enabled = 1;
    job->myPid = e->myPid;
    job->linuxPid = e->linuxPid;
    job->ipcFd = e->ipcFd;
    job->shared = sh;
    job->seq = sh->kernel_request;
    job->op = sh->kernel_op;
    job->handle = sh->kernel_handle;
    job->access = sh->kernel_access;
    job->flags = sh->kernel_flags;
    job->timeout = sh->kernel_timeout;
    job->count = sh->kernel_count;
    if (job->count > 8u) job->count = 8u;
    job->wait_all = sh->kernel_wait_all;
    job->options = sh->kernel_options;
    job->target_pid = sh->kernel_target_pid;
    job->initial = sh->kernel_initial;
    job->maximum = sh->kernel_maximum;
    job->release_count = sh->kernel_release_count;
    job->size = sh->kernel_size;
    job->offset_low = sh->kernel_offset_low;
    job->offset_high = sh->kernel_offset_high;
    for (DWORD i = 0; i < 8u; ++i) job->handles[i] = sh->kernel_handles[i];
    snprintf(job->name, sizeof(job->name), "%s", sh->kernel_name);
    snprintf(sh->kernel_status, sizeof(sh->kernel_status), "KREQ queued op=%u seq=%u", (unsigned)job->op, job->seq);
    snprintf(e->lastEvent, sizeof(e->lastEvent), "KREQ queued op=%u seq=%u", (unsigned)job->op, job->seq);

    pthread_t tid;
    int rc = pthread_create(&tid, NULL, ph_kernel_request_thread, job);
    if (rc != 0) {
        ph_kernel_ack_spawn_failure_locked(e, sh, job->seq, job->op);
        free(job);
        return;
    }
    pthread_detach(tid);
}

static void ph_record_ipc_message_locked(MyProcessHostEntry* e, const MyProcessIpcMessage* msg)
{
    if (!e || !msg) return;
    e->ipcMessages++;
    e->ipcLastOpcode = msg->opcode;
    e->ipcLastValue = msg->value;
    snprintf(e->ipcLastText, sizeof(e->ipcLastText), "%s", msg->text);
    if (msg->opcode == MYOS_IPC_OP_HELLO) e->ipcHello = 1;
    if (msg->opcode == MYOS_IPC_OP_EXIT) {
        e->ipcExitReport = 1;
        e->exitCode = msg->value;
    }
    if (msg->opcode == MYOS_IPC_OP_CREATE_WINDOW) {
        e->guiCreateRequest++;
        e->guiCreateConsumed = 0;
        memset(&e->lastCreate, 0, sizeof(e->lastCreate));
        e->lastCreate.my_pid = e->myPid;
        e->lastCreate.linux_pid = e->linuxPid;
        e->lastCreate.owner_hwnd = (HWND)msg->hwnd;
        e->lastCreate.style = (DWORD)(msg->wparam & 0xffffffffu);
        e->lastCreate.ex_style = (DWORD)((msg->wparam >> 32) & 0xffffffffu);
        if (e->shared) {
            e->lastCreate.x = (int)e->shared->gui_x;
            e->lastCreate.y = (int)e->shared->gui_y;
            e->lastCreate.w = (int)e->shared->gui_w;
            e->lastCreate.h = (int)e->shared->gui_h;
            snprintf(e->guiClass, sizeof(e->guiClass), "%s", e->shared->gui_class);
            snprintf(e->guiTitle, sizeof(e->guiTitle), "%s", e->shared->gui_title);
            snprintf(e->lastCreate.class_name, sizeof(e->lastCreate.class_name), "%s", e->shared->gui_class[0] ? e->shared->gui_class : "myOS.IPCProxyWindow");
            snprintf(e->lastCreate.title, sizeof(e->lastCreate.title), "%s", e->shared->gui_title[0] ? e->shared->gui_title : msg->text);
        } else {
            snprintf(e->guiClass, sizeof(e->guiClass), "%s", "myOS.IPCProxyWindow");
            snprintf(e->guiTitle, sizeof(e->guiTitle), "%s", msg->text);
            snprintf(e->lastCreate.class_name, sizeof(e->lastCreate.class_name), "%s", "myOS.IPCProxyWindow");
            snprintf(e->lastCreate.title, sizeof(e->lastCreate.title), "%s", msg->text);
        }
    }
    if (msg->opcode == MYOS_IPC_OP_WINDOW_DISPATCHED) {
        e->guiMsgDispatched++;
        if (msg->msg == WM_CLOSE) e->guiCloseSeen = 1;
        e->lastWindowMsg.my_pid = e->myPid;
        e->lastWindowMsg.linux_pid = e->linuxPid;
        e->lastWindowMsg.hwnd = (HWND)msg->hwnd;
        e->lastWindowMsg.msg = (UINT)msg->msg;
        e->lastWindowMsg.wparam = (WPARAM)msg->wparam;
        e->lastWindowMsg.lparam = (LPARAM)msg->lparam;
        snprintf(e->lastWindowMsg.text, sizeof(e->lastWindowMsg.text), "%s", msg->text);
    }
    if (msg->opcode == MYOS_IPC_OP_POST_MESSAGE) {
        e->guiPostRequest++;
        e->guiPostConsumed = 0;
        e->lastPost.my_pid = e->myPid;
        e->lastPost.linux_pid = e->linuxPid;
        e->lastPost.hwnd = (HWND)msg->hwnd;
        e->lastPost.msg = (UINT)msg->msg;
        e->lastPost.wparam = (WPARAM)msg->wparam;
        e->lastPost.lparam = (LPARAM)msg->lparam;
        snprintf(e->lastPost.text, sizeof(e->lastPost.text), "%s", msg->text);
    }
    if (msg->opcode == MYOS_IPC_OP_DESTROY_WINDOW) {
        e->guiDestroyRequest++;
        e->guiDestroyConsumed = 0;
        e->lastDestroy.my_pid = e->myPid;
        e->lastDestroy.linux_pid = e->linuxPid;
        e->lastDestroy.hwnd = (HWND)msg->hwnd;
        e->lastDestroy.msg = (UINT)msg->msg;
        e->lastDestroy.wparam = (WPARAM)msg->wparam;
        e->lastDestroy.lparam = (LPARAM)msg->lparam;
        snprintf(e->lastDestroy.text, sizeof(e->lastDestroy.text), "%s", msg->text);
    }
    if (msg->opcode == MYOS_IPC_OP_ENABLE_WINDOW_REQ) {
        e->guiEnableRequest++;
        e->guiEnableConsumed = 0;
        e->lastEnable.my_pid = e->myPid;
        e->lastEnable.linux_pid = e->linuxPid;
        e->lastEnable.hwnd = (HWND)msg->hwnd;
        e->lastEnable.msg = (UINT)msg->msg;
        e->lastEnable.wparam = (WPARAM)msg->wparam;
        e->lastEnable.lparam = (LPARAM)msg->lparam;
        snprintf(e->lastEnable.text, sizeof(e->lastEnable.text), "%s", msg->text);
    }
    if (msg->opcode == MYOS_IPC_OP_CREATE_CHILD_WINDOW) {
        e->childCreateRequest++;
        e->childCreateConsumed = 0;
        memset(&e->lastChildCreate, 0, sizeof(e->lastChildCreate));
        e->lastChildCreate.my_pid = e->myPid;
        e->lastChildCreate.linux_pid = e->linuxPid;
        e->lastChildCreate.parent_hwnd = (HWND)msg->hwnd;
        e->lastChildCreate.id = (UINT)(msg->value & 0xffffu);
        e->lastChildCreate.style = (DWORD)(msg->wparam & 0xffffffffu);
        e->lastChildCreate.ex_style = (DWORD)((msg->wparam >> 32) & 0xffffffffu);
        if (e->shared) {
            e->lastChildCreate.x = (int)e->shared->child_hwnd_x[0];
            e->lastChildCreate.y = (int)e->shared->child_hwnd_y[0];
            e->lastChildCreate.w = (int)e->shared->child_hwnd_w[0];
            e->lastChildCreate.h = (int)e->shared->child_hwnd_h[0];
            snprintf(e->lastChildCreate.class_name, sizeof(e->lastChildCreate.class_name), "%s", e->shared->child_hwnd_class[0]);
            snprintf(e->lastChildCreate.title, sizeof(e->lastChildCreate.title), "%s", e->shared->child_hwnd_text[0]);
        }
    }
    if (msg->opcode == MYOS_IPC_OP_CREATE_CHILD_WINDOW_BATCH) {
        e->childBatchCreateRequest++;
        e->childBatchCreateConsumed = 0;
        memset(&e->lastChildBatch, 0, sizeof(e->lastChildBatch));
        e->lastChildBatch.my_pid = e->myPid;
        e->lastChildBatch.linux_pid = e->linuxPid;
        UINT count = (UINT)msg->value;
        if (count > MYOS_IPC_MAX_CHILD_CONTROLS) count = MYOS_IPC_MAX_CHILD_CONTROLS;
        e->lastChildBatch.count = count;
        if (e->shared) {
            for (UINT i = 0; i < count; ++i) {
                MyProcessHostCreateChildWindowRequest* r = &e->lastChildBatch.items[i];
                memset(r, 0, sizeof(*r));
                r->my_pid = e->myPid;
                r->linux_pid = e->linuxPid;
                r->parent_hwnd = (HWND)msg->hwnd;
                r->id = (UINT)(e->shared->child_hwnd_ids[i] & 0xffffu);
                r->style = (DWORD)e->shared->child_hwnd_style[i];
                r->ex_style = (DWORD)e->shared->child_hwnd_ex_style[i];
                r->x = (int)e->shared->child_hwnd_x[i];
                r->y = (int)e->shared->child_hwnd_y[i];
                r->w = (int)e->shared->child_hwnd_w[i];
                r->h = (int)e->shared->child_hwnd_h[i];
                snprintf(r->class_name, sizeof(r->class_name), "%s", e->shared->child_hwnd_class[i]);
                snprintf(r->title, sizeof(r->title), "%s", e->shared->child_hwnd_text[i]);
            }
        }
        snprintf(e->lastEvent, sizeof(e->lastEvent), "ipc child batch count=%u parent=%u", (unsigned)count, (unsigned)msg->hwnd);
    }
    if (msg->opcode == MYOS_IPC_OP_CLIPBOARD_REQUEST) {
        ph_handle_clipboard_request_locked(e, msg);
    }
    if (msg->opcode == MYOS_IPC_OP_MENU_REQUEST) {
        ph_handle_menu_request_locked(e, msg);
    }
    if (msg->opcode == MYOS_IPC_OP_KERNEL_REQUEST) {
        ph_queue_kernel_request_locked(e, msg);
    }
    snprintf(e->lastEvent, sizeof(e->lastEvent), "ipc op=%u val=%u text=%.40s", msg->opcode, msg->value, msg->text);
}

static void ph_drain_ipc_locked(MyProcessHostEntry* e)
{
    if (!e || !e->ipcEnabled || e->ipcFd < 0) return;
    for (;;) {
        MyProcessIpcMessage msg;
        ssize_t n = recv(e->ipcFd, &msg, sizeof(msg), MSG_DONTWAIT);
        if (n == (ssize_t)sizeof(msg)) {
            if (msg.magic == MYOS_IPC_MAGIC && msg.version == MYOS_IPC_VERSION) ph_record_ipc_message_locked(e, &msg);
            continue;
        }
        if (n == 0) break;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

BOOL MyProcessHostTrack(DWORD dwProcessId, int nLinuxPid, LPCSTR lpImageName)
{
    if (!dwProcessId || nLinuxPid <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_alloc_locked(dwProcessId);
    if (!e) {
        pthread_mutex_unlock(&g_ProcessHostLock);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    e->myPid = dwProcessId;
    e->linuxPid = nLinuxPid;
    e->state = MYPROCESSHOST_STATE_RUNNING;
    e->exitCode = STILL_ACTIVE;
    e->rawStatus = 0;
    e->pollCount = 0;
    e->reapCount = 0;
    e->killCount = 0;
    e->cleanupCount = 0;
    e->cleanupErrorCount = 0;
    e->resourcesClosed = 0;
    e->startMs = ph_now_ms();
    e->exitMs = 0;
    snprintf(e->imageName, sizeof(e->imageName), "%s", lpImageName && lpImageName[0] ? lpImageName : "process");
    snprintf(e->lastEvent, sizeof(e->lastEvent), "track linux-pid=%d", nLinuxPid);
    pthread_mutex_unlock(&g_ProcessHostLock);
    ph_reaper_ensure();
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static BOOL ph_spawn_child_with_mode(DWORD dwProcessId,
                                     LPCSTR lpChildExePath,
                                     LPCSTR lpDirectory,
                                     LPCSTR lpImageName,
                                     LPCSTR lpMode,
                                     int argc,
                                     char* const argv[],
                                     int* outLinuxPid)
{
    if (outLinuxPid) *outLinuxPid = 0;
    if (!dwProcessId || !lpChildExePath || !lpChildExePath[0] || !lpMode || !lpMode[0]) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!ph_directory_is_usable(lpDirectory)) return FALSE;

    int sv[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    (void)ph_set_nonblocking(sv[0]);

    char shmName[96];
    snprintf(shmName, sizeof(shmName), "/myos_v73_%u_%lu", (unsigned)dwProcessId, (unsigned long)ph_now_ms());
    int shmFd = shm_open(shmName, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shmFd < 0) {
        close(sv[0]); close(sv[1]);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    /* v184: child reopens by shared-name; the inherited shm fd must not leak
       through exec into myos_apphost_child. */
    (void)fcntl(shmFd, F_SETFD, FD_CLOEXEC);
    if (ftruncate(shmFd, (off_t)sizeof(MyProcessIpcShared)) != 0) {
        close(sv[0]); close(sv[1]); close(shmFd); shm_unlink(shmName);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    MyProcessIpcShared* shared = (MyProcessIpcShared*)mmap(NULL, sizeof(MyProcessIpcShared), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (shared == MAP_FAILED) {
        close(sv[0]); close(sv[1]); close(shmFd); shm_unlink(shmName);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    memset(shared, 0, sizeof(*shared));
    shared->magic = MYOS_IPC_SHARED_MAGIC;
    shared->version = MYOS_IPC_SHARED_VERSION;
    shared->my_pid = dwProcessId;
    snprintf(shared->image, sizeof(shared->image), "%s", lpImageName && lpImageName[0] ? lpImageName : "child");
    snprintf(shared->status, sizeof(shared->status), "parent-created");

    char ipcFdStr[24];
    snprintf(ipcFdStr, sizeof(ipcFdStr), "%d", sv[1]);

    char* childArgv[MYPROCESSHOST_MAX_ARGC + 9];
    int ai = 0;
    childArgv[ai++] = (char*)lpChildExePath;
    childArgv[ai++] = "--ipc-fd";
    childArgv[ai++] = ipcFdStr;
    childArgv[ai++] = "--shared-name";
    childArgv[ai++] = shmName;
    childArgv[ai++] = "--my-pid";
    char myPidStr[24];
    snprintf(myPidStr, sizeof(myPidStr), "%u", (unsigned)dwProcessId);
    childArgv[ai++] = myPidStr;
    childArgv[ai++] = (char*)lpMode;
    childArgv[ai++] = (char*)(lpImageName && lpImageName[0] ? lpImageName : "child");
    for (int i = 0; i < argc && argv && ai < (int)(sizeof(childArgv)/sizeof(childArgv[0])) - 1; i++)
        childArgv[ai++] = argv[i];
    childArgv[ai] = NULL;

    pid_t cpid = fork();
    if (cpid < 0) {
        close(sv[0]); close(sv[1]); munmap(shared, sizeof(*shared)); close(shmFd); shm_unlink(shmName);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (cpid == 0) {
        close(sv[0]);
        ph_child_chdir_or_exit(lpDirectory);
        execv(lpChildExePath, childArgv);
        fprintf(stderr, "[v73 processhost child] execv('%s') failed: %s\n", lpChildExePath, strerror(errno));
        _exit(126);
    }

    close(sv[1]);
    if (!MyProcessHostTrack(dwProcessId, (int)cpid, lpImageName)) {
        /* v184: if the internal ProcessHost table cannot commit the child,
           rollback the already-forked Linux process instead of leaking it. */
        (void)kill(cpid, SIGKILL);
        (void)waitpid(cpid, NULL, 0);
        close(sv[0]); munmap(shared, sizeof(*shared)); close(shmFd); shm_unlink(shmName);
        return FALSE;
    }

    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (e) {
        e->ipcEnabled = 1;
        e->ipcFd = sv[0];
        e->sharedFd = shmFd;
        e->shared = shared;
        snprintf(e->sharedName, sizeof(e->sharedName), "%s", shmName);
        snprintf(e->lastEvent, sizeof(e->lastEvent), "fork/exec pid=%d ipc-fd=%d shm=v73", (int)cpid, sv[0]);
    }
    pthread_mutex_unlock(&g_ProcessHostLock);

    if (outLinuxPid) *outLinuxPid = (int)cpid;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostSpawnConsole(DWORD dwProcessId,
                               LPCSTR lpChildExePath,
                               LPCSTR lpDirectory,
                               LPCSTR lpImageName,
                               int argc,
                               char* const argv[],
                               int* outLinuxPid)
{
    if (outLinuxPid) *outLinuxPid = 0;
    if (!dwProcessId || !lpChildExePath || !lpChildExePath[0]) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!ph_directory_is_usable(lpDirectory)) return FALSE;

    int sv[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    (void)ph_set_nonblocking(sv[0]);

    char shmName[96];
    snprintf(shmName, sizeof(shmName), "/myos_v73_%u_%lu", (unsigned)dwProcessId, (unsigned long)ph_now_ms());
    int shmFd = shm_open(shmName, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shmFd < 0) {
        close(sv[0]); close(sv[1]);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    /* v184: child reopens by shared-name; the inherited shm fd must not leak
       through exec into myos_apphost_child. */
    (void)fcntl(shmFd, F_SETFD, FD_CLOEXEC);
    if (ftruncate(shmFd, (off_t)sizeof(MyProcessIpcShared)) != 0) {
        close(sv[0]); close(sv[1]); close(shmFd); shm_unlink(shmName);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    MyProcessIpcShared* shared = (MyProcessIpcShared*)mmap(NULL, sizeof(MyProcessIpcShared), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (shared == MAP_FAILED) {
        close(sv[0]); close(sv[1]); close(shmFd); shm_unlink(shmName);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    memset(shared, 0, sizeof(*shared));
    shared->magic = MYOS_IPC_SHARED_MAGIC;
    shared->version = MYOS_IPC_SHARED_VERSION;
    shared->my_pid = dwProcessId;
    snprintf(shared->image, sizeof(shared->image), "%s", lpImageName && lpImageName[0] ? lpImageName : "console-child");
    snprintf(shared->status, sizeof(shared->status), "parent-created");

    char ipcFdStr[24];
    snprintf(ipcFdStr, sizeof(ipcFdStr), "%d", sv[1]);

    char* childArgv[MYPROCESSHOST_MAX_ARGC + 9];
    int ai = 0;
    childArgv[ai++] = (char*)lpChildExePath;
    childArgv[ai++] = "--ipc-fd";
    childArgv[ai++] = ipcFdStr;
    childArgv[ai++] = "--shared-name";
    childArgv[ai++] = shmName;
    childArgv[ai++] = "--my-pid";
    char myPidStr[24];
    snprintf(myPidStr, sizeof(myPidStr), "%u", (unsigned)dwProcessId);
    childArgv[ai++] = myPidStr;
    childArgv[ai++] = "--console";
    childArgv[ai++] = (char*)(lpImageName && lpImageName[0] ? lpImageName : "console-child");
    for (int i = 0; i < argc && argv && ai < (int)(sizeof(childArgv)/sizeof(childArgv[0])) - 1; i++)
        childArgv[ai++] = argv[i];
    childArgv[ai] = NULL;

    pid_t cpid = fork();
    if (cpid < 0) {
        close(sv[0]); close(sv[1]); munmap(shared, sizeof(*shared)); close(shmFd); shm_unlink(shmName);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (cpid == 0) {
        close(sv[0]);
        ph_child_chdir_or_exit(lpDirectory);
        execv(lpChildExePath, childArgv);
        fprintf(stderr, "[v73 processhost child] execv('%s') failed: %s\n", lpChildExePath, strerror(errno));
        _exit(126);
    }

    close(sv[1]);
    if (!MyProcessHostTrack(dwProcessId, (int)cpid, lpImageName)) {
        /* v184: if the internal ProcessHost table cannot commit the child,
           rollback the already-forked Linux process instead of leaking it. */
        (void)kill(cpid, SIGKILL);
        (void)waitpid(cpid, NULL, 0);
        close(sv[0]); munmap(shared, sizeof(*shared)); close(shmFd); shm_unlink(shmName);
        return FALSE;
    }

    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (e) {
        e->ipcEnabled = 1;
        e->ipcFd = sv[0];
        e->sharedFd = shmFd;
        e->shared = shared;
        snprintf(e->sharedName, sizeof(e->sharedName), "%s", shmName);
        snprintf(e->lastEvent, sizeof(e->lastEvent), "fork/exec pid=%d ipc-fd=%d shm=v73", (int)cpid, sv[0]);
    }
    pthread_mutex_unlock(&g_ProcessHostLock);

    if (outLinuxPid) *outLinuxPid = (int)cpid;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}


BOOL MyProcessHostSpawnGui(DWORD dwProcessId,
                           LPCSTR lpChildExePath,
                           LPCSTR lpDirectory,
                           LPCSTR lpImageName,
                           LPCSTR lpTitle,
                           LPCSTR lpPath,
                           int x,
                           int y,
                           int w,
                           int h,
                           int* outLinuxPid)
{
    char argTitle[96], argX[24], argY[24], argW[24], argH[24], argPath[512];
    snprintf(argTitle, sizeof(argTitle), "%s", lpTitle && lpTitle[0] ? lpTitle : "myOS IPC GUI Child");
    snprintf(argX, sizeof(argX), "%d", x);
    snprintf(argY, sizeof(argY), "%d", y);
    snprintf(argW, sizeof(argW), "%d", w);
    snprintf(argH, sizeof(argH), "%d", h);
    snprintf(argPath, sizeof(argPath), "%s", lpPath && lpPath[0] ? lpPath : "");
    char* argv[6] = { argTitle, argX, argY, argW, argH, argPath };
    return ph_spawn_child_with_mode(dwProcessId, lpChildExePath, lpDirectory, lpImageName, "--gui", 6, argv, outLinuxPid);
}

BOOL MyProcessHostTakeCreateWindowRequest(DWORD dwProcessId, MyProcessHostCreateWindowRequest* lpRequest)
{
    if (!lpRequest) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(lpRequest, 0, sizeof(*lpRequest));

    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ph_drain_ipc_locked(e);
    if (!e->guiCreateRequest || e->guiCreateConsumed || !e->shared || !e->shared->gui_request) {
        pthread_mutex_unlock(&g_ProcessHostLock);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    *lpRequest = e->lastCreate;
    lpRequest->my_pid = e->myPid;
    lpRequest->linux_pid = e->linuxPid;
    lpRequest->x = (int)e->shared->gui_x;
    lpRequest->y = (int)e->shared->gui_y;
    lpRequest->w = (int)e->shared->gui_w;
    lpRequest->h = (int)e->shared->gui_h;
    snprintf(lpRequest->class_name, sizeof(lpRequest->class_name), "%s", e->shared->gui_class[0] ? e->shared->gui_class : (lpRequest->class_name[0] ? lpRequest->class_name : "myOS.IPCProxyWindow"));
    snprintf(lpRequest->title, sizeof(lpRequest->title), "%s", e->shared->gui_title[0] ? e->shared->gui_title : (lpRequest->title[0] ? lpRequest->title : "myOS IPC GUI Child"));
    e->guiCreateConsumed = 1;
    snprintf(e->guiClass, sizeof(e->guiClass), "%s", lpRequest->class_name);
    snprintf(e->guiTitle, sizeof(e->guiTitle), "%s", lpRequest->title);
    snprintf(e->lastEvent, sizeof(e->lastEvent), "take CREATE_WINDOW %.40s", lpRequest->title);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostAckCreateWindow(DWORD dwProcessId, HWND hwnd, DWORD windowIndex, BOOL ok, LPCSTR lpText)
{
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (e->shared) {
        e->shared->gui_ack = ok ? 1u : 2u;
        e->shared->gui_hwnd = (uint32_t)hwnd;
        e->shared->gui_window_index = windowIndex;
        snprintf(e->shared->status, sizeof(e->shared->status), "%s", ok ? "parent-created-window" : "parent-create-window-failed");
    }
    e->guiCreateAck = ok ? 1u : 2u;
    e->guiHwnd = (DWORD)hwnd;
    e->guiWindowIndex = windowIndex;
    MyProcessIpcMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = MYOS_IPC_MAGIC;
    msg.version = MYOS_IPC_VERSION;
    msg.opcode = ok ? MYOS_IPC_OP_WINDOW_ACK : MYOS_IPC_OP_WINDOW_FAIL;
    msg.my_pid = dwProcessId;
    msg.child_pid = (uint32_t)e->linuxPid;
    msg.value = (uint32_t)hwnd;
    snprintf(msg.text, sizeof(msg.text), "%s", lpText && lpText[0] ? lpText : (ok ? "CreateWindowExA ACK" : "CreateWindowExA failed"));
    if (e->ipcEnabled && e->ipcFd >= 0) (void)send(e->ipcFd, &msg, sizeof(msg), 0);
    snprintf(e->lastEvent, sizeof(e->lastEvent), "CREATE_WINDOW %s hwnd=%u idx=%u", ok ? "ack" : "fail", (unsigned)hwnd, (unsigned)windowIndex);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}


BOOL MyProcessHostUpdateGuiRect(DWORD dwProcessId, int x, int y, int w, int h)
{
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e || !e->shared) {
        pthread_mutex_unlock(&g_ProcessHostLock);
        return FALSE;
    }
    e->shared->gui_x = (uint32_t)x;
    e->shared->gui_y = (uint32_t)y;
    e->shared->gui_w = (uint32_t)w;
    e->shared->gui_h = (uint32_t)h;
    pthread_mutex_unlock(&g_ProcessHostLock);
    return TRUE;
}

BOOL MyProcessHostSendWindowMessage(DWORD dwProcessId, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LPCSTR lpText)
{
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!e->ipcEnabled || e->ipcFd < 0) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_FUNCTION); return FALSE; }

    MyProcessIpcMessage m;
    memset(&m, 0, sizeof(m));
    m.magic = MYOS_IPC_MAGIC;
    m.version = MYOS_IPC_VERSION;
    m.opcode = MYOS_IPC_OP_WINDOW_MESSAGE;
    m.my_pid = dwProcessId;
    m.child_pid = (uint32_t)e->linuxPid;
    m.value = (uint32_t)msg;
    m.hwnd = (uint32_t)hwnd;
    m.msg = (uint32_t)msg;
    m.wparam = (uint64_t)(uintptr_t)wParam;
    m.lparam = (uint64_t)(intptr_t)lParam;
    snprintf(m.text, sizeof(m.text), "%s", lpText && lpText[0] ? lpText : "parent queued MSG");
    int r = (int)send(e->ipcFd, &m, sizeof(m), 0);
    if (r == (int)sizeof(m)) {
        e->guiMsgSent++;
        e->lastWindowMsg.my_pid = e->myPid;
        e->lastWindowMsg.linux_pid = e->linuxPid;
        e->lastWindowMsg.hwnd = hwnd;
        e->lastWindowMsg.msg = msg;
        e->lastWindowMsg.wparam = wParam;
        e->lastWindowMsg.lparam = lParam;
        snprintf(e->lastWindowMsg.text, sizeof(e->lastWindowMsg.text), "%s", m.text);
        if (e->shared) {
            e->shared->gui_msg_sent = e->guiMsgSent;
            e->shared->gui_last_hwnd = (uint32_t)hwnd;
            e->shared->gui_last_msg = (uint32_t)msg;
            e->shared->gui_last_wparam = (uint64_t)(uintptr_t)wParam;
            e->shared->gui_last_lparam = (uint64_t)(intptr_t)lParam;
            snprintf(e->shared->gui_last_text, sizeof(e->shared->gui_last_text), "%s", m.text);
        }
        snprintf(e->lastEvent, sizeof(e->lastEvent), "send MSG hwnd=%u msg=0x%04x", (unsigned)hwnd, (unsigned)msg);
        pthread_mutex_unlock(&g_ProcessHostLock);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_INVALID_FUNCTION);
    return FALSE;
}

BOOL MyProcessHostTakeCreateChildWindowRequest(DWORD dwProcessId, MyProcessHostCreateChildWindowRequest* lpRequest)
{
    if (!lpRequest) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(lpRequest, 0, sizeof(*lpRequest));
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ph_drain_ipc_locked(e);
    if (!e->childCreateRequest || e->childCreateConsumed) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_FUNCTION); return FALSE; }
    *lpRequest = e->lastChildCreate;
    e->childCreateConsumed = 1;
    snprintf(e->lastEvent, sizeof(e->lastEvent), "take CHILD_CREATE parent=%u id=%u class=%.24s", (unsigned)lpRequest->parent_hwnd, (unsigned)lpRequest->id, lpRequest->class_name);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostAckCreateChildWindow(DWORD dwProcessId, HWND hwnd, UINT id, BOOL ok, LPCSTR lpText)
{
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (e->shared) {
        e->shared->child_hwnd_ack = ok ? 1u : 2u;
        e->shared->child_hwnd_last = (uint32_t)hwnd;
        e->shared->child_hwnd_last_id = id;
        if (ok) {
            uint32_t slot = e->shared->child_hwnd_created;
            if (slot < MYOS_IPC_MAX_CHILD_CONTROLS) {
                e->shared->child_hwnd_hwnds[slot] = (uint32_t)hwnd;
                e->shared->child_hwnd_ids[slot] = id;
            }
            e->shared->child_hwnd_created++;
            e->shared->child_hwnd_count = e->shared->child_hwnd_created;
        }
        snprintf(e->shared->child_hwnd_status, sizeof(e->shared->child_hwnd_status), "%s", lpText && lpText[0] ? lpText : (ok ? "child HWND ACK" : "child HWND failed"));
    }
    e->childCreateAck = ok ? 1u : 2u;
    if (ok) e->childCreated++;
    MyProcessIpcMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = MYOS_IPC_MAGIC;
    msg.version = MYOS_IPC_VERSION;
    msg.opcode = MYOS_IPC_OP_CHILD_WINDOW_ACK;
    msg.my_pid = dwProcessId;
    msg.child_pid = (uint32_t)e->linuxPid;
    msg.value = (uint32_t)hwnd;
    msg.hwnd = (uint32_t)hwnd;
    msg.msg = WM_CREATE;
    msg.wparam = (uint64_t)id;
    snprintf(msg.text, sizeof(msg.text), "%s", lpText && lpText[0] ? lpText : (ok ? "CreateWindowExA child ACK" : "CreateWindowExA child failed"));
    if (e->ipcEnabled && e->ipcFd >= 0) (void)send(e->ipcFd, &msg, sizeof(msg), 0);
    snprintf(e->lastEvent, sizeof(e->lastEvent), "CHILD_CREATE %s hwnd=%u id=%u", ok ? "ack" : "fail", (unsigned)hwnd, (unsigned)id);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostTakeCreateChildWindowBatchRequest(DWORD dwProcessId, MyProcessHostCreateChildWindowBatchRequest* lpRequest)
{
    if (!lpRequest) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(lpRequest, 0, sizeof(*lpRequest));
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ph_drain_ipc_locked(e);
    if (!e->childBatchCreateRequest || e->childBatchCreateConsumed || !e->lastChildBatch.count) {
        pthread_mutex_unlock(&g_ProcessHostLock);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    *lpRequest = e->lastChildBatch;
    e->childBatchCreateConsumed = 1;
    snprintf(e->lastEvent, sizeof(e->lastEvent), "take CHILD_BATCH_CREATE count=%u parent=%u",
             (unsigned)lpRequest->count, (unsigned)(lpRequest->count ? lpRequest->items[0].parent_hwnd : 0));
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostAckCreateChildWindowBatch(DWORD dwProcessId, const HWND* hwnds, const UINT* ids, UINT count, BOOL ok, LPCSTR lpText)
{
    if (count > MYOS_IPC_MAX_CHILD_CONTROLS) count = MYOS_IPC_MAX_CHILD_CONTROLS;
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    UINT made = 0;
    HWND last = 0;
    if (e->shared) {
        e->shared->child_hwnd_ack = ok ? 1u : 2u;
        e->shared->child_hwnd_count = count;
        e->shared->child_hwnd_last = 0;
        e->shared->child_hwnd_last_id = 0;
        for (UINT i = 0; i < count; ++i) {
            HWND h = hwnds ? hwnds[i] : 0;
            UINT id = ids ? ids[i] : 0;
            e->shared->child_hwnd_hwnds[i] = (uint32_t)h;
            e->shared->child_hwnd_ids[i] = id;
            if (h) { made++; last = h; e->shared->child_hwnd_last = (uint32_t)h; e->shared->child_hwnd_last_id = id; }
        }
        e->shared->child_hwnd_created += made;
        snprintf(e->shared->child_hwnd_status, sizeof(e->shared->child_hwnd_status), "%s",
                 lpText && lpText[0] ? lpText : (ok ? "child HWND batch ACK" : "child HWND batch failed"));
    }
    e->childBatchCreateAck = ok ? 1u : 2u;
    e->childCreated += made;
    MyProcessIpcMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = MYOS_IPC_MAGIC;
    msg.version = MYOS_IPC_VERSION;
    msg.opcode = MYOS_IPC_OP_CHILD_WINDOW_BATCH_ACK;
    msg.my_pid = dwProcessId;
    msg.child_pid = (uint32_t)e->linuxPid;
    msg.value = (uint32_t)count;
    msg.hwnd = (uint32_t)last;
    msg.msg = WM_CREATE;
    msg.wparam = (uint64_t)made;
    snprintf(msg.text, sizeof(msg.text), "%s", lpText && lpText[0] ? lpText : (ok ? "CreateWindowExA child batch ACK" : "CreateWindowExA child batch failed"));
    if (e->ipcEnabled && e->ipcFd >= 0) (void)send(e->ipcFd, &msg, sizeof(msg), 0);
    snprintf(e->lastEvent, sizeof(e->lastEvent), "CHILD_BATCH_CREATE %s made=%u/%u", ok ? "ack" : "fail", (unsigned)made, (unsigned)count);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostTakePostMessageRequest(DWORD dwProcessId, MyProcessHostWindowMessage* lpRequest)
{
    if (!lpRequest) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(lpRequest, 0, sizeof(*lpRequest));
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ph_drain_ipc_locked(e);
    if (!e->guiPostRequest || e->guiPostConsumed) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_FUNCTION); return FALSE; }
    *lpRequest = e->lastPost;
    e->guiPostConsumed = 1;
    snprintf(e->lastEvent, sizeof(e->lastEvent), "take POST hwnd=%u msg=0x%04x", (unsigned)lpRequest->hwnd, (unsigned)lpRequest->msg);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostAckPostMessage(DWORD dwProcessId, BOOL ok, LPCSTR lpText)
{
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (e->shared) {
        e->shared->gui_post_ack = ok ? 1u : 2u;
        snprintf(e->shared->gui_last_text, sizeof(e->shared->gui_last_text), "%s", lpText && lpText[0] ? lpText : (ok ? "PostMessage ACK" : "PostMessage failed"));
    }
    e->guiPostAck = ok ? 1u : 2u;
    MyProcessIpcMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = MYOS_IPC_MAGIC;
    msg.version = MYOS_IPC_VERSION;
    msg.opcode = MYOS_IPC_OP_POST_ACK;
    msg.my_pid = dwProcessId;
    msg.child_pid = (uint32_t)e->linuxPid;
    msg.value = ok ? 1u : 0u;
    snprintf(msg.text, sizeof(msg.text), "%s", lpText && lpText[0] ? lpText : (ok ? "PostMessage ACK" : "PostMessage failed"));
    if (e->ipcEnabled && e->ipcFd >= 0) (void)send(e->ipcFd, &msg, sizeof(msg), 0);
    snprintf(e->lastEvent, sizeof(e->lastEvent), "POST %s", ok ? "ack" : "fail");
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}


BOOL MyProcessHostTakeDestroyWindowRequest(DWORD dwProcessId, MyProcessHostWindowMessage* lpRequest)
{
    if (!lpRequest) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(lpRequest, 0, sizeof(*lpRequest));
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ph_drain_ipc_locked(e);
    if (!e->guiDestroyRequest || e->guiDestroyConsumed) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_FUNCTION); return FALSE; }
    *lpRequest = e->lastDestroy;
    e->guiDestroyConsumed = 1;
    snprintf(e->lastEvent, sizeof(e->lastEvent), "take DESTROY hwnd=%u", (unsigned)lpRequest->hwnd);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostAckDestroyWindow(DWORD dwProcessId, BOOL ok, LPCSTR lpText)
{
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (e->shared) {
        e->shared->gui_destroy_ack = ok ? 1u : 2u;
        snprintf(e->shared->gui_last_text, sizeof(e->shared->gui_last_text), "%s", lpText && lpText[0] ? lpText : (ok ? "DestroyWindow ACK" : "DestroyWindow failed"));
    }
    e->guiDestroyAck = ok ? 1u : 2u;
    MyProcessIpcMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = MYOS_IPC_MAGIC;
    msg.version = MYOS_IPC_VERSION;
    msg.opcode = MYOS_IPC_OP_DESTROY_ACK;
    msg.my_pid = dwProcessId;
    msg.child_pid = (uint32_t)e->linuxPid;
    msg.value = ok ? 1u : 0u;
    snprintf(msg.text, sizeof(msg.text), "%s", lpText && lpText[0] ? lpText : (ok ? "DestroyWindow ACK" : "DestroyWindow failed"));
    if (e->ipcEnabled && e->ipcFd >= 0) (void)send(e->ipcFd, &msg, sizeof(msg), 0);
    snprintf(e->lastEvent, sizeof(e->lastEvent), "DESTROY %s", ok ? "ack" : "fail");
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostTakeEnableWindowRequest(DWORD dwProcessId, MyProcessHostWindowMessage* lpRequest)
{
    if (!lpRequest) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(lpRequest, 0, sizeof(*lpRequest));
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ph_drain_ipc_locked(e);
    if (!e->guiEnableRequest || e->guiEnableConsumed) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_FUNCTION); return FALSE; }
    *lpRequest = e->lastEnable;
    e->guiEnableConsumed = 1;
    snprintf(e->lastEvent, sizeof(e->lastEvent), "take ENABLE hwnd=%u enable=%u", (unsigned)lpRequest->hwnd, (unsigned)lpRequest->wparam);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostAckEnableWindow(DWORD dwProcessId, BOOL ok, LPCSTR lpText)
{
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (e->shared) {
        snprintf(e->shared->gui_last_text, sizeof(e->shared->gui_last_text), "%s", lpText && lpText[0] ? lpText : (ok ? "EnableWindow ACK" : "EnableWindow failed"));
    }
    e->guiEnableAck = ok ? 1u : 2u;
    MyProcessIpcMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = MYOS_IPC_MAGIC;
    msg.version = MYOS_IPC_VERSION;
    msg.opcode = MYOS_IPC_OP_ENABLE_WINDOW_ACK;
    msg.my_pid = dwProcessId;
    msg.child_pid = (uint32_t)e->linuxPid;
    msg.value = ok ? 1u : 0u;
    snprintf(msg.text, sizeof(msg.text), "%s", lpText && lpText[0] ? lpText : (ok ? "EnableWindow ACK" : "EnableWindow failed"));
    if (e->ipcEnabled && e->ipcFd >= 0) (void)send(e->ipcFd, &msg, sizeof(msg), 0);
    snprintf(e->lastEvent, sizeof(e->lastEvent), "ENABLE %s", ok ? "ack" : "fail");
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostPoll(DWORD dwProcessId, BOOL* lpExited, DWORD* lpExitCode, DWORD* lpRawStatus)
{
    int osPid = 0;
    DWORD alreadyFinal = 0;
    if (lpExited) *lpExited = FALSE;
    if (lpExitCode) *lpExitCode = STILL_ACTIVE;
    if (lpRawStatus) *lpRawStatus = 0;

    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    e->pollCount++;
    ph_drain_ipc_locked(e);
    osPid = e->linuxPid;
    alreadyFinal = ph_state_is_final(e->state) ? 1u : 0u;
    if (alreadyFinal) {
        ph_cleanup_ipc_resources_locked(e, "already-final poll");
        if (lpExited) *lpExited = TRUE;
        if (lpExitCode) *lpExitCode = e->exitCode;
        if (lpRawStatus) *lpRawStatus = e->rawStatus;
        pthread_mutex_unlock(&g_ProcessHostLock);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    pthread_mutex_unlock(&g_ProcessHostLock);

    int status = 0;
    pid_t r = waitpid((pid_t)osPid, &status, WNOHANG);

    pthread_mutex_lock(&g_ProcessHostLock);
    e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ph_drain_ipc_locked(e);

    if (r == (pid_t)osPid) {
        e->state = MYPROCESSHOST_STATE_REAPED;
        e->rawStatus = (DWORD)status;
        DWORD waitExit = ph_exit_code_from_wait_status(status);
        e->exitCode = e->ipcExitReport ? e->exitCode : waitExit;
        e->exitMs = ph_now_ms();
        e->reapCount++;
        ph_drain_ipc_locked(e);
        snprintf(e->lastEvent, sizeof(e->lastEvent), "waitpid reaped status=0x%x exit=%u ipc=%u", (unsigned)status, e->exitCode, e->ipcMessages);
        ph_cleanup_ipc_resources_locked(e, "waitpid reaped");
        if (lpExited) *lpExited = TRUE;
        if (lpExitCode) *lpExitCode = e->exitCode;
        if (lpRawStatus) *lpRawStatus = e->rawStatus;
    } else if (r == 0) {
        if (lpExited) *lpExited = FALSE;
        if (lpExitCode) *lpExitCode = STILL_ACTIVE;
        if (lpRawStatus) *lpRawStatus = 0;
    } else if (errno == ECHILD) {
        e->state = MYPROCESSHOST_STATE_LOST;
        e->rawStatus = 0;
        e->exitCode = e->ipcExitReport ? e->exitCode : 0;
        e->exitMs = ph_now_ms();
        snprintf(e->lastEvent, sizeof(e->lastEvent), "waitpid ECHILD: already reaped/lost ipc=%u", e->ipcMessages);
        ph_cleanup_ipc_resources_locked(e, "waitpid ECHILD");
        if (lpExited) *lpExited = TRUE;
        if (lpExitCode) *lpExitCode = e->exitCode;
        if (lpRawStatus) *lpRawStatus = e->rawStatus;
    } else {
        snprintf(e->lastEvent, sizeof(e->lastEvent), "waitpid errno=%d", errno);
        pthread_mutex_unlock(&g_ProcessHostLock);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD MyProcessHostPollAll(void)
{
    DWORD pids[MYPROCESSHOST_MAX];
    DWORD n = 0;
    pthread_mutex_lock(&g_ProcessHostLock);
    for (int i = 0; i < MYPROCESSHOST_MAX && n < MYPROCESSHOST_MAX; i++) {
        if (g_ProcessHost[i].valid && g_ProcessHost[i].state == MYPROCESSHOST_STATE_RUNNING)
            pids[n++] = g_ProcessHost[i].myPid;
    }
    pthread_mutex_unlock(&g_ProcessHostLock);

    DWORD reaped = 0;
    for (DWORD i = 0; i < n; i++) {
        BOOL exited = FALSE;
        if (MyProcessHostPoll(pids[i], &exited, NULL, NULL) && exited) reaped++;
    }
    (void)MyProcessHostSweepFinalizedResources();
    return reaped;
}

DWORD MyProcessHostSweepFinalizedResources(void)
{
    DWORD cleaned = 0;
    pthread_mutex_lock(&g_ProcessHostLock);
    for (int i = 0; i < MYPROCESSHOST_MAX; i++) {
        MyProcessHostEntry* e = &g_ProcessHost[i];
        if (!e->valid || !ph_state_is_final(e->state)) continue;
        if (ph_entry_has_ipc_resources_locked(e)) {
            ph_cleanup_ipc_resources_locked(e, "sweep finalized");
            cleaned++;
        }
    }
    pthread_mutex_unlock(&g_ProcessHostLock);
    return cleaned;
}

BOOL MyProcessHostTerminate(DWORD dwProcessId, DWORD* lpRawStatus)
{
    int osPid = 0;
    DWORD alreadyFinal = 0;
    if (lpRawStatus) *lpRawStatus = 0;

    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) { pthread_mutex_unlock(&g_ProcessHostLock); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    ph_drain_ipc_locked(e);
    osPid = e->linuxPid;
    alreadyFinal = ph_state_is_final(e->state) ? 1u : 0u;
    if (alreadyFinal) {
        ph_cleanup_ipc_resources_locked(e, "already-final terminate");
        if (lpRawStatus) *lpRawStatus = e->rawStatus;
        pthread_mutex_unlock(&g_ProcessHostLock);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    e->killCount++;
    snprintf(e->lastEvent, sizeof(e->lastEvent), "SIGKILL requested");
    pthread_mutex_unlock(&g_ProcessHostLock);

    if (osPid > 0) (void)kill((pid_t)osPid, SIGKILL);
    int status = 0;
    pid_t r = (osPid > 0) ? waitpid((pid_t)osPid, &status, 0) : -1;

    pthread_mutex_lock(&g_ProcessHostLock);
    e = ph_find_locked(dwProcessId);
    if (e) {
        ph_drain_ipc_locked(e);
        e->state = MYPROCESSHOST_STATE_KILLED;
        if (r == (pid_t)osPid) {
            e->rawStatus = (DWORD)status;
            e->reapCount++;
        } else {
            e->rawStatus = 0;
        }
        e->exitCode = ph_exit_code_from_wait_status((int)e->rawStatus);
        e->exitMs = ph_now_ms();
        snprintf(e->lastEvent, sizeof(e->lastEvent), "terminated raw=0x%x ipc=%u", (unsigned)e->rawStatus, e->ipcMessages);
        ph_cleanup_ipc_resources_locked(e, "terminated");
        if (lpRawStatus) *lpRawStatus = e->rawStatus;
    }
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostGetAuditStats(MyProcessHostAudit* lpAudit)
{
    if (!lpAudit) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(lpAudit, 0, sizeof(*lpAudit));
    pthread_mutex_lock(&g_ProcessHostLock);
    for (int i = 0; i < MYPROCESSHOST_MAX; i++) {
        MyProcessHostEntry* e = &g_ProcessHost[i];
        if (!e->valid) continue;
        lpAudit->valid_count++;
        if (e->state == MYPROCESSHOST_STATE_RUNNING) lpAudit->running_count++;
        if (ph_state_is_final(e->state)) lpAudit->final_count++;
        if (e->state == MYPROCESSHOST_STATE_EXITED) lpAudit->exited_count++;
        if (e->state == MYPROCESSHOST_STATE_REAPED) lpAudit->reaped_count++;
        if (e->state == MYPROCESSHOST_STATE_KILLED) lpAudit->killed_count++;
        if (e->state == MYPROCESSHOST_STATE_LOST) lpAudit->lost_count++;
        if (ph_entry_has_ipc_resources_locked(e)) {
            lpAudit->open_ipc_resources++;
            if (ph_state_is_final(e->state)) lpAudit->final_open_ipc_resources++;
        }
        lpAudit->cleanup_count += e->cleanupCount;
        lpAudit->cleanup_error_count += e->cleanupErrorCount;
    }
    lpAudit->reclaimed_slots = g_ProcessHostReclaimedSlots;
    lpAudit->async_reaper_started = MyProcessHostAsyncReaperActive();
    lpAudit->async_reaper_polls = __atomic_load_n(&g_ProcessHostReaperPolls, __ATOMIC_RELAXED);
    lpAudit->async_reaper_reaps = __atomic_load_n(&g_ProcessHostReaperReaps, __ATOMIC_RELAXED);
    lpAudit->async_reaper_notifications = __atomic_load_n(&g_ProcessHostReaperNotifications, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL MyProcessHostGetInfo(DWORD dwProcessId, MyProcessHostInfo* lpInfo)
{
    if (!lpInfo) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_ProcessHostLock);
    MyProcessHostEntry* e = ph_find_locked(dwProcessId);
    if (!e) {
        memset(lpInfo, 0, sizeof(*lpInfo));
        pthread_mutex_unlock(&g_ProcessHostLock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ph_drain_ipc_locked(e);
    ph_fill_info_locked(e, lpInfo);
    pthread_mutex_unlock(&g_ProcessHostLock);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}
