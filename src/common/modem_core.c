/*
 * modem_core.c - Shared modem parser and helper-driven session logic.
 *
 * The modem core stays transport-agnostic: it produces connect/data/answer/
 * hangup actions and consumes helper events for connect, inbound bytes,
 * rings, and disconnects.
 */

#include "ipc_shared.h"
#include "modem_core.h"

#define VM_MODEM_RESULT_CONNECT     "CONNECT"
#define VM_MODEM_RESULT_RING        "RING"
#define VM_MODEM_RESULT_NO_ANSWER   "NO ANSWER"
#define VM_MODEM_RESULT_NO_CARRIER  "NO CARRIER"

#define VM_RC_OK            0U
#define VM_RC_CONNECT       1U
#define VM_RC_RING          2U
#define VM_RC_NO_CARRIER    3U
#define VM_RC_ERROR         4U
#define VM_RC_NO_ANSWER     8U
#define VM_RC_NONE          0xFFFFU

static void vm_modem_sync_status(VM_MODEM_CORE *modem);
static void vm_modem_reset_command(VM_MODEM_CORE *modem);
static void vm_modem_reset_escape(VM_MODEM_CORE *modem);
static void vm_modem_clear_actions(VM_MODEM_CORE *modem);
static void vm_modem_queue_text(VM_MODEM_CORE *modem, const char *text);
static void vm_modem_queue_bytes(VM_MODEM_CORE *modem,
                                 const unsigned char *bytes,
                                 unsigned short count);
static void vm_modem_queue_result(VM_MODEM_CORE *modem, const char *text);
static void vm_modem_queue_value_result(VM_MODEM_CORE *modem,
                                        const char *prefix,
                                        unsigned long value);
static int vm_modem_time_reached(unsigned long now_ms,
                                 unsigned long deadline_ms);
static void vm_modem_reset_ring_state(VM_MODEM_CORE *modem);
static void vm_modem_enter_idle(VM_MODEM_CORE *modem);
static void vm_modem_enter_helper_unavailable(VM_MODEM_CORE *modem);
static void vm_modem_enter_connected_data(VM_MODEM_CORE *modem,
                                          unsigned long now_ms);
static void vm_modem_enter_connected_cmd(VM_MODEM_CORE *modem);
static void vm_modem_start_ringing(VM_MODEM_CORE *modem,
                                   unsigned long now_ms);
static unsigned int vm_modem_connect_fail_result(unsigned long reason);
static unsigned char vm_modem_ascii_upper(unsigned char ch);
static int vm_modem_command_starts_with(const VM_MODEM_CORE *modem,
                                        unsigned short start,
                                        const char *text);
static int vm_modem_is_valid_destination(const VM_MODEM_CORE *modem,
                                         unsigned short start);
static int vm_modem_queue_action(VM_MODEM_CORE *modem,
                                 VM_MODEM_ACTION_TYPE type,
                                 unsigned long session_id,
                                 unsigned long status,
                                 const unsigned char *payload,
                                 unsigned short payload_length);
static int vm_modem_queue_connect_request(VM_MODEM_CORE *modem,
                                          unsigned short start);
static void vm_modem_queue_data_bytes(VM_MODEM_CORE *modem,
                                      unsigned long session_id,
                                      const unsigned char *bytes,
                                      unsigned short count);
static void vm_modem_flush_pending_escape(VM_MODEM_CORE *modem,
                                          unsigned long session_id);
static void vm_modem_terminate_session(VM_MODEM_CORE *modem,
                                       int queue_hangup,
                                       unsigned int result_code);
static unsigned short vm_modem_scan_uint(const VM_MODEM_CORE *modem,
                                          unsigned short pos,
                                          unsigned long *value_out);
static void vm_modem_send_result(VM_MODEM_CORE *modem, unsigned int code);
static void vm_modem_process_command(VM_MODEM_CORE *modem,
                                     unsigned long now_ms);

static void vm_modem_sync_status(VM_MODEM_CORE *modem)
{
    unsigned long status;

    if (modem == 0) {
        return;
    }

    status = 0;
    if (modem->port_open) {
        status = VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR;
        if (modem->state == VM_MODEM_STATE_CONNECTED_DATA ||
            modem->state == VM_MODEM_STATE_CONNECTED_CMD) {
            status |= VM_MODEM_STATUS_DCD;
        }
        if (modem->ring_signal_asserted) {
            status |= VM_MODEM_STATUS_RING;
        }
    }

    modem->modem_status = status;
}

static void vm_modem_reset_command(VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return;
    }

    modem->command_length = 0;
    modem->command_overflowed = 0;
}

static void vm_modem_reset_escape(VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return;
    }

    modem->escape_plus_count = 0;
    modem->escape_post_guard_pending = 0;
    modem->escape_post_guard_deadline_ms = 0;
}

static void vm_modem_clear_actions(VM_MODEM_CORE *modem)
{
    unsigned short i;

    if (modem == 0) {
        return;
    }

    modem->action_get = 0;
    modem->action_put = 0;
    modem->action_count = 0;

    for (i = 0; i < VM_MODEM_ACTION_QUEUE_LEN; ++i) {
        modem->action_queue[i].type = VM_MODEM_ACTION_NONE;
        modem->action_queue[i].session_id = 0;
        modem->action_queue[i].status = 0;
        modem->action_queue[i].payload_length = 0;
    }
}

static void vm_modem_queue_text(VM_MODEM_CORE *modem, const char *text)
{
    unsigned char ch;

    if (modem == 0 || text == 0) {
        return;
    }

    while (*text != '\0' && modem->output_count < VM_MODEM_OUTPUT_BUFFER_LEN) {
        ch = (unsigned char)(*text);
        modem->output_buffer[modem->output_put] = ch;
        ++modem->output_put;
        if (modem->output_put >= VM_MODEM_OUTPUT_BUFFER_LEN) {
            modem->output_put = 0;
        }
        ++modem->output_count;
        ++text;
    }
}

