myOS v233.1 - warning cleanup

Small hygiene patch on top of v233_selector_plan_op_table.

Fixed build warnings:
- Removed unused mywin_handle_cache_store() wrapper in winbase.c. The active cache store path uses mywin_handle_cache_store_ex(), preserving the object-slot-aware cache path.
- Replaced FORTIFY-tripped snprintf("%s") title copy in mywin_publish_local_hwnd_state() with bounded truncating copy.
- Replaced MDI Window-menu snprintf("&%d %s") with prefix formatting plus bounded truncating append, preserving menu text truncation semantics without compile-time truncation warnings.

Validation:
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all
