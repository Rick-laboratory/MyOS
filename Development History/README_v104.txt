myOS v104 - MSDN Compliance Smoke-Test Gate
===========================================

Ziel
----
v104 baut auf v103 ChooseFont/Common-Dialog-Core auf, fügt aber keine breite neue USER32-Funktionalität oben drauf.
Der Hauptbaustein ist ein automatischer Regression-/Smoke-Test-Modus für den Weg Richtung 100% MSDN-Kompatibilität.

Neuer CLI-Modus
---------------

    ./myos_input --smoke all
    ./myos_input --smoke kernel32
    ./myos_input --smoke user32
    ./myos_input --smoke comdlg
    ./myos_input --smoke services
    ./myos_input --smoke apphost

Der Smoke-Modus braucht keine /dev/input/event* und keinen TTY/Framebuffer-Run.
Er initialisiert Object Manager, USER32 Runtime, WindowManager, Services und AppHost soweit nötig in-process.

Exitcode-Vertrag
----------------

    0  = PASS, keine Regression im getesteten Grundvertrag
    1  = FAIL, mindestens eine Regression
    2  = falscher Smoke-Modus / Usage

Ausgabeformat
-------------

    [PASS]  getesteter Vertragspfad funktioniert
    [FAIL]  Regression / harter Gate-Bruch
    [WARN]  bekannte MSDN-Lücke, sichtbar gemacht, aber v104-Baseline blockiert noch nicht
    [INFO]  Zusammenfassung

Aktuell getestete Gruppen
-------------------------

kernel32:
- CreateEventA / OpenEventA / SetEvent / ResetEvent
- CreateMutexA / ReleaseMutex
- CreateSemaphoreA / ReleaseSemaphore
- CreateWaitableTimerA / SetWaitableTimer
- WaitForSingleObject / WaitForMultipleObjects WAIT_ANY / WAIT_ALL
- CreateFileMappingA / OpenFileMappingA / MapViewOfFile / UnmapViewOfFile
- DuplicateHandle
- CloseHandle(NULL) und WaitForSingleObject(NULL) als MSDN-Edge-WARNs

user32:
- RegisterClassExA
- CreateWindowExA
- WM_CREATE delivery
- SendMessageA
- PostMessageA
- PeekMessageA(PM_REMOVE)
- DispatchMessageA
- SetWindowTextA / GetWindowTextA
- DestroyWindow / WM_DESTROY / IsWindow after destroy
- WARN: GetWindowRect/MoveWindow bei standalone CreateWindowExA-HWNDs ohne WindowManager-backed WindowState

comdlg:
- GetOpenFileNameA validation failure path
- ChooseFontA validation failure path
- CommDlgExtendedError wird gesetzt

services:
- OpenSCManagerA
- CreateServiceA
- StartServiceA
- QueryServiceStatus
- ControlService(STOP)
- DeleteService

apphost:
- MyAppHostIsRegistered(calc)
- MyAppHostIsRegistered(argdump)
- MyAppHostLaunchEx(argdump)
- echter fork/exec Child-Prozess über myos_apphost_child
- hProcess wird waitable/signaled
- MyGetProcessLiteInfo erkennt fork_exec

Bekannte WARNs aus v104 --smoke all
-----------------------------------

1. CloseHandle(NULL) gibt FALSE zurück, setzt aber noch nicht garantiert ERROR_INVALID_HANDLE.
2. WaitForSingleObject(NULL) gibt WAIT_FAILED zurück, setzt aber noch nicht garantiert ERROR_INVALID_HANDLE.
3. GetWindowRect auf standalone CreateWindowExA-HWNDs ohne WindowManager-State ist noch kein voller Win32-Vertrag.
4. MoveWindow/SetWindowPos brauchen aktuell noch einen WindowManager-backed Top-Level-Slot.

Diese WARNs sind absichtlich im Smoke-Gate sichtbar. Genau daraus werden die nächsten Compliance-Fixes.

Build-Test
----------

    make clean && make

Erwartung:

    gebaut: ./myos_input  [BUILD: myos_v104_smoke_test_compliance_gate]
    gebaut: ./myos_apphost_child  [BUILD: myos_v104_smoke_test_compliance_gate]

Automatischer Smoke-Test
------------------------

    ./myos_input --smoke all

Erwartung in v104:

    SMOKE RESULT: PASS (0 failures)

Manueller TTY/Framebuffer-Test durch Rick
-----------------------------------------

    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

Bitte wieder prüfen:

1. Startmenü öffnet.
2. Calc startet und Buttons reagieren beim ersten Klick.
3. Calc lässt sich schließen.
4. DialogLab öffnen.
5. OpenFile / SaveFile / ChooseFont öffnen und schließen.
6. Tab / Shift+Tab / Pfeiltasten in DialogLab prüfen.
7. Menüs/Submenüs öffnen und mit Escape schließen.
8. ControlLab öffnen, Buttons/Checkbox/Radio/Listbox/Combo/Scrollbar prüfen.
9. WaitLab öffnen, Event/Open/Set/Reset/Wait testen.
10. ObjectLab öffnen und OBJECT/SECTION/EVENT/SERVICE-Zeilen prüfen.
11. ServiceLab öffnen, Create/Start/Stop/Delete prüfen.
12. Danach mehrere Fenster bewegen/schließen: OS darf nicht hängen und nicht segfaulten.

Nächster sinnvoller Schritt
---------------------------

v105 sollte die WARNs aus v104 in echte PASS-Tests verwandeln:

- SetLastError(ERROR_INVALID_HANDLE) für CloseHandle(NULL/INVALID_HANDLE_VALUE)
- SetLastError(ERROR_INVALID_HANDLE) für WaitForSingleObject/WaitForMultipleObjects invalid handle
- CreateWindowExA-Top-Level-HWNDs konsistent mit WindowState/GetWindowRect verbinden
- MoveWindow/SetWindowPos nicht nur für WindowManager-Slots, sondern sauber für USER32-created top-level HWNDs definieren

