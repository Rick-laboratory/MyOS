#include "app_waitlab.h"
#include "window.h"
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
#include "myobject.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "mycontrols.h"
#include "app_msdn_resize.h"
#include "processhost.h"
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

/* AUDIT(v119-lab): WaitLab is the kernel32/Object/Process compliance canary.
   It intentionally keeps many handles open so ObjectLab can see refcounts, and
   it mixes same-process handles, child-process handles, duplicate handles,
   file mappings, timers and ShellExecuteExA. If it breaks, classify first:
   handle ownership/access bug, wait-state bug, process-host bug, or UI command
   routing bug. Do not collapse this lab; it catches real MSDN contract drift. */


#ifndef TITLEBAR_H
#define TITLEBAR_H 24
#endif

#define WAITLAB_MAX_CREATED 32
#define WAITLAB_MAX_OPENS   16
#define WAITLAB_MAX_DUPS    16
#define WAITLAB_MAX_MANY    8
#define WAITLAB_LOG_LINES   11

typedef struct WaitLabApp {
    HWNDManager* mgr;
    HWND hWnd;
    Capability cap;
    pthread_mutex_t lock;

    HANDLE hEvent;                         // current primary event for Set/Reset/WaitOne/Duplicate/Open
    HANDLE hCreated[WAITLAB_MAX_CREATED];  // every Create Event click is kept open; ObjectLab can see them live
    char   createdName[WAITLAB_MAX_CREATED][64];
    int    createdCount;

    HANDLE hOpened[WAITLAB_MAX_OPENS];     // OpenEvent handles are kept so REF visibly rises
    int    openedCount;
    HANDLE hReadOnlyEvent;                // v31: SYNCHRONIZE-only handle for ACCESS_DENIED tests
    DWORD  accessDeniedCount;

    HANDLE hDuped[WAITLAB_MAX_DUPS];       // DuplicateHandle refs are kept so REF visibly rises
    int    dupCount;

    HANDLE hInherit;                        // inheritable duplicate used by CreateProcess-lite
    HANDLE hChildProcess;                   // parent-side PROCESS handle returned by CreateProcessA
    HANDLE hChildThread;                    // parent-side primary THREAD handle returned by CreateProcessA
    DWORD  childPid;
    DWORD  childTid;
    HANDLE hChildRemote;                    // handle value allocated in child handle table by cross-process DuplicateHandle
    HANDLE hChildContextEvent;              // v45: handle created while current runtime context == child
    DWORD  childContextCount;
    DWORD  childContextPidSeen;
    DWORD  childContextHandleOwner;
    DWORD  envApiCount;
    DWORD  moduleApiCount;
    DWORD  dllApiCount;
    DWORD  loaderApiCount;
    DWORD  consoleApiCount;
    DWORD  consolePid;
    HANDLE hConsoleProcess;
    DWORD  guiIpcApiCount;
    DWORD  guiIpcPid;
    HANDLE hGuiIpcProcess;

    HANDLE hMutex;
    HANDLE hSemaphore;
    HANDLE hTimer;
    DWORD syncCount;
    DWORD mutexReleaseCount;
    DWORD semReleaseCount;

    DWORD  spawnCount;
    DWORD  dupChildCount;
    DWORD  waitChildCount;
    DWORD  termChildCount;
    DWORD  childExitCode;

    HANDLE hMany[WAITLAB_MAX_MANY][3];     // each Create 3 creates a fresh trio; current trio is hMany[manyIndex]
    int    manyCount;
    int    manyIndex;

    DWORD serial;
    DWORD createCount;
    DWORD openCount;
    DWORD setCount;
    DWORD resetCount;
    DWORD waitCount;
    DWORD timeoutCount;
    DWORD closeCount;
    DWORD lastResult;
    char  status[256];
    MyAppResizeState resize;
    char  log[WAITLAB_LOG_LINES][128];
    int   logCount;
} WaitLabApp;

static WaitLabApp g_wait;

static void wait_log_locked(const char* s)
{
    if (!s) return;
    if (g_wait.logCount < WAITLAB_LOG_LINES) snprintf(g_wait.log[g_wait.logCount++], sizeof(g_wait.log[0]), "%s", s);
    else {
        for (int i = 1; i < WAITLAB_LOG_LINES; i++) snprintf(g_wait.log[i-1], sizeof(g_wait.log[0]), "%s", g_wait.log[i]);
        snprintf(g_wait.log[WAITLAB_LOG_LINES-1], sizeof(g_wait.log[0]), "%s", s);
    }
}


static void button(Framebuffer* fb, int x, int y, int w, const char* label)
{
    fb_rect(fb, x, y, w, 20, COLOR(45,50,70));
    fb_rect_outline(fb, x, y, w, 20, COLOR(120,140,175));
    font_draw_str(fb, x + 7, y + 6, label, WHITE);
}

static void post_self(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_wait.mgr && g_wait.hWnd) hwnd_post(g_wait.mgr, &g_wait.cap, g_wait.hWnd, msg, wp, lp);
}

static void ensure_runtime(void)
{
    MyWinBindRuntime(g_wait.mgr, &g_wait.cap);
}


static LPCSTR waitlab_prepare_demo_directory(LPCSTR path)
{
    if (!path || !path[0]) return NULL;
    if (mkdir(path, 0777) == 0 || errno == EEXIST) return path;
    return "/tmp";
}

static const char* wait_result_name(DWORD r)
{
    if (r == WAIT_OBJECT_0) return "WAIT_OBJECT_0";
    if (r == WAIT_OBJECT_0 + 1) return "WAIT_OBJECT_0+1";
    if (r == WAIT_OBJECT_0 + 2) return "WAIT_OBJECT_0+2";
    if (r == WAIT_TIMEOUT) return "WAIT_TIMEOUT";
    if (r == WAIT_FAILED) return "WAIT_FAILED";
    return "WAIT_OTHER";
}

static HANDLE waitlab_current_many(int i)
{
    if (g_wait.manyIndex < 0 || g_wait.manyIndex >= WAITLAB_MAX_MANY) return 0;
    if (i < 0 || i >= 3) return 0;
    return g_wait.hMany[g_wait.manyIndex][i];
}

