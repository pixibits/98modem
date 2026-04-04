/*
 * ipc_vxd.c - VxD-side IPC: control device registration and IOCTL dispatch.
 *
 * Milestone 0: registers the \\.\VMODEM.VXD control device path (via the
 * VxD name in the DDB - no explicit call needed) and handles the HELLO
 * handshake IOCTL.
 *
 * Milestone 1 moves VCOMM registration into serial_port.c so the COM-port
 * surface and the control-device IPC path can evolve independently.
 *
 * Key Win98 DDK notes:
 *   - PDIOCPARAMETERS->lpcbBytesReturned is a POINTER to DWORD, not DWORD.
 *   - Buffer pointers in PDIOCPARAMETERS are flat 32-bit addresses; VxD code
 *     runs in ring 0 but shares the 4 GB flat space with Win32 processes, so
 *     they are directly dereferenceable without mapping.
 *   - DIOC_OPEN and DIOC_CLOSEHANDLE are handled in serial_vxd.c before
 *     reaching IPC_Dispatch.
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

DWORD VCOMM_QueryBuildId(void);
DWORD VCOMM_QueryDefaultModemStatus(void);
DWORD VCOMM_QueryCurrentModemStatus(void);
DWORD VCOMM_QueryFrontendOwner(void);
DWORD VCOMM_QueryPortOpen(void);
DWORD VCOMM_QueryDevNode(void);
DWORD VCOMM_QueryAllocBase(void);
DWORD VCOMM_QueryAllocIrq(void);
DWORD VCOMM_QueryContentionHandler(void);
DWORD VCOMM_QueryContentionResource(void);
DWORD VMODEM_QueryContentionHandlerRequests(void);
DWORD VCOMM_QueryHelperAttached(void);
DWORD VCOMM_QueryHelperGeneration(void);
DWORD VCOMM_QueryActiveSessionId(void);
DWORD VCOMM_QueryRawModeEnabled(void);
DWORD VCOMM_QueryS0AutoAnswerRings(void);
DWORD VCOMM_QueryS1RingCount(void);
DWORD VCOMM_QueryHelperQueueDepth(void);
DWORD VCOMM_QueryLastMsgToHelper(void);
DWORD VCOMM_QueryLastMsgFromHelper(void);
void VCOMM_QueryDosUartDiagnostic(VMODEM_DOS_UART_DIAGNOSTIC *diag);
DWORD VCOMM_DrainTraceLog(char *buffer, DWORD capacity, DWORD *pDroppedCount);
const char *VCOMM_QueryBuildTag(void);
const char *VCOMM_QueryPortName(void);
DWORD VCOMM_QueryHookInstalled(void);
DWORD VCOMM_QueryHookFireCount(void);
DWORD VCOMM_QueryPortOpenCount(void);
DWORD VCOMM_QueryHookLogLen(void);
const char *VCOMM_QueryHookLog(void);
DWORD VCOMM_QueryHookCaptureEnabled(void);
DWORD VCOMM_QueryHookCaptureGeneration(void);
DWORD VCOMM_QueryHookCaptureFireCount(void);
DWORD VCOMM_QueryHookCapturePortOpenCount(void);
DWORD VCOMM_QueryHookCaptureLogLen(void);
DWORD VCOMM_QueryHookCaptureOtherFnCount(void);
const char *VCOMM_QueryHookCaptureLog(void);
const DWORD *VCOMM_QueryHookCaptureFnCounts(void);
void VCOMM_ResetHookCapture(void);
void VCOMM_SetHookCaptureEnabled(BOOL enabled);
void VCOMM_HelperClaim(DWORD hDevice, VMODEM_HELLO_ACK *ack);
void VCOMM_HelperSubmitMessage(DWORD hDevice,
                               const VMODEM_PROTOCOL_MESSAGE *message,
                               VMODEM_SUBMIT_MESSAGE_ACK *ack);
void VCOMM_HelperReceiveMessage(DWORD hDevice,
                                const VMODEM_RECEIVE_MESSAGE *request,
                                VMODEM_PROTOCOL_MESSAGE *message);

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG

/* -------------------------------------------------------------------------
 * Internal helpers (no CRT; no memcpy/memset allowed in VxD context)
 * ------------------------------------------------------------------------- */

