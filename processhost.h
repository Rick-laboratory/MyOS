#pragma once

#include "mytypes.h"
#include "process_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MYPROCESSHOST_STATE_EMPTY   0u
#define MYPROCESSHOST_STATE_RUNNING 1u
#define MYPROCESSHOST_STATE_EXITED  2u
#define MYPROCESSHOST_STATE_REAPED  3u
#define MYPROCESSHOST_STATE_LOST    4u
#define MYPROCESSHOST_STATE_KILLED  5u

typedef struct MyProcessHostCreateWindowRequest {
    DWORD my_pid;
    int   linux_pid;
    int   x;
    int   y;
    int   w;
    int   h;
    HWND  owner_hwnd;   /* v172: top-level owner HWND; distinct from WS_CHILD parent. */
    DWORD style;        /* v172: requested top-level style from child CreateWindowExA. */
    DWORD ex_style;     /* v172: requested top-level ex-style from child CreateWindowExA. */
    char  class_name[MYOS_IPC_IMAGE_MAX];
    char  title[MYOS_IPC_TEXT_MAX];
} MyProcessHostCreateWindowRequest;

typedef struct MyProcessHostWindowMessage {
    DWORD my_pid;
    int   linux_pid;
    HWND  hwnd;
    UINT  msg;
    WPARAM wparam;
    LPARAM lparam;
    char  text[MYOS_IPC_TEXT_MAX];
} MyProcessHostWindowMessage;

typedef struct MyProcessHostCreateChildWindowRequest {
    DWORD my_pid;
    int   linux_pid;
    HWND  parent_hwnd;
    UINT  id;
    DWORD style;
    DWORD ex_style;
    int   x;
    int   y;
    int   w;
    int   h;
    char  class_name[MYOS_IPC_IMAGE_MAX];
    char  title[MYOS_IPC_TEXT_MAX];
} MyProcessHostCreateChildWindowRequest;

typedef struct MyProcessHostCreateChildWindowBatchRequest {
    DWORD my_pid;
    int   linux_pid;
    UINT  count;
    MyProcessHostCreateChildWindowRequest items[MYOS_IPC_MAX_CHILD_CONTROLS];
} MyProcessHostCreateChildWindowBatchRequest;

typedef struct MyProcessHostAudit {
    DWORD valid_count;
    DWORD running_count;
    DWORD final_count;
    DWORD exited_count;
    DWORD reaped_count;
    DWORD killed_count;
    DWORD lost_count;
    DWORD open_ipc_resources;
    DWORD final_open_ipc_resources;
    DWORD cleanup_count;
    DWORD cleanup_error_count;
    DWORD reclaimed_slots;
    DWORD async_reaper_started;
    DWORD async_reaper_polls;
    DWORD async_reaper_reaps;
    DWORD async_reaper_notifications;
} MyProcessHostAudit;

