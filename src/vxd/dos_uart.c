/*
 * dos_uart.c - Pure C DOS-box UART model for the VMODEM DOS frontend.
 */

#include "dos_uart.h"

#define VM_DOS_UART_MSR_DELTA_MASK (VM_DOS_UART_MSR_DCTS | \
                                    VM_DOS_UART_MSR_DDSR | \
                                    VM_DOS_UART_MSR_TERI | \
                                    VM_DOS_UART_MSR_DDCD)
#define VM_DOS_UART_MSR_LINE_MASK  (VM_DOS_UART_MSR_CTS | \
                                    VM_DOS_UART_MSR_DSR | \
                                    VM_DOS_UART_MSR_RI  | \
                                    VM_DOS_UART_MSR_RLSD)

static void vm_dos_uart_zero_event(const VM_DOS_UART_STATE *uart,
                                   VM_DOS_UART_EVENT *event);
static void vm_dos_uart_finish_event(const VM_DOS_UART_STATE *uart,
                                     VM_DOS_UART_EVENT *event);
static void vm_dos_uart_fifo_reset(VM_DOS_UART_FIFO *fifo);
static int vm_dos_uart_fifo_push(VM_DOS_UART_FIFO *fifo, unsigned char value);
static int vm_dos_uart_fifo_pop(VM_DOS_UART_FIFO *fifo, unsigned char *value_out);
static void vm_dos_uart_recompute_irq(VM_DOS_UART_STATE *uart);
static unsigned char vm_dos_uart_map_modem_status(unsigned long modem_status);

static void vm_dos_uart_zero_event(const VM_DOS_UART_STATE *uart,
                                   VM_DOS_UART_EVENT *event)
{
    if (event == 0) {
        return;
    }

    event->flags = 0UL;
    if (uart != 0) {
        event->pending_irq_before = uart->pending_irq;
        event->pending_irq_after = uart->pending_irq;
        event->irq_asserted_before = uart->irq_asserted;
        event->irq_asserted_after = uart->irq_asserted;
    } else {
        event->pending_irq_before = VMODEM_DOS_UART_IRQ_NONE;
        event->pending_irq_after = VMODEM_DOS_UART_IRQ_NONE;
        event->irq_asserted_before = 0;
        event->irq_asserted_after = 0;
    }
    event->host_lines_changed = 0;
    event->dtr_asserted = 0;
    event->rts_asserted = 0;
    event->tx_ready = 0;
    event->tx_byte = 0;
}

static void vm_dos_uart_finish_event(const VM_DOS_UART_STATE *uart,
                                     VM_DOS_UART_EVENT *event)
{
    if (uart == 0 || event == 0) {
        return;
    }

    event->pending_irq_after = uart->pending_irq;
    event->irq_asserted_after = uart->irq_asserted;
    if (event->pending_irq_before != event->pending_irq_after ||
        event->irq_asserted_before != event->irq_asserted_after) {
        event->flags |= VM_DOS_UART_EVENT_IRQ_CHANGED;
    }
}

static void vm_dos_uart_fifo_reset(VM_DOS_UART_FIFO *fifo)
{
    if (fifo == 0) {
        return;
    }

    fifo->get = 0U;
    fifo->put = 0U;
    fifo->count = 0U;
}

static int vm_dos_uart_fifo_push(VM_DOS_UART_FIFO *fifo, unsigned char value)
{
    if (fifo == 0 || fifo->count >= VM_DOS_UART_FIFO_CAPACITY) {
        return 0;
    }

    fifo->data[fifo->put] = value;
    ++fifo->put;
    if (fifo->put >= VM_DOS_UART_FIFO_CAPACITY) {
        fifo->put = 0U;
    }
    ++fifo->count;
    return 1;
}

