/*
 * serial_port.c - VCOMM-backed virtual COM port callbacks plus the Milestone 3
 *                 modem/helper bridge.
 *
 * This module keeps the Win98/VCOMM-facing surface stable while layering in a
 * shared modem core that produces helper actions and consumes helper events.
 * The real IPC transport still lives on the VMODEM control device; this file
 * owns the serial-facing state and the VxD-side helper/session bookkeeping.
 */

#define WANTVXDWRAPS

#include <basedef.h>
#include <vmm.h>
#include <debug.h>
#include <vcomm.h>
#include <vpicd.h>
#include <vxdwraps.h>
#include <ifs.h>

#include "trace.h"
#include "ipc_shared.h"
#include "modem_core.h"
#include "dos_uart.h"

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG

#define VM_QUEUE_SIZE           4096UL
#define VM_MODEM_DRAIN_CHUNK     128U
#define VM_IPC_QUEUE_LEN          32U
#define VM_PORT_SIGNATURE       0x534D5446UL
#define VM_DEFAULT_PORT_NAME    "COM3"
#define VM_DEFAULT_BAUD         9600UL
#define VM_DEFAULT_MODEM_STATUS (MS_CTS_ON | MS_DSR_ON)
#define VM_COMMCONFIG_VERSION   0x0100
#define VM_DRIVER_BUILD_ID      0x20260403UL
#define VM_DRIVER_BUILD_TAG     "m11-dos-uart-20260403"
#define VM_TRACE_CMD_BUFFER_LEN 160
#define VM_FULL_ACTION_MASK     (fBaudRate | fBitMask | fXonLim | fXoffLim | \
                                 fByteSize | fbParity | fStopBits | \
                                 fXonChar | fXoffChar | fErrorChar | \
                                 fEofChar | fEvtChar1 | fEvtChar2 | \
                                 fTimeout | fTxDelay)

/*
 * PortGetCommConfig/PortSetCommConfig use VCOMM's _COMM_CONFIG layout from
 * VCOMMW32, not the WINBASE.H COMMCONFIG layout. The DDK serial sample calls
 * VCOMM's map helpers to translate between this Win32-style DCB and ring-0
 * _DCB.
 */
#pragma pack(push, 4)
typedef struct VM_WIN32_DCB {
    DWORD DCBlength;
    DWORD BaudRate;
    DWORD W32BitMask;
    WORD  W32Reserved;
    WORD  W32XonLim;
    WORD  W32XoffLim;
    BYTE  W32ByteSize;
    BYTE  W32Parity;
    BYTE  W32StopBits;
    char  W32XonChar;
    char  W32XoffChar;
    char  W32ErrorChar;
    char  W32EofChar;
    char  W32EvtChar;
    WORD  W32PackWORD;
} VM_WIN32_DCB;

typedef struct VM_COMMCONFIG {
    DWORD        dwSize;
    WORD         wVersion;
    WORD         wAlignDCB;
    VM_WIN32_DCB dcb;
    DWORD        dwProviderSubType;
    DWORD        dwProviderOffset;
    DWORD        dwProviderSize;
    BYTE         wcProviderData[1];
} VM_COMMCONFIG;
#pragma pack(pop)

#define DTR_CONTROL_DISABLE   0U
#define DTR_CONTROL_ENABLE    1U
#define DTR_CONTROL_HANDSHAKE 2U

#define RTS_CONTROL_DISABLE   0U
#define RTS_CONTROL_ENABLE    1U
#define RTS_CONTROL_HANDSHAKE 2U
#define RTS_CONTROL_TOGGLE    3U

#define PURGE_TXABORT         0x0001UL
#define PURGE_RXABORT         0x0002UL
#define PURGE_TXCLEAR         0x0004UL
#define PURGE_RXCLEAR         0x0008UL

#ifndef CE_RXOVER
#define CE_RXOVER             0x0001UL
#endif

typedef void (__cdecl *VM_NOTIFY_CALLBACK)(DWORD hPort,
                                           DWORD refData,
                                           DWORD event,
                                           DWORD subEvent);
typedef void (__cdecl *VM_RW_CALLBACK)(DWORD hPort, DWORD refData);
typedef void (__cdecl *VM_ANY_NOTIFY_CALLBACK)(DWORD hPort,
                                               DWORD refData,
                                               DWORD event,
                                               DWORD subEvent);
typedef DWORD (__cdecl *VM_CONTEND_NOTIFY_PROC)(DWORD refData, DWORD code);

typedef struct VModemPort {
    PortData pd;
    _DCB     dcb;
    VM_MODEM_CORE modem;
    BOOL     bOpen;
    DWORD    dwVMId;
    DWORD    dwDCRefData;
    DWORD    dwDevNode;
    DWORD    dwAllocBase;
    DWORD    dwAllocIrq;
    char     szPortName[16];
    DWORD    dwModemStatus;
    DWORD    dwContentionHandler;
    DWORD    dwContentionResource;
    DWORD    dwContentionOwnerNotify;
    DWORD    dwContentionOwnerRefData;
    DWORD    dwContentionAltNotify;
    DWORD    dwContentionAltRefData;
    BYTE    *lpMSRShadow;
    DWORD   *lpEventMaskLoc;
    DWORD    dwEvtMask;
    DWORD    dwRxTrigger;
    VM_RW_CALLBACK lpRxCallback;
    DWORD    dwRxRefData;
    DWORD    dwTxTrigger;
    VM_RW_CALLBACK lpTxCallback;
    DWORD    dwTxRefData;
    VM_NOTIFY_CALLBACK lpNotifyCallback;
    DWORD    dwNotifyRefData;
    BOOL     bDtrAsserted;
    BOOL     bRtsAsserted;
    BOOL     bRxNotifyArmed;
    BOOL     bRxNotifyPending;
    BOOL     bTxNotifyArmed;
    BOOL     bHoldHelperRxUntilRead;
    DWORD    dwHeldHelperSessionId;
    DWORD    dwHeldHelperRxCount;
    DWORD    dwHeldHelperRxGet;
    DWORD    dwHeldHelperRxPut;
    BYTE     heldHelperRx[VM_QUEUE_SIZE];
} VModemPort;

typedef struct VM_IpcQueue {
    VMODEM_PROTOCOL_MESSAGE entries[VM_IPC_QUEUE_LEN];
    unsigned short          get;
    unsigned short          put;
    unsigned short          count;
} VM_IpcQueue;

typedef struct VModemDosFrontend {
    VM_DOS_UART_STATE uart;
    VID               irqDesc;
    HIRQ              hIrq;
    DWORD             dwOwnerVmId;
    BOOL              bOwnerActive;
    BOOL              bTrapsInstalled;
    BOOL              bIrqRequested;
} VModemDosFrontend;

extern PortFunctions VM_PortFunctions;
extern BOOL __cdecl VM_VCOMM_Get_Version(void);
extern BOOL __cdecl VM_VCOMM_Register_Port_Driver(PFN pDriverControl);
extern BOOL __cdecl VM_VCOMM_Add_Port(DWORD DCRefData,
                                      PFN pPortOpen,
                                      char *pPortName);
extern PFN __cdecl VM_VCOMM_Get_Contention_Handler(char *pPortName);
extern DWORD __cdecl VM_VCOMM_Map_Name_To_Resource(char *pPortName);
extern void __cdecl VM_VCOMM_Map_Ring0DCB_To_Win32(_DCB *pRing0,
                                                   VM_WIN32_DCB *pWin32);
extern void __cdecl VM_VCOMM_Map_Win32DCB_To_Ring0(VM_WIN32_DCB *pWin32,
                                                   _DCB *pRing0);
extern void __cdecl VM_DriverControl_Thunk(DWORD fCode,
                                           DWORD DevNode,
                                           DWORD DCRefData,
                                           DWORD AllocBase,
                                           DWORD AllocIrq,
                                           char *PortName);
extern DWORD __cdecl VM_PortOpen_Thunk(char *PortName,
                                       DWORD VMId,
                                       DWORD *lpError);
/* Returns ppIFSFileHookFunc (EAX) — a DWORD* pointing to the chain slot.
 * The chain slot holds the previous hook's address; dereference to chain through. */
extern DWORD * __cdecl VM_IFSMgr_InstallFileSystemApiHook(PFN pNewHook);
extern void    __cdecl VM_IFSMgr_RemoveFileSystemApiHook(PFN pHookProc);
extern DWORD __cdecl VM_Get_System_Time(void);
extern BOOL __cdecl DOSUART_InstallIoHandler(DWORD port, DWORD handler);
extern BOOL __cdecl DOSUART_RemoveIoHandler(DWORD port, DWORD handler);
extern void __cdecl DOSUART_EnableGlobalTrapping(DWORD port);
extern void __cdecl DOSUART_DisableGlobalTrapping(DWORD port);
extern void __cdecl DOSUART_SetIntRequest(DWORD hIrq, DWORD vmHandle);
extern void __cdecl DOSUART_ClearIntRequest(DWORD hIrq);
extern void __cdecl DOSUART_IoTrapThunk(void);
extern void __cdecl DOSUART_HwIrqThunk(void);
extern void __cdecl DOSUART_BeginFastPeriod(DWORD periodMs);
extern void __cdecl DOSUART_EndFastPeriod(DWORD periodMs);

static void vm_update_msr_shadow(VModemPort *port);
static void vm_cancel_modem_timer(void);
static void __cdecl vm_modem_timer_callback(void);
static void vm_refresh_modem_timer(VModemPort *port);
static void vm_cancel_dos_uart_timer(void);
static void __cdecl vm_dos_uart_timer_callback(void);
static void vm_refresh_dos_uart_timer(void);
static void vm_begin_dos_fast_period(void);
static void vm_end_dos_fast_period(void);
static void vm_poll_modem_core(VModemPort *port);
static void vm_sync_from_modem_core(VModemPort *port);
static void vm_flush_rx_notify(VModemPort *port);
static void vm_buf_append_char(char *buf, int *pPos, int cchBuf, char ch);
static void vm_buf_append_str(char *buf, int *pPos, int cchBuf, const char *str);
static void vm_buf_append_u32_dec(char *buf, int *pPos, int cchBuf, DWORD value);
static void vm_buf_append_u32_hex(char *buf, int *pPos, int cchBuf, DWORD value);
static void vm_trace_log_line(const char *text);
static void vm_trace_log_command_line(void);
static void vm_trace_capture_tx_for_log(const BYTE *bytes, DWORD count);
static void vm_trace_log_modem_tx(const BYTE *bytes, DWORD count);
static void vm_trace_log_local_serial_rx(const BYTE *bytes, DWORD count);
static DWORD vm_enqueue_rx_bytes(VModemPort *port, const BYTE *bytes, DWORD count);
static void vm_reset_held_helper_rx(VModemPort *port);
static DWORD vm_queue_held_helper_rx(VModemPort *port,
                                     DWORD sessionId,
                                     const BYTE *bytes,
                                     DWORD count);
static void vm_reconcile_held_helper_rx(VModemPort *port);
static void vm_flush_held_helper_rx(VModemPort *port);
static DWORD vm_query_frontend_owner(void);
static BOOL vm_dos_owner_active(void);
static BOOL vm_frontend_modem_active(void);
static BOOL vm_dos_map_port_identity(const char *portName,
                                     DWORD *pBasePort,
                                     DWORD *pIrqNumber);
static void vm_dos_trace_register_access(const char *prefix,
                                         DWORD vmId,
                                         DWORD port,
                                         DWORD offset,
                                         DWORD value);
static void vm_dos_trace_irq_state(const char *prefix);
static void vm_dos_update_irq_request(void);
static void vm_dos_apply_modem_status(void);
static DWORD vm_enqueue_dos_rx_bytes(const BYTE *bytes, DWORD count);
static BOOL vm_dos_claim_owner(DWORD vmId);
static void vm_dos_release_owner(const char *reason);
static void vm_dos_initialize(VModemPort *port);
static void vm_dos_cleanup(void);
static void vm_dos_handle_vm_destroyed(DWORD vmId);

static VModemPort g_Port;
static BYTE g_RxBuf[VM_QUEUE_SIZE];
static BYTE g_TxBuf[VM_QUEUE_SIZE];
static BOOL g_bVcommRegistered = FALSE;
static const char g_BuildTag[VMODEM_BUILD_TAG_LEN] = VM_DRIVER_BUILD_TAG;
static VM_IpcQueue g_ToHelperQueue;
static VModemDosFrontend g_DosUart;
static BOOL  g_bHelperClaimed = FALSE;
static DWORD g_helperOwnerHandle = 0;
static DWORD g_helperGeneration = 0;
static DWORD g_lastMsgToHelper = VMODEM_MSG_NONE;
static DWORD g_lastMsgFromHelper = VMODEM_MSG_NONE;
static HTIMEOUT g_hModemTimer = 0;
static DWORD g_modemTimerDue = 0;
static HTIMEOUT g_hDosUartTimer = 0;
static BOOL g_bDosUartFastPeriod = FALSE;

/* IFS hook state.
 * g_ppPrevIfsHook = ppIFSFileHookFunc returned by IFSMgr_InstallFileSystemApiHook.
 * It is a DWORD* pointing into IFSMGR's chain slot; *g_ppPrevIfsHook is the
 * previous hook function to call through to on every invocation. */
static BOOL   g_bIfsHookInstalled = FALSE;
static DWORD *g_ppPrevIfsHook     = 0;
static DWORD  g_ifsHookFireCount  = 0;

/* PortOpen name log: names appended as strings separated by '|'. */
static DWORD g_portOpenCount  = 0;
static DWORD g_portOpenLogLen = 0;
static char  g_portOpenLog[VMODEM_HOOK_LOG_DATA_LEN];

/* Resettable capture window used to isolate one external reproduction attempt
 * from unrelated background IFS activity. */
static BOOL  g_bHookCaptureEnabled = FALSE;
static DWORD g_hookCaptureGeneration = 0;
static DWORD g_hookCaptureFireCount = 0;
static DWORD g_hookCapturePortOpenCount = 0;
static DWORD g_hookCaptureOtherFnCount = 0;
static DWORD g_hookCaptureFnCounts[VMODEM_HOOK_FN_BUCKETS];
static DWORD g_hookCaptureLogLen = 0;
static char  g_hookCaptureLog[VMODEM_HOOK_LOG_DATA_LEN];
static DWORD g_traceLogLen = 0;
static DWORD g_traceLogDropped = 0;
static char  g_traceLog[VMODEM_TRACE_LOG_DATA_LEN];
static unsigned short g_traceCmdLen = 0;
static BOOL  g_traceCmdOverflowed = FALSE;
static char  g_traceCmdBuffer[VM_TRACE_CMD_BUFFER_LEN];
static DWORD g_dosReadSuppressOffset = 0xFFFFFFFFUL;
static DWORD g_dosReadSuppressValue  = 0xFFFFFFFFUL;
static DWORD g_dosReadSuppressCount  = 0;

BOOL __cdecl VM_PortClose(DWORD hPort);
BOOL __cdecl VM_PortSetCommState(DWORD hPort, _DCB *pDcb, DWORD ActionMask);
DWORD __cdecl VM_VxD_Contention_Handler(DWORD functionCode,
                                        DWORD arg1,
                                        DWORD arg2,
                                        DWORD arg3,
                                        DWORD arg4);

static void vm_reset_contention_ownership(VModemPort *port)
{
    if (port == 0) {
        return;
    }

    port->dwContentionOwnerNotify = 0;
    port->dwContentionOwnerRefData = 0;
    port->dwContentionAltNotify = 0;
    port->dwContentionAltRefData = 0;
}

static VModemPort *vm_port_from_contention_resource(DWORD resource)
{
    if (resource == 0) {
        return 0;
    }

    if (resource != (DWORD)&g_Port) {
        return 0;
    }

    if (g_Port.szPortName[0] == '\0') {
        return 0;
    }

    return &g_Port;
}

static void vm_zero_bytes(void *dst, DWORD count)
{
    BYTE *d;
    DWORD i;

    if (dst == 0 || count == 0) {
        return;
    }

    d = (BYTE *)dst;
    for (i = 0; i < count; ++i) {
        d[i] = 0;
    }
}

static void vm_copy_bytes(void *dst, const void *src, DWORD count)
{
    BYTE *d;
    const BYTE *s;
    DWORD i;

    if (dst == 0 || src == 0 || count == 0) {
        return;
    }

    d = (BYTE *)dst;
    s = (const BYTE *)src;
    for (i = 0; i < count; ++i) {
        d[i] = s[i];
    }
}

