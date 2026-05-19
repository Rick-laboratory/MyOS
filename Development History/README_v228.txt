myOS v228 - USER message filter stage operation table

- v227 filter pipeline stages are now backed by a USER32 stage operation table.
- Hook/accelerator/dialog/modeless/translate/menu/dispatch each resolve to a compact op record before handler execution.
- Modal/dialog/menu pumps no longer depend on a switch ladder for stage execution; stage -> op -> handler is data-driven.
- Added MyWinQueryMessageFilterStage private diagnostic helper for smoke coverage.
- Public MSG and Win32 APIs remain unchanged.
