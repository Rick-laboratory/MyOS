myOS v73 - Cross-process HWND shared WindowState sections
=========================================================

BUILD: myos_v73_cross_process_hwnd_state_sections

Goal
----
v73 turns the existing parent-local HWND WindowStateSection into a real named
FileMapping section that out-of-process GUI children can map themselves.

Pattern now exercised:

  WindowManager/HWND manager = writer of current state
  FileMapping Section        = current HWND payload / snapshot
  Message queue              = dirty/state-change signal only

New global section
------------------
Name:

  Global\myos.v73.hwnd.state.section

Layout:

  MyWindowStateSection
    magic='WSTS'
    version=73
    capacity=64
    activeCount/destroyedCount/updateSerial
    states[64]

Each MyWindowState entry is seqlock-style:

  seqBegin odd  = writer in progress
  seqBegin even = stable
  reader accepts only seqBegin == seqEnd && even

New per-HWND fields in the shared state:

  ownerPid / ownerTid
  rcWindow / rcClient
  visible / minimized / active / focused / enabled / hasCapture / destroyed
  flags / dirtyFlags
  zOrder
  style / exStyle
  stateVersion / updateSerial
  lastMessage
  title

New OOP app
-----------
Right-click desktop menu:

  HWND StateProbe

This launches:

  hwndstate-lab / hwndstate

The probe runs as a real myos_apphost_child process, opens the global section via
OpenFileMappingA, maps it via MapViewOfFile, and renders the HWND table from its
own mmap address space. It does not receive parent pointers.

Test
----
1. Run as usual:

   sudo chvt 3
   sudo ./myos_input /dev/input/event1 /dev/input/event2

2. Open right-click menu -> HWND StateProbe.
3. Open/move/focus/close other OOP windows such as Calc, Editor, WaitLab,
   SectionLab, StateBusLab.
4. In HWND StateProbe click Refresh or let its periodic refresh run.
5. Verify the probe shows changing hwnd/pid/rect/flags/seq/title rows.

Notes / current limits
----------------------
- v73 mirrors the central HWND state table into one named section. It does not
  yet create one separate FileMapping per HWND.
- Queue subscription/dirty messages are still the existing WM_WINDOWPOSCHANGED /
  WM_ACTIVATE / WM_SHOWWINDOW / WM_DESTROY paths. v73 proves the shared payload
  path; future builds can add a dedicated WM_MYOS_STATE_DIRTY message and
  read-only mapped views per observer.
- The OOP probe is read-only by convention, but the current Section bridge maps
  the POSIX shm with O_RDWR internally. Proper read-only view protection is a
  later hardening step.
