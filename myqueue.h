#pragma once
#include "mytypes.h"
#include <pthread.h>
#include <stdint.h>
#include <time.h>

// ─────────────────────────────────────────────
//  myOS Message Queue v14
//  Zentrale Queue pro UI-Thread/Owner, nicht mehr pro HWND.
//
//  Regel:
//    Queue transportiert Ereignisse.
//    Shared Memory / App-State transportiert große Daten.
// ─────────────────────────────────────────────

#define MYQUEUE_CAP 256
#define MYQUEUE_SLOT_WORDS ((MYQUEUE_CAP + 63) / 64)
#define MYQUEUE_LANE_INDEX_COUNT ((uint32_t)_MSG_LANE_BACKGROUND + 1u)
#define MYQUEUE_QS_INDEX_BITS 11u
#define MYQUEUE_FILTER_INDEX_BITS 7u
#define MYQUEUE_MSG_BUCKET_BITS 6u
#define MYQUEUE_MSG_BUCKET_COUNT (1u << MYQUEUE_MSG_BUCKET_BITS)
#define MYQUEUE_MSG_BUCKET_MASK (MYQUEUE_MSG_BUCKET_COUNT - 1u)
#define MYQUEUE_HWND_BUCKET_BITS 6u
#define MYQUEUE_HWND_BUCKET_COUNT (1u << MYQUEUE_HWND_BUCKET_BITS)
#define MYQUEUE_HWND_BUCKET_MASK (MYQUEUE_HWND_BUCKET_COUNT - 1u)

#define MYMSG_FLAG_COALESCED 0x00000001u
#define MYMSG_FLAG_ASYNC     0x00000002u
#define MYMSG_FLAG_SYNC_REQ  0x00000004u
#define MYMSG_FLAG_REPLY     0x00000008u

#define MYMSG_PRIO_INPUT      0u
#define MYMSG_PRIO_WINDOW     1u
#define MYMSG_PRIO_NORMAL     2u
#define MYMSG_PRIO_BACKGROUND 3u

/* v230: public Win32 queue-status bits.  winuser.h also exposes these;
   myqueue.h is private/internals-facing, so keep guarded definitions here to
   avoid dragging the full SDK header into the queue core. */
#ifndef QS_KEY
#define QS_KEY             0x0001u
#define QS_MOUSEMOVE       0x0002u
#define QS_MOUSEBUTTON     0x0004u
#define QS_POSTMESSAGE     0x0008u
#define QS_TIMER           0x0010u
#define QS_PAINT           0x0020u
#define QS_SENDMESSAGE     0x0040u
#define QS_HOTKEY          0x0080u
#define QS_ALLPOSTMESSAGE  0x0100u
#define QS_RAWINPUT        0x0400u
#define QS_MOUSE           (QS_MOUSEMOVE|QS_MOUSEBUTTON)
#define QS_INPUT           (QS_MOUSE|QS_KEY|QS_RAWINPUT)
#define QS_ALLEVENTS       (QS_INPUT|QS_POSTMESSAGE|QS_TIMER|QS_PAINT|QS_HOTKEY)
#define QS_ALLINPUT        (QS_INPUT|QS_POSTMESSAGE|QS_TIMER|QS_PAINT|QS_HOTKEY|QS_SENDMESSAGE)
#endif

/* v223-v229: private USER queue lanes/states.  Public MSG stays
   MSDN-shaped.  Internally these are deliberately byte-sized state values;
   Win32-style bitmasks stay DWORD-sized #defines below. */
typedef uint8_t _MsgLane;
#define _MSG_LANE_SEND       ((uint8_t)0)
#define _MSG_LANE_INPUT      ((uint8_t)1)
#define _MSG_LANE_WINDOW     ((uint8_t)2)
#define _MSG_LANE_POSTED     ((uint8_t)3)
#define _MSG_LANE_TIMER      ((uint8_t)4)
#define _MSG_LANE_BACKGROUND ((uint8_t)5)

