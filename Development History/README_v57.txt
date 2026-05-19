myOS v57 - ProcessHost / waitpid bridge / process lifetime tracking

BUILD: myos_v57_processhost_lifetime

Ziel:
  v56 hatte echtes fork/exec fuer Console-Images, aber waitpid/kill/reap waren noch
  direkt ueber AppHost/MyWin verteilt. v57 zieht diese Linux-Prozess-Lifetime in
  einen zentralen ProcessHost.

Neu:
  - processhost.h / processhost.c
  - MyProcessHostSpawnConsole(...)
  - MyProcessHostTrack(...)
  - MyProcessHostPoll(...)
  - MyProcessHostPollAll(...)
  - MyProcessHostTerminate(...)
  - MyProcessHostGetInfo(...)

ProcessHost tracked pro Prozess:
  - myOS Process-Lite PID
  - echte Linux-PID
  - state: running/reaped/lost/killed
  - raw waitpid status
  - ExitCode
  - poll_count / reap_count / kill_count
  - start_ms / exit_ms
  - last_event Diagnose

WinAPI/Process-Lite Integration:
  - MyWinPollProcess() benutzt jetzt ProcessHost statt direkt waitpid(WNOHANG)
  - MyWinPollAllProcesses() pollt alle fork/exec-backed Process-Lite Eintraege
  - Renderloop ruft MyWinPollAllProcesses() periodisch auf, damit Zombies auch
    ohne explizites WaitForSingleObject abgeerntet werden
  - TerminateProcess() geht fuer echte Linux-Childs ueber ProcessHostTerminate()
  - GetExitCodeProcess()/WaitForSingleObject() bleiben auf PROCESS/THREAD Handles
    WinAPI-artig benutzbar

Diagnose:
  MyProcessLiteInfo / MyRuntimeContextInfo zeigen jetzt zusaetzlich:
  - process_host_state_name
  - process_host_polls
  - process_host_reaps
  - process_host_kills
  - process_host_start_ms / process_host_exit_ms
  - process_host_last_event

WaitLab:
  - Console-Button startet argdump via ShellExecuteExA + real fork/exec
  - Status zeigt host=<state>, poll/reap-Zaehler und echte Linux-PID

Test:
  make clean && make
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2
  WaitLab -> Console

Smoke intern getestet:
  - argdump fork/exec -> wait signaled, exit=57, host=reaped, raw=0x3900
  - sleeper fork/exec -> OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE), TerminateProcess(...,99)
    -> wait signaled, exit=99, host=killed, kills=1, raw SIGKILL status sichtbar

Wichtig:
  GUI-Apps laufen weiterhin im bisherigen In-Process WinMain/AppHost-Pfad.
  ProcessHost ist der Unterbau, den wir fuer den naechsten Schritt brauchen:
  GUI-AppHost per IPC an Session/WindowManager anbinden.
