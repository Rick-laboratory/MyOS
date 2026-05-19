#include "ipc.h"
#include <string.h>
#include <stdio.h>
#if defined(__GNUC__) || defined(__clang__)
#define MYOS_LIKELY(x) __builtin_expect(!!(x), 1)
#define MYOS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define MYOS_LIKELY(x) (x)
#define MYOS_UNLIKELY(x) (x)
#endif

// ─────────────────────────────────────────────
//  IPC Bus Implementierung
//  Der Bus kennt alle Prozesse.
//  Er ist der einzige der Messages zustellt.
//  Kein Prozess redet direkt mit einem anderen.
// ─────────────────────────────────────────────

static inline uint32_t ipc_hash_id(uint32_t id)
{
    uint32_t v = id ? id : 1u;
    v *= 2654435761u;
    v ^= v >> 16;
    return v ? v : 1u;
}

static inline int ipc_hash_bucket(uint32_t hash)
{
    return (int)(hash & IPC_HASH_MASK);
}

static void ipc_hash_insert_unlocked(IPCBus* bus, int idx)
{
    if (!bus || idx < 0 || idx >= IPC_MAX_PROCS || !bus->procs[idx].valid) return;
    uint32_t h = ipc_hash_id(bus->procs[idx].id);
    int b = ipc_hash_bucket(h);
    bus->procs[idx].id_hash = h;
    bus->procs[idx].hash_next = bus->hash[b];
    bus->hash[b] = idx + 1;
}

static void ipc_hash_remove_unlocked(IPCBus* bus, int idx)
{
    if (!bus || idx < 0 || idx >= IPC_MAX_PROCS || !bus->procs[idx].id_hash) return;
    int b = ipc_hash_bucket(bus->procs[idx].id_hash);
    int* link = &bus->hash[b];
    while (*link) {
        int cur = *link - 1;
        if (cur == idx) {
            *link = bus->procs[cur].hash_next;
            break;
        }
        link = &bus->procs[cur].hash_next;
    }
    bus->procs[idx].id_hash = 0;
    bus->procs[idx].hash_next = 0;
}

void ipc_init(IPCBus* bus)
{
    memset(bus, 0, sizeof(*bus));
    pthread_mutex_init(&bus->lock, NULL);
    bus->free_top = 0;
    for (int i = IPC_MAX_PROCS - 1; i >= 0; --i)
        bus->free_stack[bus->free_top++] = i;
}

// ── Prozess suchen ───────────────────────────

static IPCProcess* find_proc_unlocked(IPCBus* bus, uint32_t id)
{
    if (!bus || !id) return NULL;
    uint32_t h = ipc_hash_id(id);
    int b = ipc_hash_bucket(h);
    for (int link = bus->hash[b]; link; link = bus->procs[link - 1].hash_next) {
        int idx = link - 1;
        if (MYOS_UNLIKELY(idx < 0 || idx >= IPC_MAX_PROCS)) break;
        IPCProcess* p = &bus->procs[idx];
        if (MYOS_LIKELY(p->valid && p->id_hash == h && p->id == id))
            return p;
    }
    return NULL;
}

static IPCProcess* find_proc_locked(IPCBus* bus, uint32_t id)
{
    IPCProcess* p;
    pthread_mutex_lock(&bus->lock);
    p = find_proc_unlocked(bus, id);
    pthread_mutex_unlock(&bus->lock);
    return p;
}

int ipc_register(IPCBus* bus, uint32_t id, WndProc proc,
                 void* userdata, Capability cap)
{
    if (!bus || !id || !proc) return -1;
    pthread_mutex_lock(&bus->lock);

    IPCProcess* existing = find_proc_unlocked(bus, id);
    if (existing) {
        pthread_mutex_lock(&existing->lock);
        existing->proc = proc;
        existing->userdata = userdata;
        existing->cap = cap;
        existing->queue_head = existing->queue_tail = existing->queue_count = 0;
        pthread_mutex_unlock(&existing->lock);
        printf("[IPC] Prozess %u aktualisiert: '%s'\n", id, cap.name);
        pthread_mutex_unlock(&bus->lock);
        return (int)(existing - bus->procs);
    }

    if (bus->free_top <= 0) {
        pthread_mutex_unlock(&bus->lock);
        return -1;
    }

    int i = bus->free_stack[--bus->free_top];
    IPCProcess* p = &bus->procs[i];
    memset(p, 0, sizeof(*p));
    p->valid    = 1;
    p->id       = id;
    p->proc     = proc;
    p->userdata = userdata;
    p->cap      = cap;
    pthread_mutex_init(&p->lock, NULL);
    ipc_hash_insert_unlocked(bus, i);
    bus->live_count++;
    if (i + 1 > bus->count) bus->count = i + 1;
    printf("[IPC] Prozess %u registriert: '%s'\n", id, cap.name);
    pthread_mutex_unlock(&bus->lock);
    return i;
}