typedef uint8_t _MsgState;
#define _MSG_STATE_FREE        ((uint8_t)0)
#define _MSG_STATE_QUEUED      ((uint8_t)1)
#define _MSG_STATE_DISPATCHING ((uint8_t)2)
#define _MSG_STATE_DISPATCHED  ((uint8_t)3)
#define _MSG_STATE_COALESCED   ((uint8_t)4)

/* v224-v229: input-kind/route-state are compact state fields. */
typedef uint8_t _MsgInputKind;
#define _MSG_INPUT_NONE         ((uint8_t)0)
#define _MSG_INPUT_KEY          ((uint8_t)1)
#define _MSG_INPUT_CHAR         ((uint8_t)2)
#define _MSG_INPUT_MOUSE_MOVE   ((uint8_t)3)
#define _MSG_INPUT_MOUSE_BUTTON ((uint8_t)4)
#define _MSG_INPUT_MOUSE_WHEEL  ((uint8_t)5)

typedef uint8_t _MsgRouteState;
#define _MSG_ROUTE_NONE            ((uint8_t)0)
#define _MSG_ROUTE_TARGET_RESOLVED ((uint8_t)1)
#define _MSG_ROUTE_CAPTURED        ((uint8_t)2)
#define _MSG_ROUTE_FOCUS           ((uint8_t)3)
#define _MSG_ROUTE_HOOKED          ((uint8_t)4)
#define _MSG_ROUTE_BLOCKED         ((uint8_t)5)

/* v225/v226/v229: USER route/filter/action sets are Win32-style masks:
   #define constants, DWORD storage, cheap OR/AND composition. */
typedef uint32_t _MsgRouteReason;
#define _MSG_ROUTE_REASON_NONE        0x00000000u
#define _MSG_ROUTE_REASON_DIRECT      0x00000001u
#define _MSG_ROUTE_REASON_CAPTURE     0x00000002u
#define _MSG_ROUTE_REASON_FOCUS       0x00000004u
#define _MSG_ROUTE_REASON_HITTEST     0x00000008u
#define _MSG_ROUTE_REASON_HOVER       0x00000010u
#define _MSG_ROUTE_REASON_HOOK        0x00000020u
#define _MSG_ROUTE_REASON_ACCELERATOR 0x00000040u
#define _MSG_ROUTE_REASON_DIALOG      0x00000080u
#define _MSG_ROUTE_REASON_TIMER       0x00000100u
#define _MSG_ROUTE_REASON_THREAD      0x00000200u
#define _MSG_ROUTE_REASON_SYNTHETIC   0x00000400u

typedef uint32_t _MsgFilterStage;
#define _MSG_FILTER_NONE        0x00000000u
#define _MSG_FILTER_HOOK        0x00000001u
#define _MSG_FILTER_ACCELERATOR 0x00000002u
#define _MSG_FILTER_DIALOG      0x00000004u
#define _MSG_FILTER_TRANSLATE   0x00000008u
#define _MSG_FILTER_DISPATCH    0x00000010u
#define _MSG_FILTER_MENU        0x00000020u
#define _MSG_FILTER_MODELLESS   0x00000040u

typedef uint8_t _MsgFilterState;
#define _MSG_FILTER_STATE_NONE        ((uint8_t)0)
#define _MSG_FILTER_STATE_PENDING     ((uint8_t)1)
#define _MSG_FILTER_STATE_PASSTHROUGH ((uint8_t)2)
#define _MSG_FILTER_STATE_HANDLED     ((uint8_t)3)
#define _MSG_FILTER_STATE_BLOCKED     ((uint8_t)4)

/* v227: the filter mask becomes an explicit tiny pipeline.  A queued message
   still carries stage bits for compact state/selection, but runners can now
   materialize the ordered stage vector and advance the current stage/state
   without open-coding the pump order again. */
typedef struct _MsgFilterStep {
    uint8_t stage;       /* one active stage bit; the full mask remains DWORD */
    uint8_t state;       /* compact state enum */
} _MsgFilterStep;