static int vm_dos_uart_fifo_pop(VM_DOS_UART_FIFO *fifo, unsigned char *value_out)
{
    if (fifo == 0 || fifo->count == 0U) {
        return 0;
    }

    if (value_out != 0) {
        *value_out = fifo->data[fifo->get];
    }
    ++fifo->get;
    if (fifo->get >= VM_DOS_UART_FIFO_CAPACITY) {
        fifo->get = 0U;
    }
    --fifo->count;
    return 1;
}

static void vm_dos_uart_recompute_irq(VM_DOS_UART_STATE *uart)
{
    unsigned long pending;
    int rx_at_threshold;

    if (uart == 0) {
        return;
    }

    /* Five-level priority, highest first (matches 16550A / DOSBox-X order):
     *   1. LINE STATUS  - overrun/parity/framing/break (IER bit 2)
     *   2. FIFO TIMEOUT - partial FIFO timeout elapsed (IER bit 0)
     *   3. RX DATA      - FIFO at or above trigger threshold (IER bit 0)
     *   4. THR EMPTY    - transmit holding register empty (IER bit 1)
     *   5. MODEM STATUS - CTS/DSR/RI/DCD delta (IER bit 3)
     */
    rx_at_threshold = ((uart->lsr & VM_DOS_UART_LSR_DR) != 0U &&
                       uart->rx_fifo.count >= uart->rx_interrupt_threshold);

    pending = VMODEM_DOS_UART_IRQ_NONE;
    if ((uart->ier & VM_DOS_UART_IER_LINE_STATUS) != 0U &&
        (uart->lsr & VM_DOS_UART_LSR_ERROR_MASK) != 0U) {
        pending = VMODEM_DOS_UART_IRQ_LINE_STATUS;
    } else if ((uart->ier & VM_DOS_UART_IER_RX_DATA) != 0U &&
               uart->rx_timeout_fired) {
        pending = VMODEM_DOS_UART_IRQ_FIFO_TIMEOUT;
    } else if ((uart->ier & VM_DOS_UART_IER_RX_DATA) != 0U &&
               rx_at_threshold) {
        pending = VMODEM_DOS_UART_IRQ_RX_DATA;
    } else if ((uart->ier & VM_DOS_UART_IER_THR_EMPTY) != 0U &&
               (uart->lsr & VM_DOS_UART_LSR_THRE) != 0U) {
        pending = VMODEM_DOS_UART_IRQ_THR_EMPTY;
    } else if ((uart->ier & VM_DOS_UART_IER_MODEM_STATUS) != 0U &&
               (uart->msr & VM_DOS_UART_MSR_DELTA_MASK) != 0U) {
        pending = VMODEM_DOS_UART_IRQ_MODEM_STATUS;
    }

    uart->pending_irq = pending;
    uart->irq_asserted =
        (pending != VMODEM_DOS_UART_IRQ_NONE &&
         (uart->mcr & VM_DOS_UART_MCR_OUT2) != 0U) ? 1 : 0;
}

static unsigned char vm_dos_uart_map_modem_status(unsigned long modem_status)
{
    unsigned char msr;

    msr = 0U;
    if ((modem_status & 0x0010UL) != 0UL) {
        msr |= VM_DOS_UART_MSR_CTS;
    }
    if ((modem_status & 0x0020UL) != 0UL) {
        msr |= VM_DOS_UART_MSR_DSR;
    }
    if ((modem_status & 0x0040UL) != 0UL) {
        msr |= VM_DOS_UART_MSR_RI;
    }
    if ((modem_status & 0x0080UL) != 0UL) {
        msr |= VM_DOS_UART_MSR_RLSD;
    }
    return msr;
}

void vm_dos_uart_init(VM_DOS_UART_STATE *uart)
{
    if (uart == 0) {
        return;
    }

    uart->base_port = 0UL;
    uart->irq_number = 0UL;
    uart->enabled = 0;
    vm_dos_uart_reset(uart);
}

void vm_dos_uart_configure(VM_DOS_UART_STATE *uart,
                           unsigned long base_port,
                           unsigned long irq_number,
                           int enabled)
{
    if (uart == 0) {
        return;
    }

    uart->base_port = base_port;
    uart->irq_number = irq_number;
    uart->enabled = enabled ? 1 : 0;
    vm_dos_uart_reset(uart);
}

