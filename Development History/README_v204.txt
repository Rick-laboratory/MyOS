BUILD: myos_v204_process_thread_security_attributes

Goal
----
Move the v199-v203 SECURITY_DESCRIPTOR / DACL / Token work onto PROCESS and THREAD objects instead of keeping it only on named waitable/kernel objects.

Implemented
-----------
- CreateProcessA now consumes lpProcessAttributes and lpThreadAttributes as real SECURITY_ATTRIBUTES, not just bInheritHandle carriers.
- PROCESS and THREAD Object Manager entries receive stored SECURITY_DESCRIPTOR / DACL metadata.
- NULL process/thread SECURITY_ATTRIBUTES now get a token-default owner DACL instead of named-object public namespace defaults.
- OpenProcess now evaluates the target PROCESS object's DACL before granting a public handle.
- Added OpenThread and SDK/export coverage; OpenThread evaluates the target THREAD object's DACL before granting a public handle.
- PROCESS_ALL_ACCESS now includes PROCESS_QUERY_LIMITED_INFORMATION in this SDK surface so owner PROCESS_ALL_ACCESS ACEs really cover later limited-query opens.
- GetKernelObjectSecurity works on secured process handles and exports the self-relative descriptor path already added in v200.

Smoke
-----
make clean && make -j$(nproc)
./myos_input --smoke security
  checks=30 pass=30 fail=0 warn=0
./myos_input --smoke all
  checks=1163 pass=1163 fail=0 warn=0

Notes
-----
This does not yet turn the SCM/services layer into per-open SC_HANDLE objects, and it does not implement Windows impersonation / primary-token inheritance yet. Those are good follow-up targets after process/thread object security is grounded.
