PAGE 58,132
TITLE VCOMM_GLUE - VCOMM service wrappers and PortFunctions table for VMODEM

;****************************************************************************
;
;   vcomm_glue.asm - ASM glue between VMODEM C code and VCOMM VxD services.
;
;   Provides:
;     1. C-callable wrappers for _VCOMM_Register_Port_Driver, _VCOMM_Add_Port,
;        _VCOMM_Get_Contention_Handler, _VCOMM_Map_Name_To_Resource, and
;        VCOMM_Get_Version
;     2. The PortFunctions dispatch table (22 entries + null terminator)
;     3. DriverControl and PortOpen thunks that forward to C implementations
;     4. A small VMM Get_System_Time wrapper used by the modem bridge
;
;   All C callbacks use cdecl (__cdecl) convention:
;     - underscore prefix, no @N suffix
;     - caller cleans the stack
;
;****************************************************************************

        .386p

        .xlist
        include VMM.INC
        include VCOMM.INC
        include IFSMGR.INC
        .list

;; External C functions (cdecl: _Name, no @N suffix)

;; DriverControl and PortOpen implementations in serial_port.c
extrn _VM_DriverControl:near
extrn _VM_PortOpen:near

;; The 22 PortFunctions callbacks in serial_port.c
extrn _VM_PortSetCommState:near
extrn _VM_PortGetCommState:near
extrn _VM_PortSetup:near
extrn _VM_PortTransmitChar:near
extrn _VM_PortClose:near
extrn _VM_PortGetQueueStatus:near
extrn _VM_PortClearError:near
extrn _VM_PortSetModemStatusShadow:near
extrn _VM_PortGetProperties:near
extrn _VM_PortEscapeFunction:near
extrn _VM_PortPurge:near
extrn _VM_PortSetEventMask:near
extrn _VM_PortGetEventMask:near
extrn _VM_PortWrite:near
extrn _VM_PortRead:near
extrn _VM_PortEnableNotification:near
extrn _VM_PortSetReadCallBack:near
extrn _VM_PortSetWriteCallBack:near
extrn _VM_PortGetModemStatus:near
extrn _VM_PortGetCommConfig:near
extrn _VM_PortSetCommConfig:near
extrn _VM_PortGetError:near

;****************************************************************************
;                       Locked Data
;****************************************************************************

VxD_LOCKED_DATA_SEG

        PUBLIC _VM_PortFunctions

        align 4

_VM_PortFunctions label DWORD
        dd      OFFSET32 _VM_PortSetCommState           ; 0
        dd      OFFSET32 _VM_PortGetCommState           ; 1
        dd      OFFSET32 _VM_PortSetup                  ; 2
        dd      OFFSET32 _VM_PortTransmitChar           ; 3
        dd      OFFSET32 _VM_PortClose                  ; 4
        dd      OFFSET32 _VM_PortGetQueueStatus         ; 5
        dd      OFFSET32 _VM_PortClearError             ; 6
        dd      OFFSET32 _VM_PortSetModemStatusShadow   ; 7
        dd      OFFSET32 _VM_PortGetProperties          ; 8
        dd      OFFSET32 _VM_PortEscapeFunction         ; 9
        dd      OFFSET32 _VM_PortPurge                  ; 10
        dd      OFFSET32 _VM_PortSetEventMask           ; 11
        dd      OFFSET32 _VM_PortGetEventMask           ; 12
        dd      OFFSET32 _VM_PortWrite                  ; 13
        dd      OFFSET32 _VM_PortRead                   ; 14
        dd      OFFSET32 _VM_PortEnableNotification     ; 15
        dd      OFFSET32 _VM_PortSetReadCallBack        ; 16
        dd      OFFSET32 _VM_PortSetWriteCallBack       ; 17
        dd      OFFSET32 _VM_PortGetModemStatus         ; 18
        dd      OFFSET32 _VM_PortGetCommConfig          ; 19
        dd      OFFSET32 _VM_PortSetCommConfig          ; 20
        dd      OFFSET32 _VM_PortGetError               ; 21
        dd      0                                       ; 22: pPortDeviceIOCtl (unused)

.errnz ($ - _VM_PortFunctions - SIZE _PortFunctions)

VxD_LOCKED_DATA_ENDS