static void ipc_set_bytes_returned(PDIOCPARAMETERS p, DWORD value)
{
    if (p != 0 && p->lpcbBytesReturned != 0) {
        *(DWORD *)(p->lpcbBytesReturned) = value;
    }
}

static void ipc_copy_bytes(void *dst, const void *src, DWORD n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    DWORD i;
    if (dst == 0 || src == 0 || n == 0) {
        return;
    }
    for (i = 0; i < n; ++i) {
        d[i] = s[i];
    }
}

static void ipc_zero_bytes(void *dst, DWORD n)
{
    unsigned char *d = (unsigned char *)dst;
    DWORD i;

    if (dst == 0 || n == 0) {
        return;
    }

    for (i = 0; i < n; ++i) {
        d[i] = 0;
    }
}

/* -------------------------------------------------------------------------
 * IPC_RegisterDevice
 *
 * Called from VMODEM_Dynamic_Init.  The VxD device name "VMODEM" declared
 * in the DDB (vxd_ctrl.asm) is sufficient for CreateFile("\\.\VMODEM.VXD")
 * to work on Win98 without any additional registration call.
 *
 * Returns 1 on success, 0 on failure.
 * ------------------------------------------------------------------------- */
int IPC_RegisterDevice(void)
{
    VTRACE("VMODEM: IPC_RegisterDevice - control path via DDB name\r\n");
    return 1;
}

/* -------------------------------------------------------------------------
 * IPC_UnregisterDevice
 *
 * Called from VMODEM_Dynamic_Exit.  Mirror of IPC_RegisterDevice.
 * ------------------------------------------------------------------------- */
void IPC_UnregisterDevice(void)
{
    VTRACE("VMODEM: IPC_UnregisterDevice\r\n");
}

/* -------------------------------------------------------------------------
 * IPC_Dispatch
 *
 * Handles real IOCTLs (dwService is not DIOC_OPEN or DIOC_CLOSEHANDLE).
 * Returns NO_ERROR on success; a Win32 error code on failure.
 * ------------------------------------------------------------------------- */
