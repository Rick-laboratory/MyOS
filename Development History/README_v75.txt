myOS v75 - DWM-lite Surface Backing Store
========================================

Ziel
----
v75 ist der erste Schritt von "GDI-Commands ueber IPC replayen" zu einem echten
DWM-artigen persistenten Fensterinhalt:

    WindowState Section = Zustand / Position / Fokus / Titel
    Surface Section     = Pixel-Backing-Store
    Message/IPC         = Dirty-/Diagnose-Notify

Neue Komponenten
----------------
1) MySurfaceHeader in process_ipc.h
   - magic/version
   - width/height/stride/format
   - seqBegin/seqEnd fuer stabile Snapshots
   - dirty rect + frameSerial/paintSerial

2) SurfaceLab [OOP v75]
   - Rechtsklick-Menue -> SurfaceLab
   - echtes myos_apphost_child
   - erstellt Global\myos.v75.surface.lab per CreateFileMappingA
   - mapped die Section im Child via MapViewOfFile
   - schreibt XRGB8888-Pixel lokal in seinen eigenen mmap-View
   - Parent mapped denselben POSIX-shm Backing Store read-only und compositet ihn

3) Parent-Compositor-Pfad in window.c
   - liest surface_map_name/surface_size aus ProcessHostInfo
   - shm_open + mmap read-only
   - prueft MySurfaceHeader magic/version/seq
   - blitted Pixel in den Fenster-Clientbereich
   - GDI-IPC-Kommandos bleiben als Overlay fuer Text/Buttons/Diagnose erhalten

Testprozedur
------------
Start:

    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2

Dann:

    Rechtsklick -> SurfaceLab

Erwartung direkt nach dem Start:

    - SurfaceLab erscheint mit farbigem/gradientigem Inhalt.
    - Unten im Clientbereich steht eine gruenliche SURFACE-v75 Diagnosezeile.
    - Buttons/Overlaytext werden weiterhin ueber GDI-IPC ueber die Surface gelegt.

Buttons:

    Map       - erstellt/mappt die Surface erneut, falls nicht gemappt
    Gradient  - schreibt neuen Gradient-Frame in die Surface
    Boxes     - schreibt Box-/Pattern-Frame in die Surface
    Spam20    - schreibt 20 schnelle Frames; frame/seq/draws muessen steigen
    Clear     - schreibt dunklen Clear-Frame
    Unmap     - unmappt die Child-View; Surface sollte danach verschwinden/fallbacken

Wichtiger Persistenztest:

    1. SurfaceLab oeffnen.
    2. Gradient oder Boxes klicken.
    3. Ein anderes Fenster darueberziehen.
    4. Fenster wieder wegziehen.

Erwartung:

    Der Inhalt kommt aus der persistenten Surface-Section wieder, nicht aus
    zufaelligen alten Framebuffer-Pixeln. frame/seq bleiben stabil, bis du neu
    zeichnest.

Build
-----
Geprueft mit:

    make clean && make

Ergebnis:

    BUILD: myos_v75_dwm_lite_surface_backing_store
    gebaut: ./myos_input
    gebaut: ./myos_apphost_child

Limits
------
- Noch kein GPU-Compositor.
- Noch keine Alpha-/Transparenz-Pipeline.
- Noch kein echter Per-HWND-Surface-Objektbaum fuer alle OOP-Apps; v75 beweist
  den Pfad mit SurfaceLab und dem generischen Parent-Mapper.
- GDI-IPC existiert weiter als Overlay/Kompatibilitaetsschicht.
