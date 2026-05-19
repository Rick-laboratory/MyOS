myOS / Linux-Win32 v242 — Hotpath TLS + Queue Selector Pass
=============================================================

Focus
-----
This pass continues v241's micro-architecture work, but avoids a broad risky
rewrite.  It targets two hot areas that still showed measurable cost:

1. KERNEL32 public HANDLE churn still paid a global handle-table lookup on very
   frequent DuplicateHandle/CloseHandle paths.
2. USER32 queue selectors still rescanned QS indexes and paid extra predicate
   dispatch work even after index sources had already narrowed the candidate set.

What changed
------------

1. Per-thread handle-table lookup cache
   - mywin_get_handle_table_ref() now has a one-entry TLS cache for the current
     PID's handle table.
   - Hot same-process DuplicateHandle/CloseHandle/Wait paths no longer take the
     global g_HandleLock just to rediscover the same table.
   - The cache remains conservative: tables are long-lived in this runtime, and
     the old global lookup path remains the fallback.

2. DuplicateHandle source fast path
   - Repeated same-process DuplicateHandle() calls now reuse the existing
     per-thread public-handle lookup cache for the source handle when
     DUPLICATE_CLOSE_SOURCE is not requested.
   - This avoids taking the source table pushlock on every duplicate of one
     stable source handle.
   - DUPLICATE_CLOSE_SOURCE still uses the locked source path so
     HANDLE_FLAG_PROTECT_FROM_CLOSE remains authoritative.

3. Queue selector QS maintenance cleanup
   - myqueue selector hot paths now trust the queue's incrementally-maintained
     current_qs field instead of recomputing it from every QS slot-index bitset
     for every select attempt.
   - Public GetQueueStatus-style APIs still keep their defensive refresh path.

4. Queue bitset micro-ops
   - 256-slot queue bitsets are now explicitly treated as four 64-bit words.
   - Tiny bitset copy/zero/and/or/any helpers are unrolled for this fixed shape.
   - The lane candidate loop no longer scans bitsets twice: it directly tries to
     take the first candidate and only records an empty skip when none exists.

5. Selector predicate dispatch cleanup
   - The selector plan is still data-driven, and smoke-visible op vectors remain
     stable.
   - Candidate validation now uses a compact switch over plan ops instead of an
     indirect function-pointer call per predicate.
   - QS predicates are skipped when the plan has already intersected the exact
     QS slot-index source.

6. USER32 class lookup TLS cache
   - Atom and exact class lookup now have conservative one-entry TLS caches.
   - Exact class cache hits still validate the hash, owner PID, hInstance,
     system/app namespace and class name to avoid hash-collision drift.

Validation
----------

The final package was validated with:

    make clean && make -j2
    ./myos_input --smoke all

Expected final result:

    SMOKE RESULT: PASS (0 failures)

Observed highlights from final all-smoke
----------------------------------------

Compared with v241 final all-smoke, the most relevant hot-path movements were:

- v230 QS queue-status prefilter:       ~26.6M -> ~44.5M ops/s
- v231 hot queue scan:                  ~26.6M -> ~53.9M ops/s
- v232 indexed queue slot-scan:         ~10.3M -> ~17.2M ops/s
- v233 selector plan/index:             ~10.6M -> ~18.5M ops/s
- v234 cached selector-plan:            ~10.3M -> ~17.4M ops/s
- v235 HWND/message bucket selector:     ~7.8M -> ~13.1M ops/s
- v208 all-smoke duplicate/close:        ~5.4M/7.6M -> ~6.5M/8.0M ops/s
- v209 all-smoke multi-thread churn:     ~0.91M -> ~1.22M ops/s

Notes
-----

A tested HWND-info TLS cache was deliberately not kept: the encoded HWND -> slot
path is already so small that adding a TLS check made the pure v219 header
microbenchmark noisier/slower in local runs.  v242 keeps the safer, more
consistent queue and HANDLE wins instead.

This pass still does not attempt the larger remaining work: full USER32 control
payload out-of-line storage, per-object wait lists, per-CPU handle shards,
NT-style object directories, or a real Session Manager / SCM process dispatcher.
