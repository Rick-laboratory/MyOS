#include "os_calls.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

// ─────────────────────────────────────────────
//  os_read_file
//  Liest eine Datei - aber nur wenn das Token
//  CAP_FS_READ hat UND der Pfad erlaubt ist.
// ─────────────────────────────────────────────

int os_read_file(const Capability* cap, const char* path,
                 char* out_buf, int max_len)
{
    // Token-Check - einmal, hier, nie wieder
    if (!cap_allows_path(cap, path, CAP_FS_READ))
        return -1;

    // Ab hier: Token hat Erlaubnis - einfach machen
    FILE* f = fopen(path, "r");
    if (!f) {
        snprintf(out_buf, max_len, "Fehler: Datei nicht gefunden: %s", path);
        return -1;
    }

    int n = (int)fread(out_buf, 1, max_len-1, f);
    out_buf[n] = 0;
    fclose(f);

    printf("[OS] '%s' liest: %s (%d bytes)\n", cap->name, path, n);
    return n;
}

// ─────────────────────────────────────────────
//  os_write_file
//  Schreibt eine Datei - nur mit CAP_FS_WRITE
//  und erlaubtem Pfad.
// ─────────────────────────────────────────────

int os_write_file(const Capability* cap, const char* path,
                  const char* data, int len)
{
    if (!cap_allows_path(cap, path, CAP_FS_WRITE))
        return -1;

    FILE* f = fopen(path, "w");
    if (!f) {
        printf("[OS] '%s' Schreibfehler: %s\n", cap->name, path);
        return -1;
    }

    int n = (int)fwrite(data, 1, len, f);
    fclose(f);

    printf("[OS] '%s' schreibt: %s (%d bytes)\n", cap->name, path, n);
    return n;
}

// ─────────────────────────────────────────────
//  os_exec
//  Führt einen Prozess aus - nur mit CAP_EXEC.
//  Output wird direkt in out_buf geschrieben.
// ─────────────────────────────────────────────

int os_exec(const Capability* cap, const char* cmd,
            char* out_buf, int max_len)
{
    if (!cap_allows_exec(cap))  {
        snprintf(out_buf, max_len, "[VERWEIGERT] %s hat kein EXEC", cap->name);
        return -1;
    }

    printf("[OS] '%s' führt aus: %s\n", cap->name, cmd);

    // Pipe aufmachen, fork, exec
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }

    close(pipefd[1]);

    // Output lesen
    int total = 0, n;
    while ((n = (int)read(pipefd[0], out_buf + total,
                          max_len - total - 1)) > 0)
        total += n;
    out_buf[total] = 0;
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    return total;
}
