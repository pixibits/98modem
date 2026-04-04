#include "telnet_proto.h"

static void helper_telnet_clear_events(HELPER_TELNET_SESSION *session);
static void helper_telnet_record_event(HELPER_TELNET_SESSION *session,
                                       HELPER_TELNET_EVENT_TYPE type,
                                       unsigned char command,
                                       unsigned char option,
                                       unsigned char reply_command);
static int helper_telnet_append_payload(unsigned char *payload_out,
                                        unsigned short payload_capacity,
                                        unsigned short *payload_length,
                                        unsigned char byte_value);
static int helper_telnet_queue_control(HELPER_TELNET_SESSION *session,
                                       unsigned char command,
                                       unsigned char option);
static int helper_telnet_is_valid_mode_command(unsigned char command);
static int helper_telnet_is_remote_supported(unsigned char option);
static int helper_telnet_is_local_supported(unsigned char option);
static unsigned char *helper_telnet_option_table(HELPER_TELNET_SESSION *session,
                                                 unsigned char command);
static void helper_telnet_handle_negotiation(HELPER_TELNET_SESSION *session,
                                             unsigned char command,
                                             unsigned char option);

static void helper_telnet_clear_events(HELPER_TELNET_SESSION *session)
{
    unsigned short i;

    if (session == 0) {
        return;
    }

    session->event_count = 0;
    for (i = 0; i < HELPER_TELNET_EVENT_CAP; ++i) {
        session->events[i].type = HELPER_TELNET_EVENT_NONE;
        session->events[i].command = 0;
        session->events[i].option = 0;
        session->events[i].reply_command = 0;
    }
}

static void helper_telnet_record_event(HELPER_TELNET_SESSION *session,
                                       HELPER_TELNET_EVENT_TYPE type,
                                       unsigned char command,
                                       unsigned char option,
                                       unsigned char reply_command)
{
    HELPER_TELNET_EVENT *event;

    if (session == 0 || session->event_count >= HELPER_TELNET_EVENT_CAP) {
        return;
    }

    event = &session->events[session->event_count];
    event->type = type;
    event->command = command;
    event->option = option;
    event->reply_command = reply_command;
    ++session->event_count;
}

static int helper_telnet_append_payload(unsigned char *payload_out,
                                        unsigned short payload_capacity,
                                        unsigned short *payload_length,
                                        unsigned char byte_value)
{
    if (payload_out == 0 || payload_length == 0) {
        return 0;
    }

    if (*payload_length >= payload_capacity) {
        return 0;
    }

    payload_out[*payload_length] = byte_value;
    *payload_length = (unsigned short)(*payload_length + 1);
    return 1;
}

static int helper_telnet_queue_control(HELPER_TELNET_SESSION *session,
                                       unsigned char command,
                                       unsigned char option)
{
    unsigned short base;

    if (session == 0 ||
        session->control_length > HELPER_TELNET_CONTROL_QUEUE_CAP ||
        session->control_length + 3U > HELPER_TELNET_CONTROL_QUEUE_CAP) {
        return 0;
    }

    base = session->control_length;
    session->control_queue[base] = (unsigned char)HELPER_TELNET_IAC;
    session->control_queue[base + 1U] = command;
    session->control_queue[base + 2U] = option;
    session->control_length = (unsigned short)(session->control_length + 3U);
    return 1;
}

static int helper_telnet_is_valid_mode_command(unsigned char command)
{
    return (command == HELPER_TELNET_WILL ||
            command == HELPER_TELNET_WONT ||
            command == HELPER_TELNET_DO ||
            command == HELPER_TELNET_DONT ||
            command == HELPER_TELNET_SB) ? 1 : 0;
}

static int helper_telnet_is_remote_supported(unsigned char option)
{
    return (option == HELPER_TELNET_OPT_SGA ||
            option == HELPER_TELNET_OPT_ECHO) ? 1 : 0;
}

static int helper_telnet_is_local_supported(unsigned char option)
{
    return (option == HELPER_TELNET_OPT_SGA) ? 1 : 0;
}

