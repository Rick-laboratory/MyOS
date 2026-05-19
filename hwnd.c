#include "hwnd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include "myobject.h"

/* v133: queued HWND dispatch must run inside the target window owner
   runtime context.  v131 made HWND access policy strict; without this,
   shell/Desktop/Taskbar WndProcs executed from the compositor thread had no
   ambient Capability and legitimate shell reads like GetWindowLongPtrA failed
   with ERROR_ACCESS_DENIED.  Keep hwnd.c independent of USER32 headers by
   declaring the private context switch hooks here. */
extern BOOL MyWinEnterProcessContext(DWORD dwProcessId);
extern BOOL MyWinLeaveProcessContext(void);

typedef struct MySyncSendContext {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int done;
    int timed_out;
    LRESULT result;
} MySyncSendContext;

static void hwnd_touch_visual_locked(HWNDManager* mgr, HWND hwnd, UINT msg);

static int cond_wait_ms(pthread_cond_t* cond, pthread_mutex_t* lock, int timeout_ms)
{
    if (timeout_ms < 0) return pthread_cond_wait(cond, lock);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cond, lock, &ts);
}

// ─────────────────────────────────────────────
//  HWND Manager Implementierung
//  Der Manager ist das OS.
//  v14: HWNDs besitzen keine eigene Queue mehr. Die Queue gehört
//       dem Owner-UI-Thread (owner_pid/owner_tid). Das ist näher
//       am Win32-Modell: Fenster hängen an Thread-Message-Queues.
// ─────────────────────────────────────────────

void hwnd_manager_init(HWNDManager* mgr)
{
    memset(mgr, 0, sizeof(*mgr));
    mgr->next_hwnd     = 1;
    mgr->chain.next_id = 1;
    mgr->state_section.cbSize = sizeof(mgr->state_section);
    mgr->state_section.magic = MYOS_WINDOWSTATE_MAGIC;
    mgr->state_section.version = MYOS_WINDOWSTATE_LAYOUT_VERSION;
    mgr->state_section.capacity = MAX_HWNDS;
    mgr->state_section.activeCount = 0;
    mgr->state_section.destroyedCount = 0;
    mgr->state_section.updateSerial = 1;
    snprintf(mgr->state_section.sectionName, sizeof(mgr->state_section.sectionName), "%s", MYOS_WINDOWSTATE_SECTION_NAME);
    for (int i = 0; i < MAX_HWNDS; i++) {
        mgr->state_section.states[i].cbSize = sizeof(MyWindowState);
        mgr->state_section.states[i].seqBegin = 0;
        mgr->state_section.states[i].seqEnd = 0;
    }

    /* v240: initialize USER object free-stacks and PID/TID queue hash.  The
       old create paths scanned entries[]/threads[] for every allocation and
       queue lookup; hot USER dispatch now decodes HWND slots and resolves the
       owning UI queue through a small hash bucket. */
    mgr->hwnd_free_top = 0;
    for (int i = MAX_HWNDS - 1; i >= 0; --i)
        mgr->hwnd_free_stack[mgr->hwnd_free_top++] = i;

    mgr->thread_free_top = 0;
    for (int i = MAX_UI_THREADS - 1; i >= 0; --i)
        mgr->thread_free_stack[mgr->thread_free_top++] = i;

    for (int i = 0; i < HWND_THREAD_HASH_BUCKETS; ++i)
        mgr->thread_hash[i] = -1;
    for (int i = 0; i < MAX_UI_THREADS; ++i)
        mgr->threads[i].hash_next = -1;

    pthread_mutex_init(&mgr->lock, NULL);
}

void hwnd_manager_destroy(HWNDManager* mgr)
{
    for (int i = 0; i < MAX_UI_THREADS; i++) {
        if (mgr->threads[i].valid)
            myqueue_destroy(&mgr->threads[i].queue);
    }
    pthread_mutex_destroy(&mgr->lock);
}

static HWND hwnd_make_value(uint32_t slot, uint32_t generation)
{
    if (generation == 0) generation = 1;
    return (HWND)(_HWND_TAG | ((generation & 0xffu) << _HWND_GEN_SHIFT) | ((slot + 1u) & _HWND_SLOT_MASK));
}

static inline int hwnd_decode_fast(HWND hwnd, DWORD* outSlot, DWORD* outGeneration)
{
    if (MYOS_UNLIKELY((hwnd & _HWND_TAG_MASK) != _HWND_TAG)) return 0;
    DWORD encodedSlot = (DWORD)(hwnd & _HWND_SLOT_MASK);
    if (MYOS_UNLIKELY(encodedSlot == 0 || encodedSlot > MAX_HWNDS)) return 0;
    DWORD gen = (DWORD)((hwnd & _HWND_GEN_MASK) >> _HWND_GEN_SHIFT);
    if (MYOS_UNLIKELY(gen == 0)) return 0;
    if (outSlot) *outSlot = encodedSlot - 1u;
    if (outGeneration) *outGeneration = gen;
    return 1;
}

int hwnd_decode(HWND hwnd, DWORD* outSlot, DWORD* outGeneration)
{
    return hwnd_decode_fast(hwnd, outSlot, outGeneration);
}

static inline int find_entry_index(HWNDManager* mgr, HWND hwnd)
{
    DWORD slot = 0, gen = 0;
    if (MYOS_LIKELY(hwnd_decode_fast(hwnd, &slot, &gen))) {
        HWNDEntry* e = &mgr->entries[slot];
        if (MYOS_LIKELY(e->valid && e->hwnd == hwnd && e->hwnd_generation == gen &&
                        e->hwnd_state != _HWND_STATE_FREE &&
                        e->hwnd_state != _HWND_STATE_ZOMBIE))
            return (int)slot;
        return -1;
    }

    /* Legacy fallback for older/non-encoded diagnostic HWND values.  New v219
       HWNDs use the slot/generation fast path above. */
    for (int i = 0; i < MAX_HWNDS; i++) {
        if (mgr->entries[i].valid && mgr->entries[i].hwnd == hwnd)
            return i;
    }
    return -1;
}


static inline DWORD hwnd_state_action_mask(DWORD state)
{
    /* v241: table dispatch avoids a branchy switch in hwnd_query_action(). */
    static const DWORD masks[] = {
        0, /* FREE */
        0, /* RESERVED */
        _HWND_ACTION_QUERY | _HWND_ACTION_MESSAGE | _HWND_ACTION_MUTATE | _HWND_ACTION_DESTROY,
        _HWND_ACTION_QUERY | _HWND_ACTION_MESSAGE | _HWND_ACTION_MUTATE | _HWND_ACTION_SHOW |
        _HWND_ACTION_FOCUS | _HWND_ACTION_CAPTURE | _HWND_ACTION_PAINT | _HWND_ACTION_DESTROY |
        _HWND_ACTION_GEOMETRY | _HWND_ACTION_HITTEST,
        _HWND_ACTION_QUERY | _HWND_ACTION_MESSAGE | _HWND_ACTION_DESTROY,
        _HWND_ACTION_MESSAGE,
        0  /* ZOMBIE */
    };
    return (state < (DWORD)(sizeof(masks) / sizeof(masks[0]))) ? masks[state] : 0;
}

int hwnd_state_allows(DWORD state, DWORD action)
{
    return (hwnd_state_action_mask(state) & action) == action;
}

static inline unsigned hwnd_thread_hash_key(uint32_t pid, uint32_t tid)
{
    /* v241: one multiply per key plus a power-of-two mask; avoids DIV on the
       UI-thread queue lookup path. */
    uint32_t x = (pid * 2654435761u) ^ (tid * 2246822519u);
    x ^= x >> 16;
    return (unsigned)(x & HWND_THREAD_HASH_MASK);
}

static void hwnd_thread_hash_insert_locked(HWNDManager* mgr, int idx)
{
    if (!mgr || idx < 0 || idx >= MAX_UI_THREADS || !mgr->threads[idx].valid) return;
    unsigned bucket = hwnd_thread_hash_key(mgr->threads[idx].pid, mgr->threads[idx].tid);
    mgr->threads[idx].hash_next = mgr->thread_hash[bucket];
    mgr->thread_hash[bucket] = idx;
}

static void hwnd_thread_hash_remove_locked(HWNDManager* mgr, int idx)
{
    if (!mgr || idx < 0 || idx >= MAX_UI_THREADS) return;
    MyThreadQueue* tq = &mgr->threads[idx];
    if (!tq->valid) { tq->hash_next = -1; return; }
    unsigned bucket = hwnd_thread_hash_key(tq->pid, tq->tid);
    int* link = &mgr->thread_hash[bucket];
    while (*link >= 0 && *link < MAX_UI_THREADS) {
        if (*link == idx) {
            *link = mgr->threads[idx].hash_next;
            mgr->threads[idx].hash_next = -1;
            return;
        }
        link = &mgr->threads[*link].hash_next;
    }
    mgr->threads[idx].hash_next = -1;
}

static int hwnd_stack_contains(const int* stack, int top, int idx)
{
    for (int i = 0; i < top; ++i) if (stack[i] == idx) return 1;
    return 0;
}

static void hwnd_push_free_stack_locked(int* stack, int* top, int capacity, int idx)
{
    if (!stack || !top || idx < 0 || idx >= capacity || *top >= capacity) return;
    if (hwnd_stack_contains(stack, *top, idx)) return;
    stack[(*top)++] = idx;
}

static int hwnd_pop_hwnd_slot_locked(HWNDManager* mgr)
{
    if (!mgr) return -1;
    while (mgr->hwnd_free_top > 0) {
        int idx = mgr->hwnd_free_stack[--mgr->hwnd_free_top];
        if (idx >= 0 && idx < MAX_HWNDS && !mgr->entries[idx].valid) return idx;
    }
    for (int i = 0; i < MAX_HWNDS; ++i)
        if (!mgr->entries[i].valid) return i;
    return -1;
}

