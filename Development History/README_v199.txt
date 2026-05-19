BUILD: myos_v199_security_descriptor_acl_model
Base: myos_v198_resource_template_handle_model

Goal
----
v199 starts the real Win32 SECURITY_DESCRIPTOR / DACL / ACE model for kernel
objects instead of extending the old sd_flags shortcut.  The old sd_flags path
is preserved as a compatibility/default-descriptor fallback for legacy labs and
unnamed/internal objects, but SECURITY_ATTRIBUTES.lpSecurityDescriptor now has
real meaning for newly-created waitable and section objects.

Implemented
-----------
- SDK winnt.h security primitives:
  - SID_IDENTIFIER_AUTHORITY, SID
  - ACL, ACE_HEADER
  - ACCESS_ALLOWED_ACE / ACCESS_DENIED_ACE
  - SECURITY_DESCRIPTOR and SE_DACL_PRESENT / SE_SELF_RELATIVE bits
  - GENERIC_READ / GENERIC_WRITE / GENERIC_EXECUTE / GENERIC_ALL
- SDK/API security helpers:
  - InitializeSecurityDescriptor
  - SetSecurityDescriptorDacl
  - InitializeAcl
  - AddAccessAllowedAce
  - AddAccessDeniedAce
  - IsValidSid / GetLengthSid / EqualSid / CopySid
  - InitializeSid / GetSidSubAuthority
- Object Manager stores explicit security descriptors in a compact internal form:
  - owner SID
  - DACL-present / NULL-DACL state
  - ordered allow/deny ACE list
  - diagnostic metadata exposed through _ObjectectInfo: sd_control, dacl_present,
    dacl_null, ace_count
- Access checks now use SID/token matching when an explicit descriptor exists:
  - current process SID: S-1-5-21-<pid>
  - Everyone group: S-1-1-0
  - Administrators group for CAP_ADMIN: S-1-5-32-544
  - DENY ACEs are evaluated before ALLOW ACEs
  - NULL DACL grants access
  - empty DACL denies normal access
  - owner receives implicit READ_CONTROL | WRITE_DAC only, not blanket all-access
  - CAP_ADMIN keeps the myOS root/admin bypass for now
- SECURITY_ATTRIBUTES.lpSecurityDescriptor is consumed by:
  - CreateEventA
  - CreateMutexA
  - CreateSemaphoreA
  - CreateWaitableTimerA
  - CreateFileMappingA
- Existing-object Create*/Open* paths perform DACL checks before allocating a new
  process handle.
- Existing granted-handle access masks still gate operations like SetEvent, so a
  SYNCHRONIZE-only handle cannot modify object state.

Smoke coverage
--------------
New mode: ./myos_input --smoke security

Checks added:
- SID primitives are valid for process SID and Everyone SID
- absolute SECURITY_DESCRIPTOR + ACCESS_ALLOWED_ACE builds correctly
- CreateEventA stores explicit DACL metadata in the Object Manager
- foreign process OpenEventA is denied without a matching ALLOW ACE
- granted handle access is still enforced after DACL open succeeds
- owner ACE grants EVENT_MODIFY_STATE
- empty DACL blocks access while owner retains WRITE_DAC
- NULL DACL grants access
- DENY ACE precedes ALLOW ACE

Validation
----------
make clean && make -j$(nproc)
./myos_input --smoke security
./myos_input --smoke all

Observed result in this build:
- security smoke: 9 PASS, 0 FAIL, 0 WARN
- all smoke: 1142 PASS, 0 FAIL, 0 WARN; SMOKE RESULT: PASS

Still future work
-----------------
- self-relative SECURITY_DESCRIPTOR parsing
- SetSecurityDescriptorOwner / Group / SACL APIs
- real token objects, impersonation tokens, privileges and AccessCheck API
- inheritance flags and inheritable ACE propagation
- audit/SACL evaluation
- default DACL generation from a real token default DACL
- service/process/thread object security attributes beyond the object-manager
  storage path
