#include "process_ipc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <linux/input-event-codes.h>

/* AUDIT(v119-oop): Childhost is the out-of-process app compatibility canary.
   It currently duplicates a small WinAPI runtime locally and bridges HWND/
   message/GDI/menu calls over IPC. It will intentionally change when the child
   side starts consuming the same public SDK headers/import table as normal apps.
   Breakage here usually means parent-child ABI drift, not normal USER32 failure. */


#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_PAINT 0x000Fu
#define WM_CLOSE 0x0010u
#define WM_SIZE 0x0005u
#define WM_GETMINMAXINFO 0x0024u
#define WM_WINDOWPOSCHANGING 0x0046u
#define WM_WINDOWPOSCHANGED 0x0047u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCHAR 0x0106u
#define WM_COMMAND 0x0111u
#define WM_SYSCOMMAND 0x0112u
#define WM_INITMENU 0x0116u
#define WM_INITMENUPOPUP 0x0117u
#define WM_MENUSELECT 0x011Fu
#define WM_ENTERMENULOOP 0x0211u
#define WM_EXITMENULOOP 0x0212u
#define WM_CANCELMODE 0x001Fu
#define WM_ENABLE 0x000Au
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u
#define WM_MOUSEMOVE   0x0200u
#define WM_MOUSEWHEEL  0x020Au
#define WM_CAPTURECHANGED 0x0215u
#define WM_ENTERSIZEMOVE 0x0231u
#define WM_EXITSIZEMOVE  0x0232u
#define WM_USER 0x0400u
#define WM_MYOS_SUBSCRIBED (WM_USER + 0x0101u)
#define WM_MYOS_HWND_STATE_DIRTY (WM_USER + 0x0102u)
#define WM_MYOS_HWND_STATE_SUBSCRIBE_REQ (WM_USER + 0x0103u)
#define WM_MYOS_HWND_STATE_UNSUBSCRIBE_REQ (WM_USER + 0x0104u)
#define BN_CLICKED 0u
#define WS_VISIBLE 0x10000000u
#define WS_POPUP   0x80000000u
#define WS_CHILD   0x40000000u
#define WS_TABSTOP 0x00010000u
#define WS_GROUP   0x00020000u
#define BS_PUSHBUTTON 0x00000000u
#define BS_DEFPUSHBUTTON 0x00000001u
#define IDOK 1u
#define IDCANCEL 2u
#define LOWORD(l) ((uint16_t)((l) & 0xffffu))
#define HIWORD(l) ((uint16_t)(((l) >> 16) & 0xffffu))
#define MAKEWPARAM(l,h) ((WPARAM)(((uint16_t)(l)) | ((uint64_t)((uint16_t)(h)) << 16)))
#define MAKELPARAM(l,h) ((LPARAM)(((uint16_t)(l)) | ((uint64_t)((uint16_t)(h)) << 16)))
#define GET_X_LPARAM(lp) ((int)(int16_t)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(int16_t)HIWORD(lp))
#define MK_LBUTTON  0x0001u
#define MK_RBUTTON  0x0002u
#define MK_SHIFT    0x0004u
#define MK_CONTROL  0x0008u
#define WHEEL_DELTA 120
#define MYOS_CHILD_QUEUE_CAP 64

static int ascii_ieq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

typedef struct ChildIpcMessage {
    uint32_t hwnd;
    uint32_t msg;
    uint64_t wparam;
    uint64_t lparam;
} ChildIpcMessage;

typedef struct ChildIpcContext {
    int fd;
    unsigned myPid;
    const char* image;
    MyProcessIpcShared* shared;
    ChildIpcMessage q[MYOS_CHILD_QUEUE_CAP];
    int q_head;
    int q_tail;
    int q_count;
    uint32_t hwnd;
    uint32_t primary_hwnd;
    uint32_t window_ack_hwnd;
    uint32_t received;
    uint32_t dispatched;
    uint32_t close_seen;
    uint32_t post_ack;
    uint32_t pending_child_hwnd;
    uint32_t last_child_hwnd;
} ChildIpcContext;

static void child_make_argv_preview(char* out, size_t cb, int argc, char** argv)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    size_t used = 0;
    for (int i = 0; i < argc; i++) {
        int wrote = snprintf(out + used, cb - used, "%s%d='%s'", used ? ";" : "", i, argv && argv[i] ? argv[i] : "");
        if (wrote < 0) break;
        if ((size_t)wrote >= cb - used) { out[cb - 1] = 0; break; }
        used += (size_t)wrote;
    }
}


static int child_queue_push(ChildIpcContext* ipc, const MyProcessIpcMessage* msg)
{
    if (!ipc || !msg) return 0;
    if (ipc->q_count >= MYOS_CHILD_QUEUE_CAP) return 0;
    ChildIpcMessage* m = &ipc->q[ipc->q_tail];
    m->hwnd = msg->hwnd;
    m->msg = msg->msg;
    m->wparam = msg->wparam;
    m->lparam = msg->lparam;
    ipc->q_tail = (ipc->q_tail + 1) % MYOS_CHILD_QUEUE_CAP;
    ipc->q_count++;
    ipc->received++;
    if (msg->msg == WM_CLOSE && (!ipc->primary_hwnd || msg->hwnd == ipc->primary_hwnd)) ipc->close_seen = 1;
    if (ipc->shared) {
        ipc->shared->gui_msg_received = ipc->received;
        if (msg->msg == WM_CLOSE && (!ipc->primary_hwnd || msg->hwnd == ipc->primary_hwnd)) ipc->shared->gui_close_seen = 1;
        ipc->shared->gui_last_hwnd = msg->hwnd;
        ipc->shared->gui_last_msg = msg->msg;
        ipc->shared->gui_last_wparam = msg->wparam;
        ipc->shared->gui_last_lparam = msg->lparam;
        snprintf(ipc->shared->gui_last_text, sizeof(ipc->shared->gui_last_text), "%s", msg->text);
    }
    return 1;
}

static int child_queue_pop(ChildIpcContext* ipc, ChildIpcMessage* out)
{
    if (!ipc || ipc->q_count <= 0) return 0;
    if (out) *out = ipc->q[ipc->q_head];
    ipc->q_head = (ipc->q_head + 1) % MYOS_CHILD_QUEUE_CAP;
    ipc->q_count--;
    return 1;
}

static void child_ipc_send(ChildIpcContext* ipc, uint32_t opcode, uint32_t value, const char* text)
{
    if (!ipc || ipc->fd < 0) return;
    MyProcessIpcMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = MYOS_IPC_MAGIC;
    msg.version = MYOS_IPC_VERSION;
    msg.opcode = opcode;
    msg.my_pid = ipc->myPid;
    msg.child_pid = (uint32_t)getpid();
    msg.value = value;
    snprintf(msg.text, sizeof(msg.text), "%s", text ? text : "");
    (void)send(ipc->fd, &msg, sizeof(msg), 0);
}

static void child_shared_update(ChildIpcContext* ipc, const char* status, int argc, char** argv, uint32_t exitCode)
{
    if (!ipc || !ipc->shared) return;
    ipc->shared->magic = MYOS_IPC_SHARED_MAGIC;
    ipc->shared->version = MYOS_IPC_SHARED_VERSION;
    ipc->shared->my_pid = ipc->myPid;
    ipc->shared->child_pid = (uint32_t)getpid();
    ipc->shared->parent_pid = (uint32_t)getppid();
    ipc->shared->heartbeat++;
    ipc->shared->argc = (uint32_t)(argc < 0 ? 0 : argc);
    ipc->shared->exit_code = exitCode;
    snprintf(ipc->shared->image, sizeof(ipc->shared->image), "%s", ipc->image ? ipc->image : "console-child");
    snprintf(ipc->shared->status, sizeof(ipc->shared->status), "%s", status ? status : "running");
    child_make_argv_preview(ipc->shared->argv_preview, sizeof(ipc->shared->argv_preview), argc, argv);
}

static MyProcessIpcShared* child_open_shared(const char* name)
{
    if (!name || !name[0]) return NULL;
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return NULL;
    MyProcessIpcShared* p = (MyProcessIpcShared*)mmap(NULL, sizeof(MyProcessIpcShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    return p;
}

static int child_argdump_main(int argc, char** argv, ChildIpcContext* ipc)
{
    child_shared_update(ipc, "argdump-running", argc, argv, 0);
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)argc, "argdump argv parsed");
    printf("[v169 child pid=%ld] argdump main(argc=%d)", (long)getpid(), argc);
    for (int i = 0; i < argc; i++) printf(" argv[%d]='%s'", i, argv[i] ? argv[i] : "");
    printf("\n");
    fflush(stdout);
    child_shared_update(ipc, "argdump-exiting", argc, argv, 61);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 61, "argdump exit report");
    return 61;
}

static int child_sleeper_main(int argc, char** argv, ChildIpcContext* ipc)
{
    int seconds = 10;
    if (argc > 0 && argv && argv[0]) {
        int v = atoi(argv[0]);
        if (v > 0 && v < 120) seconds = v;
    }
    printf("[v169 child pid=%ld] sleeper sleeping %d sec\n", (long)getpid(), seconds);
    fflush(stdout);
    child_shared_update(ipc, "sleeper-running", argc, argv, 0);
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)seconds, "sleeper started");
    for (int i = 0; i < seconds; i++) {
        sleep(1);
        child_shared_update(ipc, "sleeper-heartbeat", argc, argv, 0);
        child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)(i + 1), "sleeper heartbeat");
    }
    printf("[v169 child pid=%ld] sleeper finished\n", (long)getpid());
    fflush(stdout);
    child_shared_update(ipc, "sleeper-exiting", argc, argv, 61);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 61, "sleeper exit report");
    return 61;
}



static void child_ipc_drain(ChildIpcContext* ipc)
{
    if (!ipc || ipc->fd < 0) return;
    for (;;) {
        MyProcessIpcMessage msg;
        ssize_t n = recv(ipc->fd, &msg, sizeof(msg), MSG_DONTWAIT);
        if (n == (ssize_t)sizeof(msg) && msg.magic == MYOS_IPC_MAGIC && msg.version == MYOS_IPC_VERSION) {
            if (msg.opcode == MYOS_IPC_OP_WINDOW_MESSAGE) {
                child_queue_push(ipc, &msg);
            } else if (msg.opcode == MYOS_IPC_OP_WINDOW_ACK) {
                ipc->hwnd = msg.value;
                ipc->window_ack_hwnd = msg.value;
                if (!ipc->primary_hwnd) ipc->primary_hwnd = msg.value;
                if (ipc->shared) ipc->shared->gui_hwnd = msg.value;
            } else if (msg.opcode == MYOS_IPC_OP_POST_ACK) {
                ipc->post_ack = msg.value ? 1u : 2u;
                if (ipc->shared) ipc->shared->gui_post_ack = ipc->post_ack;
            } else if (msg.opcode == MYOS_IPC_OP_DESTROY_ACK) {
                if (ipc->shared) ipc->shared->gui_destroy_ack = msg.value ? 1u : 2u;
            } else if (msg.opcode == MYOS_IPC_OP_CHILD_WINDOW_ACK) {
                ipc->last_child_hwnd = msg.value;
                if (ipc->shared) {
                    ipc->shared->child_hwnd_ack = msg.value ? 1u : 2u;
                    ipc->shared->child_hwnd_last = msg.value;
                    snprintf(ipc->shared->child_hwnd_status, sizeof(ipc->shared->child_hwnd_status), "%s", msg.text);
                }
            } else if (msg.opcode == MYOS_IPC_OP_CHILD_WINDOW_BATCH_ACK) {
                ipc->last_child_hwnd = msg.hwnd;
                if (ipc->shared) {
                    ipc->shared->child_hwnd_ack = msg.wparam ? 1u : 2u;
                    ipc->shared->child_hwnd_last = msg.hwnd;
                    ipc->shared->child_hwnd_count = msg.value;
                    snprintf(ipc->shared->child_hwnd_status, sizeof(ipc->shared->child_hwnd_status), "%s", msg.text);
                }
            } else if (msg.opcode == MYOS_IPC_OP_KERNEL_ACK) {
                if (ipc->shared) {
                    ipc->shared->kernel_ok = (uint32_t)msg.wparam;
                    ipc->shared->kernel_error = (uint32_t)msg.lparam;
                    ipc->shared->kernel_result = msg.value;
                    snprintf(ipc->shared->kernel_status, sizeof(ipc->shared->kernel_status), "%s", msg.text);
                }
            } else if (msg.opcode == MYOS_IPC_OP_MENU_ACK) {
                if (ipc->shared) {
                    ipc->shared->menu_ok = (uint32_t)msg.wparam;
                    ipc->shared->menu_ack = ipc->shared->menu_request;
                    snprintf(ipc->shared->menu_status, sizeof(ipc->shared->menu_status), "%s", msg.text);
                }
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        break;
    }
}

static int child_wait_for_window_ack(ChildIpcContext* ipc, int timeout_ms)
{
    if (!ipc || ipc->fd < 0) return 0;
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ipc->fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        int r = select(ipc->fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) child_ipc_drain(ipc);
        if (ipc->window_ack_hwnd) return (int)ipc->window_ack_hwnd;
        elapsed += 50;
    }
    child_ipc_drain(ipc);
    return (int)ipc->window_ack_hwnd;
}

static int child_wait_for_child_window_ack(ChildIpcContext* ipc, int timeout_ms)
{
    if (!ipc || ipc->fd < 0) return 0;
    ipc->last_child_hwnd = 0;
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ipc->fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        int r = select(ipc->fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) child_ipc_drain(ipc);
        if (ipc->last_child_hwnd) return (int)ipc->last_child_hwnd;
        elapsed += 50;
    }
    child_ipc_drain(ipc);
    return (int)ipc->last_child_hwnd;
}

static int child_wait_for_child_window_batch_ack(ChildIpcContext* ipc, uint32_t count, int timeout_ms)
{
    if (!ipc || ipc->fd < 0 || !ipc->shared) return 0;
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ipc->fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        int r = select(ipc->fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) child_ipc_drain(ipc);
        if (ipc->shared->child_hwnd_ack) {
            if (ipc->shared->child_hwnd_ack == 1u && ipc->shared->child_hwnd_count >= count) return 1;
            return 0;
        }
        elapsed += 50;
    }
    child_ipc_drain(ipc);
    return ipc->shared->child_hwnd_ack == 1u && ipc->shared->child_hwnd_count >= count ? 1 : 0;
}

static void child_post_message(ChildIpcContext* ipc, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, const char* text)
{
    if (!ipc || ipc->fd < 0) return;
    MyProcessIpcMessage m;
    memset(&m, 0, sizeof(m));
    m.magic = MYOS_IPC_MAGIC;
    m.version = MYOS_IPC_VERSION;
    m.opcode = MYOS_IPC_OP_POST_MESSAGE;
    m.my_pid = ipc->myPid;
    m.child_pid = (uint32_t)getpid();
    m.value = msg;
    m.hwnd = hwnd;
    m.msg = msg;
    m.wparam = wp;
    m.lparam = lp;
    snprintf(m.text, sizeof(m.text), "%s", text ? text : "child PostMessage request");
    if (ipc->shared) {
        ipc->shared->gui_post_request++;
        ipc->shared->gui_last_hwnd = hwnd;
        ipc->shared->gui_last_msg = msg;
        ipc->shared->gui_last_wparam = wp;
        ipc->shared->gui_last_lparam = lp;
        snprintf(ipc->shared->gui_last_text, sizeof(ipc->shared->gui_last_text), "%s", m.text);
    }
    (void)send(ipc->fd, &m, sizeof(m), 0);
}

static int child_dispatch_message(ChildIpcContext* ipc, const ChildIpcMessage* m)
{
    if (!ipc || !m) return 0;
    ipc->dispatched++;
    const char* name = "MSG";
    if (m->msg == WM_CREATE) name = "WM_CREATE";
    else if (m->msg == WM_PAINT) name = "WM_PAINT";
    else if (m->msg == WM_CLOSE) name = "WM_CLOSE";
    else if (m->msg == WM_MOVE) name = "WM_MOVE";
    else if (m->msg == WM_SIZE) name = "WM_SIZE";
    else if (m->msg == WM_GETMINMAXINFO) name = "WM_GETMINMAXINFO";
    else if (m->msg == WM_WINDOWPOSCHANGING) name = "WM_WINDOWPOSCHANGING";
    else if (m->msg == WM_WINDOWPOSCHANGED) name = "WM_WINDOWPOSCHANGED";
    else if (m->msg == WM_KEYDOWN) name = "WM_KEYDOWN";
    else if (m->msg == WM_KEYUP) name = "WM_KEYUP";
    else if (m->msg == WM_SYSKEYDOWN) name = "WM_SYSKEYDOWN";
    else if (m->msg == WM_SYSCOMMAND) name = "WM_SYSCOMMAND";
    else if (m->msg == WM_INITMENU) name = "WM_INITMENU";
    else if (m->msg == WM_INITMENUPOPUP) name = "WM_INITMENUPOPUP";
    else if (m->msg == WM_MENUSELECT) name = "WM_MENUSELECT";
    else if (m->msg == WM_ENTERMENULOOP) name = "WM_ENTERMENULOOP";
    else if (m->msg == WM_EXITMENULOOP) name = "WM_EXITMENULOOP";
    else if (m->msg == WM_SYSKEYUP) name = "WM_SYSKEYUP";
    else if (m->msg == WM_CHAR) name = "WM_CHAR";
    else if (m->msg == WM_SYSCHAR) name = "WM_SYSCHAR";
    else if (m->msg == WM_COMMAND) name = "WM_COMMAND";
    else if (m->msg == WM_MOUSEWHEEL) name = "WM_MOUSEWHEEL";
    else if (m->msg == WM_LBUTTONDOWN) name = "WM_LBUTTONDOWN";
    else if (m->msg == WM_LBUTTONUP) name = "WM_LBUTTONUP";
    else if (m->msg == WM_MOUSEMOVE) name = "WM_MOUSEMOVE";
    else if (m->msg == WM_CAPTURECHANGED) name = "WM_CAPTURECHANGED";
    else if (m->msg == WM_ENTERSIZEMOVE) name = "WM_ENTERSIZEMOVE";
    else if (m->msg == WM_EXITSIZEMOVE) name = "WM_EXITSIZEMOVE";
    else if (m->msg == WM_MYOS_SUBSCRIBED) name = "WM_MYOS_SUBSCRIBED";
    else if (m->msg == WM_MYOS_HWND_STATE_DIRTY) name = "WM_MYOS_HWND_STATE_DIRTY";
    else if (m->msg == (WM_USER + 0x61u)) name = "WM_USER+v61";
    if (m->msg == WM_CLOSE && (!ipc->primary_hwnd || m->hwnd == ipc->primary_hwnd)) ipc->close_seen = 1;
    if (ipc->shared) {
        ipc->shared->gui_msg_dispatched = ipc->dispatched;
        ipc->shared->gui_last_hwnd = m->hwnd;
        ipc->shared->gui_last_msg = m->msg;
        ipc->shared->gui_last_wparam = m->wparam;
        ipc->shared->gui_last_lparam = m->lparam;
        if (m->msg == WM_CLOSE && (!ipc->primary_hwnd || m->hwnd == ipc->primary_hwnd)) ipc->shared->gui_close_seen = 1;
        snprintf(ipc->shared->gui_last_text, sizeof(ipc->shared->gui_last_text), "DispatchMessage %s", name);
        snprintf(ipc->shared->status, sizeof(ipc->shared->status), "dispatched-%s", name);
    }
    MyProcessIpcMessage ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic = MYOS_IPC_MAGIC;
    ack.version = MYOS_IPC_VERSION;
    ack.opcode = MYOS_IPC_OP_WINDOW_DISPATCHED;
    ack.my_pid = ipc->myPid;
    ack.child_pid = (uint32_t)getpid();
    ack.value = m->msg;
    ack.hwnd = m->hwnd;
    ack.msg = m->msg;
    ack.wparam = m->wparam;
    ack.lparam = m->lparam;
    snprintf(ack.text, sizeof(ack.text), "DispatchMessage %s", name);
    (void)send(ipc->fd, &ack, sizeof(ack), 0);
    return (m->msg == WM_CLOSE && (!ipc->primary_hwnd || m->hwnd == ipc->primary_hwnd)) ? 0 : 1;
}

static int child_get_message(ChildIpcContext* ipc, ChildIpcMessage* out, int timeout_ms)
{
    int elapsed = 0;
    while (timeout_ms < 0 || elapsed <= timeout_ms) {
        child_ipc_drain(ipc);
        if (child_queue_pop(ipc, out)) return 1;
        fd_set rfds;
        FD_ZERO(&rfds);
        if (!ipc || ipc->fd < 0) return 0;
        FD_SET(ipc->fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        int r = select(ipc->fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) child_ipc_drain(ipc);
        if (timeout_ms >= 0) elapsed += 50;
    }
    return child_queue_pop(ipc, out);
}


/* AUDIT(v118): Child runtime locally duplicates WinAPI typedefs/constants below.
   This will drift from sdk/include headers unless the child stubs are split into
   SDK-including child_user32/child_winbase translation units. */

/* v61: child-side GUI IPC runtime API layer.
   These are deliberately tiny user32-like stubs.  The child process no longer
   pokes the IPC structs directly from the demo app; it calls RegisterClassExA,
   CreateWindowExA, PostMessageA, GetMessageA, DispatchMessageA and
   DestroyWindow.  The stubs marshal the scalar request over ProcessHost IPC.
   This is the layer we can later reuse for real out-of-process calculator/editor
   WinMain code. */
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef uint32_t HWND;
typedef uint16_t ATOM;
typedef uint64_t WPARAM;
typedef int64_t  LPARAM;
typedef int      BOOL;
typedef long     LRESULT;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t HANDLE;
typedef uint32_t HGLOBAL;
typedef uint32_t HMENU;
typedef uint32_t HACCEL;
typedef uint64_t UINT_PTR;
typedef void*    LPVOID;
typedef const void* LPCVOID;
typedef const char* LPCSTR;

typedef struct SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL  bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

#define INFINITE 0xffffffffu
#define WAIT_OBJECT_0 0x00000000u
#define WAIT_TIMEOUT 0x00000102u
#define WAIT_FAILED 0xffffffffu
#define EVENT_ALL_ACCESS 0x001f0003u
#define SYNCHRONIZE 0x00100000u
#define DUPLICATE_CLOSE_SOURCE 0x00000001u
#define DUPLICATE_SAME_ACCESS  0x00000002u
#define INVALID_HANDLE_VALUE ((HANDLE)0xffffffffu)
#define PAGE_READONLY   0x02u
#define PAGE_READWRITE  0x04u
#define FILE_MAP_READ   0x0004u
#define FILE_MAP_WRITE  0x0002u
#define FILE_MAP_ALL_ACCESS (FILE_MAP_READ|FILE_MAP_WRITE|0x0008u)

#define CF_TEXT 1u
#define GMEM_MOVEABLE 0x0002u
#define MF_STRING 0x0000u
#define MF_SEPARATOR 0x0800u
#define MF_POPUP 0x0010u
#define MF_DISABLED 0x0002u
#define MF_GRAYED   0x0001u
#define TPM_RIGHTBUTTON 0x0002u
#define TPM_RETURNCMD 0x0100u
#define TPM_NONOTIFY  0x0080u
#define FVIRTKEY 0x01u
#define FSHIFT   0x04u
#define FCONTROL 0x08u
#define FALT     0x10u
#define MYOS_KEYSTATE_SHIFT 0x0001u
#define MYOS_KEYSTATE_CTRL  0x0100u
#define MYOS_KEYSTATE_ALT   0x0200u

typedef struct tagACCEL { BYTE fVirt; WORD key; WORD cmd; } ACCEL, *LPACCEL;

typedef struct MSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
} MSG;

typedef struct WNDCLASSEXA {
    unsigned cbSize;
    unsigned style;
    WNDPROC lpfnWndProc;
    const char* lpszClassName;
} WNDCLASSEXA;

static ChildIpcContext* g_GuiIpcRuntime = NULL;
static WNDPROC g_GuiIpcWndProc = NULL;
static char g_GuiIpcClassName[MYOS_IPC_IMAGE_MAX] = "myOS.IPCProxyWindow";
static uint32_t g_GuiIpcApiCalls = 0;

static void gui_runtime_touch(const char* status)
{
    g_GuiIpcApiCalls++;
    if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) {
        g_GuiIpcRuntime->shared->gui_runtime_api_calls = g_GuiIpcApiCalls;
        if (status) snprintf(g_GuiIpcRuntime->shared->gui_runtime_status, sizeof(g_GuiIpcRuntime->shared->gui_runtime_status), "%s", status);
    }
}

static BOOL MyGuiIpcRuntimeAttach(ChildIpcContext* ipc)
{
    g_GuiIpcRuntime = ipc;
    gui_runtime_touch("runtime-attached");
    return ipc && ipc->fd >= 0 ? TRUE : FALSE;
}

static DWORD child_kernel_submit(ChildIpcContext* ipc, uint32_t op, int timeout_ms)
{
    if (!ipc || ipc->fd < 0 || !ipc->shared) return 0;
    MyProcessIpcShared* sh = ipc->shared;
    uint32_t seq = sh->kernel_request + 1u;
    if (!seq) seq = 1u;
    sh->kernel_enabled = 1;
    sh->kernel_request = seq;
    sh->kernel_ack = 0;
    sh->kernel_op = op;
    sh->kernel_ok = 0;
    sh->kernel_error = 0;
    sh->kernel_result = 0;
    snprintf(sh->kernel_status, sizeof(sh->kernel_status), "child KREQ op=%u seq=%u", op, seq);

    MyProcessIpcMessage m;
    memset(&m, 0, sizeof(m));
    m.magic = MYOS_IPC_MAGIC;
    m.version = MYOS_IPC_VERSION;
    m.opcode = MYOS_IPC_OP_KERNEL_REQUEST;
    m.my_pid = ipc->myPid;
    m.child_pid = (uint32_t)getpid();
    m.value = op;
    snprintf(m.text, sizeof(m.text), "kernel syscall-lite op=%u seq=%u", op, seq);
    if (send(ipc->fd, &m, sizeof(m), 0) != (ssize_t)sizeof(m)) return 0;

    int elapsed = 0;
    for (;;) {
        child_ipc_drain(ipc);
        if (sh->kernel_ack == seq) return sh->kernel_result;
        if (timeout_ms >= 0 && elapsed > timeout_ms) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ipc->fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        int r = select(ipc->fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) child_ipc_drain(ipc);
        if (sh->kernel_ack == seq) return sh->kernel_result;
        if (timeout_ms >= 0) elapsed += 50;
    }
    child_ipc_drain(ipc);
    return sh->kernel_ack == seq ? sh->kernel_result : 0;
}

static int child_kernel_wait_ack_budget(DWORD dwMilliseconds)
{
    if (dwMilliseconds == INFINITE) return -1;
    if (dwMilliseconds > 0x7ffff000u) return 0x7fffffff;
    return (int)dwMilliseconds + 1000;
}

static HANDLE CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared) return 0;
    MyProcessIpcShared* sh = ipc->shared;
    sh->kernel_flags = 0;
    if (lpEventAttributes && lpEventAttributes->bInheritHandle) sh->kernel_flags |= MYOS_KFLAG_INHERIT;
    if (bManualReset) sh->kernel_flags |= MYOS_KFLAG_MANUAL_RESET;
    if (bInitialState) sh->kernel_flags |= MYOS_KFLAG_INITIAL_STATE;
    sh->kernel_access = EVENT_ALL_ACCESS;
    snprintf(sh->kernel_name, sizeof(sh->kernel_name), "%s", lpName && lpName[0] ? lpName : "");
    return (HANDLE)child_kernel_submit(ipc, MYOS_KOP_CREATE_EVENT, 1200);
}

static HANDLE OpenEventA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared) return 0;
    MyProcessIpcShared* sh = ipc->shared;
    sh->kernel_flags = bInheritHandle ? MYOS_KFLAG_INHERIT : 0;
    sh->kernel_access = dwDesiredAccess ? dwDesiredAccess : EVENT_ALL_ACCESS;
    snprintf(sh->kernel_name, sizeof(sh->kernel_name), "%s", lpName && lpName[0] ? lpName : "");
    return (HANDLE)child_kernel_submit(ipc, MYOS_KOP_OPEN_EVENT, 1200);
}

static BOOL SetEvent(HANDLE hEvent)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared) return FALSE;
    ipc->shared->kernel_handle = hEvent;
    (void)child_kernel_submit(ipc, MYOS_KOP_SET_EVENT, 1200);
    return ipc->shared->kernel_ok ? TRUE : FALSE;
}

static BOOL ResetEvent(HANDLE hEvent)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared) return FALSE;
    ipc->shared->kernel_handle = hEvent;
    (void)child_kernel_submit(ipc, MYOS_KOP_RESET_EVENT, 1200);
    return ipc->shared->kernel_ok ? TRUE : FALSE;
}

static DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared) return WAIT_FAILED;
    ipc->shared->kernel_handle = hHandle;
    ipc->shared->kernel_timeout = dwMilliseconds;
    DWORD r = child_kernel_submit(ipc, MYOS_KOP_WAIT_ONE, child_kernel_wait_ack_budget(dwMilliseconds));
    return ipc->shared->kernel_ok ? r : WAIT_FAILED;
}

static DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared || !lpHandles || nCount == 0) return WAIT_FAILED;
    if (nCount > 8u) nCount = 8u;
    ipc->shared->kernel_count = nCount;
    ipc->shared->kernel_wait_all = bWaitAll ? 1u : 0u;
    ipc->shared->kernel_timeout = dwMilliseconds;
    for (DWORD i = 0; i < nCount; ++i) ipc->shared->kernel_handles[i] = lpHandles[i];
    DWORD r = child_kernel_submit(ipc, MYOS_KOP_WAIT_MANY, child_kernel_wait_ack_budget(dwMilliseconds));
    return ipc->shared->kernel_ok ? r : WAIT_FAILED;
}

static BOOL CloseHandle(HANDLE hObject)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared) return FALSE;
    ipc->shared->kernel_handle = hObject;
    (void)child_kernel_submit(ipc, MYOS_KOP_CLOSE_HANDLE, 1200);
    return ipc->shared->kernel_ok ? TRUE : FALSE;
}

static HANDLE GetCurrentProcess(void)
{
    return (HANDLE)0xffffffffu;
}

static BOOL DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                            HANDLE* lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions)
{
    (void)hSourceProcessHandle;
    (void)hTargetProcessHandle;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (lpTargetHandle) *lpTargetHandle = 0;
    if (!ipc || !ipc->shared || !lpTargetHandle || !hSourceHandle) return FALSE;
    MyProcessIpcShared* sh = ipc->shared;
    sh->kernel_handle = hSourceHandle;
    sh->kernel_access = dwDesiredAccess;
    sh->kernel_flags = bInheritHandle ? MYOS_KFLAG_INHERIT : 0;
    sh->kernel_options = dwOptions ? dwOptions : DUPLICATE_SAME_ACCESS;
    DWORD r = child_kernel_submit(ipc, MYOS_KOP_DUPLICATE_HANDLE, 1200);
    if (!sh->kernel_ok || !r) return FALSE;
    *lpTargetHandle = (HANDLE)r;
    return TRUE;
}


#define CHILD_MAX_MAPPED_SECTIONS 16

typedef struct ChildMappedSection {
    int valid;
    HANDLE hMap;
    LPVOID ptr;
    DWORD mapSize;
    DWORD access;
    char shmName[MYOS_IPC_IMAGE_MAX];
} ChildMappedSection;

static ChildMappedSection g_ChildSectionViews[CHILD_MAX_MAPPED_SECTIONS];

static HANDLE CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
                                 DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName)
{
    (void)hFile; (void)dwMaximumSizeHigh;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared) return 0;
    MyProcessIpcShared* sh = ipc->shared;
    sh->kernel_flags = (lpFileMappingAttributes && lpFileMappingAttributes->bInheritHandle) ? MYOS_KFLAG_INHERIT : 0;
    sh->kernel_access = FILE_MAP_ALL_ACCESS;
    sh->kernel_options = flProtect ? flProtect : PAGE_READWRITE;
    sh->kernel_size = dwMaximumSizeLow ? dwMaximumSizeLow : 4096u;
    sh->kernel_offset_low = sh->kernel_offset_high = 0;
    sh->kernel_map_size = 0;
    sh->kernel_map_name[0] = 0;
    snprintf(sh->kernel_name, sizeof(sh->kernel_name), "%s", lpName && lpName[0] ? lpName : "");
    return (HANDLE)child_kernel_submit(ipc, MYOS_KOP_CREATE_FILE_MAPPING, 1200);
}

static HANDLE OpenFileMappingA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpName)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared) return 0;
    MyProcessIpcShared* sh = ipc->shared;
    sh->kernel_flags = bInheritHandle ? MYOS_KFLAG_INHERIT : 0;
    sh->kernel_access = dwDesiredAccess ? dwDesiredAccess : FILE_MAP_ALL_ACCESS;
    sh->kernel_size = 0;
    sh->kernel_offset_low = sh->kernel_offset_high = 0;
    sh->kernel_map_size = 0;
    sh->kernel_map_name[0] = 0;
    snprintf(sh->kernel_name, sizeof(sh->kernel_name), "%s", lpName && lpName[0] ? lpName : "");
    return (HANDLE)child_kernel_submit(ipc, MYOS_KOP_OPEN_FILE_MAPPING, 1200);
}

static LPVOID MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                            DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, DWORD dwNumberOfBytesToMap)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared || !hFileMappingObject) return NULL;
    if (dwFileOffsetHigh || dwFileOffsetLow) return NULL; /* v71 keeps OOP mmap offset page-alignment simple: offset 0 only. */
    MyProcessIpcShared* sh = ipc->shared;
    sh->kernel_handle = hFileMappingObject;
    sh->kernel_access = dwDesiredAccess ? dwDesiredAccess : FILE_MAP_READ;
    sh->kernel_offset_high = dwFileOffsetHigh;
    sh->kernel_offset_low = dwFileOffsetLow;
    sh->kernel_size = dwNumberOfBytesToMap;
    sh->kernel_map_size = 0;
    sh->kernel_map_name[0] = 0;
    DWORD r = child_kernel_submit(ipc, MYOS_KOP_MAP_VIEW_OF_FILE, 1200);
    if (!sh->kernel_ok || !r || !sh->kernel_map_name[0] || !sh->kernel_map_size) return NULL;

    int wantWrite = (dwDesiredAccess & FILE_MAP_WRITE) ? 1 : 0;
    int openFlags = wantWrite ? O_RDWR : O_RDONLY;
    int fd = shm_open(sh->kernel_map_name, openFlags, 0);
    if (fd < 0 && !wantWrite) {
        /* Some old PoC sections were created/read back with O_RDWR-only assumptions.
           Prefer read-only for probes, but tolerate older backing objects. */
        fd = shm_open(sh->kernel_map_name, O_RDWR, 0);
    }
    if (fd < 0) {
        snprintf(sh->kernel_status, sizeof(sh->kernel_status),
                 "shm_open %.24s %s errno=%d",
                 sh->kernel_map_name, wantWrite ? "RW" : "RO", errno);
        return NULL;
    }
    int prot = PROT_READ;
    if (wantWrite) prot |= PROT_WRITE;
    LPVOID p = mmap(NULL, sh->kernel_map_size, prot, MAP_SHARED, fd, 0);
    int savedErrno = errno;
    close(fd);
    if (p == MAP_FAILED) {
        snprintf(sh->kernel_status, sizeof(sh->kernel_status),
                 "mmap %.24s bytes=%u prot=0x%x errno=%d",
                 sh->kernel_map_name, (unsigned)sh->kernel_map_size, prot, savedErrno);
        return NULL;
    }

    for (int i = 0; i < CHILD_MAX_MAPPED_SECTIONS; ++i) {
        if (!g_ChildSectionViews[i].valid) {
            g_ChildSectionViews[i].valid = 1;
            g_ChildSectionViews[i].hMap = hFileMappingObject;
            g_ChildSectionViews[i].ptr = p;
            g_ChildSectionViews[i].mapSize = sh->kernel_map_size;
            g_ChildSectionViews[i].access = dwDesiredAccess;
            snprintf(g_ChildSectionViews[i].shmName, sizeof(g_ChildSectionViews[i].shmName), "%s", sh->kernel_map_name);
            return p;
        }
    }
    munmap(p, sh->kernel_map_size);
    return NULL;
}