static void hwnd_push_hwnd_slot_locked(HWNDManager* mgr, int idx)
{
    if (!mgr || idx < 0 || idx >= MAX_HWNDS) return;
    hwnd_push_free_stack_locked(mgr->hwnd_free_stack, &mgr->hwnd_free_top, MAX_HWNDS, idx);
}

static int hwnd_pop_thread_slot_locked(HWNDManager* mgr)
{
    if (!mgr) return -1;
    while (mgr->thread_free_top > 0) {
        int idx = mgr->thread_free_stack[--mgr->thread_free_top];
        if (idx >= 0 && idx < MAX_UI_THREADS && !mgr->threads[idx].valid) return idx;
    }
    for (int i = 0; i < MAX_UI_THREADS; ++i)
        if (!mgr->threads[i].valid) return i;
    return -1;
}

static void hwnd_push_thread_slot_locked(HWNDManager* mgr, int idx)
{
    if (!mgr || idx < 0 || idx >= MAX_UI_THREADS) return;
    hwnd_push_free_stack_locked(mgr->thread_free_stack, &mgr->thread_free_top, MAX_UI_THREADS, idx);
}

static int find_thread_index_locked(HWNDManager* mgr, uint32_t pid, uint32_t tid)
{
    if (!mgr) return -1;
    unsigned bucket = hwnd_thread_hash_key(pid, tid);
    int idx = mgr->thread_hash[bucket];
    while (idx >= 0 && idx < MAX_UI_THREADS) {
        MyThreadQueue* tq = &mgr->threads[idx];
        if (MYOS_LIKELY(tq->valid && tq->pid == pid && tq->tid == tid)) return idx;
        idx = tq->hash_next;
    }
    return -1;
}

static MyThreadQueue* get_or_create_thread_queue_locked(HWNDManager* mgr, uint32_t pid, uint32_t tid)
{
    int idx = find_thread_index_locked(mgr, pid, tid);
    if (idx >= 0) return &mgr->threads[idx];

    idx = hwnd_pop_thread_slot_locked(mgr);
    if (idx < 0) return NULL;

    MyThreadQueue* tq = &mgr->threads[idx];
    memset(tq, 0, sizeof(*tq));
    tq->valid = 1;
    tq->hash_next = -1;
    tq->pid = pid;
    tq->tid = tid;
    tq->last_pump_ns = myos_now_ns();
    tq->next_timer_id = 1;
    snprintf(tq->name, sizeof(tq->name), "pid%u-tid%u", pid, tid);
    myqueue_init(&tq->queue);
    hwnd_thread_hash_insert_locked(mgr, idx);
    printf("[HWND] Create UI thread queue: pid=%u tid=%u\n", pid, tid);
    return tq;
}

static void maybe_destroy_thread_queue_locked(HWNDManager* mgr, uint32_t pid, uint32_t tid)
{
    int idx = find_thread_index_locked(mgr, pid, tid);
    if (idx < 0 || mgr->threads[idx].owner_window_count > 0) return;
    hwnd_thread_hash_remove_locked(mgr, idx);
    myqueue_destroy(&mgr->threads[idx].queue);
    memset(&mgr->threads[idx], 0, sizeof(mgr->threads[idx]));
    mgr->threads[idx].hash_next = -1;
    hwnd_push_thread_slot_locked(mgr, idx);
    printf("[HWND] Destroy UI thread queue: pid=%u tid=%u\n", pid, tid);
}


static int timer_message_matches(const MyUserTimer* t, HWND hwndFilter,
                                 UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    if (!t || !t->valid) return 0;
    if (hwndFilter && t->hwnd != hwndFilter) return 0;
    if (wMsgFilterMin || wMsgFilterMax) {
        if (WM_TIMER < wMsgFilterMin || WM_TIMER > wMsgFilterMax) return 0;
    }
    return 1;
}

static uint64_t timer_period_ns(UINT elapse_ms)
{
    UINT ms = elapse_ms ? elapse_ms : 1u;
    return (uint64_t)ms * 1000000ull;
}

static void timer_advance_phase(MyUserTimer* t, uint64_t now_ns)
{
    if (!t || !t->valid) return;
    uint64_t step = timer_period_ns(t->elapse_ms);
    if (!step) step = 1000000ull;

    /* v149: preserve timer phase.  Advance by period, then skip missed
       periods phase-aligned instead of anchoring the next tick to now. */
    t->next_due_ns += step;
    if (t->next_due_ns <= now_ns) {
        uint64_t missed = (now_ns - t->next_due_ns) / step + 1ull;
        t->next_due_ns += missed * step;
    }
    t->fire_count++;
}

static int synthesize_due_timer_locked(MyThreadQueue* tq, HWND hwndFilter,
                                       UINT wMsgFilterMin, UINT wMsgFilterMax,
                                       int remove, MyMessage* out)
{
    if (!tq || !out) return 0;
    uint64_t now = myos_now_ns();
    int best = -1;
    uint64_t best_due = UINT64_MAX;

    for (int i = 0; i < MAX_USER_TIMERS; ++i) {
        MyUserTimer* t = &tq->timers[i];
        if (!timer_message_matches(t, hwndFilter, wMsgFilterMin, wMsgFilterMax)) continue;
        if (t->next_due_ns > now) continue;
        if (t->next_due_ns < best_due) {
            best = i;
            best_due = t->next_due_ns;
        }
    }

    if (best < 0) return 0;

    MyUserTimer* t = &tq->timers[best];
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->type = 1;
    out->sender_pid = tq->pid;
    out->sender_tid = tq->tid;
    out->target_pid = tq->pid;
    out->target_tid = tq->tid;
    out->hwnd = t->hwnd;
    out->msg = WM_TIMER;
    out->wparam = (WPARAM)t->id;
    out->lparam = (LPARAM)t->callback;
    out->priority = MYMSG_PRIO_BACKGROUND;
    out->lane = _MSG_LANE_TIMER;
    out->state = _MSG_STATE_QUEUED;
    out->timestamp_ns = now;

    if (remove)
        timer_advance_phase(t, now);
    return 1;
}

static int next_due_timer_delay_ms_locked(MyThreadQueue* tq, HWND hwndFilter,
                                          UINT wMsgFilterMin, UINT wMsgFilterMax,
                                          uint64_t now_ns, int* out_ms)
{
    if (!tq || !out_ms) return 0;
    uint64_t best_due = UINT64_MAX;
    for (int i = 0; i < MAX_USER_TIMERS; ++i) {
        MyUserTimer* t = &tq->timers[i];
        if (!timer_message_matches(t, hwndFilter, wMsgFilterMin, wMsgFilterMax)) continue;
        if (t->next_due_ns < best_due) best_due = t->next_due_ns;
    }
    if (best_due == UINT64_MAX) return 0;
    if (best_due <= now_ns) { *out_ms = 0; return 1; }
    uint64_t delta_ns = best_due - now_ns;
    uint64_t delta_ms = (delta_ns + 999999ull) / 1000000ull;
    if (delta_ms > (uint64_t)0x7fffffff) delta_ms = (uint64_t)0x7fffffff;
    *out_ms = (int)delta_ms;
    return 1;
}

static int find_timer_slot_locked(MyThreadQueue* tq, HWND hwnd, UINT_PTR id)
{
    if (!tq) return -1;
    for (int i = 0; i < MAX_USER_TIMERS; ++i) {
        MyUserTimer* t = &tq->timers[i];
        if (t->valid && t->hwnd == hwnd && t->id == id) return i;
    }
    return -1;
}

static int timer_id_in_use_locked(MyThreadQueue* tq, HWND hwnd, UINT_PTR id)
{
    return find_timer_slot_locked(tq, hwnd, id) >= 0;
}

static void prune_timers_for_hwnd_locked(HWNDManager* mgr, HWND hwnd)
{
    if (!mgr || !hwnd) return;
    for (int i = 0; i < MAX_UI_THREADS; ++i) {
        MyThreadQueue* tq = &mgr->threads[i];
        if (!tq->valid) continue;
        int changed = 0;
        for (int j = 0; j < MAX_USER_TIMERS; ++j) {
            if (tq->timers[j].valid && tq->timers[j].hwnd == hwnd) {
                memset(&tq->timers[j], 0, sizeof(tq->timers[j]));
                changed = 1;
            }
        }
        if (changed) myqueue_wake(&tq->queue);
    }
}


static void hwnd_sync_state_mirror_locked(HWNDManager* mgr)
{
    if (!mgr || !mgr->state_section_mirror) return;
    mgr->state_section.updateSerial++;
    mgr->state_section.version = MYOS_WINDOWSTATE_LAYOUT_VERSION;
    mgr->state_section.cbSize = sizeof(mgr->state_section);
    mgr->state_section.magic = MYOS_WINDOWSTATE_MAGIC;
    mgr->state_section.capacity = MAX_HWNDS;
    snprintf(mgr->state_section.sectionName, sizeof(mgr->state_section.sectionName), "%s", MYOS_WINDOWSTATE_SECTION_NAME);
    memcpy(mgr->state_section_mirror, &mgr->state_section, sizeof(mgr->state_section));
}

int hwnd_attach_window_state_section(HWNDManager* mgr, MyWindowStateSection* sharedView)
{
    if (!mgr || !sharedView) return 0;
    pthread_mutex_lock(&mgr->lock);
    mgr->state_section_mirror = sharedView;
    memset(mgr->state_section_mirror, 0, sizeof(*mgr->state_section_mirror));
    hwnd_sync_state_mirror_locked(mgr);
    pthread_mutex_unlock(&mgr->lock);
    return 1;
}

static int copy_hooks(HWNDManager* mgr, Hook* out, int max_hooks)
{
    int n = 0;
    pthread_mutex_lock(&mgr->lock);
    for (int i = 0; i < MAX_HOOKS && n < max_hooks; i++) {
        if (mgr->chain.hooks[i].proc)
            out[n++] = mgr->chain.hooks[i];
    }
    pthread_mutex_unlock(&mgr->lock);
    return n;
}

