#include "terminal.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>

/* AUDIT(v119-app): Terminal is a shell/admin surface plus a text app. It depends
   on keyboard translation, process spawning, stdout/stderr collection and the
   top-level message pump. If terminal breaks after a refactor, treat it as a
   shell/process integration issue rather than a generic control regression. */


static const char key_normal[128] = {
    [KEY_A]='a',[KEY_B]='b',[KEY_C]='c',[KEY_D]='d',[KEY_E]='e',
    [KEY_F]='f',[KEY_G]='g',[KEY_H]='h',[KEY_I]='i',[KEY_J]='j',
    [KEY_K]='k',[KEY_L]='l',[KEY_M]='m',[KEY_N]='n',[KEY_O]='o',
    [KEY_P]='p',[KEY_Q]='q',[KEY_R]='r',[KEY_S]='s',[KEY_T]='t',
    [KEY_U]='u',[KEY_V]='v',[KEY_W]='w',[KEY_X]='x',[KEY_Y]='y',
    [KEY_Z]='z',
    [KEY_1]='1',[KEY_2]='2',[KEY_3]='3',[KEY_4]='4',[KEY_5]='5',
    [KEY_6]='6',[KEY_7]='7',[KEY_8]='8',[KEY_9]='9',[KEY_0]='0',
    [KEY_SPACE]=' ',[KEY_DOT]='.',[KEY_COMMA]=',',[KEY_MINUS]='-',
    [KEY_EQUAL]='=',[KEY_SEMICOLON]=';',[KEY_SLASH]='/',
    [KEY_APOSTROPHE]='\'',[KEY_BACKSLASH]='\\',[KEY_GRAVE]='`',
    [KEY_LEFTBRACE]='[',[KEY_RIGHTBRACE]=']',
};
static const char key_shift[128] = {
    [KEY_A]='A',[KEY_B]='B',[KEY_C]='C',[KEY_D]='D',[KEY_E]='E',
    [KEY_F]='F',[KEY_G]='G',[KEY_H]='H',[KEY_I]='I',[KEY_J]='J',
    [KEY_K]='K',[KEY_L]='L',[KEY_M]='M',[KEY_N]='N',[KEY_O]='O',
    [KEY_P]='P',[KEY_Q]='Q',[KEY_R]='R',[KEY_S]='S',[KEY_T]='T',
    [KEY_U]='U',[KEY_V]='V',[KEY_W]='W',[KEY_X]='X',[KEY_Y]='Y',
    [KEY_Z]='Z',
    [KEY_1]='!',[KEY_2]='"',[KEY_3]='#',[KEY_4]='$',[KEY_5]='%',
    [KEY_6]='&',[KEY_7]='/',[KEY_8]='(',[KEY_9]=')',[KEY_0]='=',
    [KEY_SPACE]=' ',[KEY_DOT]=':',[KEY_COMMA]=';',[KEY_MINUS]='_',
    [KEY_EQUAL]='+',[KEY_SEMICOLON]=':',[KEY_SLASH]='?',
    [KEY_GRAVE]='~',[KEY_LEFTBRACE]='{',[KEY_RIGHTBRACE]='}',
};

static void term_draw_clip_text(Framebuffer* fb, int x, int y, const char* s, Color c,
                                int clip_x, int clip_y, int clip_w, int clip_h)
{
    if (!fb || !s || clip_w <= 0 || clip_h <= 0) return;
    extern const unsigned char font8x8[95][8];
    while (*s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch >= 32 && ch <= 126) {
            const unsigned char* glyph = font8x8[ch - 32];
            for (int row = 0; row < 8; row++) {
                int py = y + row;
                if (py < clip_y || py >= clip_y + clip_h) continue;
                for (int col = 0; col < 8; col++) {
                    int px = x + col;
                    if (px < clip_x || px >= clip_x + clip_w) continue;
                    if (glyph[row] & (1 << col)) fb_pixel(fb, px, py, c);
                }
            }
        }
        x += 8;
        if (x >= clip_x + clip_w) break;
    }
}

static void scroll(Terminal* t)
{
    for (int r = 0; r < TERM_ROWS-1; r++)
        memcpy(t->buf[r], t->buf[r+1], TERM_COLS+1);
    memset(t->buf[TERM_ROWS-1], 0, TERM_COLS+1);
    t->row = TERM_ROWS-1;
}

