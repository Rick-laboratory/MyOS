BUILD: myos_v235_hwnd_msg_bucket_indexes

v235 continues the selector-index architecture after v234's plan cache.

Implemented:
- Added bounded 64-bucket HWND slot index per thread queue.
- Added bounded 64-bucket message-number slot index per thread queue.
- Compiled selector plans now mark HWND/message bucket index sources in indexSourceMask.
- HWND and message-range predicate ops remain in the op table as exact collision guards.
- Exact/small message-range selects use the message bucket source; broad ranges fall back to the existing exact range predicate without wasting work on a full bucket union.

Validation:
- make clean && make -j$(nproc)
- ./myos_input --smoke user32
- ./myos_input --smoke strict_handles
- ./myos_input --smoke all

Results:
- Build: 0 warnings
- user32: 71 PASS, 0 FAIL, 0 WARN
- strict_handles: 85 PASS, 0 FAIL, 0 WARN
- all: 1259 PASS, 0 FAIL, 0 WARN
- SMOKE RESULT: PASS
