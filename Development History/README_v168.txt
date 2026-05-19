myOS v168 - OOP DialogLab modal/modeless bridge fix

BUILD: myos_v168_oop_dialog_modal_bridge_fix

Base: v167 OOP DialogLab/AppHost.

What changed:
- DialogLab OOP buttons no longer stop at telemetry-only pending/requested state.
- Open Modal Dialog creates a real parent-brokered OOP dialog HWND from the child process.
- Open Modeless Dialog and the probe/common-dialog buttons now create visible OOP dialog/probe windows.
- ProcessHost gained a tiny EnableWindow broker so the child can disable/re-enable its owner during modal loops.
- Secondary child CreateWindowExA is pumped after initial launch, so a GUI child can create more than one top-level HWND.
- Secondary proxy HWNDs and child-control creation run in the child process context, keeping USER32 ownership/mutation checks coherent.
- DestroyWindow requests from the child are brokered in the child process context, so modal OK/Cancel closes the dialog instead of leaving an immortal frame.
- OOP DialogLab smoke now asserts: modeless opens a HWND, modal opens a HWND, owner disables, OK re-enables owner and closes the dialog.

Known limitation:
The visible dialog surface still uses the current per-process GDI command buffer, not a per-HWND GDI stream. The important architectural bridge is now correct: HWND creation, owner disable/enable, modal loop result, and destruction flow across the process boundary.