typedef struct _MsgFilterPipeline {
    uint32_t cbSize;
    uint32_t stages;
    uint8_t  count;
    uint8_t  _pad[3];
    _MsgFilterStep steps[8];
} _MsgFilterPipeline;

typedef struct _MsgRouteDescriptor {
    uint32_t cbSize;

    /* Compact, non-mask state values. */
    uint8_t  lane;
    uint8_t  input_kind;
    uint8_t  route_state;
    uint8_t  filter_state;
    uint8_t  filter_stage;
    uint8_t  _pad[3];

    /* Win32-style DWORD masks/flags/actions. */
    uint32_t route_reason;
    uint32_t route_flags;
    uint32_t filter_stages;
    uint32_t hwnd_action;

    HWND     target_hwnd;
    HWND     capture_hwnd;
    HWND     focus_hwnd;
    HWND     hit_hwnd;
} _MsgRouteDescriptor;

#define _MSG_INPUT_KIND_BIT(kind) (1u << (uint32_t)(kind))
#define _MSG_INPUT_KIND_MASK_ALL  (_MSG_INPUT_KIND_BIT(_MSG_INPUT_KEY) | \
                                   _MSG_INPUT_KIND_BIT(_MSG_INPUT_CHAR) | \
                                   _MSG_INPUT_KIND_BIT(_MSG_INPUT_MOUSE_MOVE) | \
                                   _MSG_INPUT_KIND_BIT(_MSG_INPUT_MOUSE_BUTTON) | \
                                   _MSG_INPUT_KIND_BIT(_MSG_INPUT_MOUSE_WHEEL))

#define _MSG_LANE_BIT(lane) (1u << (uint32_t)(lane))
#define _MSG_LANE_MASK_ALL  (_MSG_LANE_BIT(_MSG_LANE_SEND) | \
                             _MSG_LANE_BIT(_MSG_LANE_INPUT) | \
                             _MSG_LANE_BIT(_MSG_LANE_WINDOW) | \
                             _MSG_LANE_BIT(_MSG_LANE_POSTED) | \
                             _MSG_LANE_BIT(_MSG_LANE_TIMER) | \
                             _MSG_LANE_BIT(_MSG_LANE_BACKGROUND))

typedef enum _QueueSelectField {
    _QUEUE_SELECT_HWND      = 0x00000001u,
    _QUEUE_SELECT_MSG_RANGE = 0x00000002u,
    _QUEUE_SELECT_LANES     = 0x00000004u,
    _QUEUE_SELECT_REMOVE     = 0x00000008u,
    _QUEUE_SELECT_WAIT       = 0x00000010u,
    _QUEUE_SELECT_INPUT_KIND = 0x00000020u,
    _QUEUE_SELECT_FILTER_STAGE = 0x00000040u,
    _QUEUE_SELECT_QS = 0x00000080u
} _QueueSelectField;

typedef struct _QueueSelect {
    uint32_t fields;
    HWND     hwnd;
    UINT     msgMin;
    UINT     msgMax;
    uint32_t laneMask;
    uint32_t inputKindMask;
    uint32_t filterStageMask;
    uint32_t qsMask;
    int      remove;
} _QueueSelect;

/* v233: selectors are compiled once into a tiny predicate/op plan before the
   queue consumes indexed candidate slots.  Lanes/QS still provide the primary
   index path; hwnd/range/input/filter checks become compact plan ops instead
   of an open-coded candidate if-ladder. */
#define MYQUEUE_SELECT_OP_MAX 6u

typedef uint8_t _QueueSelectOp;
#define _QUEUE_SELECT_OP_NONE         ((uint8_t)0)
#define _QUEUE_SELECT_OP_QS           ((uint8_t)1)
#define _QUEUE_SELECT_OP_HWND         ((uint8_t)2)
#define _QUEUE_SELECT_OP_MSG_RANGE    ((uint8_t)3)
#define _QUEUE_SELECT_OP_INPUT_KIND   ((uint8_t)4)
#define _QUEUE_SELECT_OP_FILTER_STAGE ((uint8_t)5)
#define _QUEUE_SELECT_OP_COUNT        ((uint8_t)6)

