BUILD: myos_v131_hwnd_access_control_contract

Ziel von v131:
- Der HWND-Zugriffsvertrag ist jetzt explizit: Lesen, Mutieren und Message-Injection sind getrennte Pfade.
- CAP_WINDOW_READ erlaubt nur kontrollierte Metadatenabfragen fremder HWNDs.
- CAP_WINDOW_CONTROL mutiert fremde USER32-HWND-Metadaten nicht blind und hebelt Thread-Affinity nicht aus.
- Foreign PostMessage/SendMessage/DispatchMessage können keine WM_SETTEXT/WM_COMMAND/MDI-Messages in fremde WndProcs injizieren.
- MDI-Bäume bleiben gegen fremde WM_MDICREATE/WM_MDIDESTROY/WM_MDIACTIVATE-Injection geschützt.

Neu:
  hwnd_access summary :: checks=51 pass=51 fail=0 warn=0

Geprüft:
  make clean && make -j2
  ./myos_input --smoke all

Erwartet:
  BUILD: myos_v131_hwnd_access_control_contract
  SMOKE RESULT: PASS (0 failures)
