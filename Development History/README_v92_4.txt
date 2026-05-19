myOS v92.4 - evdev multi-device mouse wheel fix

Problem: VMware/evdev can expose mouse movement/buttons and wheel on separate /dev/input/event* devices.
v92.3 read only two devices, so REL_WHEEL on /dev/input/event3 never reached USER32.

Fix:
- myos_input now accepts any number of evdev paths:
  sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3
- input_layer_init_many() reads all devices generically in one select() loop.
- EV_REL REL_WHEEL and REL_WHEEL_HI_RES are converted to MSG_MOUSE_WHEEL from any device.
- MYOS_EVDEV_DEBUG=1 prints wheel translation.

Test:
  make clean && make
  sudo chvt 3
  sudo MYOS_EVDEV_DEBUG=1 ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

Expected debug:
  [EVDEV] fd=... REL_WHEEL value=-1 -> MSG_MOUSE_WHEEL steps=-1

Then open DialogLab -> Open Controls Dialog and wheel over LISTBOX/COMBOBOX/SCROLLBAR.
