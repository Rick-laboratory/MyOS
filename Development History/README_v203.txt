BUILD: myos_v203_token_privileges_namespace_security

Goal:
- Move default named-object security out of legacy sd_flags and into explicit namespace-inherited SECURITY_DESCRIPTOR/DACL metadata.

Implemented:
- _ObjectAce stores ACE flags.
- Object Manager stores namespace_id alongside explicit SECURITY_DESCRIPTOR metadata.
- Named objects created without SECURITY_ATTRIBUTES now receive a real _ObjectSecurity DACL:
  - owner SID gets object-specific all access
  - public Global/Local namespace names inherit Everyone read/write semantics
  - .private. names inherit owner-only semantics
  - generated ACEs carry INHERITED_ACE
  - descriptor control carries SE_DACL_AUTO_INHERITED
- GetKernelObjectSecurity preserves ACE flags when exporting self-relative SDs.
- Legacy sd_flags remain only as fallback/synthesis for older/raw objects.

Architecture rule preserved:
- Smokes were expanded to validate the new architecture; the security model was not limited to the old smoke assumptions.

Validation:
- make clean && make -j$(nproc)
- ./myos_input --smoke security -> 27 PASS, 0 FAIL
- ./myos_input --smoke all -> 1160 PASS, 0 FAIL