#define _QUEUE_SELECT_SOURCE_QS           0x01u
#define _QUEUE_SELECT_SOURCE_INPUT_KIND   0x02u
#define _QUEUE_SELECT_SOURCE_FILTER_STAGE 0x04u
#define _QUEUE_SELECT_SOURCE_MSG_BUCKET   0x08u
#define _QUEUE_SELECT_SOURCE_HWND_BUCKET  0x10u

/* v234: compiled selector plans now carry an explicit index-source mask.
   This keeps the hot selector path split into two narrow tables: slot-index
   sources first, then only the remaining predicate ops. */
typedef struct _QueueSelectPlan {
    uint32_t cbSize;
    uint32_t fields;
    HWND     hwnd;
    UINT     msgMin;
    UINT     msgMax;
    uint32_t laneMask;
    uint32_t inputKindMask;
    uint32_t filterStageMask;
    uint32_t qsMask;
    uint64_t msgBucketMask;
    uint8_t  opCount;
    uint8_t  indexSourceMask;
    uint8_t  hwndBucket;
    uint8_t  _pad0;
    uint8_t  ops[MYQUEUE_SELECT_OP_MAX];
    uint8_t  _pad[2];
} _QueueSelectPlan;

#define MYQUEUE_MATCH_REMOVE  0x00000001u
#define MYQUEUE_MATCH_WAIT    0x00000002u

// Zentraler Message-Container. HWND/WinAPI kompatibel genug,
// aber schon mit Source/Target/Seq/Payload-Feldern für spätere
// Shared-Section-IPC vorbereitet.
typedef struct MyMessage {
    uint32_t size;
    uint32_t type;

    uint32_t sender_pid;
    uint32_t sender_tid;
    uint32_t target_pid;
    uint32_t target_tid;

    HWND     hwnd;
    UINT     msg;
    WPARAM   wparam;
    LPARAM   lparam;

    uint32_t priority;
    uint32_t flags;

    /* v229: compact queue state sidecar.  State-like values are bytes;
       bitmask/action fields stay DWORD-sized and Win32-composable. */
    uint8_t  lane;
    uint8_t  state;
    uint8_t  input_kind;
    uint8_t  route_state;
    uint8_t  filter_state;
    uint8_t  filter_stage;
    uint8_t  _state_pad[2];
    uint32_t route_flags;
    uint32_t route_reason;
    uint32_t filter_stages;
    uint32_t route_action;
    HWND     route_hwnd;
    HWND     capture_hwnd;
    HWND     focus_hwnd;
    HWND     hit_hwnd;
    uint64_t seq;
    uint64_t timestamp_ns;

    uint32_t section_id;
    uint32_t payload_offset;
    uint32_t payload_size;

    // v21: sync SendMessageTimeout request/reply context. This is an in-process
    // PoC pointer; later it becomes a kernel/object handle or shared reply port.
    LPVOID   sync_ctx;
} MyMessage;

typedef struct MyQueueStats {
    uint64_t posted;
    uint64_t dispatched;
    uint64_t dropped;
    uint64_t coalesced;
    uint64_t peak_depth;
    uint32_t current_depth;
} MyQueueStats;

/* v231: queue storage is split into a cache-friendly hot ring plus cold
   metadata sidecars.  Public callers still pass/receive MyMessage; the queue
   core scans _MessageHot and only materializes _MessageCold on dequeue,
   coalescing, SendMessage/IPC, or diagnostics. */
typedef struct _MessageHot {
    HWND     hwnd;
    UINT     msg;
    WPARAM   wparam;
    LPARAM   lparam;

    uint32_t priority;
    uint32_t flags;

    uint8_t  lane;
    uint8_t  state;
    uint8_t  input_kind;
    uint8_t  route_state;
    uint8_t  filter_state;
    uint8_t  filter_stage;
    uint8_t  _pad[2];

    uint32_t route_flags;
    uint32_t route_reason;
    uint32_t filter_stages;
    uint32_t route_action;
} _MessageHot;