static unsigned char *helper_telnet_option_table(HELPER_TELNET_SESSION *session,
                                                 unsigned char command)
{
    if (session == 0) {
        return 0;
    }

    if (command == HELPER_TELNET_WILL || command == HELPER_TELNET_WONT) {
        return session->remote_option_state;
    }

    return session->local_option_state;
}

static void helper_telnet_handle_negotiation(HELPER_TELNET_SESSION *session,
                                             unsigned char command,
                                             unsigned char option)
{
    unsigned char *table;
    unsigned char current;
    int supported;
    unsigned char reply_command;

    if (session == 0) {
        return;
    }

    table = helper_telnet_option_table(session, command);
    if (table == 0) {
        return;
    }

    current = table[option];
    supported = ((command == HELPER_TELNET_WILL || command == HELPER_TELNET_WONT) ?
                    helper_telnet_is_remote_supported(option) :
                    helper_telnet_is_local_supported(option));

    reply_command = 0;

    switch (command) {
    case HELPER_TELNET_WILL:
        if (supported) {
            if (current != HELPER_TELNET_OPTION_ENABLED) {
                reply_command = HELPER_TELNET_DO;
                table[option] = HELPER_TELNET_OPTION_ENABLED;
                helper_telnet_queue_control(session, reply_command, option);
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_NEGOTIATION_ACCEPTED,
                                           command,
                                           option,
                                           reply_command);
            } else {
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_REPLY_SUPPRESSED,
                                           command,
                                           option,
                                           0);
            }
        } else {
            if (current != HELPER_TELNET_OPTION_REJECTED) {
                reply_command = HELPER_TELNET_DONT;
                table[option] = HELPER_TELNET_OPTION_REJECTED;
                helper_telnet_queue_control(session, reply_command, option);
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_NEGOTIATION_REFUSED,
                                           command,
                                           option,
                                           reply_command);
            } else {
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_REPLY_SUPPRESSED,
                                           command,
                                           option,
                                           0);
            }
        }
        return;

    case HELPER_TELNET_WONT:
        if (supported) {
            if (current == HELPER_TELNET_OPTION_ENABLED) {
                reply_command = HELPER_TELNET_DONT;
                helper_telnet_queue_control(session, reply_command, option);
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_NEGOTIATION_ACCEPTED,
                                           command,
                                           option,
                                           reply_command);
            } else {
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_REPLY_SUPPRESSED,
                                           command,
                                           option,
                                           0);
            }
            table[option] = HELPER_TELNET_OPTION_DISABLED;
        } else {
            table[option] = HELPER_TELNET_OPTION_REJECTED;
            helper_telnet_record_event(session,
                                       HELPER_TELNET_EVENT_REPLY_SUPPRESSED,
                                       command,
                                       option,
                                       0);
        }
        return;

    case HELPER_TELNET_DO:
        if (supported) {
            if (current != HELPER_TELNET_OPTION_ENABLED) {
                reply_command = HELPER_TELNET_WILL;
                table[option] = HELPER_TELNET_OPTION_ENABLED;
                helper_telnet_queue_control(session, reply_command, option);
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_NEGOTIATION_ACCEPTED,
                                           command,
                                           option,
                                           reply_command);
            } else {
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_REPLY_SUPPRESSED,
                                           command,
                                           option,
                                           0);
            }
        } else {
            if (current != HELPER_TELNET_OPTION_REJECTED) {
                reply_command = HELPER_TELNET_WONT;
                table[option] = HELPER_TELNET_OPTION_REJECTED;
                helper_telnet_queue_control(session, reply_command, option);
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_NEGOTIATION_REFUSED,
                                           command,
                                           option,
                                           reply_command);
            } else {
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_REPLY_SUPPRESSED,
                                           command,
                                           option,
                                           0);
            }
        }
        return;

    case HELPER_TELNET_DONT:
        if (supported) {
            if (current == HELPER_TELNET_OPTION_ENABLED) {
                reply_command = HELPER_TELNET_WONT;
                helper_telnet_queue_control(session, reply_command, option);
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_NEGOTIATION_ACCEPTED,
                                           command,
                                           option,
                                           reply_command);
            } else {
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_REPLY_SUPPRESSED,
                                           command,
                                           option,
                                           0);
            }
            table[option] = HELPER_TELNET_OPTION_DISABLED;
        } else {
            table[option] = HELPER_TELNET_OPTION_REJECTED;
            helper_telnet_record_event(session,
                                       HELPER_TELNET_EVENT_REPLY_SUPPRESSED,
                                       command,
                                       option,
                                       0);
        }
        return;
    }
}