static void newline(Terminal* t)
{
    t->row++;
    if (t->row >= TERM_ROWS) scroll(t);
}

void term_print(Terminal* t, const char* s)
{
    // v16.5: nicht mehr rechts abschneiden. Terminal-Text wird intern
    // auf breite Scrollback-Zeilen geschrieben und bei Bedarf umgebrochen.
    int col = (int)strlen(t->buf[t->row]);
    while (*s) {
        if (*s == '\n') {
            newline(t);
            col = 0;
        } else {
            if (col >= TERM_COLS) {
                newline(t);
                col = 0;
            }
            t->buf[t->row][col++] = *s;
            t->buf[t->row][col] = 0;
        }
        s++;
    }
}

// ─────────────────────────────────────────────
//  Kommando-Dispatcher - Tabellen-Stil
//  Jede Funktion: ein Kommando
// ─────────────────────────────────────────────

static void cmd_help(Terminal* t, const char* arg __attribute__((unused)))
{
    term_print(t, "Kommandos:");              newline(t);
    term_print(t, "  help          diese hilfe");     newline(t);
    term_print(t, "  clear         bildschirm leeren"); newline(t);
    term_print(t, "  cap           zeige mein token"); newline(t);
    term_print(t, "  write <datei> <text>  schreiben"); newline(t);
    term_print(t, "  read  <datei>         lesen");    newline(t);
    term_print(t, "  ipc <id> <txt> nachricht senden"); newline(t);
    term_print(t, "  <befehl>       shell-ausfuehrung"); newline(t);
}

static void cmd_clear(Terminal* t, const char* arg __attribute__((unused)))
{
    memset(t->buf, 0, sizeof(t->buf));
    t->row = 0;
}

static void cmd_cap(Terminal* t, const char* arg __attribute__((unused)))
{
    char line[80];
    snprintf(line, sizeof(line), "Token: '%s' (id=%u)",
             t->cap.name, t->cap.id);
    term_print(t, line); newline(t);

    // Flags anzeigen
    char flags[64] = "Flags: ";
    if (t->cap.flags & CAP_FS_READ)  strcat(flags, "READ ");
    if (t->cap.flags & CAP_FS_WRITE) strcat(flags, "WRITE ");
    if (t->cap.flags & CAP_EXEC)     strcat(flags, "EXEC ");
    if (t->cap.flags & CAP_IPC)      strcat(flags, "IPC ");
    if (t->cap.flags == CAP_NONE)    strcat(flags, "(keine)");
    term_print(t, flags); newline(t);

    // Erlaubte Pfade
    if (t->cap.path_count > 0) {
        term_print(t, "Pfade:"); newline(t);
        for (int i = 0; i < t->cap.path_count; i++) {
            char p[80];
            snprintf(p, sizeof(p), "  %s", t->cap.allowed_paths[i]);
            term_print(t, p); newline(t);
        }
    }
}

static void cmd_write(Terminal* t, const char* arg)
{
    // arg = "<pfad> <text>"
    char path[64], text[256];
    if (sscanf(arg, "%63s %255[^\n]", path, text) < 2) {
        term_print(t, "Verwendung: write <pfad> <text>"); newline(t);
        return;
    }
    int n = os_write_file(&t->cap, path, text, (int)strlen(text));
    if (n >= 0) {
        char line[80];
        snprintf(line, 80, "Geschrieben: %d bytes", n);
        term_print(t, line);
    } else {
        term_print(t, "[VERWEIGERT] Kein Schreibzugriff");
    }
    newline(t);
}

static void cmd_read(Terminal* t, const char* arg)
{
    char path[64];
    if (sscanf(arg, "%63s", path) < 1) {
        term_print(t, "Verwendung: read <pfad>"); newline(t);
        return;
    }
    char buf[512];
    int n = os_read_file(&t->cap, path, buf, sizeof(buf));
    if (n >= 0) {
        term_print(t, buf);
    } else {
        term_print(t, "[VERWEIGERT] Kein Lesezugriff");
    }
    newline(t);
}

