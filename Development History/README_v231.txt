myOS v231 - Message Queue Hot/Cold Split
========================================

v231 keeps the public Win32/MSDN-shaped MSG/MyMessage contract intact, but changes
thread queue storage from a monolithic MyMessage ring into a cache-friendly hot
ring plus cold metadata sidecars.

Architecture changes:
- MyMessageQueue now stores _MessageHot hot[MYQUEUE_CAP] and _MessageCold cold[MYQUEUE_CAP].
- Queue selector scans only _MessageHot for hwnd/msg/lane/input/filter/QS state.
- _MessageCold keeps sender/target pid/tid, seq/timestamp, IPC payload metadata,
  sync SendMessage context, and auxiliary routing HWNDs.
- Dequeue/Peek reconstructs a full MyMessage from hot+cold, preserving public
  semantics and all existing callers.
- QS_* prefiltering remains in front of the hot scan from v230.

Why:
- MyMessage remains 136 bytes because it is the public/internal full transport
  record.
- _MessageHot is 56 bytes, so the queue scan touches a <=64-byte hot entry instead
  of a 136-byte all-in-one packet.
- At MYQUEUE_CAP=256, the hot ring is 14,336 bytes instead of the old 34,816-byte
  monolithic ring. Cold metadata exists, but it is not pulled through the common
  Peek/Get/MsgWait selector scan.

Validation:
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all
