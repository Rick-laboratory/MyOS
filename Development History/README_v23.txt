myOS v23 - Section IPC Lab

Neu:
- SharedBusLab über Startmenü
- öffnet Producer + Consumer Fenster
- Producer schreibt Payload in eine named shared section
- Producer sendet nur BUSLAB_NOTIFY per Queue an Consumer
- Consumer liest die eigentliche Payload aus MapViewOfFile

Test:
1. Start -> SharedBusLab
2. Producer: Create Bus
3. Consumer: Map Bus
4. Producer: Write+Notify
5. Consumer zeigt notify/read/version/payload
6. Producer: Spam 100 zeigt Signal+Shared-Memory Verhalten
