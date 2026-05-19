myOS v41 - USER32 lifecycle / WindowLong / Subclassing
=======================================================

Ziel:
- Keine neue große Lab-Funktion, sondern USER32-Kernhärtung.
- CreateWindowExA kontrolliert jetzt den echten WinAPI-näheren Lifecycle.

Neu / geändert:

1) WM_NCCREATE / WM_NCDESTROY
   - hwnd.h definiert WM_NCCREATE (0x0081) und WM_NCDESTROY (0x0082).
   - DefWindowProcA(WM_NCCREATE) gibt TRUE zurück, damit CreateWindowExA wie Win32
     standardmäßig fortfahren kann.

2) CreateWindowExA-Reihenfolge
   - Neuer interner Core-Call: hwnd_create_no_create().
   - hwnd_create() bleibt kompatibel für ältere Labs und sendet weiter WM_CREATE.
   - CreateWindowExA() nutzt hwnd_create_no_create(), speichert dann zuerst:
       parent HWND, control ID, style, exStyle, rect, class name, text, hInstance,
       lpCreateParams, current WNDPROC
     und sendet danach synchron:
       WM_NCCREATE  (lParam = CREATESTRUCTA*)
       WM_CREATE    (lParam = CREATESTRUCTA*)
   - Das ist deutlich näher an USER32 als v40.2, wo WM_CREATE aus hwnd_create()
     kam, bevor die WinAPI-Metadaten fertig existierten.

3) CREATESTRUCTA
   - Neues CREATESTRUCTA / LPCREATESTRUCTA in mywin.h.
   - lpCreateParams wird aus CreateWindowExA(..., lpParam) durchgereicht.

4) WindowLong / Ptr API
   - GetWindowLongPtrA / SetWindowLongPtrA
   - GetWindowLongA / SetWindowLongA
   - Unterstützte Indizes:
       GWL_STYLE
       GWL_EXSTYLE
       GWLP_WNDPROC
       GWLP_HINSTANCE
       GWLP_HWNDPARENT
       GWLP_ID
       GWLP_USERDATA
       >= 0: tiny cbWndExtra backing store in LONG_PTR slots

5) Subclassing-Basis
   - GWLP_WNDPROC ist jetzt pro HWND gespeichert und per SetWindowLongPtrA änderbar.
   - Dispatch geht über den aktuellen per-window WndProc.
   - CallWindowProcA() ist vorhanden, damit Subclass-Procs den alten Proc aufrufen können.

6) Class cleanup
   - UnregisterClassA() vorhanden.

Test:
    make clean && make
    sudo ./myos_input /dev/input/event1 /dev/input/event2

Prüfen:
- ServiceLab öffnen und BUTTON child HWNDs klicken.
- ControlLab/ClipMenu/Calc/Editor öffnen, damit CreateWindowExA-Lifecycle quer
  über mehrere Apps getestet wird.
- Erwartung: ServiceLab-Click-Fix aus v39.2/v40 bleibt erhalten.

Ehrliche Grenze:
- WM_DESTROY/WM_NCDESTROY ist im Core jetzt sichtbarer, aber hwnd_destroy() ist
  noch kein vollständiger USER32 DestroyWindow-Stack mit Child-Destroy-Rekursion
  und PostQuitMessage-Semantik. Das wäre ein späterer v41.x/v42-Schritt.