static BOOL UnmapViewOfFile(LPCVOID lpBaseAddress)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared || !lpBaseAddress) return FALSE;
    for (int i = 0; i < CHILD_MAX_MAPPED_SECTIONS; ++i) {
        ChildMappedSection* v = &g_ChildSectionViews[i];
        if (v->valid && v->ptr == lpBaseAddress) {
            if (v->mapSize) msync(v->ptr, v->mapSize, MS_SYNC);
            munmap(v->ptr, v->mapSize);
            ipc->shared->kernel_handle = v->hMap;
            (void)child_kernel_submit(ipc, MYOS_KOP_UNMAP_VIEW_OF_FILE, 1200);
            memset(v, 0, sizeof(*v));
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL FlushViewOfFile(LPCVOID lpBaseAddress, DWORD dwNumberOfBytesToFlush)
{
    if (!lpBaseAddress) return FALSE;
    for (int i = 0; i < CHILD_MAX_MAPPED_SECTIONS; ++i) {
        ChildMappedSection* v = &g_ChildSectionViews[i];
        if (v->valid && v->ptr == lpBaseAddress) {
            size_t n = dwNumberOfBytesToFlush ? (size_t)dwNumberOfBytesToFlush : (size_t)v->mapSize;
            if (n > v->mapSize) n = v->mapSize;
            return msync(v->ptr, n, MS_SYNC) == 0 ? TRUE : FALSE;
        }
    }
    return FALSE;
}

static void child_kernel_bridge_selftest(ChildIpcContext* ipc)
{
    if (!ipc || !ipc->shared) return;

    const char* name = "Local\\myos.v69.child.kernelbridge";
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, name);
    if (ev) {
        BOOL setOk = SetEvent(ev);
        DWORD wr = WaitForSingleObject(ev, 0);
        HANDLE ev2 = OpenEventA(SYNCHRONIZE, FALSE, name);
        HANDLE handles[2];
        handles[0] = ev;
        handles[1] = ev2 ? ev2 : ev;
        DWORD wm = WaitForMultipleObjects(ev2 ? 2u : 1u, handles, FALSE, 0);
        BOOL resetOk = ResetEvent(ev);
        BOOL close2 = ev2 ? CloseHandle(ev2) : TRUE;
        BOOL close1 = CloseHandle(ev);
        snprintf(ipc->shared->kernel_status, sizeof(ipc->shared->kernel_status),
                 "v70 kbridge ev=%x/%x s%u w%x m%x r%u c%u%u",
                 ev, ev2, setOk ? 1u : 0u, wr, wm, resetOk ? 1u : 0u, close1 ? 1u : 0u, close2 ? 1u : 0u);
    } else {
        snprintf(ipc->shared->kernel_status, sizeof(ipc->shared->kernel_status),
                 "v70 kbridge CreateEvent failed err=%u", ipc->shared->kernel_error);
    }
}

static ATOM RegisterClassExA(const WNDCLASSEXA* wc)
{
    gui_runtime_touch("RegisterClassExA");
    if (!g_GuiIpcRuntime || !wc || !wc->lpszClassName || !wc->lpszClassName[0]) return 0;
    snprintf(g_GuiIpcClassName, sizeof(g_GuiIpcClassName), "%s", wc->lpszClassName);
    g_GuiIpcWndProc = wc->lpfnWndProc;
    if (g_GuiIpcRuntime->shared) {
        g_GuiIpcRuntime->shared->gui_register_class_calls++;
        snprintf(g_GuiIpcRuntime->shared->gui_class, sizeof(g_GuiIpcRuntime->shared->gui_class), "%s", g_GuiIpcClassName);
    }
    return 0x61u;
}

static HWND CreateWindowExA(DWORD dwExStyle,
                            const char* lpClassName,
                            const char* lpWindowName,
                            DWORD dwStyle,
                            int x, int y, int w, int h,
                            HWND hWndParent,
                            void* hMenu,
                            void* hInstance,
                            void* lpParam)
{
    (void)dwExStyle; (void)dwStyle; (void)hWndParent; (void)hMenu; (void)hInstance; (void)lpParam;
    gui_runtime_touch("CreateWindowExA");
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared) return 0;
    const char* cls = (lpClassName && lpClassName[0]) ? lpClassName : g_GuiIpcClassName;
    const char* title = (lpWindowName && lpWindowName[0]) ? lpWindowName : "IPC GUI Child";

    if (dwStyle & WS_CHILD) {
        uint32_t slot = ipc->shared->child_hwnd_request % MYOS_IPC_MAX_CHILD_CONTROLS;
        ipc->shared->child_hwnd_request++;
        ipc->shared->child_hwnd_ack = 0;
        ipc->shared->child_hwnd_parent = hWndParent;
        ipc->shared->child_hwnd_last_id = (uint32_t)(uintptr_t)hMenu;
        ipc->shared->child_hwnd_x[0] = x;
        ipc->shared->child_hwnd_y[0] = y;
        ipc->shared->child_hwnd_w[0] = w;
        ipc->shared->child_hwnd_h[0] = h;
        ipc->shared->child_hwnd_ids[slot] = (uint32_t)(uintptr_t)hMenu;
        ipc->shared->child_hwnd_style[0] = (uint32_t)dwStyle;
        ipc->shared->child_hwnd_ex_style[0] = (uint32_t)dwExStyle;
        ipc->shared->child_hwnd_style[slot] = (uint32_t)dwStyle;
        ipc->shared->child_hwnd_ex_style[slot] = (uint32_t)dwExStyle;
        ipc->shared->child_hwnd_x[slot] = x;
        ipc->shared->child_hwnd_y[slot] = y;
        ipc->shared->child_hwnd_w[slot] = w;
        ipc->shared->child_hwnd_h[slot] = h;
        snprintf(ipc->shared->child_hwnd_class[0], sizeof(ipc->shared->child_hwnd_class[0]), "%s", cls);
        snprintf(ipc->shared->child_hwnd_text[0], sizeof(ipc->shared->child_hwnd_text[0]), "%s", title);
        snprintf(ipc->shared->child_hwnd_class[slot], sizeof(ipc->shared->child_hwnd_class[slot]), "%s", cls);
        snprintf(ipc->shared->child_hwnd_text[slot], sizeof(ipc->shared->child_hwnd_text[slot]), "%s", title);
        snprintf(ipc->shared->child_hwnd_status, sizeof(ipc->shared->child_hwnd_status), "request child hwnd class=%s id=%u", cls, (unsigned)(uintptr_t)hMenu);
        MyProcessIpcMessage m;
        memset(&m, 0, sizeof(m));
        m.magic = MYOS_IPC_MAGIC;
        m.version = MYOS_IPC_VERSION;
        m.opcode = MYOS_IPC_OP_CREATE_CHILD_WINDOW;
        m.my_pid = ipc->myPid;
        m.child_pid = (uint32_t)getpid();
        m.value = (uint32_t)(uintptr_t)hMenu;
        m.hwnd = hWndParent;
        m.wparam = ((uint64_t)dwExStyle << 32) | (uint64_t)dwStyle;
        m.lparam = (uint64_t)(uintptr_t)hMenu;
        snprintf(m.text, sizeof(m.text), "CreateWindowExA child %s", cls);
        (void)send(ipc->fd, &m, sizeof(m), 0);
        return (HWND)child_wait_for_child_window_ack(ipc, 1200);
    }

    ipc->window_ack_hwnd = 0;
    ipc->shared->gui_create_window_calls++;
    ipc->shared->gui_request++;
    ipc->shared->gui_ack = 0;
    ipc->shared->gui_hwnd = 0;
    ipc->shared->gui_window_index = 0;
    ipc->shared->gui_x = (uint32_t)x;
    ipc->shared->gui_y = (uint32_t)y;
    ipc->shared->gui_w = (uint32_t)w;
    ipc->shared->gui_h = (uint32_t)h;
    snprintf(ipc->shared->gui_class, sizeof(ipc->shared->gui_class), "%s", cls);
    snprintf(ipc->shared->gui_title, sizeof(ipc->shared->gui_title), "%s", title);
    snprintf(ipc->shared->status, sizeof(ipc->shared->status), "%s", "runtime-CreateWindowExA");
    MyProcessIpcMessage m;
    memset(&m, 0, sizeof(m));
    m.magic = MYOS_IPC_MAGIC;
    m.version = MYOS_IPC_VERSION;
    m.opcode = MYOS_IPC_OP_CREATE_WINDOW;
    m.my_pid = ipc->myPid;
    m.child_pid = (uint32_t)getpid();
    m.value = 1;
    m.hwnd = hWndParent; /* v172: owner for top-level windows; WS_CHILD uses child lane above. */
    m.wparam = ((uint64_t)dwExStyle << 32) | (uint64_t)dwStyle;
    snprintf(m.text, sizeof(m.text), "%s", title);
    (void)send(ipc->fd, &m, sizeof(m), 0);
    return (HWND)child_wait_for_window_ack(ipc, 1200);
}

typedef struct ChildCreateWindowExBatchItem {
    DWORD ex_style;
    const char* class_name;
    const char* title;
    DWORD style;
    int x;
    int y;
    int w;
    int h;
    HWND parent;
    void* menu;
} ChildCreateWindowExBatchItem;

static UINT CreateChildWindowsBatchA(ChildIpcContext* ipc,
                                     const ChildCreateWindowExBatchItem* items,
                                     HWND* out_hwnds,
                                     UINT count,
                                     const char* reason)
{
    if (!ipc || !ipc->shared || !items || !out_hwnds || !count) return 0;
    for (UINT i = 0; i < count; ++i) out_hwnds[i] = 0;
    if (count > MYOS_IPC_MAX_CHILD_CONTROLS) {
        UINT made = 0;
        for (UINT i = 0; i < count; ++i) {
            out_hwnds[i] = CreateWindowExA(items[i].ex_style, items[i].class_name, items[i].title,
                                           items[i].style, items[i].x, items[i].y, items[i].w, items[i].h,
                                           items[i].parent, items[i].menu, NULL, NULL);
            if (out_hwnds[i]) made++;
        }
        return made;
    }
    HWND parent = items[0].parent;
    for (UINT i = 0; i < count; ++i) {
        if (!(items[i].style & WS_CHILD) || items[i].parent != parent) {
            UINT made = 0;
            for (UINT j = 0; j < count; ++j) {
                out_hwnds[j] = CreateWindowExA(items[j].ex_style, items[j].class_name, items[j].title,
                                               items[j].style, items[j].x, items[j].y, items[j].w, items[j].h,
                                               items[j].parent, items[j].menu, NULL, NULL);
                if (out_hwnds[j]) made++;
            }
            return made;
        }
    }

    ipc->shared->child_hwnd_ack = 0;
    ipc->shared->child_hwnd_parent = parent;
    ipc->shared->child_hwnd_count = count;
    ipc->shared->child_hwnd_last = 0;
    ipc->shared->child_hwnd_last_id = 0;
    ipc->shared->child_hwnd_request += count;
    for (UINT i = 0; i < count; ++i) {
        const char* cls = (items[i].class_name && items[i].class_name[0]) ? items[i].class_name : "BUTTON";
        const char* title = (items[i].title && items[i].title[0]) ? items[i].title : "";
        ipc->shared->child_hwnd_ids[i] = (uint32_t)(uintptr_t)items[i].menu;
        ipc->shared->child_hwnd_hwnds[i] = 0;
        ipc->shared->child_hwnd_style[i] = (uint32_t)items[i].style;
        ipc->shared->child_hwnd_ex_style[i] = (uint32_t)items[i].ex_style;
        ipc->shared->child_hwnd_x[i] = items[i].x;
        ipc->shared->child_hwnd_y[i] = items[i].y;
        ipc->shared->child_hwnd_w[i] = items[i].w;
        ipc->shared->child_hwnd_h[i] = items[i].h;
        snprintf(ipc->shared->child_hwnd_class[i], sizeof(ipc->shared->child_hwnd_class[i]), "%s", cls);
        snprintf(ipc->shared->child_hwnd_text[i], sizeof(ipc->shared->child_hwnd_text[i]), "%s", title);
    }
    snprintf(ipc->shared->child_hwnd_status, sizeof(ipc->shared->child_hwnd_status),
             "batch request child hwnds count=%u%s%s", (unsigned)count,
             reason && reason[0] ? " " : "", reason && reason[0] ? reason : "");

    MyProcessIpcMessage m;
    memset(&m, 0, sizeof(m));
    m.magic = MYOS_IPC_MAGIC;
    m.version = MYOS_IPC_VERSION;
    m.opcode = MYOS_IPC_OP_CREATE_CHILD_WINDOW_BATCH;
    m.my_pid = ipc->myPid;
    m.child_pid = (uint32_t)getpid();
    m.value = count;
    m.hwnd = parent;
    snprintf(m.text, sizeof(m.text), "CreateWindowExA child batch %u", (unsigned)count);
    (void)send(ipc->fd, &m, sizeof(m), 0);

    if (!child_wait_for_child_window_batch_ack(ipc, count, 1200)) return 0;
    UINT made = 0;
    for (UINT i = 0; i < count; ++i) {
        out_hwnds[i] = (HWND)ipc->shared->child_hwnd_hwnds[i];
        if (out_hwnds[i]) made++;
    }
    return made;
}

static BOOL PostMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    gui_runtime_touch("PostMessageA");
    if (!g_GuiIpcRuntime || !hWnd) return FALSE;
    child_post_message(g_GuiIpcRuntime, hWnd, Msg, wParam, (uint64_t)lParam, "runtime PostMessageA");
    return TRUE;
}

static BOOL GetMessageA(MSG* lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    (void)hWnd; (void)wMsgFilterMin; (void)wMsgFilterMax;
    gui_runtime_touch("GetMessageA");
    if (!g_GuiIpcRuntime || !lpMsg) return FALSE;
    if (g_GuiIpcRuntime->shared) g_GuiIpcRuntime->shared->gui_get_message_calls++;
    ChildIpcMessage cm;
    if (!child_get_message(g_GuiIpcRuntime, &cm, 100)) return FALSE;
    lpMsg->hwnd = cm.hwnd;
    lpMsg->message = cm.msg;
    lpMsg->wParam = cm.wparam;
    lpMsg->lParam = (LPARAM)cm.lparam;
    lpMsg->time = (DWORD)time(NULL);
    return TRUE;
}

static LRESULT DispatchMessageA(const MSG* lpMsg)
{
    gui_runtime_touch("DispatchMessageA");
    if (!g_GuiIpcRuntime || !lpMsg) return 0;
    if (g_GuiIpcRuntime->shared) g_GuiIpcRuntime->shared->gui_dispatch_message_calls++;
    ChildIpcMessage cm;
    cm.hwnd = lpMsg->hwnd;
    cm.msg = lpMsg->message;
    cm.wparam = lpMsg->wParam;
    cm.lparam = (uint64_t)lpMsg->lParam;
    if (g_GuiIpcWndProc)
        g_GuiIpcWndProc(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
    return child_dispatch_message(g_GuiIpcRuntime, &cm);
}

static void child_gdi_compact_without_hwnd(ChildIpcContext* ipc, uint32_t hwnd);

static BOOL __attribute__((unused)) DestroyWindow(HWND hWnd)
{
    gui_runtime_touch("DestroyWindow");
    if (!g_GuiIpcRuntime || !hWnd || g_GuiIpcRuntime->fd < 0) return FALSE;
    child_gdi_compact_without_hwnd(g_GuiIpcRuntime, (uint32_t)hWnd);
    MyProcessIpcMessage m;
    memset(&m, 0, sizeof(m));
    m.magic = MYOS_IPC_MAGIC;
    m.version = MYOS_IPC_VERSION;
    m.opcode = MYOS_IPC_OP_DESTROY_WINDOW;
    m.my_pid = g_GuiIpcRuntime->myPid;
    m.child_pid = (uint32_t)getpid();
    m.value = hWnd;
    m.hwnd = hWnd;
    m.msg = WM_CLOSE;
    snprintf(m.text, sizeof(m.text), "%s", "runtime DestroyWindow request");
    if (g_GuiIpcRuntime->shared) {
        g_GuiIpcRuntime->shared->gui_destroy_window_calls++;
        g_GuiIpcRuntime->shared->gui_destroy_request++;
        g_GuiIpcRuntime->shared->gui_last_hwnd = hWnd;
        g_GuiIpcRuntime->shared->gui_last_msg = WM_CLOSE;
        snprintf(g_GuiIpcRuntime->shared->gui_runtime_status, sizeof(g_GuiIpcRuntime->shared->gui_runtime_status), "%s", "DestroyWindow requested");
    }
    return send(g_GuiIpcRuntime->fd, &m, sizeof(m), 0) == (ssize_t)sizeof(m) ? TRUE : FALSE;
}

typedef struct RECT { int left, top, right, bottom; } RECT;

static BOOL __attribute__((unused)) InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase)
{
    (void)lpRect; (void)bErase;
    gui_runtime_touch("InvalidateRect");
    if (!hWnd) return FALSE;
    return PostMessageA(hWnd, WM_PAINT, 0, 0);
}

static HWND __attribute__((unused)) SetCapture(HWND hWnd)
{
    gui_runtime_touch("SetCapture");
    if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) {
        g_GuiIpcRuntime->shared->paint_capture_count++;
        snprintf(g_GuiIpcRuntime->shared->gui_runtime_status, sizeof(g_GuiIpcRuntime->shared->gui_runtime_status), "%s", "SetCapture requested; parent auto-captures IPC HWND");
    }
    return hWnd;
}

static BOOL __attribute__((unused)) ReleaseCapture(void)
{
    gui_runtime_touch("ReleaseCapture");
    if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) {
        g_GuiIpcRuntime->shared->paint_release_count++;
        snprintf(g_GuiIpcRuntime->shared->gui_runtime_status, sizeof(g_GuiIpcRuntime->shared->gui_runtime_status), "%s", "ReleaseCapture requested; parent releases on button-up");
    }
    return TRUE;
}

static BOOL __attribute__((unused)) EnableWindow(HWND hWnd, BOOL bEnable)
{
    gui_runtime_touch("EnableWindow");
    if (!g_GuiIpcRuntime || !hWnd || g_GuiIpcRuntime->fd < 0) return FALSE;
    MyProcessIpcMessage m;
    memset(&m, 0, sizeof(m));
    m.magic = MYOS_IPC_MAGIC;
    m.version = MYOS_IPC_VERSION;
    m.opcode = MYOS_IPC_OP_ENABLE_WINDOW_REQ;
    m.my_pid = g_GuiIpcRuntime->myPid;
    m.child_pid = (uint32_t)getpid();
    m.value = bEnable ? 1u : 0u;
    m.hwnd = hWnd;
    m.msg = WM_ENABLE;
    m.wparam = bEnable ? 1u : 0u;
    snprintf(m.text, sizeof(m.text), "runtime EnableWindow(%u,%s)", (unsigned)hWnd, bEnable ? "TRUE" : "FALSE");
    if (g_GuiIpcRuntime->shared) {
        g_GuiIpcRuntime->shared->gui_last_hwnd = hWnd;
        g_GuiIpcRuntime->shared->gui_last_msg = WM_ENABLE;
        g_GuiIpcRuntime->shared->gui_last_wparam = bEnable ? 1u : 0u;
        snprintf(g_GuiIpcRuntime->shared->gui_runtime_status, sizeof(g_GuiIpcRuntime->shared->gui_runtime_status), "%s", m.text);
    }
    return send(g_GuiIpcRuntime->fd, &m, sizeof(m), 0) == (ssize_t)sizeof(m) ? TRUE : FALSE;
}

/* v70: child-side USER32-lite Clipboard/Menu/Accelerator runtime.
   Clipboard is truly cross-process: the child keeps only HGLOBAL-style local
   memory and sends Open/Empty/Set/Get/Close requests to the parent Session via
   ProcessHost IPC.  Menus/accelerators are child-local handles with IPC
   diagnostics; commands still enter the normal child WndProc as WM_COMMAND. */
#define CHILD_MAX_GLOBALS 32
#define CHILD_MAX_MENUS 16
#define CHILD_MAX_MENU_ITEMS 16
#define CHILD_MAX_ACCELS 8
#define CHILD_MAX_ACCEL_ITEMS 16

typedef struct ChildGlobalMem { int valid; HGLOBAL handle; size_t size; char* data; int locked; } ChildGlobalMem;
typedef struct ChildMenuItem { UINT flags; UINT_PTR id; char text[64]; } ChildMenuItem;
typedef struct ChildMenuLite { int valid; HMENU handle; int popup; HWND owner; int count; ChildMenuItem items[CHILD_MAX_MENU_ITEMS]; } ChildMenuLite;
typedef struct ChildAccelLite { int valid; HACCEL handle; int count; ACCEL items[CHILD_MAX_ACCEL_ITEMS]; } ChildAccelLite;

static ChildGlobalMem g_ChildGlobals[CHILD_MAX_GLOBALS];
static ChildMenuLite g_ChildMenus[CHILD_MAX_MENUS];
static ChildAccelLite g_ChildAccels[CHILD_MAX_ACCELS];
static HGLOBAL g_NextChildGlobal = 0xc0000001u;
static HMENU g_NextChildMenu = 0xc1000001u;
static HACCEL g_NextChildAccel = 0xc2000001u;
static HGLOBAL g_ChildClipboardLastGet = 0;

static ChildGlobalMem* child_find_global(HGLOBAL h)
{
    for (int i = 0; i < CHILD_MAX_GLOBALS; i++) if (g_ChildGlobals[i].valid && g_ChildGlobals[i].handle == h) return &g_ChildGlobals[i];
    return NULL;
}

static HGLOBAL GlobalAlloc(UINT uFlags, DWORD dwBytes)
{
    (void)uFlags;
    if (dwBytes == 0) dwBytes = 1;
    for (int i = 0; i < CHILD_MAX_GLOBALS; i++) {
        if (!g_ChildGlobals[i].valid) {
            char* p = (char*)calloc(1, dwBytes);
            if (!p) return 0;
            g_ChildGlobals[i].valid = 1;
            g_ChildGlobals[i].handle = g_NextChildGlobal++;
            g_ChildGlobals[i].size = (size_t)dwBytes;
            g_ChildGlobals[i].data = p;
            return g_ChildGlobals[i].handle;
        }
    }
    return 0;
}

static LPVOID GlobalLock(HGLOBAL hMem)
{
    ChildGlobalMem* g = child_find_global(hMem);
    if (!g) return NULL;
    g->locked++;
    return g->data;
}

static BOOL GlobalUnlock(HGLOBAL hMem)
{
    ChildGlobalMem* g = child_find_global(hMem);
    if (!g) return FALSE;
    if (g->locked > 0) g->locked--;
    return TRUE;
}

static HGLOBAL GlobalFree(HGLOBAL hMem)
{
    ChildGlobalMem* g = child_find_global(hMem);
    if (!g) return hMem;
    free(g->data);
    memset(g, 0, sizeof(*g));
    if (g_ChildClipboardLastGet == hMem) g_ChildClipboardLastGet = 0;
    return 0;
}

static int child_wait_for_shared_ack(ChildIpcContext* ipc, uint32_t* ackField, uint32_t expected, int timeout_ms)
{
    int elapsed = 0;
    while (ipc && ipc->shared && elapsed <= timeout_ms) {
        child_ipc_drain(ipc);
        if (*ackField == expected) return 1;
        usleep(20 * 1000);
        elapsed += 20;
    }
    return 0;
}

static BOOL child_send_clip_request(UINT op, HWND hwnd, UINT format, const char* text)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared || ipc->fd < 0) return FALSE;
    MyProcessIpcShared* sh = ipc->shared;
    sh->clip_enabled = 1;
    sh->clip_op = op;
    sh->clip_format = format ? format : CF_TEXT;
    sh->clip_ok = 0;
    uint32_t req = sh->clip_request + 1;
    sh->clip_request = req;
    if (text) snprintf(sh->clip_text, sizeof(sh->clip_text), "%s", text);
    MyProcessIpcMessage m;
    memset(&m, 0, sizeof(m));
    m.magic = MYOS_IPC_MAGIC;
    m.version = MYOS_IPC_VERSION;
    m.opcode = MYOS_IPC_OP_CLIPBOARD_REQUEST;
    m.my_pid = ipc->myPid;
    m.child_pid = (uint32_t)getpid();
    m.value = op;
    m.hwnd = hwnd;
    snprintf(m.text, sizeof(m.text), "clipboard op=%u", (unsigned)op);
    if (send(ipc->fd, &m, sizeof(m), 0) != (ssize_t)sizeof(m)) return FALSE;
    if (!child_wait_for_shared_ack(ipc, &sh->clip_ack, req, 800)) return FALSE;
    return sh->clip_ok ? TRUE : FALSE;
}

static BOOL OpenClipboard(HWND hWndNewOwner)
{
    gui_runtime_touch("OpenClipboard");
    return child_send_clip_request(MYOS_CLIP_OP_OPEN, hWndNewOwner, CF_TEXT, NULL);
}

static BOOL CloseClipboard(void)
{
    gui_runtime_touch("CloseClipboard");
    return child_send_clip_request(MYOS_CLIP_OP_CLOSE, g_GuiIpcRuntime && g_GuiIpcRuntime->shared ? g_GuiIpcRuntime->shared->gui_hwnd : 0, CF_TEXT, NULL);
}

static BOOL EmptyClipboard(void)
{
    gui_runtime_touch("EmptyClipboard");
    return child_send_clip_request(MYOS_CLIP_OP_EMPTY, g_GuiIpcRuntime && g_GuiIpcRuntime->shared ? g_GuiIpcRuntime->shared->gui_hwnd : 0, CF_TEXT, NULL);
}

static BOOL __attribute__((unused)) IsClipboardFormatAvailable(UINT format)
{
    gui_runtime_touch("IsClipboardFormatAvailable");
    return child_send_clip_request(MYOS_CLIP_OP_ISAVAIL, g_GuiIpcRuntime && g_GuiIpcRuntime->shared ? g_GuiIpcRuntime->shared->gui_hwnd : 0, format, NULL);
}

static HGLOBAL SetClipboardData(UINT uFormat, HGLOBAL hMem)
{
    gui_runtime_touch("SetClipboardData");
    ChildGlobalMem* g = child_find_global(hMem);
    const char* text = (g && g->data) ? g->data : "";
    BOOL ok = child_send_clip_request(MYOS_CLIP_OP_SET, g_GuiIpcRuntime && g_GuiIpcRuntime->shared ? g_GuiIpcRuntime->shared->gui_hwnd : 0, uFormat, text);
    return ok ? hMem : 0;
}

static HGLOBAL GetClipboardData(UINT uFormat)
{
    gui_runtime_touch("GetClipboardData");
    if (!child_send_clip_request(MYOS_CLIP_OP_GET, g_GuiIpcRuntime && g_GuiIpcRuntime->shared ? g_GuiIpcRuntime->shared->gui_hwnd : 0, uFormat, NULL)) return 0;
    const char* text = (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) ? g_GuiIpcRuntime->shared->clip_text : "";
    size_t n = strnlen(text, 191) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (DWORD)n);
    char* p = (char*)GlobalLock(h);
    if (p) { memcpy(p, text, n); GlobalUnlock(h); }
    if (g_ChildClipboardLastGet) GlobalFree(g_ChildClipboardLastGet);
    g_ChildClipboardLastGet = h;
    return h;
}

static void child_send_menu_diag(UINT op, HWND hwnd, HMENU menu, UINT_PTR cmd, const char* text)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (!ipc || !ipc->shared || ipc->fd < 0) return;
    MyProcessIpcShared* sh = ipc->shared;
    sh->menu_enabled = 1;
    sh->menu_op = op;
    sh->menu_last_handle = menu;
    sh->menu_last_command = (uint32_t)cmd;
    uint32_t req = sh->menu_request + 1;
    sh->menu_request = req;
    MyProcessIpcMessage m;
    memset(&m, 0, sizeof(m));
    m.magic = MYOS_IPC_MAGIC;
    m.version = MYOS_IPC_VERSION;
    m.opcode = MYOS_IPC_OP_MENU_REQUEST;
    m.my_pid = ipc->myPid;
    m.child_pid = (uint32_t)getpid();
    m.value = op;
    m.hwnd = hwnd;
    m.wparam = menu;
    m.lparam = (uint64_t)cmd;
    snprintf(m.text, sizeof(m.text), "%s", text && text[0] ? text : "menu op");
    (void)send(ipc->fd, &m, sizeof(m), 0);
}

static ChildMenuLite* child_find_menu(HMENU h)
{
    for (int i = 0; i < CHILD_MAX_MENUS; i++) if (g_ChildMenus[i].valid && g_ChildMenus[i].handle == h) return &g_ChildMenus[i];
    return NULL;
}

static HMENU child_create_menu(int popup)
{
    gui_runtime_touch(popup ? "CreatePopupMenu" : "CreateMenu");
    for (int i = 0; i < CHILD_MAX_MENUS; i++) {
        if (!g_ChildMenus[i].valid) {
            memset(&g_ChildMenus[i], 0, sizeof(g_ChildMenus[i]));
            g_ChildMenus[i].valid = 1;
            g_ChildMenus[i].popup = popup;
            g_ChildMenus[i].handle = g_NextChildMenu++;
            if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) g_GuiIpcRuntime->shared->menu_create_count++;
            child_send_menu_diag(MYOS_MENU_OP_CREATE, 0, g_ChildMenus[i].handle, 0, popup ? "CreatePopupMenu" : "CreateMenu");
            return g_ChildMenus[i].handle;
        }
    }
    return 0;
}

static HMENU CreateMenu(void) { return child_create_menu(0); }
static HMENU CreatePopupMenu(void) { return child_create_menu(1); }

static BOOL AppendMenuA(HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem)
{
    gui_runtime_touch("AppendMenuA");
    ChildMenuLite* m = child_find_menu(hMenu);
    if (!m || m->count >= CHILD_MAX_MENU_ITEMS) return FALSE;
    ChildMenuItem* it = &m->items[m->count++];
    it->flags = uFlags;
    it->id = uIDNewItem;
    snprintf(it->text, sizeof(it->text), "%s", lpNewItem ? lpNewItem : "");
    if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) g_GuiIpcRuntime->shared->menu_append_count++;
    child_send_menu_diag(MYOS_MENU_OP_APPEND, m->owner, hMenu, uIDNewItem, it->text);
    return TRUE;
}

static BOOL SetMenu(HWND hWnd, HMENU hMenu)
{
    gui_runtime_touch("SetMenu");
    ChildMenuLite* m = child_find_menu(hMenu);
    if (!m) return FALSE;
    m->owner = hWnd;
    if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) g_GuiIpcRuntime->shared->menu_set_count++;
    child_send_menu_diag(MYOS_MENU_OP_SET, hWnd, hMenu, 0, "SetMenu");
    return TRUE;
}

static BOOL DestroyMenu(HMENU hMenu)
{
    gui_runtime_touch("DestroyMenu");
    ChildMenuLite* m = child_find_menu(hMenu);
    if (!m) return FALSE;
    memset(m, 0, sizeof(*m));
    if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) g_GuiIpcRuntime->shared->menu_destroy_count++;
    child_send_menu_diag(MYOS_MENU_OP_DESTROY, 0, hMenu, 0, "DestroyMenu");
    return TRUE;
}

#define CHILD_MENU_MODAL_IDLE_CANCEL_MS 80
#define CHILD_MENU_ITEM_HEIGHT 18
#define CHILD_MENU_WIDTH 180

static LRESULT child_call_wndproc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (!g_GuiIpcWndProc) return 0;
    return g_GuiIpcWndProc(hWnd, Msg, wParam, lParam);
}

static int child_menu_item_invokable(const ChildMenuItem* it)
{
    if (!it) return 0;
    if (it->flags & MF_SEPARATOR) return 0;
    if (it->flags & (MF_DISABLED | MF_GRAYED)) return 0;
    if (it->flags & MF_POPUP) return 0;
    return it->id != 0;
}

static int child_menu_first_invokable(ChildMenuLite* m)
{
    if (!m) return -1;
    for (int i = 0; i < m->count; ++i) if (child_menu_item_invokable(&m->items[i])) return i;
    return -1;
}

static void child_menu_select_notify(HWND hWnd, ChildMenuLite* m, int pos)
{
    if (!hWnd || !m || pos < 0 || pos >= m->count) return;
    ChildMenuItem* it = &m->items[pos];
    child_call_wndproc(hWnd, WM_MENUSELECT, MAKEWPARAM((WORD)it->id, (WORD)(it->flags & 0xffffu)), (LPARAM)m->handle);
}

static int child_menu_next_invokable(ChildMenuLite* m, int selected, int dir)
{
    if (!m || m->count <= 0) return -1;
    int start = selected;
    if (start < 0 || start >= m->count) start = 0;
    for (int step = 0; step < m->count; ++step) {
        int idx = (start + (dir > 0 ? step + 1 : m->count - step - 1)) % m->count;
        if (child_menu_item_invokable(&m->items[idx])) return idx;
    }
    return -1;
}

static int child_menu_index_from_point(ChildMenuLite* m, int x, int y, uint64_t lp)
{
    if (!m) return -1;
    int px = GET_X_LPARAM(lp);
    int py = GET_Y_LPARAM(lp);
    if (px < x || px >= x + CHILD_MENU_WIDTH) return -1;
    int row = (py - y) / CHILD_MENU_ITEM_HEIGHT;
    if (row < 0 || row >= m->count) return -1;
    return child_menu_item_invokable(&m->items[row]) ? row : -1;
}

static BOOL TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd, const void* prcRect)
{
    (void)prcRect;
    gui_runtime_touch("TrackPopupMenu");
    if (nReserved != 0) return FALSE;
    ChildMenuLite* m = child_find_menu(hMenu);
    if (!m) return FALSE;

    int selected = child_menu_first_invokable(m);
    if (selected < 0) {
        child_send_menu_diag(MYOS_MENU_OP_TRACK, hWnd, hMenu, 0, "TrackPopupMenu cancel empty/disabled");
        return FALSE;
    }

    if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) {
        g_GuiIpcRuntime->shared->menu_popup_count++;
        g_GuiIpcRuntime->shared->menu_last_command = 0;
    }
    child_send_menu_diag(MYOS_MENU_OP_TRACK, hWnd, hMenu, 0, "TrackPopupMenu modal begin");

    child_call_wndproc(hWnd, WM_ENTERMENULOOP, TRUE, 0);
    child_call_wndproc(hWnd, WM_INITMENU, (WPARAM)hMenu, 0);
    child_call_wndproc(hWnd, WM_INITMENUPOPUP, (WPARAM)hMenu, MAKELPARAM((WORD)0, FALSE));
    child_menu_select_notify(hWnd, m, selected);

    UINT_PTR cmd = 0;
    int cancel = 0;
    ChildIpcMessage cm;
    if (!child_get_message(g_GuiIpcRuntime, &cm, CHILD_MENU_MODAL_IDLE_CANCEL_MS)) {
        cancel = 1;
    } else {
        int commit = 0;
        if (cm.msg == WM_KEYDOWN || cm.msg == WM_SYSKEYDOWN) {
            if ((int)cm.wparam == KEY_ESC) cancel = 1;
            else if ((int)cm.wparam == KEY_DOWN) { selected = child_menu_next_invokable(m, selected, +1); child_menu_select_notify(hWnd, m, selected); }
            else if ((int)cm.wparam == KEY_UP) { selected = child_menu_next_invokable(m, selected, -1); child_menu_select_notify(hWnd, m, selected); }
            else if ((int)cm.wparam == KEY_ENTER || (int)cm.wparam == KEY_SPACE) commit = 1;
            else child_dispatch_message(g_GuiIpcRuntime, &cm);
        } else if (cm.msg == WM_LBUTTONDOWN || cm.msg == WM_LBUTTONUP || cm.msg == WM_RBUTTONDOWN || cm.msg == WM_RBUTTONUP) {
            int right = (cm.msg == WM_RBUTTONDOWN || cm.msg == WM_RBUTTONUP);
            int buttonAllowed = right ? ((uFlags & TPM_RIGHTBUTTON) != 0) : TRUE;
            if (!buttonAllowed) cancel = 1;
            else {
                int idx = child_menu_index_from_point(m, x, y, cm.lparam);
                if (idx < 0) cancel = 1;
                else { selected = idx; child_menu_select_notify(hWnd, m, selected); if (cm.msg == WM_LBUTTONUP || cm.msg == WM_RBUTTONUP) commit = 1; }
            }
        } else if (cm.msg == WM_CANCELMODE || cm.msg == WM_CLOSE) {
            cancel = 1;
            if (cm.msg == WM_CLOSE) child_dispatch_message(g_GuiIpcRuntime, &cm);
        } else {
            child_dispatch_message(g_GuiIpcRuntime, &cm);
        }
        if (commit && selected >= 0 && selected < m->count) cmd = m->items[selected].id;
    }

    child_call_wndproc(hWnd, WM_MENUSELECT, MAKEWPARAM(0, 0xffffu), 0);
    child_call_wndproc(hWnd, WM_EXITMENULOOP, TRUE, 0);

    if (!cmd || cancel) {
        child_send_menu_diag(MYOS_MENU_OP_TRACK, hWnd, hMenu, 0, "TrackPopupMenu modal cancel");
        return FALSE;
    }

    if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) g_GuiIpcRuntime->shared->menu_last_command = (uint32_t)cmd;
    child_send_menu_diag(MYOS_MENU_OP_TRACK, hWnd, hMenu, cmd, "TrackPopupMenu modal commit");
    if (uFlags & TPM_RETURNCMD) return (BOOL)cmd;
    if (!(uFlags & TPM_NONOTIFY)) {
        if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) g_GuiIpcRuntime->shared->menu_command_count++;
        child_call_wndproc(hWnd, WM_COMMAND, (WPARAM)cmd, 0);
    }
    return TRUE;
}

static ChildAccelLite* child_find_accel(HACCEL h)
{
    for (int i = 0; i < CHILD_MAX_ACCELS; i++) if (g_ChildAccels[i].valid && g_ChildAccels[i].handle == h) return &g_ChildAccels[i];
    return NULL;
}

static HACCEL CreateAcceleratorTableA(LPACCEL lpaccl, int cEntries)
{
    gui_runtime_touch("CreateAcceleratorTableA");
    if (!lpaccl || cEntries <= 0) return 0;
    if (cEntries > CHILD_MAX_ACCEL_ITEMS) cEntries = CHILD_MAX_ACCEL_ITEMS;
    for (int i = 0; i < CHILD_MAX_ACCELS; i++) {
        if (!g_ChildAccels[i].valid) {
            memset(&g_ChildAccels[i], 0, sizeof(g_ChildAccels[i]));
            g_ChildAccels[i].valid = 1;
            g_ChildAccels[i].handle = g_NextChildAccel++;
            g_ChildAccels[i].count = cEntries;
            for (int j = 0; j < cEntries; j++) g_ChildAccels[i].items[j] = lpaccl[j];
            if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) g_GuiIpcRuntime->shared->accel_count++;
            child_send_menu_diag(MYOS_MENU_OP_ACCEL, 0, (HMENU)g_ChildAccels[i].handle, 0, "CreateAcceleratorTableA");
            return g_ChildAccels[i].handle;
        }
    }
    return 0;
}

static BOOL DestroyAcceleratorTable(HACCEL hAccel)
{
    gui_runtime_touch("DestroyAcceleratorTable");
    ChildAccelLite* a = child_find_accel(hAccel);
    if (!a) return FALSE;
    memset(a, 0, sizeof(*a));
    return TRUE;
}

static WORD child_accel_normalize_key(WORD key)
{
    if (key >= 'a' && key <= 'z') return (WORD)(key - 'a' + 'A');
    return key;
}

static WORD child_accel_linux_key_to_vk(WORD key)
{
    switch (key) {
    case KEY_A: return 'A'; case KEY_B: return 'B'; case KEY_C: return 'C'; case KEY_D: return 'D';
    case KEY_E: return 'E'; case KEY_F: return 'F'; case KEY_G: return 'G'; case KEY_H: return 'H';
    case KEY_I: return 'I'; case KEY_J: return 'J'; case KEY_K: return 'K'; case KEY_L: return 'L';
    case KEY_M: return 'M'; case KEY_N: return 'N'; case KEY_O: return 'O'; case KEY_P: return 'P';
    case KEY_Q: return 'Q'; case KEY_R: return 'R'; case KEY_S: return 'S'; case KEY_T: return 'T';
    case KEY_U: return 'U'; case KEY_V: return 'V'; case KEY_W: return 'W'; case KEY_X: return 'X';
    case KEY_Y: return 'Y'; case KEY_Z: return 'Z';
    case KEY_1: return '1'; case KEY_2: return '2'; case KEY_3: return '3'; case KEY_4: return '4';
    case KEY_5: return '5'; case KEY_6: return '6'; case KEY_7: return '7'; case KEY_8: return '8';
    case KEY_9: return '9'; case KEY_0: return '0';
    default: return key;
    }
}

