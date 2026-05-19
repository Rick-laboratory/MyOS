myOS v22 Sections + v21.1 Send Status

v21.1:
- DeadlockLab trennt OK / TIMEOUT / DENY-FAIL deutlicher.
- TIMEOUT-Status kann target=HUNG anzeigen.

v22:
- Neue WinAPI-nahe Mapping Calls:
  CreateFileMappingA
  MapViewOfFile
  UnmapViewOfFile
  FlushViewOfFile
  CloseHandle
- Neue Capability: CAP_SECTION_MAP
- Neue Test-App: SectionLab
  Startmenü -> SectionLab
  F11 -> SectionLab

SectionLab-Test:
1. Create Map
2. Write
3. Read
4. Unmap

Erwartung:
- viewA schreibt in eine shared section.
- viewB liest denselben Speicher ohne File-IO.
- sections/views Counter steigen und fallen passend.
