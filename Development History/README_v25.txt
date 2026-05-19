myOS v25 - Object Manager Lite

Neu:
- myobject.h / myobject.c als erster zentraler Object Manager Lite
- Object-Tabelle mit HANDLE, type, owner_pid, access_mask, ref_count, size, name
- SECTION-Handles werden bei CreateFileMappingA registriert
- MapViewOfFile/UnmapViewOfFile/CloseHandle spiegeln Refcounts in der Object Table
- HWNDs werden bei hwnd_create registriert und bei hwnd_destroy entfernt
- neue APIs:
  MyGetObjectCount()
  MyGetObjectCountByType(...)
  MyEnumObjects(...)
  MyGetObjectInfo(...)
  MyGetObjectTypeName(...)
- ObjectLab als neue Test-App
- F12 öffnet ObjectLab
- Startmenü enthält ObjectLab

Test:
1. F12 -> ObjectLab
2. Refresh zeigt aktuelle HWND/SECTION Object Table
3. Create Section erzeugt Local\\myos.objlab.scratch
4. SectionLab oder SharedBusLab starten und ObjectLab Refresh drücken
5. Close Section entfernt die Scratch-Section wieder aus der Object Table

Architekturziel:
HANDLE wird schrittweise von einem rohen Integer zu einem Object-Manager-Eintrag:
type + owner + access mask + refcount + name + object payload.