;****************************************************************************
;                       Locked Code
;****************************************************************************

VxD_LOCKED_CODE_SEG

;******************************************************************************
;
; BOOL VM_VCOMM_Get_Version(void)
;
; Returns TRUE (1) if VCOMM is present, FALSE (0) otherwise.
; C-callable (cdecl).
;
;******************************************************************************
BeginProc VM_VCOMM_Get_Version, CCALL, PUBLIC

        VxDCall VCOMM_Get_Version
        jc      short @F
        mov     eax, 1
        ret
@@:
        xor     eax, eax
        ret

EndProc VM_VCOMM_Get_Version

;******************************************************************************
;
; BOOL VM_VCOMM_Register_Port_Driver(void *pDriverControl)
;
; Wraps VxDCall _VCOMM_Register_Port_Driver.
; C-callable (cdecl).
;
;******************************************************************************
BeginProc VM_VCOMM_Register_Port_Driver, CCALL, PUBLIC

ArgVar  pDriverControl, DWORD

        EnterProc
        VxDCall _VCOMM_Register_Port_Driver, <pDriverControl>
        LeaveProc
        return

EndProc VM_VCOMM_Register_Port_Driver

;******************************************************************************
;
; BOOL VM_VCOMM_Add_Port(DWORD DCRefData, void *pPortOpen, char *PortName)
;
; Wraps VxDCall _VCOMM_Add_Port.
; C-callable (cdecl).
;
;******************************************************************************
BeginProc VM_VCOMM_Add_Port, CCALL, PUBLIC

ArgVar  DCRefData, DWORD
ArgVar  pPortOpen, DWORD
ArgVar  pPortName, DWORD

        EnterProc
        VxDCall _VCOMM_Add_Port, <DCRefData, pPortOpen, pPortName>
        LeaveProc
        return

EndProc VM_VCOMM_Add_Port

;******************************************************************************
;
; PFN VM_VCOMM_Get_Contention_Handler(char *PortName)
;
; Wraps VxDCall _VCOMM_Get_Contention_Handler.
; C-callable (cdecl).
;
;******************************************************************************
BeginProc VM_VCOMM_Get_Contention_Handler, CCALL, PUBLIC

ArgVar  pPortName, DWORD

        EnterProc
        VxDCall _VCOMM_Get_Contention_Handler, <pPortName>
        LeaveProc
        return

EndProc VM_VCOMM_Get_Contention_Handler

;******************************************************************************
;
; DWORD VM_VCOMM_Map_Name_To_Resource(char *PortName)
;
; Wraps VxDCall _VCOMM_Map_Name_To_Resource.
; C-callable (cdecl).
;
;******************************************************************************
BeginProc VM_VCOMM_Map_Name_To_Resource, CCALL, PUBLIC

ArgVar  pPortName, DWORD

        EnterProc
        VxDCall _VCOMM_Map_Name_To_Resource, <pPortName>
        LeaveProc
        return

EndProc VM_VCOMM_Map_Name_To_Resource

;******************************************************************************
;
; void VM_VCOMM_Map_Ring0DCB_To_Win32(_DCB *pRing0, void *pWin32)
;
; VCOMM expects EAX -> ring0 DCB, EDX -> Win32-style DCB.
;
;******************************************************************************
BeginProc VM_VCOMM_Map_Ring0DCB_To_Win32, CCALL, PUBLIC

ArgVar  pRing0, DWORD
ArgVar  pWin32, DWORD

        EnterProc
        mov     eax, pRing0
        mov     edx, pWin32
        VxDCall VCOMM_Map_Ring0DCB_To_Win32
        LeaveProc
        return

EndProc VM_VCOMM_Map_Ring0DCB_To_Win32

;******************************************************************************
;
; void VM_VCOMM_Map_Win32DCB_To_Ring0(void *pWin32, _DCB *pRing0)
;
; VCOMM expects EAX -> Win32-style DCB, EDX -> ring0 DCB.
;
;******************************************************************************
BeginProc VM_VCOMM_Map_Win32DCB_To_Ring0, CCALL, PUBLIC

ArgVar  pWin32, DWORD
ArgVar  pRing0, DWORD

        EnterProc
        mov     eax, pWin32
        mov     edx, pRing0
        VxDCall VCOMM_Map_Win32DCB_To_Ring0
        LeaveProc
        return

