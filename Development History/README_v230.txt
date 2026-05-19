myOS v230 - QS queue-status bitmask dispatch

- Adds real Win32-style QS_* queue-status tracking to the private message queue.
- MyMessageQueue now carries current_qs and changed_qs, matching the GetQueueStatus LOWORD/HIGHWORD model.
- Enqueue classifies queued work once from lane/input-kind/message into QS_KEY, QS_MOUSEMOVE, QS_MOUSEBUTTON, QS_TIMER, QS_PAINT, QS_SENDMESSAGE or QS_POSTMESSAGE.
- Queue selectors can now carry a QS mask and reject non-matching queues before walking lane/message entries.
- GetQueueStatus now returns current/changed QS bits instead of queue depth.
- MsgWaitForMultipleObjects readiness now consults QS status bits first, while retaining synthetic WM_TIMER fallback.
- New user32 smokes validate QS classification, GetQueueStatus-style current/changed behavior, QS selector filtering, and prefilter skip counters.
