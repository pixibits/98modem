#ifndef VMODEM_HELPER_TELNET_PROTO_H
#define VMODEM_HELPER_TELNET_PROTO_H

#include "ipc_shared.h"

#define HELPER_TELNET_IAC   255U
#define HELPER_TELNET_DONT  254U
#define HELPER_TELNET_DO    253U
#define HELPER_TELNET_WONT  252U
#define HELPER_TELNET_WILL  251U
#define HELPER_TELNET_SB    250U
#define HELPER_TELNET_SE    240U

#define HELPER_TELNET_OPT_ECHO 1U
#define HELPER_TELNET_OPT_SGA  3U

#define HELPER_TELNET_CONTROL_QUEUE_CAP 512U
#define HELPER_TELNET_EVENT_CAP          32U
#define HELPER_TELNET_MAX_FILTERED_PAYLOAD (VMODEM_IPC_MAX_PAYLOAD + 1U)
#define HELPER_TELNET_MAX_ENCODED_PAYLOAD  (VMODEM_IPC_MAX_PAYLOAD * 2U)

typedef enum HELPER_TELNET_PARSE_STATE {
    HELPER_TELNET_PARSE_RAW_DATA = 0,
    HELPER_TELNET_PARSE_RAW_IAC = 1,
    HELPER_TELNET_PARSE_DATA = 2,
    HELPER_TELNET_PARSE_IAC = 3,
    HELPER_TELNET_PARSE_NEGOTIATION_OPTION = 4,
    HELPER_TELNET_PARSE_SUBNEG_OPTION = 5,
    HELPER_TELNET_PARSE_SUBNEG_DATA = 6,
    HELPER_TELNET_PARSE_SUBNEG_IAC = 7
} HELPER_TELNET_PARSE_STATE;

typedef enum HELPER_TELNET_OPTION_STATE {
    HELPER_TELNET_OPTION_UNKNOWN = 0,
    HELPER_TELNET_OPTION_ENABLED = 1,
    HELPER_TELNET_OPTION_DISABLED = 2,
    HELPER_TELNET_OPTION_REJECTED = 3
} HELPER_TELNET_OPTION_STATE;

typedef enum HELPER_TELNET_EVENT_TYPE {
    HELPER_TELNET_EVENT_NONE = 0,
    HELPER_TELNET_EVENT_MODE_DETECTED = 1,
    HELPER_TELNET_EVENT_NEGOTIATION_ACCEPTED = 2,
    HELPER_TELNET_EVENT_NEGOTIATION_REFUSED = 3,
    HELPER_TELNET_EVENT_SUBNEG_IGNORED = 4,
    HELPER_TELNET_EVENT_REPLY_SUPPRESSED = 5
} HELPER_TELNET_EVENT_TYPE;

typedef struct HELPER_TELNET_EVENT {
    HELPER_TELNET_EVENT_TYPE type;
    unsigned char            command;
    unsigned char            option;
    unsigned char            reply_command;
} HELPER_TELNET_EVENT;

typedef struct HELPER_TELNET_SESSION {
    int                       telnet_mode;
    HELPER_TELNET_PARSE_STATE parse_state;
    unsigned char             pending_command;
    unsigned char             remote_option_state[256];
    unsigned char             local_option_state[256];
    unsigned char             control_queue[HELPER_TELNET_CONTROL_QUEUE_CAP];
    unsigned short            control_length;
    HELPER_TELNET_EVENT       events[HELPER_TELNET_EVENT_CAP];
    unsigned short            event_count;
} HELPER_TELNET_SESSION;

void helper_telnet_init(HELPER_TELNET_SESSION *session);
unsigned short helper_telnet_filter_inbound(HELPER_TELNET_SESSION *session,
                                            const unsigned char *input,
                                            unsigned short input_length,
                                            unsigned char *payload_out,
                                            unsigned short payload_capacity);
unsigned short helper_telnet_encode_outbound(const HELPER_TELNET_SESSION *session,
                                             const unsigned char *input,
                                             unsigned short input_length,
                                             unsigned char *output,
                                             unsigned short output_capacity);
const unsigned char *helper_telnet_control_data(
    const HELPER_TELNET_SESSION *session);
unsigned short helper_telnet_control_length(
    const HELPER_TELNET_SESSION *session);
void helper_telnet_consume_control(HELPER_TELNET_SESSION *session,
                                   unsigned short count);
int helper_telnet_is_telnet_mode(const HELPER_TELNET_SESSION *session);
const char *helper_telnet_command_name(unsigned char command);
const char *helper_telnet_option_name(unsigned char option);

#endif /* VMODEM_HELPER_TELNET_PROTO_H */
