myOS v20 UI THREAD LAB

Neu:
- PumpLab App (Startmenü oder F9)
- PumpLab besitzt eine eigene external-pumped UI-ThreadQueue
- hwnd_dispatch() ignoriert external-pumped Queues; PumpLab pumpt selbst
- Hung-Erkennung: in-dispatch > 750ms oder Queue-Stau > 750ms
- Spy++ hat eine neue H-Spalte fuer hung windows
- HUD zeigt HUNG=<count>

Test:
1. F7 -> Spy++
2. F9 -> PumpLab
3. PumpLab: Post Self, Stress 1000, Start Timer testen
4. PumpLab: Hang 2s druecken
5. Erwartung: Desktop bleibt bedienbar, Spy++ zeigt PumpLab temporaer mit H