int ipc_unregister(IPCBus* bus, uint32_t id)
{
    if (!bus || !id) return -1;
    pthread_mutex_lock(&bus->lock);
    IPCProcess* proc = find_proc_unlocked(bus, id);
    if (!proc) {
        pthread_mutex_unlock(&bus->lock);
        return -1;
    }
    int idx = (int)(proc - bus->procs);
    ipc_hash_remove_unlocked(bus, idx);
    pthread_mutex_lock(&proc->lock);
    proc->valid = 0;
    proc->id = 0;
    proc->proc = NULL;
    proc->userdata = NULL;
    proc->queue_head = proc->queue_tail = proc->queue_count = 0;
    pthread_mutex_unlock(&proc->lock);
    if (bus->free_top < IPC_MAX_PROCS)
        bus->free_stack[bus->free_top++] = idx;
    if (bus->live_count > 0) bus->live_count--;
    while (bus->count > 0 && !bus->procs[bus->count - 1].valid)
        bus->count--;
    pthread_mutex_unlock(&bus->lock);
    return 0;
}

// ── PostMessage - async ───────────────────────
//  Message in Queue legen, sofort zurück.
//  Wie ein Brief in den Briefkasten werfen.

int ipc_post(IPCBus* bus, const Capability* sender_cap,
             uint32_t target, uint16_t msg,
             intptr_t wparam, intptr_t lparam)
{
    // Permission-Check - einmal, hier, nie wieder
    if (!cap_allows_ipc(sender_cap, target)) return -1;

    IPCProcess* proc = find_proc_locked(bus, target);
    if (!proc || !proc->valid) {
        printf("[IPC] Ziel %u nicht gefunden\n", target);
        return -1;
    }

    pthread_mutex_lock(&proc->lock);

    if (!proc->valid) {
        pthread_mutex_unlock(&proc->lock);
        return -1;
    }

    if (proc->queue_count >= IPC_QUEUE_SIZE) {
        printf("[IPC] Queue voll für Prozess %u\n", target);
        pthread_mutex_unlock(&proc->lock);
        return -1;
    }

    // Message in Queue schreiben - Ring-Buffer
    IPCMessage* m = &proc->queue[proc->queue_tail];
    m->sender = sender_cap->id;
    m->target = target;
    m->msg    = msg;
    m->wparam = wparam;
    m->lparam = lparam;

    proc->queue_tail  = (proc->queue_tail + 1) % IPC_QUEUE_SIZE;
    proc->queue_count++;

    printf("[IPC] PostMessage: %u → %u msg=0x%04x\n",
           sender_cap->id, target, msg);

    pthread_mutex_unlock(&proc->lock);
    return 0;
}

// ── SendMessage - sync ────────────────────────
//  WndProc direkt aufrufen, warten bis fertig.
//  Wie ein Telefonanruf.

int ipc_send(IPCBus* bus, const Capability* sender_cap,
             uint32_t target, uint16_t msg,
             intptr_t wparam, intptr_t lparam)
{
    if (!cap_allows_ipc(sender_cap, target)) return -1;

    IPCProcess* proc = find_proc_locked(bus, target);
    if (!proc || !proc->valid || !proc->proc) return -1;

    printf("[IPC] SendMessage: %u → %u msg=0x%04x\n",
           sender_cap->id, target, msg);

    // Direkt aufrufen - synchron
    proc->proc(sender_cap->id, msg, wparam, lparam, proc->userdata);
    return 0;
}

// ── Dispatch - alle wartenden Messages zustellen

void ipc_dispatch(IPCBus* bus)
{
    if (!bus) return;
    for (int i = 0; i < bus->count; i++) {
        IPCProcess* proc = &bus->procs[i];
        if (!proc->valid) continue;

        pthread_mutex_lock(&proc->lock);
        while (proc->valid && proc->queue_count > 0) {
            IPCMessage m = proc->queue[proc->queue_head];
            proc->queue_head  = (proc->queue_head + 1) % IPC_QUEUE_SIZE;
            proc->queue_count--;
            WndProc cb = proc->proc;
            void* userdata = proc->userdata;
            pthread_mutex_unlock(&proc->lock);

            // WndProc aufrufen - außerhalb des Locks
            if (cb) cb(m.sender, m.msg, m.wparam, m.lparam, userdata);

            pthread_mutex_lock(&proc->lock);
        }
        pthread_mutex_unlock(&proc->lock);
    }
}
