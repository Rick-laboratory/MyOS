# myOS / Linux-Win32 v244 - USER32 root list + title/class hash + wait smoke finalization

This pass continues from v243_windowinfo_hotcold_control_payload and focuses on
remaining table-scan / cold-string lookup paths that were still visible after the
WindowInfo hot/cold split.

## Changes

- USER32 top-level windows now participate in the intrusive list model.
  - parent == NULL uses g_RootFirstWindow / g_RootLastWindow instead of falling
    back to a whole WindowInfo table scan in common GetTopWindow(0),
    GetWindow(...GW_CHILD), Enum-style collection and FindWindowA paths.
  - The existing child first/last/next/prev sibling fields are reused for root
    windows too, with unlink/reparent logic covering both normal children and
    parentless top-level windows.

- MyWinWindowInfo now carries hot class/title hashes.
  - classNameHash and textHash are kept in the first cacheline beside the other
    hot lookup metadata.
  - CreateWindowExA, WM_SETTEXT, edit text replacement and SetWindowTextA update
    the hashes whenever the cold className/text buffers are populated or changed.
  - FindWindowExA precomputes class/title filter hashes and rejects most misses
    before touching cold string buffers.

- LISTBOX/COMBOBOX item insert/delete shifts now use bulk memmove over the
  pointer-backed item/data/selection arrays.
  - This avoids per-item copy loops while preserving the v243 out-of-line
    control payload model.

- The prepared targeted WaitForSingleObject smoke benchmark is finalized.
  - The benchmark now uses the existing smoke microsecond timer consistently.
  - wait_real validates that targeted single-object waits are exercised and that
    the global dispatcher fallback is avoided for event/mutex/semaphore/timer
    waits in the smoke path.

## Validation

- make clean && make -j2
- ./myos_input --smoke all
- Final result: SMOKE RESULT: PASS (0 failures)

## Notes

The older 4096-probe microbenchmarks remain noisy at this point; rerunning the
unchanged v243 binary in the same container produced similar swings on v219/v221.
Use the new structural checks and the smoke pass/fail result as the hard signal,
and treat single-run ns/op numbers as rough hints only.