static int child_accel_key_matches(const ACCEL* ac, UINT message, WORD msgKey)
{
    if (!ac) return 0;
    WORD accelKey = child_accel_normalize_key(ac->key);
    WORD rawKey = child_accel_normalize_key(msgKey);
    WORD vkKey = child_accel_normalize_key(child_accel_linux_key_to_vk(msgKey));
    if (ac->fVirt & FVIRTKEY) {
        if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return 0;
        return accelKey == rawKey || accelKey == vkKey;
    }
    if (message != WM_CHAR && message != WM_SYSCHAR && message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return 0;
    return accelKey == rawKey || accelKey == vkKey;
}

static int TranslateAcceleratorA(HWND hWnd, HACCEL hAccTable, MSG* lpMsg)
{
    gui_runtime_touch("TranslateAcceleratorA");
    if (!lpMsg || (lpMsg->message != WM_KEYDOWN && lpMsg->message != WM_SYSKEYDOWN &&
                   lpMsg->message != WM_CHAR && lpMsg->message != WM_SYSCHAR)) return 0;
    ChildAccelLite* a = child_find_accel(hAccTable);
    if (!a) return 0;
    WORD key = (WORD)lpMsg->wParam;
    UINT state = (UINT)lpMsg->lParam;
    WORD cmd = 0;
    UINT matchedVirt = 0;
    for (int i = 0; i < a->count; i++) {
        ACCEL* ac = &a->items[i];
        int ctrl_required = (ac->fVirt & FCONTROL) ? 1 : 0;
        int shift_required = (ac->fVirt & FSHIFT) ? 1 : 0;
        int alt_required = (ac->fVirt & FALT) ? 1 : 0;
        int ctrl_down = (state & MYOS_KEYSTATE_CTRL) ? 1 : 0;
        int shift_down = (state & MYOS_KEYSTATE_SHIFT) ? 1 : 0;
        int alt_down = (state & MYOS_KEYSTATE_ALT) ? 1 : 0;
        if (ctrl_required == ctrl_down && shift_required == shift_down && alt_required == alt_down &&
            child_accel_key_matches(ac, lpMsg->message, key)) { cmd = ac->cmd; matchedVirt = ac->fVirt; break; }
    }
    if (!cmd) return 0;
    if (g_GuiIpcRuntime && g_GuiIpcRuntime->shared) {
        g_GuiIpcRuntime->shared->accel_translate_count++;
        g_GuiIpcRuntime->shared->menu_last_command = cmd;
    }
    if (g_GuiIpcWndProc) {
        UINT outMsg = (cmd >= 0xF000u || lpMsg->message == WM_SYSKEYDOWN || lpMsg->message == WM_SYSCHAR || (matchedVirt & FALT)) ? WM_SYSCOMMAND : WM_COMMAND;
        g_GuiIpcWndProc(hWnd, outMsg, (WPARAM)cmd, 1);
    }
    child_send_menu_diag(MYOS_MENU_OP_ACCEL, hWnd, (HMENU)hAccTable, cmd, "TranslateAcceleratorA -> command");
    return 1;
}


/* v63
   The WndProc and calculator state live in this Linux child.  The parent still
   draws into its framebuffer, reading the compact state below from the shared
   IPC section.  This is intentionally the bridge before shared render surfaces. */
typedef struct CalcRect { int x, y, w, h; } CalcRect;
typedef struct CalcLayoutLite { CalcRect buttons[5][4]; int pad, gap; int cw, ch; } CalcLayoutLite;
typedef struct ChildCalcState {
    HWND hwnd;
    char display[64];
    double operand;
    char op;
    int fresh;
    int error;
    char history[5][80];
    int hist_count;
    int hist_next;
    uint32_t revision;
    uint32_t button_hits;
    char last_button[16];
} ChildCalcState;

static ChildCalcState g_ChildCalc;

typedef struct ChildCalcButton { const char* label; char action; } ChildCalcButton;
static const ChildCalcButton g_child_calc_buttons[5][4] = {
    {{"C", 'C'}, {"+-",'N'}, {"%", 'P'}, {"/", '/'}},
    {{"7", '7'}, {"8", '8'}, {"9", '9'}, {"*", '*'}},
    {{"4", '4'}, {"5", '5'}, {"6", '6'}, {"-", '-'}},
    {{"1", '1'}, {"2", '2'}, {"3", '3'}, {"+", '+'}},
    {{"0", '0'}, {".", '.'}, {"<-",'B'}, {"=", '='}},
};

static int cclamp_i(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }

static void child_calc_layout(CalcLayoutLite* l, int cw, int ch)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->cw = cw; l->ch = ch;
    int base = cw < ch ? cw : ch;
    l->pad = cclamp_i(base / 28, 6, 14);
    l->gap = cclamp_i(base / 55, 4, 10);
    int pad = l->pad, gap = l->gap;
    int display_h = cclamp_i(ch / 7, 38, 62);
    int y = pad + display_h + gap;
    int bottom_pad = pad + 6;
    int avail_after_display = ch - y - bottom_pad;
    int button_gap_total = gap * 4;
    int history_h = 0;
    int ideal_button_h = (avail_after_display - button_gap_total) / 5;
    if (ch >= 330) {
        history_h = cclamp_i(ch / 5, 48, 104);
        ideal_button_h = (avail_after_display - history_h - gap - button_gap_total) / 5;
        if (ideal_button_h < 30) {
            history_h = cclamp_i(ch / 8, 32, 56);
            ideal_button_h = (avail_after_display - history_h - gap - button_gap_total) / 5;
        }
    }
    if (history_h >= 28 && ideal_button_h >= 26) y += history_h + gap;
    int bw = (cw - pad * 2 - gap * 3) / 4;
    int bh = (ch - y - bottom_pad - gap * 4) / 5;
    if (bh < 22) bh = 22;
    if (bw < 34) bw = 34;
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            l->buttons[row][col].x = pad + col * (bw + gap);
            l->buttons[row][col].y = y + row * (bh + gap);
            l->buttons[row][col].w = bw;
            l->buttons[row][col].h = bh;
        }
    }
}

#define CHILD_COLOR(r,g,b) ((uint32_t)((b) | ((g) << 8) | ((r) << 16)))

typedef struct ChildGdiRect { int x, y, w, h; } ChildGdiRect;
typedef struct ChildGdiLayout {
    ChildGdiRect display;
    ChildGdiRect history;
    ChildGdiRect buttons[5][4];
    int history_visible;
    int pad;
    int gap;
} ChildGdiLayout;

static void child_gdi_layout(ChildGdiLayout* l, int cw, int ch)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    int base = cw < ch ? cw : ch;
    l->pad = cclamp_i(base / 28, 6, 14);
    l->gap = cclamp_i(base / 55, 4, 10);
    int pad = l->pad, gap = l->gap;
    int display_h = cclamp_i(ch / 7, 38, 62);
    l->display.x = pad; l->display.y = pad; l->display.w = cw - pad * 2; l->display.h = display_h;
    int y = l->display.y + l->display.h + gap;
    int bottom_pad = pad + 6;
    int avail_after_display = ch - y - bottom_pad;
    int button_gap_total = gap * 4;
    int ideal_button_h = (avail_after_display - button_gap_total) / 5;
    int history_h = 0;
    if (ch >= 330) {
        history_h = cclamp_i(ch / 5, 48, 104);
        ideal_button_h = (avail_after_display - history_h - gap - button_gap_total) / 5;
        if (ideal_button_h < 30) {
            history_h = cclamp_i(ch / 8, 32, 56);
            ideal_button_h = (avail_after_display - history_h - gap - button_gap_total) / 5;
        }
    }
    if (history_h >= 28 && ideal_button_h >= 26) {
        l->history_visible = 1;
        l->history.x = pad; l->history.y = y; l->history.w = cw - pad * 2; l->history.h = history_h;
        y += history_h + gap;
    }
    int bw = (cw - pad * 2 - gap * 3) / 4;
    int bh = (ch - y - bottom_pad - gap * 4) / 5;
    if (bh < 22) bh = 22;
    if (bw < 34) bw = 34;
    for (int row = 0; row < 5; row++) for (int col = 0; col < 4; col++) {
        l->buttons[row][col].x = pad + col * (bw + gap);
        l->buttons[row][col].y = y + row * (bh + gap);
        l->buttons[row][col].w = bw;
        l->buttons[row][col].h = bh;
    }
}

/* v169: retained cross-process GDI is now HWND-targeted.
   v168 had one per-process command buffer, so two top-level windows owned by
   the same OOP child could both replay the last painted surface.  Win32 paints
   a window tree/DC, not a process.  Keep one scalar IPC array for ABI simplicity,
   but treat it as a compact set of retained per-HWND command streams. */
static uint32_t g_ChildGdiTargetHwnd = 0;
static uint32_t g_ChildGdiTargetSeq = 0;

static uint32_t child_gdi_default_hwnd(ChildIpcContext* ipc)
{
    if (!ipc) return 0;
    if (ipc->primary_hwnd) return ipc->primary_hwnd;
    if (ipc->shared && ipc->shared->gui_hwnd) return ipc->shared->gui_hwnd;
    if (ipc->hwnd) return ipc->hwnd;
    return 0;
}

static void child_gdi_compact_without_hwnd(ChildIpcContext* ipc, uint32_t hwnd)
{
    if (!ipc || !ipc->shared) return;
    uint32_t n = ipc->shared->gdi_command_count;
    if (n > MYOS_GDI_MAX_COMMANDS) n = MYOS_GDI_MAX_COMMANDS;
    uint32_t out = 0;
    for (uint32_t i = 0; i < n; ++i) {
        MyGdiIpcCommand* c = &ipc->shared->gdi_commands[i];
        if (c->opcode == MYOS_GDI_OP_NONE) continue;
        if (hwnd && c->hwnd == hwnd) continue;
        if (out != i) ipc->shared->gdi_commands[out] = *c;
        out++;
    }
    for (uint32_t i = out; i < n; ++i) memset(&ipc->shared->gdi_commands[i], 0, sizeof(ipc->shared->gdi_commands[i]));
    ipc->shared->gdi_command_count = out;
}

static void child_gdi_reset_for_hwnd(ChildIpcContext* ipc, HWND hwnd, int cw, int ch, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    uint32_t target = (uint32_t)hwnd;
    if (!target) target = child_gdi_default_hwnd(ipc);
    g_ChildGdiTargetHwnd = target;
    g_ChildGdiTargetSeq++;
    ipc->shared->gdi_enabled = 1;
    child_gdi_compact_without_hwnd(ipc, target);
    ipc->shared->gdi_client_w = (uint32_t)(cw > 0 ? cw : 0);
    ipc->shared->gdi_client_h = (uint32_t)(ch > 0 ? ch : 0);
    ipc->shared->gdi_last_msg = reason;
    snprintf(ipc->shared->gdi_status, sizeof(ipc->shared->gdi_status), "HWND=%u %s",
             (unsigned)target, status ? status : "WM_PAINT/GDI");
}

static void child_gdi_reset(ChildIpcContext* ipc, int cw, int ch, UINT reason, const char* status)
{
    child_gdi_reset_for_hwnd(ipc, 0, cw, ch, reason, status);
}

static MyGdiIpcCommand* child_gdi_add(ChildIpcContext* ipc, uint32_t op)
{
    if (!ipc || !ipc->shared) return NULL;
    uint32_t n = ipc->shared->gdi_command_count;
    if (n >= MYOS_GDI_MAX_COMMANDS) return NULL;
    MyGdiIpcCommand* c = &ipc->shared->gdi_commands[n];
    memset(c, 0, sizeof(*c));
    c->opcode = op;
    c->hwnd = g_ChildGdiTargetHwnd ? g_ChildGdiTargetHwnd : child_gdi_default_hwnd(ipc);
    c->stream_seq = g_ChildGdiTargetSeq;
    ipc->shared->gdi_command_count = n + 1;
    return c;
}

static void child_gdi_fill(ChildIpcContext* ipc, int x, int y, int w, int h, uint32_t color)
{
    MyGdiIpcCommand* c = child_gdi_add(ipc, MYOS_GDI_OP_FILLRECT);
    if (!c) return;
    c->x = x; c->y = y; c->w = w; c->h = h; c->color = color;
}

static void child_gdi_rect(ChildIpcContext* ipc, int x, int y, int w, int h, uint32_t color)
{
    MyGdiIpcCommand* c = child_gdi_add(ipc, MYOS_GDI_OP_RECTANGLE);
    if (!c) return;
    c->x = x; c->y = y; c->w = w; c->h = h; c->color = color;
}

static void child_gdi_line(ChildIpcContext* ipc, int x0, int y0, int x1, int y1, uint32_t color)
{
    MyGdiIpcCommand* c = child_gdi_add(ipc, MYOS_GDI_OP_LINE);
    if (!c) return;
    c->x = x0; c->y = y0; c->w = x1; c->h = y1; c->color = color;
}

static void child_copy_cstr_bounded_n(char* dst, size_t dst_cap, const char* src, size_t src_cap)
{
    if (!dst || dst_cap == 0) return;
    if (!src || src_cap == 0) {
        dst[0] = '\0';
        return;
    }

    size_t max = dst_cap - 1;
    if (max > src_cap) max = src_cap;

    size_t n = 0;
    while (n < max && src[n] != '\0') {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

static void child_copy_cstr_bounded(char* dst, size_t dst_cap, const char* src)
{
    child_copy_cstr_bounded_n(dst, dst_cap, src, SIZE_MAX);
}

static void child_gdi_text_n(ChildIpcContext* ipc, int x, int y, int w, int h, const char* text, size_t text_cap, uint32_t color, uint32_t flags)
{
    MyGdiIpcCommand* c = child_gdi_add(ipc, MYOS_GDI_OP_DRAWTEXT);
    if (!c) return;
    c->x = x; c->y = y; c->w = w; c->h = h; c->color = color; c->flags = flags;
    child_copy_cstr_bounded_n(c->text, sizeof(c->text), text, text_cap);
}

static void child_gdi_text(ChildIpcContext* ipc, int x, int y, int w, int h, const char* text, uint32_t color, uint32_t flags)
{
    child_gdi_text_n(ipc, x, y, w, h, text, SIZE_MAX, color, flags);
}

static void child_calc_render_gdi(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    int win_w = (int)ipc->shared->gui_w;
    int win_h = (int)ipc->shared->gui_h;
    int cw = win_w - 2;
    int ch = win_h - 24 - 1;
    if (cw < 40) cw = 40;
    if (ch < 40) ch = 40;
    ChildGdiLayout l;
    child_gdi_layout(&l, cw, ch);
    child_gdi_reset(ipc, cw, ch, reason, status);

    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(23,23,34));
    child_gdi_fill(ipc, l.display.x, l.display.y, l.display.w, l.display.h, CHILD_COLOR(13,13,22));
    child_gdi_rect(ipc, l.display.x, l.display.y, l.display.w, l.display.h, CHILD_COLOR(85,85,120));
    if (g_ChildCalc.op) {
        char opbuf[32];
        snprintf(opbuf, sizeof(opbuf), "%.8g %c", g_ChildCalc.operand, g_ChildCalc.op);
        child_gdi_text_n(ipc, l.display.x + 6, l.display.y + 6, l.display.w - 12, 12, opbuf, sizeof(opbuf), CHILD_COLOR(135,135,160), MYOS_GDI_TEXT_LEFT);
    }
    child_gdi_text_n(ipc, l.display.x + 4, l.display.y, l.display.w - 8, l.display.h, g_ChildCalc.display[0] ? g_ChildCalc.display : "0", g_ChildCalc.display[0] ? sizeof(g_ChildCalc.display) : 2, CHILD_COLOR(255,255,255), MYOS_GDI_TEXT_RIGHT | MYOS_GDI_TEXT_VCENTER);

    if (l.history_visible) {
        child_gdi_fill(ipc, l.history.x, l.history.y, l.history.w, l.history.h, CHILD_COLOR(18,18,28));
        child_gdi_rect(ipc, l.history.x, l.history.y, l.history.w, l.history.h, CHILD_COLOR(70,70,95));
        child_gdi_text(ipc, l.history.x + 6, l.history.y + 5, l.history.w - 12, 12, "Verlauf [child GDI]", CHILD_COLOR(150,150,180), MYOS_GDI_TEXT_LEFT);
        for (int i = 0; i < g_ChildCalc.hist_count && i < 5; i++) {
            int idx = g_ChildCalc.hist_next - 1 - i;
            while (idx < 0) idx += 5;
            child_gdi_text_n(ipc, l.history.x + 6, l.history.y + 18 + i * 10, l.history.w - 12, 12, g_ChildCalc.history[idx % 5], sizeof(g_ChildCalc.history[0]), CHILD_COLOR(210,210,225), MYOS_GDI_TEXT_LEFT);
        }
    }

    for (int row = 0; row < 5; row++) for (int col = 0; col < 4; col++) {
        ChildGdiRect b = l.buttons[row][col];
        uint32_t bc = (row == 0 && col == 0) ? CHILD_COLOR(80,40,40) : (col == 3 ? CHILD_COLOR(40,65,130) : CHILD_COLOR(50,50,65));
        if (row == 4 && col == 3) bc = CHILD_COLOR(40,95,55);
        child_gdi_fill(ipc, b.x, b.y, b.w, b.h, bc);
        child_gdi_rect(ipc, b.x, b.y, b.w, b.h, CHILD_COLOR(92,92,125));
        child_gdi_text(ipc, b.x, b.y, b.w, b.h, g_child_calc_buttons[row][col].label, CHILD_COLOR(255,255,255), MYOS_GDI_TEXT_CENTER | MYOS_GDI_TEXT_VCENTER);
    }

    char line[96];
    snprintf(line, sizeof(line), "GDI child pid=%ld seq=%u hits=%u last=%s", (long)getpid(), (unsigned)(ipc->shared->gdi_sequence + 1), (unsigned)g_ChildCalc.button_hits, g_ChildCalc.last_button);
    child_gdi_text_n(ipc, 10, ch - 16, cw - 20, 12, line, sizeof(line), CHILD_COLOR(160,220,255), MYOS_GDI_TEXT_LEFT);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static void child_calc_history_push(const char* line)
{
    if (!line || !line[0]) return;
    snprintf(g_ChildCalc.history[g_ChildCalc.hist_next], sizeof(g_ChildCalc.history[g_ChildCalc.hist_next]), "%s", line);
    g_ChildCalc.hist_next = (g_ChildCalc.hist_next + 1) % 5;
    if (g_ChildCalc.hist_count < 5) g_ChildCalc.hist_count++;
}

static void child_calc_set_display_num(double v)
{
    if (v != v || v > 1.0e300 || v < -1.0e300) {
        snprintf(g_ChildCalc.display, sizeof(g_ChildCalc.display), "Error");
        g_ChildCalc.error = 1;
        g_ChildCalc.fresh = 1;
        return;
    }
    snprintf(g_ChildCalc.display, sizeof(g_ChildCalc.display), "%.12g", v);
    g_ChildCalc.error = 0;
}

static void child_calc_apply(char action)
{
    ChildCalcState* c = &g_ChildCalc;
    if (c->error && action != 'C') { snprintf(c->display, sizeof(c->display), "0"); c->error = 0; c->fresh = 1; }
    switch (action) {
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        if (c->fresh || strcmp(c->display, "0") == 0) { snprintf(c->display, sizeof(c->display), "%c", action); c->fresh = 0; }
        else { int l = (int)strlen(c->display); if (l < 24 && l < (int)sizeof(c->display)-1) { c->display[l] = action; c->display[l+1] = 0; } }
        break;
    case '.':
        if (c->fresh) { snprintf(c->display, sizeof(c->display), "0."); c->fresh = 0; }
        else if (!strchr(c->display, '.')) { int l = (int)strlen(c->display); if (l < (int)sizeof(c->display)-1) { c->display[l]='.'; c->display[l+1]=0; } }
        break;
    case 'B':
        if (!c->fresh) { int l = (int)strlen(c->display); if (l > 1) c->display[l-1]=0; else snprintf(c->display, sizeof(c->display), "0"); }
        break;
    case 'C':
        c->operand = 0; c->op = 0; c->fresh = 1; c->error = 0; snprintf(c->display, sizeof(c->display), "0"); break;
    case 'N': child_calc_set_display_num(-atof(c->display)); break;
    case 'P': child_calc_set_display_num(atof(c->display) / 100.0); break;
    case '+': case '-': case '*': case '/': c->operand = atof(c->display); c->op = action; c->fresh = 1; break;
    case '=':
        if (c->op) {
            double rhs = atof(c->display), out = 0.0; int ok = 1;
            if (c->op == '+') out = c->operand + rhs;
            else if (c->op == '-') out = c->operand - rhs;
            else if (c->op == '*') out = c->operand * rhs;
            else if (c->op == '/') { if (rhs == 0.0) ok = 0; else out = c->operand / rhs; }
            char line[80];
            if (ok) { snprintf(line, sizeof(line), "%.12g %c %.12g = %.12g", c->operand, c->op, rhs, out); child_calc_history_push(line); child_calc_set_display_num(out); }
            else { snprintf(line, sizeof(line), "%.12g %c %.12g = Error", c->operand, c->op, rhs); child_calc_history_push(line); snprintf(c->display, sizeof(c->display), "Error"); c->error = 1; }
            c->op = 0; c->fresh = 1;
        }
        break;
    default: break;
    }
}

static void child_calc_publish(ChildIpcContext* ipc, const char* status)
{
    if (!ipc || !ipc->shared) return;
    ipc->shared->calc_enabled = 1;
    ipc->shared->calc_revision = ++g_ChildCalc.revision;
    ipc->shared->calc_button_hits = g_ChildCalc.button_hits;
    snprintf(ipc->shared->calc_display, sizeof(ipc->shared->calc_display), "%s", g_ChildCalc.display[0] ? g_ChildCalc.display : "0");
    if (g_ChildCalc.op) snprintf(ipc->shared->calc_opline, sizeof(ipc->shared->calc_opline), "%.8g %c", g_ChildCalc.operand, g_ChildCalc.op);
    else ipc->shared->calc_opline[0] = 0;
    snprintf(ipc->shared->calc_last_button, sizeof(ipc->shared->calc_last_button), "%s", g_ChildCalc.last_button);
    ipc->shared->calc_history_preview[0] = 0;
    size_t used = 0;
    for (int i = 0; i < g_ChildCalc.hist_count; i++) {
        int idx = g_ChildCalc.hist_next - 1 - i;
        while (idx < 0) idx += 5;
        const char* line = g_ChildCalc.history[idx % 5];
        int wrote = snprintf(ipc->shared->calc_history_preview + used, sizeof(ipc->shared->calc_history_preview) - used, "%s%s", used ? " | " : "", line);
        if (wrote < 0 || (size_t)wrote >= sizeof(ipc->shared->calc_history_preview) - used) break;
        used += (size_t)wrote;
    }
    snprintf(ipc->shared->gui_runtime_status, sizeof(ipc->shared->gui_runtime_status), "%s", status ? status : "calc-state");
    child_calc_render_gdi(ipc, ipc->shared->gui_last_msg ? (UINT)ipc->shared->gui_last_msg : WM_PAINT, status ? status : "calc-state");
}

static LRESULT child_calc_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    (void)lp;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        memset(&g_ChildCalc, 0, sizeof(g_ChildCalc));
        g_ChildCalc.hwnd = hwnd;
        snprintf(g_ChildCalc.display, sizeof(g_ChildCalc.display), "0");
        g_ChildCalc.fresh = 1;
        snprintf(g_ChildCalc.last_button, sizeof(g_ChildCalc.last_button), "-");
        child_calc_publish(ipc, "calc WM_CREATE");
        return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED) { child_calc_publish(ipc, "calc WM_WINDOWPOSCHANGED/GDI repaint"); return 0; }
    if (msg == WM_PAINT) { child_calc_publish(ipc, "calc WM_PAINT -> GDI buffer"); return 0; }
    if (msg == WM_LBUTTONDOWN) {
        if (!ipc || !ipc->shared) return 0;
        int win_w = (int)ipc->shared->gui_w;
        int win_h = (int)ipc->shared->gui_h;
        int cw = win_w - 2;
        int ch = win_h - 24 - 1;
        CalcLayoutLite l;
        child_calc_layout(&l, cw, ch);
        int mx = GET_X_LPARAM(lp);
        int my = GET_Y_LPARAM(lp);
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 4; col++) {
                CalcRect b = l.buttons[row][col];
                if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) {
                    char action = g_child_calc_buttons[row][col].action;
                    child_calc_apply(action);
                    g_ChildCalc.button_hits++;
                    snprintf(g_ChildCalc.last_button, sizeof(g_ChildCalc.last_button), "%s", g_child_calc_buttons[row][col].label);
                    child_calc_publish(ipc, "calc button");
                    PostMessageA(hwnd, WM_USER + 0x62u, (WPARAM)g_ChildCalc.button_hits, 0);
                    return 0;
                }
            }
        }
        child_calc_publish(ipc, "calc click miss");
        return 0;
    }
    if (msg == WM_CLOSE) { if (ipc) ipc->close_seen = 1; child_calc_publish(ipc, "calc WM_CLOSE"); return 0; }
    return 0;
}

static int child_calc_main(int argc, char** argv, ChildIpcContext* ipc)
{
    (void)argc; (void)argv;
    const char* title = "Rechner [OOP]";
    int x = 140, y = 90, w = 320, h = 420;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (w < 240) w = 240;
    if (h < 280) h = 280;

    child_shared_update(ipc, "calc-runtime-start", argc, argv, 0);
    MyGuiIpcRuntimeAttach(ipc);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = child_calc_wndproc;
    wc.lpszClassName = "myOS.Calc";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) { child_calc_wndproc(hwnd, WM_CREATE, 0, 0); child_calc_publish(ipc, "calc window created"); }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v88 calc oop started");
    printf("[v169 child pid=%ld] OOP calc WinMain title='%s' hwnd=%u\n", (long)getpid(), title, (unsigned)hwnd);
    fflush(stdout);
    uint32_t heartbeat = 0;
    while (hwnd && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) {
            if (!DispatchMessageA(&msg)) break;
        }
        child_shared_update(ipc, "calc-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v88 calc heartbeat");
    }
    child_calc_publish(ipc, "calc exiting");
    child_shared_update(ipc, ipc->close_seen ? "calc-close-seen" : "calc-exiting", argc, argv, 63);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 63, ipc->close_seen ? "calc WM_CLOSE exit report" : "calc exit report");
    return 63;
}



/* v64: first real out-of-process Editor.
   The editor's WinMain/WndProc/text buffer/caret/save state now live in this
   exec() child process. The parent still owns the desktop frame and only renders
   generic GDI IPC commands produced by the child during WM_PAINT/state changes. */
typedef struct ChildEditorRect { int x, y, w, h; } ChildEditorRect;
typedef struct ChildEditorState {
    HWND hwnd;
    char path[512];
    char name[96];
    char text[4096];
    int len;
    int cursor;
    int scroll_line;
    int dirty;
    uint32_t revision;
    uint32_t chars_typed;
    uint32_t keydowns;
    ChildEditorRect toolbar;
    ChildEditorRect save_btn;
    ChildEditorRect text_area;
} ChildEditorState;

static ChildEditorState g_ChildEditor;

static const char* child_editor_basename(const char* p)
{
    if (!p || !p[0]) return "Unbenannt.txt";
    const char* s1 = strrchr(p, '/');
    const char* s2 = strrchr(p, '\\');
    const char* s = s1 > s2 ? s1 : s2;
    return s ? s + 1 : p;
}

static int child_editor_line_for_pos(const char* text, int pos)
{
    int line = 0;
    if (!text) return 0;
    for (int i = 0; text[i] && i < pos; i++) if (text[i] == '\n') line++;
    return line;
}

static int child_editor_line_start(const char* text, int wanted)
{
    int line = 0;
    if (!text || wanted <= 0) return 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            line++;
            if (line == wanted) return i + 1;
        }
    }
    return (int)strlen(text ? text : "");
}

static void child_editor_layout(ChildEditorState* e, int cw, int ch)
{
    if (!e) return;
    e->toolbar.x = 0; e->toolbar.y = 0; e->toolbar.w = cw; e->toolbar.h = 30;
    e->save_btn.x = 8; e->save_btn.y = 5; e->save_btn.w = 92; e->save_btn.h = 20;
    e->text_area.x = 8; e->text_area.y = e->toolbar.h + 8;
    e->text_area.w = cw - 16;
    e->text_area.h = ch - e->toolbar.h - 28;
    if (e->text_area.w < 60) e->text_area.w = 60;
    if (e->text_area.h < 40) e->text_area.h = 40;
}

static void child_editor_ensure_cursor_visible(ChildEditorState* e)
{
    if (!e) return;
    int cur_line = child_editor_line_for_pos(e->text, e->cursor);
    int visible = (e->text_area.h - 8) / 12;
    if (visible < 1) visible = 1;
    if (cur_line < e->scroll_line) e->scroll_line = cur_line;
    if (cur_line >= e->scroll_line + visible) e->scroll_line = cur_line - visible + 1;
    if (e->scroll_line < 0) e->scroll_line = 0;
}

static void child_editor_load(ChildEditorState* e)
{
    if (!e) return;
    e->text[0] = 0; e->len = 0; e->cursor = 0; e->scroll_line = 0; e->dirty = 0;
    FILE* f = fopen(e->path, "rb");
    if (!f) return;
    size_t n = fread(e->text, 1, sizeof(e->text) - 1, f);
    fclose(f);
    e->text[n] = 0; e->len = (int)n; e->cursor = e->len;
}

static int child_editor_save(ChildEditorState* e, char* status, size_t status_cb)
{
    if (!e || !e->path[0]) return 0;
    FILE* f = fopen(e->path, "wb");
    if (!f) {
        snprintf(status, status_cb, "Speichern fehlgeschlagen: %.50s", strerror(errno));
        return 0;
    }
    fwrite(e->text, 1, (size_t)e->len, f);
    fclose(f);
    e->dirty = 0;
    snprintf(status, status_cb, "Gespeichert: %.60s (%d bytes)", e->name, e->len);
    return 1;
}

static void child_editor_insert_char(ChildEditorState* e, char ch)
{
    if (!e || e->len >= (int)sizeof(e->text) - 2) return;
    memmove(e->text + e->cursor + 1, e->text + e->cursor, (size_t)(e->len - e->cursor + 1));
    e->text[e->cursor++] = ch;
    e->len++;
    e->dirty = 1;
    e->chars_typed++;
    child_editor_ensure_cursor_visible(e);
}

static void child_editor_backspace(ChildEditorState* e)
{
    if (!e || e->cursor <= 0) return;
    memmove(e->text + e->cursor - 1, e->text + e->cursor, (size_t)(e->len - e->cursor + 1));
    e->cursor--; e->len--; e->dirty = 1;
    child_editor_ensure_cursor_visible(e);
}

static void child_editor_preview(char* out, size_t cb, const char* text)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    if (!text) return;
    size_t j = 0;
    for (size_t i = 0; text[i] && j + 1 < cb; i++) {
        char ch = text[i];
        out[j++] = (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
    }
    out[j] = 0;
}

static void child_editor_publish(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    ChildEditorState* e = &g_ChildEditor;
    int win_w = (int)ipc->shared->gui_w;
    int win_h = (int)ipc->shared->gui_h;
    int cw = win_w - 2;
    int ch = win_h - 24 - 1;
    if (cw < 80) cw = 80;
    if (ch < 80) ch = 80;
    child_editor_layout(e, cw, ch);
    child_editor_ensure_cursor_visible(e);

    ipc->shared->editor_enabled = 1;
    ipc->shared->editor_revision = ++e->revision;
    ipc->shared->editor_chars_typed = e->chars_typed;
    ipc->shared->editor_keydowns = e->keydowns;
    ipc->shared->editor_cursor = (uint32_t)e->cursor;
    ipc->shared->editor_length = (uint32_t)e->len;
    ipc->shared->editor_dirty = (uint32_t)e->dirty;
    ipc->shared->editor_scroll_line = (uint32_t)e->scroll_line;
    child_copy_cstr_bounded(ipc->shared->editor_path, sizeof(ipc->shared->editor_path), e->path);
    child_copy_cstr_bounded(ipc->shared->editor_name, sizeof(ipc->shared->editor_name), e->name);
    child_copy_cstr_bounded(ipc->shared->editor_status, sizeof(ipc->shared->editor_status), status ? status : "editor-state");
    child_editor_preview(ipc->shared->editor_preview, sizeof(ipc->shared->editor_preview), e->text);
    snprintf(ipc->shared->gui_runtime_status, sizeof(ipc->shared->gui_runtime_status), "%s", status ? status : "editor-state");

    child_gdi_reset(ipc, cw, ch, reason, status ? status : "editor WM_PAINT/GDI");
    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(18,18,28));
    child_gdi_fill(ipc, e->toolbar.x, e->toolbar.y, e->toolbar.w, e->toolbar.h, CHILD_COLOR(32,32,48));
    uint32_t saveCol = e->dirty ? CHILD_COLOR(78,70,120) : CHILD_COLOR(45,65,55);
    child_gdi_fill(ipc, e->save_btn.x, e->save_btn.y, e->save_btn.w, e->save_btn.h, saveCol);
    child_gdi_rect(ipc, e->save_btn.x, e->save_btn.y, e->save_btn.w, e->save_btn.h, CHILD_COLOR(110,110,150));
    child_gdi_text(ipc, e->save_btn.x, e->save_btn.y, e->save_btn.w, e->save_btn.h, "Speichern", CHILD_COLOR(255,255,255), MYOS_GDI_TEXT_CENTER | MYOS_GDI_TEXT_VCENTER);
    char title[160];
    size_t title_pos = 0;
    title[0] = '\0';
    if (e->dirty) {
        child_copy_cstr_bounded_n(title, sizeof(title), "* ", 3);
        title_pos = strlen(title);
    }
    child_copy_cstr_bounded_n(title + title_pos, sizeof(title) - title_pos, e->name[0] ? e->name : "Unbenannt.txt", e->name[0] ? sizeof(e->name) : 13);
    child_gdi_text_n(ipc, e->save_btn.x + e->save_btn.w + 12, e->toolbar.y + 9, e->toolbar.w - e->save_btn.w - 22, 14, title, sizeof(title), CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);

    ChildEditorRect a = e->text_area;
    child_gdi_fill(ipc, a.x, a.y, a.w, a.h, CHILD_COLOR(8,8,13));
    child_gdi_rect(ipc, a.x, a.y, a.w, a.h, CHILD_COLOR(75,75,105));
    int visible = (a.h - 8) / 12;
    if (visible < 1) visible = 1;
    int pos = child_editor_line_start(e->text, e->scroll_line);
    int draw_y = a.y + 5;
    int cursor_drawn = 0;
    for (int ln = 0; ln < visible && pos <= e->len && ipc->shared->gdi_command_count + 3 < MYOS_GDI_MAX_COMMANDS; ln++, draw_y += 12) {
        char line[MYOS_GDI_TEXT_MAX];
        int start = pos;
        int l = 0;
        while (e->text[pos] && e->text[pos] != '\n' && l < (int)sizeof(line) - 1) line[l++] = e->text[pos++];
        line[l] = 0;
        child_gdi_text_n(ipc, a.x + 6, draw_y, a.w - 12, 12, line, sizeof(line), CHILD_COLOR(220,255,220), MYOS_GDI_TEXT_LEFT);
        if (e->cursor >= start && e->cursor <= start + l) {
            int cx = a.x + 6 + (e->cursor - start) * 8;
            child_gdi_fill(ipc, cx, draw_y, 2, 10, CHILD_COLOR(120,200,255));
            cursor_drawn = 1;
        }
        if (e->text[pos] == '\n') pos++;
        else if (!e->text[pos]) {
            if (e->cursor == e->len && !cursor_drawn) {
                int cx = a.x + 6 + l * 8;
                child_gdi_fill(ipc, cx, draw_y, 2, 10, CHILD_COLOR(120,200,255));
            }
            break;
        }
    }
    char line[128];
    snprintf(line, sizeof(line), "%s | len=%d cur=%d typed=%u", status ? status : "editor", e->len, e->cursor, (unsigned)e->chars_typed);
    child_gdi_text_n(ipc, 10, ch - 16, cw - 20, 12, line, sizeof(line), e->dirty ? CHILD_COLOR(255,220,140) : CHILD_COLOR(160,220,255), MYOS_GDI_TEXT_LEFT);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static void child_editor_move_vertical(ChildEditorState* e, int dir)
{
    if (!e) return;
    int line = child_editor_line_for_pos(e->text, e->cursor);
    int start = child_editor_line_start(e->text, line);
    int col = e->cursor - start;
    int target = line + dir;
    if (target < 0) target = 0;
    int ns = child_editor_line_start(e->text, target);
    int ne = ns;
    while (e->text[ne] && e->text[ne] != '\n') ne++;
    e->cursor = ns + cclamp_i(col, 0, ne - ns);
    child_editor_ensure_cursor_visible(e);
}

static LRESULT child_editor_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    ChildEditorState* e = &g_ChildEditor;
    if (msg == WM_CREATE) {
        e->hwnd = hwnd;
        child_editor_publish(ipc, msg, e->len ? "editor WM_CREATE loaded" : "editor WM_CREATE new file");
        return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_PAINT) { child_editor_publish(ipc, msg, msg == WM_PAINT ? "editor WM_PAINT -> GDI" : "editor resize/move repaint"); return 0; }
    if (msg == WM_MOUSEWHEEL) { int delta = (int16_t)HIWORD(wp); e->scroll_line += (delta < 0) ? 3 : -3; if (e->scroll_line < 0) e->scroll_line = 0; child_editor_publish(ipc, msg, "editor mousewheel"); return 0; }
    if (msg == WM_LBUTTONDOWN) {
        if (!ipc || !ipc->shared) return 0;
        int mx = GET_X_LPARAM(lp);
        int my = GET_Y_LPARAM(lp);
        if (mx >= e->save_btn.x && mx < e->save_btn.x + e->save_btn.w && my >= e->save_btn.y && my < e->save_btn.y + e->save_btn.h) {
            char st[96]; child_editor_save(e, st, sizeof(st)); child_editor_publish(ipc, msg, st); return 0;
        }
        if (mx >= e->text_area.x && mx < e->text_area.x + e->text_area.w && my >= e->text_area.y && my < e->text_area.y + e->text_area.h) {
            int col = cclamp_i((mx - e->text_area.x - 6) / 8, 0, 240);
            int row = cclamp_i((my - e->text_area.y - 5) / 12, 0, 240);
            int line = e->scroll_line + row;
            int p = child_editor_line_start(e->text, line);
            int n = 0;
            while (e->text[p + n] && e->text[p + n] != '\n' && n < col) n++;
            e->cursor = p + n;
            child_editor_ensure_cursor_visible(e);
            child_editor_publish(ipc, msg, "editor caret click");
        }
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        int key = (int)wp;
        e->keydowns++;
        if (key == KEY_F10) { char st[96]; child_editor_save(e, st, sizeof(st)); child_editor_publish(ipc, msg, st); return 0; }
        if (key == KEY_BACKSPACE) { child_editor_backspace(e); child_editor_publish(ipc, msg, "editor backspace"); return 0; }
        if (key == KEY_ENTER) { child_editor_insert_char(e, '\n'); child_editor_publish(ipc, msg, "editor enter"); return 0; }
        if (key == KEY_LEFT) { if (e->cursor > 0) e->cursor--; child_editor_ensure_cursor_visible(e); child_editor_publish(ipc, msg, "editor left"); return 0; }
        if (key == KEY_RIGHT) { if (e->cursor < e->len) e->cursor++; child_editor_ensure_cursor_visible(e); child_editor_publish(ipc, msg, "editor right"); return 0; }
        if (key == KEY_HOME) { e->cursor = child_editor_line_start(e->text, child_editor_line_for_pos(e->text, e->cursor)); child_editor_ensure_cursor_visible(e); child_editor_publish(ipc, msg, "editor home"); return 0; }
        if (key == KEY_END) { while (e->cursor < e->len && e->text[e->cursor] != '\n') e->cursor++; child_editor_ensure_cursor_visible(e); child_editor_publish(ipc, msg, "editor end"); return 0; }
        if (key == KEY_UP) { child_editor_move_vertical(e, -1); child_editor_publish(ipc, msg, "editor up"); return 0; }
        if (key == KEY_DOWN) { child_editor_move_vertical(e, 1); child_editor_publish(ipc, msg, "editor down"); return 0; }
        child_editor_publish(ipc, msg, "editor keydown");
        return 0;
    }
    if (msg == WM_CHAR) {
        unsigned ch = (unsigned)(wp & 0xffu);
        if (ch == 8) child_editor_backspace(e);
        else if (ch == '\r') child_editor_insert_char(e, '\n');
        else if (ch == '\n' || (ch >= 32 && ch < 127)) child_editor_insert_char(e, (char)ch);
        child_editor_publish(ipc, msg, "editor WM_CHAR");
        return 0;
    }
    if (msg == WM_CLOSE) { if (ipc) ipc->close_seen = 1; child_editor_publish(ipc, msg, "editor WM_CLOSE"); return 0; }
    return 0;
}