typedef struct MyProcessHostInfo {
    DWORD my_pid;
    int   linux_pid;
    DWORD state;
    DWORD exit_code;
    DWORD raw_status;
    DWORD poll_count;
    DWORD reap_count;
    DWORD kill_count;
    DWORD cleanup_count;
    DWORD cleanup_error_count;
    DWORD resources_closed;
    DWORD start_ms;
    DWORD exit_ms;
    char  state_name[24];
    char  image_name[64];
    char  last_event[96];

    // v58: per-child IPC bridge diagnostics.
    DWORD ipc_enabled;
    DWORD ipc_fd;
    DWORD ipc_messages;
    DWORD ipc_hello;
    DWORD ipc_exit_report;
    DWORD ipc_last_opcode;
    DWORD ipc_last_value;
    char  ipc_last_text[MYOS_IPC_TEXT_MAX];
    char  shared_name[96];
    DWORD shared_heartbeat;
    DWORD shared_child_pid;
    DWORD shared_argc;
    DWORD shared_exit_code;
    char  shared_status[MYOS_IPC_TEXT_MAX];
    char  shared_argv_preview[MYOS_IPC_SHARED_ARGV_MAX];

    // v59: GUI CreateWindowExA-over-IPC diagnostics.
    DWORD gui_create_request;
    DWORD gui_create_consumed;
    DWORD gui_create_ack;
    DWORD gui_hwnd;
    DWORD gui_window_index;
    DWORD gui_x;
    DWORD gui_y;
    DWORD gui_w;
    DWORD gui_h;
    char  gui_class[MYOS_IPC_IMAGE_MAX];
    char  gui_title[MYOS_IPC_TEXT_MAX];

    // v61: GUI runtime-backed cross-process message queue bridge diagnostics.
    DWORD gui_msg_sent;
    DWORD gui_msg_received;
    DWORD gui_msg_dispatched;
    DWORD gui_post_request;
    DWORD gui_post_ack;
    DWORD gui_close_seen;
    DWORD gui_last_hwnd;
    DWORD gui_last_msg;
    DWORD gui_last_wparam_lo;
    DWORD gui_last_lparam_lo;
    char  gui_last_text[MYOS_IPC_TEXT_MAX];

    // v61: child-side GUI IPC runtime API diagnostics.
    DWORD gui_runtime_api_calls;
    DWORD gui_register_class_calls;
    DWORD gui_create_window_calls;
    DWORD gui_get_message_calls;
    DWORD gui_dispatch_message_calls;
    DWORD gui_destroy_window_calls;
    DWORD gui_destroy_request;
    DWORD gui_destroy_ack;
    char  gui_runtime_status[MYOS_IPC_TEXT_MAX];

    // v62: out-of-process calculator state mirrored from child shared section.
    DWORD calc_enabled;
    DWORD calc_revision;
    DWORD calc_button_hits;
    char  calc_display[64];
    char  calc_opline[32];
    char  calc_last_button[16];
    char  calc_history_preview[192];

    // v64: out-of-process editor state mirrored from child shared section.
    DWORD editor_enabled;
    DWORD editor_revision;
    DWORD editor_chars_typed;
    DWORD editor_keydowns;
    DWORD editor_cursor;
    DWORD editor_length;
    DWORD editor_dirty;
    DWORD editor_scroll_line;
    char  editor_path[160];
    char  editor_name[64];
    char  editor_status[MYOS_IPC_TEXT_MAX];
    char  editor_preview[192];

    // v65: out-of-process PaintLab state mirrored from child shared section.
    DWORD paint_enabled;
    DWORD paint_revision;
    DWORD paint_segments;
    DWORD paint_mouse_down;
    DWORD paint_capture_count;
    DWORD paint_release_count;
    DWORD paint_move_count;
    DWORD paint_clear_count;
    int   paint_last_x;
    int   paint_last_y;
    char  paint_status[MYOS_IPC_TEXT_MAX];

    // v70: cross-process clipboard/menu/accelerator + kernel syscall-lite diagnostics.
    DWORD clip_enabled;
    DWORD clip_request;
    DWORD clip_ack;
    DWORD clip_op;
    DWORD clip_ok;
    DWORD clip_set_count;
    DWORD clip_get_count;
    DWORD clip_clear_count;
    DWORD clip_open_count;
    DWORD clip_close_count;
    char  clip_text[192];
    char  clip_local_text[192];
    char  clip_status[MYOS_IPC_TEXT_MAX];

    DWORD menu_enabled;
    DWORD menu_request;
    DWORD menu_ack;
    DWORD menu_op;
    DWORD menu_ok;
    DWORD menu_create_count;
    DWORD menu_append_count;
    DWORD menu_set_count;
    DWORD menu_popup_count;
    DWORD menu_command_count;
    DWORD accel_count;
    DWORD accel_translate_count;
    DWORD menu_last_handle;
    DWORD menu_last_command;
    char  menu_status[MYOS_IPC_TEXT_MAX];

    // v70: child -> parent kernel/object syscall bridge diagnostics.
    DWORD kernel_enabled;
    DWORD kernel_request;
    DWORD kernel_ack;
    DWORD kernel_op;
    DWORD kernel_ok;
    DWORD kernel_error;
    DWORD kernel_handle;
    DWORD kernel_result;
    DWORD kernel_access;
    DWORD kernel_flags;
    DWORD kernel_timeout;
    DWORD kernel_count;
    DWORD kernel_wait_all;
    char  kernel_name[MYOS_IPC_IMAGE_MAX];
    char  kernel_status[MYOS_IPC_TEXT_MAX];

    // v66: child HWND/control bridge diagnostics.
    DWORD child_hwnd_request;
    DWORD child_hwnd_ack;
    DWORD child_hwnd_created;
    DWORD child_hwnd_count;
    DWORD child_hwnd_parent;
    DWORD child_hwnd_last;
    DWORD child_hwnd_last_id;
    DWORD child_hwnd_last_msg;
    DWORD child_hwnd_command_count;
    DWORD child_hwnd_click_count;
    char  child_hwnd_status[MYOS_IPC_TEXT_MAX];

    // v65: generic cross-process GDI command buffer diagnostics.
    DWORD gdi_enabled;
    DWORD gdi_sequence;
    DWORD gdi_paint_count;
    DWORD gdi_command_count;
    DWORD gdi_client_w;
    DWORD gdi_client_h;
    DWORD gdi_last_msg;
    char  gdi_status[MYOS_IPC_TEXT_MAX];
    MyGdiIpcCommand gdi_commands[MYOS_GDI_MAX_COMMANDS];

    // v75: DWM-lite persistent surface/backing-store diagnostics.
    DWORD surface_enabled;
    DWORD surface_mapped;
    DWORD surface_width;
    DWORD surface_height;
    DWORD surface_stride;
    DWORD surface_format;
    DWORD surface_seq;
    DWORD surface_paint_count;
    DWORD surface_dirty_left;
    DWORD surface_dirty_top;
    DWORD surface_dirty_right;
    DWORD surface_dirty_bottom;
    DWORD surface_map_size;
    char  surface_map_name[MYOS_IPC_IMAGE_MAX];
    char  surface_status[MYOS_IPC_TEXT_MAX];
} MyProcessHostInfo;

