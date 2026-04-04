/*
 * diag_main.c - Win98 console diagnostic for the VMODEM COM port.
 *
 * This probes both the VxD control device and the COM port surface using
 * the same Win32 serial APIs HyperTerminal is likely to touch during open.
 *
 * The goal is to report the first failing API and its GetLastError value
 * instead of just showing "Unable to open COM3".
 */

#include <windows.h>
#include <stdarg.h>

#include "ipc_shared.h"
#include "ipc_helper.h"

#define DIAG_BUF_LEN 1024
#define DIAG_CC_BUF_LEN 256
#define DIAG_HYPERTRM_MASK (EV_RLSD | EV_ERR)
#define DIAG_SHORT_WAIT_MS 250
#define DIAG_OVERLAPPED_BUF_LEN 80
#define DIAG_SERIALCOMM_KEY "HARDWARE\\DEVICEMAP\\SERIALCOMM"

#ifndef ERROR_IO_PENDING
#define ERROR_IO_PENDING 997L
#endif

#ifndef ERROR_IO_INCOMPLETE
#define ERROR_IO_INCOMPLETE 996L
#endif

static void diag_write_raw(const char *text)
{
    HANDLE hOut;
    DWORD written;
    DWORD length;

    if (text == NULL) {
        return;
    }

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == NULL || hOut == INVALID_HANDLE_VALUE) {
        return;
    }

    length = (DWORD)lstrlenA(text);
    if (length == 0) {
        return;
    }

    written = 0;
    WriteFile(hOut, text, length, &written, NULL);
}

static void diag_printf(const char *fmt, ...)
{
    char buffer[DIAG_BUF_LEN];
    va_list args;

    va_start(args, fmt);
    wvsprintfA(buffer, fmt, args);
    va_end(args);

    diag_write_raw(buffer);
}

static void diag_print_last_error(const char *step, DWORD error)
{
    diag_printf("[FAIL] %s (GetLastError=%lu)\r\n", step, (unsigned long)error);
}

static void diag_print_ok(const char *step)
{
    diag_printf("[ OK ] %s\r\n", step);
}

static void diag_print_info(const char *fmt, ...)
{
    char buffer[DIAG_BUF_LEN];
    va_list args;

    va_start(args, fmt);
    wvsprintfA(buffer, fmt, args);
    va_end(args);

    diag_printf("[INFO] %s\r\n", buffer);
}

static void diag_print_reg_error(const char *step, LONG error)
{
    diag_printf("[FAIL] %s (RegError=%lu)\r\n",
                step,
                (unsigned long)error);
}

static BOOL diag_arg_equals(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return FALSE;
    }

    return lstrcmpiA(lhs, rhs) == 0;
}

static const char *diag_frontend_owner_name(unsigned long owner)
{
    switch (owner) {
    case VMODEM_FRONTEND_OWNER_VCOMM:
        return "VCOMM";
    case VMODEM_FRONTEND_OWNER_DOS:
        return "DOS";
    default:
        return "NONE";
    }
}

static void diag_dump_dos_uart(const VMODEM_DOS_UART_DIAGNOSTIC *diag)
{
    if (diag == NULL) {
        return;
    }

    diag_printf("       DOS owner=%s vm=0x%08lX enabled=%lu base=0x%04lX irq=%lu irq_asserted=%lu pending=0x%08lX\r\n",
                diag_frontend_owner_name(diag->owner_type),
                diag->owner_vm_id,
                diag->enabled,
                diag->base_port,
                diag->irq_number,
                diag->irq_asserted,
                diag->pending_irq);
    diag_printf("       DOS regs ier=0x%02X iir=0x%02X fcr=0x%02X lcr=0x%02X mcr=0x%02X lsr=0x%02X msr=0x%02X scr=0x%02X dll=0x%02X dlm=0x%02X\r\n",
                (unsigned int)diag->ier,
                (unsigned int)diag->iir,
                (unsigned int)diag->fcr,
                (unsigned int)diag->lcr,
                (unsigned int)diag->mcr,
                (unsigned int)diag->lsr,
                (unsigned int)diag->msr,
                (unsigned int)diag->scr,
                (unsigned int)diag->dll,
                (unsigned int)diag->dlm);
    diag_printf("       DOS fifo rx=%lu tx=%lu\r\n",
                diag->rx_fifo_depth,
                diag->tx_fifo_depth);
}

static void diag_copy_hook_log(char *dst,
                               const char *src,
                               unsigned long srcLen,
                               unsigned long cchDst)
{
    unsigned long copyLen;

    if (dst == NULL || cchDst == 0) {
        return;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    copyLen = srcLen;
    if (copyLen + 1 > cchDst) {
        copyLen = cchDst - 1;
    }

    if (copyLen > 0) {
        CopyMemory(dst, src, copyLen);
    }
    dst[copyLen] = '\0';
}

static const char *diag_skip_port_prefix(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    if (name[0] == '\\' &&
        name[1] == '\\' &&
        name[2] == '.' &&
        name[3] == '\\') {
        return name + 4;
    }

    return name;
}

static void diag_normalize_port_name(const char *src, char *dst, int cchDst)
{
    int i;

    if (dst == NULL || cchDst <= 0) {
        return;
    }

    dst[0] = '\0';
    if (src == NULL || src[0] == '\0') {
        return;
    }

    src = diag_skip_port_prefix(src);
    i = 0;
    while (src[i] != '\0' && src[i] != ':' && i + 1 < cchDst) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static BOOL diag_port_name_matches(const char *lhs, const char *rhs)
{
    char lhsNorm[32];
    char rhsNorm[32];

    diag_normalize_port_name(lhs, lhsNorm, sizeof(lhsNorm));
    diag_normalize_port_name(rhs, rhsNorm, sizeof(rhsNorm));
    if (lhsNorm[0] == '\0' || rhsNorm[0] == '\0') {
        return FALSE;
    }

    return lstrcmpiA(lhsNorm, rhsNorm) == 0;
}

static void diag_build_device_path(const char *portName,
                                   char *dst,
                                   int cchDst,
                                   BOOL includeColon)
{
    const char *src;
    int i;

    if (dst == NULL || cchDst <= 0) {
        return;
    }

    dst[0] = '\0';
    if (portName == NULL || portName[0] == '\0') {
        return;
    }

    src = diag_skip_port_prefix(portName);

    lstrcpynA(dst, "\\\\.\\", cchDst);
    i = lstrlenA(dst);
    while (*src != '\0' && *src != ':' && i + 1 < cchDst) {
        dst[i++] = *src++;
    }
    if (includeColon && i + 1 < cchDst) {
        dst[i++] = ':';
    }
    dst[i] = '\0';
}

static void diag_registry_serialcomm_probe(const char *portName)
{
    HKEY hKey;
    LONG regError;
    DWORD index;
    DWORD valueNameLen;
    DWORD dataLen;
    DWORD type;
    char valueName[256];
    BYTE dataBuf[256];
    char targetPort[32];
    BOOL foundTarget;

    diag_write_raw("SerialComm registry probe (HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM)\r\n");

    hKey = NULL;
    regError = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                             DIAG_SERIALCOMM_KEY,
                             0,
                             KEY_READ,
                             &hKey);
    if (regError != ERROR_SUCCESS) {
        diag_print_reg_error("RegOpenKeyExA(SerialComm)", regError);
        diag_write_raw("\r\n");
        return;
    }

    diag_print_ok("RegOpenKeyExA(SerialComm)");

    diag_normalize_port_name(portName, targetPort, sizeof(targetPort));
    foundTarget = FALSE;
    index = 0;
    for (;;) {
        valueNameLen = sizeof(valueName);
        dataLen = sizeof(dataBuf);
        type = 0;
        regError = RegEnumValueA(hKey,
                                 index,
                                 valueName,
                                 &valueNameLen,
                                 NULL,
                                 &type,
                                 dataBuf,
                                 &dataLen);
        if (regError == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (regError != ERROR_SUCCESS) {
            diag_print_reg_error("RegEnumValueA(SerialComm)", regError);
            break;
        }

        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            dataBuf[sizeof(dataBuf) - 1] = '\0';
            diag_printf("       SerialComm: %s = %s\r\n",
                        valueName,
                        (const char *)dataBuf);
            if (diag_port_name_matches((const char *)dataBuf, targetPort)) {
                foundTarget = TRUE;
            }
        } else {
            diag_printf("       SerialComm: %s (type=%lu, size=%lu)\r\n",
                        valueName,
                        (unsigned long)type,
                        (unsigned long)dataLen);
        }

        ++index;
    }

    if (targetPort[0] != '\0') {
        if (foundTarget) {
            diag_printf("[ OK ] SerialComm contains %s\r\n", targetPort);
        } else {
            diag_printf("[INFO] SerialComm does not contain %s\r\n", targetPort);
        }
    }

    RegCloseKey(hKey);
    diag_print_ok("RegCloseKey(SerialComm)");
    diag_write_raw("\r\n");
}

static void diag_close_handle_named(HANDLE h, const char *step)
{
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        return;
    }

    if (!CloseHandle(h)) {
        diag_print_last_error(step, GetLastError());
    } else {
        diag_print_ok(step);
    }
}

