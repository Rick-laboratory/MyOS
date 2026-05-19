# MyOS

A Win32/MSDN-compatible userspace built on top of a stock Linux kernel.

MyOS is a clean-room implementation of the Windows application model — windows,
message loop, kernel objects, handles, services, GDI — written from scratch in
portable C. Linux provides the underlying process, memory, and I/O primitives.
MyOS sits on top of it and presents a Win32 surface to applications.

## What this is

This is not a kernel. The Linux kernel does what it does best: scheduling,
memory management, I/O, hardware abstraction. MyOS provides what the desktop
side of Windows provides: the API surface that Win32 applications were written
against — `CreateWindowEx`, `GetMessage`, `CreateEvent`, `WaitForMultipleObjects`,
`CreateProcess`, GDI drawing, dialogs, menus, controls, services.

The goal is a desktop layer that performs the way it should, for both users
and the developers who write programs on and for it.

## What is implemented today

- **KERNEL32 partial** — handles, events, mutexes, semaphores, waitable timers,
  sections, processes, threads, services, named-object namespace
- **USER32 partial** — window classes, window creation, message queue with
  filter pipeline, hooks, dialogs, controls, MDI, capture, focus, hit-test
- **GDI partial** — DCs, brushes, pens, bitmaps, regions, basic drawing,
  text, BitBlt-style operations
- **NT-style internals** — object manager with slot+generation handles,
  dispatcher headers, per-object WaitBlock lists, central named-object
  directory, multi-process handle tables
- **Out-of-process app hosting** — applications run in separate processes,
  isolated through capability-checked IPC and shared-memory window state
- **17 demo applications** covering calculators, editors, dialogs, MDI,
  drag/drop, paint, services, IPC, deadlock detection, and inspection tools

The full Win32 API surface is large. MyOS implements roughly the subset that
mid-90s Windows applications used, plus core NT internals. Coverage is not
complete and is not advertised as such; see the development history for what
has been added in each step.

## Architecture

- **Object Manager** with type-coded slot handles (`Type | Slot | Generation`).
  Each kernel object family (Event, Mutex, Semaphore, Section, Timer, Process,
  Thread, Service) decodes its handle directly to an array slot — O(1) lookup
  without scanning type tables.
- **Dispatcher Header** shared by all native waitable objects, carrying signal
  state and a per-object wait list. This is structurally close to NT's
  `DISPATCHER_HEADER` and `KWAIT_BLOCK` model.
- **Targeted wait** — `WaitForSingleObject` and `WaitForMultipleObjects` attach
  WaitBlocks directly to the objects being waited on. A `SetEvent` wakes only
  the threads that are actually waiting on that event, not the world.
- **Per-thread caches** for the public-handle table, named-object directory,
  free-slot reuse, and multi-wait condvars. Validated through epoch and
  slot/generation tuples so unrelated mutations cannot leak stale entries.
- **Hot/cold split** in the message queue and window state, keeping the
  active scan path cache-line friendly while moving rarely-touched metadata
  to sidecar storage.
- **Documented lock order** in `LOCK_ORDER.md` — five hierarchy stages, with
  explicit rules about which locks may be held while acquiring which others.

## Testing

A built-in smoke suite (`./myos_input --smoke all`) runs a few thousand
checks across three main groups:

- `strict_handles` — handle lifecycle, object manager, named lookup,
  multi-process churn, multi-thread fanout
- `user32` — window creation/destroy, message queue, filter pipeline,
  hooks, dialogs, controls, lifecycle edge cases
- `wait_real` — synchronization primitives, single-wait, multi-wait,
  process/thread exit notification

Microbenchmarks are embedded in the smoke pass and print `ops_s` values per
hot path. They are useful for tracking regression between versions, but
single-run microbenchmarks are noisy at very low wall-times; treat them as
rough indicators, not absolute claims.

## Building

```
make clean && make -j$(nproc)
./myos_input --smoke all
```

Requires gcc/clang, glibc, pthreads. Tested on Ubuntu 24.04, x86_64.
RISC-V port is planned (DC-ROMA mini-PC, JH7110) as part of the
[DeepComputing 100 Open Source RISC-V Program](https://deepcomputing.io/program/100-open-source-projects-on-risc-v/).

## Project size

~67k lines of C across ~85 files (production code plus smoke suite plus
17 demo applications). Single-developer project, in active development.

## Development history

The `Development History/` directory contains per-version README files
documenting the architectural and performance changes in each step.

## License

See `LICENSE`.

---

*MyOS is built and maintained by [Rick Armbruster](https://github.com/Rick-laboratory).
Inquiries welcome.*