static int child_editor_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "Texteditor [OOP]";
    int x = 160, y = 100, w = 540, h = 380;
    const char* path = "/tmp/myos_editor_oop.txt";
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (argc > 5 && argv[5] && argv[5][0]) path = argv[5];
    if (w < 300) w = 300;
    if (h < 220) h = 220;

    memset(&g_ChildEditor, 0, sizeof(g_ChildEditor));
    snprintf(g_ChildEditor.path, sizeof(g_ChildEditor.path), "%s", path);
    snprintf(g_ChildEditor.name, sizeof(g_ChildEditor.name), "%.95s", child_editor_basename(path));
    child_editor_load(&g_ChildEditor);

    child_shared_update(ipc, "editor-runtime-start", argc, argv, 0);
    MyGuiIpcRuntimeAttach(ipc);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = child_editor_wndproc;
    wc.lpszClassName = "myOS.Editor";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) { child_editor_wndproc(hwnd, WM_CREATE, 0, 0); child_editor_publish(ipc, WM_PAINT, "editor window created"); }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v70 editor oop started");
    printf("[v169 child pid=%ld] OOP editor WinMain title='%s' path='%s' hwnd=%u\n", (long)getpid(), title, path, (unsigned)hwnd);
    fflush(stdout);
    uint32_t heartbeat = 0;
    while (hwnd && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) {
            if (!DispatchMessageA(&msg)) break;
        }
        child_shared_update(ipc, "editor-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v70 editor heartbeat");
    }
    child_editor_publish(ipc, WM_CLOSE, "editor exiting");
    child_shared_update(ipc, ipc->close_seen ? "editor-close-seen" : "editor-exiting", argc, argv, 64);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 64, ipc->close_seen ? "editor WM_CLOSE exit report" : "editor exit report");
    return 64;
}


/* v66: out-of-process PaintLab.
   This stresses mouse capture, dense WM_MOUSEMOVE traffic, InvalidateRect and
   GDI line commands across the process boundary. The stroke state lives in the
   Linux child; the parent only interprets the generic GDI buffer. */
#define CHILD_PAINT_MAX_SEGMENTS 180

typedef struct ChildPaintSegment { int x0, y0, x1, y1; uint32_t color; } ChildPaintSegment;
typedef struct ChildPaintRect { int x, y, w, h; } ChildPaintRect;
typedef struct ChildPaintLayout { ChildPaintRect toolbar, clear_btn, canvas; } ChildPaintLayout;
typedef struct ChildPaintState {
    HWND hwnd;
    int drawing;
    int last_x;
    int last_y;
    uint32_t revision;
    uint32_t move_count;
    uint32_t clear_count;
    uint32_t segments;
    ChildPaintSegment seg[CHILD_PAINT_MAX_SEGMENTS];
    char status[96];
} ChildPaintState;

static ChildPaintState g_ChildPaint;

static void child_paint_layout(ChildPaintLayout* l, int cw, int ch)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->toolbar.x = 0; l->toolbar.y = 0; l->toolbar.w = cw; l->toolbar.h = 40;
    l->clear_btn.x = 10; l->clear_btn.y = 8; l->clear_btn.w = 72; l->clear_btn.h = 24;
    l->canvas.x = 10; l->canvas.y = 48; l->canvas.w = cw - 20; l->canvas.h = ch - 76;
    if (l->canvas.w < 40) l->canvas.w = 40;
    if (l->canvas.h < 40) l->canvas.h = 40;
}

static int child_paint_in_rect(int x, int y, ChildPaintRect r)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int child_paint_clamp_to_canvas_x(int x, ChildPaintRect r)
{
    if (x < r.x) return r.x;
    if (x >= r.x + r.w) return r.x + r.w - 1;
    return x;
}

static int child_paint_clamp_to_canvas_y(int y, ChildPaintRect r)
{
    if (y < r.y) return r.y;
    if (y >= r.y + r.h) return r.y + r.h - 1;
    return y;
}

static void child_paint_screen_to_client(ChildIpcContext* ipc, WPARAM wp, LPARAM lp, int* mx, int* my)
{
    (void)ipc;
    (void)wp;
    if (mx) *mx = GET_X_LPARAM(lp);
    if (my) *my = GET_Y_LPARAM(lp);
}

static void child_paint_add_segment(ChildPaintState* p, int x0, int y0, int x1, int y1, uint32_t color)
{
    if (!p) return;
    if (p->segments >= CHILD_PAINT_MAX_SEGMENTS) {
        memmove(&p->seg[0], &p->seg[1], sizeof(p->seg[0]) * (CHILD_PAINT_MAX_SEGMENTS - 1));
        p->segments = CHILD_PAINT_MAX_SEGMENTS - 1;
    }
    ChildPaintSegment* s = &p->seg[p->segments++];
    s->x0 = x0; s->y0 = y0; s->x1 = x1; s->y1 = y1; s->color = color;
}

static void child_paint_render_gdi(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    ChildPaintState* p = &g_ChildPaint;
    int cw = (int)ipc->shared->gui_w - 2;
    int ch = (int)ipc->shared->gui_h - 24 - 1;
    if (cw < 180) cw = 180;
    if (ch < 120) ch = 120;
    ChildPaintLayout l;
    child_paint_layout(&l, cw, ch);
    child_gdi_reset(ipc, cw, ch, reason, status ? status : "paint WM_PAINT/GDI");

    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(12, 14, 24));
    child_gdi_fill(ipc, l.toolbar.x, l.toolbar.y, l.toolbar.w, l.toolbar.h, CHILD_COLOR(20, 24, 42));
    child_gdi_rect(ipc, l.toolbar.x, l.toolbar.y, l.toolbar.w, l.toolbar.h, CHILD_COLOR(55, 70, 110));
    child_gdi_fill(ipc, l.clear_btn.x, l.clear_btn.y, l.clear_btn.w, l.clear_btn.h, CHILD_COLOR(70, 45, 48));
    child_gdi_rect(ipc, l.clear_btn.x, l.clear_btn.y, l.clear_btn.w, l.clear_btn.h, CHILD_COLOR(130, 90, 95));
    child_gdi_text(ipc, l.clear_btn.x, l.clear_btn.y, l.clear_btn.w, l.clear_btn.h, "Clear", CHILD_COLOR(255,255,255), MYOS_GDI_TEXT_CENTER | MYOS_GDI_TEXT_VCENTER);
    child_gdi_text(ipc, 94, 14, cw - 104, 12, "PaintLab [OOP] - drag in canvas, child owns strokes", CHILD_COLOR(185,225,255), MYOS_GDI_TEXT_LEFT);

    child_gdi_fill(ipc, l.canvas.x, l.canvas.y, l.canvas.w, l.canvas.h, CHILD_COLOR(5, 7, 11));
    child_gdi_rect(ipc, l.canvas.x, l.canvas.y, l.canvas.w, l.canvas.h, CHILD_COLOR(80, 90, 130));
    uint32_t max_lines = MYOS_GDI_MAX_COMMANDS > 24 ? MYOS_GDI_MAX_COMMANDS - 24 : 0;
    uint32_t start = p->segments > max_lines ? p->segments - max_lines : 0;
    for (uint32_t i = start; i < p->segments; i++) {
        child_gdi_line(ipc, p->seg[i].x0, p->seg[i].y0, p->seg[i].x1, p->seg[i].y1, p->seg[i].color);
    }

    char line[128];
    snprintf(line, sizeof(line), "seg=%u move=%u cap=%u/%u down=%u last=(%d,%d)",
             (unsigned)p->segments, (unsigned)p->move_count,
             (unsigned)ipc->shared->paint_capture_count, (unsigned)ipc->shared->paint_release_count,
             (unsigned)p->drawing, p->last_x, p->last_y);
    child_gdi_text_n(ipc, 10, ch - 18, cw - 20, 12, line, sizeof(line), CHILD_COLOR(160,230,180), MYOS_GDI_TEXT_LEFT);

    ipc->shared->paint_enabled = 1;
    ipc->shared->paint_revision = ++p->revision;
    ipc->shared->paint_segments = p->segments;
    ipc->shared->paint_mouse_down = p->drawing ? 1u : 0u;
    ipc->shared->paint_move_count = p->move_count;
    ipc->shared->paint_clear_count = p->clear_count;
    ipc->shared->paint_last_x = p->last_x;
    ipc->shared->paint_last_y = p->last_y;
    snprintf(ipc->shared->paint_status, sizeof(ipc->shared->paint_status), "%s", status ? status : p->status);
    snprintf(ipc->shared->gui_runtime_status, sizeof(ipc->shared->gui_runtime_status), "%s", ipc->shared->paint_status);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static void child_paint_publish(ChildIpcContext* ipc, UINT msg, const char* status)
{
    if (!ipc || !ipc->shared) return;
    snprintf(g_ChildPaint.status, sizeof(g_ChildPaint.status), "%s", status ? status : "paint-state");
    child_paint_render_gdi(ipc, msg, g_ChildPaint.status);
}

static LRESULT child_paint_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    ChildPaintState* p = &g_ChildPaint;
    if (msg == WM_CREATE) {
        memset(p, 0, sizeof(*p));
        p->hwnd = hwnd;
        p->last_x = -1; p->last_y = -1;
        child_paint_publish(ipc, msg, "paint WM_CREATE");
        return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_PAINT) {
        child_paint_publish(ipc, msg, msg == WM_PAINT ? "paint WM_PAINT -> GDI" : "paint resize/move repaint");
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        if (!ipc || !ipc->shared) return 0;
        int mx = 0, my = 0;
        child_paint_screen_to_client(ipc, wp, lp, &mx, &my);
        ChildPaintLayout l;
        child_paint_layout(&l, (int)ipc->shared->gui_w - 2, (int)ipc->shared->gui_h - 25);
        if (child_paint_in_rect(mx, my, l.clear_btn)) {
            p->segments = 0;
            p->clear_count++;
            p->drawing = 0;
            p->last_x = mx; p->last_y = my;
            child_paint_publish(ipc, msg, "paint clear button");
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        if (child_paint_in_rect(mx, my, l.canvas)) {
            p->drawing = 1;
            p->last_x = child_paint_clamp_to_canvas_x(mx, l.canvas);
            p->last_y = child_paint_clamp_to_canvas_y(my, l.canvas);
            SetCapture(hwnd);
            child_paint_add_segment(p, p->last_x, p->last_y, p->last_x + 1, p->last_y, CHILD_COLOR(255, 230, 120));
            child_paint_publish(ipc, msg, "paint begin stroke SetCapture");
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        if (!ipc || !ipc->shared || !p->drawing) return 0;
        int mx = 0, my = 0;
        child_paint_screen_to_client(ipc, wp, lp, &mx, &my);
        ChildPaintLayout l;
        child_paint_layout(&l, (int)ipc->shared->gui_w - 2, (int)ipc->shared->gui_h - 25);
        mx = child_paint_clamp_to_canvas_x(mx, l.canvas);
        my = child_paint_clamp_to_canvas_y(my, l.canvas);
        if (mx != p->last_x || my != p->last_y) {
            child_paint_add_segment(p, p->last_x, p->last_y, mx, my, CHILD_COLOR(120, 220, 255));
            p->last_x = mx; p->last_y = my; p->move_count++;
            child_paint_publish(ipc, msg, "paint drag WM_MOUSEMOVE");
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        if (!ipc || !ipc->shared) return 0;
        int mx = 0, my = 0;
        child_paint_screen_to_client(ipc, wp, lp, &mx, &my);
        ChildPaintLayout l;
        child_paint_layout(&l, (int)ipc->shared->gui_w - 2, (int)ipc->shared->gui_h - 25);
        mx = child_paint_clamp_to_canvas_x(mx, l.canvas);
        my = child_paint_clamp_to_canvas_y(my, l.canvas);
        if (p->drawing) {
            child_paint_add_segment(p, p->last_x, p->last_y, mx, my, CHILD_COLOR(120, 220, 255));
            p->last_x = mx; p->last_y = my; p->drawing = 0;
            ReleaseCapture();
            child_paint_publish(ipc, msg, "paint end stroke ReleaseCapture");
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    if (msg == WM_CLOSE) { if (ipc) ipc->close_seen = 1; child_paint_publish(ipc, msg, "paint WM_CLOSE"); return 0; }
    return 0;
}

static int child_paint_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "PaintLab [OOP]";
    int x = 180, y = 120, w = 560, h = 380;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (w < 280) w = 280;
    if (h < 220) h = 220;

    child_shared_update(ipc, "paint-runtime-start", argc, argv, 0);
    MyGuiIpcRuntimeAttach(ipc);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = child_paint_wndproc;
    wc.lpszClassName = "myOS.PaintLab";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) { child_paint_wndproc(hwnd, WM_CREATE, 0, 0); child_paint_publish(ipc, WM_PAINT, "paint window created"); }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v70 paint oop started");
    printf("[v169 child pid=%ld] OOP paint WinMain title='%s' hwnd=%u\n", (long)getpid(), title, (unsigned)hwnd);
    fflush(stdout);
    uint32_t heartbeat = 0;
    while (hwnd && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) {
            if (!DispatchMessageA(&msg)) break;
        }
        child_shared_update(ipc, "paint-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v70 paint heartbeat");
    }
    child_paint_publish(ipc, WM_CLOSE, "paint exiting");
    child_shared_update(ipc, ipc->close_seen ? "paint-close-seen" : "paint-exiting", argc, argv, 65);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 65, ipc->close_seen ? "paint WM_CLOSE exit report" : "paint exit report");
    return 65;
}



/* v165: out-of-process DragLab.  The old DragLab was one of the remaining
   parent-process WinMain islands.  This child keeps the drag/capture state in
   the Linux child process and publishes only scalar GDI commands/status to the
   parent compositor. */
#define CHILD_DRAG_LOG_LINES 10
#define CHILD_DRAG_LOG_CHARS 96

typedef struct ChildDragState {
    HWND hwnd;
    uint32_t revision;
    int boxX, boxY, boxW, boxH;
    int targetX, targetY, targetW, targetH;
    int dragging;
    int capActive;
    int dragOffX, dragOffY;
    int mouseClientX, mouseClientY;
    uint32_t captureCount;
    uint32_t releaseCount;
    uint32_t moveCount;
    uint32_t dropCount;
    uint32_t cancelCount;
    char status[128];
    char log[CHILD_DRAG_LOG_LINES][CHILD_DRAG_LOG_CHARS];
    int logCount;
} ChildDragState;

static ChildDragState g_ChildDrag;

static int child_drag_inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void child_drag_reset_box(ChildDragState* d)
{
    if (!d) return;
    d->boxX = 48; d->boxY = 120; d->boxW = 90; d->boxH = 54;
    d->targetX = 410; d->targetY = 104; d->targetW = 170; d->targetH = 110;
}

static void child_drag_log(ChildDragState* d, const char* s)
{
    if (!d || !s) return;
    if (d->logCount < CHILD_DRAG_LOG_LINES) {
        child_copy_cstr_bounded(d->log[d->logCount++], sizeof(d->log[0]), s);
        return;
    }
    memmove(&d->log[0], &d->log[1], sizeof(d->log[0]) * (CHILD_DRAG_LOG_LINES - 1));
    child_copy_cstr_bounded(d->log[CHILD_DRAG_LOG_LINES - 1], sizeof(d->log[0]), s);
}

static void child_drag_publish(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    ChildDragState* d = &g_ChildDrag;
    if (status && status != d->status) snprintf(d->status, sizeof(d->status), "%s", status);
    int cw = (int)ipc->shared->gui_w - 2;
    int ch = (int)ipc->shared->gui_h - 25;
    if (cw < 520) cw = 520;
    if (ch < 300) ch = 300;

    char gdiStatus[96];
    child_copy_cstr_bounded(gdiStatus, sizeof(gdiStatus), d->status[0] ? d->status : "draglab oop");
    child_gdi_reset(ipc, cw, ch, reason, gdiStatus);
    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(8, 8, 18));

    struct Btn { int x,y,w,h; const char* t; } btns[] = {
        { 8, 8, 112, 20, "Capture" }, { 128, 8, 112, 20, "Release" },
        { 248, 8, 112, 20, "Reset" }, { 368, 8, 112, 20, "Cancel" },
    };
    for (unsigned i = 0; i < sizeof(btns)/sizeof(btns[0]); ++i) {
        child_gdi_fill(ipc, btns[i].x, btns[i].y, btns[i].w, btns[i].h, CHILD_COLOR(45, 50, 70));
        child_gdi_rect(ipc, btns[i].x, btns[i].y, btns[i].w, btns[i].h, CHILD_COLOR(120, 140, 175));
        child_gdi_text(ipc, btns[i].x, btns[i].y, btns[i].w, btns[i].h, btns[i].t, CHILD_COLOR(245,245,255), MYOS_GDI_TEXT_CENTER | MYOS_GDI_TEXT_VCENTER);
    }

    child_gdi_fill(ipc, d->targetX, d->targetY, d->targetW, d->targetH, CHILD_COLOR(28, 44, 34));
    child_gdi_rect(ipc, d->targetX, d->targetY, d->targetW, d->targetH, CHILD_COLOR(90, 190, 120));
    child_gdi_text(ipc, d->targetX + 14, d->targetY + 14, d->targetW - 28, 14, "DROP TARGET", CHILD_COLOR(160,255,180), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, d->targetX + 14, d->targetY + 34, d->targetW - 28, 14, "release box here", CHILD_COLOR(190,220,190), MYOS_GDI_TEXT_LEFT);

    child_gdi_fill(ipc, d->boxX, d->boxY, d->boxW, d->boxH, d->dragging ? CHILD_COLOR(80,120,210) : CHILD_COLOR(60,80,150));
    child_gdi_rect(ipc, d->boxX, d->boxY, d->boxW, d->boxH, CHILD_COLOR(255,255,255));
    child_gdi_text(ipc, d->boxX, d->boxY, d->boxW, d->boxH, "DRAG ME", CHILD_COLOR(255,255,255), MYOS_GDI_TEXT_CENTER | MYOS_GDI_TEXT_VCENTER);

    char line[160];
    snprintf(line, sizeof(line), "OOP pid=%u hwnd=0x%x capture=%s dragging=%s mouse=(%d,%d)",
             (unsigned)ipc->shared->child_pid, (unsigned)d->hwnd, d->capActive ? "yes" : "no",
             d->dragging ? "yes" : "no", d->mouseClientX, d->mouseClientY);
    child_gdi_text_n(ipc, 8, 238, cw - 16, 14, line, sizeof(line), CHILD_COLOR(120,255,170), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "captures=%u releases=%u moves=%u drops=%u cancels=%u box=(%d,%d)",
             (unsigned)d->captureCount, (unsigned)d->releaseCount, (unsigned)d->moveCount,
             (unsigned)d->dropCount, (unsigned)d->cancelCount, d->boxX, d->boxY);
    child_gdi_text_n(ipc, 8, 258, cw - 16, 14, line, sizeof(line), CHILD_COLOR(160,210,255), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "Status: %.100s", d->status[0] ? d->status : "-");
    child_gdi_text_n(ipc, 8, 278, cw - 16, 14, line, sizeof(line), CHILD_COLOR(255,230,160), MYOS_GDI_TEXT_LEFT);

    int log_y = 304;
    child_gdi_rect(ipc, 8, log_y, cw - 16, ch - log_y - 8, CHILD_COLOR(70,80,115));
    child_gdi_text(ipc, 14, log_y + 8, cw - 28, 14, "Capture / DragDrop log [OOP child-owned]", CHILD_COLOR(255,255,255), MYOS_GDI_TEXT_LEFT);
    for (int i = 0; i < d->logCount && log_y + 28 + i * 15 < ch - 10; ++i)
        child_gdi_text_n(ipc, 14, log_y + 28 + i * 15, cw - 28, 12, d->log[i], sizeof(d->log[0]), CHILD_COLOR(240,240,255), MYOS_GDI_TEXT_LEFT);

    ipc->shared->paint_enabled = 1;
    ipc->shared->paint_revision = ++d->revision;
    ipc->shared->paint_mouse_down = d->dragging ? 1u : 0u;
    ipc->shared->paint_capture_count = d->captureCount;
    ipc->shared->paint_release_count = d->releaseCount;
    ipc->shared->paint_move_count = d->moveCount;
    ipc->shared->paint_last_x = d->mouseClientX;
    ipc->shared->paint_last_y = d->mouseClientY;
    child_copy_cstr_bounded(ipc->shared->paint_status, sizeof(ipc->shared->paint_status), d->status[0] ? d->status : "draglab oop");
    snprintf(ipc->shared->gui_runtime_status, sizeof(ipc->shared->gui_runtime_status), "%s", ipc->shared->paint_status);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static LRESULT child_draglab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    ChildDragState* d = &g_ChildDrag;
    if (msg == WM_CREATE) {
        memset(d, 0, sizeof(*d));
        d->hwnd = hwnd;
        child_drag_reset_box(d);
        snprintf(d->status, sizeof(d->status), "DragLab OOP ready: state lives in child process");
        child_drag_log(d, d->status);
        child_drag_publish(ipc, msg, d->status);
        return 0;
    }
    if (msg == WM_PAINT || msg == WM_WINDOWPOSCHANGED || msg == WM_SIZE) {
        child_drag_publish(ipc, msg, msg == WM_PAINT ? "DragLab WM_PAINT" : "DragLab geometry repaint");
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        d->mouseClientX = cx; d->mouseClientY = cy;
        if (cy >= 8 && cy < 28) {
            if (cx >= 8 && cx < 120) {
                SetCapture(hwnd); d->capActive = 1; d->captureCount++;
                snprintf(d->status, sizeof(d->status), "SetCapture requested from child");
                child_drag_log(d, d->status); child_drag_publish(ipc, msg, d->status); return 0;
            }
            if (cx >= 128 && cx < 240) {
                ReleaseCapture(); d->capActive = 0; d->dragging = 0; d->releaseCount++;
                snprintf(d->status, sizeof(d->status), "ReleaseCapture requested from child");
                child_drag_log(d, d->status); child_drag_publish(ipc, msg, d->status); return 0;
            }
            if (cx >= 248 && cx < 360) {
                child_drag_reset_box(d); d->dragging = 0;
                snprintf(d->status, sizeof(d->status), "Reset: box returned to source position");
                child_drag_log(d, d->status); child_drag_publish(ipc, msg, d->status); return 0;
            }
            if (cx >= 368 && cx < 480) {
                ReleaseCapture(); d->capActive = 0; d->dragging = 0; d->cancelCount++;
                snprintf(d->status, sizeof(d->status), "Cancel drag -> ReleaseCapture");
                child_drag_log(d, d->status); child_drag_publish(ipc, msg, d->status); return 0;
            }
        }
        if (child_drag_inside(cx, cy, d->boxX, d->boxY, d->boxW, d->boxH)) {
            d->dragging = 1;
            d->dragOffX = cx - d->boxX;
            d->dragOffY = cy - d->boxY;
            d->capActive = 1;
            d->captureCount++;
            SetCapture(hwnd);
            snprintf(d->status, sizeof(d->status), "Begin drag in child: SetCapture hwnd=0x%x", (unsigned)hwnd);
            child_drag_log(d, d->status);
            child_drag_publish(ipc, msg, d->status);
        }
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        d->mouseClientX = cx; d->mouseClientY = cy;
        if (d->dragging) {
            d->boxX = cx - d->dragOffX;
            d->boxY = cy - d->dragOffY;
            if (d->boxX < 8) d->boxX = 8;
            if (d->boxY < 48) d->boxY = 48;
            int cw = ipc && ipc->shared ? ((int)ipc->shared->gui_w - 2) : 640;
            int ch = ipc && ipc->shared ? ((int)ipc->shared->gui_h - 25) : 365;
            int maxBoxX = cw - d->boxW - 24;
            int maxBoxY = ch - d->boxH - 28;
            if (maxBoxX < 8) maxBoxX = 8;
            if (maxBoxY < 48) maxBoxY = 48;
            if (d->boxX > maxBoxX) d->boxX = maxBoxX;
            if (d->boxY > maxBoxY) d->boxY = maxBoxY;
            d->moveCount++;
            snprintf(d->status, sizeof(d->status), "WM_MOUSEMOVE captured in child: client=(%d,%d)", cx, cy);
            child_drag_publish(ipc, msg, d->status);
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        d->mouseClientX = cx; d->mouseClientY = cy;
        int wasDrag = d->dragging;
        d->dragging = 0;
        int dropped = child_drag_inside(d->boxX + d->boxW / 2, d->boxY + d->boxH / 2, d->targetX, d->targetY, d->targetW, d->targetH);
        if (wasDrag && dropped) d->dropCount++;
        ReleaseCapture(); d->capActive = 0; d->releaseCount++;
        snprintf(d->status, sizeof(d->status), "%s: WM_LBUTTONUP client=(%d,%d) drop=%s", wasDrag ? "Drop" : "MouseUp", cx, cy, (wasDrag && dropped) ? "TARGET" : "no");
        child_drag_log(d, d->status);
        child_drag_publish(ipc, msg, d->status);
        return 0;
    }
    if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE) {
        int hadDrag = d->dragging;
        d->capActive = 0;
        d->dragging = 0;
        if (msg == WM_CANCELMODE || hadDrag) {
            snprintf(d->status, sizeof(d->status), msg == WM_CAPTURECHANGED ? "WM_CAPTURECHANGED in child" : "WM_CANCELMODE in child");
            child_drag_log(d, d->status);
            child_drag_publish(ipc, msg, d->status);
        } else {
            /* v166: parent proxy releases capture after forwarding
               WM_LBUTTONUP.  Keep the already-published Drop/MouseUp verdict
               visible; this notification is still logged and the state line
               already shows capture=no from the button-up handler. */
            child_drag_log(d, msg == WM_CAPTURECHANGED ? "WM_CAPTURECHANGED in child" : "WM_CANCELMODE in child");
            child_drag_publish(ipc, msg, d->status);
        }
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (ipc) ipc->close_seen = 1;
        child_drag_publish(ipc, msg, "DragLab WM_CLOSE");
        return 0;
    }
    return 0;
}

static int child_draglab_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "DragLab [OOP]";
    int x = 240, y = 160, w = 640, h = 390;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (w < 520) w = 520;
    if (h < 330) h = 330;

    child_shared_update(ipc, "draglab-runtime-start", argc, argv, 0);
    MyGuiIpcRuntimeAttach(ipc);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = child_draglab_wndproc;
    wc.lpszClassName = "myOS.DragLab.OOP";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) { child_draglab_wndproc(hwnd, WM_CREATE, 0, 0); child_drag_publish(ipc, WM_PAINT, "DragLab OOP window created"); }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v169 draglab oop started");
    printf("[v169 child pid=%ld] OOP draglab WinMain title='%s' hwnd=%u\n", (long)getpid(), title, (unsigned)hwnd);
    fflush(stdout);
    uint32_t heartbeat = 0;
    while (hwnd && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) {
            if (!DispatchMessageA(&msg)) break;
        }
        child_shared_update(ipc, "draglab-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v169 draglab heartbeat");
    }
    child_drag_publish(ipc, WM_CLOSE, "DragLab exiting");
    child_shared_update(ipc, ipc->close_seen ? "draglab-close-seen" : "draglab-exiting", argc, argv, 66);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 66, ipc->close_seen ? "draglab WM_CLOSE exit report" : "draglab exit report");
    return 66;
}


/* v66: out-of-process ControlLab proving cross-process child-HWNDs.
   The top-level WndProc lives in the Linux child.  It creates real child HWNDs
   by calling CreateWindowExA(WS_CHILD|WS_VISIBLE, "BUTTON", ...).  The parent
   creates those HWNDs under the proxy top-level window; mouse hits can target
   the child HWND, and the child converts button up into WM_COMMAND. */
typedef struct ChildControlLabState {
    HWND hwnd;
    HWND button;
    HWND toggle;
    HWND staticText;
    uint32_t revision;
    uint32_t clicks;
    uint32_t commands;
    int toggled;
    char status[96];
} ChildControlLabState;

static ChildControlLabState g_ChildControlLab;

static void child_controllab_render(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    int cw = (int)ipc->shared->gui_w - 2;
    int ch = (int)ipc->shared->gui_h - 25;
    if (cw < 180) cw = 180;
    if (ch < 110) ch = 110;
    child_gdi_reset(ipc, cw, ch, reason, status ? status : "controllab paint");
    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(18,22,35));
    child_gdi_rect(ipc, 8, 8, cw - 16, ch - 16, CHILD_COLOR(70,100,150));
    child_gdi_text(ipc, 18, 20, cw - 36, 16, "v70 ControlLab [OOP]", CHILD_COLOR(185,230,255), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 18, 42, cw - 36, 16, "Child creates BUTTON/STATIC HWNDs via IPC", CHILD_COLOR(210,210,235), MYOS_GDI_TEXT_LEFT);
    char line[120];
    snprintf(line, sizeof(line), "top=%u button=%u toggle=%u static=%u", (unsigned)g_ChildControlLab.hwnd, (unsigned)g_ChildControlLab.button, (unsigned)g_ChildControlLab.toggle, (unsigned)g_ChildControlLab.staticText);
    child_gdi_text_n(ipc, 18, 122, cw - 36, 14, line, sizeof(line), CHILD_COLOR(190,255,210), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "WM_COMMAND=%u clicks=%u toggle=%s", (unsigned)g_ChildControlLab.commands, (unsigned)g_ChildControlLab.clicks, g_ChildControlLab.toggled ? "ON" : "OFF");
    child_gdi_text_n(ipc, 18, 140, cw - 36, 14, line, sizeof(line), CHILD_COLOR(255,230,170), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 18, ch - 24, cw - 36, 14, g_ChildControlLab.status, sizeof(g_ChildControlLab.status), CHILD_COLOR(170,220,255), MYOS_GDI_TEXT_LEFT);

    ipc->shared->child_hwnd_command_count = g_ChildControlLab.commands;
    ipc->shared->child_hwnd_click_count = g_ChildControlLab.clicks;
    ipc->shared->child_hwnd_last_msg = reason;
    snprintf(ipc->shared->child_hwnd_status, sizeof(ipc->shared->child_hwnd_status), "%s", status ? status : g_ChildControlLab.status);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static void child_controllab_publish(ChildIpcContext* ipc, UINT reason, const char* status)
{
    g_ChildControlLab.revision++;
    if (!status) {
        child_copy_cstr_bounded(g_ChildControlLab.status, sizeof(g_ChildControlLab.status), "controllab");
    } else if (status != g_ChildControlLab.status) {
        child_copy_cstr_bounded(g_ChildControlLab.status, sizeof(g_ChildControlLab.status), status);
    }
    child_controllab_render(ipc, reason, g_ChildControlLab.status);
}

static LRESULT child_controllab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        if (g_ChildControlLab.hwnd)
            return 0; /* queued duplicate top/control WM_CREATE; keep child state intact */
        memset(&g_ChildControlLab, 0, sizeof(g_ChildControlLab));
        g_ChildControlLab.hwnd = hwnd;
        snprintf(g_ChildControlLab.status, sizeof(g_ChildControlLab.status), "WM_CREATE top-level ready");
        child_controllab_publish(ipc, msg, "ControlLab top-level created");
        return 0;
    }
    if (msg == WM_PAINT || msg == WM_WINDOWPOSCHANGED) {
        child_controllab_publish(ipc, msg, msg == WM_PAINT ? "ControlLab WM_PAINT" : "ControlLab move/resize repaint");
        return 0;
    }
    if (msg == WM_LBUTTONUP && (hwnd == g_ChildControlLab.button || hwnd == g_ChildControlLab.toggle)) {
        uint16_t id = (hwnd == g_ChildControlLab.button) ? 101u : 102u;
        g_ChildControlLab.clicks++;
        if (id == 102u) g_ChildControlLab.toggled = !g_ChildControlLab.toggled;
        snprintf(g_ChildControlLab.status, sizeof(g_ChildControlLab.status), "child HWND %u click -> PostMessage WM_COMMAND", (unsigned)id);
        PostMessageA(g_ChildControlLab.hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hwnd);
        child_controllab_publish(ipc, msg, g_ChildControlLab.status);
        return 0;
    }
    if (msg == WM_COMMAND) {
        uint16_t id = LOWORD(wp);
        uint16_t code = HIWORD(wp);
        g_ChildControlLab.commands++;
        if (code == BN_CLICKED && (id == 101u || id == 102u)) {
            g_ChildControlLab.clicks++;
            if (id == 102u) g_ChildControlLab.toggled = !g_ChildControlLab.toggled;
        }
        snprintf(g_ChildControlLab.status, sizeof(g_ChildControlLab.status), "parent got WM_COMMAND id=%u code=%u lParam=0x%llx", (unsigned)id, (unsigned)code, (unsigned long long)lp);
        child_controllab_publish(ipc, msg, g_ChildControlLab.status);
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (ipc) ipc->close_seen = 1;
        child_controllab_publish(ipc, msg, "ControlLab WM_CLOSE");
        return 0;
    }
    return 0;
}

static int child_controllab_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "ControlLab [OOP child-HWND]";
    int x = 220, y = 140, w = 520, h = 260;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (w < 360) w = 360;
    if (h < 220) h = 220;

    child_shared_update(ipc, "controllab-runtime-start", argc, argv, 0);
    MyGuiIpcRuntimeAttach(ipc);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = child_controllab_wndproc;
    wc.lpszClassName = "myOS.ControlLab.OOP";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) {
        child_controllab_wndproc(hwnd, WM_CREATE, 0, 0);
        g_ChildControlLab.button = CreateWindowExA(0, "BUTTON", "Ping", WS_CHILD|WS_VISIBLE, 20, 72, 96, 28, hwnd, (void*)(uintptr_t)101u, NULL, NULL);
        g_ChildControlLab.toggle = CreateWindowExA(0, "BUTTON", "Toggle", WS_CHILD|WS_VISIBLE, 128, 72, 108, 28, hwnd, (void*)(uintptr_t)102u, NULL, NULL);
        g_ChildControlLab.staticText = CreateWindowExA(0, "STATIC", "parent-created child HWNDs", WS_CHILD|WS_VISIBLE, 250, 76, 210, 20, hwnd, (void*)(uintptr_t)201u, NULL, NULL);
        child_controllab_publish(ipc, WM_PAINT, "ControlLab child HWNDs requested/acked");
    }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v70 controllab oop started");
    printf("[v169 child pid=%ld] OOP controllab top=%u button=%u toggle=%u static=%u\n", (long)getpid(), (unsigned)hwnd, (unsigned)g_ChildControlLab.button, (unsigned)g_ChildControlLab.toggle, (unsigned)g_ChildControlLab.staticText);
    fflush(stdout);
    uint32_t heartbeat = 0;
    while (hwnd && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) {
            if (!DispatchMessageA(&msg)) break;
        }
        child_shared_update(ipc, "controllab-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v70 controllab heartbeat");
    }
    child_controllab_publish(ipc, WM_CLOSE, "controllab exiting");
    child_shared_update(ipc, ipc->close_seen ? "controllab-close-seen" : "controllab-exiting", argc, argv, 66);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 66, ipc->close_seen ? "controllab WM_CLOSE exit report" : "controllab exit report");
    return 66;
}


/* v70: out-of-process ClipMenuLab.
   This ports the old clipboard/menu/accelerator lab onto the GUI IPC runtime:
   WinMain/WndProc/state live in the Linux child, while Clipboard ownership is
   parent/session-side and Menu/Accelerator operations are exposed through the
   child runtime stubs plus IPC diagnostics.  v190 replaces the old
   TrackPopupMenu-first-item shortcut with a small modal queue pump. */
#define CLIP_CMD_SET     3101u
#define CLIP_CMD_GET     3102u
#define CLIP_CMD_CLEAR   3103u
#define CLIP_CMD_NEW     3104u
#define CLIP_CMD_COPY    3105u
#define CLIP_CMD_PASTE   3106u
#define CLIP_CMD_POPUP   3107u
#define CLIP_CMD_ATTACH  3108u

typedef struct ChildClipState {
    HWND hwnd;
    HMENU hMenu;
    HMENU hPopup;
    HACCEL hAccel;
    uint32_t revision;
    uint32_t set_count;
    uint32_t get_count;
    uint32_t clear_count;
    uint32_t menu_count;
    uint32_t popup_count;
    uint32_t accel_count;
    uint32_t command_count;
    uint32_t serial;
    char local_text[192];
    char clipboard_text[192];
    char status[128];
    char log[8][96];
    int log_count;
} ChildClipState;

static ChildClipState g_ChildClip;

static void child_clip_log(const char* s)
{
    if (!s) s = "";
    if (g_ChildClip.log_count < 8) {
        child_copy_cstr_bounded(g_ChildClip.log[g_ChildClip.log_count++], sizeof(g_ChildClip.log[0]), s);
    } else {
        for (int i = 1; i < 8; i++) child_copy_cstr_bounded(g_ChildClip.log[i-1], sizeof(g_ChildClip.log[0]), g_ChildClip.log[i]);
        child_copy_cstr_bounded(g_ChildClip.log[7], sizeof(g_ChildClip.log[0]), s);
    }
}