static int run_hooks(HWNDManager* mgr, HWND target,
                     UINT msg, WPARAM* wp, LPARAM* lp)
{
    Hook snapshot[MAX_HOOKS];
    int count = copy_hooks(mgr, snapshot, MAX_HOOKS);
    WPARAM oldWp = wp ? *wp : 0;
    LPARAM oldLp = lp ? *lp : 0;
    int observed = 0;

    for (int i = 0; i < count; i++) {
        observed = 1;
        int result = snapshot[i].proc(target, msg, wp, lp, snapshot[i].userdata);
        if (result == HOOK_BLOCK) {
            printf("[HOOK] Message 0x%04x an hwnd=%u blockiert von '%s'\n",
                   msg, target, snapshot[i].cap.name);
            return HOOK_BLOCK;
        }
        if (result == HOOK_MODIFIED)
            observed = 2;
    }
    if (wp && lp && (*wp != oldWp || *lp != oldLp))
        observed = 2;
    return observed ? HOOK_MODIFIED : HOOK_ALLOW;
}

static int copy_entry_for_hwnd(HWNDManager* mgr, HWND hwnd, HWNDEntry* out)
{
    int ok = 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    if (idx >= 0) {
        if (out) *out = mgr->entries[idx];
        ok = 1;
    }
    pthread_mutex_unlock(&mgr->lock);
    return ok;
}

static HWND hwnd_create_internal(HWNDManager* mgr, HWNDWndProc proc,
                                void* userdata, Capability cap, int send_create)
{
    HWND hwnd = 0;

    pthread_mutex_lock(&mgr->lock);
    if (mgr->count < MAX_HWNDS) {
        int i = hwnd_pop_hwnd_slot_locked(mgr);
        if (i >= 0) {
            uint32_t owner_pid = cap.id;
            uint32_t owner_tid = cap.id; // PoC: ein UI-Thread pro App/Cap
            MyThreadQueue* tq = get_or_create_thread_queue_locked(mgr, owner_pid, owner_tid);
            if (!tq) {
                hwnd_push_hwnd_slot_locked(mgr, i);
            } else {
                HWNDEntry* e = &mgr->entries[i];

                /* v182: WSTS slots are retained diagnostic slots, but the
                   slot itself is reused when the HWND entry becomes free.
                   A reused tombstone is no longer a destroyed slot; keep the
                   header counters in sync with the per-slot truth before the
                   new HWND overwrites it. */
                MyWindowState* prevSt = &mgr->state_section.states[i];
                if (prevSt->hWnd) {
                    if (prevSt->destroyed || (prevSt->flags & MYWSF_DESTROYED)) {
                        if (mgr->state_section.destroyedCount) mgr->state_section.destroyedCount--;
                    } else {
                        if (mgr->state_section.activeCount) mgr->state_section.activeCount--;
                    }
                }

                uint32_t generation = ++mgr->hwnd_generations[i];
                if (generation == 0) generation = ++mgr->hwnd_generations[i];
                generation &= 0xffu;
                if (generation == 0) {
                    generation = 1;
                    mgr->hwnd_generations[i] = generation;
                }

                memset(e, 0, sizeof(*e));
                e->hwnd_slot = (uint32_t)i;
                e->hwnd_generation = generation;
                e->hwnd_state = send_create ? _HWND_STATE_LIVE : _HWND_STATE_NCCREATE;
                e->hwnd      = hwnd_make_value((uint32_t)i, generation);
                mgr->next_hwnd++;
                e->wndproc   = proc;
                e->userdata  = userdata;
                e->cap       = cap;
                e->owner_pid = owner_pid;
                e->owner_tid = owner_tid;
                e->valid     = 1;
                tq->owner_window_count++;

                // v17: reserve a state slot immediately. WindowManager fills
                // the real RECT/title/visibility right after the chrome object exists.
                memset(&mgr->state_section.states[i], 0, sizeof(MyWindowState));
                mgr->state_section.states[i].cbSize = sizeof(MyWindowState);
                mgr->state_section.states[i].seqBegin = 1; // odd while initializing
                mgr->state_section.states[i].hWnd = e->hwnd;
                mgr->state_section.states[i].ownerPid = owner_pid;
                mgr->state_section.states[i].ownerTid = owner_tid;
                mgr->state_section.states[i].visible = TRUE;
                mgr->state_section.states[i].enabled = TRUE;
                mgr->state_section.states[i].destroyed = FALSE;
                mgr->state_section.states[i].flags = MYWSF_VISIBLE;
                mgr->state_section.states[i].dirtyFlags = MYWS_DIRTY_OWNER|MYWS_DIRTY_VISIBLE;
                mgr->state_section.states[i].stateVersion = mgr->state_section.updateSerial++;
                mgr->state_section.states[i].updateSerial = mgr->state_section.states[i].stateVersion;
                e->hot_flags = mgr->state_section.states[i].flags;
                e->hot_update_serial = mgr->state_section.states[i].updateSerial;
                mgr->state_section.states[i].lastMessage = send_create ? WM_CREATE : WM_NCCREATE;
                mgr->state_section.states[i].seqEnd = 2;
                mgr->state_section.states[i].seqBegin = 2;
                mgr->state_section.activeCount++;
                hwnd_sync_state_mirror_locked(mgr);

                mgr->count++;
                hwnd = e->hwnd;
            }
        }
    }
    pthread_mutex_unlock(&mgr->lock);

    if (!hwnd) return 0;

    printf("[HWND] CreateWindow: hwnd=%u app='%s'\n", hwnd, cap.name);
    if (send_create && proc)
        proc(hwnd, WM_CREATE, 0, 0, userdata);
    return hwnd;
}

HWND hwnd_create(HWNDManager* mgr, HWNDWndProc proc,
                 void* userdata, Capability cap)
{
    return hwnd_create_internal(mgr, proc, userdata, cap, 1);
}

HWND hwnd_create_no_create(HWNDManager* mgr, HWNDWndProc proc,
                           void* userdata, Capability cap)
{
    return hwnd_create_internal(mgr, proc, userdata, cap, 0);
}

static int post_signal_unlocked(HWNDManager* mgr, const Capability* sender,
                                HWND queueTarget, HWND source, UINT msg,
                                WPARAM wp, LPARAM lp);

void hwnd_destroy(HWNDManager* mgr, HWND hwnd)
{
    HWNDWndProc proc = NULL;
    void* userdata = NULL;
    uint32_t owner_pid = 0, owner_tid = 0;
    Capability src_cap;
    memset(&src_cap, 0, sizeof(src_cap));
    HWND targets[MAX_SUBSCRIPTIONS + 1];
    int target_count = 0;
    DWORD final_version = 0;

    // v42: USER32-like destroy ordering. WM_DESTROY is delivered while
    // IsWindow(hwnd) is still true and WindowLong/UserData is still readable.
    // Only after that do we unlink the HWND and send the final WM_NCDESTROY.
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    if (idx >= 0) {
        HWNDEntry* e = &mgr->entries[idx];
        proc = e->wndproc;
        userdata = e->userdata;
        owner_pid = e->owner_pid;
        owner_tid = e->owner_tid;
        src_cap = e->cap;
        if (e->hwnd_state == _HWND_STATE_LIVE || e->hwnd_state == _HWND_STATE_NCCREATE)
            e->hwnd_state = _HWND_STATE_DESTROY_PENDING;
    }
    pthread_mutex_unlock(&mgr->lock);

    if (!proc) return;
    proc(hwnd, WM_DESTROY, 0, 0, userdata);

    pthread_mutex_lock(&mgr->lock);
    idx = find_entry_index(mgr, hwnd);
    if (idx >= 0) {
        HWNDEntry* e = &mgr->entries[idx];
        owner_pid = e->owner_pid;
        owner_tid = e->owner_tid;
        src_cap = e->cap;

        MyWindowState* st = &mgr->state_section.states[idx];
        DWORD seq = st->seqEnd + 1;
        if ((seq & 1u) == 0) seq++;
        st->seqBegin = seq;
        st->visible = FALSE;
        st->minimized = FALSE;
        st->active = FALSE;
        st->focused = FALSE;
        st->enabled = FALSE;
        st->hasCapture = FALSE;
        st->destroyed = TRUE;
        st->flags = MYWSF_DESTROYED;
        st->dirtyFlags = MYWS_DIRTY_DESTROY|MYWS_DIRTY_VISIBLE|MYWS_DIRTY_FOCUS;
        st->lastMessage = WM_DESTROY;
        st->stateVersion = mgr->state_section.updateSerial++;
        st->updateSerial = st->stateVersion;
        e->hot_flags = st->flags;
        e->hot_update_serial = st->updateSerial;
        final_version = st->stateVersion;
        st->seqEnd = seq + 1;
        st->seqBegin = st->seqEnd;
        if (mgr->state_section.activeCount) mgr->state_section.activeCount--;
        mgr->state_section.destroyedCount++;
        hwnd_sync_state_mirror_locked(mgr);

        UINT target_msgs[MAX_SUBSCRIPTIONS + 1];
        targets[target_count] = hwnd;
        target_msgs[target_count] = WM_DESTROY;
        target_count++;
        for (int i = 0; i < MAX_SUBSCRIPTIONS && target_count < MAX_SUBSCRIPTIONS + 1; i++) {
            HWNDSubscription* sub = &mgr->subscriptions[i];
            if (!sub->valid) continue;
            if (sub->source != hwnd && sub->source != 0) continue;
            if ((sub->msgMin || sub->msgMax) && (WM_DESTROY < sub->msgMin || WM_DESTROY > sub->msgMax)) continue;
            targets[target_count] = sub->subscriber;
            target_msgs[target_count] = sub->source == 0 ? WM_MYOS_HWND_STATE_DIRTY : WM_DESTROY;
            target_count++;
        }

        for (int i = 0; i < target_count; i++) {
            post_signal_unlocked(mgr, &src_cap, targets[i], hwnd, target_msgs[i], (WPARAM)hwnd, (LPARAM)final_version);
        }

        for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
            HWNDSubscription* sub = &mgr->subscriptions[i];
            if (sub->valid && (sub->source == hwnd || sub->subscriber == hwnd)) {
                memset(sub, 0, sizeof(*sub));
                if (mgr->subscription_count) mgr->subscription_count--;
            }
        }

        prune_timers_for_hwnd_locked(mgr, hwnd);
        e->hwnd_state = _HWND_STATE_NCDESTROY;
        int tidx = find_thread_index_locked(mgr, owner_pid, owner_tid);
        if (tidx >= 0 && mgr->threads[tidx].owner_window_count > 0)
            mgr->threads[tidx].owner_window_count--;
        memset(e, 0, sizeof(*e));
        mgr->count--;
        hwnd_push_hwnd_slot_locked(mgr, idx);
        maybe_destroy_thread_queue_locked(mgr, owner_pid, owner_tid);
    }
    pthread_mutex_unlock(&mgr->lock);

    _ObjectUnregister(hwnd);
    proc(hwnd, WM_NCDESTROY, 0, 0, userdata);
    printf("[HWND] DestroyWindow: hwnd=%u\n", hwnd);
}

