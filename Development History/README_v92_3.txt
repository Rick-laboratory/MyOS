myOS v92.3 - evdev mouse wheel input fix

Fixes the missing wheel event source before USER32 routing:
- both supplied /dev/input/event* fds are now read as generic evdev devices
- EV_KEY < BTN_MOUSE becomes keyboard input
- EV_KEY >= BTN_MOUSE becomes mouse-button input
- EV_REL REL_X/Y remains cursor movement
- EV_REL REL_WHEEL emits MSG_MOUSE_WHEEL
- EV_REL REL_WHEEL_HI_RES is accumulated in 120-unit detents and emits MSG_MOUSE_WHEEL
- horizontal wheel is recognized and debug-logged but not yet routed as WM_MOUSEHWHEEL

Debug test:
  sudo MYOS_EVDEV_DEBUG=1 ./myos_input /dev/input/event1 /dev/input/event2

When the wheel is read correctly, the TTY should print lines like:
  [EVDEV] fd=... REL_WHEEL value=-1 -> MSG_MOUSE_WHEEL steps=-1

If nothing prints, the selected event devices are wrong. Use:
  grep -H . /sys/class/input/event*/device/name
  sudo cat /dev/input/eventX | od -tx1

Then run myOS with the event node that changes when the wheel is moved.
