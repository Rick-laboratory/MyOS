#pragma once

#include <stdint.h>
#include <stddef.h>

#define MYOS_IPC_MAGIC      0x5849504dU /* 'MPIX' little-ish: myOS IPC */
#define MYOS_IPC_VERSION    76U

#define MYOS_IPC_OP_HELLO       1U
#define MYOS_IPC_OP_PING        2U
#define MYOS_IPC_OP_EXIT        3U
#define MYOS_IPC_OP_TEXT        4U
#define MYOS_IPC_OP_CREATE_WINDOW 5U
#define MYOS_IPC_OP_WINDOW_ACK    6U
#define MYOS_IPC_OP_WINDOW_FAIL   7U
#define MYOS_IPC_OP_WINDOW_MESSAGE 8U   /* parent -> child: queued MSG */
#define MYOS_IPC_OP_WINDOW_DISPATCHED 9U/* child -> parent: DispatchMessage completed */
#define MYOS_IPC_OP_POST_MESSAGE 10U    /* child -> parent: PostMessage request */
#define MYOS_IPC_OP_POST_ACK     11U    /* parent -> child: PostMessage accepted/failed */
#define MYOS_IPC_OP_DESTROY_WINDOW 12U /* child -> parent: DestroyWindow request */
#define MYOS_IPC_OP_DESTROY_ACK    13U /* parent -> child: DestroyWindow accepted/failed */
#define MYOS_IPC_OP_CREATE_CHILD_WINDOW 14U /* child -> parent: CreateWindowExA(WS_CHILD/parent) */
#define MYOS_IPC_OP_CHILD_WINDOW_ACK    15U /* parent -> child: child HWND created/failed */
#define MYOS_IPC_OP_CLIPBOARD_REQUEST    16U /* child -> parent: session clipboard request */
#define MYOS_IPC_OP_CLIPBOARD_ACK        17U /* parent -> child: session clipboard result */
#define MYOS_IPC_OP_MENU_REQUEST         18U /* child -> parent: menu/accelerator diagnostic request */
#define MYOS_IPC_OP_MENU_ACK             19U /* parent -> child: menu/accelerator diagnostic ack */
#define MYOS_IPC_OP_KERNEL_REQUEST       20U /* child -> parent: ntoskrnl/object syscall-lite request */
#define MYOS_IPC_OP_KERNEL_ACK           21U /* parent -> child: ntoskrnl/object syscall-lite result */
#define MYOS_IPC_OP_ENABLE_WINDOW_REQ    22U /* child -> parent: EnableWindow(hwnd, BOOL) */
#define MYOS_IPC_OP_ENABLE_WINDOW_ACK    23U /* parent -> child: EnableWindow accepted/failed */
#define MYOS_IPC_OP_CREATE_CHILD_WINDOW_BATCH 24U /* child -> parent: batch CreateWindowExA(WS_CHILD/parent) */
#define MYOS_IPC_OP_CHILD_WINDOW_BATCH_ACK    25U /* parent -> child: batch child HWND create ack */

#define MYOS_KOP_CREATE_EVENT       1U
#define MYOS_KOP_OPEN_EVENT         2U
#define MYOS_KOP_SET_EVENT          3U
#define MYOS_KOP_RESET_EVENT        4U
#define MYOS_KOP_CREATE_MUTEX       5U
#define MYOS_KOP_RELEASE_MUTEX      6U
#define MYOS_KOP_CREATE_SEMAPHORE   7U
#define MYOS_KOP_RELEASE_SEMAPHORE  8U
#define MYOS_KOP_CLOSE_HANDLE       9U
#define MYOS_KOP_WAIT_ONE           10U
#define MYOS_KOP_WAIT_MANY          11U
#define MYOS_KOP_DUPLICATE_HANDLE   12U
#define MYOS_KOP_CREATE_FILE_MAPPING 13U
#define MYOS_KOP_OPEN_FILE_MAPPING   14U
#define MYOS_KOP_MAP_VIEW_OF_FILE    15U
#define MYOS_KOP_UNMAP_VIEW_OF_FILE  16U

#define MYOS_KFLAG_INHERIT          0x00000001U
#define MYOS_KFLAG_MANUAL_RESET     0x00000002U
#define MYOS_KFLAG_INITIAL_STATE    0x00000004U
#define MYOS_KFLAG_INITIAL_OWNER    0x00000008U

#define MYOS_CLIP_OP_OPEN    1U
#define MYOS_CLIP_OP_CLOSE   2U
#define MYOS_CLIP_OP_EMPTY   3U
#define MYOS_CLIP_OP_SET     4U
#define MYOS_CLIP_OP_GET     5U
#define MYOS_CLIP_OP_ISAVAIL 6U