void helper_telnet_init(HELPER_TELNET_SESSION *session)
{
    unsigned short i;

    if (session == 0) {
        return;
    }

    session->telnet_mode = 0;
    session->parse_state = HELPER_TELNET_PARSE_RAW_DATA;
    session->pending_command = 0;
    session->control_length = 0;
    helper_telnet_clear_events(session);

    for (i = 0; i < 256U; ++i) {
        session->remote_option_state[i] = HELPER_TELNET_OPTION_UNKNOWN;
        session->local_option_state[i] = HELPER_TELNET_OPTION_UNKNOWN;
    }
}

unsigned short helper_telnet_filter_inbound(HELPER_TELNET_SESSION *session,
                                            const unsigned char *input,
                                            unsigned short input_length,
                                            unsigned char *payload_out,
                                            unsigned short payload_capacity)
{
    unsigned short i;
    unsigned short payload_length;
    unsigned char byte_value;

    if (session == 0 || input == 0 || payload_out == 0) {
        return 0;
    }

    helper_telnet_clear_events(session);
    payload_length = 0;

    for (i = 0; i < input_length; ++i) {
        byte_value = input[i];

        switch (session->parse_state) {
        case HELPER_TELNET_PARSE_RAW_DATA:
            if (byte_value == HELPER_TELNET_IAC) {
                session->parse_state = HELPER_TELNET_PARSE_RAW_IAC;
            } else {
                helper_telnet_append_payload(payload_out,
                                             payload_capacity,
                                             &payload_length,
                                             byte_value);
            }
            break;

        case HELPER_TELNET_PARSE_RAW_IAC:
            if (helper_telnet_is_valid_mode_command(byte_value)) {
                session->telnet_mode = 1;
                helper_telnet_record_event(session,
                                           HELPER_TELNET_EVENT_MODE_DETECTED,
                                           byte_value,
                                           0,
                                           0);
                if (byte_value == HELPER_TELNET_SB) {
                    session->parse_state = HELPER_TELNET_PARSE_SUBNEG_OPTION;
                } else {
                    session->pending_command = byte_value;
                    session->parse_state =
                        HELPER_TELNET_PARSE_NEGOTIATION_OPTION;
                }
            } else {
                helper_telnet_append_payload(payload_out,
                                             payload_capacity,
                                             &payload_length,
                                             (unsigned char)HELPER_TELNET_IAC);
                helper_telnet_append_payload(payload_out,
                                             payload_capacity,
                                             &payload_length,
                                             byte_value);
                session->parse_state = HELPER_TELNET_PARSE_RAW_DATA;
            }
            break;

        case HELPER_TELNET_PARSE_DATA:
            if (byte_value == HELPER_TELNET_IAC) {
                session->parse_state = HELPER_TELNET_PARSE_IAC;
            } else {
                helper_telnet_append_payload(payload_out,
                                             payload_capacity,
                                             &payload_length,
                                             byte_value);
            }
            break;

        case HELPER_TELNET_PARSE_IAC:
            if (byte_value == HELPER_TELNET_IAC) {
                helper_telnet_append_payload(payload_out,
                                             payload_capacity,
                                             &payload_length,
                                             byte_value);
                session->parse_state = HELPER_TELNET_PARSE_DATA;
            } else if (byte_value == HELPER_TELNET_WILL ||
                       byte_value == HELPER_TELNET_WONT ||
                       byte_value == HELPER_TELNET_DO ||
                       byte_value == HELPER_TELNET_DONT) {
                session->pending_command = byte_value;
                session->parse_state = HELPER_TELNET_PARSE_NEGOTIATION_OPTION;
            } else if (byte_value == HELPER_TELNET_SB) {
                session->parse_state = HELPER_TELNET_PARSE_SUBNEG_OPTION;
            } else {
                session->parse_state = HELPER_TELNET_PARSE_DATA;
            }
            break;

        case HELPER_TELNET_PARSE_NEGOTIATION_OPTION:
            helper_telnet_handle_negotiation(session,
                                             session->pending_command,
                                             byte_value);
            session->pending_command = 0;
            session->parse_state = HELPER_TELNET_PARSE_DATA;
            break;

        case HELPER_TELNET_PARSE_SUBNEG_OPTION:
            helper_telnet_record_event(session,
                                       HELPER_TELNET_EVENT_SUBNEG_IGNORED,
                                       HELPER_TELNET_SB,
                                       byte_value,
                                       0);
            session->parse_state = HELPER_TELNET_PARSE_SUBNEG_DATA;
            break;

        case HELPER_TELNET_PARSE_SUBNEG_DATA:
            if (byte_value == HELPER_TELNET_IAC) {
                session->parse_state = HELPER_TELNET_PARSE_SUBNEG_IAC;
            }
            break;

        case HELPER_TELNET_PARSE_SUBNEG_IAC:
            if (byte_value == HELPER_TELNET_SE) {
                session->parse_state = HELPER_TELNET_PARSE_DATA;
            } else {
                session->parse_state = HELPER_TELNET_PARSE_SUBNEG_DATA;
            }
            break;
        }
    }

    return payload_length;
}