static void vm_modem_queue_bytes(VM_MODEM_CORE *modem,
                                 const unsigned char *bytes,
                                 unsigned short count)
{
    unsigned short i;

    if (modem == 0 || bytes == 0 || count == 0) {
        return;
    }

    for (i = 0; i < count; ++i) {
        if (modem->output_count >= VM_MODEM_OUTPUT_BUFFER_LEN) {
            break;
        }

        modem->output_buffer[modem->output_put] = bytes[i];
        ++modem->output_put;
        if (modem->output_put >= VM_MODEM_OUTPUT_BUFFER_LEN) {
            modem->output_put = 0;
        }
        ++modem->output_count;
    }
}

static void vm_modem_queue_result(VM_MODEM_CORE *modem, const char *text)
{
    static const unsigned char crlf[2] = { '\r', '\n' };

    if (modem == 0 || text == 0) {
        return;
    }

    vm_modem_queue_bytes(modem, crlf, 2);
    vm_modem_queue_text(modem, text);
    vm_modem_queue_bytes(modem, crlf, 2);
}

static void vm_modem_queue_value_result(VM_MODEM_CORE *modem,
                                        const char *prefix,
                                        unsigned long value)
{
    static const unsigned char crlf[2] = { '\r', '\n' };
    char digits[16];
    unsigned short count;

    if (modem == 0 || prefix == 0) {
        return;
    }

    vm_modem_queue_bytes(modem, crlf, 2);
    vm_modem_queue_text(modem, prefix);

    count = 0;
    do {
        digits[count] = (char)('0' + (value % 10UL));
        ++count;
        value /= 10UL;
    } while (value != 0UL && count < (unsigned short)sizeof(digits));

    while (count != 0U) {
        --count;
        vm_modem_queue_bytes(modem, (const unsigned char *)&digits[count], 1);
    }

    vm_modem_queue_bytes(modem, crlf, 2);
}

static void vm_modem_send_result(VM_MODEM_CORE *modem, unsigned int code)
{
    static const char * const verbose_text[] = {
        "OK",         /* 0 */
        "CONNECT",    /* 1 */
        "RING",       /* 2 */
        "NO CARRIER", /* 3 */
        "ERROR",      /* 4 */
        0, 0, 0,      /* 5-7 unused */
        "NO ANSWER"   /* 8 */
    };

    if (modem == 0 || code == VM_RC_NONE) {
        return;
    }
    if (modem->quiet_mode == 1) {
        return;
    }
    if (modem->quiet_mode == 2 &&
        (code == VM_RC_RING ||
         code == VM_RC_CONNECT ||
         code == VM_RC_NO_CARRIER)) {
        return;
    }
    if (modem->numeric_responses) {
        vm_modem_queue_value_result(modem, "", (unsigned long)code);
    } else if (code < 9U && verbose_text[code] != 0) {
        vm_modem_queue_result(modem, verbose_text[code]);
    }
}

static int vm_modem_time_reached(unsigned long now_ms,
                                 unsigned long deadline_ms)
{
    return ((now_ms - deadline_ms) < 0x80000000UL) ? 1 : 0;
}

static void vm_modem_reset_ring_state(VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return;
    }

    modem->ring_signal_asserted = 0;
    modem->ring_next_time_ms = 0;
    modem->ring_signal_deadline_ms = 0;
    modem->ring_timeout_deadline_ms = 0;
    modem->s1_ring_count = 0;
}

static void vm_modem_enter_idle(VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return;
    }

    modem->state = VM_MODEM_STATE_IDLE_CMD;
    vm_modem_reset_escape(modem);
    vm_modem_reset_ring_state(modem);
    vm_modem_sync_status(modem);
}

static void vm_modem_enter_helper_unavailable(VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return;
    }

    modem->state = VM_MODEM_STATE_HELPER_UNAVAILABLE;
    vm_modem_reset_escape(modem);
    vm_modem_reset_ring_state(modem);
    vm_modem_sync_status(modem);
}

static void vm_modem_enter_connected_data(VM_MODEM_CORE *modem,
                                          unsigned long now_ms)
{
    if (modem == 0) {
        return;
    }

    modem->state = VM_MODEM_STATE_CONNECTED_DATA;
    modem->have_last_tx_time = 1;
    modem->last_tx_time_ms = now_ms;
    modem->last_clock_ms = now_ms;
    vm_modem_reset_escape(modem);
    vm_modem_reset_ring_state(modem);
    vm_modem_sync_status(modem);
}

static void vm_modem_enter_connected_cmd(VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return;
    }

    modem->state = VM_MODEM_STATE_CONNECTED_CMD;
    vm_modem_reset_escape(modem);
    vm_modem_reset_ring_state(modem);
    vm_modem_sync_status(modem);
}

static void vm_modem_start_ringing(VM_MODEM_CORE *modem,
                                   unsigned long now_ms)
{
    int should_auto_answer;

    if (modem == 0) {
        return;
    }

    modem->state = VM_MODEM_STATE_RINGING;
    modem->last_clock_ms = now_ms;
    vm_modem_reset_escape(modem);
    modem->ring_signal_asserted = 1;
    modem->ring_signal_deadline_ms = now_ms + VM_MODEM_RING_ON_MS;
    modem->ring_next_time_ms = now_ms + VM_MODEM_RING_ON_MS +
                               VM_MODEM_RING_OFF_MS;
    if (modem->ring_timeout_deadline_ms == 0UL) {
        modem->ring_timeout_deadline_ms = now_ms + VM_MODEM_RING_TIMEOUT_MS;
    }
    ++modem->s1_ring_count;
    vm_modem_sync_status(modem);

    should_auto_answer =
        (modem->helper_available &&
         modem->current_session_id != 0UL &&
         modem->s0_auto_answer_rings != 0UL &&
         modem->s1_ring_count >= modem->s0_auto_answer_rings) ? 1 : 0;

    if (should_auto_answer) {
        vm_modem_clear_actions(modem);
        if (!vm_modem_queue_action(modem,
                                   VM_MODEM_ACTION_ANSWER_REQ,
                                   modem->current_session_id,
                                   0UL,
                                   0,
                                   0U)) {
            vm_modem_terminate_session(modem, 1, VM_RC_NO_CARRIER);
            return;
        }
        modem->state = VM_MODEM_STATE_DIALING;
        vm_modem_reset_ring_state(modem);
        vm_modem_sync_status(modem);
        return;
    }

    vm_modem_send_result(modem, VM_RC_RING);
}

