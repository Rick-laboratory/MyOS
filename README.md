# MyOS
**MyOS** is a Win32/MSDN-compatible userspace built on top of a stock Linux kernel. Think of it as a clean-room implementation of the Windows application model, windows, message loop, kernel objects, handles, services, GDI, written from scratch in portable C, with Linux providing the underlying process, memory and I/O primitives.

An ultra-optimized, production-driven core OS System implementing native Win32/NT semantics directly on top of the Linux kernel

# Architectural Highlights & Core Philosophy

Modern operating systems have decoupled the application layer from the hardware with layers of asynchronous over-abstraction. MyOS returns to raw power, and Ease of developmen due to Global and Local Hooks exactly like on Windows.

* **Unified Handle Architecture (`HANDLE`):** Complete deterministic lifecycle tracking for files, processes, threads, and synchronization primitives.
* **Lock-Free Thread-Local Storage (TLS) Free-Batches:** Multi-lane recycling architectures that eliminate global table contention during high-frequency parent/child handle churn.
* **Deterministic Synchronization:** Zero-overhead context switching utilizing a centralized `DispatcherHeader` engine for predictable kernel waits.
* **Extreme Resource Efficiency:** Native C-based message loops running at sub-millisecond execution speeds with virtually non-measurable idle memory footprints.

## Performance & Evolution Matrix

The engine undergoes continuous hotpath optimization validated by rigorous smoke-testing layers.

| Version | Release Strategy | Handle Churn Reuse | Cross-Process IPC Efficiency
| :--- | :--- | :--- | :--- | :--- |
| **v249** | Basic Cache Tracking | Thread-Isolated Hint | Standard Fallback Inter-Process 
| **v250** | Generation Masking | Generational Slot Recycle | Lock-Contended Duplication 
| **v255** | Single-Table Free-Batch | TLS One-Table Batch | Flushed Local Cache on Switch 
| **v256** | **Multi-Table Free-Batch** | **TLS Multi-Table Lanes** | **Zero-Flush Parent/Child Churn** 