void vm_dos_uart_reset(VM_DOS_UART_STATE *uart)
{
    if (uart == 0) {
        return;
    }

    uart->fifo_enabled = 0;
    uart->ier = 0U;
    uart->lcr = 0U;
    uart->mcr = 0U;
    uart->lsr = VM_DOS_UART_LSR_THRE | VM_DOS_UART_LSR_TEMT;
    uart->msr = VM_DOS_UART_MSR_CTS | VM_DOS_UART_MSR_DSR;
    uart->scr = 0U;
    uart->dll = 0U;
    uart->dlm = 0U;
    uart->fcr = 0U;
    uart->pending_irq = VMODEM_DOS_UART_IRQ_NONE;
    uart->irq_asserted = 0;
    uart->rx_interrupt_threshold = 1U;
    uart->rx_timeout_armed = 0;
    uart->rx_timeout_fired = 0;
    uart->rx_timeout_counter = 0U;
    vm_dos_uart_fifo_reset(&uart->rx_fifo);
    vm_dos_uart_fifo_reset(&uart->tx_fifo);
    vm_dos_uart_recompute_irq(uart);
}

unsigned char vm_dos_uart_read(VM_DOS_UART_STATE *uart,
                               unsigned short offset,
                               VM_DOS_UART_EVENT *event)
{
    unsigned char value;

    if (uart == 0 || offset > VM_DOS_UART_REG_SCR) {
        vm_dos_uart_zero_event(uart, event);
        vm_dos_uart_finish_event(uart, event);
        return 0U;
    }

    vm_dos_uart_zero_event(uart, event);
    value = 0U;

    if ((uart->lcr & VM_DOS_UART_LCR_DLAB) != 0U) {
        if (offset == VM_DOS_UART_REG_DATA) {
            value = uart->dll;
            vm_dos_uart_finish_event(uart, event);
            return value;
        }
        if (offset == VM_DOS_UART_REG_IER) {
            value = uart->dlm;
            vm_dos_uart_finish_event(uart, event);
            return value;
        }
    }

    switch (offset) {
    case VM_DOS_UART_REG_DATA:
        if (vm_dos_uart_fifo_pop(&uart->rx_fifo, &value)) {
            if (uart->rx_fifo.count == 0U) {
                uart->lsr &= (unsigned char)(~VM_DOS_UART_LSR_DR);
                uart->rx_timeout_armed = 0;
                uart->rx_timeout_fired = 0;
            } else {
                /* disarm fired timeout; re-arm if still below threshold */
                uart->rx_timeout_fired = 0;
                if (uart->rx_fifo.count < uart->rx_interrupt_threshold) {
                    uart->rx_timeout_armed = 1;
                    uart->rx_timeout_counter = 4U;
                }
            }
        }
        break;

    case VM_DOS_UART_REG_IER:
        value = uart->ier;
        break;

    case VM_DOS_UART_REG_IIR_FCR:
        value = vm_dos_uart_get_iir(uart);
        break;

    case VM_DOS_UART_REG_LCR:
        value = uart->lcr;
        break;

    case VM_DOS_UART_REG_MCR:
        value = uart->mcr;
        break;

    case VM_DOS_UART_REG_LSR:
        value = uart->lsr;
        uart->lsr &= (unsigned char)(~VM_DOS_UART_LSR_ERROR_MASK);
        break;

    case VM_DOS_UART_REG_MSR:
        if ((uart->mcr & VM_DOS_UART_MCR_LOOP) != 0U) {
            /* Loopback: line bits reflect MCR outputs; delta bits accumulate
             * as normal and are cleared on read */
            value = (unsigned char)(uart->msr & VM_DOS_UART_MSR_DELTA_MASK);
            if ((uart->mcr & VM_DOS_UART_MCR_RTS) != 0U) {
                value |= VM_DOS_UART_MSR_CTS;
            }
            if ((uart->mcr & VM_DOS_UART_MCR_DTR) != 0U) {
                value |= VM_DOS_UART_MSR_DSR;
            }
            if ((uart->mcr & VM_DOS_UART_MCR_OUT1) != 0U) {
                value |= VM_DOS_UART_MSR_RI;
            }
            if ((uart->mcr & VM_DOS_UART_MCR_OUT2) != 0U) {
                value |= VM_DOS_UART_MSR_RLSD;
            }
        } else {
            value = uart->msr;
        }
        uart->msr &= (unsigned char)(~VM_DOS_UART_MSR_DELTA_MASK);
        break;

    case VM_DOS_UART_REG_SCR:
        value = uart->scr;
        break;

    default:
        break;
    }

    vm_dos_uart_recompute_irq(uart);
    vm_dos_uart_finish_event(uart, event);
    return value;
}

