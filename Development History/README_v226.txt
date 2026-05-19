myOS v226 - Message filter-stage dispatch

- Added private USER32 message filter stages for the pre-dispatch pump:
  HOOK, ACCELERATOR, DIALOG, MODELLESS, TRANSLATE, MENU, DISPATCH.
- Public MSG remains MSDN-shaped; MyMessage carries private filter_stages,
  filter_state and current filter_stage sidecar metadata.
- Queue selectors can now filter by pretranslate/filter-stage mask in addition
  to HWND, message range, lane and input kind.
- Modal/menu/dialog pump path consumes the filter-stage state through a central
  pretranslate runner instead of repeating TranslateAccelerator/IsDialogMessage/
  TranslateMessage ordering in every caller.
- Added user32 smokes and benchmark for filter-stage classification and queue
  stage selection.
