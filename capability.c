#include "capability.h"
#include <string.h>
#include <stdio.h>

// ─────────────────────────────────────────────
//  Token bauen
//  Wird einmal beim App-Start aufgerufen.
//  Danach ist das Token unveränderlich.
// ─────────────────────────────────────────────

Capability cap_create(MyCapId id, const char* name, uint32_t flags)
{
    Capability cap;
    memset(&cap, 0, sizeof(cap));
    cap.id    = id;
    cap.flags = flags;
    strncpy(cap.name, name, 31);
    return cap;
}

void cap_add_path(Capability* cap, const char* path)
{
    if (cap->path_count >= CAP_MAX_PATHS) return;
    strncpy(cap->allowed_paths[cap->path_count++], path, CAP_PATH_LEN-1);
}

void cap_add_target(Capability* cap, MyCapId target_id)
{
    if (cap->target_count >= CAP_MAX_TARGETS) return;
    cap->ipc_targets[cap->target_count++] = target_id;
}

// ─────────────────────────────────────────────
//  Checks - schnell, kein Syscall, kein Lock
//  Das Token liegt im RAM des Prozesses.
//  Ein Vergleich, fertig.
// ─────────────────────────────────────────────

int cap_allows_path(const Capability* cap, const char* path, uint32_t flag)
{
    // Flag prüfen
    if (!(cap->flags & flag)) {
        printf("[CAP] '%s' verweigert: kein %s-Flag\n",
               cap->name, flag == CAP_FS_READ ? "READ" : "WRITE");
        return 0;
    }

    // Pfad prüfen - darf der Token auf diesen Pfad?
    for (int i = 0; i < cap->path_count; i++) {
        const char* allowed = cap->allowed_paths[i];
        size_t len = strlen(allowed);
        if (len == 0) continue;

        // Boundary-Prefix:
        // /tmp erlaubt /tmp und /tmp/foo, aber NICHT /tmp_evil.
        if (strncmp(path, allowed, len) == 0 &&
            (path[len] == '\0' || path[len] == '/'))
            return 1;
    }

    printf("[CAP] '%s' verweigert: Pfad '%s' nicht erlaubt\n",
           cap->name, path);
    return 0;
}

int cap_allows_exec(const Capability* cap)
{
    if (cap->flags & CAP_EXEC) return 1;
    printf("[CAP] '%s' verweigert: kein EXEC-Flag\n", cap->name);
    return 0;
}

int cap_allows_ipc(const Capability* cap, MyCapId target_id)
{
    if (!(cap->flags & CAP_IPC)) {
        printf("[CAP] '%s' verweigert: kein IPC-Flag\n", cap->name);
        return 0;
    }
    for (int i = 0; i < cap->target_count; i++) {
        // 0 = wildcard = darf mit allen reden
        if (cap->ipc_targets[i] == 0 || cap->ipc_targets[i] == target_id)
            return 1;
    }
    printf("[CAP] '%s' verweigert: IPC zu %u nicht erlaubt\n",
           cap->name, target_id);
    return 0;
}
