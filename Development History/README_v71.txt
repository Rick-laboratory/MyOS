myOS v71 - OOP Section/FileMapping Bridge + Shared Payload Demo
================================================================

BUILD: myos_v71_oop_section_mapping_bridge
IPC shared version: 71

What changed
------------
1. Section/FileMapping calls can now cross the child -> parent Kernel Bridge:
   - CreateFileMappingA
   - OpenFileMappingA
   - MapViewOfFile
   - UnmapViewOfFile
   - FlushViewOfFile

2. Parent side now backs myOS Section objects with POSIX shared memory:
   - CreateFileMappingA creates a myOS Section object in the Object Manager.
   - The Section owns a POSIX shm backing name like /myos_sec_00006000_...
   - The parent still owns the real object, namespace, security and per-process handle table.

3. Child side MapViewOfFile no longer receives a fake parent pointer.
   Flow is now:
      Child HANDLE -> KREQ_MAP_VIEW_OF_FILE -> Parent resolves Section -> returns shm_name + bytes
      Child shm_open/mmap -> child-local pointer in its own Linux address space

4. SectionLab is now OOP by default:
   - section-lab / sectionlab launch as myos_apphost_child GUI apps.
   - section-lab-classic remains available for the old in-process lab.
   - F11 now launches SectionLab [OOP F11].

5. New visible SectionLab OOP buttons:
   - Create
   - Open
   - Map
   - Write
   - Read
   - Signal
   - Wait0
   - Unmap
   - Close

Test recipe
-----------
Open two SectionLabs.

Instance A:
  Create -> Map -> Write -> Signal

Instance B:
  Open -> Map -> Wait0 -> Read

Expected result:
  B reads the payload written by A from the same shared Section backing.
  The payload is not copied through the GUI queue; it is shared memory.
  The event is only the signal path.

Named objects used
------------------
Section:
  Global\myos.v71.shared.section

Event:
  Global\myos.v71.shared.event

Architecture note
-----------------
v71 implements the classic Win32 IPC trio:
  Section = payload
  Event   = signal
  Message = UI notification

Known limits
------------
- OOP MapViewOfFile supports offset 0 only in v71. Nonzero offsets need page-aligned mapping logic.
- Remote mapped views are reference-counted, but crash-time remote-view cleanup is still basic.
- This is still a Section/FileMapping bridge, not a full NT section object implementation with all protection/inheritance/file-backed behavior.
