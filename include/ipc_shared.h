/*
 * ipc_shared.h - Shared IPC message definitions for the VMODEM VxD and its
 *                user-mode helper.
 *
 * Must compile under both VC6 (VxD DDK, plain C89) and TDM-GCC (helper,
 * plain C89). No C99 types, no stdint.h, no // comments, no bool.
 *
 * IOCTL code layout (CTL_CODE style):
 *   DeviceType = 0x22 (FILE_DEVICE_UNKNOWN)
 *   Method     = 0    (METHOD_BUFFERED)
 *   Access     = 0    (FILE_ANY_ACCESS)
 *   Function   = 0x800 + N  (private range)
 *
 *   CTL_CODE(0x22, F, 0, 0) = (0x22 << 16) | (F << 2) = 0x00220000 | (F << 2)
 */
#ifndef VMODEM_IPC_SHARED_H
#define VMODEM_IPC_SHARED_H

/* VMODEM_IOCTL_HELLO: helper claims the active IPC role and receives the
 * negotiated helper generation plus protocol limits.
 * CTL_CODE(0x22, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS) = 0x00222000 */
#define VMODEM_IOCTL_HELLO         0x00222000UL
/* VMODEM_IOCTL_QUERY_DRIVER: helper requests loaded-driver diagnostics.
 * CTL_CODE(0x22, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS) = 0x00222004 */
#define VMODEM_IOCTL_QUERY_DRIVER  0x00222004UL
/* VMODEM_IOCTL_GET_HOOK_LOG: query IFS hook state and PortOpen name log.
 * CTL_CODE(0x22, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS) = 0x00222008 */
#define VMODEM_IOCTL_GET_HOOK_LOG  0x00222008UL
/* VMODEM_IOCTL_HOOK_CAPTURE_CONTROL: start/stop/reset the isolated IFS capture
 * window used for manual reproductions.
 * CTL_CODE(0x22, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS) = 0x0022200C */
#define VMODEM_IOCTL_HOOK_CAPTURE_CONTROL 0x0022200CUL
/* VMODEM_IOCTL_SUBMIT_MESSAGE: helper submits a protocol message to the VxD.
 * CTL_CODE(0x22, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS) = 0x00222010 */
#define VMODEM_IOCTL_SUBMIT_MESSAGE 0x00222010UL
/* VMODEM_IOCTL_RECEIVE_MESSAGE: helper receives the next protocol message
 * queued by the VxD.
 * CTL_CODE(0x22, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS) = 0x00222014 */
#define VMODEM_IOCTL_RECEIVE_MESSAGE 0x00222014UL
/* VMODEM_IOCTL_GET_TRACE_LOG: helper drains the pending driver trace spool
 * used for session diagnostics.
 * CTL_CODE(0x22, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS) = 0x00222018 */
#define VMODEM_IOCTL_GET_TRACE_LOG 0x00222018UL

#define VMODEM_IPC_VERSION           0x00000005UL
#define VMODEM_STATUS_OK             0x00000000UL
#define VMODEM_STATUS_BUSY           0x00000001UL
#define VMODEM_STATUS_NO_MESSAGE     0x00000002UL
#define VMODEM_STATUS_STALE          0x00000003UL
#define VMODEM_STATUS_BAD_STATE      0x00000004UL
#define VMODEM_STATUS_BAD_MESSAGE    0x00000005UL
#define VMODEM_STATUS_NOT_OWNER      0x00000006UL
#define VMODEM_BUILD_TAG_LEN         32U
#define VMODEM_HOOK_FN_BUCKETS       16U
#define VMODEM_IPC_MAX_PAYLOAD      256U
#define VMODEM_TRACE_LOG_DATA_LEN  2048U

#define VMODEM_HOOK_CAPTURE_ACTION_RESET 1UL
#define VMODEM_HOOK_CAPTURE_ACTION_START 2UL
#define VMODEM_HOOK_CAPTURE_ACTION_STOP  3UL

#define VMODEM_MSG_NONE          0UL
#define VMODEM_MSG_CONNECT_REQ   1UL
#define VMODEM_MSG_CONNECT_OK    2UL
#define VMODEM_MSG_CONNECT_FAIL  3UL
#define VMODEM_MSG_DATA_TO_NET   4UL
#define VMODEM_MSG_DATA_TO_SERIAL 5UL
#define VMODEM_MSG_ANSWER_REQ    6UL
#define VMODEM_MSG_HANGUP_REQ    7UL
#define VMODEM_MSG_INBOUND_RING  8UL
#define VMODEM_MSG_REMOTE_CLOSED 9UL