static unsigned int vm_modem_connect_fail_result(unsigned long reason)
{
    if (reason == VMODEM_CONNECT_FAIL_TIMEOUT) {
        return VM_RC_NO_ANSWER;
    }

    return VM_RC_NO_CARRIER;
}

static unsigned char vm_modem_ascii_upper(unsigned char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (unsigned char)(ch - ('a' - 'A'));
    }
    return ch;
}

static int vm_modem_is_command_printable(unsigned char ch)
{
    return (ch >= 0x20U && ch <= 0x7EU) ? 1 : 0;
}

static int vm_modem_command_starts_with(const VM_MODEM_CORE *modem,
                                        unsigned short start,
                                        const char *text)
{
    unsigned short i;
    unsigned char ch;

    if (modem == 0 || text == 0 || start > modem->command_length) {
        return 0;
    }

    i = start;
    while (*text != '\0') {
        if (i >= modem->command_length) {
            return 0;
        }
        ch = vm_modem_ascii_upper(modem->command_buffer[i]);
        if (ch != (unsigned char)(*text)) {
            return 0;
        }
        ++i;
        ++text;
    }

    return 1;
}

static unsigned short vm_modem_scan_uint(const VM_MODEM_CORE *modem,
                                          unsigned short pos,
                                          unsigned long *value_out)
{
    unsigned long value;

    if (modem == 0 || value_out == 0) {
        return pos;
    }
    value = 0UL;
    while (pos < modem->command_length) {
        unsigned char ch = modem->command_buffer[pos];
        if (ch < '0' || ch > '9') break;
        value = (value * 10UL) + (unsigned long)(ch - '0');
        ++pos;
    }
    *value_out = value;
    return pos;
}

static int vm_modem_is_valid_destination(const VM_MODEM_CORE *modem,
                                         unsigned short start)
{
    unsigned short i;
    unsigned short colon_index;
    unsigned short digit_count;
    int host_len;
    int port_len;
    unsigned long port_value;
    unsigned char ch;

    if (modem == 0 || start >= modem->command_length) {
        return 0;
    }

    colon_index = modem->command_length;
    digit_count = 0;
    for (i = start; i < modem->command_length; ++i) {
        ch = modem->command_buffer[i];
        if (ch < 0x21 || ch > 0x7E) {
            return 0;
        }
        if (ch == ':') {
            if (colon_index != modem->command_length) {
                return 0;
            }
            colon_index = i;
        } else if (ch >= '0' && ch <= '9') {
            ++digit_count;
        }
    }

    if (colon_index == modem->command_length) {
        if (digit_count == 0) {
            return 0;
        }

        for (i = start; i < modem->command_length; ++i) {
            ch = modem->command_buffer[i];
            if ((ch >= '0' && ch <= '9') ||
                ch == ' ' ||
                ch == '-' ||
                ch == '.' ||
                ch == '(' ||
                ch == ')' ||
                ch == '+') {
                continue;
            }
            return 0;
        }

        return (digit_count != 0) ? 1 : 0;
    }

    host_len = (int)(colon_index - start);
    port_len = (int)(modem->command_length - (colon_index + 1));
    if (host_len <= 0 || port_len <= 0) {
        return 0;
    }

    port_value = 0;
    for (i = (unsigned short)(colon_index + 1); i < modem->command_length; ++i) {
        ch = modem->command_buffer[i];
        if (ch < '0' || ch > '9') {
            return 0;
        }
        port_value = (port_value * 10UL) + (unsigned long)(ch - '0');
        if (port_value > 65535UL) {
            return 0;
        }
    }

    if (port_value == 0UL) {
        return 0;
    }

    return 1;
}

static int vm_modem_queue_action(VM_MODEM_CORE *modem,
                                 VM_MODEM_ACTION_TYPE type,
                                 unsigned long session_id,
                                 unsigned long status,
                                 const unsigned char *payload,
                                 unsigned short payload_length)
{
    VM_MODEM_ACTION *action;
    unsigned short i;

    if (modem == 0 || type == VM_MODEM_ACTION_NONE) {
        return 0;
    }

    if (payload_length > VM_MODEM_ACTION_PAYLOAD_LEN) {
        return 0;
    }

    if (modem->action_count >= VM_MODEM_ACTION_QUEUE_LEN) {
        return 0;
    }

    action = &modem->action_queue[modem->action_put];
    action->type = type;
    action->session_id = session_id;
    action->status = status;
    action->payload_length = payload_length;
    if (payload != 0 && payload_length != 0) {
        for (i = 0; i < payload_length; ++i) {
            action->payload[i] = payload[i];
        }
    }

    ++modem->action_put;
    if (modem->action_put >= VM_MODEM_ACTION_QUEUE_LEN) {
        modem->action_put = 0;
    }
    ++modem->action_count;
    return 1;
}

static int vm_modem_queue_connect_request(VM_MODEM_CORE *modem,
                                          unsigned short start)
{
    unsigned short length;
    unsigned long flags;

    if (modem == 0 || start >= modem->command_length) {
        return 0;
    }

    length = (unsigned short)(modem->command_length - start);
    flags = modem->raw_mode_enabled ? VMODEM_CONNECT_FLAG_RAW : 0UL;
    return vm_modem_queue_action(modem,
                                 VM_MODEM_ACTION_CONNECT_REQ,
                                 modem->current_session_id,
                                 flags,
                                 modem->command_buffer + start,
                                 length);
}