static void diag_dump_dcb(const DCB *dcb)
{
    if (dcb == NULL) {
        return;
    }

    diag_printf("       DCB: Baud=%lu ByteSize=%u Parity=%u StopBits=%u "
                "fBinary=%lu fParity=%lu fOutxCtsFlow=%lu fOutxDsrFlow=%lu "
                "fDtrControl=%lu fRtsControl=%lu fOutX=%lu fInX=%lu "
                "XonLim=%u XoffLim=%u XonChar=0x%02X XoffChar=0x%02X\r\n",
                (unsigned long)dcb->BaudRate,
                (unsigned int)dcb->ByteSize,
                (unsigned int)dcb->Parity,
                (unsigned int)dcb->StopBits,
                (unsigned long)dcb->fBinary,
                (unsigned long)dcb->fParity,
                (unsigned long)dcb->fOutxCtsFlow,
                (unsigned long)dcb->fOutxDsrFlow,
                (unsigned long)dcb->fDtrControl,
                (unsigned long)dcb->fRtsControl,
                (unsigned long)dcb->fOutX,
                (unsigned long)dcb->fInX,
                (unsigned int)dcb->XonLim,
                (unsigned int)dcb->XoffLim,
                (unsigned int)(unsigned char)dcb->XonChar,
                (unsigned int)(unsigned char)dcb->XoffChar);
}

static void diag_dump_commprop(const COMMPROP *prop)
{
    if (prop == NULL) {
        return;
    }

    diag_printf("       COMMPROP: dwServiceMask=0x%08lX dwProvSubType=%lu "
                "dwProvCapabilities=0x%08lX dwSettableParams=0x%08lX "
                "dwSettableBaud=0x%08lX RxQ=%lu TxQ=%lu\r\n",
                (unsigned long)prop->dwServiceMask,
                (unsigned long)prop->dwProvSubType,
                (unsigned long)prop->dwProvCapabilities,
                (unsigned long)prop->dwSettableParams,
                (unsigned long)prop->dwSettableBaud,
                (unsigned long)prop->dwCurrentRxQueue,
                (unsigned long)prop->dwCurrentTxQueue);
}

static void diag_dump_commconfig(const COMMCONFIG *cfg)
{
    if (cfg == NULL) {
        return;
    }

    diag_printf("       COMMCONFIG: dwSize=%lu wVersion=0x%04X dwProviderSubType=%lu "
                "dwProviderOffset=%lu dwProviderSize=%lu\r\n",
                (unsigned long)cfg->dwSize,
                (unsigned int)cfg->wVersion,
                (unsigned long)cfg->dwProviderSubType,
                (unsigned long)cfg->dwProviderOffset,
                (unsigned long)cfg->dwProviderSize);
    diag_dump_dcb(&cfg->dcb);
}

static void diag_dump_modem_status(DWORD modemStatus)
{
    diag_printf("       MODEM: status=0x%08lX CTS=%lu DSR=%lu RING=%lu RLSD=%lu\r\n",
                (unsigned long)modemStatus,
                (unsigned long)((modemStatus & MS_CTS_ON) ? 1 : 0),
                (unsigned long)((modemStatus & MS_DSR_ON) ? 1 : 0),
                (unsigned long)((modemStatus & MS_RING_ON) ? 1 : 0),
                (unsigned long)((modemStatus & MS_RLSD_ON) ? 1 : 0));
}

static void diag_probe_comm_state(HANDLE hPort, const char *label)
{
    DCB dcb;

    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hPort, &dcb)) {
        diag_print_last_error(label, GetLastError());
        return;
    }

    diag_printf("[ OK ] %s\r\n", label);
    diag_dump_dcb(&dcb);
}

static void diag_probe_modem_status(HANDLE hPort, const char *label)
{
    DWORD modemStatus;

    modemStatus = 0;
    if (!GetCommModemStatus(hPort, &modemStatus)) {
        diag_print_last_error(label, GetLastError());
        return;
    }

    diag_printf("[ OK ] %s\r\n", label);
    diag_dump_modem_status(modemStatus);
}

