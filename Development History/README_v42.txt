myOS v42 - USER32 Window Tree / DestroyWindow Lifetime

Basis: v41 USER32 lifecycle.

Ziel:
  DestroyWindow soll nicht nur ein einzelnes HWND aus der Tabelle werfen,
  sondern Windows-naeher als Window-Tree-Operation funktionieren.

Neu / gehaertet:

  * DestroyWindow(parent) zerstoert Child-HWNDs rekursiv zuerst.
  * WM_DESTROY wird gesendet, solange IsWindow(hwnd) noch TRUE ist.
  * WindowLong/UserData/Parent/ID bleiben waehrend WM_DESTROY lesbar.
  * Danach wird das HWND aus dem HWNDManager entfernt.
  * WM_NCDESTROY kommt als letztes Lifecycle-Signal.
  * Capture wird freigegeben, wenn das Capture-HWND oder ein Descendant zerstoert wird.

Neue USER32-nahe APIs:

  * GetAncestor(hWnd, GA_PARENT / GA_ROOT)
  * GetWindow(hWnd, GW_CHILD / GW_HWNDFIRST / GW_HWNDLAST / GW_HWNDNEXT / GW_HWNDPREV / GW_OWNER-lite)
  * IsChild(hWndParent, hWnd)
  * EnumChildWindows(hWndParent, callback, lParam)

Neue Konstanten:

  * GW_HWNDFIRST, GW_HWNDLAST, GW_HWNDNEXT, GW_HWNDPREV, GW_OWNER, GW_CHILD
  * GA_PARENT, GA_ROOT

Warum das wichtig ist:

  v40.2 brachte echte BUTTON Child-HWNDs.
  v41 brachte WM_NCCREATE/WM_CREATE und WindowLong/Subclassing.
  v42 macht daraus einen echten Window-Tree-Lifetime-Pfad: Controls haengen
  semantisch am Parent und sterben mit ihm, statt als lose Spezialobjekte
  uebrigzubleiben.

Build:

  make clean && make

Run:

  sudo ./myos_input /dev/input/event1 /dev/input/event2

Empfohlene Tests:

  * ServiceLab oeffnen, Buttons klicken, Fenster schliessen, erneut oeffnen.
  * ControlLab/ClipMenuLab testen.
  * Mehrere Labs oeffnen/schliessen, damit Child-HWND-Cleanup mehrfach laeuft.

Bekannte Grenze:

  Die reine WindowManager-Schliesslogik ruft fuer Legacy-App-Fenster noch
  teilweise direkt hwnd_destroy(). Public USER32 DestroyWindow() ist jetzt
  der sauberere Pfad. Ein spaeterer Schritt sollte WindowManager-Close komplett
  ueber USER32/WM_CLOSE/DestroyWindow vereinheitlichen.