int hwnd_post_routed(HWNDManager* mgr, const Capability* sender,
                     HWND target, UINT msg, WPARAM wp, LPARAM lp,
                     const _MsgRouteDescriptor* route)
{
    if (!sender || !(sender->flags & CAP_IPC)) {
        printf("[HWND] PostMessage verweigert: kein CAP_IPC\n");
        return -1;
    }

    int hookResult = run_hooks(mgr, target, msg, &wp, &lp);
    if (hookResult == HOOK_BLOCK)
        return 0;

    MyThreadQueue* tq = NULL;
    MyMessage m;
    memset(&m, 0, sizeof(m));

    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, target);
    if (idx < 0) {
        pthread_mutex_unlock(&mgr->lock);
        printf("[HWND] PostMessage: hwnd=%u nicht gefunden\n", target);
        return -1;
    }

    HWNDEntry* e = &mgr->entries[idx];
    int tidx = find_thread_index_locked(mgr, e->owner_pid, e->owner_tid);
    if (tidx >= 0) tq = &mgr->threads[tidx];

    m.size       = sizeof(m);
    m.type       = 1; // HWND/UI message
    m.sender_pid = sender->id;
    m.sender_tid = sender->id;
    m.target_pid = e->owner_pid;
    m.target_tid = e->owner_tid;
    m.hwnd       = target;
    m.msg        = msg;
    m.wparam     = wp;
    m.lparam     = lp;
    m.priority   = mymsg_default_priority(msg);
    m.route_hwnd = target;
    pthread_mutex_unlock(&mgr->lock);

    if (route)
        mymsg_apply_route_descriptor(&m, route);
    if (!m.route_reason)
        m.route_reason = mymsg_default_route_reason(msg, m.flags, mymsg_default_input_kind(msg));
    if (hookResult == HOOK_MODIFIED)
        m.route_reason |= _MSG_ROUTE_REASON_HOOK;

    if (!tq) {
        printf("[HWND] PostMessage: keine ThreadQueue für hwnd=%u\n", target);
        return -1;
    }

    int r = myqueue_post(&tq->queue, &m);
    if (r != 0) {
        printf("[HWND] PostMessage: Queue voll hwnd=%u dropped/coalesced möglich\n", target);
        return -1;
    }
    return 0;
}

int hwnd_post(HWNDManager* mgr, const Capability* sender,
              HWND target, UINT msg, WPARAM wp, LPARAM lp)
{
    return hwnd_post_routed(mgr, sender, target, msg, wp, lp, NULL);
}

int hwnd_send_timeout(HWNDManager* mgr, const Capability* sender,
                      HWND target, UINT msg, WPARAM wp, LPARAM lp,
                      int timeout_ms)
{
    if (!sender || !(sender->flags & CAP_IPC)) {
        printf("[HWND] SendMessageTimeout verweigert: kein CAP_IPC\n");
        return -1;
    }

    if (run_hooks(mgr, target, msg, &wp, &lp) == HOOK_BLOCK)
        return 0;

    HWNDWndProc proc = NULL;
    void* userdata = NULL;
    uint32_t owner_pid = 0, owner_tid = 0;
    int external_pump = 0;
    MyThreadQueue* tq = NULL;

    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, target);
    if (idx >= 0) {
        proc = mgr->entries[idx].wndproc;
        userdata = mgr->entries[idx].userdata;
        owner_pid = mgr->entries[idx].owner_pid;
        owner_tid = mgr->entries[idx].owner_tid;
        int tidx = find_thread_index_locked(mgr, owner_pid, owner_tid);
        if (tidx >= 0) {
            tq = &mgr->threads[tidx];
            external_pump = tq->external_pump;
        }
    }
    pthread_mutex_unlock(&mgr->lock);

    if (!proc || !owner_pid || !owner_tid) return -1;

    // Same-thread / legacy non-external queues stay direct for now. The v21 proof
    // is specifically cross-thread SendMessageTimeout to self-pumped app queues.
    if (!external_pump || (owner_pid == sender->id && owner_tid == sender->id)) {
        pthread_mutex_lock(&mgr->lock);
        int tidx = find_thread_index_locked(mgr, owner_pid, owner_tid);
        if (tidx >= 0) {
            mgr->threads[tidx].send_depth++;
            mgr->threads[tidx].in_send_dispatch = 1;
            mgr->threads[tidx].send_count++;
        }
        pthread_mutex_unlock(&mgr->lock);
        proc(target, msg, wp, lp, userdata);
        pthread_mutex_lock(&mgr->lock);
        hwnd_touch_visual_locked(mgr, target, msg);
        tidx = find_thread_index_locked(mgr, owner_pid, owner_tid);
        if (tidx >= 0) {
            if (mgr->threads[tidx].send_depth) mgr->threads[tidx].send_depth--;
            mgr->threads[tidx].in_send_dispatch = 0;
        }
        pthread_mutex_unlock(&mgr->lock);
        return 0;
    }

    MySyncSendContext* ctx = (MySyncSendContext*)calloc(1, sizeof(MySyncSendContext));
    if (!ctx) return -1;
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cond, NULL);

    MyMessage m; memset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    m.type = 1;
    m.sender_pid = sender->id;
    m.sender_tid = sender->id;
    m.target_pid = owner_pid;
    m.target_tid = owner_tid;
    m.hwnd = target;
    m.msg = msg;
    m.wparam = wp;
    m.lparam = lp;
    m.priority = mymsg_default_priority(msg);
    m.flags = MYMSG_FLAG_SYNC_REQ;
    m.sync_ctx = ctx;

    int post_r = myqueue_post(&tq->queue, &m);
    if (post_r != 0) return -1;

    pthread_mutex_lock(&mgr->lock);
    int tidx = find_thread_index_locked(mgr, owner_pid, owner_tid);
    if (tidx >= 0) mgr->threads[tidx].send_count++;
    pthread_mutex_unlock(&mgr->lock);

    pthread_mutex_lock(&ctx->lock);
    while (!ctx->done) {
        int r = cond_wait_ms(&ctx->cond, &ctx->lock, timeout_ms);
        if (r == ETIMEDOUT) {
            ctx->timed_out = 1;
            pthread_mutex_unlock(&ctx->lock);
            pthread_mutex_lock(&mgr->lock);
            tidx = find_thread_index_locked(mgr, owner_pid, owner_tid);
            if (tidx >= 0) mgr->threads[tidx].send_timeout_count++;
            pthread_mutex_unlock(&mgr->lock);
            printf("[HWND] SendMessageTimeout: TIMEOUT sender=%u target=%u msg=0x%04x timeout=%dms\n",
                   sender->id, target, msg, timeout_ms);
            return -2;
        }
        // For nonzero timeout we only wait once; for spurious wake, recompute by retrying.
    }
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}


int hwnd_send(HWNDManager* mgr, const Capability* sender,
              HWND target, UINT msg, WPARAM wp, LPARAM lp)
{
    return hwnd_send_timeout(mgr, sender, target, msg, wp, lp, 50);
}

uint32_t hwnd_set_hook(HWNDManager* mgr, HookProc proc,
                       void* userdata, Capability cap)
{
    if (!(cap.flags & CAP_HOOK)) {
        printf("[HWND] SetHook verweigert: '%s' hat kein CAP_HOOK\n", cap.name);
        return 0;
    }

    pthread_mutex_lock(&mgr->lock);
    HookChain* chain = &mgr->chain;
    if (chain->count >= MAX_HOOKS) {
        pthread_mutex_unlock(&mgr->lock);
        return 0;
    }

    for (int i = 0; i < MAX_HOOKS; i++) {
        if (!chain->hooks[i].proc) {
            chain->hooks[i].proc     = proc;
            chain->hooks[i].userdata = userdata;
            chain->hooks[i].cap      = cap;
            chain->hooks[i].hook_id  = chain->next_id++;
            chain->count++;
            uint32_t id = chain->hooks[i].hook_id;
            pthread_mutex_unlock(&mgr->lock);
            printf("[HWND] Hook gesetzt von '%s' id=%u\n", cap.name, id);
            return id;
        }
    }

    pthread_mutex_unlock(&mgr->lock);
    return 0;
}

void hwnd_remove_hook(HWNDManager* mgr, uint32_t hook_id)
{
    pthread_mutex_lock(&mgr->lock);
    HookChain* chain = &mgr->chain;
    for (int i = 0; i < MAX_HOOKS; i++) {
        if (chain->hooks[i].hook_id == hook_id) {
            memset(&chain->hooks[i], 0, sizeof(chain->hooks[i]));
            chain->count--;
            pthread_mutex_unlock(&mgr->lock);
            printf("[HWND] Hook entfernt: id=%u\n", hook_id);
            return;
        }
    }
    pthread_mutex_unlock(&mgr->lock);
}