#define MYOS_MENU_OP_CREATE  1U
#define MYOS_MENU_OP_APPEND  2U
#define MYOS_MENU_OP_SET     3U
#define MYOS_MENU_OP_TRACK   4U
#define MYOS_MENU_OP_DESTROY 5U
#define MYOS_MENU_OP_ACCEL   6U

#define MYOS_IPC_SHARED_MAGIC   0x4853504dU /* 'MPSH' */
#define MYOS_IPC_SHARED_VERSION 76U
#define MYOS_IPC_TEXT_MAX       96U
#define MYOS_IPC_IMAGE_MAX      64U
#define MYOS_IPC_SHARED_ARGV_MAX 160U


/* v64: cross-process GDI command buffer.
   The GUI child writes small scalar draw commands into the shared section during
   WM_PAINT/Invalidate-like updates. The parent Session/WindowManager only
   interprets the buffer; it no longer needs calculator-specific rendering. */
#define MYOS_GDI_MAX_COMMANDS 256U
#define MYOS_GDI_TEXT_MAX     80U

#define MYOS_GDI_OP_NONE      0U
#define MYOS_GDI_OP_FILLRECT  1U
#define MYOS_GDI_OP_RECTANGLE 2U
#define MYOS_GDI_OP_TEXTOUT   3U
#define MYOS_GDI_OP_DRAWTEXT  4U
#define MYOS_GDI_OP_LINE      5U

/* AUDIT(v118): OOP child HWND bridge has a tiny fixed child-control lane.
   Real dialog/control-heavy child apps will exceed this quickly. */
#define MYOS_IPC_MAX_CHILD_CONTROLS 32U


/* v75: DWM-lite persistent surface backing store for OOP windows.
   The child owns/mutates pixels in a named FileMapping/posix-shm view; the
   parent compositor maps the same backing object and blits the last complete
   frame.  Queue messages/IPC only carry dirty notifications and metadata. */
#define MYOS_SURFACE_MAGIC    0x5352464dU /* 'MFRS' */
#define MYOS_SURFACE_VERSION  75U
#define MYOS_SURFACE_FORMAT_XRGB8888 1U

typedef struct MySurfaceHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t seqBegin;
    uint32_t seqEnd;
    int32_t  dirtyLeft;
    int32_t  dirtyTop;
    int32_t  dirtyRight;
    int32_t  dirtyBottom;
    uint32_t dirtyFlags;
    uint32_t frameSerial;
    uint32_t paintSerial;
    uint32_t reserved[8];
} MySurfaceHeader;

#define MYOS_GDI_TEXT_LEFT    0U
#define MYOS_GDI_TEXT_CENTER  1U
#define MYOS_GDI_TEXT_RIGHT   2U
#define MYOS_GDI_TEXT_VCENTER 4U

typedef struct MyGdiIpcCommand {
    uint32_t opcode;
    uint32_t hwnd;        /* v169: target top-level HWND for retained per-window streams. */
    uint32_t stream_seq;  /* v169: sequence of the target HWND stream snapshot. */
    int32_t  x;
    int32_t  y;
    int32_t  w;
    int32_t  h;
    uint32_t color;
    uint32_t aux_color;
    uint32_t flags;
    char     text[MYOS_GDI_TEXT_MAX];
} MyGdiIpcCommand;

typedef struct MyProcessIpcMessage {
    uint32_t magic;
    uint32_t version;
    uint32_t opcode;
    uint32_t my_pid;
    uint32_t child_pid;
    uint32_t value;
    char     text[MYOS_IPC_TEXT_MAX];

    /* v61: GUI runtime-backed cross-process MSG payload.  We keep it small and scalar-only;
       pointer payloads move through shared sections later. */
    uint32_t hwnd;
    uint32_t msg;
    uint64_t wparam;
    uint64_t lparam;
} MyProcessIpcMessage;

