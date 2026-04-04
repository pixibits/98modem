#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc_shared.h"
#include "modem_core.h"

#define TEST_OUTPUT_CAP 2048

static int g_failures = 0;

static unsigned short test_strlen_u16(const char *text)
{
    unsigned short len;

    len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    return len;
}

static void expect_int(const char *label, long actual, long expected)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected %ld got %ld\n", label, expected, actual);
        ++g_failures;
    }
}

static void expect_string(const char *label, const char *actual, const char *expected)
{
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL: %s\nexpected: [%s]\nactual:   [%s]\n",
                label,
                expected,
                actual);
        ++g_failures;
    }
}

static void open_modem(VM_MODEM_CORE *modem,
                       unsigned long now_ms,
                       int helper_available)
{
    vm_modem_init(modem);
    vm_modem_set_helper_available(modem, helper_available);
    vm_modem_port_open(modem, now_ms);
}

static void feed_text(VM_MODEM_CORE *modem, const char *text, unsigned long now_ms)
{
    vm_modem_ingest_tx(modem,
                       (const unsigned char *)text,
                       test_strlen_u16(text),
                       now_ms);
}

static void feed_bytes(VM_MODEM_CORE *modem,
                       const unsigned char *bytes,
                       unsigned short count,
                       unsigned long now_ms)
{
    vm_modem_ingest_tx(modem, bytes, count, now_ms);
}

static void drain_all(VM_MODEM_CORE *modem, char *buffer, unsigned short capacity)
{
    unsigned short copied;
    unsigned short total;

    if (capacity == 0) {
        return;
    }

    total = 0;
    while (total + 1 < capacity && vm_modem_output_count(modem) != 0) {
        copied = vm_modem_drain_output(modem,
                                       (unsigned char *)(buffer + total),
                                       (unsigned short)(capacity - total - 1));
        if (copied == 0) {
            break;
        }
        total = (unsigned short)(total + copied);
    }

    buffer[total] = '\0';
}

static void quiet_modem(VM_MODEM_CORE *modem)
{
    char output[TEST_OUTPUT_CAP];

    feed_text(modem, "ATE0\r", 0UL);
    drain_all(modem, output, sizeof(output));
    expect_string("ATE0 quiet setup", output, "ATE0\r\r\nOK\r\n");
    expect_int("echo disabled", vm_modem_get_echo_enabled(modem), 0);
}

static void expect_no_action(VM_MODEM_CORE *modem, const char *label)
{
    VM_MODEM_ACTION action;

    expect_int(label, vm_modem_peek_action(modem, &action), 0);
}

static unsigned long expect_action_payload(VM_MODEM_CORE *modem,
                                           VM_MODEM_ACTION_TYPE expected_type,
                                           const char *expected_payload,
                                           const char *label)
{
    VM_MODEM_ACTION action;
    char payload[VM_MODEM_ACTION_PAYLOAD_LEN + 1];
    unsigned short i;

    expect_int(label, vm_modem_peek_action(modem, &action), 1);
    if (action.type != expected_type) {
        fprintf(stderr,
                "FAIL: %s: expected type %ld got %ld\n",
                label,
                (long)expected_type,
                (long)action.type);
        ++g_failures;
    }

    for (i = 0; i < action.payload_length; ++i) {
        payload[i] = (char)action.payload[i];
    }
    payload[action.payload_length] = '\0';

    if (expected_payload != NULL) {
        expect_string(label, payload, expected_payload);
    }

    vm_modem_pop_action(modem);
    return action.session_id;
}

static unsigned long expect_action_payload_status(
    VM_MODEM_CORE *modem,
    VM_MODEM_ACTION_TYPE expected_type,
    const char *expected_payload,
    unsigned long expected_status,
    const char *label)
{
    VM_MODEM_ACTION action;
    char payload[VM_MODEM_ACTION_PAYLOAD_LEN + 1];
    unsigned short i;

    expect_int(label, vm_modem_peek_action(modem, &action), 1);
    if (action.type != expected_type) {
        fprintf(stderr,
                "FAIL: %s: expected type %ld got %ld\n",
                label,
                (long)expected_type,
                (long)action.type);
        ++g_failures;
    }
    expect_int("action status",
               (long)action.status,
               (long)expected_status);

    for (i = 0; i < action.payload_length; ++i) {
        payload[i] = (char)action.payload[i];
    }
    payload[action.payload_length] = '\0';

    if (expected_payload != NULL) {
        expect_string(label, payload, expected_payload);
    }

    vm_modem_pop_action(modem);
    return action.session_id;
}