static void child_clip_publish(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    int win_w = (int)ipc->shared->gui_w;
    int win_h = (int)ipc->shared->gui_h;
    int cw = win_w - 2;
    int ch = win_h - 24 - 1;
    if (cw < 560) cw = 560;
    if (ch < 300) ch = 300;
    if (status) child_copy_cstr_bounded(g_ChildClip.status, sizeof(g_ChildClip.status), status);

    ipc->shared->clip_enabled = 1;
    ipc->shared->menu_enabled = 1;
    ipc->shared->clip_set_count = g_ChildClip.set_count;
    ipc->shared->clip_get_count = g_ChildClip.get_count;
    ipc->shared->clip_clear_count = g_ChildClip.clear_count;
    ipc->shared->menu_create_count = g_ChildClip.menu_count;
    ipc->shared->menu_popup_count = g_ChildClip.popup_count;
    ipc->shared->menu_command_count = g_ChildClip.command_count;
    ipc->shared->accel_translate_count = g_ChildClip.accel_count;
    child_copy_cstr_bounded(ipc->shared->clip_local_text, sizeof(ipc->shared->clip_local_text), g_ChildClip.local_text);
    child_copy_cstr_bounded(ipc->shared->clip_text, sizeof(ipc->shared->clip_text), g_ChildClip.clipboard_text);
    child_copy_cstr_bounded(ipc->shared->clip_status, sizeof(ipc->shared->clip_status), g_ChildClip.status);
    child_copy_cstr_bounded(ipc->shared->menu_status, sizeof(ipc->shared->menu_status), g_ChildClip.status);

    child_gdi_reset(ipc, cw, ch, reason, status ? status : "clipmenu WM_PAINT/GDI");
    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(8,8,18));

    const char* row1[] = { "Set Clip", "Get Clip", "Clear", "Attach Menu", "Popup" };
    const uint32_t colors1[] = { 0,0,0,0,0 };
    (void)colors1;
    for (int i = 0; i < 5; i++) {
        int x = 8 + i * 120;
        child_gdi_fill(ipc, x, 8, 112, 20, CHILD_COLOR(36,48,78));
        child_gdi_rect(ipc, x, 8, 112, 20, CHILD_COLOR(90,110,160));
        child_gdi_text(ipc, x, 8, 112, 20, row1[i], CHILD_COLOR(235,235,255), MYOS_GDI_TEXT_CENTER|MYOS_GDI_TEXT_VCENTER);
    }
    const char* row2[] = { "Ctrl+C", "Ctrl+V", "Ctrl+N" };
    for (int i = 0; i < 3; i++) {
        int x = 8 + i * 120;
        child_gdi_fill(ipc, x, 34, 112, 20, CHILD_COLOR(42,56,64));
        child_gdi_rect(ipc, x, 34, 112, 20, CHILD_COLOR(90,130,130));
        child_gdi_text(ipc, x, 34, 112, 20, row2[i], CHILD_COLOR(230,255,240), MYOS_GDI_TEXT_CENTER|MYOS_GDI_TEXT_VCENTER);
    }

    char line[192];
    snprintf(line, sizeof(line), "MENU=0x%x POPUP=0x%x ACCEL=0x%x set=%u get=%u clear=%u menu=%u popup=%u accel=%u cmd=%u",
             (unsigned)g_ChildClip.hMenu, (unsigned)g_ChildClip.hPopup, (unsigned)g_ChildClip.hAccel,
             (unsigned)g_ChildClip.set_count, (unsigned)g_ChildClip.get_count, (unsigned)g_ChildClip.clear_count,
             (unsigned)g_ChildClip.menu_count, (unsigned)g_ChildClip.popup_count, (unsigned)g_ChildClip.accel_count,
             (unsigned)g_ChildClip.command_count);
    child_gdi_text_n(ipc, 8, 66, cw - 16, 14, line, sizeof(line), CHILD_COLOR(120,255,170), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "Local: %.150s", g_ChildClip.local_text);
    child_gdi_text_n(ipc, 8, 86, cw - 16, 14, line, sizeof(line), CHILD_COLOR(210,210,255), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "Clipboard: %.140s", g_ChildClip.clipboard_text);
    child_gdi_text_n(ipc, 8, 106, cw - 16, 14, line, sizeof(line), CHILD_COLOR(210,210,255), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "Status: %.150s", g_ChildClip.status);
    child_gdi_text_n(ipc, 8, 126, cw - 16, 14, line, sizeof(line), CHILD_COLOR(255,230,160), MYOS_GDI_TEXT_LEFT);

    int log_y = 150;
    child_gdi_rect(ipc, 8, log_y, cw - 16, ch - 158, CHILD_COLOR(70,80,115));
    child_gdi_text(ipc, 14, log_y + 8, cw - 28, 12, "Clipboard / Menu / Accelerator log [OOP child]", CHILD_COLOR(255,255,255), MYOS_GDI_TEXT_LEFT);
    for (int i = 0; i < g_ChildClip.log_count && i < 8; i++) {
        child_gdi_text_n(ipc, 14, log_y + 28 + i * 15, cw - 28, 12, g_ChildClip.log[i], sizeof(g_ChildClip.log[0]), CHILD_COLOR(240,240,255), MYOS_GDI_TEXT_LEFT);
    }
    snprintf(line, sizeof(line), "v70 OOP clip/menu/accel pid=%ld req=%u/%u menu=%u/%u", (long)getpid(),
             (unsigned)ipc->shared->clip_request, (unsigned)ipc->shared->clip_ack,
             (unsigned)ipc->shared->menu_request, (unsigned)ipc->shared->menu_ack);
    child_gdi_text_n(ipc, 10, ch - 16, cw - 20, 12, line, sizeof(line), CHILD_COLOR(160,220,255), MYOS_GDI_TEXT_LEFT);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
    g_ChildClip.revision++;
}

static void child_clip_attach_menu(void)
{
    if (!g_ChildClip.hMenu) {
        g_ChildClip.hMenu = CreateMenu();
        AppendMenuA(g_ChildClip.hMenu, MF_STRING, CLIP_CMD_COPY, "Copy\tCtrl+C");
        AppendMenuA(g_ChildClip.hMenu, MF_STRING, CLIP_CMD_PASTE, "Paste\tCtrl+V");
        AppendMenuA(g_ChildClip.hMenu, MF_STRING, CLIP_CMD_NEW, "New\tCtrl+N");
    }
    if (!g_ChildClip.hPopup) {
        g_ChildClip.hPopup = CreatePopupMenu();
        AppendMenuA(g_ChildClip.hPopup, MF_STRING, CLIP_CMD_PASTE, "Popup Paste");
        AppendMenuA(g_ChildClip.hPopup, MF_STRING, CLIP_CMD_COPY, "Popup Copy");
    }
    BOOL ok = SetMenu(g_ChildClip.hwnd, g_ChildClip.hMenu);
    if (ok) g_ChildClip.menu_count++;
    snprintf(g_ChildClip.status, sizeof(g_ChildClip.status), "CreateMenu/AppendMenuA/SetMenu -> %s menu=0x%x popup=0x%x", ok ? "TRUE" : "FALSE", (unsigned)g_ChildClip.hMenu, (unsigned)g_ChildClip.hPopup);
    child_clip_log(g_ChildClip.status);
}

static void child_clip_set_text(void)
{
    char text[160];
    snprintf(text, sizeof(text), "Hello from OOP ClipMenuLab v70 #%u", (unsigned)++g_ChildClip.serial);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (DWORD)(strlen(text) + 1));
    char* p = (char*)GlobalLock(h);
    if (p) { snprintf(p, strlen(text) + 1, "%s", text); GlobalUnlock(h); }
    BOOL ok = FALSE;
    if (OpenClipboard(g_ChildClip.hwnd)) {
        EmptyClipboard();
        ok = SetClipboardData(CF_TEXT, h) ? TRUE : FALSE;
        CloseClipboard();
    }
    if (!ok && h) GlobalFree(h);
    if (ok) g_ChildClip.set_count++;
    child_copy_cstr_bounded(g_ChildClip.local_text, sizeof(g_ChildClip.local_text), text);
    if (ok) child_copy_cstr_bounded(g_ChildClip.clipboard_text, sizeof(g_ChildClip.clipboard_text), text);
    snprintf(g_ChildClip.status, sizeof(g_ChildClip.status), "SetClipboardData(CF_TEXT) via IPC -> %s", ok ? "TRUE" : "FALSE");
    child_clip_log(g_ChildClip.status);
}

static void child_clip_get_text(void)
{
    char out[192] = "<empty or unavailable>";
    BOOL ok = FALSE;
    if (OpenClipboard(g_ChildClip.hwnd)) {
        HGLOBAL h = GetClipboardData(CF_TEXT);
        if (h) {
            const char* p = (const char*)GlobalLock(h);
            if (p) { child_copy_cstr_bounded(out, sizeof(out), p); GlobalUnlock(h); ok = TRUE; }
        }
        CloseClipboard();
    }
    if (ok) g_ChildClip.get_count++;
    child_copy_cstr_bounded(g_ChildClip.clipboard_text, sizeof(g_ChildClip.clipboard_text), out);
    snprintf(g_ChildClip.status, sizeof(g_ChildClip.status), "GetClipboardData(CF_TEXT) via IPC -> %.60s", out);
    child_clip_log(g_ChildClip.status);
}

static void child_clip_clear(void)
{
    BOOL ok = FALSE;
    if (OpenClipboard(g_ChildClip.hwnd)) { ok = EmptyClipboard(); CloseClipboard(); }
    if (ok) g_ChildClip.clear_count++;
    child_copy_cstr_bounded(g_ChildClip.clipboard_text, sizeof(g_ChildClip.clipboard_text), "<cleared>");
    snprintf(g_ChildClip.status, sizeof(g_ChildClip.status), "EmptyClipboard() via IPC -> %s", ok ? "TRUE" : "FALSE");
    child_clip_log(g_ChildClip.status);
}

static void child_clip_new_doc(void)
{
    snprintf(g_ChildClip.local_text, sizeof(g_ChildClip.local_text), "<new local buffer #%u>", (unsigned)++g_ChildClip.serial);
    snprintf(g_ChildClip.status, sizeof(g_ChildClip.status), "WM_COMMAND New/Ctrl+N -> local buffer reset");
    child_clip_log(g_ChildClip.status);
}

static void child_clip_popup(void)
{
    if (!g_ChildClip.hPopup) child_clip_attach_menu();
    BOOL r = TrackPopupMenu(g_ChildClip.hPopup, 0, 0, 0, 0, g_ChildClip.hwnd, NULL);
    if (r) g_ChildClip.popup_count++;
    snprintf(g_ChildClip.status, sizeof(g_ChildClip.status), "TrackPopupMenu modal -> %s cmd=0x%x", r ? "TRUE" : "FALSE", g_GuiIpcRuntime && g_GuiIpcRuntime->shared ? (unsigned)g_GuiIpcRuntime->shared->menu_last_command : 0u);
    child_clip_log(g_ChildClip.status);
}

static void child_clip_handle_command(UINT cmd, const char* via)
{
    g_ChildClip.command_count++;
    if (cmd == CLIP_CMD_SET) { child_clip_set_text(); return; }
    if (cmd == CLIP_CMD_GET) { child_clip_get_text(); return; }
    if (cmd == CLIP_CMD_CLEAR) { child_clip_clear(); return; }
    if (cmd == CLIP_CMD_ATTACH) { child_clip_attach_menu(); return; }
    if (cmd == CLIP_CMD_POPUP) { child_clip_popup(); return; }
    if (cmd == CLIP_CMD_COPY) { child_clip_set_text(); g_ChildClip.accel_count++; snprintf(g_ChildClip.status, sizeof(g_ChildClip.status), "%s Copy/Ctrl+C -> clipboard set", via); child_clip_log(g_ChildClip.status); return; }
    if (cmd == CLIP_CMD_PASTE) { child_clip_get_text(); g_ChildClip.accel_count++; snprintf(g_ChildClip.status, sizeof(g_ChildClip.status), "%s Paste/Ctrl+V -> clipboard read", via); child_clip_log(g_ChildClip.status); return; }
    if (cmd == CLIP_CMD_NEW) { g_ChildClip.accel_count++; child_clip_new_doc(); return; }
}

static void child_clip_hit_test(ChildIpcContext* ipc, int cx, int cy)
{
    if (!ipc || !ipc->shared) return;
    if (cy >= 8 && cy < 28) {
        if (cx >= 8   && cx < 120) { PostMessageA(g_ChildClip.hwnd, WM_COMMAND, CLIP_CMD_SET, 0); return; }
        if (cx >= 128 && cx < 240) { PostMessageA(g_ChildClip.hwnd, WM_COMMAND, CLIP_CMD_GET, 0); return; }
        if (cx >= 248 && cx < 360) { PostMessageA(g_ChildClip.hwnd, WM_COMMAND, CLIP_CMD_CLEAR, 0); return; }
        if (cx >= 368 && cx < 480) { PostMessageA(g_ChildClip.hwnd, WM_COMMAND, CLIP_CMD_ATTACH, 0); return; }
        if (cx >= 488 && cx < 600) { PostMessageA(g_ChildClip.hwnd, WM_COMMAND, CLIP_CMD_POPUP, 0); return; }
    }
    if (cy >= 34 && cy < 54) {
        if (cx >= 8   && cx < 120) { PostMessageA(g_ChildClip.hwnd, WM_COMMAND, CLIP_CMD_COPY, 0); return; }
        if (cx >= 128 && cx < 240) { PostMessageA(g_ChildClip.hwnd, WM_COMMAND, CLIP_CMD_PASTE, 0); return; }
        if (cx >= 248 && cx < 360) { PostMessageA(g_ChildClip.hwnd, WM_COMMAND, CLIP_CMD_NEW, 0); return; }
    }
}

static LRESULT child_clipmenu_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        memset(&g_ChildClip, 0, sizeof(g_ChildClip));
        g_ChildClip.hwnd = hwnd;
        child_copy_cstr_bounded(g_ChildClip.local_text, sizeof(g_ChildClip.local_text), "<local buffer>");
        child_copy_cstr_bounded(g_ChildClip.clipboard_text, sizeof(g_ChildClip.clipboard_text), "<not read yet>");
        child_copy_cstr_bounded(g_ChildClip.status, sizeof(g_ChildClip.status), "ClipMenuLab OOP ready: Clipboard + Menus + Accelerators");
        child_clip_log(g_ChildClip.status);
        child_clip_attach_menu();
        ACCEL a[3];
        a[0].fVirt = FVIRTKEY | FCONTROL; a[0].key = KEY_C; a[0].cmd = CLIP_CMD_COPY;
        a[1].fVirt = FVIRTKEY | FCONTROL; a[1].key = KEY_V; a[1].cmd = CLIP_CMD_PASTE;
        a[2].fVirt = FVIRTKEY | FCONTROL; a[2].key = KEY_N; a[2].cmd = CLIP_CMD_NEW;
        g_ChildClip.hAccel = CreateAcceleratorTableA(a, 3);
        child_clip_publish(ipc, msg, g_ChildClip.status);
        return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_PAINT) { child_clip_publish(ipc, msg, msg == WM_PAINT ? "clipmenu WM_PAINT" : "clipmenu move/resize repaint"); return 0; }
    if (msg == WM_LBUTTONDOWN) { child_clip_hit_test(ipc, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); child_clip_publish(ipc, msg, "clipmenu click"); return 0; }
    if (msg == WM_KEYDOWN) {
        MSG m; memset(&m, 0, sizeof(m)); m.hwnd = hwnd; m.message = WM_KEYDOWN; m.wParam = wp; m.lParam = lp;
        if (TranslateAcceleratorA(hwnd, g_ChildClip.hAccel, &m)) { child_clip_publish(ipc, msg, "TranslateAcceleratorA -> WM_COMMAND"); return 0; }
        child_clip_publish(ipc, msg, "clipmenu keydown"); return 0;
    }
    if (msg == WM_COMMAND) { child_clip_handle_command((UINT)wp, lp ? "ACCEL/MENU" : "BUTTON/MENU"); child_clip_publish(ipc, msg, g_ChildClip.status); return 0; }
    if (msg == WM_CLOSE) {
        if (ipc) ipc->close_seen = 1;
        if (g_ChildClip.hAccel) { DestroyAcceleratorTable(g_ChildClip.hAccel); g_ChildClip.hAccel = 0; }
        if (g_ChildClip.hMenu) { DestroyMenu(g_ChildClip.hMenu); g_ChildClip.hMenu = 0; }
        if (g_ChildClip.hPopup) { DestroyMenu(g_ChildClip.hPopup); g_ChildClip.hPopup = 0; }
        child_clip_publish(ipc, msg, "clipmenu WM_CLOSE");
        return 0;
    }
    return 0;
}

static int child_clipmenu_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "ClipMenuLab [OOP]";
    int x = 280, y = 160, w = 640, h = 360;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    MyGuiIpcRuntimeAttach(ipc);
    child_shared_update(ipc, "clipmenu-runtime-start", argc, argv, 0);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = child_clipmenu_wndproc;
    wc.lpszClassName = "myOS.OOPClipMenuLab";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) { child_clipmenu_wndproc(hwnd, WM_CREATE, 0, 0); child_clip_publish(ipc, WM_PAINT, "clipmenu window created"); }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v70 clipmenu oop started");
    printf("[v169 child pid=%ld] OOP clipmenu hwnd=%u menu=0x%x accel=0x%x\n", (long)getpid(), (unsigned)hwnd, (unsigned)g_ChildClip.hMenu, (unsigned)g_ChildClip.hAccel);
    uint32_t heartbeat = 0;
    while (hwnd && ipc && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) DispatchMessageA(&msg);
        child_shared_update(ipc, "clipmenu-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v70 clipmenu heartbeat");
    }
    child_clip_publish(ipc, WM_CLOSE, "clipmenu exiting");
    child_shared_update(ipc, ipc->close_seen ? "clipmenu-close-seen" : "clipmenu-exiting", argc, argv, 67);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 67, ipc->close_seen ? "clipmenu WM_CLOSE exit report" : "clipmenu exit report");
    return 67;
}



/* v70: OOP WaitLab/ObjectProbe.
   These are intentionally small but real: the Linux child owns the GUI loop and
   all Create/Open/Set/Reset/Wait/Duplicate/Close calls go through the v69/v70
   Kernel Bridge into the parent SessionKernel under this child's myOS PID. */
typedef struct ChildMiniRect { int x, y, w, h; } ChildMiniRect;

typedef struct ChildWaitLabState {
    HWND hwnd;
    HANDLE hEvent;
    HANDLE hOpen;
    HANDLE hDup;
    DWORD lastWait;
    uint32_t ops;
    uint32_t waits;
    char status[160];
} ChildWaitLabState;

static ChildWaitLabState g_ChildWaitLab;
static const char* g_v70_wait_event_name = "Global\\myos.v70.waitlab.event";

static int child_rect_hit(const ChildMiniRect* r, int x, int y)
{
    return r && x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static void child_button_rects(ChildMiniRect* out, int count, int x, int y, int w, int h, int gap)
{
    for (int i = 0; i < count; ++i) {
        out[i].x = x + i * (w + gap);
        out[i].y = y;
        out[i].w = w;
        out[i].h = h;
    }
}

static void child_draw_button(ChildIpcContext* ipc, ChildMiniRect r, const char* text)
{
    child_gdi_fill(ipc, r.x, r.y, r.w, r.h, CHILD_COLOR(42,47,66));
    child_gdi_rect(ipc, r.x, r.y, r.w, r.h, CHILD_COLOR(115,135,175));
    child_gdi_text(ipc, r.x, r.y, r.w, r.h, text, CHILD_COLOR(245,245,255), MYOS_GDI_TEXT_CENTER | MYOS_GDI_TEXT_VCENTER);
}

static void child_waitlab_render(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    int win_w = (int)ipc->shared->gui_w;
    int win_h = (int)ipc->shared->gui_h;
    int cw = win_w - 2;
    int ch = win_h - 24 - 1;
    if (cw < 520) cw = 520;
    if (ch < 220) ch = 220;
    child_gdi_reset(ipc, cw, ch, reason, status ? status : "waitlab v70 render");
    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(11,13,24));
    child_gdi_text(ipc, 12, 10, cw - 24, 16, "WaitLab [OOP v70] - real child process, Kernel Bridge waits", CHILD_COLOR(175,230,255), MYOS_GDI_TEXT_LEFT);

    ChildMiniRect b[9];
    child_button_rects(b, 9, 12, 36, 74, 24, 6);
    const char* labels[9] = { "Create", "Open", "Set", "Reset", "Wait0", "Wait1s", "WaitInf", "Dup", "Close" };
    for (int i = 0; i < 9; ++i) child_draw_button(ipc, b[i], labels[i]);

    char line[192];
    snprintf(line, sizeof(line), "Name: %s", g_v70_wait_event_name);
    child_gdi_text_n(ipc, 12, 74, cw - 24, 14, line, sizeof(line), CHILD_COLOR(230,230,245), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "myPID=%u linuxPID=%ld hEvent=0x%x hOpen=0x%x hDup=0x%x lastWait=0x%x ops=%u waits=%u",
             ipc->myPid, (long)getpid(), (unsigned)g_ChildWaitLab.hEvent, (unsigned)g_ChildWaitLab.hOpen,
             (unsigned)g_ChildWaitLab.hDup, (unsigned)g_ChildWaitLab.lastWait,
             (unsigned)g_ChildWaitLab.ops, (unsigned)g_ChildWaitLab.waits);
    child_gdi_text_n(ipc, 12, 94, cw - 24, 14, line, sizeof(line), CHILD_COLOR(175,255,185), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, 116, cw - 24, 14, g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), CHILD_COLOR(255,230,165), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, 138, cw - 24, 14, ipc->shared->kernel_status, sizeof(ipc->shared->kernel_status), CHILD_COLOR(190,215,255), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, 166, cw - 24, 14, "Test: open two WaitLabs. In A click WaitInf. In B click Open/Set. A must wake without parent deadlock.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, 186, cw - 24, 14, "ObjectLab classic can still observe both per-process handles; ObjectProbe OOP can create/open/dup the same named object.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static HANDLE child_waitlab_primary_handle(void)
{
    if (g_ChildWaitLab.hEvent) return g_ChildWaitLab.hEvent;
    if (g_ChildWaitLab.hOpen) return g_ChildWaitLab.hOpen;
    return 0;
}

static HANDLE child_waitlab_ensure_open_or_create(void)
{
    HANDLE h = child_waitlab_primary_handle();
    if (h) return h;
    g_ChildWaitLab.hOpen = OpenEventA(EVENT_ALL_ACCESS, FALSE, g_v70_wait_event_name);
    if (g_ChildWaitLab.hOpen) return g_ChildWaitLab.hOpen;
    g_ChildWaitLab.hEvent = CreateEventA(NULL, TRUE, FALSE, g_v70_wait_event_name);
    return g_ChildWaitLab.hEvent;
}

static void child_waitlab_close_all(void)
{
    if (g_ChildWaitLab.hDup) { CloseHandle(g_ChildWaitLab.hDup); g_ChildWaitLab.hDup = 0; }
    if (g_ChildWaitLab.hOpen) { CloseHandle(g_ChildWaitLab.hOpen); g_ChildWaitLab.hOpen = 0; }
    if (g_ChildWaitLab.hEvent) { CloseHandle(g_ChildWaitLab.hEvent); g_ChildWaitLab.hEvent = 0; }
}

static void child_waitlab_action(ChildIpcContext* ipc, HWND hwnd, int action)
{
    (void)hwnd;
    g_ChildWaitLab.ops++;
    switch (action) {
    case 0:
        if (!g_ChildWaitLab.hEvent) g_ChildWaitLab.hEvent = CreateEventA(NULL, TRUE, FALSE, g_v70_wait_event_name);
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "CreateEventA -> hEvent=0x%x", (unsigned)g_ChildWaitLab.hEvent);
        break;
    case 1:
        if (!g_ChildWaitLab.hOpen) g_ChildWaitLab.hOpen = OpenEventA(EVENT_ALL_ACCESS, FALSE, g_v70_wait_event_name);
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "OpenEventA -> hOpen=0x%x", (unsigned)g_ChildWaitLab.hOpen);
        break;
    case 2: {
        HANDLE h = child_waitlab_ensure_open_or_create();
        BOOL ok = h ? SetEvent(h) : FALSE;
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "SetEvent(0x%x) -> %s", (unsigned)h, ok ? "OK" : "FAIL");
        break;
    }
    case 3: {
        HANDLE h = child_waitlab_ensure_open_or_create();
        BOOL ok = h ? ResetEvent(h) : FALSE;
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "ResetEvent(0x%x) -> %s", (unsigned)h, ok ? "OK" : "FAIL");
        break;
    }
    case 4: {
        HANDLE h = child_waitlab_ensure_open_or_create();
        g_ChildWaitLab.waits++;
        g_ChildWaitLab.lastWait = h ? WaitForSingleObject(h, 0) : WAIT_FAILED;
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "WaitForSingleObject(0x%x, 0) -> 0x%x", (unsigned)h, (unsigned)g_ChildWaitLab.lastWait);
        break;
    }
    case 5: {
        HANDLE h = child_waitlab_ensure_open_or_create();
        g_ChildWaitLab.waits++;
        g_ChildWaitLab.lastWait = h ? WaitForSingleObject(h, 1000) : WAIT_FAILED;
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "WaitForSingleObject(0x%x,1000) -> 0x%x", (unsigned)h, (unsigned)g_ChildWaitLab.lastWait);
        break;
    }
    case 6: {
        HANDLE h = child_waitlab_ensure_open_or_create();
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "WAIT INFINITE pending on 0x%x - set from another WaitLab", (unsigned)h);
        child_waitlab_render(ipc, WM_PAINT, "waitlab infinite wait pending");
        g_ChildWaitLab.waits++;
        g_ChildWaitLab.lastWait = h ? WaitForSingleObject(h, INFINITE) : WAIT_FAILED;
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "WaitForSingleObject(0x%x,INFINITE) -> 0x%x", (unsigned)h, (unsigned)g_ChildWaitLab.lastWait);
        break;
    }
    case 7: {
        HANDLE h = child_waitlab_ensure_open_or_create();
        HANDLE dup = 0;
        BOOL ok = h ? DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS) : FALSE;
        if (ok) { if (g_ChildWaitLab.hDup) CloseHandle(g_ChildWaitLab.hDup); g_ChildWaitLab.hDup = dup; }
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "DuplicateHandle(0x%x) -> %s dup=0x%x", (unsigned)h, ok ? "OK" : "FAIL", (unsigned)dup);
        break;
    }
    case 8:
        child_waitlab_close_all();
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "CloseHandle all local child handles");
        break;
    default: break;
    }
    child_waitlab_render(ipc, WM_PAINT, g_ChildWaitLab.status);
}

static LRESULT child_waitlab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        memset(&g_ChildWaitLab, 0, sizeof(g_ChildWaitLab));
        g_ChildWaitLab.hwnd = hwnd;
        snprintf(g_ChildWaitLab.status, sizeof(g_ChildWaitLab.status), "WaitLab OOP ready. Use Global event across two child processes.");
        child_waitlab_render(ipc, msg, "waitlab create");
        return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_PAINT) { child_waitlab_render(ipc, msg, g_ChildWaitLab.status); return 0; }
    if (msg == WM_LBUTTONDOWN) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        ChildMiniRect b[9]; child_button_rects(b, 9, 12, 36, 74, 24, 6);
        for (int i = 0; i < 9; ++i) if (child_rect_hit(&b[i], x, y)) { child_waitlab_action(ipc, hwnd, i); return 0; }
        return 0;
    }
    if (msg == WM_CLOSE) { if (ipc) ipc->close_seen = 1; child_waitlab_close_all(); child_waitlab_render(ipc, msg, "waitlab close"); return 0; }
    return 0;
}

static int child_waitlab_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "WaitLab [OOP v70]";
    int x = 460, y = 240, w = 760, h = 310;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (w < 680) w = 680;
    if (h < 260) h = 260;
    MyGuiIpcRuntimeAttach(ipc);
    child_shared_update(ipc, "waitlab-oop-runtime-start", argc, argv, 0);
    WNDCLASSEXA wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = child_waitlab_wndproc; wc.lpszClassName = "myOS.OOPWaitLab";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) { child_waitlab_wndproc(hwnd, WM_CREATE, 0, 0); child_waitlab_render(ipc, WM_PAINT, "waitlab window created"); }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v70 waitlab oop started");
    uint32_t heartbeat = 0;
    while (hwnd && ipc && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) DispatchMessageA(&msg);
        child_shared_update(ipc, "waitlab-oop-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v70 waitlab heartbeat");
    }
    child_waitlab_close_all();
    child_shared_update(ipc, ipc->close_seen ? "waitlab-close-seen" : "waitlab-exiting", argc, argv, 70);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 70, ipc->close_seen ? "waitlab WM_CLOSE exit report" : "waitlab exit report");
    return 70;
}

typedef struct ChildObjectProbeState {
    HWND hwnd;
    HANDLE hEvent;
    HANDLE hOpen;
    HANDLE hDup;
    uint32_t ops;
    char status[160];
} ChildObjectProbeState;

static ChildObjectProbeState g_ChildObjectProbe;

static void child_objectprobe_render(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    int cw = (int)ipc->shared->gui_w - 2;
    int ch = (int)ipc->shared->gui_h - 25;
    if (cw < 560) cw = 560;
    if (ch < 220) ch = 220;
    child_gdi_reset(ipc, cw, ch, reason, status ? status : "objectprobe render");
    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(12,12,22));
    child_gdi_text(ipc, 12, 10, cw - 24, 16, "ObjectProbe [OOP v70] - child handle table/Object Manager bridge probe", CHILD_COLOR(185,230,255), MYOS_GDI_TEXT_LEFT);
    ChildMiniRect b[5]; child_button_rects(b, 5, 12, 38, 112, 24, 8);
    const char* labels[5] = { "Create Event", "Open Event", "Dup Self", "Set Event", "Close" };
    for (int i = 0; i < 5; ++i) child_draw_button(ipc, b[i], labels[i]);
    char line[192];
    snprintf(line, sizeof(line), "Shared name: %s", g_v70_wait_event_name);
    child_gdi_text_n(ipc, 12, 78, cw - 24, 14, line, sizeof(line), CHILD_COLOR(230,230,245), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "myPID=%u linuxPID=%ld hEvent=0x%x hOpen=0x%x hDup=0x%x ops=%u",
             ipc->myPid, (long)getpid(), (unsigned)g_ChildObjectProbe.hEvent, (unsigned)g_ChildObjectProbe.hOpen, (unsigned)g_ChildObjectProbe.hDup, (unsigned)g_ChildObjectProbe.ops);
    child_gdi_text_n(ipc, 12, 100, cw - 24, 14, line, sizeof(line), CHILD_COLOR(175,255,185), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, 124, cw - 24, 14, g_ChildObjectProbe.status, sizeof(g_ChildObjectProbe.status), CHILD_COLOR(255,230,165), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, 146, cw - 24, 14, ipc->shared->kernel_status, sizeof(ipc->shared->kernel_status), CHILD_COLOR(190,215,255), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, 176, cw - 24, 14, "This OOP probe cannot enumerate kernel tables itself yet; it creates handles visible in parent/ObjectLab classic/Spy diagnostics.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static HANDLE child_objectprobe_any(void)
{
    if (g_ChildObjectProbe.hEvent) return g_ChildObjectProbe.hEvent;
    if (g_ChildObjectProbe.hOpen) return g_ChildObjectProbe.hOpen;
    return 0;
}

static void child_objectprobe_close_all(void)
{
    if (g_ChildObjectProbe.hDup) { CloseHandle(g_ChildObjectProbe.hDup); g_ChildObjectProbe.hDup = 0; }
    if (g_ChildObjectProbe.hOpen) { CloseHandle(g_ChildObjectProbe.hOpen); g_ChildObjectProbe.hOpen = 0; }
    if (g_ChildObjectProbe.hEvent) { CloseHandle(g_ChildObjectProbe.hEvent); g_ChildObjectProbe.hEvent = 0; }
}

static void child_objectprobe_action(ChildIpcContext* ipc, int action)
{
    g_ChildObjectProbe.ops++;
    switch (action) {
    case 0:
        if (!g_ChildObjectProbe.hEvent) g_ChildObjectProbe.hEvent = CreateEventA(NULL, TRUE, FALSE, g_v70_wait_event_name);
        snprintf(g_ChildObjectProbe.status, sizeof(g_ChildObjectProbe.status), "CreateEventA -> hEvent=0x%x", (unsigned)g_ChildObjectProbe.hEvent);
        break;
    case 1:
        if (!g_ChildObjectProbe.hOpen) g_ChildObjectProbe.hOpen = OpenEventA(EVENT_ALL_ACCESS, FALSE, g_v70_wait_event_name);
        snprintf(g_ChildObjectProbe.status, sizeof(g_ChildObjectProbe.status), "OpenEventA -> hOpen=0x%x", (unsigned)g_ChildObjectProbe.hOpen);
        break;
    case 2: {
        HANDLE h = child_objectprobe_any();
        if (!h) h = (g_ChildObjectProbe.hEvent = CreateEventA(NULL, TRUE, FALSE, g_v70_wait_event_name));
        HANDLE dup = 0;
        BOOL ok = h ? DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS) : FALSE;
        if (ok) { if (g_ChildObjectProbe.hDup) CloseHandle(g_ChildObjectProbe.hDup); g_ChildObjectProbe.hDup = dup; }
        snprintf(g_ChildObjectProbe.status, sizeof(g_ChildObjectProbe.status), "DuplicateHandle(0x%x) -> %s dup=0x%x", (unsigned)h, ok ? "OK" : "FAIL", (unsigned)dup);
        break;
    }
    case 3: {
        HANDLE h = child_objectprobe_any();
        if (!h) h = (g_ChildObjectProbe.hOpen = OpenEventA(EVENT_ALL_ACCESS, FALSE, g_v70_wait_event_name));
        BOOL ok = h ? SetEvent(h) : FALSE;
        snprintf(g_ChildObjectProbe.status, sizeof(g_ChildObjectProbe.status), "SetEvent(0x%x) -> %s", (unsigned)h, ok ? "OK" : "FAIL");
        break;
    }
    case 4:
        child_objectprobe_close_all();
        snprintf(g_ChildObjectProbe.status, sizeof(g_ChildObjectProbe.status), "CloseHandle all ObjectProbe handles");
        break;
    default: break;
    }
    child_objectprobe_render(ipc, WM_PAINT, g_ChildObjectProbe.status);
}

static LRESULT child_objectprobe_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        memset(&g_ChildObjectProbe, 0, sizeof(g_ChildObjectProbe));
        g_ChildObjectProbe.hwnd = hwnd;
        snprintf(g_ChildObjectProbe.status, sizeof(g_ChildObjectProbe.status), "ObjectProbe OOP ready - create/open/dup named object handles.");
        child_objectprobe_render(ipc, msg, "objectprobe create"); return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_PAINT) { child_objectprobe_render(ipc, msg, g_ChildObjectProbe.status); return 0; }
    if (msg == WM_LBUTTONDOWN) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        ChildMiniRect b[5]; child_button_rects(b, 5, 12, 38, 112, 24, 8);
        for (int i = 0; i < 5; ++i) if (child_rect_hit(&b[i], x, y)) { child_objectprobe_action(ipc, i); return 0; }
        return 0;
    }
    if (msg == WM_CLOSE) { if (ipc) ipc->close_seen = 1; child_objectprobe_close_all(); child_objectprobe_render(ipc, msg, "objectprobe close"); return 0; }
    return 0;
}

static int child_objectprobe_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "ObjectProbe [OOP v70]";
    int x = 420, y = 220, w = 700, h = 270;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (w < 620) w = 620;
    if (h < 240) h = 240;
    MyGuiIpcRuntimeAttach(ipc);
    child_shared_update(ipc, "objectprobe-oop-runtime-start", argc, argv, 0);
    WNDCLASSEXA wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = child_objectprobe_wndproc; wc.lpszClassName = "myOS.OOPObjectProbe";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) { child_objectprobe_wndproc(hwnd, WM_CREATE, 0, 0); child_objectprobe_render(ipc, WM_PAINT, "objectprobe window created"); }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v70 objectprobe oop started");
    uint32_t heartbeat = 0;
    while (hwnd && ipc && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) DispatchMessageA(&msg);
        child_shared_update(ipc, "objectprobe-oop-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v70 objectprobe heartbeat");
    }
    child_objectprobe_close_all();
    child_shared_update(ipc, ipc->close_seen ? "objectprobe-close-seen" : "objectprobe-exiting", argc, argv, 70);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 70, ipc->close_seen ? "objectprobe WM_CLOSE exit report" : "objectprobe exit report");
    return 70;
}


/* v71: OOP SectionLab. Parent owns the Section HANDLE/Object Manager entry;
   child maps the POSIX shm backing locally. This is the real payload side of
   "Section = payload, Event = signal, Message = UI notification". */
static const char* g_v71_section_name = "Global\\myos.v71.shared.section";
static const char* g_v71_section_event_name = "Global\\myos.v71.shared.event";

typedef struct ChildSectionLabState {
    HWND hwnd;
    HANDLE hMap;
    HANDLE hOpen;
    HANDLE hEvent;
    LPVOID view;
    DWORD writes;
    DWORD reads;
    DWORD signals;
    DWORD waits;
    DWORD lastWait;
    DWORD ops;
    char status[160];
    char lastRead[192];
} ChildSectionLabState;

static ChildSectionLabState g_ChildSectionLab;

static HANDLE child_section_any_map(void)
{
    if (g_ChildSectionLab.hMap) return g_ChildSectionLab.hMap;
    if (g_ChildSectionLab.hOpen) return g_ChildSectionLab.hOpen;
    return 0;
}

static void child_section_ensure_event(void)
{
    if (!g_ChildSectionLab.hEvent) g_ChildSectionLab.hEvent = OpenEventA(EVENT_ALL_ACCESS, FALSE, g_v71_section_event_name);
    if (!g_ChildSectionLab.hEvent) g_ChildSectionLab.hEvent = CreateEventA(NULL, TRUE, FALSE, g_v71_section_event_name);
}

static void child_section_close_all(void)
{
    if (g_ChildSectionLab.view) { UnmapViewOfFile(g_ChildSectionLab.view); g_ChildSectionLab.view = NULL; }
    if (g_ChildSectionLab.hOpen) { CloseHandle(g_ChildSectionLab.hOpen); g_ChildSectionLab.hOpen = 0; }
    if (g_ChildSectionLab.hMap) { CloseHandle(g_ChildSectionLab.hMap); g_ChildSectionLab.hMap = 0; }
    if (g_ChildSectionLab.hEvent) { CloseHandle(g_ChildSectionLab.hEvent); g_ChildSectionLab.hEvent = 0; }
}

