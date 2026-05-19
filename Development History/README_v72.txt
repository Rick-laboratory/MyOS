myOS v72 - OOP StateBus Dirty-Notify + Shared State Section
============================================================

BUILD: myos_v72_oop_statebus_dirty_notify
IPC shared version: 72

What changed
------------
1. New OOP StateBusLab:
   - statebus-lab / statebus launch as myos_apphost_child GUI apps.
   - Right-click desktop menu item "StateBusLab" launches a publisher and a subscriber pair.
   - Each instance is a real Linux child process with its own myOS PID, HWND and handle table.

2. Dirty-notify architecture demo:
   - Shared Section carries the payload/current state.
   - Event carries a wait-style signal.
   - PostMessage carries only a dirty notification.

   This is the intended WindowState/DWM-style pattern:
      Shared Section = current state / payload
      Event          = wait/signal lane
      Message        = dirty notification only

3. New named objects used by StateBusLab:
   Section:
     Global\myos.v72.statebus.section
   Event:
     Global\myos.v72.statebus.event

4. StateBus payload uses a small seqlock-style layout:
   - seqBegin odd while writer is active
   - seqBegin == seqEnd and even means reader has a stable snapshot
   - Publisher writes latest state into the Section
   - Subscriber reads the latest state from its own mmap

5. Coalesced dirty notification:
   - Publisher sets notifyPending when posting STATEBUS_NOTIFY.
   - Further publishes while notifyPending is set are coalesced.
   - Subscriber clears notifyPending when it receives the dirty message and reads the latest Section version.
   - Spam100 demonstrates "100 writes, ideally few queue messages, latest state wins".

Test recipe
-----------
Right-click desktop -> StateBusLab.
This opens two OOP children:
  - StateBus Publisher [OOP v72]
  - StateBus Subscriber [OOP v72]

Typical test:
  Subscriber: Sub / Map
  Publisher:  Pub / Publish
  Subscriber should receive STATEBUS_NOTIFY and read the shared payload.

Stress-ish test:
  Subscriber: Sub / Map
  Publisher:  Pub / Spam100
  Subscriber should read the latest shared version; intermediate messages may be coalesced.

Manual event test:
  Publisher: Publish
  Subscriber: Wait0
  Subscriber reads the shared payload after the event signal.

Why this matters
----------------
v71 proved OOP Sections/FileMappings: payload can live in shared memory.
v72 proves the next layer: cross-process apps can treat the queue as a signal lane,
not as a payload transport. That is the architecture needed for scalable WindowState,
DWM-style surfaces and app-to-app state observation.

Known limits
------------
- StateBus is a lab protocol, not yet the global WindowState section exported to every child.
- Shared payload updates are seqlock-style but not yet backed by kernel atomics/futex/eventfd.
- Crash-time owner cleanup for the named section/event is still the existing Object Manager behavior.
- The classic in-process SharedBusLab code still exists in the tree but the desktop menu now opens the OOP StateBusLab pair.
