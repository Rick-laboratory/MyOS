myOS v80.1 - Spy++ / Nonclient signal fix
==========================================

Fix fuer v80 Regression:
- Mit offenem Spy++ konnte ein Titelbar-Move haengen.
- Ursache: WSTS/Spy subscription bekam transienten NC/System-Message-Namen
  (WM_NCHITTEST/WM_NCLBUTTONDOWN/WM_SYSCOMMAND), aber nur Signal-Payload
  (wParam=source HWND). DefWindowProc konnte das spaeter als echten HT*/SC_*
  Payload interpretieren.

Aenderungen:
1. Transiente NC/query/control-flow Messages aktualisieren weiterhin WSTS
   lastMessage, werden aber nicht mehr als Queue-Signale gepublished:
   WM_NCHITTEST, WM_NCMOUSEMOVE, WM_NCLBUTTONDOWN, WM_NCLBUTTONUP,
   WM_SYSCOMMAND, WM_WINDOWPOSCHANGING.
2. Spy++ nutzt Global-WSTS dirty notify (source==0) statt per-HWND Original-
   Message-Subscriptions.
3. Spy++ behandelt alle Subscription-Diagnose-Messages selbst und laesst sie
   nie in DefWindowProc fallen.
4. SetForegroundWindow no-oppt wenn das HWND bereits focused/active ist, damit
   bei Move/Resize keine unnoetigen ACTIVATE-Signale entstehen.
5. Versionstrings auf v80.1 aktualisiert.

Test:
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2

Regressionstest:
1. Terminal bewegen ohne Spy++ -> klappt.
2. Spy++ oeffnen -> Terminal bewegen -> darf nicht haengen.
3. Spy++ selbst an Titelbar bewegen -> darf nicht haengen.
4. In Spy++ ein Terminal in den Vordergrund holen -> danach Terminal bewegen -> darf nicht haengen.
5. Close/Minimize/Resize/Client-Clicks aus v79/v80 weiter testen.
