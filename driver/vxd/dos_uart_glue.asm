PAGE 58,132
TITLE DOS_UART_GLUE - DOS UART trap and VMM helper shims for VMODEM

        .386p

        .xlist
        include VMM.INC
        include DEBUG.INC
        include VPICD.INC
        .list

extrn _DOSUART_OnIoTrap:near

VxD_LOCKED_CODE_SEG

BeginProc DOSUART_InstallIoHandler, CCALL, PUBLIC

ArgVar  port, DWORD
ArgVar  handler, DWORD

        EnterProc
        mov     edx, port
        mov     esi, handler
        VMMCall Install_IO_Handler
        jc      short dosuart_install_fail
        mov     eax, 1
        LeaveProc
        return

dosuart_install_fail:
        xor     eax, eax
        LeaveProc
        return

EndProc DOSUART_InstallIoHandler

BeginProc DOSUART_RemoveIoHandler, CCALL, PUBLIC

ArgVar  port, DWORD
ArgVar  handler, DWORD

        EnterProc
        mov     edx, port
        mov     esi, handler
        VMMCall Remove_IO_Handler
        jc      short dosuart_remove_fail
        mov     eax, 1
        LeaveProc
        return

dosuart_remove_fail:
        xor     eax, eax
        LeaveProc
        return

EndProc DOSUART_RemoveIoHandler

BeginProc DOSUART_EnableGlobalTrapping, CCALL, PUBLIC

ArgVar  port, DWORD

        EnterProc
        mov     edx, port
        VMMCall Enable_Global_Trapping
        LeaveProc
        return

EndProc DOSUART_EnableGlobalTrapping

BeginProc DOSUART_DisableGlobalTrapping, CCALL, PUBLIC

ArgVar  port, DWORD

        EnterProc
        mov     edx, port
        VMMCall Disable_Global_Trapping
        LeaveProc
        return

EndProc DOSUART_DisableGlobalTrapping

BeginProc DOSUART_SetIntRequest, CCALL, PUBLIC

ArgVar  hIrq, DWORD
ArgVar  vmHandle, DWORD

        EnterProc
        mov     eax, hIrq
        mov     ebx, vmHandle
        VxDCall VPICD_Set_Int_Request
        LeaveProc
        return

EndProc DOSUART_SetIntRequest

BeginProc DOSUART_ClearIntRequest, CCALL, PUBLIC

ArgVar  hIrq, DWORD

        EnterProc
        mov     eax, hIrq
        VxDCall VPICD_Clear_Int_Request
        LeaveProc
        return

EndProc DOSUART_ClearIntRequest

        PUBLIC  _DOSUART_IoTrapThunk
_DOSUART_IoTrapThunk PROC NEAR
        pushad
        mov     esi, esp
        movzx   eax, BYTE PTR [esi + Pushad_EAX]
        push    eax
        mov     eax, [esi + Pushad_ECX]
        push    eax
        mov     eax, [esi + Pushad_EDX]
        push    eax
        mov     eax, [esi + Pushad_EBX]
        push    eax
        call    _DOSUART_OnIoTrap
        add     esp, 16
        mov     [esi + Pushad_EAX], eax
        popad
        ret
_DOSUART_IoTrapThunk ENDP

        PUBLIC  _DOSUART_HwIrqThunk
_DOSUART_HwIrqThunk PROC NEAR
        VxDjmp  VPICD_Set_Int_Request
_DOSUART_HwIrqThunk ENDP

VxD_LOCKED_CODE_ENDS

        END
