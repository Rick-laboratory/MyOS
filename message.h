#pragma once
#include <stdint.h>

// ─────────────────────────────────────────────
//  Das zentrale Nachrichtenformat des gesamten OS.
//  Jede Ebene spricht diese Sprache - von evdev
//  bis zur App. Immer dasselbe struct, immer
//  derselbe Dispatcher. Wie im Breadcaster,
//  nur systemweit.
// ─────────────────────────────────────────────

typedef struct {
    uint8_t  type;   // Index in die Handler-Tabelle
    intptr_t val1;   // Erster Wert  (key-code, x-pos, ...)
    intptr_t val2;   // Zweiter Wert (up/down, y-pos, ...)
} Message;

// ── Message-Typen (Stufe 1: Input-Layer) ──────
#define MSG_KEY_DOWN     0   // val1 = keycode
#define MSG_KEY_UP       1   // val1 = keycode
#define MSG_MOUSE_MOVE   2   // val1 = x, val2 = y (absolute / normalized)
#define MSG_MOUSE_DELTA  3   // val1 = dx, val2 = dy (relative mouse / VM)
#define MSG_MOUSE_BTN    4   // val1 = button, val2 = down/up
#define MSG_MOUSE_WHEEL  5   // val1 = delta
#define MSG_SYS_QUIT     6   // system beenden
#define MSG_COUNT        7   // immer am Ende - Größe der Tabelle

// ── Dispatcher-Makro ──────────────────────────
//  context  = zeiger auf was der handler braucht
//  table    = array von funktionszeigern
//  msg      = die nachricht
#define DISPATCH(context, table, msg) \
    do { \
        if ((msg).type < MSG_COUNT && (table)[(msg).type]) \
            (table)[(msg).type]((context), &(msg)); \
    } while(0)