typedef struct _MessageCold {
    uint32_t size;
    uint32_t type;

    uint32_t sender_pid;
    uint32_t sender_tid;
    uint32_t target_pid;
    uint32_t target_tid;

    HWND     route_hwnd;
    HWND     capture_hwnd;
    HWND     focus_hwnd;
    HWND     hit_hwnd;

    uint64_t seq;
    uint64_t timestamp_ns;

    uint32_t section_id;
    uint32_t payload_offset;
    uint32_t payload_size;
    uint32_t _pad;

    LPVOID   sync_ctx;
} _MessageCold;

typedef struct MyMessageQueue {
    _MessageHot  hot[MYQUEUE_CAP];
    _MessageCold cold[MYQUEUE_CAP];
    int head;
    int tail;
    int count;
    uint64_t next_seq;
    MyQueueStats stats;

    /* v230: Win32-style QS_* queue state.  current_qs is the cheap
       readiness prefilter; changed_qs is the high-word GetQueueStatus state
       since the last matching query. */
    uint32_t current_qs;
    uint32_t changed_qs;
    uint64_t qs_prefilter_skips;

    /* v232: per-slot dispatch indexes.  The hot ring remains the source of
       truth, but selectors can now intersect QS_* and lane bitsets and jump
       straight to matching physical ring slots while resolving order relative
       to head. */
    uint64_t lane_slot_bits[_MSG_LANE_BACKGROUND + 1][MYQUEUE_SLOT_WORDS];
    uint64_t qs_slot_bits[MYQUEUE_QS_INDEX_BITS][MYQUEUE_SLOT_WORDS];

    /* v233: second-level selector indexes for predicates that were still
       candidate-side checks in v232.  Input-kind and filter-stage selectors now
       intersect tiny slot-bitsets before touching _MessageHot. */
    uint64_t input_kind_slot_bits[_MSG_INPUT_MOUSE_WHEEL + 1][MYQUEUE_SLOT_WORDS];
    uint64_t filter_stage_slot_bits[MYQUEUE_FILTER_INDEX_BITS][MYQUEUE_SLOT_WORDS];

    /* v235: HWND and message range selectors get their own small hash-bucket
       slot indexes.  They are intentionally bounded (64 buckets each): the
       bucket intersection narrows the hot candidate set, and the existing
       HWND/range predicates remain the exact semantic guard against hash
       collisions. */
    uint64_t hwnd_bucket_slot_bits[MYQUEUE_HWND_BUCKET_COUNT][MYQUEUE_SLOT_WORDS];
    uint64_t msg_bucket_slot_bits[MYQUEUE_MSG_BUCKET_COUNT][MYQUEUE_SLOT_WORDS];

    uint64_t indexed_candidate_probes;
    uint64_t indexed_empty_skips;
    uint64_t slot_index_rebuilds;

    /* v233: selector-plan diagnostics.  These are intentionally separate from
       queue correctness; they prove candidates flow through the table-driven
       predicate plan instead of reintroducing scattered branch paths. */
    uint64_t selector_plan_builds;
    uint64_t selector_plan_predicate_ops;
    uint64_t selector_plan_candidate_rejects;
    uint64_t selector_plan_fast_accepts;

    /* v234: the last selector plan is cached per queue.  Real pumps call the
       same GetMessage/PeekMessage shape repeatedly; recompiling the tiny op
       vector every pass is needless branch work. */
    _QueueSelect selector_plan_cache_select;
    _QueueSelectPlan selector_plan_cache;
    uint8_t selector_plan_cache_valid;
    uint8_t _selector_plan_cache_pad[7];
    uint64_t selector_plan_cache_hits;

    pthread_mutex_t lock;
    pthread_cond_t wake;
} MyMessageQueue;

void myqueue_init(MyMessageQueue* q);
void myqueue_destroy(MyMessageQueue* q);

// Postet eine Message. Coalescable Messages werden ersetzt,
// wenn im Ring bereits eine passende alte Message liegt.
int  myqueue_post(MyMessageQueue* q, const MyMessage* in);

