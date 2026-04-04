#include <stdio.h>
#include <string.h>

#include "modem_core.h"
#include "ipc_shared.h"
#include "dos_uart.h"

#define TEST_QUEUE_CAP 64

static int g_failures = 0;

typedef struct TEST_FRONTEND_HARNESS {
    VM_DOS_UART_STATE uart;
    VM_MODEM_CORE     modem;
    int               windows_open;
    unsigned long     dos_owner_vm_id;
    unsigned char     windows_rx[TEST_QUEUE_CAP];
    unsigned short    windows_rx_count;
} TEST_FRONTEND_HARNESS;

static void expect_int(const char *label, long actual, long expected)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected %ld got %ld\n",
                label,
                expected,
                actual);
        ++g_failures;
    }
}

static void expect_hex(const char *label, unsigned int actual, unsigned int expected)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected 0x%02X got 0x%02X\n",
                label,
                expected,
                actual);
        ++g_failures;
    }
}

static void test_uart_dlab(void)
{
    VM_DOS_UART_STATE uart;

    vm_dos_uart_init(&uart);
    vm_dos_uart_configure(&uart, 0x3E8UL, 4UL, 1);
    vm_dos_uart_write(&uart, VM_DOS_UART_REG_LCR, VM_DOS_UART_LCR_DLAB, 0);
    vm_dos_uart_write(&uart, VM_DOS_UART_REG_DATA, 0x34U, 0);
    vm_dos_uart_write(&uart, VM_DOS_UART_REG_IER, 0x12U, 0);

    expect_hex("dll readback",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_DATA, 0),
               0x34U);
    expect_hex("dlm readback",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_IER, 0),
               0x12U);

    vm_dos_uart_write(&uart, VM_DOS_UART_REG_LCR, 0x03U, 0);
    expect_hex("lcr clear dlab",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_LCR, 0),
               0x03U);
}

static void test_uart_fifo_reset(void)
{
    VM_DOS_UART_STATE uart;
    static const unsigned char sample[] = { 'A', 'B', 'C' };

    vm_dos_uart_init(&uart);
    vm_dos_uart_configure(&uart, 0x3E8UL, 4UL, 1);
    vm_dos_uart_write(&uart,
                      VM_DOS_UART_REG_IIR_FCR,
                      VM_DOS_UART_FCR_ENABLE,
                      0);
    expect_hex("fifo bits set in iir",
               vm_dos_uart_get_iir(&uart) & VM_DOS_UART_IIR_FIFO_BITS,
               VM_DOS_UART_IIR_FIFO_BITS);

    vm_dos_uart_enqueue_rx(&uart, sample, sizeof(sample), 0);
    expect_int("rx depth after enqueue",
               (long)vm_dos_uart_get_rx_depth(&uart),
               3L);

    vm_dos_uart_write(&uart,
                      VM_DOS_UART_REG_IIR_FCR,
                      VM_DOS_UART_FCR_ENABLE |
                      VM_DOS_UART_FCR_CLEAR_RX |
                      VM_DOS_UART_FCR_CLEAR_TX,
                      0);
    expect_int("rx depth after clear",
               (long)vm_dos_uart_get_rx_depth(&uart),
               0L);
    expect_int("tx depth after clear",
               (long)vm_dos_uart_get_tx_depth(&uart),
               0L);
    expect_hex("lsr empty after clear",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_LSR, 0),
               VM_DOS_UART_LSR_THRE | VM_DOS_UART_LSR_TEMT);
}