static void waitlab_create_event(void)
{
    ensure_runtime();

    char name[64];
    DWORD serial = ++g_wait.serial;
    snprintf(name, sizeof(name), "Local\\myos.waitlab.manual.%u", serial);
    HANDLE h = CreateEventA(NULL, TRUE, FALSE, name);

    pthread_mutex_lock(&g_wait.lock);
    g_wait.createCount++;
    if (h) {
        g_wait.hEvent = h;
        if (g_wait.createdCount < WAITLAB_MAX_CREATED) {
            int idx = g_wait.createdCount++;
            g_wait.hCreated[idx] = h;
            snprintf(g_wait.createdName[idx], sizeof(g_wait.createdName[idx]), "%s", name);
        }
        snprintf(g_wait.status, sizeof(g_wait.status), "CreateEventA unique manual-reset name=%s handle=0x%x", name, h);
        wait_log_locked(g_wait.status);
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "CreateEventA failed - event table full or no CAP_IPC");
        wait_log_locked(g_wait.status);
    }
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_open_event(void)
{
    ensure_runtime();
    const char* name = NULL;
    pthread_mutex_lock(&g_wait.lock);
    if (g_wait.createdCount > 0) name = g_wait.createdName[g_wait.createdCount - 1];
    pthread_mutex_unlock(&g_wait.lock);

    HANDLE h = name ? OpenEventA(EVENT_ALL_ACCESS, FALSE, name) : 0;

    pthread_mutex_lock(&g_wait.lock);
    g_wait.openCount++;
    if (h) {
        if (g_wait.openedCount < WAITLAB_MAX_OPENS) g_wait.hOpened[g_wait.openedCount++] = h;
        else CloseHandle(h);
        snprintf(g_wait.status, sizeof(g_wait.status), "OpenEventA latest name=%s -> handle=0x%x kept opens=%d", name, h, g_wait.openedCount);
        wait_log_locked(g_wait.status);
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "OpenEventA failed - create an event first");
        wait_log_locked(g_wait.status);
    }
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_set_event(void)
{
    ensure_runtime();
    if (!g_wait.hEvent) waitlab_create_event();
    BOOL ok = g_wait.hEvent ? SetEvent(g_wait.hEvent) : FALSE;
    pthread_mutex_lock(&g_wait.lock);
    if (ok) g_wait.setCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "SetEvent(current=0x%x) -> %s", g_wait.hEvent, ok ? "TRUE/signaled" : "FALSE");
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_reset_event(void)
{
    ensure_runtime();
    BOOL ok = g_wait.hEvent ? ResetEvent(g_wait.hEvent) : FALSE;
    pthread_mutex_lock(&g_wait.lock);
    if (ok) g_wait.resetCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "ResetEvent(current=0x%x) -> %s", g_wait.hEvent, ok ? "TRUE/non-signaled" : "FALSE");
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_wait_one(DWORD ms)
{
    ensure_runtime();
    DWORD r = g_wait.hEvent ? WaitForSingleObject(g_wait.hEvent, ms) : WAIT_FAILED;
    pthread_mutex_lock(&g_wait.lock);
    g_wait.lastResult = r;
    g_wait.waitCount++;
    if (r == WAIT_TIMEOUT) g_wait.timeoutCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "WaitForSingleObject(current=0x%x, %ums) -> %s", g_wait.hEvent, ms, wait_result_name(r));
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_create_3(void)
{
    ensure_runtime();
    int slot = g_wait.manyCount % WAITLAB_MAX_MANY;
    for (int i = 0; i < 3; i++) {
        if (g_wait.hMany[slot][i]) { CloseHandle(g_wait.hMany[slot][i]); g_wait.hMany[slot][i] = 0; }
    }

    DWORD serial = ++g_wait.serial;
    HANDLE made[3] = {0,0,0};
    for (int i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof(name), "Local\\myos.waitlab.multi.%u.%d", serial, i);
        made[i] = CreateEventA(NULL, TRUE, i == 1 ? TRUE : FALSE, name);
        g_wait.hMany[slot][i] = made[i];
    }

    pthread_mutex_lock(&g_wait.lock);
    g_wait.manyIndex = slot;
    if (g_wait.manyCount < WAITLAB_MAX_MANY) g_wait.manyCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "Create 3 unique -> [0]=0x%x [1]=0x%x(signaled) [2]=0x%x current trio=%d", made[0], made[1], made[2], g_wait.manyIndex);
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_wait_many(BOOL all)
{
    ensure_runtime();
    if (!waitlab_current_many(0) || !waitlab_current_many(1) || !waitlab_current_many(2)) waitlab_create_3();
    HANDLE arr[3] = { waitlab_current_many(0), waitlab_current_many(1), waitlab_current_many(2) };
    DWORD r = WaitForMultipleObjects(3, arr, all, 100);
    pthread_mutex_lock(&g_wait.lock);
    g_wait.lastResult = r;
    g_wait.waitCount++;
    if (r == WAIT_TIMEOUT) g_wait.timeoutCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "WaitForMultipleObjects(current trio, waitAll=%s, 100ms) -> %s", all ? "TRUE" : "FALSE", wait_result_name(r));
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_duplicate(void)
{
    ensure_runtime();
    if (!g_wait.hEvent) waitlab_create_event();
    HANDLE h = 0;
    BOOL ok = DuplicateHandle(0, g_wait.hEvent, 0, &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    pthread_mutex_lock(&g_wait.lock);
    if (ok) {
        if (g_wait.dupCount < WAITLAB_MAX_DUPS) g_wait.hDuped[g_wait.dupCount++] = h;
        else CloseHandle(h);
    }
    _ObjectectInfo oi; memset(&oi, 0, sizeof(oi));
    BOOL hasInfo = g_wait.hEvent ? MyGetObjectInfo(g_wait.hEvent, &oi) : FALSE;
    snprintf(g_wait.status, sizeof(g_wait.status), "DuplicateHandle(current=0x%x) -> %s dup=0x%x kept=%d visible REF=%u", g_wait.hEvent, ok ? "TRUE" : "FALSE", h, g_wait.dupCount, hasInfo ? oi.ref_count : 0);
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_spawn_child(void)
{
    ensure_runtime();
    if (!g_wait.hEvent) waitlab_create_event();

    HANDLE hInh = 0;
    BOOL dupInh = g_wait.hEvent ? DuplicateHandle(GetCurrentProcess(), g_wait.hEvent, GetCurrentProcess(), &hInh, 0, TRUE, DUPLICATE_SAME_ACCESS) : FALSE;
    STARTUPINFOA si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.lpTitle = "WaitLab child via STARTUPINFO";
    si.dwX = 77;
    si.dwY = 88;
    si.dwFlags = STARTF_USEPOSITION | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOW;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    char cmdLine[] = "--from-waitlab --v61-runtime-api-ready";
    char envBlock[] =
        "SystemRoot=C:\\myOS\0"
        "MYOS_VERSION=v61\0"
        "WAITLAB=env-block\0"
        "PATH=C:\\myOS\\System32;C:\\Labs\0"
        "\0";
    BOOL ok = dupInh ? CreateProcessA("waitlab-child-lite", cmdLine, NULL, NULL, TRUE, 0, envBlock, "/tmp/myos-v61", &si, &pi) : FALSE;

    pthread_mutex_lock(&g_wait.lock);
    g_wait.spawnCount++;
    if (ok) {
        if (g_wait.hInherit) CloseHandle(g_wait.hInherit);
        if (g_wait.hChildProcess) CloseHandle(g_wait.hChildProcess);
        if (g_wait.hChildThread) CloseHandle(g_wait.hChildThread);
        g_wait.hInherit = hInh;
        g_wait.hChildProcess = pi.hProcess;
        g_wait.hChildThread = pi.hThread;
        g_wait.childPid = pi.dwProcessId;
        g_wait.childTid = pi.dwThreadId;
        MyProcessLiteInfo pli; memset(&pli, 0, sizeof(pli));
        MyGetProcessLiteInfo(g_wait.childPid, &pli);
        snprintf(g_wait.status, sizeof(g_wait.status), "CreateProcessA+STARTUPINFO -> PID=%u TID=%u inherited=%u show=%u pos=%u,%u title=%.24s", g_wait.childPid, g_wait.childTid, pli.inherited_handles, pli.show_window, pli.startup_x, pli.startup_y, pli.window_title);
        wait_log_locked(g_wait.status);
    } else {
        if (hInh) CloseHandle(hInh);
        snprintf(g_wait.status, sizeof(g_wait.status), "CreateProcessA child failed - needs CAP_EXEC or inheritable source handle");
        wait_log_locked(g_wait.status);
    }
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_dup_to_child(void)
{
    ensure_runtime();
    if (!g_wait.hEvent) waitlab_create_event();
    if (!g_wait.hChildProcess) waitlab_spawn_child();

    HANDLE hRemote = 0;
    BOOL ok = (g_wait.hChildProcess && g_wait.hEvent) ? DuplicateHandle(GetCurrentProcess(), g_wait.hEvent, g_wait.hChildProcess, &hRemote, 0, FALSE, DUPLICATE_SAME_ACCESS) : FALSE;
    pthread_mutex_lock(&g_wait.lock);
    if (ok) {
        g_wait.hChildRemote = hRemote;
        g_wait.dupChildCount++;
        MyProcessLiteInfo pli; memset(&pli, 0, sizeof(pli));
        MyGetProcessLiteInfo(g_wait.childPid, &pli);
        snprintf(g_wait.status, sizeof(g_wait.status), "DuplicateHandle(parent current -> child PID=%u) -> child-handle=0x%x duplicated-in=%u", g_wait.childPid, hRemote, pli.duplicated_in);
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "DuplicateHandle into child failed - spawn child first");
    }
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}


static void waitlab_wait_child(void)
{
    ensure_runtime();
    if (!g_wait.hChildProcess) waitlab_spawn_child();
    DWORD r = g_wait.hChildProcess ? WaitForSingleObject(g_wait.hChildProcess, 100) : WAIT_FAILED;
    DWORD ec = STILL_ACTIVE;
    if (g_wait.hChildProcess) GetExitCodeProcess(g_wait.hChildProcess, &ec);
    pthread_mutex_lock(&g_wait.lock);
    g_wait.lastResult = r;
    g_wait.waitCount++;
    g_wait.waitChildCount++;
    g_wait.childExitCode = ec;
    if (r == WAIT_TIMEOUT) g_wait.timeoutCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "WaitForSingleObject(hProcess=0x%x,100ms) -> %s exit=%u", g_wait.hChildProcess, wait_result_name(r), ec);
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_terminate_child(void)
{
    ensure_runtime();
    if (!g_wait.hChildProcess) waitlab_spawn_child();
    BOOL ok = g_wait.hChildProcess ? TerminateProcess(g_wait.hChildProcess, 44) : FALSE;
    DWORD ec = STILL_ACTIVE;
    if (g_wait.hChildProcess) GetExitCodeProcess(g_wait.hChildProcess, &ec);
    pthread_mutex_lock(&g_wait.lock);
    if (ok) g_wait.termChildCount++;
    g_wait.childExitCode = ec;
    MyProcessLiteInfo pli; memset(&pli, 0, sizeof(pli));
    if (g_wait.childPid) MyGetProcessLiteInfo(g_wait.childPid, &pli);
    snprintf(g_wait.status, sizeof(g_wait.status), "TerminateProcess(child PID=%u,44) -> %s exit=%u childHandles=%u flags=0x%x", g_wait.childPid, ok ? "TRUE" : "FALSE", ec, pli.handle_count, pli.flags);
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_child_context_event(void)
{
    ensure_runtime();
    if (!g_wait.hChildProcess) waitlab_spawn_child();

    DWORD parentPid = GetCurrentProcessId();
    DWORD seenPid = 0;
    DWORD ownerPid = 0;
    HANDLE hChildLocal = 0;
    BOOL entered = g_wait.childPid ? MyWinEnterProcessContext(g_wait.childPid) : FALSE;
    if (entered) {
        seenPid = GetCurrentProcessId();
        char name[64];
        snprintf(name, sizeof(name), "Local\\myos.waitlab.childctx.%u", g_wait.childContextCount + 1);
        hChildLocal = CreateEventA(NULL, TRUE, TRUE, name);
        MyHandleInfo hi; memset(&hi, 0, sizeof(hi));
        if (hChildLocal && MyGetHandleInfo(hChildLocal, &hi)) ownerPid = hi.pid;
        MyWinLeaveProcessContext();
    }

    pthread_mutex_lock(&g_wait.lock);
    if (entered && hChildLocal) {
        g_wait.childContextCount++;
        g_wait.hChildContextEvent = hChildLocal;
        g_wait.childContextPidSeen = seenPid;
        g_wait.childContextHandleOwner = ownerPid;
        MyProcessLiteInfo pli; memset(&pli, 0, sizeof(pli));
        MyGetProcessLiteInfo(g_wait.childPid, &pli);
        snprintf(g_wait.status, sizeof(g_wait.status),
                 "ChildCtx: parent=%u enter PID=%u CreateEvent -> child-handle=0x%x ownerPID=%u childHandles=%u enters=%u",
                 parentPid, seenPid, hChildLocal, ownerPid, pli.handle_count, pli.runtime_enters);
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "ChildCtx failed - spawn live child first or no runtime cap");
    }
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}


static void waitlab_child_env_api(void)
{
    ensure_runtime();
    if (!g_wait.hChildProcess) waitlab_spawn_child();

    DWORD seenPid = 0;
    char cmd[128]; cmd[0] = 0;
    char cwdBefore[128]; cwdBefore[0] = 0;
    char cwdAfter[128]; cwdAfter[0] = 0;
    char myosVersion[32]; myosVersion[0] = 0;
    char pathSmall[6]; pathSmall[0] = 0;
    char expanded[128]; expanded[0] = 0;
    DWORD cwdNeed = 0;
    DWORD verLen = 0;
    DWORD pathNeed = 0;
    DWORD expNeed = 0;
    STARTUPINFOA si; memset(&si, 0, sizeof(si));
    BOOL setOk = FALSE;
    BOOL setEnvOk = FALSE;

    BOOL entered = g_wait.childPid ? MyWinEnterProcessContext(g_wait.childPid) : FALSE;
    if (entered) {
        seenPid = GetCurrentProcessId();
        snprintf(cmd, sizeof(cmd), "%s", GetCommandLineA());
        GetStartupInfoA(&si);
        cwdNeed = GetCurrentDirectoryA(sizeof(cwdBefore), cwdBefore);
        setOk = SetCurrentDirectoryA("/tmp/myos-v61-child-cwd");
        GetCurrentDirectoryA(sizeof(cwdAfter), cwdAfter);

        verLen = GetEnvironmentVariableA("MYOS_VERSION", myosVersion, sizeof(myosVersion));
        pathNeed = GetEnvironmentVariableA("PATH", pathSmall, sizeof(pathSmall));
        setEnvOk = SetEnvironmentVariableA("WAITLAB_RUNTIME", "child-ctx");
        expNeed = ExpandEnvironmentStringsA("%SystemRoot%|%MYOS_VERSION%|%WAITLAB_RUNTIME%", expanded, sizeof(expanded));
        MyWinLeaveProcessContext();
    }

    pthread_mutex_lock(&g_wait.lock);
    if (entered) {
        g_wait.envApiCount++;
        MyProcessLiteInfo pli; memset(&pli, 0, sizeof(pli));
        MyGetProcessLiteInfo(g_wait.childPid, &pli);
        snprintf(g_wait.status, sizeof(g_wait.status),
                 "EnvAPI: PID=%u cmd=%.24s cwdNeed=%u setCwd=%s MYOS=%s(%u) PATHneed=%u setEnv=%s expNeed=%u exp=%.44s env#=%u",
                 seenPid, cmd[0] ? cmd : "-", cwdNeed, setOk ? "TRUE" : "FALSE",
                 myosVersion[0] ? myosVersion : "-", verLen, pathNeed,
                 setEnvOk ? "TRUE" : "FALSE", expNeed, expanded[0] ? expanded : "-",
                 pli.environment_count);
        (void)cwdBefore; (void)cwdAfter; (void)pathSmall; (void)si;
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "EnvAPI failed - spawn child first or no runtime cap");
    }
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}


static void waitlab_child_module_api(void)
{
    char file[MYWIN_MAX_MODULE_PATH]; file[0] = 0;
    char tiny[16]; tiny[0] = 0;
    DWORD seenPid = 0;
    DWORD len = 0;
    DWORD tinyRet = 0;
    HMODULE self = 0;
    HMODULE named = 0;
    HMODULE bad = 0;
    BOOL entered = g_wait.childPid ? MyWinEnterProcessContext(g_wait.childPid) : FALSE;

    if (entered) {
        seenPid = GetCurrentProcessId();
        self = GetModuleHandleA(NULL);
        named = GetModuleHandleA("waitlab-child-lite");
        bad = GetModuleHandleA("missing.dll");
        len = GetModuleFileNameA(0, file, sizeof(file));
        tinyRet = GetModuleFileNameA(self, tiny, sizeof(tiny));
        MyWinLeaveProcessContext();
    }

    pthread_mutex_lock(&g_wait.lock);
    if (entered) {
        g_wait.moduleApiCount++;
        MyProcessLiteInfo pli; memset(&pli, 0, sizeof(pli));
        MyGetProcessLiteInfo(g_wait.childPid, &pli);
        snprintf(g_wait.status, sizeof(g_wait.status),
                 "ModuleAPI: PID=%u HMOD=0x%x named=0x%x bad=0x%x len=%u tinyRet=%u file=%.54s infoMod=0x%x name=%.20s",
                 seenPid, self, named, bad, len, tinyRet, file[0] ? file : "-", pli.main_module, pli.module_name);
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "ModuleAPI failed - spawn child first or no runtime cap");
    }
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}