void vm_dos_uart_write(VM_DOS_UART_STATE *uart,
                       unsigned short offset,
                       unsigned char value,
                       VM_DOS_UART_EVENT *event)
{
    unsigned char old_mcr;
    int new_fifo_enabled;

    if (uart == 0 || offset > VM_DOS_UART_REG_SCR) {
        vm_dos_uart_zero_event(uart, event);
        vm_dos_uart_finish_event(uart, event);
        return;
    }

    vm_dos_uart_zero_event(uart, event);

    if ((uart->lcr & VM_DOS_UART_LCR_DLAB) != 0U) {
        if (offset == VM_DOS_UART_REG_DATA) {
            uart->dll = value;
            vm_dos_uart_finish_event(uart, event);
            return;
        }
        if (offset == VM_DOS_UART_REG_IER) {
            uart->dlm = value;
            vm_dos_uart_finish_event(uart, event);
            return;
        }
    }

    switch (offset) {
    case VM_DOS_UART_REG_DATA:
        if ((uart->mcr & VM_DOS_UART_MCR_LOOP) != 0U) {
            /* Loopback: byte goes directly to RX FIFO, TX appears empty */
            if (vm_dos_uart_fifo_push(&uart->rx_fifo, value)) {
                uart->lsr |= VM_DOS_UART_LSR_DR;
                if (uart->rx_fifo.count < uart->rx_interrupt_threshold) {
                    uart->rx_timeout_armed = 1;
                    uart->rx_timeout_counter = 4U;
                } else {
                    uart->rx_timeout_armed = 0;
                    uart->rx_timeout_fired = 0;
                }
            } else {
                uart->lsr |= VM_DOS_UART_LSR_OE;
            }
            /* TX side stays empty in loopback */
        } else {
            if (vm_dos_uart_fifo_push(&uart->tx_fifo, value)) {
                uart->lsr &= (unsigned char)(~(VM_DOS_UART_LSR_THRE |
                                               VM_DOS_UART_LSR_TEMT));
            }
        }
        break;

    case VM_DOS_UART_REG_IER:
        uart->ier = (unsigned char)(value &
                     (VM_DOS_UART_IER_RX_DATA |
                      VM_DOS_UART_IER_THR_EMPTY |
                      VM_DOS_UART_IER_LINE_STATUS |
                      VM_DOS_UART_IER_MODEM_STATUS));
        break;

    case VM_DOS_UART_REG_IIR_FCR:
        uart->fcr = value;
        new_fifo_enabled = ((value & VM_DOS_UART_FCR_ENABLE) != 0U) ? 1 : 0;
        if (!new_fifo_enabled && uart->fifo_enabled) {
            vm_dos_uart_fifo_reset(&uart->rx_fifo);
            vm_dos_uart_fifo_reset(&uart->tx_fifo);
            uart->lsr &= (unsigned char)(~VM_DOS_UART_LSR_DR);
            uart->lsr |= VM_DOS_UART_LSR_THRE | VM_DOS_UART_LSR_TEMT;
            uart->rx_timeout_armed = 0;
            uart->rx_timeout_fired = 0;
            uart->rx_interrupt_threshold = 1U;
            if (event != 0) {
                event->flags |= VM_DOS_UART_EVENT_RX_RESET |
                                VM_DOS_UART_EVENT_TX_RESET;
            }
        }
        uart->fifo_enabled = new_fifo_enabled;
        if ((value & VM_DOS_UART_FCR_CLEAR_RX) != 0U) {
            vm_dos_uart_fifo_reset(&uart->rx_fifo);
            uart->lsr &= (unsigned char)(~(VM_DOS_UART_LSR_DR |
                                           VM_DOS_UART_LSR_OE));
            uart->rx_timeout_armed = 0;
            uart->rx_timeout_fired = 0;
            if (event != 0) {
                event->flags |= VM_DOS_UART_EVENT_RX_RESET;
            }
        }
        if ((value & VM_DOS_UART_FCR_CLEAR_TX) != 0U) {
            vm_dos_uart_fifo_reset(&uart->tx_fifo);
            uart->lsr |= VM_DOS_UART_LSR_THRE | VM_DOS_UART_LSR_TEMT;
            if (event != 0) {
                event->flags |= VM_DOS_UART_EVENT_TX_RESET;
            }
        }
        if (uart->fifo_enabled) {
            switch ((uart->fcr >> 6) & 0x03U) {
            case 0: uart->rx_interrupt_threshold = 1U;  break;
            case 1: uart->rx_interrupt_threshold = 4U;  break;
            case 2: uart->rx_interrupt_threshold = 8U;  break;
            case 3: uart->rx_interrupt_threshold = 14U; break;
            }
        } else {
            uart->rx_interrupt_threshold = 1U;
        }
        break;

    case VM_DOS_UART_REG_LCR:
        uart->lcr = value;
        break;

    case VM_DOS_UART_REG_MCR:
        old_mcr = uart->mcr;
        uart->mcr = value;
        if ((uart->mcr & VM_DOS_UART_MCR_LOOP) != 0U) {
            /* In loopback, MCR outputs reflect back into MSR.
             * Recompute MSR delta bits from the new MCR state.
             * RTS->CTS, DTR->DSR, OUT1->RI, OUT2->RLSD */
            {
                unsigned char old_msr_lines;
                unsigned char new_msr_lines;
                unsigned char delta;

                old_msr_lines = (unsigned char)(uart->msr &
                                 VM_DOS_UART_MSR_LINE_MASK);
                new_msr_lines = 0U;
                if ((uart->mcr & VM_DOS_UART_MCR_RTS) != 0U) {
                    new_msr_lines |= VM_DOS_UART_MSR_CTS;
                }
                if ((uart->mcr & VM_DOS_UART_MCR_DTR) != 0U) {
                    new_msr_lines |= VM_DOS_UART_MSR_DSR;
                }
                if ((uart->mcr & VM_DOS_UART_MCR_OUT1) != 0U) {
                    new_msr_lines |= VM_DOS_UART_MSR_RI;
                }
                if ((uart->mcr & VM_DOS_UART_MCR_OUT2) != 0U) {
                    new_msr_lines |= VM_DOS_UART_MSR_RLSD;
                }

                delta = (unsigned char)(uart->msr & VM_DOS_UART_MSR_DELTA_MASK);
                if (((old_msr_lines ^ new_msr_lines) & VM_DOS_UART_MSR_CTS) != 0U) {
                    delta |= VM_DOS_UART_MSR_DCTS;
                }
                if (((old_msr_lines ^ new_msr_lines) & VM_DOS_UART_MSR_DSR) != 0U) {
                    delta |= VM_DOS_UART_MSR_DDSR;
                }
                if ((old_msr_lines & VM_DOS_UART_MSR_RI) != 0U &&
                    (new_msr_lines & VM_DOS_UART_MSR_RI) == 0U) {
                    delta |= VM_DOS_UART_MSR_TERI;
                }
                if (((old_msr_lines ^ new_msr_lines) & VM_DOS_UART_MSR_RLSD) != 0U) {
                    delta |= VM_DOS_UART_MSR_DDCD;
                }
                uart->msr = (unsigned char)(new_msr_lines | delta);
            }
            /* Do not propagate DTR/RTS to modem core while in loopback */
        } else {
            if (((old_mcr ^ uart->mcr) &
                 (VM_DOS_UART_MCR_DTR | VM_DOS_UART_MCR_RTS)) != 0U &&
                event != 0) {
                event->flags |= VM_DOS_UART_EVENT_HOST_LINES_CHANGED;
                event->host_lines_changed = 1;
                event->dtr_asserted =
                    ((uart->mcr & VM_DOS_UART_MCR_DTR) != 0U) ? 1 : 0;
                event->rts_asserted =
                    ((uart->mcr & VM_DOS_UART_MCR_RTS) != 0U) ? 1 : 0;
            }
        }
        break;

    case VM_DOS_UART_REG_SCR:
        uart->scr = value;
        break;

    default:
        break;
    }

    vm_dos_uart_recompute_irq(uart);
    vm_dos_uart_finish_event(uart, event);
}

