myOS v188 - msgwait/message queue waits
BUILD: myos_v188_msgwait_message_queue_waits

Adds USER32/KERNEL32 wait integration:
- WaitMessage() now uses MsgWaitForMultipleObjects-style queue wait.
- MsgWaitForMultipleObjects() and MsgWaitForMultipleObjectsEx() are exported.
- Current UI-thread message availability wakes as WAIT_OBJECT_0+nCount.
- Waitable handles still use the v187 WaitForMultipleObjects path.
- MsgWait observes queued/synthetic timer messages without consuming them.