static void collect_data_actions(VM_MODEM_CORE *modem,
                                 char *buffer,
                                 unsigned short capacity,
                                 unsigned long expected_session_id)
{
    VM_MODEM_ACTION action;
    unsigned short total;
    unsigned short i;

    if (capacity == 0) {
        return;
    }

    total = 0;
    while (vm_modem_peek_action(modem, &action)) {
        if (action.type != VM_MODEM_ACTION_DATA_TO_NET) {
            break;
        }

        expect_int("data action session",
                   (long)action.session_id,
                   (long)expected_session_id);
        for (i = 0; i < action.payload_length && total + 1 < capacity; ++i) {
            buffer[total++] = (char)action.payload[i];
        }
        vm_modem_pop_action(modem);
    }

    buffer[total] = '\0';
}

static void helper_complete_connect(VM_MODEM_CORE *modem,
                                    unsigned long session_id,
                                    unsigned long now_ms)
{
    static const char banner[] = "98MODEM FAKE PEER READY\r\n";

    expect_int("connect ok accepted",
               vm_modem_on_connect_ok(modem, session_id, now_ms),
               1);
    vm_modem_on_serial_from_helper(modem,
                                   session_id,
                                   (const unsigned char *)banner,
                                   test_strlen_u16(banner));
}

static void test_degraded_without_helper(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];

    open_modem(&modem, 0UL, 0);
    expect_int("helper unavailable state",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_HELPER_UNAVAILABLE);

    feed_text(&modem, "AT\r", 10UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("AT with no helper", output, "AT\r\r\nOK\r\n");

    quiet_modem(&modem);
    feed_text(&modem, "ATDTexample.com:23\r", 20UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("dial without helper", output, "\r\nNO CARRIER\r\n");
    expect_no_action(&modem, "no connect action without helper");

    vm_modem_set_helper_available(&modem, 1);
    expect_int("helper attach returns to idle",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
}

static void test_echo_toggle(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];

    open_modem(&modem, 0UL, 1);

    feed_text(&modem, "ATE0\r", 10UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATE0 response", output, "ATE0\r\r\nOK\r\n");
    expect_int("ATE0 echo", vm_modem_get_echo_enabled(&modem), 0);

    feed_text(&modem, "AT\r", 20UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("AT no echo", output, "\r\nOK\r\n");

    feed_text(&modem, "ATE1\r", 30UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATE1 response", output, "\r\nOK\r\n");
    expect_int("ATE1 echo", vm_modem_get_echo_enabled(&modem), 1);

    feed_text(&modem, "AT\r", 40UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("AT echo restored", output, "AT\r\r\nOK\r\n");
}

static void test_raw_mode_and_s_registers(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;
    static const unsigned char junk_then_atm0[] = {
        0xB8, 0x7F, 0x6F, 0x0F, 0x09, 0x00, 0x09, 0x00, 0x00,
        'A', 'T', 'M', '0', '\r'
    };

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "AT+VRAW?\r", 10UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("raw mode default query",
                  output,
                  "\r\n+VRAW: 1\r\n\r\nOK\r\n");
    expect_int("raw mode default enabled",
               vm_modem_get_raw_mode_enabled(&modem),
               1);

    feed_text(&modem, "AT+VRAW=0\r", 20UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("disable raw mode", output, "\r\nOK\r\n");
    expect_int("raw mode disabled",
               vm_modem_get_raw_mode_enabled(&modem),
               0);

    feed_text(&modem, "ATDTexample.com:23\r", 30UL);
    session_id = expect_action_payload_status(&modem,
                                              VM_MODEM_ACTION_CONNECT_REQ,
                                              "example.com:23",
                                              0UL,
                                              "connect request raw off");
    expect_int("dialing after raw off connect request",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_DIALING);
    expect_int("cancel raw-off dial",
               vm_modem_on_connect_fail(&modem,
                                        session_id,
                                        VMODEM_CONNECT_FAIL_LOCAL),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("raw off dial canceled", output, "\r\nNO CARRIER\r\n");

    feed_text(&modem, "ATS0?\r", 40UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("S0 default query", output, "\r\n0\r\n\r\nOK\r\n");

    feed_text(&modem, "ATS1?\r", 50UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("S1 default query", output, "\r\n0\r\n\r\nOK\r\n");

    feed_text(&modem, "ATS0=2\r", 60UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("set S0", output, "\r\nOK\r\n");
    expect_int("S0 set to two",
               (long)vm_modem_get_s0_auto_answer_rings(&modem),
               2L);

    feed_text(&modem, "ATS0?\r", 70UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("S0 query after set", output, "\r\n2\r\n\r\nOK\r\n");

    feed_text(&modem, "ATS1=1\r", 80UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("S1 set rejected", output, "\r\nERROR\r\n");

    feed_text(&modem, "ATM0\r", 85UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATM0 accepted", output, "\r\nOK\r\n");

    feed_text(&modem, "ATM1\r", 88UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATM1 accepted", output, "\r\nOK\r\n");

    feed_bytes(&modem,
               junk_then_atm0,
               (unsigned short)sizeof(junk_then_atm0),
               89UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("junk before ATM0 resyncs to AT", output, "\r\nOK\r\n");

    feed_text(&modem, "ATZ\r", 90UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATZ resets raw and s registers", output, "\r\nOK\r\n");
    expect_int("raw mode restored on ATZ",
               vm_modem_get_raw_mode_enabled(&modem),
               1);
    expect_int("S0 reset on ATZ",
               (long)vm_modem_get_s0_auto_answer_rings(&modem),
               0L);
    expect_int("S1 reset on ATZ",
               (long)vm_modem_get_s1_ring_count(&modem),
               0L);

    feed_text(&modem, "ATDTexample.com:23\r", 100UL);
    expect_action_payload_status(&modem,
                                 VM_MODEM_ACTION_CONNECT_REQ,
                                 "example.com:23",
                                 VMODEM_CONNECT_FLAG_RAW,
                                 "connect request raw on after ATZ");
}

static void test_error_and_overflow(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    char long_cmd[140];
    int i;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATA\r", 50UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATA not ringing", output, "\r\nERROR\r\n");

    for (i = 0; i < 129; ++i) {
        long_cmd[i] = 'A';
    }
    long_cmd[129] = '\r';
    long_cmd[130] = '\0';

    feed_text(&modem, long_cmd, 60UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("overflow response", output, "\r\nERROR\r\n");

    feed_text(&modem, "AT\r", 70UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("overflow recovery", output, "\r\nOK\r\n");
}

static void test_connect_and_data_path(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    char net_data[TEST_OUTPUT_CAP];
    unsigned long session_id;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATDP192.168.1.10:2323\r", 10UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("dial has no immediate serial output", output, "");
    expect_int("dial state", vm_modem_get_state(&modem), VM_MODEM_STATE_DIALING);

    session_id = expect_action_payload(&modem,
                                       VM_MODEM_ACTION_CONNECT_REQ,
                                       "192.168.1.10:2323",
                                       "connect request payload");

    helper_complete_connect(&modem, session_id, 20UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("helper connect output",
                  output,
                  "\r\nCONNECT\r\n98MODEM FAKE PEER READY\r\n");
    expect_int("connected state",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_CONNECTED_DATA);
    expect_int("connected status",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR | VM_MODEM_STATUS_DCD);

    feed_text(&modem, "hello", 30UL);
    collect_data_actions(&modem, net_data, sizeof(net_data), session_id);
    expect_string("data forwarded to helper", net_data, "hello");

    vm_modem_on_serial_from_helper(&modem,
                                   session_id,
                                   (const unsigned char *)"world",
                                   test_strlen_u16("world"));
    drain_all(&modem, output, sizeof(output));
    expect_string("data from helper to serial", output, "world");
}

static void test_large_connected_write_batches(void)
{
    VM_MODEM_CORE modem;
    VM_MODEM_ACTION action;
    char payload[145 + 1];
    char collected[145 + 1];
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;
    unsigned short i;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATDTexample.com:23\r", 10UL);
    session_id = expect_action_payload(&modem,
                                       VM_MODEM_ACTION_CONNECT_REQ,
                                       "example.com:23",
                                       "connect request before large write");
    helper_complete_connect(&modem, session_id, 20UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("connect output before large write",
                  output,
                  "\r\nCONNECT\r\n98MODEM FAKE PEER READY\r\n");

    for (i = 0; i < 145U; ++i) {
        payload[i] = (char)('A' + (i % 26U));
    }
    payload[145] = '\0';

    feed_bytes(&modem, (const unsigned char *)payload, 145U, 30UL);
    expect_int("large write action queued",
               vm_modem_peek_action(&modem, &action),
               1);
    expect_int("large write action type",
               (long)action.type,
               (long)VM_MODEM_ACTION_DATA_TO_NET);
    expect_int("large write session",
               (long)action.session_id,
               (long)session_id);
    expect_int("large write kept in one action",
               (long)action.payload_length,
               145L);

    collect_data_actions(&modem, collected, sizeof(collected), session_id);
    expect_string("large write payload preserved", collected, payload);
}

static void test_phone_style_dial_reaches_helper(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATDT555-1212\r", 10UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("phone style dial has no immediate serial output", output, "");
    expect_int("phone style dial state",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_DIALING);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_CONNECT_REQ,
                          "555-1212",
                          "phone style connect request payload");

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);
    feed_text(&modem, "ATDTexample\r", 10UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("non phone non hostport dial rejected", output, "\r\nERROR\r\n");
    expect_no_action(&modem, "no action for invalid non phone dial");
}

static void test_connect_fail_and_stale_events(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATDTexample.com:23\r", 10UL);
    session_id = expect_action_payload(&modem,
                                       VM_MODEM_ACTION_CONNECT_REQ,
                                       "example.com:23",
                                       "connect request for stale test");

    expect_int("stale connect ok rejected",
               vm_modem_on_connect_ok(&modem, session_id + 1, 20UL),
               0);
    expect_int("stale connect fail rejected",
               vm_modem_on_connect_fail(&modem,
                                        session_id + 1,
                                        VMODEM_CONNECT_FAIL_TIMEOUT),
               0);
    expect_int("stale remote close rejected",
               vm_modem_on_remote_closed(&modem, session_id + 1),
               0);
    drain_all(&modem, output, sizeof(output));
    expect_string("no output from stale events", output, "");
    expect_int("still dialing after stale events",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_DIALING);

    expect_int("connect fail accepted",
               vm_modem_on_connect_fail(&modem,
                                        session_id,
                                        VMODEM_CONNECT_FAIL_TIMEOUT),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("connect timeout output", output, "\r\nNO ANSWER\r\n");
    expect_int("idle after connect fail",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);

    expect_int("late connect ok rejected after fail",
               vm_modem_on_connect_ok(&modem, session_id, 30UL),
               0);
}

static void test_connect_fail_reason_mapping(void)
{
    static const unsigned long reasons[] = {
        VMODEM_CONNECT_FAIL_DNS,
        VMODEM_CONNECT_FAIL_REFUSED,
        VMODEM_CONNECT_FAIL_NETWORK,
        VMODEM_CONNECT_FAIL_LOCAL
    };
    static const char *labels[] = {
        "dns fail output",
        "refused fail output",
        "network fail output",
        "local fail output"
    };
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;
    unsigned short i;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    for (i = 0; i < 4U; ++i) {
        feed_text(&modem, "ATDTexample.com:23\r", (unsigned long)(100UL + i));
        session_id = expect_action_payload(&modem,
                                           VM_MODEM_ACTION_CONNECT_REQ,
                                           "example.com:23",
                                           "connect request for fail mapping");
        expect_int("connect fail accepted for no carrier mapping",
                   vm_modem_on_connect_fail(&modem, session_id, reasons[i]),
                   1);
        drain_all(&modem, output, sizeof(output));
        expect_string(labels[i], output, "\r\nNO CARRIER\r\n");
        expect_int("idle after mapped connect fail",
                   vm_modem_get_state(&modem),
                   VM_MODEM_STATE_IDLE_CMD);
    }
}

static void test_escape_ato_and_hangup(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    char net_data[TEST_OUTPUT_CAP];
    unsigned long session_id;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATDTexample.com:23\r", 10UL);
    session_id = expect_action_payload(&modem,
                                       VM_MODEM_ACTION_CONNECT_REQ,
                                       "example.com:23",
                                       "connect request before escape");
    helper_complete_connect(&modem, session_id, 20UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("connect output before escape",
                  output,
                  "\r\nCONNECT\r\n98MODEM FAKE PEER READY\r\n");

    feed_text(&modem, "+++", 600UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("+++ immediate output", output, "");
    expect_no_action(&modem, "+++ produces no data action");

    vm_modem_poll(&modem, 1100UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("+++ success output", output, "\r\nOK\r\n");
    expect_int("connected cmd after escape",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_CONNECTED_CMD);

    feed_text(&modem, "ATO\r", 1200UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATO response", output, "\r\nCONNECT\r\n");
    expect_int("state after ATO",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_CONNECTED_DATA);

    feed_text(&modem, "X", 1300UL);
    feed_text(&modem, "+++", 1400UL);
    collect_data_actions(&modem, net_data, sizeof(net_data), session_id);
    expect_string("invalid escape becomes payload", net_data, "X+++");

    feed_text(&modem, "+++", 2000UL);
    feed_text(&modem, "X", 2100UL);
    collect_data_actions(&modem, net_data, sizeof(net_data), session_id);
    expect_string("canceled post-guard escape becomes payload", net_data, "+++X");

    feed_text(&modem, "+++", 3000UL);
    vm_modem_poll(&modem, 3500UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("second escape output", output, "\r\nOK\r\n");

    feed_text(&modem, "ATH0\r", 3600UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATH0 output", output, "\r\nOK\r\n");
    expect_int("idle after ATH0",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_HANGUP_REQ,
                          NULL,
                          "hangup request after ATH0");
    expect_int("late remote close rejected after ATH0",
               vm_modem_on_remote_closed(&modem, session_id),
               0);
}

static void test_helper_loss_and_reconnect(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATDTexample.com:23\r", 10UL);
    session_id = expect_action_payload(&modem,
                                       VM_MODEM_ACTION_CONNECT_REQ,
                                       "example.com:23",
                                       "connect request before helper loss");
    helper_complete_connect(&modem, session_id, 20UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("connect output before helper loss",
                  output,
                  "\r\nCONNECT\r\n98MODEM FAKE PEER READY\r\n");

    vm_modem_set_helper_available(&modem, 0);
    drain_all(&modem, output, sizeof(output));
    expect_string("helper loss output", output, "\r\nNO CARRIER\r\n");
    expect_int("helper unavailable after loss",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_HELPER_UNAVAILABLE);
    expect_int("stale remote close after helper loss",
               vm_modem_on_remote_closed(&modem, session_id),
               0);

    vm_modem_set_helper_available(&modem, 1);
    expect_int("back to idle after helper returns",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
}

static void test_status_words_output_clear_and_port_close(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;

    vm_modem_init(&modem);
    expect_int("closed status after init",
               vm_modem_get_status(&modem),
               0L);

    open_modem(&modem, 0UL, 1);
    expect_int("idle open status",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR);

    feed_text(&modem, "AT\r", 10UL);
    expect_int("output queued before clear",
               vm_modem_output_count(&modem) != 0,
               1);
    vm_modem_clear_output(&modem);
    expect_int("output cleared",
               vm_modem_output_count(&modem),
               0L);
    drain_all(&modem, output, sizeof(output));
    expect_string("no output after clear", output, "");

    open_modem(&modem, 0UL, 0);
    expect_int("helper unavailable open status",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR);

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);
    expect_int("ring accepted for status test",
               vm_modem_on_inbound_ring(&modem, 200UL),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("ring output for status test", output, "\r\nRING\r\n");
    expect_int("ringing status word",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR | VM_MODEM_STATUS_RING);
    vm_modem_poll(&modem, VM_MODEM_RING_ON_MS);
    expect_int("ringing status after ring on interval",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR);

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);
    feed_text(&modem, "ATDTexample.com:23\r", 20UL);
    session_id = expect_action_payload(&modem,
                                       VM_MODEM_ACTION_CONNECT_REQ,
                                       "example.com:23",
                                       "connect request before status close");
    helper_complete_connect(&modem, session_id, 30UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("connect output for status test",
                  output,
                  "\r\nCONNECT\r\n98MODEM FAKE PEER READY\r\n");
    expect_int("connected status word",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR | VM_MODEM_STATUS_DCD);

    vm_modem_port_close(&modem);
    expect_int("closed status after port close",
               vm_modem_get_status(&modem),
               0L);
    expect_int("no output after port close",
               vm_modem_output_count(&modem),
               0L);
    expect_no_action(&modem, "no action after port close");
}

static void test_dtr_drop_hangup(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATDTexample.com:23\r", 10UL);
    session_id = expect_action_payload(&modem,
                                       VM_MODEM_ACTION_CONNECT_REQ,
                                       "example.com:23",
                                       "connect request before DTR drop");
    helper_complete_connect(&modem, session_id, 20UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("connect output before DTR drop",
                  output,
                  "\r\nCONNECT\r\n98MODEM FAKE PEER READY\r\n");

    vm_modem_on_serial_from_helper(&modem,
                                   session_id,
                                   (const unsigned char *)"RX",
                                   test_strlen_u16("RX"));
    vm_modem_set_host_lines(&modem, 0, 1, 30UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("DTR drop output", output, "RX\r\nNO CARRIER\r\n");
    expect_int("idle after DTR drop",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_HANGUP_REQ,
                          NULL,
                          "hangup request after DTR drop");
    expect_int("stale remote close after DTR drop",
               vm_modem_on_remote_closed(&modem, session_id),
               0);
    vm_modem_set_host_lines(&modem, 0, 1, 31UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("repeated low DTR is idempotent", output, "");
    expect_no_action(&modem, "repeated low DTR no action");
    vm_modem_set_host_lines(&modem, 1, 1, 32UL);
    vm_modem_set_host_lines(&modem, 1, 1, 33UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("repeated high DTR is idempotent", output, "");
    expect_no_action(&modem, "repeated high DTR no action");
}

static void test_dtr_drop_dialing_ringing_and_connected_cmd(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATDTexample.com:23\r", 10UL);
    session_id = expect_action_payload(&modem,
                                       VM_MODEM_ACTION_CONNECT_REQ,
                                       "example.com:23",
                                       "connect request before dialing DTR");
    vm_modem_set_host_lines(&modem, 0, 1, 20UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("DTR drop during dialing output", output, "\r\nNO CARRIER\r\n");
    expect_int("idle after DTR drop during dialing",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_HANGUP_REQ,
                          NULL,
                          "hangup request after dialing DTR");
    expect_int("stale connect ok after dialing DTR",
               vm_modem_on_connect_ok(&modem, session_id, 30UL),
               0);

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    session_id = 301UL;
    expect_int("ring accepted before DTR drop while ringing",
               vm_modem_on_inbound_ring(&modem, session_id),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("ring output before DTR drop while ringing",
                  output,
                  "\r\nRING\r\n");
    vm_modem_set_host_lines(&modem, 0, 1, 100UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("DTR drop during ringing output", output, "\r\nNO CARRIER\r\n");
    expect_int("idle after DTR drop during ringing",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_HANGUP_REQ,
                          NULL,
                          "hangup request after ringing DTR");

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATDTexample.com:23\r", 200UL);
    session_id = expect_action_payload(&modem,
                                       VM_MODEM_ACTION_CONNECT_REQ,
                                       "example.com:23",
                                       "connect request before connected-cmd DTR");
    helper_complete_connect(&modem, session_id, 220UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("connect output before connected-cmd DTR",
                  output,
                  "\r\nCONNECT\r\n98MODEM FAKE PEER READY\r\n");
    feed_text(&modem, "+++", 800UL);
    vm_modem_poll(&modem, 1300UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("escape output before connected-cmd DTR", output, "\r\nOK\r\n");
    expect_int("connected cmd before DTR drop",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_CONNECTED_CMD);

    vm_modem_set_host_lines(&modem, 0, 1, 1400UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("DTR drop during connected cmd output",
                  output,
                  "\r\nNO CARRIER\r\n");
    expect_int("idle after DTR drop during connected cmd",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_HANGUP_REQ,
                          NULL,
                          "hangup request after connected cmd DTR");
}

static void test_inbound_ring_answer_and_atz(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    session_id = 77UL;
    expect_int("inbound ring accepted",
               vm_modem_on_inbound_ring(&modem, session_id),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("inbound ring output", output, "\r\nRING\r\n");
    expect_int("ringing state",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_RINGING);
    expect_int("ring status",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR | VM_MODEM_STATUS_RING);

    feed_text(&modem, "ATA\r", 100UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATA waits for helper connect", output, "");
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_ANSWER_REQ,
                          NULL,
                          "answer request");
    expect_int("dialing after ATA",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_DIALING);

    helper_complete_connect(&modem, session_id, 120UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATA connect output",
                  output,
                  "\r\nCONNECT\r\n98MODEM FAKE PEER READY\r\n");

    feed_text(&modem, "+++", 700UL);
    vm_modem_poll(&modem, 1200UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("escape before ATZ", output, "\r\nOK\r\n");

    feed_text(&modem, "ATZ\r", 1300UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATZ output", output, "\r\nOK\r\n");
    expect_int("ATZ reset state",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_int("ATZ reset echo",
               vm_modem_get_echo_enabled(&modem),
               1);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_HANGUP_REQ,
                          NULL,
                          "hangup request after ATZ");
}

static void test_inbound_ring_cadence_remote_close_and_timeout(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    session_id = 77UL;
    expect_int("first inbound ring accepted",
               vm_modem_on_inbound_ring(&modem, session_id),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("first ring output", output, "\r\nRING\r\n");
    expect_int("ring asserted immediately",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR | VM_MODEM_STATUS_RING);

    vm_modem_poll(&modem, VM_MODEM_RING_ON_MS - 1UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("no extra output before ring-on ends", output, "");
    expect_int("ring still asserted before ring-on ends",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR | VM_MODEM_STATUS_RING);

    vm_modem_poll(&modem, VM_MODEM_RING_ON_MS);
    expect_int("ring deasserted after ring-on interval",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR);

    vm_modem_poll(&modem, VM_MODEM_RING_ON_MS + VM_MODEM_RING_OFF_MS - 1UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("no cadence output before next burst", output, "");

    vm_modem_poll(&modem, VM_MODEM_RING_ON_MS + VM_MODEM_RING_OFF_MS);
    drain_all(&modem, output, sizeof(output));
    expect_string("second cadence ring output", output, "\r\nRING\r\n");
    expect_int("ring asserted on second cadence",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR | VM_MODEM_STATUS_RING);

    vm_modem_poll(&modem,
                  VM_MODEM_RING_ON_MS + VM_MODEM_RING_OFF_MS +
                  VM_MODEM_RING_ON_MS);
    expect_int("ring deasserted after second ring-on interval",
               vm_modem_get_status(&modem),
               VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR);

    expect_int("pending remote close accepted",
               vm_modem_on_remote_closed(&modem, session_id),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("pending remote close output", output, "\r\nNO CARRIER\r\n");
    expect_int("idle after pending remote close",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_no_action(&modem, "no hangup action on pending remote close");

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    session_id = 88UL;
    expect_int("timeout ring accepted",
               vm_modem_on_inbound_ring(&modem, session_id),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("timeout initial ring output", output, "\r\nRING\r\n");

    vm_modem_poll(&modem, VM_MODEM_RING_TIMEOUT_MS);
    drain_all(&modem, output, sizeof(output));
    expect_string("timeout output", output, "\r\nNO CARRIER\r\n");
    expect_int("idle after unanswered timeout",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_HANGUP_REQ,
                          NULL,
                          "hangup request after unanswered timeout");
}

static void test_inbound_ring_reject_and_helper_loss(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    expect_int("ATH0 ring accepted",
               vm_modem_on_inbound_ring(&modem, 101UL),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATH0 ring output", output, "\r\nRING\r\n");

    feed_text(&modem, "ATH0\r", 100UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATH0 while ringing output", output, "\r\nOK\r\n");
    expect_int("idle after ATH0 while ringing",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_HANGUP_REQ,
                          NULL,
                          "hangup request after ATH0 while ringing");

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    expect_int("ATZ ring accepted",
               vm_modem_on_inbound_ring(&modem, 102UL),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATZ ring output", output, "\r\nRING\r\n");

    feed_text(&modem, "ATZ\r", 100UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("ATZ while ringing output", output, "\r\nOK\r\n");
    expect_int("idle after ATZ while ringing",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_IDLE_CMD);
    expect_int("echo restored after ATZ while ringing",
               vm_modem_get_echo_enabled(&modem),
               1);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_HANGUP_REQ,
                          NULL,
                          "hangup request after ATZ while ringing");

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    expect_int("helper loss ring accepted",
               vm_modem_on_inbound_ring(&modem, 103UL),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("helper loss initial ring output", output, "\r\nRING\r\n");

    vm_modem_set_helper_available(&modem, 0);
    drain_all(&modem, output, sizeof(output));
    expect_string("helper loss while ringing output", output, "\r\nNO CARRIER\r\n");
    expect_int("helper unavailable after ringing loss",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_HELPER_UNAVAILABLE);
    expect_no_action(&modem, "no hangup action on helper loss while ringing");
}

static void test_inbound_auto_answer_s0_and_s1(void)
{
    VM_MODEM_CORE modem;
    char output[TEST_OUTPUT_CAP];
    unsigned long session_id;

    open_modem(&modem, 0UL, 1);
    quiet_modem(&modem);

    feed_text(&modem, "ATS0=2\r", 10UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("set S0 for auto-answer", output, "\r\nOK\r\n");

    session_id = 401UL;
    expect_int("auto-answer inbound ring accepted",
               vm_modem_on_inbound_ring(&modem, session_id),
               1);
    drain_all(&modem, output, sizeof(output));
    expect_string("first auto-answer ring output", output, "\r\nRING\r\n");
    expect_int("S1 after first ring",
               (long)vm_modem_get_s1_ring_count(&modem),
               1L);
    expect_no_action(&modem, "no answer action on first ring");
    expect_int("still ringing before auto-answer threshold",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_RINGING);

    vm_modem_poll(&modem, 10UL + VM_MODEM_RING_ON_MS + VM_MODEM_RING_OFF_MS);
    drain_all(&modem, output, sizeof(output));
    expect_string("second auto-answer ring suppressed", output, "");
    expect_int("state becomes dialing for auto-answer",
               vm_modem_get_state(&modem),
               VM_MODEM_STATE_DIALING);
    expect_action_payload(&modem,
                          VM_MODEM_ACTION_ANSWER_REQ,
                          NULL,
                          "auto-answer request after S0 rings");

    helper_complete_connect(&modem, session_id, 9000UL);
    drain_all(&modem, output, sizeof(output));
    expect_string("auto-answer connect output",
                  output,
                  "\r\nCONNECT\r\n98MODEM FAKE PEER READY\r\n");
    expect_int("S1 reset after connect",
               (long)vm_modem_get_s1_ring_count(&modem),
               0L);
}

int main(void)
{
    test_degraded_without_helper();
    test_echo_toggle();
    test_raw_mode_and_s_registers();
    test_error_and_overflow();
    test_connect_and_data_path();
    test_large_connected_write_batches();
    test_phone_style_dial_reaches_helper();
    test_connect_fail_and_stale_events();
    test_connect_fail_reason_mapping();
    test_escape_ato_and_hangup();
    test_helper_loss_and_reconnect();
    test_status_words_output_clear_and_port_close();
    test_dtr_drop_hangup();
    test_dtr_drop_dialing_ringing_and_connected_cmd();
    test_inbound_ring_answer_and_atz();
    test_inbound_ring_cadence_remote_close_and_timeout();
    test_inbound_ring_reject_and_helper_loss();
    test_inbound_auto_answer_s0_and_s1();

    if (g_failures != 0) {
        fprintf(stderr, "%d modem-core tests failed\n", g_failures);
        return 1;
    }

    printf("modem_core_tests: all checks passed\n");
    return 0;
}