unsigned short vm_dos_uart_enqueue_rx(VM_DOS_UART_STATE *uart,
                                      const unsigned char *bytes,
                                      unsigned short count,
                                      VM_DOS_UART_EVENT *event)
{
    unsigned short written;

    if (uart == 0 || bytes == 0 || count == 0U) {
        vm_dos_uart_zero_event(uart, event);
        vm_dos_uart_finish_event(uart, event);
        return 0U;
    }

    vm_dos_uart_zero_event(uart, event);

    written = 0U;
    while (written < count) {
        if (!vm_dos_uart_fifo_push(&uart->rx_fifo, bytes[written])) {
            uart->lsr |= VM_DOS_UART_LSR_OE;
            break;
        }
        ++written;
    }

    if (uart->rx_fifo.count != 0U) {
        uart->lsr |= VM_DOS_UART_LSR_DR;
        /* Arm timeout if FIFO has data but hasn't reached the threshold.
         * If at/above threshold the RX_DATA interrupt will fire directly. */
        if (uart->rx_fifo.count < uart->rx_interrupt_threshold) {
            uart->rx_timeout_armed = 1;
            uart->rx_timeout_counter = 4U;
        } else {
            uart->rx_timeout_armed = 0;
            uart->rx_timeout_fired = 0;
        }
    }

    vm_dos_uart_recompute_irq(uart);
    vm_dos_uart_finish_event(uart, event);
    return written;
}

