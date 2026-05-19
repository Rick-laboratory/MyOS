myOS v17 - Shared WindowState Section

Neu:
- HWNDManager besitzt jetzt eine shared/read-only WindowState-Section.
- WindowManager schreibt den aktuellen Fensterzustand vor jedem Signal in diese Section.
- Queue trägt für Subscriber nur noch ein Signal:
  - Msg = WM_WINDOWPOSCHANGED
  - wParam = Source HWND
  - lParam = State-Version
- MyGetWindowState() liest primär aus der HWND-State-Section.
- MyGetWindowStateSection() gibt einen const Pointer auf die Section zurück.
- Spy++ pollt im Draw-Pfad nicht mehr; es updated seine Zeilen aus den Signalen und liest dann den aktuellen State.

Damit ist der geplante Schnitt drin:
Queue = Signal
Shared WindowState Section = Zustand
