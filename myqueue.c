#include "myqueue.h"
#include "hwnd.h"
#include <errno.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define MYQUEUE_LIKELY(x)   __builtin_expect(!!(x), 1)
#define MYQUEUE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define MYQUEUE_LIKELY(x)   (x)
#define MYQUEUE_UNLIKELY(x) (x)
#endif

_Static_assert(MYQUEUE_SLOT_WORDS == 4, "v242 queue bitset fast path expects 256 queue slots");

uint64_t myos_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int mymsg_is_coalescable(UINT msg)
{
    switch (msg) {
        case WM_MOUSEMOVE:
        case WM_PAINT:
        case WM_WINDOWPOSCHANGING:
        case WM_WINDOWPOSCHANGED:
        case WM_MOVE:
        case WM_SIZE:
        case WM_SURFACE_DIRTY:
            return 1;
        default:
            return 0;
    }
}

uint32_t mymsg_default_priority(UINT msg)
{
    switch (msg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL:
            return MYMSG_PRIO_INPUT;
        case WM_WINDOWPOSCHANGING:
        case WM_WINDOWPOSCHANGED:
        case WM_MOVE:
        case WM_SIZE:
        case WM_PAINT:
        case WM_SURFACE_DIRTY:
            return MYMSG_PRIO_WINDOW;
        default:
            return MYMSG_PRIO_NORMAL;
    }
}

uint32_t mymsg_qs_bits(UINT msg, uint32_t lane, uint32_t inputKind, uint32_t flags)
{
    if (flags & MYMSG_FLAG_SYNC_REQ) return QS_SENDMESSAGE;
    if (lane == _MSG_LANE_SEND) return QS_SENDMESSAGE;

    if (!inputKind) inputKind = mymsg_default_input_kind(msg);
    if (lane > _MSG_LANE_BACKGROUND) lane = mymsg_default_lane(msg, flags);

    switch (inputKind) {
        case _MSG_INPUT_KEY:
        case _MSG_INPUT_CHAR:
            return QS_KEY;
        case _MSG_INPUT_MOUSE_MOVE:
            return QS_MOUSEMOVE;
        case _MSG_INPUT_MOUSE_BUTTON:
        case _MSG_INPUT_MOUSE_WHEEL:
            return QS_MOUSEBUTTON;
        default:
            break;
    }

    switch (msg) {
        case WM_TIMER:
            return QS_TIMER;
        case WM_PAINT:
        case WM_SURFACE_DIRTY:
            return QS_PAINT;
        default:
            break;
    }

    if (lane == _MSG_LANE_TIMER) return QS_TIMER;
    if (lane == _MSG_LANE_WINDOW && (msg == WM_PAINT || msg == WM_SURFACE_DIRTY)) return QS_PAINT;
    return QS_POSTMESSAGE;
}

uint32_t mymsg_default_lane(UINT msg, uint32_t flags)
{
    if (flags & MYMSG_FLAG_SYNC_REQ)
        return _MSG_LANE_SEND;
    switch (msg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL:
            return _MSG_LANE_INPUT;
        case WM_WINDOWPOSCHANGING:
        case WM_WINDOWPOSCHANGED:
        case WM_MOVE:
        case WM_SIZE:
        case WM_PAINT:
        case WM_SURFACE_DIRTY:
            return _MSG_LANE_WINDOW;
        case WM_TIMER:
            return _MSG_LANE_TIMER;
        default:
            return _MSG_LANE_POSTED;
    }
}

uint32_t mymsg_default_input_kind(UINT msg)
{
    switch (msg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            return _MSG_INPUT_KEY;
        case WM_CHAR:
        case WM_SYSCHAR:
            return _MSG_INPUT_CHAR;
        case WM_MOUSEMOVE:
            return _MSG_INPUT_MOUSE_MOVE;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return _MSG_INPUT_MOUSE_BUTTON;
        case WM_MOUSEWHEEL:
            return _MSG_INPUT_MOUSE_WHEEL;
        default:
            return _MSG_INPUT_NONE;
    }
}

uint32_t mymsg_default_route_reason(UINT msg, uint32_t flags, uint32_t inputKind)
{
    if (flags & MYMSG_FLAG_SYNC_REQ)
        return _MSG_ROUTE_REASON_DIRECT;
    if (msg == WM_TIMER)
        return _MSG_ROUTE_REASON_TIMER;
    switch (inputKind) {
        case _MSG_INPUT_KEY:
        case _MSG_INPUT_CHAR:
            return _MSG_ROUTE_REASON_FOCUS;
        case _MSG_INPUT_MOUSE_WHEEL:
            return _MSG_ROUTE_REASON_HOVER | _MSG_ROUTE_REASON_HITTEST;
        case _MSG_INPUT_MOUSE_MOVE:
        case _MSG_INPUT_MOUSE_BUTTON:
            return _MSG_ROUTE_REASON_HITTEST;
        default:
            return _MSG_ROUTE_REASON_DIRECT;
    }
}

uint32_t mymsg_required_hwnd_action_for_route(uint32_t lane, uint32_t inputKind, uint32_t routeReason)
{
    uint32_t action = _HWND_ACTION_MESSAGE;
    if (lane == _MSG_LANE_TIMER)
        return action;
    if (lane == _MSG_LANE_INPUT) {
        if (routeReason & (_MSG_ROUTE_REASON_CAPTURE | _MSG_ROUTE_REASON_HITTEST | _MSG_ROUTE_REASON_HOVER))
            action |= _HWND_ACTION_HITTEST;
        if (routeReason & _MSG_ROUTE_REASON_CAPTURE)
            action |= _HWND_ACTION_CAPTURE;
        if (routeReason & (_MSG_ROUTE_REASON_FOCUS | _MSG_ROUTE_REASON_ACCELERATOR | _MSG_ROUTE_REASON_DIALOG))
            action |= _HWND_ACTION_FOCUS;
        if (inputKind == _MSG_INPUT_KEY || inputKind == _MSG_INPUT_CHAR)
            action |= _HWND_ACTION_FOCUS;
        if (inputKind == _MSG_INPUT_MOUSE_MOVE || inputKind == _MSG_INPUT_MOUSE_BUTTON || inputKind == _MSG_INPUT_MOUSE_WHEEL)
            action |= _HWND_ACTION_HITTEST;
    }
    return action;
}

uint32_t mymsg_default_filter_stages(UINT msg, uint32_t lane, uint32_t inputKind, uint32_t routeReason)
{
    uint32_t stages = _MSG_FILTER_HOOK | _MSG_FILTER_DISPATCH;
    if (lane == _MSG_LANE_SEND)
        return stages;
    if (lane == _MSG_LANE_TIMER)
        return stages;
    if (routeReason & _MSG_ROUTE_REASON_HOOK)
        stages |= _MSG_FILTER_HOOK;
    if (routeReason & _MSG_ROUTE_REASON_ACCELERATOR)
        stages |= _MSG_FILTER_ACCELERATOR;
    if (routeReason & _MSG_ROUTE_REASON_DIALOG)
        stages |= _MSG_FILTER_DIALOG;
    if (routeReason & _MSG_ROUTE_REASON_HOVER)
        stages |= _MSG_FILTER_MENU;

    switch (inputKind) {
        case _MSG_INPUT_KEY:
        case _MSG_INPUT_CHAR:
            stages |= _MSG_FILTER_ACCELERATOR | _MSG_FILTER_DIALOG | _MSG_FILTER_MODELLESS | _MSG_FILTER_TRANSLATE;
            break;
        case _MSG_INPUT_MOUSE_BUTTON:
        case _MSG_INPUT_MOUSE_WHEEL:
            stages |= _MSG_FILTER_MENU;
            break;
        case _MSG_INPUT_MOUSE_MOVE:
            break;
        default:
            break;
    }
    (void)msg;
    return stages;
}