DWORD IPC_Dispatch(DWORD hDevice, PDIOCPARAMETERS p)
{
    VMODEM_HELLO     hello;
    VMODEM_HELLO_ACK ack;
    VMODEM_QUERY_DRIVER     query;
    VMODEM_QUERY_DRIVER_ACK info;
    VMODEM_PROTOCOL_MESSAGE protocolMessage;
    VMODEM_SUBMIT_MESSAGE_ACK submitAck;
    VMODEM_RECEIVE_MESSAGE receiveMessage;
    VMODEM_GET_HOOK_LOG     hookQuery;
    VMODEM_GET_HOOK_LOG_ACK hookInfo;
    VMODEM_GET_TRACE_LOG     traceQuery;
    VMODEM_GET_TRACE_LOG_ACK traceInfo;
    VMODEM_HOOK_CAPTURE_CONTROL     captureControl;
    VMODEM_HOOK_CAPTURE_CONTROL_ACK captureAck;

    if (p == 0) {
        return ERROR_INVALID_PARAMETER;
    }

    ipc_set_bytes_returned(p, 0);

    switch ((DWORD)(p->dwIoControlCode)) {

    case VMODEM_IOCTL_HELLO:
        VTRACE("VMODEM: IPC_Dispatch HELLO\r\n");

        if (p->lpvInBuffer == 0 ||
            p->cbInBuffer < sizeof(VMODEM_HELLO)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }
        if (p->lpvOutBuffer == 0 ||
            p->cbOutBuffer < sizeof(VMODEM_HELLO_ACK)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }

        ipc_copy_bytes(&hello, (const void *)(p->lpvInBuffer),
                       sizeof(VMODEM_HELLO));

        /* Version check: accept only the current protocol version. */
        if (hello.version != VMODEM_IPC_VERSION) {
            VTRACE("VMODEM: HELLO version mismatch\r\n");
            return ERROR_REVISION_MISMATCH;
        }

        ipc_zero_bytes(&ack, sizeof(ack));
        VCOMM_HelperClaim(hDevice, &ack);
        ipc_copy_bytes((void *)(p->lpvOutBuffer), &ack,
                       sizeof(VMODEM_HELLO_ACK));
        ipc_set_bytes_returned(p, sizeof(VMODEM_HELLO_ACK));

        VTRACE("VMODEM: HELLO ACK sent\r\n");
        return NO_ERROR;

    case VMODEM_IOCTL_QUERY_DRIVER:
        VTRACE("VMODEM: IPC_Dispatch QUERY_DRIVER\r\n");

        if (p->lpvInBuffer == 0 ||
            p->cbInBuffer < sizeof(VMODEM_QUERY_DRIVER)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }
        if (p->lpvOutBuffer == 0 ||
            p->cbOutBuffer < sizeof(VMODEM_QUERY_DRIVER_ACK)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }

        ipc_copy_bytes(&query, (const void *)(p->lpvInBuffer),
                       sizeof(VMODEM_QUERY_DRIVER));

        if (query.version != VMODEM_IPC_VERSION) {
            VTRACE("VMODEM: QUERY_DRIVER version mismatch\r\n");
            return ERROR_REVISION_MISMATCH;
        }

        ipc_zero_bytes(&info, sizeof(info));
        info.status = VMODEM_STATUS_OK;
        info.build_id = VCOMM_QueryBuildId();
        info.modem_status = VCOMM_QueryCurrentModemStatus();
        info.default_status = VCOMM_QueryDefaultModemStatus();
        info.frontend_owner = VCOMM_QueryFrontendOwner();
        info.port_open = VCOMM_QueryPortOpen();
        info.dev_node = VCOMM_QueryDevNode();
        info.alloc_base = VCOMM_QueryAllocBase();
        info.alloc_irq = VCOMM_QueryAllocIrq();
        info.contention_handler = VCOMM_QueryContentionHandler();
        info.contention_resource = VCOMM_QueryContentionResource();
        info.contention_requests = VMODEM_QueryContentionHandlerRequests();
        info.helper_attached = VCOMM_QueryHelperAttached();
        info.helper_generation = VCOMM_QueryHelperGeneration();
        info.active_session_id = VCOMM_QueryActiveSessionId();
        info.raw_mode_enabled = VCOMM_QueryRawModeEnabled();
        info.s0_auto_answer_rings = VCOMM_QueryS0AutoAnswerRings();
        info.s1_ring_count = VCOMM_QueryS1RingCount();
        info.helper_queue_depth = VCOMM_QueryHelperQueueDepth();
        info.last_msg_to_helper = VCOMM_QueryLastMsgToHelper();
        info.last_msg_from_helper = VCOMM_QueryLastMsgFromHelper();
        ipc_copy_bytes(info.build_tag, VCOMM_QueryBuildTag(),
                       sizeof(info.build_tag));
        ipc_copy_bytes(info.port_name, VCOMM_QueryPortName(),
                       sizeof(info.port_name));
        VCOMM_QueryDosUartDiagnostic(&info.dos_uart);
        ipc_copy_bytes((void *)(p->lpvOutBuffer), &info,
                       sizeof(VMODEM_QUERY_DRIVER_ACK));
        ipc_set_bytes_returned(p, sizeof(VMODEM_QUERY_DRIVER_ACK));
        return NO_ERROR;

    case VMODEM_IOCTL_SUBMIT_MESSAGE:
        VTRACE("VMODEM: IPC_Dispatch SUBMIT_MESSAGE\r\n");

        if (p->lpvInBuffer == 0 ||
            p->cbInBuffer < sizeof(VMODEM_PROTOCOL_MESSAGE)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }
        if (p->lpvOutBuffer == 0 ||
            p->cbOutBuffer < sizeof(VMODEM_SUBMIT_MESSAGE_ACK)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }

        ipc_copy_bytes(&protocolMessage, (const void *)(p->lpvInBuffer),
                       sizeof(VMODEM_PROTOCOL_MESSAGE));

        if (protocolMessage.version != VMODEM_IPC_VERSION) {
            return ERROR_REVISION_MISMATCH;
        }
        if (protocolMessage.payload_length > VMODEM_IPC_MAX_PAYLOAD) {
            return ERROR_INVALID_PARAMETER;
        }

        ipc_zero_bytes(&submitAck, sizeof(submitAck));
        VCOMM_HelperSubmitMessage(hDevice, &protocolMessage, &submitAck);
        ipc_copy_bytes((void *)(p->lpvOutBuffer), &submitAck,
                       sizeof(VMODEM_SUBMIT_MESSAGE_ACK));
        ipc_set_bytes_returned(p, sizeof(VMODEM_SUBMIT_MESSAGE_ACK));
        return NO_ERROR;

    case VMODEM_IOCTL_RECEIVE_MESSAGE:
        VTRACE("VMODEM: IPC_Dispatch RECEIVE_MESSAGE\r\n");

        if (p->lpvInBuffer == 0 ||
            p->cbInBuffer < sizeof(VMODEM_RECEIVE_MESSAGE)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }
        if (p->lpvOutBuffer == 0 ||
            p->cbOutBuffer < sizeof(VMODEM_PROTOCOL_MESSAGE)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }

        ipc_copy_bytes(&receiveMessage, (const void *)(p->lpvInBuffer),
                       sizeof(VMODEM_RECEIVE_MESSAGE));

        if (receiveMessage.version != VMODEM_IPC_VERSION) {
            return ERROR_REVISION_MISMATCH;
        }

        ipc_zero_bytes(&protocolMessage, sizeof(protocolMessage));
        VCOMM_HelperReceiveMessage(hDevice, &receiveMessage, &protocolMessage);
        ipc_copy_bytes((void *)(p->lpvOutBuffer), &protocolMessage,
                       sizeof(VMODEM_PROTOCOL_MESSAGE));
        ipc_set_bytes_returned(p, sizeof(VMODEM_PROTOCOL_MESSAGE));
        return NO_ERROR;

    case VMODEM_IOCTL_GET_HOOK_LOG:
        VTRACE("VMODEM: IPC_Dispatch GET_HOOK_LOG\r\n");

        if (p->lpvInBuffer == 0 ||
            p->cbInBuffer < sizeof(VMODEM_GET_HOOK_LOG)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }
        if (p->lpvOutBuffer == 0 ||
            p->cbOutBuffer < sizeof(VMODEM_GET_HOOK_LOG_ACK)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }

        ipc_copy_bytes(&hookQuery, (const void *)(p->lpvInBuffer),
                       sizeof(VMODEM_GET_HOOK_LOG));

        if (hookQuery.version != VMODEM_IPC_VERSION) {
            return ERROR_REVISION_MISMATCH;
        }

        ipc_zero_bytes(&hookInfo, sizeof(hookInfo));
        hookInfo.status          = VMODEM_STATUS_OK;
        hookInfo.hook_installed  = VCOMM_QueryHookInstalled();
        hookInfo.hook_fire_count = VCOMM_QueryHookFireCount();
        hookInfo.port_open_count = VCOMM_QueryPortOpenCount();
        hookInfo.log_len         = VCOMM_QueryHookLogLen();
        if (hookInfo.log_len > sizeof(hookInfo.log_data)) {
            hookInfo.log_len = sizeof(hookInfo.log_data);
        }
        hookInfo.capture_enabled = VCOMM_QueryHookCaptureEnabled();
        hookInfo.capture_generation = VCOMM_QueryHookCaptureGeneration();
        hookInfo.capture_hook_fire_count = VCOMM_QueryHookCaptureFireCount();
        hookInfo.capture_port_open_count = VCOMM_QueryHookCapturePortOpenCount();
        hookInfo.capture_log_len = VCOMM_QueryHookCaptureLogLen();
        if (hookInfo.capture_log_len > sizeof(hookInfo.capture_log_data)) {
            hookInfo.capture_log_len = sizeof(hookInfo.capture_log_data);
        }
        hookInfo.capture_other_fn_count = VCOMM_QueryHookCaptureOtherFnCount();
        ipc_copy_bytes(hookInfo.capture_fn_counts,
                       VCOMM_QueryHookCaptureFnCounts(),
                       sizeof(hookInfo.capture_fn_counts));
        ipc_copy_bytes(hookInfo.log_data, VCOMM_QueryHookLog(),
                       hookInfo.log_len);
        ipc_copy_bytes(hookInfo.capture_log_data,
                       VCOMM_QueryHookCaptureLog(),
                       hookInfo.capture_log_len);
        ipc_copy_bytes((void *)(p->lpvOutBuffer), &hookInfo,
                       sizeof(VMODEM_GET_HOOK_LOG_ACK));
        ipc_set_bytes_returned(p, sizeof(VMODEM_GET_HOOK_LOG_ACK));
        return NO_ERROR;

    case VMODEM_IOCTL_GET_TRACE_LOG:
        VTRACE("VMODEM: IPC_Dispatch GET_TRACE_LOG\r\n");

        if (p->lpvInBuffer == 0 ||
            p->cbInBuffer < sizeof(VMODEM_GET_TRACE_LOG)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }
        if (p->lpvOutBuffer == 0 ||
            p->cbOutBuffer < sizeof(VMODEM_GET_TRACE_LOG_ACK)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }

        ipc_copy_bytes(&traceQuery, (const void *)(p->lpvInBuffer),
                       sizeof(VMODEM_GET_TRACE_LOG));

        if (traceQuery.version != VMODEM_IPC_VERSION) {
            return ERROR_REVISION_MISMATCH;
        }

        ipc_zero_bytes(&traceInfo, sizeof(traceInfo));
        traceInfo.status = VMODEM_STATUS_OK;
        traceInfo.log_len = VCOMM_DrainTraceLog(traceInfo.log_data,
                                                sizeof(traceInfo.log_data),
                                                &traceInfo.dropped_count);
        ipc_copy_bytes((void *)(p->lpvOutBuffer), &traceInfo,
                       sizeof(VMODEM_GET_TRACE_LOG_ACK));
        ipc_set_bytes_returned(p, sizeof(VMODEM_GET_TRACE_LOG_ACK));
        return NO_ERROR;

    case VMODEM_IOCTL_HOOK_CAPTURE_CONTROL:
        VTRACE("VMODEM: IPC_Dispatch HOOK_CAPTURE_CONTROL\r\n");

        if (p->lpvInBuffer == 0 ||
            p->cbInBuffer < sizeof(VMODEM_HOOK_CAPTURE_CONTROL)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }
        if (p->lpvOutBuffer == 0 ||
            p->cbOutBuffer < sizeof(VMODEM_HOOK_CAPTURE_CONTROL_ACK)) {
            return ERROR_INSUFFICIENT_BUFFER;
        }

        ipc_copy_bytes(&captureControl, (const void *)(p->lpvInBuffer),
                       sizeof(VMODEM_HOOK_CAPTURE_CONTROL));

        if (captureControl.version != VMODEM_IPC_VERSION) {
            return ERROR_REVISION_MISMATCH;
        }

        switch (captureControl.action) {
        case VMODEM_HOOK_CAPTURE_ACTION_RESET:
            VCOMM_ResetHookCapture();
            break;
        case VMODEM_HOOK_CAPTURE_ACTION_START:
            VCOMM_ResetHookCapture();
            VCOMM_SetHookCaptureEnabled(TRUE);
            break;
        case VMODEM_HOOK_CAPTURE_ACTION_STOP:
            VCOMM_SetHookCaptureEnabled(FALSE);
            break;
        default:
            return ERROR_INVALID_PARAMETER;
        }

        ipc_zero_bytes(&captureAck, sizeof(captureAck));
        captureAck.status = VMODEM_STATUS_OK;
        captureAck.capture_enabled = VCOMM_QueryHookCaptureEnabled();
        captureAck.capture_generation = VCOMM_QueryHookCaptureGeneration();
        ipc_copy_bytes((void *)(p->lpvOutBuffer), &captureAck,
                       sizeof(VMODEM_HOOK_CAPTURE_CONTROL_ACK));
        ipc_set_bytes_returned(p, sizeof(VMODEM_HOOK_CAPTURE_CONTROL_ACK));
        return NO_ERROR;

    default:
        VTRACE("VMODEM: IPC_Dispatch unknown IOCTL\r\n");
        return ERROR_NOT_SUPPORTED;
    }
}