static void diag_hook_log_probe(HANDLE hDevice)
{
    VMODEM_GET_HOOK_LOG_ACK hookInfo;
    char logText[VMODEM_HOOK_LOG_DATA_LEN];
    char captureLogText[VMODEM_HOOK_LOG_DATA_LEN];
    unsigned long i;
    BOOL sawCaptureFn;
    int result;

    diag_write_raw("IFS hook probe\r\n");

    ZeroMemory(&hookInfo, sizeof(hookInfo));
    result = IPC_GetHookLog(hDevice, VMODEM_IPC_VERSION, &hookInfo);
    if (!result) {
        diag_print_last_error("IPC_GetHookLog", GetLastError());
        diag_write_raw("\r\n");
        return;
    }

    diag_printf("       hook_installed=%lu  hook_fire_count=%lu  port_open_count=%lu\r\n",
                hookInfo.hook_installed,
                hookInfo.hook_fire_count,
                hookInfo.port_open_count);
    diag_printf("       capture_enabled=%lu  capture_generation=%lu  capture_hook_fire_count=%lu  capture_port_open_count=%lu\r\n",
                hookInfo.capture_enabled,
                hookInfo.capture_generation,
                hookInfo.capture_hook_fire_count,
                hookInfo.capture_port_open_count);

    if (hookInfo.hook_installed) {
        diag_print_ok("IFS hook is installed");
    } else {
        diag_write_raw("[FAIL] IFS hook is NOT installed\r\n");
    }

    if (hookInfo.port_open_count == 0) {
        diag_write_raw("[INFO] PortOpen has not been called yet\r\n");
    } else if (hookInfo.log_len == 0) {
        diag_write_raw("[INFO] PortOpen was called but log is empty\r\n");
    } else {
        diag_copy_hook_log(logText,
                           hookInfo.log_data,
                           hookInfo.log_len,
                           sizeof(logText));
        diag_printf("[ OK ] PortOpen names seen: %s\r\n", logText);
    }

    sawCaptureFn = FALSE;
    for (i = 0; i < VMODEM_HOOK_FN_BUCKETS; ++i) {
        if (hookInfo.capture_fn_counts[i] == 0) {
            continue;
        }
        if (!sawCaptureFn) {
            diag_write_raw("[ OK ] Capture fn counts:\r\n");
            sawCaptureFn = TRUE;
        }
        diag_printf("       fn=%lu count=%lu\r\n",
                    i,
                    hookInfo.capture_fn_counts[i]);
    }
    if (hookInfo.capture_other_fn_count != 0) {
        if (!sawCaptureFn) {
            diag_write_raw("[ OK ] Capture fn counts:\r\n");
            sawCaptureFn = TRUE;
        }
        diag_printf("       fn=other count=%lu\r\n",
                    hookInfo.capture_other_fn_count);
    }
    if (hookInfo.capture_hook_fire_count != 0 && !sawCaptureFn) {
        diag_write_raw("[INFO] Capture saw hook activity but no fn buckets were recorded\r\n");
    }

    if (hookInfo.capture_log_len != 0) {
        diag_copy_hook_log(captureLogText,
                           hookInfo.capture_log_data,
                           hookInfo.capture_log_len,
                           sizeof(captureLogText));
        diag_printf("[ OK ] Capture log: %s\r\n", captureLogText);
    } else if (hookInfo.capture_generation != 0) {
        diag_write_raw("[INFO] Capture log is empty\r\n");
    }

    diag_write_raw("\r\n");
}

static int diag_hook_capture_control_on_handle(HANDLE hDevice,
                                               unsigned long action,
                                               VMODEM_HOOK_CAPTURE_CONTROL_ACK *pInfoOut)
{
    VMODEM_HOOK_CAPTURE_CONTROL_ACK controlInfo;
    int result;

    ZeroMemory(&controlInfo, sizeof(controlInfo));
    result = IPC_HookCaptureControl(hDevice,
                                    VMODEM_IPC_VERSION,
                                    action,
                                    &controlInfo);
    if (!result) {
        diag_print_last_error("IPC_HookCaptureControl", GetLastError());
        return 0;
    }

    diag_printf("[ OK ] IPC_HookCaptureControl action=%lu enabled=%lu generation=%lu\r\n",
                action,
                controlInfo.capture_enabled,
                controlInfo.capture_generation);

    if (pInfoOut != NULL) {
        *pInfoOut = controlInfo;
    }

    return 1;
}

static void diag_hook_capture_control(const char *heading,
                                      unsigned long action)
{
    HANDLE hDevice;
    int result;

    diag_printf("%s\r\n", heading);

    hDevice = INVALID_HANDLE_VALUE;
    result = IPC_Open(&hDevice);
    if (result != 0) {
        diag_print_last_error("IPC_Open", (DWORD)result);
        return;
    }
    diag_print_ok("IPC_Open");

    if (!diag_hook_capture_control_on_handle(hDevice, action, NULL)) {
        IPC_Close(hDevice);
        return;
    }

    diag_hook_log_probe(hDevice);
    IPC_Close(hDevice);
    diag_print_ok("IPC_Close");
}

static void diag_capture_open_probe(const char *heading,
                                    const char *portPath,
                                    DWORD createFlags,
                                    const char *openStep)
{
    HANDLE hDevice;
    HANDLE hPort;
    int result;

    diag_printf("%s (%s)\r\n", heading, portPath);

    hDevice = INVALID_HANDLE_VALUE;
    result = IPC_Open(&hDevice);
    if (result != 0) {
        diag_print_last_error("IPC_Open", (DWORD)result);
        diag_write_raw("\r\n");
        return;
    }
    diag_print_ok("IPC_Open");

    if (!diag_hook_capture_control_on_handle(hDevice,
                                             VMODEM_HOOK_CAPTURE_ACTION_START,
                                             NULL)) {
        IPC_Close(hDevice);
        diag_print_ok("IPC_Close");
        diag_write_raw("\r\n");
        return;
    }

    hPort = CreateFileA(portPath,
                        GENERIC_READ | GENERIC_WRITE,
                        0,
                        NULL,
                        OPEN_EXISTING,
                        createFlags,
                        NULL);
    if (hPort == INVALID_HANDLE_VALUE) {
        diag_print_last_error(openStep, GetLastError());
    } else {
        diag_print_ok(openStep);
        diag_close_handle_named(hPort, "CloseHandle(captured open handle)");
    }

    if (diag_hook_capture_control_on_handle(hDevice,
                                            VMODEM_HOOK_CAPTURE_ACTION_STOP,
                                            NULL)) {
        diag_hook_log_probe(hDevice);
    }

    IPC_Close(hDevice);
    diag_print_ok("IPC_Close");
    diag_write_raw("\r\n");
}

static void diag_control_path_probe(void)
{
    HANDLE hDevice;
    VMODEM_QUERY_DRIVER_ACK driverInfo;
    unsigned long status;
    int result;

    diag_write_raw("Control-device probe (\\\\.\\VMODEM.VXD)\r\n");

    hDevice = INVALID_HANDLE_VALUE;
    result = IPC_Open(&hDevice);
    if (result != 0) {
        diag_print_last_error("IPC_Open", (DWORD)result);
        return;
    }
    diag_print_ok("IPC_Open");

    status = 0xFFFFFFFFUL;
    result = IPC_SendHello(hDevice, VMODEM_IPC_VERSION, &status);
    if (!result) {
        diag_print_last_error("IPC_SendHello", status);
        IPC_Close(hDevice);
        return;
    }

    diag_printf("[ OK ] IPC_SendHello status=%lu\r\n", status);

    ZeroMemory(&driverInfo, sizeof(driverInfo));
    result = IPC_QueryDriver(hDevice, VMODEM_IPC_VERSION, &driverInfo);
    if (!result) {
        diag_print_last_error("IPC_QueryDriver", GetLastError());
        IPC_Close(hDevice);
        return;
    }

    diag_printf("[ OK ] IPC_QueryDriver status=%lu build=0x%08lX open=%lu "
                "dev=0x%08lX base=0x%08lX irq=%lu port=%s tag=%s\r\n",
                driverInfo.status,
                driverInfo.build_id,
                driverInfo.port_open,
                driverInfo.dev_node,
                driverInfo.alloc_base,
                driverInfo.alloc_irq,
                driverInfo.port_name,
                driverInfo.build_tag);
    diag_printf("       DRIVER frontend owner=%s\r\n",
                diag_frontend_owner_name(driverInfo.frontend_owner));
    diag_write_raw("       DRIVER default modem status:\r\n");
    diag_dump_modem_status(driverInfo.default_status);
    diag_write_raw("       DRIVER current modem status:\r\n");
    diag_dump_modem_status(driverInfo.modem_status);
    diag_printf("       DRIVER contention handler=0x%08lX resource=0x%08lX requests=%lu\r\n",
                driverInfo.contention_handler,
                driverInfo.contention_resource,
                driverInfo.contention_requests);
    diag_printf("       IPC helper=%lu generation=%lu session=%lu queue=%lu "
                "to=%lu from=%lu\r\n",
                driverInfo.helper_attached,
                driverInfo.helper_generation,
                driverInfo.active_session_id,
                driverInfo.helper_queue_depth,
                driverInfo.last_msg_to_helper,
                driverInfo.last_msg_from_helper);
    diag_printf("       MODE raw=%lu s0=%lu s1=%lu\r\n",
                driverInfo.raw_mode_enabled,
                driverInfo.s0_auto_answer_rings,
                driverInfo.s1_ring_count);
    diag_dump_dos_uart(&driverInfo.dos_uart);

    diag_hook_log_probe(hDevice);

    IPC_Close(hDevice);
    diag_print_ok("IPC_Close");
    diag_write_raw("\r\n");
}

