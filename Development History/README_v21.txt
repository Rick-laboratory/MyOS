myOS v21 - SendMessageTimeout / DeadlockLab

New:
- DeadlockLab app (Start menu or F10)
- Cross-thread SendMessageTimeout to external-pumped queues
- Sync request/reply path through the target ThreadQueue
- Timeout detection without killing desktop responsiveness
- HUD SEND/TMO counters

Test:
1. F9 -> open PumpLab
2. F10 -> open DeadlockLab
3. DeadlockLab: Send 250ms -> expected OK
4. PumpLab: Hang 2s, then DeadlockLab: Send 250ms -> expected TIMEOUT
5. DeadlockLab: Hang+Send -> queues PumpLab hang, then sends; expected TIMEOUT while desktop remains responsive
6. Spy++ should still remain usable and show H for hung windows.
