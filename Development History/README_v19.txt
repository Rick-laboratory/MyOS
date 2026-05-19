myOS v19 - Capability + Process/App AccessLab

Neu:
- v18/v19 zusammengezogen: Window-Capabilities + lightweight Process/App enumeration.
- Neue Flags:
  CAP_WINDOW_ENUM, CAP_WINDOW_READ, CAP_WINDOW_CONTROL, CAP_WINDOW_SUBSCRIBE, CAP_PROCESS_ENUM
- User32-nahe Calls prüfen jetzt Capabilities:
  EnumWindows, FindWindowA, MyGetWindowState, MyGetWindowStateSection,
  SetForegroundWindow, SetWindowPos, SetWindowTextA, MySubscribeWindowMessage,
  MyEnumProcesses, MyGetProcessInfo.
- Eigene Fenster darf eine App weiterhin lesen/kontrollieren; fremde Fenster brauchen explizite Rechte.
- Spy++ hat Admin/System-Rechte und bleibt das Kontroll-/Beobachtungswerkzeug.
- Orange A/M/S/D-Legende unten im Spy entfernt.
- Neue Test-App: AccessLab.

AccessLab:
- Startmenü -> AccessLab oder F8
- Probe: testet EnumWindows, MyEnumProcesses, MyGetWindowState
- Subscribe: testet Subscriptions auf ein fremdes Ziel
- Control: versucht SetWindowTextA auf ein fremdes Ziel
- Das Standard-AccessLab hat ENUM/READ/SUBSCRIBE/PROCESS_ENUM, aber KEIN CONTROL.
  Erwartung: ENUM/PROC/READ/SUB OK, CONTROL DENIED.

Bester Testablauf:
1. myOS starten.
2. F7 öffnen: Spy++ muss weiterhin Fenster listen, foreground/rename können.
3. F8 öffnen: AccessLab.
4. In AccessLab Probe drücken.
   Erwartet: ENUM=OK, PROC=OK, READ=OK.
5. Subscribe drücken.
   Erwartet: SUB=OK.
6. Control drücken.
   Erwartet: CONTROL=DENY, weil CAP_WINDOW_CONTROL fehlt.
7. Jetzt im Spy ein Fenster verschieben/umbenennen.
   AccessLab sollte bei Subscribe ein Signal zählen.
