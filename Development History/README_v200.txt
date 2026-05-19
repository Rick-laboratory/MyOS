BUILD: myos_v200_self_relative_sd_accesscheck

Goal:
- Move the v199 object DACL model outward into the Win32 SECURITY_DESCRIPTOR API surface.
- Smokes were updated to validate the architecture; code was not bent to old smoke assumptions.

Implemented:
- IsValidSecurityDescriptor / GetSecurityDescriptorLength.
- GetSecurityDescriptorControl.
- Set/GetSecurityDescriptorOwner.
- Set/GetSecurityDescriptorGroup.
- GetSecurityDescriptorDacl.
- MakeSelfRelativeSD / MakeAbsoluteSD roundtrip support.
- GENERIC_MAPPING + MapGenericMask.
- Public AccessCheck first stage.
- GetKernelObjectSecurity / SetKernelObjectSecurity minimal object-DACL integration.
- Object DACL ACE masks are mapped through object type generic mappings before Object Manager storage.

Semantics:
- Absolute SECURITY_DESCRIPTORs remain accepted by CreateEvent/CreateMutex/CreateSemaphore/CreateWaitableTimer/CreateFileMapping.
- Self-relative SECURITY_DESCRIPTORs are accepted by parser/getters and are returned by GetKernelObjectSecurity.
- DENY ACEs still precede ALLOW ACEs.
- NULL DACL remains intentionally open; empty DACL blocks non-owner access except owner implicit READ_CONTROL/WRITE_DAC.

Validation:
- ./myos_input --smoke security
- ./myos_input --smoke all