static BOOL diag_apply_roundtrip_commconfig(HANDLE hPort)
{
    BYTE ccBuf[DIAG_CC_BUF_LEN];
    COMMCONFIG *cfg;
    DWORD cfgSize;
    BOOL ok;

    cfg = (COMMCONFIG *)ccBuf;
    cfgSize = sizeof(ccBuf);
    ok = GetCommConfig(hPort, cfg, &cfgSize);
    if (!ok) {
        diag_print_last_error("GetCommConfig(for SetCommConfig)", GetLastError());
        return FALSE;
    }

    cfg->dcb.fBinary = TRUE;
    cfg->dcb.fParity = FALSE;
    cfg->dcb.fOutxCtsFlow = FALSE;
    cfg->dcb.fOutxDsrFlow = FALSE;
    cfg->dcb.fDtrControl = DTR_CONTROL_ENABLE;
    cfg->dcb.fDsrSensitivity = FALSE;
    cfg->dcb.fTXContinueOnXoff = FALSE;
    cfg->dcb.fOutX = FALSE;
    cfg->dcb.fInX = FALSE;
    cfg->dcb.fErrorChar = FALSE;
    cfg->dcb.fNull = FALSE;
    cfg->dcb.fRtsControl = RTS_CONTROL_ENABLE;
    cfg->dcb.fAbortOnError = FALSE;
    cfg->dcb.XonLim = 128;
    cfg->dcb.XoffLim = 128;
    cfg->dcb.BaudRate = 9600;
    cfg->dcb.ByteSize = 8;
    cfg->dcb.Parity = NOPARITY;
    cfg->dcb.StopBits = ONESTOPBIT;
    cfg->dcb.XonChar = 0x11;
    cfg->dcb.XoffChar = 0x13;
    cfg->dcb.ErrorChar = 0;
    cfg->dcb.EofChar = 0;
    cfg->dcb.EvtChar = 0;

    ok = SetCommConfig(hPort, cfg, cfgSize);
    if (!ok) {
        diag_print_last_error("SetCommConfig", GetLastError());
        return FALSE;
    }

    diag_print_ok("SetCommConfig");
    return TRUE;
}

static void diag_probe_overlapped_write(HANDLE hPort)
{
    static const char kWriteBuf[] = "AT\r";
    HANDLE hEvent;
    OVERLAPPED ov;
    DWORD bytesDone;
    DWORD waitRc;
    DWORD error;
    BOOL ok;

    hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (hEvent == NULL) {
        diag_print_last_error("CreateEventA(write)", GetLastError());
        return;
    }

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = hEvent;
    bytesDone = 0;
    ResetEvent(hEvent);

    ok = WriteFile(hPort, kWriteBuf, 3, &bytesDone, &ov);
    if (ok) {
        diag_printf("[ OK ] Overlapped WriteFile completed immediately wrote=%lu\r\n",
                    (unsigned long)bytesDone);
        diag_close_handle_named(hEvent, "CloseHandle(write event)");
        return;
    }

    error = GetLastError();
    if (error != ERROR_IO_PENDING) {
        diag_print_last_error("Overlapped WriteFile", error);
        diag_close_handle_named(hEvent, "CloseHandle(write event)");
        return;
    }

    diag_print_info("Overlapped WriteFile returned ERROR_IO_PENDING");
    waitRc = WaitForSingleObject(hEvent, DIAG_SHORT_WAIT_MS);
    if (waitRc == WAIT_OBJECT_0) {
        bytesDone = 0;
        ok = GetOverlappedResult(hPort, &ov, &bytesDone, FALSE);
        if (!ok) {
            diag_print_last_error("GetOverlappedResult(WriteFile)", GetLastError());
        } else {
            diag_printf("[ OK ] GetOverlappedResult(WriteFile) wrote=%lu\r\n",
                        (unsigned long)bytesDone);
        }
    } else if (waitRc == WAIT_TIMEOUT) {
        diag_print_info("Overlapped WriteFile stayed pending");
        ok = PurgeComm(hPort, PURGE_TXABORT | PURGE_TXCLEAR);
        if (!ok) {
            diag_print_last_error("PurgeComm(PURGE_TXABORT|PURGE_TXCLEAR)",
                                  GetLastError());
        } else {
            diag_print_ok("PurgeComm(PURGE_TXABORT|PURGE_TXCLEAR)");
        }
    } else {
        diag_print_last_error("WaitForSingleObject(write event)", GetLastError());
    }

    diag_close_handle_named(hEvent, "CloseHandle(write event)");
}

static void diag_probe_overlapped_read(HANDLE hPort)
{
    HANDLE hEvent;
    OVERLAPPED ov;
    DWORD bytesDone;
    DWORD waitRc;
    DWORD error;
    char readBuf[DIAG_OVERLAPPED_BUF_LEN];
    BOOL ok;

    hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (hEvent == NULL) {
        diag_print_last_error("CreateEventA(read)", GetLastError());
        return;
    }

    ZeroMemory(readBuf, sizeof(readBuf));
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = hEvent;
    bytesDone = 0;
    ResetEvent(hEvent);

    ok = ReadFile(hPort, readBuf, sizeof(readBuf), &bytesDone, &ov);
    if (ok) {
        diag_printf("[ OK ] Overlapped ReadFile completed immediately read=%lu\r\n",
                    (unsigned long)bytesDone);
        diag_close_handle_named(hEvent, "CloseHandle(read event)");
        return;
    }

    error = GetLastError();
    if (error != ERROR_IO_PENDING) {
        diag_print_last_error("Overlapped ReadFile", error);
        diag_close_handle_named(hEvent, "CloseHandle(read event)");
        return;
    }

    diag_print_info("Overlapped ReadFile returned ERROR_IO_PENDING");
    waitRc = WaitForSingleObject(hEvent, DIAG_SHORT_WAIT_MS);
    if (waitRc == WAIT_OBJECT_0) {
        bytesDone = 0;
        ok = GetOverlappedResult(hPort, &ov, &bytesDone, FALSE);
        if (!ok) {
            diag_print_last_error("GetOverlappedResult(ReadFile)", GetLastError());
        } else {
            diag_printf("[ OK ] GetOverlappedResult(ReadFile) read=%lu\r\n",
                        (unsigned long)bytesDone);
        }
    } else if (waitRc == WAIT_TIMEOUT) {
        diag_print_info("Overlapped ReadFile stayed pending");
        ok = PurgeComm(hPort, PURGE_RXABORT | PURGE_RXCLEAR);
        if (!ok) {
            diag_print_last_error("PurgeComm(PURGE_RXABORT|PURGE_RXCLEAR)",
                                  GetLastError());
        } else {
            diag_print_ok("PurgeComm(PURGE_RXABORT|PURGE_RXCLEAR)");
            waitRc = WaitForSingleObject(hEvent, DIAG_SHORT_WAIT_MS);
            if (waitRc == WAIT_OBJECT_0) {
                bytesDone = 0;
                ok = GetOverlappedResult(hPort, &ov, &bytesDone, FALSE);
                if (!ok) {
                    diag_print_last_error("GetOverlappedResult(ReadFile after purge)",
                                          GetLastError());
                } else {
                    diag_printf("[ OK ] GetOverlappedResult(ReadFile after purge) "
                                "read=%lu\r\n",
                                (unsigned long)bytesDone);
                }
            } else if (waitRc == WAIT_TIMEOUT) {
                diag_print_info("Overlapped ReadFile still pending after purge");
            } else {
                diag_print_last_error("WaitForSingleObject(read event after purge)",
                                      GetLastError());
            }
        }
    } else {
        diag_print_last_error("WaitForSingleObject(read event)", GetLastError());
    }

    diag_close_handle_named(hEvent, "CloseHandle(read event)");
}

