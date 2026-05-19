BUILD: myos_v150_per_process_user32_class_table_v1

v150 moves USER32 window-class ownership from a single global name table to a
process/module aware class resolver while preserving global system classes.

Implemented
-----------
- MyWinClassEntry now records:
  - ownerPid
  - hInstance
  - systemClass
- USER32 class capacity raised from 64 to 128 entries.
- RegisterClassExA isolates app-defined classes by current process id and
  hInstance.
- Builtin/system classes remain global:
  - BUTTON
  - STATIC
  - EDIT
  - LISTBOX
  - SCROLLBAR
  - COMBOBOX
  - MDICLIENT
  - #32770
  - #32769
  - Shell_TrayWnd
- CreateWindowExA class resolution now follows:
  1. atom lookup
  2. caller-owned class for current process/hInstance
  3. caller-owned class for current process with NULL hInstance fallback
  4. system class lookup
- UnregisterClassA removes only caller-owned app classes.
- Apps cannot unregister system classes such as BUTTON.
- MyUser32CleanupProcessClasses(pid) removes app-owned classes at process exit.
- mywin_mark_process_exited() now invokes USER32 class cleanup.
- App/lab class registration helpers no longer cache a single process-global
  atom that would skip registration for a later capability/process context.

Why this matters
----------------
Before v150, two apps registering the same class name shared one global WNDPROC.
That was convenient for the in-process labs, but it is not acceptable for the
future PE/loader model: two unrelated processes can legally use the same class
name and must not steal each other's window procedure or UnregisterClass state.

Compatibility note
------------------
The in-tree demo apps run inside one Unix process today, so their old static
ATOM caches behaved like accidental global DLL state. v150 keeps the visible app
behavior, but every process/capability context now calls RegisterClassExA for its
own class namespace before CreateWindowExA.

New smoke group
---------------
./myos_input --smoke user32_class

Covered checks
--------------
- Two process contexts can register the same class name.
- Each process resolves the same class name to its own WNDPROC.
- Atom class lookup still works.
- UnregisterClassA removes only the caller-owned class.
- Foreign unregister does not remove another process' class.
- System classes are protected from app unregister.
- BUTTON remains usable after attempted unregister.
- Process class cleanup removes app-owned classes.

Tested
------
make clean && make -j2
./myos_input --smoke user32_class
./myos_input --smoke ownership
./myos_input --smoke shell_broker
./myos_input --smoke strict_handles
./myos_input --smoke wait_real
./myos_input --smoke user32_timer
./myos_input --smoke all

Full smoke result for v150:
- 673 PASS
- 0 FAIL
- 0 WARN
