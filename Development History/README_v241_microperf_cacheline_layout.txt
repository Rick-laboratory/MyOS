myOS / Linux-Win32 v241 — Microperf + Cacheline Layout Pass
============================================================

Focus
-----
This pass follows the v240 O(1)/dispatch/cache-quality work with a narrower
micro-architecture sprint.  The goal is to keep the v240 structural wins
(free stacks, hashes, sibling links, live counters) while taking pressure off
hot lookup/dispatch paths that regressed because larger structures were no
longer explicitly laid out for cacheline-friendly access.

What changed
------------

1. Power-of-two hash buckets
   - Converted remaining hot hash tables from prime-size bucket counts to
     power-of-two counts and masks.
   - Applies to HWND thread queues, ProcessHost PID lookup, IPC process lookup,
     kernel named objects, per-process handle-table PID lookup, GDI handle maps,
     SCM service names, USER32 class maps and USER32 lite object handles.
   - This removes DIV/modulo from those bucket selections.

2. Cheaper hot-path hashes
   - Simplified PID/IPC/GDI/lite-handle hash helpers to one-multiply style
     mixing plus a mask where appropriate.
   - The goal is not cryptographic distribution; it is fast, stable table
     dispatch for small/dense runtime IDs and handles.

3. Branch-friendly lookup loops
   - Hot bucket walks now bias the successful/invariant path with MYOS_LIKELY /
     MYOS_UNLIKELY.
   - Valid/range guards are kept for robustness, but they no longer dominate the
     expected branch shape in the hot path.

4. Cacheline-aligned hot headers
   - HWNDEntry is cacheline-aligned and its resolve/dispatch header is packed
     into the first cacheline; the heavier capability/window-state payload is
     explicitly cold.
   - MyWinWindowInfo is cacheline-aligned and reordered so HWND, parent/owner,
     PID/TID, style/exStyle, class atom, WndProc, z-order and destruction flags
     sit in the first cacheline.
   - v240 sibling links and control/dialog/MDI/text state are intentionally cold.
   - Static asserts guard the layout so future fields cannot silently push hot
     metadata into later cachelines.

5. USER32 child traversal fast paths
   - GetTopWindow(), GetWindow(..., GW_CHILD) and FindWindowExA() now use the
     v240 intrusive child list directly for real parent HWNDs.
   - The fallback table scan remains for corrupted/legacy edge cases and for
     top-level enumeration where the old global order semantics still matter.

6. Handle-table hot-path cleanup
   - Per-process handle tables preallocate a larger free-stack sidecar to avoid
     early realloc spikes.
   - Allocating a new handle no longer invalidates the handle-resolution TLS
     cache, because existing handle entries are not mutated by allocation.
   - Close/reuse/removal paths still invalidate as before.

7. Benchmark visibility
   - Added/kept the v241 child sibling-list smoke benchmark so the structural
     v240/v241 win is visible: repeated GetTopWindow() + FindWindowExA() probes
     against a medium child tree.

Validation
----------

The final package was validated with:

    make clean && make -j2
    ./myos_input --smoke all

Expected final result:

    SMOKE RESULT: PASS (0 failures)

Notes
-----

This pass intentionally avoids more invasive work such as per-CPU handle-table
sharding, full dispatcher-object wait lists, a global NT-style object directory,
or moving every USER32 control payload out-of-line.  Those are still the next
large steps toward truly 100% O(1)/cacheline/compatibility quality.