static void child_section_render(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    int cw = ipc->shared->gdi_client_w ? (int)ipc->shared->gdi_client_w : 760;
    int ch = ipc->shared->gdi_client_h ? (int)ipc->shared->gdi_client_h : 360;
    if (cw < 680) cw = 680;
    if (ch < 300) ch = 300;
    child_gdi_reset(ipc, cw, ch, reason, status ? status : "sectionlab v71 render");
    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(10,13,23));
    child_gdi_text(ipc, 12, 10, cw - 24, 16, "SectionLab [OOP v71] - CreateFileMapping/OpenFileMapping/MapView via Kernel Bridge", CHILD_COLOR(185,235,255), MYOS_GDI_TEXT_LEFT);

    const char* labels[9] = { "Create", "Open", "Map", "Write", "Read", "Signal", "Wait0", "Unmap", "Close" };
    ChildMiniRect b[9]; child_button_rects(b, 9, 12, 38, 74, 24, 6);
    for (int i = 0; i < 9; ++i) child_draw_button(ipc, b[i], labels[i]);

    char line[256];
    snprintf(line, sizeof(line), "myPid=%u linuxPid=%ld hMap=0x%x hOpen=0x%x hEvent=0x%x view=%p writes=%u reads=%u signals=%u waits=%u lastWait=0x%x",
             ipc->myPid, (long)getpid(), (unsigned)g_ChildSectionLab.hMap, (unsigned)g_ChildSectionLab.hOpen,
             (unsigned)g_ChildSectionLab.hEvent, g_ChildSectionLab.view, (unsigned)g_ChildSectionLab.writes,
             (unsigned)g_ChildSectionLab.reads, (unsigned)g_ChildSectionLab.signals, (unsigned)g_ChildSectionLab.waits,
             (unsigned)g_ChildSectionLab.lastWait);
    child_gdi_text_n(ipc, 12, 78, cw - 24, 14, line, sizeof(line), CHILD_COLOR(230,230,245), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, 102, cw - 24, 14, g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), CHILD_COLOR(255,230,165), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, 124, cw - 24, 14, ipc->shared->kernel_status, sizeof(ipc->shared->kernel_status), CHILD_COLOR(190,215,255), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "Section: %s", g_v71_section_name);
    child_gdi_text_n(ipc, 12, 152, cw - 24, 14, line, sizeof(line), CHILD_COLOR(175,255,185), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "Mapped backing: %s bytes=%u", ipc->shared->kernel_map_name[0] ? ipc->shared->kernel_map_name : "<none yet>", (unsigned)ipc->shared->kernel_map_size);
    child_gdi_text_n(ipc, 12, 174, cw - 24, 14, line, sizeof(line), CHILD_COLOR(175,255,185), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "Last read: %.180s", g_ChildSectionLab.lastRead[0] ? g_ChildSectionLab.lastRead : "<empty>");
    child_gdi_text_n(ipc, 12, 202, cw - 24, 14, line, sizeof(line), CHILD_COLOR(235,245,255), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, 232, cw - 24, 14, "Test: open two SectionLabs. A Create/Map/Write/Signal. B Open/Map/Wait0/Read. Both see the same shared payload.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, 252, cw - 24, 14, "Important: child never receives a parent pointer; it receives shm metadata and mmap()s its own address space.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static void child_section_action(ChildIpcContext* ipc, int action)
{
    g_ChildSectionLab.ops++;
    switch (action) {
    case 0: /* Create */
        if (!g_ChildSectionLab.hMap) g_ChildSectionLab.hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, g_v71_section_name);
        child_section_ensure_event();
        snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "CreateFileMappingA -> hMap=0x%x event=0x%x", (unsigned)g_ChildSectionLab.hMap, (unsigned)g_ChildSectionLab.hEvent);
        break;
    case 1: /* Open */
        if (!g_ChildSectionLab.hOpen) g_ChildSectionLab.hOpen = OpenFileMappingA(FILE_MAP_READ|FILE_MAP_WRITE, FALSE, g_v71_section_name);
        child_section_ensure_event();
        snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "OpenFileMappingA -> hOpen=0x%x event=0x%x", (unsigned)g_ChildSectionLab.hOpen, (unsigned)g_ChildSectionLab.hEvent);
        break;
    case 2: { /* Map */
        HANDLE h = child_section_any_map();
        if (!h) h = (g_ChildSectionLab.hOpen = OpenFileMappingA(FILE_MAP_READ|FILE_MAP_WRITE, FALSE, g_v71_section_name));
        if (!h) h = (g_ChildSectionLab.hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, g_v71_section_name));
        if (!g_ChildSectionLab.view && h) g_ChildSectionLab.view = MapViewOfFile(h, FILE_MAP_READ|FILE_MAP_WRITE, 0, 0, 4096);
        snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "MapViewOfFile(0x%x) -> %p shm=%s", (unsigned)h, g_ChildSectionLab.view, ipc && ipc->shared ? ipc->shared->kernel_map_name : "");
        break;
    }
    case 3: /* Write */
        if (!g_ChildSectionLab.view) child_section_action(ipc, 2);
        if (g_ChildSectionLab.view) {
            g_ChildSectionLab.writes++;
            snprintf((char*)g_ChildSectionLab.view, 4096, "v71 shared payload #%u from myPid=%u linuxPid=%ld view=%p",
                     (unsigned)g_ChildSectionLab.writes, ipc ? ipc->myPid : 0u, (long)getpid(), g_ChildSectionLab.view);
            FlushViewOfFile(g_ChildSectionLab.view, 0);
            snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "WRITE -> shared section #%u", (unsigned)g_ChildSectionLab.writes);
        } else snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "WRITE failed: no mapped view");
        break;
    case 4: /* Read */
        if (!g_ChildSectionLab.view) child_section_action(ipc, 2);
        if (g_ChildSectionLab.view) {
            g_ChildSectionLab.reads++;
            snprintf(g_ChildSectionLab.lastRead, sizeof(g_ChildSectionLab.lastRead), "%s", (const char*)g_ChildSectionLab.view);
            snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "READ <- %.80s", g_ChildSectionLab.lastRead[0] ? g_ChildSectionLab.lastRead : "<empty>");
        } else snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "READ failed: no mapped view");
        break;
    case 5: /* Signal */
        child_section_ensure_event();
        if (g_ChildSectionLab.hEvent && SetEvent(g_ChildSectionLab.hEvent)) {
            g_ChildSectionLab.signals++;
            snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "SetEvent(section event) -> OK signals=%u", (unsigned)g_ChildSectionLab.signals);
        } else snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "SetEvent(section event) -> FAIL");
        break;
    case 6: /* Wait0 */
        child_section_ensure_event();
        g_ChildSectionLab.lastWait = g_ChildSectionLab.hEvent ? WaitForSingleObject(g_ChildSectionLab.hEvent, 0) : WAIT_FAILED;
        g_ChildSectionLab.waits++;
        if (g_ChildSectionLab.lastWait == WAIT_OBJECT_0) {
            ResetEvent(g_ChildSectionLab.hEvent);
            child_section_action(ipc, 4);
        } else snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "WaitForSingleObject(event,0) -> 0x%x", (unsigned)g_ChildSectionLab.lastWait);
        break;
    case 7: /* Unmap */
        if (g_ChildSectionLab.view) { UnmapViewOfFile(g_ChildSectionLab.view); g_ChildSectionLab.view = NULL; }
        snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "UnmapViewOfFile -> done; handles remain open");
        break;
    case 8: /* Close */
        child_section_close_all();
        snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "Unmap + CloseHandle all section/event handles");
        break;
    default: break;
    }
    child_section_render(ipc, WM_PAINT, g_ChildSectionLab.status);
}

static LRESULT child_section_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        memset(&g_ChildSectionLab, 0, sizeof(g_ChildSectionLab));
        g_ChildSectionLab.hwnd = hwnd;
        snprintf(g_ChildSectionLab.status, sizeof(g_ChildSectionLab.status), "SectionLab OOP ready - Create/Open/Map shared section.");
        child_section_render(ipc, msg, "sectionlab create"); return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_PAINT) { child_section_render(ipc, msg, g_ChildSectionLab.status); return 0; }
    if (msg == WM_LBUTTONDOWN) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        ChildMiniRect b[9]; child_button_rects(b, 9, 12, 38, 74, 24, 6);
        for (int i = 0; i < 9; ++i) if (child_rect_hit(&b[i], x, y)) { child_section_action(ipc, i); return 0; }
        return 0;
    }
    if (msg == WM_CLOSE) { if (ipc) ipc->close_seen = 1; child_section_close_all(); child_section_render(ipc, msg, "sectionlab close"); return 0; }
    return 0;
}

static int child_sectionlab_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "SectionLab [OOP v71]";
    int x = 380, y = 180, w = 820, h = 360;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (w < 700) w = 700;
    if (h < 320) h = 320;
    MyGuiIpcRuntimeAttach(ipc);
    child_shared_update(ipc, "sectionlab-oop-runtime-start", argc, argv, 0);
    WNDCLASSEXA wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = child_section_wndproc; wc.lpszClassName = "myOS.OOPSectionLab";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) { child_section_wndproc(hwnd, WM_CREATE, 0, 0); child_section_render(ipc, WM_PAINT, "sectionlab window created"); }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v71 sectionlab oop started");
    uint32_t heartbeat = 0;
    while (hwnd && ipc && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) DispatchMessageA(&msg);
        child_shared_update(ipc, "sectionlab-oop-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v71 sectionlab heartbeat");
    }
    child_section_close_all();
    child_shared_update(ipc, ipc->close_seen ? "sectionlab-close-seen" : "sectionlab-exiting", argc, argv, 71);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 71, ipc->close_seen ? "sectionlab WM_CLOSE exit report" : "sectionlab exit report");
    return 71;
}


/* v72: OOP StateBusLab / dirty-notify shared-state demo.
   Two independent GUI children share a named Section as the payload lane.
   The publisher writes the latest state into the section and then posts only
   a tiny dirty signal (plus SetEvent for wait-style probes).  The subscriber
   reads the current state from its own mmap; no payload is copied in the
   window/message queue. */
#define STATEBUS_MAGIC 0x53544232u /* 'STB2' */
#define STATEBUS_NOTIFY (WM_USER + 0x720u)

static const char* g_v72_statebus_section_name = "Global\\myos.v72.statebus.section";
static const char* g_v72_statebus_event_name   = "Global\\myos.v72.statebus.event";

typedef struct ChildStateBusPayload {
    uint32_t magic;
    uint32_t seqBegin;
    uint32_t version;
    uint32_t seqEnd;
    uint32_t publisherPid;
    uint32_t publisherHwnd;
    uint32_t subscriberPid;
    uint32_t subscriberHwnd;
    uint32_t writes;
    uint32_t notifyPosts;
    uint32_t notifyReceived;
    uint32_t coalesced;
    uint32_t eventSignals;
    uint32_t waitHits;
    uint32_t notifyPending;
    uint32_t lastMsg;
    int32_t  lastX;
    int32_t  lastY;
    int32_t  lastW;
    int32_t  lastH;
    char     text[256];
} ChildStateBusPayload;

typedef struct ChildStateBusLabState {
    HWND hwnd;
    HANDLE hMap;
    HANDLE hOpen;
    HANDLE hEvent;
    ChildStateBusPayload* view;
    uint32_t role; /* 0 = neutral, 1 = publisher, 2 = subscriber */
    uint32_t ops;
    uint32_t localPublishes;
    uint32_t localReads;
    uint32_t localNotifies;
    uint32_t localWaits;
    uint32_t lastSeenVersion;
    uint32_t lastWait;
    char status[180];
    char lastRead[256];
} ChildStateBusLabState;

static ChildStateBusLabState g_ChildStateBus;

static void child_statebus_close_all(void)
{
    if (g_ChildStateBus.view) { UnmapViewOfFile(g_ChildStateBus.view); g_ChildStateBus.view = NULL; }
    if (g_ChildStateBus.hMap) { CloseHandle(g_ChildStateBus.hMap); g_ChildStateBus.hMap = 0; }
    if (g_ChildStateBus.hOpen) { CloseHandle(g_ChildStateBus.hOpen); g_ChildStateBus.hOpen = 0; }
    if (g_ChildStateBus.hEvent) { CloseHandle(g_ChildStateBus.hEvent); g_ChildStateBus.hEvent = 0; }
}

static void child_statebus_ensure_event(void)
{
    if (!g_ChildStateBus.hEvent) g_ChildStateBus.hEvent = OpenEventA(EVENT_ALL_ACCESS, FALSE, g_v72_statebus_event_name);
    if (!g_ChildStateBus.hEvent) g_ChildStateBus.hEvent = CreateEventA(NULL, TRUE, FALSE, g_v72_statebus_event_name);
}

static int child_statebus_ensure_map(ChildIpcContext* ipc, int writeAccess)
{
    (void)ipc;
    if (!g_ChildStateBus.hMap && !g_ChildStateBus.hOpen)
        g_ChildStateBus.hOpen = OpenFileMappingA(FILE_MAP_READ|FILE_MAP_WRITE, FALSE, g_v72_statebus_section_name);
    if (!g_ChildStateBus.hMap && !g_ChildStateBus.hOpen)
        g_ChildStateBus.hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, g_v72_statebus_section_name);

    HANDLE h = g_ChildStateBus.hMap ? g_ChildStateBus.hMap : g_ChildStateBus.hOpen;
    if (!g_ChildStateBus.view && h) {
        DWORD access = FILE_MAP_READ | (writeAccess ? FILE_MAP_WRITE : 0u);
        g_ChildStateBus.view = (ChildStateBusPayload*)MapViewOfFile(h, access, 0, 0, 4096);
    }
    if (g_ChildStateBus.view && g_ChildStateBus.view->magic != STATEBUS_MAGIC && writeAccess) {
        memset(g_ChildStateBus.view, 0, sizeof(*g_ChildStateBus.view));
        g_ChildStateBus.view->magic = STATEBUS_MAGIC;
        g_ChildStateBus.view->seqBegin = 2;
        g_ChildStateBus.view->seqEnd = 2;
        FlushViewOfFile(g_ChildStateBus.view, sizeof(*g_ChildStateBus.view));
    }
    child_statebus_ensure_event();
    return g_ChildStateBus.view != NULL;
}

static int child_statebus_read_snapshot(ChildStateBusPayload* out)
{
    if (!out || !g_ChildStateBus.view || g_ChildStateBus.view->magic != STATEBUS_MAGIC) return 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t a = g_ChildStateBus.view->seqBegin;
        ChildStateBusPayload tmp = *g_ChildStateBus.view;
        uint32_t b = g_ChildStateBus.view->seqEnd;
        if (a == b && ((a & 1u) == 0u) && tmp.magic == STATEBUS_MAGIC) {
            *out = tmp;
            return 1;
        }
        usleep(1000);
    }
    return 0;
}

static void child_statebus_write_payload(ChildIpcContext* ipc, int spamIndex)
{
    if (!child_statebus_ensure_map(ipc, 1) || !g_ChildStateBus.view) {
        snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "PUBLISH failed: no shared state section");
        return;
    }

    ChildStateBusPayload* p = g_ChildStateBus.view;
    uint32_t seq = p->seqEnd + 1u;
    if ((seq & 1u) == 0u) seq++;
    p->seqBegin = seq;
    p->magic = STATEBUS_MAGIC;
    p->version++;
    p->publisherPid = ipc ? ipc->myPid : 0u;
    p->publisherHwnd = g_ChildStateBus.hwnd;
    p->writes++;
    p->lastMsg = STATEBUS_NOTIFY;
    p->lastX = spamIndex;
    p->lastY = (int32_t)g_ChildStateBus.localPublishes;
    p->lastW = ipc && ipc->shared ? (int32_t)ipc->shared->gui_w : 0;
    p->lastH = ipc && ipc->shared ? (int32_t)ipc->shared->gui_h : 0;
    snprintf(p->text, sizeof(p->text), "v72 StateBus payload v%u from myPid=%u hwnd=%u linuxPid=%ld spam=%d",
             (unsigned)p->version, ipc ? ipc->myPid : 0u, (unsigned)g_ChildStateBus.hwnd, (long)getpid(), spamIndex);

    int shouldPost = 0;
    if (!p->notifyPending && p->subscriberHwnd) {
        p->notifyPending = 1;
        p->notifyPosts++;
        shouldPost = 1;
    } else if (p->notifyPending) {
        p->coalesced++;
    }
    p->eventSignals++;
    uint32_t version = p->version;
    HWND target = (HWND)p->subscriberHwnd;
    seq++;
    if (seq & 1u) seq++;
    p->seqEnd = seq;
    p->seqBegin = seq;
    FlushViewOfFile(p, sizeof(*p));

    g_ChildStateBus.localPublishes++;
    if (g_ChildStateBus.hEvent) SetEvent(g_ChildStateBus.hEvent);
    if (shouldPost && target) PostMessageA(target, STATEBUS_NOTIFY, (WPARAM)version, (LPARAM)g_ChildStateBus.hwnd);
    snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status),
             "PUBLISH v%u -> subHWND=%u %s", (unsigned)version, (unsigned)target, shouldPost ? "POSTED dirty msg" : "COALESCED/no-sub");
}

static void child_statebus_read_now(ChildIpcContext* ipc, const char* reason)
{
    child_statebus_ensure_map(ipc, 1);
    ChildStateBusPayload snap;
    if (!child_statebus_read_snapshot(&snap)) {
        snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "%s failed: no stable shared state", reason ? reason : "READ");
        return;
    }
    g_ChildStateBus.localReads++;
    g_ChildStateBus.lastSeenVersion = snap.version;
    snprintf(g_ChildStateBus.lastRead, sizeof(g_ChildStateBus.lastRead), "%s", snap.text);
    snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status),
             "%s <- v%u writes=%u posted=%u coal=%u pending=%u", reason ? reason : "READ",
             (unsigned)snap.version, (unsigned)snap.writes, (unsigned)snap.notifyPosts,
             (unsigned)snap.coalesced, (unsigned)snap.notifyPending);
}

static void child_statebus_bind_role(ChildIpcContext* ipc, uint32_t role)
{
    if (!child_statebus_ensure_map(ipc, 1) || !g_ChildStateBus.view) {
        snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "ROLE failed: map missing");
        return;
    }
    g_ChildStateBus.role = role;
    ChildStateBusPayload* p = g_ChildStateBus.view;
    uint32_t seq = p->seqEnd + 1u;
    if ((seq & 1u) == 0u) seq++;
    p->seqBegin = seq;
    p->magic = STATEBUS_MAGIC;
    if (role == 1) { p->publisherPid = ipc ? ipc->myPid : 0u; p->publisherHwnd = g_ChildStateBus.hwnd; }
    if (role == 2) { p->subscriberPid = ipc ? ipc->myPid : 0u; p->subscriberHwnd = g_ChildStateBus.hwnd; p->notifyPending = 0; }
    seq++;
    if (seq & 1u) seq++;
    p->seqEnd = seq;
    p->seqBegin = seq;
    FlushViewOfFile(p, sizeof(*p));
    snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "%s bound: myPid=%u hwnd=%u section=%s",
             role == 1 ? "Publisher" : "Subscriber", ipc ? ipc->myPid : 0u, (unsigned)g_ChildStateBus.hwnd, g_v72_statebus_section_name);
}

static void child_statebus_clear_pending(ChildIpcContext* ipc)
{
    if (!child_statebus_ensure_map(ipc, 1) || !g_ChildStateBus.view || g_ChildStateBus.view->magic != STATEBUS_MAGIC) return;
    uint32_t seq = g_ChildStateBus.view->seqEnd + 1u;
    if ((seq & 1u) == 0u) seq++;
    g_ChildStateBus.view->seqBegin = seq;
    g_ChildStateBus.view->notifyPending = 0;
    g_ChildStateBus.view->notifyReceived++;
    seq++;
    if (seq & 1u) seq++;
    g_ChildStateBus.view->seqEnd = seq;
    g_ChildStateBus.view->seqBegin = seq;
    FlushViewOfFile(g_ChildStateBus.view, sizeof(*g_ChildStateBus.view));
}

static void child_statebus_render(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    int win_w = (int)ipc->shared->gui_w;
    int win_h = (int)ipc->shared->gui_h;
    int cw = win_w - 2;
    int ch = win_h - 24 - 1;
    if (cw < 720) cw = 720;
    if (ch < 300) ch = 300;
    child_gdi_reset(ipc, cw, ch, reason, status ? status : "statebus v72 render");
    child_gdi_text(ipc, 12, 10, cw - 24, 16, "StateBusLab [OOP v72] - shared Section payload + coalesced dirty PostMessage/Event", CHILD_COLOR(185,235,255), MYOS_GDI_TEXT_LEFT);

    ChildMiniRect b[9]; child_button_rects(b, 9, 12, 38, 78, 24, 6);
    const char* labels[9] = { "Pub", "Sub", "Map", "Publish", "Read", "Wait0", "Spam100", "Reset", "Close" };
    for (int i = 0; i < 9; ++i) child_draw_button(ipc, b[i], labels[i]);

    ChildStateBusPayload snap;
    int have = child_statebus_read_snapshot(&snap);
    char line[256];
    snprintf(line, sizeof(line), "myPid=%u linuxPid=%ld hwnd=%u role=%s hMap=0x%x hOpen=0x%x hEvent=0x%x view=%p local pub/read/notif/wait=%u/%u/%u/%u",
             ipc->myPid, (long)getpid(), (unsigned)g_ChildStateBus.hwnd,
             g_ChildStateBus.role == 1 ? "PUB" : (g_ChildStateBus.role == 2 ? "SUB" : "neutral"),
             (unsigned)g_ChildStateBus.hMap, (unsigned)g_ChildStateBus.hOpen, (unsigned)g_ChildStateBus.hEvent,
             g_ChildStateBus.view, (unsigned)g_ChildStateBus.localPublishes, (unsigned)g_ChildStateBus.localReads,
             (unsigned)g_ChildStateBus.localNotifies, (unsigned)g_ChildStateBus.localWaits);
    child_gdi_text_n(ipc, 12, 78, cw - 24, 14, line, sizeof(line), CHILD_COLOR(230,230,245), MYOS_GDI_TEXT_LEFT);

    child_gdi_text_n(ipc, 12, 100, cw - 24, 14, g_ChildStateBus.status, sizeof(g_ChildStateBus.status), CHILD_COLOR(255,230,165), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, 122, cw - 24, 14, ipc->shared->kernel_status, sizeof(ipc->shared->kernel_status), CHILD_COLOR(190,215,255), MYOS_GDI_TEXT_LEFT);

    if (have) {
        snprintf(line, sizeof(line), "shared v=%u pubPid=%u pubHWND=%u subPid=%u subHWND=%u writes=%u posts=%u recv=%u coal=%u pending=%u eventSig=%u waitHits=%u",
                 (unsigned)snap.version, (unsigned)snap.publisherPid, (unsigned)snap.publisherHwnd,
                 (unsigned)snap.subscriberPid, (unsigned)snap.subscriberHwnd, (unsigned)snap.writes,
                 (unsigned)snap.notifyPosts, (unsigned)snap.notifyReceived, (unsigned)snap.coalesced,
                 (unsigned)snap.notifyPending, (unsigned)snap.eventSignals, (unsigned)snap.waitHits);
    } else {
        snprintf(line, sizeof(line), "shared: <not mapped/initialized yet> section=%s", g_v72_statebus_section_name);
    }
    child_gdi_text_n(ipc, 12, 150, cw - 24, 14, line, sizeof(line), CHILD_COLOR(175,255,185), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "last read: %.190s", g_ChildStateBus.lastRead[0] ? g_ChildStateBus.lastRead : (have ? snap.text : "<empty>"));
    child_gdi_text_n(ipc, 12, 174, cw - 24, 14, line, sizeof(line), CHILD_COLOR(235,245,255), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "map backing: %s bytes=%u", ipc->shared->kernel_map_name[0] ? ipc->shared->kernel_map_name : "<none yet>", (unsigned)ipc->shared->kernel_map_size);
    child_gdi_text_n(ipc, 12, 198, cw - 24, 14, line, sizeof(line), CHILD_COLOR(175,255,185), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, 232, cw - 24, 14, "Test: open pair. Subscriber clicks Sub/Map. Publisher clicks Pub/Publish or Spam100. Subscriber gets one dirty msg and reads latest state.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, 252, cw - 24, 14, "This is the WindowState/DWM pattern: shared memory holds current state; queue/event only says 'dirty, re-read'.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static void child_statebus_action(ChildIpcContext* ipc, int action)
{
    g_ChildStateBus.ops++;
    switch (action) {
    case 0: child_statebus_bind_role(ipc, 1); break; /* Pub */
    case 1: child_statebus_bind_role(ipc, 2); break; /* Sub */
    case 2:
        if (child_statebus_ensure_map(ipc, 1)) snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "MAP OK section=%s view=%p", g_v72_statebus_section_name, g_ChildStateBus.view);
        else snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "MAP failed");
        break;
    case 3: child_statebus_write_payload(ipc, 0); break;
    case 4: child_statebus_read_now(ipc, "READ"); break;
    case 5:
        child_statebus_ensure_event();
        g_ChildStateBus.lastWait = g_ChildStateBus.hEvent ? WaitForSingleObject(g_ChildStateBus.hEvent, 0) : WAIT_FAILED;
        g_ChildStateBus.localWaits++;
        if (g_ChildStateBus.lastWait == WAIT_OBJECT_0) {
            if (g_ChildStateBus.view && g_ChildStateBus.view->magic == STATEBUS_MAGIC) { g_ChildStateBus.view->waitHits++; FlushViewOfFile(g_ChildStateBus.view, sizeof(*g_ChildStateBus.view)); }
            ResetEvent(g_ChildStateBus.hEvent);
            child_statebus_read_now(ipc, "WAIT0 dirty");
        } else snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "WaitForSingleObject(event,0) -> 0x%x", (unsigned)g_ChildStateBus.lastWait);
        break;
    case 6:
        for (int i = 1; i <= 100; ++i) child_statebus_write_payload(ipc, i);
        break;
    case 7:
        if (g_ChildStateBus.view && g_ChildStateBus.view->magic == STATEBUS_MAGIC) { g_ChildStateBus.view->notifyPending = 0; FlushViewOfFile(g_ChildStateBus.view, sizeof(*g_ChildStateBus.view)); }
        if (g_ChildStateBus.hEvent) ResetEvent(g_ChildStateBus.hEvent);
        snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "Reset dirty-pending + event");
        break;
    case 8:
        child_statebus_close_all();
        snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "Unmap + CloseHandle all StateBus handles");
        break;
    default: break;
    }
    child_statebus_render(ipc, WM_PAINT, g_ChildStateBus.status);
}

static LRESULT child_statebus_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        memset(&g_ChildStateBus, 0, sizeof(g_ChildStateBus));
        g_ChildStateBus.hwnd = hwnd;
        snprintf(g_ChildStateBus.status, sizeof(g_ChildStateBus.status), "StateBus OOP ready - bind Pub/Sub and share current state via Section.");
        child_statebus_render(ipc, msg, "statebus create");
        return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_PAINT) { child_statebus_render(ipc, msg, g_ChildStateBus.status); return 0; }
    if (msg == WM_LBUTTONDOWN) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        ChildMiniRect b[9]; child_button_rects(b, 9, 12, 38, 78, 24, 6);
        for (int i = 0; i < 9; ++i) if (child_rect_hit(&b[i], x, y)) { child_statebus_action(ipc, i); return 0; }
        return 0;
    }
    if (msg == STATEBUS_NOTIFY) {
        g_ChildStateBus.localNotifies++;
        child_statebus_clear_pending(ipc);
        child_statebus_read_now(ipc, "DIRTY MSG");
        child_statebus_render(ipc, msg, g_ChildStateBus.status);
        return 0;
    }
    if (msg == WM_CLOSE) { if (ipc) ipc->close_seen = 1; child_statebus_close_all(); child_statebus_render(ipc, msg, "statebus close"); return 0; }
    (void)wp;
    return 0;
}

static int child_statebus_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "StateBusLab [OOP v72]";
    int x = 320, y = 180, w = 860, h = 380;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (w < 760) w = 760;
    if (h < 340) h = 340;
    MyGuiIpcRuntimeAttach(ipc);
    child_shared_update(ipc, "statebus-oop-runtime-start", argc, argv, 0);
    WNDCLASSEXA wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = child_statebus_wndproc; wc.lpszClassName = "myOS.OOPStateBusLab";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) {
        child_statebus_wndproc(hwnd, WM_CREATE, 0, 0);
        if (strstr(title, "Publisher") || strstr(title, "PUB")) child_statebus_action(ipc, 0);
        if (strstr(title, "Subscriber") || strstr(title, "SUB")) child_statebus_action(ipc, 1);
        child_statebus_render(ipc, WM_PAINT, "statebus window created");
    }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v72 statebus oop started");
    uint32_t heartbeat = 0;
    while (hwnd && ipc && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) DispatchMessageA(&msg);
        child_shared_update(ipc, "statebus-oop-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v72 statebus heartbeat");
    }
    child_statebus_close_all();
    child_shared_update(ipc, ipc->close_seen ? "statebus-close-seen" : "statebus-exiting", argc, argv, 72);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 72, ipc->close_seen ? "statebus WM_CLOSE exit report" : "statebus exit report");
    return 72;
}




/* v75: OOP SurfaceLab.
   This is the first DWM-lite backing-store producer: the child creates a
   named FileMapping, maps it in its own address space, writes XRGB8888 pixels,
   and publishes only shm metadata/dirty counters through the existing IPC
   diagnostics section. The parent compositor maps that same shm read-only and
   blits the persistent frame. */
#define CHILD_SURFACE_NAME "Global\\myos.v75.surface.lab"
#define CHILD_SURFACE_W 560u
#define CHILD_SURFACE_H 300u

typedef struct ChildSurfaceLab {
    HWND hwnd;
    HANDLE hMap;
    MySurfaceHeader* hdr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t frame;
    uint32_t clicks;
    uint32_t maps;
    uint32_t draws;
    uint32_t spam;
    uint32_t unmaps;
    uint32_t initialized;
    uint32_t duplicateCreates;
    char status[MYOS_IPC_TEXT_MAX];
} ChildSurfaceLab;

static ChildSurfaceLab g_ChildSurface;

static uint32_t child_surface_rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return ((r & 255u) << 16) | ((g & 255u) << 8) | (b & 255u);
}

static size_t child_surface_total_size(uint32_t w, uint32_t h, uint32_t stride)
{
    (void)w;
    return sizeof(MySurfaceHeader) + (size_t)stride * (size_t)h;
}

static void child_surface_publish_shared(ChildIpcContext* ipc, const char* status)
{
    if (!ipc || !ipc->shared) return;
    MyProcessIpcShared* sh = ipc->shared;
    sh->surface_enabled = 1;
    sh->surface_mapped = g_ChildSurface.hdr ? 1u : 0u;
    sh->surface_width = g_ChildSurface.width;
    sh->surface_height = g_ChildSurface.height;
    sh->surface_stride = g_ChildSurface.stride;
    sh->surface_format = MYOS_SURFACE_FORMAT_XRGB8888;
    sh->surface_seq = g_ChildSurface.hdr ? g_ChildSurface.hdr->seqEnd : g_ChildSurface.frame;
    sh->surface_paint_count = g_ChildSurface.draws;
    if (g_ChildSurface.hdr) {
        sh->surface_dirty_left = (uint32_t)g_ChildSurface.hdr->dirtyLeft;
        sh->surface_dirty_top = (uint32_t)g_ChildSurface.hdr->dirtyTop;
        sh->surface_dirty_right = (uint32_t)g_ChildSurface.hdr->dirtyRight;
        sh->surface_dirty_bottom = (uint32_t)g_ChildSurface.hdr->dirtyBottom;
    }
    sh->surface_map_size = (uint32_t)child_surface_total_size(g_ChildSurface.width, g_ChildSurface.height, g_ChildSurface.stride);
    if (sh->kernel_map_name[0]) snprintf(sh->surface_map_name, sizeof(sh->surface_map_name), "%s", sh->kernel_map_name);
    snprintf(sh->surface_status, sizeof(sh->surface_status), "%s", status ? status : g_ChildSurface.status);
}

static int child_surface_map(ChildIpcContext* ipc)
{
    if (g_ChildSurface.hdr) return 1;
    g_ChildSurface.width = CHILD_SURFACE_W;
    g_ChildSurface.height = CHILD_SURFACE_H;
    g_ChildSurface.stride = CHILD_SURFACE_W * 4u;
    DWORD total = (DWORD)child_surface_total_size(g_ChildSurface.width, g_ChildSurface.height, g_ChildSurface.stride);
    if (!g_ChildSurface.hMap) {
        g_ChildSurface.hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, total, CHILD_SURFACE_NAME);
    }
    if (!g_ChildSurface.hMap) {
        snprintf(g_ChildSurface.status, sizeof(g_ChildSurface.status), "CreateFileMapping failed: %.48s", ipc && ipc->shared ? ipc->shared->kernel_status : "no ipc");
        child_surface_publish_shared(ipc, g_ChildSurface.status);
        return 0;
    }
    g_ChildSurface.hdr = (MySurfaceHeader*)MapViewOfFile(g_ChildSurface.hMap, FILE_MAP_READ|FILE_MAP_WRITE, 0, 0, total);
    if (!g_ChildSurface.hdr) {
        snprintf(g_ChildSurface.status, sizeof(g_ChildSurface.status), "MapViewOfFile failed h=0x%x %.42s", (unsigned)g_ChildSurface.hMap, ipc && ipc->shared ? ipc->shared->kernel_status : "");
        child_surface_publish_shared(ipc, g_ChildSurface.status);
        return 0;
    }
    memset(g_ChildSurface.hdr, 0, total);
    g_ChildSurface.hdr->magic = MYOS_SURFACE_MAGIC;
    g_ChildSurface.hdr->version = MYOS_SURFACE_VERSION;
    g_ChildSurface.hdr->width = g_ChildSurface.width;
    g_ChildSurface.hdr->height = g_ChildSurface.height;
    g_ChildSurface.hdr->stride = g_ChildSurface.stride;
    g_ChildSurface.hdr->format = MYOS_SURFACE_FORMAT_XRGB8888;
    g_ChildSurface.hdr->seqBegin = 2;
    g_ChildSurface.hdr->seqEnd = 2;
    g_ChildSurface.hdr->dirtyLeft = 0;
    g_ChildSurface.hdr->dirtyTop = 0;
    g_ChildSurface.hdr->dirtyRight = (int32_t)g_ChildSurface.width;
    g_ChildSurface.hdr->dirtyBottom = (int32_t)g_ChildSurface.height;
    g_ChildSurface.maps++;
    snprintf(g_ChildSurface.status, sizeof(g_ChildSurface.status), "Map OK view=%p shm=%s bytes=%u", (void*)g_ChildSurface.hdr, ipc && ipc->shared ? ipc->shared->kernel_map_name : "", (unsigned)total);
    child_surface_publish_shared(ipc, g_ChildSurface.status);
    return 1;
}

static void child_surface_unmap(ChildIpcContext* ipc)
{
    if (g_ChildSurface.hdr) {
        UnmapViewOfFile(g_ChildSurface.hdr);
        g_ChildSurface.hdr = NULL;
    }
    g_ChildSurface.unmaps++;
    snprintf(g_ChildSurface.status, sizeof(g_ChildSurface.status), "Unmap done; hMap=0x%x maps=%u unmaps=%u", (unsigned)g_ChildSurface.hMap, (unsigned)g_ChildSurface.maps, (unsigned)g_ChildSurface.unmaps);
    if (ipc && ipc->shared) {
        ipc->shared->surface_mapped = 0;
        ipc->shared->surface_map_name[0] = 0;
    }
    child_surface_publish_shared(ipc, g_ChildSurface.status);
}

static void child_surface_draw_pattern(ChildIpcContext* ipc, int mode)
{
    if (!child_surface_map(ipc) || !g_ChildSurface.hdr) return;
    MySurfaceHeader* h = g_ChildSurface.hdr;
    h->seqBegin = h->seqEnd + 1u; /* odd = writer in progress */
    uint8_t* base = ((uint8_t*)h) + sizeof(MySurfaceHeader);
    uint32_t f = ++g_ChildSurface.frame;
    for (uint32_t y = 0; y < h->height; ++y) {
        uint32_t* row = (uint32_t*)(void*)(base + (size_t)y * h->stride);
        for (uint32_t x = 0; x < h->width; ++x) {
            uint32_t r = (x + f * 3u) & 255u;
            uint32_t g = (y * 2u + f * 5u) & 255u;
            uint32_t b = ((x ^ y) + f * 7u) & 255u;
            if (mode == 1) {
                int bx = (int)((x / 56u) % 2u);
                int by = (int)((y / 38u) % 2u);
                r = bx ? 60u : 20u; g = by ? 180u : 70u; b = ((x + y + f * 11u) & 127u) + 80u;
            } else if (mode == 2) {
                r = g = b = 18u;
            }
            row[x] = child_surface_rgb(r, g, b);
        }
    }
    /* A moving persistent rectangle proves we are changing pixels, not just IPC text. */
    int rx = (int)((f * 17u) % (h->width - 90u));
    int ry = 70 + (int)((f * 9u) % 120u);
    for (int yy = ry; yy < ry + 58 && yy < (int)h->height; ++yy) {
        uint32_t* row = (uint32_t*)(void*)(base + (size_t)yy * h->stride);
        for (int xx = rx; xx < rx + 90 && xx < (int)h->width; ++xx) row[xx] = child_surface_rgb(255u, 235u, 90u);
    }
    h->dirtyLeft = 0; h->dirtyTop = 0; h->dirtyRight = (int32_t)h->width; h->dirtyBottom = (int32_t)h->height;
    h->dirtyFlags = 1;
    h->frameSerial = f;
    h->paintSerial++;
    h->seqEnd = h->seqBegin + 1u; /* even stable */
    g_ChildSurface.draws++;
    snprintf(g_ChildSurface.status, sizeof(g_ChildSurface.status), "Draw mode=%d frame=%u seq=%u dirty=full maps=%u", mode, (unsigned)f, (unsigned)h->seqEnd, (unsigned)g_ChildSurface.maps);
    child_surface_publish_shared(ipc, g_ChildSurface.status);
    FlushViewOfFile(h, 0);
}

typedef struct ChildSurfaceButton { int x, y, w, h; const char* label; int id; } ChildSurfaceButton;
static const ChildSurfaceButton g_SurfaceBtns[] = {
    { 12, 14, 78, 22, "Map", 1 },
    { 96, 14, 86, 22, "Gradient", 2 },
    { 188, 14, 72, 22, "Boxes", 3 },
    { 266, 14, 70, 22, "Spam20", 4 },
    { 342, 14, 68, 22, "Clear", 5 },
    { 416, 14, 76, 22, "Unmap", 6 },
};