static void test_uart_overrun_and_rbr(void)
{
    VM_DOS_UART_STATE uart;
    unsigned char buffer[VM_DOS_UART_FIFO_CAPACITY + 1U];
    unsigned short i;

    for (i = 0U; i < (unsigned short)sizeof(buffer); ++i) {
        buffer[i] = (unsigned char)i;
    }

    vm_dos_uart_init(&uart);
    vm_dos_uart_configure(&uart, 0x3E8UL, 4UL, 1);
    expect_int("overrun accepted bytes",
               (long)vm_dos_uart_enqueue_rx(&uart,
                                            buffer,
                                            (unsigned short)sizeof(buffer),
                                            0),
               (long)VM_DOS_UART_FIFO_CAPACITY);
    expect_hex("lsr with dr+oe",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_LSR, 0),
               VM_DOS_UART_LSR_DR |
               VM_DOS_UART_LSR_OE |
               VM_DOS_UART_LSR_THRE |
               VM_DOS_UART_LSR_TEMT);
    expect_hex("lsr clears oe on read",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_LSR, 0),
               VM_DOS_UART_LSR_DR |
               VM_DOS_UART_LSR_THRE |
               VM_DOS_UART_LSR_TEMT);

    for (i = 0U; i < VM_DOS_UART_FIFO_CAPACITY; ++i) {
        expect_hex("rbr sequence",
                   vm_dos_uart_read(&uart, VM_DOS_UART_REG_DATA, 0),
                   buffer[i]);
    }
    expect_hex("lsr clears dr after drain",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_LSR, 0),
               VM_DOS_UART_LSR_THRE | VM_DOS_UART_LSR_TEMT);
}

static void test_uart_interrupt_priority_and_out2(void)
{
    VM_DOS_UART_STATE uart;

    vm_dos_uart_init(&uart);
    vm_dos_uart_configure(&uart, 0x3E8UL, 4UL, 1);
    vm_dos_uart_write(&uart,
                      VM_DOS_UART_REG_IER,
                      VM_DOS_UART_IER_RX_DATA |
                      VM_DOS_UART_IER_THR_EMPTY |
                      VM_DOS_UART_IER_MODEM_STATUS,
                      0);
    vm_dos_uart_write(&uart,
                      VM_DOS_UART_REG_MCR,
                      VM_DOS_UART_MCR_OUT2,
                      0);

    expect_int("thr empty pending first",
               (long)vm_dos_uart_get_pending_irq(&uart),
               (long)VMODEM_DOS_UART_IRQ_THR_EMPTY);
    expect_int("irq asserted with out2",
               vm_dos_uart_get_irq_asserted(&uart),
               1);

    vm_dos_uart_apply_modem_status(&uart, 0x00B0UL | 0x0080UL, 0, 0);
    expect_int("modem delta loses to thr empty",
               (long)vm_dos_uart_get_pending_irq(&uart),
               (long)VMODEM_DOS_UART_IRQ_THR_EMPTY);

    vm_dos_uart_enqueue_rx(&uart,
                           (const unsigned char *)"Z",
                           1U,
                           0);
    expect_int("rx data is highest priority",
               (long)vm_dos_uart_get_pending_irq(&uart),
               (long)VMODEM_DOS_UART_IRQ_RX_DATA);
    expect_hex("iir reports rx",
               vm_dos_uart_get_iir(&uart) & 0x07U,
               VM_DOS_UART_IIR_RX_DATA);

    vm_dos_uart_read(&uart, VM_DOS_UART_REG_DATA, 0);
    expect_int("returns to thr empty after rx drain",
               (long)vm_dos_uart_get_pending_irq(&uart),
               (long)VMODEM_DOS_UART_IRQ_THR_EMPTY);

    vm_dos_uart_write(&uart, VM_DOS_UART_REG_DATA, 'Q', 0);
    expect_int("write clears thr empty pending",
               (long)vm_dos_uart_get_pending_irq(&uart),
               (long)VMODEM_DOS_UART_IRQ_MODEM_STATUS);
    expect_int("queued tx depth",
               (long)vm_dos_uart_get_tx_depth(&uart),
               1L);
    vm_dos_uart_pop_tx(&uart, 0);
    vm_dos_uart_complete_tx(&uart, 0);
    expect_int("thr empty returns after tx complete",
               (long)vm_dos_uart_get_pending_irq(&uart),
               (long)VMODEM_DOS_UART_IRQ_THR_EMPTY);

    vm_dos_uart_write(&uart, VM_DOS_UART_REG_MCR, 0U, 0);
    expect_int("out2 gates asserted irq only",
               vm_dos_uart_get_irq_asserted(&uart),
               0);
    expect_int("pending reason survives out2 drop",
               (long)vm_dos_uart_get_pending_irq(&uart),
               (long)VMODEM_DOS_UART_IRQ_THR_EMPTY);
}