static const uint32_t _g_MsgFilterStageOrder[] = {
    _MSG_FILTER_HOOK,
    _MSG_FILTER_ACCELERATOR,
    _MSG_FILTER_DIALOG,
    _MSG_FILTER_MODELLESS,
    _MSG_FILTER_TRANSLATE,
    _MSG_FILTER_MENU,
    _MSG_FILTER_DISPATCH
};

uint32_t mymsg_first_filter_stage(uint32_t filterStages)
{
    for (unsigned i = 0; i < sizeof(_g_MsgFilterStageOrder)/sizeof(_g_MsgFilterStageOrder[0]); ++i) {
        if (filterStages & _g_MsgFilterStageOrder[i])
            return _g_MsgFilterStageOrder[i];
    }
    return _MSG_FILTER_NONE;
}

uint32_t mymsg_next_filter_stage(uint32_t filterStages, uint32_t afterStage)
{
    int seen = (afterStage == _MSG_FILTER_NONE) ? 1 : 0;
    for (unsigned i = 0; i < sizeof(_g_MsgFilterStageOrder)/sizeof(_g_MsgFilterStageOrder[0]); ++i) {
        uint32_t stage = _g_MsgFilterStageOrder[i];
        if (!seen) {
            if (stage == afterStage) seen = 1;
            continue;
        }
        if (filterStages & stage) return stage;
    }
    return _MSG_FILTER_NONE;
}

int mymsg_build_filter_pipeline(uint32_t filterStages, _MsgFilterPipeline* out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->cbSize = sizeof(*out);
    out->stages = filterStages;
    for (unsigned i = 0; i < sizeof(_g_MsgFilterStageOrder)/sizeof(_g_MsgFilterStageOrder[0]); ++i) {
        uint32_t stage = _g_MsgFilterStageOrder[i];
        if (!(filterStages & stage)) continue;
        if (out->count >= (uint32_t)(sizeof(out->steps)/sizeof(out->steps[0]))) break;
        out->steps[out->count].stage = stage;
        out->steps[out->count].state = _MSG_FILTER_STATE_PENDING;
        out->count++;
    }
    return (int)out->count;
}

int mymsg_advance_filter_stage(MyMessage* msg, uint32_t completedStage, uint32_t resultState)
{
    if (!msg || !completedStage) return 0;
    if (!msg->filter_stages) {
        uint32_t lane = (msg->lane <= _MSG_LANE_BACKGROUND) ? msg->lane : mymsg_default_lane(msg->msg, msg->flags);
        uint32_t kind = msg->input_kind ? msg->input_kind : mymsg_default_input_kind(msg->msg);
        uint32_t reason = msg->route_reason ? msg->route_reason : mymsg_default_route_reason(msg->msg, msg->flags, kind);
        msg->filter_stages = mymsg_default_filter_stages(msg->msg, lane, kind, reason);
    }
    msg->filter_stage = completedStage;
    msg->filter_state = resultState ? resultState : _MSG_FILTER_STATE_PASSTHROUGH;
    if (msg->filter_state == _MSG_FILTER_STATE_PASSTHROUGH) {
        uint32_t next = mymsg_next_filter_stage(msg->filter_stages, completedStage);
        msg->filter_stage = next ? next : completedStage;
        if (next) msg->filter_state = _MSG_FILTER_STATE_PENDING;
    }
    return 1;
}

void mymsg_make_route_descriptor(const MyMessage* msg, _MsgRouteDescriptor* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->cbSize = sizeof(*out);
    if (!msg) return;
    out->lane = (msg->lane <= _MSG_LANE_BACKGROUND) ? msg->lane : mymsg_default_lane(msg->msg, msg->flags);
    out->input_kind = msg->input_kind ? msg->input_kind : mymsg_default_input_kind(msg->msg);
    out->route_state = msg->route_state;
    out->route_flags = msg->route_flags;
    out->route_reason = msg->route_reason ? msg->route_reason : mymsg_default_route_reason(msg->msg, msg->flags, out->input_kind);
    out->filter_stages = msg->filter_stages ? msg->filter_stages : mymsg_default_filter_stages(msg->msg, out->lane, out->input_kind, out->route_reason);
    out->filter_state = msg->filter_state ? msg->filter_state : _MSG_FILTER_STATE_PENDING;
    out->filter_stage = msg->filter_stage ? msg->filter_stage : mymsg_first_filter_stage(out->filter_stages);
    out->target_hwnd = msg->route_hwnd ? msg->route_hwnd : msg->hwnd;
    out->capture_hwnd = msg->capture_hwnd;
    out->focus_hwnd = msg->focus_hwnd;
    out->hit_hwnd = msg->hit_hwnd;
    out->hwnd_action = msg->route_action ? msg->route_action : mymsg_required_hwnd_action_for_route(out->lane, out->input_kind, out->route_reason);
}

void mymsg_apply_route_descriptor(MyMessage* msg, const _MsgRouteDescriptor* route)
{
    if (!msg || !route) return;
    msg->lane = route->lane;
    msg->input_kind = route->input_kind;
    msg->route_state = route->route_state;
    msg->route_flags = route->route_flags;
    msg->route_reason = route->route_reason;
    msg->filter_stages = route->filter_stages;
    msg->filter_state = route->filter_state;
    msg->filter_stage = route->filter_stage;
    msg->route_action = route->hwnd_action;
    msg->route_hwnd = route->target_hwnd;
    msg->capture_hwnd = route->capture_hwnd;
    msg->focus_hwnd = route->focus_hwnd;
    msg->hit_hwnd = route->hit_hwnd;
}

static uint32_t lane_mask_for_range(UINT minMsg, UINT maxMsg)
{
    if (!minMsg && !maxMsg) return _MSG_LANE_MASK_ALL;
    if (maxMsg < minMsg) return _MSG_LANE_MASK_ALL;

    /* Keep pathological broad filters cheap.  The exact message number check
       still happens in message_matches_select(); this mask is only a lane
       preselector for the iterator. */
    if ((maxMsg - minMsg) > 4096u) return _MSG_LANE_MASK_ALL;

    uint32_t mask = 0;
    for (UINT m = minMsg; m <= maxMsg; ++m) {
        mask |= _MSG_LANE_BIT(mymsg_default_lane(m, 0));
        if (m == 0xffffffffu || m == maxMsg) break;
    }
    return mask ? mask : _MSG_LANE_MASK_ALL;
}

static uint32_t qs_mask_for_range(UINT minMsg, UINT maxMsg)
{
    if (!minMsg && !maxMsg) return QS_ALLINPUT;
    if (maxMsg < minMsg) return QS_ALLINPUT;
    if ((maxMsg - minMsg) > 4096u) return QS_ALLINPUT;

    uint32_t mask = 0;
    for (UINT m = minMsg; m <= maxMsg; ++m) {
        uint32_t lane = mymsg_default_lane(m, 0);
        uint32_t kind = mymsg_default_input_kind(m);
        mask |= mymsg_qs_bits(m, lane, kind, 0);
        if (m == 0xffffffffu || m == maxMsg) break;
    }
    return mask ? mask : QS_ALLINPUT;
}