static void waitlab_child_dll_api(void)
{
    HMODULE k1 = 0;
    HMODULE k2 = 0;
    HMODULE got = 0;
    HMODULE user = 0;
    HMODULE missingDll = 0;
    FARPROC pCmd = NULL;
    FARPROC pCreateEvent = NULL;
    FARPROC pMissing = NULL;
    BOOL free1 = FALSE;
    BOOL free2 = FALSE;
    BOOL setDir = FALSE;
    DWORD seenPid = 0;
    DWORD errMissingDll = 0;
    DWORD errMissingProc = 0;
    DWORD errFinal = 0;
    char dllPath[MYWIN_MAX_MODULE_PATH]; dllPath[0] = 0;
    char dllDir[MYWIN_MAX_MODULE_PATH]; dllDir[0] = 0;
    DWORD dllPathLen = 0;
    DWORD dllDirLen = 0;
    BOOL entered = g_wait.childPid ? MyWinEnterProcessContext(g_wait.childPid) : FALSE;

    if (entered) {
        seenPid = GetCurrentProcessId();
        setDir = SetDllDirectoryA("C:\\myOS\\TestDlls");
        dllDirLen = GetDllDirectoryA(sizeof(dllDir), dllDir);
        k1 = LoadLibraryA("kernel32.dll");
        k2 = LoadLibraryA("kernel32");
        got = GetModuleHandleA("kernel32.dll");
        user = LoadLibraryA("user32.dll");
        missingDll = LoadLibraryA("missing53.dll");
        errMissingDll = GetLastError();
        pCmd = k1 ? GetProcAddress(k1, "GetCommandLineA") : NULL;
        pCreateEvent = k1 ? GetProcAddress(k1, "CreateEventA") : NULL;
        pMissing = k1 ? GetProcAddress(k1, "NoSuchExport") : NULL;
        errMissingProc = GetLastError();
        dllPathLen = k1 ? GetModuleFileNameA(k1, dllPath, sizeof(dllPath)) : 0;
        free1 = k2 ? FreeLibrary(k2) : FALSE; // refcount 2 -> 1
        free2 = user ? FreeLibrary(user) : FALSE; // unload user32 again
        errFinal = GetLastError();
        MyWinLeaveProcessContext();
    }

    pthread_mutex_lock(&g_wait.lock);
    if (entered) {
        g_wait.dllApiCount++;
        MyProcessLiteInfo pli; memset(&pli, 0, sizeof(pli));
        MyGetProcessLiteInfo(g_wait.childPid, &pli);
        snprintf(g_wait.status, sizeof(g_wait.status),
                 "DLLAPI: PID=%u k=0x%x got=0x%x dir=%s proc(cmd=%s evt=%s miss=%s) err(dll=%u proc=%u final=%u) free=%s/%s dll#=%u %.34s",
                 seenPid, k1, got, setDir ? dllDir : "-",
                 pCmd ? "yes" : "no", pCreateEvent ? "yes" : "no", pMissing ? "yes" : "no",
                 errMissingDll, errMissingProc, errFinal, free1 ? "TRUE" : "FALSE", free2 ? "TRUE" : "FALSE",
                 pli.dll_count, pli.dll_preview[0] ? pli.dll_preview : "-");
        (void)dllPath; (void)dllPathLen; (void)dllDirLen; (void)missingDll;
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "DLLAPI failed - spawn child first or no runtime cap");
    }
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}


