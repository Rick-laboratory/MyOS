myOS v17.2 - WSTS hardening

Neu:
- Shared WindowState Section mit seqlock-style seqBegin/seqEnd Snapshots
- Spy++ zeigt WSTS-Slot, State-Version, Destroyed-Flag und LastMessage
- Subscriptions werden für POS/ACT/SHOW/TEXT/DESTROY als Queue-Signal genutzt
- WM_DESTROY bleibt als Tombstone in der WSTS sichtbar, damit Subscriber Close/Destroy sehen
- Title changes feuern WM_WINDOWTEXTCHANGED
- Minimize/Restore feuert WM_SHOWWINDOW
- Focus changes feuern WM_ACTIVATE

Prinzip bleibt:
Queue = Signal, WSTS = Zustand.
