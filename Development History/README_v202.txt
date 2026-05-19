BUILD: myos_v202_token_privileges_accesscheck

Goal:
- Deepen v201 token handles into a privilege-bearing token model and make AccessCheck closer to the SRM shape.

Implemented:
- CheckTokenMembership public API.
- PrivilegeCheck public API.
- GetTokenInformation extensions:
  - TokenOwner
  - TokenPrimaryGroup
  - TokenDefaultDacl
  - TokenPrivileges now returns enabled admin privileges instead of an empty placeholder.
- Admin/CAP_ADMIN tokens expose stable myOS LUIDs for:
  - SeSecurityPrivilege
  - SeTakeOwnershipPrivilege
  - SeBackupPrivilege
  - SeRestorePrivilege
- AccessCheck now handles:
  - ACCESS_SYSTEM_SECURITY via SeSecurityPrivilege
  - MAXIMUM_ALLOWED as a computed maximum access result
  - PrivilegeSet used-access reporting for SeSecurityPrivilege
- Inherit-only ACEs are ignored by DACL evaluation.

Validation:
- security smoke adds TokenOwner/TokenDefaultDacl, CheckTokenMembership, PrivilegeCheck, ACCESS_SYSTEM_SECURITY and MAXIMUM_ALLOWED probes.
