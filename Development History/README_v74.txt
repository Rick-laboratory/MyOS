myOS v74 - HWND State Dirty Notify Live
======================================

Goal
----
v73 proved that an out-of-process child can map the global HWND WindowState
Section and read stable HWND entries. v74 turns that into a live observer model:

    WindowManager writes current HWND state into the shared WSTS Section
    MessageQueue sends only WM_MYOS_HWND_STATE_DIRTY
    StateProbe re-reads the latest payload from shared memory

What changed
------------
* New explicit message IDs:
    WM_MYOS_HWND_STATE_DIRTY
    WM_MYOS_HWND_STATE_SUBSCRIBE_REQ
    WM_MYOS_HWND_STATE_UNSUBSCRIBE_REQ

* HWND subscriptions now support source == 0 as a privileged global WSTS
  dirty subscription. Specific subscriptions still behave as before.

* The OOP HWND StateProbe has two new buttons:
    Subscribe
    Unsub

* StateProbe now shows live diagnostics:
    mapped=YES/NO
    sub=YES/NO
    dirty=<count>
    src=<source hwnd>
    serial=<dirty serial>
    viewSeq=<section updateSerial>
    maps/closes/refresh/paints/auto

* StateProbe has CAP_WINDOW_SUBSCRIBE in the app registry.

Test procedure
--------------
Start as usual:

    sudo chvt 3
    sudo ./myos_input /dev/input/event1 /dev/input/event2

1. Open the probe
   Right click desktop -> HWND StateProbe

2. Initial expected state
   The probe should show:

      mapped=NO
      sub=NO
      dirty=0

3. Click Map
   Expected:

      mapped=YES
      table shows Terminal/Calc/StateProbe/etc.

4. Click Subscribe
   Expected after a moment:

      sub=YES
      lastAction=WM_MYOS_SUBSCRIBED ack ... subscribed

5. Live dirty test
   Without clicking Refresh, do any of this:

      move Calc or Terminal
      focus another window
      open another app
      close another app

   Expected:

      dirty counter rises automatically
      src changes to the HWND that changed
      serial rises
      table updates without manual Refresh

6. Unsubscribe test
   Click Unsub.
   Expected:

      sub=NO

   Now move/focus/open windows again.
   Expected:

      dirty counter should stop rising automatically

7. Re-subscribe test
   Click Subscribe again.
   Expected:

      sub=YES again
      live dirty messages resume

8. CloseMap test
   Click CloseMap.
   Expected:

      mapped=NO
      table disappears

   Click Subscribe while unmapped.
   Expected:

      Subscribe maps once again and resumes live updates.

Known limits
------------
This is still a global-PoC subscription model, not a full Windows event hook
or DWM event system. The payload is intentionally not in the message. The
message only means: the shared WSTS section is dirty, re-read it.