EndProc VM_VCOMM_Map_Win32DCB_To_Ring0

;******************************************************************************
;
; DWORD VM_Get_System_Time(void)
;
; Wraps VMM Get_System_Time for cdecl C callers.
;
;******************************************************************************
BeginProc VM_Get_System_Time, CCALL, PUBLIC

        VMMCall Get_System_Time
        ret

EndProc VM_Get_System_Time

;******************************************************************************
;
; VM_DriverControl_Thunk
;
; Called by VCOMM after _VCOMM_Register_Port_Driver.
; Receives: (fCode, DevNode, DCRefData, AllocBase, AllocIrq, PortName)
; Forwards to _VM_DriverControl in serial_port.c with identical stack frame.
; Both are CCALL, so the stack layout is directly compatible.
;
;******************************************************************************
BeginProc VM_DriverControl_Thunk, CCALL, PUBLIC

ArgVar  fCode, DWORD
ArgVar  DevNode, DWORD
ArgVar  DCRefData, DWORD
ArgVar  AllocBase, DWORD
ArgVar  AllocIrq, DWORD
ArgVar  PortName, DWORD

        EnterProc

        push    PortName
        push    AllocIrq
        push    AllocBase
        push    DCRefData
        push    DevNode
        push    fCode
        call    _VM_DriverControl
        add     esp, 24

        LeaveProc
        return

EndProc VM_DriverControl_Thunk

;******************************************************************************
;
; VM_PortOpen_Thunk
;
; Called by VCOMM when an application opens the port.
; Receives: (PortName, VMId, lpError)
; Forwards to _VM_PortOpen in serial_port.c.
; Returns: PortHandle (pointer to PortData) or 0 on failure.
;
;******************************************************************************
BeginProc VM_PortOpen_Thunk, CCALL, PUBLIC

ArgVar  pnPortName, DWORD
ArgVar  VMId, DWORD
ArgVar  lpError, DWORD

        EnterProc

        push    lpError
        push    VMId
        push    pnPortName
        call    _VM_PortOpen
        add     esp, 12

        LeaveProc
        return

EndProc VM_PortOpen_Thunk

;******************************************************************************
;
; BOOL VM_IFSMgr_InstallFileSystemApiHook(PFN pNewHook)
;
; Wraps _IFSMgr_InstallFileSystemApiHook.
; eax = new hook proc address (register-based per IFSMGR convention).
; edx = 0 (no secondary output pointer; caller stores EAX return value).
; Returns old hook proc address in EAX (0 if no previous hook).
; C-callable (cdecl).
;
;******************************************************************************
;
; DWORD * VM_IFSMgr_InstallFileSystemApiHook(PFN pNewHook)
;
; Wraps IFSMgr_InstallFileSystemApiHook.
; DDK says: Entry TOS = hook proc address (C _cdecl stack convention).
;           Exit  EAX = ppIFSFileHookFunc (pointer to the chain slot).
; Pass pNewHook via the VxDCall Param list so the macro pushes it onto
; the stack before the INT; do NOT use EAX/EDX for this service.
; EAX (ppIFSFileHookFunc) is returned naturally.
; C-callable (cdecl).
;
;******************************************************************************
BeginProc VM_IFSMgr_InstallFileSystemApiHook, CCALL, PUBLIC

ArgVar  pNewHook, DWORD

        EnterProc
        VxDCall IFSMgr_InstallFileSystemApiHook, <pNewHook>
        LeaveProc
        return

EndProc VM_IFSMgr_InstallFileSystemApiHook

;******************************************************************************
;
; void VM_IFSMgr_RemoveFileSystemApiHook(PFN pHookProc)
;
; Wraps _IFSMgr_RemoveFileSystemApiHook.
; eax = hook proc address to remove (register-based per IFSMGR convention).
; C-callable (cdecl).
;
;******************************************************************************
BeginProc VM_IFSMgr_RemoveFileSystemApiHook, CCALL, PUBLIC

ArgVar  pHookProc, DWORD

        EnterProc
        mov     eax, pHookProc
        VxDCall IFSMgr_RemoveFileSystemApiHook
        LeaveProc
        return

EndProc VM_IFSMgr_RemoveFileSystemApiHook

VxD_LOCKED_CODE_ENDS

        END