static void diag_probe_hyperterminal_concurrent_io(HANDLE hPort)
{
    static const char kWriteBuf[] = "AT\r";
    HANDLE waitEvent;
    HANDLE readEvent;
    HANDLE writeEvent;
    HANDLE waitHandles[3];
    OVERLAPPED waitOv;
    OVERLAPPED readOv;
    OVERLAPPED writeOv;
    DWORD waitMask;
    DWORD bytesDone;
    DWORD waitRc;
    DWORD error;
    char readBuf[DIAG_OVERLAPPED_BUF_LEN];
    BOOL ok;
    BOOL waitPending;
    BOOL readPending;
    BOOL writePending;

    diag_write_raw("       Concurrent overlapped probe\r\n");

    waitEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (waitEvent == NULL) {
        diag_print_last_error("CreateEventA(concurrent wait)", GetLastError());
        return;
    }

    readEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (readEvent == NULL) {
        diag_print_last_error("CreateEventA(concurrent read)", GetLastError());
        diag_close_handle_named(waitEvent, "CloseHandle(concurrent wait event)");
        return;
    }

    writeEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (writeEvent == NULL) {
        diag_print_last_error("CreateEventA(concurrent write)", GetLastError());
        diag_close_handle_named(readEvent, "CloseHandle(concurrent read event)");
        diag_close_handle_named(waitEvent, "CloseHandle(concurrent wait event)");
        return;
    }

    waitHandles[0] = waitEvent;
    waitHandles[1] = readEvent;
    waitHandles[2] = writeEvent;

    waitPending = FALSE;
    readPending = FALSE;
    writePending = FALSE;
    waitMask = 0;
    bytesDone = 0;
    ZeroMemory(readBuf, sizeof(readBuf));
    ZeroMemory(&waitOv, sizeof(waitOv));
    ZeroMemory(&readOv, sizeof(readOv));
    ZeroMemory(&writeOv, sizeof(writeOv));
    waitOv.hEvent = waitEvent;
    readOv.hEvent = readEvent;
    writeOv.hEvent = writeEvent;
    ResetEvent(waitEvent);
    ResetEvent(readEvent);
    ResetEvent(writeEvent);

    ok = SetCommMask(hPort, DIAG_HYPERTRM_MASK);
    if (!ok) {
        diag_print_last_error("SetCommMask(concurrent)", GetLastError());
        goto cleanup;
    }
    diag_printf("[ OK ] SetCommMask(concurrent) mask=0x%08lX\r\n",
                (unsigned long)DIAG_HYPERTRM_MASK);

    ok = WaitCommEvent(hPort, &waitMask, &waitOv);
    if (ok) {
        diag_printf("[ OK ] Concurrent WaitCommEvent completed immediately mask=0x%08lX\r\n",
                    (unsigned long)waitMask);
    } else {
        error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            diag_print_last_error("Concurrent WaitCommEvent", error);
            goto cleanup;
        }

        waitPending = TRUE;
        diag_print_info("Concurrent WaitCommEvent returned ERROR_IO_PENDING");
    }

    bytesDone = 0;
    ok = ReadFile(hPort, readBuf, sizeof(readBuf), &bytesDone, &readOv);
    if (ok) {
        diag_printf("[ OK ] Concurrent ReadFile completed immediately read=%lu\r\n",
                    (unsigned long)bytesDone);
    } else {
        error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            diag_print_last_error("Concurrent ReadFile", error);
            goto cleanup;
        }

        readPending = TRUE;
        diag_print_info("Concurrent ReadFile returned ERROR_IO_PENDING");
    }

    bytesDone = 0;
    ok = WriteFile(hPort, kWriteBuf, 3, &bytesDone, &writeOv);
    if (ok) {
        diag_printf("[ OK ] Concurrent WriteFile completed immediately wrote=%lu\r\n",
                    (unsigned long)bytesDone);
    } else {
        error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            diag_print_last_error("Concurrent WriteFile", error);
            goto cleanup;
        }

        writePending = TRUE;
        diag_print_info("Concurrent WriteFile returned ERROR_IO_PENDING");
    }

    waitRc = WaitForMultipleObjects(3, waitHandles, FALSE, DIAG_SHORT_WAIT_MS);
    if (waitRc < WAIT_OBJECT_0 + 3) {
        const char *which;

        which = "wait";
        if (waitRc == WAIT_OBJECT_0 + 1) {
            which = "read";
        } else if (waitRc == WAIT_OBJECT_0 + 2) {
            which = "write";
        }

        diag_printf("[ OK ] WaitForMultipleObjects signaled %s event\r\n", which);
    } else if (waitRc == WAIT_TIMEOUT) {
        diag_print_info("WaitForMultipleObjects timed out with concurrent I/O still pending");
    } else {
        diag_print_last_error("WaitForMultipleObjects(concurrent)", GetLastError());
    }

    if (writePending) {
        bytesDone = 0;
        ok = GetOverlappedResult(hPort, &writeOv, &bytesDone, FALSE);
        if (ok) {
            diag_printf("[ OK ] GetOverlappedResult(Concurrent WriteFile) wrote=%lu\r\n",
                        (unsigned long)bytesDone);
            writePending = FALSE;
        } else {
            error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) {
                diag_print_info("Concurrent WriteFile still pending");
            } else {
                diag_print_last_error("GetOverlappedResult(Concurrent WriteFile)", error);
                writePending = FALSE;
            }
        }
    }

    if (readPending) {
        bytesDone = 0;
        ok = GetOverlappedResult(hPort, &readOv, &bytesDone, FALSE);
        if (ok) {
            diag_printf("[ OK ] GetOverlappedResult(Concurrent ReadFile) read=%lu\r\n",
                        (unsigned long)bytesDone);
            readPending = FALSE;
        } else {
            error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) {
                diag_print_info("Concurrent ReadFile still pending");
            } else {
                diag_print_last_error("GetOverlappedResult(Concurrent ReadFile)", error);
                readPending = FALSE;
            }
        }
    }

    if (waitPending) {
        bytesDone = 0;
        ok = GetOverlappedResult(hPort, &waitOv, &bytesDone, FALSE);
        if (ok) {
            diag_printf("[ OK ] GetOverlappedResult(Concurrent WaitCommEvent) "
                        "bytes=%lu mask=0x%08lX\r\n",
                        (unsigned long)bytesDone,
                        (unsigned long)waitMask);
            waitPending = FALSE;
        } else {
            error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) {
                diag_print_info("Concurrent WaitCommEvent still pending");
            } else {
                diag_print_last_error("GetOverlappedResult(Concurrent WaitCommEvent)",
                                      error);
                waitPending = FALSE;
            }
        }
    }

    if (readPending) {
        ok = PurgeComm(hPort, PURGE_RXABORT | PURGE_RXCLEAR);
        if (!ok) {
            diag_print_last_error("PurgeComm(concurrent RX)", GetLastError());
        } else {
            diag_print_ok("PurgeComm(concurrent RX)");
            waitRc = WaitForSingleObject(readEvent, DIAG_SHORT_WAIT_MS);
            if (waitRc == WAIT_OBJECT_0) {
                bytesDone = 0;
                ok = GetOverlappedResult(hPort, &readOv, &bytesDone, FALSE);
                if (!ok) {
                    diag_print_last_error("GetOverlappedResult(Concurrent ReadFile after purge)",
                                          GetLastError());
                } else {
                    diag_printf("[ OK ] GetOverlappedResult(Concurrent ReadFile after purge) "
                                "read=%lu\r\n",
                                (unsigned long)bytesDone);
                }
            } else if (waitRc == WAIT_TIMEOUT) {
                diag_print_info("Concurrent ReadFile stayed pending after purge");
            } else {
                diag_print_last_error("WaitForSingleObject(concurrent read event)",
                                      GetLastError());
            }
        }
    }

    if (waitPending) {
        ok = SetCommMask(hPort, 0);
        if (!ok) {
            diag_print_last_error("SetCommMask(0) for concurrent wait release",
                                  GetLastError());
        } else {
            diag_print_ok("SetCommMask(0) for concurrent wait release");
            waitRc = WaitForSingleObject(waitEvent, DIAG_SHORT_WAIT_MS);
            if (waitRc == WAIT_OBJECT_0) {
                bytesDone = 0;
                ok = GetOverlappedResult(hPort, &waitOv, &bytesDone, FALSE);
                if (!ok) {
                    diag_print_last_error("GetOverlappedResult(Concurrent WaitCommEvent after release)",
                                          GetLastError());
                } else {
                    diag_printf("[ OK ] GetOverlappedResult(Concurrent WaitCommEvent after release) "
                                "bytes=%lu mask=0x%08lX\r\n",
                                (unsigned long)bytesDone,
                                (unsigned long)waitMask);
                }
            } else if (waitRc == WAIT_TIMEOUT) {
                diag_print_info("Concurrent WaitCommEvent stayed pending after SetCommMask(0)");
            } else {
                diag_print_last_error("WaitForSingleObject(concurrent wait event)",
                                      GetLastError());
            }
        }
    }

    if (writePending) {
        ok = PurgeComm(hPort, PURGE_TXABORT | PURGE_TXCLEAR);
        if (!ok) {
            diag_print_last_error("PurgeComm(concurrent TX)", GetLastError());
        } else {
            diag_print_ok("PurgeComm(concurrent TX)");
            waitRc = WaitForSingleObject(writeEvent, DIAG_SHORT_WAIT_MS);
            if (waitRc == WAIT_OBJECT_0) {
                bytesDone = 0;
                ok = GetOverlappedResult(hPort, &writeOv, &bytesDone, FALSE);
                if (!ok) {
                    diag_print_last_error("GetOverlappedResult(Concurrent WriteFile after purge)",
                                          GetLastError());
                } else {
                    diag_printf("[ OK ] GetOverlappedResult(Concurrent WriteFile after purge) "
                                "wrote=%lu\r\n",
                                (unsigned long)bytesDone);
                }
            } else if (waitRc == WAIT_TIMEOUT) {
                diag_print_info("Concurrent WriteFile stayed pending after purge");
            } else {
                diag_print_last_error("WaitForSingleObject(concurrent write event)",
                                      GetLastError());
            }
        }
    }

