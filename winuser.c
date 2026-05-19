#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "mywin_pendant_internal.h"
#include "myobject.h"
#include "mycontrols.h"
#include "processhost.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <limits.h>
#include <stddef.h>
#include <linux/input-event-codes.h>

/* AUDIT(v118): USER32 is still backed by fixed global tables and several
   desktop/session-global variables. Good for current labs; multiple UI threads,
   desktops or foreign apps will need per-process/thread/desktop-owned state. */
#define MYWIN_MAX_CLASSES 128
#define MYWIN_CLASS_HASH_BUCKETS 128
#define MYWIN_CLASS_HASH_MASK (MYWIN_CLASS_HASH_BUCKETS - 1)

typedef struct MyWinClassEntry {
    int valid;
    ATOM atom;
    DWORD classHash;
    int classHashNext;
    int atomHashNext;
    WNDCLASSEXA wc;
    char className[64];
    DWORD ownerPid;
    HINSTANCE hInstance;
    int systemClass;
    int clsExtraBytes;
    LONG_PTR* clsExtra;
} MyWinClassEntry;

typedef struct MyWinThunk {
    WNDPROC proc;
    LPVOID  lpParam;
} MyWinThunk;

HWNDManager* g_lpHwndManager = NULL;
WindowManager* g_lpWindowManager = NULL;
__thread Capability   g_CurrentCapability;
__thread int          g_HasCapability = 0;
__thread MyWinRuntimeFrame g_RuntimeStack[MYWIN_MAX_RUNTIME_STACK];
__thread int g_RuntimeDepth = 0;
static MyWinClassEntry g_Classes[MYWIN_MAX_CLASSES];
static int g_ClassNameHash[MYWIN_CLASS_HASH_BUCKETS];
static int g_ClassAtomHash[MYWIN_CLASS_HASH_BUCKETS];
static int g_ClassFreeStack[MYWIN_MAX_CLASSES];
static int g_ClassFreeTop = 0;
static int g_ClassFreeInit = 0;
static ATOM g_NextAtom = 1;

static __thread ATOM g_ClassAtomLookupCacheAtom = 0;
static __thread MyWinClassEntry* g_ClassAtomLookupCachePtr = NULL;
static __thread DWORD g_ClassExactLookupCacheHash = 0;
static __thread DWORD g_ClassExactLookupCachePid = 0;
static __thread HINSTANCE g_ClassExactLookupCacheInstance = 0;
static __thread BOOL g_ClassExactLookupCacheSystemOnly = FALSE;
static __thread MyWinClassEntry* g_ClassExactLookupCachePtr = NULL;
/* AUDIT(v118): Capture/focus are global. MSDN behavior is queue/thread/desktop
   scoped; nested modal/OOP scenarios can steal each other's focus/capture. */
static HWND g_CaptureHwnd = 0;
static HWND g_FocusHwnd = 0;

static void mywin_copy_cstr_trunc(char* dst, size_t dstSize, const char* src)
{
    if (!dst || dstSize == 0) return;
    if (!src) src = "";

    size_t i = 0;
    while (i + 1 < dstSize && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void mywin_append_cstr_trunc(char* dst, size_t dstSize, const char* src)
{
    if (!dst || dstSize == 0) return;
    size_t len = 0;
    while (len < dstSize && dst[len]) ++len;
    if (len >= dstSize) {
        dst[dstSize - 1] = '\0';
        return;
    }
    mywin_copy_cstr_trunc(dst + len, dstSize - len, src);
}

/* v123: public MSG is now MSDN-shaped again.  Queue-private data that used to
   leak through MSG._myos is retained in a thread-local dispatch sidecar so
   DispatchMessageA can still complete sync/IPC dispatches that originated from
   GetMessage/PeekMessage, while manually constructed MSG values dispatch from
   only their documented public fields. */
#define MYWIN_MSG_SIDECAR_SLOTS 64

typedef struct MyWinMsgSidecar {
    int       valid;
    uint64_t  serial;
    MSG       pub;
    MyMessage mm;
} MyWinMsgSidecar;

static __thread MyWinMsgSidecar g_MsgSidecars[MYWIN_MSG_SIDECAR_SLOTS];
static __thread uint64_t g_MsgSidecarSerial = 1;

static int mywin_msg_public_equal(const MSG* a, const MSG* b)
{
    return a && b &&
           a->hwnd == b->hwnd &&
           a->message == b->message &&
           a->wParam == b->wParam &&
           a->lParam == b->lParam &&
           a->time == b->time &&
           a->pt.x == b->pt.x &&
           a->pt.y == b->pt.y;
}

static void mywin_store_msg_sidecar(const MSG* pub, const MyMessage* mm)
{
    if (!pub || !mm) return;
    unsigned idx = (unsigned)(g_MsgSidecarSerial % MYWIN_MSG_SIDECAR_SLOTS);
    g_MsgSidecars[idx].valid = 1;
    g_MsgSidecars[idx].serial = g_MsgSidecarSerial++;
    g_MsgSidecars[idx].pub = *pub;
    g_MsgSidecars[idx].mm = *mm;
}

static BOOL mywin_take_msg_sidecar(const MSG* pub, MyMessage* out)
{
    if (!pub || !out) return FALSE;
    int best = -1;
    uint64_t bestSerial = 0;
    for (int i = 0; i < MYWIN_MSG_SIDECAR_SLOTS; i++) {
        if (!g_MsgSidecars[i].valid) continue;
        if (!mywin_msg_public_equal(pub, &g_MsgSidecars[i].pub)) continue;
        if (g_MsgSidecars[i].serial >= bestSerial) {
            bestSerial = g_MsgSidecars[i].serial;
            best = i;
        }
    }
    if (best < 0) return FALSE;
    *out = g_MsgSidecars[best].mm;
    memset(&g_MsgSidecars[best], 0, sizeof(g_MsgSidecars[best]));
    return TRUE;
}

static void mywin_make_mymsg_from_public(const MSG* pub, MyMessage* out)
{
    if (!pub || !out) return;
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->type = 1;
    out->sender_pid = g_HasCapability ? g_CurrentCapability.id : 0;
    out->sender_tid = out->sender_pid;
    out->target_pid = g_lpHwndManager ? hwnd_get_owner_pid(g_lpHwndManager, pub->hwnd) : 0;
    out->target_tid = out->target_pid; /* current PoC uses pid==tid per UI owner */
    out->hwnd = pub->hwnd;
    out->msg = pub->message;
    out->wparam = pub->wParam;
    out->lparam = pub->lParam;
    out->priority = mymsg_default_priority(pub->message);
    out->lane = mymsg_default_lane(pub->message, 0);
    out->input_kind = mymsg_default_input_kind(pub->message);
    out->route_reason = mymsg_default_route_reason(pub->message, 0, out->input_kind);
    out->route_action = mymsg_required_hwnd_action_for_route(out->lane, out->input_kind, out->route_reason);
    out->filter_stages = mymsg_default_filter_stages(pub->message, out->lane, out->input_kind, out->route_reason);
    out->filter_state = _MSG_FILTER_STATE_PENDING;
    out->filter_stage = mymsg_first_filter_stage(out->filter_stages);
    out->route_hwnd = pub->hwnd;
    if (out->lane == _MSG_LANE_INPUT) out->route_state = _MSG_ROUTE_TARGET_RESOLVED;
    out->timestamp_ns = (uint64_t)pub->time * 1000000ull;
}


#define MYWIN_MAX_WINDOW_INFOS 128
#define MYWIN_WINDOW_CLASS_CHARS 64
#define MYWIN_WINDOW_TEXT_CHARS 512
#define MYWIN_CC_MAX_ITEMS 128
#define MYWIN_CC_TEXT_CHARS 96

typedef struct MyWinWindowControlState {
    int    pressed;
    int    checkState;
    int    editCaret;
    int    editSelStart;
    int    editSelEnd;
    int    editFirstLine;
    int    editHScroll;
    int    editPasswordChar;
    char   (*ccItems)[MYWIN_CC_TEXT_CHARS];
    LPARAM* ccItemData;
    unsigned char* ccSel;
    int    ccCount;
    int    ccCurSel;
    int    ccTopIndex;
    int    ccCaretIndex;
    int    ccAnchorIndex;
    int    ccItemHeight;
    int    ccDropped;
    int    ccDropHeight;
    int    ccScrollMin;
    int    ccScrollMax;
    int    ccScrollPage;
    int    ccScrollPos;
    int    ccScrollTracking;
    int    ccScrollTrackOff;
    int    ccScrollPressedPart;
    int    ccScrollTrackPos;
    int    stdVScrollVisible;
    int    stdHScrollVisible;
    UINT   stdVScrollArrows;
    UINT   stdHScrollArrows;
    int    stdHScrollMin;
    int    stdHScrollMax;
    int    stdHScrollPage;
    int    stdHScrollPos;
    int    stdHScrollTrackPos;
} MyWinWindowControlState;

typedef struct MyWinWindowMdiState {
    HWND   mdiActiveChild;
    HMENU  mdiFrameMenu;
    HMENU  mdiWindowMenu;
    UINT   mdiFirstChildId;
    int    mdiChildSeq;
    int    mdiIsChild;
} MyWinWindowMdiState;

typedef struct MYOS_CACHELINE_ALIGN MyWinWindowInfo {
    /* v241: hot HWND metadata is deliberately kept inside the first cacheline.
       v240 added intrusive sibling links before the owner/style fields; bucket
       validation and common GetWindow/GetWindowLong/SetWindowLong paths then paid a
       wider cache footprint.  Keep sibling/control/text state cold. */
    int    valid;
    HWND   hWnd;
    HWND   hParent;  /* visual/clip hierarchy for WS_CHILD windows */
    HWND   hOwner;   /* owner/Z-order relationship for top-level owned windows */
    DWORD  dwProcessId;
    DWORD  dwThreadId;
    UINT   id;
    DWORD  style;
    DWORD  exStyle;
    ATOM   classAtom;
    WORD   hotPad;
    WNDPROC wndproc;       // v41: current WndProc, changeable via GWLP_WNDPROC
    DWORD  zOrder;
    HINSTANCE hInstance;
    DWORD  classNameHash;  // v244: hot reject before touching cold className text
    DWORD  textHash;       // v244: hot reject before touching cold window title text

    /* Cold/tree/control payload starts at cacheline 2. */
    int    redrawEnabled; // v87: WM_SETREDRAW-lite placeholder
    int    destroying;     // v43: DestroyWindow reentrancy guard
    HWND   firstChild;
    HWND   lastChild;
    HWND   nextSibling;
    HWND   prevSibling;
    RECT   rcClient;   // for child controls: parent-client coordinates
    WNDPROC originalProc;  // v41: class/default proc for diagnostics/subclass chains
    LPVOID lpCreateParams;
    LONG_PTR userData;
    int      wndExtraBytes;
    LONG_PTR* wndExtra;  // v151: cbWndExtra-backed LONG_PTR slots, sized from the owning class
    char*  className;    // v243: cold text/control buffers are out-of-line
    char*  text;         // keeps HWND array traversal compact and cacheline friendly
    MyWinWindowControlState* control;
    MyWinWindowMdiState* mdi;
    int    ncDestroyed;    // v43: lifecycle diagnostic flag
} MyWinWindowInfo;

_Static_assert(offsetof(MyWinWindowInfo, firstChild) >= MYOS_CACHELINE_BYTES,
               "MyWinWindowInfo sibling links must stay out of the first cacheline");
_Static_assert(offsetof(MyWinWindowInfo, wndproc) < MYOS_CACHELINE_BYTES,
               "MyWinWindowInfo WndProc must stay in the first cacheline");
_Static_assert(offsetof(MyWinWindowInfo, textHash) < MYOS_CACHELINE_BYTES,
               "MyWinWindowInfo class/title hashes must stay in the first cacheline");
_Static_assert(sizeof(MyWinWindowInfo) % MYOS_CACHELINE_BYTES == 0,
               "MyWinWindowInfo array stride must preserve 64-byte alignment");
_Static_assert(sizeof(MyWinWindowInfo) <= (MYOS_CACHELINE_BYTES * 8),
               "MyWinWindowInfo hot/warm stride unexpectedly grew past four cachelines");

size_t MyWinDebugWindowInfoSize(void)
{
    return sizeof(MyWinWindowInfo);
}

size_t MyWinDebugWindowInfoColdBufferBytes(void)
{
    return sizeof(MyWinWindowControlState) + sizeof(MyWinWindowMdiState) +
           (size_t)MYWIN_WINDOW_CLASS_CHARS +
           (size_t)MYWIN_WINDOW_TEXT_CHARS +
           (size_t)MYWIN_CC_MAX_ITEMS * (size_t)MYWIN_CC_TEXT_CHARS +
           (size_t)MYWIN_CC_MAX_ITEMS * sizeof(LPARAM) +
           (size_t)MYWIN_CC_MAX_ITEMS * sizeof(unsigned char);
}


static BOOL mywin_can_read_window(HWND hWnd);
static BOOL mywin_is_int_atom_class(LPCSTR lpClassName);
static DWORD mywin_class_name_hash(LPCSTR s);
static DWORD mywin_title_hash(LPCSTR s);
static BOOL mywin_find_class_matches(const MyWinWindowInfo* wi, LPCSTR lpClassName);
static BOOL mywin_find_title_matches(const MyWinWindowInfo* wi, LPCSTR lpWindowName);

static MyWinWindowInfo g_WindowInfos[MYWIN_MAX_WINDOW_INFOS];
static DWORD g_NextWindowZOrder = 1;
static HWND g_RootFirstWindow = 0;
static HWND g_RootLastWindow = 0;



#define MYWIN_MAX_DIALOGS 16

typedef struct MyWinDialogInfo {
    int     valid;
    HWND    hDlg;
    HWND    hParent;
    DLGPROC dlgProc;
    LPARAM  initParam;
    INT_PTR result;
    int     modal;
    int     modeless;   /* v194: registered modeless dialog visible to shared pump */
    int     ended;
    int     initSent;
    int     defId;       /* v95/v100: persistent DM_SETDEFID default pushbutton id */
    int     focusDefId;  /* v100: temporary focused pushbutton default visual id */
    int     dragActive;
    int     dragOffX;
    int     dragOffY;
    int     dlgBaseX;   /* v90: dialog-unit base width from dialog font metrics-lite */
    int     dlgBaseY;   /* v90: dialog-unit base height from dialog font metrics-lite */
    char    templateName[64];
} MyWinDialogInfo;

static MyWinDialogInfo g_DialogInfos[MYWIN_MAX_DIALOGS];
static ATOM g_DialogClassAtom = 0;
static DWORD g_ModelessRegisterCount = 0;
static DWORD g_ModelessUnregisterCount = 0;
static DWORD g_ModelessPumpHitCount = 0;
static DWORD g_ModelessPumpMissCount = 0;

#ifndef WA_INACTIVE
#define WA_INACTIVE 0u
#endif
#ifndef WA_ACTIVE
#define WA_ACTIVE 1u
#endif
#ifndef WA_CLICKACTIVE
#define WA_CLICKACTIVE 2u
#endif

#define MYWIN_MAX_MODAL_STACK 16

typedef struct MyWinModalState {
    int   valid;
    int   depth;
    HWND  hDlg;
    HWND  hOwner;
    HWND  previousFocus;
    HWND  previousCapture;
    HWND  previousForeground;
    BOOL  ownerWasEnabled;
    BOOL  disabledOwner;
} MyWinModalState;

static MyWinModalState g_ModalStack[MYWIN_MAX_MODAL_STACK];
static int g_ModalDepth = 0;
static unsigned g_ModalPushCount = 0;
static unsigned g_ModalPopCount = 0;
static unsigned g_ModalRestoreFocusCount = 0;
static unsigned g_ModalRestoreCaptureCount = 0;


static DLGPROC mywin_dialog_current_proc(HWND hDlg, MyWinDialogInfo* di);
static void mywin_dialog_store_proc(HWND hDlg, DLGPROC proc);
static void mywin_dialog_set_msg_result(HWND hDlg, LRESULT value);
static LRESULT mywin_dialog_get_msg_result(HWND hDlg);

#define MYWIN_MAX_DIALOG_TEMPLATES 32
typedef struct MyWinDialogTemplateEntry {
    int valid;
    char name[64];
    LPCDLGTEMPLATEA lpTemplate;
} MyWinDialogTemplateEntry;

static MyWinDialogTemplateEntry g_DialogTemplates[MYWIN_MAX_DIALOG_TEMPLATES];

static MYWIN_MODALIDLEPROC g_MyWinModalIdleProc = NULL;
static void* g_MyWinModalIdleContext = NULL;

void MyWinSetModalIdleProc(MYWIN_MODALIDLEPROC lpProc, void* lpContext)
{
    g_MyWinModalIdleProc = lpProc;
    g_MyWinModalIdleContext = lpContext;
}

static void mywin_modal_idle(void)
{
    if (g_MyWinModalIdleProc)
        g_MyWinModalIdleProc(g_MyWinModalIdleContext);
}

#define MYWIN_DIALOG_CAPTION_H 22

static int mywin_muldiv_int(int nNumber, int nNumerator, int nDenominator)
{
    if (!nDenominator) return 0;
    long long v = (long long)nNumber * (long long)nNumerator;
    if (v >= 0) v += nDenominator / 2;
    else v -= nDenominator / 2;
    return (int)(v / nDenominator);
}

static void mywin_dialog_default_base(int* baseX, int* baseY)
{
    if (baseX) *baseX = 6;
    if (baseY) *baseY = 13;
}

static void mywin_dialog_font_base(DWORD style, WORD pointSize, LPCSTR faceName, int* baseX, int* baseY)
{
    int bx = 6, by = 13;
    /* v195 font-metrics-lite: true Win32 derives dialog units from the
       selected dialog font via GetTextMetrics.  myOS still has one bitmap
       font, but DLGTEMPLATE/DLGTEMPLATEEX must not ignore DS_SETFONT or
       DS_SHELLFONT because real resource layouts depend on the DLU base. */
    if (style & DS_SETFONT) {
        if (pointSize >= 12) { bx = 8; by = 18; }
        else if (pointSize >= 10) { bx = 7; by = 16; }
        else if (pointSize <= 7 && pointSize > 0) { bx = 5; by = 11; }
        if (faceName && (strstr(faceName, "Fixed") || strstr(faceName, "Courier")))
            bx = (bx < 7) ? 7 : bx;
        if ((style & DS_SHELLFONT) == DS_SHELLFONT && faceName && strstr(faceName, "MS Shell Dlg")) {
            if (bx < 6) bx = 6;
            if (by < 13) by = 13;
        }
    }
    if (baseX) *baseX = bx;
    if (baseY) *baseY = by;
}

static RECT mywin_dialog_units_to_pixels_rect(int baseX, int baseY, short x, short y, short cx, short cy)
{
    RECT r;
    r.left = mywin_muldiv_int((int)x, baseX, 4);
    r.top = mywin_muldiv_int((int)y, baseY, 8);
    r.right = r.left + mywin_muldiv_int((int)cx, baseX, 4);
    r.bottom = r.top + mywin_muldiv_int((int)cy, baseY, 8);
    return r;
}


static BOOL mywin_client_origin_screen(HWND hWnd, int* outX, int* outY);
static int mywin_nc_client_y_offset(MyWinWindowInfo* wi);

static DWORD _HwndActionForMessageSidecar(const MyMessage* mm)
{
    if (!mm) return _HWND_ACTION_MESSAGE;
    _MsgRouteDescriptor route;
    mymsg_make_route_descriptor(mm, &route);
    return route.hwnd_action ? route.hwnd_action : _HWND_ACTION_MESSAGE;
}

static int mywin_wheel_lines(WPARAM wParam);
static HWND mywin_dialog_current_focus(HWND hDlg);
static LRESULT CALLBACK MyMdiClientWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
static BOOL mywin_mdi_is_client(HWND hWnd);
static BOOL mywin_mdi_is_child(HWND hWnd);
static BOOL mywin_mdi_activate_child(HWND hClient, HWND hChild);

static MyWinDialogInfo* mywin_find_dialog(HWND hDlg)
{
    for (int i = 0; i < MYWIN_MAX_DIALOGS; i++)
        if (g_DialogInfos[i].valid && g_DialogInfos[i].hDlg == hDlg) return &g_DialogInfos[i];
    return NULL;
}

static MyWinDialogInfo* mywin_alloc_dialog(HWND hDlg)
{
    MyWinDialogInfo* old = mywin_find_dialog(hDlg);
    if (old) return old;
    for (int i = 0; i < MYWIN_MAX_DIALOGS; i++) {
        if (!g_DialogInfos[i].valid) {
            memset(&g_DialogInfos[i], 0, sizeof(g_DialogInfos[i]));
            g_DialogInfos[i].valid = 1;
            g_DialogInfos[i].hDlg = hDlg;
            return &g_DialogInfos[i];
        }
    }
    return NULL;
}

static void mywin_free_dialog(HWND hDlg)
{
    for (int i = 0; i < MYWIN_MAX_DIALOGS; i++) {
        if (g_DialogInfos[i].valid && g_DialogInfos[i].hDlg == hDlg) {
            if (g_DialogInfos[i].modeless)
                g_ModelessUnregisterCount++;
            memset(&g_DialogInfos[i], 0, sizeof(g_DialogInfos[i]));
            return;
        }
    }
}

static DWORD mywin_modeless_dialog_count(void)
{
    DWORD n = 0;
    for (int i = 0; i < MYWIN_MAX_DIALOGS; ++i) {
        MyWinDialogInfo* di = &g_DialogInfos[i];
        if (!di->valid || !di->modeless || di->ended || !di->hDlg) continue;
        if (!IsWindow(di->hDlg)) continue;
        ++n;
    }
    return n;
}

static HWND mywin_modeless_dialog_from_message(const MSG* lpMsg)
{
    if (!lpMsg) return 0;
    HWND target = lpMsg->hwnd;

    /* Prefer the dialog tree that owns the message target.  Real Win32 apps
       normally call IsDialogMessage for every modeless dialog from their pump;
       in myOS the desktop/child pumps can only see the MSG, so the registry
       resolves the owning #32770 before dispatch. */
    for (int i = MYWIN_MAX_DIALOGS - 1; i >= 0; --i) {
        MyWinDialogInfo* di = &g_DialogInfos[i];
        if (!di->valid || !di->modeless || di->ended || !di->hDlg) continue;
        if (!IsWindow(di->hDlg)) continue;
        if (target && (target == di->hDlg || IsChild(di->hDlg, target)))
            return di->hDlg;
    }

    /* If the message is thread-wide or targets the owner while focus is inside
       a modeless dialog, route keyboard messages to that focused modeless
       dialog.  This mirrors the practical app-pump pattern while avoiding
       modal owner disable semantics. */
    HWND focus = GetFocus();
    if (focus) {
        for (int i = MYWIN_MAX_DIALOGS - 1; i >= 0; --i) {
            MyWinDialogInfo* di = &g_DialogInfos[i];
            if (!di->valid || !di->modeless || di->ended || !di->hDlg) continue;
            if (!IsWindow(di->hDlg)) continue;
            if (focus == di->hDlg || IsChild(di->hDlg, focus))
                return di->hDlg;
        }
    }
    return 0;
}

static inline BOOL mywin_decode_hwnd_slot_fast(HWND hWnd, DWORD* outSlot)
{
    if (MYOS_UNLIKELY((hWnd & _HWND_TAG_MASK) != _HWND_TAG)) return FALSE;
    DWORD encodedSlot = (DWORD)(hWnd & _HWND_SLOT_MASK);
    if (MYOS_UNLIKELY(encodedSlot == 0 || encodedSlot > MYWIN_MAX_WINDOW_INFOS)) return FALSE;
    DWORD gen = (DWORD)((hWnd & _HWND_GEN_MASK) >> _HWND_GEN_SHIFT);
    if (MYOS_UNLIKELY(gen == 0)) return FALSE;
    if (outSlot) *outSlot = encodedSlot - 1u;
    return TRUE;
}

static inline MyWinWindowInfo* mywin_find_info(HWND hWnd)
{
    DWORD slot = 0;
    if (MYOS_LIKELY(mywin_decode_hwnd_slot_fast(hWnd, &slot))) {
        MyWinWindowInfo* wi = &g_WindowInfos[slot];
        if (MYOS_LIKELY(wi->valid && wi->hWnd == hWnd)) return wi;
        return NULL;
    }
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; i++)
        if (g_WindowInfos[i].valid && g_WindowInfos[i].hWnd == hWnd) return &g_WindowInfos[i];
    return NULL;
}

/* v240/v244: USER32 keeps intrusive child lists per parent and, now, an
   intrusive root list for parent==NULL.  Common tree operations no longer need
   to scan the whole WindowInfo table for normal top-level windows either. */
static HWND mywin_first_link_for_parent(HWND hParent)
{
    if (!hParent) return g_RootFirstWindow;
    MyWinWindowInfo* parent = mywin_find_info(hParent);
    return parent ? parent->firstChild : 0;
}

static void mywin_unlink_child_from_parent(MyWinWindowInfo* wi)
{
    if (!wi) return;

    MyWinWindowInfo* parent = wi->hParent ? mywin_find_info(wi->hParent) : NULL;
    MyWinWindowInfo* prev = mywin_find_info(wi->prevSibling);
    MyWinWindowInfo* next = mywin_find_info(wi->nextSibling);

    if (prev) prev->nextSibling = wi->nextSibling;
    else if (parent && parent->firstChild == wi->hWnd) parent->firstChild = wi->nextSibling;
    else if (!wi->hParent && g_RootFirstWindow == wi->hWnd) g_RootFirstWindow = wi->nextSibling;

    if (next) next->prevSibling = wi->prevSibling;
    else if (parent && parent->lastChild == wi->hWnd) parent->lastChild = wi->prevSibling;
    else if (!wi->hParent && g_RootLastWindow == wi->hWnd) g_RootLastWindow = wi->prevSibling;

    wi->prevSibling = 0;
    wi->nextSibling = 0;
}

static void mywin_link_child_tail(MyWinWindowInfo* wi, HWND hParent)
{
    if (!wi) return;
    wi->hParent = hParent;
    wi->prevSibling = 0;
    wi->nextSibling = 0;

    if (!hParent) {
        HWND oldTail = g_RootLastWindow;
        MyWinWindowInfo* tail = mywin_find_info(oldTail);
        wi->prevSibling = tail ? oldTail : 0;
        wi->nextSibling = 0;
        if (tail) tail->nextSibling = wi->hWnd;
        else g_RootFirstWindow = wi->hWnd;
        g_RootLastWindow = wi->hWnd;
        return;
    }

    MyWinWindowInfo* parent = mywin_find_info(hParent);
    if (!parent || !parent->valid) return;

    HWND oldTail = parent->lastChild;
    MyWinWindowInfo* tail = mywin_find_info(oldTail);
    wi->prevSibling = tail ? oldTail : 0;
    wi->nextSibling = 0;
    if (tail) tail->nextSibling = wi->hWnd;
    else parent->firstChild = wi->hWnd;
    parent->lastChild = wi->hWnd;
}

static void mywin_set_parent_linked(MyWinWindowInfo* wi, HWND hParent)
{
    if (!wi) return;
    if (wi->hParent == hParent) {
        if (hParent || wi->prevSibling || wi->nextSibling || g_RootFirstWindow == wi->hWnd || g_RootLastWindow == wi->hWnd)
            return;
    }
    mywin_unlink_child_from_parent(wi);
    wi->hParent = 0;
    mywin_link_child_tail(wi, hParent);
}

static void mywin_free_window_cold_buffers(MyWinWindowInfo* wi);

static int mywin_alloc_window_cold_buffers(MyWinWindowInfo* wi)
{
    if (!wi) return 0;
    wi->className = (char*)calloc(MYWIN_WINDOW_CLASS_CHARS, 1);
    wi->text = (char*)calloc(MYWIN_WINDOW_TEXT_CHARS, 1);
    wi->control = (MyWinWindowControlState*)calloc(1, sizeof(*wi->control));
    wi->mdi = (MyWinWindowMdiState*)calloc(1, sizeof(*wi->mdi));
    if (wi->control) {
        wi->control->ccItems = (char (*)[MYWIN_CC_TEXT_CHARS])calloc(MYWIN_CC_MAX_ITEMS, sizeof(*wi->control->ccItems));
        wi->control->ccItemData = (LPARAM*)calloc(MYWIN_CC_MAX_ITEMS, sizeof(*wi->control->ccItemData));
        wi->control->ccSel = (unsigned char*)calloc(MYWIN_CC_MAX_ITEMS, sizeof(*wi->control->ccSel));
    }
    if (wi->className && wi->text && wi->control && wi->mdi &&
        wi->control->ccItems && wi->control->ccItemData && wi->control->ccSel)
        return 1;
    mywin_free_window_cold_buffers(wi);
    return 0;
}

static void mywin_free_window_cold_buffers(MyWinWindowInfo* wi)
{
    if (!wi) return;
    free(wi->className);
    free(wi->text);
    if (wi->control) {
        free(wi->control->ccItems);
        free(wi->control->ccItemData);
        free(wi->control->ccSel);
    }
    free(wi->control);
    free(wi->mdi);
    wi->className = NULL;
    wi->text = NULL;
    wi->control = NULL;
    wi->mdi = NULL;
}


static MyWinWindowInfo* mywin_alloc_info(HWND hWnd)
{
    MyWinWindowInfo* old = mywin_find_info(hWnd);
    if (old) return old;

    DWORD slot = 0, gen = 0;
    if (hwnd_decode(hWnd, &slot, &gen) && slot < MYWIN_MAX_WINDOW_INFOS) {
        MyWinWindowInfo* wi = &g_WindowInfos[slot];
        if (!wi->valid) {
            memset(wi, 0, sizeof(*wi));
            if (!mywin_alloc_window_cold_buffers(wi)) {
                memset(wi, 0, sizeof(*wi));
                return NULL;
            }
            wi->valid = 1;
            wi->hWnd = hWnd;
            wi->zOrder = g_NextWindowZOrder++;
            if (!wi->zOrder) wi->zOrder = g_NextWindowZOrder++;
            return wi;
        }
    }

    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; i++) {
        if (!g_WindowInfos[i].valid) {
            memset(&g_WindowInfos[i], 0, sizeof(g_WindowInfos[i]));
            if (!mywin_alloc_window_cold_buffers(&g_WindowInfos[i])) {
                memset(&g_WindowInfos[i], 0, sizeof(g_WindowInfos[i]));
                return NULL;
            }
            g_WindowInfos[i].valid = 1;
            g_WindowInfos[i].hWnd = hWnd;
            g_WindowInfos[i].zOrder = g_NextWindowZOrder++;
            if (!g_WindowInfos[i].zOrder) g_WindowInfos[i].zOrder = g_NextWindowZOrder++;
            return &g_WindowInfos[i];
        }
    }
    return NULL;
}

static void mywin_free_info(HWND hWnd)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return;
    mywin_unlink_child_from_parent(wi);
    wi->firstChild = 0;
    wi->lastChild = 0;
    if (wi->wndExtra) {
        free(wi->wndExtra);
        wi->wndExtra = NULL;
    }
    mywin_free_window_cold_buffers(wi);
    memset(wi, 0, sizeof(*wi));
}

static BOOL mywin_is_class(HWND hWnd, LPCSTR cls)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    return (wi && cls && strcmp(wi->className, cls) == 0) ? TRUE : FALSE;
}

static BOOL mywin_pt_in_rect(const RECT* r, int x, int y)
{
    return (r && x >= r->left && x < r->right && y >= r->top && y < r->bottom) ? TRUE : FALSE;
}

static inline int mywin_info_slot(HWND hWnd)
{
    DWORD slot = 0;
    if (MYOS_LIKELY(mywin_decode_hwnd_slot_fast(hWnd, &slot)))
        return (g_WindowInfos[slot].valid && g_WindowInfos[slot].hWnd == hWnd) ? (int)slot : -1;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i)
        if (g_WindowInfos[i].valid && g_WindowInfos[i].hWnd == hWnd) return i;
    return -1;
}

static int mywin_z_compare(HWND a, HWND b)
{
    MyWinWindowInfo* ai = mywin_find_info(a);
    MyWinWindowInfo* bi = mywin_find_info(b);
    DWORD az = ai ? ai->zOrder : 0;
    DWORD bz = bi ? bi->zOrder : 0;
    if (az < bz) return -1;
    if (az > bz) return 1;
    int as = mywin_info_slot(a);
    int bs = mywin_info_slot(b);
    if (as < bs) return -1;
    if (as > bs) return 1;
    return 0;
}

static void mywin_sort_hwnds_bottom_to_top(HWND* hwnds, int count)
{
    if (!hwnds || count <= 1) return;
    for (int i = 1; i < count; ++i) {
        HWND key = hwnds[i];
        int j = i - 1;
        while (j >= 0 && mywin_z_compare(hwnds[j], key) > 0) {
            hwnds[j + 1] = hwnds[j];
            --j;
        }
        hwnds[j + 1] = key;
    }
}

static void mywin_raise_window_to_top(HWND hWnd)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi || !wi->valid) return;
    wi->zOrder = g_NextWindowZOrder++;
    if (!wi->zOrder) wi->zOrder = g_NextWindowZOrder++;
}


static DWORD mywin_next_zorder_after_siblings(HWND hWnd, HWND hWndInsertAfter, UINT flags)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi || !wi->valid || (flags & SWP_NOZORDER)) return wi ? wi->zOrder : 0;

    HWND parent = wi->hParent;
    if (hWndInsertAfter == HWND_TOP || hWndInsertAfter == 0 || hWndInsertAfter == HWND_TOPMOST || hWndInsertAfter == HWND_NOTOPMOST) {
        return g_NextWindowZOrder++;
    }

    if (hWndInsertAfter == HWND_BOTTOM) {
        DWORD minz = 0;
        for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
            MyWinWindowInfo* sib = &g_WindowInfos[i];
            if (!sib->valid || sib->hWnd == hWnd || sib->hParent != parent) continue;
            if (!minz || sib->zOrder < minz) minz = sib->zOrder;
        }
        if (!minz || minz > 1) return minz ? minz - 1 : 1;
        /* Renormalize sibling band upward to keep room at the bottom. */
        for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
            MyWinWindowInfo* sib = &g_WindowInfos[i];
            if (sib->valid && sib->hParent == parent) sib->zOrder += 1024u;
        }
        g_NextWindowZOrder += 1024u;
        return 1;
    }

    MyWinWindowInfo* after = mywin_find_info(hWndInsertAfter);
    if (!after || !after->valid || after->hParent != parent) {
        /* Win32 rejects foreign/invalid insert-after handles. */
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    /* Insert just above hWndInsertAfter.  If another sibling already occupies
       that slot, renormalize all siblings bottom->top and retry deterministically. */
    DWORD target = after->zOrder + 1u;
    int collision = 0;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* sib = &g_WindowInfos[i];
        if (sib->valid && sib->hWnd != hWnd && sib->hParent == parent && sib->zOrder == target) { collision = 1; break; }
    }
    if (!collision) return target;

    HWND siblings[MYWIN_MAX_WINDOW_INFOS];
    int n = 0;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS && n < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* sib = &g_WindowInfos[i];
        if (sib->valid && sib->hParent == parent && sib->hWnd != hWnd) siblings[n++] = sib->hWnd;
    }
    mywin_sort_hwnds_bottom_to_top(siblings, n);
    DWORD z = 1024u;
    for (int i = 0; i < n; ++i) {
        MyWinWindowInfo* sib = mywin_find_info(siblings[i]);
        if (sib) { sib->zOrder = z; z += 1024u; }
    }
    g_NextWindowZOrder = z + 1024u;
    after = mywin_find_info(hWndInsertAfter);
    return after ? after->zOrder + 1u : g_NextWindowZOrder++;
}

static BOOL mywin_apply_local_zorder(HWND hWnd, HWND hWndInsertAfter, UINT flags)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi || !wi->valid || (flags & SWP_NOZORDER)) return TRUE;
    DWORD newZ = mywin_next_zorder_after_siblings(hWnd, hWndInsertAfter, flags);
    if (!newZ) return FALSE;
    wi->zOrder = newZ;
    if (g_NextWindowZOrder <= newZ) g_NextWindowZOrder = newZ + 1u;
    return TRUE;
}

static BOOL mywin_window_own_visible(const MyWinWindowInfo* wi)
{
    return (wi && wi->valid && (wi->style & WS_VISIBLE)) ? TRUE : FALSE;
}

static BOOL mywin_is_visible_chain(HWND hWnd)
{
    if (!hWnd || !IsWindow(hWnd)) return FALSE;
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi || !mywin_window_own_visible(wi)) return FALSE;
    HWND p = wi->hParent;
    while (p) {
        MyWinWindowInfo* pi = mywin_find_info(p);
        if (!pi || !pi->valid || !(pi->style & WS_VISIBLE)) return FALSE;
        p = pi->hParent;
    }
    return TRUE;
}

static int mywin_collect_children(HWND hWndParent, HWND* lpChildren, int cMaxChildren)
{
    int count = 0;
    if (!lpChildren || cMaxChildren <= 0) return 0;

    /* v241/v244: collect from the intrusive child/root list first, then keep
       the old bottom->top z-sort.  That preserves public GetWindow/Enum order
       while changing common tree walks from O(window-table) to O(children). */
    {
        HWND cur = mywin_first_link_for_parent(hWndParent);
        if (cur || !hWndParent) {
            int guard = 0;
            BOOL linkedOk = TRUE;
            while (cur && count < cMaxChildren && guard++ < MYWIN_MAX_WINDOW_INFOS) {
                MyWinWindowInfo* child = mywin_find_info(cur);
                if (MYOS_UNLIKELY(!child || !child->valid || child->hParent != hWndParent)) { linkedOk = FALSE; break; }
                lpChildren[count++] = child->hWnd;
                cur = child->nextSibling;
            }
            if (MYOS_UNLIKELY(cur != 0)) linkedOk = FALSE;
            if (MYOS_LIKELY(linkedOk)) {
                mywin_sort_hwnds_bottom_to_top(lpChildren, count);
                return count;
            }
            count = 0;
        }
    }

    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS && count < cMaxChildren; i++) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (wi->valid && wi->hParent == hWndParent)
            lpChildren[count++] = wi->hWnd;
    }
    mywin_sort_hwnds_bottom_to_top(lpChildren, count);
    return count;
}

static int mywin_collect_children_linked_order(HWND hWndParent, HWND* lpChildren, int cMaxChildren)
{
    /* v240: Dialog tab/group order follows sibling creation/link order. */
    int count = 0;
    if (!lpChildren || cMaxChildren <= 0) return 0;

    {
        HWND cur = mywin_first_link_for_parent(hWndParent);
        if (cur || !hWndParent) {
            int guard = 0;
            while (cur && count < cMaxChildren && guard++ < MYWIN_MAX_WINDOW_INFOS) {
                MyWinWindowInfo* child = mywin_find_info(cur);
                if (!child || !child->valid || child->hParent != hWndParent) break;
                lpChildren[count++] = child->hWnd;
                cur = child->nextSibling;
            }
            if (!cur || count >= cMaxChildren)
                return count;
            count = 0;
        }
    }

    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS && count < cMaxChildren; i++) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (wi->valid && wi->hParent == hWndParent)
            lpChildren[count++] = wi->hWnd;
    }
    return count;
}

static HWND mywin_top_child_unchecked(HWND hWndParent)
{
    HWND best = 0;
    BOOL linkedOk = FALSE;
    {
        HWND cur = mywin_first_link_for_parent(hWndParent);
        if (cur || !hWndParent) {
            linkedOk = TRUE;
            int guard = 0;
            while (cur && guard++ < MYWIN_MAX_WINDOW_INFOS) {
                MyWinWindowInfo* child = mywin_find_info(cur);
                if (MYOS_UNLIKELY(!child || !child->valid || child->hParent != hWndParent)) { linkedOk = FALSE; break; }
                if (!best || mywin_z_compare(child->hWnd, best) > 0) best = child->hWnd;
                cur = child->nextSibling;
            }
            if (MYOS_UNLIKELY(cur != 0)) linkedOk = FALSE;
            if (MYOS_LIKELY(linkedOk)) return best;
            best = 0;
        }
    }

    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (!wi->valid || wi->hParent != hWndParent) continue;
        if (!best || mywin_z_compare(wi->hWnd, best) > 0) best = wi->hWnd;
    }
    return best;
}

static HWND mywin_find_window_ex_best(HWND hWndParent, HWND hWndChildAfter, LPCSTR lpClassName, LPCSTR lpWindowName)
{
    HWND best = 0;
    BOOL linkedOk = FALSE;
    BOOL classAtomFilter = (lpClassName && mywin_is_int_atom_class(lpClassName)) ? TRUE : FALSE;
    ATOM classAtomFilterValue = classAtomFilter ? (ATOM)(uintptr_t)lpClassName : 0;
    DWORD classFilterHash = (lpClassName && !classAtomFilter) ? mywin_class_name_hash(lpClassName) : 0;
    DWORD titleFilterHash = lpWindowName ? mywin_title_hash(lpWindowName) : 0;

#define MYWIN_CONSIDER_FIND_CHILD(wi_) do { \
        MyWinWindowInfo* _wi = (wi_); \
        if (!_wi || !_wi->valid || _wi->hParent != hWndParent) break; \
        if (hWndChildAfter && mywin_z_compare(_wi->hWnd, hWndChildAfter) >= 0) break; \
        if (lpClassName) { \
            if (classAtomFilter) { if (_wi->classAtom != classAtomFilterValue) break; } \
            else { if (_wi->classNameHash != classFilterHash || strcmp(_wi->className, lpClassName) != 0) break; } \
        } \
        if (lpWindowName && (_wi->textHash != titleFilterHash || strcmp(_wi->text, lpWindowName) != 0)) break; \
        if (!mywin_can_read_window(_wi->hWnd)) break; \
        if (!best || mywin_z_compare(_wi->hWnd, best) > 0) best = _wi->hWnd; \
    } while (0)

    {
        HWND cur = mywin_first_link_for_parent(hWndParent);
        if (cur || !hWndParent) {
            linkedOk = TRUE;
            int guard = 0;
            while (cur && guard++ < MYWIN_MAX_WINDOW_INFOS) {
                MyWinWindowInfo* child = mywin_find_info(cur);
                if (MYOS_UNLIKELY(!child || !child->valid || child->hParent != hWndParent)) { linkedOk = FALSE; break; }
                MYWIN_CONSIDER_FIND_CHILD(child);
                cur = child->nextSibling;
            }
            if (MYOS_UNLIKELY(cur != 0)) linkedOk = FALSE;
            if (MYOS_LIKELY(linkedOk)) {
#undef MYWIN_CONSIDER_FIND_CHILD
                return best;
            }
            best = 0;
        }
    }

#define MYWIN_CONSIDER_FIND_CHILD(wi_) do { \
        MyWinWindowInfo* _wi = (wi_); \
        if (!_wi || !_wi->valid || _wi->hParent != hWndParent) break; \
        if (hWndChildAfter && mywin_z_compare(_wi->hWnd, hWndChildAfter) >= 0) break; \
        if (lpClassName) { \
            if (classAtomFilter) { if (_wi->classAtom != classAtomFilterValue) break; } \
            else { if (_wi->classNameHash != classFilterHash || strcmp(_wi->className, lpClassName) != 0) break; } \
        } \
        if (lpWindowName && (_wi->textHash != titleFilterHash || strcmp(_wi->text, lpWindowName) != 0)) break; \
        if (!mywin_can_read_window(_wi->hWnd)) break; \
        if (!best || mywin_z_compare(_wi->hWnd, best) > 0) best = _wi->hWnd; \
    } while (0)
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i)
        MYWIN_CONSIDER_FIND_CHILD(&g_WindowInfos[i]);
#undef MYWIN_CONSIDER_FIND_CHILD
    return best;
}

static void mywin_invalidate_window_and_children(HWND hWnd, BOOL erase)
{
    if (!hWnd || !IsWindow(hWnd)) return;
    InvalidateRect(hWnd, NULL, erase);
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int cnt = mywin_collect_children(hWnd, children, MYWIN_MAX_WINDOW_INFOS);
    for (int i = 0; i < cnt; ++i) {
        if (children[i] && IsWindow(children[i]))
            InvalidateRect(children[i], NULL, erase);
    }
}

static BOOL mywin_is_descendant(HWND hWndParent, HWND hWnd)
{
    HWND cur = GetParent(hWnd);
    while (cur) {
        if (cur == hWndParent) return TRUE;
        cur = GetParent(cur);
    }
    return FALSE;
}

static BOOL mywin_post_control_command(HWND hControl, HWND hParent, UINT id, UINT code)
{
    if (!g_lpHwndManager || !hControl || !hParent) return FALSE;

    /*
       v77.1: Built-in controls must not use the ambient thread-local
       g_CurrentCapability when they notify their parent.  The ambient runtime
       may currently belong to another app or to the session/state setup code.
       Real Win32 controls post WM_COMMAND as the control/window owner, not as
       whatever code last called MyWinBindRuntime().
    */
    DWORD ownerPid = hwnd_get_owner_pid(g_lpHwndManager, hControl);
    if (!ownerPid) ownerPid = hwnd_get_owner_pid(g_lpHwndManager, hParent);
    if (!ownerPid) ownerPid = g_HasCapability ? g_CurrentCapability.id : 0;
    if (!ownerPid) return FALSE;

    Capability ownerCap = cap_create(ownerPid, "button-owner", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&ownerCap, 0);
    return hwnd_post(g_lpHwndManager, &ownerCap, hParent, WM_COMMAND,
                     MAKEWPARAM((WORD)id, (WORD)code), (LPARAM)hControl) == 0;
}

static BOOL mywin_post_control_scroll(HWND hControl, HWND hParent, UINT msg, UINT code, int pos)
{
    if (!g_lpHwndManager || !hControl || !hParent) return FALSE;
    DWORD ownerPid = hwnd_get_owner_pid(g_lpHwndManager, hControl);
    if (!ownerPid) ownerPid = hwnd_get_owner_pid(g_lpHwndManager, hParent);
    if (!ownerPid) ownerPid = g_HasCapability ? g_CurrentCapability.id : 0;
    if (!ownerPid) return FALSE;
    Capability ownerCap = cap_create(ownerPid, "scrollbar-owner", CAP_IPC|CAP_WINDOW_READ|CAP_WINDOW_CONTROL);
    cap_add_target(&ownerCap, 0);
    return hwnd_post(g_lpHwndManager, &ownerCap, hParent, msg,
                     MAKEWPARAM((WORD)code, (WORD)pos), (LPARAM)hControl) == 0;
}


static LRESULT mywin_def_text_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    switch (Msg) {
    case WM_GETTEXTLENGTH:
        return (LRESULT)strlen(wi->text);
    case WM_GETTEXT: {
        LPSTR out = (LPSTR)lParam;
        int maxc = (int)wParam;
        if (!out || maxc <= 0) return 0;
        snprintf(out, (size_t)maxc, "%s", wi->text);
        return (LRESULT)strlen(out);
    }
    case WM_SETTEXT: {
        LPCSTR text = (LPCSTR)lParam;
        snprintf(wi->text, MYWIN_WINDOW_TEXT_CHARS, "%s", text ? text : "");
        wi->textHash = mywin_title_hash(wi->text);
        if (g_HasCapability)
            hwnd_post(g_lpHwndManager, &g_CurrentCapability, hWnd, WM_WINDOWTEXTCHANGED, 0, 0);
        return TRUE;
    }
    default:
        return 0;
    }
}

static void mywin_notify_parent(HWND hControl, UINT code)
{
    MyWinWindowInfo* wi = mywin_find_info(hControl);
    if (wi && wi->hParent)
        mywin_post_control_command(hControl, wi->hParent, wi->id, code);
}

static UINT mywin_button_type_from_style(DWORD style)
{
    return (UINT)(style & BS_TYPEMASK);
}

static UINT mywin_button_type(const MyWinWindowInfo* wi)
{
    return wi ? mywin_button_type_from_style(wi->style) : BS_PUSHBUTTON;
}

static BOOL mywin_button_is_groupbox_type(UINT type)
{
    return type == BS_GROUPBOX ? TRUE : FALSE;
}

static BOOL mywin_button_is_check_type(UINT type)
{
    return (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
            type == BS_3STATE || type == BS_AUTO3STATE) ? TRUE : FALSE;
}

static BOOL mywin_button_is_radio_type(UINT type)
{
    return (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) ? TRUE : FALSE;
}

static BOOL mywin_button_is_push_type(UINT type)
{
    return (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) ? TRUE : FALSE;
}

static HWND mywin_dialog_from_descendant(HWND hWnd)
{
    HWND cur = hWnd;
    int guard = 0;
    while (cur && guard++ < MYWIN_MAX_WINDOW_INFOS) {
        MyWinWindowInfo* wi = mywin_find_info(cur);
        if (!wi) break;
        if (strcmp(wi->className, "#32770") == 0) return cur;
        cur = wi->hParent;
    }
    return 0;
}

static BOOL mywin_is_dlg_tabstop(MyWinWindowInfo* wi);

static BOOL mywin_button_is_focusable(const MyWinWindowInfo* wi)
{
    if (!wi || !wi->valid || (wi->style & WS_DISABLED) || !(wi->style & WS_VISIBLE)) return FALSE;
    return mywin_button_is_groupbox_type(mywin_button_type(wi)) ? FALSE : TRUE;
}


static BOOL mywin_is_radio_hwnd(HWND hWnd)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    return (wi && strcmp(wi->className, "BUTTON") == 0 && mywin_button_is_radio_type(mywin_button_type(wi))) ? TRUE : FALSE;
}

static HWND __attribute__((unused)) mywin_first_tabstop_in_group(HWND hDlg, HWND hAnyInGroup)
{
    if (!hDlg) return 0;
    HWND items[MYWIN_MAX_WINDOW_INFOS];
    int n = 0, cur = -1;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS && n < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (!wi->valid || wi->hParent != hDlg) continue;
        if (!(wi->style & WS_VISIBLE) || (wi->style & WS_DISABLED)) continue;
        if (wi->hWnd == hAnyInGroup) cur = n;
        items[n++] = wi->hWnd;
    }
    if (n <= 0) return 0;
    if (cur < 0) cur = 0;
    int start = 0, end = n - 1;
    for (int i = cur; i >= 0; --i) {
        MyWinWindowInfo* wi = mywin_find_info(items[i]);
        if (wi && (wi->style & WS_GROUP)) { start = i; break; }
    }
    for (int i = cur + 1; i < n; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(items[i]);
        if (wi && (wi->style & WS_GROUP)) { end = i - 1; break; }
    }
    for (int i = start; i <= end; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(items[i]);
        if (wi && mywin_is_dlg_tabstop(wi)) return items[i];
    }
    return 0;
}

static BOOL mywin_dialog_button_id_is_push(HWND hDlg, int id)
{
    HWND h = GetDlgItem(hDlg, id);
    MyWinWindowInfo* wi = mywin_find_info(h);
    if (!wi || strcmp(wi->className, "BUTTON") != 0) return FALSE;
    if (!(wi->style & WS_VISIBLE) || (wi->style & WS_DISABLED)) return FALSE;
    return mywin_button_is_push_type(mywin_button_type(wi));
}

static void mywin_dialog_apply_default_visual(HWND hDlg, int id)
{
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (!wi->valid || wi->hParent != hDlg || strcmp(wi->className, "BUTTON") != 0) continue;
        UINT type = mywin_button_type(wi);
        if (!mywin_button_is_push_type(type)) continue;

        UINT newType = ((int)wi->id == id && id != 0 && !(wi->style & WS_DISABLED) && (wi->style & WS_VISIBLE))
            ? BS_DEFPUSHBUTTON
            : BS_PUSHBUTTON;
        if (type != newType) {
            wi->style = (wi->style & ~BS_TYPEMASK) | newType;
            InvalidateRect(wi->hWnd, NULL, TRUE);
        }
    }
}

static void mywin_dialog_update_default_for_focus(HWND hDlg, HWND hFocus)
{
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    if (!di) return;

    int visualId = di->defId;
    di->focusDefId = 0;

    MyWinWindowInfo* wi = mywin_find_info(hFocus);
    if (wi && wi->hParent == hDlg && strcmp(wi->className, "BUTTON") == 0 &&
        !(wi->style & WS_DISABLED) && (wi->style & WS_VISIBLE) &&
        mywin_button_is_push_type(mywin_button_type(wi))) {
        visualId = (int)wi->id;
        di->focusDefId = visualId;
    }

    mywin_dialog_apply_default_visual(hDlg, visualId);
}

static HWND mywin_dialog_get_default_button(HWND hDlg)
{
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    if (di && di->focusDefId && mywin_dialog_button_id_is_push(hDlg, di->focusDefId)) {
        HWND h = GetDlgItem(hDlg, di->focusDefId);
        if (h) return h;
    }
    if (di && di->defId && mywin_dialog_button_id_is_push(hDlg, di->defId)) {
        HWND h = GetDlgItem(hDlg, di->defId);
        if (h) return h;
    }
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (!wi->valid || wi->hParent != hDlg || strcmp(wi->className, "BUTTON") != 0) continue;
        if ((wi->style & WS_VISIBLE) && !(wi->style & WS_DISABLED) && mywin_button_type(wi) == BS_DEFPUSHBUTTON)
            return wi->hWnd;
    }
    HWND ok = GetDlgItem(hDlg, IDOK);
    if (ok && mywin_dialog_button_id_is_push(hDlg, IDOK)) return ok;
    return 0;
}

static int mywin_dialog_get_default_id(HWND hDlg)
{
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    if (di && di->focusDefId && mywin_dialog_button_id_is_push(hDlg, di->focusDefId)) return di->focusDefId;
    if (di && di->defId) return di->defId;
    HWND h = mywin_dialog_get_default_button(hDlg);
    MyWinWindowInfo* wi = mywin_find_info(h);
    return wi ? (int)wi->id : 0;
}

static void mywin_dialog_apply_default_id(HWND hDlg, int id)
{
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    if (di) {
        di->defId = id;
        di->focusDefId = 0;
    }
    HWND cur = mywin_dialog_current_focus(hDlg);
    if (cur && cur != hDlg) mywin_dialog_update_default_for_focus(hDlg, cur);
    else mywin_dialog_apply_default_visual(hDlg, id);
}

static UINT mywin_get_dlg_code(HWND hWnd, MSG* msg)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi || !wi->wndproc) return 0;
    return (UINT)wi->wndproc(hWnd, WM_GETDLGCODE, msg ? msg->wParam : 0, (LPARAM)msg);
}

static HWND mywin_radio_checked_in_group(HWND hDlg, HWND hAnyInGroup)
{
    if (!hDlg || !hAnyInGroup) return 0;
    HWND items[MYWIN_MAX_WINDOW_INFOS];
    int n = 0, cur = -1;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS && n < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (!wi->valid || wi->hParent != hDlg) continue;
        if (!(wi->style & WS_VISIBLE) || (wi->style & WS_DISABLED)) continue;
        if (wi->hWnd == hAnyInGroup) cur = n;
        items[n++] = wi->hWnd;
    }
    if (n <= 0) return 0;
    if (cur < 0) cur = 0;
    int start = 0, end = n - 1;
    for (int i = cur; i >= 0; --i) {
        MyWinWindowInfo* wi = mywin_find_info(items[i]);
        if (wi && (wi->style & WS_GROUP)) { start = i; break; }
    }
    for (int i = cur + 1; i < n; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(items[i]);
        if (wi && (wi->style & WS_GROUP)) { end = i - 1; break; }
    }
    HWND firstRadio = 0;
    for (int i = start; i <= end; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(items[i]);
        if (!wi || strcmp(wi->className, "BUTTON") != 0) continue;
        if (!mywin_button_is_radio_type(mywin_button_type(wi))) continue;
        if (!firstRadio) firstRadio = items[i];
        if (wi->control->checkState == BST_CHECKED) return items[i];
    }
    return firstRadio;
}

static BOOL mywin_radio_is_tab_candidate(HWND hDlg, HWND hRadio)
{
    if (!hDlg || !mywin_is_radio_hwnd(hRadio)) return TRUE;
    /* Win32 dialog navigation treats an auto-radio group as one tab stop.
       The checked radio receives Tab focus; if none is checked, the first
       radio in the WS_GROUP range receives it.  Without this filter, Tab can
       get trapped because every radio button in our DLGTEMPLATEs commonly has
       WS_TABSTOP. */
    HWND representative = mywin_radio_checked_in_group(hDlg, hRadio);
    return (!representative || representative == hRadio) ? TRUE : FALSE;
}

static int mywin_mnemonic_match_pos(const char* text, char needleLower)
{
    if (!text || !needleLower) return -1;
    int visible = 0;
    for (int i = 0; text[i]; ++i) {
        if (text[i] == '&') {
            if (text[i + 1] == '&') { visible++; i++; continue; }
            if (text[i + 1]) {
                char c = (char)tolower((unsigned char)text[i + 1]);
                if (c == needleLower) return visible;
                continue;
            }
        }
        visible++;
    }
    return -1;
}

static void mywin_strip_mnemonics(const char* src, char* dst, size_t dstSize)
{
    if (!dst || dstSize == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dstSize; ++i) {
        if (src[i] == '&') {
            if (src[i + 1] == '&') { dst[out++] = '&'; i++; }
            else if (src[i + 1]) { continue; }
            else { dst[out++] = '&'; }
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = 0;
}

static void mywin_draw_text_mnemonic(Framebuffer* fb, int x, int y, const char* text, Color c,
                                     int clip_x, int clip_y, int clip_w, int clip_h)
{
    char out[160];
    int underline = -1;
    if (text) {
        for (int i = 0, visible = 0; text[i]; ++i) {
            if (text[i] == '&') {
                if (text[i + 1] == '&') { visible++; i++; continue; }
                if (text[i + 1] && underline < 0) { underline = visible; continue; }
            }
            visible++;
        }
    }
    mywin_strip_mnemonics(text, out, sizeof(out));
    DrawClipTextA(fb, x, y, out, c, clip_x, clip_y, clip_w, clip_h);
    if (underline >= 0) {
        int ux = x + underline * 8;
        int uy = y + 8;
        if (ux >= clip_x && ux < clip_x + clip_w && uy >= clip_y && uy < clip_y + clip_h)
            fb_rect(fb, ux, uy, 7, 1, c);
    }
}

static HWND mywin_dialog_find_mnemonic(HWND hDlg, char chLower)
{
    if (!hDlg || !chLower) return 0;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (!wi->valid || wi->hParent != hDlg) continue;
        if (!(wi->style & WS_VISIBLE) || (wi->style & WS_DISABLED)) continue;
        if (mywin_mnemonic_match_pos(wi->text, chLower) >= 0)
            return wi->hWnd;
    }
    return 0;
}

static BOOL mywin_dialog_execute_mnemonic(HWND hDlg, char chLower)
{
    HWND hCtl = mywin_dialog_find_mnemonic(hDlg, chLower);
    if (!hCtl) return FALSE;
    MyWinWindowInfo* wi = mywin_find_info(hCtl);
    if (!wi) return FALSE;
    if (strcmp(wi->className, "BUTTON") == 0) {
        UINT type = mywin_button_type(wi);
        if (type == BS_GROUPBOX) {
            HWND next = GetNextDlgTabItem(hDlg, hCtl, FALSE);
            if (next) SetFocus(next);
            return TRUE;
        }
        if (mywin_button_is_focusable(wi)) SetFocus(hCtl);
        SendMessageA(hCtl, BM_CLICK, 0, 0);
        return TRUE;
    }
    if (strcmp(wi->className, "STATIC") == 0) {
        HWND next = GetNextDlgTabItem(hDlg, hCtl, FALSE);
        if (next) SetFocus(next);
        return TRUE;
    }
    if (mywin_is_dlg_tabstop(wi)) {
        SetFocus(hCtl);
        return TRUE;
    }
    return FALSE;
}

static void mywin_button_clear_radio_group(HWND hWnd)
{
    MyWinWindowInfo* cur = mywin_find_info(hWnd);
    if (!cur || !cur->hParent) return;

    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int n = mywin_collect_children_linked_order(cur->hParent, children, MYWIN_MAX_WINDOW_INFOS);
    int idx = -1;
    for (int i = 0; i < n; ++i) if (children[i] == hWnd) { idx = i; break; }
    if (idx < 0) return;

    int start = 0, end = n - 1;
    for (int i = idx; i >= 0; --i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (wi && (wi->style & WS_GROUP)) { start = i; break; }
    }
    for (int i = idx + 1; i < n; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (wi && (wi->style & WS_GROUP)) { end = i - 1; break; }
    }

    for (int i = start; i <= end; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (!wi || children[i] == hWnd || strcmp(wi->className, "BUTTON") != 0) continue;
        if (mywin_button_is_radio_type(mywin_button_type(wi))) {
            wi->control->checkState = BST_UNCHECKED;
            InvalidateRect(children[i], NULL, TRUE);
        }
    }
}

static void mywin_button_sync_radio_group_tabstop(HWND hWnd)
{
    MyWinWindowInfo* cur = mywin_find_info(hWnd);
    if (!cur || !cur->hParent) return;

    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int n = mywin_collect_children_linked_order(cur->hParent, children, MYWIN_MAX_WINDOW_INFOS);
    int idx = -1;
    for (int i = 0; i < n; ++i) if (children[i] == hWnd) { idx = i; break; }
    if (idx < 0) return;

    int start = 0, end = n - 1;
    for (int i = idx; i >= 0; --i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (wi && (wi->style & WS_GROUP)) { start = i; break; }
    }
    for (int i = idx + 1; i < n; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (wi && (wi->style & WS_GROUP)) { end = i - 1; break; }
    }

    for (int i = start; i <= end; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (!wi || strcmp(wi->className, "BUTTON") != 0) continue;
        if (!mywin_button_is_radio_type(mywin_button_type(wi))) continue;
        if (children[i] == hWnd) wi->style |= WS_TABSTOP;
        else wi->style &= ~WS_TABSTOP;
    }
}

static void mywin_button_auto_toggle(HWND hWnd, MyWinWindowInfo* wi)
{
    if (!wi) return;
    UINT type = mywin_button_type(wi);
    switch (type) {
    case BS_AUTOCHECKBOX:
        wi->control->checkState = (wi->control->checkState == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED;
        break;
    case BS_AUTO3STATE:
        if (wi->control->checkState == BST_UNCHECKED) wi->control->checkState = BST_CHECKED;
        else if (wi->control->checkState == BST_CHECKED) wi->control->checkState = BST_INDETERMINATE;
        else wi->control->checkState = BST_UNCHECKED;
        break;
    case BS_AUTORADIOBUTTON:
        if (wi->control->checkState != BST_CHECKED) {
            mywin_button_clear_radio_group(hWnd);
            wi->control->checkState = BST_CHECKED;
            mywin_button_sync_radio_group_tabstop(hWnd);
        }
        break;
    default:
        break;
    }
    InvalidateRect(hWnd, NULL, TRUE);
}

static LRESULT CALLBACK MyButtonWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    UINT type = mywin_button_type(wi);

    switch (Msg) {
    case WM_GETDLGCODE:
        if (mywin_button_is_groupbox_type(type)) return DLGC_STATIC;
        if (mywin_button_is_radio_type(type)) return DLGC_BUTTON | DLGC_RADIOBUTTON;
        return DLGC_BUTTON | ((type == BS_DEFPUSHBUTTON) ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON);
    case WM_GETTEXT:
    case WM_GETTEXTLENGTH:
    case WM_SETTEXT:
        return mywin_def_text_proc(hWnd, Msg, wParam, lParam);
    case WM_ENABLE:
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case WM_SETFOCUS:
        if (!mywin_button_is_groupbox_type(type)) mywin_notify_parent(hWnd, BN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        wi->control->pressed = 0;
        if (!mywin_button_is_groupbox_type(type)) mywin_notify_parent(hWnd, BN_KILLFOCUS);
        return 0;
    case BM_CLICK:
        if (wi->style & WS_DISABLED) return 0;
        if (mywin_button_is_groupbox_type(type)) return 0;
        mywin_button_auto_toggle(hWnd, wi);
        mywin_post_control_command(hWnd, wi->hParent, wi->id, BN_CLICKED);
        return 0;
    case BM_GETSTATE:
        return (wi->control->pressed ? BST_PUSHED : 0) | (GetFocus() == hWnd ? BST_FOCUS : 0);
    case BM_SETSTATE:
        if (!mywin_button_is_groupbox_type(type)) {
            wi->control->pressed = wParam ? 1 : 0;
            InvalidateRect(hWnd, NULL, TRUE);
        }
        return 0;
    case BM_GETCHECK:
        return (LRESULT)wi->control->checkState;
    case BM_SETCHECK:
        if (mywin_button_is_check_type(type) || mywin_button_is_radio_type(type)) {
            wi->control->checkState = (int)wParam;
            if (mywin_button_is_radio_type(type) && wi->control->checkState == BST_CHECKED)
                mywin_button_sync_radio_group_tabstop(hWnd);
            InvalidateRect(hWnd, NULL, TRUE);
        }
        return 0;
    case BM_SETSTYLE:
        /* Preserve high window styles and replace only the BUTTON type/style bits.
           MSDN: lParam != 0 requests redraw after BM_SETSTYLE. */
        wi->style = (wi->style & 0xffff0000u) | ((DWORD)wParam & 0x0000ffffu);
        if (lParam) InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case WM_KEYDOWN:
        if ((wi->style & WS_DISABLED) || mywin_button_is_groupbox_type(type)) return 0;
        if ((int)wParam == KEY_SPACE || (int)wParam == KEY_ENTER) {
            wi->control->pressed = 1;
            InvalidateRect(hWnd, NULL, TRUE);
            if ((int)wParam == KEY_ENTER) {
                wi->control->pressed = 0;
                InvalidateRect(hWnd, NULL, TRUE);
                mywin_button_auto_toggle(hWnd, wi);
                if (wi->hParent) mywin_post_control_command(hWnd, wi->hParent, wi->id, BN_CLICKED);
            }
            return 0;
        }
        return 0;
    case WM_KEYUP:
        if ((wi->style & WS_DISABLED) || mywin_button_is_groupbox_type(type)) return 0;
        if ((int)wParam == KEY_SPACE) {
            int wasPressed = wi->control->pressed;
            wi->control->pressed = 0;
            InvalidateRect(hWnd, NULL, TRUE);
            if (wasPressed && wi->hParent) {
                mywin_button_auto_toggle(hWnd, wi);
                mywin_post_control_command(hWnd, wi->hParent, wi->id, BN_CLICKED);
            }
            return 0;
        }
        return 0;
    case WM_LBUTTONDOWN:
        if ((wi->style & WS_DISABLED) || mywin_button_is_groupbox_type(type)) return 0;
        SetFocus(hWnd);
        wi->control->pressed = 1;
        InvalidateRect(hWnd, NULL, TRUE);
        SetCapture(hWnd);
        return 0;
    case WM_LBUTTONUP: {
        if (mywin_button_is_groupbox_type(type)) return 0;
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        int bw = (int)(wi->rcClient.right - wi->rcClient.left);
        int bh = (int)(wi->rcClient.bottom - wi->rcClient.top);
        BOOL inside = (x >= 0 && y >= 0 && x < bw && y < bh);
        int wasPressed = wi->control->pressed;
        wi->control->pressed = 0;
        InvalidateRect(hWnd, NULL, TRUE);
        if (GetCapture() == hWnd) ReleaseCapture();
        if (wasPressed && inside && wi->hParent && !(wi->style & WS_DISABLED)) {
            mywin_button_auto_toggle(hWnd, wi);
            mywin_post_control_command(hWnd, wi->hParent, wi->id, BN_CLICKED);
        }
        return 0;
    }
    case WM_DESTROY:
        wi->control->pressed = 0;
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}


static int mywin_static_type(DWORD style)
{
    return (int)(style & SS_TYPEMASK);
}

static int mywin_text_no_amp_width(const char* s)
{
    int n = 0;
    if (!s) return 0;
    for (; *s; ++s) {
        if (*s == '&') {
            if (s[1] == '&') { n++; s++; }
            else continue;
        } else n++;
    }
    return n * 8;
}

static void mywin_draw_static_control(Framebuffer* fb, MyWinWindowInfo* wi, int x, int y, int w, int h)
{
    if (!fb || !wi) return;
    int type = mywin_static_type(wi->style);
    DWORD colText = (wi->style & WS_DISABLED) ? COLOR(120,120,130) : WHITE;

    switch (type) {
    case SS_BLACKRECT:  fb_rect(fb, x, y, w, h, COLOR(0,0,0)); return;
    case SS_GRAYRECT:   fb_rect(fb, x, y, w, h, COLOR(90,95,110)); return;
    case SS_WHITERECT:  fb_rect(fb, x, y, w, h, COLOR(220,220,230)); return;
    case SS_BLACKFRAME: fb_rect_outline(fb, x, y, w, h, COLOR(0,0,0)); return;
    case SS_GRAYFRAME:  fb_rect_outline(fb, x, y, w, h, COLOR(100,110,130)); return;
    case SS_WHITEFRAME: fb_rect_outline(fb, x, y, w, h, COLOR(225,225,235)); return;
    case SS_ETCHEDHORZ:
        fb_rect(fb, x, y + h/2, w, 1, COLOR(70,78,100));
        fb_rect(fb, x, y + h/2 + 1, w, 1, COLOR(160,170,205));
        return;
    case SS_ETCHEDVERT:
        fb_rect(fb, x + w/2, y, 1, h, COLOR(70,78,100));
        fb_rect(fb, x + w/2 + 1, y, 1, h, COLOR(160,170,205));
        return;
    case SS_ETCHEDFRAME:
        fb_rect_outline(fb, x, y, w, h, COLOR(70,78,100));
        fb_rect_outline(fb, x + 1, y + 1, w - 2, h - 2, COLOR(160,170,205));
        return;
    case SS_ICON:
        fb_rect(fb, x, y, w < h ? w : h, w < h ? w : h, COLOR(52,64,94));
        fb_rect_outline(fb, x, y, w < h ? w : h, w < h ? w : h, COLOR(170,190,230));
        DrawClipTextA(fb, x + 3, y + 5, wi->text[0] ? wi->text : "ICON", colText, x, y, w, h);
        return;
    default:
        break;
    }

    int tw = mywin_text_no_amp_width(wi->text);
    int tx = x;
    if (type == SS_CENTER) tx = x + (w - tw) / 2;
    else if (type == SS_RIGHT) tx = x + w - tw - 2;
    if (tx < x) tx = x;
    int ty = (wi->style & SS_CENTERIMAGE) ? (y + (h - 8) / 2) : (y + 5);
    if (wi->style & SS_NOPREFIX) DrawClipTextA(fb, tx, ty, wi->text, colText, x, y, w, h);
    else mywin_draw_text_mnemonic(fb, tx, ty, wi->text, colText, x, y, w, h);
}

static LRESULT CALLBACK MyStaticWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg) {
    case WM_GETDLGCODE:
        return DLGC_STATIC;
    case WM_SETTEXT: {
        LRESULT r = mywin_def_text_proc(hWnd, Msg, wParam, lParam);
        InvalidateRect(hWnd, NULL, TRUE);
        return r;
    }
    case WM_GETTEXT:
    case WM_GETTEXTLENGTH:
        return mywin_def_text_proc(hWnd, Msg, wParam, lParam);
    case WM_ENABLE:
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static int mywin_edit_len(MyWinWindowInfo* wi)
{
    return wi ? (int)strlen(wi->text) : 0;
}

static void mywin_edit_clamp(MyWinWindowInfo* wi)
{
    if (!wi) return;
    int n = mywin_edit_len(wi);
    if (wi->control->editCaret < 0) wi->control->editCaret = 0;
    if (wi->control->editCaret > n) wi->control->editCaret = n;
    if (wi->control->editSelStart < 0) wi->control->editSelStart = 0;
    if (wi->control->editSelEnd < 0) wi->control->editSelEnd = 0;
    if (wi->control->editSelStart > n) wi->control->editSelStart = n;
    if (wi->control->editSelEnd > n) wi->control->editSelEnd = n;
}

static int mywin_edit_sel_min(MyWinWindowInfo* wi)
{
    return wi->control->editSelStart < wi->control->editSelEnd ? wi->control->editSelStart : wi->control->editSelEnd;
}

static int mywin_edit_sel_max(MyWinWindowInfo* wi)
{
    return wi->control->editSelStart > wi->control->editSelEnd ? wi->control->editSelStart : wi->control->editSelEnd;
}

static BOOL mywin_edit_has_sel(MyWinWindowInfo* wi)
{
    return wi && wi->control->editSelStart != wi->control->editSelEnd;
}

static void mywin_edit_notify_change(HWND hWnd, MyWinWindowInfo* wi)
{
    if (!wi) return;
    if (wi->hParent) mywin_post_control_command(hWnd, wi->hParent, wi->id, EN_UPDATE);
    if (wi->hParent) mywin_post_control_command(hWnd, wi->hParent, wi->id, EN_CHANGE);
    InvalidateRect(hWnd, NULL, TRUE);
}

static void mywin_edit_set_sel(MyWinWindowInfo* wi, int a, int b)
{
    if (!wi) return;
    int n = mywin_edit_len(wi);
    if (a < 0) a = n;
    if (b < 0) b = n;
    wi->control->editSelStart = a;
    wi->control->editSelEnd = b;
    wi->control->editCaret = b;
    mywin_edit_clamp(wi);
}

static BOOL mywin_edit_replace_range(HWND hWnd, MyWinWindowInfo* wi, int a, int b, const char* repl, BOOL canUndo)
{
    (void)canUndo;
    if (!wi) return FALSE;
    if (wi->style & ES_READONLY) return FALSE;
    mywin_edit_clamp(wi);
    int n = mywin_edit_len(wi);
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (a > n) a = n;
    if (b > n) b = n;
    if (a > b) { int t = a; a = b; b = t; }
    if (!repl) repl = "";
    char ins[256];
    size_t ri = 0;
    for (const char* p = repl; *p && ri + 1 < sizeof(ins); ++p) {
        unsigned char ch = (unsigned char)*p;
        if ((wi->style & ES_NUMBER) && (ch < '0' || ch > '9')) continue;
        if (wi->style & ES_UPPERCASE) ch = (unsigned char)toupper(ch);
        if (wi->style & ES_LOWERCASE) ch = (unsigned char)tolower(ch);
        if (!(wi->style & ES_MULTILINE) && (ch == '\n' || ch == '\r')) continue;
        ins[ri++] = (char)ch;
    }
    ins[ri] = 0;
    char out[MYWIN_WINDOW_TEXT_CHARS];
    int prefix = a;
    int suffix = n - b;
    int maxIns = (int)sizeof(out) - 1 - prefix - suffix;
    if (maxIns < 0) maxIns = 0;
    int inLen = (int)strlen(ins);
    if (inLen > maxIns) inLen = maxIns;
    memcpy(out, wi->text, (size_t)prefix);
    memcpy(out + prefix, ins, (size_t)inLen);
    memcpy(out + prefix + inLen, wi->text + b, (size_t)suffix);
    out[prefix + inLen + suffix] = 0;
    snprintf(wi->text, MYWIN_WINDOW_TEXT_CHARS, "%s", out);
    wi->textHash = mywin_title_hash(wi->text);
    wi->control->editCaret = prefix + inLen;
    wi->control->editSelStart = wi->control->editSelEnd = wi->control->editCaret;
    mywin_edit_notify_change(hWnd, wi);
    return TRUE;
}

static int mywin_edit_line_start(const char* s, int line)
{
    int pos = 0;
    int cur = 0;
    if (line <= 0) return 0;
    while (s[pos]) {
        if (s[pos++] == '\n') {
            cur++;
            if (cur == line) return pos;
        }
    }
    return pos;
}

static int mywin_edit_line_from_char(const char* s, int pos)
{
    int line = 0;
    for (int i = 0; s[i] && i < pos; ++i) if (s[i] == '\n') line++;
    return line;
}

static int mywin_edit_line_end(const char* s, int start)
{
    int p = start;
    while (s[p] && s[p] != '\n') p++;
    return p;
}

static int mywin_edit_line_count(const char* s)
{
    int n = 1;
    if (!s || !*s) return 1;
    for (; *s; ++s) if (*s == '\n') n++;
    return n;
}

static void mywin_edit_move_home(MyWinWindowInfo* wi)
{
    int line = mywin_edit_line_from_char(wi->text, wi->control->editCaret);
    wi->control->editCaret = mywin_edit_line_start(wi->text, line);
    wi->control->editSelStart = wi->control->editSelEnd = wi->control->editCaret;
}

static void mywin_edit_move_end(MyWinWindowInfo* wi)
{
    int line = mywin_edit_line_from_char(wi->text, wi->control->editCaret);
    int st = mywin_edit_line_start(wi->text, line);
    wi->control->editCaret = mywin_edit_line_end(wi->text, st);
    wi->control->editSelStart = wi->control->editSelEnd = wi->control->editCaret;
}

static void mywin_edit_move_vertical(MyWinWindowInfo* wi, int dir)
{
    int line = mywin_edit_line_from_char(wi->text, wi->control->editCaret);
    int st = mywin_edit_line_start(wi->text, line);
    int col = wi->control->editCaret - st;
    int newLine = line + dir;
    int count = mywin_edit_line_count(wi->text);
    if (newLine < 0) newLine = 0;
    if (newLine >= count) newLine = count - 1;
    int ns = mywin_edit_line_start(wi->text, newLine);
    int ne = mywin_edit_line_end(wi->text, ns);
    wi->control->editCaret = ns + col;
    if (wi->control->editCaret > ne) wi->control->editCaret = ne;
    wi->control->editSelStart = wi->control->editSelEnd = wi->control->editCaret;
    if (wi->control->editCaret < 0) wi->control->editCaret = 0;
}

static void mywin_edit_ensure_visible(MyWinWindowInfo* wi)
{
    if (!wi) return;
    int w = (int)(wi->rcClient.right - wi->rcClient.left);
    int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
    if (wi->style & ES_MULTILINE) {
        int line = mywin_edit_line_from_char(wi->text, wi->control->editCaret);
        int vis = (h - 8) / 12;
        if (vis < 1) vis = 1;
        if (line < wi->control->editFirstLine) wi->control->editFirstLine = line;
        if (line >= wi->control->editFirstLine + vis) wi->control->editFirstLine = line - vis + 1;
        if (wi->control->editFirstLine < 0) wi->control->editFirstLine = 0;
    } else {
        int chars = (w - 12) / 8;
        if (chars < 1) chars = 1;
        if (wi->control->editCaret < wi->control->editHScroll) wi->control->editHScroll = wi->control->editCaret;
        if (wi->control->editCaret > wi->control->editHScroll + chars) wi->control->editHScroll = wi->control->editCaret - chars;
        if (wi->control->editHScroll < 0) wi->control->editHScroll = 0;
    }
}

static int mywin_edit_char_from_point(MyWinWindowInfo* wi, int x, int y)
{
    if (!wi) return 0;
    if (wi->style & ES_MULTILINE) {
        int line = wi->control->editFirstLine + (y - 4) / 12;
        if (line < 0) line = 0;
        int st = mywin_edit_line_start(wi->text, line);
        int en = mywin_edit_line_end(wi->text, st);
        int col = (x - 6) / 8;
        if (col < 0) col = 0;
        int pos = st + col;
        if (pos > en) pos = en;
        return pos;
    }
    int pos = wi->control->editHScroll + (x - 6) / 8;
    if (pos < 0) pos = 0;
    int n = mywin_edit_len(wi);
    if (pos > n) pos = n;
    return pos;
}

static const char* mywin_edit_display_text(MyWinWindowInfo* wi, char* out, size_t outsz)
{
    if (!wi || !out || outsz == 0) return "";
    if (wi->style & ES_PASSWORD) {
        char pc = wi->control->editPasswordChar ? (char)wi->control->editPasswordChar : '*';
        size_t n = strlen(wi->text);
        if (n >= outsz) n = outsz - 1;
        memset(out, pc, n);
        out[n] = 0;
        return out;
    }
    snprintf(out, outsz, "%s", wi->text);
    return out;
}

static void mywin_draw_edit_control(Framebuffer* fb, MyWinWindowInfo* wi, int x, int y, int w, int h)
{
    if (!fb || !wi) return;
    BOOL ro = (wi->style & ES_READONLY) ? TRUE : FALSE;
    DWORD bg = (wi->style & WS_DISABLED) ? COLOR(20,20,24) : (ro ? COLOR(22,24,31) : COLOR(12,14,20));
    DWORD fg = (wi->style & WS_DISABLED) ? COLOR(130,130,140) : WHITE;
    fb_rect(fb, x, y, w, h, bg);
    fb_rect_outline(fb, x, y, w, h, g_FocusHwnd == wi->hWnd ? COLOR(255,230,120) : COLOR(105,120,150));

    char disp[MYWIN_WINDOW_TEXT_CHARS];
    const char* txt = mywin_edit_display_text(wi, disp, sizeof(disp));
    int selA = mywin_edit_sel_min(wi);
    int selB = mywin_edit_sel_max(wi);

    if (wi->style & ES_MULTILINE) {
        int lineH = 12;
        int vis = (h - 8) / lineH;
        if (vis < 1) vis = 1;
        for (int row = 0; row < vis; ++row) {
            int line = wi->control->editFirstLine + row;
            int st = mywin_edit_line_start(txt, line);
            int en = mywin_edit_line_end(txt, st);
            if (st > (int)strlen(txt)) break;
            char linebuf[256];
            int ln = en - st;
            if (ln >= (int)sizeof(linebuf)) ln = (int)sizeof(linebuf) - 1;
            memcpy(linebuf, txt + st, (size_t)ln);
            linebuf[ln] = 0;
            DrawClipTextA(fb, x + 6, y + 5 + row * lineH, linebuf, fg, x+4, y+4, w-8, h-8);
            if (g_FocusHwnd == wi->hWnd && wi->control->editCaret >= st && wi->control->editCaret <= en) {
                int col = wi->control->editCaret - st;
                int cx = x + 6 + col * 8;
                if (cx >= x + 4 && cx < x + w - 4) fb_rect(fb, cx, y + 5 + row * lineH, 1, lineH - 2, COLOR(255,230,120));
            }
        }
    } else {
        int n = (int)strlen(txt);
        int first = wi->control->editHScroll;
        if (first < 0) first = 0;
        if (first > n) first = n;
        const char* shown = txt + first;
        int tw = ((int)strlen(shown)) * 8;
        int tx = x + 6;
        int align = wi->style & 0x3u;
        if (align == ES_CENTER) tx = x + (w - tw) / 2;
        else if (align == ES_RIGHT) tx = x + w - tw - 6;
        if (tx < x + 6) tx = x + 6;

        if (mywin_edit_has_sel(wi)) {
            int a = selA - first;
            int b = selB - first;
            if (a < 0) a = 0;
            if (b < 0) b = 0;
            int maxc = (w - 12) / 8;
            if (b > maxc) b = maxc;
            if (b > a) fb_rect(fb, tx + a * 8, y + 5, (b - a) * 8, h - 10, COLOR(64,88,135));
        }
        DrawClipTextA(fb, tx, y + 7, shown, fg, x+4, y+4, w-8, h-8);
        if (g_FocusHwnd == wi->hWnd) {
            int cx = tx + (wi->control->editCaret - first) * 8;
            if (cx >= x + 4 && cx < x + w - 4) fb_rect(fb, cx, y + 5, 1, h - 10, COLOR(255,230,120));
        }
    }
}

static LRESULT CALLBACK MyEditWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    switch (Msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS | ((wi->style & ES_WANTRETURN) ? DLGC_WANTALLKEYS : 0);
    case WM_GETTEXT:
    case WM_GETTEXTLENGTH:
        return mywin_def_text_proc(hWnd, Msg, wParam, lParam);
    case WM_SETTEXT: {
        LRESULT r = mywin_def_text_proc(hWnd, Msg, wParam, lParam);
        wi->control->editCaret = (int)strlen(wi->text);
        wi->control->editSelStart = wi->control->editSelEnd = wi->control->editCaret;
        wi->control->editFirstLine = wi->control->editHScroll = 0;
        InvalidateRect(hWnd, NULL, TRUE);
        return r;
    }
    case WM_SETFOCUS:
        if (wi->hParent) mywin_post_control_command(hWnd, wi->hParent, wi->id, EN_SETFOCUS);
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case WM_KILLFOCUS:
        if (!(wi->style & ES_NOHIDESEL)) wi->control->editSelStart = wi->control->editSelEnd = wi->control->editCaret;
        if (wi->hParent) mywin_post_control_command(hWnd, wi->hParent, wi->id, EN_KILLFOCUS);
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case WM_ENABLE:
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case WM_LBUTTONDOWN:
        if (wi->style & WS_DISABLED) return 0;
        SetFocus(hWnd);
        wi->control->editCaret = mywin_edit_char_from_point(wi, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        wi->control->editSelStart = wi->control->editSelEnd = wi->control->editCaret;
        SetCapture(hWnd);
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == hWnd && (wParam & MK_LBUTTON)) {
            wi->control->editSelEnd = mywin_edit_char_from_point(wi, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            wi->control->editCaret = wi->control->editSelEnd;
            mywin_edit_ensure_visible(wi);
            InvalidateRect(hWnd, NULL, TRUE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hWnd) ReleaseCapture();
        return 0;
    case WM_KEYDOWN: {
        int key = (int)wParam;
        mywin_edit_clamp(wi);
        if (key == KEY_BACKSPACE) {
            if (mywin_edit_has_sel(wi)) mywin_edit_replace_range(hWnd, wi, mywin_edit_sel_min(wi), mywin_edit_sel_max(wi), "", TRUE);
            else if (wi->control->editCaret > 0) mywin_edit_replace_range(hWnd, wi, wi->control->editCaret - 1, wi->control->editCaret, "", TRUE);
            return 0;
        }
        if (key == KEY_DELETE) {
            int n = mywin_edit_len(wi);
            if (mywin_edit_has_sel(wi)) mywin_edit_replace_range(hWnd, wi, mywin_edit_sel_min(wi), mywin_edit_sel_max(wi), "", TRUE);
            else if (wi->control->editCaret < n) mywin_edit_replace_range(hWnd, wi, wi->control->editCaret, wi->control->editCaret + 1, "", TRUE);
            return 0;
        }
        if (key == KEY_LEFT) { if (wi->control->editCaret > 0) wi->control->editCaret--; wi->control->editSelStart = wi->control->editSelEnd = wi->control->editCaret; mywin_edit_ensure_visible(wi); InvalidateRect(hWnd, NULL, TRUE); return 0; }
        if (key == KEY_RIGHT) { if (wi->control->editCaret < mywin_edit_len(wi)) wi->control->editCaret++; wi->control->editSelStart = wi->control->editSelEnd = wi->control->editCaret; mywin_edit_ensure_visible(wi); InvalidateRect(hWnd, NULL, TRUE); return 0; }
        if (key == KEY_HOME) { mywin_edit_move_home(wi); mywin_edit_ensure_visible(wi); InvalidateRect(hWnd, NULL, TRUE); return 0; }
        if (key == KEY_END) { mywin_edit_move_end(wi); mywin_edit_ensure_visible(wi); InvalidateRect(hWnd, NULL, TRUE); return 0; }
        if ((wi->style & ES_MULTILINE) && key == KEY_UP) { mywin_edit_move_vertical(wi, -1); mywin_edit_ensure_visible(wi); InvalidateRect(hWnd, NULL, TRUE); return 0; }
        if ((wi->style & ES_MULTILINE) && key == KEY_DOWN) { mywin_edit_move_vertical(wi, 1); mywin_edit_ensure_visible(wi); InvalidateRect(hWnd, NULL, TRUE); return 0; }
        if ((wi->style & ES_MULTILINE) && key == KEY_ENTER) { mywin_edit_replace_range(hWnd, wi, mywin_edit_sel_min(wi), mywin_edit_sel_max(wi), "\n", TRUE); mywin_edit_ensure_visible(wi); return 0; }
        return 0;
    }
    case WM_CHAR: {
        unsigned char ch = (unsigned char)wParam;
        if (ch >= 32 && ch < 127) {
            char tmp[2] = { (char)ch, 0 };
            mywin_edit_replace_range(hWnd, wi, mywin_edit_sel_min(wi), mywin_edit_sel_max(wi), tmp, TRUE);
            mywin_edit_ensure_visible(wi);
        } else if ((wi->style & ES_MULTILINE) && (ch == '\r' || ch == '\n')) {
            mywin_edit_replace_range(hWnd, wi, mywin_edit_sel_min(wi), mywin_edit_sel_max(wi), "\n", TRUE);
            mywin_edit_ensure_visible(wi);
        }
        return 0;
    }
    case EM_GETSEL:
        if (wParam) *(DWORD*)wParam = (DWORD)mywin_edit_sel_min(wi);
        if (lParam) *(DWORD*)lParam = (DWORD)mywin_edit_sel_max(wi);
        return MAKELPARAM((WORD)mywin_edit_sel_min(wi), (WORD)mywin_edit_sel_max(wi));
    case EM_SETSEL:
        mywin_edit_set_sel(wi, (int)wParam, (int)lParam);
        mywin_edit_ensure_visible(wi);
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case EM_REPLACESEL:
        mywin_edit_replace_range(hWnd, wi, mywin_edit_sel_min(wi), mywin_edit_sel_max(wi), (LPCSTR)lParam, (BOOL)wParam);
        mywin_edit_ensure_visible(wi);
        return 0;
    case EM_GETLINECOUNT:
        return mywin_edit_line_count(wi->text);
    case EM_LINEFROMCHAR:
        return mywin_edit_line_from_char(wi->text, (int)wParam < 0 ? wi->control->editCaret : (int)wParam);
    case EM_LINEINDEX:
        return mywin_edit_line_start(wi->text, (int)wParam < 0 ? mywin_edit_line_from_char(wi->text, wi->control->editCaret) : (int)wParam);
    case EM_LINELENGTH: {
        int pos = (int)wParam;
        if (pos < 0) pos = wi->control->editCaret;
        int line = mywin_edit_line_from_char(wi->text, pos);
        int st = mywin_edit_line_start(wi->text, line);
        int en = mywin_edit_line_end(wi->text, st);
        return en - st;
    }
    case EM_GETLINE: {
        int line = (int)wParam;
        LPSTR out = (LPSTR)lParam;
        if (!out) return 0;
        int maxc = *(WORD*)out;
        if (maxc <= 0) maxc = 255;
        int st = mywin_edit_line_start(wi->text, line);
        int en = mywin_edit_line_end(wi->text, st);
        int len = en - st;
        if (len > maxc) len = maxc;
        memcpy(out, wi->text + st, (size_t)len);
        return len;
    }
    case EM_GETFIRSTVISIBLELINE:
        return wi->control->editFirstLine;
    case EM_LINESCROLL:
        wi->control->editHScroll += (int)wParam;
        wi->control->editFirstLine += (int)lParam;
        if (wi->control->editHScroll < 0) wi->control->editHScroll = 0;
        if (wi->control->editFirstLine < 0) wi->control->editFirstLine = 0;
        InvalidateRect(hWnd, NULL, TRUE);
        return TRUE;
    case EM_SCROLLCARET:
        mywin_edit_ensure_visible(wi);
        InvalidateRect(hWnd, NULL, TRUE);
        return TRUE;
    case EM_SETREADONLY:
        if (wParam) wi->style |= ES_READONLY; else wi->style &= ~ES_READONLY;
        InvalidateRect(hWnd, NULL, TRUE);
        return TRUE;
    case EM_GETPASSWORDCHAR:
        return (wi->style & ES_PASSWORD) ? (wi->control->editPasswordChar ? wi->control->editPasswordChar : '*') : 0;
    case EM_SETPASSWORDCHAR:
        wi->control->editPasswordChar = (int)wParam;
        if (wParam) wi->style |= ES_PASSWORD; else wi->style &= ~ES_PASSWORD;
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}




static int mywin_cc_visible_count(MyWinWindowInfo* wi)
{
    if (!wi) return 0;
    int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
    int ih = wi->control->ccItemHeight > 0 ? wi->control->ccItemHeight : 16;
    if (strcmp(wi->className, "COMBOBOX") == 0) h = wi->control->ccDropHeight > 0 ? wi->control->ccDropHeight : 96;
    int n = h / ih;
    return n > 0 ? n : 1;
}

/* v102.3: COMBOBOX dropdown geometry lives outside the closed control
   rectangle, but it must still behave like top-level combo chrome: drawn above
   later sibling controls and hit-tested before them.  Keep the closed height
   MSDN-like while treating the drop list as transient popup space. */
static int mywin_combo_drop_height(MyWinWindowInfo* wi)
{
    if (!wi) return 96;
    int ih = wi->control->ccItemHeight > 0 ? wi->control->ccItemHeight : 16;
    int rows = wi->control->ccCount > 0 ? wi->control->ccCount : 1;
    if (rows > 6) rows = 6;
    int byItems = rows * ih + 2;
    int configured = wi->control->ccDropHeight > 0 ? wi->control->ccDropHeight : 96;
    if (byItems < ih + 2) byItems = ih + 2;
    return byItems < configured ? byItems : configured;
}

static void mywin_combo_dropdown_rect_local(MyWinWindowInfo* wi, RECT* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!wi) return;
    int w = (int)(wi->rcClient.right - wi->rcClient.left);
    int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
    int dh = mywin_combo_drop_height(wi);
    out->left = 0;
    out->top = h;
    out->right = w;
    out->bottom = h + dh;
}

static void mywin_combo_dropdown_rect_parent(MyWinWindowInfo* wi, RECT* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!wi) return;
    RECT lr;
    mywin_combo_dropdown_rect_local(wi, &lr);
    out->left = wi->rcClient.left + lr.left;
    out->top = wi->rcClient.top + lr.top;
    out->right = wi->rcClient.left + lr.right;
    out->bottom = wi->rcClient.top + lr.bottom;
}

static void mywin_cc_clamp_top(MyWinWindowInfo* wi)
{
    if (!wi) return;
    int vis = mywin_cc_visible_count(wi);
    int maxTop = wi->control->ccCount - vis;
    if (maxTop < 0) maxTop = 0;
    if (wi->control->ccTopIndex < 0) wi->control->ccTopIndex = 0;
    if (wi->control->ccTopIndex > maxTop) wi->control->ccTopIndex = maxTop;
}

/* v92.1: mouse wheel helpers for classic common controls.
   WM_MOUSEWHEEL carries the signed delta in HIWORD(wParam).  Keep the
   per-notch movement at the classic three lines until SystemParametersInfo
   exists in myOS. */
static int mywin_wheel_lines(WPARAM wParam)
{
    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    if (delta == 0) return 0;
    int notches = delta / WHEEL_DELTA;
    if (notches == 0) notches = (delta > 0) ? 1 : -1;
    return -notches * 3; /* positive return means scroll down / forward */
}

static void mywin_cc_scroll_top_by(MyWinWindowInfo* wi, int lines)
{
    if (!wi || lines == 0) return;
    wi->control->ccTopIndex += lines;
    mywin_cc_clamp_top(wi);
}

static void mywin_cc_ensure_visible(MyWinWindowInfo* wi, int idx)
{
    if (!wi || idx < 0 || idx >= wi->control->ccCount) return;
    int vis = mywin_cc_visible_count(wi);
    if (idx < wi->control->ccTopIndex) wi->control->ccTopIndex = idx;
    if (idx >= wi->control->ccTopIndex + vis) wi->control->ccTopIndex = idx - vis + 1;
    mywin_cc_clamp_top(wi);
}

static int mywin_cc_insert_item(MyWinWindowInfo* wi, int index, LPCSTR text, BOOL sort)
{
    if (!wi || wi->control->ccCount >= MYWIN_CC_MAX_ITEMS) return LB_ERRSPACE;
    if (!text) text = "";
    if (sort) {
        index = wi->control->ccCount;
        for (int i = 0; i < wi->control->ccCount; ++i) {
            if (strcasecmp(text, wi->control->ccItems[i]) < 0) { index = i; break; }
        }
    } else {
        if (index < 0 || index > wi->control->ccCount) index = wi->control->ccCount;
    }
    if (index < wi->control->ccCount) {
        size_t moveCount = (size_t)(wi->control->ccCount - index);
        memmove(&wi->control->ccItems[index + 1], &wi->control->ccItems[index],
                moveCount * sizeof(wi->control->ccItems[0]));
        memmove(&wi->control->ccItemData[index + 1], &wi->control->ccItemData[index],
                moveCount * sizeof(wi->control->ccItemData[0]));
        memmove(&wi->control->ccSel[index + 1], &wi->control->ccSel[index],
                moveCount * sizeof(wi->control->ccSel[0]));
    }
    snprintf(wi->control->ccItems[index], sizeof(wi->control->ccItems[index]), "%.*s", MYWIN_CC_TEXT_CHARS - 1, text);
    wi->control->ccItemData[index] = 0;
    wi->control->ccSel[index] = 0;
    wi->control->ccCount++;
    if (wi->control->ccCurSel >= index) wi->control->ccCurSel++;
    mywin_cc_clamp_top(wi);
    return index;
}

static int mywin_cc_delete_item(MyWinWindowInfo* wi, int index)
{
    if (!wi || index < 0 || index >= wi->control->ccCount) return LB_ERR;
    if (index < wi->control->ccCount - 1) {
        size_t moveCount = (size_t)(wi->control->ccCount - index - 1);
        memmove(&wi->control->ccItems[index], &wi->control->ccItems[index + 1],
                moveCount * sizeof(wi->control->ccItems[0]));
        memmove(&wi->control->ccItemData[index], &wi->control->ccItemData[index + 1],
                moveCount * sizeof(wi->control->ccItemData[0]));
        memmove(&wi->control->ccSel[index], &wi->control->ccSel[index + 1],
                moveCount * sizeof(wi->control->ccSel[0]));
    }
    wi->control->ccCount--;
    if (wi->control->ccCount < 0) wi->control->ccCount = 0;
    if (wi->control->ccCurSel == index) wi->control->ccCurSel = -1;
    else if (wi->control->ccCurSel > index) wi->control->ccCurSel--;
    if (wi->control->ccCurSel >= wi->control->ccCount) wi->control->ccCurSel = wi->control->ccCount - 1;
    if (wi->control->ccCaretIndex >= wi->control->ccCount) wi->control->ccCaretIndex = wi->control->ccCount - 1;
    if (wi->control->ccAnchorIndex >= wi->control->ccCount) wi->control->ccAnchorIndex = wi->control->ccCount - 1;
    mywin_cc_clamp_top(wi);
    return wi->control->ccCount;
}

static int mywin_cc_find(MyWinWindowInfo* wi, int start, LPCSTR needle, BOOL exact)
{
    if (!wi || !needle || wi->control->ccCount <= 0) return LB_ERR;
    if (start < -1 || start >= wi->control->ccCount) start = -1;
    for (int n = 0; n < wi->control->ccCount; ++n) {
        int i = (start + 1 + n) % wi->control->ccCount;
        if (exact) {
            if (strcasecmp(wi->control->ccItems[i], needle) == 0) return i;
        } else {
            size_t m = strlen(needle);
            if (strncasecmp(wi->control->ccItems[i], needle, m) == 0) return i;
        }
    }
    return LB_ERR;
}

static void mywin_cc_reset(MyWinWindowInfo* wi)
{
    if (!wi) return;
    wi->control->ccCount = 0;
    wi->control->ccCurSel = -1;
    wi->control->ccTopIndex = 0;
    wi->control->ccCaretIndex = -1;
    wi->control->ccAnchorIndex = -1;
    wi->control->ccDropped = 0;
    memset(wi->control->ccSel, 0, MYWIN_CC_MAX_ITEMS);
}

static void mywin_cc_select_single(HWND hWnd, MyWinWindowInfo* wi, int idx, UINT notifyCode)
{
    if (!wi) return;
    if (idx < -1 || idx >= wi->control->ccCount) return;
    int old = wi->control->ccCurSel;
    wi->control->ccCurSel = idx;
    wi->control->ccCaretIndex = idx;
    if (!(wi->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL))) {
        memset(wi->control->ccSel, 0, MYWIN_CC_MAX_ITEMS);
        if (idx >= 0) wi->control->ccSel[idx] = 1;
    }
    mywin_cc_ensure_visible(wi, idx);
    if (notifyCode && old != idx && wi->hParent) mywin_post_control_command(hWnd, wi->hParent, wi->id, notifyCode);
}

static LRESULT CALLBACK MyListBoxWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    switch (Msg) {
    case WM_CREATE:
        wi->control->ccItemHeight = 16;
        wi->control->ccCurSel = -1;
        wi->control->ccCaretIndex = -1;
        wi->control->ccAnchorIndex = -1;
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
        mywin_notify_parent(hWnd, LBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        mywin_notify_parent(hWnd, LBN_KILLFOCUS);
        return 0;
    case LB_ADDSTRING:
        return mywin_cc_insert_item(wi, wi->control->ccCount, (LPCSTR)lParam, (wi->style & LBS_SORT) ? TRUE : FALSE);
    case LB_INSERTSTRING:
        return mywin_cc_insert_item(wi, (int)wParam, (LPCSTR)lParam, FALSE);
    case LB_DELETESTRING:
        return mywin_cc_delete_item(wi, (int)wParam);
    case LB_RESETCONTENT:
        mywin_cc_reset(wi);
        return 0;
    case LB_GETCOUNT:
        return wi->control->ccCount;
    case LB_SETCURSEL:
        if ((int)wParam == -1) { mywin_cc_select_single(hWnd, wi, -1, 0); return 0; }
        if ((int)wParam < 0 || (int)wParam >= wi->control->ccCount) return LB_ERR;
        mywin_cc_select_single(hWnd, wi, (int)wParam, 0);
        return wi->control->ccCurSel;
    case LB_GETCURSEL:
        return wi->control->ccCurSel >= 0 ? wi->control->ccCurSel : LB_ERR;
    case LB_SETSEL: {
        int idx = (int)lParam;
        if (idx < 0 || idx >= wi->control->ccCount) return LB_ERR;
        wi->control->ccSel[idx] = wParam ? 1 : 0;
        if (wParam) wi->control->ccCurSel = idx;
        return 0;
    }
    case LB_GETSEL: {
        int idx = (int)wParam;
        if (idx < 0 || idx >= wi->control->ccCount) return LB_ERR;
        return wi->control->ccSel[idx] ? 1 : 0;
    }
    case LB_GETSELCOUNT: {
        int n = 0;
        for (int i = 0; i < wi->control->ccCount; ++i) if (wi->control->ccSel[i]) n++;
        return n;
    }
    case LB_GETSELITEMS: {
        int max = (int)wParam;
        int* out = (int*)lParam;
        if (!out || max <= 0) return 0;
        int n = 0;
        for (int i = 0; i < wi->control->ccCount && n < max; ++i) if (wi->control->ccSel[i]) out[n++] = i;
        return n;
    }
    case LB_GETTEXT: {
        int idx = (int)wParam;
        LPSTR out = (LPSTR)lParam;
        if (idx < 0 || idx >= wi->control->ccCount || !out) return LB_ERR;
        snprintf(out, MYWIN_CC_TEXT_CHARS, "%s", wi->control->ccItems[idx]);
        return (LRESULT)strlen(out);
    }
    case LB_GETTEXTLEN: {
        int idx = (int)wParam;
        if (idx < 0 || idx >= wi->control->ccCount) return LB_ERR;
        return (LRESULT)strlen(wi->control->ccItems[idx]);
    }
    case LB_FINDSTRING:
        return mywin_cc_find(wi, (int)wParam, (LPCSTR)lParam, FALSE);
    case LB_FINDSTRINGEXACT:
        return mywin_cc_find(wi, (int)wParam, (LPCSTR)lParam, TRUE);
    case LB_SELECTSTRING: {
        int idx = mywin_cc_find(wi, (int)wParam, (LPCSTR)lParam, FALSE);
        if (idx >= 0) mywin_cc_select_single(hWnd, wi, idx, 0);
        return idx;
    }
    case LB_GETTOPINDEX:
        return wi->control->ccTopIndex;
    case LB_SETTOPINDEX:
        wi->control->ccTopIndex = (int)wParam;
        mywin_cc_clamp_top(wi);
        return 0;
    case LB_GETITEMDATA: {
        int idx = (int)wParam;
        if (idx < 0 || idx >= wi->control->ccCount) return LB_ERR;
        return wi->control->ccItemData[idx];
    }
    case LB_SETITEMDATA: {
        int idx = (int)wParam;
        if (idx < 0 || idx >= wi->control->ccCount) return LB_ERR;
        wi->control->ccItemData[idx] = lParam;
        return 0;
    }
    case LB_SETITEMHEIGHT:
        wi->control->ccItemHeight = (int)lParam > 4 ? (int)lParam : 16;
        mywin_cc_clamp_top(wi);
        return 0;
    case LB_GETITEMHEIGHT:
        return wi->control->ccItemHeight > 0 ? wi->control->ccItemHeight : 16;
    case LB_GETITEMRECT: {
        int idx = (int)wParam;
        LPRECT rc = (LPRECT)lParam;
        if (!rc || idx < 0 || idx >= wi->control->ccCount) return LB_ERR;
        int ih = wi->control->ccItemHeight > 0 ? wi->control->ccItemHeight : 16;
        rc->left = 0;
        rc->top = (idx - wi->control->ccTopIndex) * ih;
        rc->right = wi->rcClient.right - wi->rcClient.left;
        rc->bottom = rc->top + ih;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (wi->style & WS_DISABLED) return 0;
        SetFocus(hWnd);
        SetCapture(hWnd);
        int y = GET_Y_LPARAM(lParam);
        int ih = wi->control->ccItemHeight > 0 ? wi->control->ccItemHeight : 16;
        int idx = wi->control->ccTopIndex + y / ih;
        if (idx >= 0 && idx < wi->control->ccCount) {
            if (wi->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) {
                wi->control->ccSel[idx] = !wi->control->ccSel[idx];
                wi->control->ccCurSel = idx;
                wi->control->ccCaretIndex = idx;
                mywin_notify_parent(hWnd, LBN_SELCHANGE);
            } else {
                mywin_cc_select_single(hWnd, wi, idx, (wi->style & LBS_NOTIFY) ? LBN_SELCHANGE : 0);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (GetCapture() == hWnd) ReleaseCapture();
        return 0;
    case WM_MOUSEWHEEL:
        mywin_cc_scroll_top_by(wi, mywin_wheel_lines(wParam));
        return 0;
    case WM_KEYDOWN: {
        int key = (int)wParam;
        int idx = wi->control->ccCurSel >= 0 ? wi->control->ccCurSel : 0;
        int vis = mywin_cc_visible_count(wi);
        if (key == KEY_UP) idx--;
        else if (key == KEY_DOWN) idx++;
        else if (key == KEY_HOME) idx = 0;
        else if (key == KEY_END) idx = wi->control->ccCount - 1;
        else if (key == KEY_PAGEUP) idx -= vis;
        else if (key == KEY_PAGEDOWN) idx += vis;
        else return 0;
        if (idx < 0) idx = 0;
        if (idx >= wi->control->ccCount) idx = wi->control->ccCount - 1;
        if (wi->control->ccCount > 0) mywin_cc_select_single(hWnd, wi, idx, (wi->style & LBS_NOTIFY) ? LBN_SELCHANGE : 0);
        return 0;
    }
    case WM_DESTROY:
        mywin_cc_reset(wi);
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static void mywin_scrollbar_thumb_rect(MyWinWindowInfo* wi, int* outTop, int* outBottom)
{
    int h = wi ? (int)(wi->rcClient.bottom - wi->rcClient.top) : 0;
    int arrow = 16;
    int track = h - arrow * 2;
    if (track < 12) track = 12;
    int min = wi->control->ccScrollMin, max = wi->control->ccScrollMax;
    if (max < min) max = min;
    int range = max - min + 1;
    int page = wi->control->ccScrollPage > 0 ? wi->control->ccScrollPage : 1;
    int thumb = (range > 0) ? (track * page) / (range + page) : track;
    if (thumb < 12) thumb = 12;
    if (thumb > track) thumb = track;
    int denom = max - min;
    int movable = track - thumb;
    int pos = wi->control->ccScrollPos;
    if (pos < min) pos = min;
    if (pos > max) pos = max;
    int top = arrow + (denom > 0 ? mywin_muldiv_int(pos - min, movable, denom) : 0);
    if (outTop) *outTop = top;
    if (outBottom) *outBottom = top + thumb;
}

static void mywin_scrollbar_set_pos(HWND hWnd, MyWinWindowInfo* wi, int pos, UINT code, BOOL notify)
{
    if (!wi) return;
    if (pos < wi->control->ccScrollMin) pos = wi->control->ccScrollMin;
    if (pos > wi->control->ccScrollMax) pos = wi->control->ccScrollMax;
    wi->control->ccScrollPos = pos;
    if (notify && wi->hParent) {
        UINT msg = (wi->style & SBS_VERT) ? WM_VSCROLL : WM_HSCROLL;
        mywin_post_control_scroll(hWnd, wi->hParent, msg, code, pos);
    }
}

static LRESULT CALLBACK MyScrollBarWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    switch (Msg) {
    case WM_CREATE:
        wi->control->ccScrollMin = 0;
        wi->control->ccScrollMax = 100;
        wi->control->ccScrollPage = 10;
        wi->control->ccScrollPos = 0;
        return 0;
    case WM_GETDLGCODE:
        /* v98: Scrollbar controls are keyboard controls in dialogs.  Without
           DLGC_WANTARROWS, IsDialogMessageA consumes arrow/page/home/end routing
           for LISTBOX/COMBOBOX but never forwards it to SCROLLBAR. */
        return DLGC_WANTARROWS;
    case SBM_SETPOS: {
        int old = wi->control->ccScrollPos;
        mywin_scrollbar_set_pos(hWnd, wi, (int)wParam, 0, FALSE);
        return old;
    }
    case SBM_GETPOS:
        return wi->control->ccScrollPos;
    case SBM_SETRANGE:
    case SBM_SETRANGEREDRAW:
        wi->control->ccScrollMin = (int)wParam;
        wi->control->ccScrollMax = (int)lParam;
        if (wi->control->ccScrollMax < wi->control->ccScrollMin) wi->control->ccScrollMax = wi->control->ccScrollMin;
        mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos, 0, FALSE);
        return TRUE;
    case SBM_GETRANGE:
        if ((LPINT)wParam) *((LPINT)wParam) = wi->control->ccScrollMin;
        if ((LPINT)lParam) *((LPINT)lParam) = wi->control->ccScrollMax;
        return TRUE;
    case SBM_ENABLE_ARROWS:
        return TRUE;
    case WM_LBUTTONDOWN: {
        if (wi->style & WS_DISABLED) return 0;
        SetFocus(hWnd);
        SetCapture(hWnd);
        int y = GET_Y_LPARAM(lParam);
        int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
        int t0 = 0, t1 = 0;
        mywin_scrollbar_thumb_rect(wi, &t0, &t1);
        if (y < 16) { wi->control->ccScrollPressedPart = SB_LINEUP; mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos - 1, SB_LINEUP, TRUE); }
        else if (y >= h - 16) { wi->control->ccScrollPressedPart = SB_LINEDOWN; mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos + 1, SB_LINEDOWN, TRUE); }
        else if (y >= t0 && y < t1) { wi->control->ccScrollTracking = 1; wi->control->ccScrollTrackOff = y - t0; wi->control->ccScrollPressedPart = SB_THUMBTRACK; }
        else if (y < t0) { wi->control->ccScrollPressedPart = SB_PAGEUP; mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos - (wi->control->ccScrollPage > 0 ? wi->control->ccScrollPage : 10), SB_PAGEUP, TRUE); }
        else { wi->control->ccScrollPressedPart = SB_PAGEDOWN; mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos + (wi->control->ccScrollPage > 0 ? wi->control->ccScrollPage : 10), SB_PAGEDOWN, TRUE); }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (wi->control->ccScrollTracking && GetCapture() == hWnd) {
            int y = GET_Y_LPARAM(lParam) - wi->control->ccScrollTrackOff;
            int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
            int arrow = 16, track = h - arrow * 2;
            int t0 = 0, t1 = 0;
            mywin_scrollbar_thumb_rect(wi, &t0, &t1);
            int thumb = t1 - t0;
            int movable = track - thumb;
            int pos = wi->control->ccScrollMin;
            if (movable > 0) pos = wi->control->ccScrollMin + mywin_muldiv_int(y - arrow, wi->control->ccScrollMax - wi->control->ccScrollMin, movable);
            mywin_scrollbar_set_pos(hWnd, wi, pos, SB_THUMBTRACK, TRUE);
            return 0;
        }
        return 0;
    case WM_LBUTTONUP:
        if (wi->control->ccScrollTracking) {
            mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos, SB_THUMBPOSITION, TRUE);
        }
        if (GetCapture() == hWnd) ReleaseCapture();
        if (wi->hParent) {
            UINT msg = (wi->style & SBS_VERT) ? WM_VSCROLL : WM_HSCROLL;
            mywin_post_control_scroll(hWnd, wi->hParent, msg, SB_ENDSCROLL, wi->control->ccScrollPos);
        }
        wi->control->ccScrollTracking = 0;
        wi->control->ccScrollPressedPart = 0;
        return 0;
    case WM_MOUSEWHEEL: {
        int lines = mywin_wheel_lines(wParam);
        if (lines) {
            int code = (lines > 0) ? SB_LINEDOWN : SB_LINEUP;
            mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos + lines, (UINT)code, TRUE);
            if (wi->hParent) {
                UINT msg = (wi->style & SBS_VERT) ? WM_VSCROLL : WM_HSCROLL;
                mywin_post_control_scroll(hWnd, wi->hParent, msg, SB_ENDSCROLL, wi->control->ccScrollPos);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if ((int)wParam == KEY_UP || (int)wParam == KEY_LEFT) mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos - 1, SB_LINEUP, TRUE);
        else if ((int)wParam == KEY_DOWN || (int)wParam == KEY_RIGHT) mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos + 1, SB_LINEDOWN, TRUE);
        else if ((int)wParam == KEY_PAGEUP) mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos - (wi->control->ccScrollPage > 0 ? wi->control->ccScrollPage : 10), SB_PAGEUP, TRUE);
        else if ((int)wParam == KEY_PAGEDOWN) mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollPos + (wi->control->ccScrollPage > 0 ? wi->control->ccScrollPage : 10), SB_PAGEDOWN, TRUE);
        else if ((int)wParam == KEY_HOME) mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollMin, SB_TOP, TRUE);
        else if ((int)wParam == KEY_END) mywin_scrollbar_set_pos(hWnd, wi, wi->control->ccScrollMax, SB_BOTTOM, TRUE);
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static void mywin_combo_close(HWND hWnd, MyWinWindowInfo* wi, UINT endNotify)
{
    if (!wi || !wi->control->ccDropped) return;
    wi->control->ccDropped = 0;
    if (GetCapture() == hWnd) ReleaseCapture();
    if (wi->hParent) {
        if (endNotify) mywin_notify_parent(hWnd, endNotify);
        mywin_notify_parent(hWnd, CBN_CLOSEUP);
    }
}

static void mywin_combo_open(HWND hWnd, MyWinWindowInfo* wi)
{
    if (!wi || wi->control->ccDropped) return;
    wi->control->ccDropped = 1;
    mywin_cc_ensure_visible(wi, wi->control->ccCurSel);
    SetCapture(hWnd);
    if (wi->hParent) mywin_notify_parent(hWnd, CBN_DROPDOWN);
}

static LRESULT CALLBACK MyComboBoxWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    switch (Msg) {
    case WM_CREATE:
        wi->control->ccItemHeight = 16;
        wi->control->ccDropHeight = 96;
        wi->control->ccCurSel = -1;
        wi->control->ccCaretIndex = -1;
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
        mywin_notify_parent(hWnd, CBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        mywin_combo_close(hWnd, wi, CBN_SELENDCANCEL);
        mywin_notify_parent(hWnd, CBN_KILLFOCUS);
        return 0;
    case CB_ADDSTRING:
        return mywin_cc_insert_item(wi, wi->control->ccCount, (LPCSTR)lParam, (wi->style & CBS_SORT) ? TRUE : FALSE);
    case CB_INSERTSTRING:
        return mywin_cc_insert_item(wi, (int)wParam, (LPCSTR)lParam, FALSE);
    case CB_DELETESTRING:
        return mywin_cc_delete_item(wi, (int)wParam);
    case CB_RESETCONTENT:
        mywin_cc_reset(wi);
        return 0;
    case CB_GETCOUNT:
        return wi->control->ccCount;
    case CB_GETCURSEL:
        return wi->control->ccCurSel >= 0 ? wi->control->ccCurSel : CB_ERR;
    case CB_SETCURSEL:
        if ((int)wParam == -1) { mywin_cc_select_single(hWnd, wi, -1, 0); return -1; }
        if ((int)wParam < 0 || (int)wParam >= wi->control->ccCount) return CB_ERR;
        mywin_cc_select_single(hWnd, wi, (int)wParam, 0);
        return wi->control->ccCurSel;
    case CB_GETLBTEXT: {
        int idx = (int)wParam;
        LPSTR out = (LPSTR)lParam;
        if (idx < 0 || idx >= wi->control->ccCount || !out) return CB_ERR;
        snprintf(out, MYWIN_CC_TEXT_CHARS, "%s", wi->control->ccItems[idx]);
        return (LRESULT)strlen(out);
    }
    case CB_GETLBTEXTLEN: {
        int idx = (int)wParam;
        if (idx < 0 || idx >= wi->control->ccCount) return CB_ERR;
        return (LRESULT)strlen(wi->control->ccItems[idx]);
    }
    case CB_FINDSTRING:
        return mywin_cc_find(wi, (int)wParam, (LPCSTR)lParam, FALSE);
    case CB_FINDSTRINGEXACT:
        return mywin_cc_find(wi, (int)wParam, (LPCSTR)lParam, TRUE);
    case CB_SELECTSTRING: {
        int idx = mywin_cc_find(wi, (int)wParam, (LPCSTR)lParam, FALSE);
        if (idx >= 0) mywin_cc_select_single(hWnd, wi, idx, CBN_SELCHANGE);
        return idx;
    }
    case CB_SHOWDROPDOWN:
        if (wParam) mywin_combo_open(hWnd, wi);
        else mywin_combo_close(hWnd, wi, 0);
        return TRUE;
    case CB_GETDROPPEDSTATE:
        return wi->control->ccDropped ? TRUE : FALSE;
    case CB_GETITEMDATA: {
        int idx = (int)wParam;
        if (idx < 0 || idx >= wi->control->ccCount) return CB_ERR;
        return wi->control->ccItemData[idx];
    }
    case CB_SETITEMDATA: {
        int idx = (int)wParam;
        if (idx < 0 || idx >= wi->control->ccCount) return CB_ERR;
        wi->control->ccItemData[idx] = lParam;
        return 0;
    }
    case CB_SETITEMHEIGHT:
        if ((int)wParam == -1) wi->control->ccItemHeight = (int)lParam > 4 ? (int)lParam : 16;
        else wi->control->ccItemHeight = (int)lParam > 4 ? (int)lParam : 16;
        return 0;
    case CB_GETITEMHEIGHT:
        return wi->control->ccItemHeight > 0 ? wi->control->ccItemHeight : 16;
    case CB_GETTOPINDEX:
        return wi->control->ccTopIndex;
    case CB_SETTOPINDEX:
        wi->control->ccTopIndex = (int)wParam; mywin_cc_clamp_top(wi); return 0;
    case CB_GETDROPPEDCONTROLRECT: {
        LPRECT rc = (LPRECT)lParam;
        if (!rc) return CB_ERR;
        int ox = 0, oy = 0;
        mywin_client_origin_screen(hWnd, &ox, &oy);
        RECT dr;
        mywin_combo_dropdown_rect_local(wi, &dr);
        int w = (int)(wi->rcClient.right - wi->rcClient.left);
        int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
        rc->left = ox;
        rc->top = oy;
        rc->right = ox + w;
        rc->bottom = oy + h;
        if (ox + dr.left < rc->left) rc->left = ox + dr.left;
        if (oy + dr.top < rc->top) rc->top = oy + dr.top;
        if (ox + dr.right > rc->right) rc->right = ox + dr.right;
        if (oy + dr.bottom > rc->bottom) rc->bottom = oy + dr.bottom;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (wi->style & WS_DISABLED) return 0;
        SetFocus(hWnd);
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        int ih = wi->control->ccItemHeight > 0 ? wi->control->ccItemHeight : 16;
        if (wi->control->ccDropped) {
            RECT dr;
            mywin_combo_dropdown_rect_local(wi, &dr);
            if (mywin_pt_in_rect(&dr, x, y)) {
                int rowY = y - dr.top - 1;
                int idx = (rowY >= 0) ? wi->control->ccTopIndex + rowY / ih : -1;
                if (idx >= 0 && idx < wi->control->ccCount) {
                    mywin_cc_select_single(hWnd, wi, idx, CBN_SELCHANGE);
                    mywin_combo_close(hWnd, wi, CBN_SELENDOK);
                }
            } else {
                mywin_combo_close(hWnd, wi, CBN_SELENDCANCEL);
            }
        } else {
            mywin_combo_open(hWnd, wi);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        return 0;
    case WM_LBUTTONUP:
        return 0;
    case WM_MOUSEWHEEL: {
        int lines = mywin_wheel_lines(wParam);
        if (!lines) return 0;
        if (wi->control->ccDropped) {
            mywin_cc_scroll_top_by(wi, lines);
            return 0;
        }
        if (wi->control->ccCount > 0) {
            int idx = wi->control->ccCurSel >= 0 ? wi->control->ccCurSel : 0;
            idx += (lines > 0) ? 1 : -1;
            if (idx < 0) idx = 0;
            if (idx >= wi->control->ccCount) idx = wi->control->ccCount - 1;
            mywin_cc_select_single(hWnd, wi, idx, CBN_SELCHANGE);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        int key = (int)wParam;
        if (key == KEY_ESC) { mywin_combo_close(hWnd, wi, CBN_SELENDCANCEL); return 0; }
        if (key == KEY_ENTER) { mywin_combo_close(hWnd, wi, CBN_SELENDOK); return 0; }
        if (key == KEY_SPACE) { if (wi->control->ccDropped) mywin_combo_close(hWnd, wi, CBN_SELENDCANCEL); else mywin_combo_open(hWnd, wi); return 0; }
        int idx = wi->control->ccCurSel >= 0 ? wi->control->ccCurSel : 0;
        if (key == KEY_UP) idx--;
        else if (key == KEY_DOWN) idx++;
        else if (key == KEY_HOME) idx = 0;
        else if (key == KEY_END) idx = wi->control->ccCount - 1;
        else return 0;
        if (idx < 0) idx = 0;
        if (idx >= wi->control->ccCount) idx = wi->control->ccCount - 1;
        if (wi->control->ccCount > 0) mywin_cc_select_single(hWnd, wi, idx, CBN_SELCHANGE);
        return 0;
    }
    case WM_DESTROY:
        mywin_cc_reset(wi);
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

BOOL MyIsDialogWindow(HWND hWnd)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    return (wi && strcmp(wi->className, "#32770") == 0) ? TRUE : FALSE;
}

HWND MyGetDialogOwner(HWND hWnd)
{
    if (!hWnd) return 0;
    HWND cur = hWnd;
    while (cur) {
        if (MyIsDialogWindow(cur)) {
            MyWinDialogInfo* di = mywin_find_dialog(cur);
            return di ? di->hParent : 0;
        }
        cur = GetParent(cur);
    }
    return 0;
}

BOOL MyIsOwnedDialogChild(HWND hOwner, HWND hWnd)
{
    if (!hOwner || !hWnd) return FALSE;
    for (int i = 0; i < MYWIN_MAX_DIALOGS; ++i) {
        MyWinDialogInfo* di = &g_DialogInfos[i];
        if (!di->valid || !di->hDlg || di->hParent != hOwner) continue;
        if (hWnd == di->hDlg || IsChild(di->hDlg, hWnd)) return TRUE;
    }
    return FALSE;
}

BOOL MyTopLevelDialogHitTest(HWND hOwner, int screenX, int screenY, HWND* lpHitHwnd)
{
    if (lpHitHwnd) *lpHitHwnd = 0;
    for (int i = MYWIN_MAX_DIALOGS - 1; i >= 0; --i) {
        MyWinDialogInfo* di = &g_DialogInfos[i];
        if (!di->valid || !di->hDlg || di->ended) continue;
        if (hOwner && di->hParent != hOwner) continue;
        MyWinWindowInfo* wi = mywin_find_info(di->hDlg);
        if (!wi || wi->hParent != 0 || !(wi->style & WS_VISIBLE)) continue;
        int l = (int)wi->rcClient.left;
        int t = (int)wi->rcClient.top;
        int r = (int)wi->rcClient.right;
        int b = (int)wi->rcClient.bottom;
        int clientY0 = t + MYWIN_DIALOG_CAPTION_H;

        /* v92.2: test child controls first using dialog-client coordinates.
           ChildWindowFromPoint() already expands COMBOBOX when its dropdown is
           open, so wheel/click hit-testing can reach the dropdown even if it
           extends below the normal #32770 rectangle. */
        if (screenY >= clientY0) {
            POINT pt;
            pt.x = screenX - l;
            pt.y = screenY - clientY0;
            HWND child = ChildWindowFromPoint(di->hDlg, pt);
            if (child && IsWindowEnabled(child)) {
                if (lpHitHwnd) *lpHitHwnd = child;
                return TRUE;
            }
        }

        if (screenX < l || screenX >= r || screenY < t || screenY >= b) continue;
        if (lpHitHwnd) *lpHitHwnd = di->hDlg;
        return TRUE;
    }
    return FALSE;
}

static HWND mywin_dialog_child_at(HWND hDlg, int x, int y)
{
    POINT pt; pt.x = x; pt.y = y;
    HWND child = ChildWindowFromPoint(hDlg, pt);
    return child;
}

static void mywin_dialog_forward_mouse(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);
    HWND child = mywin_dialog_child_at(hDlg, x, y);
    if (!child || child == hDlg) return;
    MyWinWindowInfo* ci = mywin_find_info(child);
    if (!ci || !ci->wndproc) return;
    LPARAM clp = MAKELPARAM((WORD)(x - ci->rcClient.left), (WORD)(y - ci->rcClient.top));
    ci->wndproc(child, Msg, wParam, clp);
}

static void mywin_dialog_move_from_client(HWND hDlg, int clientX, int clientY, int offX, int offY)
{
    MyWinWindowInfo* wi = mywin_find_info(hDlg);
    if (!wi) return;
    int w = (int)(wi->rcClient.right - wi->rcClient.left);
    int h = (int)(wi->rcClient.bottom - wi->rcClient.top);

    /* v88.2: dialogs are owned top-level USER32-lite windows, not child
       controls clipped to the owner client.  Mouse messages arrive in dialog
       client coordinates, so reconstruct the current screen point and move the
       top-left without clamping to the parent rectangle. */
    int screenX = (int)wi->rcClient.left + clientX;
    int screenY = (int)wi->rcClient.top + MYWIN_DIALOG_CAPTION_H + clientY;
    int nx = screenX - offX;
    int ny = screenY - offY;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    wi->rcClient.left = nx;
    wi->rcClient.top = ny;
    wi->rcClient.right = nx + w;
    wi->rcClient.bottom = ny + h;
    if (g_HasCapability)
        hwnd_post(g_lpHwndManager, &g_CurrentCapability, hDlg, WM_MOVE, 0, MAKELPARAM((WORD)nx, (WORD)ny));
}

static HWND mywin_find_dialog_button(HWND hDlg, int id)
{
    HWND h = GetDlgItem(hDlg, id);
    if (h) return h;
    return 0;
}

static void mywin_dialog_send_command(HWND hDlg, int id, UINT code)
{
    HWND ctl = GetDlgItem(hDlg, id);
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    DLGPROC dlgProc = mywin_dialog_current_proc(hDlg, di);
    if (dlgProc) {
        mywin_dialog_set_msg_result(hDlg, 0);
        if (dlgProc(hDlg, WM_COMMAND, MAKEWPARAM((WORD)id, (WORD)code), (LPARAM)ctl))
            return;
    }
    if (id == IDCANCEL) EndDialog(hDlg, IDCANCEL);
    else if (id == IDOK) EndDialog(hDlg, IDOK);
}

LRESULT CALLBACK DefDlgProcA(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);

    /* v153: #32770 keeps DefDlgProcA as GWLP_WNDPROC.  The application
       DLGPROC lives in DWLP_DLGPROC and returns TRUE/FALSE for handled state;
       the actual LRESULT is DWLP_MSGRESULT, matching the Win32 dialog contract. */
    DLGPROC dlgProc = mywin_dialog_current_proc(hDlg, di);
    if (dlgProc && Msg != WM_NCCREATE && Msg != WM_CREATE && Msg != WM_INITDIALOG) {
        mywin_dialog_set_msg_result(hDlg, 0);
        INT_PTR handled = dlgProc(hDlg, Msg, wParam, lParam);
        if (handled) return mywin_dialog_get_msg_result(hDlg);
    }

    switch (Msg) {
    case DM_GETDEFID: {
        int id = mywin_dialog_get_default_id(hDlg);
        return id ? MAKELRESULT((WORD)id, DC_HASDEFID) : 0;
    }
    case DM_SETDEFID:
        mywin_dialog_apply_default_id(hDlg, (int)wParam);
        return TRUE;
    case WM_NEXTDLGCTL: {
        HWND target = 0;
        if (lParam) target = (HWND)wParam;
        else {
            HWND cur = mywin_dialog_current_focus(hDlg);
            target = GetNextDlgTabItem(hDlg, (cur && cur != hDlg) ? cur : 0, wParam ? TRUE : FALSE);
        }
        if (target) { SetFocus(target); return TRUE; }
        return FALSE;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        /* ScreenToClient(#32770) returns dialog-client coordinates.  The
           caption therefore appears as negative client Y.  Use that NC band
           for a lightweight modal/modeless child-dialog move path. */
        if (y < 0 && y >= -MYWIN_DIALOG_CAPTION_H) {
            if (di) {
                di->dragActive = 1;
                di->dragOffX = x;
                di->dragOffY = y + MYWIN_DIALOG_CAPTION_H;
            }
            SetFocus(hDlg);
            SetCapture(hDlg);
            return 0;
        }
        mywin_dialog_forward_mouse(hDlg, Msg, wParam, lParam);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (di && di->dragActive && GetCapture() == hDlg) {
            mywin_dialog_move_from_client(hDlg, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), di->dragOffX, di->dragOffY);
            return 0;
        }
        mywin_dialog_forward_mouse(hDlg, Msg, wParam, lParam);
        return 0;
    case WM_LBUTTONUP:
        if (di && di->dragActive) {
            di->dragActive = 0;
            if (GetCapture() == hDlg) ReleaseCapture();
            return 0;
        }
        mywin_dialog_forward_mouse(hDlg, Msg, wParam, lParam);
        return 0;
    case WM_KEYDOWN:
        if ((int)wParam == KEY_ESC) { mywin_dialog_send_command(hDlg, IDCANCEL, BN_CLICKED); return TRUE; }
        if ((int)wParam == KEY_ENTER) {
            HWND def = mywin_dialog_get_default_button(hDlg);
            MyWinWindowInfo* defWi = mywin_find_info(def);
            mywin_dialog_send_command(hDlg, defWi ? (int)defWi->id : IDOK, BN_CLICKED);
            return TRUE;
        }
        break;
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        UINT code = HIWORD(wParam);
        /* v97.2: BN_SETFOCUS/BN_KILLFOCUS notifications from OK/Cancel must
           not activate the dialog result.  v97.1 let any WM_COMMAND with
           id==IDOK/IDCANCEL close the dialog, so pressing Tab onto OK/Cancel
           looked like a Cancel/OK click. */
        if ((id == IDOK || id == IDCANCEL) && (code == BN_CLICKED || code == 0)) {
            EndDialog(hDlg, (INT_PTR)id);
            return TRUE;
        }
        return 0;
    }
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    case WM_DESTROY:
        if (di && !di->ended) {
            di->ended = 1;
            di->result = IDCANCEL;
        }
        return 0;
    default:
        return DefWindowProcA(hDlg, Msg, wParam, lParam);
    }
    return DefWindowProcA(hDlg, Msg, wParam, lParam);
}

static ATOM mywin_register_dialog_class(void)
{
    if (g_DialogClassAtom) return g_DialogClassAtom;
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefDlgProcA;
    wc.cbWndExtra = DLGWINDOWEXTRA; /* v153: DWLP_MSGRESULT/DLGPROC/USER storage */
    wc.lpszClassName = "#32770";
    g_DialogClassAtom = RegisterClassExA(&wc);
    return g_DialogClassAtom;
}

static BOOL MyRegisterBuiltinControls(void)
{
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);

    wc.lpfnWndProc = MyButtonWndProc;
    wc.lpszClassName = "BUTTON";
    RegisterClassExA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MyStaticWndProc;
    wc.lpszClassName = "STATIC";
    RegisterClassExA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MyEditWndProc;
    wc.lpszClassName = "EDIT";
    RegisterClassExA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MyListBoxWndProc;
    wc.lpszClassName = "LISTBOX";
    RegisterClassExA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MyScrollBarWndProc;
    wc.lpszClassName = "SCROLLBAR";
    RegisterClassExA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MyComboBoxWndProc;
    wc.lpszClassName = "COMBOBOX";
    RegisterClassExA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MyMdiClientWndProc;
    wc.lpszClassName = "MDICLIENT";
    RegisterClassExA(&wc);

    return TRUE;
}


static BOOL mywin_has_cap(DWORD dwCap)
{
    return (g_HasCapability && (g_CurrentCapability.flags & dwCap)) ? TRUE : FALSE;
}

static DWORD mywin_current_process_id(void)
{
    return g_HasCapability ? (DWORD)g_CurrentCapability.id : 0;
}

static DWORD mywin_current_thread_id(void)
{
    /* v130: current PoC still has one UI thread per Capability, but USER32
       code must use the thread dimension explicitly so real thread IDs can
       replace this later without rewriting window ownership checks. */
    return g_HasCapability ? (DWORD)g_CurrentCapability.id : 0;
}

static BOOL mywin_owns_window(HWND hWnd)
{
    if (!g_lpHwndManager || !g_HasCapability || !hWnd) return FALSE;
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (wi)
        return (wi->dwProcessId == mywin_current_process_id() &&
                wi->dwThreadId == mywin_current_thread_id()) ? TRUE : FALSE;
    return (hwnd_get_owner_pid(g_lpHwndManager, hWnd) == g_CurrentCapability.id &&
            hwnd_get_owner_tid(g_lpHwndManager, hWnd) == g_CurrentCapability.id) ? TRUE : FALSE;
}

#define MYWIN_MAX_INPUT_ATTACH 16
typedef struct MyWinInputAttach {
    int valid;
    DWORD a;
    DWORD b;
} MyWinInputAttach;

static MyWinInputAttach g_InputAttach[MYWIN_MAX_INPUT_ATTACH];

static BOOL mywin_threads_attached(DWORD a, DWORD b)
{
    if (!a || !b) return FALSE;
    if (a == b) return TRUE;
    for (int i = 0; i < MYWIN_MAX_INPUT_ATTACH; ++i) {
        if (!g_InputAttach[i].valid) continue;
        if ((g_InputAttach[i].a == a && g_InputAttach[i].b == b) ||
            (g_InputAttach[i].a == b && g_InputAttach[i].b == a))
            return TRUE;
    }
    return FALSE;
}

static BOOL mywin_is_current_input_thread(HWND hWnd)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    DWORD tid = wi ? wi->dwThreadId : (g_lpHwndManager ? hwnd_get_owner_tid(g_lpHwndManager, hWnd) : 0);
    return mywin_threads_attached(mywin_current_thread_id(), tid);
}

static BOOL mywin_same_window_owner(HWND a, HWND b)
{
    MyWinWindowInfo* wa = mywin_find_info(a);
    MyWinWindowInfo* wb = mywin_find_info(b);
    DWORD ap = wa ? wa->dwProcessId : (g_lpHwndManager ? hwnd_get_owner_pid(g_lpHwndManager, a) : 0);
    DWORD at = wa ? wa->dwThreadId : (g_lpHwndManager ? hwnd_get_owner_tid(g_lpHwndManager, a) : 0);
    DWORD bp = wb ? wb->dwProcessId : (g_lpHwndManager ? hwnd_get_owner_pid(g_lpHwndManager, b) : 0);
    DWORD bt = wb ? wb->dwThreadId : (g_lpHwndManager ? hwnd_get_owner_tid(g_lpHwndManager, b) : 0);
    return (ap && at && ap == bp && at == bt) ? TRUE : FALSE;
}

static BOOL mywin_can_read_window(HWND hWnd)
{
    if (!g_lpHwndManager || !hWnd ||
        !hwnd_query_action(g_lpHwndManager, hWnd, _HWND_ACTION_QUERY, NULL))
        return FALSE;
    return mywin_owns_window(hWnd) || mywin_has_cap(CAP_WINDOW_READ);
}

static BOOL mywin_can_control_window(HWND hWnd)
{
    if (!g_lpHwndManager || !hWnd ||
        !hwnd_query_action(g_lpHwndManager, hWnd, _HWND_ACTION_MUTATE, NULL))
        return FALSE;
    return mywin_owns_window(hWnd) || mywin_has_cap(CAP_WINDOW_CONTROL);
}

static BOOL mywin_can_mutate_window(HWND hWnd)
{
    /* v131: HWND access-control contract.  CAP_WINDOW_CONTROL may request
       controlled cross-process operations through documented paths later, but
       it must not mutate foreign USER32 metadata or inject into foreign MDI
       trees directly.  USER32-owned HWND state is thread-affine and owner-only
       for destructive/mutating APIs in this pass. */
    return mywin_owns_window(hWnd);
}

static BOOL mywin_is_foreign_window(HWND hWnd)
{
    return (g_lpHwndManager && hWnd &&
            hwnd_query_action(g_lpHwndManager, hWnd, _HWND_ACTION_QUERY, NULL) &&
            !mywin_owns_window(hWnd)) ? TRUE : FALSE;
}

static BOOL mywin_is_safe_foreign_message(UINT Msg, WPARAM wParam)
{
    /* v131: a tiny request-only allow-list.  Arbitrary WM_COMMAND/WM_SETTEXT/
       MDI messages are injection, not cross-process control.  WM_CLOSE and
       SC_CLOSE remain request-shaped and are delivered through the queue path. */
    if (Msg == WM_CLOSE) return TRUE;
    if (Msg == WM_SYSCOMMAND && (wParam & 0xfff0u) == SC_CLOSE) return TRUE;
    return FALSE;
}

static BOOL mywin_can_message_window(HWND hWnd, UINT Msg, WPARAM wParam)
{
    if (!g_lpHwndManager || !g_HasCapability || !hWnd ||
        !hwnd_query_action(g_lpHwndManager, hWnd, _HWND_ACTION_MESSAGE, NULL)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (mywin_owns_window(hWnd)) return TRUE;
    if (!mywin_has_cap(CAP_IPC)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (mywin_is_safe_foreign_message(Msg, wParam)) return TRUE;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

/* v221: USER32-local action context.  Public HWNDs resolve once into an
   internal header/action state; later code consumes the resolved state instead
   of rechecking the lifecycle with scattered IsWindow()/destroying tests. */
typedef struct _UserHwndRef {
    HWND hWnd;
    _HwndHeader header;
    MyWinWindowInfo* wi;
    DWORD action;
} _UserHwndRef;

static BOOL _HwndResolveForAction(HWND hWnd, DWORD action, _UserHwndRef* out, DWORD invalidError)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!g_lpHwndManager || !hWnd) {
        SetLastError(invalidError);
        return FALSE;
    }

    _HwndHeader header;
    if (!hwnd_query_action(g_lpHwndManager, hWnd, action, &header)) {
        SetLastError(invalidError);
        return FALSE;
    }

    if (out) {
        out->hWnd = hWnd;
        out->header = header;
        out->wi = mywin_find_info(hWnd);
        out->action = action;
    }
    return TRUE;
}


static BOOL mywin_get_local_window_rect_screen(MyWinWindowInfo* wi, RECT* out)
{
    if (!wi || !wi->valid || !out) return FALSE;

    RECT r = wi->rcClient;
    if (wi->hParent) {
        int px = 0, py = 0;
        if (!mywin_client_origin_screen(wi->hParent, &px, &py)) return FALSE;
        r.left   += px;
        r.right  += px;
        r.top    += py;
        r.bottom += py;
    }

    *out = r;
    return TRUE;
}


static BOOL mywin_effective_enabled_from_info(MyWinWindowInfo* wi)
{
    while (wi) {
        if (wi->style & WS_DISABLED) return FALSE;
        if (strcmp(wi->className, "#32770") == 0) break;
        if (!wi->hParent) break;
        wi = mywin_find_info(wi->hParent);
    }
    return wi ? TRUE : FALSE;
}

static void mywin_publish_local_hwnd_state(HWND hWnd, UINT msg, DWORD dirtyFlags)
{
    if (!g_lpHwndManager || !hWnd) return;
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi || !wi->valid) return;

    MyWindowState st;
    memset(&st, 0, sizeof(st));
    st.cbSize = sizeof(st);
    st.hWnd = hWnd;
    st.ownerPid = wi->dwProcessId ? wi->dwProcessId : mywin_current_process_id();
    st.ownerTid = wi->dwThreadId ? wi->dwThreadId : st.ownerPid;

    RECT wr;
    if (!mywin_get_local_window_rect_screen(wi, &wr))
        wr = wi->rcClient;
    st.rcWindow = wr;
    st.rcClient = wr;

    st.visible = mywin_is_visible_chain(hWnd) ? TRUE : FALSE;
    st.minimized = FALSE;
    st.active = (g_FocusHwnd == hWnd) ? TRUE : FALSE;
    st.focused = st.active;
    st.enabled = mywin_effective_enabled_from_info(wi);
    st.hasCapture = (g_CaptureHwnd == hWnd || mywin_is_descendant(hWnd, g_CaptureHwnd)) ? TRUE : FALSE;
    st.destroyed = FALSE;
    st.dirtyFlags = dirtyFlags ? dirtyFlags : (MYWS_DIRTY_RECT|MYWS_DIRTY_VISIBLE|MYWS_DIRTY_TEXT|MYWS_DIRTY_OWNER);
    st.zOrder = wi->zOrder;
    st.style = wi->style;
    st.exStyle = wi->exStyle;
    st.lastMessage = msg;
    mywin_copy_cstr_trunc(st.szTitle, sizeof(st.szTitle), wi->text);

    hwnd_update_window_state(g_lpHwndManager, &st);
}

static void mywin_publish_local_hwnd_tree_state(HWND hWnd, UINT msg, DWORD dirtyFlags)
{
    if (!hWnd) return;
    mywin_publish_local_hwnd_state(hWnd, msg, dirtyFlags);
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int count = mywin_collect_children(hWnd, children, MYWIN_MAX_WINDOW_INFOS);
    for (int i = 0; i < count; ++i)
        mywin_publish_local_hwnd_tree_state(children[i], msg, dirtyFlags);
}

static void mywin_local_send_window_pos_messages(HWND hWnd, MyWinWindowInfo* wi,
                                                 const RECT* oldRc, const RECT* newRc,
                                                 HWND hWndInsertAfter, UINT flags)
{
    if (!wi || !wi->wndproc || !oldRc || !newRc) return;

    WINDOWPOS wp;
    memset(&wp, 0, sizeof(wp));
    wp.hwnd = hWnd;
    wp.hwndInsertAfter = hWndInsertAfter;
    wp.x = (int)newRc->left;
    wp.y = (int)newRc->top;
    wp.cx = (int)(newRc->right - newRc->left);
    wp.cy = (int)(newRc->bottom - newRc->top);
    wp.flags = flags;

    wi->wndproc(hWnd, WM_WINDOWPOSCHANGED, 0, (LPARAM)&wp);

    if (!(flags & SWP_NOMOVE) && (oldRc->left != newRc->left || oldRc->top != newRc->top))
        wi->wndproc(hWnd, WM_MOVE, 0, MAKELPARAM((WORD)newRc->left, (WORD)newRc->top));

    if (!(flags & SWP_NOSIZE) &&
        ((oldRc->right - oldRc->left) != (newRc->right - newRc->left) ||
         (oldRc->bottom - oldRc->top) != (newRc->bottom - newRc->top))) {
        int cw = (int)(newRc->right - newRc->left);
        int ch = (int)(newRc->bottom - newRc->top);
        if (cw < 0) cw = 0;
        if (ch < 0) ch = 0;
        wi->wndproc(hWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM((WORD)cw, (WORD)ch));
    }
}

static BOOL mywin_can_subscribe(HWND hWndSource, HWND hWndSubscriber)
{
    if (!g_lpHwndManager || !g_HasCapability || !hWndSubscriber) return FALSE;
    /* v74: hWndSource==0 means global HWND-state dirty subscription. */
    if (!hWndSource) return mywin_has_cap(CAP_WINDOW_SUBSCRIBE);
    if (mywin_owns_window(hWndSource) && mywin_owns_window(hWndSubscriber)) return TRUE;
    return mywin_has_cap(CAP_WINDOW_SUBSCRIBE);
}

HWND SetCapture(HWND hWnd)
{
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_CAPTURE, &ref, ERROR_INVALID_HANDLE)) return 0;
    if (!(mywin_owns_window(hWnd) || mywin_has_cap(CAP_WINDOW_CONTROL)) || !IsWindowEnabled(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    if (!mywin_is_current_input_thread(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }

    /* v222: capture enters through the HWND state-machine action path.  The
       later input routing code consumes the resolved window identity instead
       of re-deriving lifecycle validity from scattered IsWindow checks. */
    HWND old = g_CaptureHwnd;
    if (old == hWnd) return old;
    g_CaptureHwnd = hWnd;
    if (old && hwnd_query_action(g_lpHwndManager, old, _HWND_ACTION_MESSAGE, NULL))
        SendMessageA(old, WM_CAPTURECHANGED, 0, (LPARAM)hWnd);
    return old;
}

BOOL ReleaseCapture(void)
{
    if (!g_lpHwndManager || !g_CaptureHwnd) return FALSE;
    HWND old = g_CaptureHwnd;
    g_CaptureHwnd = 0;
    if (old && hwnd_query_action(g_lpHwndManager, old, _HWND_ACTION_MESSAGE, NULL))
        SendMessageA(old, WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}

HWND GetCapture(void)
{
    return g_CaptureHwnd;
}

void mywin_note_runtime_process(const Capability* cap, HANDLE processObject, HANDLE threadObject, const char* imageName);

BOOL MyWinBindRuntime(HWNDManager* lpHwndManager, const Capability* lpCapability)
{
    if (!lpHwndManager || !lpCapability) return FALSE;
    g_lpHwndManager = lpHwndManager;
    g_CurrentCapability = *lpCapability;
    g_HasCapability = 1;
    MyRegisterBuiltinControls();

    HANDLE hProcessObject = 0;
    HANDLE hThreadObject = 0;
    if (!mywin_ensure_runtime_process_objects(lpCapability, &hProcessObject, &hThreadObject)) return FALSE;
    mywin_note_runtime_process(lpCapability, hProcessObject, hThreadObject, lpCapability->name);
    return TRUE;
}

void MyWinUnbindRuntime(void)
{
    g_lpHwndManager = NULL;
    g_lpWindowManager = NULL;
    memset(&g_CurrentCapability, 0, sizeof(g_CurrentCapability));
    memset(g_RuntimeStack, 0, sizeof(g_RuntimeStack));
    g_RuntimeDepth = 0;
    g_HasCapability = 0;
}

BOOL MyWinBindDesktop(WindowManager* lpWindowManager)
{
    g_lpWindowManager = lpWindowManager;
    return lpWindowManager ? TRUE : FALSE;
}

const Capability* MyWinGetCurrentCapability(void)
{
    return g_HasCapability ? &g_CurrentCapability : NULL;
}

BOOL MyWinEnsureSessionInputRuntime(HWNDManager* lpHwndManager, WindowManager* lpWindowManager, const Capability* lpPreferredCapability)
{
    HWNDManager* mgr = lpHwndManager ? lpHwndManager : g_lpHwndManager;
    if (!mgr && lpWindowManager) mgr = lpWindowManager->mgr;
    if (!mgr) return FALSE;

    const DWORD required = CAP_IPC | CAP_WINDOW_ENUM | CAP_WINDOW_READ | CAP_WINDOW_CONTROL | CAP_WINDOW_SUBSCRIBE;
    const Capability* cur = MyWinGetCurrentCapability();
    if (cur && ((cur->flags & required) == required)) {
        if (lpWindowManager) MyWinBindDesktop(lpWindowManager);
        return TRUE;
    }

    Capability cap;
    if (lpPreferredCapability && lpPreferredCapability->id) {
        cap = *lpPreferredCapability;
        cap.flags |= required;
    } else {
        cap = cap_create(78, "session-input", CAP_ADMIN | required);
    }
    cap_add_target(&cap, 0);

    if (!MyWinBindRuntime(mgr, &cap)) return FALSE;
    if (lpWindowManager) MyWinBindDesktop(lpWindowManager);
    return TRUE;
}

HWNDManager* MyWinGetHwndManager(void)
{
    return g_lpHwndManager;
}

static BOOL mywin_is_int_atom_class(LPCSTR lpClassName)
{
    return lpClassName && (((uintptr_t)lpClassName >> 16) == 0);
}

static BOOL mywin_is_system_class_name(LPCSTR lpClassName)
{
    if (!lpClassName || mywin_is_int_atom_class(lpClassName)) return FALSE;
    return strcasecmp(lpClassName, "BUTTON") == 0 ||
           strcasecmp(lpClassName, "STATIC") == 0 ||
           strcasecmp(lpClassName, "EDIT") == 0 ||
           strcasecmp(lpClassName, "LISTBOX") == 0 ||
           strcasecmp(lpClassName, "SCROLLBAR") == 0 ||
           strcasecmp(lpClassName, "COMBOBOX") == 0 ||
           strcasecmp(lpClassName, "MDICLIENT") == 0 ||
           strcmp(lpClassName, "#32770") == 0 ||
           strcmp(lpClassName, "#32769") == 0 ||
           strcmp(lpClassName, "Shell_TrayWnd") == 0;
}


static int mywin_extra_slots_for_bytes(int bytes)
{
    if (bytes <= 0) return 0;
    int sz = (int)sizeof(LONG_PTR);
    return (bytes + sz - 1) / sz;
}

static BOOL mywin_extra_index_valid(int nIndex, int cbExtra)
{
    if (nIndex < 0) return FALSE;
    if ((nIndex % (int)sizeof(LONG_PTR)) != 0) return FALSE;
    if (nIndex + (int)sizeof(LONG_PTR) > cbExtra) return FALSE;
    return TRUE;
}

static void mywin_class_freestack_init(void)
{
    if (g_ClassFreeInit) return;
    g_ClassFreeTop = 0;
    for (int i = MYWIN_MAX_CLASSES - 1; i >= 0; --i) g_ClassFreeStack[g_ClassFreeTop++] = i;
    g_ClassFreeInit = 1;
}

static int mywin_class_alloc_slot(void)
{
    mywin_class_freestack_init();
    if (g_ClassFreeTop <= 0) return -1;
    return g_ClassFreeStack[--g_ClassFreeTop];
}

static void mywin_class_release_slot(int idx)
{
    mywin_class_freestack_init();
    if (idx < 0 || idx >= MYWIN_MAX_CLASSES || g_ClassFreeTop >= MYWIN_MAX_CLASSES) return;
    g_ClassFreeStack[g_ClassFreeTop++] = idx;
}

static DWORD mywin_class_name_hash(LPCSTR s)
{
    DWORD h = 2166136261u;
    if (!s) return h;
    while (*s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
        h ^= ch;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static DWORD mywin_title_hash(LPCSTR s)
{
    DWORD h = 2166136261u;
    if (!s) return h;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static DWORD mywin_class_exact_hash(LPCSTR name, DWORD ownerPid, HINSTANCE hInstance, BOOL systemClass)
{
    DWORD h = mywin_class_name_hash(name);
    h ^= ownerPid + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= (DWORD)((uintptr_t)hInstance & 0xffffffffu) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= (systemClass ? 0x51ed270bu : 0x2c1b3c6du);
    return h ? h : 1u;
}

static inline int mywin_class_bucket(DWORD hash)
{
    return (int)(hash & MYWIN_CLASS_HASH_MASK);
}

static void mywin_class_hash_insert(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_CLASSES || !g_Classes[idx].valid) return;
    DWORD h = mywin_class_exact_hash(g_Classes[idx].className, g_Classes[idx].ownerPid,
                                     g_Classes[idx].hInstance, g_Classes[idx].systemClass ? TRUE : FALSE);
    int b = mywin_class_bucket(h);
    g_Classes[idx].classHash = h;
    g_Classes[idx].classHashNext = g_ClassNameHash[b];
    g_ClassNameHash[b] = idx + 1;

    DWORD ah = g_Classes[idx].atom ? (DWORD)g_Classes[idx].atom : 1u;
    int ab = mywin_class_bucket(ah);
    g_Classes[idx].atomHashNext = g_ClassAtomHash[ab];
    g_ClassAtomHash[ab] = idx + 1;
}

static void mywin_class_hash_remove(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_CLASSES || !g_Classes[idx].valid) return;
    if (g_Classes[idx].classHash) {
        int b = mywin_class_bucket(g_Classes[idx].classHash);
        int* link = &g_ClassNameHash[b];
        while (*link) {
            int cur = *link - 1;
            if (cur == idx) { *link = g_Classes[cur].classHashNext; break; }
            link = &g_Classes[cur].classHashNext;
        }
    }
    if (g_Classes[idx].atom) {
        int b = mywin_class_bucket((DWORD)g_Classes[idx].atom);
        int* link = &g_ClassAtomHash[b];
        while (*link) {
            int cur = *link - 1;
            if (cur == idx) { *link = g_Classes[cur].atomHashNext; break; }
            link = &g_Classes[cur].atomHashNext;
        }
    }
    g_Classes[idx].classHash = 0;
    g_Classes[idx].classHashNext = 0;
    g_Classes[idx].atomHashNext = 0;
}

static void mywin_free_class_entry(MyWinClassEntry* cls)
{
    if (!cls) return;
    int idx = -1;
    if (cls >= g_Classes && cls < g_Classes + MYWIN_MAX_CLASSES) {
        idx = (int)(cls - g_Classes);
        mywin_class_hash_remove(idx);
    }
    if (cls->clsExtra) {
        free(cls->clsExtra);
        cls->clsExtra = NULL;
    }
    memset(cls, 0, sizeof(*cls));
    if (idx >= 0) mywin_class_release_slot(idx);
}

static MyWinClassEntry* find_class_by_atom(ATOM atom)
{
    if (!atom) return NULL;
    if (MYOS_LIKELY(g_ClassAtomLookupCacheAtom == atom && g_ClassAtomLookupCachePtr)) {
        MyWinClassEntry* cached = g_ClassAtomLookupCachePtr;
        if (MYOS_LIKELY(cached->valid && cached->atom == atom)) return cached;
        g_ClassAtomLookupCacheAtom = 0;
        g_ClassAtomLookupCachePtr = NULL;
    }
    int b = mywin_class_bucket((DWORD)atom);
    for (int link = g_ClassAtomHash[b]; link; link = g_Classes[link - 1].atomHashNext) {
        int idx = link - 1;
        if (MYOS_UNLIKELY(idx < 0 || idx >= MYWIN_MAX_CLASSES)) break;
        MyWinClassEntry* cls = &g_Classes[idx];
        if (MYOS_LIKELY(cls->valid && cls->atom == atom)) {
            g_ClassAtomLookupCacheAtom = atom;
            g_ClassAtomLookupCachePtr = cls;
            return cls;
        }
    }
    for (int i = 0; i < MYWIN_MAX_CLASSES; i++) {
        if (g_Classes[i].valid && g_Classes[i].atom == atom) {
            g_ClassAtomLookupCacheAtom = atom;
            g_ClassAtomLookupCachePtr = &g_Classes[i];
            return &g_Classes[i];
        }
    }
    return NULL;
}

static MyWinClassEntry* find_class_exact(LPCSTR lpClassName, DWORD ownerPid, HINSTANCE hInstance, BOOL systemOnly)
{
    if (!lpClassName || mywin_is_int_atom_class(lpClassName)) return NULL;
    DWORD h = mywin_class_exact_hash(lpClassName, ownerPid, hInstance, systemOnly ? TRUE : FALSE);
    if (MYOS_LIKELY(g_ClassExactLookupCacheHash == h &&
                    g_ClassExactLookupCachePid == ownerPid &&
                    g_ClassExactLookupCacheInstance == hInstance &&
                    g_ClassExactLookupCacheSystemOnly == (systemOnly ? TRUE : FALSE) &&
                    g_ClassExactLookupCachePtr)) {
        MyWinClassEntry* cached = g_ClassExactLookupCachePtr;
        if (MYOS_LIKELY(cached->valid && cached->classHash == h &&
                        strcasecmp(cached->className, lpClassName) == 0 &&
                        ((systemOnly && cached->systemClass) ||
                         (!systemOnly && !cached->systemClass && cached->ownerPid == ownerPid && cached->hInstance == hInstance))))
            return cached;
        g_ClassExactLookupCacheHash = 0;
        g_ClassExactLookupCachePtr = NULL;
    }
    int b = mywin_class_bucket(h);
    for (int link = g_ClassNameHash[b]; link; link = g_Classes[link - 1].classHashNext) {
        int idx = link - 1;
        if (MYOS_UNLIKELY(idx < 0 || idx >= MYWIN_MAX_CLASSES)) break;
        MyWinClassEntry* cls = &g_Classes[idx];
        if (MYOS_UNLIKELY(!cls->valid)) continue;
        if (cls->classHash != h) continue;
        if (strcasecmp(cls->className, lpClassName) != 0) continue;
        if (systemOnly) {
            if (cls->systemClass) {
                g_ClassExactLookupCacheHash = h;
                g_ClassExactLookupCachePid = ownerPid;
                g_ClassExactLookupCacheInstance = hInstance;
                g_ClassExactLookupCacheSystemOnly = TRUE;
                g_ClassExactLookupCachePtr = cls;
                return cls;
            }
            continue;
        }
        if (!cls->systemClass && cls->ownerPid == ownerPid && cls->hInstance == hInstance) {
            g_ClassExactLookupCacheHash = h;
            g_ClassExactLookupCachePid = ownerPid;
            g_ClassExactLookupCacheInstance = hInstance;
            g_ClassExactLookupCacheSystemOnly = FALSE;
            g_ClassExactLookupCachePtr = cls;
            return cls;
        }
    }
    for (int i = 0; i < MYWIN_MAX_CLASSES; i++) {
        if (!g_Classes[i].valid) continue;
        if (strcasecmp(g_Classes[i].className, lpClassName) != 0) continue;
        if (systemOnly) {
            if (g_Classes[i].systemClass) {
                g_ClassExactLookupCacheHash = h;
                g_ClassExactLookupCachePid = ownerPid;
                g_ClassExactLookupCacheInstance = hInstance;
                g_ClassExactLookupCacheSystemOnly = TRUE;
                g_ClassExactLookupCachePtr = &g_Classes[i];
                return &g_Classes[i];
            }
            continue;
        }
        if (!g_Classes[i].systemClass &&
            g_Classes[i].ownerPid == ownerPid &&
            g_Classes[i].hInstance == hInstance) {
            g_ClassExactLookupCacheHash = h;
            g_ClassExactLookupCachePid = ownerPid;
            g_ClassExactLookupCacheInstance = hInstance;
            g_ClassExactLookupCacheSystemOnly = FALSE;
            g_ClassExactLookupCachePtr = &g_Classes[i];
            return &g_Classes[i];
        }
    }
    return NULL;
}

static MyWinClassEntry* find_class_by_name(LPCSTR lpClassName)
{
    /* v150: Win32 user classes are process/module scoped; builtins live in the
       system class namespace.  Resolver order mirrors USER32's caller-first
       behavior: atom, caller-owned class, then system class. */
    if (!lpClassName) return NULL;
    if (mywin_is_int_atom_class(lpClassName))
        return find_class_by_atom((ATOM)(uintptr_t)lpClassName);

    DWORD pid = mywin_current_process_id();
    MyWinClassEntry* app = find_class_exact(lpClassName, pid, 0, FALSE);
    if (app) return app;
    return find_class_exact(lpClassName, 0, 0, TRUE);
}

static MyWinClassEntry* find_class_for_create(LPCSTR lpClassName, HINSTANCE hInstance)
{
    if (!lpClassName) return NULL;
    if (mywin_is_int_atom_class(lpClassName))
        return find_class_by_atom((ATOM)(uintptr_t)lpClassName);

    DWORD pid = mywin_current_process_id();
    MyWinClassEntry* app = find_class_exact(lpClassName, pid, hInstance, FALSE);
    if (app) return app;
    if (hInstance) {
        app = find_class_exact(lpClassName, pid, 0, FALSE);
        if (app) return app;
    }
    return find_class_exact(lpClassName, 0, 0, TRUE);
}

ATOM RegisterClassExA(const WNDCLASSEXA* lpWndClass)
{
    /* v150: app-defined classes are isolated by owner PID + hInstance; system
       controls and desktop/dialog classes remain global.  This prevents two
       processes that both register "MainWindow" from sharing the wrong WndProc. */
    if (!lpWndClass || !lpWndClass->lpszClassName ||
        mywin_is_int_atom_class(lpWndClass->lpszClassName) ||
        !lpWndClass->lpfnWndProc) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    BOOL systemClass = mywin_is_system_class_name(lpWndClass->lpszClassName);
    DWORD ownerPid = systemClass ? 0u : mywin_current_process_id();
    HINSTANCE inst = systemClass ? 0 : lpWndClass->hInstance;
    if (!systemClass && !ownerPid) { SetLastError(ERROR_ACCESS_DENIED); return 0; }

    MyWinClassEntry* existing = find_class_exact(lpWndClass->lpszClassName, ownerPid, inst, systemClass);
    if (existing) return existing->atom;

    int i = mywin_class_alloc_slot();
    if (i < 0) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    memset(&g_Classes[i], 0, sizeof(g_Classes[i]));
    g_Classes[i].valid = 1;
    g_Classes[i].atom = g_NextAtom++;
    if (!g_Classes[i].atom) g_Classes[i].atom = g_NextAtom++;
    g_Classes[i].wc = *lpWndClass;
    g_Classes[i].ownerPid = ownerPid;
    g_Classes[i].hInstance = inst;
    g_Classes[i].systemClass = systemClass ? 1 : 0;
    g_Classes[i].clsExtraBytes = lpWndClass->cbClsExtra > 0 ? lpWndClass->cbClsExtra : 0;
    int clsSlots = mywin_extra_slots_for_bytes(g_Classes[i].clsExtraBytes);
    if (clsSlots > 0) {
        g_Classes[i].clsExtra = (LONG_PTR*)calloc((size_t)clsSlots, sizeof(LONG_PTR));
        if (!g_Classes[i].clsExtra) {
            memset(&g_Classes[i], 0, sizeof(g_Classes[i]));
            mywin_class_release_slot(i);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
    }
    snprintf(g_Classes[i].className, sizeof(g_Classes[i].className), "%s", lpWndClass->lpszClassName);
    g_Classes[i].wc.lpszClassName = g_Classes[i].className;
    g_Classes[i].wc.hInstance = inst;
    g_Classes[i].wc.cbClsExtra = g_Classes[i].clsExtraBytes;
    mywin_class_hash_insert(i);
    printf("[USER32] RegisterClassExA: class='%s' atom=%u owner=%lu%s\n",
           g_Classes[i].className, (unsigned)g_Classes[i].atom,
           (unsigned long)g_Classes[i].ownerPid,
           g_Classes[i].systemClass ? " system" : "");
    return g_Classes[i].atom;
}

ATOM UnregisterClassA(LPCSTR lpClassName, HINSTANCE hInstance)
{
    if (!lpClassName) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    MyWinClassEntry* cls = NULL;
    if (mywin_is_int_atom_class(lpClassName)) {
        cls = find_class_by_atom((ATOM)(uintptr_t)lpClassName);
    } else {
        DWORD pid = mywin_current_process_id();
        cls = find_class_exact(lpClassName, pid, hInstance, FALSE);
        if (!cls && hInstance) cls = find_class_exact(lpClassName, pid, 0, FALSE);
        if (!cls && find_class_exact(lpClassName, 0, 0, TRUE)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return 0;
        }
    }
    if (!cls) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (cls->systemClass || cls->ownerPid != mywin_current_process_id()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    ATOM atom = cls->atom;
    char name[64];
    snprintf(name, sizeof(name), "%s", cls->className);
    mywin_free_class_entry(cls);
    printf("[USER32] UnregisterClassA: class='%s' atom=%u owner=%lu\n",
           name, (unsigned)atom, (unsigned long)mywin_current_process_id());
    return atom;
}

void MyUser32CleanupProcessClasses(DWORD dwProcessId)
{
    if (!dwProcessId) return;
    for (int i = 0; i < MYWIN_MAX_CLASSES; i++) {
        if (g_Classes[i].valid && !g_Classes[i].systemClass && g_Classes[i].ownerPid == dwProcessId) {
            printf("[USER32] CleanupProcessClasses: pid=%lu class='%s' atom=%u\n",
                   (unsigned long)dwProcessId, g_Classes[i].className, (unsigned)g_Classes[i].atom);
            mywin_free_class_entry(&g_Classes[i]);
        }
    }
}

static LONG_PTR mywin_get_extra(MyWinWindowInfo* wi, int nIndex)
{
    if (!wi || !mywin_extra_index_valid(nIndex, wi->wndExtraBytes) || !wi->wndExtra) {
        SetLastError(ERROR_INVALID_INDEX);
        return 0;
    }
    SetLastError(ERROR_SUCCESS);
    return wi->wndExtra[nIndex / (int)sizeof(LONG_PTR)];
}

static LONG_PTR mywin_set_extra(MyWinWindowInfo* wi, int nIndex, LONG_PTR value)
{
    if (!wi || !mywin_extra_index_valid(nIndex, wi->wndExtraBytes) || !wi->wndExtra) {
        SetLastError(ERROR_INVALID_INDEX);
        return 0;
    }
    int slot = nIndex / (int)sizeof(LONG_PTR);
    LONG_PTR old = wi->wndExtra[slot];
    wi->wndExtra[slot] = value;
    SetLastError(ERROR_SUCCESS);
    return old;
}

static BOOL mywin_is_dialog_window_info(const MyWinWindowInfo* wi)
{
    return (wi && wi->valid && strcmp(wi->className, "#32770") == 0) ? TRUE : FALSE;
}

static BOOL mywin_dialog_offset_valid(int nIndex)
{
    return (nIndex == DWLP_MSGRESULT || nIndex == DWLP_DLGPROC || nIndex == DWLP_USER) ? TRUE : FALSE;
}

static LONG_PTR mywin_dialog_get_extra_direct(MyWinWindowInfo* wi, int nIndex)
{
    if (!mywin_is_dialog_window_info(wi) || !mywin_dialog_offset_valid(nIndex) ||
        !mywin_extra_index_valid(nIndex, wi->wndExtraBytes) || !wi->wndExtra)
        return 0;
    return wi->wndExtra[nIndex / (int)sizeof(LONG_PTR)];
}

static LONG_PTR mywin_dialog_set_extra_direct(MyWinWindowInfo* wi, int nIndex, LONG_PTR value)
{
    if (!mywin_is_dialog_window_info(wi) || !mywin_dialog_offset_valid(nIndex) ||
        !mywin_extra_index_valid(nIndex, wi->wndExtraBytes) || !wi->wndExtra)
        return 0;
    int slot = nIndex / (int)sizeof(LONG_PTR);
    LONG_PTR old = wi->wndExtra[slot];
    wi->wndExtra[slot] = value;
    return old;
}

static DLGPROC mywin_dialog_current_proc(HWND hDlg, MyWinDialogInfo* di)
{
    MyWinWindowInfo* wi = mywin_find_info(hDlg);
    DLGPROC proc = (DLGPROC)mywin_dialog_get_extra_direct(wi, DWLP_DLGPROC);
    if (!proc && di) proc = di->dlgProc;
    return proc;
}

static void mywin_dialog_store_proc(HWND hDlg, DLGPROC proc)
{
    MyWinWindowInfo* wi = mywin_find_info(hDlg);
    mywin_dialog_set_extra_direct(wi, DWLP_DLGPROC, (LONG_PTR)proc);
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    if (di) di->dlgProc = proc;
}

static void mywin_dialog_set_msg_result(HWND hDlg, LRESULT value)
{
    MyWinWindowInfo* wi = mywin_find_info(hDlg);
    mywin_dialog_set_extra_direct(wi, DWLP_MSGRESULT, (LONG_PTR)value);
}

static LRESULT mywin_dialog_get_msg_result(HWND hDlg)
{
    MyWinWindowInfo* wi = mywin_find_info(hDlg);
    return (LRESULT)mywin_dialog_get_extra_direct(wi, DWLP_MSGRESULT);
}

static MyWinClassEntry* mywin_class_for_window_info(MyWinWindowInfo* wi)
{
    if (!wi) return NULL;
    if (wi->classAtom) {
        MyWinClassEntry* cls = find_class_by_atom(wi->classAtom);
        if (cls) return cls;
    }
    if (wi->className[0]) {
        MyWinClassEntry* cls = find_class_exact(wi->className, wi->dwProcessId, wi->hInstance, FALSE);
        if (cls) return cls;
        cls = find_class_exact(wi->className, wi->dwProcessId, 0, FALSE);
        if (cls) return cls;
        cls = find_class_exact(wi->className, 0, 0, TRUE);
        if (cls) return cls;
    }
    return NULL;
}

static LONG_PTR mywin_get_class_extra(MyWinClassEntry* cls, int nIndex)
{
    if (!cls || !mywin_extra_index_valid(nIndex, cls->clsExtraBytes) || !cls->clsExtra) {
        SetLastError(ERROR_INVALID_INDEX);
        return 0;
    }
    SetLastError(ERROR_SUCCESS);
    return cls->clsExtra[nIndex / (int)sizeof(LONG_PTR)];
}

static LONG_PTR mywin_set_class_extra(MyWinClassEntry* cls, int nIndex, LONG_PTR value)
{
    if (!cls || !mywin_extra_index_valid(nIndex, cls->clsExtraBytes) || !cls->clsExtra) {
        SetLastError(ERROR_INVALID_INDEX);
        return 0;
    }
    int slot = nIndex / (int)sizeof(LONG_PTR);
    LONG_PTR old = cls->clsExtra[slot];
    cls->clsExtra[slot] = value;
    SetLastError(ERROR_SUCCESS);
    return old;
}

LONG_PTR GetWindowLongPtrA(HWND hWnd, int nIndex)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    switch (nIndex) {
    case GWL_STYLE:       SetLastError(ERROR_SUCCESS); return (LONG_PTR)wi->style;
    case GWL_EXSTYLE:     SetLastError(ERROR_SUCCESS); return (LONG_PTR)wi->exStyle;
    case GWLP_WNDPROC:    SetLastError(ERROR_SUCCESS); return (LONG_PTR)wi->wndproc;
    case GWLP_HINSTANCE:  SetLastError(ERROR_SUCCESS); return (LONG_PTR)wi->hInstance;
    case GWLP_HWNDPARENT: SetLastError(ERROR_SUCCESS); return (LONG_PTR)((wi->style & WS_CHILD) ? wi->hParent : wi->hOwner);
    case GWLP_ID:         SetLastError(ERROR_SUCCESS); return (LONG_PTR)wi->id;
    case GWLP_USERDATA:   SetLastError(ERROR_SUCCESS); return wi->userData;
    default:
        if (mywin_is_dialog_window_info(wi) && mywin_dialog_offset_valid(nIndex)) {
            SetLastError(ERROR_SUCCESS);
            return mywin_dialog_get_extra_direct(wi, nIndex);
        }
        return mywin_get_extra(wi, nIndex);
    }
}

LONG_PTR SetWindowLongPtrA(HWND hWnd, int nIndex, LONG_PTR dwNewLong)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (!mywin_can_mutate_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    LONG_PTR old = 0;
    switch (nIndex) {
    case GWL_STYLE:       old = (LONG_PTR)wi->style; wi->style = (DWORD)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GWL_EXSTYLE:     old = (LONG_PTR)wi->exStyle; wi->exStyle = (DWORD)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GWLP_WNDPROC:    old = (LONG_PTR)wi->wndproc; wi->wndproc = (WNDPROC)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GWLP_HINSTANCE:  old = (LONG_PTR)wi->hInstance; wi->hInstance = (HINSTANCE)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GWLP_HWNDPARENT:
        old = (LONG_PTR)((wi->style & WS_CHILD) ? wi->hParent : wi->hOwner);
        if (wi->style & WS_CHILD) mywin_set_parent_linked(wi, (HWND)dwNewLong);
        else wi->hOwner = (HWND)dwNewLong;
        SetLastError(ERROR_SUCCESS);
        return old;
    case GWLP_ID:         old = (LONG_PTR)wi->id; wi->id = (UINT)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GWLP_USERDATA:   old = wi->userData; wi->userData = dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    default:
        if (mywin_is_dialog_window_info(wi) && mywin_dialog_offset_valid(nIndex)) {
            old = mywin_dialog_set_extra_direct(wi, nIndex, dwNewLong);
            if (nIndex == DWLP_DLGPROC) {
                MyWinDialogInfo* di = mywin_find_dialog(hWnd);
                if (di) di->dlgProc = (DLGPROC)dwNewLong;
            }
            SetLastError(ERROR_SUCCESS);
            return old;
        }
        return mywin_set_extra(wi, nIndex, dwNewLong);
    }
}

LONG_PTR GetClassLongPtrA(HWND hWnd, int nIndex)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    MyWinClassEntry* cls = mywin_class_for_window_info(wi);
    if (!cls) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    switch (nIndex) {
    case GCL_STYLE:       SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.style;
    case GCL_CBCLSEXTRA:  SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.cbClsExtra;
    case GCL_CBWNDEXTRA:  SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.cbWndExtra;
    case GCLP_WNDPROC:    SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.lpfnWndProc;
    case GCLP_HMODULE:    SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.hInstance;
    case GCLP_HICON:      SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.hIcon;
    case GCLP_HCURSOR:    SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.hCursor;
    case GCLP_HBRBACKGROUND: SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.hbrBackground;
    case GCLP_MENUNAME:   SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.lpszMenuName;
    case GCW_ATOM:        SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->atom;
    case GCLP_HICONSM:    SetLastError(ERROR_SUCCESS); return (LONG_PTR)cls->wc.hIconSm;
    default:              return mywin_get_class_extra(cls, nIndex);
    }
}

LONG_PTR SetClassLongPtrA(HWND hWnd, int nIndex, LONG_PTR dwNewLong)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (!mywin_can_mutate_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    MyWinClassEntry* cls = mywin_class_for_window_info(wi);
    if (!cls) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (cls->systemClass || cls->ownerPid != mywin_current_process_id()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    LONG_PTR old = 0;
    switch (nIndex) {
    case GCL_STYLE:       old = (LONG_PTR)cls->wc.style; cls->wc.style = (UINT)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GCL_CBCLSEXTRA:
    case GCL_CBWNDEXTRA:
    case GCW_ATOM:
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    case GCLP_WNDPROC:    old = (LONG_PTR)cls->wc.lpfnWndProc; cls->wc.lpfnWndProc = (WNDPROC)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GCLP_HMODULE:    old = (LONG_PTR)cls->wc.hInstance; cls->wc.hInstance = (HINSTANCE)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GCLP_HICON:      old = (LONG_PTR)cls->wc.hIcon; cls->wc.hIcon = (HICON)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GCLP_HCURSOR:    old = (LONG_PTR)cls->wc.hCursor; cls->wc.hCursor = (HCURSOR)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GCLP_HBRBACKGROUND: old = (LONG_PTR)cls->wc.hbrBackground; cls->wc.hbrBackground = (HBRUSH)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GCLP_MENUNAME:   old = (LONG_PTR)cls->wc.lpszMenuName; cls->wc.lpszMenuName = (LPCSTR)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    case GCLP_HICONSM:    old = (LONG_PTR)cls->wc.hIconSm; cls->wc.hIconSm = (HICON)dwNewLong; SetLastError(ERROR_SUCCESS); return old;
    default:              return mywin_set_class_extra(cls, nIndex, dwNewLong);
    }
}

LONG GetClassLongA(HWND hWnd, int nIndex)
{
    return (LONG)GetClassLongPtrA(hWnd, nIndex);
}

LONG SetClassLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
    return (LONG)SetClassLongPtrA(hWnd, nIndex, (LONG_PTR)dwNewLong);
}

LONG GetWindowLongA(HWND hWnd, int nIndex)
{
    return (LONG)GetWindowLongPtrA(hWnd, nIndex);
}

LONG SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
    return (LONG)SetWindowLongPtrA(hWnd, nIndex, (LONG_PTR)dwNewLong);
}

LRESULT CallWindowProcA(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    return lpPrevWndFunc ? lpPrevWndFunc(hWnd, Msg, wParam, lParam) : 0;
}

static void mywin_hwnd_proc_thunk(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, void* lpUserData)
{
    MyWinThunk* thunk = (MyWinThunk*)lpUserData;
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    WNDPROC proc = wi && wi->wndproc ? wi->wndproc : (thunk ? thunk->proc : NULL);
    if (!proc) return;
    proc(hWnd, Msg, wParam, lParam);
}

/* AUDIT(v118): CreateWindowExA is now in the right MSDN-named file, but top-level
   geometry still splits between USER32 metadata and WindowManager slots. */
HWND CreateWindowExA(
    DWORD     dwExStyle,
    LPCSTR    lpClassName,
    LPCSTR    lpWindowName,
    DWORD     dwStyle,
    int       X,
    int       Y,
    int       nWidth,
    int       nHeight,
    HWND      hWndParent,
    HMENU     hMenu,
    HINSTANCE hInstance,
    LPVOID    lpParam)
{
    if (!g_lpHwndManager || !g_HasCapability) {
        printf("[USER32] CreateWindowExA failed: Runtime nicht gebunden\n");
        return 0;
    }

    MyWinClassEntry* cls = find_class_for_create(lpClassName, hInstance);
    if (!cls) {
        printf("[USER32] CreateWindowExA failed: class='%s' nicht registriert\n", lpClassName ? lpClassName : "(null)");
        return 0;
    }

    /* v124: lpParam is application data for CREATESTRUCTA::lpCreateParams.
       Do not reinterpret/dereference it as an internal thunk; Win32 callers are
       allowed to pass any pointer value meaningful to their own process. */
    HWNDWndProc proc = mywin_hwnd_proc_thunk;
    void* userdata = NULL;
    static MyWinThunk fallbackThunks[MAX_HWNDS];
    static int fallbackNext = 0;
    MyWinThunk* fb = &fallbackThunks[fallbackNext++ % MAX_HWNDS];
    fb->proc = cls->wc.lpfnWndProc;
    fb->lpParam = lpParam;
    userdata = fb;

    BOOL isChildWindow = (dwStyle & WS_CHILD) ? TRUE : FALSE;
    HWND visualParent = isChildWindow ? hWndParent : 0;
    HWND ownerWindow = (!isChildWindow) ? hWndParent : 0;

    /* v130: Parent, owner and thread-affinity are separate Win32 dimensions.
       Child parentage is the visual/clip hierarchy and must not cross into a
       foreign USER32 tree.  Top-level owner is tracked separately and no
       longer masquerades as GetParent(). */
    if (visualParent) {
        if (!IsWindow(visualParent)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
        if (!mywin_owns_window(visualParent)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    }
    if (ownerWindow) {
        if (!IsWindow(ownerWindow)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
        if (!mywin_is_current_input_thread(ownerWindow)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    }

    HWND hWnd = hwnd_create_no_create(g_lpHwndManager, proc, userdata, g_CurrentCapability);
    if (hWnd) {
        MyWinWindowInfo* wi = mywin_alloc_info(hWnd);
        if (wi) {
            wi->hParent = 0;
            wi->hOwner = ownerWindow;
            mywin_set_parent_linked(wi, visualParent);
            wi->dwProcessId = mywin_current_process_id();
            wi->dwThreadId = mywin_current_thread_id();
            wi->id = (UINT)(DWORD_PTR)hMenu;
            wi->style = dwStyle;
            wi->exStyle = dwExStyle;
            wi->rcClient.left = X;
            wi->rcClient.top = Y;
            wi->rcClient.right = X + nWidth;
            wi->rcClient.bottom = Y + nHeight;
            wi->classAtom = cls->atom;
            wi->wndproc = cls->wc.lpfnWndProc;
            wi->originalProc = cls->wc.lpfnWndProc;
            wi->hInstance = hInstance;
            wi->lpCreateParams = lpParam;
            wi->wndExtraBytes = cls->wc.cbWndExtra > 0 ? cls->wc.cbWndExtra : 0;
            int wndSlots = mywin_extra_slots_for_bytes(wi->wndExtraBytes);
            if (wndSlots > 0) {
                wi->wndExtra = (LONG_PTR*)calloc((size_t)wndSlots, sizeof(LONG_PTR));
                if (!wi->wndExtra) {
                    mywin_free_info(hWnd);
                    hwnd_destroy(g_lpHwndManager, hWnd);
                    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                    return 0;
                }
            }
            snprintf(wi->className, MYWIN_WINDOW_CLASS_CHARS, "%s", cls->className);
            wi->classNameHash = mywin_class_name_hash(wi->className);
            snprintf(wi->text, MYWIN_WINDOW_TEXT_CHARS, "%s", lpWindowName ? lpWindowName : "");
            wi->textHash = mywin_title_hash(wi->text);
            if (strcmp(wi->className, "MDICLIENT") == 0 && lpParam) {
                const CLIENTCREATESTRUCT* ccs = (const CLIENTCREATESTRUCT*)lpParam;
                wi->mdi->mdiWindowMenu = ccs->hWindowMenu;
                wi->mdi->mdiFirstChildId = ccs->idFirstChild;
            }
            if (visualParent && mywin_mdi_is_client(visualParent))
                wi->mdi->mdiIsChild = 1;

            CREATESTRUCTA cs;
            memset(&cs, 0, sizeof(cs));
            cs.lpCreateParams = lpParam;
            cs.hInstance = hInstance;
            cs.hMenu = hMenu;
            cs.hwndParent = hWndParent;
            cs.cy = nHeight;
            cs.cx = nWidth;
            cs.y = Y;
            cs.x = X;
            cs.style = (LONG)dwStyle;
            cs.lpszName = lpWindowName;
            cs.lpszClass = cls->className;
            cs.dwExStyle = dwExStyle;

            // v41: real USER32-ish lifecycle order. Metadata exists before
            // user code sees WM_NCCREATE / WM_CREATE.
            LRESULT nc = wi->wndproc(hWnd, WM_NCCREATE, 0, (LPARAM)&cs);
            if (nc == FALSE) {
                DestroyWindow(hWnd);
                SetLastError(ERROR_INVALID_PARAMETER);
                return 0;
            }

            LRESULT cr = wi->wndproc(hWnd, WM_CREATE, 0, (LPARAM)&cs);
            if (cr == -1) {
                DestroyWindow(hWnd);
                SetLastError(ERROR_INVALID_PARAMETER);
                return 0;
            }
            hwnd_set_state(g_lpHwndManager, hWnd, _HWND_STATE_LIVE);

            /* v124: lock down the first visible-window lifecycle edge.  A
               WS_VISIBLE CreateWindowExA synchronously notifies WM_SHOWWINDOW
               after WM_NCCREATE/WM_CREATE have succeeded, matching the order
               applications rely on before USER code sees the returned HWND. */
            if (dwStyle & WS_VISIBLE)
                wi->wndproc(hWnd, WM_SHOWWINDOW, TRUE, 0);

            /* v182: USER32-local HWNDs (especially child controls created by
               OOP batch CreateWindowExA) must publish their real rect/text/
               owner state into WSTS immediately.  The initial HWNDManager slot
               is only a reservation and still contains 0x0/<untitled>. */
            mywin_publish_local_hwnd_state(hWnd, WM_CREATE,
                                           MYWS_DIRTY_OWNER|MYWS_DIRTY_RECT|MYWS_DIRTY_TEXT|MYWS_DIRTY_VISIBLE);
        }
    }
    return hWnd;
}

BOOL DestroyWindow(HWND hWnd)
{
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_DESTROY, &ref, ERROR_INVALID_HANDLE))
        return FALSE;
    if (!mywin_owns_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

    MyWinWindowInfo* wi = ref.wi;
    HWND exposeOwner = wi ? (wi->hOwner ? wi->hOwner : wi->hParent) : 0;
    if (wi && wi->destroying)
        return TRUE; // v43: recursive DestroyWindow during WM_DESTROY is a no-op.
    if (wi)
        wi->destroying = 1;

    // v42/v43: USER32-style window tree teardown. DestroyWindow(parent)
    // destroys child HWNDs first, then the parent. WindowLong/UserData
    // metadata remains readable until after WM_NCDESTROY has been delivered.
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int count = mywin_collect_children(hWnd, children, MYWIN_MAX_WINDOW_INFOS);
    for (int i = count - 1; i >= 0; i--) {
        if (IsWindow(children[i]))
            DestroyWindow(children[i]);
    }

    if (g_CaptureHwnd == hWnd || mywin_is_descendant(hWnd, g_CaptureHwnd))
        ReleaseCapture();
    if (g_FocusHwnd == hWnd || mywin_is_descendant(hWnd, g_FocusHwnd))
        SetFocus(0);

    if (wi && wi->mdi->mdiIsChild && wi->hParent && IsWindow(wi->hParent)) {
        MyWinWindowInfo* ci = mywin_find_info(wi->hParent);
        if (ci && ci->mdi->mdiActiveChild == hWnd) {
            ci->mdi->mdiActiveChild = 0;
            HWND next = GetWindow(hWnd, GW_HWNDNEXT);
            if (!next) next = GetWindow(hWnd, GW_HWNDPREV);
            if (next && mywin_mdi_is_child(next))
                mywin_mdi_activate_child(wi->hParent, next);
        }
    }

    hwnd_destroy(g_lpHwndManager, hWnd);

    /* v153: modeless dialogs destroyed directly (not via EndDialog) must not
       leak their small DialogInfo slot.  Modal loops keep the slot until the
       waiter reads the result. */
    MyWinDialogInfo* destroyedDialog = mywin_find_dialog(hWnd);
    if (destroyedDialog && !destroyedDialog->modal)
        mywin_free_dialog(hWnd);

    // The top-level desktop frame is the USER-visible shell object. When an
    // app closes itself via DefWindowProc(WM_CLOSE)->DestroyWindow(), keep the
    // WindowManager slot in sync instead of leaving a dead frame onscreen.
    if (g_lpWindowManager)
        wm_on_destroyed_hwnd(g_lpWindowManager, hWnd);

    /* v172: destroying a visible child/owned top-level exposes pixels below it.
       Win32 translates that into update-region damage for the owner/parent.
       This is deliberately USER32-side, not DialogLab-specific. */
    if (exposeOwner && IsWindow(exposeOwner))
        mywin_invalidate_window_and_children(exposeOwner, TRUE);

    wi = mywin_find_info(hWnd);
    if (wi) wi->ncDestroyed = 1;
    mywin_free_info(hWnd);
    return TRUE;
}

BOOL PostMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (!g_lpHwndManager || !g_HasCapability) return FALSE;
    if (!mywin_can_message_window(hWnd, Msg, wParam)) return FALSE;
    return hwnd_post(g_lpHwndManager, &g_CurrentCapability, hWnd, Msg, wParam, lParam) == 0;
}

LRESULT SendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    DWORD_PTR dwResult = 0;
    SendMessageTimeoutA(hWnd, Msg, wParam, lParam, SMTO_ABORTIFHUNG, 50, &dwResult);
    return (LRESULT)dwResult;
}

/* AUDIT(v118): Synchronous local path calls WNDPROC directly and fallback path
   uses the old HWND transport. Cross-thread SendMessage reentrancy/timeouts are
   not yet the real USER32 blocking/reply contract. */
BOOL SendMessageTimeoutA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam,
                         UINT fuFlags, UINT uTimeout, PDWORD_PTR lpdwResult)
{
    (void)fuFlags;
    if (lpdwResult) *lpdwResult = 0;
    if (!g_lpHwndManager || !g_HasCapability) return FALSE;
    if (!mywin_can_message_window(hWnd, Msg, wParam)) return FALSE;

    /* v98: USER32-created windows keep a real Win32 WNDPROC in MyWinWindowInfo.
       The lower HWND transport still has an old void HWNDWndProc carrier, so
       using it for SendMessageA lost every LRESULT.  Dialog code depends on
       synchronous return values for DM_GETDEFID, WM_GETDLGCODE, BM_GETCHECK,
       LB_GETCURSEL, scrollbar probes, etc.  Call the USER32 WNDPROC directly
       when we have local metadata; fall back to the transport path for legacy
       hwnd_create()/cross-pump windows. */
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (wi && wi->wndproc && mywin_owns_window(hWnd)) {
        LRESULT r = wi->wndproc(hWnd, Msg, wParam, lParam);
        if (lpdwResult) *lpdwResult = (DWORD_PTR)r;
        return TRUE;
    }

    /* v130: cross-thread/cross-process USER32 HWNDs no longer bypass the
       thread queue by directly invoking the foreign WndProc.  The lower HWND
       transport still provides the blocking/sync request slot used by OOP
       queues; real LRESULT propagation for foreign USER32 metadata remains a
       documented next step. */
    return hwnd_send_timeout(g_lpHwndManager, &g_CurrentCapability, hWnd, Msg, wParam, lParam, (int)uTimeout) == 0;
}

static BOOL convert_mymsg_to_msg(const MyMessage* in, MSG* out)
{
    if (!in || !out) return FALSE;
    memset(out, 0, sizeof(*out));
    out->hwnd = in->hwnd;
    out->message = in->msg;
    out->wParam = in->wparam;
    out->lParam = in->lparam;
    out->time = (DWORD)(in->timestamp_ns / 1000000ull);
    mywin_store_msg_sidecar(out, in);
    return TRUE;
}

static BOOL queue_get_for_current_process(MSG* lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, int wait, int timeout_ms, int remove)
{
    if (!g_lpHwndManager || !g_HasCapability || !lpMsg) return FALSE;

    // PoC-Regel: pro Capability/App genau ein UI-Thread.
    // Dadurch entspricht hWnd == NULL jetzt wirklich der aktuellen ThreadQueue,
    // nicht mehr "erste Fensterqueue scannen".
    MyMessage mm;
    int got = wait
        ? hwnd_get_thread_message_wait(g_lpHwndManager,
                                       g_CurrentCapability.id,
                                       g_CurrentCapability.id,
                                       hWnd,
                                       wMsgFilterMin,
                                       wMsgFilterMax,
                                       remove,
                                       timeout_ms,
                                       &mm)
        : hwnd_get_thread_message(g_lpHwndManager,
                                  g_CurrentCapability.id,
                                  g_CurrentCapability.id,
                                  hWnd,
                                  wMsgFilterMin,
                                  wMsgFilterMax,
                                  remove,
                                  &mm);
    if (!got) return FALSE;
    return convert_mymsg_to_msg(&mm, lpMsg);
}

BOOL PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
{
    int remove = (wRemoveMsg & PM_REMOVE) ? 1 : 0;
    return queue_get_for_current_process(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, 0, 0, remove);
}

BOOL GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    if (!queue_get_for_current_process(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, 1, -1, 1))
        return FALSE;
    // Win32 message-loop convention: GetMessage returns 0 after WM_QUIT.
    return (lpMsg && lpMsg->message == WM_QUIT) ? FALSE : TRUE;
}

UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc)
{
    if (!g_lpHwndManager || !g_HasCapability) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    UINT_PTR id = hwnd_set_user_timer(g_lpHwndManager,
                                      g_CurrentCapability.id,
                                      g_CurrentCapability.id,
                                      hWnd,
                                      nIDEvent,
                                      uElapse ? uElapse : 1u,
                                      (uintptr_t)lpTimerFunc);
    if (!id) SetLastError(hWnd ? ERROR_INVALID_HANDLE : ERROR_NOT_ENOUGH_MEMORY);
    else SetLastError(ERROR_SUCCESS);
    return id;
}

BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent)
{
    if (!g_lpHwndManager || !g_HasCapability) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    int ok = hwnd_kill_user_timer(g_lpHwndManager,
                                  g_CurrentCapability.id,
                                  g_CurrentCapability.id,
                                  hWnd,
                                  uIDEvent);
    if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
    else SetLastError(ERROR_SUCCESS);
    return ok ? TRUE : FALSE;
}


static SHORT g_MyWinKeyState[512];

void MyWinSetKeyDown(int vKey, BOOL bDown)
{
    if (vKey < 0 || vKey >= (int)(sizeof(g_MyWinKeyState)/sizeof(g_MyWinKeyState[0]))) return;
    if (bDown) g_MyWinKeyState[vKey] |= (SHORT)0x8000;
    else       g_MyWinKeyState[vKey] &= (SHORT)~0x8000;
}

SHORT GetKeyState(int nVirtKey)
{
    if (nVirtKey < 0 || nVirtKey >= (int)(sizeof(g_MyWinKeyState)/sizeof(g_MyWinKeyState[0]))) return 0;
    return g_MyWinKeyState[nVirtKey];
}

SHORT GetAsyncKeyState(int vKey)
{
    return GetKeyState(vKey);
}

static char mywin_keycode_to_char(int key, int shift)
{
    static const char normal[128] = {
        [KEY_A]='a',[KEY_B]='b',[KEY_C]='c',[KEY_D]='d',[KEY_E]='e',[KEY_F]='f',[KEY_G]='g',[KEY_H]='h',[KEY_I]='i',[KEY_J]='j',
        [KEY_K]='k',[KEY_L]='l',[KEY_M]='m',[KEY_N]='n',[KEY_O]='o',[KEY_P]='p',[KEY_Q]='q',[KEY_R]='r',[KEY_S]='s',[KEY_T]='t',
        [KEY_U]='u',[KEY_V]='v',[KEY_W]='w',[KEY_X]='x',[KEY_Y]='y',[KEY_Z]='z',
        [KEY_1]='1',[KEY_2]='2',[KEY_3]='3',[KEY_4]='4',[KEY_5]='5',[KEY_6]='6',[KEY_7]='7',[KEY_8]='8',[KEY_9]='9',[KEY_0]='0',
        [KEY_SPACE]=' ',[KEY_DOT]='.',[KEY_COMMA]=',',[KEY_MINUS]='-',[KEY_EQUAL]='=',[KEY_SEMICOLON]=';',[KEY_SLASH]='/',
        [KEY_APOSTROPHE]='\'',[KEY_BACKSLASH]='\\',[KEY_GRAVE]='`',[KEY_LEFTBRACE]='[',[KEY_RIGHTBRACE]=']'
    };
    static const char shifted[128] = {
        [KEY_A]='A',[KEY_B]='B',[KEY_C]='C',[KEY_D]='D',[KEY_E]='E',[KEY_F]='F',[KEY_G]='G',[KEY_H]='H',[KEY_I]='I',[KEY_J]='J',
        [KEY_K]='K',[KEY_L]='L',[KEY_M]='M',[KEY_N]='N',[KEY_O]='O',[KEY_P]='P',[KEY_Q]='Q',[KEY_R]='R',[KEY_S]='S',[KEY_T]='T',
        [KEY_U]='U',[KEY_V]='V',[KEY_W]='W',[KEY_X]='X',[KEY_Y]='Y',[KEY_Z]='Z',
        [KEY_1]='!',[KEY_2]='"',[KEY_3]='#',[KEY_4]='$',[KEY_5]='%',[KEY_6]='&',[KEY_7]='/',[KEY_8]='(',[KEY_9]=')',[KEY_0]='=',
        [KEY_SPACE]=' ',[KEY_DOT]=':',[KEY_COMMA]=';',[KEY_MINUS]='_',[KEY_EQUAL]='+',[KEY_SEMICOLON]=':',[KEY_SLASH]='?',
        [KEY_GRAVE]='~',[KEY_LEFTBRACE]='{',[KEY_RIGHTBRACE]='}'
    };
    if (key < 0 || key >= 128) return 0;
    return shift ? shifted[key] : normal[key];
}

BOOL TranslateMessage(const MSG* lpMsg)
{
    if (!lpMsg) return FALSE;
    if (lpMsg->message != WM_KEYDOWN && lpMsg->message != WM_SYSKEYDOWN) return FALSE;
    int shift = (lpMsg->lParam & MYOS_KEYSTATE_SHIFT) ? 1 : 0;
    char ch = mywin_keycode_to_char((int)lpMsg->wParam, shift);
    if (!ch) return FALSE;
    PostMessageA(lpMsg->hwnd,
                 lpMsg->message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR,
                 (WPARAM)(unsigned char)ch,
                 lpMsg->lParam);
    return TRUE;
}

LRESULT DispatchMessageA(const MSG* lpMsg)
{
    if (!lpMsg || !g_lpHwndManager) return 0;

    /* v149: DispatchMessage handles TIMERPROC callbacks for synthetic WM_TIMER.
       The queue stores the callback in lParam, matching the documented USER32
       SetTimer callback path, instead of routing it through the window WndProc. */
    if (lpMsg->message == WM_TIMER && lpMsg->lParam) {
        TIMERPROC proc = (TIMERPROC)(uintptr_t)lpMsg->lParam;
        proc(lpMsg->hwnd, WM_TIMER, (UINT_PTR)lpMsg->wParam, lpMsg->time);
        return 0;
    }

    /* v123: Public MSG contains only MSDN fields.  Prefer the queued sidecar
       when this MSG came from GetMessage/PeekMessage so sync SendMessageTimeout
       metadata still completes.  If a caller manually constructs MSG, synthesize
       the private transport fields from the documented public fields. */
    MyMessage mm;
    if (mywin_take_msg_sidecar(lpMsg, &mm)) {
        hwnd_dispatch_message(g_lpHwndManager, &mm);
        return 0;
    }

    mywin_make_mymsg_from_public(lpMsg, &mm);
    _UserHwndRef dispatchRef;
    DWORD manualAction = _HwndActionForMessageSidecar(&mm);
    if (_HwndResolveForAction(lpMsg->hwnd, manualAction, &dispatchRef, ERROR_INVALID_HANDLE) &&
        dispatchRef.wi && dispatchRef.wi->wndproc) {
        if (!mywin_owns_window(lpMsg->hwnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
        return dispatchRef.wi->wndproc(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
    }

    if (mywin_is_foreign_window(lpMsg->hwnd) && !mywin_can_message_window(lpMsg->hwnd, lpMsg->message, lpMsg->wParam))
        return 0;
    hwnd_dispatch_message(g_lpHwndManager, &mm);
    return 0;
}

static BOOL mywin_mdi_is_client(HWND hWnd)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    return (wi && wi->valid && strcmp(wi->className, "MDICLIENT") == 0) ? TRUE : FALSE;
}

static BOOL mywin_mdi_is_child(HWND hWnd)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    return (wi && wi->valid && wi->mdi->mdiIsChild && wi->hParent && mywin_mdi_is_client(wi->hParent)) ? TRUE : FALSE;
}

static HWND mywin_mdi_first_child(HWND hClient)
{
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (wi->valid && wi->hParent == hClient && wi->mdi->mdiIsChild)
            return wi->hWnd;
    }
    return 0;
}

static HWND mywin_mdi_next_child(HWND hClient, HWND hChild, BOOL bPrevious)
{
    HWND items[MYWIN_MAX_WINDOW_INFOS];
    int n = 0, cur = -1;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS && n < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (!wi->valid || wi->hParent != hClient || !wi->mdi->mdiIsChild) continue;
        if (wi->hWnd == hChild) cur = n;
        items[n++] = wi->hWnd;
    }
    if (n <= 0) return 0;
    if (cur < 0) return items[0];
    if (n == 1) return items[cur];
    int idx = bPrevious ? (cur - 1 + n) % n : (cur + 1) % n;
    return items[idx];
}

static int mywin_mdi_collect_children(HWND hClient, HWND* out, int maxOut)
{
    int n = 0;
    if (!out || maxOut <= 0) return 0;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS && n < maxOut; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (!wi->valid || wi->hParent != hClient || !wi->mdi->mdiIsChild) continue;
        out[n++] = wi->hWnd;
    }
    return n;
}

/* v144: first-manual-launch drag fallback.  The compositor raw route normally
   catches MDI child captions before generic client dispatch.  Manual testing
   exposed a first MDILab-instance path where the raw pre-route could miss and
   the click arrived at the MDICLIENT as an ordinary WM_LBUTTONDOWN.  Treat the
   MDI child's visible non-client caption as a first-class sub-hit-region here
   too, so both routes share the same spatial contract: MDICLIENT client point
   -> topmost MDI child window rect -> caption band -> HTCAPTION. */
static HWND mywin_mdi_child_from_client_point_visual(HWND hClient, POINT pt, int* lpHitTest)
{
    if (lpHitTest) *lpHitTest = HTNOWHERE;
    if (!hClient || !mywin_mdi_is_client(hClient)) return 0;

    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int n = mywin_mdi_collect_children(hClient, children, MYWIN_MAX_WINDOW_INFOS);
    mywin_sort_hwnds_bottom_to_top(children, n);

    for (int i = n - 1; i >= 0; --i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (!wi || !wi->valid || !(wi->style & WS_VISIBLE) || !wi->mdi->mdiIsChild) continue;
        if (pt.x < wi->rcClient.left || pt.x >= wi->rcClient.right ||
            pt.y < wi->rcClient.top  || pt.y >= wi->rcClient.bottom) continue;
        int localY = pt.y - (int)wi->rcClient.top;
        if (lpHitTest) *lpHitTest = (localY >= 0 && localY < MYWIN_DIALOG_CAPTION_H) ? HTCAPTION : HTCLIENT;
        return wi->hWnd;
    }
    return 0;
}

static HWND mywin_mdi_child_from_menu_id(HWND hClient, UINT id)
{
    MyWinWindowInfo* ci = mywin_find_info(hClient);
    if (!ci || !ci->mdi->mdiFirstChildId || id < ci->mdi->mdiFirstChildId) return 0;
    UINT idx = id - ci->mdi->mdiFirstChildId;
    if (idx >= 0x1000u) return 0;
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int n = mywin_mdi_collect_children(hClient, children, MYWIN_MAX_WINDOW_INFOS);
    return (idx < (UINT)n) ? children[idx] : 0;
}

static BOOL mywin_mdi_is_window_command(HWND hClient, UINT id)
{
    return mywin_mdi_child_from_menu_id(hClient, id) ? TRUE : FALSE;
}

static BOOL mywin_mdi_activate_child(HWND hClient, HWND hChild);

static void mywin_mdi_refresh_window_menu(HWND hClient)
{
    MyWinWindowInfo* ci = mywin_find_info(hClient);
    if (!ci || !ci->mdi->mdiWindowMenu || !ci->mdi->mdiFirstChildId) return;

    for (;;) {
        int removed = 0;
        int count = GetMenuItemCount(ci->mdi->mdiWindowMenu);
        for (int i = 0; i < count; ++i) {
            UINT id = GetMenuItemID(ci->mdi->mdiWindowMenu, i);
            if (id >= ci->mdi->mdiFirstChildId && id < ci->mdi->mdiFirstChildId + 0x1000u) {
                RemoveMenu(ci->mdi->mdiWindowMenu, (UINT)i, MF_BYPOSITION);
                removed = 1;
                break;
            }
        }
        if (!removed) break;
    }

    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int n = mywin_mdi_collect_children(hClient, children, MYWIN_MAX_WINDOW_INFOS);
    for (int i = 0; i < n; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        char text[96];
        int prefix = snprintf(text, sizeof(text), "&%d ", i + 1);
        if (prefix < 0) {
            text[0] = '\0';
        } else if ((size_t)prefix >= sizeof(text)) {
            text[sizeof(text) - 1] = '\0';
        } else {
            mywin_append_cstr_trunc(text, sizeof(text), (wi && wi->text[0]) ? wi->text : "MDI Child");
        }
        UINT id = ci->mdi->mdiFirstChildId + (UINT)i;
        AppendMenuA(ci->mdi->mdiWindowMenu, MF_STRING, id, text);
    }
}

static BOOL mywin_mdi_layout_tile(HWND hClient, WPARAM flags)
{
    (void)flags;
    MyWinWindowInfo* ci = mywin_find_info(hClient);
    if (!ci || !mywin_owns_window(hClient)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int n = mywin_mdi_collect_children(hClient, children, MYWIN_MAX_WINDOW_INFOS);
    if (n <= 0) return TRUE;

    int clientW = (int)(ci->rcClient.right - ci->rcClient.left);
    int clientH = (int)(ci->rcClient.bottom - ci->rcClient.top);
    if (clientW < 1) clientW = 1;
    if (clientH < 1) clientH = 1;

    int cols = 1;
    while (cols * cols < n) cols++;
    int rows = (n + cols - 1) / cols;
    if ((flags & MDITILE_HORIZONTAL) && n > 1) { rows = 1; cols = n; }
    int cellW = clientW / cols;
    int cellH = clientH / rows;
    if (cellW < 1) cellW = 1;
    if (cellH < 1) cellH = 1;

    for (int i = 0; i < n; ++i) {
        int r = i / cols;
        int c = i % cols;
        int x = c * cellW;
        int y = r * cellH;
        int w = (c == cols - 1) ? (clientW - x) : cellW;
        int h = (r == rows - 1) ? (clientH - y) : cellH;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        MoveWindow(children[i], x, y, w, h, TRUE);
    }
    return TRUE;
}

static BOOL mywin_mdi_layout_cascade(HWND hClient, WPARAM flags)
{
    (void)flags;
    MyWinWindowInfo* ci = mywin_find_info(hClient);
    if (!ci || !mywin_owns_window(hClient)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int n = mywin_mdi_collect_children(hClient, children, MYWIN_MAX_WINDOW_INFOS);
    if (n <= 0) return TRUE;

    int clientW = (int)(ci->rcClient.right - ci->rcClient.left);
    int clientH = (int)(ci->rcClient.bottom - ci->rcClient.top);
    if (clientW < 120) clientW = 120;
    if (clientH < 90) clientH = 90;

    int childW = (clientW * 2) / 3;
    int childH = (clientH * 2) / 3;
    if (childW < 160) childW = (clientW < 160) ? clientW : 160;
    if (childH < 100) childH = (clientH < 100) ? clientH : 100;
    if (childW > clientW) childW = clientW;
    if (childH > clientH) childH = clientH;

    int stepX = 24;
    int stepY = 24;
    if (n > 1) {
        int maxStepX = (clientW - childW) / (n - 1);
        int maxStepY = (clientH - childH) / (n - 1);
        if (maxStepX < stepX) stepX = maxStepX;
        if (maxStepY < stepY) stepY = maxStepY;
        if (stepX < 0) stepX = 0;
        if (stepY < 0) stepY = 0;
    }

    for (int i = 0; i < n; ++i) {
        int x = i * stepX;
        int y = i * stepY;
        if (x > clientW - childW) x = clientW - childW;
        if (y > clientH - childH) y = clientH - childH;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        MoveWindow(children[i], x, y, childW, childH, TRUE);
    }
    if (ci->mdi->mdiActiveChild && IsWindow(ci->mdi->mdiActiveChild)) {
        /* v143: Cascade changes the overlapping visual stack immediately after
           create/startup.  Re-run the MDI activation contract instead of only
           SetFocus(), so the active child is also normalized in the USER32
           child Z-order before the first physical caption hit/drag. */
        mywin_mdi_activate_child(hClient, ci->mdi->mdiActiveChild);
    }
    return TRUE;
}

static BOOL mywin_mdi_activate_child(HWND hClient, HWND hChild)
{
    MyWinWindowInfo* ci = mywin_find_info(hClient);
    if (!ci || !mywin_mdi_is_client(hClient)) return FALSE;
    if (hChild && (!mywin_mdi_is_child(hChild) || GetParent(hChild) != hClient || !mywin_same_window_owner(hClient, hChild))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    HWND old = ci->mdi->mdiActiveChild;
    if (hChild && IsWindow(hChild))
        mywin_raise_window_to_top(hChild);
    if (old == hChild) {
        if (hChild) SetFocus(hChild);
        mywin_mdi_refresh_window_menu(hClient);
        return TRUE;
    }

    ci->mdi->mdiActiveChild = hChild;
    if (old && IsWindow(old))
        SendMessageA(old, WM_MDIACTIVATE, (WPARAM)hChild, (LPARAM)old);
    if (hChild && IsWindow(hChild)) {
        SendMessageA(hChild, WM_CHILDACTIVATE, 0, 0);
        SendMessageA(hChild, WM_MDIACTIVATE, (WPARAM)old, (LPARAM)hChild);
        SetFocus(hChild);
    }
    return TRUE;
}

static HWND mywin_mdi_create_child(HWND hClient, const MDICREATESTRUCTA* mcs)
{
    if (!mcs || !mcs->szClass) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    MyWinWindowInfo* ci = mywin_find_info(hClient);
    if (!ci || !mywin_mdi_is_client(hClient)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (!mywin_owns_window(hClient)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }

    int x = (mcs->x == CW_USEDEFAULT) ? 8 + ci->mdi->mdiChildSeq * 18 : mcs->x;
    int y = (mcs->y == CW_USEDEFAULT) ? 8 + ci->mdi->mdiChildSeq * 18 : mcs->y;
    int cx = (mcs->cx == CW_USEDEFAULT || mcs->cx <= 0) ? 240 : mcs->cx;
    int cy = (mcs->cy == CW_USEDEFAULT || mcs->cy <= 0) ? 160 : mcs->cy;
    DWORD style = mcs->style | WS_CHILD | WS_VISIBLE;
    UINT id = ci->mdi->mdiFirstChildId ? (ci->mdi->mdiFirstChildId + (UINT)ci->mdi->mdiChildSeq) : 0;
    ci->mdi->mdiChildSeq++;

    HWND hChild = CreateWindowExA(0, mcs->szClass, mcs->szTitle ? mcs->szTitle : "",
                                  style, x, y, cx, cy, hClient, (HMENU)(UINT_PTR)id,
                                  (HINSTANCE)mcs->hOwner, (LPVOID)mcs->lParam);
    if (hChild) {
        MyWinWindowInfo* child = mywin_find_info(hChild);
        if (child) child->mdi->mdiIsChild = 1;
        mywin_mdi_activate_child(hClient, hChild);
        mywin_mdi_refresh_window_menu(hClient);
    }
    return hChild;
}

static BOOL mywin_mdi_destroy_child(HWND hClient, HWND hChild)
{
    if (!hChild || !mywin_mdi_is_child(hChild) || GetParent(hChild) != hClient) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    BOOL ok = DestroyWindow(hChild);
    if (ok) {
        MyWinWindowInfo* ci = mywin_find_info(hClient);
        HWND children[MYWIN_MAX_WINDOW_INFOS];
        int n = mywin_mdi_collect_children(hClient, children, MYWIN_MAX_WINDOW_INFOS);
        if (ci && n <= 0) {
            /* v139: when the last MDI child is closed the default CW_USEDEFAULT
               cascade cursor must reset.  Otherwise the next New appears at the
               old last-child slot even though the MDI desktop is empty. */
            ci->mdi->mdiChildSeq = 0;
            ci->mdi->mdiActiveChild = 0;
        }
        mywin_mdi_refresh_window_menu(hClient);
    }
    return ok;
}

static LRESULT CALLBACK MyMdiClientWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    MyWinWindowInfo* ci = mywin_find_info(hWnd);
    switch (Msg) {
    case WM_CREATE: {
        if (ci) {
            CREATESTRUCTA* cs = (CREATESTRUCTA*)lParam;
            CLIENTCREATESTRUCT* ccs = cs ? (CLIENTCREATESTRUCT*)cs->lpCreateParams : NULL;
            if (ccs) {
                ci->mdi->mdiWindowMenu = ccs->hWindowMenu;
                ci->mdi->mdiFirstChildId = ccs->idFirstChild;
            }
        }
        return 0;
    }
    case WM_MDICREATE:
        return (LRESULT)mywin_mdi_create_child(hWnd, (const MDICREATESTRUCTA*)lParam);
    case WM_MDIDESTROY:
        return mywin_mdi_destroy_child(hWnd, (HWND)wParam) ? 1 : 0;
    case WM_MDIACTIVATE:
        return mywin_mdi_activate_child(hWnd, (HWND)wParam) ? 0 : 0;
    case WM_MDINEXT: {
        HWND cur = (HWND)wParam;
        if (!cur && ci) cur = ci->mdi->mdiActiveChild;
        if (!cur) cur = mywin_mdi_first_child(hWnd);
        HWND next = mywin_mdi_next_child(hWnd, cur, (BOOL)lParam);
        if (next) mywin_mdi_activate_child(hWnd, next);
        return 0;
    }
    case WM_MDIGETACTIVE:
        if (lParam) *(BOOL*)lParam = FALSE;
        return ci ? (LRESULT)ci->mdi->mdiActiveChild : 0;
    case WM_MDITILE:
        return mywin_mdi_layout_tile(hWnd, wParam) ? 1 : 0;
    case WM_MDICASCADE:
        return mywin_mdi_layout_cascade(hWnd, wParam) ? 1 : 0;
    case WM_MDIICONARRANGE:
        return 1; /* v132: no iconic MDI surface yet, but message is safe/no-op. */
    case WM_MDISETMENU: {
        HMENU old = ci ? ci->mdi->mdiFrameMenu : 0;
        if (ci) {
            ci->mdi->mdiFrameMenu = (HMENU)wParam;
            ci->mdi->mdiWindowMenu = (HMENU)lParam;
            mywin_mdi_refresh_window_menu(hWnd);
        }
        return (LRESULT)old;
    }
    case WM_LBUTTONDOWN: {
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);

        int ht = HTNOWHERE;
        HWND visualChild = mywin_mdi_child_from_client_point_visual(hWnd, pt, &ht);
        if (visualChild && mywin_mdi_is_child(visualChild)) {
            mywin_mdi_activate_child(hWnd, visualChild);
            if (ht == HTCAPTION) {
                int ox = 0, oy = 0;
                if (mywin_client_origin_screen(hWnd, &ox, &oy)) {
                    LPARAM screenLp = MAKELPARAM((WORD)(ox + pt.x), (WORD)(oy + pt.y));
                    SendMessageA(visualChild, WM_NCLBUTTONDOWN, (WPARAM)HTCAPTION, screenLp);
                }
                return 0;
            }
        }

        HWND child = ChildWindowFromPoint(hWnd, pt);
        if (child && mywin_mdi_is_child(child))
            mywin_mdi_activate_child(hWnd, child);
        return 0;
    }
    case WM_COMMAND:
        if (ci) {
            UINT id = LOWORD(wParam);
            HWND menuChild = mywin_mdi_child_from_menu_id(hWnd, id);
            if (menuChild) {
                mywin_mdi_activate_child(hWnd, menuChild);
                return 1;
            }
        }
        /* fall through */
    case WM_SYSCOMMAND:
        if (ci && ci->mdi->mdiActiveChild && IsWindow(ci->mdi->mdiActiveChild)) {
            LRESULT r = SendMessageA(ci->mdi->mdiActiveChild, Msg, wParam, lParam);
            if (r) return r;
        }
        return 0;
    case WM_DESTROY:
        if (ci) { ci->mdi->mdiActiveChild = 0; ci->mdi->mdiFrameMenu = 0; ci->mdi->mdiWindowMenu = 0; }
        return 0;
    default:
        return DefWindowProcA(hWnd, Msg, wParam, lParam);
    }
}

static HWND g_MdiDragHwnd = 0;
static int  g_MdiDragOffX = 0;
static int  g_MdiDragOffY = 0;
static int  g_MdiDragOuterCaptionCoords = 0;

static int mywin_mdi_child_caption_hit_client(HWND hWnd, int y)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi || ((wi->style & WS_CAPTION) != WS_CAPTION)) return 0;
    if (y < 0 && y >= -MYWIN_DIALOG_CAPTION_H) return 1;
    /* v137 compatibility guard: every correct ScreenToClient() path for a
       captioned MDI child produces negative caption Y.  Some older raw/client
       dispatch paths can still deliver the same visual caption band as positive
       outer-window Y.  Treat it as the same caption contract instead of letting
       cascade-mode drag silently fail. */
    if (y >= 0 && y < MYWIN_DIALOG_CAPTION_H) return 1;
    return 0;
}

static void mywin_mdi_child_start_drag(HWND hWnd, int x, int y)
{
    g_MdiDragHwnd = hWnd;
    g_MdiDragOffX = x;
    if (y < 0) {
        g_MdiDragOuterCaptionCoords = 0;
        g_MdiDragOffY = y + MYWIN_DIALOG_CAPTION_H;
    } else {
        g_MdiDragOuterCaptionCoords = 1;
        g_MdiDragOffY = y;
    }
    SetCapture(hWnd);
}

static void mywin_mdi_child_clear_drag(HWND hWnd)
{
    if (g_MdiDragHwnd == hWnd) {
        g_MdiDragHwnd = 0;
        g_MdiDragOuterCaptionCoords = 0;
    }
}

static void mywin_mdi_child_move_from_client(HWND hWnd, int clientX, int clientY)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi || !wi->hParent || !mywin_mdi_is_client(wi->hParent)) return;

    MyWinWindowInfo* pi = mywin_find_info(wi->hParent);
    if (!pi) return;

    int childW = (int)(wi->rcClient.right - wi->rcClient.left);
    int childH = (int)(wi->rcClient.bottom - wi->rcClient.top);
    int parentW = (int)(pi->rcClient.right - pi->rcClient.left);
    int parentH = (int)(pi->rcClient.bottom - pi->rcClient.top);
    if (childW < 24 || childH < 24 || parentW < 1 || parentH < 1) return;

    int childOriginX = 0, childOriginY = 0;
    int parentOriginX = 0, parentOriginY = 0;
    if (!mywin_client_origin_screen(hWnd, &childOriginX, &childOriginY)) return;
    if (!mywin_client_origin_screen(wi->hParent, &parentOriginX, &parentOriginY)) return;

    /* Mouse messages for captioned child windows are delivered in client
       coordinates.  The caption band is negative Y, because ScreenToClient()
       subtracts the child client origin, not the outer frame top.  Rebuild the
       current screen mouse position, then convert it once into the MDICLIENT
       coordinate space.  This avoids the classic double-relative offset bug
       that made MDI hit/drag lag behind the drawn rectangles. */
    int screenX = childOriginX + clientX;
    int screenY = (g_MdiDragOuterCaptionCoords ? (childOriginY - MYWIN_DIALOG_CAPTION_H) : childOriginY) + clientY;
    int nx = screenX - parentOriginX - g_MdiDragOffX;
    int ny = screenY - parentOriginY - g_MdiDragOffY;

    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx + childW > parentW) nx = (parentW > childW) ? (parentW - childW) : 0;
    if (ny + childH > parentH) ny = (parentH > childH) ? (parentH - childH) : 0;

    MoveWindow(hWnd, nx, ny, childW, childH, TRUE);
}

LRESULT DefMDIChildProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    HWND parent = GetParent(hWnd);
    if ((Msg == WM_LBUTTONDOWN || Msg == WM_NCLBUTTONDOWN || Msg == WM_SETFOCUS) &&
        parent && mywin_mdi_is_client(parent)) {
        mywin_mdi_activate_child(parent, hWnd);
    }

    switch (Msg) {
    case WM_NCHITTEST: {
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hWnd, &pt);
        if (parent && mywin_mdi_is_client(parent) && mywin_mdi_child_caption_hit_client(hWnd, pt.y))
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDOWN: {
        if (parent && mywin_mdi_is_client(parent) && wParam == HTCAPTION) {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            ScreenToClient(hWnd, &pt);
            if (mywin_mdi_child_caption_hit_client(hWnd, pt.y)) {
                mywin_mdi_child_start_drag(hWnd, pt.x, pt.y);
                return 0;
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        /* v137: render rect and hit rect now share the same caption contract.
           Correct ScreenToClient() paths hit the caption as negative client Y;
           the positive fallback covers stale raw child paths without creating a
           second command/hit-test surface. */
        if (parent && mywin_mdi_is_client(parent) && mywin_mdi_child_caption_hit_client(hWnd, y)) {
            mywin_mdi_child_start_drag(hWnd, x, y);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (g_MdiDragHwnd == hWnd && GetCapture() == hWnd) {
            mywin_mdi_child_move_from_client(hWnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
    case WM_LBUTTONUP:
        if (g_MdiDragHwnd == hWnd) {
            mywin_mdi_child_clear_drag(hWnd);
            if (GetCapture() == hWnd) ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        mywin_mdi_child_clear_drag(hWnd);
        break;
    case WM_CLOSE:
        if (parent && mywin_mdi_is_client(parent)) {
            SendMessageA(parent, WM_MDIDESTROY, (WPARAM)hWnd, 0);
            return 0;
        }
        break;
    }
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

LRESULT DefFrameProcA(HWND hWnd, HWND hWndMDIClient, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (hWndMDIClient && mywin_mdi_is_client(hWndMDIClient)) {
        if (Msg == WM_MDITILE || Msg == WM_MDICASCADE || Msg == WM_MDIICONARRANGE ||
            Msg == WM_MDINEXT || Msg == WM_MDIACTIVATE || Msg == WM_MDIDESTROY ||
            Msg == WM_MDISETMENU || Msg == WM_MDIGETACTIVE)
            return SendMessageA(hWndMDIClient, Msg, wParam, lParam);

        if (Msg == WM_COMMAND) {
            UINT id = LOWORD(wParam);
            if (mywin_mdi_is_window_command(hWndMDIClient, id))
                return SendMessageA(hWndMDIClient, WM_COMMAND, wParam, lParam);
        }

        if (Msg == WM_COMMAND || Msg == WM_SYSCOMMAND) {
            MyWinWindowInfo* ci = mywin_find_info(hWndMDIClient);
            if (ci && ci->mdi->mdiActiveChild && IsWindow(ci->mdi->mdiActiveChild)) {
                LRESULT r = SendMessageA(ci->mdi->mdiActiveChild, Msg, wParam, lParam);
                if (r) return r;
            }
        }
    }
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

LRESULT DefWindowProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    // Win32 convention: returning TRUE from WM_NCCREATE lets creation continue.
    if (Msg == WM_NCCREATE) return TRUE;

    if (Msg == WM_GETTEXT || Msg == WM_GETTEXTLENGTH || Msg == WM_SETTEXT)
        return mywin_def_text_proc(hWnd, Msg, wParam, lParam);

    if (Msg == WM_ERASEBKGND) {
        HDC hdc = (HDC)wParam;
        HBRUSH hbr = (HBRUSH)GetClassLongPtrA(hWnd, GCLP_HBRBACKGROUND);
        if (!hdc || !hbr) return FALSE;
        RECT rc = {0, 0, 4096, 4096};
        MyWinWindowInfo* wi = mywin_find_info(hWnd);
        if (wi && wi->valid) {
            int w = (int)(wi->rcClient.right - wi->rcClient.left);
            int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
            if (w > 0) rc.right = w;
            if (h > 0) rc.bottom = h;
        }
        return FillRect(hdc, &rc, hbr) ? TRUE : FALSE;
    }

    // v78: frame/non-client default behavior lives in the WindowManager frame
    // bridge for top-level app HWNDs.  This keeps WM_NCHITTEST,
    // WM_NCLBUTTONDOWN and WM_SYSCOMMAND on a DefWindowProc-shaped path while
    // OOP child WndProcs can still call DefWindowProcA safely.
    if ((Msg == WM_NCHITTEST || Msg == WM_NCLBUTTONDOWN ||
         Msg == WM_NCLBUTTONUP || Msg == WM_NCRBUTTONDOWN ||
         Msg == WM_NCRBUTTONUP || Msg == WM_NCMOUSEMOVE ||
         Msg == WM_SYSCOMMAND) && g_lpWindowManager) {
        return wm_def_window_proc(g_lpWindowManager, hWnd, Msg, wParam, lParam);
    }

    if (Msg == WM_MOUSEWHEEL) {
        MyWinWindowInfo* wi = mywin_find_info(hWnd);
        if (wi && (wi->style & WS_VSCROLL)) {
            int lines = mywin_wheel_lines(wParam);
            if (lines) {
                int old = GetScrollPos(hWnd, SB_VERT);
                int page = wi->control->ccScrollPage > 0 ? wi->control->ccScrollPage : 1;
                (void)page;
                SetScrollPos(hWnd, SB_VERT, old + lines, TRUE);
                int pos = GetScrollPos(hWnd, SB_VERT);
                if (wi->hParent) mywin_post_control_scroll(hWnd, wi->hParent, WM_VSCROLL, (lines > 0) ? SB_LINEDOWN : SB_LINEUP, pos);
            }
            return 0;
        }
    }

    if (Msg == WM_CLOSE) {
        DestroyWindow(hWnd);
        return 0;
    }
    return 0;
}

BOOL ShowWindow(HWND hWnd, int nCmdShow)
{
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_SHOW, &ref, ERROR_INVALID_WINDOW_HANDLE))
        return FALSE;
    if (!mywin_can_control_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    MyWinWindowInfo* wi = ref.wi;
    if (wi && !mywin_can_mutate_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

    BOOL wasVisible = wi ? mywin_window_own_visible(wi) : FALSE;
    UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER;
    switch (nCmdShow) {
    case SW_HIDE:
    case SW_MINIMIZE:
    case SW_SHOWMINIMIZED:
    case SW_SHOWMINNOACTIVE:
        flags |= SWP_HIDEWINDOW | SWP_NOACTIVATE;
        break;
    case SW_SHOWNOACTIVATE:
    case SW_SHOWNA:
        flags |= SWP_SHOWWINDOW | SWP_NOACTIVATE;
        break;
    case SW_MAXIMIZE:
    case SW_RESTORE:
    case SW_SHOWNORMAL:
    case SW_SHOW:
    case SW_SHOWDEFAULT:
    default:
        flags |= SWP_SHOWWINDOW;
        break;
    }

    RECT r;
    if (!GetWindowRect(hWnd, &r) && wi) r = wi->rcClient;
    int x = (int)r.left, y = (int)r.top;
    int w = (int)(r.right - r.left), h = (int)(r.bottom - r.top);
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (!SetWindowPos(hWnd, HWND_TOP, x, y, w, h, flags)) return FALSE;
    return wasVisible;
}

BOOL IsWindow(HWND hWnd)
{
    return (g_lpHwndManager && hwnd_is_window(g_lpHwndManager, hWnd)) ? TRUE : FALSE;
}

BOOL IsWindowVisible(HWND hWnd)
{
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_QUERY, &ref, ERROR_INVALID_WINDOW_HANDLE))
        return FALSE;
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    MyWinWindowInfo* wi = ref.wi;
    if (wi) return mywin_is_visible_chain(hWnd);
    MyWindowState st;
    memset(&st, 0, sizeof(st));
    if (MyGetWindowState(hWnd, &st)) return st.visible ? TRUE : FALSE;
    return FALSE;
}


HWND SetFocus(HWND hWnd)
{
    if (hWnd) {
        _UserHwndRef ref;
        if (!_HwndResolveForAction(hWnd, _HWND_ACTION_FOCUS, &ref, ERROR_INVALID_HANDLE))
            return 0;
    }
    if (hWnd && !IsWindowEnabled(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (hWnd && !mywin_is_current_input_thread(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }

    HWND old = g_FocusHwnd;
    if (old == hWnd) return old;
    g_FocusHwnd = hWnd;

    HWND oldDlg = mywin_dialog_from_descendant(old);
    HWND newDlg = mywin_dialog_from_descendant(hWnd);
    if (oldDlg && oldDlg != newDlg) mywin_dialog_update_default_for_focus(oldDlg, 0);
    if (newDlg) mywin_dialog_update_default_for_focus(newDlg, hWnd);

    /* v125: focus transitions are sent synchronously.  Posting these made
       dialog navigation and button focus canaries order-dependent because
       GetFocus() changed before WM_KILLFOCUS/WM_SETFOCUS reached controls. */
    if (old && hwnd_is_window(g_lpHwndManager, old)) {
        SendMessageA(old, WM_KILLFOCUS, (WPARAM)hWnd, 0);
        mywin_publish_local_hwnd_state(old, WM_KILLFOCUS, MYWS_DIRTY_FOCUS);
    }
    if (hWnd && hwnd_is_window(g_lpHwndManager, hWnd)) {
        SendMessageA(hWnd, WM_SETFOCUS, (WPARAM)old, 0);
        mywin_publish_local_hwnd_state(hWnd, WM_SETFOCUS, MYWS_DIRTY_FOCUS);
    }
    return old;
}

HWND GetFocus(void)
{
    return g_FocusHwnd;
}

static BOOL mywin_is_dlg_tabstop(MyWinWindowInfo* wi)
{
    if (!wi || !wi->valid || !wi->hParent) return FALSE;
    if (!(wi->style & WS_VISIBLE)) return FALSE;
    if (wi->style & WS_DISABLED) return FALSE;
    if (strcmp(wi->className, "BUTTON") == 0) {
        if (!mywin_button_is_focusable(wi)) return FALSE;
        return (wi->style & WS_TABSTOP) ? TRUE : FALSE;
    }
    if (wi->style & WS_TABSTOP) return TRUE;
    if (strcmp(wi->className, "EDIT") == 0) return TRUE;
    if (strcmp(wi->className, "LISTBOX") == 0) return TRUE;
    if (strcmp(wi->className, "COMBOBOX") == 0) return TRUE;
    if (strcmp(wi->className, "SCROLLBAR") == 0) return TRUE;
    return FALSE;
}

HWND GetNextDlgTabItem(HWND hDlg, HWND hCtl, BOOL bPrevious)
{
    if (!hDlg) return 0;
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    HWND items[MYWIN_MAX_WINDOW_INFOS];
    int childCount = mywin_collect_children_linked_order(hDlg, children, MYWIN_MAX_WINDOW_INFOS);
    int n = 0, cur = -1;
    for (int i = 0; i < childCount; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (!wi || !wi->valid || wi->hParent != hDlg) continue;
        if (!mywin_is_dlg_tabstop(wi)) continue;
        if (mywin_is_radio_hwnd(wi->hWnd) && !mywin_radio_is_tab_candidate(hDlg, wi->hWnd)) continue;
        if (n < MYWIN_MAX_WINDOW_INFOS) {
            if (wi->hWnd == hCtl) cur = n;
            items[n++] = wi->hWnd;
        }
    }
    if (n <= 0) return 0;
    if (cur < 0) return bPrevious ? items[n-1] : items[0];
    int next = bPrevious ? (cur - 1 + n) % n : (cur + 1) % n;
    return items[next];
}

HWND GetNextDlgGroupItem(HWND hDlg, HWND hCtl, BOOL bPrevious)
{
    if (!hDlg) return 0;
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    HWND items[MYWIN_MAX_WINDOW_INFOS];
    int childCount = mywin_collect_children_linked_order(hDlg, children, MYWIN_MAX_WINDOW_INFOS);
    int n = 0, cur = -1;
    for (int i = 0; i < childCount && n < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (!wi || !wi->valid || wi->hParent != hDlg) continue;
        if (!(wi->style & WS_VISIBLE) || (wi->style & WS_DISABLED)) continue;
        if (wi->hWnd == hCtl) cur = n;
        items[n++] = wi->hWnd;
    }
    if (n <= 0) return 0;
    if (cur < 0) return bPrevious ? items[n-1] : items[0];

    int start = 0, end = n - 1;
    for (int i = cur; i >= 0; --i) {
        MyWinWindowInfo* wi = mywin_find_info(items[i]);
        if (wi && (wi->style & WS_GROUP)) { start = i; break; }
    }
    for (int i = cur + 1; i < n; ++i) {
        MyWinWindowInfo* wi = mywin_find_info(items[i]);
        if (wi && (wi->style & WS_GROUP)) { end = i - 1; break; }
    }
    if (end < start) { start = 0; end = n - 1; }
    if (bPrevious) return (cur <= start) ? items[end] : items[cur - 1];
    return (cur >= end) ? items[start] : items[cur + 1];
}

BOOL MapDialogRect(HWND hDlg, LPRECT lpRect)
{
    if (!lpRect) return FALSE;
    int bx = 0, by = 0;
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    if (di) { bx = di->dlgBaseX; by = di->dlgBaseY; }
    if (!bx || !by) mywin_dialog_default_base(&bx, &by);
    lpRect->left   = mywin_muldiv_int((int)lpRect->left,   bx, 4);
    lpRect->right  = mywin_muldiv_int((int)lpRect->right,  bx, 4);
    lpRect->top    = mywin_muldiv_int((int)lpRect->top,    by, 8);
    lpRect->bottom = mywin_muldiv_int((int)lpRect->bottom, by, 8);
    return TRUE;
}

static HWND mywin_dialog_current_focus(HWND hDlg)
{
    HWND f = GetFocus();
    if (f && (f == hDlg || IsChild(hDlg, f))) return f;
    return 0;
}

static BOOL mywin_dialog_activate_child(HWND hDlg, HWND hChild)
{
    if (!hDlg || !hChild) return FALSE;
    SetFocus(hChild);
    return TRUE;
}

static BOOL mywin_dialog_send_key_sync(HWND hWnd, UINT msg, WPARAM key, LPARAM lParam)
{
    if (!hWnd || !IsWindow(hWnd)) return FALSE;
    /* v97.2: Dialog keyboard navigation is synchronous like USER32's
       IsDialogMessage/DefDlgProc path.  The previous v97.1 fix posted
       arrow/space/enter messages back into the queue; in modal/modeless
       dialog tests those messages could be delayed behind the desktop pump,
       making LISTBOX/COMBOBOX/SCROLLBAR arrows appear dead. */
    SendMessageA(hWnd, msg, key, lParam);
    return TRUE;
}

static BOOL mywin_dialog_send_command_sync(HWND hDlg, int id, UINT code, HWND hCtl)
{
    if (!hDlg || !IsWindow(hDlg)) return FALSE;
    SendMessageA(hDlg, WM_COMMAND, MAKEWPARAM((WORD)id, (WORD)code), (LPARAM)hCtl);
    return TRUE;
}

static BOOL mywin_dialog_arrow_move_group(HWND hDlg, HWND hCtl, BOOL bPrevious)
{
    if (!hDlg) return FALSE;
    HWND start = (hCtl && hCtl != hDlg) ? hCtl : GetNextDlgTabItem(hDlg, 0, FALSE);
    if (!start) return FALSE;

    HWND next = GetNextDlgGroupItem(hDlg, start, bPrevious);
    int guard = 0;
    while (next && guard++ < MYWIN_MAX_WINDOW_INFOS) {
        MSG probe;
        memset(&probe, 0, sizeof(probe));
        probe.hwnd = next;
        probe.message = WM_GETDLGCODE;
        UINT code = mywin_get_dlg_code(next, &probe);

        if (!(code & DLGC_STATIC)) {
            MyWinWindowInfo* wi = mywin_find_info(next);
            if (wi && wi->valid && (wi->style & WS_VISIBLE) && !(wi->style & WS_DISABLED)) {
                SetFocus(next);
                if (strcmp(wi->className, "BUTTON") == 0 && mywin_button_type(wi) == BS_AUTORADIOBUTTON)
                    SendMessageA(next, BM_CLICK, 0, 0);
                return TRUE;
            }
        }

        if (next == start) break;
        next = GetNextDlgGroupItem(hDlg, next, bPrevious);
    }
    return FALSE;
}

static int mywin_dialog_key_to_vk(int key)
{
    switch (key) {
    case KEY_TAB: return VK_TAB;
    case KEY_ENTER: case KEY_KPENTER: return VK_RETURN;
    case KEY_ESC: return VK_ESCAPE;
    case KEY_SPACE: return VK_SPACE;
    case KEY_LEFT: return VK_LEFT;
    case KEY_RIGHT: return VK_RIGHT;
    case KEY_UP: return VK_UP;
    case KEY_DOWN: return VK_DOWN;
    case KEY_HOME: return VK_HOME;
    case KEY_END: return VK_END;
    case KEY_PAGEUP: return VK_PRIOR;
    case KEY_PAGEDOWN: return VK_NEXT;
    default: return key;
    }
}

static WPARAM mywin_dialog_vk_to_internal_key(int vk, WPARAM original)
{
    switch (vk) {
    case VK_TAB: return KEY_TAB;
    case VK_RETURN: return KEY_ENTER;
    case VK_ESCAPE: return KEY_ESC;
    case VK_SPACE: return KEY_SPACE;
    case VK_LEFT: return KEY_LEFT;
    case VK_RIGHT: return KEY_RIGHT;
    case VK_UP: return KEY_UP;
    case VK_DOWN: return KEY_DOWN;
    case VK_HOME: return KEY_HOME;
    case VK_END: return KEY_END;
    case VK_PRIOR: return KEY_PAGEUP;
    case VK_NEXT: return KEY_PAGEDOWN;
    default: return original;
    }
}

static BOOL mywin_dialog_control_wants_message(HWND hCtl, UINT code, UINT message)
{
    if (!hCtl) return FALSE;
    if (code & DLGC_WANTALLKEYS) return TRUE;
    if ((message == WM_CHAR || message == WM_SYSCHAR) && (code & DLGC_WANTCHARS)) return TRUE;
    return FALSE;
}

DWORD MyWinGetModelessDialogCount(void)
{
    return mywin_modeless_dialog_count();
}

BOOL MyIsModelessDialog(HWND hDlg)
{
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    return (di && di->valid && di->modeless && !di->ended && IsWindow(hDlg)) ? TRUE : FALSE;
}

BOOL MyTranslateModelessDialogMessageA(LPMSG lpMsg)
{
    HWND hDlg = mywin_modeless_dialog_from_message(lpMsg);
    if (!hDlg) { g_ModelessPumpMissCount++; return FALSE; }
    BOOL handled = IsDialogMessageA(hDlg, lpMsg);
    if (handled) g_ModelessPumpHitCount++;
    else g_ModelessPumpMissCount++;
    return handled;
}

BOOL MyGetModelessDialogAudit(DWORD* registered, DWORD* unregistered, DWORD* live, DWORD* pumpHits, DWORD* pumpMisses)
{
    if (registered) *registered = g_ModelessRegisterCount;
    if (unregistered) *unregistered = g_ModelessUnregisterCount;
    if (live) *live = mywin_modeless_dialog_count();
    if (pumpHits) *pumpHits = g_ModelessPumpHitCount;
    if (pumpMisses) *pumpMisses = g_ModelessPumpMissCount;
    return TRUE;
}

BOOL IsDialogMessageA(HWND hDlg, LPMSG lpMsg)
{
    if (!hDlg || !lpMsg) return FALSE;
    if (lpMsg->message != WM_KEYDOWN && lpMsg->message != WM_SYSKEYDOWN &&
        lpMsg->message != WM_CHAR && lpMsg->message != WM_SYSCHAR) return FALSE;

    HWND cur = mywin_dialog_current_focus(hDlg);
    MyWinWindowInfo* curWi = mywin_find_info(cur);
    UINT dlgCode = curWi ? mywin_get_dlg_code(cur, lpMsg) : 0;

    Capability cap;
    int haveCap = 0;
    if (g_HasCapability) { cap = g_CurrentCapability; haveCap = 1; }
    else if (g_lpHwndManager) { cap = cap_create(86, "dialog-manager", CAP_IPC|CAP_WINDOW_CONTROL|CAP_WINDOW_READ); cap_add_target(&cap, 0); haveCap = 1; }

    /* v192: Dialog manager now accepts both the current evdev KEY_* values
       and Win32 VK_* values.  This matters for foreign-style code that posts
       VK_RETURN/VK_TAB directly while the desktop input path still delivers
       KEY_ENTER/KEY_TAB.  Controls still receive the internal KEY_* messages
       they already implement. */
    int rawKey = (int)lpMsg->wParam;
    int vk = mywin_dialog_key_to_vk(rawKey);
    WPARAM internalKey = mywin_dialog_vk_to_internal_key(vk, lpMsg->wParam);
    int shift = (lpMsg->lParam & MYOS_KEYSTATE_SHIFT) ? 1 : 0;

    if (lpMsg->message == WM_SYSCHAR) {
        char ch = (char)lpMsg->wParam;
        if (ch && mywin_dialog_execute_mnemonic(hDlg, (char)tolower((unsigned char)ch)))
            return TRUE;
    }

    if (lpMsg->message == WM_SYSKEYDOWN) {
        char ch = mywin_keycode_to_char(rawKey, 0);
        if (!ch && rawKey >= 0x20 && rawKey < 0x7f) ch = (char)rawKey;
        if (ch && mywin_dialog_execute_mnemonic(hDlg, (char)tolower((unsigned char)ch)))
            return TRUE;
    }

    if (lpMsg->message == WM_CHAR || lpMsg->message == WM_SYSCHAR) {
        if (cur && cur != hDlg && haveCap && mywin_dialog_control_wants_message(cur, dlgCode, lpMsg->message)) {
            SendMessageA(cur, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
            return TRUE;
        }
        return FALSE;
    }

    if (vk == VK_TAB) {
        if (cur && cur != hDlg && ((dlgCode & DLGC_WANTTAB) || (dlgCode & DLGC_WANTALLKEYS)) && haveCap) {
            mywin_dialog_send_key_sync(cur, WM_KEYDOWN, internalKey, lpMsg->lParam);
            return TRUE;
        }
        HWND next = GetNextDlgTabItem(hDlg, (cur && cur != hDlg) ? cur : 0, shift ? TRUE : FALSE);
        if (next && mywin_is_radio_hwnd(next)) {
            HWND checked = mywin_radio_checked_in_group(hDlg, next);
            if (checked) next = checked;
        }
        if (next) { mywin_dialog_activate_child(hDlg, next); return TRUE; }
        return FALSE;
    }

    if ((vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
         vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT) &&
        cur && cur != hDlg && (dlgCode & DLGC_WANTARROWS) && haveCap) {
        mywin_dialog_send_key_sync(cur, WM_KEYDOWN, internalKey, lpMsg->lParam);
        return TRUE;
    }

    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
        BOOL prev = (vk == VK_LEFT || vk == VK_UP) ? TRUE : FALSE;
        if (mywin_dialog_arrow_move_group(hDlg, cur, prev)) return TRUE;
    }

    if (vk == VK_SPACE) {
        if (!cur || cur == hDlg) cur = GetNextDlgTabItem(hDlg, 0, FALSE);
        if (cur && cur != hDlg && haveCap) {
            mywin_dialog_send_key_sync(cur, WM_KEYDOWN, internalKey, lpMsg->lParam);
            mywin_dialog_send_key_sync(cur, WM_KEYUP, internalKey, lpMsg->lParam);
            return TRUE;
        }
        return FALSE;
    }

    if (vk == VK_RETURN) {
        if (cur && cur != hDlg && (dlgCode & DLGC_WANTALLKEYS) && haveCap) {
            mywin_dialog_send_key_sync(cur, WM_KEYDOWN, internalKey, lpMsg->lParam);
            return TRUE;
        }
        if (curWi && strcmp(curWi->className, "COMBOBOX") == 0 && curWi->control->ccDropped && haveCap) {
            mywin_dialog_send_key_sync(cur, WM_KEYDOWN, internalKey, lpMsg->lParam);
            return TRUE;
        }
        if (curWi && strcmp(curWi->className, "BUTTON") == 0 &&
            mywin_button_is_push_type(mywin_button_type(curWi)) && haveCap) {
            mywin_dialog_send_key_sync(cur, WM_KEYDOWN, internalKey, lpMsg->lParam);
            return TRUE;
        }
        HWND def = mywin_dialog_get_default_button(hDlg);
        MyWinWindowInfo* defWi = mywin_find_info(def);
        if (!def && !MyIsDialogWindow(hDlg)) return FALSE;
        if (defWi && haveCap) mywin_dialog_send_command_sync(hDlg, (int)defWi->id, BN_CLICKED, def);
        return def ? TRUE : FALSE;
    }

    if (vk == VK_ESCAPE) {
        if (curWi && strcmp(curWi->className, "COMBOBOX") == 0 && curWi->control->ccDropped && haveCap) {
            mywin_dialog_send_key_sync(cur, WM_KEYDOWN, internalKey, lpMsg->lParam);
            return TRUE;
        }
        HWND cancel = mywin_find_dialog_button(hDlg, IDCANCEL);
        if (!cancel && !MyIsDialogWindow(hDlg)) return FALSE;
        if (haveCap) mywin_dialog_send_command_sync(hDlg, IDCANCEL, BN_CLICKED, cancel);
        return TRUE;
    }
    return FALSE;
}

BOOL IsDialogMessageW(HWND hDlg, LPMSG lpMsg)
{
    return IsDialogMessageA(hDlg, lpMsg);
}



static const BYTE* mywin_align_dword_ptr(const BYTE* p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v + 3u) & ~(uintptr_t)3u;
    return (const BYTE*)v;
}

static WORD mywin_read_word(const BYTE* p)
{
    WORD v;
    memcpy(&v, p, sizeof(v));
    return v;
}



static const BYTE* mywin_parse_sz_or_ord(const BYTE* p, char* out, size_t outCap, WORD* lpAtom, BOOL* lpIsOrdinal)
{
    if (outCap) out[0] = 0;
    if (lpAtom) *lpAtom = 0;
    if (lpIsOrdinal) *lpIsOrdinal = FALSE;
    WORD first = mywin_read_word(p);
    p += sizeof(WORD);
    if (first == 0x0000u) return p;
    if (first == 0xFFFFu) {
        WORD atom = mywin_read_word(p);
        if (lpAtom) *lpAtom = atom;
        if (lpIsOrdinal) *lpIsOrdinal = TRUE;
        p += sizeof(WORD);
        return p;
    }

    size_t n = 0;
    WORD ch = first;
    while (ch != 0) {
        if (out && outCap && n + 1 < outCap)
            out[n++] = (ch < 0x80u) ? (char)ch : '?';
        ch = mywin_read_word(p);
        p += sizeof(WORD);
    }
    if (out && outCap) out[n] = 0;
    return p;
}

static const char* mywin_dialog_class_from_atom(WORD atom)
{
    switch (atom) {
    case MYOS_DLG_CLASS_BUTTON:    return "BUTTON";
    case MYOS_DLG_CLASS_EDIT:      return "EDIT";
    case MYOS_DLG_CLASS_STATIC:    return "STATIC";
    case MYOS_DLG_CLASS_LISTBOX:   return "LISTBOX";
    case MYOS_DLG_CLASS_SCROLLBAR: return "SCROLLBAR";
    case MYOS_DLG_CLASS_COMBOBOX:  return "COMBOBOX";
    default: return NULL;
    }
}

static BOOL mywin_dialog_template_is_extended(LPCDLGTEMPLATEA lpTemplate)
{
    if (!lpTemplate) return FALSE;
    const BYTE* p = (const BYTE*)lpTemplate;
    return mywin_read_word(p) == 1u && mywin_read_word(p + sizeof(WORD)) == 0xFFFFu;
}

BOOL RegisterDialogTemplateA(LPCSTR lpTemplateName, LPCDLGTEMPLATEA lpTemplate)
{
    if (!lpTemplateName || !lpTemplateName[0] || !lpTemplate) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (int i = 0; i < MYWIN_MAX_DIALOG_TEMPLATES; i++) {
        if (g_DialogTemplates[i].valid && strcmp(g_DialogTemplates[i].name, lpTemplateName) == 0) {
            g_DialogTemplates[i].lpTemplate = lpTemplate;
            return TRUE;
        }
    }

    for (int i = 0; i < MYWIN_MAX_DIALOG_TEMPLATES; i++) {
        if (!g_DialogTemplates[i].valid) {
            memset(&g_DialogTemplates[i], 0, sizeof(g_DialogTemplates[i]));
            g_DialogTemplates[i].valid = 1;
            snprintf(g_DialogTemplates[i].name, sizeof(g_DialogTemplates[i].name), "%s", lpTemplateName);
            g_DialogTemplates[i].lpTemplate = lpTemplate;
            return TRUE;
        }
    }

    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
}

LPCDLGTEMPLATEA FindDialogTemplateA(LPCSTR lpTemplateName)
{
    if (!lpTemplateName || !lpTemplateName[0]) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    for (int i = 0; i < MYWIN_MAX_DIALOG_TEMPLATES; i++) {
        if (g_DialogTemplates[i].valid && strcmp(g_DialogTemplates[i].name, lpTemplateName) == 0)
            return g_DialogTemplates[i].lpTemplate;
    }
    SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
    return NULL;
}


static BOOL mywin_is_int_resource_name(LPCSTR s)
{
    return s && IS_INTRESOURCE(s);
}

static LPCDLGTEMPLATEA mywin_find_dialog_template_by_instance_name(HINSTANCE hInstance, LPCSTR lpTemplateName)
{
    if (!lpTemplateName) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    if (!mywin_is_int_resource_name(lpTemplateName)) {
        LPCDLGTEMPLATEA legacy = FindDialogTemplateA(lpTemplateName);
        if (legacy) return legacy;
        /* Fall through to the module resource table.  This lets current labs keep
           using RegisterDialogTemplateA while real HINSTANCE+RT_DIALOG resources
           work through the same public DialogBoxParam/CreateDialogParam surface. */
    }

    HMODULE hModule = (HMODULE)hInstance;
    HRSRC hRes = FindResourceA(hModule, lpTemplateName, RT_DIALOG);
    if (!hRes) return NULL;
    HGLOBAL hData = LoadResource(hModule, hRes);
    if (!hData) return NULL;
    LPCDLGTEMPLATEA tpl = (LPCDLGTEMPLATEA)LockResource(hData);
    if (!tpl) return NULL;
    return tpl;
}

static void mywin_template_name_for_debug(LPCSTR lpTemplateName, char* out, size_t cb)
{
    if (!out || cb == 0) return;
    out[0] = 0;
    if (!lpTemplateName) { snprintf(out, cb, "<null>"); return; }
    if (mywin_is_int_resource_name(lpTemplateName)) snprintf(out, cb, "#%u", (unsigned)(WORD)(ULONG_PTR)lpTemplateName);
    else snprintf(out, cb, "%s", lpTemplateName);
}

typedef struct MyParsedDialogHeader {
    BOOL extended;
    DWORD helpID;
    DWORD style;
    DWORD exStyle;
    WORD itemCount;
    short x, y, cx, cy;
    WORD fontPointSize;
    WORD fontWeight;
    BYTE fontItalic;
    BYTE fontCharset;
    char fontFace[64];
    int baseX;
    int baseY;
    char title[96];
    char className[64];
    char menuName[64];
    WORD menuAtom;
    BOOL menuOrdinal;
    const BYTE* items;
} MyParsedDialogHeader;

static const BYTE* mywin_parse_dialog_header(LPCDLGTEMPLATEA lpTemplate, MyParsedDialogHeader* out)
{
    if (!lpTemplate || !out) return NULL;
    memset(out, 0, sizeof(*out));
    mywin_dialog_default_base(&out->baseX, &out->baseY);
    const BYTE* p = (const BYTE*)lpTemplate;

    out->extended = mywin_dialog_template_is_extended(lpTemplate);
    if (out->extended) {
        const DLGTEMPLATEEX* t = (const DLGTEMPLATEEX*)p;
        out->helpID = t->helpID;
        out->style = t->style;
        out->exStyle = t->exStyle;
        out->itemCount = t->cDlgItems;
        out->x = t->x; out->y = t->y; out->cx = t->cx; out->cy = t->cy;
        p += sizeof(DLGTEMPLATEEX);
    } else {
        const DLGTEMPLATE* t = (const DLGTEMPLATE*)p;
        out->style = t->style;
        out->exStyle = t->dwExtendedStyle;
        out->itemCount = t->cdit;
        out->x = t->x; out->y = t->y; out->cx = t->cx; out->cy = t->cy;
        p += sizeof(DLGTEMPLATE);
    }

    p = mywin_parse_sz_or_ord(p, out->menuName, sizeof(out->menuName), &out->menuAtom, &out->menuOrdinal); /* menu */

    WORD clsAtom = 0;
    BOOL clsOrdinal = FALSE;
    p = mywin_parse_sz_or_ord(p, out->className, sizeof(out->className), &clsAtom, &clsOrdinal);
    if (clsOrdinal) {
        const char* builtin = mywin_dialog_class_from_atom(clsAtom);
        if (builtin) snprintf(out->className, sizeof(out->className), "%s", builtin);
    }
    if (!out->className[0]) snprintf(out->className, sizeof(out->className), "#32770");

    p = mywin_parse_sz_or_ord(p, out->title, sizeof(out->title), NULL, NULL);

    if (out->style & DS_SETFONT) {
        out->fontPointSize = mywin_read_word(p);
        p += sizeof(WORD);
        if (out->extended) {
            out->fontWeight = mywin_read_word(p);
            p += sizeof(WORD);
            out->fontItalic = *p++;
            out->fontCharset = *p++;
        }
        p = mywin_parse_sz_or_ord(p, out->fontFace, sizeof(out->fontFace), NULL, NULL);
    }
    mywin_dialog_font_base(out->style, out->fontPointSize, out->fontFace, &out->baseX, &out->baseY);

    out->items = mywin_align_dword_ptr(p);
    return out->items;
}

static const BYTE* mywin_create_old_dialog_item(HWND hDlg, HINSTANCE hInstance, const MyParsedDialogHeader* hdr, const BYTE* p)
{
    p = mywin_align_dword_ptr(p);
    const DLGITEMTEMPLATE* it = (const DLGITEMTEMPLATE*)p;
    p += sizeof(DLGITEMTEMPLATE);

    char className[64];
    char title[96];
    WORD clsAtom = 0;
    BOOL clsOrdinal = FALSE;
    p = mywin_parse_sz_or_ord(p, className, sizeof(className), &clsAtom, &clsOrdinal);
    if (clsOrdinal) {
        const char* builtin = mywin_dialog_class_from_atom(clsAtom);
        if (builtin) snprintf(className, sizeof(className), "%s", builtin);
        else snprintf(className, sizeof(className), "#%u", (unsigned)clsAtom);
    }
    p = mywin_parse_sz_or_ord(p, title, sizeof(title), NULL, NULL);
    WORD extra = mywin_read_word(p);
    p += sizeof(WORD) + extra;

    DWORD style = it->style | WS_CHILD;
    if (className[0] && find_class_by_name(className)) {
        RECT prc = mywin_dialog_units_to_pixels_rect(hdr ? hdr->baseX : 6, hdr ? hdr->baseY : 13,
                                                       it->x, it->y, it->cx, it->cy);
        CreateWindowExA(it->dwExtendedStyle, className, title, style,
                        (int)prc.left, (int)prc.top, (int)(prc.right - prc.left), (int)(prc.bottom - prc.top),
                        hDlg, (HMENU)(UINT_PTR)it->id, hInstance, NULL);
    }
    return mywin_align_dword_ptr(p);
}

static const BYTE* mywin_create_extended_dialog_item(HWND hDlg, HINSTANCE hInstance, const MyParsedDialogHeader* hdr, const BYTE* p)
{
    p = mywin_align_dword_ptr(p);
    const DLGITEMTEMPLATEEX* it = (const DLGITEMTEMPLATEEX*)p;
    p += sizeof(DLGITEMTEMPLATEEX);

    char className[64];
    char title[96];
    WORD clsAtom = 0;
    BOOL clsOrdinal = FALSE;
    p = mywin_parse_sz_or_ord(p, className, sizeof(className), &clsAtom, &clsOrdinal);
    if (clsOrdinal) {
        const char* builtin = mywin_dialog_class_from_atom(clsAtom);
        if (builtin) snprintf(className, sizeof(className), "%s", builtin);
        else snprintf(className, sizeof(className), "#%u", (unsigned)clsAtom);
    }
    p = mywin_parse_sz_or_ord(p, title, sizeof(title), NULL, NULL);
    WORD extra = mywin_read_word(p);
    p += sizeof(WORD) + extra;

    DWORD style = it->style | WS_CHILD;
    if (className[0] && find_class_by_name(className)) {
        RECT prc = mywin_dialog_units_to_pixels_rect(hdr ? hdr->baseX : 6, hdr ? hdr->baseY : 13,
                                                       it->x, it->y, it->cx, it->cy);
        CreateWindowExA(it->exStyle, className, title, style,
                        (int)prc.left, (int)prc.top, (int)(prc.right - prc.left), (int)(prc.bottom - prc.top),
                        hDlg, (HMENU)(UINT_PTR)it->id, hInstance, NULL);
    }
    return mywin_align_dword_ptr(p);
}

static void mywin_create_dialog_items_from_template(HWND hDlg, HINSTANCE hInstance, const MyParsedDialogHeader* hdr)
{
    if (!hdr) return;
    const BYTE* p = hdr->items;
    for (WORD i = 0; i < hdr->itemCount; i++) {
        p = hdr->extended
            ? mywin_create_extended_dialog_item(hDlg, hInstance, hdr, p)
            : mywin_create_old_dialog_item(hDlg, hInstance, hdr, p);
    }
}

static HWND mywin_create_dialog_from_template_common(HINSTANCE hInstance, LPCDLGTEMPLATEA lpTemplate, LPCSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam, BOOL modal)
{
    if (!lpTemplate || !lpDialogFunc) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (!mywin_register_dialog_class()) { SetLastError(ERROR_INVALID_FUNCTION); return 0; }

    MyParsedDialogHeader hdr;
    if (!mywin_parse_dialog_header(lpTemplate, &hdr)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    RECT dlgPx = mywin_dialog_units_to_pixels_rect(hdr.baseX, hdr.baseY, hdr.x, hdr.y,
                                                    hdr.cx > 0 ? hdr.cx : 213,
                                                    hdr.cy > 0 ? hdr.cy : 63);
    int x = (int)dlgPx.left;
    int y = (int)dlgPx.top;
    int w = (int)(dlgPx.right - dlgPx.left);
    int h = (int)(dlgPx.bottom - dlgPx.top) + MYWIN_DIALOG_CAPTION_H;
    RECT pr;
    if (hWndParent && GetWindowRect(hWndParent, &pr)) {
        x = pr.left + 44 + (int)dlgPx.left;
        y = pr.top + 54 + (int)dlgPx.top;
    }

    DWORD dlgStyle = hdr.style | WS_VISIBLE;
    LPCSTR dlgClass = hdr.className[0] ? hdr.className : "#32770";
    LPCSTR dlgTitle = hdr.title[0] ? hdr.title : (lpTemplateName && lpTemplateName[0] ? lpTemplateName : "Dialog");

    HWND hDlg = CreateWindowExA(hdr.exStyle, dlgClass, dlgTitle, dlgStyle,
                                x, y, w, h, hWndParent, 0, hInstance, NULL);
    if (!hDlg) return 0;

    MyWinDialogInfo* di = mywin_alloc_dialog(hDlg);
    if (!di) { DestroyWindow(hDlg); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    di->hDlg = hDlg;
    di->hParent = hWndParent;
    di->dlgProc = lpDialogFunc;
    di->initParam = dwInitParam;
    di->modal = modal ? 1 : 0;
    di->modeless = modal ? 0 : 1;
    if (di->modeless) g_ModelessRegisterCount++;
    di->ended = 0;
    di->result = IDCANCEL;
    di->dlgBaseX = hdr.baseX;
    di->dlgBaseY = hdr.baseY;
    snprintf(di->templateName, sizeof(di->templateName), "%s", lpTemplateName ? lpTemplateName : "<indirect>");

    /* v153: DWLP slots are live before controls and WM_INITDIALOG so dialog
       procedures can store state in DWLP_USER and return LRESULT through
       DWLP_MSGRESULT. */
    mywin_dialog_set_msg_result(hDlg, 0);
    SetWindowLongPtrA(hDlg, DWLP_USER, 0);
    mywin_dialog_store_proc(hDlg, lpDialogFunc);

    mywin_create_dialog_items_from_template(hDlg, hInstance, &hdr);
    HWND def = mywin_dialog_get_default_button(hDlg);
    MyWinWindowInfo* defWi = mywin_find_info(def);
    if (defWi) di->defId = (int)defWi->id;
    mywin_dialog_apply_default_visual(hDlg, di->defId);

    HWND first = GetNextDlgTabItem(hDlg, 0, FALSE);
    INT_PTR initResult = lpDialogFunc(hDlg, WM_INITDIALOG, (WPARAM)first, dwInitParam);
    di->initSent = 1;
    mywin_dialog_apply_default_visual(hDlg, di->defId);
    if (initResult && first) SetFocus(first);
    return hDlg;
}

HWND CreateDialogIndirectParamA(HINSTANCE hInstance, LPCDLGTEMPLATEA lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    return mywin_create_dialog_from_template_common(hInstance, lpTemplate, "<indirect>", hWndParent, lpDialogFunc, dwInitParam, FALSE);
}

HWND CreateDialogParamA(HINSTANCE hInstance, LPCSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    LPCDLGTEMPLATEA tpl = mywin_find_dialog_template_by_instance_name(hInstance, lpTemplateName);
    if (!tpl) return 0;
    char dbgName[64];
    mywin_template_name_for_debug(lpTemplateName, dbgName, sizeof(dbgName));
    return mywin_create_dialog_from_template_common(hInstance, tpl, dbgName, hWndParent, lpDialogFunc, dwInitParam, FALSE);
}

BOOL EndDialog(HWND hDlg, INT_PTR nResult)
{
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    if (!di) { if (IsWindow(hDlg)) return DestroyWindow(hDlg); return FALSE; }
    int modal = di->modal;
    di->result = nResult;
    di->ended = 1;
    HWND parent = di->hParent;
    /* v193: modal focus restoration belongs to the modal owner/focus stack,
       not to EndDialog itself.  Setting focus to a disabled owner during a
       modal close fails and can leave the old dialog/control as the apparent
       focus until DestroyWindow clears it.  Modeless dialogs keep the old
       lightweight behavior and hand focus back to their parent immediately. */
    if (GetFocus() && (GetFocus() == hDlg || IsChild(hDlg, GetFocus()))) {
        if (modal) SetFocus(0);
        else SetFocus(parent);
    }
    DestroyWindow(hDlg);
    /* Modal DialogBoxParamA keeps the result/state alive until its private
       message loop returns.  Modeless CreateDialogParamA has no waiter, so
       free the dialog slot immediately after destruction to avoid exhausting
       the small dialog table during repeated open/close tests. */
    if (!modal) mywin_free_dialog(hDlg);
    return TRUE;
}

typedef struct MyWinModalPumpSpec {
    HWND hDlg;
    HWND hAccelWnd;
    HACCEL hAccel;
    int timeoutMs;
    int translateMessage;
    const char* reason;
} MyWinModalPumpSpec;

typedef struct _UserMsgFilterContext {
    const MyWinModalPumpSpec* spec;
    MSG* msg;
    MyMessage mm;
    _MsgFilterPipeline pipeline;
    uint32_t handled_stage;
    uint32_t final_state;
} _UserMsgFilterContext;

typedef struct _UserMsgFilterOp _UserMsgFilterOp;
typedef uint32_t (*_UserMsgFilterHandler)(_UserMsgFilterContext* ctx, const _UserMsgFilterOp* op);

struct _UserMsgFilterOp {
    uint32_t stage;
    uint32_t order;
    uint32_t can_handle;
    uint32_t required_action;
    const char* name;
    _UserMsgFilterHandler handler;
};

static uint32_t mywin_filter_hook_stage(_UserMsgFilterContext* ctx, const _UserMsgFilterOp* op)
{
    (void)ctx; (void)op;
    /* Low-level/global hooks have already annotated the route descriptor in the
       input side.  v228 keeps this as a table-driven stage so a real hook proc
       can return BLOCKED/HANDLED later without changing pump ordering. */
    return _MSG_FILTER_STATE_PASSTHROUGH;
}

static uint32_t mywin_filter_accelerator_stage(_UserMsgFilterContext* ctx, const _UserMsgFilterOp* op)
{
    (void)op;
    if (!ctx || !ctx->msg) return _MSG_FILTER_STATE_PASSTHROUGH;
    const MyWinModalPumpSpec* spec = ctx->spec;
    MSG* msg = ctx->msg;
    if (spec && spec->hAccel &&
        TranslateAcceleratorA(spec->hAccelWnd ? spec->hAccelWnd : msg->hwnd, spec->hAccel, msg))
        return _MSG_FILTER_STATE_HANDLED;
    return _MSG_FILTER_STATE_PASSTHROUGH;
}

static uint32_t mywin_filter_dialog_stage(_UserMsgFilterContext* ctx, const _UserMsgFilterOp* op)
{
    (void)op;
    if (!ctx || !ctx->msg) return _MSG_FILTER_STATE_PASSTHROUGH;
    const MyWinModalPumpSpec* spec = ctx->spec;
    if (spec && spec->hDlg && IsDialogMessageA(spec->hDlg, ctx->msg))
        return _MSG_FILTER_STATE_HANDLED;
    return _MSG_FILTER_STATE_PASSTHROUGH;
}

static uint32_t mywin_filter_modeless_stage(_UserMsgFilterContext* ctx, const _UserMsgFilterOp* op)
{
    (void)op;
    if (!ctx || !ctx->msg) return _MSG_FILTER_STATE_PASSTHROUGH;
    const MyWinModalPumpSpec* spec = ctx->spec;
    if ((!spec || !spec->hDlg) && MyTranslateModelessDialogMessageA(ctx->msg))
        return _MSG_FILTER_STATE_HANDLED;
    return _MSG_FILTER_STATE_PASSTHROUGH;
}

static uint32_t mywin_filter_translate_stage(_UserMsgFilterContext* ctx, const _UserMsgFilterOp* op)
{
    (void)op;
    if (!ctx || !ctx->msg) return _MSG_FILTER_STATE_PASSTHROUGH;
    const MyWinModalPumpSpec* spec = ctx->spec;
    if (spec && spec->translateMessage) TranslateMessage(ctx->msg);
    return _MSG_FILTER_STATE_PASSTHROUGH;
}

static uint32_t mywin_filter_menu_stage(_UserMsgFilterContext* ctx, const _UserMsgFilterOp* op)
{
    (void)ctx; (void)op;
    /* Menu modal tracking still consumes mouse/button stages in TrackPopupMenu
       code today.  The stage exists in the op table so menu hooks/modal menu
       filtering can be installed by replacing this handler, not by adding more
       WM_* if-chains to pumps. */
    return _MSG_FILTER_STATE_PASSTHROUGH;
}

static uint32_t mywin_filter_dispatch_stage(_UserMsgFilterContext* ctx, const _UserMsgFilterOp* op)
{
    (void)ctx; (void)op;
    return _MSG_FILTER_STATE_PASSTHROUGH;
}

static const _UserMsgFilterOp g_UserMsgFilterOps[] = {
    { _MSG_FILTER_HOOK,        0, 0, _HWND_ACTION_MESSAGE, "hook",        mywin_filter_hook_stage },
    { _MSG_FILTER_ACCELERATOR, 1, 1, _HWND_ACTION_MESSAGE, "accelerator", mywin_filter_accelerator_stage },
    { _MSG_FILTER_DIALOG,      2, 1, _HWND_ACTION_MESSAGE, "dialog",      mywin_filter_dialog_stage },
    { _MSG_FILTER_MODELLESS,   3, 1, _HWND_ACTION_MESSAGE, "modeless",    mywin_filter_modeless_stage },
    { _MSG_FILTER_TRANSLATE,   4, 0, _HWND_ACTION_MESSAGE, "translate",   mywin_filter_translate_stage },
    { _MSG_FILTER_MENU,        5, 1, _HWND_ACTION_MESSAGE, "menu",        mywin_filter_menu_stage },
    { _MSG_FILTER_DISPATCH,    6, 0, _HWND_ACTION_MESSAGE, "dispatch",    mywin_filter_dispatch_stage }
};

static const _UserMsgFilterOp* mywin_filter_op_for_stage(uint32_t stage)
{
    for (uint32_t i = 0; i < sizeof(g_UserMsgFilterOps)/sizeof(g_UserMsgFilterOps[0]); ++i) {
        if (g_UserMsgFilterOps[i].stage == stage) return &g_UserMsgFilterOps[i];
    }
    return NULL;
}

BOOL MyWinQueryMessageFilterStage(DWORD dwStage, DWORD* lpOrder, DWORD* lpCanHandle, DWORD* lpRequiredAction)
{
    const _UserMsgFilterOp* op = mywin_filter_op_for_stage((uint32_t)dwStage);
    if (!op) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (lpOrder) *lpOrder = op->order;
    if (lpCanHandle) *lpCanHandle = op->can_handle;
    if (lpRequiredAction) *lpRequiredAction = op->required_action;
    return TRUE;
}

static uint32_t mywin_run_pretranslate_stage(_UserMsgFilterContext* ctx, uint32_t stage)
{
    if (!ctx || !ctx->msg || !stage) return _MSG_FILTER_STATE_PASSTHROUGH;
    const _UserMsgFilterOp* op = mywin_filter_op_for_stage(stage);
    if (!op || !op->handler) return _MSG_FILTER_STATE_PASSTHROUGH;
    return op->handler(ctx, op);
}

/* v227: central USER32 pre-dispatch pipeline runner.  v226 made every message
   carry stage bits; v227 materializes those bits into ordered stage records and
   advances state through PASSTHROUGH/HANDLED/BLOCKED.  The behavior is the same
   Win32 pump shape, but the pump is now driven by a compact state vector rather
   than a hand-written if ladder per modal/menu/dialog loop. */
static int mywin_pretranslate_filter_stages(const MyWinModalPumpSpec* spec, MSG* msg)
{
    if (!msg) return 0;
    _UserMsgFilterContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.spec = spec;
    ctx.msg = msg;
    mywin_make_mymsg_from_public(msg, &ctx.mm);
    uint32_t stages = ctx.mm.filter_stages ? ctx.mm.filter_stages :
        mymsg_default_filter_stages(ctx.mm.msg, ctx.mm.lane, ctx.mm.input_kind, ctx.mm.route_reason);
    mymsg_build_filter_pipeline(stages, &ctx.pipeline);

    for (uint32_t i = 0; i < ctx.pipeline.count; ++i) {
        uint32_t stage = ctx.pipeline.steps[i].stage;
        uint32_t result = mywin_run_pretranslate_stage(&ctx, stage);
        ctx.pipeline.steps[i].state = result;
        mymsg_advance_filter_stage(&ctx.mm, stage, result);

        if (result == _MSG_FILTER_STATE_HANDLED || result == _MSG_FILTER_STATE_BLOCKED) {
            ctx.handled_stage = stage;
            ctx.final_state = result;
            return 1;
        }
    }
    ctx.final_state = _MSG_FILTER_STATE_PASSTHROUGH;
    return 0;
}

/* v191: one small USER32 modal-pump primitive for dialog/menu style loops.
   The important ordering is now central instead of hand-open-coded:
       wait -> WM_QUIT -> TranslateAccelerator -> IsDialogMessage -> dispatch.
   translateMessage is intentionally optional because myOS desktop input already
   synthesizes WM_CHAR/WM_SYSCHAR for several current app paths. */
static int mywin_modal_pump_once(const MyWinModalPumpSpec* spec, MSG* outMsg)
{
    MSG msg;
    memset(&msg, 0, sizeof(msg));
    int timeoutMs = spec ? spec->timeoutMs : -1;
    if (!queue_get_for_current_process(&msg, 0, 0, 0, 1, timeoutMs, 1))
        return 0;
    if (msg.message == WM_QUIT) {
        if (outMsg) *outMsg = msg;
        return -1;
    }
    if (mywin_pretranslate_filter_stages(spec, &msg))
        return 2;
    if (outMsg) *outMsg = msg;
    return 1;
}

static void mywin_modal_send_activate(HWND hWnd, BOOL active, HWND other)
{
    if (!hWnd || !IsWindow(hWnd)) return;
    SendMessageA(hWnd, WM_ACTIVATE, active ? WA_ACTIVE : WA_INACTIVE, (LPARAM)other);
    mywin_publish_local_hwnd_state(hWnd, WM_ACTIVATE, MYWS_DIRTY_FOCUS|MYWS_DIRTY_OWNER|MYWS_DIRTY_VISIBLE);
}

static HWND mywin_modal_initial_focus(HWND hDlg)
{
    HWND f = GetFocus();
    if (f && (f == hDlg || IsChild(hDlg, f)) && IsWindowEnabled(f)) return f;
    HWND first = GetNextDlgTabItem(hDlg, 0, FALSE);
    if (first && IsWindowEnabled(first)) return first;
    return hDlg;
}

static MyWinModalState* mywin_modal_begin(HWND hDlg, HWND hOwner)
{
    if (!hDlg || !IsWindow(hDlg)) return NULL;
    MyWinModalState* st = NULL;
    if (g_ModalDepth < MYWIN_MAX_MODAL_STACK) {
        st = &g_ModalStack[g_ModalDepth++];
    } else {
        /* Keep modal behavior working even if the diagnostic stack is full. */
        st = &g_ModalStack[MYWIN_MAX_MODAL_STACK - 1];
        memset(st, 0, sizeof(*st));
    }
    memset(st, 0, sizeof(*st));
    st->valid = 1;
    st->depth = g_ModalDepth;
    st->hDlg = hDlg;
    st->hOwner = (hOwner && IsWindow(hOwner)) ? hOwner : 0;
    st->previousFocus = GetFocus();
    st->previousCapture = GetCapture();
    st->previousForeground = GetForegroundWindow();
    st->ownerWasEnabled = st->hOwner ? IsWindowEnabled(st->hOwner) : FALSE;
    st->disabledOwner = FALSE;
    g_ModalPushCount++;

    if (st->hOwner) {
        mywin_modal_send_activate(st->hOwner, FALSE, hDlg);
        if (st->ownerWasEnabled) {
            EnableWindow(st->hOwner, FALSE);
            st->disabledOwner = TRUE;
        }
    }

    HWND focusTarget = mywin_modal_initial_focus(hDlg);
    if (focusTarget && IsWindow(focusTarget) && IsWindowEnabled(focusTarget))
        SetFocus(focusTarget);
    mywin_modal_send_activate(hDlg, TRUE, st->hOwner);
    return st;
}

static void mywin_modal_end(MyWinModalState* st)
{
    if (!st || !st->valid) return;
    HWND hDlg = st->hDlg;
    HWND hOwner = st->hOwner;

    mywin_modal_send_activate(hDlg, FALSE, hOwner);

    if (st->disabledOwner && hOwner && IsWindow(hOwner) && !IsWindowEnabled(hOwner))
        EnableWindow(hOwner, TRUE);

    if (st->previousCapture && IsWindow(st->previousCapture) &&
        IsWindowEnabled(st->previousCapture) && mywin_is_current_input_thread(st->previousCapture)) {
        if (SetCapture(st->previousCapture))
            g_ModalRestoreCaptureCount++;
        else if (GetCapture() == st->previousCapture)
            g_ModalRestoreCaptureCount++;
    }

    HWND focusTarget = 0;
    if (st->previousFocus && IsWindow(st->previousFocus) && IsWindowEnabled(st->previousFocus))
        focusTarget = st->previousFocus;
    else if (hOwner && IsWindow(hOwner) && IsWindowEnabled(hOwner))
        focusTarget = hOwner;

    if (focusTarget && SetFocus(focusTarget) != 0)
        g_ModalRestoreFocusCount++;
    else if (focusTarget && GetFocus() == focusTarget)
        g_ModalRestoreFocusCount++;

    if (st->previousForeground && IsWindow(st->previousForeground))
        SetForegroundWindow(st->previousForeground);

    mywin_modal_send_activate(focusTarget ? focusTarget : hOwner, TRUE, hDlg);

    st->valid = 0;
    g_ModalPopCount++;
    if (g_ModalDepth > 0 && st == &g_ModalStack[g_ModalDepth - 1])
        g_ModalDepth--;
}

static INT_PTR mywin_dialog_modal_loop(HWND hDlg, HWND hWndParent)
{
    MyWinDialogInfo* di = mywin_find_dialog(hDlg);
    if (!di) return -1;

    MyWinModalState* modalState = mywin_modal_begin(hDlg, hWndParent);

    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    if (g_lpHwndManager && pid && tid)
        hwnd_set_thread_external_pump(g_lpHwndManager, pid, tid, 1, "DialogBoxIndirectParamA modal loop");

    /* v102.2/v191: preserve the short timed wait so the in-process compositor
       can idle between messages, but route the message ordering through the
       shared modal-pump helper. */
    mywin_modal_idle();

    MSG msg;
    MyWinModalPumpSpec pump;
    memset(&pump, 0, sizeof(pump));
    pump.hDlg = hDlg;
    pump.timeoutMs = 16;
    pump.translateMessage = 0;
    pump.reason = "DialogBoxIndirectParamA";

    while (di->valid && !di->ended && IsWindow(hDlg)) {
        memset(&msg, 0, sizeof(msg));
        int pr = mywin_modal_pump_once(&pump, &msg);
        if (pr == 0 || pr == 2) {
            mywin_modal_idle();
            di = mywin_find_dialog(hDlg);
            if (!di) break;
            continue;
        }
        if (pr < 0) break;
        DispatchMessageA(&msg);
        mywin_modal_idle();
        di = mywin_find_dialog(hDlg);
        if (!di) break;
    }

    INT_PTR result = di ? di->result : IDCANCEL;

    if (g_lpHwndManager && pid && tid)
        hwnd_set_thread_external_pump(g_lpHwndManager, pid, tid, 0, "internal hwnd_dispatch");

    mywin_modal_end(modalState);
    mywin_free_dialog(hDlg);
    mywin_modal_idle();
    return result;
}

INT_PTR DialogBoxIndirectParamA(HINSTANCE hInstance, LPCDLGTEMPLATEA hDialogTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    HWND hDlg = mywin_create_dialog_from_template_common(hInstance, hDialogTemplate, "<indirect>", hWndParent, lpDialogFunc, dwInitParam, TRUE);
    if (!hDlg) return -1;
    return mywin_dialog_modal_loop(hDlg, hWndParent);
}

INT_PTR DialogBoxParamA(HINSTANCE hInstance, LPCSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    LPCDLGTEMPLATEA tpl = mywin_find_dialog_template_by_instance_name(hInstance, lpTemplateName);
    if (!tpl) return -1;
    char dbgName[64];
    mywin_template_name_for_debug(lpTemplateName, dbgName, sizeof(dbgName));
    HWND hDlg = mywin_create_dialog_from_template_common(hInstance, tpl, dbgName, hWndParent, lpDialogFunc, dwInitParam, TRUE);
    if (!hDlg) return -1;
    return mywin_dialog_modal_loop(hDlg, hWndParent);
}

INT_PTR MyDialogBoxParamA(LPCSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    return DialogBoxParamA(0, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
}

HWND GetForegroundWindow(void)
{
    return g_lpWindowManager ? wm_get_foreground_hwnd(g_lpWindowManager) : 0;
}

HWND GetDesktopWindow(void)
{
    return g_lpWindowManager ? wm_get_desktop_hwnd(g_lpWindowManager) : 0;
}

BOOL SetForegroundWindow(HWND hWnd)
{
    if (!g_lpWindowManager || !mywin_can_control_window(hWnd)) return FALSE;
    return wm_set_foreground_hwnd(g_lpWindowManager, hWnd) ? TRUE : FALSE;
}

BOOL GetWindowRect(HWND hWnd, LPRECT lpRect)
{
    if (!lpRect) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

    /* v122: USER32-created HWNDs can exist without a compositor/WindowManager
       slot.  Their geometry still belongs to USER32 and must be queryable by
       normal Win32 code.  Child HWNDs store parent-client coordinates, so this
       path converts them to screen coordinates for the public GetWindowRect
       contract. */
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    int wmBacked = (g_lpWindowManager && wm_find_hwnd(g_lpWindowManager, hWnd) >= 0) ? 1 : 0;
    if (!wmBacked && mywin_get_local_window_rect_screen(wi, lpRect))
        return TRUE;

    MyWindowState st;
    if (MyGetWindowState(hWnd, &st)) {
        *lpRect = st.rcWindow;
        return TRUE;
    }

    if (mywin_get_local_window_rect_screen(wi, lpRect))
        return TRUE;

    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

static BOOL mywin_get_client_size_for_ref(const _UserHwndRef* ref, int* outW, int* outH)
{
    if (!ref || !ref->hWnd || !outW || !outH) return FALSE;

    MyWinWindowInfo* wi = ref->wi ? ref->wi : mywin_find_info(ref->hWnd);
    int w = 0;
    int h = 0;

    if (wi) {
        w = (int)(wi->rcClient.right - wi->rcClient.left);
        h = (int)(wi->rcClient.bottom - wi->rcClient.top);

        if (!wi->hParent && g_lpWindowManager) {
            int idx = wm_find_hwnd(g_lpWindowManager, ref->hWnd);
            if (idx >= 0) {
                WindowRect wr;
                if (wm_get_window_rect(g_lpWindowManager, idx, &wr)) {
                    w = wr.w - 2;
                    h = wr.h - TITLEBAR_H - 1;
                }
            }
        } else if (strcmp(wi->className, "#32770") == 0) {
            h -= MYWIN_DIALOG_CAPTION_H;
        } else if (wi->hParent) {
            h -= mywin_nc_client_y_offset(wi);
        }
    } else {
        MyWindowState st;
        if (!MyGetWindowState(ref->hWnd, &st)) return FALSE;
        w = (int)(st.rcWindow.right - st.rcWindow.left) - 2;
        h = (int)(st.rcWindow.bottom - st.rcWindow.top) - TITLEBAR_H - 1;
    }

    if (w < 0) w = 0;
    if (h < 0) h = 0;
    *outW = w;
    *outH = h;
    return TRUE;
}

BOOL GetClientRect(HWND hWnd, LPRECT lpRect)
{
    if (!lpRect) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_GEOMETRY, &ref, ERROR_INVALID_WINDOW_HANDLE)) return FALSE;
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

    int w = 0, h = 0;
    if (!mywin_get_client_size_for_ref(&ref, &w, &h)) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    lpRect->left = 0;
    lpRect->top = 0;
    lpRect->right = w;
    lpRect->bottom = h;
    return TRUE;
}

static int mywin_nc_client_y_offset(MyWinWindowInfo* wi)
{
    if (!wi) return 0;
    if (strcmp(wi->className, "#32770") == 0) return MYWIN_DIALOG_CAPTION_H;

    /* v102.5: WS_CAPTION is a composite style (WS_BORDER | WS_DLGFRAME).
       Testing it with a plain bitwise non-zero check treated ordinary bordered
       child controls as captioned windows.  A dropped COMBOBOX uses SetCapture,
       so the mouse route goes through ScreenToClient(child); the false caption
       offset pushed the combo client origin down by MYWIN_DIALOG_CAPTION_H and
       made dropdown hit-testing react one row/element below the drawn item. */
    if (wi->hParent && ((wi->style & WS_CAPTION) == WS_CAPTION))
        return MYWIN_DIALOG_CAPTION_H;
    return 0;
}

static BOOL mywin_client_origin_screen(HWND hWnd, int* outX, int* outY)
{
    if (!hWnd || !outX || !outY) return FALSE;

    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (wi && strcmp(wi->className, "#32770") == 0 && wi->hParent == 0) {
        *outX = (int)wi->rcClient.left;
        *outY = (int)wi->rcClient.top + MYWIN_DIALOG_CAPTION_H;
        return TRUE;
    }
    if (wi && !wi->hParent) {
        /* v136: top-level USER32 HWNDs that are backed by a WindowManager frame
           have their child/control coordinates drawn relative to the frame
           client origin (x+1, y+TITLEBAR_H), not the outer frame top-left.
           ScreenToClient() used the raw USER32 metadata here, while the
           compositor and ChildWindowFromPoint path used the framed client
           surface.  That made toolbar/control hit-testing lag by one chrome
           transform and was especially visible in MDILab. */
        if (g_lpWindowManager) {
            int idx = wm_find_hwnd(g_lpWindowManager, hWnd);
            if (idx >= 0) {
                WindowRect wr;
                if (wm_get_window_rect(g_lpWindowManager, idx, &wr)) {
                    *outX = wr.x + 1;
                    *outY = wr.y + TITLEBAR_H;
                    return TRUE;
                }
            }
        }
        *outX = (int)wi->rcClient.left;
        *outY = (int)wi->rcClient.top;
        return TRUE;
    }
    if (wi && wi->hParent) {
        int px = 0, py = 0;
        if (!mywin_client_origin_screen(wi->hParent, &px, &py)) return FALSE;
        *outX = px + (int)wi->rcClient.left;
        *outY = py + (int)wi->rcClient.top + mywin_nc_client_y_offset(wi);
        return TRUE;
    }

    MyWindowState st;
    memset(&st, 0, sizeof(st));
    if (!MyGetWindowState(hWnd, &st)) return FALSE;

    if (wi && (strcmp(wi->className, "#32769") == 0 ||
               strcmp(wi->className, "Shell_TrayWnd") == 0)) {
        *outX = st.rcWindow.left;
        *outY = st.rcWindow.top;
        return TRUE;
    }

    *outX = st.rcWindow.left + 1;
    *outY = st.rcWindow.top + TITLEBAR_H;
    return TRUE;
}

BOOL ScreenToClient(HWND hWnd, LPPOINT lpPoint)
{
    if (!lpPoint) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_GEOMETRY, &ref, ERROR_INVALID_WINDOW_HANDLE)) return FALSE;
    int ox = 0, oy = 0;
    if (!mywin_client_origin_screen(hWnd, &ox, &oy)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    lpPoint->x -= ox;
    lpPoint->y -= oy;
    return TRUE;
}

BOOL ClientToScreen(HWND hWnd, LPPOINT lpPoint)
{
    if (!lpPoint) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_GEOMETRY, &ref, ERROR_INVALID_WINDOW_HANDLE)) return FALSE;
    int ox = 0, oy = 0;
    if (!mywin_client_origin_screen(hWnd, &ox, &oy)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    lpPoint->x += ox;
    lpPoint->y += oy;
    return TRUE;
}

int MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints)
{
    if (!lpPoints && cPoints) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }

    int fromX = 0, fromY = 0;
    int toX = 0, toY = 0;

    if (hWndFrom) {
        _UserHwndRef fromRef;
        if (!_HwndResolveForAction(hWndFrom, _HWND_ACTION_GEOMETRY, &fromRef, ERROR_INVALID_WINDOW_HANDLE)) return 0;
        if (!mywin_client_origin_screen(hWndFrom, &fromX, &fromY)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    }
    if (hWndTo) {
        _UserHwndRef toRef;
        if (!_HwndResolveForAction(hWndTo, _HWND_ACTION_GEOMETRY, &toRef, ERROR_INVALID_WINDOW_HANDLE)) return 0;
        if (!mywin_client_origin_screen(hWndTo, &toX, &toY)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    }

    int dx = fromX - toX;
    int dy = fromY - toY;
    for (UINT i = 0; i < cPoints; ++i) {
        lpPoints[i].x += dx;
        lpPoints[i].y += dy;
    }
    return (int)MAKELRESULT((SHORT)dx, (SHORT)dy);
}

BOOL SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_GEOMETRY, &ref, ERROR_INVALID_HANDLE)) return FALSE;
    if (!mywin_can_control_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    MyWinWindowInfo* wi = ref.wi ? ref.wi : mywin_find_info(hWnd);
    if (wi && !mywin_owns_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

    if (!wi) {
        if (g_lpWindowManager) {
            int idx = wm_find_hwnd(g_lpWindowManager, hWnd);
            if (idx >= 0)
                return wm_set_window_pos_ex(g_lpWindowManager, idx, hWndInsertAfter, X, Y, cx, cy, uFlags) ? TRUE : FALSE;
        }
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    RECT oldRc = wi->rcClient;
    BOOL oldVisible = mywin_window_own_visible(wi);

    /* Keep USER32 metadata authoritative even when a top-level frame is backed
       by the compositor WindowManager.  v152 makes visibility/style/z-order
       observable through USER32 APIs instead of leaving ShowWindow/SetWindowPos
       as a WM-only side effect. */
    if (!(uFlags & SWP_NOZORDER)) {
        if (!mywin_apply_local_zorder(hWnd, hWndInsertAfter, uFlags)) return FALSE;
    }

    if (g_lpWindowManager) {
        int idx = wm_find_hwnd(g_lpWindowManager, hWnd);
        if (idx >= 0) {
            if (uFlags & SWP_HIDEWINDOW) wi->style &= ~WS_VISIBLE;
            if (uFlags & SWP_SHOWWINDOW) wi->style |= WS_VISIBLE;
            int ok = wm_set_window_pos_ex(g_lpWindowManager, idx, hWndInsertAfter, X, Y, cx, cy, uFlags) ? TRUE : FALSE;
            if (!ok) return FALSE;
            WindowRect wr;
            if (wm_get_window_rect(g_lpWindowManager, idx, &wr)) {
                wi->rcClient.left = wr.x;
                wi->rcClient.top = wr.y;
                wi->rcClient.right = wr.x + wr.w;
                wi->rcClient.bottom = wr.y + wr.h;
            }
            return TRUE;
        }
    }

    /* v122/v152: local USER32 geometry fallback for standalone HWNDs and child
       controls that are not represented by a WindowManager frame. */
    RECT newRc = oldRc;
    int newW = (int)(oldRc.right - oldRc.left);
    int newH = (int)(oldRc.bottom - oldRc.top);

    if (!(uFlags & SWP_NOMOVE)) {
        newRc.left = X;
        newRc.top = Y;
    }
    if (!(uFlags & SWP_NOSIZE)) {
        newW = cx;
        newH = cy;
    }
    if (newW < 0) newW = 0;
    if (newH < 0) newH = 0;
    newRc.right = newRc.left + newW;
    newRc.bottom = newRc.top + newH;

    WINDOWPOS wp;
    memset(&wp, 0, sizeof(wp));
    wp.hwnd = hWnd;
    wp.hwndInsertAfter = hWndInsertAfter;
    wp.x = (int)newRc.left;
    wp.y = (int)newRc.top;
    wp.cx = (int)(newRc.right - newRc.left);
    wp.cy = (int)(newRc.bottom - newRc.top);
    wp.flags = uFlags;

    if (wi->wndproc)
        wi->wndproc(hWnd, WM_WINDOWPOSCHANGING, 0, (LPARAM)&wp);

    if (wp.flags & SWP_NOMOVE) { wp.x = (int)oldRc.left; wp.y = (int)oldRc.top; }
    if (wp.flags & SWP_NOSIZE) { wp.cx = (int)(oldRc.right - oldRc.left); wp.cy = (int)(oldRc.bottom - oldRc.top); }
    if (wp.cx < 0) wp.cx = 0;
    if (wp.cy < 0) wp.cy = 0;

    newRc.left = wp.x;
    newRc.top = wp.y;
    newRc.right = wp.x + wp.cx;
    newRc.bottom = wp.y + wp.cy;
    uFlags = wp.flags;

    wi->rcClient = newRc;
    if (uFlags & SWP_HIDEWINDOW) wi->style &= ~WS_VISIBLE;
    if (uFlags & SWP_SHOWWINDOW) wi->style |= WS_VISIBLE;
    BOOL newVisible = mywin_window_own_visible(wi);

    mywin_local_send_window_pos_messages(hWnd, wi, &oldRc, &newRc, hWndInsertAfter, uFlags);
    if (oldVisible != newVisible && wi->wndproc)
        wi->wndproc(hWnd, WM_SHOWWINDOW, newVisible ? TRUE : FALSE, 0);

    if (!(uFlags & SWP_NOACTIVATE) && !(uFlags & SWP_HIDEWINDOW))
        SetForegroundWindow(hWnd);

    mywin_publish_local_hwnd_state(hWnd, WM_WINDOWPOSCHANGED,
                                   MYWS_DIRTY_RECT|MYWS_DIRTY_VISIBLE|MYWS_DIRTY_ZORDER|MYWS_DIRTY_FOCUS);
    return TRUE;
}

BOOL MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint)
{
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (!bRepaint) flags |= SWP_NOREDRAW;
    return SetWindowPos(hWnd, HWND_TOP, X, Y, nWidth, nHeight, flags);
}

static BOOL mywin_intersect_rect_local(RECT* out, const RECT* a, const RECT* b)
{
    if (!out || !a || !b) return FALSE;
    out->left = a->left > b->left ? a->left : b->left;
    out->top = a->top > b->top ? a->top : b->top;
    out->right = a->right < b->right ? a->right : b->right;
    out->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (out->right <= out->left || out->bottom <= out->top) {
        memset(out, 0, sizeof(*out));
        return FALSE;
    }
    return TRUE;
}

static BOOL mywin_union_rect_local(RECT* dst, const RECT* src)
{
    if (!dst || !src || src->right <= src->left || src->bottom <= src->top) return FALSE;
    if (dst->right <= dst->left || dst->bottom <= dst->top) {
        *dst = *src;
        return TRUE;
    }
    if (src->left < dst->left) dst->left = src->left;
    if (src->top < dst->top) dst->top = src->top;
    if (src->right > dst->right) dst->right = src->right;
    if (src->bottom > dst->bottom) dst->bottom = src->bottom;
    return TRUE;
}

static BOOL mywin_rect_empty_local(const RECT* rc)
{
    return (!rc || rc->right <= rc->left || rc->bottom <= rc->top) ? TRUE : FALSE;
}

static RECT mywin_offset_rect_value(RECT rc, int dx, int dy)
{
    rc.left += dx;
    rc.right += dx;
    rc.top += dy;
    rc.bottom += dy;
    return rc;
}

#define MYWIN_SCROLL_UPDATE_MAX_RECTS 4

static int mywin_scroll_update_region_parts(const RECT* effective, int dx, int dy, RECT* prcUpdate, RECT* piecesOut, int maxPieces)
{
    if (prcUpdate) memset(prcUpdate, 0, sizeof(*prcUpdate));
    if (!effective || mywin_rect_empty_local(effective) || (dx == 0 && dy == 0)) return 0;

    RECT moved = mywin_offset_rect_value(*effective, dx, dy);
    RECT overlap;
    RECT bounds = {0,0,0,0};
    int pieces = 0;
#define MYWIN_ADD_SCROLL_PART(rval) do { \
        RECT _rr = (rval); \
        if (mywin_union_rect_local(&bounds, &_rr)) { \
            if (piecesOut && pieces < maxPieces) piecesOut[pieces] = _rr; \
            pieces++; \
        } \
    } while (0)

    if (!mywin_intersect_rect_local(&overlap, effective, &moved)) {
        MYWIN_ADD_SCROLL_PART(*effective);
    } else {
        RECT r;
        r.left = effective->left; r.top = effective->top; r.right = effective->right; r.bottom = overlap.top;
        MYWIN_ADD_SCROLL_PART(r);
        r.left = effective->left; r.top = overlap.bottom; r.right = effective->right; r.bottom = effective->bottom;
        MYWIN_ADD_SCROLL_PART(r);
        r.left = effective->left; r.top = overlap.top; r.right = overlap.left; r.bottom = overlap.bottom;
        MYWIN_ADD_SCROLL_PART(r);
        r.left = overlap.right; r.top = overlap.top; r.right = effective->right; r.bottom = overlap.bottom;
        MYWIN_ADD_SCROLL_PART(r);
    }
#undef MYWIN_ADD_SCROLL_PART

    if (pieces <= 0) return 0;
    if (prcUpdate) *prcUpdate = bounds;
    return pieces;
}

static int mywin_scroll_update_region_type(const RECT* effective, int dx, int dy, RECT* prcUpdate)
{
    int pieces = mywin_scroll_update_region_parts(effective, dx, dy, prcUpdate, NULL, 0);
    if (pieces <= 0) return NULLREGION;
    return (pieces == 1) ? SIMPLEREGION : COMPLEXREGION;
}

static void mywin_set_scroll_update_hrgn(HRGN hrgnUpdate, const RECT* effective, int dx, int dy)
{
    if (!hrgnUpdate) return;
    SetRectRgn(hrgnUpdate, 0, 0, 0, 0);
    RECT bounds;
    RECT parts[MYWIN_SCROLL_UPDATE_MAX_RECTS];
    int n = mywin_scroll_update_region_parts(effective, dx, dy, &bounds, parts, MYWIN_SCROLL_UPDATE_MAX_RECTS);
    (void)bounds;
    for (int i = 0; i < n && i < MYWIN_SCROLL_UPDATE_MAX_RECTS; ++i) {
        HRGN tmp = CreateRectRgnIndirect(&parts[i]);
        if (tmp) {
            CombineRgn(hrgnUpdate, hrgnUpdate, tmp, RGN_OR);
            DeleteObject((HGDIOBJ)tmp);
        }
    }
}

static BOOL mywin_child_intersects_scroll_rect(HWND hChild, const RECT* prcScroll)
{
    MyWinWindowInfo* ci = mywin_find_info(hChild);
    if (!ci || !ci->valid || !prcScroll) return FALSE;
    RECT hit;
    return mywin_intersect_rect_local(&hit, &ci->rcClient, prcScroll);
}

static void mywin_scroll_intersecting_children(HWND hWnd, const RECT* prcScroll, int dx, int dy)
{
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int count = mywin_collect_children(hWnd, children, MYWIN_MAX_WINDOW_INFOS);
    for (int i = 0; i < count; ++i) {
        HWND child = children[i];
        MyWinWindowInfo* ci = mywin_find_info(child);
        if (!ci || !ci->valid) continue;
        if (!mywin_child_intersects_scroll_rect(child, prcScroll)) continue;

        int w = (int)(ci->rcClient.right - ci->rcClient.left);
        int h = (int)(ci->rcClient.bottom - ci->rcClient.top);
        int nx = (int)ci->rcClient.left + dx;
        int ny = (int)ci->rcClient.top + dy;
        int oldX = (int)ci->rcClient.left;
        int oldY = (int)ci->rcClient.top;

        if (!SetWindowPos(child, HWND_TOP, nx, ny, w, h,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
            continue;
        }

        /* MSDN: SW_SCROLLCHILDREN sends WM_MOVE to every intersecting child even
           if the child does not move.  SetWindowPos suppresses WM_MOVE when the
           coordinates are unchanged, so mirror the observable message here. */
        if (nx == oldX && ny == oldY && ci->wndproc)
            ci->wndproc(child, WM_MOVE, 0, MAKELPARAM((WORD)nx, (WORD)ny));
    }
}

int ScrollWindowEx(HWND hWnd, int dx, int dy, const RECT* prcScroll, const RECT* prcClip, HRGN hrgnUpdate, LPRECT prcUpdate, UINT flags)
{
    if (prcUpdate) memset(prcUpdate, 0, sizeof(*prcUpdate));
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_PAINT, &ref, ERROR_INVALID_WINDOW_HANDLE)) return ERROR;
    if (!mywin_can_control_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return ERROR; }
    if (hrgnUpdate && !SetRectRgn(hrgnUpdate, 0, 0, 0, 0)) return ERROR;

    MyWinWindowInfo* wi = ref.wi ? ref.wi : mywin_find_info(hWnd);
    if (!wi || !wi->valid) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return ERROR; }

    RECT client;
    client.left = 0;
    client.top = 0;
    client.right = wi->rcClient.right - wi->rcClient.left;
    client.bottom = wi->rcClient.bottom - wi->rcClient.top;
    if (client.right < 0) client.right = 0;
    if (client.bottom < 0) client.bottom = 0;

    RECT scroll = prcScroll ? *prcScroll : client;
    RECT effective;
    if (!mywin_intersect_rect_local(&effective, &scroll, &client)) {
        if (flags & SW_SCROLLCHILDREN) mywin_scroll_intersecting_children(hWnd, &scroll, dx, dy);
        return NULLREGION;
    }
    if (prcClip) {
        RECT clipped;
        if (!mywin_intersect_rect_local(&clipped, &effective, prcClip)) {
            if (flags & SW_SCROLLCHILDREN) mywin_scroll_intersecting_children(hWnd, &scroll, dx, dy);
            return NULLREGION;
        }
        effective = clipped;
    }

    int regionType = mywin_scroll_update_region_type(&effective, dx, dy, prcUpdate);
    mywin_set_scroll_update_hrgn(hrgnUpdate, &effective, dx, dy);

    /* ScrollWindowEx scrolls client bits immediately.  The current myOS renderer
       is command-buffer based, so this translates retained commands that are
       wholly inside the effective source rectangle and relies on invalidation for
       uncovered/partial areas, matching the visible Win32 contract available in
       v158's GDI-lite layer. */
    MyGdiScrollWindowContent(hWnd, dx, dy, &effective, prcClip);

    if (flags & SW_SCROLLCHILDREN)
        mywin_scroll_intersecting_children(hWnd, &scroll, dx, dy);

    if ((flags & (SW_INVALIDATE | SW_ERASE)) && regionType != NULLREGION) {
        RECT update = {0,0,0,0};
        if (prcUpdate) update = *prcUpdate;
        else (void)mywin_scroll_update_region_type(&effective, dx, dy, &update);
        if (!mywin_rect_empty_local(&update))
            InvalidateRect(hWnd, &update, (flags & SW_ERASE) ? TRUE : FALSE);
    }

    return regionType;
}

BOOL ScrollWindow(HWND hWnd, int XAmount, int YAmount, const RECT* lpRect, const RECT* lpClipRect)
{
    UINT flags = SW_INVALIDATE;
    if (!lpRect) flags |= SW_SCROLLCHILDREN;
    return ScrollWindowEx(hWnd, XAmount, YAmount, lpRect, lpClipRect, 0, NULL, flags) != ERROR;
}


#define MYWIN_MAX_DEFER_HANDLES 16
#define MYWIN_MAX_DEFER_OPS     64
#define MYWIN_DEFER_MAGIC_BASE  0xD5000000u

typedef struct MyWinDeferOp {
    HWND hWnd;
    HWND hWndInsertAfter;
    int x, y, cx, cy;
    UINT flags;
} MyWinDeferOp;

typedef struct MyWinDeferBatch {
    int valid;
    HDWP handle;
    int count;
    int capacity;
    MyWinDeferOp ops[MYWIN_MAX_DEFER_OPS];
} MyWinDeferBatch;

static MyWinDeferBatch g_DeferBatches[MYWIN_MAX_DEFER_HANDLES];
static DWORD g_NextDeferCookie = 1;

static MyWinDeferBatch* mywin_find_defer(HDWP h)
{
    if (!h) return NULL;
    for (int i = 0; i < MYWIN_MAX_DEFER_HANDLES; ++i)
        if (g_DeferBatches[i].valid && g_DeferBatches[i].handle == h) return &g_DeferBatches[i];
    return NULL;
}

HDWP BeginDeferWindowPos(int nNumWindows)
{
    if (nNumWindows < 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    for (int i = 0; i < MYWIN_MAX_DEFER_HANDLES; ++i) {
        if (!g_DeferBatches[i].valid) {
            memset(&g_DeferBatches[i], 0, sizeof(g_DeferBatches[i]));
            g_DeferBatches[i].valid = 1;
            g_DeferBatches[i].capacity = nNumWindows > 0 ? nNumWindows : 1;
            if (g_DeferBatches[i].capacity > MYWIN_MAX_DEFER_OPS) g_DeferBatches[i].capacity = MYWIN_MAX_DEFER_OPS;
            g_DeferBatches[i].handle = (HDWP)(MYWIN_DEFER_MAGIC_BASE | ((g_NextDeferCookie++ & 0x0fffu) << 4) | (DWORD)(i + 1));
            return g_DeferBatches[i].handle;
        }
    }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

HDWP DeferWindowPos(HDWP hWinPosInfo, HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy, UINT uFlags)
{
    MyWinDeferBatch* b = mywin_find_defer(hWinPosInfo);
    if (!b) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); b->valid = 0; return 0; }
    if (b->count >= MYWIN_MAX_DEFER_OPS || b->count >= b->capacity) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); b->valid = 0; return 0; }
    MyWinDeferOp* op = &b->ops[b->count++];
    op->hWnd = hWnd;
    op->hWndInsertAfter = hWndInsertAfter;
    op->x = x; op->y = y; op->cx = cx; op->cy = cy; op->flags = uFlags;
    return hWinPosInfo;
}

BOOL EndDeferWindowPos(HDWP hWinPosInfo)
{
    MyWinDeferBatch* b = mywin_find_defer(hWinPosInfo);
    if (!b) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    MyWinDeferBatch local = *b;
    b->valid = 0;
    BOOL ok = TRUE;
    for (int i = 0; i < local.count; ++i) {
        MyWinDeferOp* op = &local.ops[i];
        if (!SetWindowPos(op->hWnd, op->hWndInsertAfter, op->x, op->y, op->cx, op->cy, op->flags)) ok = FALSE;
    }
    return ok;
}

BOOL GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT* lpwndpl)
{
    if (!lpwndpl || lpwndpl->length < sizeof(WINDOWPLACEMENT)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

    WINDOWPLACEMENT out;
    memset(&out, 0, sizeof(out));
    out.length = sizeof(out);
    out.flags = 0;
    out.ptMinPosition.x = -1;
    out.ptMinPosition.y = -1;
    out.ptMaxPosition.x = -1;
    out.ptMaxPosition.y = -1;

    RECT rc;
    if (!GetWindowRect(hWnd, &rc)) return FALSE;
    out.rcNormalPosition = rc;

    MyWindowState st;
    memset(&st, 0, sizeof(st));
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (MyGetWindowState(hWnd, &st) && st.minimized)
        out.showCmd = SW_SHOWMINIMIZED;
    else if (wi && !(wi->style & WS_VISIBLE))
        out.showCmd = SW_HIDE;
    else
        out.showCmd = SW_SHOWNORMAL;

    *lpwndpl = out;
    return TRUE;
}

BOOL SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT* lpwndpl)
{
    if (!lpwndpl || lpwndpl->length < sizeof(WINDOWPLACEMENT)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    if (!mywin_can_control_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

    const RECT* r = &lpwndpl->rcNormalPosition;
    int w = (int)(r->right - r->left);
    int h = (int)(r->bottom - r->top);
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (!SetWindowPos(hWnd, HWND_TOP, (int)r->left, (int)r->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE))
        return FALSE;
    (void)ShowWindow(hWnd, (int)lpwndpl->showCmd);
    return TRUE;
}

int GetClassNameA(HWND hWnd, LPSTR lpClassName, int nMaxCount)
{
    if (!lpClassName || nMaxCount <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    lpClassName[0] = 0;
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    snprintf(lpClassName, (size_t)nMaxCount, "%s", wi->className);
    return (int)strlen(lpClassName);
}

int GetWindowTextA(HWND hWnd, LPSTR lpString, int nMaxCount)
{
    if (!lpString || nMaxCount <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    lpString[0] = 0;
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (wi) {
        snprintf(lpString, (size_t)nMaxCount, "%s", wi->text);
        return (int)strlen(lpString);
    }
    MyWindowState st;
    if (!MyGetWindowState(hWnd, &st)) { lpString[0] = 0; return 0; }
    snprintf(lpString, (size_t)nMaxCount, "%s", st.szTitle);
    return (int)strlen(lpString);
}

BOOL SetWindowTextA(HWND hWnd, LPCSTR lpString)
{
    if (!lpString) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_can_mutate_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (wi) {
        snprintf(wi->text, MYWIN_WINDOW_TEXT_CHARS, "%s", lpString);
        wi->textHash = mywin_title_hash(wi->text);
        mywin_publish_local_hwnd_state(hWnd, WM_WINDOWTEXTCHANGED, MYWS_DIRTY_TEXT);
        if (g_HasCapability)
            hwnd_post(g_lpHwndManager, &g_CurrentCapability, hWnd, WM_WINDOWTEXTCHANGED, 0, 0);
        return TRUE;
    }
    if (!g_lpWindowManager || !mywin_can_control_window(hWnd)) return FALSE;
    return wm_set_window_title_by_hwnd(g_lpWindowManager, hWnd, lpString) ? TRUE : FALSE;
}

int GetWindowTextLengthA(HWND hWnd)
{
    char tmp[4];
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (wi) return (int)strlen(wi->text);
    return GetWindowTextA(hWnd, tmp, (int)sizeof(tmp));
}

HWND GetDlgItem(HWND hDlg, int nIDDlgItem)
{
    if (!hDlg) return 0;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (wi->valid && wi->hParent == hDlg && (int)wi->id == nIDDlgItem)
            return wi->hWnd;
    }
    return 0;
}

LRESULT SendDlgItemMessageA(HWND hDlg, int nIDDlgItem, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    HWND h = GetDlgItem(hDlg, nIDDlgItem);
    if (!h) return 0;
    MyWinWindowInfo* wi = mywin_find_info(h);
    if (wi && wi->wndproc) return wi->wndproc(h, Msg, wParam, lParam);
    return SendMessageA(h, Msg, wParam, lParam);
}

BOOL SetDlgItemTextA(HWND hDlg, int nIDDlgItem, LPCSTR lpString)
{
    HWND h = GetDlgItem(hDlg, nIDDlgItem);
    return h ? SetWindowTextA(h, lpString ? lpString : "") : FALSE;
}

UINT GetDlgItemTextA(HWND hDlg, int nIDDlgItem, LPSTR lpString, int cchMax)
{
    HWND h = GetDlgItem(hDlg, nIDDlgItem);
    return h ? (UINT)GetWindowTextA(h, lpString, cchMax) : 0u;
}

UINT GetDlgItemInt(HWND hDlg, int nIDDlgItem, BOOL* lpTranslated, BOOL bSigned)
{
    char buf[64];
    if (lpTranslated) *lpTranslated = FALSE;
    if (!GetDlgItemTextA(hDlg, nIDDlgItem, buf, (int)sizeof(buf))) return 0;
    char* end = NULL;
    errno = 0;
    if (bSigned) {
        long v = strtol(buf, &end, 10);
        if (end == buf || errno == ERANGE) return 0;
        while (end && *end && isspace((unsigned char)*end)) end++;
        if (end && *end) return 0;
        if (lpTranslated) *lpTranslated = TRUE;
        return (UINT)(int)v;
    }
    unsigned long v = strtoul(buf, &end, 10);
    if (end == buf || errno == ERANGE) return 0;
    while (end && *end && isspace((unsigned char)*end)) end++;
    if (end && *end) return 0;
    if (lpTranslated) *lpTranslated = TRUE;
    return (UINT)v;
}

BOOL SetDlgItemInt(HWND hDlg, int nIDDlgItem, UINT uValue, BOOL bSigned)
{
    char buf[64];
    if (bSigned) snprintf(buf, sizeof(buf), "%d", (int)uValue);
    else snprintf(buf, sizeof(buf), "%u", (unsigned)uValue);
    return SetDlgItemTextA(hDlg, nIDDlgItem, buf);
}

BOOL EnableWindow(HWND hWnd, BOOL bEnable)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mywin_can_mutate_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    BOOL wasEnabled = (wi->style & WS_DISABLED) ? FALSE : TRUE;
    BOOL nowEnabled = bEnable ? TRUE : FALSE;
    if (bEnable) wi->style &= ~WS_DISABLED;
    else wi->style |= WS_DISABLED;

    if (wasEnabled != nowEnabled) {
        if (!nowEnabled) {
            if (g_CaptureHwnd == hWnd || mywin_is_descendant(hWnd, g_CaptureHwnd))
                ReleaseCapture();
            if (g_FocusHwnd == hWnd || mywin_is_descendant(hWnd, g_FocusHwnd))
                SetFocus(0);
        }
        SendMessageA(hWnd, WM_ENABLE, (WPARAM)nowEnabled, 0);
        /* v172: enabled state affects non-client/client visuals and the
           effective enabled state of descendants (IsWindowEnabled walks the
           parent chain).  Queue paint damage instead of relying on a later
           incidental repaint. */
        mywin_invalidate_window_and_children(hWnd, TRUE);
        mywin_publish_local_hwnd_tree_state(hWnd, WM_ENABLE, MYWS_DIRTY_VISIBLE|MYWS_DIRTY_FOCUS|MYWS_DIRTY_OWNER);
    }
    return wasEnabled;
}

BOOL IsWindowEnabled(HWND hWnd)
{
    _UserHwndRef ref;
    if (!_HwndResolveForAction(hWnd, _HWND_ACTION_QUERY, &ref, ERROR_INVALID_WINDOW_HANDLE)) return FALSE;
    MyWinWindowInfo* wi = ref.wi ? ref.wi : mywin_find_info(hWnd);
    while (wi) {
        if (wi->style & WS_DISABLED) return FALSE;
        /* v88: #32770 dialogs are owned by their parent for routing/drawing,
           but modal DialogBoxParamA disables the owner.  Treat the dialog as
           an enabled owned top-level boundary so its controls stay usable. */
        if (strcmp(wi->className, "#32770") == 0) break;
        if (!wi->hParent) break;
        wi = mywin_find_info(wi->hParent);
    }
    return wi ? TRUE : FALSE;
}

BOOL CheckDlgButton(HWND hDlg, int nIDButton, UINT uCheck)
{
    HWND h = GetDlgItem(hDlg, nIDButton);
    MyWinWindowInfo* wi = mywin_find_info(h);
    if (!wi) return FALSE;
    wi->control->checkState = (int)uCheck;
    InvalidateRect(h, NULL, TRUE);
    return TRUE;
}

BOOL CheckRadioButton(HWND hDlg, int nIDFirstButton, int nIDLastButton, int nIDCheckButton)
{
    if (!hDlg || nIDFirstButton > nIDLastButton) return FALSE;
    BOOL found = FALSE;
    for (int id = nIDFirstButton; id <= nIDLastButton; ++id) {
        HWND h = GetDlgItem(hDlg, id);
        MyWinWindowInfo* wi = mywin_find_info(h);
        if (!wi) continue;
        wi->control->checkState = (id == nIDCheckButton) ? BST_CHECKED : BST_UNCHECKED;
        if (id == nIDCheckButton) {
            found = TRUE;
            if (strcmp(wi->className, "BUTTON") == 0 && mywin_button_is_radio_type(mywin_button_type(wi)))
                mywin_button_sync_radio_group_tabstop(h);
        }
        InvalidateRect(h, NULL, TRUE);
    }
    return found;
}

UINT IsDlgButtonChecked(HWND hDlg, int nIDButton)
{
    HWND h = GetDlgItem(hDlg, nIDButton);
    MyWinWindowInfo* wi = mywin_find_info(h);
    return wi ? (UINT)wi->control->checkState : BST_UNCHECKED;
}

static void mywin_scroll_get_state(MyWinWindowInfo* wi, int nBar, int** minp, int** maxp, int** pagep, int** posp, int** trackp)
{
    if (!wi) { if (minp) *minp = NULL; if (maxp) *maxp = NULL; if (pagep) *pagep = NULL; if (posp) *posp = NULL; if (trackp) *trackp = NULL; return; }
    if (nBar == SB_HORZ) {
        if (minp) *minp = &wi->control->stdHScrollMin;
        if (maxp) *maxp = &wi->control->stdHScrollMax;
        if (pagep) *pagep = &wi->control->stdHScrollPage;
        if (posp) *posp = &wi->control->stdHScrollPos;
        if (trackp) *trackp = &wi->control->stdHScrollTrackPos;
    } else {
        if (minp) *minp = &wi->control->ccScrollMin;
        if (maxp) *maxp = &wi->control->ccScrollMax;
        if (pagep) *pagep = &wi->control->ccScrollPage;
        if (posp) *posp = &wi->control->ccScrollPos;
        if (trackp) *trackp = &wi->control->ccScrollTrackPos;
    }
}

static int mywin_scroll_clamp_value(int pos, int minv, int maxv, int page)
{
    int maxPos = maxv;
    if (page > 1 && maxv - minv + 1 > page) maxPos = maxv - page + 1;
    if (maxPos < minv) maxPos = minv;
    if (pos < minv) pos = minv;
    if (pos > maxPos) pos = maxPos;
    return pos;
}

static void mywin_scroll_apply_style(HWND hWnd, MyWinWindowInfo* wi, int nBar, BOOL show)
{
    if (!wi) return;
    if (nBar == SB_VERT || nBar == SB_BOTH) {
        wi->control->stdVScrollVisible = show ? 1 : 0;
        if (show) wi->style |= WS_VSCROLL; else wi->style &= ~WS_VSCROLL;
    }
    if (nBar == SB_HORZ || nBar == SB_BOTH) {
        wi->control->stdHScrollVisible = show ? 1 : 0;
        if (show) wi->style |= WS_HSCROLL; else wi->style &= ~WS_HSCROLL;
    }
    InvalidateRect(hWnd, NULL, TRUE);
}

int SetScrollPos(HWND hWnd, int nBar, int nPos, BOOL bRedraw)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    int *minp, *maxp, *pagep, *posp, *trackp;
    mywin_scroll_get_state(wi, nBar == SB_HORZ ? SB_HORZ : SB_VERT, &minp, &maxp, &pagep, &posp, &trackp);
    if (!posp) return 0;
    int old = *posp;
    *posp = mywin_scroll_clamp_value(nPos, *minp, *maxp, *pagep);
    if (trackp) *trackp = *posp;
    if (bRedraw) InvalidateRect(hWnd, NULL, TRUE);
    return old;
}

int GetScrollPos(HWND hWnd, int nBar)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    int *minp, *maxp, *pagep, *posp, *trackp;
    mywin_scroll_get_state(wi, nBar == SB_HORZ ? SB_HORZ : SB_VERT, &minp, &maxp, &pagep, &posp, &trackp);
    return posp ? *posp : 0;
}

BOOL SetScrollRange(HWND hWnd, int nBar, int nMinPos, int nMaxPos, BOOL bRedraw)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return FALSE;
    int *minp, *maxp, *pagep, *posp, *trackp;
    mywin_scroll_get_state(wi, nBar == SB_HORZ ? SB_HORZ : SB_VERT, &minp, &maxp, &pagep, &posp, &trackp);
    if (!minp || !maxp || !posp) return FALSE;
    *minp = nMinPos;
    *maxp = nMaxPos < nMinPos ? nMinPos : nMaxPos;
    *posp = mywin_scroll_clamp_value(*posp, *minp, *maxp, pagep ? *pagep : 0);
    if (trackp) *trackp = *posp;
    if (bRedraw) InvalidateRect(hWnd, NULL, TRUE);
    return TRUE;
}

BOOL GetScrollRange(HWND hWnd, int nBar, LPINT lpMinPos, LPINT lpMaxPos)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return FALSE;
    int *minp, *maxp, *pagep, *posp, *trackp;
    mywin_scroll_get_state(wi, nBar == SB_HORZ ? SB_HORZ : SB_VERT, &minp, &maxp, &pagep, &posp, &trackp);
    (void)pagep; (void)posp; (void)trackp;
    if (!minp || !maxp) return FALSE;
    if (lpMinPos) *lpMinPos = *minp;
    if (lpMaxPos) *lpMaxPos = *maxp;
    return TRUE;
}

int SetScrollInfo(HWND hWnd, int nBar, LPCSCROLLINFO lpsi, BOOL redraw)
{
    if (!lpsi || lpsi->cbSize < sizeof(SCROLLINFO)) return 0;
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return 0;
    int *minp, *maxp, *pagep, *posp, *trackp;
    mywin_scroll_get_state(wi, nBar == SB_HORZ ? SB_HORZ : SB_VERT, &minp, &maxp, &pagep, &posp, &trackp);
    if (!minp || !maxp || !pagep || !posp || !trackp) return 0;
    if (lpsi->fMask & SIF_RANGE) { *minp = lpsi->nMin; *maxp = lpsi->nMax < lpsi->nMin ? lpsi->nMin : lpsi->nMax; }
    if (lpsi->fMask & SIF_PAGE) *pagep = (int)lpsi->nPage;
    if (lpsi->fMask & SIF_POS) *posp = lpsi->nPos;
    if (lpsi->fMask & SIF_TRACKPOS) *trackp = lpsi->nTrackPos;
    *posp = mywin_scroll_clamp_value(*posp, *minp, *maxp, *pagep);
    if (!(lpsi->fMask & SIF_TRACKPOS)) *trackp = *posp;
    if ((lpsi->fMask & SIF_DISABLENOSCROLL) && (*maxp <= *minp)) {
        if (nBar == SB_HORZ) wi->control->stdHScrollArrows = ESB_DISABLE_BOTH;
        else wi->control->stdVScrollArrows = ESB_DISABLE_BOTH;
    }
    if (redraw) InvalidateRect(hWnd, NULL, TRUE);
    return *posp;
}

BOOL GetScrollInfo(HWND hWnd, int nBar, LPSCROLLINFO lpsi)
{
    if (!lpsi || lpsi->cbSize < sizeof(SCROLLINFO)) return FALSE;
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return FALSE;
    int *minp, *maxp, *pagep, *posp, *trackp;
    mywin_scroll_get_state(wi, nBar == SB_HORZ ? SB_HORZ : SB_VERT, &minp, &maxp, &pagep, &posp, &trackp);
    if (!minp || !maxp || !pagep || !posp || !trackp) return FALSE;
    if (lpsi->fMask & SIF_RANGE) { lpsi->nMin = *minp; lpsi->nMax = *maxp; }
    if (lpsi->fMask & SIF_PAGE) lpsi->nPage = (UINT)(*pagep < 0 ? 0 : *pagep);
    if (lpsi->fMask & SIF_POS) lpsi->nPos = *posp;
    if (lpsi->fMask & SIF_TRACKPOS) lpsi->nTrackPos = *trackp;
    return TRUE;
}

BOOL ShowScrollBar(HWND hWnd, int wBar, BOOL bShow)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return FALSE;
    mywin_scroll_apply_style(hWnd, wi, wBar, bShow);
    return TRUE;
}

BOOL EnableScrollBar(HWND hWnd, UINT wSBflags, UINT wArrows)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi) return FALSE;
    if (wSBflags == SB_VERT || wSBflags == SB_BOTH || wSBflags == SB_CTL) wi->control->stdVScrollArrows = wArrows;
    if (wSBflags == SB_HORZ || wSBflags == SB_BOTH) wi->control->stdHScrollArrows = wArrows;
    InvalidateRect(hWnd, NULL, TRUE);
    return TRUE;
}

DWORD GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId)
{
    if (lpdwProcessId) *lpdwProcessId = 0;
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (wi) {
        if (lpdwProcessId) *lpdwProcessId = wi->dwProcessId;
        return wi->dwThreadId;
    }
    if (!g_lpHwndManager || !hWnd || !hwnd_is_window(g_lpHwndManager, hWnd)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    DWORD pid = hwnd_get_owner_pid(g_lpHwndManager, hWnd);
    DWORD tid = hwnd_get_owner_tid(g_lpHwndManager, hWnd);
    if (lpdwProcessId) *lpdwProcessId = pid;
    return tid;
}

BOOL AttachThreadInput(DWORD idAttach, DWORD idAttachTo, BOOL fAttach)
{
    if (!idAttach || !idAttachTo) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (idAttach == idAttachTo) return TRUE;
    for (int i = 0; i < MYWIN_MAX_INPUT_ATTACH; ++i) {
        if (!g_InputAttach[i].valid) continue;
        if ((g_InputAttach[i].a == idAttach && g_InputAttach[i].b == idAttachTo) ||
            (g_InputAttach[i].a == idAttachTo && g_InputAttach[i].b == idAttach)) {
            if (!fAttach) memset(&g_InputAttach[i], 0, sizeof(g_InputAttach[i]));
            return TRUE;
        }
    }
    if (!fAttach) return TRUE;
    for (int i = 0; i < MYWIN_MAX_INPUT_ATTACH; ++i) {
        if (!g_InputAttach[i].valid) {
            g_InputAttach[i].valid = 1;
            g_InputAttach[i].a = idAttach;
            g_InputAttach[i].b = idAttachTo;
            return TRUE;
        }
    }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
}

HWND SetParent(HWND hWndChild, HWND hWndNewParent)
{
    MyWinWindowInfo* child = mywin_find_info(hWndChild);
    if (!child) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (!mywin_can_mutate_window(hWndChild)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    if (hWndNewParent) {
        if (!IsWindow(hWndNewParent)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
        if (!mywin_same_window_owner(hWndChild, hWndNewParent)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    }
    HWND old = child->hParent;
    mywin_set_parent_linked(child, hWndNewParent);
    if (hWndNewParent) child->style |= WS_CHILD;
    return old;
}

HWND GetParent(HWND hWnd)
{
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    return (wi && (wi->style & WS_CHILD)) ? wi->hParent : 0;
}

HWND GetAncestor(HWND hWnd, UINT gaFlags)
{
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (gaFlags == GA_PARENT) return GetParent(hWnd);
    if (gaFlags == GA_ROOT || gaFlags == GA_ROOTOWNER) {
        HWND cur = hWnd;
        HWND parent = GetParent(cur);
        while (parent) {
            cur = parent;
            parent = GetParent(cur);
        }
        if (gaFlags == GA_ROOTOWNER) {
            HWND owner = GetWindow(cur, GW_OWNER);
            while (owner) {
                cur = owner;
                owner = GetWindow(cur, GW_OWNER);
            }
        }
        SetLastError(ERROR_SUCCESS);
        return cur;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
}

HWND GetTopWindow(HWND hWnd)
{
    if (hWnd && !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (hWnd && !mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    HWND top = mywin_top_child_unchecked(hWnd);
    SetLastError(top ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND);
    return top;
}

BOOL BringWindowToTop(HWND hWnd)
{
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

HWND GetWindow(HWND hWnd, UINT uCmd)
{
    if (!hWnd || !IsWindow(hWnd)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    if (!mywin_can_read_window(hWnd)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    if (uCmd == GW_OWNER) {
        MyWinWindowInfo* ownerWi = mywin_find_info(hWnd);
        return ownerWi ? ownerWi->hOwner : 0;
    }
    if (uCmd == GW_CHILD) {
        return mywin_top_child_unchecked(hWnd); /* top child */
    }

    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    HWND parent = wi ? wi->hParent : 0;
    HWND siblings[MYWIN_MAX_WINDOW_INFOS];
    int count = mywin_collect_children(parent, siblings, MYWIN_MAX_WINDOW_INFOS);
    int idx = -1;
    for (int i = 0; i < count; ++i) {
        if (siblings[i] == hWnd) { idx = i; break; }
    }
    if (idx < 0) return 0;

    /* mywin_collect_children returns bottom->top.  Win32 GW_HWNDFIRST/GW_CHILD
       expose the top of the stack; GW_HWNDNEXT walks downward in Z-order. */
    switch (uCmd) {
    case GW_HWNDFIRST: return (count > 0) ? siblings[count - 1] : 0;
    case GW_HWNDLAST:  return (count > 0) ? siblings[0] : 0;
    case GW_HWNDNEXT:  return (idx > 0) ? siblings[idx - 1] : 0;
    case GW_HWNDPREV:  return (idx + 1 < count) ? siblings[idx + 1] : 0;
    default:           return 0;
    }
}

BOOL IsChild(HWND hWndParent, HWND hWnd)
{
    return (hWndParent && hWnd && hWndParent != hWnd && mywin_is_descendant(hWndParent, hWnd)) ? TRUE : FALSE;
}

BOOL EnumChildWindows(HWND hWndParent, WNDENUMPROC lpEnumFunc, LPARAM lParam)
{
    if (!lpEnumFunc) return FALSE;
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int count = mywin_collect_children(hWndParent, children, MYWIN_MAX_WINDOW_INFOS);
    for (int i = 0; i < count; i++) {
        if (IsWindow(children[i]) && !lpEnumFunc(children[i], lParam))
            return FALSE;
    }
    return TRUE;
}

int GetDlgCtrlID(HWND hWnd)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    return wi ? (int)wi->id : 0;
}

static HWND mywin_child_from_point_ex(HWND hWndParent, POINT Point, UINT flags)
{
    _UserHwndRef parentRef;
    if (!_HwndResolveForAction(hWndParent, _HWND_ACTION_HITTEST, &parentRef, ERROR_INVALID_WINDOW_HANDLE)) return 0;
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int count = mywin_collect_children(hWndParent, children, MYWIN_MAX_WINDOW_INFOS);

    /* Dropped COMBOBOX lists are popup-like chrome.  They must win hit-testing
       over later sibling controls such as the Common Dialog "Read only"
       checkbox.  Walk top->bottom in the same local Z-order used for paint. */
    if (!(flags & CWP_SKIPINVISIBLE)) {
        for (int i = count - 1; i >= 0; --i) {
            if (!hwnd_query_action(g_lpHwndManager, children[i], _HWND_ACTION_HITTEST, NULL)) continue;
            MyWinWindowInfo* wi = mywin_find_info(children[i]);
            if (!wi || !(wi->style & WS_VISIBLE)) continue;
            if ((flags & CWP_SKIPDISABLED) && (wi->style & WS_DISABLED)) continue;
            if ((flags & CWP_SKIPTRANSPARENT) && (wi->exStyle & WS_EX_TRANSPARENT)) continue;
            if (strcmp(wi->className, "COMBOBOX") != 0 || !wi->control->ccDropped) continue;
            RECT dropRc;
            mywin_combo_dropdown_rect_parent(wi, &dropRc);
            if (mywin_pt_in_rect(&wi->rcClient, (int)Point.x, (int)Point.y) ||
                mywin_pt_in_rect(&dropRc, (int)Point.x, (int)Point.y))
                return wi->hWnd;
        }
    }

    for (int i = count - 1; i >= 0; --i) {
        if (!hwnd_query_action(g_lpHwndManager, children[i], _HWND_ACTION_HITTEST, NULL)) continue;
        MyWinWindowInfo* wi = mywin_find_info(children[i]);
        if (!wi) continue;
        if ((flags & CWP_SKIPINVISIBLE) && !(wi->style & WS_VISIBLE)) continue;
        if ((flags & CWP_SKIPDISABLED) && (wi->style & WS_DISABLED)) continue;
        if ((flags & CWP_SKIPTRANSPARENT) && (wi->exStyle & WS_EX_TRANSPARENT)) continue;
        if (mywin_pt_in_rect(&wi->rcClient, (int)Point.x, (int)Point.y))
            return wi->hWnd;
    }
    return 0;
}

HWND ChildWindowFromPoint(HWND hWndParent, POINT Point)
{
    return mywin_child_from_point_ex(hWndParent, Point, CWP_SKIPINVISIBLE);
}

HWND ChildWindowFromPointEx(HWND hWndParent, POINT Point, UINT uFlags)
{
    return mywin_child_from_point_ex(hWndParent, Point, uFlags);
}

HWND RealChildWindowFromPoint(HWND hwndParent, POINT ptParentClientCoords)
{
    return mywin_child_from_point_ex(hwndParent, ptParentClientCoords, CWP_SKIPINVISIBLE);
}

HWND WindowFromPoint(POINT Point)
{
    HWND best = 0;
    DWORD bestZ = 0;
    for (int i = 0; i < MYWIN_MAX_WINDOW_INFOS; ++i) {
        MyWinWindowInfo* wi = &g_WindowInfos[i];
        if (!wi->valid || wi->hParent || !(wi->style & WS_VISIBLE)) continue;
        if (!hwnd_query_action(g_lpHwndManager, wi->hWnd, _HWND_ACTION_HITTEST, NULL)) continue;
        RECT rc;
        if (!mywin_get_local_window_rect_screen(wi, &rc)) continue;
        if (!mywin_pt_in_rect(&rc, (int)Point.x, (int)Point.y)) continue;
        if (!best || wi->zOrder >= bestZ) { best = wi->hWnd; bestZ = wi->zOrder; }
    }

    if (!best && g_lpWindowManager) {
        HWND wmHit = 0;
        Capability* cap = NULL;
        if (wm_client_endpoint_at_point(g_lpWindowManager, (int)Point.x, (int)Point.y, &wmHit, &cap) &&
            wmHit && hwnd_query_action(g_lpHwndManager, wmHit, _HWND_ACTION_HITTEST, NULL))
            best = wmHit;
    }

    if (!best) return 0;

    POINT clientPt = Point;
    if (ScreenToClient(best, &clientPt)) {
        HWND child = ChildWindowFromPoint(best, clientPt);
        if (child) return child;
    }
    return best;
}


static void mywin_draw_listbox_control(Framebuffer* fb, MyWinWindowInfo* wi, int x, int y, int w, int h)
{
    fb_rect(fb, x, y, w, h, (wi->style & WS_DISABLED) ? COLOR(20,20,24) : COLOR(14,16,24));
    fb_rect_outline(fb, x, y, w, h, g_FocusHwnd == wi->hWnd ? COLOR(255,230,120) : COLOR(100,118,150));
    int ih = wi->control->ccItemHeight > 0 ? wi->control->ccItemHeight : 16;
    int vis = h / ih;
    if (vis < 1) vis = 1;
    mywin_cc_clamp_top(wi);
    for (int row = 0; row < vis; ++row) {
        int idx = wi->control->ccTopIndex + row;
        if (idx >= wi->control->ccCount) break;
        int iy = y + row * ih;
        int selected = (idx == wi->control->ccCurSel) || wi->control->ccSel[idx];
        if (selected) fb_rect(fb, x + 2, iy + 1, w - 4, ih - 2, COLOR(64,88,135));
        DrawClipTextA(fb, x + 6, iy + 4, wi->control->ccItems[idx], (wi->style & WS_DISABLED) ? COLOR(120,120,130) : WHITE, x+4, iy, w-8, ih);
    }
    if (wi->control->ccCount > vis) {
        int sx = x + w - 7;
        fb_rect(fb, sx, y + 2, 5, h - 4, COLOR(34,38,54));
        int thumbH = h * vis / wi->control->ccCount;
        if (thumbH < 10) thumbH = 10;
        int maxTop = wi->control->ccCount - vis;
        int thumbY = y + 2 + (maxTop > 0 ? mywin_muldiv_int(wi->control->ccTopIndex, h - 4 - thumbH, maxTop) : 0);
        fb_rect(fb, sx + 1, thumbY, 3, thumbH, COLOR(130,150,190));
    }
}

static void mywin_draw_scrollbar_control(Framebuffer* fb, MyWinWindowInfo* wi, int x, int y, int w, int h)
{
    fb_rect(fb, x, y, w, h, (wi->style & WS_DISABLED) ? COLOR(25,25,28) : COLOR(28,32,42));
    fb_rect_outline(fb, x, y, w, h, g_FocusHwnd == wi->hWnd ? COLOR(255,230,120) : COLOR(98,112,145));
    if (h < 32) return;
    fb_rect(fb, x + 1, y + 1, w - 2, 15, COLOR(48,56,75));
    fb_rect(fb, x + 1, y + h - 16, w - 2, 15, COLOR(48,56,75));
    int cx = x + w / 2;
    font_draw_str(fb, cx - 3, y + 5, "^", WHITE);
    font_draw_str(fb, cx - 3, y + h - 12, "v", WHITE);
    int t0 = 0, t1 = 0;
    mywin_scrollbar_thumb_rect(wi, &t0, &t1);
    fb_rect(fb, x + 2, y + t0, w - 4, t1 - t0, wi->control->ccScrollTracking ? COLOR(180,195,230) : COLOR(112,130,170));
}

static int mywin_button_text_width_no_mnemonic(const char* text)
{
    char tmp[160];
    mywin_strip_mnemonics(text, tmp, sizeof(tmp));
    return (int)strlen(tmp) * 8;
}

static int mywin_button_text_x(const MyWinWindowInfo* wi, int x, int w, int textW, int leftInset, int rightInset)
{
    DWORD align = wi ? (wi->style & (BS_LEFT | BS_RIGHT | BS_CENTER)) : BS_LEFT;
    if (align == BS_RIGHT) {
        int tx = x + w - rightInset - textW;
        return tx < x + leftInset ? x + leftInset : tx;
    }
    if (align == BS_CENTER) {
        int tx = x + (w - textW) / 2;
        if (tx < x + leftInset) tx = x + leftInset;
        if (tx + textW > x + w - rightInset) tx = x + w - rightInset - textW;
        return tx < x + leftInset ? x + leftInset : tx;
    }
    return x + leftInset;
}

static int mywin_button_text_y(const MyWinWindowInfo* wi, int y, int h)
{
    DWORD align = wi ? (wi->style & (BS_TOP | BS_BOTTOM | BS_VCENTER)) : BS_VCENTER;
    if (align == BS_TOP) return y + 4;
    if (align == BS_BOTTOM) return y + h - 12;
    return y + (h - 8) / 2;
}

static void mywin_draw_radio_glyph(Framebuffer* fb, int bx, int by, int box, int checked, int disabled)
{
    Color edge = disabled ? COLOR(80,80,90) : COLOR(170,185,220);
    Color dot  = disabled ? COLOR(120,120,130) : COLOR(255,230,120);
    /* Rect-only circle approximation: short top/bottom lines plus tall sides. */
    fb_rect(fb, bx + 3, by, box - 6, 1, edge);
    fb_rect(fb, bx + 2, by + 1, box - 4, 1, edge);
    fb_rect(fb, bx + 1, by + 2, 1, box - 4, edge);
    fb_rect(fb, bx + box - 2, by + 2, 1, box - 4, edge);
    fb_rect(fb, bx + 2, by + box - 2, box - 4, 1, edge);
    fb_rect(fb, bx + 3, by + box - 1, box - 6, 1, edge);
    if (checked) {
        int d = box >= 12 ? 5 : 3;
        int dx = bx + (box - d) / 2;
        int dy = by + (box - d) / 2;
        fb_rect(fb, dx + 1, dy, d - 2, d, dot);
        fb_rect(fb, dx, dy + 1, d, d - 2, dot);
    }
}

static void mywin_draw_button_family_control(Framebuffer* fb, MyWinWindowInfo* wi, int x, int y, int w, int h)
{
    if (!fb || !wi) return;
    UINT type = mywin_button_type(wi);
    int disabled = (wi->style & WS_DISABLED) ? 1 : 0;
    int pushedLike = (wi->style & BS_PUSHLIKE) ? 1 : 0;
    int checked = (wi->control->checkState == BST_CHECKED || wi->control->checkState == BST_INDETERMINATE);
    Color textColor = disabled ? COLOR(130,130,140) : WHITE;
    Color focusColor = COLOR(255,230,120);
    Color edgeColor = disabled ? COLOR(80,80,90) : COLOR(120,135,170);
    Color faceColor = disabled ? COLOR(34,35,45) : COLOR(45,49,70);
    if (wi->control->pressed || (pushedLike && checked)) faceColor = disabled ? COLOR(30,30,36) : COLOR(34,38,58);

    if (type == BS_GROUPBOX) {
        int titleW = mywin_button_text_width_no_mnemonic(wi->text) + 12;
        int tx = x + 10;
        int ty = y;
        Color frame = disabled ? COLOR(70,72,84) : COLOR(110,120,145);
        /* Win32 group boxes are BUTTON controls with a frame and a caption gap. */
        fb_rect(fb, x + 1, y + 7, 8, 1, frame);
        if (titleW > 12) {
            mywin_draw_text_mnemonic(fb, tx, ty + 1, wi->text, textColor, x, y, w, 12);
            int after = tx + titleW - 2;
            if (after < x + w - 1) fb_rect(fb, after, y + 7, x + w - 1 - after, 1, frame);
        } else {
            fb_rect(fb, x + 1, y + 7, w - 2, 1, frame);
        }
        fb_rect(fb, x + 1, y + h - 1, w - 2, 1, frame);
        fb_rect(fb, x, y + 7, 1, h - 8, frame);
        fb_rect(fb, x + w - 1, y + 7, 1, h - 8, frame);
        return;
    }

    if ((mywin_button_is_check_type(type) || mywin_button_is_radio_type(type)) && !pushedLike) {
        int leftText = (wi->style & BS_LEFTTEXT) ? 1 : 0;
        int box = (h < 14) ? h - 2 : 12;
        if (box < 8) box = 8;
        int by = y + (h - box) / 2;
        int bx = leftText ? (x + w - box - 4) : (x + 4);
        int txBase = leftText ? x + 4 : (bx + box + 6);
        int tw = leftText ? (bx - x - 8) : (w - (txBase - x) - 2);
        int textW = mywin_button_text_width_no_mnemonic(wi->text);
        int tx = mywin_button_text_x(wi, txBase - (leftText ? 0 : x), leftText ? (bx - txBase) : tw, textW, 0, 0);
        if (!leftText) tx = txBase;
        else if ((wi->style & (BS_LEFT | BS_RIGHT | BS_CENTER)) == BS_RIGHT) tx = bx - 8 - textW;
        else if ((wi->style & (BS_LEFT | BS_RIGHT | BS_CENTER)) == BS_CENTER) tx = x + 4 + ((bx - x - 12) - textW) / 2;
        if (tx < x + 4) tx = x + 4;
        int ty = mywin_button_text_y(wi, y, h);

        if (mywin_button_is_radio_type(type)) {
            mywin_draw_radio_glyph(fb, bx, by, box, wi->control->checkState == BST_CHECKED, disabled);
        } else {
            fb_rect(fb, bx, by, box, box, disabled ? COLOR(20,20,24) : COLOR(12,14,20));
            fb_rect_outline(fb, bx, by, box, box, disabled ? COLOR(80,80,90) : COLOR(170,185,220));
            if (wi->control->checkState == BST_CHECKED) {
                /* clear x-ish check mark, still using the tiny bitmap font style */
                font_draw_str(fb, bx + 2, by + 2, "x", disabled ? COLOR(120,120,130) : COLOR(255,230,120));
            } else if (wi->control->checkState == BST_INDETERMINATE) {
                fb_rect(fb, bx + 3, by + 3, box - 6, box - 6, disabled ? COLOR(90,90,100) : COLOR(255,230,120));
            }
        }
        mywin_draw_text_mnemonic(fb, tx, ty, wi->text, textColor, leftText ? x : txBase, y, tw, h);
        if (g_FocusHwnd == wi->hWnd) fb_rect_outline(fb, x, y, w, h, focusColor);
        if (wi->control->pressed) fb_rect_outline(fb, x+1, y+1, w-2, h-2, COLOR(255,255,255));
        return;
    }

    fb_rect(fb, x, y, w, h, faceColor);
    fb_rect_outline(fb, x, y, w, h, edgeColor);
    if (!disabled && !wi->control->pressed) {
        fb_rect(fb, x + 1, y + 1, w - 2, 1, COLOR(78,86,116));
        fb_rect(fb, x + 1, y + 1, 1, h - 2, COLOR(78,86,116));
    }
    if (type == BS_DEFPUSHBUTTON) {
        fb_rect_outline(fb, x - 2, y - 2, w + 4, h + 4, focusColor);
        fb_rect_outline(fb, x - 1, y - 1, w + 2, h + 2, COLOR(25,28,40));
    }
    if (g_FocusHwnd == wi->hWnd) {
        fb_rect_outline(fb, x, y, w, h, focusColor);
        fb_rect_outline(fb, x + 3, y + 3, w - 6, h - 6, focusColor);
    }
    if (wi->control->pressed) fb_rect_outline(fb, x+1, y+1, w-2, h-2, COLOR(255,255,255));

    int textW = mywin_button_text_width_no_mnemonic(wi->text);
    int tx = mywin_button_text_x(wi, x, w, textW, 7, 7);
    int ty = mywin_button_text_y(wi, y, h);
    if (wi->style & BS_MULTILINE) {
        char buf[160];
        mywin_strip_mnemonics(wi->text, buf, sizeof(buf));
        char* br = strchr(buf, '|');
        if (br) {
            *br = 0;
            int w1 = (int)strlen(buf) * 8;
            int w2 = (int)strlen(br + 1) * 8;
            int tx1 = mywin_button_text_x(wi, x, w, w1, 7, 7);
            int tx2 = mywin_button_text_x(wi, x, w, w2, 7, 7);
            DrawClipTextA(fb, tx1, y + h/2 - 9, buf, textColor, x + 4, y + 3, w - 8, h - 6);
            DrawClipTextA(fb, tx2, y + h/2 + 1, br + 1, textColor, x + 4, y + 3, w - 8, h - 6);
            return;
        }
    }
    mywin_draw_text_mnemonic(fb, tx + (wi->control->pressed ? 1 : 0), ty + (wi->control->pressed ? 1 : 0), wi->text, textColor, x + 4, y + 3, w - 8, h - 6);
}

static void mywin_draw_combo_dropdown_popup(Framebuffer* fb, MyWinWindowInfo* wi, int x, int y, int w, int h)
{
    (void)h;
    if (!fb || !wi || !wi->control->ccDropped) return;
    RECT dr;
    mywin_combo_dropdown_rect_local(wi, &dr);
    int dh = dr.bottom - dr.top;
    int dy = y + dr.top;
    fb_rect(fb, x + dr.left, dy, w, dh, COLOR(13,15,23));
    fb_rect_outline(fb, x + dr.left, dy, w, dh, COLOR(155,170,210));
    int ih = wi->control->ccItemHeight > 0 ? wi->control->ccItemHeight : 16;
    int vis = (dh - 2) / ih;
    if (vis < 1) vis = 1;
    mywin_cc_clamp_top(wi);
    for (int row = 0; row < vis; ++row) {
        int idx = wi->control->ccTopIndex + row;
        if (idx >= wi->control->ccCount) break;
        int iy = dy + 1 + row * ih;
        if (idx == wi->control->ccCurSel) fb_rect(fb, x + dr.left + 2, iy, w - 4, ih, COLOR(64,88,135));
        DrawClipTextA(fb, x + dr.left + 6, iy + 3, wi->control->ccItems[idx], WHITE, x + dr.left + 4, iy, w - 8, ih);
    }
}

static void mywin_draw_combo_control(Framebuffer* fb, MyWinWindowInfo* wi, int x, int y, int w, int h)
{
    fb_rect(fb, x, y, w, h, (wi->style & WS_DISABLED) ? COLOR(22,22,25) : COLOR(12,14,20));
    fb_rect_outline(fb, x, y, w, h, g_FocusHwnd == wi->hWnd ? COLOR(255,230,120) : COLOR(105,120,150));
    int arrowW = h > 18 ? h : 18;
    fb_rect(fb, x + w - arrowW, y + 1, arrowW - 1, h - 2, COLOR(46,54,75));
    font_draw_str(fb, x + w - arrowW + 6, y + h/2 - 3, "v", WHITE);
    const char* txt = (wi->control->ccCurSel >= 0 && wi->control->ccCurSel < wi->control->ccCount) ? wi->control->ccItems[wi->control->ccCurSel] : wi->text;
    DrawClipTextA(fb, x + 6, y + 6, txt ? txt : "", (wi->style & WS_DISABLED) ? COLOR(130,130,140) : WHITE, x + 4, y + 2, w - arrowW - 8, h - 4);
    mywin_draw_combo_dropdown_popup(fb, wi, x, y, w, h);
}


/* v179: per-child-control backing cache.
   v177/v178 cache whole top-level windows, but a single BUTTON state change
   still forced draw_window_uncached() to run MyDrawChildWindows() and repaint
   every child control in order.  That was correct but visually wasteful for
   OOP/DialogLab: unchanged command buttons could still appear as a tiny
   sequential build-up during initial bridge acks or owner repaint.  Keep the
   Win32 model (controls are HWNDs with their own state) and cache each control
   by a conservative visual signature.  A control only rerenders when its own
   state/geometry/text/focus/effective-enabled data changes; otherwise we blit
   the retained control bitmap into the current top-level backing surface. */
#define MYWIN_CONTROL_CACHE_SLOTS 384

typedef struct MyWinControlBackingCache {
    int valid;
    HWND hwnd;
    int w;
    int h;
    int stride;
    unsigned long long sig;
    uint8_t* pixels;
} MyWinControlBackingCache;

static MyWinControlBackingCache g_ControlBackingCache[MYWIN_CONTROL_CACHE_SLOTS];
static unsigned g_ControlBackingVictim = 0;

static unsigned long long mywin_hash_u64_local(unsigned long long h, unsigned long long v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h ? h : 0x6d6f73635f686173ull;
}

static unsigned long long mywin_hash_cstr_local(unsigned long long h, const char* s)
{
    if (!s) return mywin_hash_u64_local(h, 0);
    while (*s) h = mywin_hash_u64_local(h, (unsigned char)*s++);
    return h;
}

static unsigned long long mywin_control_visual_signature(MyWinWindowInfo* wi, int w, int h)
{
    unsigned long long sig = 0xcbf29ce484222325ull;
    if (!wi) return 0;
    sig = mywin_hash_u64_local(sig, (uintptr_t)wi->hWnd);
    sig = mywin_hash_u64_local(sig, (uintptr_t)wi->hParent);
    sig = mywin_hash_u64_local(sig, (uintptr_t)wi->hOwner);
    sig = mywin_hash_u64_local(sig, wi->dwProcessId);
    sig = mywin_hash_u64_local(sig, wi->dwThreadId);
    sig = mywin_hash_u64_local(sig, wi->id);
    sig = mywin_hash_u64_local(sig, wi->style);
    sig = mywin_hash_u64_local(sig, wi->exStyle);
    sig = mywin_hash_u64_local(sig, (unsigned long long)(uint32_t)wi->rcClient.left);
    sig = mywin_hash_u64_local(sig, (unsigned long long)(uint32_t)wi->rcClient.top);
    sig = mywin_hash_u64_local(sig, (unsigned long long)(uint32_t)wi->rcClient.right);
    sig = mywin_hash_u64_local(sig, (unsigned long long)(uint32_t)wi->rcClient.bottom);
    sig = mywin_hash_u64_local(sig, (unsigned)w);
    sig = mywin_hash_u64_local(sig, (unsigned)h);
    sig = mywin_hash_cstr_local(sig, wi->className);
    sig = mywin_hash_cstr_local(sig, wi->text);
    sig = mywin_hash_u64_local(sig, wi->control->pressed);
    sig = mywin_hash_u64_local(sig, wi->control->checkState);
    sig = mywin_hash_u64_local(sig, (g_FocusHwnd == wi->hWnd) ? 1u : 0u);
    sig = mywin_hash_u64_local(sig, IsWindowEnabled(wi->hWnd) ? 1u : 0u);

    sig = mywin_hash_u64_local(sig, wi->control->editCaret);
    sig = mywin_hash_u64_local(sig, wi->control->editSelStart);
    sig = mywin_hash_u64_local(sig, wi->control->editSelEnd);
    sig = mywin_hash_u64_local(sig, wi->control->editFirstLine);
    sig = mywin_hash_u64_local(sig, wi->control->editHScroll);
    sig = mywin_hash_u64_local(sig, wi->control->editPasswordChar);

    sig = mywin_hash_u64_local(sig, wi->control->ccCount);
    sig = mywin_hash_u64_local(sig, wi->control->ccCurSel);
    sig = mywin_hash_u64_local(sig, wi->control->ccTopIndex);
    sig = mywin_hash_u64_local(sig, wi->control->ccCaretIndex);
    sig = mywin_hash_u64_local(sig, wi->control->ccAnchorIndex);
    sig = mywin_hash_u64_local(sig, wi->control->ccItemHeight);
    sig = mywin_hash_u64_local(sig, wi->control->ccDropped);
    sig = mywin_hash_u64_local(sig, wi->control->ccDropHeight);
    sig = mywin_hash_u64_local(sig, wi->control->ccScrollMin);
    sig = mywin_hash_u64_local(sig, wi->control->ccScrollMax);
    sig = mywin_hash_u64_local(sig, wi->control->ccScrollPage);
    sig = mywin_hash_u64_local(sig, wi->control->ccScrollPos);
    sig = mywin_hash_u64_local(sig, wi->control->ccScrollTracking);
    sig = mywin_hash_u64_local(sig, wi->control->ccScrollPressedPart);
    sig = mywin_hash_u64_local(sig, wi->control->stdVScrollVisible);
    sig = mywin_hash_u64_local(sig, wi->control->stdHScrollVisible);
    sig = mywin_hash_u64_local(sig, wi->control->stdVScrollArrows);
    sig = mywin_hash_u64_local(sig, wi->control->stdHScrollArrows);
    sig = mywin_hash_u64_local(sig, wi->control->stdHScrollPos);
    for (int i = 0; i < wi->control->ccCount && i < MYWIN_CC_MAX_ITEMS; ++i) {
        sig = mywin_hash_cstr_local(sig, wi->control->ccItems[i]);
        sig = mywin_hash_u64_local(sig, (unsigned long long)wi->control->ccItemData[i]);
        sig = mywin_hash_u64_local(sig, wi->control->ccSel[i]);
    }
    return sig ? sig : 1ull;
}

static int mywin_control_is_cacheable(MyWinWindowInfo* wi)
{
    if (!wi || !wi->valid) return 0;
    /* Dialog containers own recursive child composition; cache leaf controls. */
    return mywin_is_class(wi->hWnd, "BUTTON")   || mywin_is_class(wi->hWnd, "STATIC") ||
           mywin_is_class(wi->hWnd, "EDIT")     || mywin_is_class(wi->hWnd, "LISTBOX") ||
           mywin_is_class(wi->hWnd, "SCROLLBAR")|| mywin_is_class(wi->hWnd, "COMBOBOX");
}

static MyWinControlBackingCache* mywin_control_cache_slot(HWND hwnd)
{
    for (int i = 0; i < MYWIN_CONTROL_CACHE_SLOTS; ++i)
        if (g_ControlBackingCache[i].valid && g_ControlBackingCache[i].hwnd == hwnd)
            return &g_ControlBackingCache[i];
    for (int i = 0; i < MYWIN_CONTROL_CACHE_SLOTS; ++i)
        if (!g_ControlBackingCache[i].valid) {
            g_ControlBackingCache[i].valid = 1;
            g_ControlBackingCache[i].hwnd = hwnd;
            return &g_ControlBackingCache[i];
        }
    MyWinControlBackingCache* c = &g_ControlBackingCache[g_ControlBackingVictim++ % MYWIN_CONTROL_CACHE_SLOTS];
    free(c->pixels);
    memset(c, 0, sizeof(*c));
    c->valid = 1;
    c->hwnd = hwnd;
    return c;
}

static int mywin_control_cache_ensure(MyWinControlBackingCache* c, int w, int h)
{
    if (!c || w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    int stride = w * (int)sizeof(uint32_t);
    if (c->pixels && c->w == w && c->h == h && c->stride == stride) return 1;
    uint8_t* p = (uint8_t*)calloc((size_t)stride, (size_t)h);
    if (!p) return 0;
    free(c->pixels);
    c->pixels = p;
    c->w = w;
    c->h = h;
    c->stride = stride;
    c->sig = 0;
    return 1;
}

static void mywin_blit_control_cache(const MyWinControlBackingCache* c, Framebuffer* fb, int dstX, int dstY)
{
    if (!c || !c->pixels || !fb || !fb->backbuf) return;
    int sx = 0, sy = 0, w = c->w, h = c->h;
    int dx = dstX, dy = dstY;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > fb->width) w = fb->width - dx;
    if (dy + h > fb->height) h = fb->height - dy;
    if (fb->clip_enabled) {
        int cx1 = fb->clip_x, cy1 = fb->clip_y;
        int cx2 = fb->clip_x + fb->clip_w, cy2 = fb->clip_y + fb->clip_h;
        if (dx < cx1) { int d = cx1 - dx; sx += d; w -= d; dx = cx1; }
        if (dy < cy1) { int d = cy1 - dy; sy += d; h -= d; dy = cy1; }
        if (dx + w > cx2) w = cx2 - dx;
        if (dy + h > cy2) h = cy2 - dy;
    }
    if (w <= 0 || h <= 0) return;
    size_t bytes = (size_t)w * sizeof(uint32_t);
    for (int y = 0; y < h; ++y) {
        memcpy(fb->backbuf + (size_t)(dy + y) * (size_t)fb->stride + (size_t)dx * sizeof(uint32_t),
               c->pixels + (size_t)(sy + y) * (size_t)c->stride + (size_t)sx * sizeof(uint32_t),
               bytes);
    }
}

static void mywin_draw_child_control_uncached(Framebuffer* fb, MyWinWindowInfo* wi, int x, int y, int w, int h)
{
    if (!fb || !wi) return;
    if (mywin_is_class(wi->hWnd, "BUTTON")) {
        mywin_draw_button_family_control(fb, wi, x, y, w, h);
    } else if (mywin_is_class(wi->hWnd, "STATIC")) {
        mywin_draw_static_control(fb, wi, x, y, w, h);
    } else if (mywin_is_class(wi->hWnd, "EDIT")) {
        mywin_draw_edit_control(fb, wi, x, y, w, h);
    } else if (mywin_is_class(wi->hWnd, "LISTBOX")) {
        mywin_draw_listbox_control(fb, wi, x, y, w, h);
    } else if (mywin_is_class(wi->hWnd, "SCROLLBAR")) {
        mywin_draw_scrollbar_control(fb, wi, x, y, w, h);
    } else if (mywin_is_class(wi->hWnd, "COMBOBOX")) {
        mywin_draw_combo_control(fb, wi, x, y, w, h);
    }
}

static void mywin_draw_child_control_cached(Framebuffer* fb, MyWinWindowInfo* wi, int x, int y, int w, int h)
{
    if (!fb || !wi || w <= 0 || h <= 0) return;
    if (!mywin_control_is_cacheable(wi)) {
        mywin_draw_child_control_uncached(fb, wi, x, y, w, h);
        return;
    }

    unsigned long long sig = mywin_control_visual_signature(wi, w, h);
    MyWinControlBackingCache* c = mywin_control_cache_slot(wi->hWnd);
    if (!c || !mywin_control_cache_ensure(c, w, h)) {
        mywin_draw_child_control_uncached(fb, wi, x, y, w, h);
        return;
    }

    if (c->sig != sig) {
        Framebuffer tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.backbuf = c->pixels;
        tmp.width = w;
        tmp.height = h;
        tmp.stride = c->stride;
        fb_reset_clip(&tmp);
        memset(c->pixels, 0, (size_t)c->stride * (size_t)c->h);
        mywin_draw_child_control_uncached(&tmp, wi, 0, 0, w, h);
        c->sig = sig;
    }
    mywin_blit_control_cache(c, fb, x, y);
}

BOOL MyDrawChildWindows(HWND hWndParent, Framebuffer* fb, int xOrigin, int yOrigin)
{
    if (!fb) return FALSE;
    HWND children[MYWIN_MAX_WINDOW_INFOS];
    int childCount = mywin_collect_children(hWndParent, children, MYWIN_MAX_WINDOW_INFOS);
    for (int zi = 0; zi < childCount; ++zi) {
        MyWinWindowInfo* wi = mywin_find_info(children[zi]);
        if (!wi || !(wi->style & WS_VISIBLE)) continue;
        int x = xOrigin + (int)wi->rcClient.left;
        int y = yOrigin + (int)wi->rcClient.top;
        int w = (int)(wi->rcClient.right - wi->rcClient.left);
        int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
        if (mywin_is_class(wi->hWnd, "#32770")) {
            fb_rect(fb, x, y, w, h, (wi->style & WS_DISABLED) ? COLOR(26,26,30) : COLOR(28,34,50));
            fb_rect_outline(fb, x, y, w, h, COLOR(180,190,230));
            fb_rect(fb, x, y, w, MYWIN_DIALOG_CAPTION_H, COLOR(54,68,96));
            DrawClipTextA(fb, x + 8, y + 7, wi->text[0] ? wi->text : "Dialog", WHITE, x + 4, y, w - 8, MYWIN_DIALOG_CAPTION_H);
            MyDrawChildWindows(wi->hWnd, fb, x, y + MYWIN_DIALOG_CAPTION_H);
        } else {
            mywin_draw_child_control_cached(fb, wi, x, y, w, h);
        }
    }

    /* v102.4/v179: dropdown popup still belongs on top of later siblings.  The
       closed COMBOBOX body may have come from the control cache above; the open
       popup is an overlay and is deliberately redrawn after all siblings. */
    for (int zi = 0; zi < childCount; ++zi) {
        MyWinWindowInfo* wi = mywin_find_info(children[zi]);
        if (!wi || !(wi->style & WS_VISIBLE)) continue;
        if (!mywin_is_class(wi->hWnd, "COMBOBOX") || !wi->control->ccDropped) continue;
        int x = xOrigin + (int)wi->rcClient.left;
        int y = yOrigin + (int)wi->rcClient.top;
        int w = (int)(wi->rcClient.right - wi->rcClient.left);
        int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
        mywin_draw_combo_dropdown_popup(fb, wi, x, y, w, h);
    }
    return TRUE;
}

static void mywin_draw_standard_scrollbar_glyph(Framebuffer* fb, int x, int y, int w, int h, BOOL vert, int minv, int maxv, int page, int pos, UINT arrows)
{
    if (!fb || w <= 0 || h <= 0) return;
    fb_rect(fb, x, y, w, h, COLOR(34,38,52));
    fb_rect_outline(fb, x, y, w, h, COLOR(100,115,150));
    BOOL disabled = (arrows == ESB_DISABLE_BOTH);
    Color txt = disabled ? COLOR(100,100,110) : WHITE;
    if (vert) {
        fb_rect(fb, x + 1, y + 1, w - 2, 14, COLOR(48,56,78));
        fb_rect(fb, x + 1, y + h - 15, w - 2, 14, COLOR(48,56,78));
        font_draw_str(fb, x + 4, y + 4, "^", txt);
        font_draw_str(fb, x + 4, y + h - 13, "v", txt);
        int trackY = y + 16, trackH = h - 32;
        if (trackH < 4) return;
        int range = maxv - minv + 1;
        if (range < 1) range = 1;
        if (page < 1) page = 1;
        int thumbH = (range > 0) ? (trackH * page) / (range + page) : trackH;
        if (thumbH < 8) thumbH = 8;
        if (thumbH > trackH) thumbH = trackH;
        int maxPos = maxv - page + 1;
        if (maxPos < minv) maxPos = minv;
        int denom = maxPos - minv;
        int ty = trackY + (denom ? ((pos - minv) * (trackH - thumbH)) / denom : 0);
        fb_rect(fb, x + 2, ty, w - 4, thumbH, disabled ? COLOR(70,70,80) : COLOR(92,108,150));
    } else {
        fb_rect(fb, x + 1, y + 1, 14, h - 2, COLOR(48,56,78));
        fb_rect(fb, x + w - 15, y + 1, 14, h - 2, COLOR(48,56,78));
        font_draw_str(fb, x + 5, y + 4, "<", txt);
        font_draw_str(fb, x + w - 12, y + 4, ">", txt);
        int trackX = x + 16, trackW = w - 32;
        if (trackW < 4) return;
        int range = maxv - minv + 1;
        if (range < 1) range = 1;
        if (page < 1) page = 1;
        int thumbW = (range > 0) ? (trackW * page) / (range + page) : trackW;
        if (thumbW < 8) thumbW = 8;
        if (thumbW > trackW) thumbW = trackW;
        int maxPos = maxv - page + 1;
        if (maxPos < minv) maxPos = minv;
        int denom = maxPos - minv;
        int tx = trackX + (denom ? ((pos - minv) * (trackW - thumbW)) / denom : 0);
        fb_rect(fb, tx, y + 2, thumbW, h - 4, disabled ? COLOR(70,70,80) : COLOR(92,108,150));
    }
}

BOOL MyDrawStandardScrollBars(HWND hWnd, Framebuffer* fb, int clientX, int clientY, int clientW, int clientH)
{
    MyWinWindowInfo* wi = mywin_find_info(hWnd);
    if (!wi || !fb || clientW <= 0 || clientH <= 0) return FALSE;
    BOOL any = FALSE;
    int sb = 16;
    if ((wi->style & WS_VSCROLL) && wi->control->stdVScrollVisible != 0) {
        mywin_draw_standard_scrollbar_glyph(fb, clientX + clientW - sb, clientY, sb, clientH - ((wi->style & WS_HSCROLL) ? sb : 0), TRUE,
                                           wi->control->ccScrollMin, wi->control->ccScrollMax, wi->control->ccScrollPage, wi->control->ccScrollPos, wi->control->stdVScrollArrows);
        any = TRUE;
    }
    if ((wi->style & WS_HSCROLL) && wi->control->stdHScrollVisible != 0) {
        mywin_draw_standard_scrollbar_glyph(fb, clientX, clientY + clientH - sb, clientW - ((wi->style & WS_VSCROLL) ? sb : 0), sb, FALSE,
                                           wi->control->stdHScrollMin, wi->control->stdHScrollMax, wi->control->stdHScrollPage, wi->control->stdHScrollPos, wi->control->stdHScrollArrows);
        any = TRUE;
    }
    if ((wi->style & WS_VSCROLL) && (wi->style & WS_HSCROLL) && wi->control->stdVScrollVisible && wi->control->stdHScrollVisible)
        fb_rect(fb, clientX + clientW - sb, clientY + clientH - sb, sb, sb, COLOR(42,46,60));
    return any;
}

BOOL MyDrawTopLevelDialogs(Framebuffer* fb)
{
    if (!fb) return FALSE;
    for (int i = 0; i < MYWIN_MAX_DIALOGS; ++i) {
        MyWinDialogInfo* di = &g_DialogInfos[i];
        if (!di->valid || !di->hDlg || di->ended) continue;
        MyWinWindowInfo* wi = mywin_find_info(di->hDlg);
        if (!wi || wi->hParent != 0 || !(wi->style & WS_VISIBLE)) continue;
        int x = (int)wi->rcClient.left;
        int y = (int)wi->rcClient.top;
        int w = (int)(wi->rcClient.right - wi->rcClient.left);
        int h = (int)(wi->rcClient.bottom - wi->rcClient.top);
        fb_rect(fb, x, y, w, h, (wi->style & WS_DISABLED) ? COLOR(26,26,30) : COLOR(28,34,50));
        fb_rect_outline(fb, x, y, w, h, COLOR(180,190,230));
        fb_rect(fb, x, y, w, MYWIN_DIALOG_CAPTION_H, COLOR(54,68,96));
        DrawClipTextA(fb, x + 8, y + 7, wi->text[0] ? wi->text : "Dialog", WHITE, x + 4, y, w - 8, MYWIN_DIALOG_CAPTION_H);
        MyDrawChildWindows(wi->hWnd, fb, x, y + MYWIN_DIALOG_CAPTION_H);
    }
    return TRUE;
}

static BOOL __attribute__((unused)) mywin_find_class_matches(const MyWinWindowInfo* wi, LPCSTR lpClassName)
{
    if (!wi || !wi->valid) return FALSE;
    if (!lpClassName) return TRUE;
    if (mywin_is_int_atom_class(lpClassName))
        return wi->classAtom == (ATOM)(uintptr_t)lpClassName ? TRUE : FALSE;
    DWORD qh = mywin_class_name_hash(lpClassName);
    if (wi->classNameHash != qh) return FALSE;
    return strcmp(wi->className, lpClassName) == 0 ? TRUE : FALSE;
}

static BOOL __attribute__((unused)) mywin_find_title_matches(const MyWinWindowInfo* wi, LPCSTR lpWindowName)
{
    if (!wi || !wi->valid) return FALSE;
    if (!lpWindowName) return TRUE;
    DWORD qh = mywin_title_hash(lpWindowName);
    if (wi->textHash != qh) return FALSE;
    return strcmp(wi->text, lpWindowName) == 0 ? TRUE : FALSE;
}

static HWND mywin_find_window_ex_local(HWND hWndParent, HWND hWndChildAfter, LPCSTR lpClassName, LPCSTR lpWindowName)
{
    return mywin_find_window_ex_best(hWndParent, hWndChildAfter, lpClassName, lpWindowName);
}

HWND FindWindowExA(HWND hWndParent, HWND hWndChildAfter, LPCSTR lpClassName, LPCSTR lpWindowName)
{
    if (!mywin_has_cap(CAP_WINDOW_ENUM)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    if (hWndParent && !IsWindow(hWndParent)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (hWndChildAfter) {
        if (!IsWindow(hWndChildAfter)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
        MyWinWindowInfo* after = mywin_find_info(hWndChildAfter);
        if (!after || after->hParent != hWndParent) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    }
    HWND ret = mywin_find_window_ex_local(hWndParent, hWndChildAfter, lpClassName, lpWindowName);
    SetLastError(ret ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND);
    return ret;
}

HWND FindWindowA(LPCSTR lpClassName, LPCSTR lpWindowName)
{
    return FindWindowExA(0, 0, lpClassName, lpWindowName);
}



typedef struct MyWinEnumThunk {
    WNDENUMPROC proc;
    LPARAM lParam;
} MyWinEnumThunk;

static int mywin_enum_window_proc(int index, const Window* win, void* userdata)
{
    (void)index;
    MyWinEnumThunk* thunk = (MyWinEnumThunk*)userdata;
    if (!thunk || !thunk->proc || !win) return 0;
    HWND hWnd = (win->app_type == APP_TERMINAL) ? (win->term ? win->term->hwnd : 0) : win->app_hwnd;
    if (!hWnd) return 1;
    return thunk->proc(hWnd, thunk->lParam) ? 1 : 0;
}

BOOL EnumWindows(WNDENUMPROC lpEnumFunc, LPARAM lParam)
{
    if (!g_lpWindowManager || !lpEnumFunc || !mywin_has_cap(CAP_WINDOW_ENUM)) return FALSE;
    MyWinEnumThunk thunk;
    thunk.proc = lpEnumFunc;
    thunk.lParam = lParam;
    wm_enum_windows(g_lpWindowManager, mywin_enum_window_proc, &thunk);
    return TRUE;
}

BOOL MySubscribeWindowMessage(HWND hWndSource, HWND hWndSubscriber, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    if (!g_lpHwndManager || !g_HasCapability || !mywin_can_subscribe(hWndSource, hWndSubscriber)) return FALSE;
    return hwnd_subscribe(g_lpHwndManager, &g_CurrentCapability, hWndSource, hWndSubscriber,
                          wMsgFilterMin, wMsgFilterMax) == 0;
}

BOOL MyUnsubscribeWindowMessage(HWND hWndSource, HWND hWndSubscriber, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    if (!g_lpHwndManager || !g_HasCapability || !mywin_can_subscribe(hWndSource, hWndSubscriber)) return FALSE;
    return hwnd_unsubscribe(g_lpHwndManager, &g_CurrentCapability, hWndSource, hWndSubscriber,
                            wMsgFilterMin, wMsgFilterMax) == 0;
}

BOOL MyGetWindowState(HWND hWnd, MyWindowState* lpState)
{
    if (!lpState || !mywin_can_read_window(hWnd)) return FALSE;

    // v17: public reads come from the shared/read-only HWND state section.
    // WindowManager remains a fallback for very early creation before first publish.
    if (g_lpHwndManager && hwnd_copy_window_state(g_lpHwndManager, hWnd, lpState))
        return TRUE;
    if (g_lpWindowManager)
        return wm_get_window_state(g_lpWindowManager, hWnd, lpState) ? TRUE : FALSE;
    return FALSE;
}

const MyWindowStateSection* MyGetWindowStateSection(void)
{
    if (!mywin_has_cap(CAP_WINDOW_READ)) return NULL;
    return g_lpHwndManager ? hwnd_get_window_state_section(g_lpHwndManager) : NULL;
}

BOOL MyEnumProcesses(MYPROCESSENUMPROC lpEnumFunc, LPARAM lParam)
{
    if (!g_lpHwndManager || !lpEnumFunc || !mywin_has_cap(CAP_PROCESS_ENUM)) return FALSE;
    return hwnd_enum_processes(g_lpHwndManager, lpEnumFunc, lParam) > 0 ? TRUE : FALSE;
}

BOOL MyGetProcessInfo(DWORD dwProcessId, MyProcessInfo* lpInfo)
{
    if (!g_lpHwndManager || !lpInfo || !mywin_has_cap(CAP_PROCESS_ENUM)) return FALSE;
    return hwnd_get_process_info(g_lpHwndManager, dwProcessId, lpInfo) ? TRUE : FALSE;
}

DWORD GetCurrentProcessId(void)
{
    return g_HasCapability ? g_CurrentCapability.id : 0;
}

DWORD GetCurrentThreadId(void)
{
    // PoC rule: one UI thread id per capability/app for now.
    return g_HasCapability ? g_CurrentCapability.id : 0;
}

BOOL PostThreadMessageA(DWORD idThread, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (!g_lpHwndManager || !g_HasCapability || !idThread) return FALSE;
    return hwnd_post_thread_message(g_lpHwndManager, &g_CurrentCapability, idThread, Msg, wParam, lParam) == 0;
}

void PostQuitMessage(int nExitCode)
{
    if (!g_lpHwndManager || !g_HasCapability) return;
    hwnd_post_thread_message(g_lpHwndManager, &g_CurrentCapability, GetCurrentThreadId(), WM_QUIT, (WPARAM)nExitCode, 0);
}

static int mywin_msgwait_queue_ready(DWORD wakeMask)
{
    if (!wakeMask || !g_lpHwndManager || !g_HasCapability) return 0;

    /* v230: queue wake masks are now real QS_* status bits.  Queued
       messages hit the per-thread QueueStatus fast path.  Synthetic timers
       are still checked through the timer synthesizer because they do not
       occupy the ring until observed. */
    if (hwnd_thread_queue_has_status(g_lpHwndManager,
                                     g_CurrentCapability.id,
                                     g_CurrentCapability.id,
                                     wakeMask))
        return 1;

    if (wakeMask & QS_TIMER) {
        MyMessage mm;
        return hwnd_get_thread_message(g_lpHwndManager,
                                       g_CurrentCapability.id,
                                       g_CurrentCapability.id,
                                       0,
                                       WM_TIMER,
                                       WM_TIMER,
                                       0,
                                       &mm);
    }
    return 0;
}

static int mywin_msgwait_wait_queue_once(DWORD wakeMask, DWORD timeoutMs)
{
    if (!wakeMask || !g_lpHwndManager || !g_HasCapability) {
        if (timeoutMs && timeoutMs != INFINITE) usleep((useconds_t)timeoutMs * 1000u);
        else if (timeoutMs == INFINITE) usleep(1000);
        return 0;
    }

    int waitMs;
    if (timeoutMs == INFINITE) waitMs = -1;
    else if (timeoutMs > 0x7fffffffu) waitMs = 0x7fffffff;
    else waitMs = (int)timeoutMs;

    MyMessage mm;
    return hwnd_get_thread_message_wait(g_lpHwndManager,
                                        g_CurrentCapability.id,
                                        g_CurrentCapability.id,
                                        0,
                                        0,
                                        0,
                                        0,
                                        waitMs,
                                        &mm);
}

static DWORD mywin_elapsed_ms(uint64_t startNs)
{
    uint64_t now = myos_now_ns();
    if (now <= startNs) return 0;
    uint64_t diff = (now - startNs) / 1000000ull;
    return diff > 0xffffffffull ? 0xffffffffu : (DWORD)diff;
}

DWORD MsgWaitForMultipleObjectsEx(DWORD nCount, const HANDLE* pHandles, DWORD dwMilliseconds, DWORD dwWakeMask, DWORD dwFlags)
{
    if (nCount > MAXIMUM_WAIT_OBJECTS || (nCount && !pHandles)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }

    if (dwFlags & ~(MWMO_WAITALL | MWMO_ALERTABLE | MWMO_INPUTAVAILABLE)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }

    /* myOS currently has no APC/alertable wait delivery.  Accepting
       MWMO_ALERTABLE is harmless for USER32 pumps and keeps the call ABI
       compatible; the flag becomes meaningful once APCs exist. */
    BOOL waitAll = (dwFlags & MWMO_WAITALL) ? TRUE : FALSE;
    uint64_t startNs = myos_now_ns();

    for (;;) {
        if (nCount) {
            DWORD hr = WaitForMultipleObjects(nCount, pHandles, waitAll, 0);
            if (hr != WAIT_TIMEOUT) return hr;
        }

        if (mywin_msgwait_queue_ready(dwWakeMask)) {
            SetLastError(ERROR_SUCCESS);
            return WAIT_OBJECT_0 + nCount;
        }

        if (dwMilliseconds == 0) {
            SetLastError(ERROR_SUCCESS);
            return WAIT_TIMEOUT;
        }

        DWORD slice = 8;
        if (dwMilliseconds != INFINITE) {
            DWORD elapsed = mywin_elapsed_ms(startNs);
            if (elapsed >= dwMilliseconds) {
                SetLastError(ERROR_SUCCESS);
                return WAIT_TIMEOUT;
            }
            DWORD remaining = dwMilliseconds - elapsed;
            if (remaining < slice) slice = remaining;
        }

        if (nCount) {
            DWORD hr = WaitForMultipleObjects(nCount, pHandles, waitAll, slice);
            if (hr != WAIT_TIMEOUT) return hr;
            if (mywin_msgwait_queue_ready(dwWakeMask)) {
                SetLastError(ERROR_SUCCESS);
                return WAIT_OBJECT_0 + nCount;
            }
        } else {
            DWORD qwait = (dwMilliseconds == INFINITE) ? INFINITE : slice;
            if (mywin_msgwait_wait_queue_once(dwWakeMask, qwait)) {
                SetLastError(ERROR_SUCCESS);
                return WAIT_OBJECT_0;
            }
        }
    }
}

DWORD MsgWaitForMultipleObjects(DWORD nCount, const HANDLE* pHandles, BOOL bWaitAll, DWORD dwMilliseconds, DWORD dwWakeMask)
{
    return MsgWaitForMultipleObjectsEx(nCount, pHandles, dwMilliseconds, dwWakeMask, bWaitAll ? MWMO_WAITALL : 0u);
}

BOOL WaitMessage(void)
{
    DWORD r = MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE, QS_ALLINPUT);
    return r == WAIT_OBJECT_0;
}

DWORD GetQueueStatus(UINT flags)
{
    if (!g_lpHwndManager || !g_HasCapability || !flags) return 0;
    return hwnd_get_thread_queue_status_bits(g_lpHwndManager,
                                             g_CurrentCapability.id,
                                             g_CurrentCapability.id,
                                             flags);
}

BOOL IsHungAppWindow(HWND hWnd)
{
    if (!g_lpHwndManager || !mywin_can_read_window(hWnd)) return FALSE;
    return hwnd_is_window_hung(g_lpHwndManager, hWnd, 750) ? TRUE : FALSE;
}

// v109: KERNEL32/WinBase public entrypoints moved to winbase.c
//   LastError, process/env/loader, named objects, wait, handles, sections

/* AUDIT(v154): Clipboard/global-memory remains session-scalar and quota-limited.
   Menus now have a small modal TrackPopupMenu/TrackPopupMenuEx tracker, but
   still use fixed tables and a non-rendered PoC surface model. */
#define MYWIN_MAX_GLOBALS 64
#define MYWIN_MAX_MENUS   32
#define MYWIN_MAX_MENU_ITEMS 32
#define MYWIN_MAX_ACCELS  16
#define MYWIN_MAX_ACCEL_ITEMS 16
#define MYWIN_LITE_HANDLE_HASH_BUCKETS 128
#define MYWIN_LITE_HANDLE_HASH_MASK (MYWIN_LITE_HANDLE_HASH_BUCKETS - 1)

typedef struct MyGlobalMem {
    int valid;
    HGLOBAL handle;
    DWORD handleHash;
    int handleHashNext;
    DWORD size;
    BYTE* data;
    int locked;
} MyGlobalMem;

typedef struct MyMenuItem {
    UINT flags;
    UINT_PTR id;
    HMENU submenu;
    ULONG_PTR itemData;
    char text[96];
} MyMenuItem;

typedef struct MyMenuLite {
    int valid;
    HMENU handle;
    DWORD handleHash;
    int handleHashNext;
    int is_popup;
    HWND owner;
    int count;
    MyMenuItem items[MYWIN_MAX_MENU_ITEMS];
} MyMenuLite;

typedef struct MyAccelLite {
    int valid;
    HACCEL handle;
    DWORD handleHash;
    int handleHashNext;
    int count;
    ACCEL items[MYWIN_MAX_ACCEL_ITEMS];
} MyAccelLite;

static pthread_mutex_t g_User32LiteLock = PTHREAD_MUTEX_INITIALIZER;
static MyGlobalMem g_GlobalMem[MYWIN_MAX_GLOBALS];
static MyMenuLite  g_Menus[MYWIN_MAX_MENUS];
static MyAccelLite g_Accels[MYWIN_MAX_ACCELS];
static int g_GlobalMemHash[MYWIN_LITE_HANDLE_HASH_BUCKETS];
static int g_MenuHash[MYWIN_LITE_HANDLE_HASH_BUCKETS];
static int g_AccelHash[MYWIN_LITE_HANDLE_HASH_BUCKETS];
static int g_MenuFreeStack[MYWIN_MAX_MENUS];
static int g_AccelFreeStack[MYWIN_MAX_ACCELS];
static int g_MenuFreeTop = 0;
static int g_AccelFreeTop = 0;
static int g_User32LiteFreeInit = 0;
static HGLOBAL g_NextGlobal = 0xa0000001u;
static HMENU   g_NextMenu   = 0xa1000001u;
static HACCEL  g_NextAccel  = 0xa2000001u;
static HWND    g_ClipboardOpenOwner = 0;
static HWND    g_ClipboardOwner = 0;
static UINT    g_ClipboardFormat = 0;
static HGLOBAL g_ClipboardData = 0;

static inline DWORD mywin_lite_handle_hash(uintptr_t h)
{
    uint32_t x = (uint32_t)h;
    x *= 2654435761u;
    x ^= x >> 16;
    return x ? x : 1u;
}

static inline int mywin_lite_handle_bucket(DWORD hash)
{
    return (int)(hash & MYWIN_LITE_HANDLE_HASH_MASK);
}

static void mywin_lite_freestack_init(void)
{
    if (g_User32LiteFreeInit) return;
    g_MenuFreeTop = 0;
    for (int i = MYWIN_MAX_MENUS - 1; i >= 0; --i) g_MenuFreeStack[g_MenuFreeTop++] = i;
    g_AccelFreeTop = 0;
    for (int i = MYWIN_MAX_ACCELS - 1; i >= 0; --i) g_AccelFreeStack[g_AccelFreeTop++] = i;
    g_User32LiteFreeInit = 1;
}

static int mywin_lite_pop_free(int* stack, int* top)
{
    mywin_lite_freestack_init();
    if (!stack || !top || *top <= 0) return -1;
    return stack[--(*top)];
}

static void mywin_lite_push_free(int* stack, int* top, int max, int idx)
{
    mywin_lite_freestack_init();
    if (!stack || !top || idx < 0 || idx >= max || *top >= max) return;
    stack[(*top)++] = idx;
}

static void mywin_global_hash_insert(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_GLOBALS || !g_GlobalMem[idx].valid || !g_GlobalMem[idx].handle) return;
    DWORD h = mywin_lite_handle_hash((uintptr_t)g_GlobalMem[idx].handle);
    int b = mywin_lite_handle_bucket(h);
    g_GlobalMem[idx].handleHash = h;
    g_GlobalMem[idx].handleHashNext = g_GlobalMemHash[b];
    g_GlobalMemHash[b] = idx + 1;
}

static void mywin_global_hash_remove(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_GLOBALS || !g_GlobalMem[idx].handleHash) return;
    int b = mywin_lite_handle_bucket(g_GlobalMem[idx].handleHash);
    int* link = &g_GlobalMemHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) {
            *link = g_GlobalMem[cur].handleHashNext;
            g_GlobalMem[cur].handleHashNext = 0;
            g_GlobalMem[cur].handleHash = 0;
            return;
        }
        if (cur < 0 || cur >= MYWIN_MAX_GLOBALS) break;
        link = &g_GlobalMem[cur].handleHashNext;
    }
}

static void mywin_menu_hash_insert(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_MENUS || !g_Menus[idx].valid || !g_Menus[idx].handle) return;
    DWORD h = mywin_lite_handle_hash((uintptr_t)g_Menus[idx].handle);
    int b = mywin_lite_handle_bucket(h);
    g_Menus[idx].handleHash = h;
    g_Menus[idx].handleHashNext = g_MenuHash[b];
    g_MenuHash[b] = idx + 1;
}

static void mywin_menu_hash_remove(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_MENUS || !g_Menus[idx].handleHash) return;
    int b = mywin_lite_handle_bucket(g_Menus[idx].handleHash);
    int* link = &g_MenuHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) {
            *link = g_Menus[cur].handleHashNext;
            g_Menus[cur].handleHashNext = 0;
            g_Menus[cur].handleHash = 0;
            return;
        }
        if (cur < 0 || cur >= MYWIN_MAX_MENUS) break;
        link = &g_Menus[cur].handleHashNext;
    }
}

static void mywin_accel_hash_insert(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_ACCELS || !g_Accels[idx].valid || !g_Accels[idx].handle) return;
    DWORD h = mywin_lite_handle_hash((uintptr_t)g_Accels[idx].handle);
    int b = mywin_lite_handle_bucket(h);
    g_Accels[idx].handleHash = h;
    g_Accels[idx].handleHashNext = g_AccelHash[b];
    g_AccelHash[b] = idx + 1;
}

static void mywin_accel_hash_remove(int idx)
{
    if (idx < 0 || idx >= MYWIN_MAX_ACCELS || !g_Accels[idx].handleHash) return;
    int b = mywin_lite_handle_bucket(g_Accels[idx].handleHash);
    int* link = &g_AccelHash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) {
            *link = g_Accels[cur].handleHashNext;
            g_Accels[cur].handleHashNext = 0;
            g_Accels[cur].handleHash = 0;
            return;
        }
        if (cur < 0 || cur >= MYWIN_MAX_ACCELS) break;
        link = &g_Accels[cur].handleHashNext;
    }
}

static MyGlobalMem* mywin_find_global(HGLOBAL h)
{
    if (!h) return NULL;
    DWORD hv = mywin_lite_handle_hash((uintptr_t)h);
    for (int link = g_GlobalMemHash[mywin_lite_handle_bucket(hv)]; link; ) {
        int idx = link - 1;
        if (idx < 0 || idx >= MYWIN_MAX_GLOBALS) break;
        MyGlobalMem* g = &g_GlobalMem[idx];
        if (MYOS_LIKELY(g->valid && g->handle == h)) return g;
        link = g->handleHashNext;
    }
    for (int i = 0; i < MYWIN_MAX_GLOBALS; i++)
        if (g_GlobalMem[i].valid && g_GlobalMem[i].handle == h) return &g_GlobalMem[i];
    return NULL;
}

HGLOBAL GlobalAlloc(UINT uFlags, DWORD dwBytes)
{
    (void)uFlags;
    if (dwBytes == 0) dwBytes = 1;
    pthread_mutex_lock(&g_User32LiteLock);
    for (int i = 0; i < MYWIN_MAX_GLOBALS; i++) {
        if (!g_GlobalMem[i].valid) {
            BYTE* p = (BYTE*)calloc(1, dwBytes);
            if (!p) { pthread_mutex_unlock(&g_User32LiteLock); return 0; }
            g_GlobalMem[i].valid = 1;
            g_GlobalMem[i].handle = g_NextGlobal++;
            g_GlobalMem[i].size = dwBytes;
            g_GlobalMem[i].data = p;
            g_GlobalMem[i].locked = 0;
            mywin_global_hash_insert(i);
            HGLOBAL h = g_GlobalMem[i].handle;
            pthread_mutex_unlock(&g_User32LiteLock);
            return h;
        }
    }
    pthread_mutex_unlock(&g_User32LiteLock);
    return 0;
}

LPVOID GlobalLock(HGLOBAL hMem)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyGlobalMem* g = mywin_find_global(hMem);
    if (!g) { pthread_mutex_unlock(&g_User32LiteLock); return NULL; }
    g->locked++;
    LPVOID p = g->data;
    pthread_mutex_unlock(&g_User32LiteLock);
    return p;
}

BOOL GlobalUnlock(HGLOBAL hMem)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyGlobalMem* g = mywin_find_global(hMem);
    if (!g) { pthread_mutex_unlock(&g_User32LiteLock); return FALSE; }
    if (g->locked > 0) g->locked--;
    pthread_mutex_unlock(&g_User32LiteLock);
    return TRUE;
}

HGLOBAL GlobalFree(HGLOBAL hMem)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyGlobalMem* g = mywin_find_global(hMem);
    if (!g) { pthread_mutex_unlock(&g_User32LiteLock); return hMem; }
    if (g_ClipboardData == hMem) { g_ClipboardData = 0; g_ClipboardFormat = 0; }
    mywin_global_hash_remove((int)(g - g_GlobalMem));
    free(g->data);
    memset(g, 0, sizeof(*g));
    pthread_mutex_unlock(&g_User32LiteLock);
    return 0;
}

BOOL OpenClipboard(HWND hWndNewOwner)
{
    pthread_mutex_lock(&g_User32LiteLock);
    if (g_ClipboardOpenOwner && g_ClipboardOpenOwner != hWndNewOwner) {
        pthread_mutex_unlock(&g_User32LiteLock);
        return FALSE;
    }
    g_ClipboardOpenOwner = hWndNewOwner ? hWndNewOwner : (HWND)0xffffffffu;
    pthread_mutex_unlock(&g_User32LiteLock);
    return TRUE;
}

BOOL CloseClipboard(void)
{
    pthread_mutex_lock(&g_User32LiteLock);
    g_ClipboardOpenOwner = 0;
    pthread_mutex_unlock(&g_User32LiteLock);
    return TRUE;
}

BOOL EmptyClipboard(void)
{
    pthread_mutex_lock(&g_User32LiteLock);
    if (!g_ClipboardOpenOwner) { pthread_mutex_unlock(&g_User32LiteLock); return FALSE; }
    if (g_ClipboardData) {
        MyGlobalMem* g = mywin_find_global(g_ClipboardData);
        if (g) { mywin_global_hash_remove((int)(g - g_GlobalMem)); free(g->data); memset(g, 0, sizeof(*g)); }
    }
    g_ClipboardData = 0;
    g_ClipboardFormat = 0;
    g_ClipboardOwner = g_ClipboardOpenOwner;
    pthread_mutex_unlock(&g_User32LiteLock);
    return TRUE;
}

BOOL IsClipboardFormatAvailable(UINT format)
{
    pthread_mutex_lock(&g_User32LiteLock);
    BOOL ok = (g_ClipboardData && g_ClipboardFormat == format) ? TRUE : FALSE;
    pthread_mutex_unlock(&g_User32LiteLock);
    return ok;
}

HGLOBAL SetClipboardData(UINT uFormat, HGLOBAL hMem)
{
    pthread_mutex_lock(&g_User32LiteLock);
    if (!g_ClipboardOpenOwner || !mywin_find_global(hMem)) {
        pthread_mutex_unlock(&g_User32LiteLock);
        return 0;
    }
    if (g_ClipboardData && g_ClipboardData != hMem) {
        MyGlobalMem* old = mywin_find_global(g_ClipboardData);
        if (old) { mywin_global_hash_remove((int)(old - g_GlobalMem)); free(old->data); memset(old, 0, sizeof(*old)); }
    }
    g_ClipboardFormat = uFormat;
    g_ClipboardData = hMem;
    g_ClipboardOwner = g_ClipboardOpenOwner;
    pthread_mutex_unlock(&g_User32LiteLock);
    return hMem;
}

HGLOBAL GetClipboardData(UINT uFormat)
{
    pthread_mutex_lock(&g_User32LiteLock);
    HGLOBAL h = (g_ClipboardData && g_ClipboardFormat == uFormat) ? g_ClipboardData : 0;
    pthread_mutex_unlock(&g_User32LiteLock);
    return h;
}

static MyMenuLite* mywin_find_menu(HMENU h)
{
    if (!h) return NULL;
    DWORD hv = mywin_lite_handle_hash((uintptr_t)h);
    for (int link = g_MenuHash[mywin_lite_handle_bucket(hv)]; link; ) {
        int idx = link - 1;
        if (idx < 0 || idx >= MYWIN_MAX_MENUS) break;
        MyMenuLite* m = &g_Menus[idx];
        if (MYOS_LIKELY(m->valid && m->handle == h)) return m;
        link = m->handleHashNext;
    }
    for (int i = 0; i < MYWIN_MAX_MENUS; i++)
        if (g_Menus[i].valid && g_Menus[i].handle == h) return &g_Menus[i];
    return NULL;
}

static HMENU mywin_create_menu(int popup)
{
    pthread_mutex_lock(&g_User32LiteLock);
    int i = mywin_lite_pop_free(g_MenuFreeStack, &g_MenuFreeTop);
    if (i < 0) { pthread_mutex_unlock(&g_User32LiteLock); return 0; }
    memset(&g_Menus[i], 0, sizeof(g_Menus[i]));
    g_Menus[i].valid = 1;
    g_Menus[i].handle = g_NextMenu++;
    g_Menus[i].is_popup = popup;
    mywin_menu_hash_insert(i);
    HMENU h = g_Menus[i].handle;
    pthread_mutex_unlock(&g_User32LiteLock);
    return h;
}

HMENU CreateMenu(void) { return mywin_create_menu(0); }
HMENU CreatePopupMenu(void) { return mywin_create_menu(1); }

static int mywin_menu_find_index(MyMenuLite* m, UINT uPosition, UINT uFlags)
{
    if (!m) return -1;
    if (uFlags & MF_BYPOSITION) {
        return (uPosition < (UINT)m->count) ? (int)uPosition : -1;
    }
    for (int i = 0; i < m->count; ++i) {
        if ((UINT)m->items[i].id == uPosition) return i;
    }
    return -1;
}

static void mywin_menu_fill_item(MyMenuItem* it, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem)
{
    if (!it) return;
    memset(it, 0, sizeof(*it));
    it->flags = uFlags;
    if (uFlags & MF_POPUP) {
        it->submenu = (HMENU)uIDNewItem;
        it->id = (UINT_PTR)-1;
    } else {
        it->id = uIDNewItem;
        it->submenu = 0;
    }
    it->itemData = (ULONG_PTR)(uintptr_t)lpNewItem;
    if (lpNewItem && !(uFlags & MF_OWNERDRAW)) snprintf(it->text, sizeof(it->text), "%s", lpNewItem);
    else if (uFlags & MF_OWNERDRAW) snprintf(it->text, sizeof(it->text), "<owner-draw>");
    else it->text[0] = 0;
}

BOOL InsertMenuA(HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    if (!m || m->count >= MYWIN_MAX_MENU_ITEMS) { pthread_mutex_unlock(&g_User32LiteLock); return FALSE; }

    int pos;
    if (uFlags & MF_BYPOSITION) pos = (uPosition > (UINT)m->count) ? m->count : (int)uPosition;
    else {
        pos = mywin_menu_find_index(m, uPosition, uFlags);
        if (pos < 0) pos = m->count;
    }
    if (pos < 0) pos = 0;
    if (pos > m->count) pos = m->count;
    for (int i = m->count; i > pos; --i) m->items[i] = m->items[i - 1];
    mywin_menu_fill_item(&m->items[pos], uFlags, uIDNewItem, lpNewItem);
    m->count++;
    pthread_mutex_unlock(&g_User32LiteLock);
    return TRUE;
}

BOOL AppendMenuA(HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem)
{
    return InsertMenuA(hMenu, (UINT)-1, MF_BYPOSITION | uFlags, uIDNewItem, lpNewItem);
}

BOOL ModifyMenuA(HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCSTR lpNewItem)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    int idx = mywin_menu_find_index(m, uPosition, uFlags);
    if (!m || idx < 0) { pthread_mutex_unlock(&g_User32LiteLock); return FALSE; }
    mywin_menu_fill_item(&m->items[idx], uFlags, uIDNewItem, lpNewItem);
    pthread_mutex_unlock(&g_User32LiteLock);
    return TRUE;
}

BOOL RemoveMenu(HMENU hMenu, UINT uPosition, UINT uFlags)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    int idx = mywin_menu_find_index(m, uPosition, uFlags);
    if (!m || idx < 0) { pthread_mutex_unlock(&g_User32LiteLock); return FALSE; }
    for (int i = idx; i < m->count - 1; ++i) m->items[i] = m->items[i + 1];
    memset(&m->items[m->count - 1], 0, sizeof(m->items[m->count - 1]));
    m->count--;
    pthread_mutex_unlock(&g_User32LiteLock);
    return TRUE;
}

BOOL DeleteMenu(HMENU hMenu, UINT uPosition, UINT uFlags)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    int idx = mywin_menu_find_index(m, uPosition, uFlags);
    HMENU sub = (m && idx >= 0) ? m->items[idx].submenu : 0;
    pthread_mutex_unlock(&g_User32LiteLock);
    if (!m || idx < 0) return FALSE;
    if (sub) DestroyMenu(sub);
    return RemoveMenu(hMenu, uPosition, uFlags);
}

UINT CheckMenuItem(HMENU hMenu, UINT uIDCheckItem, UINT uCheck)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    int idx = mywin_menu_find_index(m, uIDCheckItem, uCheck);
    if (!m || idx < 0) { pthread_mutex_unlock(&g_User32LiteLock); return (UINT)-1; }
    UINT old = (m->items[idx].flags & MF_CHECKED) ? MF_CHECKED : MF_UNCHECKED;
    m->items[idx].flags &= ~MF_CHECKED;
    m->items[idx].flags |= (uCheck & MF_CHECKED);
    pthread_mutex_unlock(&g_User32LiteLock);
    return old;
}

UINT EnableMenuItem(HMENU hMenu, UINT uIDEnableItem, UINT uEnable)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    int idx = mywin_menu_find_index(m, uIDEnableItem, uEnable);
    if (!m || idx < 0) { pthread_mutex_unlock(&g_User32LiteLock); return (UINT)-1; }
    UINT old = m->items[idx].flags & (MF_DISABLED | MF_GRAYED);
    m->items[idx].flags &= ~(MF_DISABLED | MF_GRAYED);
    m->items[idx].flags |= (uEnable & (MF_DISABLED | MF_GRAYED));
    pthread_mutex_unlock(&g_User32LiteLock);
    return old;
}

BOOL SetMenu(HWND hWnd, HMENU hMenu)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    if (!m) { pthread_mutex_unlock(&g_User32LiteLock); return FALSE; }
    for (int i = 0; i < MYWIN_MAX_MENUS; ++i) if (g_Menus[i].valid && g_Menus[i].owner == hWnd) g_Menus[i].owner = 0;
    m->owner = hWnd;
    pthread_mutex_unlock(&g_User32LiteLock);
    if (hWnd) InvalidateRect(hWnd, NULL, TRUE);
    return TRUE;
}

HMENU GetMenu(HWND hWnd)
{
    pthread_mutex_lock(&g_User32LiteLock);
    HMENU h = 0;
    for (int i = 0; i < MYWIN_MAX_MENUS; ++i) {
        if (g_Menus[i].valid && !g_Menus[i].is_popup && g_Menus[i].owner == hWnd) { h = g_Menus[i].handle; break; }
    }
    pthread_mutex_unlock(&g_User32LiteLock);
    return h;
}

BOOL DrawMenuBar(HWND hWnd)
{
    if (!hWnd) return FALSE;
    return InvalidateRect(hWnd, NULL, TRUE);
}

BOOL DestroyMenu(HMENU hMenu)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    if (!m) { pthread_mutex_unlock(&g_User32LiteLock); return FALSE; }
    HMENU subs[MYWIN_MAX_MENU_ITEMS];
    int nsubs = 0;
    for (int i = 0; i < m->count && nsubs < MYWIN_MAX_MENU_ITEMS; ++i) if (m->items[i].submenu) subs[nsubs++] = m->items[i].submenu;
    int mIdx = (int)(m - g_Menus);
    mywin_menu_hash_remove(mIdx);
    memset(m, 0, sizeof(*m));
    mywin_lite_push_free(g_MenuFreeStack, &g_MenuFreeTop, MYWIN_MAX_MENUS, mIdx);
    pthread_mutex_unlock(&g_User32LiteLock);
    for (int i = 0; i < nsubs; ++i) DestroyMenu(subs[i]);
    return TRUE;
}

HMENU GetSubMenu(HMENU hMenu, int nPos)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    HMENU sub = (m && nPos >= 0 && nPos < m->count) ? m->items[nPos].submenu : 0;
    pthread_mutex_unlock(&g_User32LiteLock);
    return sub;
}

int GetMenuItemCount(HMENU hMenu)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    int c = m ? m->count : -1;
    pthread_mutex_unlock(&g_User32LiteLock);
    return c;
}

UINT GetMenuItemID(HMENU hMenu, int nPos)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    UINT id = (UINT)-1;
    if (m && nPos >= 0 && nPos < m->count) {
        if (m->items[nPos].flags & MF_POPUP) id = (UINT)-1;
        else id = (UINT)m->items[nPos].id;
    }
    pthread_mutex_unlock(&g_User32LiteLock);
    return id;
}

BOOL GetMenuItemInfoA(HMENU hMenu, UINT item, BOOL fByPosition, LPMENUITEMINFOA lpmii)
{
    if (!lpmii || lpmii->cbSize < sizeof(MENUITEMINFOA)) return FALSE;

    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    int idx = -1;
    if (m) {
        if (fByPosition) idx = (item < (UINT)m->count) ? (int)item : -1;
        else idx = mywin_menu_find_index(m, item, MF_BYCOMMAND);
    }
    if (!m || idx < 0) { pthread_mutex_unlock(&g_User32LiteLock); return FALSE; }

    MyMenuItem* it = &m->items[idx];
    UINT mask = lpmii->fMask;
    if (mask & (MIIM_FTYPE | MIIM_TYPE)) {
        lpmii->fType = it->flags & (MF_STRING | MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR);
        if (!(lpmii->fType & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) lpmii->fType |= MFT_STRING;
    }
    if (mask & MIIM_STATE) {
        lpmii->fState = it->flags & (MF_CHECKED | MF_GRAYED | MF_DISABLED);
        if (!(lpmii->fState & (MF_GRAYED | MF_DISABLED))) lpmii->fState |= MFS_ENABLED;
    }
    if (mask & MIIM_ID) lpmii->wID = (it->flags & MF_POPUP) ? (UINT)-1 : (UINT)it->id;
    if (mask & MIIM_SUBMENU) lpmii->hSubMenu = it->submenu;
    if (mask & MIIM_DATA) lpmii->dwItemData = it->itemData;
    if (mask & MIIM_STRING) {
        UINT len = (UINT)strlen(it->text);
        if (lpmii->dwTypeData && lpmii->cch) {
            UINT n = (len < lpmii->cch - 1u) ? len : lpmii->cch - 1u;
            memcpy(lpmii->dwTypeData, it->text, n);
            lpmii->dwTypeData[n] = 0;
        }
        lpmii->cch = len;
    }

    pthread_mutex_unlock(&g_User32LiteLock);
    return TRUE;
}

#define MYWIN_MAX_TRACK_MENU_ITEMS 64
#define MYWIN_MENU_ITEM_HEIGHT 18

typedef struct MyMenuTrackItem {
    HMENU menu;
    HMENU parentMenu;
    int   pos;
    int   depth;
    UINT  flags;
    UINT_PTR id;
} MyMenuTrackItem;

typedef struct MyMenuModalState {
    int active;
    HWND owner;
    HMENU root;
    HMENU selectedMenu;
    int selectedPos;
    UINT selectedFlags;
    UINT_PTR selectedId;
    UINT flags;
    int x;
    int y;
} MyMenuModalState;

static MyMenuModalState g_MenuModalState;

static int mywin_menu_item_is_invokable(const MyMenuItem* it)
{
    if (!it) return 0;
    if (it->flags & MF_SEPARATOR) return 0;
    if (it->flags & (MF_DISABLED | MF_GRAYED)) return 0;
    if ((it->flags & MF_POPUP) && it->submenu) return 0;
    return it->id != 0;
}

static int mywin_menu_flatten_locked(MyMenuLite* m, int depth, MyMenuTrackItem* out, int max)
{
    int n = 0;
    if (!m || !out || max <= 0 || depth > 8) return 0;
    for (int i = 0; i < m->count && n < max; ++i) {
        MyMenuItem* it = &m->items[i];
        if (it->flags & MF_SEPARATOR) continue;
        if (it->flags & (MF_DISABLED | MF_GRAYED)) continue;
        if ((it->flags & MF_POPUP) && it->submenu) {
            MyMenuLite* sub = mywin_find_menu(it->submenu);
            n += mywin_menu_flatten_locked(sub, depth + 1, out + n, max - n);
            continue;
        }
        if (mywin_menu_item_is_invokable(it)) {
            out[n].menu = m->handle;
            out[n].parentMenu = m->handle;
            out[n].pos = i;
            out[n].depth = depth;
            out[n].flags = it->flags;
            out[n].id = it->id;
            n++;
        }
    }
    return n;
}

static int mywin_menu_collect_items(HMENU hMenu, MyMenuTrackItem* out, int max)
{
    int n = 0;
    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    if (m) n = mywin_menu_flatten_locked(m, 0, out, max);
    pthread_mutex_unlock(&g_User32LiteLock);
    return n;
}

static void mywin_menu_owner_draw_probe(HWND hWnd, HMENU hMenu, UINT_PTR cmd, UINT flags, int pos)
{
    if (!hWnd || !(flags & MF_OWNERDRAW)) return;
    MEASUREITEMSTRUCT mi;
    memset(&mi, 0, sizeof(mi));
    mi.CtlType = ODT_MENU;
    mi.CtlID = 0;
    mi.itemID = (UINT)cmd;
    mi.itemWidth = 160;
    mi.itemHeight = 18;
    mi.itemData = (ULONG_PTR)cmd;
    SendMessageA(hWnd, WM_MEASUREITEM, 0, (LPARAM)&mi);

    DRAWITEMSTRUCT di;
    memset(&di, 0, sizeof(di));
    di.CtlType = ODT_MENU;
    di.CtlID = 0;
    di.itemID = (UINT)cmd;
    di.itemAction = ODA_DRAWENTIRE;
    di.itemState = 0;
    di.hwndItem = (HWND)hMenu;
    di.hDC = 0;
    di.rcItem.left = 0;
    di.rcItem.top = pos * (int)(mi.itemHeight ? mi.itemHeight : 18);
    di.rcItem.right = (int)(mi.itemWidth ? mi.itemWidth : 160);
    di.rcItem.bottom = di.rcItem.top + (int)(mi.itemHeight ? mi.itemHeight : 18);
    di.itemData = (ULONG_PTR)cmd;
    SendMessageA(hWnd, WM_DRAWITEM, 0, (LPARAM)&di);
}

static void mywin_menu_select_notify(HWND hWnd, const MyMenuTrackItem* it)
{
    if (!hWnd || !it) return;
    SendMessageA(hWnd, WM_MENUSELECT,
                 MAKEWPARAM((WORD)it->id, (WORD)(it->flags & 0xffffu)),
                 (LPARAM)it->menu);
}

#define MYWIN_MENU_MODAL_IDLE_CANCEL_MS 60u
#define MYWIN_MENU_MODAL_WIDTH          180

static int mywin_menu_index_from_point(const MyMenuTrackItem* items, int count, int x, int y, const MSG* msg)
{
    if (!items || count <= 0 || !msg) return -1;
    int px = (short)LOWORD((DWORD_PTR)msg->lParam);
    int py = (short)HIWORD((DWORD_PTR)msg->lParam);
    if (px < x || px >= x + MYWIN_MENU_MODAL_WIDTH) return -1;
    int row = (py - y) / MYWIN_MENU_ITEM_HEIGHT;
    if (row < 0 || row >= count) return -1;
    return row;
}

static int mywin_track_popup_pump(HWND hWnd, UINT uFlags, int x, int y,
                                  MyMenuTrackItem* items, int count, int* selected,
                                  UINT_PTR* outCmd, int* outCancel)
{
    MSG msg;
    if (!selected || !outCmd || !outCancel || !items || count <= 0) return 0;
    *outCmd = 0;
    *outCancel = 0;

    uint64_t startNs = myos_now_ns();
    for (;;) {
        DWORD elapsed = mywin_elapsed_ms(startNs);
        if (elapsed >= MYWIN_MENU_MODAL_IDLE_CANCEL_MS) {
            *outCancel = 1;
            return 1;
        }

        DWORD waitMs = MYWIN_MENU_MODAL_IDLE_CANCEL_MS - elapsed;
        DWORD wr = MsgWaitForMultipleObjects(0, NULL, FALSE, waitMs, QS_ALLINPUT);
        if (wr == WAIT_TIMEOUT) {
            *outCancel = 1;
            return 1;
        }
        if (wr == WAIT_FAILED) {
            *outCancel = 1;
            return 1;
        }

        int drained = 0;
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            drained = 1;
            int commit = 0;
            if (msg.message == WM_CANCELMODE) {
                *outCancel = 1;
                return 1;
            }
            if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
                switch ((int)msg.wParam) {
                case KEY_ESC:
                    *outCancel = 1;
                    return 1;
                case KEY_DOWN:
                    *selected = (*selected + 1) % count;
                    mywin_menu_select_notify(hWnd, &items[*selected]);
                    break;
                case KEY_UP:
                    *selected = (*selected + count - 1) % count;
                    mywin_menu_select_notify(hWnd, &items[*selected]);
                    break;
                case KEY_ENTER:
                case KEY_SPACE:
                    commit = 1;
                    break;
                default:
                    break;
                }
            } else if (msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONUP ||
                       msg.message == WM_RBUTTONDOWN || msg.message == WM_RBUTTONUP) {
                int right = (msg.message == WM_RBUTTONDOWN || msg.message == WM_RBUTTONUP);
                int buttonAllowed = right ? ((uFlags & TPM_RIGHTBUTTON) != 0) : TRUE;
                if (!buttonAllowed) continue;
                int idx = mywin_menu_index_from_point(items, count, x, y, &msg);
                if (idx < 0) {
                    *outCancel = 1;
                    return 1;
                }
                *selected = idx;
                mywin_menu_select_notify(hWnd, &items[*selected]);
                if (msg.message == WM_LBUTTONUP || msg.message == WM_RBUTTONUP) commit = 1;
            } else {
                DispatchMessageA(&msg);
            }

            if (commit) {
                *outCmd = items[*selected].id;
                return 1;
            }
        }
        if (!drained) {
            *outCancel = 1;
            return 1;
        }
    }
}

HMENU GetSystemMenu(HWND hWnd, BOOL bRevert)
{
    (void)hWnd; (void)bRevert;
    HMENU h = CreatePopupMenu();
    if (!h) return 0;
    AppendMenuA(h, MF_STRING, SC_RESTORE,  "Restore");
    AppendMenuA(h, MF_STRING, SC_MOVE,     "Move");
    AppendMenuA(h, MF_STRING, SC_SIZE,     "Size");
    AppendMenuA(h, MF_STRING, SC_MINIMIZE, "Minimize");
    AppendMenuA(h, MF_STRING, SC_MAXIMIZE, "Maximize");
    AppendMenuA(h, MF_SEPARATOR, 0, NULL);
    AppendMenuA(h, MF_STRING, SC_CLOSE,    "Close");
    return h;
}

static BOOL mywin_track_popup_menu_core(HMENU hMenu, UINT uFlags, int x, int y,
                                        HWND hWnd, const TPMPARAMS* lptpm)
{
    (void)lptpm;
    MyMenuTrackItem items[MYWIN_MAX_TRACK_MENU_ITEMS];
    memset(items, 0, sizeof(items));

    pthread_mutex_lock(&g_User32LiteLock);
    MyMenuLite* m = mywin_find_menu(hMenu);
    int validMenu = (m != NULL);
    pthread_mutex_unlock(&g_User32LiteLock);
    if (!validMenu) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

    BOOL hasOwner = (hWnd && IsWindow(hWnd)) ? TRUE : FALSE;
    HWND oldCapture = GetCapture();
    HWND oldFocus = GetFocus();
    int captureChanged = 0;
    BOOL result = FALSE;
    UINT_PTR cmd = 0;
    int cancel = 0;

    if (hasOwner) {
        if (oldCapture && oldCapture != hWnd && IsWindow(oldCapture))
            SendMessageA(oldCapture, WM_CANCELMODE, 0, 0);
        SendMessageA(hWnd, WM_ENTERMENULOOP, TRUE, 0);
        if (GetCapture() != hWnd) {
            SetCapture(hWnd);
            captureChanged = 1;
        }
        SendMessageA(hWnd, WM_INITMENU, (WPARAM)hMenu, 0);
        SendMessageA(hWnd, WM_INITMENUPOPUP, (WPARAM)hMenu, MAKELPARAM((WORD)0, FALSE));
    }

    /* v154: collect after WM_INITMENUPOPUP so owner code can enable/disable or
       rewrite popup contents before the modal tracker chooses an item. */
    int count = mywin_menu_collect_items(hMenu, items, MYWIN_MAX_TRACK_MENU_ITEMS);
    int selected = (count > 0) ? 0 : -1;
    if (count > 0 && hasOwner) {
        if (items[selected].menu && items[selected].menu != hMenu)
            SendMessageA(hWnd, WM_INITMENUPOPUP, (WPARAM)items[selected].menu,
                         MAKELPARAM((WORD)items[selected].pos, FALSE));
        mywin_menu_select_notify(hWnd, &items[selected]);
    }

    memset(&g_MenuModalState, 0, sizeof(g_MenuModalState));
    g_MenuModalState.active = 1;
    g_MenuModalState.owner = hWnd;
    g_MenuModalState.root = hMenu;
    g_MenuModalState.flags = uFlags;
    g_MenuModalState.x = x;
    g_MenuModalState.y = y;

    if (count > 0) {
        if (!mywin_track_popup_pump(hasOwner ? hWnd : 0, uFlags, x, y,
                                    items, count, &selected, &cmd, &cancel))
            cancel = 1;
    } else {
        cancel = 1;
    }

    if (cmd && selected >= 0 && selected < count) {
        g_MenuModalState.selectedMenu = items[selected].menu;
        g_MenuModalState.selectedPos = items[selected].pos;
        g_MenuModalState.selectedFlags = items[selected].flags;
        g_MenuModalState.selectedId = cmd;
        if (hasOwner) mywin_menu_owner_draw_probe(hWnd, items[selected].menu, cmd, items[selected].flags, items[selected].pos);
        if (uFlags & TPM_RETURNCMD) result = (BOOL)cmd;
        else {
            if (hasOwner && !(uFlags & TPM_NONOTIFY))
                SendMessageA(hWnd, WM_COMMAND, (WPARAM)cmd, 0);
            result = TRUE;
        }
    } else {
        result = FALSE;
    }

    if (hasOwner) {
        SendMessageA(hWnd, WM_MENUSELECT, MAKEWPARAM(0, 0xffffu), 0);
        if (selected >= 0 && selected < count && items[selected].menu && items[selected].menu != hMenu)
            SendMessageA(hWnd, WM_UNINITMENUPOPUP, (WPARAM)items[selected].menu, 0);
        SendMessageA(hWnd, WM_UNINITMENUPOPUP, (WPARAM)hMenu, 0);

        if (captureChanged) {
            if (oldCapture && IsWindow(oldCapture)) SetCapture(oldCapture);
            else if (GetCapture() == hWnd) ReleaseCapture();
        }
        if (oldFocus && IsWindow(oldFocus) && GetFocus() != oldFocus) SetFocus(oldFocus);
        SendMessageA(hWnd, WM_EXITMENULOOP, TRUE, 0);
    }

    memset(&g_MenuModalState, 0, sizeof(g_MenuModalState));
    if (!cmd) SetLastError(ERROR_CANCELLED);
    else SetLastError(ERROR_SUCCESS);
    return result;
}

BOOL TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd, const void* prcRect)
{
    if (nReserved != 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return mywin_track_popup_menu_core(hMenu, uFlags, x, y, hWnd, (const TPMPARAMS*)prcRect);
}

BOOL TrackPopupMenuEx(HMENU hMenu, UINT uFlags, int x, int y, HWND hWnd, const TPMPARAMS* lptpm)
{
    if (lptpm && lptpm->cbSize < sizeof(TPMPARAMS)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return mywin_track_popup_menu_core(hMenu, uFlags, x, y, hWnd, lptpm);
}

static MyAccelLite* mywin_find_accel(HACCEL h)
{
    if (!h) return NULL;
    DWORD hv = mywin_lite_handle_hash((uintptr_t)h);
    for (int link = g_AccelHash[mywin_lite_handle_bucket(hv)]; link; ) {
        int idx = link - 1;
        if (idx < 0 || idx >= MYWIN_MAX_ACCELS) break;
        MyAccelLite* a = &g_Accels[idx];
        if (MYOS_LIKELY(a->valid && a->handle == h)) return a;
        link = a->handleHashNext;
    }
    for (int i = 0; i < MYWIN_MAX_ACCELS; i++)
        if (g_Accels[i].valid && g_Accels[i].handle == h) return &g_Accels[i];
    return NULL;
}

HACCEL CreateAcceleratorTableA(LPACCEL lpaccl, int cEntries)
{
    if (!lpaccl || cEntries <= 0) return 0;
    if (cEntries > MYWIN_MAX_ACCEL_ITEMS) cEntries = MYWIN_MAX_ACCEL_ITEMS;
    pthread_mutex_lock(&g_User32LiteLock);
    int i = mywin_lite_pop_free(g_AccelFreeStack, &g_AccelFreeTop);
    if (i < 0) { pthread_mutex_unlock(&g_User32LiteLock); return 0; }
    memset(&g_Accels[i], 0, sizeof(g_Accels[i]));
    g_Accels[i].valid = 1;
    g_Accels[i].handle = g_NextAccel++;
    g_Accels[i].count = cEntries;
    for (int j = 0; j < cEntries; j++) g_Accels[i].items[j] = lpaccl[j];
    mywin_accel_hash_insert(i);
    HACCEL h = g_Accels[i].handle;
    pthread_mutex_unlock(&g_User32LiteLock);
    return h;
}

BOOL DestroyAcceleratorTable(HACCEL hAccel)
{
    pthread_mutex_lock(&g_User32LiteLock);
    MyAccelLite* a = mywin_find_accel(hAccel);
    if (!a) { pthread_mutex_unlock(&g_User32LiteLock); return FALSE; }
    int aIdx = (int)(a - g_Accels);
    mywin_accel_hash_remove(aIdx);
    memset(a, 0, sizeof(*a));
    mywin_lite_push_free(g_AccelFreeStack, &g_AccelFreeTop, MYWIN_MAX_ACCELS, aIdx);
    pthread_mutex_unlock(&g_User32LiteLock);
    return TRUE;
}

static WORD mywin_accel_normalize_key(WORD key)
{
    if (key >= 'a' && key <= 'z') return (WORD)(key - 'a' + 'A');
    return key;
}

static WORD mywin_accel_linux_key_to_vk(WORD key)
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

static int mywin_accel_key_matches(const ACCEL* ac, UINT message, WORD msgKey)
{
    if (!ac) return 0;
    WORD accelKey = mywin_accel_normalize_key(ac->key);
    WORD rawKey = mywin_accel_normalize_key(msgKey);
    WORD vkKey = mywin_accel_normalize_key(mywin_accel_linux_key_to_vk(msgKey));
    if (ac->fVirt & FVIRTKEY) {
        if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return 0;
        return accelKey == rawKey || accelKey == vkKey;
    }
    if (message != WM_CHAR && message != WM_SYSCHAR && message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return 0;
    return accelKey == rawKey || accelKey == vkKey;
}

int TranslateAcceleratorA(HWND hWnd, HACCEL hAccTable, LPMSG lpMsg)
{
    if (!lpMsg || (lpMsg->message != WM_KEYDOWN && lpMsg->message != WM_SYSKEYDOWN &&
                   lpMsg->message != WM_CHAR && lpMsg->message != WM_SYSCHAR)) return 0;
    pthread_mutex_lock(&g_User32LiteLock);
    MyAccelLite* a = mywin_find_accel(hAccTable);
    if (!a) { pthread_mutex_unlock(&g_User32LiteLock); return 0; }
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
            mywin_accel_key_matches(ac, lpMsg->message, key)) {
            cmd = ac->cmd;
            matchedVirt = ac->fVirt;
            break;
        }
    }
    pthread_mutex_unlock(&g_User32LiteLock);
    if (!cmd) return 0;

    /* MSDN shape: TranslateAccelerator sends WM_COMMAND for normal accelerators
       and WM_SYSCOMMAND for system accelerators.  The menu/accelerator source is
       visible to the target through lParam != 0 in this build. */
    if (cmd >= 0xF000u || lpMsg->message == WM_SYSKEYDOWN || lpMsg->message == WM_SYSCHAR || (matchedVirt & FALT))
        SendMessageA(hWnd, WM_SYSCOMMAND, (WPARAM)cmd, 1);
    else
        SendMessageA(hWnd, WM_COMMAND, (WPARAM)cmd, 1);
    return 1;
}


// v110: GDI32 public entrypoints moved to wingdi.c
//   InvalidateRect / BeginPaint / GetDC / GDI object/drawing commands / MyGdi* helpers