static void vm_modem_queue_data_bytes(VM_MODEM_CORE *modem,
                                      unsigned long session_id,
                                      const unsigned char *bytes,
                                      unsigned short count)
{
    unsigned short offset;
    unsigned short chunk;

    if (modem == 0 || bytes == 0 || count == 0 || session_id == 0) {
        return;
    }

    offset = 0;
    while (offset < count) {
        chunk = (unsigned short)(count - offset);
        if (chunk > VM_MODEM_ACTION_PAYLOAD_LEN) {
            chunk = VM_MODEM_ACTION_PAYLOAD_LEN;
        }

        if (!vm_modem_queue_action(modem,
                                   VM_MODEM_ACTION_DATA_TO_NET,
                                   session_id,
                                   0,
                                   bytes + offset,
                                   chunk)) {
            return;
        }

        offset = (unsigned short)(offset + chunk);
    }
}

static void vm_modem_flush_pending_escape(VM_MODEM_CORE *modem,
                                          unsigned long session_id)
{
    static const unsigned char pluses[3] = { '+', '+', '+' };

    if (modem == 0 || modem->escape_plus_count == 0) {
        return;
    }

    vm_modem_queue_data_bytes(modem,
                              session_id,
                              pluses,
                              modem->escape_plus_count);
    vm_modem_reset_escape(modem);
}

static void vm_modem_terminate_session(VM_MODEM_CORE *modem,
                                       int queue_hangup,
                                       unsigned int result_code)
{
    unsigned long session_id;

    if (modem == 0) {
        return;
    }

    session_id = modem->current_session_id;
    vm_modem_clear_actions(modem);
    if (queue_hangup && modem->helper_available && session_id != 0) {
        vm_modem_queue_action(modem,
                              VM_MODEM_ACTION_HANGUP_REQ,
                              session_id,
                              0,
                              0,
                              0);
    }

    modem->current_session_id = 0;
    if (modem->helper_available) {
        vm_modem_enter_idle(modem);
    } else {
        vm_modem_enter_helper_unavailable(modem);
    }

    vm_modem_send_result(modem, result_code);
}