static void vm_copy_text(char *dst, const char *src, DWORD cchDst)
{
    DWORD i;

    if (dst == 0 || cchDst == 0) {
        return;
    }

    if (src == 0) {
        dst[0] = '\0';
        return;
    }

    for (i = 0; i + 1 < cchDst && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static char vm_fold_ascii(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (char)(ch - ('a' - 'A'));
    }
    return ch;
}

static const char *vm_skip_port_prefix(const char *name)
{
    if (name == 0) {
        return 0;
    }

    if (name[0] == '\\' &&
        name[1] == '\\' &&
        name[2] == '.' &&
        name[3] == '\\') {
        return name + 4;
    }

    return name;
}

static BOOL vm_port_name_matches(const char *lhs, const char *rhs)
{
    DWORD i;
    char a;
    char b;

    lhs = vm_skip_port_prefix(lhs);
    rhs = vm_skip_port_prefix(rhs);

    if (lhs == 0 || rhs == 0) {
        return FALSE;
    }

    for (i = 0;; ++i) {
        a = vm_fold_ascii(lhs[i]);
        b = vm_fold_ascii(rhs[i]);
        if (a == ':') {
            a = '\0';
        }
        if (b == ':') {
            b = '\0';
        }
        if (a != b) {
            return FALSE;
        }
        if (a == '\0') {
            return TRUE;
        }
    }
}

static DWORD vm_query_frontend_owner(void)
{
    if (g_Port.bOpen) {
        return VMODEM_FRONTEND_OWNER_VCOMM;
    }

    if (g_DosUart.bOwnerActive) {
        return VMODEM_FRONTEND_OWNER_DOS;
    }

    return VMODEM_FRONTEND_OWNER_NONE;
}

static BOOL vm_dos_owner_active(void)
{
    return g_DosUart.bOwnerActive ? TRUE : FALSE;
}

static BOOL vm_frontend_modem_active(void)
{
    return (g_Port.bOpen || g_DosUart.bOwnerActive) ? TRUE : FALSE;
}

static BOOL vm_dos_map_port_identity(const char *portName,
                                     DWORD *pBasePort,
                                     DWORD *pIrqNumber)
{
    const char *name;

    if (pBasePort == 0 || pIrqNumber == 0) {
        return FALSE;
    }

    *pBasePort = 0;
    *pIrqNumber = 0;
    name = vm_skip_port_prefix(portName);
    if (name == 0) {
        return FALSE;
    }

    if (vm_port_name_matches(name, "COM1")) {
        *pBasePort = 0x3F8UL;
        *pIrqNumber = 4UL;
        return TRUE;
    }
    if (vm_port_name_matches(name, "COM2")) {
        *pBasePort = 0x2F8UL;
        *pIrqNumber = 3UL;
        return TRUE;
    }
    if (vm_port_name_matches(name, "COM3")) {
        *pBasePort = 0x3E8UL;
        *pIrqNumber = 4UL;
        return TRUE;
    }
    if (vm_port_name_matches(name, "COM4")) {
        *pBasePort = 0x2E8UL;
        *pIrqNumber = 3UL;
        return TRUE;
    }

    return FALSE;
}

static void vm_dos_trace_flush_read_suppress(void)
{
    char line[80];
    int pos;

    if (g_dosReadSuppressCount == 0) {
        return;
    }

    if (g_dosReadSuppressCount > 1) {
        pos = 0;
        line[0] = '\0';
        vm_buf_append_str(line, &pos, sizeof(line), "DOSUART_READ_x");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), g_dosReadSuppressCount);
        vm_buf_append_str(line, &pos, sizeof(line), " off=");
        vm_buf_append_u32_hex(line, &pos, sizeof(line), g_dosReadSuppressOffset);
        vm_buf_append_str(line, &pos, sizeof(line), " value=");
        vm_buf_append_u32_hex(line, &pos, sizeof(line), g_dosReadSuppressValue);
        vm_trace_log_line(line);
    }

    g_dosReadSuppressOffset = 0xFFFFFFFFUL;
    g_dosReadSuppressValue  = 0xFFFFFFFFUL;
    g_dosReadSuppressCount  = 0;
}

