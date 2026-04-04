PAGE 58,132
TITLE VMODEM - ControlDispatch for 98Modem Win98 Virtual COM Port VxD

        .386p

        .xlist
        include vmm.inc
        include debug.inc
        .list

; Mark this as a dynamic VxD so it can be loaded/unloaded at runtime
; via CreateFile("\\.\VMODEM.VXD") without a static registry entry.
VMODEM_DYNAMIC EQU 1

DECLARE_VIRTUAL_DEVICE VMODEM, 1, 0, VMODEM_Control, \
                       UNDEFINED_DEVICE_ID, UNDEFINED_INIT_ORDER, ,

VxD_LOCKED_CODE_SEG

; C entry points (stdcall mangling: underscore prefix, @N byte-count suffix)
extrn _VMODEM_Dynamic_Init@0:near
extrn _VMODEM_Dynamic_Exit@0:near
extrn _VMODEM_Destroy_VM@4:near
extrn _VMODEM_Get_Contention_Handler@0:near
extrn _VMODEM_W32_DeviceIOControl@16:near

BeginProc VMODEM_Control
        ; Handle static load (non-dynamic case, if ever used)
        Control_Dispatch DEVICE_INIT,             VMODEM_Dynamic_Init,       sCall
        ; Dynamic load/unload
        Control_Dispatch SYS_DYNAMIC_DEVICE_INIT, VMODEM_Dynamic_Init,       sCall
        Control_Dispatch SYS_DYNAMIC_DEVICE_EXIT, VMODEM_Dynamic_Exit,       sCall
        Control_Dispatch Destroy_VM,             VMODEM_Destroy_VM,         sCall, <ebx>
        Control_Dispatch GET_CONTENTION_HANDLER,  VMODEM_Get_Contention_Handler, sCall
        ; Win32 DeviceIoControl: passes ecx=dwService, ebx=dwDDB, edx=hDevice, esi=lpDIOCParms
        Control_Dispatch W32_DEVICEIOCONTROL,     VMODEM_W32_DeviceIOControl, sCall, <ecx, ebx, edx, esi>
        clc
        ret
EndProc VMODEM_Control

VxD_LOCKED_CODE_ENDS

VxD_REAL_INIT_SEG

Load_Success_Msg DB 'VMODEM: Real Mode Init',13,10
Load_Msg_Len     EQU $ - Load_Success_Msg

BeginProc VMODEM_Real_Init
        mov     ah, 40h
        mov     bx, 1
        mov     cx, Load_Msg_Len
        mov     dx, OFFSET Load_Success_Msg
        int     21h

        xor     bx, bx
        xor     si, si
        xor     edx, edx
        mov     ax, Device_Load_Ok
        ret
EndProc VMODEM_Real_Init

VxD_REAL_INIT_ENDS

        END VMODEM_Real_Init