static void waitlab_child_loader_api(void)
{
    ensure_runtime();
    if (!g_wait.hChildProcess) waitlab_spawn_child();

    MyProcessLiteInfo pli; memset(&pli, 0, sizeof(pli));
    BOOL ok = g_wait.childPid ? MyGetProcessLiteInfo(g_wait.childPid, &pli) : FALSE;

    pthread_mutex_lock(&g_wait.lock);
    if (ok) {
        g_wait.loaderApiCount++;
        snprintf(g_wait.status, sizeof(g_wait.status),
                 "LoaderAPI: PID=%u entry=%s called=%u imports=%u/%u err=%u dll#=%u preview=%.58s",
                 g_wait.childPid,
                 pli.loader_entry[0] ? pli.loader_entry : "-",
                 pli.loader_entry_called,
                 pli.loader_resolved_count,
                 pli.loader_import_count,
                 pli.loader_error,
                 pli.dll_count,
                 pli.loader_import_preview[0] ? pli.loader_import_preview : "-");
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "LoaderAPI failed - spawn child first");
    }
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

typedef struct WaitLabFindProcessCtx {
    DWORD latestPid;
    MyProcessLiteInfo info;
} WaitLabFindProcessCtx;

static BOOL waitlab_find_argdump_proc(const MyProcessLiteInfo* info, LPARAM lp)
{
    WaitLabFindProcessCtx* ctx = (WaitLabFindProcessCtx*)lp;
    if (!info || !ctx) return TRUE;
    if ((strcmp(info->image_name, "argdump") == 0 || strcmp(info->image_name, "argdump.exe") == 0 || strcmp(info->image_name, "argv-lab") == 0) && info->pid >= ctx->latestPid) {
        ctx->latestPid = info->pid;
        ctx->info = *info;
    }
    return TRUE;
}


static BOOL waitlab_find_ipc_gui_proc(const MyProcessLiteInfo* info, LPARAM lp)
{
    WaitLabFindProcessCtx* ctx = (WaitLabFindProcessCtx*)lp;
    if (!info || !ctx) return TRUE;
    if ((strcmp(info->image_name, "ipc-gui-lab") == 0 || strcmp(info->image_name, "ipcgui") == 0) && info->pid >= ctx->latestPid) {
        ctx->latestPid = info->pid;
        ctx->info = *info;
    }
    return TRUE;
}