static uint32_t qs_mask_for_input_kind_mask(uint32_t inputKindMask)
{
    uint32_t mask = 0;
    if (!inputKindMask) inputKindMask = _MSG_INPUT_KIND_MASK_ALL;
    if (inputKindMask & (_MSG_INPUT_KIND_BIT(_MSG_INPUT_KEY) | _MSG_INPUT_KIND_BIT(_MSG_INPUT_CHAR)))
        mask |= QS_KEY;
    if (inputKindMask & _MSG_INPUT_KIND_BIT(_MSG_INPUT_MOUSE_MOVE))
        mask |= QS_MOUSEMOVE;
    if (inputKindMask & (_MSG_INPUT_KIND_BIT(_MSG_INPUT_MOUSE_BUTTON) | _MSG_INPUT_KIND_BIT(_MSG_INPUT_MOUSE_WHEEL)))
        mask |= QS_MOUSEBUTTON;
    return mask ? mask : QS_INPUT;
}


void myqueue_make_select(_QueueSelect* select, HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax, int remove)
{
    if (!select) return;
    memset(select, 0, sizeof(*select));
    if (hwndFilter) { select->fields |= _QUEUE_SELECT_HWND; select->hwnd = hwndFilter; }
    if (wMsgFilterMin || wMsgFilterMax) {
        select->fields |= _QUEUE_SELECT_MSG_RANGE | _QUEUE_SELECT_LANES | _QUEUE_SELECT_QS;
        select->msgMin = wMsgFilterMin;
        select->msgMax = wMsgFilterMax;
        select->laneMask = lane_mask_for_range(wMsgFilterMin, wMsgFilterMax);
        select->qsMask = qs_mask_for_range(wMsgFilterMin, wMsgFilterMax);
    } else {
        select->fields |= _QUEUE_SELECT_LANES | _QUEUE_SELECT_QS;
        select->laneMask = _MSG_LANE_MASK_ALL;
        select->qsMask = QS_ALLINPUT;
    }
    if (remove) { select->fields |= _QUEUE_SELECT_REMOVE; select->remove = 1; }
}

void myqueue_make_input_select(_QueueSelect* select, HWND hwndFilter, uint32_t inputKindMask, int remove)
{
    if (!select) return;
    memset(select, 0, sizeof(*select));
    if (hwndFilter) { select->fields |= _QUEUE_SELECT_HWND; select->hwnd = hwndFilter; }
    select->fields |= _QUEUE_SELECT_LANES | _QUEUE_SELECT_INPUT_KIND | _QUEUE_SELECT_QS;
    select->laneMask = _MSG_LANE_BIT(_MSG_LANE_INPUT);
    select->inputKindMask = inputKindMask ? inputKindMask : _MSG_INPUT_KIND_MASK_ALL;
    select->qsMask = qs_mask_for_input_kind_mask(select->inputKindMask);
    if (remove) { select->fields |= _QUEUE_SELECT_REMOVE; select->remove = 1; }
}

void myqueue_make_filter_select(_QueueSelect* select, HWND hwndFilter, uint32_t filterStageMask, int remove)
{
    if (!select) return;
    memset(select, 0, sizeof(*select));
    if (hwndFilter) { select->fields |= _QUEUE_SELECT_HWND; select->hwnd = hwndFilter; }
    select->fields |= _QUEUE_SELECT_LANES | _QUEUE_SELECT_FILTER_STAGE | _QUEUE_SELECT_QS;
    select->laneMask = _MSG_LANE_MASK_ALL;
    select->filterStageMask = filterStageMask ? filterStageMask : (_MSG_FILTER_HOOK | _MSG_FILTER_ACCELERATOR | _MSG_FILTER_DIALOG | _MSG_FILTER_MODELLESS | _MSG_FILTER_TRANSLATE | _MSG_FILTER_MENU | _MSG_FILTER_DISPATCH);
    select->qsMask = QS_ALLINPUT;
    if (remove) { select->fields |= _QUEUE_SELECT_REMOVE; select->remove = 1; }
}

void myqueue_make_qs_select(_QueueSelect* select, HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax, uint32_t qsMask, int remove)
{
    myqueue_make_select(select, hwndFilter, wMsgFilterMin, wMsgFilterMax, remove);
    if (!select) return;
    select->fields |= _QUEUE_SELECT_QS;
    select->qsMask = qsMask ? qsMask : QS_ALLINPUT;
}

static uint32_t hot_effective_lane(const _MessageHot* h)
{
    if (!h) return _MSG_LANE_BACKGROUND;
    if (h->lane <= _MSG_LANE_BACKGROUND) return h->lane;
    return mymsg_default_lane(h->msg, h->flags);
}

static uint32_t qs_bits_for_hot(const _MessageHot* h)
{
    if (!h) return 0;
    uint32_t lane = hot_effective_lane(h);
    uint32_t kind = h->input_kind ? h->input_kind : mymsg_default_input_kind(h->msg);
    return mymsg_qs_bits(h->msg, lane, kind, h->flags);
}

static uint32_t msg_bucket_for_msg(UINT msg)
{
    return ((uint32_t)msg) & MYQUEUE_MSG_BUCKET_MASK;
}

static uint32_t hwnd_bucket_for_hwnd(HWND hwnd)
{
    uint32_t v = (uint32_t)hwnd;
    /* Keep the hash tiny but avoid depending on HANDLE allocation's lowest
       bits only.  Collisions are fine: the exact HWND predicate remains in the
       selector op-table. */
    v ^= v >> MYQUEUE_HWND_BUCKET_BITS;
    v ^= v >> (MYQUEUE_HWND_BUCKET_BITS * 2u);
    return v & MYQUEUE_HWND_BUCKET_MASK;
}

static uint64_t msg_bucket_mask_for_range(UINT minMsg, UINT maxMsg)
{
    if (!minMsg && !maxMsg) return 0;
    if (maxMsg < minMsg) return 0;

    uint64_t span = (uint64_t)maxMsg - (uint64_t)minMsg;
    /* A broad range would touch every 64-way bucket and buy nothing.  Keep the
       v235 message index source for exact/small-range selectors; the old
       msg-range predicate remains the full semantic fallback for broad ranges. */
    if (span >= MYQUEUE_MSG_BUCKET_COUNT) return 0;

    uint64_t mask = 0;
    for (UINT m = minMsg;; ++m) {
        mask |= 1ull << msg_bucket_for_msg(m);
        if (m == maxMsg || m == 0xffffffffu) break;
    }
    return mask;
}

static int hot_is_queue_visible(const _MessageHot* h)
{
    return h && (h->state == _MSG_STATE_QUEUED || h->state == _MSG_STATE_COALESCED);
}

static inline void bitset_clear_slot(uint64_t words[MYQUEUE_SLOT_WORDS], int idx)
{
    if (MYQUEUE_UNLIKELY(!words || idx < 0 || idx >= MYQUEUE_CAP)) return;
    words[(uint32_t)idx >> 6] &= ~(1ull << (uint32_t)(idx & 63));
}

static inline void bitset_set_slot(uint64_t words[MYQUEUE_SLOT_WORDS], int idx)
{
    if (MYQUEUE_UNLIKELY(!words || idx < 0 || idx >= MYQUEUE_CAP)) return;
    words[(uint32_t)idx >> 6] |= (1ull << (uint32_t)(idx & 63));
}

