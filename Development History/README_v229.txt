myOS v229 - Compact USER message sidecars / Win32-style masks

Goal:
- Keep public MSG/MSDN ABI unchanged.
- Stop storing tiny internal queue-state enums as DWORD-sized fields.
- Keep true bitmask/action fields DWORD-sized and #define-based, matching Win32 style.

Changes:
- _MSG_LANE_*, _MSG_INPUT_*, _MSG_ROUTE_*, _MSG_FILTER_STATE_* are now compact byte-sized private state constants.
- _MSG_ROUTE_REASON_*, _MSG_FILTER_* remain DWORD bitmasks with #define values for cheap OR/AND composition.
- _MsgRouteDescriptor shrank from 56 bytes to 44 bytes on current myOS HANDLE/HWND layout.
- MyMessage shrank from 152 bytes to 136 bytes, saving 16 bytes per queued message and 4096 bytes per 256-entry queue.
- _MsgFilterPipeline shrank from 76 bytes to 28 bytes; _MsgFilterStep is now 2 bytes.
- No public Win32 names or MSG layout changed.

Validation:
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all