static void child_surface_render_overlay(ChildIpcContext* ipc, UINT reason)
{
    int cw = ipc && ipc->shared && ipc->shared->gdi_client_w ? (int)ipc->shared->gdi_client_w : 620;
    int ch = ipc && ipc->shared && ipc->shared->gdi_client_h ? (int)ipc->shared->gdi_client_h : 380;
    child_gdi_reset(ipc, cw, ch, reason, "surface overlay");

    /* v75.1: do not let an unmapped SurfaceLab look transparent.
       When a real surface is mapped the parent draws the pixel backing-store
       first and then replays this GDI overlay.  Therefore only fill the full
       client when no surface exists; otherwise fill just the toolbar/status
       strips so the persistent pixels remain visible. */
    if (!g_ChildSurface.hdr) {
        child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(12,17,29));
    } else {
        child_gdi_fill(ipc, 0, 0, cw, 42, CHILD_COLOR(12,17,29));
        child_gdi_fill(ipc, 0, ch - 70, cw, 70, CHILD_COLOR(12,17,29));
    }
    child_gdi_rect(ipc, 6, 6, cw - 12, ch - 12, CHILD_COLOR(120,190,255));
    child_gdi_text(ipc, 12, 44, cw - 24, 16, "SurfaceLab [OOP v75.1] - persistent pixels live in a shared Surface Section", CHILD_COLOR(245,245,255), MYOS_GDI_TEXT_LEFT);
    for (size_t i = 0; i < sizeof(g_SurfaceBtns)/sizeof(g_SurfaceBtns[0]); ++i) {
        const ChildSurfaceButton* b = &g_SurfaceBtns[i];
        child_gdi_fill(ipc, b->x, b->y, b->w, b->h, CHILD_COLOR(28,34,52));
        child_gdi_rect(ipc, b->x, b->y, b->w, b->h, CHILD_COLOR(255,255,255));
        child_gdi_text(ipc, b->x, b->y, b->w, b->h, b->label, CHILD_COLOR(255,255,255), MYOS_GDI_TEXT_CENTER|MYOS_GDI_TEXT_VCENTER);
    }
    char line[224];
    snprintf(line, sizeof(line), "mapped=%s frame=%u seq=%u clicks=%u maps=%u draws=%u spam=%u dupCreate=%u size=%ux%u stride=%u",
             g_ChildSurface.hdr ? "YES" : "NO", (unsigned)g_ChildSurface.frame,
             g_ChildSurface.hdr ? (unsigned)g_ChildSurface.hdr->seqEnd : 0u,
             (unsigned)g_ChildSurface.clicks, (unsigned)g_ChildSurface.maps,
             (unsigned)g_ChildSurface.draws, (unsigned)g_ChildSurface.spam,
             (unsigned)g_ChildSurface.duplicateCreates,
             (unsigned)g_ChildSurface.width, (unsigned)g_ChildSurface.height, (unsigned)g_ChildSurface.stride);
    child_gdi_text_n(ipc, 12, ch - 58, cw - 24, 14, line, sizeof(line), CHILD_COLOR(220,255,220), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, ch - 38, cw - 24, 14, g_ChildSurface.status, sizeof(g_ChildSurface.status), CHILD_COLOR(255,235,170), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, ch - 18, cw - 24, 14, "Test: Map/Gradient, cover with another window, uncover. Pixels persist because parent reads Surface Section, not transient framebuffer.", CHILD_COLOR(235,235,245), MYOS_GDI_TEXT_LEFT);
    if (ipc && ipc->shared) ipc->shared->gdi_sequence++;
}

static LRESULT child_surface_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        /* v75.1: CreateWindowExA may be followed by a queued WM_CREATE after
           the manual bootstrap call in child_surfacelab_main().  v75 reset the
           whole SurfaceLab state on the second WM_CREATE, so the auto-created
           surface was immediately forgotten and the window looked transparent.
           Initialize once and treat later WM_CREATE messages as harmless. */
        if (!g_ChildSurface.initialized) {
            memset(&g_ChildSurface, 0, sizeof(g_ChildSurface));
            g_ChildSurface.initialized = 1;
            g_ChildSurface.hwnd = hwnd;
            g_ChildSurface.width = CHILD_SURFACE_W;
            g_ChildSurface.height = CHILD_SURFACE_H;
            g_ChildSurface.stride = CHILD_SURFACE_W * 4u;
            snprintf(g_ChildSurface.status, sizeof(g_ChildSurface.status), "ready: click Map or Gradient to create persistent surface");
        } else {
            g_ChildSurface.duplicateCreates++;
            if (!g_ChildSurface.hwnd) g_ChildSurface.hwnd = hwnd;
            snprintf(g_ChildSurface.status, sizeof(g_ChildSurface.status), "duplicate WM_CREATE ignored; surface state preserved dup=%u", (unsigned)g_ChildSurface.duplicateCreates);
        }
        if (ipc && ipc->shared) ipc->shared->surface_enabled = 1;
        child_surface_render_overlay(ipc, msg);
        return 0;
    }
    if (msg == WM_PAINT || msg == WM_WINDOWPOSCHANGED) { child_surface_render_overlay(ipc, msg); return 0; }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (msg == WM_LBUTTONUP) {
            g_ChildSurface.clicks++;
            int handled = 0;
            for (size_t i = 0; i < sizeof(g_SurfaceBtns)/sizeof(g_SurfaceBtns[0]); ++i) {
                const ChildSurfaceButton* b = &g_SurfaceBtns[i];
                if (x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h) {
                    handled = 1;
                    if (b->id == 1) child_surface_map(ipc);
                    else if (b->id == 2) child_surface_draw_pattern(ipc, 0);
                    else if (b->id == 3) child_surface_draw_pattern(ipc, 1);
                    else if (b->id == 4) { for (int k = 0; k < 20; ++k) child_surface_draw_pattern(ipc, k & 1); g_ChildSurface.spam++; }
                    else if (b->id == 5) child_surface_draw_pattern(ipc, 2);
                    else if (b->id == 6) child_surface_unmap(ipc);
                    break;
                }
            }
            if (!handled) snprintf(g_ChildSurface.status, sizeof(g_ChildSurface.status), "click outside buttons x=%d y=%d", x, y);
            child_surface_render_overlay(ipc, msg);
        }
        return 0;
    }
    if (msg == WM_CLOSE) {
        child_surface_unmap(ipc);
        if (ipc) ipc->close_seen = 1;
        return 0;
    }
    return 0;
}

static int child_surfacelab_main(int argc, char** argv, ChildIpcContext* ipc)
{
    (void)argc; (void)argv;
    MyGuiIpcRuntimeAttach(ipc);
    const char* title = "SurfaceLab [OOP v75.1]";
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    child_shared_update(ipc, "surfacelab-oop-runtime-start", argc, argv, 0);
    WNDCLASSEXA wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = child_surface_wndproc; wc.lpszClassName = "myOS.OOPSurfaceLab";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, title, WS_VISIBLE, 120, 110, 640, 400, 0, 0, 0, NULL);
    if (hwnd) {
        child_surface_wndproc(hwnd, WM_CREATE, 0, 0);
        child_surface_draw_pattern(ipc, 0); /* Start with visible proof pixels. */
        child_surface_render_overlay(ipc, WM_PAINT);
    }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v75.1 surfacelab backing-store started");
    uint32_t heartbeat = 0;
    while (!ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) DispatchMessageA(&msg);
        if ((heartbeat++ % 18u) == 0u) child_surface_render_overlay(ipc, WM_PAINT);
        child_shared_update(ipc, "surfacelab-oop-message-loop", argc, argv, 0);
        if ((heartbeat % 36u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v75.1 surfacelab heartbeat");
    }
    child_surface_unmap(ipc);
    child_shared_update(ipc, "surfacelab-close-seen", argc, argv, 75);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 75, "surfacelab exit report");
    (void)atom;
    return 751;
}

/* v74: OOP HWND StateProbe.
   The parent mirrors HWNDManager::state_section into a real named FileMapping.
   This child maps that section itself and reads current HWND state without any
   parent pointer or per-window payload messages. */
#define CHILD_WSTS_MAGIC 0x57535453u
#define CHILD_WSTS_LAYOUT_VERSION 74u
#define CHILD_WSTS_SECTION_NAME "Global\\myos.v74.hwnd.state.section"
#define CHILD_WSTS_CAPACITY 64u

#define CHILD_WS_VISIBLE   0x00000001u
#define CHILD_WS_MINIMIZED 0x00000002u
#define CHILD_WS_ACTIVE    0x00000004u
#define CHILD_WS_DESTROYED 0x00000008u

#define CHILD_WSD_RECT     0x00000001u
#define CHILD_WSD_STYLE    0x00000002u
#define CHILD_WSD_TEXT     0x00000004u
#define CHILD_WSD_VISIBLE  0x00000008u
#define CHILD_WSD_FOCUS    0x00000010u
#define CHILD_WSD_ZORDER   0x00000020u
#define CHILD_WSD_DESTROY  0x00000040u
#define CHILD_WSD_OWNER    0x00000080u

typedef struct ChildWindowState {
    DWORD cbSize;
    DWORD seqBegin;
    HWND  hWnd;
    DWORD ownerPid;
    DWORD ownerTid;
    RECT  rcWindow;
    RECT  rcClient;
    BOOL  visible;
    BOOL  minimized;
    BOOL  active;
    BOOL  focused;
    BOOL  enabled;
    BOOL  hasCapture;
    BOOL  destroyed;
    DWORD flags;
    DWORD dirtyFlags;
    DWORD zOrder;
    DWORD style;
    DWORD exStyle;
    DWORD stateVersion;
    DWORD updateSerial;
    UINT  lastMessage;
    char  szTitle[64];
    DWORD seqEnd;
} ChildWindowState;

typedef struct ChildWindowStateSection {
    DWORD cbSize;
    DWORD magic;
    DWORD version;
    DWORD capacity;
    DWORD activeCount;
    DWORD destroyedCount;
    DWORD updateSerial;
    char  sectionName[96];
    ChildWindowState states[CHILD_WSTS_CAPACITY];
} ChildWindowStateSection;

typedef struct ChildHwndStateProbe {
    HWND hwnd;
    HANDLE hMap;
    ChildWindowStateSection* view;
    uint32_t refreshes;
    uint32_t stableReads;
    uint32_t liveCount;
    uint32_t deadCount;
    uint32_t selectedIndex;
    uint32_t clicks;
    uint32_t lastAction;
    uint32_t maps;
    uint32_t closeMaps;
    uint32_t paints;
    uint32_t autoRefresh;
    uint32_t subscribed;
    uint32_t subscribeReqs;
    uint32_t unsubscribeReqs;
    uint32_t dirtyMsgs;
    uint32_t subscribeAcks;
    uint32_t lastDirtySource;
    uint32_t lastDirtySerial;
    uint32_t lastViewSerial;
    int lastMouseX;
    int lastMouseY;
    char status[192];
    char lastActionText[96];
} ChildHwndStateProbe;

static ChildHwndStateProbe g_ChildHwndState;

static void child_hwndstate_close_all(void)
{
    if (g_ChildHwndState.view) { UnmapViewOfFile(g_ChildHwndState.view); g_ChildHwndState.view = NULL; }
    if (g_ChildHwndState.hMap) { CloseHandle(g_ChildHwndState.hMap); g_ChildHwndState.hMap = 0; }
}

static int child_hwndstate_ensure_map(void)
{
    int hadView = g_ChildHwndState.view ? 1 : 0;
    if (!g_ChildHwndState.hMap)
        g_ChildHwndState.hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, CHILD_WSTS_SECTION_NAME);
    if (!g_ChildHwndState.hMap) {
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "Map failed: OpenFileMapping(%s)", CHILD_WSTS_SECTION_NAME);
        return 0;
    }
    if (!g_ChildHwndState.view)
        g_ChildHwndState.view = (ChildWindowStateSection*)MapViewOfFile(g_ChildHwndState.hMap, FILE_MAP_READ, 0, 0, sizeof(ChildWindowStateSection));
    if (!g_ChildHwndState.view) {
        MyProcessIpcShared* sh = g_GuiIpcRuntime ? g_GuiIpcRuntime->shared : NULL;
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status),
                 "Map failed: h=0x%x ok=%u err=%u shm=%s bytes=%u",
                 (unsigned)g_ChildHwndState.hMap,
                 sh ? (unsigned)sh->kernel_ok : 0u,
                 sh ? (unsigned)sh->kernel_error : 0u,
                 (sh && sh->kernel_map_name[0]) ? sh->kernel_map_name : "-",
                 sh ? (unsigned)sh->kernel_map_size : 0u);
        return 0;
    }
    if (g_ChildHwndState.view->magic != CHILD_WSTS_MAGIC) {
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "Map failed: WSTS magic mismatch: 0x%x", (unsigned)g_ChildHwndState.view->magic);
        return 0;
    }
    if (!hadView) g_ChildHwndState.maps++;
    snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "Map OK #%u: view=%p seq=%u active=%u dead=%u",
             (unsigned)g_ChildHwndState.maps, (void*)g_ChildHwndState.view,
             (unsigned)g_ChildHwndState.view->updateSerial,
             (unsigned)g_ChildHwndState.view->activeCount,
             (unsigned)g_ChildHwndState.view->destroyedCount);
    return 1;
}

static int child_hwndstate_copy_slot(uint32_t idx, ChildWindowState* out)
{
    if (!out || !g_ChildHwndState.view || idx >= g_ChildHwndState.view->capacity || idx >= CHILD_WSTS_CAPACITY) return 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint32_t a = g_ChildHwndState.view->states[idx].seqBegin;
        ChildWindowState tmp = g_ChildHwndState.view->states[idx];
        uint32_t b = g_ChildHwndState.view->states[idx].seqEnd;
        if (a == b && ((a & 1u) == 0u)) {
            *out = tmp;
            return tmp.hWnd ? 1 : 0;
        }
    }
    return 0;
}

static int child_hwndstate_refresh_counts_mapped(void)
{
    g_ChildHwndState.refreshes++;
    g_ChildHwndState.liveCount = 0;
    g_ChildHwndState.deadCount = 0;
    g_ChildHwndState.stableReads = 0;
    if (!g_ChildHwndState.view) return 0;
    if (g_ChildHwndState.view->magic != CHILD_WSTS_MAGIC) return 0;
    uint32_t cap = g_ChildHwndState.view->capacity;
    if (cap > CHILD_WSTS_CAPACITY) cap = CHILD_WSTS_CAPACITY;
    for (uint32_t i = 0; i < cap; ++i) {
        ChildWindowState st;
        if (!child_hwndstate_copy_slot(i, &st)) continue;
        g_ChildHwndState.stableReads++;
        if (st.destroyed || (st.flags & CHILD_WS_DESTROYED)) g_ChildHwndState.deadCount++;
        else g_ChildHwndState.liveCount++;
    }
    return 1;
}

static int child_hwndstate_find_window(HWND hwnd, ChildWindowState* out)
{
    if (!hwnd || !out) return 0;
    if (!child_hwndstate_ensure_map()) return 0;
    if (!g_ChildHwndState.view || g_ChildHwndState.view->magic != CHILD_WSTS_MAGIC) return 0;
    uint32_t cap = g_ChildHwndState.view->capacity;
    if (cap > CHILD_WSTS_CAPACITY) cap = CHILD_WSTS_CAPACITY;
    for (uint32_t i = 0; i < cap; ++i) {
        ChildWindowState st;
        if (!child_hwndstate_copy_slot(i, &st)) continue;
        if (st.hWnd != hwnd) continue;
        if (st.destroyed || (st.flags & CHILD_WS_DESTROYED)) return 0;
        *out = st;
        return 1;
    }
    return 0;
}

static void child_hwnd_client_size(HWND hwnd, int fallbackWinW, int fallbackWinH, int minCw, int minCh, int* outCw, int* outCh)
{
    int cw = fallbackWinW > 2 ? fallbackWinW - 2 : fallbackWinW;
    int ch = fallbackWinH > 25 ? fallbackWinH - 25 : fallbackWinH;
    ChildWindowState st;
    if (child_hwndstate_find_window(hwnd, &st)) {
        int w = st.rcClient.right - st.rcClient.left;
        int h = st.rcClient.bottom - st.rcClient.top;
        if (w > 0) cw = w;
        if (h > 0) ch = h;
    }
    if (cw < minCw) cw = minCw;
    if (ch < minCh) ch = minCh;
    if (outCw) *outCw = cw;
    if (outCh) *outCh = ch;
}

static void child_hwndstate_render(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    g_ChildHwndState.paints++;
    if (g_ChildHwndState.autoRefresh && g_ChildHwndState.view)
        (void)child_hwndstate_refresh_counts_mapped();
    int win_w = (int)ipc->shared->gui_w;
    int win_h = (int)ipc->shared->gui_h;
    int cw = win_w - 2;
    int ch = win_h - 24 - 1;
    if (cw < 680) cw = 680;
    if (ch < 330) ch = 330;
    child_gdi_reset(ipc, cw, ch, reason, status ? status : "hwndstate render");
    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(16,22,36));
    child_gdi_rect(ipc, 6, 6, cw - 12, ch - 12, CHILD_COLOR(70,105,155));
    child_gdi_text(ipc, 12, 10, cw - 24, 16, "HWND StateProbe [OOP v74] - live dirty notify + manual diagnostics", CHILD_COLOR(185,235,255), MYOS_GDI_TEXT_LEFT);
    ChildMiniRect b[6]; child_button_rects(b, 6, 12, 36, 92, 24, 8);
    child_draw_button(ipc, b[0], g_ChildHwndState.view ? "Map OK" : "Map");
    child_draw_button(ipc, b[1], "Refresh");
    child_draw_button(ipc, b[2], "CloseMap");
    child_draw_button(ipc, b[3], g_ChildHwndState.autoRefresh ? "Auto ON" : "Auto OFF");
    child_draw_button(ipc, b[4], g_ChildHwndState.subscribed ? "Sub OK" : "Subscribe");
    child_draw_button(ipc, b[5], "Unsub");

    char line[320];
    const char* mapped = g_ChildHwndState.view ? "YES" : "NO";
    snprintf(line, sizeof(line),
             "mapped=%s sub=%s hMap=0x%x view=%p pid=%u/%ld hwnd=%u clicks=%u dirty=%u src=%u serial=%u viewSeq=%u maps=%u closes=%u refresh=%u paints=%u auto=%u",
             mapped, g_ChildHwndState.subscribed ? "YES" : "NO",
             (unsigned)g_ChildHwndState.hMap, (void*)g_ChildHwndState.view,
             ipc->myPid, (long)getpid(), (unsigned)g_ChildHwndState.hwnd,
             (unsigned)g_ChildHwndState.clicks, (unsigned)g_ChildHwndState.dirtyMsgs,
             (unsigned)g_ChildHwndState.lastDirtySource, (unsigned)g_ChildHwndState.lastDirtySerial,
             (unsigned)g_ChildHwndState.lastViewSerial,
             (unsigned)g_ChildHwndState.maps, (unsigned)g_ChildHwndState.closeMaps,
             (unsigned)g_ChildHwndState.refreshes, (unsigned)g_ChildHwndState.paints,
             (unsigned)g_ChildHwndState.autoRefresh);
    child_gdi_text_n(ipc, 12, 70, cw - 24, 14, line, sizeof(line), CHILD_COLOR(230,230,245), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, 92, cw - 24, 14, g_ChildHwndState.lastActionText[0] ? g_ChildHwndState.lastActionText : "lastAction=<none yet>", sizeof(g_ChildHwndState.lastActionText), CHILD_COLOR(210,255,210), MYOS_GDI_TEXT_LEFT);
    child_gdi_text_n(ipc, 12, 114, cw - 24, 14, g_ChildHwndState.status, sizeof(g_ChildHwndState.status), CHILD_COLOR(255,230,165), MYOS_GDI_TEXT_LEFT);
    if (ipc->shared) child_gdi_text_n(ipc, 12, 136, cw - 24, 14, ipc->shared->kernel_status, sizeof(ipc->shared->kernel_status), CHILD_COLOR(190,215,255), MYOS_GDI_TEXT_LEFT);

    if (g_ChildHwndState.view && g_ChildHwndState.view->magic == CHILD_WSTS_MAGIC) {
        snprintf(line, sizeof(line), "section: name=%s seq=%u layout=%u active=%u dead=%u stable=%u capacity=%u",
                 g_ChildHwndState.view->sectionName,
                 (unsigned)g_ChildHwndState.view->updateSerial,
                 (unsigned)g_ChildHwndState.view->version,
                 (unsigned)g_ChildHwndState.view->activeCount,
                 (unsigned)g_ChildHwndState.view->destroyedCount,
                 (unsigned)g_ChildHwndState.stableReads,
                 (unsigned)g_ChildHwndState.view->capacity);
    } else {
        snprintf(line, sizeof(line), "section: not mapped - click Map. Subscribe will map once so DirtyNotify can refresh live.");
    }
    child_gdi_text_n(ipc, 12, 158, cw - 24, 14, line, sizeof(line), CHILD_COLOR(235,235,245), MYOS_GDI_TEXT_LEFT);

    child_gdi_text(ipc, 12, 184, cw - 24, 14, "slot hwnd pid/tid  rect              flags dirty seq msg title", CHILD_COLOR(160,220,255), MYOS_GDI_TEXT_LEFT);
    int y = 204;
    uint32_t shown = 0;
    if (g_ChildHwndState.view && g_ChildHwndState.view->magic == CHILD_WSTS_MAGIC) {
        uint32_t cap = g_ChildHwndState.view->capacity;
        if (cap > CHILD_WSTS_CAPACITY) cap = CHILD_WSTS_CAPACITY;
        for (uint32_t i = 0; i < cap && shown < 13u; ++i) {
            ChildWindowState st;
            if (!child_hwndstate_copy_slot(i, &st)) continue;
            if (!st.hWnd) continue;
            snprintf(line, sizeof(line), "%02u   %3u  %4u/%-4u %4d,%3d %4dx%-4d 0x%02x 0x%02x  %4u 0x%04x %.54s",
                     (unsigned)i, (unsigned)st.hWnd, (unsigned)st.ownerPid, (unsigned)st.ownerTid,
                     st.rcWindow.left, st.rcWindow.top,
                     st.rcWindow.right - st.rcWindow.left,
                     st.rcWindow.bottom - st.rcWindow.top,
                     (unsigned)st.flags, (unsigned)st.dirtyFlags,
                     (unsigned)st.stateVersion, (unsigned)st.lastMessage,
                     st.szTitle[0] ? st.szTitle : "<untitled>");
            uint32_t col = (st.flags & CHILD_WS_DESTROYED) ? CHILD_COLOR(190,150,150) : ((st.flags & CHILD_WS_ACTIVE) ? CHILD_COLOR(175,255,190) : CHILD_COLOR(235,245,255));
            child_gdi_text_n(ipc, 12, y, cw - 24, 12, line, sizeof(line), col, MYOS_GDI_TEXT_LEFT);
            y += 16;
            shown++;
        }
    }
    if (!shown) child_gdi_text(ipc, 12, y, cw - 24, 14, "No stable HWND state entries visible yet.", CHILD_COLOR(255,190,160), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, ch - 50, cw - 24, 14, "Test A: Map -> Subscribe, then move/focus/open/close any window. dirty/src/serial and table must update without Refresh.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, ch - 34, cw - 24, 14, "Test B: Unsub stops dirty counter. CloseMap clears table; Subscribe maps again and resumes live updates.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 12, ch - 18, cw - 24, 14, "v74: global HWND-state subscriber; MessageQueue carries only WM_MYOS_HWND_STATE_DIRTY, payload lives in WSTS section.", CHILD_COLOR(220,220,235), MYOS_GDI_TEXT_LEFT);
}

static void child_hwndstate_action(ChildIpcContext* ipc, int action)
{
    g_ChildHwndState.clicks++;
    g_ChildHwndState.lastAction = (uint32_t)action;
    if (action == 0) {
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=Map clicked #%u", (unsigned)g_ChildHwndState.clicks);
        if (child_hwndstate_ensure_map()) (void)child_hwndstate_refresh_counts_mapped();
    }
    else if (action == 1) {
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=Refresh clicked #%u", (unsigned)g_ChildHwndState.clicks);
        if (g_ChildHwndState.view) {
            (void)child_hwndstate_refresh_counts_mapped();
            snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "Refresh OK #%u: stable=%u live=%u dead=%u seq=%u",
                     (unsigned)g_ChildHwndState.refreshes, (unsigned)g_ChildHwndState.stableReads,
                     (unsigned)g_ChildHwndState.liveCount, (unsigned)g_ChildHwndState.deadCount,
                     (unsigned)g_ChildHwndState.view->updateSerial);
        } else {
            snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "Refresh clicked while unmapped: OK, no auto-map in v74");
        }
    }
    else if (action == 2) {
        child_hwndstate_close_all();
        g_ChildHwndState.closeMaps++;
        g_ChildHwndState.liveCount = 0;
        g_ChildHwndState.deadCount = 0;
        g_ChildHwndState.stableReads = 0;
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=CloseMap clicked #%u", (unsigned)g_ChildHwndState.clicks);
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "CloseMap OK #%u: mapped=NO, handle closed, view cleared", (unsigned)g_ChildHwndState.closeMaps);
    }
    else if (action == 3) {
        g_ChildHwndState.autoRefresh = !g_ChildHwndState.autoRefresh;
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=Auto toggled #%u -> %s", (unsigned)g_ChildHwndState.clicks, g_ChildHwndState.autoRefresh ? "ON" : "OFF");
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "AutoRefresh %s", g_ChildHwndState.autoRefresh ? "ON" : "OFF");
    }
    else if (action == 4) {
        g_ChildHwndState.subscribeReqs++;
        if (!g_ChildHwndState.view) (void)child_hwndstate_ensure_map();
        if (g_ChildHwndState.view) (void)child_hwndstate_refresh_counts_mapped();
        child_post_message(ipc, g_ChildHwndState.hwnd, WM_MYOS_HWND_STATE_SUBSCRIBE_REQ, 0, 0, "v74 HWND StateProbe global WSTS subscribe request");
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=Subscribe request #%u", (unsigned)g_ChildHwndState.subscribeReqs);
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "Subscribe request sent: waiting for WM_MYOS_SUBSCRIBED/dirty notify");
    }
    else if (action == 5) {
        g_ChildHwndState.unsubscribeReqs++;
        child_post_message(ipc, g_ChildHwndState.hwnd, WM_MYOS_HWND_STATE_UNSUBSCRIBE_REQ, 0, 0, "v74 HWND StateProbe global WSTS unsubscribe request");
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=Unsubscribe request #%u", (unsigned)g_ChildHwndState.unsubscribeReqs);
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "Unsubscribe request sent");
    }
    child_hwndstate_render(ipc, WM_PAINT, g_ChildHwndState.status);
}

static LRESULT child_hwndstate_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        memset(&g_ChildHwndState, 0, sizeof(g_ChildHwndState));
        g_ChildHwndState.hwnd = hwnd;
        g_ChildHwndState.autoRefresh = 0;
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "ready - Map then Subscribe; live dirty notify will update without Refresh");
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=<none yet>");
        child_hwndstate_render(ipc, msg, "hwndstate create");
        return 0;
    }
    if (msg == WM_MYOS_SUBSCRIBED) {
        g_ChildHwndState.subscribeAcks++;
        g_ChildHwndState.subscribed = wp ? 1u : 0u;
        g_ChildHwndState.lastDirtySerial = (uint32_t)lp;
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=WM_MYOS_SUBSCRIBED ack #%u -> %s", (unsigned)g_ChildHwndState.subscribeAcks, g_ChildHwndState.subscribed ? "subscribed" : "unsubscribed");
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "Subscribe ACK: subscribed=%u sectionSerial=%u", (unsigned)g_ChildHwndState.subscribed, (unsigned)g_ChildHwndState.lastDirtySerial);
        if (g_ChildHwndState.view) (void)child_hwndstate_refresh_counts_mapped();
        child_hwndstate_render(ipc, msg, g_ChildHwndState.status);
        return 0;
    }
    if (msg == WM_MYOS_HWND_STATE_DIRTY) {
        g_ChildHwndState.dirtyMsgs++;
        g_ChildHwndState.lastDirtySource = (uint32_t)wp;
        g_ChildHwndState.lastDirtySerial = (uint32_t)lp;
        if (!g_ChildHwndState.view) (void)child_hwndstate_ensure_map();
        if (g_ChildHwndState.view) {
            (void)child_hwndstate_refresh_counts_mapped();
            g_ChildHwndState.lastViewSerial = g_ChildHwndState.view->updateSerial;
        }
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=DIRTY msg #%u source=%u serial=%u", (unsigned)g_ChildHwndState.dirtyMsgs, (unsigned)g_ChildHwndState.lastDirtySource, (unsigned)g_ChildHwndState.lastDirtySerial);
        snprintf(g_ChildHwndState.status, sizeof(g_ChildHwndState.status), "Live dirty notify: source=%u serial=%u stable=%u live=%u dead=%u", (unsigned)g_ChildHwndState.lastDirtySource, (unsigned)g_ChildHwndState.lastDirtySerial, (unsigned)g_ChildHwndState.stableReads, (unsigned)g_ChildHwndState.liveCount, (unsigned)g_ChildHwndState.deadCount);
        child_hwndstate_render(ipc, msg, g_ChildHwndState.status);
        return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_PAINT) { child_hwndstate_render(ipc, msg, g_ChildHwndState.status); return 0; }
    if (msg == WM_LBUTTONUP) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        g_ChildHwndState.lastMouseX = x;
        g_ChildHwndState.lastMouseY = y;
        ChildMiniRect b[6]; child_button_rects(b, 6, 12, 36, 92, 24, 8);
        for (int i = 0; i < 6; ++i) if (child_rect_hit(&b[i], x, y)) { child_hwndstate_action(ipc, i); return 0; }
        snprintf(g_ChildHwndState.lastActionText, sizeof(g_ChildHwndState.lastActionText), "lastAction=client click outside buttons at %d,%d", x, y);
        child_hwndstate_render(ipc, WM_PAINT, g_ChildHwndState.status);
    }
    if (msg == WM_CLOSE) { if (ipc) ipc->close_seen = 1; child_hwndstate_close_all(); child_hwndstate_render(ipc, msg, "hwndstate close"); return 0; }
    return 0;
}

static int child_hwndstate_main(int argc, char** argv, ChildIpcContext* ipc)
{
    (void)argc; (void)argv;
    MyGuiIpcRuntimeAttach(ipc);
    const char* title = "HWND StateProbe [OOP v74]";
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    child_shared_update(ipc, "hwndstate-oop-runtime-start", argc, argv, 0);
    WNDCLASSEXA wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = child_hwndstate_wndproc; wc.lpszClassName = "myOS.OOPHwndStateProbe";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, title, WS_VISIBLE, 80, 80, 900, 430, 0, 0, 0, NULL);
    if (hwnd) {
        child_hwndstate_wndproc(hwnd, WM_CREATE, 0, 0);
        child_hwndstate_render(ipc, WM_PAINT, "hwndstate window created - Map then Subscribe for live dirty notify");
    }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v74 hwndstate live dirty notify oop started");
    uint32_t heartbeat = 0;
    while (!ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) DispatchMessageA(&msg);
        if ((heartbeat++ % 12u) == 0u) child_hwndstate_render(ipc, WM_PAINT, g_ChildHwndState.status);
        child_shared_update(ipc, "hwndstate-oop-message-loop", argc, argv, 0);
        if ((heartbeat % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v74 hwndstate live heartbeat");
    }
    child_hwndstate_close_all();
    child_shared_update(ipc, ipc->close_seen ? "hwndstate-close-seen" : "hwndstate-exiting", argc, argv, 73);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 73, ipc->close_seen ? "hwndstate WM_CLOSE exit report" : "hwndstate exit report");
    (void)atom;
    return 731;
}



/* v167: OOP DialogLab / modal-isolation scaffold.
   The first step is deliberately architectural: dialog-lab is now a real
   fork/exec GUI child.  The child owns the WndProc, command state and GDI
   rendering; the parent only brokers HWND creation, input and painting.  The
   classic in-process DLGTEMPLATE DialogLab remains available as
   dialog-lab-classic while this child grows toward the full DialogBoxParamA /
   CreateDialogParamA contract. */
#define CHILD_DLAB_CMD_MODAL      0x8801u
#define CHILD_DLAB_CMD_MODELESS   0x8802u
#define CHILD_DLAB_CMD_DUMP       0x8803u
#define CHILD_DLAB_CMD_CONTROLS   0x8804u
#define CHILD_DLAB_CMD_BUTTONS    0x8805u
#define CHILD_DLAB_CMD_TEXT       0x8806u
#define CHILD_DLAB_CMD_KEYBOARD   0x8807u
#define CHILD_DLAB_CMD_SCROLLSTD  0x8808u
#define CHILD_DLAB_CMD_MENU       0x8809u
#define CHILD_DLAB_CMD_DUMPNAV    0x880Au
#define CHILD_DLAB_CMD_OPENFILE   0x880Bu
#define CHILD_DLAB_CMD_SAVEFILE   0x880Cu
#define CHILD_DLAB_CMD_CHOOSEFONT 0x880Du
#define CHILD_DLAB_MAX_BUTTONS 13

typedef struct ChildDialogButtonDef {
    uint16_t id;
    const char* text;
    int x, y, w, h;
} ChildDialogButtonDef;

static const ChildDialogButtonDef g_ChildDialogButtons[CHILD_DLAB_MAX_BUTTONS] = {
    { CHILD_DLAB_CMD_MODAL,      "Open Modal Dialog",    14,  12, 160, 24 },
    { CHILD_DLAB_CMD_MODELESS,   "Open Modeless Dialog", 184, 12, 176, 24 },
    { CHILD_DLAB_CMD_DUMP,       "Dump Template",        370, 12, 130, 24 },
    { CHILD_DLAB_CMD_CONTROLS,   "Open Controls Dialog", 510, 12, 170, 24 },
    { CHILD_DLAB_CMD_BUTTONS,    "Open Button Dialog",   690, 12, 160, 24 },
    { CHILD_DLAB_CMD_TEXT,       "Open Text Dialog",     14,  42, 160, 24 },
    { CHILD_DLAB_CMD_KEYBOARD,   "Open Keyboard Dialog", 184, 42, 176, 24 },
    { CHILD_DLAB_CMD_SCROLLSTD,  "Enable Std Scrollbars",370, 42, 180, 24 },
    { CHILD_DLAB_CMD_MENU,       "Menu APIs Probe",      560, 42, 170, 24 },
    { CHILD_DLAB_CMD_DUMPNAV,    "Dump Dialog Nav",      740, 42, 160, 24 },
    { CHILD_DLAB_CMD_OPENFILE,   "GetOpenFileNameA",     14,  72, 200, 24 },
    { CHILD_DLAB_CMD_SAVEFILE,   "GetSaveFileNameA",     224, 72, 200, 24 },
    { CHILD_DLAB_CMD_CHOOSEFONT, "ChooseFontA",          434, 72, 160, 24 },
};

#define CHILD_DLAB_MAX_DIALOGS 8
#define CHILD_DLAB_ID_BASE      0x8900u
#define CHILD_DLAB_ID_OK_BASE   0x8A00u
#define CHILD_DLAB_ID_CAN_BASE  0x8B00u
#define CHILD_DLAB_ID_CLOSE_BASE 0x8C00u

typedef struct ChildDialogProbe {
    HWND hwnd;
    HWND text;
    HWND ok;
    HWND cancel;
    int winW;
    int winH;
    uint16_t command;
    uint16_t result;
    uint8_t modal;
    uint8_t active;
    char title[96];
} ChildDialogProbe;

typedef struct ChildDialogLabState {
    HWND hwnd;
    int winW;
    int winH;
    uint16_t lastModalResult;
    HWND buttons[CHILD_DLAB_MAX_BUTTONS];
    ChildDialogProbe probes[CHILD_DLAB_MAX_DIALOGS];
    uint32_t commands;
    uint32_t clicks;
    uint32_t modalCount;
    uint32_t modelessCount;
    uint32_t navDumps;
    uint32_t pseudoDialogsOpen;
    uint32_t realDialogsOpened;
    uint32_t realDialogsClosed;
    uint32_t tabLoopCanary;
    uint16_t lastCommand;
    char lastResult[96];
    char status[192];
    char dump[4][180];
} ChildDialogLabState;

static ChildDialogLabState g_ChildDialogLab;

static const char* child_dialog_cmd_name(uint16_t id)
{
    switch (id) {
    case CHILD_DLAB_CMD_MODAL: return "Open Modal Dialog";
    case CHILD_DLAB_CMD_MODELESS: return "Open Modeless Dialog";
    case CHILD_DLAB_CMD_DUMP: return "Dump Template";
    case CHILD_DLAB_CMD_CONTROLS: return "Open Controls Dialog";
    case CHILD_DLAB_CMD_BUTTONS: return "Open Button Dialog";
    case CHILD_DLAB_CMD_TEXT: return "Open Text Dialog";
    case CHILD_DLAB_CMD_KEYBOARD: return "Open Keyboard Dialog";
    case CHILD_DLAB_CMD_SCROLLSTD: return "Enable Std Scrollbars";
    case CHILD_DLAB_CMD_MENU: return "Menu APIs Probe";
    case CHILD_DLAB_CMD_DUMPNAV: return "Dump Dialog Nav";
    case CHILD_DLAB_CMD_OPENFILE: return "GetOpenFileNameA";
    case CHILD_DLAB_CMD_SAVEFILE: return "GetSaveFileNameA";
    case CHILD_DLAB_CMD_CHOOSEFONT: return "ChooseFontA";
    default: return "unknown";
    }
}

static uint16_t child_dialog_id_from_hwnd(HWND hwnd)
{
    for (int i = 0; i < CHILD_DLAB_MAX_BUTTONS; ++i)
        if (g_ChildDialogLab.buttons[i] == hwnd) return g_ChildDialogButtons[i].id;
    return 0;
}

static BOOL child_dialog_is_command_button_id(uint16_t id)
{
    for (int i = 0; i < CHILD_DLAB_MAX_BUTTONS; ++i)
        if (g_ChildDialogButtons[i].id == id) return TRUE;
    return FALSE;
}

static BOOL child_dialog_command_is_activation(WPARAM wp)
{
    /* Win32 BUTTON controls send WM_COMMAND for focus transitions too:
       LOWORD=id, HIWORD=BN_SETFOCUS/BN_KILLFOCUS, lParam=button HWND.
       DialogLab commands must fire only for real activation paths:
       BN_CLICKED (0) or menu/accelerator-style code 0.  v170 treated every
       notification as activation, so a physical click generated one modal on
       BN_SETFOCUS and a second modal on BN_CLICKED. */
    return HIWORD(wp) == BN_CLICKED ? TRUE : FALSE;
}

static int child_dialog_probe_index_from_hwnd(HWND hwnd)
{
    if (!hwnd) return -1;
    for (int i = 0; i < CHILD_DLAB_MAX_DIALOGS; ++i) {
        ChildDialogProbe* p = &g_ChildDialogLab.probes[i];
        if (!p->active) continue;
        if (p->hwnd == hwnd || p->ok == hwnd || p->cancel == hwnd || p->text == hwnd) return i;
    }
    return -1;
}

