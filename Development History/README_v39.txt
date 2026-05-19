myOS v39 - Service Control Manager Lite + ServiceLab

New:
- SCM-lite service database
- OpenSCManagerA
- CreateServiceA
- OpenServiceA
- StartServiceA
- ControlService(STOP)
- QueryServiceStatus
- DeleteService mark-for-delete
- CloseServiceHandle
- SERVICE objects registered in ObjectLab as \\ServicesActive\\...
- ServiceLab app in Start menu, optional F17 hotkey

Test:
1. Start ServiceLab.
2. Open SCM.
3. Create.
4. Start.
5. Query.
6. Open ObjectLab and Refresh; SERVICE rows should be visible.
7. Stop, Delete, Close.
