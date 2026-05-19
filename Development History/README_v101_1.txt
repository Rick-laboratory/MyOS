myOS v101.1 - DialogLab status cleanup

BUILD: myos_v101_1_dialoglab_status_cleanup

Small UI cleanup on top of v101 interactive menus:

- removed the stale DialogLab inline "Last text" status segment
- removed the old fake "MenuBar: File View OwnerDraw" in-client debug row
- kept the real compositor-owned HMENU menubar at the correct top chrome position
- shifted DialogLab status/debug rows upward slightly after removing the obsolete row

No menu API semantics were intentionally changed in this version.
