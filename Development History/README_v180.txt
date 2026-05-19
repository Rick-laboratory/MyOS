myOS v180 - child-HWND batch CreateWindowExA for OOP DialogLab
BUILD: myos_v180_child_hwnd_batch_create

What changed:
- Adds MYOS_IPC_OP_CREATE_CHILD_WINDOW_BATCH / BATCH_ACK.
- Expands the OOP child-control lane from 8 to 32 controls.
- Stores per-control style/ex-style in the shared child-HWND request arrays.
- Parent creates a full batch of child HWND controls in one process-context entry and acknowledges once.
- OOP DialogLab now creates its 13 command buttons via one batch request.
- Modal/modeless OOP dialog probes now create STATIC + OK/Cancel/Close controls via one batch request.

Goal:
Remove the visible sequential one-button-after-another construction path while keeping the controls as real USER32 child HWNDs.
