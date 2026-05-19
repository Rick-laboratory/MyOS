#pragma once
#include "capability.h"

// ─────────────────────────────────────────────
//  OS-Calls
//  Das sind die einzigen Wege wie eine App
//  auf Ressourcen zugreift. Kein direktes
//  fopen(), kein direktes system().
//  Immer durch das OS, immer mit Token.
//
//  Der Token wurde einmal beim Start geprüft.
//  Hier findet kein erneuter Permission-Check
//  statt - das Token IS die Erlaubnis.
//  Wir schauen nur ob das Token den Zugriff
//  grundsätzlich erlaubt.
// ─────────────────────────────────────────────

// Datei lesen - nur wenn CAP_FS_READ + Pfad erlaubt
int os_read_file(const Capability* cap,
                 const char* path,
                 char* out_buf,
                 int   max_len);

// Datei schreiben - nur wenn CAP_FS_WRITE + Pfad erlaubt
int os_write_file(const Capability* cap,
                  const char* path,
                  const char* data,
                  int         len);

// Prozess starten - nur wenn CAP_EXEC
// gibt pipe-fd zurück für output, -1 bei fehler
int os_exec(const Capability* cap,
            const char* cmd,
            char* out_buf,
            int   max_len);