static void vm_modem_process_command(VM_MODEM_CORE *modem,
                                     unsigned long now_ms)
{
    unsigned short pos;
    unsigned short old_pos;
    unsigned char ch;
    unsigned char ch2;
    unsigned long value;
    unsigned long reg_value;

    if (modem == 0) {
        return;
    }

    if (modem->command_overflowed || modem->command_length < 2) {
        vm_modem_send_result(modem, VM_RC_ERROR);
        vm_modem_reset_command(modem);
        return;
    }

    if (vm_modem_ascii_upper(modem->command_buffer[0]) != 'A' ||
        vm_modem_ascii_upper(modem->command_buffer[1]) != 'T') {
        vm_modem_send_result(modem, VM_RC_ERROR);
        vm_modem_reset_command(modem);
        return;
    }

    pos = 2;

    while (pos < modem->command_length) {
        ch = vm_modem_ascii_upper(modem->command_buffer[pos]);
        ++pos;

        switch (ch) {
        case ' ':
            break; /* space between sub-commands — skip */
        case 'E':
            pos = vm_modem_scan_uint(modem, pos, &value);
            if (value == 0) {
                modem->echo_enabled = 0;
            } else if (value == 1) {
                modem->echo_enabled = 1;
            } else {
                goto send_error;
            }
            break;

        case 'Q':
            pos = vm_modem_scan_uint(modem, pos, &value);
            if (value <= 2) {
                modem->quiet_mode = (unsigned char)value;
            } else {
                goto send_error;
            }
            break;

        case 'V':
            pos = vm_modem_scan_uint(modem, pos, &value);
            if (value == 0) {
                modem->numeric_responses = 1;
            } else if (value == 1) {
                modem->numeric_responses = 0;
            } else {
                goto send_error;
            }
            break;

        case 'X':
            pos = vm_modem_scan_uint(modem, pos, &value);
            if (value > 4) {
                goto send_error;
            }
            /* stub: we always send full responses regardless of level */
            break;

        case 'M': /* speaker mode - stub */
        case 'L': /* volume - stub */
        case 'T': /* tone dial - ignored */
        case 'P': /* pulse dial - ignored */
            pos = vm_modem_scan_uint(modem, pos, &value);
            break;

        case 'I': /* firmware info - stub */
            pos = vm_modem_scan_uint(modem, pos, &value);
            break;

        case 'Z': /* reset */
            pos = vm_modem_scan_uint(modem, pos, &value);
            modem->echo_enabled = 1;
            modem->raw_mode_enabled = 1;
            modem->quiet_mode = 0;
            modem->numeric_responses = 0;
            modem->s0_auto_answer_rings = 0UL;
            modem->s1_ring_count = 0UL;
            vm_modem_terminate_session(modem, 1, VM_RC_NONE);
            vm_modem_reset_command(modem);
            vm_modem_send_result(modem, VM_RC_OK);
            return;

        case 'A': /* answer incoming call */
            if (modem->state == VM_MODEM_STATE_RINGING &&
                modem->helper_available &&
                modem->current_session_id != 0) {
                vm_modem_clear_actions(modem);
                if (!vm_modem_queue_action(modem,
                                           VM_MODEM_ACTION_ANSWER_REQ,
                                           modem->current_session_id,
                                           0, 0, 0)) {
                    goto send_error;
                }
                modem->state = VM_MODEM_STATE_DIALING;
                vm_modem_reset_ring_state(modem);
                vm_modem_sync_status(modem);
            } else {
                goto send_error;
            }
            vm_modem_reset_command(modem);
            return; /* no OK yet — wait for CONNECT or fail */

        case 'O': /* return to data mode */
            pos = vm_modem_scan_uint(modem, pos, &value);
            if (value != 0) {
                goto send_error;
            }
            if (modem->state == VM_MODEM_STATE_CONNECTED_CMD &&
                modem->current_session_id != 0) {
                vm_modem_enter_connected_data(modem, now_ms);
                vm_modem_reset_command(modem);
                vm_modem_send_result(modem, VM_RC_CONNECT);
                return;
            }
            goto send_error;

        case 'H': /* hang up */
            pos = vm_modem_scan_uint(modem, pos, &value);
            if (value != 0) {
                goto send_error;
            }
            if (modem->state == VM_MODEM_STATE_CONNECTED_DATA ||
                modem->state == VM_MODEM_STATE_CONNECTED_CMD ||
                modem->state == VM_MODEM_STATE_DIALING ||
                modem->state == VM_MODEM_STATE_RINGING) {
                vm_modem_terminate_session(modem, 1, VM_RC_NONE);
            }
            vm_modem_reset_command(modem);
            vm_modem_send_result(modem, VM_RC_OK);
            return;

        case 'D': /* dial — consumes rest of command line */
            if (pos < modem->command_length) {
                ch2 = vm_modem_ascii_upper(modem->command_buffer[pos]);
                if (ch2 == 'T' || ch2 == 'P') {
                    ++pos;
                }
            }
            if (!vm_modem_is_valid_destination(modem, pos)) {
                goto send_error;
            }
            if (!modem->helper_available) {
                vm_modem_enter_helper_unavailable(modem);
                vm_modem_reset_command(modem);
                vm_modem_send_result(modem, VM_RC_NO_CARRIER);
                return;
            }
            modem->current_session_id = modem->next_session_id++;
            vm_modem_clear_actions(modem);
            if (!vm_modem_queue_connect_request(modem, pos)) {
                modem->current_session_id = 0;
                vm_modem_enter_idle(modem);
                vm_modem_reset_command(modem);
                vm_modem_send_result(modem, VM_RC_NO_CARRIER);
                return;
            }
            modem->state = VM_MODEM_STATE_DIALING;
            vm_modem_sync_status(modem);
            vm_modem_reset_command(modem);
            return; /* no OK yet — wait for CONNECT or fail */

        case 'S': { /* S-register read/write */
            pos = vm_modem_scan_uint(modem, pos, &value); /* register index */
            if (pos >= modem->command_length) {
                goto send_error;
            }
            if (modem->command_buffer[pos] == '=') {
                ++pos;
                old_pos = pos;
                pos = vm_modem_scan_uint(modem, pos, &reg_value);
                if (pos == old_pos) {
                    goto send_error; /* no digits after = */
                }
                if (value == 0) {
                    modem->s0_auto_answer_rings = reg_value;
                } else if (value == 1) {
                    goto send_error; /* S1 is read-only */
                }
                /* other S-registers: accept and silently ignore */
            } else if (modem->command_buffer[pos] == '?') {
                ++pos;
                if (value == 0) {
                    vm_modem_queue_value_result(modem, "",
                                                modem->s0_auto_answer_rings);
                } else if (value == 1) {
                    vm_modem_queue_value_result(modem, "",
                                                modem->s1_ring_count);
                } else {
                    vm_modem_queue_value_result(modem, "", 0UL);
                }
            } else {
                goto send_error;
            }
            break;
        }

        case '&': { /* &-prefix commands */
            if (pos >= modem->command_length) {
                goto send_error;
            }
            ch2 = vm_modem_ascii_upper(modem->command_buffer[pos]);
            ++pos;
            pos = vm_modem_scan_uint(modem, pos, &value);
            switch (ch2) {
            case 'C': if (value > 1) goto send_error; break; /* DCD - stub */
            case 'D': if (value > 3) goto send_error; break; /* DTR - stub */
            case 'K': if (value > 5) goto send_error; break; /* flow ctrl - stub */
            default:  goto send_error;
            }
            break;
        }

        case '+': { /* multi-character + commands */
            if (vm_modem_command_starts_with(modem, pos, "VRAW?") &&
                pos + 5U == modem->command_length) {
                vm_modem_queue_value_result(modem, "+VRAW: ",
                                            modem->raw_mode_enabled ? 1UL : 0UL);
                pos += 5U;
            } else if (vm_modem_command_starts_with(modem, pos, "VRAW=") &&
                       pos + 6U <= modem->command_length) {
                ch2 = modem->command_buffer[pos + 5U];
                if (ch2 == '0') {
                    modem->raw_mode_enabled = 0;
                } else if (ch2 == '1') {
                    modem->raw_mode_enabled = 1;
                } else {
                    goto send_error;
                }
                pos += 6U;
            } else {
                goto send_error;
            }
            break;
        }

        case '\\': { /* \-prefix commands */
            if (pos >= modem->command_length) {
                goto send_error;
            }
            ch2 = vm_modem_ascii_upper(modem->command_buffer[pos]);
            ++pos;
            pos = vm_modem_scan_uint(modem, pos, &value);
            switch (ch2) {
            case 'N': if (value > 5) goto send_error; break; /* error corr - stub */
            default:  goto send_error;
            }
            break;
        }

        default:
            goto send_error;
        }
    }

    vm_modem_reset_command(modem);
    vm_modem_send_result(modem, VM_RC_OK);
    return;

send_error:
    vm_modem_reset_command(modem);
    vm_modem_send_result(modem, VM_RC_ERROR);
}

void vm_modem_init(VM_MODEM_CORE *modem)
{
    unsigned short i;
    unsigned short j;

    if (modem == 0) {
        return;
    }

    modem->state = VM_MODEM_STATE_IDLE_CMD;
    modem->modem_status = VM_MODEM_STATUS_CTS | VM_MODEM_STATUS_DSR;
    modem->echo_enabled = 1;
    modem->port_open = 0;
    modem->helper_available = 0;
    modem->host_dtr_asserted = 1;
    modem->host_rts_asserted = 1;
    modem->raw_mode_enabled = 1;
    modem->quiet_mode = 0;
    modem->numeric_responses = 0;
    modem->next_session_id = 1;
    modem->current_session_id = 0;
    modem->s0_auto_answer_rings = 0UL;
    modem->s1_ring_count = 0UL;
    modem->command_length = 0;
    modem->command_overflowed = 0;
    modem->output_get = 0;
    modem->output_put = 0;
    modem->output_count = 0;
    modem->action_get = 0;
    modem->action_put = 0;
    modem->action_count = 0;
    modem->have_last_tx_time = 0;
    modem->last_tx_time_ms = 0;
    modem->last_clock_ms = 0;
    modem->escape_plus_count = 0;
    modem->escape_post_guard_pending = 0;
    modem->escape_post_guard_deadline_ms = 0;
    modem->ring_signal_asserted = 0;
    modem->ring_next_time_ms = 0;
    modem->ring_signal_deadline_ms = 0;
    modem->ring_timeout_deadline_ms = 0;

    for (i = 0; i < VM_MODEM_CMD_BUFFER_LEN; ++i) {
        modem->command_buffer[i] = 0;
    }
    for (i = 0; i < VM_MODEM_OUTPUT_BUFFER_LEN; ++i) {
        modem->output_buffer[i] = 0;
    }
    for (i = 0; i < VM_MODEM_ACTION_QUEUE_LEN; ++i) {
        modem->action_queue[i].type = VM_MODEM_ACTION_NONE;
        modem->action_queue[i].session_id = 0;
        modem->action_queue[i].status = 0;
        modem->action_queue[i].payload_length = 0;
        for (j = 0; j < VM_MODEM_ACTION_PAYLOAD_LEN; ++j) {
            modem->action_queue[i].payload[j] = 0;
        }
    }

    vm_modem_sync_status(modem);
}