static inline int bitset_any(const uint64_t words[MYQUEUE_SLOT_WORDS])
{
    if (MYQUEUE_UNLIKELY(!words)) return 0;
    return (words[0] | words[1] | words[2] | words[3]) ? 1 : 0;
}

static inline void bitset_or_into(uint64_t dst[MYQUEUE_SLOT_WORDS], const uint64_t src[MYQUEUE_SLOT_WORDS])
{
    if (MYQUEUE_UNLIKELY(!dst || !src)) return;
    dst[0] |= src[0];
    dst[1] |= src[1];
    dst[2] |= src[2];
    dst[3] |= src[3];
}

static inline void bitset_and_into(uint64_t dst[MYQUEUE_SLOT_WORDS], const uint64_t src[MYQUEUE_SLOT_WORDS])
{
    if (MYQUEUE_UNLIKELY(!dst || !src)) return;
    dst[0] &= src[0];
    dst[1] &= src[1];
    dst[2] &= src[2];
    dst[3] &= src[3];
}

static inline void bitset_copy_words(uint64_t dst[MYQUEUE_SLOT_WORDS], const uint64_t src[MYQUEUE_SLOT_WORDS])
{
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

static inline void bitset_zero_words(uint64_t dst[MYQUEUE_SLOT_WORDS])
{
    dst[0] = dst[1] = dst[2] = dst[3] = 0ull;
}

static void slot_index_clear_slot_locked(MyMessageQueue* q, int idx)
{
    if (!q || idx < 0 || idx >= MYQUEUE_CAP) return;
    for (unsigned lane = 0; lane <= _MSG_LANE_BACKGROUND; ++lane)
        bitset_clear_slot(q->lane_slot_bits[lane], idx);
    for (unsigned bit = 0; bit < MYQUEUE_QS_INDEX_BITS; ++bit)
        bitset_clear_slot(q->qs_slot_bits[bit], idx);
    for (unsigned kind = 0; kind <= _MSG_INPUT_MOUSE_WHEEL; ++kind)
        bitset_clear_slot(q->input_kind_slot_bits[kind], idx);
    for (unsigned bit = 0; bit < MYQUEUE_FILTER_INDEX_BITS; ++bit)
        bitset_clear_slot(q->filter_stage_slot_bits[bit], idx);
    for (unsigned bucket = 0; bucket < MYQUEUE_HWND_BUCKET_COUNT; ++bucket)
        bitset_clear_slot(q->hwnd_bucket_slot_bits[bucket], idx);
    for (unsigned bucket = 0; bucket < MYQUEUE_MSG_BUCKET_COUNT; ++bucket)
        bitset_clear_slot(q->msg_bucket_slot_bits[bucket], idx);
}

static uint32_t current_qs_from_slot_index_locked(const MyMessageQueue* q)
{
    uint32_t bits = 0;
    if (!q) return 0;
    for (unsigned bit = 0; bit < MYQUEUE_QS_INDEX_BITS; ++bit) {
        if (bitset_any(q->qs_slot_bits[bit]))
            bits |= (1u << bit);
    }
    return bits;
}

static uint32_t slot_index_add_hot_locked(MyMessageQueue* q, int idx, int markChanged)
{
    if (!q || idx < 0 || idx >= MYQUEUE_CAP) return 0;
    const _MessageHot* h = &q->hot[idx];
    if (!hot_is_queue_visible(h)) return 0;

    uint32_t lane = hot_effective_lane(h);
    if (lane <= _MSG_LANE_BACKGROUND)
        bitset_set_slot(q->lane_slot_bits[lane], idx);

    uint32_t qs = qs_bits_for_hot(h);
    for (unsigned bit = 0; bit < MYQUEUE_QS_INDEX_BITS; ++bit) {
        if (qs & (1u << bit))
            bitset_set_slot(q->qs_slot_bits[bit], idx);
    }

    uint32_t kind = h->input_kind ? h->input_kind : mymsg_default_input_kind(h->msg);
    if (kind <= _MSG_INPUT_MOUSE_WHEEL)
        bitset_set_slot(q->input_kind_slot_bits[kind], idx);

    uint32_t reason = h->route_reason ? h->route_reason : mymsg_default_route_reason(h->msg, h->flags, kind);
    uint32_t stages = h->filter_stages ? h->filter_stages : mymsg_default_filter_stages(h->msg, lane, kind, reason);
    for (unsigned bit = 0; bit < MYQUEUE_FILTER_INDEX_BITS; ++bit) {
        if (stages & (1u << bit))
            bitset_set_slot(q->filter_stage_slot_bits[bit], idx);
    }

    if (h->hwnd)
        bitset_set_slot(q->hwnd_bucket_slot_bits[hwnd_bucket_for_hwnd(h->hwnd)], idx);
    bitset_set_slot(q->msg_bucket_slot_bits[msg_bucket_for_msg(h->msg)], idx);

    q->current_qs |= qs;
    if (markChanged) q->changed_qs |= qs;
    return qs;
}

static void rebuild_slot_indexes_locked(MyMessageQueue* q)
{
    if (!q) return;
    memset(q->lane_slot_bits, 0, sizeof(q->lane_slot_bits));
    memset(q->qs_slot_bits, 0, sizeof(q->qs_slot_bits));
    memset(q->input_kind_slot_bits, 0, sizeof(q->input_kind_slot_bits));
    memset(q->filter_stage_slot_bits, 0, sizeof(q->filter_stage_slot_bits));
    memset(q->hwnd_bucket_slot_bits, 0, sizeof(q->hwnd_bucket_slot_bits));
    memset(q->msg_bucket_slot_bits, 0, sizeof(q->msg_bucket_slot_bits));
    q->current_qs = 0;
    for (int n = 0; n < q->count; ++n) {
        int idx = (q->head + n) % MYQUEUE_CAP;
        slot_index_add_hot_locked(q, idx, 0);
    }
    q->current_qs = current_qs_from_slot_index_locked(q);
    q->slot_index_rebuilds++;
}

static void qs_candidate_slots_locked(const MyMessageQueue* q, uint32_t qsMask, uint64_t out[MYQUEUE_SLOT_WORDS])
{
    if (!out) return;
    bitset_zero_words(out);
    if (!q || !qsMask) return;
    for (unsigned bit = 0; bit < MYQUEUE_QS_INDEX_BITS; ++bit) {
        if (qsMask & (1u << bit))
            bitset_or_into(out, q->qs_slot_bits[bit]);
    }
}

static void input_kind_candidate_slots_locked(const MyMessageQueue* q, uint32_t inputKindMask, uint64_t out[MYQUEUE_SLOT_WORDS])
{
    if (!out) return;
    bitset_zero_words(out);
    if (!q || !inputKindMask) return;
    for (unsigned kind = 0; kind <= _MSG_INPUT_MOUSE_WHEEL; ++kind) {
        if (inputKindMask & _MSG_INPUT_KIND_BIT(kind))
            bitset_or_into(out, q->input_kind_slot_bits[kind]);
    }
}

static void filter_stage_candidate_slots_locked(const MyMessageQueue* q, uint32_t filterStageMask, uint64_t out[MYQUEUE_SLOT_WORDS])
{
    if (!out) return;
    bitset_zero_words(out);
    if (!q || !filterStageMask) return;
    for (unsigned bit = 0; bit < MYQUEUE_FILTER_INDEX_BITS; ++bit) {
        if (filterStageMask & (1u << bit))
            bitset_or_into(out, q->filter_stage_slot_bits[bit]);
    }
}

static void hwnd_bucket_candidate_slots_locked(const MyMessageQueue* q, uint8_t hwndBucket, uint64_t out[MYQUEUE_SLOT_WORDS])
{
    if (!out) return;
    bitset_zero_words(out);
    if (!q || hwndBucket >= MYQUEUE_HWND_BUCKET_COUNT) return;
    bitset_copy_words(out, q->hwnd_bucket_slot_bits[hwndBucket]);
}

static void msg_bucket_candidate_slots_locked(const MyMessageQueue* q, uint64_t bucketMask, uint64_t out[MYQUEUE_SLOT_WORDS])
{
    if (!out) return;
    bitset_zero_words(out);
    if (!q || !bucketMask) return;
    for (unsigned bucket = 0; bucket < MYQUEUE_MSG_BUCKET_COUNT; ++bucket) {
        if (bucketMask & (1ull << bucket))
            bitset_or_into(out, q->msg_bucket_slot_bits[bucket]);
    }
}


static void queue_select_plan_add_op(_QueueSelectPlan* plan, uint8_t op)
{
    if (!plan || !op || plan->opCount >= MYQUEUE_SELECT_OP_MAX) return;
    plan->ops[plan->opCount++] = op;
}

static int queue_select_compile_plan_internal(const _QueueSelect* select, _QueueSelectPlan* out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->cbSize = sizeof(*out);
    out->laneMask = _MSG_LANE_MASK_ALL;
    out->qsMask = QS_ALLINPUT;

    if (select) {
        out->fields = select->fields;
        out->hwnd = select->hwnd;
        out->msgMin = select->msgMin;
        out->msgMax = select->msgMax;
        out->laneMask = (select->fields & _QUEUE_SELECT_LANES) ? select->laneMask : _MSG_LANE_MASK_ALL;
        out->inputKindMask = select->inputKindMask;
        out->filterStageMask = select->filterStageMask;
        out->qsMask = (select->fields & _QUEUE_SELECT_QS) ? select->qsMask : QS_ALLINPUT;

        if (select->fields & _QUEUE_SELECT_QS) {
            out->indexSourceMask |= _QUEUE_SELECT_SOURCE_QS;
            queue_select_plan_add_op(out, _QUEUE_SELECT_OP_QS);
        }
        if (select->fields & _QUEUE_SELECT_INPUT_KIND)
            out->indexSourceMask |= _QUEUE_SELECT_SOURCE_INPUT_KIND;
        if (select->fields & _QUEUE_SELECT_FILTER_STAGE)
            out->indexSourceMask |= _QUEUE_SELECT_SOURCE_FILTER_STAGE;
        if (select->fields & _QUEUE_SELECT_HWND) {
            out->hwndBucket = (uint8_t)hwnd_bucket_for_hwnd(select->hwnd);
            out->indexSourceMask |= _QUEUE_SELECT_SOURCE_HWND_BUCKET;
            queue_select_plan_add_op(out, _QUEUE_SELECT_OP_HWND);
        }
        if (select->fields & _QUEUE_SELECT_MSG_RANGE) {
            out->msgBucketMask = msg_bucket_mask_for_range(select->msgMin, select->msgMax);
            if (out->msgBucketMask)
                out->indexSourceMask |= _QUEUE_SELECT_SOURCE_MSG_BUCKET;
            queue_select_plan_add_op(out, _QUEUE_SELECT_OP_MSG_RANGE);
        }
        /* v234-v235: INPUT_KIND/FILTER_STAGE/HWND/MESSAGE buckets are explicit
           index sources in the compiled plan.  They narrow the slot bitset
           before exact predicate ops see a hot entry, so the candidate path
           stays a tiny table-driven state machine instead of an if ladder. */
    }

    return (int)out->opCount;
}

int myqueue_debug_compile_select_plan(const _QueueSelect* select, _QueueSelectPlan* out)
{
    return queue_select_compile_plan_internal(select, out);
}

static void queue_select_normalize_for_cache(const _QueueSelect* select, _QueueSelect* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (select) *out = *select;
}

static int queue_select_cache_key_equal(const _QueueSelect* a, const _QueueSelect* b)
{
    return a && b &&
           a->fields == b->fields &&
           a->hwnd == b->hwnd &&
           a->msgMin == b->msgMin &&
           a->msgMax == b->msgMax &&
           a->laneMask == b->laneMask &&
           a->inputKindMask == b->inputKindMask &&
           a->filterStageMask == b->filterStageMask &&
           a->qsMask == b->qsMask &&
           a->remove == b->remove;
}

static const _QueueSelectPlan* queue_select_get_cached_plan_locked(MyMessageQueue* q, const _QueueSelect* select)
{
    _QueueSelect key;
    queue_select_normalize_for_cache(select, &key);

    if (q && q->selector_plan_cache_valid &&
        queue_select_cache_key_equal(&q->selector_plan_cache_select, &key)) {
        q->selector_plan_cache_hits++;
        return &q->selector_plan_cache;
    }

    if (!q) return NULL;
    q->selector_plan_cache_select = key;
    queue_select_compile_plan_internal(select, &q->selector_plan_cache);
    q->selector_plan_cache_valid = 1;
    q->selector_plan_builds++;
    return &q->selector_plan_cache;
}

static inline int queue_select_pred_qs(const _MessageHot* h, const _QueueSelectPlan* plan)
{
    if (!h || !plan) return 0;
    return (qs_bits_for_hot(h) & plan->qsMask) ? 1 : 0;
}

static inline int queue_select_pred_hwnd(const _MessageHot* h, const _QueueSelectPlan* plan)
{
    return h && plan && h->hwnd == plan->hwnd;
}

static inline int queue_select_pred_msg_range(const _MessageHot* h, const _QueueSelectPlan* plan)
{
    return h && plan && h->msg >= plan->msgMin && h->msg <= plan->msgMax;
}

static inline int queue_select_pred_input_kind(const _MessageHot* h, const _QueueSelectPlan* plan)
{
    if (!h || !plan) return 0;
    uint32_t kind = h->input_kind ? h->input_kind : mymsg_default_input_kind(h->msg);
    return (plan->inputKindMask & _MSG_INPUT_KIND_BIT(kind)) ? 1 : 0;
}

static inline int queue_select_pred_filter_stage(const _MessageHot* h, const _QueueSelectPlan* plan)
{
    if (!h || !plan) return 0;
    uint32_t kind = h->input_kind ? h->input_kind : mymsg_default_input_kind(h->msg);
    uint32_t lane = hot_effective_lane(h);
    uint32_t reason = h->route_reason ? h->route_reason : mymsg_default_route_reason(h->msg, h->flags, kind);
    uint32_t stages = h->filter_stages ? h->filter_stages : mymsg_default_filter_stages(h->msg, lane, kind, reason);
    return (stages & plan->filterStageMask) ? 1 : 0;
}

static int message_matches_select_plan(MyMessageQueue* q, const _MessageHot* h, const _QueueSelectPlan* plan)
{
    if (!h || !plan) return 0;
    if (!hot_is_queue_visible(h)) return 0;

    for (uint8_t i = 0; i < plan->opCount; ++i) {
        uint8_t op = plan->ops[i];
        int ok = 0;
        if (q) q->selector_plan_predicate_ops++;

        switch (op) {
            case _QUEUE_SELECT_OP_QS:
                /* v242: QS is an exact slot index source, not a hash bucket.
                   If the plan already intersected the QS bitset, avoid
                   recomputing mymsg_qs_bits() for every surviving candidate. */
                ok = (plan->indexSourceMask & _QUEUE_SELECT_SOURCE_QS) ? 1 : queue_select_pred_qs(h, plan);
                break;
            case _QUEUE_SELECT_OP_HWND:
                ok = queue_select_pred_hwnd(h, plan);
                break;
            case _QUEUE_SELECT_OP_MSG_RANGE:
                ok = queue_select_pred_msg_range(h, plan);
                break;
            case _QUEUE_SELECT_OP_INPUT_KIND:
                ok = queue_select_pred_input_kind(h, plan);
                break;
            case _QUEUE_SELECT_OP_FILTER_STAGE:
                ok = queue_select_pred_filter_stage(h, plan);
                break;
            default:
                ok = 0;
                break;
        }
        if (!ok) {
            if (q) q->selector_plan_candidate_rejects++;
            return 0;
        }
    }

    if (q) q->selector_plan_fast_accepts++;
    return 1;
}

static int bitset_take_first_from_head(uint64_t words[MYQUEUE_SLOT_WORDS], int head)
{
    if (!words) return -1;
    if (head < 0 || head >= MYQUEUE_CAP) head = 0;

    unsigned headWord = (unsigned)head >> 6;
    unsigned headBit = (unsigned)head & 63u;

    for (unsigned w = headWord; w < MYQUEUE_SLOT_WORDS; ++w) {
        uint64_t m = words[w];
        if (w == headWord) m &= (~0ull << headBit);
        if (m) {
            unsigned b = (unsigned)__builtin_ctzll(m);
            int idx = (int)(w * 64u + b);
            if (idx >= MYQUEUE_CAP) return -1;
            words[w] &= ~(1ull << b);
            return idx;
        }
    }

    for (unsigned w = 0; w <= headWord && w < MYQUEUE_SLOT_WORDS; ++w) {
        uint64_t m = words[w];
        if (w == headWord) {
            if (headBit == 0) m = 0;
            else m &= ((1ull << headBit) - 1ull);
        }
        if (m) {
            unsigned b = (unsigned)__builtin_ctzll(m);
            int idx = (int)(w * 64u + b);
            if (idx >= MYQUEUE_CAP) return -1;
            words[w] &= ~(1ull << b);
            return idx;
        }
    }
    return -1;
}

static void message_split_hot_cold(const MyMessage* m, _MessageHot* hot, _MessageCold* cold)
{
    if (!m || !hot || !cold) return;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));

    hot->hwnd = m->hwnd;
    hot->msg = m->msg;
    hot->wparam = m->wparam;
    hot->lparam = m->lparam;
    hot->priority = m->priority;
    hot->flags = m->flags;
    hot->lane = m->lane;
    hot->state = m->state;
    hot->input_kind = m->input_kind;
    hot->route_state = m->route_state;
    hot->filter_state = m->filter_state;
    hot->filter_stage = m->filter_stage;
    hot->route_flags = m->route_flags;
    hot->route_reason = m->route_reason;
    hot->filter_stages = m->filter_stages;
    hot->route_action = m->route_action;

    cold->size = m->size ? m->size : sizeof(MyMessage);
    cold->type = m->type;
    cold->sender_pid = m->sender_pid;
    cold->sender_tid = m->sender_tid;
    cold->target_pid = m->target_pid;
    cold->target_tid = m->target_tid;
    cold->route_hwnd = m->route_hwnd;
    cold->capture_hwnd = m->capture_hwnd;
    cold->focus_hwnd = m->focus_hwnd;
    cold->hit_hwnd = m->hit_hwnd;
    cold->seq = m->seq;
    cold->timestamp_ns = m->timestamp_ns;
    cold->section_id = m->section_id;
    cold->payload_offset = m->payload_offset;
    cold->payload_size = m->payload_size;
    cold->sync_ctx = m->sync_ctx;
}

