BUILD: myos_v148_dispatcher_wait_condvar_v1

v148 replaces the polling/usleep KERNEL32 wait loop with a central dispatcher
condition variable. WaitForSingleObject and WaitForMultipleObjects now block on
pthread_cond_wait/pthread_cond_timedwait and wake when supported waitable object
state changes.

Implemented:
- central dispatcher mutex/condvar and sequence counter in winbase.c
- SetEvent/ResetEvent/ReleaseSemaphore/ReleaseMutex/SetWaitableTimer/CancelWaitableTimer wake the dispatcher
- Mutex/Semaphore/Timer objects now carry condition variables alongside Events
- WaitForMultipleObjects WAIT_ALL now uses dispatcher-locked scan+commit for the supported consumable objects
- WaitableTimer waits compute a timed condvar deadline from the nearest active timer
- Process/thread handle waits use bounded condvar timed re-polling because ProcessHost has no async kernel callback yet
- new smoke group: ./myos_input --smoke wait_real

Known limits:
- This is still a user-space dispatcher-lite, not an NT dispatcher object table.
- ProcessHost exit wakeups are not fully eventfd/futex-backed yet; they are condvar-timed reaped.
- WAIT_ABANDONED and recursive mutex ownership semantics remain future work.