typedef struct MyProcessIpcShared {
    uint32_t magic;
    uint32_t version;
    uint32_t my_pid;
    uint32_t child_pid;
    uint32_t parent_pid;
    uint32_t heartbeat;
    uint32_t argc;
    uint32_t exit_code;
    uint32_t flags;
    char     image[MYOS_IPC_IMAGE_MAX];
    char     status[MYOS_IPC_TEXT_MAX];
    char     argv_preview[MYOS_IPC_SHARED_ARGV_MAX];

    /* v59: GUI child -> parent CreateWindowExA bridge request/ack. */
    uint32_t gui_request;
    uint32_t gui_ack;
    uint32_t gui_hwnd;
    uint32_t gui_window_index;
    uint32_t gui_x;
    uint32_t gui_y;
    uint32_t gui_w;
    uint32_t gui_h;
    char     gui_class[MYOS_IPC_IMAGE_MAX];
    char     gui_title[MYOS_IPC_TEXT_MAX];

    /* v61: parent<->GUI-child message queue bridge diagnostics. */
    uint32_t gui_msg_sent;
    uint32_t gui_msg_received;
    uint32_t gui_msg_dispatched;
    uint32_t gui_post_request;
    uint32_t gui_post_ack;
    uint32_t gui_close_seen;
    uint32_t gui_last_hwnd;
    uint32_t gui_last_msg;
    uint64_t gui_last_wparam;
    uint64_t gui_last_lparam;
    char     gui_last_text[MYOS_IPC_TEXT_MAX];

    /* v61: child-side GUI IPC runtime API diagnostics. */
    uint32_t gui_runtime_api_calls;
    uint32_t gui_register_class_calls;
    uint32_t gui_create_window_calls;
    uint32_t gui_get_message_calls;
    uint32_t gui_dispatch_message_calls;
    uint32_t gui_destroy_window_calls;
    uint32_t gui_destroy_request;
    uint32_t gui_destroy_ack;
    char     gui_runtime_status[MYOS_IPC_TEXT_MAX];

    /* v62: first real out-of-process GUI app payload: Calculator state.
       v63 keeps this for diagnostics, but rendering now flows through the
       generic GDI IPC command buffer below. */
    uint32_t calc_enabled;
    uint32_t calc_revision;
    uint32_t calc_button_hits;
    char     calc_display[64];
    char     calc_opline[32];
    char     calc_last_button[16];
    char     calc_history_preview[192];

    /* v64: first real out-of-process Editor payload. Rendering remains generic
       GDI commands; these fields expose child-side text/caret/save state for
       diagnostics without making the parent understand the editor UI. */
    uint32_t editor_enabled;
    uint32_t editor_revision;
    uint32_t editor_chars_typed;
    uint32_t editor_keydowns;
    uint32_t editor_cursor;
    uint32_t editor_length;
    uint32_t editor_dirty;
    uint32_t editor_scroll_line;
    char     editor_path[160];
    char     editor_name[64];
    char     editor_status[MYOS_IPC_TEXT_MAX];
    char     editor_preview[192];

    /* v65: out-of-process PaintLab payload. The stroke list itself is rendered
       via the generic GDI command buffer; these fields are diagnostics only. */
    uint32_t paint_enabled;
    uint32_t paint_revision;
    uint32_t paint_segments;
    uint32_t paint_mouse_down;
    uint32_t paint_capture_count;
    uint32_t paint_release_count;
    uint32_t paint_move_count;
    uint32_t paint_clear_count;
    int32_t  paint_last_x;
    int32_t  paint_last_y;
    char     paint_status[MYOS_IPC_TEXT_MAX];


    /* v70: cross-process clipboard/menu/accelerator runtime bridge + kernel syscall-lite diagnostics.
       Clipboard data is session-owned by the parent; the child sends scalar
       requests and mirrors text/status here for diagnostics and ACK waits. */
    uint32_t clip_enabled;
    uint32_t clip_request;
    uint32_t clip_ack;
    uint32_t clip_op;
    uint32_t clip_format;
    uint32_t clip_ok;
    uint32_t clip_set_count;
    uint32_t clip_get_count;
    uint32_t clip_clear_count;
    uint32_t clip_open_count;
    uint32_t clip_close_count;
    uint32_t clip_isavail_count;
    char     clip_text[192];
    char     clip_local_text[192];
    char     clip_status[MYOS_IPC_TEXT_MAX];

    uint32_t menu_enabled;
    uint32_t menu_request;
    uint32_t menu_ack;
    uint32_t menu_op;
    uint32_t menu_ok;
    uint32_t menu_create_count;
    uint32_t menu_append_count;
    uint32_t menu_set_count;
    uint32_t menu_popup_count;
    uint32_t menu_destroy_count;
    uint32_t menu_command_count;
    uint32_t accel_count;
    uint32_t accel_translate_count;
    uint32_t menu_last_handle;
    uint32_t menu_last_command;
    char     menu_status[MYOS_IPC_TEXT_MAX];

    /* v70: child -> parent kernel/object syscall bridge.
       This is the first generic bridge where an out-of-process child asks the
       parent SessionKernel to operate on the parent's real per-process handle
       table/Object Manager under the child's myOS PID context. */
    uint32_t kernel_enabled;
    uint32_t kernel_request;
    uint32_t kernel_ack;
    uint32_t kernel_op;
    uint32_t kernel_ok;
    uint32_t kernel_error;
    uint32_t kernel_handle;
    uint32_t kernel_result;
    uint32_t kernel_access;
    uint32_t kernel_flags;
    uint32_t kernel_timeout;
    uint32_t kernel_count;
    uint32_t kernel_wait_all;
    uint32_t kernel_options;       /* v70: DuplicateHandle dwOptions / v71 section protect */
    uint32_t kernel_target_pid;    /* reserved for cross-process DuplicateHandle */
    uint32_t kernel_size;          /* v71: section size / map bytes */
    uint32_t kernel_offset_low;    /* v71: low mapping offset */
    uint32_t kernel_offset_high;   /* v71: high mapping offset, currently must be 0 */
    uint32_t kernel_map_size;      /* v71: returned byte count for child mmap */
    int32_t  kernel_initial;
    int32_t  kernel_maximum;
    int32_t  kernel_release_count;
    uint32_t kernel_handles[8];
    char     kernel_name[MYOS_IPC_IMAGE_MAX];
    char     kernel_map_name[MYOS_IPC_IMAGE_MAX]; /* v71: POSIX shm backing name for child mmap */
    char     kernel_status[MYOS_IPC_TEXT_MAX];

    /* v66: cross-process child-HWND/control bridge. A GUI child can call
       CreateWindowExA with WS_CHILD / hWndParent; the parent creates a real
       USER32 child HWND under the proxy top-level HWND and returns the handle. */
    uint32_t child_hwnd_request;
    uint32_t child_hwnd_ack;
    uint32_t child_hwnd_created;
    uint32_t child_hwnd_count;
    uint32_t child_hwnd_parent;
    uint32_t child_hwnd_last;
    uint32_t child_hwnd_last_id;
    uint32_t child_hwnd_last_msg;
    uint32_t child_hwnd_command_count;
    uint32_t child_hwnd_click_count;
    uint32_t child_hwnd_ids[MYOS_IPC_MAX_CHILD_CONTROLS];
    uint32_t child_hwnd_hwnds[MYOS_IPC_MAX_CHILD_CONTROLS];
    uint32_t child_hwnd_style[MYOS_IPC_MAX_CHILD_CONTROLS];
    uint32_t child_hwnd_ex_style[MYOS_IPC_MAX_CHILD_CONTROLS];
    int32_t  child_hwnd_x[MYOS_IPC_MAX_CHILD_CONTROLS];
    int32_t  child_hwnd_y[MYOS_IPC_MAX_CHILD_CONTROLS];
    int32_t  child_hwnd_w[MYOS_IPC_MAX_CHILD_CONTROLS];
    int32_t  child_hwnd_h[MYOS_IPC_MAX_CHILD_CONTROLS];
    char     child_hwnd_class[MYOS_IPC_MAX_CHILD_CONTROLS][MYOS_IPC_IMAGE_MAX];
    char     child_hwnd_text[MYOS_IPC_MAX_CHILD_CONTROLS][MYOS_IPC_TEXT_MAX];
    char     child_hwnd_status[MYOS_IPC_TEXT_MAX];

    /* v65: cross-process WM_PAINT/GDI command surface. Coordinates are client-
       relative; the parent renderer offsets them by the window client origin. */
    uint32_t gdi_enabled;
    uint32_t gdi_sequence;
    uint32_t gdi_paint_count;
    uint32_t gdi_command_count;
    uint32_t gdi_client_w;
    uint32_t gdi_client_h;
    uint32_t gdi_last_msg;
    char     gdi_status[MYOS_IPC_TEXT_MAX];
    MyGdiIpcCommand gdi_commands[MYOS_GDI_MAX_COMMANDS];

    /* v75: OOP DWM-lite surface/backing-store diagnostics.
       surface_map_name is a POSIX shm name returned by the kernel section
       bridge. Parent maps it read-only and composites pixels from the backing
       store instead of replaying only transient GDI command buffers. */
    uint32_t surface_enabled;
    uint32_t surface_mapped;
    uint32_t surface_width;
    uint32_t surface_height;
    uint32_t surface_stride;
    uint32_t surface_format;
    uint32_t surface_seq;
    uint32_t surface_paint_count;
    uint32_t surface_dirty_left;
    uint32_t surface_dirty_top;
    uint32_t surface_dirty_right;
    uint32_t surface_dirty_bottom;
    uint32_t surface_map_size;
    char     surface_map_name[MYOS_IPC_IMAGE_MAX];
    char     surface_status[MYOS_IPC_TEXT_MAX];
} MyProcessIpcShared;
