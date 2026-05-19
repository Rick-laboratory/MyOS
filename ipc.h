#pragma once
#include "message.h"
#include "capability.h"
#include <pthread.h>

// ─────────────────────────────────────────────
//  IPC Bus - Windows Message Style
//
//  PostMessage → async, kehrt sofort zurück
//  SendMessage → sync, wartet auf Antwort
//
//  Das OS ist der Postbote.
//  Absender weiß nicht ob Empfänger läuft.
//  Empfänger weiß nicht wer schickt.
//  Permission-Check einmal beim Senden.
//  Danach: Message liegt in der Queue.
// ─────────────────────────────────────────────

#define IPC_QUEUE_SIZE  64
#define IPC_MAX_PROCS   32
#define IPC_HASH_BUCKETS 64
#define IPC_HASH_MASK (IPC_HASH_BUCKETS - 1)

// IPC Message - wie WPARAM/LPARAM in WinAPI
typedef struct {
    uint32_t  sender;    // wer schickt  (wie HWND src)
    uint32_t  target;    // wer empfängt (wie HWND dst)
    uint16_t  msg;       // was          (wie WM_*)
    intptr_t  wparam;    // wert 1       (wie WPARAM)
    intptr_t  lparam;    // wert 2       (wie LPARAM)
} IPCMessage;

// WndProc equivalent - was der Empfänger registriert
typedef void (*WndProc)(uint32_t sender, uint16_t msg,
                        intptr_t wparam, intptr_t lparam,
                        void* userdata);

// Prozess-Eintrag im OS
typedef struct {
    int         valid;
    uint32_t    id;
    uint32_t    id_hash;
    int         hash_next;
    WndProc     proc;       // der message-handler
    void*       userdata;
    Capability  cap;
    // Message Queue
    IPCMessage  queue[IPC_QUEUE_SIZE];
    int         queue_head;
    int         queue_tail;
    int         queue_count;
    pthread_mutex_t lock;
} IPCProcess;

// Der globale Bus - kennt alle Prozesse
typedef struct {
    IPCProcess  procs[IPC_MAX_PROCS];
    int         count;       // high-water mark for dispatch iteration
    int         live_count;
    int         free_stack[IPC_MAX_PROCS];
    int         free_top;
    int         hash[IPC_HASH_BUCKETS];
    pthread_mutex_t lock;
} IPCBus;

// ── IPC Message-Typen (WM_* equivalent) ──────
#define WM_APP_DATA     0x0400  // generische daten
#define WM_APP_TEXT     0x0401  // text-nachricht
#define WM_APP_NOTIFY   0x0402  // benachrichtigung
#define WM_APP_REQUEST  0x0403  // anfrage mit antwort
#define WM_APP_REPLY    0x0404  // antwort auf anfrage

// Lifecycle
void ipc_init(IPCBus* bus);
int  ipc_register(IPCBus* bus, uint32_t id, WndProc proc,
                  void* userdata, Capability cap);
int  ipc_unregister(IPCBus* bus, uint32_t id);

// PostMessage - async, kehrt sofort zurück
int  ipc_post(IPCBus* bus, const Capability* sender_cap,
              uint32_t target, uint16_t msg,
              intptr_t wparam, intptr_t lparam);

// SendMessage - sync, wartet bis WndProc fertig
int  ipc_send(IPCBus* bus, const Capability* sender_cap,
              uint32_t target, uint16_t msg,
              intptr_t wparam, intptr_t lparam);

// Dispatch - vom render-thread aufrufen
// verteilt alle wartenden messages an ihre WndProcs
void ipc_dispatch(IPCBus* bus);