static void waitlab_console_api(void)
{
    ensure_runtime();

    SHELLEXECUTEINFOA sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "open";
    sei.lpFile = "argdump";
    sei.lpParameters = "alpha \"two words\" /flag";
    sei.lpDirectory = waitlab_prepare_demo_directory("/tmp/myos-v61-console");
    sei.nShow = SW_HIDE;

    BOOL ok = ShellExecuteExA(&sei);
    DWORD waitRes = WAIT_FAILED;
    DWORD exitCode = 0;
    WaitLabFindProcessCtx ctx; memset(&ctx, 0, sizeof(ctx));

    if (ok && sei.hProcess) {
        waitRes = WaitForSingleObject(sei.hProcess, 1000);
        GetExitCodeProcess(sei.hProcess, &exitCode);
        MyEnumProcessLite(waitlab_find_argdump_proc, (LPARAM)&ctx);
    }

    pthread_mutex_lock(&g_wait.lock);
    g_wait.consoleApiCount++;
    if (ok) {
        if (g_wait.hConsoleProcess) CloseHandle(g_wait.hConsoleProcess);
        g_wait.hConsoleProcess = sei.hProcess;
        g_wait.consolePid = ctx.latestPid;
        snprintf(g_wait.status, sizeof(g_wait.status),
                 "ConsoleAPI: v61 IPC argc=%u wait=%s exit=%u pid=%u linux=%d host=%s msg=%u hello=%u x=%u hb=%u sh=%s argv=%.24s",
                 ctx.info.argc, wait_result_name(waitRes), exitCode, ctx.latestPid, ctx.info.linux_pid,
                 ctx.info.process_host_state_name[0] ? ctx.info.process_host_state_name : "-",
                 ctx.info.ipc_messages, ctx.info.ipc_hello, ctx.info.ipc_exit_report, ctx.info.ipc_shared_heartbeat,
                 ctx.info.ipc_shared_status[0] ? ctx.info.ipc_shared_status : "-",
                 ctx.info.ipc_shared_argv_preview[0] ? ctx.info.ipc_shared_argv_preview : "-");
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "ConsoleAPI: ShellExecuteExA(argdump) failed err=%u", GetLastError());
    }
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_gui_ipc_api(void)
{
    ensure_runtime();

    SHELLEXECUTEINFOA sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "open";
    sei.lpFile = "ipc-gui-lab";
    sei.lpParameters = "";
    sei.lpDirectory = waitlab_prepare_demo_directory("/tmp/myos-v61-gui");
    sei.nShow = SW_SHOW;

    BOOL ok = ShellExecuteExA(&sei);
    WaitLabFindProcessCtx ctx; memset(&ctx, 0, sizeof(ctx));
    MyProcessHostInfo phi; memset(&phi, 0, sizeof(phi));
    if (ok) {
        MyEnumProcessLite(waitlab_find_ipc_gui_proc, (LPARAM)&ctx);
        if (ctx.latestPid) MyProcessHostGetInfo(ctx.latestPid, &phi);
    }

    pthread_mutex_lock(&g_wait.lock);
    g_wait.guiIpcApiCount++;
    if (ok) {
        if (g_wait.hGuiIpcProcess) CloseHandle(g_wait.hGuiIpcProcess);
        g_wait.hGuiIpcProcess = sei.hProcess;
        g_wait.guiIpcPid = ctx.latestPid;
        snprintf(g_wait.status, sizeof(g_wait.status),
                 "GuiIPC: rt pid=%u linux=%d hwnd=%u idx=%u cw=%u/%u q=%u/%u/%u post=%u/%u close=%u api=%u gm=%u dm=%u last=0x%04x",
                 ctx.latestPid, phi.linux_pid, phi.gui_hwnd, phi.gui_window_index,
                 phi.gui_create_request, phi.gui_create_ack,
                 phi.gui_msg_sent, phi.gui_msg_received, phi.gui_msg_dispatched,
                 phi.gui_post_request, phi.gui_post_ack, phi.gui_close_seen, phi.gui_runtime_api_calls, phi.gui_get_message_calls, phi.gui_dispatch_message_calls, phi.gui_last_msg);
    } else {
        snprintf(g_wait.status, sizeof(g_wait.status), "GuiIPC: ShellExecuteExA(ipc-gui-lab) failed err=%u", GetLastError());
    }
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}