static int child_dialog_probe_button_role(HWND hwnd, int* outIndex)
{
    for (int i = 0; i < CHILD_DLAB_MAX_DIALOGS; ++i) {
        ChildDialogProbe* p = &g_ChildDialogLab.probes[i];
        if (!p->active) continue;
        if (p->ok == hwnd) { if (outIndex) *outIndex = i; return IDOK; }
        if (p->cancel == hwnd) { if (outIndex) *outIndex = i; return IDCANCEL; }
    }
    if (outIndex) *outIndex = -1;
    return 0;
}

static void child_dialog_dump_template(void)
{
    snprintf(g_ChildDialogLab.dump[0], sizeof(g_ChildDialogLab.dump[0]),
             "OOP DialogLab: top HWND=%u child buttons=%u tabstops=%u", (unsigned)g_ChildDialogLab.hwnd,
             (unsigned)CHILD_DLAB_MAX_BUTTONS, (unsigned)CHILD_DLAB_MAX_BUTTONS);
    snprintf(g_ChildDialogLab.dump[1], sizeof(g_ChildDialogLab.dump[1]),
             "v180: DialogLab child-HWND batch create; scoped damage + retained cache.");
    snprintf(g_ChildDialogLab.dump[2], sizeof(g_ChildDialogLab.dump[2]),
             "Modal owner is disabled/enabled through ProcessHost EnableWindow IPC; modeless stays independent.");
    snprintf(g_ChildDialogLab.dump[3], sizeof(g_ChildDialogLab.dump[3]),
             "WS_TABSTOP is set on command and dialog buttons, so tab walk remains circular and never falls through to IDOK.");
}

static void child_dialoglab_render(ChildIpcContext* ipc, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    int fallbackW = g_ChildDialogLab.winW > 0 ? g_ChildDialogLab.winW : 920;
    int fallbackH = g_ChildDialogLab.winH > 0 ? g_ChildDialogLab.winH : 320;
    int cw = 0, ch = 0;
    child_hwnd_client_size(g_ChildDialogLab.hwnd, fallbackW, fallbackH, 760, 260, &cw, &ch);
    child_gdi_reset_for_hwnd(ipc, g_ChildDialogLab.hwnd, cw, ch, reason, status ? status : "DialogLab OOP render");
    child_gdi_fill(ipc, 0, 0, cw, ch, CHILD_COLOR(12,14,22));
    child_gdi_rect(ipc, 0, 0, cw, ch, CHILD_COLOR(65,75,105));
    child_gdi_text(ipc, 14, 104, cw - 28, 14, "DialogLab [OOP v180] - child-HWND batch create + backing cache", CHILD_COLOR(190,235,210), MYOS_GDI_TEXT_LEFT);

    char line[320];
    snprintf(line, sizeof(line), "myPID=%u linuxPID=%ld top=%u commands=%u clicks=%u modal=%u modeless=%u open=%u real=%u/%u tabCanary=%u",
             ipc->myPid, (long)getpid(), (unsigned)g_ChildDialogLab.hwnd,
             (unsigned)g_ChildDialogLab.commands, (unsigned)g_ChildDialogLab.clicks,
             (unsigned)g_ChildDialogLab.modalCount, (unsigned)g_ChildDialogLab.modelessCount,
             (unsigned)g_ChildDialogLab.pseudoDialogsOpen, (unsigned)g_ChildDialogLab.realDialogsOpened,
             (unsigned)g_ChildDialogLab.realDialogsClosed, (unsigned)g_ChildDialogLab.tabLoopCanary);
    child_gdi_text_n(ipc, 14, 126, cw - 28, 14, line, sizeof(line), CHILD_COLOR(210,190,255), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "status: %s", g_ChildDialogLab.status[0] ? g_ChildDialogLab.status : "ready");
    child_gdi_text_n(ipc, 14, 148, cw - 28, 14, line, sizeof(line), CHILD_COLOR(170,210,255), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "last result: %s   last command=0x%04x (%s)",
             g_ChildDialogLab.lastResult[0] ? g_ChildDialogLab.lastResult : "<none>",
             (unsigned)g_ChildDialogLab.lastCommand, child_dialog_cmd_name(g_ChildDialogLab.lastCommand));
    child_gdi_text_n(ipc, 14, 170, cw - 28, 14, line, sizeof(line), CHILD_COLOR(255,230,170), MYOS_GDI_TEXT_LEFT);
    for (int i = 0; i < 4; ++i)
        if (g_ChildDialogLab.dump[i][0]) child_gdi_text_n(ipc, 14, 194 + i * 18, cw - 28, 14, g_ChildDialogLab.dump[i], sizeof(g_ChildDialogLab.dump[i]), CHILD_COLOR(200,210,225), MYOS_GDI_TEXT_LEFT);

    ipc->shared->child_hwnd_command_count = g_ChildDialogLab.commands;
    ipc->shared->child_hwnd_click_count = g_ChildDialogLab.clicks;
    ipc->shared->child_hwnd_last_msg = reason;
    child_copy_cstr_bounded(ipc->shared->child_hwnd_status, sizeof(ipc->shared->child_hwnd_status), g_ChildDialogLab.status[0] ? g_ChildDialogLab.status : "DialogLab OOP ready");
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static void child_dialoglab_render_probe(ChildIpcContext* ipc, int index, UINT reason, const char* status)
{
    if (!ipc || !ipc->shared) return;
    if (index < 0 || index >= CHILD_DLAB_MAX_DIALOGS) return;
    ChildDialogProbe* p = &g_ChildDialogLab.probes[index];
    if (!p->active || !p->hwnd) return;

    int w = p->winW > 0 ? p->winW : 440;
    int h = p->winH > 0 ? p->winH : 190;
    if (!p->winW && (p->command == CHILD_DLAB_CMD_OPENFILE || p->command == CHILD_DLAB_CMD_SAVEFILE || p->command == CHILD_DLAB_CMD_CHOOSEFONT)) {
        w = 500;
        h = 210;
    }
    int cw = 0, ch = 0;
    child_hwnd_client_size(p->hwnd, w, h, 220, 100, &cw, &ch);

    child_gdi_reset_for_hwnd(ipc, p->hwnd, cw, ch, reason, status ? status : "Dialog probe WM_PAINT/GDI");
    child_gdi_fill(ipc, 0, 0, cw, ch, p->modal ? CHILD_COLOR(24,18,30) : CHILD_COLOR(18,23,31));
    child_gdi_rect(ipc, 0, 0, cw, ch, p->modal ? CHILD_COLOR(120,85,150) : CHILD_COLOR(85,115,155));

    char line[220];
    snprintf(line, sizeof(line), "%s", p->title[0] ? p->title : "OOP dialog");
    child_gdi_text_n(ipc, 14, 8, cw - 28, 14, line, sizeof(line), CHILD_COLOR(245,245,255), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "HWND=%u owner=%u stream=%u; child PID %ld owns this top-level DC",
             (unsigned)p->hwnd, (unsigned)g_ChildDialogLab.hwnd, (unsigned)g_ChildGdiTargetSeq, (long)getpid());
    child_gdi_text_n(ipc, 14, 54, cw - 28, 14, line, sizeof(line), CHILD_COLOR(180,225,255), MYOS_GDI_TEXT_LEFT);
    snprintf(line, sizeof(line), "%s", status && status[0] ? status : "per-HWND retained GDI stream active");
    child_gdi_text_n(ipc, 14, 74, cw - 28, 14, line, sizeof(line), CHILD_COLOR(255,230,165), MYOS_GDI_TEXT_LEFT);
    child_gdi_text(ipc, 14, ch - 44, cw - 28, 14,
                   p->modal ? "Modal: owner disabled, OK/Cancel ends the modal loop." : "Modeless: independent top-level, Close only destroys this dialog.",
                   CHILD_COLOR(205,215,235), MYOS_GDI_TEXT_LEFT);

    ipc->shared->child_hwnd_last_msg = reason;
    ipc->shared->gdi_sequence++;
    ipc->shared->gdi_paint_count++;
}

static void child_dialoglab_set_status(ChildIpcContext* ipc, UINT reason, const char* text)
{
    if (text) child_copy_cstr_bounded(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), text);
    child_dialoglab_render(ipc, reason, g_ChildDialogLab.status);
}

static int child_dialoglab_alloc_probe(void)
{
    for (int i = 0; i < CHILD_DLAB_MAX_DIALOGS; ++i)
        if (!g_ChildDialogLab.probes[i].active) return i;
    return -1;
}

static void child_dialoglab_request_owner_enable(HWND owner, BOOL enable)
{
    if (owner) (void)EnableWindow(owner, enable);
}

static void child_dialoglab_close_probe(ChildIpcContext* ipc, int index, uint16_t result, const char* why)
{
    if (index < 0 || index >= CHILD_DLAB_MAX_DIALOGS) return;
    ChildDialogProbe* p = &g_ChildDialogLab.probes[index];
    if (!p->active) return;
    HWND hwnd = p->hwnd;
    HWND text = p->text;
    HWND okBtn = p->ok;
    HWND cancelBtn = p->cancel;
    uint8_t wasModal = p->modal;
    uint16_t cmd = p->command;
    p->result = result;
    p->active = 0;
    p->modal = 0;
    p->hwnd = 0;
    p->text = 0;
    p->ok = 0;
    p->cancel = 0;
    /* v172: retained per-HWND GDI streams must have window lifetime.  When a
       top-level OOP dialog dies, forget its own stream and its child-control
       streams before publishing the owner's exposed repaint. */
    if (hwnd) child_gdi_compact_without_hwnd(ipc, (uint32_t)hwnd);
    if (text) child_gdi_compact_without_hwnd(ipc, (uint32_t)text);
    if (okBtn) child_gdi_compact_without_hwnd(ipc, (uint32_t)okBtn);
    if (cancelBtn) child_gdi_compact_without_hwnd(ipc, (uint32_t)cancelBtn);
    if (g_ChildDialogLab.pseudoDialogsOpen) g_ChildDialogLab.pseudoDialogsOpen--;
    g_ChildDialogLab.realDialogsClosed++;
    g_ChildDialogLab.lastModalResult = result;
    if (wasModal) child_dialoglab_request_owner_enable(g_ChildDialogLab.hwnd, TRUE);
    if (g_ChildDialogLab.hwnd) InvalidateRect(g_ChildDialogLab.hwnd, NULL, TRUE);
    snprintf(g_ChildDialogLab.lastResult, sizeof(g_ChildDialogLab.lastResult), "%s result=%u for %s", wasModal ? "modal" : "modeless", (unsigned)result, child_dialog_cmd_name(cmd));
    snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "%s closed via %s", p->title[0] ? p->title : "OOP dialog", why ? why : "command");
    if (hwnd) (void)DestroyWindow(hwnd);
    /* v173.4: Re-rendering the owner here is semantically correct: the
       DialogLab body shows the new lastResult/status immediately.  This must
       stay safe because child_dialoglab_render() no longer reads the
       process-global shared->gui_w/gui_h polluted by the dialog CreateWindowExA. */
    child_dialoglab_render(ipc, WM_COMMAND, g_ChildDialogLab.status);
}

static HWND child_dialoglab_open_probe(ChildIpcContext* ipc, uint16_t command, BOOL modal)
{
    int index = child_dialoglab_alloc_probe();
    if (index < 0) {
        snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "No free OOP dialog probe slots");
        return 0;
    }
    ChildDialogProbe* p = &g_ChildDialogLab.probes[index];
    memset(p, 0, sizeof(*p));
    p->command = command;
    p->modal = modal ? 1u : 0u;
    p->active = 1u;
    snprintf(p->title, sizeof(p->title), "%s%s", child_dialog_cmd_name(command), modal ? " [modal OOP]" : " [modeless OOP]");

    int x = 320 + index * 22;
    int y = 190 + index * 18;
    int w = 440;
    int h = 190;
    if (command == CHILD_DLAB_CMD_OPENFILE || command == CHILD_DLAB_CMD_SAVEFILE || command == CHILD_DLAB_CMD_CHOOSEFONT) { w = 500; h = 210; }
    p->winW = w;
    p->winH = h;
    if (modal) child_dialoglab_request_owner_enable(g_ChildDialogLab.hwnd, FALSE);
    HWND dlg = CreateWindowExA(0, "myOS.OOPDialogLab.Dialog", p->title, WS_POPUP | WS_VISIBLE, x, y, w, h, modal ? g_ChildDialogLab.hwnd : 0, NULL, NULL, NULL);
    p->hwnd = dlg;
    if (!dlg) {
        p->active = 0;
        if (modal) child_dialoglab_request_owner_enable(g_ChildDialogLab.hwnd, TRUE);
        snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "CreateWindowExA failed for %s", p->title);
        return 0;
    }

    char text[160];
    snprintf(text, sizeof(text), "%s lives in child PID %ld; parent only brokers HWNDs. Tab must wrap between these buttons.", child_dialog_cmd_name(command), (long)getpid());
    ChildCreateWindowExBatchItem batch[3];
    HWND madeHwnds[3];
    memset(batch, 0, sizeof(batch));
    memset(madeHwnds, 0, sizeof(madeHwnds));
    UINT n = 0;
    batch[n].class_name = "STATIC";
    batch[n].title = text;
    batch[n].style = WS_CHILD | WS_VISIBLE;
    batch[n].x = 18; batch[n].y = 24; batch[n].w = w - 38; batch[n].h = 38;
    batch[n].parent = dlg;
    batch[n].menu = (void*)(uintptr_t)(CHILD_DLAB_ID_BASE + index);
    n++;
    if (modal) {
        batch[n].class_name = "BUTTON";
        batch[n].title = "OK";
        batch[n].style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON;
        batch[n].x = w - 178; batch[n].y = h - 76; batch[n].w = 72; batch[n].h = 26;
        batch[n].parent = dlg;
        batch[n].menu = (void*)(uintptr_t)(CHILD_DLAB_ID_OK_BASE + index);
        n++;
        batch[n].class_name = "BUTTON";
        batch[n].title = "Cancel";
        batch[n].style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
        batch[n].x = w - 96; batch[n].y = h - 76; batch[n].w = 78; batch[n].h = 26;
        batch[n].parent = dlg;
        batch[n].menu = (void*)(uintptr_t)(CHILD_DLAB_ID_CAN_BASE + index);
        n++;
    } else {
        batch[n].class_name = "BUTTON";
        batch[n].title = "Close";
        batch[n].style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON;
        batch[n].x = w - 106; batch[n].y = h - 76; batch[n].w = 88; batch[n].h = 26;
        batch[n].parent = dlg;
        batch[n].menu = (void*)(uintptr_t)(CHILD_DLAB_ID_CLOSE_BASE + index);
        n++;
    }
    UINT made = CreateChildWindowsBatchA(ipc, batch, madeHwnds, n, modal ? "modal dialog controls" : "modeless dialog controls");
    p->text = madeHwnds[0];
    p->ok = madeHwnds[1];
    p->cancel = modal ? madeHwnds[2] : 0;
    if (made != n) {
        if (!p->text) p->text = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, 18, 24, w - 38, 38, dlg, (void*)(uintptr_t)(CHILD_DLAB_ID_BASE + index), NULL, NULL);
        if (modal) {
            if (!p->ok) p->ok = CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON, w - 178, h - 76, 72, 26, dlg, (void*)(uintptr_t)(CHILD_DLAB_ID_OK_BASE + index), NULL, NULL);
            if (!p->cancel) p->cancel = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, w - 96, h - 76, 78, 26, dlg, (void*)(uintptr_t)(CHILD_DLAB_ID_CAN_BASE + index), NULL, NULL);
        } else {
            if (!p->ok) p->ok = CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON, w - 106, h - 76, 88, 26, dlg, (void*)(uintptr_t)(CHILD_DLAB_ID_CLOSE_BASE + index), NULL, NULL);
        }
    }
    g_ChildDialogLab.pseudoDialogsOpen++;
    g_ChildDialogLab.realDialogsOpened++;
    snprintf(g_ChildDialogLab.lastResult, sizeof(g_ChildDialogLab.lastResult), "%s HWND=%u", modal ? "modal opened" : "modeless opened", (unsigned)dlg);
    snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "%s opened as real OOP HWND=%u%s", p->title, (unsigned)dlg, modal ? " owner disabled" : "");
    child_dialoglab_render_probe(ipc, index, WM_PAINT, "initial per-HWND dialog paint");
    child_dialoglab_render(ipc, WM_COMMAND, g_ChildDialogLab.status);
    return dlg;
}

static uint16_t child_dialoglab_modal_loop(ChildIpcContext* ipc, HWND dlg)
{
    int index = child_dialog_probe_index_from_hwnd(dlg);
    if (index < 0) return IDCANCEL;
    uint32_t spins = 0;
    while (ipc && !ipc->close_seen) {
        index = child_dialog_probe_index_from_hwnd(dlg);
        if (index < 0 || !g_ChildDialogLab.probes[index].active) break;
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) {
            (void)DispatchMessageA(&msg);
        }
        if ((spins++ % 25u) == 0u) child_shared_update(ipc, "dialoglab-modal-loop", 0, NULL, 0);
    }
    index = child_dialog_probe_index_from_hwnd(dlg);
    return index >= 0 ? g_ChildDialogLab.probes[index].result : (g_ChildDialogLab.lastModalResult ? g_ChildDialogLab.lastModalResult : IDOK);
}

static void child_dialoglab_command(ChildIpcContext* ipc, uint16_t id)
{
    g_ChildDialogLab.commands++;
    g_ChildDialogLab.lastCommand = id;
    switch (id) {
    case CHILD_DLAB_CMD_MODAL: {
        g_ChildDialogLab.modalCount++;
        HWND dlg = child_dialoglab_open_probe(ipc, id, TRUE);
        uint16_t r = dlg ? child_dialoglab_modal_loop(ipc, dlg) : IDCANCEL;
        snprintf(g_ChildDialogLab.lastResult, sizeof(g_ChildDialogLab.lastResult), "DialogBoxParam-style modal returned %u", (unsigned)r);
        snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "OOP modal loop returned result=%u; owner re-enabled", (unsigned)r);
        break;
    }
    case CHILD_DLAB_CMD_MODELESS:
        g_ChildDialogLab.modelessCount++;
        (void)child_dialoglab_open_probe(ipc, id, FALSE);
        break;
    case CHILD_DLAB_CMD_DUMP:
        child_dialog_dump_template();
        snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "Dumped OOP bridge + real dialog HWND canaries");
        break;
    case CHILD_DLAB_CMD_CONTROLS:
    case CHILD_DLAB_CMD_BUTTONS:
    case CHILD_DLAB_CMD_TEXT:
    case CHILD_DLAB_CMD_KEYBOARD:
    case CHILD_DLAB_CMD_OPENFILE:
    case CHILD_DLAB_CMD_SAVEFILE:
    case CHILD_DLAB_CMD_CHOOSEFONT:
        if (id == CHILD_DLAB_CMD_KEYBOARD) g_ChildDialogLab.tabLoopCanary++;
        if (id == CHILD_DLAB_CMD_OPENFILE || id == CHILD_DLAB_CMD_SAVEFILE || id == CHILD_DLAB_CMD_CHOOSEFONT)
            snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "%s OOP placeholder opened; full COMDLG parser bridge remains classic", child_dialog_cmd_name(id));
        (void)child_dialoglab_open_probe(ipc, id, FALSE);
        break;
    case CHILD_DLAB_CMD_SCROLLSTD:
        snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "Std scrollbar probe requested from OOP DialogLab");
        break;
    case CHILD_DLAB_CMD_MENU:
        snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "Menu APIs probe requested from OOP DialogLab");
        break;
    case CHILD_DLAB_CMD_DUMPNAV:
        g_ChildDialogLab.navDumps++;
        snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "Dialog nav dump #%u: %u command tabstops + real dialog buttons are circular", (unsigned)g_ChildDialogLab.navDumps, (unsigned)CHILD_DLAB_MAX_BUTTONS);
        break;
    default:
        snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "Unknown DialogLab command id=0x%04x", id);
        break;
    }
    child_dialoglab_render(ipc, WM_COMMAND, g_ChildDialogLab.status);
}

static LRESULT child_dialoglab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ChildIpcContext* ipc = g_GuiIpcRuntime;
    if (msg == WM_CREATE) {
        if (!g_ChildDialogLab.hwnd) {
            memset(&g_ChildDialogLab, 0, sizeof(g_ChildDialogLab));
            g_ChildDialogLab.hwnd = hwnd;
            if (ipc && ipc->shared) {
                g_ChildDialogLab.winW = (int)ipc->shared->gui_w;
                g_ChildDialogLab.winH = (int)ipc->shared->gui_h;
            }
            if (g_ChildDialogLab.winW < 760) g_ChildDialogLab.winW = 920;
            if (g_ChildDialogLab.winH < 300) g_ChildDialogLab.winH = 320;
            child_dialog_dump_template();
            snprintf(g_ChildDialogLab.lastResult, sizeof(g_ChildDialogLab.lastResult), "IDCANCEL baseline");
            snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "DialogLab OOP ready: command buttons + real dialog HWND bridge");
            child_dialoglab_render(ipc, msg, g_ChildDialogLab.status);
        }
        return 0;
    }
    if (msg == WM_SIZE && hwnd == g_ChildDialogLab.hwnd) {
        /* v173.4: WM_SIZE is the compositor's authoritative client-size
           notification (LOWORD/HIWORD of lParam, MSDN-style).  The DialogLab
           owner must update its per-HWND cached frame size before repainting;
           otherwise resizing the shell frame leaves the retained OOP-GDI stream
           at the previous dimensions until some later repaint happens to heal it.
           Do not read process-global shared->gui_w/gui_h here: modal dialog
           CreateWindowExA calls legitimately overwrite that slot. */
        int cw = (int)(uint16_t)((uint32_t)lp & 0xffffu);
        int ch = (int)(uint16_t)(((uint32_t)lp >> 16) & 0xffffu);
        if (cw > 0) g_ChildDialogLab.winW = cw + 2;
        if (ch > 0) g_ChildDialogLab.winH = ch + 25;
        child_dialoglab_render(ipc, msg, g_ChildDialogLab.status);
        return 0;
    }
    if (msg == WM_SIZE) {
        int di = child_dialog_probe_index_from_hwnd(hwnd);
        if (di >= 0 && g_ChildDialogLab.probes[di].hwnd == hwnd) {
            int cw = (int)(uint16_t)((uint32_t)lp & 0xffffu);
            int ch = (int)(uint16_t)(((uint32_t)lp >> 16) & 0xffffu);
            if (cw > 0) g_ChildDialogLab.probes[di].winW = cw + 2;
            if (ch > 0) g_ChildDialogLab.probes[di].winH = ch + 25;
            child_dialoglab_render_probe(ipc, di, msg, g_ChildDialogLab.status);
        }
        return 0;
    }
    if (msg == WM_PAINT || msg == WM_WINDOWPOSCHANGED || msg == WM_ENABLE) {
        int di = child_dialog_probe_index_from_hwnd(hwnd);
        if (di >= 0 && g_ChildDialogLab.probes[di].hwnd == hwnd)
            child_dialoglab_render_probe(ipc, di, msg, g_ChildDialogLab.status);
        else if (hwnd == g_ChildDialogLab.hwnd)
            child_dialoglab_render(ipc, msg, g_ChildDialogLab.status);
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        uint16_t id = child_dialog_id_from_hwnd(hwnd);
        if (id) {
            g_ChildDialogLab.clicks++;
            PostMessageA(g_ChildDialogLab.hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hwnd);
            snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "child command button up id=0x%04x -> WM_COMMAND", id);
            child_dialoglab_render(ipc, msg, g_ChildDialogLab.status);
            return 0;
        }
        int di = -1;
        int role = child_dialog_probe_button_role(hwnd, &di);
        if (role) {
            g_ChildDialogLab.clicks++;
            child_dialoglab_close_probe(ipc, di, (uint16_t)role, role == IDOK ? "OK" : (role == IDCANCEL ? "Cancel" : "Close"));
            return 0;
        }
    }
    if (msg == WM_COMMAND) {
        uint16_t id = LOWORD(wp);
        if (!child_dialog_command_is_activation(wp)) {
            snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status),
                     "ignored WM_COMMAND notify id=0x%04x code=%u; waiting for BN_CLICKED/code0",
                     (unsigned)id, (unsigned)HIWORD(wp));
            if (hwnd == g_ChildDialogLab.hwnd)
                child_dialoglab_render(ipc, msg, g_ChildDialogLab.status);
            return 0;
        }
        if (id >= CHILD_DLAB_ID_OK_BASE && id < CHILD_DLAB_ID_OK_BASE + CHILD_DLAB_MAX_DIALOGS) {
            child_dialoglab_close_probe(ipc, (int)(id - CHILD_DLAB_ID_OK_BASE), IDOK, "OK/WM_COMMAND");
            return 0;
        }
        if (id >= CHILD_DLAB_ID_CAN_BASE && id < CHILD_DLAB_ID_CAN_BASE + CHILD_DLAB_MAX_DIALOGS) {
            child_dialoglab_close_probe(ipc, (int)(id - CHILD_DLAB_ID_CAN_BASE), IDCANCEL, "Cancel/WM_COMMAND");
            return 0;
        }
        if (id >= CHILD_DLAB_ID_CLOSE_BASE && id < CHILD_DLAB_ID_CLOSE_BASE + CHILD_DLAB_MAX_DIALOGS) {
            child_dialoglab_close_probe(ipc, (int)(id - CHILD_DLAB_ID_CLOSE_BASE), IDOK, "Close/WM_COMMAND");
            return 0;
        }
        if (child_dialog_is_command_button_id(id)) {
            child_dialoglab_command(ipc, id);
            return 0;
        }
        snprintf(g_ChildDialogLab.status, sizeof(g_ChildDialogLab.status), "ignored unknown WM_COMMAND id=0x%04x", (unsigned)id);
        if (hwnd == g_ChildDialogLab.hwnd) child_dialoglab_render(ipc, msg, g_ChildDialogLab.status);
        return 0;
    }
    if (msg == WM_KEYDOWN && (int)wp == KEY_TAB) {
        g_ChildDialogLab.tabLoopCanary++;
        child_dialoglab_set_status(ipc, msg, "WM_KEYDOWN TAB reached child fallback; expected desktop IsDialogMessage to consume/wrap");
        return 0;
    }
    if (msg == WM_KEYDOWN && ((int)wp == KEY_ENTER || (int)wp == KEY_KPENTER)) {
        int di = child_dialog_probe_index_from_hwnd(hwnd);
        if (di >= 0) { child_dialoglab_close_probe(ipc, di, IDOK, "Enter/OK"); return 0; }
    }
    if (msg == WM_KEYDOWN && (int)wp == KEY_ESC) {
        int di = child_dialog_probe_index_from_hwnd(hwnd);
        if (di >= 0) { child_dialoglab_close_probe(ipc, di, IDCANCEL, "Escape/Cancel"); return 0; }
    }
    if (msg == WM_CLOSE) {
        int di = child_dialog_probe_index_from_hwnd(hwnd);
        if (di >= 0) {
            child_dialoglab_close_probe(ipc, di, IDCANCEL, "WM_CLOSE");
            return 0;
        }
        if (hwnd == g_ChildDialogLab.hwnd) {
            if (ipc) ipc->close_seen = 1;
            for (int i = 0; i < CHILD_DLAB_MAX_DIALOGS; ++i)
                if (g_ChildDialogLab.probes[i].active) child_dialoglab_close_probe(ipc, i, IDCANCEL, "owner WM_CLOSE");
            child_dialoglab_set_status(ipc, msg, "DialogLab OOP WM_CLOSE");
        }
        return 0;
    }
    return 0;
}

static int child_dialoglab_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = "DialogLab [OOP v180]";
    int x = 260, y = 140, w = 920, h = 320;
    if (argc > 0 && argv && argv[0] && argv[0][0]) title = argv[0];
    if (argc > 1 && argv[1]) x = atoi(argv[1]);
    if (argc > 2 && argv[2]) y = atoi(argv[2]);
    if (argc > 3 && argv[3]) w = atoi(argv[3]);
    if (argc > 4 && argv[4]) h = atoi(argv[4]);
    if (w < 760) w = 760;
    if (h < 300) h = 300;

    child_shared_update(ipc, "dialoglab-oop-runtime-start", argc, argv, 0);
    MyGuiIpcRuntimeAttach(ipc);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = child_dialoglab_wndproc;
    wc.lpszClassName = "myOS.OOPDialogLab";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    if (hwnd) {
        child_dialoglab_wndproc(hwnd, WM_CREATE, 0, 0);
        g_ChildDialogLab.winW = w;
        g_ChildDialogLab.winH = h;
        ChildCreateWindowExBatchItem batch[CHILD_DLAB_MAX_BUTTONS];
        memset(batch, 0, sizeof(batch));
        for (int i = 0; i < CHILD_DLAB_MAX_BUTTONS; ++i) {
            const ChildDialogButtonDef* b = &g_ChildDialogButtons[i];
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
            if (i == 0) style |= WS_GROUP;
            if (b->id == CHILD_DLAB_CMD_DUMP) style |= BS_DEFPUSHBUTTON;
            batch[i].ex_style = 0;
            batch[i].class_name = "BUTTON";
            batch[i].title = b->text;
            batch[i].style = style;
            batch[i].x = b->x;
            batch[i].y = b->y;
            batch[i].w = b->w;
            batch[i].h = b->h;
            batch[i].parent = hwnd;
            batch[i].menu = (void*)(uintptr_t)b->id;
        }
        UINT made = CreateChildWindowsBatchA(ipc, batch, g_ChildDialogLab.buttons, CHILD_DLAB_MAX_BUTTONS, "DialogLab command buttons");
        if (made != CHILD_DLAB_MAX_BUTTONS) {
            for (int i = 0; i < CHILD_DLAB_MAX_BUTTONS; ++i) {
                if (g_ChildDialogLab.buttons[i]) continue;
                const ChildDialogButtonDef* b = &g_ChildDialogButtons[i];
                DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
                if (i == 0) style |= WS_GROUP;
                if (b->id == CHILD_DLAB_CMD_DUMP) style |= BS_DEFPUSHBUTTON;
                g_ChildDialogLab.buttons[i] = CreateWindowExA(0, "BUTTON", b->text, style, b->x, b->y, b->w, b->h, hwnd, (void*)(uintptr_t)b->id, NULL, NULL);
            }
        }
        child_dialoglab_render(ipc, WM_PAINT, made == CHILD_DLAB_MAX_BUTTONS ? "DialogLab OOP child command HWND batch requested/acked" : "DialogLab OOP child command HWND batch fallback path");
    }
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, "v180 dialoglab oop started");
    printf("[v180 child pid=%ld] OOP dialoglab top=%u buttons=%u\n", (long)getpid(), (unsigned)hwnd, (unsigned)CHILD_DLAB_MAX_BUTTONS);
    fflush(stdout);
    uint32_t heartbeat = 0;
    while (hwnd && ipc && !ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) {
            if (!DispatchMessageA(&msg)) break;
        }
        child_shared_update(ipc, "dialoglab-oop-message-loop", argc, argv, 0);
        if ((heartbeat++ % 30u) == 0u) child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "v180 dialoglab heartbeat");
    }
    child_dialoglab_render(ipc, WM_CLOSE, "dialoglab exiting");
    child_shared_update(ipc, ipc->close_seen ? "dialoglab-close-seen" : "dialoglab-exiting", argc, argv, 74);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 74, ipc->close_seen ? "dialoglab WM_CLOSE exit report" : "dialoglab exit report");
    return 74;
}


static int child_gui_main(int argc, char** argv, ChildIpcContext* ipc)
{
    const char* title = argc > 0 && argv && argv[0] ? argv[0] : "myOS IPC GUI Child";
    int x = argc > 1 && argv[1] ? atoi(argv[1]) : 180;
    int y = argc > 2 && argv[2] ? atoi(argv[2]) : 140;
    int w = argc > 3 && argv[3] ? atoi(argv[3]) : 360;
    int h = argc > 4 && argv[4] ? atoi(argv[4]) : 180;
    if (w < 180) w = 180;
    if (h < 100) h = 100;

    child_shared_update(ipc, "gui-runtime-start", argc, argv, 0);
    MyGuiIpcRuntimeAttach(ipc);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpszClassName = "myOS.IPCProxyWindow";
    ATOM atom = RegisterClassExA(&wc);
    HWND hwnd = atom ? CreateWindowExA(0, wc.lpszClassName, title, 0, x, y, w, h, 0, NULL, NULL, NULL) : 0;
    child_kernel_bridge_selftest(ipc);

    char ackText[96];
    snprintf(ackText, sizeof(ackText), "v88 runtime hwnd=%u atom=%u", (unsigned)hwnd, (unsigned)atom);
    child_shared_update(ipc, hwnd ? "runtime-window-created" : "runtime-window-timeout", argc, argv, 0);
    child_ipc_send(ipc, MYOS_IPC_OP_PING, (uint32_t)hwnd, ackText);
    printf("[v169 child pid=%ld] GUI runtime RegisterClassExA/CreateWindowExA title='%s' hwnd=%u\n", (long)getpid(), title, (unsigned)hwnd);
    fflush(stdout);

    if (hwnd) PostMessageA(hwnd, WM_USER + 0x61u, 0x6100u, 0x0061u);

    /* v61.2: this window is a real long-lived GUI child now.
       v61/v61.1 used a fixed 240-iteration demo loop. That made the proxy
       destroy itself after a short while; dragging the window accelerated the
       counter because WM_WINDOWPOSCHANGED messages arrived continuously.
       Keep the child alive until WM_CLOSE/DestroyWindow is delivered. */
    uint32_t heartbeat = 0;
    while (!ipc->close_seen) {
        MSG msg;
        if (GetMessageA(&msg, 0, 0, 0)) {
            if (!DispatchMessageA(&msg)) break;
        }
        child_shared_update(ipc, "runtime-message-loop", argc, argv, 0);
        if ((heartbeat++ % 20u) == 0u)
            child_ipc_send(ipc, MYOS_IPC_OP_PING, heartbeat, "gui runtime heartbeat");
    }

    child_shared_update(ipc, ipc->close_seen ? "gui-runtime-close-seen" : "gui-runtime-exiting", argc, argv, 61);
    child_ipc_send(ipc, MYOS_IPC_OP_EXIT, 61, ipc->close_seen ? "gui runtime WM_CLOSE exit report" : "gui runtime exit report");
    return 61;
}

int main(int argc, char** argv)
{
    int ipcFd = -1;
    const char* sharedName = NULL;
    unsigned myPid = 0;
    int i = 1;
    while (i < argc) {
        if (ascii_ieq(argv[i], "--ipc-fd") && i + 1 < argc) { ipcFd = atoi(argv[i + 1]); i += 2; continue; }
        if (ascii_ieq(argv[i], "--shared-name") && i + 1 < argc) { sharedName = argv[i + 1]; i += 2; continue; }
        if (ascii_ieq(argv[i], "--my-pid") && i + 1 < argc) { myPid = (unsigned)strtoul(argv[i + 1], NULL, 10); i += 2; continue; }
        break;
    }
    if (i + 1 >= argc || (!ascii_ieq(argv[i], "--console") && !ascii_ieq(argv[i], "--gui"))) {
        fprintf(stderr, "myos_apphost_child v75: usage: %s [--ipc-fd N --shared-name NAME --my-pid PID] (--console|--gui) <image> [argv...]\n", argv[0] ? argv[0] : "myos_apphost_child");
        return 125;
    }

    int isGui = ascii_ieq(argv[i], "--gui");
    const char* image = argv[i + 1];
    int app_argc = argc - (i + 2);
    char** app_argv = argv + (i + 2);

    ChildIpcContext ipc;
    memset(&ipc, 0, sizeof(ipc));
    ipc.fd = ipcFd;
    ipc.myPid = myPid;
    ipc.image = image;
    ipc.shared = child_open_shared(sharedName);

    child_shared_update(&ipc, "child-hello", app_argc, app_argv, 0);
    child_ipc_send(&ipc, MYOS_IPC_OP_HELLO, (uint32_t)app_argc, "hello from exec child");

    int rc = 126;
    if (isGui) {
        if (ascii_ieq(image, "calc") || ascii_ieq(image, "calc.exe") || ascii_ieq(image, "calculator") || ascii_ieq(image, "calculator.exe"))
            rc = child_calc_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "editor") || ascii_ieq(image, "editor.exe") || ascii_ieq(image, "notepad") || ascii_ieq(image, "notepad.exe") || ascii_ieq(image, "texteditor"))
            rc = child_editor_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "paint-lab") || ascii_ieq(image, "paintlab") || ascii_ieq(image, "paint-lab.exe"))
            rc = child_paint_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "drag-lab") || ascii_ieq(image, "draglab") || ascii_ieq(image, "drag-lab.exe"))
            rc = child_draglab_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "control-lab") || ascii_ieq(image, "controllab") || ascii_ieq(image, "control-lab.exe"))
            rc = child_controllab_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "dialog-lab") || ascii_ieq(image, "dialoglab") || ascii_ieq(image, "dialog-lab.exe"))
            rc = child_dialoglab_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "clip-menu-lab") || ascii_ieq(image, "clipmenu") || ascii_ieq(image, "clip-menu-lab.exe"))
            rc = child_clipmenu_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "wait-lab") || ascii_ieq(image, "waitlab") || ascii_ieq(image, "wait-lab.exe"))
            rc = child_waitlab_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "object-lab") || ascii_ieq(image, "objectlab") || ascii_ieq(image, "object-lab.exe"))
            rc = child_objectprobe_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "section-lab") || ascii_ieq(image, "sectionlab") || ascii_ieq(image, "section-lab.exe"))
            rc = child_sectionlab_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "statebus-lab") || ascii_ieq(image, "statebus") || ascii_ieq(image, "statebus-lab.exe"))
            rc = child_statebus_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "hwndstate-lab") || ascii_ieq(image, "hwndstate") || ascii_ieq(image, "hwndstate-lab.exe"))
            rc = child_hwndstate_main(app_argc, app_argv, &ipc);
        else if (ascii_ieq(image, "surface-lab") || ascii_ieq(image, "surfacelab") || ascii_ieq(image, "surface-lab.exe"))
            rc = child_surfacelab_main(app_argc, app_argv, &ipc);
        else
            rc = child_gui_main(app_argc, app_argv, &ipc);
    } else if (ascii_ieq(image, "argdump") || ascii_ieq(image, "argdump.exe") || ascii_ieq(image, "argv-lab")) {
        rc = child_argdump_main(app_argc, app_argv, &ipc);
    } else if (ascii_ieq(image, "sleeper") || ascii_ieq(image, "sleeper.exe")) {
        rc = child_sleeper_main(app_argc, app_argv, &ipc);
    } else {
        fprintf(stderr, "myos_apphost_child v75: unknown console image '%s'\n", image ? image : "");
        child_shared_update(&ipc, "unknown-image", app_argc, app_argv, 126);
        child_ipc_send(&ipc, MYOS_IPC_OP_EXIT, 126, "unknown console image");
        rc = 126;
    }

    if (ipc.shared) munmap(ipc.shared, sizeof(MyProcessIpcShared));
    if (ipc.fd >= 0) close(ipc.fd);
    return rc;
}