int vm_dos_uart_pop_tx(VM_DOS_UART_STATE *uart, unsigned char *byte_out)
{
    if (uart == 0) {
        return 0;
    }

    return vm_dos_uart_fifo_pop(&uart->tx_fifo, byte_out);
}

void vm_dos_uart_complete_tx(VM_DOS_UART_STATE *uart,
                             VM_DOS_UART_EVENT *event)
{
    if (uart == 0) {
        vm_dos_uart_zero_event(uart, event);
        vm_dos_uart_finish_event(uart, event);
        return;
    }

    vm_dos_uart_zero_event(uart, event);
    if (uart->tx_fifo.count == 0U) {
        uart->lsr |= VM_DOS_UART_LSR_THRE | VM_DOS_UART_LSR_TEMT;
    }
    vm_dos_uart_recompute_irq(uart);
    vm_dos_uart_finish_event(uart, event);
}

void vm_dos_uart_tick(VM_DOS_UART_STATE *uart, VM_DOS_UART_EVENT *event)
{
    if (uart == 0) {
        return;
    }

    vm_dos_uart_zero_event(uart, event);

    if (uart->rx_timeout_armed && !uart->rx_timeout_fired) {
        if (uart->rx_timeout_counter > 0U) {
            --uart->rx_timeout_counter;
        }
        if (uart->rx_timeout_counter == 0U) {
            if (uart->rx_fifo.count > 0U) {
                uart->rx_timeout_armed = 0;
                uart->rx_timeout_fired = 1;
                if (event != 0) {
                    event->flags |= VM_DOS_UART_EVENT_RX_TIMEOUT;
                }
            } else {
                uart->rx_timeout_armed = 0;
            }
        }
    }

    vm_dos_uart_recompute_irq(uart);
    vm_dos_uart_finish_event(uart, event);
}

