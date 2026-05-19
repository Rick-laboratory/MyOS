BUILD: myos_v201_token_accesscheck_sd_roundtrip

Goal:
- Add the first real process token handle model on top of v200 SECURITY_DESCRIPTOR APIs.

Implemented:
- _OBJECT_TYPE_TOKEN Object Manager type.
- OpenProcessToken.
- GetTokenInformation for TokenUser, TokenGroups, TokenPrivileges baseline.
- TOKEN_USER / TOKEN_GROUPS / TOKEN_PRIVILEGES / SID_AND_ATTRIBUTES / PRIVILEGE_SET SDK definitions.
- TOKEN_QUERY/TOKEN_* access constants.
- Process token contains:
  - User SID: S-1-5-21-<pid>
  - Everyone group: S-1-1-0
  - Builtin Administrators group S-1-5-32-544 when CAP_ADMIN is present.
- AccessCheck consumes token handles and evaluates DACLs using SID/group membership.
- Token handles participate in CloseHandle and DuplicateHandle table semantics.

Important architecture rule:
- The smokes are compliance probes, not design drivers. v200/v201 updates the smoke surface to prove the MSDN-like architecture instead of constraining the architecture to legacy tests.

Validation:
- security smoke: self-relative SD, MakeAbsoluteSD, MapGenericMask, AccessCheck allow/deny, TokenUser/TokenGroups, Get/SetKernelObjectSecurity.
- all smoke remains green.
