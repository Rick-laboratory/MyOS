BUILD: myos_v130_thread_window_ownership_contract

Ziel von v130:
  USER32 Thread-/Window-Ownership fuer den naechsten MDI-Ausbau haerten.

Wichtigste Aenderungen:
  - MyWinWindowInfo trennt jetzt Parent, Owner und Thread-Affinity:
      hParent     = visuelle/Clipping-Hierarchie fuer WS_CHILD
      hOwner      = Owner/Z-Order-Beziehung fuer Top-Level/Popup-Fenster
      dwProcessId = Prozess-/Capability-Owner
      dwThreadId  = UI-Thread-Affinity
  - GetParent() gibt nur den visuellen Parent zurueck.
  - GetWindow(GW_OWNER) gibt den Top-Level-Owner zurueck.
  - GWLP_HWNDPARENT bleibt als Win32-Legacy-Slot erhalten:
      Child  -> Parent
      Top-Level -> Owner
  - GetWindowThreadProcessId() ist jetzt Public-USER32-Vertrag.
  - AttachThreadInput() existiert als klare Lite-Semantik fuer gekoppelte Input-Queues.
  - Foreign CreateWindowExA(WS_CHILD, foreign parent) wird abgewiesen.
  - Foreign MoveWindow/DestroyWindow auf USER32-HWNDs wird abgewiesen.
  - SetFocus/SetCapture folgen Thread-/Input-Ownership statt globalem Durchgriff.
  - Cross-owner SendMessageA ruft nicht mehr blind fremde USER32-WNDPROCs direkt auf.
  - MDI-Aktivierung prueft gleiche Window-Owner-Invariante zwischen MDICLIENT und Child.

Neue Smoke-Gruppe:
  ownership summary :: checks=28 pass=28 fail=0 warn=0

Verifikation:
  make clean && make -j2
  ./myos_input --smoke all

Erwartet:
  BUILD: myos_v130_thread_window_ownership_contract
  SMOKE RESULT: PASS (0 failures)
