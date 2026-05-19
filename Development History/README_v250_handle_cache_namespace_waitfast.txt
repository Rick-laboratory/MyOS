myOS / Linux-Win32 v250 - handle cache, named namespace, and WFMO fast paths
=============================================================================

This pass continues the v249 hotpath work with a deliberately small set of
measurable changes.

1. Multi-entry TLS public-handle cache
-------------------------------------
The old source-handle TLS cache effectively behaved as a one-entry cache.  That
is ideal for DuplicateHandle(source) churn but weak for WaitForMultipleObjects,
message pumps, and mixed USER/KERNEL code that repeatedly bounce between a small
set of stable HANDLE values.

v250 changes the TLS cache to a small direct-mapped table.  Cache validation is
still slot/generation/object-tuple based, so unrelated CloseHandle() mutations do
not make stale handles observable.

2. Handle free-slot hint and free-stack markbits
-----------------------------------------------
A same-thread CloseHandle -> DuplicateHandle/Create path often wants to reuse the
slot that was just closed.  v250 keeps a tiny TLS free-slot hint and adds per-leaf
free markbits so duplicate-free protection remains O(1).  Stale free-stack pops
are audited but no longer become the common path.

3. WFMO prevalidation/resolved target arrays
-------------------------------------------
WaitForMultipleObjects now validates and resolves all public handles once at the
start of the call, then carries resolved object handles and types through the
probe/commit loop.  This removes repeated public handle-table resolution from
multi-wait scans and keeps the small handle TLS cache warm.

4. Delayed WaitBlock linking for process/thread WFMO immediate cases
--------------------------------------------------------------------
v248 delayed WaitBlock registration only for native Event/Mutex/Semaphore/Timer
sets.  v250 extends this to all targeted sets, including Process/Thread handles.
Already-signaled process/thread waits can now return immediately without linking
WaitBlocks.

5. Central named kernel-object directory
----------------------------------------
The per-type name hashes remain as fast typed payload lookups, but v250 adds a
shared named-object directory for Section/Event/Mutex/Semaphore/Timer names.  It
catches cross-type collisions in one global namespace, e.g. an existing Event
name cannot be reused as a Mutex until the Event's final handle is closed.

Validation
----------
Final commands:

    make clean && make -j2
    ./myos_input --smoke all

Final smoke result:

    SMOKE RESULT: PASS (0 failures)

Relevant v250 smoke coverage includes:

    v250 named directory rejects cross-type collision
    v250 named directory releases name on last close
    v250 handle free-slot TLS hint reuses close->alloc slots
    v250 WFMO prevalidation and multi-entry handle TLS cache
    v250 targeted WFMO immediate process fast path skips WaitBlocks