void vm_dos_uart_apply_modem_status(VM_DOS_UART_STATE *uart,
                                    unsigned long modem_status,
                                    int clear_delta_bits,
                                    VM_DOS_UART_EVENT *event)
{
    unsigned char old_msr;
    unsigned char old_lines;
    unsigned char new_lines;
    unsigned char delta;

    if (uart == 0) {
        vm_dos_uart_zero_event(uart, event);
        vm_dos_uart_finish_event(uart, event);
        return;
    }

    vm_dos_uart_zero_event(uart, event);

    old_msr = uart->msr;
    old_lines = (unsigned char)(old_msr & VM_DOS_UART_MSR_LINE_MASK);
    new_lines = vm_dos_uart_map_modem_status(modem_status);
    delta = clear_delta_bits ? 0U : (unsigned char)(old_msr &
            VM_DOS_UART_MSR_DELTA_MASK);

    if (((old_lines ^ new_lines) & VM_DOS_UART_MSR_CTS) != 0U) {
        delta |= VM_DOS_UART_MSR_DCTS;
    }
    if (((old_lines ^ new_lines) & VM_DOS_UART_MSR_DSR) != 0U) {
        delta |= VM_DOS_UART_MSR_DDSR;
    }
    if ((old_lines & VM_DOS_UART_MSR_RI) != 0U &&
        (new_lines & VM_DOS_UART_MSR_RI) == 0U) {
        delta |= VM_DOS_UART_MSR_TERI;
    }
    if (((old_lines ^ new_lines) & VM_DOS_UART_MSR_RLSD) != 0U) {
        delta |= VM_DOS_UART_MSR_DDCD;
    }

    uart->msr = (unsigned char)(new_lines | delta);
    if (uart->msr != old_msr && event != 0) {
        event->flags |= VM_DOS_UART_EVENT_MODEM_CHANGED;
    }

    vm_dos_uart_recompute_irq(uart);
    vm_dos_uart_finish_event(uart, event);
}

unsigned char vm_dos_uart_read_inert(unsigned short offset)
{
    switch (offset) {
    case VM_DOS_UART_REG_IIR_FCR:
        return VM_DOS_UART_IIR_NONE;
    case VM_DOS_UART_REG_LSR:
        return VM_DOS_UART_LSR_THRE | VM_DOS_UART_LSR_TEMT;
    case VM_DOS_UART_REG_MSR:
        return VM_DOS_UART_MSR_CTS | VM_DOS_UART_MSR_DSR;
    default:
        return 0U;
    }
}

unsigned char vm_dos_uart_get_iir(const VM_DOS_UART_STATE *uart)
{
    unsigned char value;

    if (uart == 0) {
        return VM_DOS_UART_IIR_NONE;
    }

    value = VM_DOS_UART_IIR_NONE;
    if (uart->pending_irq == VMODEM_DOS_UART_IRQ_LINE_STATUS) {
        value = VM_DOS_UART_IIR_LINE_STATUS;
    } else if (uart->pending_irq == VMODEM_DOS_UART_IRQ_FIFO_TIMEOUT) {
        value = VM_DOS_UART_IIR_FIFO_TIMEOUT;
    } else if (uart->pending_irq == VMODEM_DOS_UART_IRQ_RX_DATA) {
        value = VM_DOS_UART_IIR_RX_DATA;
    } else if (uart->pending_irq == VMODEM_DOS_UART_IRQ_THR_EMPTY) {
        value = VM_DOS_UART_IIR_THR_EMPTY;
    } else if (uart->pending_irq == VMODEM_DOS_UART_IRQ_MODEM_STATUS) {
        value = VM_DOS_UART_IIR_MODEM_STATUS;
    }

    if (uart->fifo_enabled) {
        value |= VM_DOS_UART_IIR_FIFO_BITS;
    }
    return value;
}