static void test_uart_msr_delta_clearing(void)
{
    VM_DOS_UART_STATE uart;

    vm_dos_uart_init(&uart);
    vm_dos_uart_configure(&uart, 0x3E8UL, 4UL, 1);
    vm_dos_uart_apply_modem_status(&uart, 0x0030UL, 1, 0);
    expect_hex("baseline msr",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_MSR, 0),
               VM_DOS_UART_MSR_CTS | VM_DOS_UART_MSR_DSR);

    vm_dos_uart_apply_modem_status(&uart, 0x00B0UL, 0, 0);
    expect_hex("dcd delta set",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_MSR, 0),
               VM_DOS_UART_MSR_DDCD |
               VM_DOS_UART_MSR_CTS |
               VM_DOS_UART_MSR_DSR |
               VM_DOS_UART_MSR_RLSD);
    expect_hex("msr deltas clear on read",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_MSR, 0),
               VM_DOS_UART_MSR_CTS |
               VM_DOS_UART_MSR_DSR |
               VM_DOS_UART_MSR_RLSD);

    vm_dos_uart_apply_modem_status(&uart, 0x0070UL, 0, 0);
    vm_dos_uart_read(&uart, VM_DOS_UART_REG_MSR, 0);
    vm_dos_uart_apply_modem_status(&uart, 0x0030UL, 0, 0);
    expect_hex("teri on trailing ring edge",
               vm_dos_uart_read(&uart, VM_DOS_UART_REG_MSR, 0),
               VM_DOS_UART_MSR_TERI |
               VM_DOS_UART_MSR_CTS |
               VM_DOS_UART_MSR_DSR);
}

static void harness_init(TEST_FRONTEND_HARNESS *harness)
{
    memset(harness, 0, sizeof(*harness));
    vm_dos_uart_init(&harness->uart);
    vm_dos_uart_configure(&harness->uart, 0x3E8UL, 4UL, 1);
    vm_modem_init(&harness->modem);
    vm_modem_set_helper_available(&harness->modem, 1);
}

static int harness_claim_dos(TEST_FRONTEND_HARNESS *harness,
                             unsigned long vm_id)
{
    if (harness->windows_open || harness->dos_owner_vm_id != 0UL) {
        return 0;
    }

    harness->dos_owner_vm_id = vm_id;
    vm_modem_port_open(&harness->modem, 0UL);
    vm_modem_set_host_lines(&harness->modem,
                            vm_dos_uart_get_dtr(&harness->uart),
                            vm_dos_uart_get_rts(&harness->uart),
                            0UL);
    return 1;
}

static int harness_try_windows_open(TEST_FRONTEND_HARNESS *harness)
{
    if (harness->dos_owner_vm_id != 0UL) {
        return 0;
    }

    harness->windows_open = 1;
    return 1;
}

static unsigned char harness_dos_read(TEST_FRONTEND_HARNESS *harness,
                                      unsigned short offset,
                                      unsigned long *owner_vm_id_out)
{
    if (harness->windows_open) {
        if (owner_vm_id_out != 0) {
            *owner_vm_id_out = harness->dos_owner_vm_id;
        }
        return vm_dos_uart_read_inert(offset);
    }

    if (owner_vm_id_out != 0) {
        *owner_vm_id_out = harness->dos_owner_vm_id;
    }
    return vm_dos_uart_read(&harness->uart, offset, 0);
}