static void message_merge_hot_cold(const _MessageHot* hot, const _MessageCold* cold, MyMessage* out)
{
    if (!hot || !cold || !out) return;
    memset(out, 0, sizeof(*out));
    out->size = cold->size ? cold->size : sizeof(MyMessage);
    out->type = cold->type;
    out->sender_pid = cold->sender_pid;
    out->sender_tid = cold->sender_tid;
    out->target_pid = cold->target_pid;
    out->target_tid = cold->target_tid;
    out->hwnd = hot->hwnd;
    out->msg = hot->msg;
    out->wparam = hot->wparam;
    out->lparam = hot->lparam;
    out->priority = hot->priority;
    out->flags = hot->flags;
    out->lane = hot->lane;
    out->state = hot->state;
    out->input_kind = hot->input_kind;
    out->route_state = hot->route_state;
    out->filter_state = hot->filter_state;
    out->filter_stage = hot->filter_stage;
    out->route_flags = hot->route_flags;
    out->route_reason = hot->route_reason;
    out->filter_stages = hot->filter_stages;
    out->route_action = hot->route_action;
    out->route_hwnd = cold->route_hwnd;
    out->capture_hwnd = cold->capture_hwnd;
    out->focus_hwnd = cold->focus_hwnd;
    out->hit_hwnd = cold->hit_hwnd;
    out->seq = cold->seq;
    out->timestamp_ns = cold->timestamp_ns;
    out->section_id = cold->section_id;
    out->payload_offset = cold->payload_offset;
    out->payload_size = cold->payload_size;
    out->sync_ctx = cold->sync_ctx;
}