#define VMODEM_CONNECT_FAIL_NONE     0UL
#define VMODEM_CONNECT_FAIL_TIMEOUT  1UL
#define VMODEM_CONNECT_FAIL_DNS      2UL
#define VMODEM_CONNECT_FAIL_REFUSED  3UL
#define VMODEM_CONNECT_FAIL_NETWORK  4UL
#define VMODEM_CONNECT_FAIL_LOCAL    5UL

#define VMODEM_CONNECT_FLAG_RAW      0x00000001UL

#define VMODEM_FRONTEND_OWNER_NONE   0UL
#define VMODEM_FRONTEND_OWNER_VCOMM  1UL
#define VMODEM_FRONTEND_OWNER_DOS    2UL

#define VMODEM_DOS_UART_IRQ_NONE          0UL
#define VMODEM_DOS_UART_IRQ_MODEM_STATUS  0x00000001UL
#define VMODEM_DOS_UART_IRQ_THR_EMPTY     0x00000002UL
#define VMODEM_DOS_UART_IRQ_RX_DATA       0x00000004UL
#define VMODEM_DOS_UART_IRQ_LINE_STATUS   0x00000008UL
#define VMODEM_DOS_UART_IRQ_FIFO_TIMEOUT  0x00000010UL

#pragma pack(push, 1)

typedef struct {
    unsigned long enabled;          /* non-zero when DOS UART is configured */
    unsigned long owner_type;       /* VMODEM_FRONTEND_OWNER_* */
    unsigned long owner_vm_id;      /* owning DOS VM, if any */
    unsigned long base_port;        /* DOS-visible UART base address */
    unsigned long irq_number;       /* DOS-visible IRQ */
    unsigned long irq_asserted;     /* non-zero when virtual IRQ is asserted */
    unsigned long pending_irq;      /* VMODEM_DOS_UART_IRQ_* bitmask */
    unsigned long rx_fifo_depth;    /* current RX FIFO bytes */
    unsigned long tx_fifo_depth;    /* current TX staging bytes */
    unsigned char  ier;
    unsigned char  iir;
    unsigned char  fcr;
    unsigned char  lcr;
    unsigned char  mcr;
    unsigned char  lsr;
    unsigned char  msr;
    unsigned char  scr;
    unsigned char  dll;
    unsigned char  dlm;
    unsigned short rx_interrupt_threshold;
    unsigned long  loopback_active;
} VMODEM_DOS_UART_DIAGNOSTIC;

/* Sent by the helper in the input buffer of VMODEM_IOCTL_HELLO. */
typedef struct {
    unsigned long version;   /* must be VMODEM_IPC_VERSION */
    unsigned long reserved;
} VMODEM_HELLO;

/* Returned by the VxD in the output buffer of VMODEM_IOCTL_HELLO. */
typedef struct {
    unsigned long status;             /* VMODEM_STATUS_* */
    unsigned long protocol_version;   /* currently VMODEM_IPC_VERSION */
    unsigned long helper_generation;  /* current claimed helper epoch */
    unsigned long max_payload;        /* maximum payload bytes per message */
} VMODEM_HELLO_ACK;

/* Sent by the helper in the input buffer of VMODEM_IOCTL_QUERY_DRIVER. */
typedef struct {
    unsigned long version;   /* must be VMODEM_IPC_VERSION */
    unsigned long reserved;
} VMODEM_QUERY_DRIVER;

/* Returned by the VxD in the output buffer of VMODEM_IOCTL_QUERY_DRIVER. */
typedef struct {
    unsigned long status;         /* VMODEM_STATUS_OK on success */
    unsigned long build_id;       /* diagnostic build id compiled into the VxD */
    unsigned long modem_status;   /* current internal modem-status word */
    unsigned long default_status; /* compiled-in default modem-status word */
    unsigned long frontend_owner; /* VMODEM_FRONTEND_OWNER_* */
    unsigned long port_open;      /* non-zero if the VCOMM port is open */
    unsigned long dev_node;       /* current port devnode, if known */
    unsigned long alloc_base;     /* allocated I/O base from VCOMM init */
    unsigned long alloc_irq;      /* allocated IRQ from VCOMM init */
    unsigned long contention_handler;  /* VCOMM/VCD contention handler pointer */
    unsigned long contention_resource; /* mapped contention resource */
    unsigned long contention_requests; /* GET_CONTENTION_HANDLER call count */
    unsigned long helper_attached;     /* non-zero if a helper is claimed */
    unsigned long helper_generation;   /* current claimed helper epoch */
    unsigned long active_session_id;   /* current modem/helper session */
    unsigned long raw_mode_enabled;    /* current outbound RAW mode default */
    unsigned long s0_auto_answer_rings;/* current S0 auto-answer setting */
    unsigned long s1_ring_count;       /* current S1 ring counter */
    unsigned long helper_queue_depth;  /* queued VxD->helper messages */
    unsigned long last_msg_to_helper;  /* last queued outbound message type */
    unsigned long last_msg_from_helper;/* last accepted inbound message type */
    char          build_tag[VMODEM_BUILD_TAG_LEN];
    char          port_name[16];
    VMODEM_DOS_UART_DIAGNOSTIC dos_uart;
} VMODEM_QUERY_DRIVER_ACK;

