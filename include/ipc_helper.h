/*
 * ipc_helper.h - Helper-side IPC API for the VMODEM control device.
 *
 * Helper / Win32 application only. Do NOT include in VxD code.
 */
#ifndef VMODEM_IPC_HELPER_H
#define VMODEM_IPC_HELPER_H

#include <windows.h>
#include "ipc_shared.h"

/*
 * IPC_Open - open the VMODEM control device (\\.\VMODEM.VXD).
 * Dynamically loads the VxD if not already loaded.
 * Returns 0 on success; Windows error code on failure.
 * On success, *phDevice is a valid handle; caller must call IPC_Close.
 */
int IPC_Open(HANDLE *phDevice);

/*
 * IPC_Close - close a handle obtained from IPC_Open.
 */
void IPC_Close(HANDLE hDevice);

/*
 * IPC_ClaimHelper - claim the active helper role and retrieve protocol limits.
 * pAckOut receives the HELLO/claim response on success.
 * Returns non-zero on success; zero on DeviceIoControl failure.
 */
int IPC_ClaimHelper(HANDLE hDevice,
                    unsigned long version,
                    VMODEM_HELLO_ACK *pAckOut);

/*
 * IPC_SendHello - compatibility helper for callers that only need the status
 * field from the HELLO/claim exchange.
 */
int IPC_SendHello(HANDLE hDevice,
                  unsigned long version,
                  unsigned long *pStatusOut);

/*
 * IPC_QueryDriver - query the loaded VxD's build/runtime state.
 * pInfoOut receives the VxD response on success.
 * Returns non-zero on success; zero on DeviceIoControl failure.
 */
int IPC_QueryDriver(HANDLE hDevice,
                    unsigned long version,
                    VMODEM_QUERY_DRIVER_ACK *pInfoOut);

/*
 * IPC_SubmitMessage - submit one structured helper->VxD protocol message.
 * pAckOut receives the VxD acknowledgement on success.
 * Returns non-zero on success; zero on DeviceIoControl failure.
 */
int IPC_SubmitMessage(HANDLE hDevice,
                      const VMODEM_PROTOCOL_MESSAGE *pMessage,
                      VMODEM_SUBMIT_MESSAGE_ACK *pAckOut);

/*
 * IPC_ReceiveMessage - poll for the next VxD->helper protocol message for the
 * claimed helper generation.
 * pMessageOut receives either a queued message or a VMODEM_STATUS_NO_MESSAGE
 * result on success.
 * Returns non-zero on success; zero on DeviceIoControl failure.
 */
int IPC_ReceiveMessage(HANDLE hDevice,
                       unsigned long version,
                       unsigned long helper_generation,
                       VMODEM_PROTOCOL_MESSAGE *pMessageOut);

/*
 * IPC_GetHookLog - retrieve IFS hook state and the PortOpen name log.
 * pInfoOut receives the VxD response on success.
 * Returns non-zero on success; zero on DeviceIoControl failure.
 */
int IPC_GetHookLog(HANDLE hDevice,
                   unsigned long version,
                   VMODEM_GET_HOOK_LOG_ACK *pInfoOut);

/*
 * IPC_GetTraceLog - drain the pending driver-side trace spool used for
 * helper session diagnostics.
 * pInfoOut receives the VxD response on success.
 * Returns non-zero on success; zero on DeviceIoControl failure.
 */
int IPC_GetTraceLog(HANDLE hDevice,
                    unsigned long version,
                    VMODEM_GET_TRACE_LOG_ACK *pInfoOut);

/*
 * IPC_HookCaptureControl - start/stop/reset the isolated hook-capture window.
 * pInfoOut receives the VxD response on success.
 * Returns non-zero on success; zero on DeviceIoControl failure.
 */
int IPC_HookCaptureControl(HANDLE hDevice,
                           unsigned long version,
                           unsigned long action,
                           VMODEM_HOOK_CAPTURE_CONTROL_ACK *pInfoOut);

#endif /* VMODEM_IPC_HELPER_H */
