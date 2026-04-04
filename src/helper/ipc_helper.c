/*
 * ipc_helper.c - Helper-side IPC for the VMODEM control device.
 *
 * Opens \\.\VMODEM.VXD via CreateFile and communicates via DeviceIoControl.
 *
 * Targets Windows 98. Plain C89. No CRT.
 */

#include <windows.h>
#include "ipc_shared.h"
#include "ipc_helper.h"

#define VMODEM_DEVICE_PATH  "\\\\.\\VMODEM.VXD"

int IPC_Open(HANDLE *phDevice)
{
    HANDLE h;

    if (phDevice == NULL) {
        return ERROR_INVALID_PARAMETER;
    }

    *phDevice = INVALID_HANDLE_VALUE;

    h = CreateFileA(VMODEM_DEVICE_PATH,
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL);

    if (h == INVALID_HANDLE_VALUE) {
        return (int)GetLastError();
    }

    *phDevice = h;
    return 0;
}

void IPC_Close(HANDLE hDevice)
{
    if (hDevice != NULL && hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(hDevice);
    }
}

int IPC_ClaimHelper(HANDLE hDevice,
                    unsigned long version,
                    VMODEM_HELLO_ACK *pAckOut)
{
    VMODEM_HELLO     hello;
    VMODEM_HELLO_ACK ack;
    DWORD            bytesReturned;
    BOOL             ok;

    if (pAckOut == NULL) {
        return 0;
    }

    hello.version  = version;
    hello.reserved = 0;

    ZeroMemory(&ack, sizeof(ack));
    ack.status = (unsigned long)0xFFFFFFFFUL;

    bytesReturned = 0;
    ok = DeviceIoControl(hDevice,
                         (DWORD)VMODEM_IOCTL_HELLO,
                         &hello, (DWORD)sizeof(hello),
                         &ack,   (DWORD)sizeof(ack),
                         &bytesReturned,
                         NULL);

    if (!ok) {
        return 0;
    }

    *pAckOut = ack;
    return 1;
}

int IPC_SendHello(HANDLE hDevice,
                  unsigned long version,
                  unsigned long *pStatusOut)
{
    VMODEM_HELLO_ACK ack;
    int              ok;

    if (pStatusOut == NULL) {
        return 0;
    }

    ok = IPC_ClaimHelper(hDevice, version, &ack);
    if (!ok) {
        *pStatusOut = (unsigned long)GetLastError();
        return 0;
    }

    *pStatusOut = ack.status;
    return 1;
}

int IPC_QueryDriver(HANDLE hDevice,
                    unsigned long version,
                    VMODEM_QUERY_DRIVER_ACK *pInfoOut)
{
    VMODEM_QUERY_DRIVER     query;
    VMODEM_QUERY_DRIVER_ACK ack;
    DWORD                   bytesReturned;
    BOOL                    ok;

    if (pInfoOut == NULL) {
        return 0;
    }

    query.version  = version;
    query.reserved = 0;

    ZeroMemory(&ack, sizeof(ack));
    ack.status = (unsigned long)0xFFFFFFFFUL;

    bytesReturned = 0;
    ok = DeviceIoControl(hDevice,
                         (DWORD)VMODEM_IOCTL_QUERY_DRIVER,
                         &query, (DWORD)sizeof(query),
                         &ack,   (DWORD)sizeof(ack),
                         &bytesReturned,
                         NULL);

    if (!ok) {
        return 0;
    }

    *pInfoOut = ack;
    return 1;
}

int IPC_SubmitMessage(HANDLE hDevice,
                      const VMODEM_PROTOCOL_MESSAGE *pMessage,
                      VMODEM_SUBMIT_MESSAGE_ACK *pAckOut)
{
    VMODEM_PROTOCOL_MESSAGE submit;
    VMODEM_SUBMIT_MESSAGE_ACK ack;
    DWORD bytesReturned;
    BOOL  ok;

    if (pMessage == NULL || pAckOut == NULL) {
        return 0;
    }

    submit = *pMessage;
    submit.version = VMODEM_IPC_VERSION;

    ZeroMemory(&ack, sizeof(ack));
    ack.status = (unsigned long)0xFFFFFFFFUL;

    bytesReturned = 0;
    ok = DeviceIoControl(hDevice,
                         (DWORD)VMODEM_IOCTL_SUBMIT_MESSAGE,
                         &submit, (DWORD)sizeof(submit),
                         &ack,    (DWORD)sizeof(ack),
                         &bytesReturned,
                         NULL);

    if (!ok) {
        return 0;
    }

    *pAckOut = ack;
    return 1;
}

