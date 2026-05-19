myOS v30 - Sync Objects

Added Windows-style waitable synchronization objects on top of the v29 process-local handle table:

- CreateMutexA / OpenMutexA / ReleaseMutex
- CreateSemaphoreA / OpenSemaphoreA / ReleaseSemaphore
- CreateWaitableTimerA / SetWaitableTimer / CancelWaitableTimer
- WaitForSingleObject and WaitForMultipleObjects now accept mixed waitable objects: EVENT, MUTEX, SEMAPHORE, TIMER.

WaitLab is now effectively SyncLab:
- Mutex creates an initially owned MUTEX object.
- RelMutex releases it.
- Semaphore creates count=1/max=3.
- RelSem increments it.
- Timer creates an auto-reset waitable timer and arms it for relative 250ms.
- WaitMixed waits over EVENT + SEMAPHORE + TIMER.

ObjectLab shows PROCESS, SECTION, EVENT, MUTEX, SEMAPHORE and TIMER rows plus the per-process handle table.