void vm_modem_reset(VM_MODEM_CORE *modem)
{
    vm_modem_init(modem);
}

void vm_modem_port_open(VM_MODEM_CORE *modem, unsigned long now_ms)
{
    int helper_available;
    unsigned long next_session_id;

    if (modem == 0) {
        return;
    }

    helper_available = modem->helper_available;
    next_session_id = modem->next_session_id;
    vm_modem_init(modem);
    modem->helper_available = helper_available ? 1 : 0;
    modem->next_session_id = next_session_id;
    modem->port_open = 1;
    modem->have_last_tx_time = 1;
    modem->last_tx_time_ms = now_ms;
    modem->last_clock_ms = now_ms;

    if (modem->helper_available) {
        vm_modem_enter_idle(modem);
    } else {
        vm_modem_enter_helper_unavailable(modem);
    }
}

void vm_modem_port_close(VM_MODEM_CORE *modem)
{
    int helper_available;
    unsigned long next_session_id;

    if (modem == 0) {
        return;
    }

    helper_available = modem->helper_available;
    next_session_id = modem->next_session_id;
    modem->state = VM_MODEM_STATE_CLOSING;
    vm_modem_reset(modem);
    modem->helper_available = helper_available ? 1 : 0;
    modem->next_session_id = next_session_id;
    modem->port_open = 0;
    modem->have_last_tx_time = 0;
    modem->last_clock_ms = 0;
    if (!modem->helper_available) {
        vm_modem_enter_helper_unavailable(modem);
    }
}

void vm_modem_set_helper_available(VM_MODEM_CORE *modem, int available)
{
    int was_available;

    if (modem == 0) {
        return;
    }

    was_available = modem->helper_available;
    modem->helper_available = available ? 1 : 0;

    if (!modem->helper_available) {
        vm_modem_clear_actions(modem);
        if (modem->current_session_id != 0 &&
            (modem->state == VM_MODEM_STATE_DIALING ||
             modem->state == VM_MODEM_STATE_RINGING ||
             modem->state == VM_MODEM_STATE_CONNECTED_DATA ||
             modem->state == VM_MODEM_STATE_CONNECTED_CMD)) {
            modem->current_session_id = 0;
            vm_modem_enter_helper_unavailable(modem);
            vm_modem_send_result(modem, VM_RC_NO_CARRIER);
        } else {
            vm_modem_enter_helper_unavailable(modem);
        }
        return;
    }

    if (!was_available &&
        modem->port_open &&
        modem->state == VM_MODEM_STATE_HELPER_UNAVAILABLE &&
        modem->current_session_id == 0) {
        vm_modem_enter_idle(modem);
    }
}

void vm_modem_set_host_lines(VM_MODEM_CORE *modem,
                             int dtr_asserted,
                             int rts_asserted,
                             unsigned long now_ms)
{
    int was_dtr_asserted;

    if (modem == 0) {
        return;
    }

    modem->last_clock_ms = now_ms;
    was_dtr_asserted = modem->host_dtr_asserted;
    modem->host_dtr_asserted = dtr_asserted ? 1 : 0;
    modem->host_rts_asserted = rts_asserted ? 1 : 0;

    if (was_dtr_asserted &&
        !modem->host_dtr_asserted &&
        modem->current_session_id != 0 &&
        (modem->state == VM_MODEM_STATE_DIALING ||
         modem->state == VM_MODEM_STATE_RINGING ||
         modem->state == VM_MODEM_STATE_CONNECTED_DATA ||
         modem->state == VM_MODEM_STATE_CONNECTED_CMD)) {
        vm_modem_terminate_session(modem, 1, VM_RC_NO_CARRIER);
    }
}

void vm_modem_poll(VM_MODEM_CORE *modem, unsigned long now_ms)
{
    if (modem == 0) {
        return;
    }

    modem->last_clock_ms = now_ms;

    if (modem->state == VM_MODEM_STATE_RINGING) {
        if (modem->ring_timeout_deadline_ms != 0 &&
            vm_modem_time_reached(now_ms, modem->ring_timeout_deadline_ms)) {
            vm_modem_terminate_session(modem, 1, VM_RC_NO_CARRIER);
            return;
        }

        if (modem->ring_signal_asserted &&
            modem->ring_signal_deadline_ms != 0 &&
            vm_modem_time_reached(now_ms, modem->ring_signal_deadline_ms)) {
            modem->ring_signal_asserted = 0;
            modem->ring_signal_deadline_ms = 0;
            vm_modem_sync_status(modem);
        }

        if (modem->ring_next_time_ms != 0 &&
            vm_modem_time_reached(now_ms, modem->ring_next_time_ms)) {
            vm_modem_start_ringing(modem, now_ms);
            if (modem->state != VM_MODEM_STATE_RINGING) {
                return;
            }
        }
    }

    if (modem->escape_post_guard_pending &&
        vm_modem_time_reached(now_ms, modem->escape_post_guard_deadline_ms)) {
        vm_modem_enter_connected_cmd(modem);
        vm_modem_send_result(modem, VM_RC_OK);
    }
}

