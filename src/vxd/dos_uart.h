/*
 * dos_uart.h - Pure C DOS-box UART model used by the VMODEM VxD and
 *              host-side tests.
 *
 * This header intentionally avoids Win32 and VxD-specific headers so the
 * UART behavior can be unit-tested natively.
 */
#ifndef VMODEM_DOS_UART_H
#define VMODEM_DOS_UART_H

#include "ipc_shared.h"

#define VM_DOS_UART_FIFO_CAPACITY    16U

#define VM_DOS_UART_REG_DATA         0U
#define VM_DOS_UART_REG_IER          1U
#define VM_DOS_UART_REG_IIR_FCR      2U
#define VM_DOS_UART_REG_LCR          3U
#define VM_DOS_UART_REG_MCR          4U
#define VM_DOS_UART_REG_LSR          5U
#define VM_DOS_UART_REG_MSR          6U
#define VM_DOS_UART_REG_SCR          7U

#define VM_DOS_UART_IER_RX_DATA      0x01U
#define VM_DOS_UART_IER_THR_EMPTY    0x02U
#define VM_DOS_UART_IER_MODEM_STATUS 0x08U

#define VM_DOS_UART_FCR_ENABLE       0x01U
#define VM_DOS_UART_FCR_CLEAR_RX     0x02U
#define VM_DOS_UART_FCR_CLEAR_TX     0x04U

#define VM_DOS_UART_LCR_DLAB         0x80U

#define VM_DOS_UART_MCR_DTR          0x01U
#define VM_DOS_UART_MCR_RTS          0x02U
#define VM_DOS_UART_MCR_OUT1         0x04U
#define VM_DOS_UART_MCR_OUT2         0x08U
#define VM_DOS_UART_MCR_LOOP         0x10U

#define VM_DOS_UART_LSR_DR           0x01U
#define VM_DOS_UART_LSR_OE           0x02U
#define VM_DOS_UART_LSR_THRE         0x20U
#define VM_DOS_UART_LSR_TEMT         0x40U

#define VM_DOS_UART_MSR_DCTS         0x01U
#define VM_DOS_UART_MSR_DDSR         0x02U
#define VM_DOS_UART_MSR_TERI         0x04U
#define VM_DOS_UART_MSR_DDCD         0x08U
#define VM_DOS_UART_MSR_CTS          0x10U
#define VM_DOS_UART_MSR_DSR          0x20U
#define VM_DOS_UART_MSR_RI           0x40U
#define VM_DOS_UART_MSR_RLSD         0x80U

#define VM_DOS_UART_IIR_MODEM_STATUS 0x00U
#define VM_DOS_UART_IIR_THR_EMPTY    0x02U
#define VM_DOS_UART_IIR_RX_DATA      0x04U
#define VM_DOS_UART_IIR_NONE         0x01U
#define VM_DOS_UART_IIR_FIFO_BITS    0xC0U

#define VM_DOS_UART_EVENT_TX_READY           0x00000001UL
#define VM_DOS_UART_EVENT_HOST_LINES_CHANGED 0x00000002UL
#define VM_DOS_UART_EVENT_RX_RESET           0x00000004UL
#define VM_DOS_UART_EVENT_TX_RESET           0x00000008UL
#define VM_DOS_UART_EVENT_IRQ_CHANGED        0x00000010UL
#define VM_DOS_UART_EVENT_MODEM_CHANGED      0x00000020UL

typedef struct VM_DOS_UART_FIFO {
    unsigned char data[VM_DOS_UART_FIFO_CAPACITY];
    unsigned short get;
    unsigned short put;
    unsigned short count;
} VM_DOS_UART_FIFO;

typedef struct VM_DOS_UART_STATE {
    unsigned long  base_port;
    unsigned long  irq_number;
    int            enabled;
    int            fifo_enabled;
    unsigned char  ier;
    unsigned char  lcr;
    unsigned char  mcr;
    unsigned char  lsr;
    unsigned char  msr;
    unsigned char  scr;
    unsigned char  dll;
    unsigned char  dlm;
    unsigned char  fcr;
    unsigned long  pending_irq;
    int            irq_asserted;
    VM_DOS_UART_FIFO rx_fifo;
    VM_DOS_UART_FIFO tx_fifo;
} VM_DOS_UART_STATE;

typedef struct VM_DOS_UART_EVENT {
    unsigned long flags;
    unsigned long pending_irq_before;
    unsigned long pending_irq_after;
    int           irq_asserted_before;
    int           irq_asserted_after;
    int           host_lines_changed;
    int           dtr_asserted;
    int           rts_asserted;
    int           tx_ready;
    unsigned char tx_byte;
} VM_DOS_UART_EVENT;

void vm_dos_uart_init(VM_DOS_UART_STATE *uart);
void vm_dos_uart_configure(VM_DOS_UART_STATE *uart,
                           unsigned long base_port,
                           unsigned long irq_number,
                           int enabled);
void vm_dos_uart_reset(VM_DOS_UART_STATE *uart);

unsigned char vm_dos_uart_read(VM_DOS_UART_STATE *uart,
                               unsigned short offset,
                               VM_DOS_UART_EVENT *event);
void vm_dos_uart_write(VM_DOS_UART_STATE *uart,
                       unsigned short offset,
                       unsigned char value,
                       VM_DOS_UART_EVENT *event);

unsigned short vm_dos_uart_enqueue_rx(VM_DOS_UART_STATE *uart,
                                      const unsigned char *bytes,
                                      unsigned short count,
                                      VM_DOS_UART_EVENT *event);
int vm_dos_uart_pop_tx(VM_DOS_UART_STATE *uart, unsigned char *byte_out);
void vm_dos_uart_complete_tx(VM_DOS_UART_STATE *uart,
                             VM_DOS_UART_EVENT *event);
void vm_dos_uart_apply_modem_status(VM_DOS_UART_STATE *uart,
                                    unsigned long modem_status,
                                    int clear_delta_bits,
                                    VM_DOS_UART_EVENT *event);

unsigned char vm_dos_uart_read_inert(unsigned short offset);
unsigned char vm_dos_uart_get_iir(const VM_DOS_UART_STATE *uart);
unsigned long vm_dos_uart_get_pending_irq(const VM_DOS_UART_STATE *uart);
int vm_dos_uart_get_irq_asserted(const VM_DOS_UART_STATE *uart);
unsigned short vm_dos_uart_get_rx_depth(const VM_DOS_UART_STATE *uart);
unsigned short vm_dos_uart_get_tx_depth(const VM_DOS_UART_STATE *uart);
int vm_dos_uart_get_dtr(const VM_DOS_UART_STATE *uart);
int vm_dos_uart_get_rts(const VM_DOS_UART_STATE *uart);
void vm_dos_uart_fill_diagnostic(const VM_DOS_UART_STATE *uart,
                                 unsigned long owner_type,
                                 unsigned long owner_vm_id,
                                 VMODEM_DOS_UART_DIAGNOSTIC *diag);

#endif /* VMODEM_DOS_UART_H */