static void waitlab_create_mutex(void)
{
    ensure_runtime();
    char name[64];
    DWORD serial = ++g_wait.serial;
    snprintf(name, sizeof(name), "Local\\myos.synclab.mutex.%u", serial);
    HANDLE h = CreateMutexA(NULL, TRUE, name);
    pthread_mutex_lock(&g_wait.lock);
    if (h) {
        if (g_wait.hMutex) CloseHandle(g_wait.hMutex);
        g_wait.hMutex = h;
        g_wait.syncCount++;
        snprintf(g_wait.status, sizeof(g_wait.status), "CreateMutexA(initialOwner=TRUE) name=%s handle=0x%x owned=yes", name, h);
    } else snprintf(g_wait.status, sizeof(g_wait.status), "CreateMutexA failed");
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_release_mutex(void)
{
    ensure_runtime();
    BOOL ok = g_wait.hMutex ? ReleaseMutex(g_wait.hMutex) : FALSE;
    pthread_mutex_lock(&g_wait.lock);
    if (ok) g_wait.mutexReleaseCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "ReleaseMutex(0x%x) -> %s", g_wait.hMutex, ok ? "TRUE/free" : "FALSE");
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_create_semaphore(void)
{
    ensure_runtime();
    char name[64];
    DWORD serial = ++g_wait.serial;
    snprintf(name, sizeof(name), "Local\\myos.synclab.sem.%u", serial);
    HANDLE h = CreateSemaphoreA(NULL, 1, 3, name);
    pthread_mutex_lock(&g_wait.lock);
    if (h) {
        if (g_wait.hSemaphore) CloseHandle(g_wait.hSemaphore);
        g_wait.hSemaphore = h;
        g_wait.syncCount++;
        snprintf(g_wait.status, sizeof(g_wait.status), "CreateSemaphoreA(count=1,max=3) name=%s handle=0x%x", name, h);
    } else snprintf(g_wait.status, sizeof(g_wait.status), "CreateSemaphoreA failed");
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_release_semaphore(void)
{
    ensure_runtime();
    LONG prev = -1;
    BOOL ok = g_wait.hSemaphore ? ReleaseSemaphore(g_wait.hSemaphore, 1, &prev) : FALSE;
    pthread_mutex_lock(&g_wait.lock);
    if (ok) g_wait.semReleaseCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "ReleaseSemaphore(0x%x,+1) -> %s prev=%ld", g_wait.hSemaphore, ok ? "TRUE" : "FALSE", (long)prev);
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_create_timer(void)
{
    ensure_runtime();
    char name[64];
    DWORD serial = ++g_wait.serial;
    snprintf(name, sizeof(name), "Local\\myos.synclab.timer.%u", serial);
    HANDLE h = CreateWaitableTimerA(NULL, FALSE, name);
    LARGE_INTEGER due; due.QuadPart = -(long long)250 * 10000ll; // Win32-style relative 250ms in 100ns units
    BOOL ok = h ? SetWaitableTimer(h, &due, 0, NULL, NULL, FALSE) : FALSE;
    pthread_mutex_lock(&g_wait.lock);
    if (ok) {
        if (g_wait.hTimer) CloseHandle(g_wait.hTimer);
        g_wait.hTimer = h;
        g_wait.syncCount++;
        snprintf(g_wait.status, sizeof(g_wait.status), "CreateWaitableTimerA + SetWaitableTimer(relative 250ms) handle=0x%x", h);
    } else {
        if (h) CloseHandle(h);
        snprintf(g_wait.status, sizeof(g_wait.status), "CreateWaitableTimerA/SetWaitableTimer failed");
    }
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_wait_mixed(void)
{
    ensure_runtime();
    if (!g_wait.hEvent) waitlab_create_event();
    if (!g_wait.hSemaphore) waitlab_create_semaphore();
    if (!g_wait.hTimer) waitlab_create_timer();
    SetEvent(g_wait.hEvent);
    HANDLE arr[3] = { g_wait.hEvent, g_wait.hSemaphore, g_wait.hTimer };
    DWORD r = WaitForMultipleObjects(3, arr, FALSE, 300);
    pthread_mutex_lock(&g_wait.lock);
    g_wait.lastResult = r;
    g_wait.waitCount++;
    if (r == WAIT_TIMEOUT) g_wait.timeoutCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "WaitForMultipleObjects(EVENT,SEMAPHORE,TIMER, waitAny,300ms) -> %s", wait_result_name(r));
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}


static void waitlab_open_readonly(void)
{
    ensure_runtime();
    const char* name = NULL;
    pthread_mutex_lock(&g_wait.lock);
    if (g_wait.createdCount > 0) name = g_wait.createdName[g_wait.createdCount - 1];
    pthread_mutex_unlock(&g_wait.lock);
    if (!name) waitlab_create_event();
    pthread_mutex_lock(&g_wait.lock);
    if (g_wait.createdCount > 0) name = g_wait.createdName[g_wait.createdCount - 1];
    pthread_mutex_unlock(&g_wait.lock);

    HANDLE h = name ? OpenEventA(SYNCHRONIZE, FALSE, name) : 0;
    pthread_mutex_lock(&g_wait.lock);
    if (g_wait.hReadOnlyEvent) CloseHandle(g_wait.hReadOnlyEvent);
    g_wait.hReadOnlyEvent = h;
    snprintf(g_wait.status, sizeof(g_wait.status), "OpenEventA(SYNCHRONIZE) latest -> roHandle=0x%x; SetEvent should DENY", h);
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_try_ro_set(void)
{
    ensure_runtime();
    if (!g_wait.hReadOnlyEvent) waitlab_open_readonly();
    BOOL ok = g_wait.hReadOnlyEvent ? SetEvent(g_wait.hReadOnlyEvent) : FALSE;
    pthread_mutex_lock(&g_wait.lock);
    if (!ok) g_wait.accessDeniedCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "SetEvent(roHandle=0x%x) -> %s%s", g_wait.hReadOnlyEvent, ok ? "TRUE" : "FALSE", ok ? "" : " / ACCESS_DENIED");
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_try_ro_wait(void)
{
    ensure_runtime();
    if (!g_wait.hReadOnlyEvent) waitlab_open_readonly();
    DWORD r = g_wait.hReadOnlyEvent ? WaitForSingleObject(g_wait.hReadOnlyEvent, 100) : WAIT_FAILED;
    pthread_mutex_lock(&g_wait.lock);
    g_wait.lastResult = r;
    g_wait.waitCount++;
    if (r == WAIT_TIMEOUT) g_wait.timeoutCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "WaitForSingleObject(roHandle=0x%x,100ms) -> %s", g_wait.hReadOnlyEvent, wait_result_name(r));
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_close_all(void)
{
    ensure_runtime();

    for (int i = 0; i < g_wait.dupCount; i++) if (g_wait.hDuped[i]) { CloseHandle(g_wait.hDuped[i]); g_wait.hDuped[i] = 0; }
    for (int i = 0; i < g_wait.openedCount; i++) if (g_wait.hOpened[i]) { CloseHandle(g_wait.hOpened[i]); g_wait.hOpened[i] = 0; }
    for (int m = 0; m < WAITLAB_MAX_MANY; m++) for (int i = 0; i < 3; i++) if (g_wait.hMany[m][i]) { CloseHandle(g_wait.hMany[m][i]); g_wait.hMany[m][i] = 0; }
    for (int i = 0; i < g_wait.createdCount; i++) if (g_wait.hCreated[i]) { CloseHandle(g_wait.hCreated[i]); g_wait.hCreated[i] = 0; }
    if (g_wait.hInherit) { CloseHandle(g_wait.hInherit); g_wait.hInherit = 0; }
    if (g_wait.hChildProcess) { CloseHandle(g_wait.hChildProcess); g_wait.hChildProcess = 0; }
    if (g_wait.hChildThread) { CloseHandle(g_wait.hChildThread); g_wait.hChildThread = 0; }
    if (g_wait.hMutex) { CloseHandle(g_wait.hMutex); g_wait.hMutex = 0; }
    if (g_wait.hSemaphore) { CloseHandle(g_wait.hSemaphore); g_wait.hSemaphore = 0; }
    if (g_wait.hTimer) { CloseHandle(g_wait.hTimer); g_wait.hTimer = 0; }
    if (g_wait.hReadOnlyEvent) { CloseHandle(g_wait.hReadOnlyEvent); g_wait.hReadOnlyEvent = 0; }
    if (g_wait.hConsoleProcess) { CloseHandle(g_wait.hConsoleProcess); g_wait.hConsoleProcess = 0; }
    if (g_wait.hGuiIpcProcess) { CloseHandle(g_wait.hGuiIpcProcess); g_wait.hGuiIpcProcess = 0; }

    pthread_mutex_lock(&g_wait.lock);
    g_wait.hEvent = 0;
    g_wait.createdCount = 0;
    g_wait.openedCount = 0;
    g_wait.dupCount = 0;
    g_wait.manyCount = 0;
    g_wait.manyIndex = -1;
    g_wait.childPid = 0;
    g_wait.childTid = 0;
    g_wait.hChildRemote = 0;
    g_wait.hChildContextEvent = 0;
    g_wait.childContextPidSeen = 0;
    g_wait.childContextHandleOwner = 0;
    g_wait.consolePid = 0;
    g_wait.closeCount++;
    snprintf(g_wait.status, sizeof(g_wait.status), "CloseHandle on events + sync objects done #%u", g_wait.closeCount);
    wait_log_locked(g_wait.status);
    pthread_mutex_unlock(&g_wait.lock);
}

static void waitlab_hit_test(int cx, int cy)
{
    if (cy >= 8 && cy < 28) {
        if (cx >= 8 && cx < 116) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_CREATE_EVENT, 0), 0); return; }
        if (cx >= 124 && cx < 224) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_OPEN_EVENT, 0), 0); return; }
        if (cx >= 232 && cx < 316) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_SET_EVENT, 0), 0); return; }
        if (cx >= 324 && cx < 416) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_RESET_EVENT, 0), 0); return; }
        if (cx >= 424 && cx < 524) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_WAIT_100, 0), 0); return; }
        if (cx >= 532 && cx < 640) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_WAIT_1000, 0), 0); return; }
    }
    if (cy >= 36 && cy < 56) {
        if (cx >= 8 && cx < 116) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_CREATE_3, 0), 0); return; }
        if (cx >= 124 && cx < 224) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_WAIT_ANY, 0), 0); return; }
        if (cx >= 232 && cx < 332) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_WAIT_ALL, 0), 0); return; }
        if (cx >= 340 && cx < 456) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_DUP_HANDLE, 0), 0); return; }
        if (cx >= 464 && cx < 572) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_CLOSE_EVENT, 0), 0); return; }
    }
    if (cy >= 64 && cy < 84) {
        if (cx >= 8 && cx < 132) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_SPAWN_CHILD, 0), 0); return; }
        if (cx >= 140 && cx < 264) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_DUP_TO_CHILD, 0), 0); return; }
        if (cx >= 272 && cx < 364) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_WAIT_CHILD, 0), 0); return; }
        if (cx >= 372 && cx < 472) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_TERM_CHILD, 0), 0); return; }
        if (cx >= 480 && cx < 588) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_CHILD_CONTEXT, 0), 0); return; }
        if (cx >= 596 && cx < 696) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_GUI_IPC_API, 0), 0); return; }
    }
    if (cy >= 92 && cy < 112) {
        if (cx >= 8 && cx < 100) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_RO_OPEN, 0), 0); return; }
        if (cx >= 108 && cx < 200) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_RO_SET, 0), 0); return; }
        if (cx >= 208 && cx < 300) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_RO_WAIT, 0), 0); return; }
        if (cx >= 308 && cx < 416) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_ENV_API, 0), 0); return; }
        if (cx >= 424 && cx < 532) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_MODULE_API, 0), 0); return; }
        if (cx >= 540 && cx < 640) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_DLL_API, 0), 0); return; }
        if (cx >= 648 && cx < 756) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_LOADER_API, 0), 0); return; }
    }
    if (cy >= 120 && cy < 140) {
        if (cx >= 8 && cx < 116) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_CREATE_MUTEX, 0), 0); return; }
        if (cx >= 124 && cx < 232) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_RELEASE_MUTEX, 0), 0); return; }
        if (cx >= 240 && cx < 348) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_CREATE_SEM, 0), 0); return; }
        if (cx >= 356 && cx < 464) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_RELEASE_SEM, 0), 0); return; }
        if (cx >= 472 && cx < 580) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_CREATE_TIMER, 0), 0); return; }
        if (cx >= 588 && cx < 704) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_WAIT_MIXED, 0), 0); return; }
        if (cx >= 712 && cx < 772) { post_self(WM_COMMAND, MAKEWPARAM((WORD)WAITLAB_CONSOLE_API, 0), 0); return; }
    }
}

