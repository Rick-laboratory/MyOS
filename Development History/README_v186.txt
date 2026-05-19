myOS v186 - Duplicate / Inherit Handle Audit
BUILD: myos_v186_duplicate_inherit_handle_audit

Focus:
- strict cross-process DuplicateHandle semantics
- DUPLICATE_CLOSE_SOURCE works across source process handle tables
- process-exit handle sweep preserves objects while other processes still own refs
- dead target process tables are rejected, no fallback/raten
- access masks are enforced for duplicate desired access

Smoke:
- parent->child duplicate keeps waitable alive after parent CloseHandle
- child exit releases the duplicated ref
- cross-process DUPLICATE_CLOSE_SOURCE closes source table entry
- SYNCHRONIZE-only source cannot duplicate EVENT_MODIFY_STATE
- DuplicateHandle rejects exited target process table