// Non-blocking Pop. 1 = Message geliefert, 0 = leer.
int  myqueue_get(MyMessageQueue* q, MyMessage* out);

// Blocking Pop mit Timeout in Millisekunden.
// timeout_ms < 0 wartet unbegrenzt.
int  myqueue_get_wait(MyMessageQueue* q, MyMessage* out, int timeout_ms);

// WinAPI-nahe Peek/Get-Grundlage mit HWND- und Message-Filter.
// remove=0 entspricht PM_NOREMOVE, remove=1 entspricht PM_REMOVE/GetMessage.
int  myqueue_peek_select(MyMessageQueue* q, MyMessage* out, const _QueueSelect* select);
int  myqueue_peek_match(MyMessageQueue* q, MyMessage* out,
                        HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                        int remove);
int  myqueue_wait_select(MyMessageQueue* q, MyMessage* out, const _QueueSelect* select, int timeout_ms);
int  myqueue_wait_match(MyMessageQueue* q, MyMessage* out,
                        HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                        int remove, int timeout_ms);

/* v149: wait once and return to the caller after a wake even if no queued
   message matched.  USER32 uses this to recompute synthetic WM_TIMER due-times. */
int  myqueue_wait_select_or_wake(MyMessageQueue* q, MyMessage* out, const _QueueSelect* select, int timeout_ms);
int  myqueue_wait_match_or_wake(MyMessageQueue* q, MyMessage* out,
                                HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                                int remove, int timeout_ms);
void myqueue_wake(MyMessageQueue* q);
int  myqueue_remove_matching(MyMessageQueue* q, HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax);

void myqueue_mark_dispatched(MyMessageQueue* q);
void myqueue_get_stats(MyMessageQueue* q, MyQueueStats* out);

int  mymsg_is_coalescable(UINT msg);
uint32_t mymsg_default_priority(UINT msg);
uint32_t mymsg_qs_bits(UINT msg, uint32_t lane, uint32_t inputKind, uint32_t flags);
void myqueue_make_qs_select(_QueueSelect* select, HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax, uint32_t qsMask, int remove);
DWORD myqueue_get_queue_status(MyMessageQueue* q, UINT flags);
uint32_t myqueue_peek_queue_status(MyMessageQueue* q);
int myqueue_has_queue_status(MyMessageQueue* q, UINT flags);
uint32_t mymsg_default_lane(UINT msg, uint32_t flags);
uint32_t mymsg_default_input_kind(UINT msg);
uint32_t mymsg_default_route_reason(UINT msg, uint32_t flags, uint32_t inputKind);
uint32_t mymsg_required_hwnd_action_for_route(uint32_t lane, uint32_t inputKind, uint32_t routeReason);
uint32_t mymsg_default_filter_stages(UINT msg, uint32_t lane, uint32_t inputKind, uint32_t routeReason);
uint32_t mymsg_first_filter_stage(uint32_t filterStages);
uint32_t mymsg_next_filter_stage(uint32_t filterStages, uint32_t afterStage);
int      mymsg_build_filter_pipeline(uint32_t filterStages, _MsgFilterPipeline* out);
int      mymsg_advance_filter_stage(MyMessage* msg, uint32_t completedStage, uint32_t resultState);
void mymsg_make_route_descriptor(const MyMessage* msg, _MsgRouteDescriptor* out);
void mymsg_apply_route_descriptor(MyMessage* msg, const _MsgRouteDescriptor* route);
void myqueue_make_select(_QueueSelect* select, HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax, int remove);
void myqueue_make_input_select(_QueueSelect* select, HWND hwndFilter, uint32_t inputKindMask, int remove);
void myqueue_make_filter_select(_QueueSelect* select, HWND hwndFilter, uint32_t filterStageMask, int remove);
int  myqueue_debug_compile_select_plan(const _QueueSelect* select, _QueueSelectPlan* out);
uint64_t myos_now_ns(void);