static void message_from_queue_slot(const MyMessageQueue* q, int idx, MyMessage* out)
{
    if (!q || !out || idx < 0 || idx >= MYQUEUE_CAP) return;
    message_merge_hot_cold(&q->hot[idx], &q->cold[idx], out);
}

static uint32_t recompute_qs_locked(const MyMessageQueue* q)
{
    return current_qs_from_slot_index_locked(q);
}

static void refresh_qs_locked(MyMessageQueue* q)
{
    if (!q) return;
    q->current_qs = recompute_qs_locked(q);
}

static int same_coalesce_key_hot(const _MessageHot* aHot, const _MessageCold* aCold, const MyMessage* b)
{
    return aHot && aCold && b &&
           aHot->hwnd == b->hwnd &&
           aHot->msg  == b->msg  &&
           aCold->target_pid == b->target_pid &&
           aCold->target_tid == b->target_tid;
}

void myqueue_init(MyMessageQueue* q)
{
    memset(q, 0, sizeof(*q));
    q->next_seq = 1;
    pthread_mutex_init(&q->lock, NULL);

    /* v149: keep queue waits on the same monotonic timebase as USER32 timers. */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#ifdef CLOCK_MONOTONIC
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&q->wake, &attr);
    pthread_condattr_destroy(&attr);
}

void myqueue_destroy(MyMessageQueue* q)
{
    pthread_cond_destroy(&q->wake);
    pthread_mutex_destroy(&q->lock);
}

static int find_coalesce_slot(MyMessageQueue* q, const MyMessage* m)
{
    for (int n = 0; n < q->count; n++) {
        int idx = (q->head + n) % MYQUEUE_CAP;
        if (same_coalesce_key_hot(&q->hot[idx], &q->cold[idx], m))
            return idx;
    }
    return -1;
}

