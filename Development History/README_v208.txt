myOS v208 - Standard Handles + Handle Benchmark Smoke
=====================================================

Goal
----
Continue the post-v205 handle-table line with the Win32 standard-handle surface:
GetStdHandle/SetStdHandle, STARTF_USESTDHANDLES, and child-process standard
handle materialization. Also add a tiny smoke-time handle benchmark so we have a
rough feel for sparse handle-table allocation/close behavior after the 24-bit
slot work.

Implemented
-----------
- Added SDK constants:
  - STD_INPUT_HANDLE
  - STD_OUTPUT_HANDLE
  - STD_ERROR_HANDLE
  - STARTF_USESTDHANDLES
- Extended STARTUPINFOA with:
  - hStdInput
  - hStdOutput
  - hStdError
- Added KERNEL32-style APIs:
  - GetStdHandle
  - SetStdHandle
- Added per-process standard-handle slots to Process-Lite state:
  - stdInput
  - stdOutput
  - stdError
- GetStartupInfoA now reports stored standard handles.
- CreateProcessA now honors STARTF_USESTDHANDLES by materializing parent handle-table
  handles into the child process table and storing the child-local handle values.
- MyProcessLiteInfo/MyRuntimeContextInfo expose std_input/std_output/std_error for
  smoke/diagnostics.
- Added strict_handles smoke coverage for:
  - GetStdHandle/SetStdHandle stdin/stdout/stderr
  - invalid std-handle selector LastError
  - CreateProcessA + STARTF_USESTDHANDLES child materialization
- Added v208 handle benchmark INFO line in strict_handles:
  - duplicate 4096 handles
  - close 4096 handles
  - print dup_ms/close_ms/ops/s/count_after

Notes
-----
This benchmark is deliberately informational. It is not a conformance rule and
must not shape the architecture. It only gives us a coarse regression signal for
sparse table growth and close cost.

Validation
----------
make clean && make -j$(nproc)
./myos_input --smoke strict_handles
./myos_input --smoke all

Observed in this build:
- strict_handles: 71 PASS, 0 FAIL, 0 WARN
- all: PASS, no failures
