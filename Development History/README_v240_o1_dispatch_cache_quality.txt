myOS / Linux-Win32 v240 - O(1) Dispatch & Cache Quality Pass
=============================================================

Basis: myos_v239_perf_o1_cache_quality

Ziel dieses Passes:
- weitere lineare Hotpath-Suchen durch slot-/hash-/free-stack-basierte Pfade ersetzen
- große Cold-Payloads aus heißen Window-Slots herausziehen
- Dialog-/USER32-Kompatibilität bei aggressiver Slot-Wiederverwendung stabil halten
- Smoke-Kompatibilität nach den Strukturänderungen beibehalten

Wesentliche Änderungen
----------------------

1) HWND Manager / USER object slots
- HWND-Slots werden über Free-Stack wiederverwendet statt über globalen Scan.
- UI-Thread-Queue-Slots werden über Free-Stack verwaltet.
- PID/TID -> UI-Thread-Queue nutzt Hash-Buckets statt linearem Scan.
- Thread-Queue-Tear-down nutzt owner_window_count statt die gesamte HWND-Tabelle zu scannen.

2) USER32 Child/Sibling-Metadaten
- MyWinWindowInfo hat intrusive sibling links:
  firstChild, lastChild, nextSibling, prevSibling.
- CreateWindowExA, SetParent und GWLP_HWNDPARENT pflegen diese Links.
- Dialog-Taborder und Dialog-Gruppen laufen jetzt über die Sibling-/Creation-Order.
  Das ist wichtig, weil HWND-Slots durch Free-Stacks nicht mehr automatisch creation order entsprechen.
- Z-/Hit-Test-nahe Child-Sammlung bleibt bewusst slot-order + z-sort kompatibel, damit bestehende Broker-/Proxy-Hit-Pfade nicht kippen.

3) WindowManager / Terminal Hot-Cold-Split
- Window enthält Terminal* statt inline Terminal.
- Der große Terminal-Scrollback/Input-Payload wird nur für APP_TERMINAL allokiert.
- Common compositor/window table walks berühren dadurch deutlich weniger Cold-Daten.
- Terminal IPC/pipe-Ressourcen werden beim Slot-Schließen freigegeben.

4) WindowManager Slot-Reuse
- Desktop/WindowManager-Slots nutzen einen Free-Stack für O(1)-nahe Wiederverwendung.
- Wiederverwendung geschlossener Slots vermeidet lineares Suchen über alte Tombstones.

5) IPC Bus
- IPC-Bus nutzt ID-Hash für Capability/Process-Auflösung.
- IPCProcess-Slots nutzen Free-Stack-Reuse.
- ipc_unregister entfernt Einträge sauber aus Hash-Buckets.

6) GDI Command Buffer
- GDI-Kommandos nutzen Free-Stack statt linearem freien Slot-Scan.
- WindowState führt eine per-HWND Command Chain.
- Clear/scroll/blit Pfade können per-HWND-Kommandos direkt einsammeln.
- GDI-Objekt-Free-Stacks und Hash-Indizes aus v239 bleiben erhalten.

7) Object Manager Audit-Counters
- _ObjectGetCount und _ObjectGetCountByType verwenden Live-Counter statt jedes Mal die gesamte Object-Tabelle zu scannen.
- Counter werden beim Prepare/Clear der Slots gepflegt.

8) Terminal-Pointer-Folgen
- main.c/window.c/winuser.c wurden auf Terminal* umgestellt.
- Null-Guards verhindern Zugriff auf nicht vorhandene Terminal-Payloads.

Validierung
-----------

Finale Validierung:
- make clean && make -j2: PASS
- ./myos_input --smoke all: PASS

Smoke-Ende:
SMOKE RESULT: PASS (0 failures)

Bewertung nach diesem Pass
--------------------------

Grobe technische Einschätzung:
- O(1)/Dispatch/Resolve: ca. 88-91%
- Cacheline-/Datenlayout-Qualität: ca. 70-75%

Noch nicht bei 100%, weil weiterhin große strukturelle Arbeiten offen bleiben:
- vollständiger Hot/Cold-Split für MyWinWindowInfo-Control-State, Dialog/MDI/Combo/Listbox-Daten
- zentraler Object Directory Namespace statt separater Objektfamilien-Indizes
- gezielte Waiter-Listen pro Dispatcher-Object statt globaler Broadcast-Semantik
- echter PE/DLL-Loader und vollständiger Service/Session-Manager