unsigned long vm_dos_uart_get_pending_irq(const VM_DOS_UART_STATE *uart)
{
    if (uart == 0) {
        return VMODEM_DOS_UART_IRQ_NONE;
    }

    return uart->pending_irq;
}

int vm_dos_uart_get_irq_asserted(const VM_DOS_UART_STATE *uart)
{
    if (uart == 0) {
        return 0;
    }

    return uart->irq_asserted;
}

unsigned short vm_dos_uart_get_rx_depth(const VM_DOS_UART_STATE *uart)
{
    if (uart == 0) {
        return 0U;
    }

    return uart->rx_fifo.count;
}

unsigned short vm_dos_uart_get_tx_depth(const VM_DOS_UART_STATE *uart)
{
    if (uart == 0) {
        return 0U;
    }

    return uart->tx_fifo.count;
}

int vm_dos_uart_get_dtr(const VM_DOS_UART_STATE *uart)
{
    if (uart == 0) {
        return 0;
    }

    return ((uart->mcr & VM_DOS_UART_MCR_DTR) != 0U) ? 1 : 0;
}

int vm_dos_uart_get_rts(const VM_DOS_UART_STATE *uart)
{
    if (uart == 0) {
        return 0;
    }

    return ((uart->mcr & VM_DOS_UART_MCR_RTS) != 0U) ? 1 : 0;
}

void vm_dos_uart_fill_diagnostic(const VM_DOS_UART_STATE *uart,
                                 unsigned long owner_type,
                                 unsigned long owner_vm_id,
                                 VMODEM_DOS_UART_DIAGNOSTIC *diag)
{
    if (diag == 0) {
        return;
    }

    diag->enabled = (uart != 0 && uart->enabled) ? 1UL : 0UL;
    diag->owner_type = owner_type;
    diag->owner_vm_id = owner_vm_id;
    diag->base_port = (uart != 0) ? uart->base_port : 0UL;
    diag->irq_number = (uart != 0) ? uart->irq_number : 0UL;
    diag->irq_asserted = (uart != 0 && uart->irq_asserted) ? 1UL : 0UL;
    diag->pending_irq = (uart != 0) ? uart->pending_irq
                                    : VMODEM_DOS_UART_IRQ_NONE;
    diag->rx_fifo_depth = (uart != 0) ? uart->rx_fifo.count : 0UL;
    diag->tx_fifo_depth = (uart != 0) ? uart->tx_fifo.count : 0UL;
    diag->ier = (uart != 0) ? uart->ier : 0U;
    diag->iir = (uart != 0) ? vm_dos_uart_get_iir(uart) : VM_DOS_UART_IIR_NONE;
    diag->fcr = (uart != 0) ? uart->fcr : 0U;
    diag->lcr = (uart != 0) ? uart->lcr : 0U;
    diag->mcr = (uart != 0) ? uart->mcr : 0U;
    diag->lsr = (uart != 0) ? uart->lsr : 0U;
    diag->msr = (uart != 0) ? uart->msr : 0U;
    diag->scr = (uart != 0) ? uart->scr : 0U;
    diag->dll = (uart != 0) ? uart->dll : 0U;
    diag->dlm = (uart != 0) ? uart->dlm : 0U;
    diag->rx_interrupt_threshold =
        (uart != 0) ? uart->rx_interrupt_threshold : 1U;
    diag->loopback_active =
        (uart != 0 && (uart->mcr & VM_DOS_UART_MCR_LOOP) != 0U) ? 1UL : 0UL;
}