static int waitlab_is_command(UINT cmd)
{
    switch (cmd) {
    case WAITLAB_CREATE_EVENT: case WAITLAB_OPEN_EVENT: case WAITLAB_SET_EVENT:
    case WAITLAB_RESET_EVENT: case WAITLAB_WAIT_100: case WAITLAB_WAIT_1000:
    case WAITLAB_CREATE_3: case WAITLAB_WAIT_ANY: case WAITLAB_WAIT_ALL:
    case WAITLAB_DUP_HANDLE: case WAITLAB_CLOSE_EVENT: case WAITLAB_SPAWN_CHILD:
    case WAITLAB_DUP_TO_CHILD: case WAITLAB_WAIT_CHILD: case WAITLAB_TERM_CHILD:
    case WAITLAB_CHILD_CONTEXT: case WAITLAB_ENV_API: case WAITLAB_MODULE_API:
    case WAITLAB_DLL_API: case WAITLAB_LOADER_API: case WAITLAB_CONSOLE_API:
    case WAITLAB_GUI_IPC_API: case WAITLAB_CREATE_MUTEX: case WAITLAB_RELEASE_MUTEX:
    case WAITLAB_CREATE_SEM: case WAITLAB_RELEASE_SEM: case WAITLAB_CREATE_TIMER:
    case WAITLAB_WAIT_MIXED: case WAITLAB_RO_OPEN: case WAITLAB_RO_SET:
    case WAITLAB_RO_WAIT:
        return 1;
    default:
        return 0;
    }
}

static void waitlab_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, void* userdata)
{
    (void)userdata;
    (void)wp;
    switch (msg) {
    case WM_CREATE:
        pthread_mutex_lock(&g_wait.lock);
        g_wait.hWnd = hwnd;
        MyAppResizeInit(&g_wait.resize, WAITLAB_W, WAITLAB_H, TITLEBAR_H);
        g_wait.lastResult = WAIT_FAILED;
        g_wait.manyIndex = -1;
        snprintf(g_wait.status, sizeof(g_wait.status), "SyncLab ready - v31 access checks + v32 namespace");
        wait_log_locked("WM_CREATE: Sync + Access + Namespace lab ready");
        pthread_mutex_unlock(&g_wait.lock);
        break;
    case WM_LBUTTONDOWN: waitlab_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); break;
    case WM_COMMAND: {
        UINT cmd = (UINT)LOWORD(wp);
        if (waitlab_is_command(cmd)) post_self(cmd, 0, 0);
        break;
    }
    case WAITLAB_CREATE_EVENT: waitlab_create_event(); break;
    case WAITLAB_OPEN_EVENT: waitlab_open_event(); break;
    case WAITLAB_SET_EVENT: waitlab_set_event(); break;
    case WAITLAB_RESET_EVENT: waitlab_reset_event(); break;
    case WAITLAB_WAIT_100: waitlab_wait_one(100); break;
    case WAITLAB_WAIT_1000: waitlab_wait_one(1000); break;
    case WAITLAB_CREATE_3: waitlab_create_3(); break;
    case WAITLAB_WAIT_ANY: waitlab_wait_many(FALSE); break;
    case WAITLAB_WAIT_ALL: waitlab_wait_many(TRUE); break;
    case WAITLAB_DUP_HANDLE: waitlab_duplicate(); break;
    case WAITLAB_CLOSE_EVENT: waitlab_close_all(); break;
    case WAITLAB_SPAWN_CHILD: waitlab_spawn_child(); break;
    case WAITLAB_DUP_TO_CHILD: waitlab_dup_to_child(); break;
    case WAITLAB_WAIT_CHILD: waitlab_wait_child(); break;
    case WAITLAB_TERM_CHILD: waitlab_terminate_child(); break;
    case WAITLAB_CHILD_CONTEXT: waitlab_child_context_event(); break;
    case WAITLAB_ENV_API: waitlab_child_env_api(); break;
    case WAITLAB_MODULE_API: waitlab_child_module_api(); break;
    case WAITLAB_DLL_API: waitlab_child_dll_api(); break;
    case WAITLAB_LOADER_API: waitlab_child_loader_api(); break;
    case WAITLAB_CONSOLE_API: waitlab_console_api(); break;
    case WAITLAB_GUI_IPC_API: waitlab_gui_ipc_api(); break;
    case WAITLAB_CREATE_MUTEX: waitlab_create_mutex(); break;
    case WAITLAB_RELEASE_MUTEX: waitlab_release_mutex(); break;
    case WAITLAB_CREATE_SEM: waitlab_create_semaphore(); break;
    case WAITLAB_RELEASE_SEM: waitlab_release_semaphore(); break;
    case WAITLAB_CREATE_TIMER: waitlab_create_timer(); break;
    case WAITLAB_WAIT_MIXED: waitlab_wait_mixed(); break;
    case WAITLAB_RO_OPEN: waitlab_open_readonly(); break;
    case WAITLAB_RO_SET: waitlab_try_ro_set(); break;
    case WAITLAB_RO_WAIT: waitlab_try_ro_wait(); break;
    case WM_GETMINMAXINFO:
        MyAppResizeOnGetMinMaxInfo(&g_wait.resize, lp, WAITLAB_MIN_W, WAITLAB_MIN_H);
        break;
    case WM_WINDOWPOSCHANGING:
        MyAppResizeOnWindowPosChanging(&g_wait.resize, lp);
        break;
    case WM_WINDOWPOSCHANGED:
        MyAppResizeOnWindowPosChanged(&g_wait.resize, lp, TITLEBAR_H);
        break;
    case WM_MOVE:
        MyAppResizeOnMove(&g_wait.resize, lp);
        break;
    case WM_SIZE:
        MyAppResizeOnSize(&g_wait.resize, wp, lp);
        break;
    case WM_CLOSE: DestroyWindow(hwnd); break;
    case WM_DESTROY: waitlab_close_all(); break;
    default: break;
    }
}

HWND waitlab_create(HWNDManager* mgr, int x, int y, Capability cap)
{
    (void)x; (void)y;
    memset(&g_wait, 0, sizeof(g_wait));
    pthread_mutex_init(&g_wait.lock, NULL);
    g_wait.mgr = mgr;
    g_wait.cap = cap;
    g_wait.manyIndex = -1;
    HWND h = hwnd_create(mgr, waitlab_wndproc, NULL, cap);
    g_wait.hWnd = h;
    return h;
}

void waitlab_destroy(void)
{
    waitlab_close_all();
    pthread_mutex_destroy(&g_wait.lock);
}