/* Shared helper/VxD protocol envelope. */
typedef struct {
    unsigned long version;           /* must be VMODEM_IPC_VERSION */
    unsigned long message_type;      /* VMODEM_MSG_* */
    unsigned long helper_generation; /* owner epoch */
    unsigned long session_id;        /* current or pending session */
    unsigned long status;            /* reason/status for this message */
    unsigned long payload_length;    /* valid bytes in payload[] */
    unsigned char payload[VMODEM_IPC_MAX_PAYLOAD];
} VMODEM_PROTOCOL_MESSAGE;

/* Sent by the helper in the input buffer of VMODEM_IOCTL_RECEIVE_MESSAGE. */
typedef struct {
    unsigned long version;           /* must be VMODEM_IPC_VERSION */
    unsigned long helper_generation; /* helper epoch expected by caller */
    unsigned long reserved0;
    unsigned long reserved1;
} VMODEM_RECEIVE_MESSAGE;

/* Returned by the VxD for message submission status. */
typedef struct {
    unsigned long status;            /* VMODEM_STATUS_* */
    unsigned long helper_generation; /* current helper epoch */
    unsigned long session_id;        /* session tied to the submission */
    unsigned long message_type;      /* echoed type for tracing */
} VMODEM_SUBMIT_MESSAGE_ACK;

/* Sent by the helper in the input buffer of VMODEM_IOCTL_GET_HOOK_LOG. */
typedef struct {
    unsigned long version;
    unsigned long reserved;
} VMODEM_GET_HOOK_LOG;

/* Sent by the helper in the input buffer of VMODEM_IOCTL_GET_TRACE_LOG. */
typedef struct {
    unsigned long version;
    unsigned long reserved;
} VMODEM_GET_TRACE_LOG;

/* Sent by the helper in the input buffer of VMODEM_IOCTL_HOOK_CAPTURE_CONTROL. */
typedef struct {
    unsigned long version;
    unsigned long action;
    unsigned long reserved0;
    unsigned long reserved1;
} VMODEM_HOOK_CAPTURE_CONTROL;

/* Size of the PortOpen name log returned by VMODEM_IOCTL_GET_HOOK_LOG.
 * Names are appended as null-terminated strings separated by '|'. */
#define VMODEM_HOOK_LOG_DATA_LEN  480U

/* Returned by the VxD for VMODEM_IOCTL_HOOK_CAPTURE_CONTROL. */
typedef struct {
    unsigned long status;
    unsigned long capture_enabled;
    unsigned long capture_generation;
    unsigned long reserved;
} VMODEM_HOOK_CAPTURE_CONTROL_ACK;

/* Returned by the VxD in the output buffer of VMODEM_IOCTL_GET_HOOK_LOG. */
typedef struct {
    unsigned long status;
    unsigned long hook_installed;   /* 1 if IFS hook is installed */
    unsigned long hook_fire_count;  /* IFS hook invocations since VxD load */
    unsigned long port_open_count;  /* PortOpen calls since VxD load */
    unsigned long log_len;          /* valid bytes in log_data */
    unsigned long capture_enabled;  /* isolated capture window enabled */
    unsigned long capture_generation;    /* increments on reset/start */
    unsigned long capture_hook_fire_count; /* hook calls during capture window */
    unsigned long capture_port_open_count; /* PortOpen calls during capture */
    unsigned long capture_log_len;       /* valid bytes in capture_log_data */
    unsigned long capture_other_fn_count; /* fn values >= BUCKETS during capture */
    unsigned long capture_fn_counts[VMODEM_HOOK_FN_BUCKETS];
    char          log_data[VMODEM_HOOK_LOG_DATA_LEN];
    char          capture_log_data[VMODEM_HOOK_LOG_DATA_LEN];
} VMODEM_GET_HOOK_LOG_ACK;

/* Returned by the VxD in the output buffer of VMODEM_IOCTL_GET_TRACE_LOG. */
typedef struct {
    unsigned long status;
    unsigned long dropped_count; /* trace lines dropped since last drain */
    unsigned long log_len;       /* valid bytes in log_data */
    char          log_data[VMODEM_TRACE_LOG_DATA_LEN];
} VMODEM_GET_TRACE_LOG_ACK;

#pragma pack(pop)

#endif /* VMODEM_IPC_SHARED_H */