cleanup:
    diag_close_handle_named(writeEvent, "CloseHandle(concurrent write event)");
    diag_close_handle_named(readEvent, "CloseHandle(concurrent read event)");
    diag_close_handle_named(waitEvent, "CloseHandle(concurrent wait event)");
}

static void diag_port_probe_one(const char *portPath)
{
    HANDLE hPort;
    DCB dcb;
    COMMPROP prop;
    COMMTIMEOUTS timeouts;
    COMSTAT stat;
    DWORD eventMask;
    DWORD errors;
    DWORD bytesDone;
    char ioBuf[16];
    BYTE ccBuf[DIAG_CC_BUF_LEN];
    COMMCONFIG *cfg;
    DWORD cfgSize;
    BOOL ok;

    diag_printf("Serial probe (%s)\r\n", portPath);

    hPort = CreateFileA(portPath,
                        GENERIC_READ | GENERIC_WRITE,
                        0,
                        NULL,
                        OPEN_EXISTING,
                        0,
                        NULL);
    if (hPort == INVALID_HANDLE_VALUE) {
        diag_print_last_error("CreateFileA", GetLastError());
        diag_write_raw("\r\n");
        return;
    }
    diag_print_ok("CreateFileA");

    dcb.DCBlength = sizeof(dcb);
    ok = GetCommState(hPort, &dcb);
    if (!ok) {
        diag_print_last_error("GetCommState", GetLastError());
    } else {
        diag_print_ok("GetCommState");
        diag_dump_dcb(&dcb);
    }

    ok = GetCommProperties(hPort, &prop);
    if (!ok) {
        diag_print_last_error("GetCommProperties", GetLastError());
    } else {
        diag_print_ok("GetCommProperties");
        diag_dump_commprop(&prop);
    }

    cfg = (COMMCONFIG *)ccBuf;
    cfgSize = 0;
    ok = GetCommConfig(hPort, cfg, &cfgSize);
    if (!ok) {
        diag_printf("[INFO] GetCommConfig(size probe) returned FALSE, "
                    "GetLastError=%lu requiredSize=%lu\r\n",
                    (unsigned long)GetLastError(),
                    (unsigned long)cfgSize);
    } else {
        diag_printf("[INFO] GetCommConfig(size probe) returned TRUE "
                    "requiredSize=%lu\r\n",
                    (unsigned long)cfgSize);
    }

    cfgSize = sizeof(ccBuf);
    ok = GetCommConfig(hPort, cfg, &cfgSize);
    if (!ok) {
        diag_print_last_error("GetCommConfig(full)", GetLastError());
    } else {
        diag_print_ok("GetCommConfig(full)");
        diag_dump_commconfig(cfg);
        diag_apply_roundtrip_commconfig(hPort);
    }

    ok = GetCommTimeouts(hPort, &timeouts);
    if (!ok) {
        diag_print_last_error("GetCommTimeouts", GetLastError());
    } else {
        diag_printf("[ OK ] GetCommTimeouts RI=%lu RM=%lu RC=%lu WM=%lu WC=%lu\r\n",
                    (unsigned long)timeouts.ReadIntervalTimeout,
                    (unsigned long)timeouts.ReadTotalTimeoutMultiplier,
                    (unsigned long)timeouts.ReadTotalTimeoutConstant,
                    (unsigned long)timeouts.WriteTotalTimeoutMultiplier,
                    (unsigned long)timeouts.WriteTotalTimeoutConstant);
    }

    ok = SetupComm(hPort, 4096, 4096);
    if (!ok) {
        diag_print_last_error("SetupComm", GetLastError());
    } else {
        diag_print_ok("SetupComm");
    }

    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX = TRUE;
    dcb.fInX = TRUE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;
    dcb.XonLim = 128;
    dcb.XoffLim = 128;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.XonChar = 0x11;
    dcb.XoffChar = 0x13;
    dcb.ErrorChar = 0;
    dcb.EofChar = 0;
    dcb.EvtChar = 0;

    ok = SetCommState(hPort, &dcb);
    if (!ok) {
        diag_print_last_error("SetCommState", GetLastError());
    } else {
        diag_print_ok("SetCommState");
    }
    diag_probe_comm_state(hPort, "GetCommState(after SetCommState)");

    diag_probe_modem_status(hPort, "GetCommModemStatus(initial)");

    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    ok = SetCommState(hPort, &dcb);
    if (!ok) {
        diag_print_last_error("SetCommState(DTR disable)", GetLastError());
    } else {
        diag_print_ok("SetCommState(DTR disable)");
    }
    diag_probe_comm_state(hPort, "GetCommState(after DTR disable)");
    diag_probe_modem_status(hPort, "GetCommModemStatus(after DTR disable)");

    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    ok = SetCommState(hPort, &dcb);
    if (!ok) {
        diag_print_last_error("SetCommState(DTR enable)", GetLastError());
    } else {
        diag_print_ok("SetCommState(DTR enable)");
    }
    diag_probe_comm_state(hPort, "GetCommState(after DTR enable)");
    diag_probe_modem_status(hPort, "GetCommModemStatus(after DTR enable)");

    ok = EscapeCommFunction(hPort, CLRDTR);
    if (!ok) {
        diag_print_last_error("EscapeCommFunction(CLRDTR)", GetLastError());
    } else {
        diag_print_ok("EscapeCommFunction(CLRDTR)");
    }
    diag_probe_modem_status(hPort, "GetCommModemStatus(after CLRDTR)");

    ok = EscapeCommFunction(hPort, SETDTR);
    if (!ok) {
        diag_print_last_error("EscapeCommFunction(SETDTR)", GetLastError());
    } else {
        diag_print_ok("EscapeCommFunction(SETDTR)");
    }
    diag_probe_modem_status(hPort, "GetCommModemStatus(after SETDTR)");

    ok = EscapeCommFunction(hPort, SETRTS);
    if (!ok) {
        diag_print_last_error("EscapeCommFunction(SETRTS)", GetLastError());
    } else {
        diag_print_ok("EscapeCommFunction(SETRTS)");
    }

    ok = EscapeCommFunction(hPort, SETXOFF);
    if (!ok) {
        diag_print_last_error("EscapeCommFunction(SETXOFF)", GetLastError());
    } else {
        diag_print_ok("EscapeCommFunction(SETXOFF)");
    }

    ok = EscapeCommFunction(hPort, SETXON);
    if (!ok) {
        diag_print_last_error("EscapeCommFunction(SETXON)", GetLastError());
    } else {
        diag_print_ok("EscapeCommFunction(SETXON)");
    }

    eventMask = EV_RXCHAR | EV_TXEMPTY | EV_CTS | EV_DSR | EV_RLSD | EV_RING;
    ok = SetCommMask(hPort, eventMask);
    if (!ok) {
        diag_print_last_error("SetCommMask", GetLastError());
    } else {
        diag_print_ok("SetCommMask");
    }

    eventMask = 0;
    ok = GetCommMask(hPort, &eventMask);
    if (!ok) {
        diag_print_last_error("GetCommMask", GetLastError());
    } else {
        diag_printf("[ OK ] GetCommMask mask=0x%08lX\r\n",
                    (unsigned long)eventMask);
    }

    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;
    ok = SetCommTimeouts(hPort, &timeouts);
    if (!ok) {
        diag_print_last_error("SetCommTimeouts", GetLastError());
    } else {
        diag_print_ok("SetCommTimeouts");
    }

    ok = GetCommTimeouts(hPort, &timeouts);
    if (!ok) {
        diag_print_last_error("GetCommTimeouts(after set)", GetLastError());
    } else {
        diag_printf("[ OK ] GetCommTimeouts(after set) RI=%lu RM=%lu RC=%lu WM=%lu WC=%lu\r\n",
                    (unsigned long)timeouts.ReadIntervalTimeout,
                    (unsigned long)timeouts.ReadTotalTimeoutMultiplier,
                    (unsigned long)timeouts.ReadTotalTimeoutConstant,
                    (unsigned long)timeouts.WriteTotalTimeoutMultiplier,
                    (unsigned long)timeouts.WriteTotalTimeoutConstant);
    }

    ok = TransmitCommChar(hPort, 'X');
    if (!ok) {
        diag_print_last_error("TransmitCommChar", GetLastError());
    } else {
        diag_print_ok("TransmitCommChar");
    }

    lstrcpyA(ioBuf, "AT\r");
    bytesDone = 0;
    ok = WriteFile(hPort, ioBuf, 3, &bytesDone, NULL);
    if (!ok) {
        diag_print_last_error("WriteFile", GetLastError());
    } else {
        diag_printf("[ OK ] WriteFile wrote=%lu\r\n", (unsigned long)bytesDone);
    }

    bytesDone = 0;
    ioBuf[0] = '\0';
    ok = ReadFile(hPort, ioBuf, sizeof(ioBuf), &bytesDone, NULL);
    if (!ok) {
        diag_print_last_error("ReadFile", GetLastError());
    } else {
        diag_printf("[ OK ] ReadFile read=%lu\r\n", (unsigned long)bytesDone);
    }

    ok = ClearCommError(hPort, &errors, &stat);
    if (!ok) {
        diag_print_last_error("ClearCommError", GetLastError());
    } else {
        diag_printf("[ OK ] ClearCommError errors=0x%08lX cbInQue=%lu cbOutQue=%lu\r\n",
                    (unsigned long)errors,
                    (unsigned long)stat.cbInQue,
                    (unsigned long)stat.cbOutQue);
    }

    ok = PurgeComm(hPort,
                   PURGE_TXABORT | PURGE_RXABORT |
                   PURGE_TXCLEAR | PURGE_RXCLEAR);
    if (!ok) {
        diag_print_last_error("PurgeComm", GetLastError());
    } else {
        diag_print_ok("PurgeComm");
        ok = ClearCommError(hPort, &errors, &stat);
        if (!ok) {
            diag_print_last_error("ClearCommError(after PurgeComm)",
                                  GetLastError());
        } else {
            diag_printf("[ OK ] ClearCommError(after PurgeComm) errors=0x%08lX "
                        "cbInQue=%lu cbOutQue=%lu\r\n",
                        (unsigned long)errors,
                        (unsigned long)stat.cbInQue,
                        (unsigned long)stat.cbOutQue);
        }
    }

    if (!CloseHandle(hPort)) {
        diag_print_last_error("CloseHandle", GetLastError());
    } else {
        diag_print_ok("CloseHandle");
    }

    diag_write_raw("\r\n");
}