static int find_match_slot_select(MyMessageQueue* q, const _QueueSelect* select)
{
    if (!q || q->count <= 0) return -1;

    const _QueueSelectPlan* cachedPlan = queue_select_get_cached_plan_locked(q, select);
    if (!cachedPlan) return -1;
    _QueueSelectPlan plan = *cachedPlan;

    /* v242: current_qs is maintained incrementally by slot-index add/remove and
       rebuild paths.  Selector hot paths should not rescan every QS bitset just
       to reject an empty mask. */
    if ((plan.fields & _QUEUE_SELECT_QS) && plan.qsMask && !(q->current_qs & plan.qsMask)) {
        q->qs_prefilter_skips++;
        return -1;
    }

    uint64_t qsCandidates[MYQUEUE_SLOT_WORDS] = {0};
    uint64_t inputKindCandidates[MYQUEUE_SLOT_WORDS] = {0};
    uint64_t filterStageCandidates[MYQUEUE_SLOT_WORDS] = {0};
    uint64_t hwndBucketCandidates[MYQUEUE_SLOT_WORDS] = {0};
    uint64_t msgBucketCandidates[MYQUEUE_SLOT_WORDS] = {0};
    int useQsIndex = (plan.indexSourceMask & _QUEUE_SELECT_SOURCE_QS) ? 1 : 0;
    int useInputKindIndex = (plan.indexSourceMask & _QUEUE_SELECT_SOURCE_INPUT_KIND) ? 1 : 0;
    int useFilterStageIndex = (plan.indexSourceMask & _QUEUE_SELECT_SOURCE_FILTER_STAGE) ? 1 : 0;
    int useHwndBucketIndex = (plan.indexSourceMask & _QUEUE_SELECT_SOURCE_HWND_BUCKET) ? 1 : 0;
    int useMsgBucketIndex = (plan.indexSourceMask & _QUEUE_SELECT_SOURCE_MSG_BUCKET) ? 1 : 0;
    if (useQsIndex)
        qs_candidate_slots_locked(q, plan.qsMask, qsCandidates);
    if (useInputKindIndex)
        input_kind_candidate_slots_locked(q, plan.inputKindMask, inputKindCandidates);
    if (useFilterStageIndex)
        filter_stage_candidate_slots_locked(q, plan.filterStageMask, filterStageCandidates);
    if (useHwndBucketIndex)
        hwnd_bucket_candidate_slots_locked(q, plan.hwndBucket, hwndBucketCandidates);
    if (useMsgBucketIndex)
        msg_bucket_candidate_slots_locked(q, plan.msgBucketMask, msgBucketCandidates);

    /* v233: the selector is now a two-stage state machine:
       1) candidate source indexes (lane, QS, input-kind, filter-stage bitsets),
       2) compact predicate op-table for fields that cannot be slot-indexed yet.
       Lane order and FIFO are still the externally visible policy. */
    static const uint32_t order[] = {
        _MSG_LANE_SEND,
        _MSG_LANE_INPUT,
        _MSG_LANE_WINDOW,
        _MSG_LANE_POSTED,
        _MSG_LANE_TIMER,
        _MSG_LANE_BACKGROUND
    };
    for (unsigned oi = 0; oi < sizeof(order)/sizeof(order[0]); ++oi) {
        uint32_t lane = order[oi];
        if ((plan.fields & _QUEUE_SELECT_LANES) &&
            !(plan.laneMask & _MSG_LANE_BIT(lane)))
            continue;

        uint64_t candidates[MYQUEUE_SLOT_WORDS];
        bitset_copy_words(candidates, q->lane_slot_bits[lane]);
        if (useQsIndex) bitset_and_into(candidates, qsCandidates);
        if (useInputKindIndex) bitset_and_into(candidates, inputKindCandidates);
        if (useFilterStageIndex) bitset_and_into(candidates, filterStageCandidates);
        if (useHwndBucketIndex) bitset_and_into(candidates, hwndBucketCandidates);
        if (useMsgBucketIndex) bitset_and_into(candidates, msgBucketCandidates);

        int idx = bitset_take_first_from_head(candidates, q->head);
        if (idx < 0) {
            q->indexed_empty_skips++;
            continue;
        }
        do {
            q->indexed_candidate_probes++;
            if (message_matches_select_plan(q, &q->hot[idx], &plan))
                return idx;
            idx = bitset_take_first_from_head(candidates, q->head);
        } while (idx >= 0);
    }
    return -1;
}

static void remove_slot(MyMessageQueue* q, int idx, MyMessage* out)
{
    if (out) message_from_queue_slot(q, idx, out);

    // Ringbuffer-Lücke schließen: für MYQUEUE_CAP=256 ist das billig und hält die Logik simpel.
    while (idx != q->head) {
        int prev = (idx - 1 + MYQUEUE_CAP) % MYQUEUE_CAP;
        q->hot[idx] = q->hot[prev];
        q->cold[idx] = q->cold[prev];
        idx = prev;
    }
    q->head = (q->head + 1) % MYQUEUE_CAP;
    q->count--;
    q->stats.current_depth = (uint32_t)q->count;
    rebuild_slot_indexes_locked(q);
}

int myqueue_post(MyMessageQueue* q, const MyMessage* in)
{
    MyMessage m = *in;
    if (!m.size) m.size = sizeof(MyMessage);
    if (!m.timestamp_ns) m.timestamp_ns = myos_now_ns();
    m.priority = mymsg_default_priority(m.msg);
    /* lane 0 is a valid SEND lane, but also the zero-initialized default.
       Preserve explicit SEND through MYMSG_FLAG_SYNC_REQ; otherwise classify
       unset messages from their public WM_* value. */
    if (m.flags & MYMSG_FLAG_SYNC_REQ) m.lane = _MSG_LANE_SEND;
    else m.lane = (m.lane != 0 && m.lane <= _MSG_LANE_BACKGROUND) ? m.lane : mymsg_default_lane(m.msg, m.flags);
    m.state = _MSG_STATE_QUEUED;
    m.input_kind = m.input_kind ? m.input_kind : mymsg_default_input_kind(m.msg);
    if (!m.route_reason)
        m.route_reason = mymsg_default_route_reason(m.msg, m.flags, m.input_kind);
    if (!m.route_action)
        m.route_action = mymsg_required_hwnd_action_for_route(m.lane, m.input_kind, m.route_reason);
    if (!m.filter_stages)
        m.filter_stages = mymsg_default_filter_stages(m.msg, m.lane, m.input_kind, m.route_reason);
    if (!m.filter_state)
        m.filter_state = _MSG_FILTER_STATE_PENDING;
    if (!m.filter_stage)
        m.filter_stage = mymsg_first_filter_stage(m.filter_stages);
    if (!m.route_hwnd) m.route_hwnd = m.hwnd;
    if (!m.hit_hwnd && (m.route_reason & (_MSG_ROUTE_REASON_HITTEST | _MSG_ROUTE_REASON_HOVER)))
        m.hit_hwnd = m.hwnd;
    if (!m.route_state && m.lane == _MSG_LANE_INPUT) {
        if (m.route_reason & _MSG_ROUTE_REASON_CAPTURE) m.route_state = _MSG_ROUTE_CAPTURED;
        else if (m.route_reason & _MSG_ROUTE_REASON_FOCUS) m.route_state = _MSG_ROUTE_FOCUS;
        else m.route_state = _MSG_ROUTE_TARGET_RESOLVED;
    }

    pthread_mutex_lock(&q->lock);

    if (mymsg_is_coalescable(m.msg)) {
        int idx = find_coalesce_slot(q, &m);
        if (idx >= 0) {
            uint64_t old_seq = q->cold[idx].seq;
            m.seq = old_seq;
            m.flags |= MYMSG_FLAG_COALESCED | MYMSG_FLAG_ASYNC;
            m.state = _MSG_STATE_COALESCED;
            slot_index_clear_slot_locked(q, idx);
            message_split_hot_cold(&m, &q->hot[idx], &q->cold[idx]);
            q->stats.coalesced++;
            slot_index_add_hot_locked(q, idx, 1);
            refresh_qs_locked(q);
            pthread_cond_signal(&q->wake);
            pthread_mutex_unlock(&q->lock);
            return 0;
        }
    }

    if (q->count >= MYQUEUE_CAP) {
        q->stats.dropped++;
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    m.seq = q->next_seq++;
    m.flags |= MYMSG_FLAG_ASYNC;
    int post_idx = q->tail;
    message_split_hot_cold(&m, &q->hot[post_idx], &q->cold[post_idx]);
    slot_index_add_hot_locked(q, post_idx, 1);
    q->tail = (q->tail + 1) % MYQUEUE_CAP;
    q->count++;
    q->stats.posted++;
    q->stats.current_depth = (uint32_t)q->count;
    if ((uint64_t)q->count > q->stats.peak_depth)
        q->stats.peak_depth = (uint64_t)q->count;
    pthread_cond_signal(&q->wake);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int myqueue_peek_select(MyMessageQueue* q, MyMessage* out, const _QueueSelect* select)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->lock);
    int idx = find_match_slot_select(q, select);
    if (idx < 0) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }
    int remove = (select && select->remove) ? 1 : 0;
    if (remove)
        remove_slot(q, idx, out);
    else if (out)
        message_from_queue_slot(q, idx, out);
    pthread_mutex_unlock(&q->lock);
    return 1;
}