static void vm_dos_trace_register_access(const char *prefix,
                                         DWORD vmId,
                                         DWORD port,
                                         DWORD offset,
                                         DWORD value)
{
    char line[160];
    int pos;
    int is_read;

    is_read = (prefix[7] == 'R' && prefix[8] == 'E' &&
               prefix[9] == 'A' && prefix[10] == 'D' &&
               prefix[11] == '\0');

    if (is_read) {
        if (offset == g_dosReadSuppressOffset &&
            value  == g_dosReadSuppressValue) {
            ++g_dosReadSuppressCount;
            return;
        }
        vm_dos_trace_flush_read_suppress();
        g_dosReadSuppressOffset = offset;
        g_dosReadSuppressValue  = value;
        g_dosReadSuppressCount  = 1;
        /* fall through to log the first occurrence */
    } else {
        vm_dos_trace_flush_read_suppress();
    }

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), prefix);
    vm_buf_append_str(line, &pos, sizeof(line), " vmid=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), vmId);
    vm_buf_append_str(line, &pos, sizeof(line), " port=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), port);
    vm_buf_append_str(line, &pos, sizeof(line), " off=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), offset);
    vm_buf_append_str(line, &pos, sizeof(line), " value=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), value);
    vm_trace_log_line(line);
}

static void vm_dos_trace_irq_state(const char *prefix)
{
    char line[160];
    int pos;

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), prefix);
    vm_buf_append_str(line, &pos, sizeof(line), " vmid=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), g_DosUart.dwOwnerVmId);
    vm_buf_append_str(line, &pos, sizeof(line), " pending=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line),
                          vm_dos_uart_get_pending_irq(&g_DosUart.uart));
    vm_buf_append_str(line, &pos, sizeof(line), " asserted=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line),
                          vm_dos_uart_get_irq_asserted(&g_DosUart.uart) ? 1UL : 0UL);
    vm_trace_log_line(line);
}

static void vm_dos_update_irq_request(void)
{
    if (g_DosUart.hIrq == 0) {
        g_DosUart.bIrqRequested = FALSE;
        return;
    }

    if (!g_DosUart.bOwnerActive ||
        !vm_dos_uart_get_irq_asserted(&g_DosUart.uart)) {
        if (g_DosUart.bIrqRequested) {
            DOSUART_ClearIntRequest((DWORD)g_DosUart.hIrq);
            g_DosUart.bIrqRequested = FALSE;
            vm_dos_trace_irq_state("DOSUART_IRQ_CLEAR");
        }
        return;
    }

    if (!g_DosUart.bIrqRequested) {
        DOSUART_SetIntRequest((DWORD)g_DosUart.hIrq, g_DosUart.dwOwnerVmId);
        g_DosUart.bIrqRequested = TRUE;
        vm_dos_trace_irq_state("DOSUART_IRQ_ASSERT");
    }
}

static void vm_dos_apply_modem_status(void)
{
    VM_DOS_UART_EVENT event;

    if (!g_DosUart.bOwnerActive) {
        return;
    }

    vm_dos_uart_apply_modem_status(&g_DosUart.uart,
                                   (DWORD)vm_modem_get_status(&g_Port.modem),
                                   0,
                                   &event);
    if ((event.flags & VM_DOS_UART_EVENT_MODEM_CHANGED) != 0UL) {
        char line[160];
        int pos;

        pos = 0;
        line[0] = '\0';
        vm_buf_append_str(line, &pos, sizeof(line), "DOSUART_MSR oldvmid=");
        vm_buf_append_u32_hex(line, &pos, sizeof(line), g_DosUart.dwOwnerVmId);
        vm_buf_append_str(line, &pos, sizeof(line), " msr=");
        vm_buf_append_u32_hex(line, &pos, sizeof(line), g_DosUart.uart.msr);
        vm_trace_log_line(line);
    }
    vm_dos_update_irq_request();
}

static DWORD vm_enqueue_dos_rx_bytes(const BYTE *bytes, DWORD count)
{
    DWORD written;
    unsigned short chunk;
    VM_DOS_UART_EVENT event;
    char line[128];
    int pos;

    if (bytes == 0 || count == 0 || !g_DosUart.bOwnerActive) {
        return 0;
    }

    written = 0;
    while (written < count) {
        if ((count - written) > 0xFFFFUL) {
            chunk = 0xFFFFU;
        } else {
            chunk = (unsigned short)(count - written);
        }

        chunk = vm_dos_uart_enqueue_rx(&g_DosUart.uart,
                                       bytes + written,
                                       chunk,
                                       &event);
        if (chunk == 0U) {
            break;
        }

        written += chunk;
        pos = 0;
        line[0] = '\0';
        vm_buf_append_str(line, &pos, sizeof(line), "DOSUART_RX_ENQUEUE len=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), chunk);
        vm_buf_append_str(line, &pos, sizeof(line), " depth=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line),
                              vm_dos_uart_get_rx_depth(&g_DosUart.uart));
        vm_trace_log_line(line);
    }

    vm_dos_update_irq_request();
    vm_refresh_dos_uart_timer();
    return written;
}

static BOOL vm_dos_claim_owner(DWORD vmId)
{
    DWORD now;
    char line[160];
    int pos;

    if (!g_DosUart.uart.enabled ||
        vmId == 0 ||
        Test_Sys_VM_Handle((HVM)vmId) ||
        g_Port.bOpen) {
        return FALSE;
    }

    if (g_DosUart.bOwnerActive) {
        return (g_DosUart.dwOwnerVmId == vmId) ? TRUE : FALSE;
    }

    g_DosUart.bOwnerActive = TRUE;
    g_DosUart.dwOwnerVmId = vmId;
    g_DosUart.bIrqRequested = FALSE;
    vm_dos_uart_reset(&g_DosUart.uart);

    now = VM_Get_System_Time();
    vm_modem_port_open(&g_Port.modem, now);
    vm_modem_set_host_lines(&g_Port.modem,
                            vm_dos_uart_get_dtr(&g_DosUart.uart),
                            vm_dos_uart_get_rts(&g_DosUart.uart),
                            now);
    vm_dos_uart_apply_modem_status(&g_DosUart.uart,
                                   (DWORD)vm_modem_get_status(&g_Port.modem),
                                   1,
                                   0);
    vm_sync_from_modem_core(&g_Port);

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "DOSUART_OWNER_ACQUIRE vmid=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), vmId);
    vm_buf_append_str(line, &pos, sizeof(line), " base=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), g_DosUart.uart.base_port);
    vm_buf_append_str(line, &pos, sizeof(line), " irq=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), g_DosUart.uart.irq_number);
    vm_trace_log_line(line);
    return TRUE;
}

static void vm_dos_release_owner(const char *reason)
{
    DWORD oldVmId;
    char line[192];
    int pos;

    if (!g_DosUart.bOwnerActive) {
        return;
    }

    oldVmId = g_DosUart.dwOwnerVmId;
    if (g_DosUart.bIrqRequested && g_DosUart.hIrq != 0) {
        DOSUART_ClearIntRequest((DWORD)g_DosUart.hIrq);
        g_DosUart.bIrqRequested = FALSE;
    }

    g_DosUart.bOwnerActive = FALSE;
    g_DosUart.dwOwnerVmId = 0;
    vm_dos_uart_reset(&g_DosUart.uart);

    if (!g_Port.bOpen) {
        vm_reset_held_helper_rx(&g_Port);
        vm_modem_port_close(&g_Port.modem);
        g_Port.dwModemStatus = (DWORD)vm_modem_get_status(&g_Port.modem);
        vm_update_msr_shadow(&g_Port);
        if (!vm_frontend_modem_active()) {
            vm_cancel_modem_timer();
        }
    }
    vm_cancel_dos_uart_timer();

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "DOSUART_OWNER_RELEASE vmid=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), oldVmId);
    if (reason != 0 && reason[0] != '\0') {
        vm_buf_append_str(line, &pos, sizeof(line), " reason=");
        vm_buf_append_str(line, &pos, sizeof(line), reason);
    }
    vm_trace_log_line(line);
}

static void vm_dos_initialize(VModemPort *port)
{
    DWORD basePort;
    DWORD irqNumber;
    DWORD portIndex;
    BOOL ok;

    vm_zero_bytes(&g_DosUart, sizeof(g_DosUart));
    vm_dos_uart_init(&g_DosUart.uart);
    if (port == 0) {
        return;
    }

    if (!vm_dos_map_port_identity(port->szPortName, &basePort, &irqNumber)) {
        vm_trace_log_line("DOSUART_DISABLED unsupported_port_identity");
        return;
    }

    vm_dos_uart_configure(&g_DosUart.uart, basePort, irqNumber, 1);

    vm_zero_bytes(&g_DosUart.irqDesc, sizeof(g_DosUart.irqDesc));
    g_DosUart.irqDesc.VID_IRQ_Number = (USHORT)irqNumber;
    g_DosUart.irqDesc.VID_Options = VPICD_OPT_CAN_SHARE;
    g_DosUart.irqDesc.VID_Hw_Int_Proc = (ULONG)(DWORD)(PFN)DOSUART_HwIrqThunk;
    g_DosUart.irqDesc.VID_IRET_Time_Out = 500UL;
    g_DosUart.hIrq = VPICD_Virtualize_IRQ(&g_DosUart.irqDesc);
    if (g_DosUart.hIrq == 0) {
        vm_trace_log_line("DOSUART_IRQ_VIRTUALIZE failed");
    }

    ok = TRUE;
    for (portIndex = 0; portIndex < 8UL; ++portIndex) {
        if (!DOSUART_InstallIoHandler(basePort + portIndex,
                                      (DWORD)(PFN)DOSUART_IoTrapThunk)) {
            ok = FALSE;
            break;
        }
        DOSUART_EnableGlobalTrapping(basePort + portIndex);
    }

    if (!ok) {
        while (portIndex != 0UL) {
            --portIndex;
            DOSUART_DisableGlobalTrapping(basePort + portIndex);
            DOSUART_RemoveIoHandler(basePort + portIndex,
                                    (DWORD)(PFN)DOSUART_IoTrapThunk);
        }
        g_DosUart.uart.enabled = 0;
        vm_trace_log_line("DOSUART_DISABLED trap_install_failed");
        return;
    }

    g_DosUart.bTrapsInstalled = TRUE;
}

static void vm_dos_cleanup(void)
{
    DWORD portIndex;
    DWORD basePort;

    basePort = g_DosUart.uart.base_port;
    vm_dos_release_owner("cleanup");

    if (g_DosUart.bTrapsInstalled && basePort != 0UL) {
        for (portIndex = 0; portIndex < 8UL; ++portIndex) {
            DOSUART_DisableGlobalTrapping(basePort + portIndex);
            DOSUART_RemoveIoHandler(basePort + portIndex,
                                    (DWORD)(PFN)DOSUART_IoTrapThunk);
        }
    }

    g_DosUart.bTrapsInstalled = FALSE;
    g_DosUart.hIrq = 0;
    vm_zero_bytes(&g_DosUart.irqDesc, sizeof(g_DosUart.irqDesc));
}

static void vm_dos_handle_vm_destroyed(DWORD vmId)
{
    if (g_DosUart.bOwnerActive && g_DosUart.dwOwnerVmId == vmId) {
        vm_dos_release_owner("vm_exit");
    }
}

static void vm_ipc_queue_reset(VM_IpcQueue *queue)
{
    if (queue == 0) {
        return;
    }

    vm_zero_bytes(queue, sizeof(*queue));
}

static BOOL vm_ipc_queue_push(VM_IpcQueue *queue,
                              const VMODEM_PROTOCOL_MESSAGE *message)
{
    if (queue == 0 || message == 0) {
        return FALSE;
    }

    if (queue->count >= VM_IPC_QUEUE_LEN) {
        return FALSE;
    }

    queue->entries[queue->put] = *message;
    ++queue->put;
    if (queue->put >= VM_IPC_QUEUE_LEN) {
        queue->put = 0;
    }
    ++queue->count;
    return TRUE;
}

static BOOL vm_ipc_queue_pop(VM_IpcQueue *queue,
                             VMODEM_PROTOCOL_MESSAGE *message)
{
    if (queue == 0 || queue->count == 0) {
        return FALSE;
    }

    if (message != 0) {
        *message = queue->entries[queue->get];
    }
    vm_zero_bytes(&queue->entries[queue->get], sizeof(queue->entries[queue->get]));
    ++queue->get;
    if (queue->get >= VM_IPC_QUEUE_LEN) {
        queue->get = 0;
    }
    --queue->count;
    return TRUE;
}

static void vm_trace_ipc_to_helper(DWORD messageType)
{
    switch (messageType) {
    case VMODEM_MSG_CONNECT_REQ:
        VTRACE("VMODEM: IPC -> helper CONNECT_REQ\r\n");
        break;
    case VMODEM_MSG_DATA_TO_NET:
        VTRACE("VMODEM: IPC -> helper DATA_TO_NET\r\n");
        break;
    case VMODEM_MSG_ANSWER_REQ:
        VTRACE("VMODEM: IPC -> helper ANSWER_REQ\r\n");
        break;
    case VMODEM_MSG_HANGUP_REQ:
        VTRACE("VMODEM: IPC -> helper HANGUP_REQ\r\n");
        break;
    default:
        VTRACE("VMODEM: IPC -> helper UNKNOWN\r\n");
        break;
    }
}

static void vm_trace_ipc_from_helper(DWORD messageType)
{
    switch (messageType) {
    case VMODEM_MSG_CONNECT_OK:
        VTRACE("VMODEM: IPC <- helper CONNECT_OK\r\n");
        break;
    case VMODEM_MSG_CONNECT_FAIL:
        VTRACE("VMODEM: IPC <- helper CONNECT_FAIL\r\n");
        break;
    case VMODEM_MSG_DATA_TO_SERIAL:
        VTRACE("VMODEM: IPC <- helper DATA_TO_SERIAL\r\n");
        break;
    case VMODEM_MSG_INBOUND_RING:
        VTRACE("VMODEM: IPC <- helper INBOUND_RING\r\n");
        break;
    case VMODEM_MSG_REMOTE_CLOSED:
        VTRACE("VMODEM: IPC <- helper REMOTE_CLOSED\r\n");
        break;
    default:
        VTRACE("VMODEM: IPC <- helper UNKNOWN\r\n");
        break;
    }
}

static BOOL vm_queue_message_to_helper(DWORD messageType,
                                       DWORD sessionId,
                                       DWORD status,
                                       const BYTE *payload,
                                       DWORD payloadLength)
{
    VMODEM_PROTOCOL_MESSAGE message;

    if (!g_bHelperClaimed) {
        VTRACE("VMODEM: IPC queue skipped, no helper claimed\r\n");
        return FALSE;
    }

    if (payloadLength > VMODEM_IPC_MAX_PAYLOAD) {
        VTRACE("VMODEM: IPC queue rejected oversized payload\r\n");
        return FALSE;
    }

    vm_zero_bytes(&message, sizeof(message));
    message.version = VMODEM_IPC_VERSION;
    message.message_type = messageType;
    message.helper_generation = g_helperGeneration;
    message.session_id = sessionId;
    message.status = status;
    message.payload_length = payloadLength;
    if (payload != 0 && payloadLength != 0) {
        vm_copy_bytes(message.payload, payload, payloadLength);
    }

    if (!vm_ipc_queue_push(&g_ToHelperQueue, &message)) {
        VTRACE("VMODEM: IPC queue full, deferring outbound helper message\r\n");
        return FALSE;
    }

    g_lastMsgToHelper = messageType;
    vm_trace_ipc_to_helper(messageType);
    return TRUE;
}

static void vm_sync_helper_availability_to_modem(void)
{
    if (g_Port.szPortName[0] == '\0') {
        return;
    }

    vm_modem_set_helper_available(&g_Port.modem, g_bHelperClaimed ? 1 : 0);
    g_Port.dwModemStatus = (DWORD)vm_modem_get_status(&g_Port.modem);
    vm_update_msr_shadow(&g_Port);
    vm_dos_apply_modem_status();

    if (vm_frontend_modem_active()) {
        vm_sync_from_modem_core(&g_Port);
        if (g_Port.bOpen) {
            vm_flush_rx_notify(&g_Port);
        }
    }
}

static DWORD vm_ring0_baud_to_user_baud(DWORD baud)
{
    switch (baud) {
    case CBR_110:
        return 110UL;
    case CBR_300:
        return 300UL;
    case CBR_600:
        return 600UL;
    case CBR_1200:
        return 1200UL;
    case CBR_2400:
        return 2400UL;
    case CBR_4800:
        return 4800UL;
    case CBR_9600:
        return 9600UL;
    case CBR_14400:
        return 14400UL;
    case CBR_19200:
        return 19200UL;
    case CBR_38400:
        return 38400UL;
    case CBR_56000:
        return 56000UL;
    case CBR_128000:
        return 128000UL;
    case CBR_256000:
        return 256000UL;
    default:
        return baud;
    }
}

static void vm_init_default_ring0_dcb(_DCB *pDcb)
{
    if (pDcb == 0) {
        return;
    }

    vm_zero_bytes(pDcb, sizeof(*pDcb));
    pDcb->DCBLength = sizeof(*pDcb);
    pDcb->BaudRate = VM_DEFAULT_BAUD;
    pDcb->BitMask = fBinary | fDtrEnable | fRTSEnable;
    pDcb->XonLim = VM_QUEUE_SIZE / 4;
    pDcb->XoffLim = VM_QUEUE_SIZE / 4;
    pDcb->ByteSize = 8;
    pDcb->Parity = NOPARITY;
    pDcb->StopBits = ONESTOPBIT;
    pDcb->XonChar = 0x11;
    pDcb->XoffChar = 0x13;
}

static void vm_sync_host_lines_from_dcb(VModemPort *port)
{
    DWORD bitMask;

    if (port == 0) {
        return;
    }

    bitMask = port->dcb.BitMask;
    port->bDtrAsserted = ((bitMask & fDTRDisable) == 0) ? TRUE : FALSE;
    port->bRtsAsserted = ((bitMask & fRTSDisable) == 0) ? TRUE : FALSE;
}

static void vm_reset_queue_state(PortData *pd)
{
    if (pd == 0) {
        return;
    }

    pd->QInCount = 0;
    pd->QInGet = 0;
    pd->QInPut = 0;
    pd->QOutCount = 0;
    pd->QOutGet = 0;
    pd->QOutPut = 0;
}

static void vm_use_internal_queues(VModemPort *port)
{
    if (port == 0) {
        return;
    }

    port->pd.QInAddr = (DWORD)g_RxBuf;
    port->pd.QInSize = VM_QUEUE_SIZE;
    port->pd.QOutAddr = (DWORD)g_TxBuf;
    port->pd.QOutSize = VM_QUEUE_SIZE;
    vm_reset_queue_state(&port->pd);
}

static void vm_reset_callbacks(VModemPort *port)
{
    if (port == 0) {
        return;
    }

    port->dwEvtMask = 0;
    port->dwRxTrigger = 0xFFFFFFFFUL;
    port->lpRxCallback = 0;
    port->dwRxRefData = 0;
    port->dwTxTrigger = 0;
    port->lpTxCallback = 0;
    port->dwTxRefData = 0;
    port->lpNotifyCallback = 0;
    port->dwNotifyRefData = 0;
    port->bRxNotifyArmed = FALSE;
    port->bRxNotifyPending = FALSE;
    port->bTxNotifyArmed = FALSE;
    port->pd.dwClientEventMask = 0;
    port->pd.lpClientEventNotify = 0;
    port->pd.lpClientReadNotify = 0;
    port->pd.lpClientWriteNotify = 0;
    port->pd.dwClientRefData = 0;
}

static void vm_update_msr_shadow(VModemPort *port)
{
    BYTE shadow;

    if (port == 0) {
        return;
    }

    shadow = (BYTE)(port->dwModemStatus & MS_Modem_Status);
    port->pd.bMSRShadow = shadow;

    if (port->lpMSRShadow != 0) {
        *(port->lpMSRShadow) = shadow;
    }
}

static void vm_invoke_client_callback(DWORD callback,
                                      DWORD hPort,
                                      DWORD refData,
                                      DWORD event,
                                      DWORD subEvent)
{
    if (callback == 0) {
        return;
    }

    /*
     * The stock Win9x serial driver pushes a 4-DWORD frame for all client
     * notifications, including read/write threshold callbacks. Clients that
     * only care about hPort/refData ignore the trailing values, while shared
     * handlers can still inspect CN_EVENT/CN_RECEIVE/CN_TRANSMIT.
     */
    ((VM_ANY_NOTIFY_CALLBACK)callback)(hPort, refData, event, subEvent);
}

static void vm_record_event(VModemPort *port, DWORD eventBits)
{
    DWORD enabledBits;
    char  line[128];
    int   pos;

    if (port == 0 || eventBits == 0) {
        return;
    }

    enabledBits = eventBits & port->dwEvtMask;
    if (enabledBits == 0) {
        return;
    }

    port->pd.dwDetectedEvents |= enabledBits;
    if (port->lpEventMaskLoc != 0) {
        *(port->lpEventMaskLoc) |= enabledBits;
    }

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "EVENT bits=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), enabledBits);
    vm_buf_append_str(line, &pos, sizeof(line), " detected=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), port->pd.dwDetectedEvents);
    vm_buf_append_str(line, &pos, sizeof(line), " mask=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), port->dwEvtMask);
    vm_trace_log_line(line);

    vm_invoke_client_callback((DWORD)port->lpNotifyCallback,
                              (DWORD)port,
                              port->dwNotifyRefData,
                              CN_EVENT,
                              port->pd.dwDetectedEvents);
}

static void vm_update_rx_notify_armed(VModemPort *port)
{
    if (port == 0) {
        return;
    }

    if (port->lpRxCallback == 0 || port->dwRxTrigger == 0xFFFFFFFFUL) {
        port->bRxNotifyArmed = FALSE;
        return;
    }

    port->bRxNotifyArmed = (port->pd.QInCount < port->dwRxTrigger) ? TRUE : FALSE;
}

static void vm_notify_rx_available(VModemPort *port)
{
    if (port == 0 || port->lpRxCallback == 0 || port->dwRxTrigger == 0xFFFFFFFFUL) {
        return;
    }

    /*
     * The stock serial driver also issues a timeout-based CN_RECEIVE wake for
     * short reads that never reach the requested trigger. HyperTerminal uses a
     * trigger of 80 bytes but still expects small modem result bursts to wake
     * its read side, so we treat "any data available" as a one-shot wake until
     * the queue drops below the trigger again.
     */
    if (port->bRxNotifyArmed && port->pd.QInCount != 0) {
        port->bRxNotifyArmed = FALSE;
        port->bRxNotifyPending = TRUE;
    }
}

static void vm_flush_rx_notify(VModemPort *port)
{
    char line[96];
    int  pos;

    if (port == 0 || !port->bRxNotifyPending || port->lpRxCallback == 0) {
        return;
    }

    port->bRxNotifyPending = FALSE;
    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "READ_CALLBACK qin=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), port->pd.QInCount);
    vm_buf_append_str(line, &pos, sizeof(line), " trigger=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), port->dwRxTrigger);
    vm_trace_log_line(line);
    vm_invoke_client_callback((DWORD)port->lpRxCallback,
                              (DWORD)port,
                              port->dwRxRefData,
                              CN_RECEIVE,
                              port->pd.dwDetectedEvents);
}

static void vm_update_tx_notify_armed(VModemPort *port)
{
    if (port == 0) {
        return;
    }

    if (port->lpTxCallback == 0 || port->dwTxTrigger == 0) {
        port->bTxNotifyArmed = FALSE;
        return;
    }

    if (port->pd.QOutCount >= port->dwTxTrigger) {
        port->bTxNotifyArmed = TRUE;
    }
}

static void vm_notify_tx_ready(VModemPort *port)
{
    char line[96];
    int  pos;

    if (port == 0 || port->lpTxCallback == 0 || port->dwTxTrigger == 0) {
        return;
    }

    if (port->bTxNotifyArmed && port->pd.QOutCount < port->dwTxTrigger) {
        port->bTxNotifyArmed = FALSE;
        pos = 0;
        line[0] = '\0';
        vm_buf_append_str(line, &pos, sizeof(line), "WRITE_CALLBACK qout=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), port->pd.QOutCount);
        vm_buf_append_str(line, &pos, sizeof(line), " trigger=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), port->dwTxTrigger);
        vm_trace_log_line(line);
        vm_invoke_client_callback((DWORD)port->lpTxCallback,
                                  (DWORD)port,
                                  port->dwTxRefData,
                                  CN_TRANSMIT,
                                  port->pd.dwDetectedEvents);
    }
}

static void vm_stage_tx_bytes(VModemPort *port, DWORD count)
{
    DWORD nextPut;

    if (port == 0 || count == 0) {
        return;
    }

    port->pd.QOutCount = count;
    if (port->pd.QOutSize != 0) {
        nextPut = port->pd.QOutPut + count;
        if (nextPut >= port->pd.QOutSize) {
            nextPut %= port->pd.QOutSize;
        }
        port->pd.QOutPut = nextPut;
    }

    vm_update_tx_notify_armed(port);
}

static void vm_complete_tx_bytes(VModemPort *port, DWORD count)
{
    DWORD nextGet;

    if (port == 0) {
        return;
    }

    if (count != 0 && port->pd.QOutSize != 0) {
        nextGet = port->pd.QOutGet + count;
        if (nextGet >= port->pd.QOutSize) {
            nextGet %= port->pd.QOutSize;
        }
        port->pd.QOutGet = nextGet;
    } else {
        port->pd.QOutGet = port->pd.QOutPut;
    }

    if (count != 0) {
        vm_record_event(port, EV_TXCHAR);
    }

    port->pd.QOutCount = 0;
    vm_notify_tx_ready(port);
    if (count != 0) {
        vm_record_event(port, EV_TXEMPTY);
    }
}

static void vm_fill_comstat(const VModemPort *port, _COMSTAT *pComstat)
{
    if (port == 0 || pComstat == 0) {
        return;
    }

    pComstat->BitMask = 0;
    pComstat->cbInque = port->pd.QInCount;
    pComstat->cbOutque = port->pd.QOutCount;
}

static void vm_fill_commprop(VModemPort *port, _COMMPROP *pCommprop)
{
    if (pCommprop == 0) {
        return;
    }

    vm_zero_bytes(pCommprop, sizeof(*pCommprop));
    pCommprop->wPacketLength = (WORD)sizeof(*pCommprop);
    pCommprop->wPacketVersion = 2;
    pCommprop->dwServiceMask = SP_SERIALCOMM;
    pCommprop->dwMaxTxQueue = VM_QUEUE_SIZE;
    pCommprop->dwMaxRxQueue = VM_QUEUE_SIZE;
    pCommprop->dwMaxBaud = 115200UL;
    pCommprop->dwProvSubType = PST_RS232;
    pCommprop->dwProvCapabilities = PCF_DTRDSR | PCF_RTSCTS | PCF_RLSD |
                                    PCF_XONXOFF | PCF_SETXCHAR |
                                    PCF_TOTALTIMEOUTS | PCF_INTTIMEOUTS |
                                    PCF_SPECIALCHARS;
    pCommprop->dwSettableParams = SP_PARITY | SP_BAUD | SP_DATABITS |
                                  SP_STOPBITS | SP_HANDSHAKING |
                                  SP_PARITY_CHECK | SP_RLSD;
    pCommprop->dwSettableBaud = BAUD_1200 | BAUD_2400 | BAUD_4800 |
                                BAUD_9600 | BAUD_14400 | BAUD_19200 |
                                BAUD_38400 | BAUD_56K | BAUD_USER;
    pCommprop->wSettableData = DATABITS_8;
    pCommprop->wSettableStopParity = STOPBITS_10 | PARITY_NONE;
    pCommprop->dwCurrentTxQueue = (port != 0) ? port->pd.QOutSize : VM_QUEUE_SIZE;
    pCommprop->dwCurrentRxQueue = (port != 0) ? port->pd.QInSize : VM_QUEUE_SIZE;
    pCommprop->dwProvSpec1 = 0;
    pCommprop->dwProvSpec2 = 0;
    pCommprop->wcProvChar[0] = 0;
    pCommprop->filler = 0;
}

static BOOL vm_is_valid_hport(DWORD hPort)
{
    return (hPort == (DWORD)&g_Port) ? TRUE : FALSE;
}

static VModemPort *vm_port_from_hport(DWORD hPort)
{
    if (!vm_is_valid_hport(hPort)) {
        return 0;
    }
    return (VModemPort *)hPort;
}

static BOOL vm_copy_rx_queue_out(VModemPort *port,
                                 char *buffer,
                                 DWORD count,
                                 DWORD *pRead)
{
    DWORD copied;
    BYTE *base;

    if (pRead != 0) {
        *pRead = 0;
    }

    if (port == 0 || buffer == 0 || pRead == 0) {
        return FALSE;
    }

    copied = 0;
    base = (BYTE *)(port->pd.QInAddr);

    while (copied < count &&
           port->pd.QInCount != 0 &&
           base != 0 &&
           port->pd.QInSize != 0) {
        buffer[copied] = (char)base[port->pd.QInGet];
        ++copied;
        --port->pd.QInCount;
        ++port->pd.QInGet;
        if (port->pd.QInGet >= port->pd.QInSize) {
            port->pd.QInGet = 0;
        }
    }

    *pRead = copied;
    vm_update_rx_notify_armed(port);
    return TRUE;
}

static DWORD vm_rx_queue_free(const VModemPort *port)
{
    if (port == 0 || port->pd.QInSize == 0 || port->pd.QInCount >= port->pd.QInSize) {
        return 0;
    }

    return port->pd.QInSize - port->pd.QInCount;
}

static void vm_reset_held_helper_rx(VModemPort *port)
{
    if (port == 0) {
        return;
    }

    port->bHoldHelperRxUntilRead = FALSE;
    port->dwHeldHelperSessionId = 0;
    port->dwHeldHelperRxCount = 0;
    port->dwHeldHelperRxGet = 0;
    port->dwHeldHelperRxPut = 0;
}

static DWORD vm_queue_held_helper_rx(VModemPort *port,
                                     DWORD sessionId,
                                     const BYTE *bytes,
                                     DWORD count)
{
    DWORD written;
    char line[96];
    int pos;

    if (port == 0 || bytes == 0 || count == 0) {
        return 0;
    }

    if (port->dwHeldHelperSessionId == 0) {
        port->dwHeldHelperSessionId = sessionId;
    }

    if (port->dwHeldHelperSessionId != sessionId) {
        return 0;
    }

    written = 0;
    while (written < count && port->dwHeldHelperRxCount < VM_QUEUE_SIZE) {
        port->heldHelperRx[port->dwHeldHelperRxPut] = bytes[written];
        ++written;
        ++port->dwHeldHelperRxCount;
        ++port->dwHeldHelperRxPut;
        if (port->dwHeldHelperRxPut >= VM_QUEUE_SIZE) {
            port->dwHeldHelperRxPut = 0;
        }
    }

    if (written != 0) {
        pos = 0;
        line[0] = '\0';
        vm_buf_append_str(line, &pos, sizeof(line), "POST_CONNECT_HOLD session=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), sessionId);
        vm_buf_append_str(line, &pos, sizeof(line), " len=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), written);
        vm_trace_log_line(line);
    }

    if (written < count) {
        port->pd.dwCommError |= CE_RXOVER;
        vm_trace_log_line("POST_CONNECT_HOLD overflow");
    }

    return written;
}

static void vm_reconcile_held_helper_rx(VModemPort *port)
{
    VM_MODEM_STATE state;

    if (port == 0) {
        return;
    }

    if (!port->bHoldHelperRxUntilRead && port->dwHeldHelperRxCount == 0) {
        return;
    }

    state = vm_modem_get_state(&port->modem);
    if (port->dwHeldHelperSessionId == 0 ||
        port->modem.current_session_id == 0 ||
        port->dwHeldHelperSessionId != port->modem.current_session_id ||
        (state != VM_MODEM_STATE_CONNECTED_DATA &&
         state != VM_MODEM_STATE_CONNECTED_CMD)) {
        vm_reset_held_helper_rx(port);
    }
}

static void vm_flush_held_helper_rx(VModemPort *port)
{
    BYTE temp[VM_MODEM_DRAIN_CHUNK];
    DWORD freeBytes;
    DWORD chunk;

    if (port == 0 || port->bHoldHelperRxUntilRead) {
        return;
    }

    while (port->dwHeldHelperRxCount != 0) {
        if (vm_dos_owner_active()) {
            freeBytes = VM_DOS_UART_FIFO_CAPACITY -
                        vm_dos_uart_get_rx_depth(&g_DosUart.uart);
        } else {
            freeBytes = vm_rx_queue_free(port);
        }

        if (freeBytes == 0 || (!g_Port.bOpen && !vm_dos_owner_active())) {
            break;
        }

        if (freeBytes > VM_MODEM_DRAIN_CHUNK) {
            freeBytes = VM_MODEM_DRAIN_CHUNK;
        }
        if (freeBytes > port->dwHeldHelperRxCount) {
            freeBytes = port->dwHeldHelperRxCount;
        }

        chunk = 0;
        while (chunk < freeBytes) {
            temp[chunk] = port->heldHelperRx[port->dwHeldHelperRxGet];
            ++chunk;
            --port->dwHeldHelperRxCount;
            ++port->dwHeldHelperRxGet;
            if (port->dwHeldHelperRxGet >= VM_QUEUE_SIZE) {
                port->dwHeldHelperRxGet = 0;
            }
        }

        vm_trace_log_local_serial_rx(temp, chunk);
        if (vm_dos_owner_active()) {
            vm_enqueue_dos_rx_bytes(temp, chunk);
        } else {
            vm_enqueue_rx_bytes(port, temp, chunk);
        }
    }

    if (port->dwHeldHelperRxCount == 0) {
        if (port->dwHeldHelperSessionId != 0) {
            vm_trace_log_line("POST_CONNECT_RELEASE");
        }
        port->dwHeldHelperSessionId = 0;
    }
}

static DWORD vm_enqueue_rx_bytes(VModemPort *port, const BYTE *bytes, DWORD count)
{
    BYTE *base;
    DWORD written;
    DWORD eventBits;
    char eventChar1;
    char eventChar2;

    if (port == 0 || bytes == 0 || count == 0 || port->pd.QInAddr == 0 ||
        port->pd.QInSize == 0) {
        return 0;
    }

    base = (BYTE *)(port->pd.QInAddr);
    written = 0;
    eventBits = 0;
    eventChar1 = port->dcb.EvtChar1;
    eventChar2 = port->dcb.EvtChar2;

    while (written < count && port->pd.QInCount < port->pd.QInSize) {
        base[port->pd.QInPut] = bytes[written];
        if ((char)bytes[written] == eventChar1 ||
            (char)bytes[written] == eventChar2) {
            eventBits |= EV_RXFLAG;
        }
        ++written;
        ++port->pd.QInCount;
        ++port->pd.QInPut;
        if (port->pd.QInPut >= port->pd.QInSize) {
            port->pd.QInPut = 0;
        }
    }

    if (written != 0) {
        vm_record_event(port, EV_RXCHAR | eventBits);
        vm_notify_rx_available(port);
    }

    if (written < count) {
        port->pd.dwCommError |= CE_RXOVER;
    }

    return written;
}

static void vm_apply_modem_status(VModemPort *port, DWORD newStatus)
{
    DWORD oldStatus;
    DWORD changed;
    DWORD eventBits;
    char  line[128];
    int   pos;

    if (port == 0) {
        return;
    }

    oldStatus = port->dwModemStatus;
    port->dwModemStatus = newStatus;
    vm_update_msr_shadow(port);

    changed = oldStatus ^ newStatus;
    if (changed == 0) {
        return;
    }

    eventBits = 0;
    if ((changed & MS_CTS_ON) != 0) {
        eventBits |= EV_CTS;
    }
    if ((changed & MS_DSR_ON) != 0) {
        eventBits |= EV_DSR;
    }
    if ((changed & MS_RLSD_ON) != 0) {
        eventBits |= EV_RLSD;
    }
    if (((changed & MS_RING_ON) != 0) && ((newStatus & MS_RING_ON) != 0)) {
        eventBits |= EV_RING;
    }

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "MODEM_STATUS old=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), oldStatus);
    vm_buf_append_str(line, &pos, sizeof(line), " new=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), newStatus);
    vm_buf_append_str(line, &pos, sizeof(line), " changed=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), changed);
    vm_trace_log_line(line);

    vm_record_event(port, eventBits);
}

static void vm_cancel_modem_timer(void)
{
    if (g_hModemTimer != 0) {
        Cancel_Time_Out(g_hModemTimer);
        g_hModemTimer = 0;
    }

    g_modemTimerDue = 0;
}

static void __cdecl vm_modem_timer_callback(void)
{
    g_hModemTimer = 0;
    g_modemTimerDue = 0;

    if (!vm_frontend_modem_active()) {
        return;
    }

    vm_poll_modem_core(&g_Port);
}

static void vm_refresh_modem_timer(VModemPort *port)
{
    DWORD deadline;
    DWORD now;
    DWORD delay;

    if (port == 0 || !vm_frontend_modem_active()) {
        vm_cancel_modem_timer();
        return;
    }

    if (!vm_modem_get_next_timer_deadline(&port->modem, &deadline)) {
        vm_cancel_modem_timer();
        return;
    }

    if (g_hModemTimer != 0 && g_modemTimerDue == deadline) {
        return;
    }

    vm_cancel_modem_timer();

    now = VM_Get_System_Time();
    if ((now - deadline) < 0x80000000UL) {
        delay = 1;
    } else {
        delay = deadline - now;
        if (delay == 0) {
            delay = 1;
        }
    }

    g_hModemTimer = Set_Global_Time_Out(vm_modem_timer_callback, delay, 0);
    if (g_hModemTimer != 0) {
        g_modemTimerDue = deadline;
    }
}

#define VM_DOS_UART_FAST_PERIOD_MS 1UL

static void vm_begin_dos_fast_period(void)
{
    if (g_bDosUartFastPeriod) {
        return;
    }
    DOSUART_BeginFastPeriod(VM_DOS_UART_FAST_PERIOD_MS);
    g_bDosUartFastPeriod = TRUE;
}

static void vm_end_dos_fast_period(void)
{
    if (!g_bDosUartFastPeriod) {
        return;
    }
    DOSUART_EndFastPeriod(VM_DOS_UART_FAST_PERIOD_MS);
    g_bDosUartFastPeriod = FALSE;
}

static void vm_cancel_dos_uart_timer(void)
{
    if (g_hDosUartTimer != 0) {
        Cancel_Time_Out(g_hDosUartTimer);
        g_hDosUartTimer = 0;
    }
    vm_end_dos_fast_period();
}

static void __cdecl vm_dos_uart_timer_callback(void)
{
    VM_DOS_UART_EVENT event;

    g_hDosUartTimer = 0;

    if (!g_DosUart.bOwnerActive) {
        return;
    }

    vm_dos_uart_tick(&g_DosUart.uart, &event);
    vm_sync_from_modem_core(&g_Port);

    if ((event.flags & VM_DOS_UART_EVENT_RX_TIMEOUT) != 0UL) {
        vm_dos_update_irq_request();
    }

    vm_refresh_dos_uart_timer();
}

static void vm_refresh_dos_uart_timer(void)
{
    int needTimer;

    if (!g_DosUart.bOwnerActive) {
        vm_cancel_dos_uart_timer();
        return;
    }

    /* Keep the timer running if the FIFO timeout is pending OR if the modem
     * output buffer has data waiting to be pushed into the FIFO. */
    needTimer = (g_DosUart.uart.rx_timeout_armed &&
                 !g_DosUart.uart.rx_timeout_fired) ||
                (vm_modem_output_count(&g_Port.modem) > 0U);

    if (!needTimer) {
        vm_cancel_dos_uart_timer();
        return;
    }

    /* Acquire the fast timer period only while modem output is pending.
     * The FIFO timeout path alone does not need sub-millisecond latency. */
    if (vm_modem_output_count(&g_Port.modem) > 0U) {
        vm_begin_dos_fast_period();
    } else {
        vm_end_dos_fast_period();
    }

    if (g_hDosUartTimer != 0) {
        return;
    }

    g_hDosUartTimer = Set_Global_Time_Out(vm_dos_uart_timer_callback, 1, 0);
}

static void vm_sync_modem_actions_to_helper(VModemPort *port)
{
    VM_MODEM_ACTION action;
    DWORD messageType;

    if (port == 0 || !g_bHelperClaimed) {
        return;
    }

    while (vm_modem_peek_action(&port->modem, &action)) {
        switch (action.type) {
        case VM_MODEM_ACTION_CONNECT_REQ:
            messageType = VMODEM_MSG_CONNECT_REQ;
            break;
        case VM_MODEM_ACTION_DATA_TO_NET:
            messageType = VMODEM_MSG_DATA_TO_NET;
            break;
        case VM_MODEM_ACTION_ANSWER_REQ:
            messageType = VMODEM_MSG_ANSWER_REQ;
            break;
        case VM_MODEM_ACTION_HANGUP_REQ:
            messageType = VMODEM_MSG_HANGUP_REQ;
            break;
        default:
            vm_modem_pop_action(&port->modem);
            continue;
        }

        if (!vm_queue_message_to_helper(messageType,
                                        action.session_id,
                                        action.status,
                                        action.payload,
                                        action.payload_length)) {
            break;
        }

        vm_modem_pop_action(&port->modem);
    }
}

static void vm_sync_from_modem_core(VModemPort *port)
{
    BYTE temp[VM_MODEM_DRAIN_CHUNK];
    DWORD freeBytes;
    DWORD ownerType;
    unsigned short chunk;

    if (port == 0) {
        return;
    }

    vm_apply_modem_status(port, (DWORD)vm_modem_get_status(&port->modem));
    vm_dos_apply_modem_status();
    vm_sync_modem_actions_to_helper(port);
    vm_reconcile_held_helper_rx(port);
    ownerType = vm_query_frontend_owner();

    while (ownerType != VMODEM_FRONTEND_OWNER_NONE &&
           vm_modem_output_count(&port->modem) != 0) {
        if (ownerType == VMODEM_FRONTEND_OWNER_DOS) {
            freeBytes = VM_DOS_UART_FIFO_CAPACITY -
                        vm_dos_uart_get_rx_depth(&g_DosUart.uart);
        } else {
            freeBytes = vm_rx_queue_free(port);
        }

        if (freeBytes == 0) {
            break;
        }
        if (freeBytes > VM_MODEM_DRAIN_CHUNK) {
            freeBytes = VM_MODEM_DRAIN_CHUNK;
        }

        chunk = vm_modem_drain_output(&port->modem, temp, (unsigned short)freeBytes);
        if (chunk == 0) {
            break;
        }

        vm_trace_log_local_serial_rx(temp, (DWORD)chunk);
        if (ownerType == VMODEM_FRONTEND_OWNER_DOS) {
            vm_enqueue_dos_rx_bytes(temp, (DWORD)chunk);
        } else {
            vm_enqueue_rx_bytes(port, temp, (DWORD)chunk);
        }
    }

    if (ownerType != VMODEM_FRONTEND_OWNER_NONE) {
        vm_flush_held_helper_rx(port);
    }
    vm_refresh_modem_timer(port);
}

static void vm_poll_modem_core(VModemPort *port)
{
    DWORD now;

    if (port == 0) {
        return;
    }

    now = VM_Get_System_Time();
    vm_modem_poll(&port->modem, now);
    vm_sync_from_modem_core(port);
}

static void vm_sync_host_lines_to_modem(VModemPort *port)
{
    DWORD now;

    if (port == 0) {
        return;
    }

    now = VM_Get_System_Time();
    vm_modem_poll(&port->modem, now);
    vm_modem_set_host_lines(&port->modem,
                            port->bDtrAsserted ? 1 : 0,
                            port->bRtsAsserted ? 1 : 0,
                            now);
    vm_sync_from_modem_core(port);
}

static void vm_process_modem_tx(VModemPort *port, const BYTE *bytes, DWORD count)
{
    DWORD now;
    DWORD remaining;
    unsigned short chunk;

    if (port == 0) {
        return;
    }

    now = VM_Get_System_Time();
    vm_modem_poll(&port->modem, now);
    vm_trace_log_modem_tx(bytes, count);
    vm_trace_capture_tx_for_log(bytes, count);

    remaining = count;
    while (bytes != 0 && remaining != 0) {
        if (remaining > 0xFFFFUL) {
            chunk = 0xFFFFU;
        } else {
            chunk = (unsigned short)remaining;
        }

        vm_modem_ingest_tx(&port->modem, bytes, chunk, now);
        bytes += chunk;
        remaining -= chunk;
    }

    vm_sync_from_modem_core(port);
}

static void vm_prepare_closed_state(VModemPort *port)
{
    if (port == 0) {
        return;
    }

    vm_cancel_modem_timer();
    vm_reset_held_helper_rx(port);
    port->bOpen = FALSE;
    port->dwVMId = 0;
    port->lpMSRShadow = &port->pd.bMSRShadow;
    port->lpEventMaskLoc = &port->pd.dwDetectedEvents;
    port->pd.dwDetectedEvents = 0;
    port->pd.dwCommError = 0;
    port->pd.dwLastError = 0;
    port->pd.dwCallerVMId = 0;
    port->pd.ValidPortData = 0;
    vm_reset_queue_state(&port->pd);
    vm_reset_callbacks(port);
    vm_reset_contention_ownership(port);
    vm_modem_port_close(&port->modem);
    port->dwModemStatus = (DWORD)vm_modem_get_status(&port->modem);
    vm_update_msr_shadow(port);
}

static void vm_initialize_port_instance(VModemPort *port,
                                        DWORD devNode,
                                        DWORD dcRefData,
                                        DWORD allocBase,
                                        DWORD allocIrq,
                                        const char *portName)
{
    if (port == 0) {
        return;
    }

    vm_zero_bytes(port, sizeof(*port));
    port->pd.PDLength = sizeof(PortData);
    port->pd.PDVersion = 0x010A;
    port->pd.PDfunctions = &VM_PortFunctions;
    port->pd.PDNumFunctions = sizeof(PortFunctions) / sizeof(DWORD);
    port->dwDevNode = devNode;
    port->dwDCRefData = dcRefData;
    port->dwAllocBase = allocBase;
    port->dwAllocIrq = allocIrq;
    vm_copy_text(port->szPortName, portName, sizeof(port->szPortName));
    vm_init_default_ring0_dcb(&port->dcb);
    vm_sync_host_lines_from_dcb(port);
    vm_modem_init(&port->modem);
    vm_modem_set_helper_available(&port->modem, g_bHelperClaimed ? 1 : 0);
    vm_use_internal_queues(port);
    vm_prepare_closed_state(port);
    vm_dos_initialize(port);
}

static void vm_refresh_contention_info(VModemPort *port)
{
    PFN contentionHandler;

    if (port == 0) {
        return;
    }

    port->dwContentionHandler = 0;
    port->dwContentionResource = 0;

    contentionHandler = VM_VCOMM_Get_Contention_Handler(port->szPortName);
    port->dwContentionHandler = (DWORD)contentionHandler;
    if (contentionHandler != 0) {
        port->dwContentionResource =
            VM_VCOMM_Map_Name_To_Resource(port->szPortName);
    }
}

static DWORD vm_contention_map_name_to_resource(DWORD pName,
                                                DWORD devNode,
                                                DWORD ioBase)
{
    const char *portName;

    (void)devNode;
    (void)ioBase;

    portName = (const char *)pName;
    if (portName == 0 || !vm_port_name_matches(g_Port.szPortName, portName)) {
        return 0;
    }

    return (DWORD)&g_Port;
}

static DWORD vm_contention_acquire_resource(DWORD resource,
                                            DWORD notifyProc,
                                            DWORD notifyRefData,
                                            DWORD stealFlag)
{
    VModemPort *port;
    VM_CONTEND_NOTIFY_PROC notify;

    port = vm_port_from_contention_resource(resource);
    if (port == 0) {
        return 0;
    }

    if (port->dwContentionOwnerNotify == 0) {
        port->dwContentionOwnerNotify = notifyProc;
        port->dwContentionOwnerRefData = notifyRefData;
        port->dwContentionAltNotify = 0;
        port->dwContentionAltRefData = 0;
        return resource;
    }

    if (port->dwContentionOwnerNotify == notifyProc &&
        port->dwContentionOwnerRefData == notifyRefData) {
        return resource;
    }

    if (stealFlag == 0) {
        return 0;
    }

    notify = (VM_CONTEND_NOTIFY_PROC)port->dwContentionOwnerNotify;
    if (notify != 0 && !notify(port->dwContentionOwnerRefData, 0)) {
        return 0;
    }

    port->dwContentionAltNotify = port->dwContentionOwnerNotify;
    port->dwContentionAltRefData = port->dwContentionOwnerRefData;
    port->dwContentionOwnerNotify = notifyProc;
    port->dwContentionOwnerRefData = notifyRefData;
    return resource;
}

static DWORD vm_contention_steal_resource(DWORD resourceHandle,
                                          DWORD notifyProc)
{
    VModemPort *port;

    port = vm_port_from_contention_resource(resourceHandle);
    if (port == 0) {
        return 0;
    }

    if (port->dwContentionOwnerNotify == notifyProc) {
        return 1UL;
    }

    if (port->dwContentionAltNotify == notifyProc) {
        port->dwContentionOwnerNotify = port->dwContentionAltNotify;
        port->dwContentionOwnerRefData = port->dwContentionAltRefData;
        port->dwContentionAltNotify = 0;
        port->dwContentionAltRefData = 0;
        return 1UL;
    }

    return 0;
}

static DWORD vm_contention_release_resource(DWORD resourceHandle,
                                            DWORD notifyProc)
{
    VModemPort *port;
    VM_CONTEND_NOTIFY_PROC notify;

    port = vm_port_from_contention_resource(resourceHandle);
    if (port == 0) {
        return 0;
    }

    if (port->dwContentionOwnerNotify == notifyProc) {
        port->dwContentionOwnerNotify = 0;
        port->dwContentionOwnerRefData = 0;
        if (port->dwContentionAltNotify != 0) {
            port->dwContentionOwnerNotify = port->dwContentionAltNotify;
            port->dwContentionOwnerRefData = port->dwContentionAltRefData;
            port->dwContentionAltNotify = 0;
            port->dwContentionAltRefData = 0;
            notify = (VM_CONTEND_NOTIFY_PROC)port->dwContentionOwnerNotify;
            if (notify != 0) {
                notify(port->dwContentionOwnerRefData, 1);
            }
        }
        return 1UL;
    }

    if (port->dwContentionAltNotify == notifyProc) {
        port->dwContentionAltNotify = 0;
        port->dwContentionAltRefData = 0;
        return 1UL;
    }

    return 0;
}

DWORD __cdecl VM_VxD_Contention_Handler(DWORD functionCode,
                                        DWORD arg1,
                                        DWORD arg2,
                                        DWORD arg3,
                                        DWORD arg4)
{
    switch (functionCode) {
    case MAP_DEVICE_TO_RESOURCE:
        return vm_contention_map_name_to_resource(arg1, arg2, arg3);
    case ACQUIRE_RESOURCE:
        return vm_contention_acquire_resource(arg1, arg2, arg3, arg4);
    case STEAL_RESOURCE:
        return vm_contention_steal_resource(arg1, arg2);
    case RELEASE_RESOURCE:
        return vm_contention_release_resource(arg1, arg2);
    case ADD_RESOURCE:
        return (DWORD)&g_Port;
    case REMOVE_RESOURCE:
        vm_reset_contention_ownership(&g_Port);
        return 1UL;
    default:
        return 0;
    }
}

static void vm_log_append_to(char *logBuf, DWORD *pLogLen, const char *str)
{
    DWORD i;
    DWORD logLen;

    if (logBuf == 0 || pLogLen == 0 || str == 0) {
        return;
    }

    logLen = *pLogLen;
    for (i = 0; str[i] != '\0'; ++i) {
        if (logLen + 1 >= VMODEM_HOOK_LOG_DATA_LEN) {
            return;
        }
        logBuf[logLen++] = str[i];
    }

    if (logLen + 1 < VMODEM_HOOK_LOG_DATA_LEN) {
        logBuf[logLen++] = '|';
    }

    logBuf[logLen] = '\0';
    *pLogLen = logLen;
}

static void vm_log_append(const char *str)
{
    vm_log_append_to(g_portOpenLog, &g_portOpenLogLen, str);
}

static void vm_capture_log_append(const char *str)
{
    vm_log_append_to(g_hookCaptureLog, &g_hookCaptureLogLen, str);
}

static void vm_log_append_active(const char *str)
{
    vm_log_append(str);
    if (g_bHookCaptureEnabled) {
        vm_capture_log_append(str);
    }
}

static void vm_buf_append_char(char *buf, int *pPos, int cchBuf, char ch)
{
    int pos;

    if (buf == 0 || pPos == 0 || cchBuf <= 0) {
        return;
    }

    pos = *pPos;
    if (pos + 1 >= cchBuf) {
        return;
    }

    buf[pos++] = ch;
    buf[pos] = '\0';
    *pPos = pos;
}

static void vm_buf_append_str(char *buf, int *pPos, int cchBuf, const char *str)
{
    int i;

    if (str == 0) {
        return;
    }

    for (i = 0; str[i] != '\0'; ++i) {
        vm_buf_append_char(buf, pPos, cchBuf, str[i]);
    }
}

static void vm_buf_append_u32_dec(char *buf, int *pPos, int cchBuf, DWORD value)
{
    char digits[10];
    int  count;

    if (value == 0) {
        vm_buf_append_char(buf, pPos, cchBuf, '0');
        return;
    }

    count = 0;
    while (value != 0 && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (count > 0) {
        vm_buf_append_char(buf, pPos, cchBuf, digits[--count]);
    }
}

static void vm_buf_append_u32_hex(char *buf, int *pPos, int cchBuf, DWORD value)
{
    static const char s_hexDigits[] = "0123456789ABCDEF";
    int shift;

    vm_buf_append_str(buf, pPos, cchBuf, "0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        vm_buf_append_char(buf, pPos, cchBuf,
                           s_hexDigits[(value >> shift) & 0x0FU]);
    }
}

static void vm_trace_log_reset_command_capture(void)
{
    g_traceCmdLen = 0;
    g_traceCmdOverflowed = FALSE;
    g_traceCmdBuffer[0] = '\0';
}

static void vm_trace_log_line(const char *text)
{
    DWORD i;
    DWORD len;

    if (text == 0 || text[0] == '\0') {
        return;
    }

    len = 0;
    while (text[len] != '\0') {
        ++len;
    }

    if (g_traceLogLen + len + 3U >= VMODEM_TRACE_LOG_DATA_LEN) {
        ++g_traceLogDropped;
        return;
    }

    for (i = 0; i < len; ++i) {
        g_traceLog[g_traceLogLen++] = text[i];
    }
    g_traceLog[g_traceLogLen++] = '\r';
    g_traceLog[g_traceLogLen++] = '\n';
    g_traceLog[g_traceLogLen] = '\0';
}

static void vm_trace_log_command_line(void)
{
    char line[VM_TRACE_CMD_BUFFER_LEN + 24];
    int pos;

    if (g_traceCmdLen < 2U ||
        vm_fold_ascii(g_traceCmdBuffer[0]) != 'A' ||
        vm_fold_ascii(g_traceCmdBuffer[1]) != 'T') {
        vm_trace_log_reset_command_capture();
        return;
    }

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "AT_CMD text=");
    vm_buf_append_str(line, &pos, sizeof(line), g_traceCmdBuffer);
    if (g_traceCmdOverflowed) {
        vm_buf_append_str(line, &pos, sizeof(line), " (truncated)");
    }
    vm_trace_log_line(line);
    vm_trace_log_reset_command_capture();
}

static void vm_trace_capture_tx_for_log(const BYTE *bytes, DWORD count)
{
    DWORD i;
    BYTE ch;

    if (bytes == 0 || count == 0 ||
        g_Port.modem.state == VM_MODEM_STATE_CONNECTED_DATA) {
        return;
    }

    for (i = 0; i < count; ++i) {
        ch = bytes[i];
        if (ch == '\r' || ch == '\n') {
            if (g_traceCmdLen != 0U) {
                vm_trace_log_command_line();
            }
            continue;
        }

        if (ch == 0x08U || ch == 0x7FU) {
            if (g_traceCmdLen != 0U) {
                --g_traceCmdLen;
                g_traceCmdBuffer[g_traceCmdLen] = '\0';
            }
            continue;
        }

        if (ch < 0x20U || ch > 0x7EU) {
            continue;
        }

        if (g_traceCmdLen + 1U >= VM_TRACE_CMD_BUFFER_LEN) {
            g_traceCmdOverflowed = TRUE;
            continue;
        }

        g_traceCmdBuffer[g_traceCmdLen++] = (char)ch;
        g_traceCmdBuffer[g_traceCmdLen] = '\0';
    }
}

static void vm_buf_append_byte_hex(char *buf,
                                   int *pPos,
                                   int cchBuf,
                                   BYTE value)
{
    static const char s_hexDigits[] = "0123456789ABCDEF";

    vm_buf_append_char(buf, pPos, cchBuf, s_hexDigits[(value >> 4) & 0x0FU]);
    vm_buf_append_char(buf, pPos, cchBuf, s_hexDigits[value & 0x0FU]);
}

static void vm_buf_append_escaped_byte(char *buf,
                                       int *pPos,
                                       int cchBuf,
                                       BYTE value)
{
    if (value == '\r') {
        vm_buf_append_str(buf, pPos, cchBuf, "\\r");
    } else if (value == '\n') {
        vm_buf_append_str(buf, pPos, cchBuf, "\\n");
    } else if (value == '\t') {
        vm_buf_append_str(buf, pPos, cchBuf, "\\t");
    } else if (value >= 0x20U && value <= 0x7EU) {
        vm_buf_append_char(buf, pPos, cchBuf, (char)value);
    } else {
        vm_buf_append_str(buf, pPos, cchBuf, "\\x");
        vm_buf_append_byte_hex(buf, pPos, cchBuf, value);
    }
}

static void vm_trace_log_local_serial_rx(const BYTE *bytes, DWORD count)
{
    char line[192];
    int pos;
    DWORD i;
    DWORD limit;

    if (bytes == 0 || count == 0) {
        return;
    }

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "MODEM_RX len=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), count);
    vm_buf_append_str(line, &pos, sizeof(line), " text=");

    limit = count;
    if (limit > 48UL) {
        limit = 48UL;
    }

    for (i = 0; i < limit; ++i) {
        vm_buf_append_escaped_byte(line, &pos, sizeof(line), bytes[i]);
    }

    if (limit < count) {
        vm_buf_append_str(line, &pos, sizeof(line), "...");
    }

    vm_trace_log_line(line);
}

static void vm_trace_log_modem_tx(const BYTE *bytes, DWORD count)
{
    char line[192];
    int pos;
    DWORD i;
    DWORD limit;

    if (bytes == 0 || count == 0) {
        return;
    }

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "MODEM_TX len=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), count);
    vm_buf_append_str(line, &pos, sizeof(line), " text=");

    limit = count;
    if (limit > 48UL) {
        limit = 48UL;
    }

    for (i = 0; i < limit; ++i) {
        vm_buf_append_escaped_byte(line, &pos, sizeof(line), bytes[i]);
    }

    if (limit < count) {
        vm_buf_append_str(line, &pos, sizeof(line), "...");
    }

    vm_trace_log_line(line);
}

static void vm_trace_log_port_read_bytes(const BYTE *bytes, DWORD count)
{
    char line[160];
    int  pos;
    DWORD offset;
    DWORD chunk;
    DWORD i;

    if (bytes == 0 || count == 0) {
        return;
    }

    offset = 0;
    while (offset < count) {
        chunk = count - offset;
        if (chunk > 16UL) {
            chunk = 16UL;
        }

        pos = 0;
        line[0] = '\0';
        vm_buf_append_str(line, &pos, sizeof(line), "PORT_READ_DATA offset=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), offset);
        vm_buf_append_str(line, &pos, sizeof(line), " len=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), chunk);
        vm_buf_append_str(line, &pos, sizeof(line), " hex=");

        for (i = 0; i < chunk; ++i) {
            if (i != 0UL) {
                vm_buf_append_char(line, &pos, sizeof(line), ' ');
            }
            vm_buf_append_byte_hex(line, &pos, sizeof(line), bytes[offset + i]);
        }

        vm_trace_log_line(line);
        offset += chunk;
    }
}

static void vm_capture_log_event(DWORD pfn,
                                 DWORD fn,
                                 DWORD Drive,
                                 DWORD ResType,
                                 DWORD CodePage,
                                 DWORD pir)
{
    char buf[96];
    int  pos;

    if (!g_bHookCaptureEnabled) {
        return;
    }

    pos = 0;
    buf[0] = '\0';
    vm_buf_append_str(buf, &pos, sizeof(buf), "evt:fn=");
    vm_buf_append_u32_dec(buf, &pos, sizeof(buf), fn);
    vm_buf_append_str(buf, &pos, sizeof(buf), ",pfn=");
    vm_buf_append_u32_hex(buf, &pos, sizeof(buf), pfn);
    vm_buf_append_str(buf, &pos, sizeof(buf), ",drv=");
    vm_buf_append_u32_hex(buf, &pos, sizeof(buf), Drive);
    vm_buf_append_str(buf, &pos, sizeof(buf), ",res=");
    vm_buf_append_u32_hex(buf, &pos, sizeof(buf), ResType);
    vm_buf_append_str(buf, &pos, sizeof(buf), ",cp=");
    vm_buf_append_u32_hex(buf, &pos, sizeof(buf), CodePage);
    vm_buf_append_str(buf, &pos, sizeof(buf), ",pir=");
    vm_buf_append_u32_hex(buf, &pos, sizeof(buf), pir);
    vm_capture_log_append(buf);
}

static void vm_reset_hook_capture_state(void)
{
    DWORD i;

    ++g_hookCaptureGeneration;
    g_hookCaptureFireCount = 0;
    g_hookCapturePortOpenCount = 0;
    g_hookCaptureOtherFnCount = 0;
    g_hookCaptureLogLen = 0;
    g_hookCaptureLog[0] = '\0';
    for (i = 0; i < VMODEM_HOOK_FN_BUCKETS; ++i) {
        g_hookCaptureFnCounts[i] = 0;
    }
}

static void vm_record_hook_capture(DWORD fn)
{
    if (!g_bHookCaptureEnabled) {
        return;
    }

    ++g_hookCaptureFireCount;
    if (fn < VMODEM_HOOK_FN_BUCKETS) {
        ++g_hookCaptureFnCounts[fn];
    } else {
        ++g_hookCaptureOtherFnCount;
    }
}

/*
 * vm_ifs_log_path - if the pioreq has a valid ir_ppath, convert each
 * PathElement from UTF-16LE to ASCII (high byte must be 0) and append
 * to the hook log.  Called only on VFN_OPEN so pir/ir_ppath are valid.
 * Prefixes each entry with "fn:N:" so we can correlate with the IFS
 * function code.
 */
/*
 * vm_ifs_should_log_path - restrict parsed-path diagnostics to request types
 * whose ir_ppath is expected to be valid.
 */
static BOOL vm_ifs_should_log_path(DWORD fn, pioreq pir)
{
    if (pir == 0) {
        return FALSE;
    }

    /* In practice the IFS hook can receive non-open requests with a non-null
     * pioreq whose ir_ppath is not safe to parse. For the \\.\COM3 question we
     * only need the open path, so keep the diagnostic on the documented stable
     * case instead of touching arbitrary request layouts. */
    if (fn != VFN_OPEN) {
        return FALSE;
    }

    return TRUE;
}

/*
 * vm_ifs_get_element_info - validate one variable-length PathElement before
 * advancing with IFSNextElement() or touching its UTF-16 payload.
 */
static BOOL vm_ifs_get_element_info(struct PathElement *pel,
                                    unsigned short consumed,
                                    unsigned short total,
                                    unsigned short *pelenOut,
                                    unsigned short *ncharsOut)
{
    unsigned short pelen;

    if (pel == 0 || pelenOut == 0 || ncharsOut == 0) {
        return FALSE;
    }

    if (consumed >= total) {
        return FALSE;
    }

    pelen = pel->pe_length;
    if (pelen < sizeof(unsigned short)) {
        return FALSE;
    }

    if ((pelen & 1U) != 0) {
        return FALSE;
    }

    if (pelen > (unsigned short)(total - consumed)) {
        return FALSE;
    }

    *pelenOut = pelen;
    *ncharsOut = (unsigned short)((pelen - sizeof(unsigned short)) /
                                  sizeof(unsigned short));
    return TRUE;
}

static BOOL vm_ifs_get_parsed_path(pioreq pir, ParsedPath **ppOut)
{
    ParsedPath *pp;

    if (pir == 0 || ppOut == 0) {
        return FALSE;
    }

    pp = pir->ir_ppath;
    if (pp == 0) {
        return FALSE;
    }

    if ((pp->pp_totalLength < sizeof(unsigned short) * 2) ||
        ((pp->pp_totalLength & 1U) != 0) ||
        (pp->pp_prefixLength > pp->pp_totalLength)) {
        return FALSE;
    }

    *ppOut = pp;
    return TRUE;
}

/*
 * vm_pe_contains_com - return 1 if a PathElement's text contains "COM"
 * (case-insensitive, ASCII only).
 */
static int vm_pe_contains_com(struct PathElement *pel, unsigned short nchars)
{
    unsigned short i;
    char c0, c1, c2;

    if (pel == 0 || nchars < 3) return 0;
    for (i = 0; i + 2 < nchars; ++i) {
        c0 = (char)(pel->pe_unichars[i]   & 0x7F);
        c1 = (char)(pel->pe_unichars[i+1] & 0x7F);
        c2 = (char)(pel->pe_unichars[i+2] & 0x7F);
        if ((c0 == 'C' || c0 == 'c') &&
            (c1 == 'O' || c1 == 'o') &&
            (c2 == 'M' || c2 == 'm'))
            return 1;
    }
    return 0;
}

static void vm_ifs_log_path(DWORD fn, pioreq pir)
{
    ParsedPath         *pp;
    struct PathElement *pel;
    struct PathElement *scan;
    unsigned short      total, consumed;
    unsigned short      pelen, nchars;
    char                buf[32];
    int                 i, j;
    int                 has_com;

    if (!vm_ifs_get_parsed_path(pir, &pp)) {
        return;
    }

    /* First pass: check if any element contains "COM" — skip if not. */
    total    = pp->pp_totalLength;
    consumed = sizeof(unsigned short) * 2;
    scan     = &pp->pp_elements[0];
    has_com  = 0;
    while (consumed < total) {
        if (!vm_ifs_get_element_info(scan, consumed, total, &pelen, &nchars)) {
            break;
        }
        if (vm_pe_contains_com(scan, nchars)) { has_com = 1; break; }
        consumed += pelen;
        scan = IFSNextElement(scan);
    }
    if (!has_com) return;

    /* fn prefix: fn is at most 2 digits */
    buf[0] = (char)('0' + (fn / 10 % 10));
    buf[1] = (char)('0' + (fn % 10));
    buf[2] = ':';
    buf[3] = '\0';
    vm_log_append_active(buf);

    /* Second pass: log each element. */
    consumed = sizeof(unsigned short) * 2;
    pel      = &pp->pp_elements[0];
    while (consumed < total) {
        if (!vm_ifs_get_element_info(pel, consumed, total, &pelen, &nchars)) {
            break;
        }
        if (nchars >= (unsigned short)(sizeof(buf) - 1))
            nchars = (unsigned short)(sizeof(buf) - 1);
        j = 0;
        for (i = 0; i < (int)nchars; ++i) {
            unsigned short uc = pel->pe_unichars[i];
            buf[j++] = (uc < 0x80) ? (char)uc : '?';
        }
        buf[j] = '\0';
        vm_log_append_active(buf);

        consumed += pelen;
        pel = IFSNextElement(pel);
    }
}

/*
 * VM_IFSHook - installed in the IFS hook chain to observe file open paths.
 *
 * Signature matches IFSFileHookFunc from IFS.H (DDK confirmed _cdecl):
 *   int _cdecl IFSFileHookFunc(pIFSFunc pfn, int fn, int Drive,
 *                              int ResType, int CodePage, pioreq pir)
 *
 * We log VFN_OPEN (fn==5) paths to see whether \\.\COM3 reaches IFS.
 * Chain-through: call (*g_ppPrevIfsHook)(pfn, fn, ...) with same 6 args.
 */
DWORD __cdecl VM_IFSHook(DWORD pfn, DWORD fn, DWORD Drive,
                          DWORD ResType, DWORD CodePage, DWORD pir)
{
    typedef DWORD (__cdecl *ChainProc)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);

    ++g_ifsHookFireCount;
    vm_record_hook_capture(fn);
    vm_capture_log_event(pfn, fn, Drive, ResType, CodePage, pir);

    if (vm_ifs_should_log_path(fn, (pioreq)pir))
        vm_ifs_log_path(fn, (pioreq)pir);

    return ((ChainProc)(*g_ppPrevIfsHook))(pfn, fn, Drive, ResType, CodePage, pir);
}

BOOL VCOMM_Init(void)
{
    if (g_bVcommRegistered) {
        return TRUE;
    }

    vm_ipc_queue_reset(&g_ToHelperQueue);

    if (!VM_VCOMM_Get_Version()) {
        VTRACE("VMODEM: VCOMM not present\r\n");
        return FALSE;
    }

    if (!VM_VCOMM_Register_Port_Driver((PFN)VM_DriverControl_Thunk)) {
        VTRACE("VMODEM: VCOMM_Register_Port_Driver failed\r\n");
        return FALSE;
    }

    g_bVcommRegistered = TRUE;
    VTRACE("VMODEM: VCOMM port driver registered\r\n");

    return TRUE;
}

void VCOMM_Cleanup(void)
{
    if (g_Port.bOpen) {
        VM_PortClose((DWORD)&g_Port);
    }
    vm_dos_cleanup();
    g_bHelperClaimed = FALSE;
    g_helperOwnerHandle = 0;
    vm_ipc_queue_reset(&g_ToHelperQueue);
    vm_prepare_closed_state(&g_Port);
}

void VCOMM_HelperHandleOpened(DWORD hDevice)
{
    (void)hDevice;
    VTRACE("VMODEM: helper opened control device\r\n");
}

void VCOMM_HelperHandleClosed(DWORD hDevice)
{
    if (g_bHelperClaimed && hDevice == g_helperOwnerHandle) {
        VTRACE("VMODEM: active helper closed control device\r\n");
        g_bHelperClaimed = FALSE;
        g_helperOwnerHandle = 0;
        g_lastMsgToHelper = VMODEM_MSG_NONE;
        g_lastMsgFromHelper = VMODEM_MSG_NONE;
        vm_ipc_queue_reset(&g_ToHelperQueue);
        vm_sync_helper_availability_to_modem();
        return;
    }

    VTRACE("VMODEM: non-owner control handle closed\r\n");
}

void VCOMM_HelperClaim(DWORD hDevice, VMODEM_HELLO_ACK *ack)
{
    if (ack == 0) {
        return;
    }

    ack->status = VMODEM_STATUS_OK;
    ack->protocol_version = VMODEM_IPC_VERSION;
    ack->helper_generation = g_helperGeneration;
    ack->max_payload = VMODEM_IPC_MAX_PAYLOAD;

    if (g_bHelperClaimed && g_helperOwnerHandle != hDevice) {
        ack->status = VMODEM_STATUS_BUSY;
        return;
    }

    if (!g_bHelperClaimed) {
        ++g_helperGeneration;
        g_bHelperClaimed = TRUE;
        g_helperOwnerHandle = hDevice;
        g_lastMsgToHelper = VMODEM_MSG_NONE;
        g_lastMsgFromHelper = VMODEM_MSG_NONE;
        vm_ipc_queue_reset(&g_ToHelperQueue);
        VTRACE("VMODEM: helper claimed active IPC role\r\n");
        vm_sync_helper_availability_to_modem();
    }

    ack->helper_generation = g_helperGeneration;
}

void VCOMM_HelperSubmitMessage(DWORD hDevice,
                               const VMODEM_PROTOCOL_MESSAGE *message,
                               VMODEM_SUBMIT_MESSAGE_ACK *ack)
{
    DWORD now;
    unsigned short copied;
    int accepted;

    if (ack == 0) {
        return;
    }

    ack->status = VMODEM_STATUS_OK;
    ack->helper_generation = g_helperGeneration;
    ack->session_id = (message != 0) ? message->session_id : 0;
    ack->message_type = (message != 0) ? message->message_type : VMODEM_MSG_NONE;

    if (!g_bHelperClaimed || hDevice != g_helperOwnerHandle) {
        ack->status = VMODEM_STATUS_NOT_OWNER;
        return;
    }

    if (message == 0 || message->helper_generation != g_helperGeneration) {
        ack->status = VMODEM_STATUS_STALE;
        return;
    }

    now = VM_Get_System_Time();
    vm_modem_poll(&g_Port.modem, now);
    accepted = 0;
    copied = 0;

    switch (message->message_type) {
    case VMODEM_MSG_CONNECT_OK:
        accepted = vm_modem_on_connect_ok(&g_Port.modem,
                                          message->session_id,
                                          now);
        if (accepted) {
            vm_reset_held_helper_rx(&g_Port);
            g_Port.bHoldHelperRxUntilRead = TRUE;
            g_Port.dwHeldHelperSessionId = message->session_id;
            vm_trace_log_line("POST_CONNECT_HOLD armed");
        }
        break;

    case VMODEM_MSG_CONNECT_FAIL:
        accepted = vm_modem_on_connect_fail(&g_Port.modem,
                                            message->session_id,
                                            message->status);
        break;

    case VMODEM_MSG_DATA_TO_SERIAL:
        if (g_Port.bHoldHelperRxUntilRead) {
            copied = (unsigned short)vm_queue_held_helper_rx(&g_Port,
                                                             message->session_id,
                                                             message->payload,
                                                             message->payload_length);
        } else {
            copied = vm_modem_on_serial_from_helper(&g_Port.modem,
                                                    message->session_id,
                                                    message->payload,
                                                    (unsigned short)message->payload_length);
        }
        accepted = (copied != 0 || message->payload_length == 0) ? 1 : 0;
        break;

    case VMODEM_MSG_INBOUND_RING:
        accepted = vm_modem_on_inbound_ring(&g_Port.modem,
                                            message->session_id);
        break;

    case VMODEM_MSG_REMOTE_CLOSED:
        accepted = vm_modem_on_remote_closed(&g_Port.modem,
                                             message->session_id);
        break;

    default:
        ack->status = VMODEM_STATUS_BAD_MESSAGE;
        return;
    }

    if (!accepted) {
        ack->status = VMODEM_STATUS_STALE;
        return;
    }

    g_lastMsgFromHelper = message->message_type;
    vm_trace_ipc_from_helper(message->message_type);

    if (vm_frontend_modem_active()) {
        vm_sync_from_modem_core(&g_Port);
        if (g_Port.bOpen) {
            vm_flush_rx_notify(&g_Port);
        }
    } else {
        g_Port.dwModemStatus = (DWORD)vm_modem_get_status(&g_Port.modem);
        vm_update_msr_shadow(&g_Port);
    }
}

void VCOMM_HelperReceiveMessage(DWORD hDevice,
                                const VMODEM_RECEIVE_MESSAGE *request,
                                VMODEM_PROTOCOL_MESSAGE *message)
{
    if (message == 0) {
        return;
    }

    vm_zero_bytes(message, sizeof(*message));
    message->version = VMODEM_IPC_VERSION;
    message->helper_generation = g_helperGeneration;

    if (!g_bHelperClaimed || hDevice != g_helperOwnerHandle) {
        message->status = VMODEM_STATUS_NOT_OWNER;
        return;
    }

    if (request == 0 || request->helper_generation != g_helperGeneration) {
        message->status = VMODEM_STATUS_STALE;
        return;
    }

    if (!vm_ipc_queue_pop(&g_ToHelperQueue, message)) {
        message->status = VMODEM_STATUS_NO_MESSAGE;
        message->message_type = VMODEM_MSG_NONE;
        return;
    }
}

DWORD __cdecl DOSUART_OnIoTrap(DWORD vmId,
                               DWORD port,
                               DWORD ioType,
                               DWORD value)
{
    DWORD offset;
    DWORD hadRxData;
    BYTE txByte;
    BYTE readValue;
    VM_DOS_UART_EVENT event;

    if (!g_DosUart.uart.enabled ||
        port < g_DosUart.uart.base_port ||
        port >= (g_DosUart.uart.base_port + 8UL)) {
        return vm_dos_uart_read_inert((unsigned short)(port & 7U));
    }

    offset = port - g_DosUart.uart.base_port;
    if (g_Port.bOpen ||
        Test_Sys_VM_Handle((HVM)vmId) ||
        (g_DosUart.bOwnerActive && g_DosUart.dwOwnerVmId != vmId)) {
        if (ioType == 0) {
            readValue = vm_dos_uart_read_inert((unsigned short)offset);
            vm_dos_trace_register_access("DOSUART_INERT_READ",
                                         vmId,
                                         port,
                                         offset,
                                         readValue);
            return readValue;
        }

        vm_dos_trace_register_access("DOSUART_INERT_WRITE",
                                     vmId,
                                     port,
                                     offset,
                                     value & 0xFFUL);
        return 0;
    }

    if (!g_DosUart.bOwnerActive && !vm_dos_claim_owner(vmId)) {
        if (ioType == 0) {
            readValue = vm_dos_uart_read_inert((unsigned short)offset);
            vm_dos_trace_register_access("DOSUART_REJECT_READ",
                                         vmId,
                                         port,
                                         offset,
                                         readValue);
            return readValue;
        }

        vm_dos_trace_register_access("DOSUART_REJECT_WRITE",
                                     vmId,
                                     port,
                                     offset,
                                     value & 0xFFUL);
        return 0;
    }

    if (ioType != 0) {
        vm_dos_trace_register_access("DOSUART_WRITE",
                                     vmId,
                                     port,
                                     offset,
                                     value & 0xFFUL);
        vm_dos_uart_write(&g_DosUart.uart,
                          (unsigned short)offset,
                          (BYTE)value,
                          &event);
        if ((event.flags & VM_DOS_UART_EVENT_RX_RESET) != 0UL ||
            (event.flags & VM_DOS_UART_EVENT_TX_RESET) != 0UL) {
            vm_trace_log_line("DOSUART_FIFO_RESET");
        }
        if (event.host_lines_changed) {
            DWORD now;

            now = VM_Get_System_Time();
            vm_modem_poll(&g_Port.modem, now);
            vm_modem_set_host_lines(&g_Port.modem,
                                    event.dtr_asserted,
                                    event.rts_asserted,
                                    now);
            vm_sync_from_modem_core(&g_Port);
        }

        while (vm_dos_uart_pop_tx(&g_DosUart.uart, &txByte)) {
            vm_process_modem_tx(&g_Port, &txByte, 1UL);
        }
        vm_dos_uart_complete_tx(&g_DosUart.uart, 0);
        vm_dos_update_irq_request();
        return 0;
    }

    hadRxData = (vm_dos_uart_get_rx_depth(&g_DosUart.uart) != 0U) ? 1UL : 0UL;
    readValue = vm_dos_uart_read(&g_DosUart.uart, (unsigned short)offset, &event);
    vm_dos_trace_register_access("DOSUART_READ",
                                 vmId,
                                 port,
                                 offset,
                                 readValue);
    if (offset == VM_DOS_UART_REG_DATA && hadRxData != 0UL) {
        if (g_Port.bHoldHelperRxUntilRead) {
            g_Port.bHoldHelperRxUntilRead = FALSE;
        }
    }
    vm_dos_update_irq_request();
    vm_refresh_dos_uart_timer();

    return readValue;
}

void VCOMM_HandleVmDestroyed(DWORD vmId)
{
    vm_dos_handle_vm_destroyed(vmId);
}

DWORD VCOMM_QueryBuildId(void)
{
    return VM_DRIVER_BUILD_ID;
}

DWORD VCOMM_QueryDefaultModemStatus(void)
{
    return VM_DEFAULT_MODEM_STATUS;
}

DWORD VCOMM_QueryCurrentModemStatus(void)
{
    return g_Port.dwModemStatus;
}

DWORD VCOMM_QueryFrontendOwner(void)
{
    return vm_query_frontend_owner();
}

DWORD VCOMM_QueryPortOpen(void)
{
    return g_Port.bOpen ? 1UL : 0UL;
}

const char *VCOMM_QueryBuildTag(void)
{
    return g_BuildTag;
}

const char *VCOMM_QueryPortName(void)
{
    return g_Port.szPortName;
}

DWORD VCOMM_QueryDevNode(void)
{
    return g_Port.dwDevNode;
}

DWORD VCOMM_QueryAllocBase(void)
{
    return g_Port.dwAllocBase;
}

DWORD VCOMM_QueryAllocIrq(void)
{
    return g_Port.dwAllocIrq;
}

DWORD VCOMM_QueryContentionHandler(void)
{
    return g_Port.dwContentionHandler;
}

DWORD VCOMM_QueryContentionResource(void)
{
    return g_Port.dwContentionResource;
}

DWORD VCOMM_QueryHelperAttached(void)
{
    return g_bHelperClaimed ? 1UL : 0UL;
}

DWORD VCOMM_QueryHelperGeneration(void)
{
    return g_helperGeneration;
}

DWORD VCOMM_QueryActiveSessionId(void)
{
    return vm_modem_get_active_session_id(&g_Port.modem);
}

DWORD VCOMM_QueryRawModeEnabled(void)
{
    return vm_modem_get_raw_mode_enabled(&g_Port.modem) ? 1UL : 0UL;
}

DWORD VCOMM_QueryS0AutoAnswerRings(void)
{
    return vm_modem_get_s0_auto_answer_rings(&g_Port.modem);
}

DWORD VCOMM_QueryS1RingCount(void)
{
    return vm_modem_get_s1_ring_count(&g_Port.modem);
}

DWORD VCOMM_QueryHelperQueueDepth(void)
{
    return g_ToHelperQueue.count;
}

DWORD VCOMM_QueryLastMsgToHelper(void)
{
    return g_lastMsgToHelper;
}

DWORD VCOMM_QueryLastMsgFromHelper(void)
{
    return g_lastMsgFromHelper;
}

void VCOMM_QueryDosUartDiagnostic(VMODEM_DOS_UART_DIAGNOSTIC *diag)
{
    DWORD ownerType;
    DWORD ownerVmId;

    ownerType = vm_query_frontend_owner();
    ownerVmId = g_DosUart.bOwnerActive ? g_DosUart.dwOwnerVmId : 0UL;
    vm_dos_uart_fill_diagnostic(&g_DosUart.uart, ownerType, ownerVmId, diag);
}

DWORD VCOMM_DrainTraceLog(char *buffer, DWORD capacity, DWORD *pDroppedCount)
{
    DWORD copied;

    copied = g_traceLogLen;
    if (copied > capacity) {
        copied = capacity;
    }

    if (buffer != 0 && copied != 0) {
        vm_copy_bytes(buffer, g_traceLog, copied);
    }

    if (pDroppedCount != 0) {
        *pDroppedCount = g_traceLogDropped;
    }

    g_traceLogLen = 0;
    g_traceLogDropped = 0;
    g_traceLog[0] = '\0';
    return copied;
}

DWORD VCOMM_QueryHookInstalled(void)
{
    return g_bIfsHookInstalled ? 1UL : 0UL;
}

DWORD VCOMM_QueryHookFireCount(void)
{
    return g_ifsHookFireCount;
}

DWORD VCOMM_QueryPortOpenCount(void)
{
    return g_portOpenCount;
}

DWORD VCOMM_QueryHookLogLen(void)
{
    return g_portOpenLogLen;
}

const char *VCOMM_QueryHookLog(void)
{
    return g_portOpenLog;
}

DWORD VCOMM_QueryHookCaptureEnabled(void)
{
    return g_bHookCaptureEnabled ? 1UL : 0UL;
}

DWORD VCOMM_QueryHookCaptureGeneration(void)
{
    return g_hookCaptureGeneration;
}

DWORD VCOMM_QueryHookCaptureFireCount(void)
{
    return g_hookCaptureFireCount;
}

DWORD VCOMM_QueryHookCapturePortOpenCount(void)
{
    return g_hookCapturePortOpenCount;
}

DWORD VCOMM_QueryHookCaptureLogLen(void)
{
    return g_hookCaptureLogLen;
}

DWORD VCOMM_QueryHookCaptureOtherFnCount(void)
{
    return g_hookCaptureOtherFnCount;
}

const char *VCOMM_QueryHookCaptureLog(void)
{
    return g_hookCaptureLog;
}

const DWORD *VCOMM_QueryHookCaptureFnCounts(void)
{
    return g_hookCaptureFnCounts;
}

void VCOMM_ResetHookCapture(void)
{
    vm_reset_hook_capture_state();
}

void VCOMM_SetHookCaptureEnabled(BOOL enabled)
{
    g_bHookCaptureEnabled = enabled ? TRUE : FALSE;
}

void __cdecl VM_DriverControl(DWORD fCode,
                              DWORD DevNode,
                              DWORD DCRefData,
                              DWORD AllocBase,
                              DWORD AllocIrq,
                              char *PortName)
{
    (void)DevNode;
    (void)AllocBase;
    (void)AllocIrq;

    if (fCode != DC_Initialize) {
        VTRACE("VMODEM: DriverControl unexpected function\r\n");
        return;
    }

    if (PortName == 0 || PortName[0] == '\0') {
        PortName = VM_DEFAULT_PORT_NAME;
    }

    vm_initialize_port_instance(&g_Port,
                                DevNode,
                                DCRefData,
                                AllocBase,
                                AllocIrq,
                                PortName);

    if (!VM_VCOMM_Add_Port(DCRefData,
                           (PFN)VM_PortOpen_Thunk,
                           g_Port.szPortName)) {
        VTRACE("VMODEM: VCOMM_Add_Port failed\r\n");
        return;
    }

    vm_refresh_contention_info(&g_Port);

    VTRACE("VMODEM: DriverControl added VCOMM port\r\n");
}

DWORD __cdecl VM_PortOpen(char *PortName, DWORD VMId, DWORD *lpError)
{
    char line[96];
    int  pos;
    DWORD now;

    ++g_portOpenCount;
    vm_log_append_active(PortName != 0 ? PortName : "(null)");
    if (g_bHookCaptureEnabled) {
        ++g_hookCapturePortOpenCount;
    }

    if (lpError != 0) {
        *lpError = 0;
    }

    if (!vm_port_name_matches(g_Port.szPortName, PortName)) {
        if (lpError != 0) {
            *lpError = IE_BADID;
        }
        return 0;
    }

    if (g_Port.bOpen) {
        if (lpError != 0) {
            *lpError = IE_OPEN;
        }
        return 0;
    }

    if (g_DosUart.bOwnerActive) {
        if (lpError != 0) {
            *lpError = IE_OPEN;
        }
        vm_trace_log_line("PORT_OPEN_BLOCKED owner=DOSUART");
        return 0;
    }

    g_Port.bOpen = TRUE;
    g_Port.dwVMId = VMId;
    g_Port.pd.dwCallerVMId = VMId;
    g_Port.pd.dwCommError = 0;
    g_Port.pd.dwLastError = 0;
    g_Port.pd.dwDetectedEvents = 0;
    vm_use_internal_queues(&g_Port);
    vm_reset_callbacks(&g_Port);
    g_Port.lpMSRShadow = &g_Port.pd.bMSRShadow;
    g_Port.lpEventMaskLoc = &g_Port.pd.dwDetectedEvents;
    g_Port.pd.ValidPortData = VM_PORT_SIGNATURE;
    vm_trace_log_reset_command_capture();
    now = VM_Get_System_Time();
    vm_modem_port_open(&g_Port.modem, now);
    vm_modem_set_host_lines(&g_Port.modem,
                            g_Port.bDtrAsserted ? 1 : 0,
                            g_Port.bRtsAsserted ? 1 : 0,
                            now);
    vm_sync_from_modem_core(&g_Port);
    vm_flush_rx_notify(&g_Port);

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "PORT_OPEN name=");
    vm_buf_append_str(line, &pos, sizeof(line),
                      (PortName != 0) ? PortName : "(null)");
    vm_buf_append_str(line, &pos, sizeof(line), " vmid=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), VMId);
    vm_trace_log_line(line);
    VTRACE("VMODEM: PortOpen\r\n");
    return (DWORD)&g_Port;
}

BOOL __cdecl VM_PortSetCommState(DWORD hPort, _DCB *pDcb, DWORD ActionMask)
{
    VModemPort *port;
    char line[160];
    int  pos;
    DWORD mask;

    port = vm_port_from_hport(hPort);
    if (port == 0 || pDcb == 0) {
        return FALSE;
    }

    mask = ActionMask;
    if (mask == 0xFFFFFFFFUL) {
        mask = VM_FULL_ACTION_MASK;
    }

    if (mask & fBaudRate) {
        port->dcb.BaudRate = pDcb->BaudRate;
    }
    if (mask & fBitMask) {
        port->dcb.BitMask = pDcb->BitMask;
    }
    if (mask & fXonLim) {
        port->dcb.XonLim = pDcb->XonLim;
    }
    if (mask & fXoffLim) {
        port->dcb.XoffLim = pDcb->XoffLim;
    }
    if (mask & fByteSize) {
        port->dcb.ByteSize = pDcb->ByteSize;
    }
    if (mask & fbParity) {
        port->dcb.Parity = pDcb->Parity;
    }
    if (mask & fStopBits) {
        port->dcb.StopBits = pDcb->StopBits;
    }
    if (mask & fXonChar) {
        port->dcb.XonChar = pDcb->XonChar;
    }
    if (mask & fXoffChar) {
        port->dcb.XoffChar = pDcb->XoffChar;
    }
    if (mask & fErrorChar) {
        port->dcb.ErrorChar = pDcb->ErrorChar;
    }
    if (mask & fEofChar) {
        port->dcb.EofChar = pDcb->EofChar;
    }
    if (mask & fEvtChar1) {
        port->dcb.EvtChar1 = pDcb->EvtChar1;
    }
    if (mask & fEvtChar2) {
        port->dcb.EvtChar2 = pDcb->EvtChar2;
    }
    if (mask & fRlsTimeout) {
        port->dcb.RlsTimeout = pDcb->RlsTimeout;
    }
    if (mask & fCtsTimeout) {
        port->dcb.CtsTimeout = pDcb->CtsTimeout;
    }
    if (mask & fDsrTimeout) {
        port->dcb.DsrTimeout = pDcb->DsrTimeout;
    }
    if (mask & fTxDelay) {
        port->dcb.TxDelay = pDcb->TxDelay;
    }

    port->dcb.DCBLength = sizeof(port->dcb);
    vm_sync_host_lines_from_dcb(port);
    vm_sync_host_lines_to_modem(port);
    port->pd.dwLastError = 0;

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "SET_COMM_STATE mask=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), mask);
    vm_buf_append_str(line, &pos, sizeof(line), " baud=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line),
                          vm_ring0_baud_to_user_baud(port->dcb.BaudRate));
    vm_buf_append_str(line, &pos, sizeof(line), " bitmask=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), port->dcb.BitMask);
    vm_buf_append_str(line, &pos, sizeof(line), " evt1=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line),
                          (DWORD)(BYTE)port->dcb.EvtChar1);
    vm_buf_append_str(line, &pos, sizeof(line), " evt2=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line),
                          (DWORD)(BYTE)port->dcb.EvtChar2);
    vm_trace_log_line(line);
    VTRACE("VMODEM: SetCommState\r\n");
    return TRUE;
}

BOOL __cdecl VM_PortGetCommState(DWORD hPort, _DCB *pDcb)
{
    VModemPort *port;

    port = vm_port_from_hport(hPort);
    if (port == 0 || pDcb == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    vm_copy_bytes(pDcb, &port->dcb, sizeof(*pDcb));
    port->pd.dwLastError = 0;
    return TRUE;
}

BOOL __cdecl VM_PortSetup(DWORD hPort,
                          BYTE *RxBase,
                          DWORD RxLength,
                          BYTE *TxBase,
                          DWORD TxLength)
{
    VModemPort *port;
    char line[128];
    int  pos;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    port->pd.QInAddr = (DWORD)((RxBase != 0) ? RxBase : g_RxBuf);
    port->pd.QInSize = (RxLength != 0) ? RxLength : VM_QUEUE_SIZE;
    port->pd.QOutAddr = (DWORD)((TxBase != 0) ? TxBase : g_TxBuf);
    port->pd.QOutSize = (TxLength != 0) ? TxLength : VM_QUEUE_SIZE;
    vm_reset_queue_state(&port->pd);
    vm_sync_from_modem_core(port);
    port->pd.dwLastError = 0;

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "PORT_SETUP rx=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), port->pd.QInSize);
    vm_buf_append_str(line, &pos, sizeof(line), " tx=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), port->pd.QOutSize);
    vm_trace_log_line(line);
    VTRACE("VMODEM: PortSetup\r\n");
    return TRUE;
}

BOOL __cdecl VM_PortTransmitChar(DWORD hPort, char ch)
{
    VModemPort *port;
    BYTE byteValue;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    byteValue = (BYTE)ch;
    port->pd.dwLastError = 0;
    vm_process_modem_tx(port, &byteValue, 1);
    vm_stage_tx_bytes(port, 1);
    vm_complete_tx_bytes(port, 1);
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortClose(DWORD hPort)
{
    VModemPort *port;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    vm_prepare_closed_state(port);
    vm_use_internal_queues(port);
    vm_trace_log_line("PORT_CLOSE");
    vm_trace_log_reset_command_capture();
    VTRACE("VMODEM: PortClose\r\n");
    return TRUE;
}

BOOL __cdecl VM_PortGetQueueStatus(DWORD hPort, _COMSTAT *pComstat)
{
    VModemPort *port;

    port = vm_port_from_hport(hPort);
    if (port == 0 || pComstat == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    vm_fill_comstat(port, pComstat);
    port->pd.dwLastError = 0;
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortClearError(DWORD hPort, _COMSTAT *pComstat, DWORD *pError)
{
    VModemPort *port;

    port = vm_port_from_hport(hPort);
    if (port == 0 || pError == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    if (pComstat != 0) {
        vm_fill_comstat(port, pComstat);
    }

    *pError = port->pd.dwCommError;
    port->pd.dwCommError = 0;
    port->pd.dwLastError = 0;
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortSetModemStatusShadow(DWORD hPort, BYTE *MSRShadow)
{
    VModemPort *port;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    port->lpMSRShadow = (MSRShadow != 0) ? MSRShadow : &port->pd.bMSRShadow;
    port->pd.dwLastError = 0;
    vm_update_msr_shadow(port);
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortGetProperties(DWORD hPort, _COMMPROP *pCommprop)
{
    VModemPort *port;

    port = vm_port_from_hport(hPort);
    if (port == 0 || pCommprop == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    vm_fill_commprop(port, pCommprop);
    port->pd.dwLastError = 0;
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortEscapeFunction(DWORD hPort,
                                   DWORD function,
                                   DWORD inData,
                                   DWORD outData)
{
    VModemPort *port;
    char line[64];
    int  pos;

    (void)inData;
    (void)outData;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    port->pd.dwLastError = 0;

    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "ESCAPE fn=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), function);

    switch (function) {
    case SETRTS:
        port->bRtsAsserted = TRUE;
        port->dcb.BitMask &= ~(fRTSDisable | fRTSFlow | fRTSToggle);
        port->dcb.BitMask |= fRTSEnable;
        vm_sync_host_lines_to_modem(port);
        vm_buf_append_str(line, &pos, sizeof(line), " name=SETRTS");
        vm_trace_log_line(line);
        VTRACE("VMODEM: Escape SETRTS\r\n");
        vm_flush_rx_notify(port);
        return TRUE;

    case CLRRTS:
        port->bRtsAsserted = FALSE;
        port->dcb.BitMask &= ~(fRTSFlow | fRTSEnable | fRTSToggle);
        port->dcb.BitMask |= fRTSDisable;
        vm_sync_host_lines_to_modem(port);
        vm_buf_append_str(line, &pos, sizeof(line), " name=CLRRTS");
        vm_trace_log_line(line);
        VTRACE("VMODEM: Escape CLRRTS\r\n");
        vm_flush_rx_notify(port);
        return TRUE;

    case SETDTR:
        port->bDtrAsserted = TRUE;
        port->dcb.BitMask &= ~(fDTRDisable | fDTRFlow);
        port->dcb.BitMask |= fDtrEnable;
        vm_sync_host_lines_to_modem(port);
        vm_buf_append_str(line, &pos, sizeof(line), " name=SETDTR");
        vm_trace_log_line(line);
        VTRACE("VMODEM: Escape SETDTR\r\n");
        vm_flush_rx_notify(port);
        return TRUE;

    case CLRDTR:
        port->bDtrAsserted = FALSE;
        port->dcb.BitMask &= ~(fDTRFlow | fDtrEnable);
        port->dcb.BitMask |= fDTRDisable;
        vm_sync_host_lines_to_modem(port);
        vm_buf_append_str(line, &pos, sizeof(line), " name=CLRDTR");
        vm_trace_log_line(line);
        VTRACE("VMODEM: Escape CLRDTR\r\n");
        vm_flush_rx_notify(port);
        return TRUE;

    case SETXOFF:
        vm_poll_modem_core(port);
        vm_buf_append_str(line, &pos, sizeof(line), " name=SETXOFF");
        vm_trace_log_line(line);
        VTRACE("VMODEM: Escape SETXOFF (no-op)\r\n");
        return TRUE;

    case SETXON:
        vm_poll_modem_core(port);
        vm_buf_append_str(line, &pos, sizeof(line), " name=SETXON");
        vm_trace_log_line(line);
        VTRACE("VMODEM: Escape SETXON (no-op)\r\n");
        return TRUE;

    case RESETDEV:
        vm_poll_modem_core(port);
        vm_buf_append_str(line, &pos, sizeof(line), " name=RESETDEV");
        vm_trace_log_line(line);
        VTRACE("VMODEM: Escape RESETDEV (no-op)\r\n");
        return TRUE;

    case SETBREAK:
        vm_poll_modem_core(port);
        vm_buf_append_str(line, &pos, sizeof(line), " name=SETBREAK");
        vm_trace_log_line(line);
        VTRACE("VMODEM: Escape SETBREAK (no-op)\r\n");
        return TRUE;

    case CLEARBREAK:
        vm_poll_modem_core(port);
        vm_buf_append_str(line, &pos, sizeof(line), " name=CLEARBREAK");
        vm_trace_log_line(line);
        VTRACE("VMODEM: Escape CLEARBREAK (no-op)\r\n");
        return TRUE;

    default:
        port->pd.dwLastError = IE_EXTINVALID;
        return FALSE;
    }
}

BOOL __cdecl VM_PortPurge(DWORD hPort, DWORD dwFlags)
{
    VModemPort *port;
    char line[64];
    int  pos;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    if ((dwFlags & (PURGE_RXABORT | PURGE_RXCLEAR)) != 0) {
        port->pd.QInCount = 0;
        port->pd.QInGet = 0;
        port->pd.QInPut = 0;
        vm_modem_clear_output(&port->modem);
        vm_reset_held_helper_rx(port);
        vm_update_rx_notify_armed(port);
    }
    if ((dwFlags & (PURGE_TXABORT | PURGE_TXCLEAR)) != 0) {
        port->pd.QOutCount = 0;
        port->pd.QOutGet = 0;
        port->pd.QOutPut = 0;
        vm_notify_tx_ready(port);
        vm_record_event(port, EV_TXEMPTY);
    }

    port->pd.dwLastError = 0;
    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "PORT_PURGE flags=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), dwFlags);
    vm_trace_log_line(line);
    VTRACE("VMODEM: PortPurge\r\n");
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortSetEventMask(DWORD hPort, DWORD EventMask, DWORD *EventMaskLoc)
{
    VModemPort *port;
    char line[96];
    int  pos;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    port->dwEvtMask = EventMask;
    port->pd.dwClientEventMask = EventMask;
    if (EventMaskLoc != 0) {
        port->lpEventMaskLoc = EventMaskLoc;
    } else {
        port->lpEventMaskLoc = &port->pd.dwDetectedEvents;
    }
    port->pd.dwDetectedEvents &= EventMask;
    if (port->lpEventMaskLoc != 0) {
        *(port->lpEventMaskLoc) = port->pd.dwDetectedEvents;
    }
    port->pd.lpClientEventNotify = (DWORD)port->lpEventMaskLoc;
    port->pd.dwLastError = 0;
    if ((port->pd.dwDetectedEvents & port->dwEvtMask) != 0) {
        vm_invoke_client_callback((DWORD)port->lpNotifyCallback,
                                  (DWORD)port,
                                  port->dwNotifyRefData,
                                  CN_EVENT,
                                  port->pd.dwDetectedEvents & port->dwEvtMask);
    }
    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "SET_EVENT_MASK mask=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), EventMask);
    vm_buf_append_str(line, &pos, sizeof(line), " loc=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), (DWORD)EventMaskLoc);
    vm_trace_log_line(line);
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortGetEventMask(DWORD hPort, DWORD EventMask, DWORD *OldEventMask)
{
    VModemPort *port;
    char line[96];
    int  pos;
    DWORD oldMask;

    port = vm_port_from_hport(hPort);
    if (port == 0 || OldEventMask == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    oldMask = (port->lpEventMaskLoc != 0) ? *(port->lpEventMaskLoc)
                                          : port->pd.dwDetectedEvents;
    if (port->lpEventMaskLoc != 0) {
        *(port->lpEventMaskLoc) = oldMask & (~EventMask);
    }
    port->pd.dwDetectedEvents = oldMask & (~EventMask);
    *OldEventMask = oldMask;
    port->pd.dwLastError = 0;
    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "GET_EVENT_MASK clear=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), EventMask);
    vm_buf_append_str(line, &pos, sizeof(line), " old=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), oldMask);
    vm_trace_log_line(line);
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortWrite(DWORD hPort,
                          char *lpBuf,
                          DWORD count,
                          DWORD *pWritten)
{
    VModemPort *port;
    char line[64];
    int  pos;

    port = vm_port_from_hport(hPort);
    if (port == 0 || pWritten == 0 || (count != 0 && lpBuf == 0)) {
        return FALSE;
    }

    *pWritten = count;
    vm_process_modem_tx(port, (const BYTE *)lpBuf, count);
    vm_stage_tx_bytes(port, count);
    vm_complete_tx_bytes(port, count);
    port->pd.dwLastError = 0;
    if (count != 0) {
        pos = 0;
        line[0] = '\0';
        vm_buf_append_str(line, &pos, sizeof(line), "PORT_WRITE count=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), count);
        vm_trace_log_line(line);
    }
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortRead(DWORD hPort,
                         char *lpBuf,
                         DWORD count,
                         DWORD *pRead)
{
    VModemPort *port;
    char line[64];
    int  pos;

    port = vm_port_from_hport(hPort);
    if (port == 0 || pRead == 0 || (count != 0 && lpBuf == 0)) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    if (!vm_copy_rx_queue_out(port, lpBuf, count, pRead)) {
        return FALSE;
    }

    if (*pRead != 0 && port->bHoldHelperRxUntilRead) {
        port->bHoldHelperRxUntilRead = FALSE;
    }

    vm_sync_from_modem_core(port);
    port->pd.dwLastError = 0;
    if (*pRead != 0) {
        pos = 0;
        line[0] = '\0';
        vm_buf_append_str(line, &pos, sizeof(line), "PORT_READ count=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), count);
        vm_buf_append_str(line, &pos, sizeof(line), " read=");
        vm_buf_append_u32_dec(line, &pos, sizeof(line), *pRead);
        vm_trace_log_line(line);
        vm_trace_log_port_read_bytes((const BYTE *)lpBuf, *pRead);
    }
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortEnableNotification(DWORD hPort, DWORD lpFunc, DWORD RefData)
{
    VModemPort *port;
    char line[96];
    int  pos;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    port->lpNotifyCallback = (VM_NOTIFY_CALLBACK)lpFunc;
    port->dwNotifyRefData = RefData;
    port->pd.dwClientRefData = RefData;
    port->pd.dwLastError = 0;
    if ((port->pd.dwDetectedEvents & port->dwEvtMask) != 0) {
        vm_invoke_client_callback((DWORD)port->lpNotifyCallback,
                                  (DWORD)port,
                                  port->dwNotifyRefData,
                                  CN_EVENT,
                                  port->pd.dwDetectedEvents & port->dwEvtMask);
    }
    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "ENABLE_NOTIFY cb=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), lpFunc);
    vm_buf_append_str(line, &pos, sizeof(line), " ref=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), RefData);
    vm_trace_log_line(line);
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortSetReadCallBack(DWORD hPort,
                                    DWORD RxTrigger,
                                    DWORD RxCallBack,
                                    DWORD RxRefData)
{
    VModemPort *port;
    char line[128];
    int  pos;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    port->dwRxTrigger = (RxCallBack != 0) ? RxTrigger : 0xFFFFFFFFUL;
    port->lpRxCallback = (VM_RW_CALLBACK)RxCallBack;
    port->dwRxRefData = RxRefData;
    port->pd.lpClientReadNotify = RxCallBack;
    port->pd.dwLastError = 0;
    if (port->lpRxCallback == 0 || port->dwRxTrigger == 0xFFFFFFFFUL) {
        port->bRxNotifyArmed = FALSE;
        port->bRxNotifyPending = FALSE;
    } else {
        port->bRxNotifyArmed = TRUE;
        vm_notify_rx_available(port);
        if (port->pd.QInCount < port->dwRxTrigger) {
            port->bRxNotifyArmed = TRUE;
        }
    }
    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "SET_READ_CALLBACK trigger=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), port->dwRxTrigger);
    vm_buf_append_str(line, &pos, sizeof(line), " cb=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), RxCallBack);
    vm_buf_append_str(line, &pos, sizeof(line), " ref=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), RxRefData);
    vm_trace_log_line(line);
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortSetWriteCallBack(DWORD hPort,
                                     DWORD TxTrigger,
                                     DWORD TxCallBack,
                                     DWORD TxRefData)
{
    VModemPort *port;
    char line[128];
    int  pos;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    port->dwTxTrigger = (TxCallBack != 0) ? TxTrigger : 0;
    if (port->dwTxTrigger != 0 &&
        port->pd.QOutSize != 0 &&
        port->dwTxTrigger > port->pd.QOutSize) {
        port->dwTxTrigger = port->pd.QOutSize;
    }
    port->lpTxCallback = (VM_RW_CALLBACK)TxCallBack;
    port->dwTxRefData = TxRefData;
    port->bTxNotifyArmed = FALSE;
    port->pd.lpClientWriteNotify = TxCallBack;
    port->pd.dwLastError = 0;
    vm_update_tx_notify_armed(port);
    pos = 0;
    line[0] = '\0';
    vm_buf_append_str(line, &pos, sizeof(line), "SET_WRITE_CALLBACK trigger=");
    vm_buf_append_u32_dec(line, &pos, sizeof(line), port->dwTxTrigger);
    vm_buf_append_str(line, &pos, sizeof(line), " cb=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), TxCallBack);
    vm_buf_append_str(line, &pos, sizeof(line), " ref=");
    vm_buf_append_u32_hex(line, &pos, sizeof(line), TxRefData);
    vm_trace_log_line(line);
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortGetModemStatus(DWORD hPort, DWORD *pModemStatus)
{
    VModemPort *port;

    port = vm_port_from_hport(hPort);
    if (port == 0 || pModemStatus == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    *pModemStatus = port->dwModemStatus;
    port->pd.dwLastError = 0;
    vm_update_msr_shadow(port);
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortGetCommConfig(DWORD hPort, VM_COMMCONFIG *lpCC, DWORD *lpSize)
{
    VModemPort *port;
    DWORD callerSize;
    DWORD requiredSize;

    port = vm_port_from_hport(hPort);
    if (port == 0 || lpSize == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    requiredSize = sizeof(VM_COMMCONFIG);
    callerSize = *lpSize;
    *lpSize = requiredSize;
    port->pd.dwLastError = 0;

    if (lpCC == 0 || callerSize < requiredSize) {
        vm_flush_rx_notify(port);
        return TRUE;
    }

    vm_zero_bytes(lpCC, sizeof(VM_COMMCONFIG));
    lpCC->dwSize = requiredSize;
    lpCC->wVersion = VM_COMMCONFIG_VERSION;
    lpCC->wAlignDCB = 0;
    VM_VCOMM_Map_Ring0DCB_To_Win32(&port->dcb, &lpCC->dcb);
    lpCC->dwProviderSubType = PST_RS232;
    lpCC->dwProviderOffset = 0;
    lpCC->dwProviderSize = 0;
    lpCC->wcProviderData[0] = 0;
    vm_flush_rx_notify(port);
    return TRUE;
}

BOOL __cdecl VM_PortSetCommConfig(DWORD hPort, const VM_COMMCONFIG *lpCC, DWORD dwSize)
{
    VModemPort *port;
    _DCB ring0;

    port = vm_port_from_hport(hPort);
    if (port == 0) {
        return FALSE;
    }

    if (lpCC == 0 || dwSize < sizeof(VM_COMMCONFIG)) {
        port->pd.dwLastError = IE_INVALIDPARAM;
        return FALSE;
    }

    VM_VCOMM_Map_Win32DCB_To_Ring0((VM_WIN32_DCB *)&lpCC->dcb, &ring0);
    return VM_PortSetCommState(hPort, &ring0, VM_FULL_ACTION_MASK);
}

BOOL __cdecl VM_PortGetError(DWORD hPort, DWORD *pError)
{
    VModemPort *port;

    port = vm_port_from_hport(hPort);
    if (port == 0 || pError == 0) {
        return FALSE;
    }

    vm_poll_modem_core(port);
    *pError = port->pd.dwLastError;
    port->pd.dwLastError = 0;
    vm_flush_rx_notify(port);
    return TRUE;
}