static void diag_hyperterminal_style_probe_one(const char *portPath)
{
    HANDLE hPort;
    HANDLE hEvent;
    OVERLAPPED ov;
    COMMTIMEOUTS timeouts;
    DWORD waitMask;
    DWORD bytesDone;
    DWORD waitRc;
    DWORD error;
    BOOL ok;

    diag_printf("HyperTerminal-style probe (%s)\r\n", portPath);

    hPort = CreateFileA(portPath,
                        GENERIC_READ | GENERIC_WRITE,
                        0,
                        NULL,
                        OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED,
                        NULL);
    if (hPort == INVALID_HANDLE_VALUE) {
        diag_print_last_error("CreateFileA(FILE_FLAG_OVERLAPPED)", GetLastError());
        diag_write_raw("\r\n");
        return;
    }
    diag_print_ok("CreateFileA(FILE_FLAG_OVERLAPPED)");

    ok = SetupComm(hPort, 0x2000, 0x2000);
    if (!ok) {
        diag_print_last_error("SetupComm(0x2000,0x2000)", GetLastError());
    } else {
        diag_print_ok("SetupComm(0x2000,0x2000)");
    }

    diag_apply_roundtrip_commconfig(hPort);

    timeouts.ReadIntervalTimeout = 10;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 5000;
    ok = SetCommTimeouts(hPort, &timeouts);
    if (!ok) {
        diag_print_last_error("SetCommTimeouts(HyperTerminal style)", GetLastError());
    } else {
        diag_print_ok("SetCommTimeouts(HyperTerminal style)");
    }

    ok = SetCommMask(hPort, DIAG_HYPERTRM_MASK);
    if (!ok) {
        diag_print_last_error("SetCommMask(HyperTerminal style)", GetLastError());
    } else {
        diag_printf("[ OK ] SetCommMask(HyperTerminal style) mask=0x%08lX\r\n",
                    (unsigned long)DIAG_HYPERTRM_MASK);
    }

    hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (hEvent == NULL) {
        diag_print_last_error("CreateEventA", GetLastError());
    } else {
        diag_print_ok("CreateEventA");

        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = hEvent;
        waitMask = 0;
        ResetEvent(hEvent);

        ok = WaitCommEvent(hPort, &waitMask, &ov);
        if (ok) {
            diag_printf("[ OK ] WaitCommEvent returned immediately mask=0x%08lX\r\n",
                        (unsigned long)waitMask);
        } else {
            error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                diag_print_info("WaitCommEvent returned ERROR_IO_PENDING");

                ok = SetCommMask(hPort, 0);
                if (!ok) {
                    diag_print_last_error("SetCommMask(0) to release WaitCommEvent",
                                          GetLastError());
                } else {
                    diag_print_ok("SetCommMask(0) to release WaitCommEvent");
                    waitRc = WaitForSingleObject(hEvent, DIAG_SHORT_WAIT_MS);
                    if (waitRc == WAIT_OBJECT_0) {
                        bytesDone = 0;
                        ok = GetOverlappedResult(hPort, &ov, &bytesDone, FALSE);
                        if (!ok) {
                            diag_print_last_error("GetOverlappedResult(WaitCommEvent)",
                                                  GetLastError());
                        } else {
                            diag_printf("[ OK ] GetOverlappedResult(WaitCommEvent) "
                                        "bytes=%lu mask=0x%08lX\r\n",
                                        (unsigned long)bytesDone,
                                        (unsigned long)waitMask);
                        }
                    } else if (waitRc == WAIT_TIMEOUT) {
                        diag_print_info("WaitCommEvent stayed pending after SetCommMask(0)");
                    } else {
                        diag_print_last_error("WaitForSingleObject(WaitCommEvent)",
                                              GetLastError());
                    }
                }
            } else {
                diag_print_last_error("WaitCommEvent", error);
            }
        }

        diag_probe_overlapped_write(hPort);
        diag_probe_overlapped_read(hPort);
        diag_probe_hyperterminal_concurrent_io(hPort);
        diag_close_handle_named(hEvent, "CloseHandle(event)");
    }

    diag_close_handle_named(hPort, "CloseHandle(overlapped port)");

    diag_write_raw("\r\n");
}