UINT_PTR hwnd_set_user_timer(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                             HWND hwnd, UINT_PTR idEvent, UINT elapse_ms, uintptr_t callback)
{
    if (!mgr || !owner_pid || !owner_tid) return 0;
    if (elapse_ms == 0) elapse_ms = 1;

    pthread_mutex_lock(&mgr->lock);

    if (hwnd) {
        int eidx = find_entry_index(mgr, hwnd);
        if (eidx < 0 || mgr->entries[eidx].owner_pid != owner_pid || mgr->entries[eidx].owner_tid != owner_tid) {
            pthread_mutex_unlock(&mgr->lock);
            return 0;
        }
        if (idEvent == 0) {
            pthread_mutex_unlock(&mgr->lock);
            return 0;
        }
    }

    MyThreadQueue* tq = get_or_create_thread_queue_locked(mgr, owner_pid, owner_tid);
    if (!tq) { pthread_mutex_unlock(&mgr->lock); return 0; }

    if (tq->next_timer_id == 0) tq->next_timer_id = 1;
    UINT_PTR id = idEvent;
    if (!hwnd && id == 0) {
        do {
            id = tq->next_timer_id++;
            if (id == 0) id = tq->next_timer_id++;
        } while (timer_id_in_use_locked(tq, 0, id));
    }

    int slot = find_timer_slot_locked(tq, hwnd, id);
    if (slot < 0) {
        for (int i = 0; i < MAX_USER_TIMERS; ++i) {
            if (!tq->timers[i].valid) { slot = i; break; }
        }
    }
    if (slot < 0) { pthread_mutex_unlock(&mgr->lock); return 0; }

    MyUserTimer* t = &tq->timers[slot];
    memset(t, 0, sizeof(*t));
    t->valid = 1;
    t->hwnd = hwnd;
    t->id = id;
    t->elapse_ms = elapse_ms;
    t->next_due_ns = myos_now_ns() + timer_period_ns(elapse_ms);
    t->callback = callback;

    myqueue_wake(&tq->queue);
    pthread_mutex_unlock(&mgr->lock);
    return id;
}

int hwnd_kill_user_timer(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                         HWND hwnd, UINT_PTR idEvent)
{
    if (!mgr || !owner_pid || !owner_tid) return 0;
    int ok = 0;
    pthread_mutex_lock(&mgr->lock);

    MyThreadQueue* tq = NULL;
    int tidx = find_thread_index_locked(mgr, owner_pid, owner_tid);
    if (tidx >= 0) tq = &mgr->threads[tidx];
    if (tq) {
        int slot = find_timer_slot_locked(tq, hwnd, idEvent);
        if (slot >= 0) {
            memset(&tq->timers[slot], 0, sizeof(tq->timers[slot]));
            ok = 1;
            myqueue_wake(&tq->queue);
        }
    }

    pthread_mutex_unlock(&mgr->lock);
    return ok;
}

int hwnd_get_thread_message(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                            HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                            int remove, MyMessage* out)
{
    MyThreadQueue* tq = NULL;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_thread_index_locked(mgr, owner_pid, owner_tid);
    if (idx >= 0) tq = &mgr->threads[idx];
    pthread_mutex_unlock(&mgr->lock);
    if (!tq) return 0;

    if (myqueue_peek_match(&tq->queue, out, hwndFilter, wMsgFilterMin, wMsgFilterMax, remove))
        return 1;

    pthread_mutex_lock(&mgr->lock);
    idx = find_thread_index_locked(mgr, owner_pid, owner_tid);
    int got = 0;
    if (idx >= 0)
        got = synthesize_due_timer_locked(&mgr->threads[idx], hwndFilter, wMsgFilterMin, wMsgFilterMax, remove, out);
    pthread_mutex_unlock(&mgr->lock);
    return got;
}

int hwnd_get_thread_message_wait(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                                 HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax,
                                 int remove, int timeout_ms, MyMessage* out)
{
    if (!mgr) return 0;
    uint64_t start_ns = myos_now_ns();
    uint64_t user_timeout_ns = timeout_ms > 0 ? (uint64_t)timeout_ms * 1000000ull : 0;

    for (;;) {
        MyThreadQueue* tq = NULL;
        pthread_mutex_lock(&mgr->lock);
        int idx = find_thread_index_locked(mgr, owner_pid, owner_tid);
        if (idx >= 0) tq = &mgr->threads[idx];
        pthread_mutex_unlock(&mgr->lock);
        if (!tq) return 0;

        /* WM_TIMER is low-priority/synthetic: real queued messages win first. */
        if (myqueue_peek_match(&tq->queue, out, hwndFilter, wMsgFilterMin, wMsgFilterMax, remove))
            return 1;

        pthread_mutex_lock(&mgr->lock);
        idx = find_thread_index_locked(mgr, owner_pid, owner_tid);
        tq = (idx >= 0) ? &mgr->threads[idx] : NULL;
        if (idx >= 0 && synthesize_due_timer_locked(tq, hwndFilter, wMsgFilterMin, wMsgFilterMax, remove, out)) {
            pthread_mutex_unlock(&mgr->lock);
            return 1;
        }
        int timer_ms = -1;
        int has_timer = (idx >= 0) ? next_due_timer_delay_ms_locked(tq, hwndFilter, wMsgFilterMin, wMsgFilterMax, myos_now_ns(), &timer_ms) : 0;
        pthread_mutex_unlock(&mgr->lock);
        if (!tq) return 0;

        if (timeout_ms == 0) return 0;

        int wait_ms = -1;
        if (timeout_ms > 0) {
            uint64_t now = myos_now_ns();
            uint64_t elapsed = now - start_ns;
            if (elapsed >= user_timeout_ns) return 0;
            uint64_t rem_ms_u64 = (user_timeout_ns - elapsed + 999999ull) / 1000000ull;
            if (rem_ms_u64 > (uint64_t)0x7fffffff) rem_ms_u64 = (uint64_t)0x7fffffff;
            wait_ms = (int)rem_ms_u64;
        }
        if (has_timer && (wait_ms < 0 || timer_ms < wait_ms)) wait_ms = timer_ms;

        if (myqueue_wait_match_or_wake(&tq->queue, out, hwndFilter, wMsgFilterMin, wMsgFilterMax, remove, wait_ms))
            return 1;
        /* A wake without a queued message may mean a timer was added/killed or
           its deadline expired; loop and re-scan under the HWND manager lock. */
    }
}

int hwnd_remove_thread_messages(HWNDManager* mgr, uint32_t owner_pid, uint32_t owner_tid,
                                HWND hwndFilter, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    if (!mgr || !owner_pid || !owner_tid) return 0;
    MyThreadQueue* tq = NULL;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_thread_index_locked(mgr, owner_pid, owner_tid);
    if (idx >= 0) tq = &mgr->threads[idx];
    pthread_mutex_unlock(&mgr->lock);
    if (!tq) return 0;
    return myqueue_remove_matching(&tq->queue, hwndFilter, wMsgFilterMin, wMsgFilterMax);
}

int hwnd_remove_queued_messages_for_hwnd(HWNDManager* mgr, HWND hwnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
    if (!mgr || !hwnd) return 0;
    uint32_t owner_pid = 0, owner_tid = 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    if (idx >= 0) {
        owner_pid = mgr->entries[idx].owner_pid;
        owner_tid = mgr->entries[idx].owner_tid;
    }
    pthread_mutex_unlock(&mgr->lock);
    if (!owner_pid) return 0;
    return hwnd_remove_thread_messages(mgr, owner_pid, owner_tid, hwnd, wMsgFilterMin, wMsgFilterMax);
}

int hwnd_get_message(HWNDManager* mgr, HWND hwnd, MyMessage* out)
{
    uint32_t owner_pid = 0, owner_tid = 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    if (idx >= 0) {
        owner_pid = mgr->entries[idx].owner_pid;
        owner_tid = mgr->entries[idx].owner_tid;
    }
    pthread_mutex_unlock(&mgr->lock);
    if (!owner_pid) return 0;
    return hwnd_get_thread_message(mgr, owner_pid, owner_tid, hwnd, 0, 0, 1, out);
}

int hwnd_get_message_wait(HWNDManager* mgr, HWND hwnd, MyMessage* out, int timeout_ms)
{
    uint32_t owner_pid = 0, owner_tid = 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    if (idx >= 0) {
        owner_pid = mgr->entries[idx].owner_pid;
        owner_tid = mgr->entries[idx].owner_tid;
    }
    pthread_mutex_unlock(&mgr->lock);
    if (!owner_pid) return 0;
    return hwnd_get_thread_message_wait(mgr, owner_pid, owner_tid, hwnd, 0, 0, 1, timeout_ms, out);
}


static DWORD hwnd_required_action_for_message(const MyMessage* msg)
{
    if (!msg) return _HWND_ACTION_MESSAGE;
    _MsgRouteDescriptor route;
    mymsg_make_route_descriptor(msg, &route);
    return route.hwnd_action ? route.hwnd_action : _HWND_ACTION_MESSAGE;
}

static void mark_dispatch_begin(HWNDManager* mgr, const MyMessage* msg)
{
    pthread_mutex_lock(&mgr->lock);
    int idx = find_thread_index_locked(mgr, msg->target_pid, msg->target_tid);
    if (idx >= 0) {
        mgr->threads[idx].in_dispatch = 1;
        mgr->threads[idx].dispatch_start_ns = myos_now_ns();
    }
    pthread_mutex_unlock(&mgr->lock);
}

