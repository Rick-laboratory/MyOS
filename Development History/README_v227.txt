myOS v227 - USER message filter pipeline stage runner

v226 attached private USER32 pre-dispatch stage masks to queued messages.
v227 turns those masks into an explicit ordered filter pipeline so modal/menu/dialog
pumps can advance a message through HOOK -> ACCELERATOR -> DIALOG -> MODELESS ->
TRANSLATE -> MENU -> DISPATCH using stage results rather than a fresh ad-hoc if ladder.

Highlights:
- _MsgFilterPipeline and _MsgFilterStep added as internal USER queue state.
- mymsg_build_filter_pipeline() materializes the compact stage bitmask into an ordered stage vector.
- mymsg_advance_filter_stage() records PASSTHROUGH/HANDLED/BLOCKED and advances the active stage.
- mywin_pretranslate_filter_stages() now runs a pipeline context instead of directly checking each stage inline.
- Public MSG remains unchanged/MSDN-shaped; all new fields are private queue sidecar/state-machine metadata.

Validation:
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all