void vm_modem_ingest_tx(VM_MODEM_CORE *modem,
                        const unsigned char *bytes,
                        unsigned short count,
                        unsigned long now_ms)
{
    unsigned short i;
    unsigned short run_start;
    unsigned short run_length;
    unsigned char ch;
    unsigned long quiet_time;

    if (modem == 0 || bytes == 0 || count == 0) {
        return;
    }

    vm_modem_poll(modem, now_ms);
    run_start = 0;
    run_length = 0;

    for (i = 0; i < count; ++i) {
        ch = bytes[i];

        if (modem->state == VM_MODEM_STATE_CONNECTED_DATA &&
            modem->current_session_id != 0) {
            if (modem->escape_post_guard_pending) {
                if (run_length != 0) {
                    vm_modem_queue_data_bytes(modem,
                                              modem->current_session_id,
                                              bytes + run_start,
                                              run_length);
                    run_length = 0;
                }
                vm_modem_flush_pending_escape(modem, modem->current_session_id);
                if (run_length == 0) {
                    run_start = i;
                }
                ++run_length;
                modem->have_last_tx_time = 1;
                modem->last_tx_time_ms = now_ms;
                continue;
            }

            if (ch == '+') {
                if (modem->escape_plus_count == 0) {
                    if (!modem->have_last_tx_time) {
                        quiet_time = VM_MODEM_ESCAPE_GUARD_MS;
                    } else {
                        quiet_time = now_ms - modem->last_tx_time_ms;
                    }

                    if (quiet_time >= VM_MODEM_ESCAPE_GUARD_MS) {
                        if (run_length != 0) {
                            vm_modem_queue_data_bytes(modem,
                                                      modem->current_session_id,
                                                      bytes + run_start,
                                                      run_length);
                            run_length = 0;
                        }
                        modem->escape_plus_count = 1;
                    } else {
                        if (run_length == 0) {
                            run_start = i;
                        }
                        ++run_length;
                    }
                } else if (modem->escape_plus_count < 3) {
                    ++modem->escape_plus_count;
                    if (modem->escape_plus_count == 3) {
                        modem->escape_post_guard_pending = 1;
                        modem->escape_post_guard_deadline_ms =
                            now_ms + VM_MODEM_ESCAPE_GUARD_MS;
                    }
                }

                modem->have_last_tx_time = 1;
                modem->last_tx_time_ms = now_ms;
                continue;
            }

            if (modem->escape_plus_count != 0) {
                if (run_length != 0) {
                    vm_modem_queue_data_bytes(modem,
                                              modem->current_session_id,
                                              bytes + run_start,
                                              run_length);
                    run_length = 0;
                }
                vm_modem_flush_pending_escape(modem, modem->current_session_id);
            }

            if (run_length == 0) {
                run_start = i;
            }
            ++run_length;
            modem->have_last_tx_time = 1;
            modem->last_tx_time_ms = now_ms;
            continue;
        }

        if (modem->state != VM_MODEM_STATE_IDLE_CMD &&
            modem->state != VM_MODEM_STATE_CONNECTED_CMD &&
            modem->state != VM_MODEM_STATE_RINGING &&
            modem->state != VM_MODEM_STATE_HELPER_UNAVAILABLE) {
            modem->have_last_tx_time = 1;
            modem->last_tx_time_ms = now_ms;
            continue;
        }

        if (ch == '\n') {
            continue;
        }

        if (ch == '\r') {
            if (modem->command_length == 0 && !modem->command_overflowed) {
                continue;
            }
            if (modem->echo_enabled) {
                vm_modem_queue_bytes(modem, &ch, 1);
            }
            vm_modem_process_command(modem, now_ms);
            continue;
        }

        if (ch == 0x08 || ch == 0x7F) {
            if (modem->command_length > 0) {
                --modem->command_length;
            }
            continue;
        }

        if (!vm_modem_is_command_printable(ch)) {
            continue;
        }

        if (modem->command_length == 0) {
            if (vm_modem_ascii_upper(ch) != 'A') {
                continue;
            }
        } else if (vm_modem_ascii_upper(modem->command_buffer[0]) != 'A' &&
                   vm_modem_ascii_upper(ch) == 'A') {
            modem->command_length = 0;
            modem->command_overflowed = 0;
        }

        if (modem->echo_enabled) {
            vm_modem_queue_bytes(modem, &ch, 1);
        }

        if (modem->command_overflowed) {
            continue;
        }

        if (modem->command_length >= VM_MODEM_CMD_BUFFER_LEN) {
            modem->command_overflowed = 1;
            continue;
        }

        modem->command_buffer[modem->command_length] = ch;
        ++modem->command_length;
    }

    if (run_length != 0) {
        vm_modem_queue_data_bytes(modem,
                                  modem->current_session_id,
                                  bytes + run_start,
                                  run_length);
    }
}

int vm_modem_on_connect_ok(VM_MODEM_CORE *modem,
                           unsigned long session_id,
                           unsigned long now_ms)
{
    if (modem == 0 ||
        session_id == 0 ||
        session_id != modem->current_session_id) {
        return 0;
    }

    if (modem->state != VM_MODEM_STATE_DIALING &&
        modem->state != VM_MODEM_STATE_RINGING) {
        return 0;
    }

    vm_modem_enter_connected_data(modem, now_ms);
    vm_modem_send_result(modem, VM_RC_CONNECT);
    return 1;
}

int vm_modem_on_connect_fail(VM_MODEM_CORE *modem,
                             unsigned long session_id,
                             unsigned long reason)
{
    if (modem == 0 ||
        session_id == 0 ||
        session_id != modem->current_session_id) {
        return 0;
    }

    if (modem->state != VM_MODEM_STATE_DIALING &&
        modem->state != VM_MODEM_STATE_RINGING) {
        return 0;
    }

    vm_modem_terminate_session(modem,
                               0,
                               vm_modem_connect_fail_result(reason));
    return 1;
}