static void mark_dispatched_for_message(HWNDManager* mgr, const MyMessage* msg)
{
    pthread_mutex_lock(&mgr->lock);
    int idx = find_thread_index_locked(mgr, msg->target_pid, msg->target_tid);
    if (idx >= 0) {
        mgr->threads[idx].in_dispatch = 0;
        mgr->threads[idx].dispatch_start_ns = 0;
        mgr->threads[idx].last_pump_ns = myos_now_ns();
        mgr->threads[idx].dispatch_count++;
        myqueue_mark_dispatched(&mgr->threads[idx].queue);
    }
    pthread_mutex_unlock(&mgr->lock);
}

int hwnd_dispatch_message(HWNDManager* mgr, const MyMessage* msg)
{
    if (!msg) return -1;

    HWNDWndProc proc = NULL;
    void* userdata = NULL;

    DWORD requiredAction = hwnd_required_action_for_message(msg);
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, msg->hwnd);
    if (idx >= 0 && hwnd_state_allows(mgr->entries[idx].hwnd_state, requiredAction)) {
        proc = mgr->entries[idx].wndproc;
        userdata = mgr->entries[idx].userdata;
    }
    pthread_mutex_unlock(&mgr->lock);

    if (!proc) return -1;
    mark_dispatch_begin(mgr, msg);

    /* v133: Dispatch in the receiver's runtime context.  This is the USER32
       side of thread/window ownership: queued messages execute as the target
       UI thread, not as whatever compositor/render thread happened to pump the
       queue.  It fixes shell broker regressions from v131 while preserving the
       injection guards in PostMessageA/SendMessageA/DispatchMessageA. */
    BOOL entered_context = FALSE;
    if (msg->target_pid)
        entered_context = MyWinEnterProcessContext((DWORD)msg->target_pid);

    proc(msg->hwnd, msg->msg, msg->wparam, msg->lparam, userdata);

    pthread_mutex_lock(&mgr->lock);
    hwnd_touch_visual_locked(mgr, msg->hwnd, msg->msg);
    pthread_mutex_unlock(&mgr->lock);

    if (entered_context)
        MyWinLeaveProcessContext();

    if ((msg->flags & MYMSG_FLAG_SYNC_REQ) && msg->sync_ctx) {
        MySyncSendContext* ctx = (MySyncSendContext*)msg->sync_ctx;
        pthread_mutex_lock(&ctx->lock);
        if (!ctx->timed_out) {
            ctx->result = 1;
            ctx->done = 1;
            pthread_cond_signal(&ctx->cond);
        }
        pthread_mutex_unlock(&ctx->lock);
    }
    mark_dispatched_for_message(mgr, msg);
    return 0;
}

void hwnd_dispatch(HWNDManager* mgr)
{
    for (;;) {
        MyMessage msg;
        int found = 0;

        uint32_t pids[MAX_UI_THREADS];
        uint32_t tids[MAX_UI_THREADS];
        int n = 0;

        pthread_mutex_lock(&mgr->lock);
        for (int i = 0; i < MAX_UI_THREADS; i++) {
            if (mgr->threads[i].valid && !mgr->threads[i].external_pump) {
                pids[n] = mgr->threads[i].pid;
                tids[n] = mgr->threads[i].tid;
                n++;
            }
        }
        pthread_mutex_unlock(&mgr->lock);

        for (int i = 0; i < n; i++) {
            if (hwnd_get_thread_message(mgr, pids[i], tids[i], 0, 0, 0, 1, &msg)) {
                found = 1;
                hwnd_dispatch_message(mgr, &msg);
            }
        }

        if (!found) break;
    }
}


static inline void hwnd_fill_header_locked(HWNDEntry* e, _HwndHeader* out)
{
    out->cbSize = sizeof(*out);
    out->hwnd = e->hwnd;
    out->hwnd_slot = e->hwnd_slot;
    out->hwnd_generation = e->hwnd_generation;
    out->hwnd_state = e->hwnd_state;
    out->owner_pid = e->owner_pid;
    out->owner_tid = e->owner_tid;
    out->state_flags = e->hot_flags;
    out->update_serial = e->hot_update_serial;
}

int hwnd_query_header(HWNDManager* mgr, HWND hwnd, _HwndHeader* out)
{
    if (MYOS_UNLIKELY(!mgr || !hwnd || !out)) return 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    if (MYOS_UNLIKELY(idx < 0)) {
        pthread_mutex_unlock(&mgr->lock);
        memset(out, 0, sizeof(*out));
        return 0;
    }
    HWNDEntry* e = &mgr->entries[idx];
    hwnd_fill_header_locked(e, out);
    pthread_mutex_unlock(&mgr->lock);
    return 1;
}

int hwnd_query_action(HWNDManager* mgr, HWND hwnd, DWORD action, _HwndHeader* out)
{
    if (MYOS_UNLIKELY(!mgr || !hwnd)) {
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }

    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    if (MYOS_UNLIKELY(idx < 0)) {
        pthread_mutex_unlock(&mgr->lock);
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }

    HWNDEntry* e = &mgr->entries[idx];
    if (MYOS_UNLIKELY(!hwnd_state_allows(e->hwnd_state, action))) {
        pthread_mutex_unlock(&mgr->lock);
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }

    if (out) hwnd_fill_header_locked(e, out);
    pthread_mutex_unlock(&mgr->lock);
    return 1;
}


int hwnd_set_state(HWNDManager* mgr, HWND hwnd, DWORD state)
{
    if (!mgr || !hwnd) return 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    if (idx < 0) {
        pthread_mutex_unlock(&mgr->lock);
        return 0;
    }
    mgr->entries[idx].hwnd_state = state;
    pthread_mutex_unlock(&mgr->lock);
    return 1;
}

int hwnd_is_window(HWNDManager* mgr, HWND hwnd)
{
    if (!mgr || !hwnd) return 0;
    return hwnd_query_action(mgr, hwnd, _HWND_ACTION_QUERY, NULL);
}

int hwnd_subscribe(HWNDManager* mgr, const Capability* subscriberCap,
                   HWND source, HWND subscriber, UINT msgMin, UINT msgMax)
{
    if (!mgr || !subscriberCap || !subscriber) return -1;
    if (!(subscriberCap->flags & CAP_IPC)) return -1;
    /* v74: source==0 means global HWND-state dirty subscription.
       This is intentionally privilege gated because it observes every top-level HWND. */
    if (!source && !(subscriberCap->flags & CAP_WINDOW_SUBSCRIBE)) return -1;

    pthread_mutex_lock(&mgr->lock);
    if ((source && find_entry_index(mgr, source) < 0) || find_entry_index(mgr, subscriber) < 0) {
        pthread_mutex_unlock(&mgr->lock);
        return -1;
    }
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        HWNDSubscription* sub = &mgr->subscriptions[i];
        if (sub->valid && sub->source == source && sub->subscriber == subscriber &&
            sub->msgMin == msgMin && sub->msgMax == msgMax) {
            pthread_mutex_unlock(&mgr->lock);
            return 0;
        }
    }
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        HWNDSubscription* sub = &mgr->subscriptions[i];
        if (!sub->valid) {
            sub->valid = 1;
            sub->source = source;
            sub->subscriber = subscriber;
            sub->msgMin = msgMin;
            sub->msgMax = msgMax;
            sub->subscriber_pid = subscriberCap->id;
            mgr->subscription_count++;
            pthread_mutex_unlock(&mgr->lock);
            printf("[HWND] Subscribe: hwnd=%u observes %s%u msg=%u..%u\n", subscriber, source ? "hwnd=" : "GLOBAL-WSTS ", source, msgMin, msgMax);
            return 0;
        }
    }
    pthread_mutex_unlock(&mgr->lock);
    return -1;
}

