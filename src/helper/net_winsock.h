#ifndef VMODEM_HELPER_NET_WINSOCK_H
#define VMODEM_HELPER_NET_WINSOCK_H

#include <windows.h>
#include <winsock.h>

#include "ipc_shared.h"
#include "net_common.h"
#include "session_log.h"
#include "telnet_proto.h"
#include "vmodem_peer.h"

#define HELPER_NET_PROTOCOL_FATAL 0
#define HELPER_NET_PROTOCOL_OK    1
#define HELPER_NET_PROTOCOL_DEFER 2

#define HELPER_NET_NEGOTIATION_WINDOW_MS 250UL
#define HELPER_NET_PREANSWER_BUFFER_CAP  2048U
#define HELPER_NET_PEER_CONTROL_CAP      32U

typedef enum HELPER_NET_STATE {
    HELPER_NET_STATE_IDLE = 0,
    HELPER_NET_STATE_RESOLVING = 1,
    HELPER_NET_STATE_CONNECTING = 2,
    HELPER_NET_STATE_WAIT_HELLO_ACK = 3,
    HELPER_NET_STATE_WAIT_ANSWERED = 4,
    HELPER_NET_STATE_CONNECTED = 5
} HELPER_NET_STATE;

typedef enum HELPER_NET_PENDING_MODE {
    HELPER_NET_PENDING_MODE_NONE = 0,
    HELPER_NET_PENDING_MODE_PROBE = 1,
    HELPER_NET_PENDING_MODE_RAW = 2,
    HELPER_NET_PENDING_MODE_DELAYED = 3
} HELPER_NET_PENDING_MODE;

typedef struct HELPER_NET_RUNTIME {
    HANDLE            hDevice;
    unsigned long     helper_generation;
    unsigned long     active_session_id;
    unsigned long     pending_session_id;
    unsigned long     next_inbound_session_id;
    HELPER_NET_STATE  state;
    HWND              hwnd;
    int               winsock_started;
    SOCKET            socket_handle;
    SOCKET            listen_socket;
    SOCKET            pending_socket;
    HANDLE            dns_request;
    DWORD             deadline_tick;
    DWORD             peer_deadline_tick;
    DWORD             pending_started_tick;
    DWORD             pending_deadline_tick;
    unsigned long     connect_flags;
    HELPER_NET_TARGET target;
    HELPER_NET_LISTENER_CONFIG listener_config;
    char              dns_buffer[MAXGETHOSTSTRUCT];
    HELPER_TELNET_SESSION telnet;
    HELPER_NET_PENDING_MODE pending_mode;
    unsigned char     peer_control[HELPER_NET_PEER_CONTROL_CAP];
    unsigned short    peer_control_length;
    unsigned char     peer_preanswer[HELPER_NET_PREANSWER_BUFFER_CAP];
    unsigned short    peer_preanswer_length;
    unsigned char     pending_preanswer[HELPER_NET_PREANSWER_BUFFER_CAP];
    unsigned short    pending_preanswer_length;
    unsigned char     pending_send[HELPER_NET_PENDING_SEND_CAP];
    unsigned short    pending_send_length;
    HELPER_SESSION_LOG *session_log;
    DWORD             fatal_error;
    int               fatal;
} HELPER_NET_RUNTIME;

int helper_net_runtime_init(HELPER_NET_RUNTIME *net,
                            HINSTANCE hInstance,
                            HANDLE hDevice,
                            unsigned long helper_generation,
                            const HELPER_NET_LISTENER_CONFIG *listener_config);
void helper_net_runtime_shutdown(HELPER_NET_RUNTIME *net);
void helper_net_runtime_set_protocol(HELPER_NET_RUNTIME *net,
                                     HANDLE hDevice,
                                     unsigned long helper_generation);
void helper_net_runtime_set_session_log(HELPER_NET_RUNTIME *net,
                                        HELPER_SESSION_LOG *session_log);
int helper_net_runtime_apply_listener_config(
    HELPER_NET_RUNTIME *net,
    const HELPER_NET_LISTENER_CONFIG *listener_config);
int helper_net_runtime_submit_connect_fail(HELPER_NET_RUNTIME *net,
                                           unsigned long session_id,
                                           unsigned long reason);
unsigned int helper_net_runtime_pump_messages(HELPER_NET_RUNTIME *net);
void helper_net_runtime_check_timeout(HELPER_NET_RUNTIME *net);
int helper_net_runtime_handle_protocol(HELPER_NET_RUNTIME *net,
                                       const VMODEM_PROTOCOL_MESSAGE *message);
int helper_net_runtime_has_fatal_error(const HELPER_NET_RUNTIME *net);
DWORD helper_net_runtime_get_fatal_error(const HELPER_NET_RUNTIME *net);

#endif /* VMODEM_HELPER_NET_WINSOCK_H */
