myos_v97_menus_submenus_ownerdraw_foundation

BUILD:
  make clean && make

RUN:
  sudo chvt 3
  sudo ./myos_input /dev/input/event1 /dev/input/event2 /dev/input/event3

v97 focus:
  - USER32 menu API width: Insert/Modify/Remove/Delete, GetSubMenu, GetMenuItemCount, GetMenuItemID
  - Menu state APIs: CheckMenuItem, EnableMenuItem, GetMenu, DrawMenuBar
  - Submenu support via MF_POPUP
  - Menu notifications: WM_INITMENUPOPUP, WM_MENUSELECT, WM_COMMAND
  - Owner-draw foundation structs/messages: MEASUREITEMSTRUCT, DRAWITEMSTRUCT, WM_MEASUREITEM, WM_DRAWITEM, ODT_MENU, ODA_*, ODS_*
  - DialogLab adds "Menu APIs Probe" to create a menu bar, submenus, checked/disabled/separator items, and an owner-draw popup test.

Known limitation:
  Generic app window menu-bar compositor drawing is still foundation-level. DialogLab draws a pseudo menu bar and validates USER32 state/messages. Full interactive menu bar rendering/navigation is the next natural hardening step.