int main(int argc, char **argv)
{
    const char *portBase;
    const char *capturePath;
    char portWithColon[32];
    char devicePath[40];
    char devicePathWithColon[40];

    portBase = "COM3";
    capturePath = "\\\\.\\COM3";
    if (argc >= 2 && argv[1] != NULL && argv[1][0] != '\0') {
        portBase = argv[1];
    }
    if (argc >= 3 && argv[2] != NULL && argv[2][0] != '\0') {
        capturePath = argv[2];
    }

    diag_write_raw("98Modem Win98 Diagnostic\r\n");
    diag_write_raw("========================\r\n\r\n");

    if (argc >= 2 && diag_arg_equals(argv[1], "capture-start")) {
        diag_hook_capture_control("Hook capture start (\\\\.\\VMODEM.VXD)",
                                  VMODEM_HOOK_CAPTURE_ACTION_START);
        diag_write_raw("Done.\r\n");
        return 0;
    }

    if (argc >= 2 && diag_arg_equals(argv[1], "capture-stop")) {
        diag_hook_capture_control("Hook capture stop (\\\\.\\VMODEM.VXD)",
                                  VMODEM_HOOK_CAPTURE_ACTION_STOP);
        diag_write_raw("Done.\r\n");
        return 0;
    }

    if (argc >= 2 && diag_arg_equals(argv[1], "capture-reset")) {
        diag_hook_capture_control("Hook capture reset (\\\\.\\VMODEM.VXD)",
                                  VMODEM_HOOK_CAPTURE_ACTION_RESET);
        diag_write_raw("Done.\r\n");
        return 0;
    }

    if (argc >= 2 && diag_arg_equals(argv[1], "capture-open")) {
        diag_capture_open_probe("Hook capture around CreateFileA",
                                capturePath,
                                0,
                                "CreateFileA");
        diag_write_raw("Done.\r\n");
        return 0;
    }

    if (argc >= 2 && diag_arg_equals(argv[1], "capture-open-ov")) {
        diag_capture_open_probe("Hook capture around CreateFileA(FILE_FLAG_OVERLAPPED)",
                                capturePath,
                                FILE_FLAG_OVERLAPPED,
                                "CreateFileA(FILE_FLAG_OVERLAPPED)");
        diag_write_raw("Done.\r\n");
        return 0;
    }

    diag_control_path_probe();
    diag_registry_serialcomm_probe(portBase);

    diag_port_probe_one(portBase);
    diag_hyperterminal_style_probe_one(portBase);

    diag_build_device_path(portBase, devicePath, sizeof(devicePath), FALSE);
    if (devicePath[0] != '\0') {
        diag_port_probe_one(devicePath);
        diag_hyperterminal_style_probe_one(devicePath);
    }

    diag_build_device_path(portBase,
                           devicePathWithColon,
                           sizeof(devicePathWithColon),
                           TRUE);
    if (devicePathWithColon[0] != '\0') {
        diag_port_probe_one(devicePathWithColon);
        diag_hyperterminal_style_probe_one(devicePathWithColon);
    }

    if (lstrlenA(portBase) > 0 && portBase[lstrlenA(portBase) - 1] != ':') {
        lstrcpynA(portWithColon, portBase, sizeof(portWithColon));
        lstrcatA(portWithColon, ":");
        diag_port_probe_one(portWithColon);
        diag_hyperterminal_style_probe_one(portWithColon);
    }

    diag_write_raw("Done.\r\n");
    return 0;
}