void waitlab_blit(HWND hwnd, int wx, int wy, int ww, int wh, Framebuffer* fb)
{
    (void)hwnd;
    int cx = wx + 1;
    int cy = wy + TITLEBAR_H;
    int cw = ww - 2;
    int ch = wh - TITLEBAR_H - 1;
    fb_rect(fb, cx, cy, cw, ch, COLOR(12,14,24));

    button(fb, cx + 8,   cy + 8, 108, "Create Event");
    button(fb, cx + 124, cy + 8, 100, "Open Event");
    button(fb, cx + 232, cy + 8, 84,  "Set");
    button(fb, cx + 324, cy + 8, 92,  "Reset");
    button(fb, cx + 424, cy + 8, 100, "Wait 100");
    button(fb, cx + 532, cy + 8, 108, "Wait 1000");

    button(fb, cx + 8,   cy + 36, 108, "Create 3");
    button(fb, cx + 124, cy + 36, 100, "WaitAny");
    button(fb, cx + 232, cy + 36, 100, "WaitAll");
    button(fb, cx + 340, cy + 36, 116, "Duplicate");
    button(fb, cx + 464, cy + 36, 108, "Close All");

    button(fb, cx + 8,   cy + 64, 124, "Spawn Child");
    button(fb, cx + 140, cy + 64, 124, "Dup->Child");
    button(fb, cx + 272, cy + 64, 92,  "WaitProc");
    button(fb, cx + 372, cy + 64, 100, "TermProc");
    button(fb, cx + 480, cy + 64, 108, "ChildCtx");
    button(fb, cx + 596, cy + 64, 100, "GUI IPC");

    button(fb, cx + 8,   cy + 92, 92,  "RO Open");
    button(fb, cx + 108, cy + 92, 92,  "RO Set");
    button(fb, cx + 208, cy + 92, 92,  "RO Wait");
    button(fb, cx + 308, cy + 92, 108, "EnvAPI");
    button(fb, cx + 424, cy + 92, 108, "Module");
    button(fb, cx + 540, cy + 92, 100, "DLL API");
    button(fb, cx + 648, cy + 92, 108, "Loader");

    button(fb, cx + 8,   cy + 120, 108, "Mutex");
    button(fb, cx + 124, cy + 120, 108, "RelMutex");
    button(fb, cx + 240, cy + 120, 108, "Semaphore");
    button(fb, cx + 356, cy + 120, 108, "RelSem");
    button(fb, cx + 472, cy + 120, 108, "Timer");
    button(fb, cx + 588, cy + 120, 116, "WaitMixed");
    button(fb, cx + 712, cy + 120, 60,  "Console");

    pthread_mutex_lock(&g_wait.lock);
    _ObjectectInfo oi; memset(&oi, 0, sizeof(oi));
    BOOL hasInfo = g_wait.hEvent ? MyGetObjectInfo(g_wait.hEvent, &oi) : FALSE;
    char line[256];
    snprintf(line, sizeof(line), "CUR=0x%x %s sig=%s man=%s REF=%u cre=%d open=%d dup=%d ro=0x%x denies=%u wait=%u tmo=%u last=%s",
             g_wait.hEvent, hasInfo ? _ObjectTypeName(oi.type) : "-",
             (hasInfo && (oi.flags & _OBJECT_FLAG_EVENT_SIGNALED)) ? "yes" : "no",
             (hasInfo && (oi.flags & _OBJECT_FLAG_EVENT_MANUAL_RESET)) ? "yes" : "no",
             hasInfo ? oi.ref_count : 0, g_wait.createdCount, g_wait.openedCount, g_wait.dupCount, g_wait.hReadOnlyEvent, g_wait.accessDeniedCount,
             g_wait.waitCount, g_wait.timeoutCount, wait_result_name(g_wait.lastResult));
    draw_clip_text(fb, cx + 8, cy + 152, line, COLOR(160,255,190), cx+8, cy+148, cw-16, 14);
    draw_clip_text(fb, cx + 8, cy + 168, g_wait.status, COLOR(230,230,250), cx+8, cy+164, cw-16, 14);

    char multi[256];
    MyProcessLiteInfo pli; memset(&pli, 0, sizeof(pli));
    if (g_wait.childPid) MyGetProcessLiteInfo(g_wait.childPid, &pli);
    snprintf(multi, sizeof(multi), "trio [0]=0x%x [1]=0x%x [2]=0x%x | child PID=%u tid=%u hProc=0x%x hThread=0x%x childHandle=0x%x",
             waitlab_current_many(0), waitlab_current_many(1), waitlab_current_many(2), g_wait.childPid, pli.thread_id, g_wait.hChildProcess, g_wait.hChildThread, g_wait.hChildRemote);
    draw_clip_text(fb, cx + 8, cy + 184, multi, COLOR(180,210,255), cx+8, cy+180, cw-16, 14);

    char proc[256];
    snprintf(proc, sizeof(proc), "process-lite flags=0x%x exit=%u handles=%u cap=0x%x show=%u start=%u,%u ctx=%u Env=%u Mod=%u DLL=%u Ldr=%u Con=%u enters=%u env#=%u dll#=%u lastErr=%u ldr=%u/%u err=%u host=%s",
             pli.flags, pli.exit_code, pli.handle_count, pli.cap_flags, pli.show_window, pli.startup_x, pli.startup_y, g_wait.childContextCount, g_wait.envApiCount, g_wait.moduleApiCount, g_wait.dllApiCount, g_wait.loaderApiCount, g_wait.consoleApiCount, pli.runtime_enters, pli.environment_count, pli.dll_count, pli.last_error, pli.loader_resolved_count, pli.loader_import_count, pli.loader_error, pli.process_host_state_name[0] ? pli.process_host_state_name : "-");
    draw_clip_text(fb, cx + 8, cy + 200, proc, COLOR(255,190,160), cx+8, cy+196, cw-16, 14);

    char sync[256];
    snprintf(sync, sizeof(sync), "sync MUTEX=0x%x SEM=0x%x TIMER=0x%x sync=%u relM=%u relS=%u mod=0x%x entry=%.8s sub=%.8s linux=%d fork=%u ph=%u/%u kill=%u ipc=%u msg=%u hb=%u sh=%.18s",
             g_wait.hMutex, g_wait.hSemaphore, g_wait.hTimer, g_wait.syncCount, g_wait.mutexReleaseCount, g_wait.semReleaseCount, pli.main_module, pli.loader_entry[0] ? pli.loader_entry : "-", pli.subsystem[0] ? pli.subsystem : "-", pli.linux_pid, pli.fork_exec, pli.process_host_polls, pli.process_host_reaps, pli.process_host_kills, pli.ipc_enabled, pli.ipc_messages, pli.ipc_shared_heartbeat, pli.ipc_shared_status[0] ? pli.ipc_shared_status : "-");
    draw_clip_text(fb, cx + 8, cy + 216, sync, COLOR(255,220,150), cx+8, cy+212, cw-16, 14);

    int log_y = cy + 242;
    fb_rect(fb, cx + 8, log_y - 4, cw - 16, ch - 254, COLOR(8,9,16));
    fb_rect_outline(fb, cx + 8, log_y - 4, cw - 16, ch - 254, COLOR(70,80,115));
    font_draw_str(fb, cx + 14, log_y, "Event / Wait log", COLOR(210,230,255));
    for (int i = 0; i < g_wait.logCount; i++) {
        draw_clip_text(fb, cx + 14, log_y + 18 + i * 16, g_wait.log[i], COLOR(220,220,230), cx+12, log_y+14+i*16, cw-24, 14);
    }
    pthread_mutex_unlock(&g_wait.lock);
}