unsigned short vm_modem_on_serial_from_helper(VM_MODEM_CORE *modem,
                                              unsigned long session_id,
                                              const unsigned char *bytes,
                                              unsigned short count)
{
    unsigned short before;

    if (modem == 0 || bytes == 0 || count == 0) {
        return 0;
    }

    if (session_id == 0 || session_id != modem->current_session_id) {
        return 0;
    }

    if (modem->state != VM_MODEM_STATE_CONNECTED_DATA &&
        modem->state != VM_MODEM_STATE_CONNECTED_CMD) {
        return 0;
    }

    before = modem->output_count;
    vm_modem_queue_bytes(modem, bytes, count);
    return (unsigned short)(modem->output_count - before);
}

int vm_modem_on_inbound_ring(VM_MODEM_CORE *modem,
                             unsigned long session_id)
{
    if (modem == 0 || session_id == 0 || !modem->helper_available) {
        return 0;
    }

    if (modem->current_session_id != 0 &&
        modem->current_session_id != session_id) {
        return 0;
    }

    if (modem->current_session_id == 0) {
        modem->current_session_id = session_id;
        if (session_id >= modem->next_session_id) {
            modem->next_session_id = session_id + 1;
        }
    }

    if (modem->state == VM_MODEM_STATE_CONNECTED_DATA ||
        modem->state == VM_MODEM_STATE_CONNECTED_CMD ||
        modem->state == VM_MODEM_STATE_DIALING) {
        return 0;
    }

    if (modem->state == VM_MODEM_STATE_RINGING) {
        return 1;
    }

    vm_modem_start_ringing(modem, modem->last_clock_ms);
    return 1;
}

int vm_modem_on_remote_closed(VM_MODEM_CORE *modem,
                              unsigned long session_id)
{
    if (modem == 0 ||
        session_id == 0 ||
        session_id != modem->current_session_id) {
        return 0;
    }

    if (modem->state != VM_MODEM_STATE_CONNECTED_DATA &&
        modem->state != VM_MODEM_STATE_CONNECTED_CMD &&
        modem->state != VM_MODEM_STATE_DIALING &&
        modem->state != VM_MODEM_STATE_RINGING) {
        return 0;
    }

    vm_modem_terminate_session(modem, 0, VM_RC_NO_CARRIER);
    return 1;
}

int vm_modem_peek_action(const VM_MODEM_CORE *modem,
                         VM_MODEM_ACTION *action)
{
    if (modem == 0 || action == 0 || modem->action_count == 0) {
        return 0;
    }

    *action = modem->action_queue[modem->action_get];
    return 1;
}

void vm_modem_pop_action(VM_MODEM_CORE *modem)
{
    VM_MODEM_ACTION *action;

    if (modem == 0 || modem->action_count == 0) {
        return;
    }

    action = &modem->action_queue[modem->action_get];
    action->type = VM_MODEM_ACTION_NONE;
    action->session_id = 0;
    action->status = 0;
    action->payload_length = 0;

    ++modem->action_get;
    if (modem->action_get >= VM_MODEM_ACTION_QUEUE_LEN) {
        modem->action_get = 0;
    }
    --modem->action_count;
}

unsigned short vm_modem_drain_output(VM_MODEM_CORE *modem,
                                     unsigned char *buffer,
                                     unsigned short capacity)
{
    unsigned short copied;

    if (modem == 0 || buffer == 0 || capacity == 0) {
        return 0;
    }

    copied = 0;
    while (copied < capacity && modem->output_count != 0) {
        buffer[copied] = modem->output_buffer[modem->output_get];
        ++copied;
        --modem->output_count;
        ++modem->output_get;
        if (modem->output_get >= VM_MODEM_OUTPUT_BUFFER_LEN) {
            modem->output_get = 0;
        }
    }

    return copied;
}

void vm_modem_clear_output(VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return;
    }

    modem->output_get = 0;
    modem->output_put = 0;
    modem->output_count = 0;
}

unsigned short vm_modem_output_count(const VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return 0;
    }
    return modem->output_count;
}

VM_MODEM_STATE vm_modem_get_state(const VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return VM_MODEM_STATE_IDLE_CMD;
    }
    return modem->state;
}

unsigned long vm_modem_get_status(const VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return 0;
    }
    return modem->modem_status;
}

int vm_modem_get_echo_enabled(const VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return 1;
    }
    return modem->echo_enabled;
}

unsigned long vm_modem_get_active_session_id(const VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return 0;
    }
    return modem->current_session_id;
}

int vm_modem_get_helper_available(const VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return 0;
    }
    return modem->helper_available;
}

int vm_modem_get_raw_mode_enabled(const VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return 1;
    }
    return modem->raw_mode_enabled;
}

unsigned long vm_modem_get_s0_auto_answer_rings(const VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return 0UL;
    }
    return modem->s0_auto_answer_rings;
}

unsigned long vm_modem_get_s1_ring_count(const VM_MODEM_CORE *modem)
{
    if (modem == 0) {
        return 0UL;
    }
    return modem->s1_ring_count;
}

int vm_modem_get_next_timer_deadline(const VM_MODEM_CORE *modem,
                                     unsigned long *deadline_ms)
{
    unsigned long deadline;

    if (modem == 0 || deadline_ms == 0) {
        return 0;
    }

    if (modem->state != VM_MODEM_STATE_RINGING) {
        return 0;
    }

    deadline = 0;
    if (modem->ring_timeout_deadline_ms != 0) {
        deadline = modem->ring_timeout_deadline_ms;
    }
    if (modem->ring_signal_deadline_ms != 0 &&
        (deadline == 0 ||
         vm_modem_time_reached(deadline, modem->ring_signal_deadline_ms))) {
        deadline = modem->ring_signal_deadline_ms;
    }
    if (modem->ring_next_time_ms != 0 &&
        (deadline == 0 ||
         vm_modem_time_reached(deadline, modem->ring_next_time_ms))) {
        deadline = modem->ring_next_time_ms;
    }

    if (deadline == 0) {
        return 0;
    }

    *deadline_ms = deadline;
    return 1;
}