BOOL MyProcessHostSpawnConsole(DWORD dwProcessId,
                               LPCSTR lpChildExePath,
                               LPCSTR lpDirectory,
                               LPCSTR lpImageName,
                               int argc,
                               char* const argv[],
                               int* outLinuxPid);
BOOL MyProcessHostTrack(DWORD dwProcessId, int nLinuxPid, LPCSTR lpImageName);
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
                           int* outLinuxPid);
BOOL MyProcessHostTakeCreateWindowRequest(DWORD dwProcessId, MyProcessHostCreateWindowRequest* lpRequest);
BOOL MyProcessHostAckCreateWindow(DWORD dwProcessId, HWND hwnd, DWORD windowIndex, BOOL ok, LPCSTR lpText);
BOOL MyProcessHostSendWindowMessage(DWORD dwProcessId, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LPCSTR lpText);
BOOL MyProcessHostUpdateGuiRect(DWORD dwProcessId, int x, int y, int w, int h);
BOOL MyProcessHostTakeCreateChildWindowRequest(DWORD dwProcessId, MyProcessHostCreateChildWindowRequest* lpRequest);
BOOL MyProcessHostAckCreateChildWindow(DWORD dwProcessId, HWND hwnd, UINT id, BOOL ok, LPCSTR lpText);
BOOL MyProcessHostTakeCreateChildWindowBatchRequest(DWORD dwProcessId, MyProcessHostCreateChildWindowBatchRequest* lpRequest);
BOOL MyProcessHostAckCreateChildWindowBatch(DWORD dwProcessId, const HWND* hwnds, const UINT* ids, UINT count, BOOL ok, LPCSTR lpText);
BOOL MyProcessHostTakePostMessageRequest(DWORD dwProcessId, MyProcessHostWindowMessage* lpRequest);
BOOL MyProcessHostAckPostMessage(DWORD dwProcessId, BOOL ok, LPCSTR lpText);
BOOL MyProcessHostTakeDestroyWindowRequest(DWORD dwProcessId, MyProcessHostWindowMessage* lpRequest);
BOOL MyProcessHostAckDestroyWindow(DWORD dwProcessId, BOOL ok, LPCSTR lpText);
BOOL MyProcessHostTakeEnableWindowRequest(DWORD dwProcessId, MyProcessHostWindowMessage* lpRequest);
BOOL MyProcessHostAckEnableWindow(DWORD dwProcessId, BOOL ok, LPCSTR lpText);

BOOL MyProcessHostPoll(DWORD dwProcessId, BOOL* lpExited, DWORD* lpExitCode, DWORD* lpRawStatus);
DWORD MyProcessHostPollAll(void);
DWORD MyProcessHostSweepFinalizedResources(void);
BOOL MyProcessHostTerminate(DWORD dwProcessId, DWORD* lpRawStatus);
BOOL MyProcessHostGetInfo(DWORD dwProcessId, MyProcessHostInfo* lpInfo);
BOOL MyProcessHostGetAuditStats(MyProcessHostAudit* lpAudit);
BOOL MyProcessHostAsyncReaperActive(void);
void MyWinNotifyProcessHostExit(DWORD dwProcessId, DWORD dwExitCode, DWORD dwRawStatus);
const char* MyProcessHostStateName(DWORD state);

#ifdef __cplusplus
}
#endif