static void cmd_exec(Terminal* t, const char* cmd)
{
    // Pipe-basiert für streaming output
    if (t->pipe_fd >= 0) { close(t->pipe_fd); t->pipe_fd = -1; }

    if (!cap_allows_exec(&t->cap)) {
        term_print(t, "[VERWEIGERT] Kein EXEC-Recht"); newline(t);
        return;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        char cols_env[32];
        snprintf(cols_env, sizeof(cols_env), "%d", t->last_visible_cols > 8 ? t->last_visible_cols : 80);
        setenv("COLUMNS", cols_env, 1);
        setenv("TERM", "xterm", 1);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    t->pipe_fd = pipefd[0];
}


// IPC Text Buffer - pro Terminal, bleibt gültig bis WndProc ihn liest
// 8 Slots im Ring damit schnelle Nachrichten nicht überschrieben werden
#define IPC_TEXT_SLOTS 8
static char ipc_text_ring[IPC_TEXT_SLOTS][64];
static int  ipc_text_idx = 0;

static void cmd_ipc(Terminal* t, const char* arg)
{
    uint32_t target;
    char text[64];
    if (sscanf(arg, "%u %63[^\n]", &target, text) < 2) {
        term_print(t, "Verwendung: ipc <hwnd> <text>"); newline(t);
        term_print(t, "Beispiel:   ipc 2 Hallo"); newline(t);
        return;
    }
    if (!t->mgr) {
        term_print(t, "[IPC] Kein HWND Manager"); newline(t);
        return;
    }
    // Text in Ring-Slot kopieren - sicher auch bei schnellen Posts
    int slot = ipc_text_idx % IPC_TEXT_SLOTS;
    ipc_text_idx++;
    memcpy(ipc_text_ring[slot], text, 64);
    ipc_text_ring[slot][63] = 0;

    int r = hwnd_post(t->mgr, &t->cap, (HWND)target,
                      WM_APP_TEXT,
                      (intptr_t)ipc_text_ring[slot],
                      (intptr_t)t->hwnd);
    char line[80];
    if (r == 0)
        snprintf(line, sizeof(line), "PostMessage → hwnd=%u OK", target);
    else
        snprintf(line, sizeof(line), "[VERWEIGERT] hwnd=%u", target);
    term_print(t, line); newline(t);
}

// ── Kommando-Tabelle - Dispatcher-Stil ───────

typedef struct {
    const char* name;
    void (*fn)(Terminal*, const char*);
} Command;

static const Command cmd_table[] = {
    { "help",  cmd_help  },
    { "clear", cmd_clear },
    { "cap",   cmd_cap   },
    { "write", cmd_write },
    { "read",  cmd_read  },
    { "ipc",   cmd_ipc   },
    { NULL, NULL }
};

static void handle_command(Terminal* t, const char* input)
{
    // Prompt anzeigen
    char line[TERM_COLS+4];
    snprintf(line, sizeof(line), "> %s", input);
    term_print(t, line); newline(t);

    if (!input[0]) return;

    // Kommando-Name extrahieren
    char name[32]; const char* arg = "";
    int i = 0;
    while (input[i] && input[i] != ' ' && i < 31) {
        name[i] = input[i]; i++;
    }
    name[i] = 0;
    if (input[i] == ' ') arg = input + i + 1;

    // Tabelle durchsuchen - O(n) aber n ist klein
    for (int j = 0; cmd_table[j].name; j++) {
        if (strcmp(name, cmd_table[j].name) == 0) {
            cmd_table[j].fn(t, arg);
            return;
        }
    }

    // Nicht gefunden → Shell (wenn EXEC erlaubt)
    cmd_exec(t, input);
}

// ── Öffentliche API ───────────────────────────

void term_init(Terminal* t, Capability cap, IPCBus* bus, HWNDManager* mgr)
{
    memset(t, 0, sizeof(*t));
    t->pipe_fd = -1;
    t->last_visible_cols = 80;
    t->cap     = cap;
    t->bus     = bus;
    t->mgr     = mgr;

    // Begrüßung mit Token-Info
    char line[80];
    snprintf(line, sizeof(line),
             "myos terminal - Token: %s", cap.name);
    term_print(t, line); newline(t);

    // Token-Flags anzeigen
    char flags[64] = "Rechte: ";
    if (cap.flags & CAP_FS_READ)  strcat(flags, "READ ");
    if (cap.flags & CAP_FS_WRITE) strcat(flags, "WRITE ");
    if (cap.flags & CAP_EXEC)     strcat(flags, "EXEC ");
    if (cap.flags == CAP_NONE)    strcat(flags, "KEINE");
    term_print(t, flags); newline(t);
}

void term_poll(Terminal* t)
{
    if (t->pipe_fd < 0) return;
    char buf[256];
    ssize_t n = read(t->pipe_fd, buf, sizeof(buf)-1);
    if (n > 0) { buf[n]=0; term_print(t, buf); }
    else if (n == 0) { close(t->pipe_fd); t->pipe_fd = -1; }
}

void term_keycode(Terminal* t, int keycode, int shift)
{
    if (keycode == KEY_ENTER) {
        t->input[t->input_len] = 0;
        handle_command(t, t->input);
        memset(t->input, 0, sizeof(t->input));
        t->input_len = 0;
        return;
    }
    if (keycode == KEY_BACKSPACE) {
        if (t->input_len > 0) t->input[--t->input_len] = 0;
        return;
    }
    if (keycode < 128 && t->input_len < TERM_COLS-1) {
        char c = shift ? key_shift[keycode] : key_normal[keycode];
        if (c) t->input[t->input_len++] = c;
    }
}

void term_draw(Terminal* t, Framebuffer* fb, int x, int y, int w, int h)
{
    fb_rect(fb, x, y, w, h, COLOR(15,15,15));

    int clip_x = x + TERM_PADDING;
    int clip_y = y + TERM_PADDING;
    int clip_w = w - TERM_PADDING * 2;
    int input_h = 14;
    int line_h = 10;
    if (clip_w < 8) clip_w = 8;

    int visible_cols = clip_w / 8;
    if (visible_cols < 8) visible_cols = 8;
    if (visible_cols > TERM_COLS) visible_cols = TERM_COLS;
    t->last_visible_cols = visible_cols;

    int iy = y + h - input_h;
    int clip_h = iy - clip_y - 1;
    int visible_rows = clip_h / line_h;
    if (visible_rows < 1) visible_rows = 1;

    // v16.6: Word-aware Soft-Wrap. Vorher wurde stumpf nach sichtbaren
    // Spalten geschnitten; das war sicher, sah bei ls/dir-Ausgaben aber
    // komisch aus. Jetzt brechen wir bevorzugt an Leerzeichen um und geben
    // Shell-Kommandos zusaetzlich COLUMNS=<Fensterbreite> mit.
    typedef struct TermSeg { int row; int start; int len; } TermSeg;
    enum { MAX_SEG = 4096 };
    TermSeg segs[MAX_SEG];
    int seg_count = 0;

    for (int r = 0; r < TERM_ROWS; r++) {
        const char* src = t->buf[r];
        int len = (int)strlen(src);
        int start = 0;
        while (start < len && seg_count < MAX_SEG) {
            while (start < len && src[start] == ' ') start++; // Fortsetzungszeilen nicht mit Leerraum fluten
            int remaining = len - start;
            int take = remaining;
            if (take > visible_cols) {
                take = visible_cols;
                int cut = -1;
                for (int k = take - 1; k >= visible_cols / 3; k--) {
                    if (src[start + k] == ' ' || src[start + k] == '	') { cut = k; break; }
                }
                if (cut > 0) take = cut;
            }
            if (take <= 0) take = remaining > visible_cols ? visible_cols : remaining;
            segs[seg_count].row = r;
            segs[seg_count].start = start;
            segs[seg_count].len = take;
            seg_count++;
            start += take;
            while (start < len && src[start] == ' ') start++;
        }
    }

    int first = seg_count - visible_rows;
    if (first < 0) first = 0;

    int ty = clip_y;
    char line[TERM_COLS + 1];
    for (int i = first; i < seg_count && ty + 8 < iy - 1; i++) {
        const char* src = t->buf[segs[i].row] + segs[i].start;
        int n = segs[i].len;
        if (n > visible_cols) n = visible_cols;
        if (n > TERM_COLS) n = TERM_COLS;
        memcpy(line, src, (size_t)n);
        line[n] = 0;
        term_draw_clip_text(fb, clip_x, ty, line, COLOR(180,255,180),
                            clip_x, clip_y, clip_w, clip_h);
        ty += line_h;
    }

    // Eingabezeile
    fb_rect(fb, x, iy, w, input_h, COLOR(20,20,30));
    fb_rect(fb, x, iy, w,  1, COLOR(50,50,80));

    int max_input_visible = visible_cols - 2;
    if (max_input_visible < 1) max_input_visible = 1;
    int input_start = 0;
    if (t->input_len > max_input_visible)
        input_start = t->input_len - max_input_visible;

    char prompt[TERM_COLS + 4];
    snprintf(prompt, sizeof(prompt), "$ %s", t->input + input_start);
    term_draw_clip_text(fb, clip_x, iy + 3, prompt, COLOR(100,200,255),
                        clip_x, iy + 1, clip_w, input_h - 1);

    if (t->blink) {
        int shown = t->input_len - input_start;
        int cx = clip_x + (shown + 2) * 8;
        if (cx > clip_x + clip_w - 7) cx = clip_x + clip_w - 7;
        fb_rect(fb, cx, iy + 3, 7, 9, COLOR(100,200,255));
    }
}

// ─────────────────────────────────────────────
//  WndProc - empfängt IPC Messages
//  Wird vom Bus aufgerufen wenn jemand
//  PostMessage oder SendMessage schickt.
//  Wie WndProc in der WinAPI.
// ─────────────────────────────────────────────

void term_wndproc(uint32_t sender, uint16_t msg,
                  intptr_t wparam, intptr_t lparam,
                  void* userdata)
{
    Terminal* t = (Terminal*)userdata;
    char line[80];

    switch (msg) {

    case WM_APP_TEXT: {
        // wparam = string, lparam = sender hwnd
        const char* text = (const char*)wparam;
        if (text && text[0]) {
            // sicher kopieren bevor wir irgendwas machen
            char safe[64];
            memcpy(safe, text, 63);
            safe[63] = 0;
            snprintf(line, 80, "[IPC hwnd=%ld] ", lparam);
            strncat(line, safe, 80 - strlen(line) - 1);
            term_print(t, line);
            newline(t);
        }
        break;
    }

    case WM_APP_NOTIFY:
        snprintf(line, sizeof(line),
                 "[IPC] Benachrichtigung von %u: %ld",
                 sender, wparam);
        term_print(t, line);
        newline(t);
        break;

    case WM_APP_DATA:
        snprintf(line, sizeof(line),
                 "[IPC] Daten von %u: wparam=%ld lparam=%ld",
                 sender, wparam, lparam);
        term_print(t, line);
        newline(t);
        break;

    default:
        snprintf(line, sizeof(line),
                 "[IPC] Unbekannte Message 0x%04x von %u", msg, sender);
        term_print(t, line);
        newline(t);
        break;
    }
}

// ─────────────────────────────────────────────
//  Terminal WndProc - empfängt alle OS Messages
//  Genau wie WndProc in der WinAPI.
//  Jede Tastatur, Maus, IPC Message kommt hier an.
// ─────────────────────────────────────────────

void term_hwnd_proc(HWND hwnd, UINT msg,
                    WPARAM wparam, LPARAM lparam,
                    void* userdata)
{
    Terminal* t = (Terminal*)userdata;
    char line[80];

    switch (msg) {

    case WM_CREATE:
        snprintf(line, sizeof(line), "HWND=%u erstellt", hwnd);
        term_print(t, line); newline(t);
        break;

    case WM_KEYDOWN:
        // wparam = keycode, lparam = shift
        term_keycode(t, (int)wparam, ((int)lparam) & 1);
        break;

    case WM_APP_TEXT: {
        // wparam = string, lparam = sender hwnd
        const char* text = (const char*)wparam;
        if (text && text[0]) {
            char safe[64];
            memcpy(safe, text, 63); safe[63] = 0;
            snprintf(line, 80, "[IPC hwnd=%ld] ", lparam);
            strncat(line, safe, 80 - strlen(line) - 1);
            term_print(t, line); newline(t);
        }
        break;
    }

    case WM_CLOSE:
        term_print(t, "Fenster wird geschlossen...");
        newline(t);
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        break;

    default:
        break;
    }
}