int hwnd_unsubscribe(HWNDManager* mgr, const Capability* subscriberCap,
                     HWND source, HWND subscriber, UINT msgMin, UINT msgMax)
{
    if (!mgr || !subscriberCap) return -1;
    pthread_mutex_lock(&mgr->lock);
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        HWNDSubscription* sub = &mgr->subscriptions[i];
        if (sub->valid && sub->source == source && sub->subscriber == subscriber &&
            sub->msgMin == msgMin && sub->msgMax == msgMax &&
            sub->subscriber_pid == subscriberCap->id) {
            memset(sub, 0, sizeof(*sub));
            if (mgr->subscription_count) mgr->subscription_count--;
            pthread_mutex_unlock(&mgr->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&mgr->lock);
    return -1;
}

static int post_signal_unlocked(HWNDManager* mgr, const Capability* sender,
                                HWND queueTarget, HWND source, UINT msg,
                                WPARAM wp, LPARAM lp)
{
    if (!sender || !(sender->flags & CAP_IPC)) return -1;

    int idx = find_entry_index(mgr, queueTarget);
    if (idx < 0) return -1;

    HWNDEntry* e = &mgr->entries[idx];
    int tidx = find_thread_index_locked(mgr, e->owner_pid, e->owner_tid);
    if (tidx < 0) return -1;

    MyMessage m;
    memset(&m, 0, sizeof(m));
    m.size       = sizeof(m);
    m.type       = 1;
    m.sender_pid = sender->id;
    m.sender_tid = sender->id;
    m.target_pid = e->owner_pid;
    m.target_tid = e->owner_tid;
    m.hwnd       = queueTarget;          // receiver HWND
    m.msg        = msg;                  // signal only
    m.wparam     = wp ? wp : (WPARAM)source; // source HWND by convention for subscriptions
    m.lparam     = lp;                   // usually shared state version
    m.priority   = mymsg_default_priority(msg);
    return myqueue_post(&mgr->threads[tidx].queue, &m);
}

static int find_state_index_locked(HWNDManager* mgr, HWND hwnd)
{
    for (int i = 0; i < MAX_HWNDS; i++) {
        if (mgr->state_section.states[i].hWnd == hwnd)
            return i;
    }
    return -1;
}

/* v178: visual-sequence lane for retained in-process window caching.
   Window geometry/state updates already advance MyWindowState.stateVersion, but
   legacy in-process apps mutate client pixels inside their WndProcs without
   necessarily changing the top-level Window record.  Touch the target HWND
   after a message has actually been dispatched so the compositor can safely
   treat that HWND (and all HWNDs owned by the same UI-thread/capability) as a
   new visual revision.  This is conservative: query messages may bump the
   serial too, but correctness beats stale backing-cache pixels. */
static void hwnd_touch_visual_locked(HWNDManager* mgr, HWND hwnd, UINT msg)
{
    if (!mgr || !hwnd) return;
    int idx = find_state_index_locked(mgr, hwnd);
    if (idx < 0) return;

    MyWindowState* st = &mgr->state_section.states[idx];
    if (!st->hWnd || st->destroyed) return;

    DWORD seq = st->seqEnd + 1;
    if ((seq & 1u) == 0) seq++;
    st->seqBegin = seq; /* odd while writing */
    st->lastMessage = msg;
    st->dirtyFlags |= MYWS_DIRTY_VISIBLE;
    st->stateVersion = mgr->state_section.updateSerial++;
    st->updateSerial = st->stateVersion;
    if (idx >= 0 && idx < MAX_HWNDS && mgr->entries[idx].valid) {
        mgr->entries[idx].hot_flags = st->flags;
        mgr->entries[idx].hot_update_serial = st->updateSerial;
    }
    st->seqEnd = seq + 1;
    st->seqBegin = st->seqEnd; /* even = stable */
    hwnd_sync_state_mirror_locked(mgr);
}

uint64_t hwnd_get_owner_visual_signature(HWNDManager* mgr, DWORD ownerPid, HWND rootHwnd)
{
    if (!mgr) return 0;
    uint64_t h = 1469598103934665603ull;
    pthread_mutex_lock(&mgr->lock);
    for (int i = 0; i < MAX_HWNDS; ++i) {
        MyWindowState* st = &mgr->state_section.states[i];
        if (!st->hWnd || st->destroyed) continue;
        if (ownerPid && st->ownerPid != ownerPid) continue;
        if (!ownerPid && rootHwnd && st->hWnd != rootHwnd) continue;
        h ^= (uint64_t)st->hWnd;          h *= 1099511628211ull;
        h ^= (uint64_t)st->ownerPid;      h *= 1099511628211ull;
        h ^= (uint64_t)st->ownerTid;      h *= 1099511628211ull;
        h ^= (uint64_t)st->stateVersion;  h *= 1099511628211ull;
        h ^= (uint64_t)st->updateSerial;  h *= 1099511628211ull;
        h ^= (uint64_t)st->lastMessage;   h *= 1099511628211ull;
        h ^= (uint64_t)st->dirtyFlags;    h *= 1099511628211ull;
        h ^= (uint64_t)st->flags;         h *= 1099511628211ull;
        h ^= (uint64_t)st->enabled;       h *= 1099511628211ull;
        h ^= (uint64_t)st->focused;       h *= 1099511628211ull;
        h ^= (uint64_t)st->visible;       h *= 1099511628211ull;
        h ^= (uint64_t)st->rcWindow.left; h *= 1099511628211ull;
        h ^= (uint64_t)st->rcWindow.top;  h *= 1099511628211ull;
        h ^= (uint64_t)st->rcWindow.right;h *= 1099511628211ull;
        h ^= (uint64_t)st->rcWindow.bottom;h *= 1099511628211ull;
        h ^= (uint64_t)st->rcClient.left; h *= 1099511628211ull;
        h ^= (uint64_t)st->rcClient.top;  h *= 1099511628211ull;
        h ^= (uint64_t)st->rcClient.right;h *= 1099511628211ull;
        h ^= (uint64_t)st->rcClient.bottom;h *= 1099511628211ull;
        const unsigned char* p = (const unsigned char*)st->szTitle;
        for (size_t j = 0; j < sizeof(st->szTitle); ++j) { h ^= (uint64_t)p[j]; h *= 1099511628211ull; }
    }
    pthread_mutex_unlock(&mgr->lock);
    return h ? h : 1;
}

int hwnd_update_window_state(HWNDManager* mgr, const MyWindowState* state)
{
    if (!mgr || !state || !state->hWnd) return -1;

    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, state->hWnd);
    if (idx < 0) idx = find_state_index_locked(mgr, state->hWnd);
    if (idx < 0) {
        pthread_mutex_unlock(&mgr->lock);
        return -1;
    }

    MyWindowState* dst = &mgr->state_section.states[idx];
    DWORD seq = dst->seqEnd + 1;
    if ((seq & 1u) == 0) seq++;
    dst->seqBegin = seq; // odd = writer active

    HWND hwnd = dst->hWnd ? dst->hWnd : state->hWnd;
    *dst = *state;
    dst->cbSize = sizeof(MyWindowState);
    dst->hWnd = hwnd;
    dst->flags = 0;
    if (dst->visible)   dst->flags |= MYWSF_VISIBLE;
    if (dst->minimized) dst->flags |= MYWSF_MINIMIZED;
    if (dst->active)    dst->flags |= MYWSF_ACTIVE;
    if (dst->destroyed) dst->flags |= MYWSF_DESTROYED;
    if (dst->stateVersion == 0)
        dst->stateVersion = mgr->state_section.updateSerial++;
    dst->updateSerial = dst->stateVersion;
    mgr->state_section.updateSerial = dst->stateVersion + 1;
    mgr->state_section.version = MYOS_WINDOWSTATE_LAYOUT_VERSION;
    if (idx >= 0 && idx < MAX_HWNDS && mgr->entries[idx].valid) {
        mgr->entries[idx].hot_flags = dst->flags;
        mgr->entries[idx].hot_update_serial = dst->updateSerial;
    }

    dst->seqEnd = seq + 1;
    dst->seqBegin = dst->seqEnd; // even = stable
    hwnd_sync_state_mirror_locked(mgr);
    pthread_mutex_unlock(&mgr->lock);
    return 0;
}

int hwnd_copy_window_state(HWNDManager* mgr, HWND hwnd, MyWindowState* out)
{
    if (!mgr || !hwnd || !out) return 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        MyWindowState tmp;
        pthread_mutex_lock(&mgr->lock);
        int idx = find_entry_index(mgr, hwnd);
        if (idx < 0) idx = find_state_index_locked(mgr, hwnd);
        if (idx < 0) {
            pthread_mutex_unlock(&mgr->lock);
            return 0;
        }
        DWORD a = mgr->state_section.states[idx].seqBegin;
        tmp = mgr->state_section.states[idx];
        DWORD b = mgr->state_section.states[idx].seqEnd;
        pthread_mutex_unlock(&mgr->lock);

        if (a == b && (a & 1u) == 0 && tmp.hWnd == hwnd) {
            *out = tmp;
            out->cbSize = sizeof(MyWindowState);
            return 1;
        }
    }
    return 0;
}

const MyWindowStateSection* hwnd_get_window_state_section(HWNDManager* mgr)
{
    if (!mgr) return NULL;
    return &mgr->state_section;
}

int hwnd_publish_from_window(HWNDManager* mgr, HWND source, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!mgr || !source) return -1;
    HWNDEntry src;
    if (!copy_entry_for_hwnd(mgr, source, &src)) return -1;

    if (run_hooks(mgr, source, msg, &wp, &lp) == HOOK_BLOCK)
        return 0;

    /* v82.1: this function is the HWND-State-Section dirty-notify path, not
       normal Win32 message delivery.  v82 accidentally posted msg itself back
       to the source HWND with lParam=stateVersion.  After the legacy apps were
       ported to the MSDN WINDOWPOS contract they correctly interpreted
       WM_WINDOWPOSCHANGED.lParam as WINDOWPOS*, so the synthetic self-signal
       dereferenced a tiny integer and crashed on move/resize.

       Rule now:
         - real messages are delivered by wm_send_window_message/SendMessage;
         - WSTS notifications are payload-free signals;
         - every subscriber receives WM_MYOS_HWND_STATE_DIRTY with
           wParam=source HWND and lParam=state serial;
         - the source HWND is never signalled here. */
    HWND targets[MAX_SUBSCRIPTIONS];
    int target_count = 0;

    pthread_mutex_lock(&mgr->lock);
    for (int i = 0; i < MAX_SUBSCRIPTIONS && target_count < MAX_SUBSCRIPTIONS; i++) {
        HWNDSubscription* sub = &mgr->subscriptions[i];
        if (!sub->valid) continue;
        if (sub->subscriber == source) continue; /* no synthetic self-message */
        if (sub->source != source && sub->source != 0) continue;
        if ((sub->msgMin || sub->msgMax) && (msg < sub->msgMin || msg > sub->msgMax)) continue;
        targets[target_count++] = sub->subscriber;
    }

    for (int i = 0; i < target_count; i++) {
        // wParam = source HWND, lParam = shared state version/signal id.
        post_signal_unlocked(mgr, &src.cap, targets[i], source,
                             WM_MYOS_HWND_STATE_DIRTY, (WPARAM)source, lp);
    }
    pthread_mutex_unlock(&mgr->lock);
    return 0;
}