unsigned short helper_telnet_encode_outbound(const HELPER_TELNET_SESSION *session,
                                             const unsigned char *input,
                                             unsigned short input_length,
                                             unsigned char *output,
                                             unsigned short output_capacity)
{
    unsigned short i;
    unsigned short output_length;

    if (session == 0 || input == 0 || output == 0) {
        return 0;
    }

    output_length = 0;
    for (i = 0; i < input_length; ++i) {
        if (!session->telnet_mode || input[i] != HELPER_TELNET_IAC) {
            if (!helper_telnet_append_payload(output,
                                              output_capacity,
                                              &output_length,
                                              input[i])) {
                return 0;
            }
            continue;
        }

        if (!helper_telnet_append_payload(output,
                                          output_capacity,
                                          &output_length,
                                          input[i])) {
            return 0;
        }
        if (!helper_telnet_append_payload(output,
                                          output_capacity,
                                          &output_length,
                                          input[i])) {
            return 0;
        }
    }

    return output_length;
}

const unsigned char *helper_telnet_control_data(
    const HELPER_TELNET_SESSION *session)
{
    if (session == 0 || session->control_length == 0) {
        return 0;
    }

    return session->control_queue;
}

unsigned short helper_telnet_control_length(
    const HELPER_TELNET_SESSION *session)
{
    if (session == 0) {
        return 0;
    }

    return session->control_length;
}

void helper_telnet_consume_control(HELPER_TELNET_SESSION *session,
                                   unsigned short count)
{
    unsigned short i;
    unsigned short remaining;

    if (session == 0 || count == 0 || session->control_length == 0) {
        return;
    }

    if (count >= session->control_length) {
        session->control_length = 0;
        return;
    }

    remaining = (unsigned short)(session->control_length - count);
    for (i = 0; i < remaining; ++i) {
        session->control_queue[i] = session->control_queue[count + i];
    }
    session->control_length = remaining;
}

int helper_telnet_is_telnet_mode(const HELPER_TELNET_SESSION *session)
{
    if (session == 0) {
        return 0;
    }

    return session->telnet_mode;
}

const char *helper_telnet_command_name(unsigned char command)
{
    switch (command) {
    case HELPER_TELNET_DO:
        return "DO";
    case HELPER_TELNET_DONT:
        return "DONT";
    case HELPER_TELNET_WILL:
        return "WILL";
    case HELPER_TELNET_WONT:
        return "WONT";
    case HELPER_TELNET_SB:
        return "SB";
    case HELPER_TELNET_SE:
        return "SE";
    case HELPER_TELNET_IAC:
        return "IAC";
    default:
        return "OTHER";
    }
}

const char *helper_telnet_option_name(unsigned char option)
{
    switch (option) {
    case HELPER_TELNET_OPT_ECHO:
        return "ECHO";
    case HELPER_TELNET_OPT_SGA:
        return "SUPPRESS-GO-AHEAD";
    default:
        return "OTHER";
    }
}