int myqueue_peek_match(MyMessageQueue* q, MyMessage* out,
                       HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                       int remove)
{
    _QueueSelect select;
    myqueue_make_select(&select, hwndFilter, wMsgFilterMin, wMsgFilterMax, remove);
    return myqueue_peek_select(q, out, &select);
}

int myqueue_wait_select(MyMessageQueue* q, MyMessage* out, const _QueueSelect* select, int timeout_ms)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->lock);

    for (;;) {
        int idx = find_match_slot_select(q, select);
        if (idx >= 0) {
            int remove = (select && select->remove) ? 1 : 0;
            if (remove)
                remove_slot(q, idx, out);
            else if (out)
                message_from_queue_slot(q, idx, out);
            pthread_mutex_unlock(&q->lock);
            return 1;
        }

        if (timeout_ms == 0) {
            pthread_mutex_unlock(&q->lock);
            return 0;
        }

        if (timeout_ms < 0) {
            pthread_cond_wait(&q->wake, &q->lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec  += timeout_ms / 1000;
            ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            int r = pthread_cond_timedwait(&q->wake, &q->lock, &ts);
            if (r == ETIMEDOUT) {
                pthread_mutex_unlock(&q->lock);
                return 0;
            }
        }
    }
}

int myqueue_wait_match(MyMessageQueue* q, MyMessage* out,
                       HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                       int remove, int timeout_ms)
{
    _QueueSelect select;
    myqueue_make_select(&select, hwndFilter, wMsgFilterMin, wMsgFilterMax, remove);
    select.fields |= _QUEUE_SELECT_WAIT;
    return myqueue_wait_select(q, out, &select, timeout_ms);
}

int myqueue_wait_select_or_wake(MyMessageQueue* q, MyMessage* out, const _QueueSelect* select, int timeout_ms)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->lock);

    int idx = find_match_slot_select(q, select);
    if (idx >= 0) {
        int remove = (select && select->remove) ? 1 : 0;
        if (remove)
            remove_slot(q, idx, out);
        else if (out)
            message_from_queue_slot(q, idx, out);
        pthread_mutex_unlock(&q->lock);
        return 1;
    }

    if (timeout_ms == 0) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }

    if (timeout_ms < 0) {
        pthread_cond_wait(&q->wake, &q->lock);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&q->wake, &q->lock, &ts);
    }

    idx = find_match_slot_select(q, select);
    if (idx >= 0) {
        int remove = (select && select->remove) ? 1 : 0;
        if (remove)
            remove_slot(q, idx, out);
        else if (out)
            message_from_queue_slot(q, idx, out);
        pthread_mutex_unlock(&q->lock);
        return 1;
    }

    pthread_mutex_unlock(&q->lock);
    return 0;
}

int myqueue_wait_match_or_wake(MyMessageQueue* q, MyMessage* out,
                               HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                               int remove, int timeout_ms)
{
    _QueueSelect select;
    myqueue_make_select(&select, hwndFilter, wMsgFilterMin, wMsgFilterMax, remove);
    select.fields |= _QUEUE_SELECT_WAIT;
    return myqueue_wait_select_or_wake(q, out, &select, timeout_ms);
}

void myqueue_wake(MyMessageQueue* q)
{
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    pthread_cond_broadcast(&q->wake);
    pthread_mutex_unlock(&q->lock);
}

int myqueue_remove_matching(MyMessageQueue* q, HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    if (!q) return 0;
    int removed = 0;
    pthread_mutex_lock(&q->lock);
    for (;;) {
        _QueueSelect select;
        myqueue_make_select(&select, hwndFilter, wMsgFilterMin, wMsgFilterMax, 1);
        int idx = find_match_slot_select(q, &select);
        if (idx < 0) break;
        remove_slot(q, idx, NULL);
        removed++;
    }
    if (removed) pthread_cond_broadcast(&q->wake);
    pthread_mutex_unlock(&q->lock);
    return removed;
}

int myqueue_get(MyMessageQueue* q, MyMessage* out)
{
    return myqueue_peek_match(q, out, 0, 0, 0, 1);
}

int myqueue_get_wait(MyMessageQueue* q, MyMessage* out, int timeout_ms)
{
    return myqueue_wait_match(q, out, 0, 0, 0, 1, timeout_ms);
}

uint32_t myqueue_peek_queue_status(MyMessageQueue* q)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->lock);
    refresh_qs_locked(q);
    uint32_t bits = q->current_qs;
    pthread_mutex_unlock(&q->lock);
    return bits;
}

int myqueue_has_queue_status(MyMessageQueue* q, UINT flags)
{
    if (!q || !flags) return 0;
    pthread_mutex_lock(&q->lock);
    refresh_qs_locked(q);
    int ready = (q->current_qs & flags) ? 1 : 0;
    pthread_mutex_unlock(&q->lock);
    return ready;
}

DWORD myqueue_get_queue_status(MyMessageQueue* q, UINT flags)
{
    if (!q || !flags) return 0;
    pthread_mutex_lock(&q->lock);
    refresh_qs_locked(q);
    DWORD current = q->current_qs & flags;
    DWORD changed = q->changed_qs & flags;
    q->changed_qs &= ~flags;
    pthread_mutex_unlock(&q->lock);
    return (changed << 16) | current;
}


void myqueue_mark_dispatched(MyMessageQueue* q)
{
    pthread_mutex_lock(&q->lock);
    q->stats.dispatched++;
    pthread_mutex_unlock(&q->lock);
}

void myqueue_get_stats(MyMessageQueue* q, MyQueueStats* out)
{
    if (!out) return;
    pthread_mutex_lock(&q->lock);
    *out = q->stats;
    out->current_depth = (uint32_t)q->count;
    pthread_mutex_unlock(&q->lock);
}
