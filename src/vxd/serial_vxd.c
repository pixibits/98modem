/*
 * serial_vxd.c - VMODEM VxD dynamic init, exit, and W32 DeviceIoControl
 *                entry points.
 *
 * These three functions are called by the control procedure in vxd_ctrl.asm
 * via the DDK Control_Dispatch / sCall mechanism.  They are the outermost
 * C layer of the driver.
 *
 * Milestone 1 adds VCOMM registration so Win98 can surface the virtual port
 * as a normal COM device while the existing control-device IPC path remains
 * available for helper communication.
 */

#define WANTVXDWRAPS

#include <basedef.h>
#include <vmm.h>
#include <vwin32.h>
#include <debug.h>
#include <vxdwraps.h>
#include <winerror.h>

#include "trace.h"
#include "ipc_shared.h"

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG

/* Forward declarations for ipc_vxd.c functions. */
int   IPC_RegisterDevice(void);
void  IPC_UnregisterDevice(void);
DWORD IPC_Dispatch(DWORD hDevice, PDIOCPARAMETERS p);
BOOL  VCOMM_Init(void);
void  VCOMM_Cleanup(void);
void  VCOMM_HandleVmDestroyed(DWORD vmId);
void  VCOMM_HelperHandleOpened(DWORD hDevice);
void  VCOMM_HelperHandleClosed(DWORD hDevice);
DWORD __cdecl VM_VxD_Contention_Handler(DWORD functionCode,
                                        DWORD arg1,
                                        DWORD arg2,
                                        DWORD arg3,
                                        DWORD arg4);

static DWORD g_ContentionHandlerRequests = 0;

/* -------------------------------------------------------------------------
 * VMODEM_Dynamic_Init
 *
 * Called on SYS_DYNAMIC_DEVICE_INIT (and DEVICE_INIT for static load).
 * Return VXD_SUCCESS to allow load; VXD_FAILURE to abort.
 * ------------------------------------------------------------------------- */
DWORD _stdcall VMODEM_Dynamic_Init(void)
{
    BOOL vcommOk;

    VTRACE("VMODEM: Dynamic Init\r\n");

    if (!IPC_RegisterDevice()) {
        VTRACE("VMODEM: IPC_RegisterDevice failed - aborting load\r\n");
        return VXD_FAILURE;
    }

    vcommOk = VCOMM_Init();
    if (!vcommOk) {
        VTRACE("VMODEM: VCOMM_Init failed - continuing with IPC only\r\n");
    }

    VTRACE("VMODEM: init complete\r\n");
    return VXD_SUCCESS;
}

/* -------------------------------------------------------------------------
 * VMODEM_Dynamic_Exit
 *
 * Called on SYS_DYNAMIC_DEVICE_EXIT.  Must always succeed.
 * ------------------------------------------------------------------------- */
DWORD _stdcall VMODEM_Dynamic_Exit(void)
{
    VTRACE("VMODEM: Dynamic Exit\r\n");
    VCOMM_Cleanup();
    IPC_UnregisterDevice();
    return VXD_SUCCESS;
}

DWORD _stdcall VMODEM_Get_Contention_Handler(void)
{
    VTRACE("VMODEM: Get Contention Handler\r\n");
    ++g_ContentionHandlerRequests;
    return (DWORD)(PFN)VM_VxD_Contention_Handler;
}

DWORD VMODEM_QueryContentionHandlerRequests(void)
{
    return g_ContentionHandlerRequests;
}

DWORD _stdcall VMODEM_Destroy_VM(DWORD vmId)
{
    VCOMM_HandleVmDestroyed(vmId);
    return VXD_SUCCESS;
}

/* -------------------------------------------------------------------------
 * VMODEM_W32_DeviceIOControl
 *
 * Called for every Win32 DeviceIoControl on the \\.\VMODEM.VXD handle.
 *
 * dwService   - the IOCTL code, or DIOC_OPEN / DIOC_CLOSEHANDLE
 * dwDDB       - pointer to our Device Descriptor Block (unused here)
 * hDevice     - the Win32 handle
 * lpDIOCParms - DIOC parameters for normal IOCTLs (NULL for open/close)
 * ------------------------------------------------------------------------- */
DWORD _stdcall VMODEM_W32_DeviceIOControl(DWORD          dwService,
                                           DWORD          dwDDB,
                                           DWORD          hDevice,
                                           PDIOCPARAMETERS lpDIOCParms)
{
    (void)dwDDB;

    if (dwService == DIOC_OPEN) {
        VCOMM_HelperHandleOpened(hDevice);
        return NO_ERROR;
    }

    if ((int)dwService == DIOC_CLOSEHANDLE) {
        VCOMM_HelperHandleClosed(hDevice);
        return NO_ERROR;
    }

    return IPC_Dispatch(hDevice, lpDIOCParms);
}