static void harness_route_helper_bytes(TEST_FRONTEND_HARNESS *harness,
                                       const unsigned char *bytes,
                                       unsigned short count)
{
    unsigned short copy_count;

    if (harness->windows_open) {
        copy_count = count;
        if ((unsigned short)(copy_count + harness->windows_rx_count) >
            TEST_QUEUE_CAP) {
            copy_count = (unsigned short)(TEST_QUEUE_CAP -
                                          harness->windows_rx_count);
        }
        if (copy_count != 0U) {
            memcpy(harness->windows_rx + harness->windows_rx_count,
                   bytes,
                   copy_count);
            harness->windows_rx_count =
                (unsigned short)(harness->windows_rx_count + copy_count);
        }
        return;
    }

    if (harness->dos_owner_vm_id != 0UL) {
        vm_dos_uart_enqueue_rx(&harness->uart, bytes, count, 0);
    }
}

static void harness_release_dos(TEST_FRONTEND_HARNESS *harness)
{
    harness->dos_owner_vm_id = 0UL;
    vm_dos_uart_reset(&harness->uart);
    vm_modem_port_close(&harness->modem);
}

static void test_frontend_arbitration(void)
{
    TEST_FRONTEND_HARNESS harness;
    VM_DOS_UART_EVENT event;

    harness_init(&harness);
    expect_int("dos claim succeeds", harness_claim_dos(&harness, 0x1234UL), 1);
    expect_int("windows open blocked while dos owns",
               harness_try_windows_open(&harness),
               0);

    vm_dos_uart_write(&harness.uart,
                      VM_DOS_UART_REG_MCR,
                      VM_DOS_UART_MCR_DTR | VM_DOS_UART_MCR_RTS,
                      &event);
    if (event.host_lines_changed) {
        vm_modem_set_host_lines(&harness.modem,
                                event.dtr_asserted,
                                event.rts_asserted,
                                1UL);
    }
    expect_int("dos dtr maps to modem",
               harness.modem.host_dtr_asserted,
               1);
    expect_int("dos rts maps to modem",
               harness.modem.host_rts_asserted,
               1);

    harness_route_helper_bytes(&harness,
                               (const unsigned char *)"OK",
                               2U);
    expect_int("dos frontend receives helper rx",
               (long)vm_dos_uart_get_rx_depth(&harness.uart),
               2L);

    harness_release_dos(&harness);
    expect_int("dos release clears rx fifo",
               (long)vm_dos_uart_get_rx_depth(&harness.uart),
               0L);
    expect_int("dos release clears tx fifo",
               (long)vm_dos_uart_get_tx_depth(&harness.uart),
               0L);

    harness_init(&harness);
    expect_int("windows open succeeds without dos owner",
               harness_try_windows_open(&harness),
               1);
    expect_hex("inert rbr while windows owner",
               harness_dos_read(&harness, VM_DOS_UART_REG_DATA, 0),
               0U);
    expect_hex("inert lsr while windows owner",
               harness_dos_read(&harness, VM_DOS_UART_REG_LSR, 0),
               VM_DOS_UART_LSR_THRE | VM_DOS_UART_LSR_TEMT);
    expect_hex("inert msr while windows owner",
               harness_dos_read(&harness, VM_DOS_UART_REG_MSR, 0),
               VM_DOS_UART_MSR_CTS | VM_DOS_UART_MSR_DSR);
    expect_int("windows inert path does not claim dos owner",
               (long)harness.dos_owner_vm_id,
               0L);

    harness_route_helper_bytes(&harness,
                               (const unsigned char *)"HI",
                               2U);
    expect_int("windows frontend receives helper rx",
               (long)harness.windows_rx_count,
               2L);
    expect_int("dos fifo untouched under windows owner",
               (long)vm_dos_uart_get_rx_depth(&harness.uart),
               0L);
}

int main(void)
{
    test_uart_dlab();
    test_uart_fifo_reset();
    test_uart_overrun_and_rbr();
    test_uart_interrupt_priority_and_out2();
    test_uart_msr_delta_clearing();
    test_frontend_arbitration();

    if (g_failures != 0) {
        fprintf(stderr, "%d dos_uart test(s) failed\n", g_failures);
        return 1;
    }

    printf("dos_uart tests passed\n");
    return 0;
}
