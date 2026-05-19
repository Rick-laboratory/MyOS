#pragma once
#include "mytypes.h"
#include <stdint.h>

// ─────────────────────────────────────────────
//  Capability-System
//
//  Jede App bekommt beim Start ein Token.
//  Das Token entscheidet was sie darf.
//  Danach kein Check mehr - das Token IS
//  die Erlaubnis. Einmal geprüft, fertig.
//
//  Wie ein Hausausweis:
//  Einmal an der Tür kontrolliert,
//  danach kommst du in alle Räume
//  die dein Ausweis erlaubt.
// ─────────────────────────────────────────────

// Flags - was darf die App
#define CAP_NONE        0x00
#define CAP_FS_READ     0x01    // dateien lesen
#define CAP_FS_WRITE    0x02    // dateien schreiben
#define CAP_EXEC        0x04    // prozesse starten
#define CAP_IPC         0x08    // mit anderen apps reden
#define CAP_NET         0x10    // netzwerk (später)
#define CAP_HOOK        0x00000020u    // messages abfangen/verändern

// v18: Desktop-/Window-Capabilities, bewusst getrennt von normalem IPC.
#define CAP_WINDOW_ENUM      0x00000100u    // EnumWindows / Fensterliste
#define CAP_WINDOW_READ      0x00000200u    // GetWindowText/GetWindowRect/WSTS lesen
#define CAP_WINDOW_CONTROL   0x00000400u    // SetForegroundWindow/SetWindowPos/SetWindowText
#define CAP_WINDOW_SUBSCRIBE 0x00000800u    // auf fremde Window-State-Events subscriben
#define CAP_PROCESS_ENUM     0x00001000u    // v19: Process/App-Liste lesen
#define CAP_SECTION_MAP      0x00002000u    // v22: FileMapping/MapViewOfFile Section-Objekte

#define CAP_ALL         (CAP_FS_READ|CAP_FS_WRITE|CAP_EXEC|CAP_IPC|CAP_NET|CAP_HOOK|CAP_WINDOW_ENUM|CAP_WINDOW_READ|CAP_WINDOW_CONTROL|CAP_WINDOW_SUBSCRIBE|CAP_PROCESS_ENUM|CAP_SECTION_MAP)
#define CAP_ADMIN       CAP_ALL // alles bekannte (nur root-apps)

#define CAP_MAX_PATHS    8
#define CAP_MAX_TARGETS  8
#define CAP_PATH_LEN    64

typedef struct {
    MyCapId id;                             // eindeutige Capability-/App-ID
    uint32_t flags;                         // was darf sie
    char     allowed_paths[CAP_MAX_PATHS][CAP_PATH_LEN]; // welche pfade
    int      path_count;
    MyCapId ipc_targets[CAP_MAX_TARGETS];   // mit wem darf sie reden
    int      target_count;
    char     name[32];                      // app-name für logs
} Capability;

// Prüfung - nur einmal beim start aufgerufen
// Danach gibt es nur noch cap_can_* checks auf dem token
int  cap_allows_path(const Capability* cap, const char* path, uint32_t flag);
int  cap_allows_exec(const Capability* cap);
int  cap_allows_ipc (const Capability* cap, MyCapId target_id);

// Token-Builder - beim app-start aufrufen
Capability cap_create(MyCapId id, const char* name, uint32_t flags);
void       cap_add_path(Capability* cap, const char* path);
void       cap_add_target(Capability* cap, MyCapId target_id);