int IPC_ReceiveMessage(HANDLE hDevice,
                       unsigned long version,
                       unsigned long helper_generation,
                       VMODEM_PROTOCOL_MESSAGE *pMessageOut)
{
    VMODEM_RECEIVE_MESSAGE request;
    VMODEM_PROTOCOL_MESSAGE message;
    DWORD bytesReturned;
    BOOL  ok;

    if (pMessageOut == NULL) {
        return 0;
    }

    request.version = version;
    request.helper_generation = helper_generation;
    request.reserved0 = 0;
    request.reserved1 = 0;

    ZeroMemory(&message, sizeof(message));
    message.status = (unsigned long)0xFFFFFFFFUL;

    bytesReturned = 0;
    ok = DeviceIoControl(hDevice,
                         (DWORD)VMODEM_IOCTL_RECEIVE_MESSAGE,
                         &request, (DWORD)sizeof(request),
                         &message, (DWORD)sizeof(message),
                         &bytesReturned,
                         NULL);

    if (!ok) {
        return 0;
    }

    *pMessageOut = message;
    return 1;
}

int IPC_GetHookLog(HANDLE hDevice,
                   unsigned long version,
                   VMODEM_GET_HOOK_LOG_ACK *pInfoOut)
{
    VMODEM_GET_HOOK_LOG     query;
    VMODEM_GET_HOOK_LOG_ACK ack;
    DWORD                   bytesReturned;
    BOOL                    ok;

    if (pInfoOut == NULL) {
        return 0;
    }

    query.version  = version;
    query.reserved = 0;

    ZeroMemory(&ack, sizeof(ack));
    ack.status = (unsigned long)0xFFFFFFFFUL;

    bytesReturned = 0;
    ok = DeviceIoControl(hDevice,
                         (DWORD)VMODEM_IOCTL_GET_HOOK_LOG,
                         &query, (DWORD)sizeof(query),
                         &ack,   (DWORD)sizeof(ack),
                         &bytesReturned,
                         NULL);

    if (!ok) {
        return 0;
    }

    *pInfoOut = ack;
    return 1;
}

int IPC_GetTraceLog(HANDLE hDevice,
                    unsigned long version,
                    VMODEM_GET_TRACE_LOG_ACK *pInfoOut)
{
    VMODEM_GET_TRACE_LOG     query;
    VMODEM_GET_TRACE_LOG_ACK ack;
    DWORD                    bytesReturned;
    BOOL                     ok;

    if (pInfoOut == NULL) {
        return 0;
    }

    query.version = version;
    query.reserved = 0;

    ZeroMemory(&ack, sizeof(ack));
    ack.status = (unsigned long)0xFFFFFFFFUL;

    bytesReturned = 0;
    ok = DeviceIoControl(hDevice,
                         (DWORD)VMODEM_IOCTL_GET_TRACE_LOG,
                         &query, (DWORD)sizeof(query),
                         &ack,   (DWORD)sizeof(ack),
                         &bytesReturned,
                         NULL);

    if (!ok) {
        return 0;
    }

    *pInfoOut = ack;
    return 1;
}

int IPC_HookCaptureControl(HANDLE hDevice,
                           unsigned long version,
                           unsigned long action,
                           VMODEM_HOOK_CAPTURE_CONTROL_ACK *pInfoOut)
{
    VMODEM_HOOK_CAPTURE_CONTROL     control;
    VMODEM_HOOK_CAPTURE_CONTROL_ACK ack;
    DWORD                           bytesReturned;
    BOOL                            ok;

    if (pInfoOut == NULL) {
        return 0;
    }

    control.version   = version;
    control.action    = action;
    control.reserved0 = 0;
    control.reserved1 = 0;

    ZeroMemory(&ack, sizeof(ack));
    ack.status = (unsigned long)0xFFFFFFFFUL;

    bytesReturned = 0;
    ok = DeviceIoControl(hDevice,
                         (DWORD)VMODEM_IOCTL_HOOK_CAPTURE_CONTROL,
                         &control, (DWORD)sizeof(control),
                         &ack,     (DWORD)sizeof(ack),
                         &bytesReturned,
                         NULL);

    if (!ok) {
        return 0;
    }

    *pInfoOut = ack;
    return 1;
}