void hwnd_get_stats(HWNDManager* mgr, HWNDManagerStats* out)
{
    if (!mgr || !out) return;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&mgr->lock);
    out->hwnd_count = (uint32_t)mgr->count;
    out->subscription_count = mgr->subscription_count;
    out->state_version = mgr->state_section.updateSerial;
    out->state_capacity = mgr->state_section.capacity;
    out->state_active = mgr->state_section.activeCount;
    out->state_destroyed = mgr->state_section.destroyedCount;
    for (int i = 0; i < MAX_UI_THREADS; i++) {
        if (!mgr->threads[i].valid) continue;
        MyThreadQueue* tq = &mgr->threads[i];
        out->thread_count++;
        MyQueueStats s;
        myqueue_get_stats(&tq->queue, &s);
        out->posted += s.posted;
        out->dispatched += s.dispatched;
        out->dropped += s.dropped;
        out->coalesced += s.coalesced;
        out->current_depth += s.current_depth;
        out->send_count += tq->send_count;
        out->send_timeouts += tq->send_timeout_count;
        if (s.peak_depth > out->peak_depth) out->peak_depth = s.peak_depth;
        uint64_t now = myos_now_ns();
        uint64_t limit = 750ull * 1000000ull;
        int thread_hung = 0;
        if (tq->in_dispatch && tq->dispatch_start_ns && now - tq->dispatch_start_ns > limit)
            thread_hung = 1;
        if (s.current_depth > 0 && tq->last_pump_ns && now - tq->last_pump_ns > limit)
            thread_hung = 1;
        if (thread_hung) {
            for (int j = 0; j < MAX_HWNDS; j++) {
                if (mgr->entries[j].valid && mgr->entries[j].owner_pid == tq->pid && mgr->entries[j].owner_tid == tq->tid)
                    out->hung_windows++;
            }
        }
    }
    pthread_mutex_unlock(&mgr->lock);
}

uint32_t hwnd_get_owner_pid(HWNDManager* mgr, HWND hwnd)
{
    if (!mgr || !hwnd) return 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    uint32_t pid = (idx >= 0) ? mgr->entries[idx].owner_pid : 0;
    pthread_mutex_unlock(&mgr->lock);
    return pid;
}

uint32_t hwnd_get_owner_tid(HWNDManager* mgr, HWND hwnd)
{
    if (!mgr || !hwnd) return 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_entry_index(mgr, hwnd);
    uint32_t tid = (idx >= 0) ? mgr->entries[idx].owner_tid : 0;
    pthread_mutex_unlock(&mgr->lock);
    return tid;
}

static void hwnd_fill_process_info_locked(HWNDManager* mgr, DWORD pid, MyProcessInfo* out)
{
    memset(out, 0, sizeof(*out));
    out->cbSize = sizeof(*out);
    out->pid = pid;
    out->alive = TRUE;
    snprintf(out->szName, sizeof(out->szName), "pid-%u", pid);

    for (int i = 0; i < MAX_HWNDS; i++) {
        HWNDEntry* e = &mgr->entries[i];
        if (!e->valid || e->owner_pid != pid) continue;
        out->hwndCount++;
        out->capabilityFlags = e->cap.flags;
        snprintf(out->szName, sizeof(out->szName), "%s", e->cap.name);
    }

    for (int i = 0; i < MAX_UI_THREADS; i++) {
        MyThreadQueue* tq = &mgr->threads[i];
        if (!tq->valid || tq->pid != pid) continue;
        MyQueueStats qs;
        myqueue_get_stats(&tq->queue, &qs);
        out->uiQueueDepth += qs.current_depth;
    }
}

int hwnd_get_process_info(HWNDManager* mgr, DWORD pid, MyProcessInfo* out)
{
    if (!mgr || !pid || !out) return 0;
    pthread_mutex_lock(&mgr->lock);
    int seen = 0;
    for (int i = 0; i < MAX_HWNDS; i++) {
        if (mgr->entries[i].valid && mgr->entries[i].owner_pid == pid) { seen = 1; break; }
    }
    if (seen) hwnd_fill_process_info_locked(mgr, pid, out);
    pthread_mutex_unlock(&mgr->lock);
    return seen;
}

int hwnd_enum_processes(HWNDManager* mgr, MYPROCESSENUMPROC proc, LPARAM lParam)
{
    if (!mgr || !proc) return 0;
    DWORD pids[MAX_HWNDS];
    int count = 0;

    pthread_mutex_lock(&mgr->lock);
    for (int i = 0; i < MAX_HWNDS; i++) {
        HWNDEntry* e = &mgr->entries[i];
        if (!e->valid || !e->owner_pid) continue;
        int known = 0;
        for (int j = 0; j < count; j++) if (pids[j] == e->owner_pid) { known = 1; break; }
        if (!known && count < MAX_HWNDS) pids[count++] = e->owner_pid;
    }

    MyProcessInfo infos[MAX_HWNDS];
    for (int i = 0; i < count; i++) hwnd_fill_process_info_locked(mgr, pids[i], &infos[i]);
    pthread_mutex_unlock(&mgr->lock);

    for (int i = 0; i < count; i++) {
        if (!proc(&infos[i], lParam)) break;
    }
    return count;
}


int hwnd_set_thread_external_pump(HWNDManager* mgr, DWORD pid, DWORD tid, int enabled, const char* name)
{
    if (!mgr || !pid || !tid) return 0;
    pthread_mutex_lock(&mgr->lock);
    MyThreadQueue* tq = get_or_create_thread_queue_locked(mgr, pid, tid);
    if (!tq) { pthread_mutex_unlock(&mgr->lock); return 0; }
    tq->external_pump = enabled ? 1 : 0;
    tq->last_pump_ns = myos_now_ns();
    if (name && name[0]) snprintf(tq->name, sizeof(tq->name), "%s", name);
    pthread_mutex_unlock(&mgr->lock);
    return 1;
}

DWORD hwnd_get_thread_queue_status(HWNDManager* mgr, DWORD pid, DWORD tid)
{
    if (!mgr || !pid || !tid) return 0;
    DWORD depth = 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_thread_index_locked(mgr, pid, tid);
    if (idx >= 0) {
        MyQueueStats qs;
        myqueue_get_stats(&mgr->threads[idx].queue, &qs);
        depth = qs.current_depth;
    }
    pthread_mutex_unlock(&mgr->lock);
    return depth;
}


DWORD hwnd_get_thread_queue_status_bits(HWNDManager* mgr, DWORD pid, DWORD tid, UINT flags)
{
    if (!mgr || !pid || !tid || !flags) return 0;
    MyThreadQueue* tq = NULL;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_thread_index_locked(mgr, pid, tid);
    if (idx >= 0) tq = &mgr->threads[idx];
    pthread_mutex_unlock(&mgr->lock);
    if (!tq) return 0;
    return myqueue_get_queue_status(&tq->queue, flags);
}

int hwnd_thread_queue_has_status(HWNDManager* mgr, DWORD pid, DWORD tid, UINT flags)
{
    if (!mgr || !pid || !tid || !flags) return 0;
    MyThreadQueue* tq = NULL;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_thread_index_locked(mgr, pid, tid);
    if (idx >= 0) tq = &mgr->threads[idx];
    pthread_mutex_unlock(&mgr->lock);
    if (!tq) return 0;
    return myqueue_has_queue_status(&tq->queue, flags);
}

uint64_t hwnd_get_thread_send_count(HWNDManager* mgr, DWORD pid, DWORD tid)
{
    if (!mgr || !pid || !tid) return 0;
    uint64_t v = 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_thread_index_locked(mgr, pid, tid);
    if (idx >= 0) v = mgr->threads[idx].send_count;
    pthread_mutex_unlock(&mgr->lock);
    return v;
}

uint64_t hwnd_get_thread_send_timeout_count(HWNDManager* mgr, DWORD pid, DWORD tid)
{
    if (!mgr || !pid || !tid) return 0;
    uint64_t v = 0;
    pthread_mutex_lock(&mgr->lock);
    int idx = find_thread_index_locked(mgr, pid, tid);
    if (idx >= 0) v = mgr->threads[idx].send_timeout_count;
    pthread_mutex_unlock(&mgr->lock);
    return v;
}

int hwnd_post_thread_message(HWNDManager* mgr, const Capability* sender, DWORD tid, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!mgr || !sender || !(sender->flags & CAP_IPC) || !tid) return -1;
    MyThreadQueue* tq = NULL;
    pthread_mutex_lock(&mgr->lock);
    for (int i = 0; i < MAX_UI_THREADS; i++) {
        if (mgr->threads[i].valid && mgr->threads[i].tid == tid) { tq = &mgr->threads[i]; break; }
    }
    if (!tq) { pthread_mutex_unlock(&mgr->lock); return -1; }
    MyMessage m; memset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    m.type = 1;
    m.sender_pid = sender->id;
    m.sender_tid = sender->id;
    m.target_pid = tq->pid;
    m.target_tid = tq->tid;
    m.hwnd = 0;
    m.msg = msg;
    m.wparam = wp;
    m.lparam = lp;
    m.priority = mymsg_default_priority(msg);
    int r = myqueue_post(&tq->queue, &m);
    pthread_mutex_unlock(&mgr->lock);
    return r;
}

int hwnd_is_window_hung(HWNDManager* mgr, HWND hwnd, DWORD timeout_ms)
{
    if (!mgr || !hwnd) return 0;
    uint32_t pid = 0, tid = 0;
    int in_dispatch = 0;
    uint64_t dispatch_start = 0;
    uint64_t last_pump = 0;
    uint32_t depth = 0;
    uint64_t now = myos_now_ns();

    pthread_mutex_lock(&mgr->lock);
    int eidx = find_entry_index(mgr, hwnd);
    if (eidx >= 0) {
        pid = mgr->entries[eidx].owner_pid;
        tid = mgr->entries[eidx].owner_tid;
        int tidx = find_thread_index_locked(mgr, pid, tid);
        if (tidx >= 0) {
            MyThreadQueue* tq = &mgr->threads[tidx];
            in_dispatch = tq->in_dispatch;
            dispatch_start = tq->dispatch_start_ns;
            last_pump = tq->last_pump_ns;
            MyQueueStats qs;
            myqueue_get_stats(&tq->queue, &qs);
            depth = qs.current_depth;
        }
    }
    pthread_mutex_unlock(&mgr->lock);

    uint64_t limit = (uint64_t)timeout_ms * 1000000ull;
    if (in_dispatch && dispatch_start && now - dispatch_start > limit) return 1;
    if (depth > 0 && last_pump && now - last_pump > limit) return 1;
    return 0;
}
