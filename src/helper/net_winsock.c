#include <stdarg.h>

#include "ipc_helper.h"
#include "net_winsock.h"

#define HELPER_NET_WINDOW_CLASS "VMODEMHelperNetWindow"
#define HELPER_NET_WM_SOCKET    (WM_USER + 0x61)
#define HELPER_NET_WM_DNS       (WM_USER + 0x62)
#define HELPER_NET_WM_LISTEN    (WM_USER + 0x63)
#define HELPER_NET_WM_PENDING   (WM_USER + 0x64)

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x0001
#endif

static HELPER_NET_RUNTIME *g_helper_net_runtime = NULL;
static int g_helper_net_class_registered = 0;

static void helper_net_mark_fatal(HELPER_NET_RUNTIME *net, DWORD detail);
static void helper_net_tracef(const char *format, ...);
static void helper_net_log_event(HELPER_NET_RUNTIME *net,
                                 const char *event_name,
                                 unsigned long session_id,
                                 const char *detail);
static void helper_net_log_bytes(HELPER_NET_RUNTIME *net,
                                 const char *event_name,
                                 unsigned long session_id,
                                 const unsigned char *bytes,
                                 unsigned short length);
static void helper_net_log_protocol(HELPER_NET_RUNTIME *net,
                                    const char *direction,
                                    DWORD messageType,
                                    DWORD sessionId,
                                    DWORD status,
                                    const unsigned char *payload,
                                    DWORD payloadLength);
static int helper_net_submit(HANDLE hDevice,
                             DWORD messageType,
                             DWORD helperGeneration,
                             DWORD sessionId,
                             DWORD status,
                             const unsigned char *payload,
                             DWORD payloadLength,
                             DWORD *pAckStatus);
static void helper_net_close_socket(HELPER_NET_RUNTIME *net);
static void helper_net_close_listen_socket(HELPER_NET_RUNTIME *net);
static void helper_net_close_pending_socket(HELPER_NET_RUNTIME *net);
static void helper_net_cancel_dns(HELPER_NET_RUNTIME *net);
static void helper_net_reset_pending_inbound(HELPER_NET_RUNTIME *net);
static void helper_net_reset_session(HELPER_NET_RUNTIME *net);
static void helper_net_clear_peer_control(HELPER_NET_RUNTIME *net);
static void helper_net_try_enable_nodelay(SOCKET socketHandle);
static int helper_net_is_ipv4_literal(const char *host, unsigned long *pAddress);
static int helper_net_append_bytes(unsigned char *buffer,
                                   unsigned short *length,
                                   unsigned short capacity,
                                   const unsigned char *payload,
                                   unsigned short payloadLength);
static void helper_net_consume_bytes(unsigned char *buffer,
                                     unsigned short *length,
                                     unsigned short consumed);
static int helper_net_append_pending(HELPER_NET_RUNTIME *net,
                                     const unsigned char *payload,
                                     unsigned short payloadLength);
static SOCKET helper_net_get_control_socket(const HELPER_NET_RUNTIME *net);
static unsigned long helper_net_get_control_session_id(
    const HELPER_NET_RUNTIME *net);
static int helper_net_send_socket_bytes(HELPER_NET_RUNTIME *net,
                                        SOCKET socketHandle,
                                        unsigned long sessionId,
                                        const unsigned char *buffer,
                                        unsigned short length,
                                        unsigned short *pConsumed,
                                        int *pClosed);
static int helper_net_flush_peer_control(HELPER_NET_RUNTIME *net);
static int helper_net_queue_peer_frame(HELPER_NET_RUNTIME *net,
                                       unsigned char type,
                                       unsigned char flags);
static int helper_net_process_connected_inbound(HELPER_NET_RUNTIME *net,
                                                const unsigned char *payload,
                                                unsigned short payloadLength);
static int helper_net_deliver_preanswer_buffer(HELPER_NET_RUNTIME *net,
                                               unsigned char *payload,
                                               unsigned short *payloadLength);
static int helper_net_finish_connected(HELPER_NET_RUNTIME *net);
static int helper_net_fallback_to_raw(HELPER_NET_RUNTIME *net,
                                      const char *reason);
static int helper_net_begin_delayed_connect(HELPER_NET_RUNTIME *net);
static int helper_net_begin_pending_ring(HELPER_NET_RUNTIME *net,
                                         HELPER_NET_PENDING_MODE mode,
                                         const char *reason);
static void helper_net_close_active_as_remote_closed(
    HELPER_NET_RUNTIME *net,
    unsigned long sessionId);
static void helper_net_close_pending_as_remote_closed(
    HELPER_NET_RUNTIME *net,
    unsigned long sessionId);
static int helper_net_handle_wait_hello_ack_bytes(HELPER_NET_RUNTIME *net,
                                                  const unsigned char *payload,
                                                  unsigned short payloadLength);
static int helper_net_handle_wait_answered_bytes(HELPER_NET_RUNTIME *net,
                                                 const unsigned char *payload,
                                                 unsigned short payloadLength);
static int helper_net_handle_pending_probe_bytes(HELPER_NET_RUNTIME *net,
                                                 const unsigned char *payload,
                                                 unsigned short payloadLength);
static int helper_net_handle_pending_delayed_bytes(
    HELPER_NET_RUNTIME *net,
    const unsigned char *payload,
    unsigned short payloadLength);
static int helper_net_submit_connect_ok(HELPER_NET_RUNTIME *net);
static int helper_net_submit_connect_fail(HELPER_NET_RUNTIME *net,
                                          unsigned long sessionId,
                                          unsigned long reason);
static int helper_net_submit_inbound_ring(HELPER_NET_RUNTIME *net,
                                          unsigned long sessionId);
static int helper_net_submit_remote_closed(HELPER_NET_RUNTIME *net,
                                           unsigned long sessionId);
static int helper_net_submit_data_to_serial(HELPER_NET_RUNTIME *net,
                                            const unsigned char *payload,
                                            unsigned short payloadLength);
static void helper_net_trace_telnet_events(HELPER_NET_RUNTIME *net);
static int helper_net_send_buffer(HELPER_NET_RUNTIME *net,
                                  const unsigned char *buffer,
                                  unsigned short length,
                                  unsigned short *pConsumed);
static int helper_net_flush_pending_send(HELPER_NET_RUNTIME *net);
static int helper_net_prepare_active_socket(HELPER_NET_RUNTIME *net,
                                            SOCKET socketHandle);
static int helper_net_start_listener(HELPER_NET_RUNTIME *net);
static int helper_net_begin_socket_connect(HELPER_NET_RUNTIME *net,
                                           const struct sockaddr_in *address);
static int helper_net_begin_connect(HELPER_NET_RUNTIME *net,
                                    const VMODEM_PROTOCOL_MESSAGE *message);
static void helper_net_handle_dns_message(HELPER_NET_RUNTIME *net,
                                          WPARAM wParam,
                                          LPARAM lParam);
static void helper_net_handle_listen_message(HELPER_NET_RUNTIME *net,
                                             WPARAM wParam,
                                             LPARAM lParam);
static void helper_net_handle_pending_message(HELPER_NET_RUNTIME *net,
                                              WPARAM wParam,
                                              LPARAM lParam);
static void helper_net_handle_socket_message(HELPER_NET_RUNTIME *net,
                                             WPARAM wParam,
                                             LPARAM lParam);

static void helper_net_mark_fatal(HELPER_NET_RUNTIME *net, DWORD detail)
{
    if (net == NULL) {
        return;
    }

    net->fatal = 1;
    net->fatal_error = (detail != 0) ? detail : ERROR_GEN_FAILURE;
}

static void helper_net_tracef(const char *format, ...)
{
    char buffer[512];
    va_list args;

    va_start(args, format);
    wvsprintfA(buffer, format, args);
    va_end(args);

    OutputDebugStringA(buffer);
}

static void helper_net_log_event(HELPER_NET_RUNTIME *net,
                                 const char *event_name,
                                 unsigned long session_id,
                                 const char *detail)
{
    if (net == NULL || net->session_log == NULL) {
        return;
    }

    helper_session_log_event(net->session_log, event_name, session_id, detail);
}

static void helper_net_log_bytes(HELPER_NET_RUNTIME *net,
                                 const char *event_name,
                                 unsigned long session_id,
                                 const unsigned char *bytes,
                                 unsigned short length)
{
    if (net == NULL || net->session_log == NULL) {
        return;
    }

    helper_session_log_bytes(net->session_log,
                             event_name,
                             session_id,
                             bytes,
                             length);
}

static void helper_net_log_protocol(HELPER_NET_RUNTIME *net,
                                    const char *direction,
                                    DWORD messageType,
                                    DWORD sessionId,
                                    DWORD status,
                                    const unsigned char *payload,
                                    DWORD payloadLength)
{
    VMODEM_PROTOCOL_MESSAGE message;

    if (net == NULL || net->session_log == NULL) {
        return;
    }

    ZeroMemory(&message, sizeof(message));
    message.version = VMODEM_IPC_VERSION;
    message.message_type = messageType;
    message.helper_generation = net->helper_generation;
    message.session_id = sessionId;
    message.status = status;
    message.payload_length = payloadLength;
    if (payload != NULL && payloadLength != 0U) {
        CopyMemory(message.payload, payload, payloadLength);
    }

    helper_session_log_protocol(net->session_log, direction, &message);
}

static int helper_net_submit(HANDLE hDevice,
                             DWORD messageType,
                             DWORD helperGeneration,
                             DWORD sessionId,
                             DWORD status,
                             const unsigned char *payload,
                             DWORD payloadLength,
                             DWORD *pAckStatus)
{
    VMODEM_PROTOCOL_MESSAGE message;
    VMODEM_SUBMIT_MESSAGE_ACK ack;

    if (pAckStatus != NULL) {
        *pAckStatus = VMODEM_STATUS_OK;
    }

    if (payloadLength > VMODEM_IPC_MAX_PAYLOAD) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    ZeroMemory(&message, sizeof(message));
    message.version = VMODEM_IPC_VERSION;
    message.message_type = messageType;
    message.helper_generation = helperGeneration;
    message.session_id = sessionId;
    message.status = status;
    message.payload_length = payloadLength;
    if (payload != NULL && payloadLength != 0) {
        CopyMemory(message.payload, payload, payloadLength);
    }

    ZeroMemory(&ack, sizeof(ack));
    if (!IPC_SubmitMessage(hDevice, &message, &ack)) {
        return 0;
    }

    if (pAckStatus != NULL) {
        *pAckStatus = ack.status;
    }

    if (ack.status != VMODEM_STATUS_OK) {
        SetLastError((DWORD)ack.status);
        return 0;
    }

    return 1;
}

static void helper_net_close_socket(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return;
    }

    if (net->socket_handle != INVALID_SOCKET) {
        closesocket(net->socket_handle);
        net->socket_handle = INVALID_SOCKET;
    }
}

static void helper_net_close_listen_socket(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return;
    }

    if (net->listen_socket != INVALID_SOCKET) {
        closesocket(net->listen_socket);
        net->listen_socket = INVALID_SOCKET;
    }
}

static void helper_net_close_pending_socket(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return;
    }

    if (net->pending_socket != INVALID_SOCKET) {
        closesocket(net->pending_socket);
        net->pending_socket = INVALID_SOCKET;
    }
}

static void helper_net_cancel_dns(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return;
    }

    if (net->dns_request != NULL) {
        WSACancelAsyncRequest(net->dns_request);
        net->dns_request = NULL;
    }
}

static void helper_net_reset_pending_inbound(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return;
    }

    helper_net_close_pending_socket(net);
    net->pending_session_id = 0;
    net->pending_started_tick = 0;
    net->pending_deadline_tick = 0;
    net->pending_mode = HELPER_NET_PENDING_MODE_NONE;
    net->pending_preanswer_length = 0;
    if (net->active_session_id == 0UL) {
        helper_net_clear_peer_control(net);
    }
}

static void helper_net_reset_session(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return;
    }

    helper_net_cancel_dns(net);
    helper_net_close_socket(net);
    net->state = HELPER_NET_STATE_IDLE;
    net->active_session_id = 0;
    net->connect_flags = 0;
    net->deadline_tick = 0;
    net->peer_deadline_tick = 0;
    helper_net_clear_peer_control(net);
    net->peer_preanswer_length = 0;
    net->pending_send_length = 0;
    net->target.host[0] = '\0';
    net->target.port = 0;
    ZeroMemory(net->dns_buffer, sizeof(net->dns_buffer));
    helper_telnet_init(&net->telnet);
}

static void helper_net_clear_peer_control(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return;
    }

    net->peer_control_length = 0;
}

static void helper_net_try_enable_nodelay(SOCKET socketHandle)
{
    int flag;
    int result;
    int error;

    if (socketHandle == INVALID_SOCKET) {
        return;
    }

    flag = 1;
    result = setsockopt(socketHandle,
                        IPPROTO_TCP,
                        TCP_NODELAY,
                        (const char *)&flag,
                        sizeof(flag));
    if (result == SOCKET_ERROR) {
        error = WSAGetLastError();
        helper_net_tracef("VMODEM helper: TCP_NODELAY enable failed error=%d\r\n",
                          error);
        return;
    }

    helper_net_tracef("VMODEM helper: TCP_NODELAY enabled\r\n");
}

static int helper_net_append_bytes(unsigned char *buffer,
                                   unsigned short *length,
                                   unsigned short capacity,
                                   const unsigned char *payload,
                                   unsigned short payloadLength)
{
    if (buffer == NULL ||
        length == NULL ||
        payload == NULL ||
        payloadLength == 0U ||
        *length > capacity ||
        payloadLength > (unsigned short)(capacity - *length)) {
        return 0;
    }

    CopyMemory(buffer + *length, payload, payloadLength);
    *length = (unsigned short)(*length + payloadLength);
    return 1;
}

static void helper_net_consume_bytes(unsigned char *buffer,
                                     unsigned short *length,
                                     unsigned short consumed)
{
    unsigned short remaining;
    unsigned short i;

    if (buffer == NULL || length == NULL || consumed == 0U) {
        return;
    }

    if (consumed >= *length) {
        *length = 0U;
        return;
    }

    remaining = (unsigned short)(*length - consumed);
    for (i = 0U; i < remaining; ++i) {
        buffer[i] = buffer[consumed + i];
    }
    *length = remaining;
}

static int helper_net_is_ipv4_literal(const char *host, unsigned long *pAddress)
{
    unsigned long address;

    if (host == NULL || pAddress == NULL) {
        return 0;
    }

    address = inet_addr(host);
    if (address == INADDR_NONE &&
        lstrcmpA(host, "255.255.255.255") != 0) {
        return 0;
    }

    *pAddress = address;
    return 1;
}

static int helper_net_append_pending(HELPER_NET_RUNTIME *net,
                                     const unsigned char *payload,
                                     unsigned short payloadLength)
{
    if (net == NULL) {
        return 0;
    }

    return helper_net_append_bytes(net->pending_send,
                                   &net->pending_send_length,
                                   HELPER_NET_PENDING_SEND_CAP,
                                   payload,
                                   payloadLength);
}

static SOCKET helper_net_get_control_socket(const HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return INVALID_SOCKET;
    }

    if (net->pending_socket != INVALID_SOCKET &&
        net->pending_session_id != 0UL &&
        net->pending_mode == HELPER_NET_PENDING_MODE_DELAYED &&
        net->active_session_id == 0UL) {
        return net->pending_socket;
    }

    return net->socket_handle;
}

static unsigned long helper_net_get_control_session_id(
    const HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return 0UL;
    }

    if (net->pending_socket != INVALID_SOCKET &&
        net->pending_session_id != 0UL &&
        net->pending_mode == HELPER_NET_PENDING_MODE_DELAYED &&
        net->active_session_id == 0UL) {
        return net->pending_session_id;
    }

    return net->active_session_id;
}

static int helper_net_send_socket_bytes(HELPER_NET_RUNTIME *net,
                                        SOCKET socketHandle,
                                        unsigned long sessionId,
                                        const unsigned char *buffer,
                                        unsigned short length,
                                        unsigned short *pConsumed,
                                        int *pClosed)
{
    int sent;
    int error;

    if (pConsumed != NULL) {
        *pConsumed = 0U;
    }
    if (pClosed != NULL) {
        *pClosed = 0;
    }

    if (net == NULL ||
        socketHandle == INVALID_SOCKET ||
        buffer == NULL ||
        length == 0U) {
        return 0;
    }

    sent = send(socketHandle, (const char *)buffer, length, 0);
    if (sent > 0) {
        if (pConsumed != NULL) {
            *pConsumed = (unsigned short)sent;
        }
        helper_net_log_bytes(net, "SOCKET_TX", sessionId, buffer, (unsigned short)sent);
        return 1;
    }

    if (sent == 0) {
        if (pClosed != NULL) {
            *pClosed = 1;
        }
        return 1;
    }

    error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
        return 1;
    }

    if (pClosed != NULL) {
        *pClosed = 1;
    }
    return 1;
}

static int helper_net_flush_peer_control(HELPER_NET_RUNTIME *net)
{
    SOCKET socketHandle;
    unsigned long sessionId;
    unsigned short consumed;
    unsigned short remaining;
    unsigned short i;
    int closed;

    if (net == NULL) {
        return 0;
    }

    while (net->peer_control_length != 0U) {
        socketHandle = helper_net_get_control_socket(net);
        sessionId = helper_net_get_control_session_id(net);
        if (socketHandle == INVALID_SOCKET || sessionId == 0UL) {
            return 0;
        }

        consumed = 0U;
        closed = 0;
        if (!helper_net_send_socket_bytes(net,
                                          socketHandle,
                                          sessionId,
                                          net->peer_control,
                                          net->peer_control_length,
                                          &consumed,
                                          &closed)) {
            return 0;
        }

        if (closed) {
            if (socketHandle == net->pending_socket &&
                sessionId == net->pending_session_id) {
                helper_net_close_pending_as_remote_closed(net, sessionId);
            } else {
                helper_net_close_active_as_remote_closed(net, sessionId);
            }
            return 1;
        }

        if (consumed == 0U) {
            return 1;
        }

        if (consumed >= net->peer_control_length) {
            net->peer_control_length = 0U;
            continue;
        }

        remaining = (unsigned short)(net->peer_control_length - consumed);
        for (i = 0U; i < remaining; ++i) {
            net->peer_control[i] = net->peer_control[consumed + i];
        }
        net->peer_control_length = remaining;
    }

    return 1;
}

static int helper_net_queue_peer_frame(HELPER_NET_RUNTIME *net,
                                       unsigned char type,
                                       unsigned char flags)
{
    unsigned char frame[HELPER_VMODEM_FRAME_LEN];

    if (net == NULL) {
        return 0;
    }

    helper_vmodem_build_frame(type,
                              flags,
                              frame,
                              (unsigned short)sizeof(frame));
    return helper_net_append_bytes(net->peer_control,
                                   &net->peer_control_length,
                                   HELPER_NET_PEER_CONTROL_CAP,
                                   frame,
                                   (unsigned short)sizeof(frame));
}

static int helper_net_submit_connect_ok(HELPER_NET_RUNTIME *net)
{
    DWORD ackStatus;

    if (net == NULL) {
        return 0;
    }

    helper_net_tracef("VMODEM helper: connect success session=%lu\r\n",
                      net->active_session_id);

    if (!helper_net_submit(net->hDevice,
                           VMODEM_MSG_CONNECT_OK,
                           net->helper_generation,
                           net->active_session_id,
                           VMODEM_STATUS_OK,
                           NULL,
                           0,
                           &ackStatus)) {
        if (ackStatus == VMODEM_STATUS_STALE) {
            helper_net_tracef("VMODEM helper: stale connect ok session=%lu\r\n",
                              net->active_session_id);
            helper_net_reset_session(net);
            return 1;
        }
        helper_net_mark_fatal(net, GetLastError());
        return 0;
    }

    helper_net_log_protocol(net,
                            "IPC_TX",
                            VMODEM_MSG_CONNECT_OK,
                            net->active_session_id,
                            VMODEM_STATUS_OK,
                            NULL,
                            0);
    return 1;
}

static int helper_net_submit_connect_fail(HELPER_NET_RUNTIME *net,
                                          unsigned long sessionId,
                                          unsigned long reason)
{
    DWORD ackStatus;

    if (net == NULL) {
        return 0;
    }

    helper_net_tracef("VMODEM helper: connect failed session=%lu reason=%s\r\n",
                      sessionId,
                      helper_net_fail_reason_name(reason));

    if (!helper_net_submit(net->hDevice,
                           VMODEM_MSG_CONNECT_FAIL,
                           net->helper_generation,
                           sessionId,
                           reason,
                           NULL,
                           0,
                           &ackStatus)) {
        if (ackStatus == VMODEM_STATUS_STALE) {
            helper_net_tracef("VMODEM helper: stale connect fail session=%lu\r\n",
                              sessionId);
            return 1;
        }
        helper_net_mark_fatal(net, GetLastError());
        return 0;
    }

    helper_net_log_protocol(net,
                            "IPC_TX",
                            VMODEM_MSG_CONNECT_FAIL,
                            sessionId,
                            reason,
                            NULL,
                            0);
    return 1;
}

static int helper_net_submit_inbound_ring(HELPER_NET_RUNTIME *net,
                                          unsigned long sessionId)
{
    DWORD ackStatus;

    if (net == NULL) {
        return 0;
    }

    helper_net_tracef("VMODEM helper: inbound ring session=%lu\r\n",
                      sessionId);

    if (!helper_net_submit(net->hDevice,
                           VMODEM_MSG_INBOUND_RING,
                           net->helper_generation,
                           sessionId,
                           VMODEM_STATUS_OK,
                           NULL,
                           0,
                           &ackStatus)) {
        if (ackStatus == VMODEM_STATUS_STALE) {
            helper_net_tracef("VMODEM helper: stale inbound ring session=%lu\r\n",
                              sessionId);
            helper_net_reset_pending_inbound(net);
            return 1;
        }
        helper_net_mark_fatal(net, GetLastError());
        return 0;
    }

    helper_net_log_protocol(net,
                            "IPC_TX",
                            VMODEM_MSG_INBOUND_RING,
                            sessionId,
                            VMODEM_STATUS_OK,
                            NULL,
                            0);
    return 1;
}

static int helper_net_submit_remote_closed(HELPER_NET_RUNTIME *net,
                                           unsigned long sessionId)
{
    DWORD ackStatus;

    if (net == NULL) {
        return 0;
    }

    helper_net_tracef("VMODEM helper: remote close session=%lu\r\n",
                      sessionId);

    if (!helper_net_submit(net->hDevice,
                           VMODEM_MSG_REMOTE_CLOSED,
                           net->helper_generation,
                           sessionId,
                           VMODEM_STATUS_OK,
                           NULL,
                           0,
                           &ackStatus)) {
        if (ackStatus == VMODEM_STATUS_STALE) {
            helper_net_tracef("VMODEM helper: stale remote close session=%lu\r\n",
                              sessionId);
            return 1;
        }
        helper_net_mark_fatal(net, GetLastError());
        return 0;
    }

    helper_net_log_protocol(net,
                            "IPC_TX",
                            VMODEM_MSG_REMOTE_CLOSED,
                            sessionId,
                            VMODEM_STATUS_OK,
                            NULL,
                            0);
    return 1;
}

static int helper_net_submit_data_to_serial(HELPER_NET_RUNTIME *net,
                                            const unsigned char *payload,
                                            unsigned short payloadLength)
{
    unsigned short offset;
    unsigned short chunk;
    DWORD ackStatus;

    if (net == NULL || payload == NULL || payloadLength == 0) {
        return 0;
    }

    offset = 0;
    while (offset < payloadLength) {
        chunk = (unsigned short)(payloadLength - offset);
        if (chunk > VMODEM_IPC_MAX_PAYLOAD) {
            chunk = VMODEM_IPC_MAX_PAYLOAD;
        }

        if (!helper_net_submit(net->hDevice,
                               VMODEM_MSG_DATA_TO_SERIAL,
                               net->helper_generation,
                               net->active_session_id,
                               VMODEM_STATUS_OK,
                               payload + offset,
                               chunk,
                               &ackStatus)) {
            if (ackStatus == VMODEM_STATUS_STALE) {
                helper_net_tracef("VMODEM helper: stale data to serial session=%lu\r\n",
                                  net->active_session_id);
                return 1;
            }
            helper_net_mark_fatal(net, GetLastError());
            return 0;
        }

        helper_net_log_protocol(net,
                                "IPC_TX",
                                VMODEM_MSG_DATA_TO_SERIAL,
                                net->active_session_id,
                                VMODEM_STATUS_OK,
                                payload + offset,
                                chunk);
        helper_net_log_bytes(net,
                             "SERIAL_RX",
                             net->active_session_id,
                             payload + offset,
                             chunk);
        offset = (unsigned short)(offset + chunk);
    }

    return 1;
}

static int helper_net_process_connected_inbound(HELPER_NET_RUNTIME *net,
                                                const unsigned char *payload,
                                                unsigned short payloadLength)
{
    unsigned char filtered[HELPER_TELNET_MAX_FILTERED_PAYLOAD];
    unsigned short filteredLength;

    if (net == NULL || payload == NULL || payloadLength == 0U) {
        return 1;
    }

    filteredLength = helper_telnet_filter_inbound(&net->telnet,
                                                  payload,
                                                  payloadLength,
                                                  filtered,
                                                  sizeof(filtered));
    helper_net_trace_telnet_events(net);
    if (filteredLength != 0U &&
        !helper_net_submit_data_to_serial(net, filtered, filteredLength)) {
        return 0;
    }

    return helper_net_flush_pending_send(net);
}

static int helper_net_deliver_preanswer_buffer(HELPER_NET_RUNTIME *net,
                                               unsigned char *payload,
                                               unsigned short *payloadLength)
{
    unsigned short length;

    if (net == NULL || payload == NULL || payloadLength == NULL) {
        return 0;
    }

    length = *payloadLength;
    if (length == 0U) {
        return 1;
    }

    if (!helper_net_process_connected_inbound(net, payload, length)) {
        return 0;
    }

    *payloadLength = 0U;
    return 1;
}

static void helper_net_trace_telnet_events(HELPER_NET_RUNTIME *net)
{
    unsigned short i;
    HELPER_TELNET_EVENT *event;

    if (net == NULL) {
        return;
    }

    for (i = 0; i < net->telnet.event_count; ++i) {
        event = &net->telnet.events[i];
        switch (event->type) {
        case HELPER_TELNET_EVENT_MODE_DETECTED:
            helper_net_tracef("VMODEM helper: telnet mode detected session=%lu via %s\r\n",
                              net->active_session_id,
                              helper_telnet_command_name(event->command));
            helper_net_log_event(net,
                                 "TELNET_MODE",
                                 net->active_session_id,
                                 helper_telnet_command_name(event->command));
            break;

        case HELPER_TELNET_EVENT_NEGOTIATION_ACCEPTED:
            helper_net_tracef("VMODEM helper: telnet accept session=%lu %s %s(%u) -> %s\r\n",
                              net->active_session_id,
                              helper_telnet_command_name(event->command),
                              helper_telnet_option_name(event->option),
                              (unsigned int)event->option,
                              helper_telnet_command_name(event->reply_command));
        {
            char detail[96];

            wsprintfA(detail,
                      "%s %s(%u) -> %s",
                      helper_telnet_command_name(event->command),
                      helper_telnet_option_name(event->option),
                      (unsigned int)event->option,
                      helper_telnet_command_name(event->reply_command));
            helper_net_log_event(net,
                                 "TELNET_ACCEPT",
                                 net->active_session_id,
                                 detail);
        }
            break;

        case HELPER_TELNET_EVENT_NEGOTIATION_REFUSED:
            helper_net_tracef("VMODEM helper: telnet refuse session=%lu %s %s(%u) -> %s\r\n",
                              net->active_session_id,
                              helper_telnet_command_name(event->command),
                              helper_telnet_option_name(event->option),
                              (unsigned int)event->option,
                              helper_telnet_command_name(event->reply_command));
        {
            char detail[96];

            wsprintfA(detail,
                      "%s %s(%u) -> %s",
                      helper_telnet_command_name(event->command),
                      helper_telnet_option_name(event->option),
                      (unsigned int)event->option,
                      helper_telnet_command_name(event->reply_command));
            helper_net_log_event(net,
                                 "TELNET_REFUSE",
                                 net->active_session_id,
                                 detail);
        }
            break;

        case HELPER_TELNET_EVENT_SUBNEG_IGNORED:
            helper_net_tracef("VMODEM helper: telnet ignore subnegotiation session=%lu option=%s(%u)\r\n",
                              net->active_session_id,
                              helper_telnet_option_name(event->option),
                              (unsigned int)event->option);
        {
            char detail[64];

            wsprintfA(detail,
                      "option=%s(%u)",
                      helper_telnet_option_name(event->option),
                      (unsigned int)event->option);
            helper_net_log_event(net,
                                 "TELNET_SUBNEG_IGNORED",
                                 net->active_session_id,
                                 detail);
        }
            break;

        case HELPER_TELNET_EVENT_REPLY_SUPPRESSED:
            helper_net_tracef("VMODEM helper: telnet suppress repeat session=%lu %s %s(%u)\r\n",
                              net->active_session_id,
                              helper_telnet_command_name(event->command),
                              helper_telnet_option_name(event->option),
                              (unsigned int)event->option);
        {
            char detail[80];

            wsprintfA(detail,
                      "%s %s(%u)",
                      helper_telnet_command_name(event->command),
                      helper_telnet_option_name(event->option),
                      (unsigned int)event->option);
            helper_net_log_event(net,
                                 "TELNET_REPLY_SUPPRESSED",
                                 net->active_session_id,
                                 detail);
        }
            break;

        default:
            break;
        }
    }
}

static int helper_net_send_buffer(HELPER_NET_RUNTIME *net,
                                  const unsigned char *buffer,
                                  unsigned short length,
                                  unsigned short *pConsumed)
{
    int sent;
    int error;
    unsigned long sessionId;

    if (pConsumed != NULL) {
        *pConsumed = 0;
    }

    if (net == NULL || buffer == NULL || length == 0) {
        return 1;
    }

    if (net->socket_handle == INVALID_SOCKET ||
        net->state != HELPER_NET_STATE_CONNECTED) {
        return 0;
    }

    sent = send(net->socket_handle, (const char *)buffer, length, 0);
    if (sent > 0) {
        if (pConsumed != NULL) {
            *pConsumed = (unsigned short)sent;
        }
        helper_net_log_bytes(net,
                             "SOCKET_TX",
                             net->active_session_id,
                             buffer,
                             (unsigned short)sent);
        return 1;
    }

    if (sent == 0) {
        sessionId = net->active_session_id;
        helper_net_reset_session(net);
        helper_net_submit_remote_closed(net, sessionId);
        return 0;
    }

    error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
        return 1;
    }

    helper_net_tracef("VMODEM helper: send error session=%lu reason=%s\r\n",
                      net->active_session_id,
                      helper_net_fail_reason_name(
                          helper_net_map_socket_error((long)error)));
    sessionId = net->active_session_id;
    helper_net_reset_session(net);
    helper_net_submit_remote_closed(net, sessionId);
    return 0;
}

static int helper_net_flush_pending_send(HELPER_NET_RUNTIME *net)
{
    const unsigned char *controlData;
    unsigned short controlLength;
    unsigned short consumed;
    unsigned short i;
    unsigned short remaining;

    if (net == NULL || net->socket_handle == INVALID_SOCKET) {
        return 0;
    }

    if (!helper_net_flush_peer_control(net)) {
        return helper_net_runtime_has_fatal_error(net) ? 0 : 1;
    }

    while (net->state == HELPER_NET_STATE_CONNECTED) {
        controlLength = helper_telnet_control_length(&net->telnet);
        if (controlLength != 0) {
            controlData = helper_telnet_control_data(&net->telnet);
            consumed = 0;
            if (!helper_net_send_buffer(net, controlData, controlLength, &consumed)) {
                return helper_net_runtime_has_fatal_error(net) ? 0 : 1;
            }
            if (consumed == 0) {
                return 1;
            }
            helper_telnet_consume_control(&net->telnet, consumed);
            continue;
        }

        if (net->pending_send_length == 0) {
            break;
        }

        consumed = 0;
        if (!helper_net_send_buffer(net,
                                    net->pending_send,
                                    net->pending_send_length,
                                    &consumed)) {
            return helper_net_runtime_has_fatal_error(net) ? 0 : 1;
        }
        if (consumed == 0) {
            return 1;
        }
        if (consumed >= net->pending_send_length) {
            net->pending_send_length = 0;
            continue;
        }

        remaining = (unsigned short)(net->pending_send_length - consumed);
        for (i = 0; i < remaining; ++i) {
            net->pending_send[i] = net->pending_send[consumed + i];
        }
        net->pending_send_length = remaining;
    }

    return 1;
}

static int helper_net_prepare_active_socket(HELPER_NET_RUNTIME *net,
                                            SOCKET socketHandle)
{
    if (net == NULL || socketHandle == INVALID_SOCKET) {
        return 0;
    }

    helper_net_try_enable_nodelay(socketHandle);

    if (WSAAsyncSelect(socketHandle,
                       net->hwnd,
                       HELPER_NET_WM_SOCKET,
                       FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
        return 0;
    }

    net->socket_handle = socketHandle;
    net->state = HELPER_NET_STATE_CONNECTED;
    net->deadline_tick = 0;
    net->peer_deadline_tick = 0;
    net->pending_send_length = 0;
    net->target.host[0] = '\0';
    net->target.port = 0;
    helper_telnet_init(&net->telnet);
    return 1;
}

static void helper_net_close_active_as_remote_closed(
    HELPER_NET_RUNTIME *net,
    unsigned long sessionId)
{
    if (net == NULL) {
        return;
    }

    helper_net_reset_session(net);
    if (sessionId != 0UL) {
        helper_net_submit_remote_closed(net, sessionId);
    }
}

static void helper_net_close_pending_as_remote_closed(
    HELPER_NET_RUNTIME *net,
    unsigned long sessionId)
{
    if (net == NULL) {
        return;
    }

    helper_net_reset_pending_inbound(net);
    if (sessionId != 0UL) {
        helper_net_submit_remote_closed(net, sessionId);
    }
}

static int helper_net_finish_connected(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return 0;
    }

    net->state = HELPER_NET_STATE_CONNECTED;
    net->deadline_tick = 0;
    net->peer_deadline_tick = 0;
    if (!helper_net_submit_connect_ok(net)) {
        return 0;
    }
    if (net->active_session_id == 0UL ||
        net->state == HELPER_NET_STATE_IDLE) {
        return 1;
    }
    if (!helper_net_deliver_preanswer_buffer(net,
                                             net->peer_preanswer,
                                             &net->peer_preanswer_length)) {
        return 0;
    }
    return helper_net_flush_pending_send(net);
}

static int helper_net_fallback_to_raw(HELPER_NET_RUNTIME *net,
                                      const char *reason)
{
    if (net == NULL) {
        return 0;
    }

    helper_net_tracef("VMODEM helper: delayed-connect fallback to raw session=%lu reason=%s\r\n",
                      net->active_session_id,
                      (reason != NULL) ? reason : "(none)");
    helper_net_log_event(net,
                         "PEER_FALLBACK_RAW",
                         net->active_session_id,
                         reason);
    helper_net_clear_peer_control(net);
    return helper_net_finish_connected(net);
}

static int helper_net_begin_delayed_connect(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return 0;
    }

    net->state = HELPER_NET_STATE_WAIT_HELLO_ACK;
    net->deadline_tick = 0;
    net->peer_deadline_tick = GetTickCount() + HELPER_NET_NEGOTIATION_WINDOW_MS;
    helper_net_clear_peer_control(net);
    if (!helper_net_queue_peer_frame(net,
                                     HELPER_VMODEM_TYPE_HELLO,
                                     HELPER_VMODEM_FLAG_DELAYED_CONNECT)) {
        return 0;
    }
    helper_net_log_event(net,
                         "PEER_HELLO",
                         net->active_session_id,
                         "delayed-connect");
    return helper_net_flush_peer_control(net);
}

static int helper_net_begin_pending_ring(HELPER_NET_RUNTIME *net,
                                         HELPER_NET_PENDING_MODE mode,
                                         const char *reason)
{
    if (net == NULL || net->pending_session_id == 0UL) {
        return 0;
    }

    net->pending_mode = mode;
    net->pending_deadline_tick = 0;
    helper_net_log_event(net,
                         (mode == HELPER_NET_PENDING_MODE_DELAYED) ?
                             "INBOUND_DELAYED_RING" :
                             "INBOUND_RAW_RING",
                         net->pending_session_id,
                         reason);
    return helper_net_submit_inbound_ring(net, net->pending_session_id);
}

static int helper_net_handle_wait_answered_bytes(HELPER_NET_RUNTIME *net,
                                                 const unsigned char *payload,
                                                 unsigned short payloadLength)
{
    HELPER_VMODEM_FRAME frame;

    if (net == NULL) {
        return 0;
    }

    if (payload != NULL && payloadLength != 0U &&
        !helper_net_append_bytes(net->peer_preanswer,
                                 &net->peer_preanswer_length,
                                 HELPER_NET_PREANSWER_BUFFER_CAP,
                                 payload,
                                 payloadLength)) {
        helper_net_close_active_as_remote_closed(net, net->active_session_id);
        return 1;
    }

    if (!helper_vmodem_frame_prefix_valid(net->peer_preanswer,
                                          (net->peer_preanswer_length <
                                           HELPER_VMODEM_FRAME_LEN) ?
                                              net->peer_preanswer_length :
                                              HELPER_VMODEM_FRAME_LEN)) {
        helper_net_log_event(net,
                             "PEER_PROTOCOL_ERROR",
                             net->active_session_id,
                             "pre-answer payload");
        helper_net_close_active_as_remote_closed(net, net->active_session_id);
        return 1;
    }

    if (net->peer_preanswer_length < HELPER_VMODEM_FRAME_LEN) {
        return 1;
    }

    if (!helper_vmodem_frame_parse(net->peer_preanswer,
                                   HELPER_VMODEM_FRAME_LEN,
                                   &frame) ||
        frame.type != HELPER_VMODEM_TYPE_ANSWERED) {
        helper_net_log_event(net,
                             "PEER_PROTOCOL_ERROR",
                             net->active_session_id,
                             "invalid ANSWERED");
        helper_net_close_active_as_remote_closed(net, net->active_session_id);
        return 1;
    }

    helper_net_consume_bytes(net->peer_preanswer,
                             &net->peer_preanswer_length,
                             HELPER_VMODEM_FRAME_LEN);
    helper_net_log_event(net,
                         "PEER_ANSWERED",
                         net->active_session_id,
                         NULL);
    return helper_net_finish_connected(net);
}

static int helper_net_handle_wait_hello_ack_bytes(HELPER_NET_RUNTIME *net,
                                                  const unsigned char *payload,
                                                  unsigned short payloadLength)
{
    HELPER_VMODEM_FRAME frame;

    if (net == NULL) {
        return 0;
    }

    if (payload != NULL && payloadLength != 0U &&
        !helper_net_append_bytes(net->peer_preanswer,
                                 &net->peer_preanswer_length,
                                 HELPER_NET_PREANSWER_BUFFER_CAP,
                                 payload,
                                 payloadLength)) {
        return helper_net_fallback_to_raw(net, "buffer-full");
    }

    if (!helper_vmodem_frame_prefix_valid(net->peer_preanswer,
                                          (net->peer_preanswer_length <
                                           HELPER_VMODEM_FRAME_LEN) ?
                                              net->peer_preanswer_length :
                                              HELPER_VMODEM_FRAME_LEN)) {
        return helper_net_fallback_to_raw(net, "non-handshake");
    }

    if (net->peer_preanswer_length < HELPER_VMODEM_FRAME_LEN) {
        return 1;
    }

    if (!helper_vmodem_frame_parse(net->peer_preanswer,
                                   HELPER_VMODEM_FRAME_LEN,
                                   &frame) ||
        frame.type != HELPER_VMODEM_TYPE_HELLO_ACK ||
        (frame.flags & HELPER_VMODEM_FLAG_DELAYED_CONNECT) == 0U) {
        return helper_net_fallback_to_raw(net, "invalid-hello-ack");
    }

    helper_net_consume_bytes(net->peer_preanswer,
                             &net->peer_preanswer_length,
                             HELPER_VMODEM_FRAME_LEN);
    net->state = HELPER_NET_STATE_WAIT_ANSWERED;
    net->peer_deadline_tick = 0;
    helper_net_log_event(net,
                         "PEER_HELLO_ACK",
                         net->active_session_id,
                         "accepted");
    if (net->peer_preanswer_length != 0U) {
        return helper_net_handle_wait_answered_bytes(net, NULL, 0U);
    }
    return 1;
}

static int helper_net_handle_pending_probe_bytes(HELPER_NET_RUNTIME *net,
                                                 const unsigned char *payload,
                                                 unsigned short payloadLength)
{
    HELPER_VMODEM_FRAME frame;

    if (net == NULL || net->pending_session_id == 0UL) {
        return 0;
    }

    if (payload != NULL && payloadLength != 0U &&
        !helper_net_append_bytes(net->pending_preanswer,
                                 &net->pending_preanswer_length,
                                 HELPER_NET_PREANSWER_BUFFER_CAP,
                                 payload,
                                 payloadLength)) {
        helper_net_close_pending_as_remote_closed(net, net->pending_session_id);
        return 1;
    }

    if (!helper_vmodem_frame_prefix_valid(net->pending_preanswer,
                                          (net->pending_preanswer_length <
                                           HELPER_VMODEM_FRAME_LEN) ?
                                              net->pending_preanswer_length :
                                              HELPER_VMODEM_FRAME_LEN)) {
        return helper_net_begin_pending_ring(net,
                                             HELPER_NET_PENDING_MODE_RAW,
                                             "non-handshake");
    }

    if (net->pending_preanswer_length < HELPER_VMODEM_FRAME_LEN) {
        return 1;
    }

    if (!helper_vmodem_frame_parse(net->pending_preanswer,
                                   HELPER_VMODEM_FRAME_LEN,
                                   &frame) ||
        frame.type != HELPER_VMODEM_TYPE_HELLO ||
        (frame.flags & HELPER_VMODEM_FLAG_DELAYED_CONNECT) == 0U) {
        return helper_net_begin_pending_ring(net,
                                             HELPER_NET_PENDING_MODE_RAW,
                                             "invalid-hello");
    }

    helper_net_consume_bytes(net->pending_preanswer,
                             &net->pending_preanswer_length,
                             HELPER_VMODEM_FRAME_LEN);
    net->pending_mode = HELPER_NET_PENDING_MODE_DELAYED;
    net->pending_deadline_tick = 0;
    helper_net_log_event(net,
                         "PEER_HELLO_RX",
                         net->pending_session_id,
                         "delayed-connect");
    helper_net_clear_peer_control(net);
    if (!helper_net_queue_peer_frame(net,
                                     HELPER_VMODEM_TYPE_HELLO_ACK,
                                     HELPER_VMODEM_FLAG_DELAYED_CONNECT) ||
        !helper_net_flush_peer_control(net)) {
        return 0;
    }
    if (!helper_net_begin_pending_ring(net,
                                       HELPER_NET_PENDING_MODE_DELAYED,
                                       "hello")) {
        return 0;
    }
    if (net->pending_preanswer_length != 0U) {
        helper_net_log_event(net,
                             "PEER_PROTOCOL_ERROR",
                             net->pending_session_id,
                             "extra bytes after HELLO");
        helper_net_close_pending_as_remote_closed(net, net->pending_session_id);
    }
    return 1;
}

static int helper_net_handle_pending_delayed_bytes(
    HELPER_NET_RUNTIME *net,
    const unsigned char *payload,
    unsigned short payloadLength)
{
    (void)payload;
    (void)payloadLength;

    if (net == NULL || net->pending_session_id == 0UL) {
        return 0;
    }

    helper_net_log_event(net,
                         "PEER_PROTOCOL_ERROR",
                         net->pending_session_id,
                         "unexpected pre-answer data");
    helper_net_close_pending_as_remote_closed(net, net->pending_session_id);
    return 1;
}

static int helper_net_start_listener(HELPER_NET_RUNTIME *net)
{
    struct sockaddr_in address;
    struct hostent *host;
    unsigned long ipAddress;
    int reuseValue;
    int error;

    if (net == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!net->listener_config.enabled) {
        return 1;
    }

    if (!helper_net_listener_config_validate(&net->listener_config)) {
        helper_net_tracef("VMODEM helper: invalid listener config, listener disabled\r\n");
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    helper_net_close_listen_socket(net);

    net->listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (net->listen_socket == INVALID_SOCKET) {
        error = WSAGetLastError();
        helper_net_tracef("VMODEM helper: listen socket create failed error=%d\r\n",
                          error);
        SetLastError((DWORD)error);
        return 0;
    }

    reuseValue = 1;
    setsockopt(net->listen_socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               (const char *)&reuseValue,
               sizeof(reuseValue));

    ZeroMemory(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(net->listener_config.port);
    if (helper_net_is_ipv4_literal(net->listener_config.bind_host, &ipAddress)) {
        address.sin_addr.s_addr = ipAddress;
    } else {
        host = gethostbyname(net->listener_config.bind_host);
        if (host == NULL ||
            host->h_addrtype != AF_INET ||
            host->h_length < (int)sizeof(address.sin_addr) ||
            host->h_addr_list == NULL ||
            host->h_addr_list[0] == NULL) {
            error = WSAGetLastError();
            helper_net_tracef("VMODEM helper: listener resolve failed host=%s error=%d\r\n",
                              net->listener_config.bind_host,
                              error);
            helper_net_close_listen_socket(net);
            SetLastError((DWORD)error);
            return 0;
        }

        CopyMemory(&address.sin_addr,
                   host->h_addr_list[0],
                   sizeof(address.sin_addr));
    }

    if (bind(net->listen_socket,
             (const struct sockaddr *)&address,
             sizeof(address)) == SOCKET_ERROR) {
        error = WSAGetLastError();
        helper_net_tracef("VMODEM helper: listener bind failed host=%s port=%u error=%d\r\n",
                          net->listener_config.bind_host,
                          (unsigned int)net->listener_config.port,
                          error);
        helper_net_close_listen_socket(net);
        SetLastError((DWORD)error);
        return 0;
    }

    if (listen(net->listen_socket, 1) == SOCKET_ERROR) {
        error = WSAGetLastError();
        helper_net_tracef("VMODEM helper: listener listen failed port=%u error=%d\r\n",
                          (unsigned int)net->listener_config.port,
                          error);
        helper_net_close_listen_socket(net);
        SetLastError((DWORD)error);
        return 0;
    }

    if (WSAAsyncSelect(net->listen_socket,
                       net->hwnd,
                       HELPER_NET_WM_LISTEN,
                       FD_ACCEPT | FD_CLOSE) == SOCKET_ERROR) {
        error = WSAGetLastError();
        helper_net_tracef("VMODEM helper: listener async-select failed error=%d\r\n",
                          error);
        helper_net_close_listen_socket(net);
        SetLastError((DWORD)error);
        return 0;
    }

    helper_net_tracef("VMODEM helper: listener started host=%s port=%u\r\n",
                      net->listener_config.bind_host,
                      (unsigned int)net->listener_config.port);
    return 1;
}

static int helper_net_begin_socket_connect(HELPER_NET_RUNTIME *net,
                                           const struct sockaddr_in *address)
{
    int result;
    int error;
    unsigned long sessionId;

    if (net == NULL || address == NULL) {
        return 0;
    }

    helper_net_close_socket(net);

    net->socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (net->socket_handle == INVALID_SOCKET) {
        sessionId = net->active_session_id;
        error = WSAGetLastError();
        helper_net_reset_session(net);
        return helper_net_submit_connect_fail(net,
                                              sessionId,
                                              helper_net_map_socket_error((long)error));
    }

    helper_net_try_enable_nodelay(net->socket_handle);

    if (WSAAsyncSelect(net->socket_handle,
                       net->hwnd,
                       HELPER_NET_WM_SOCKET,
                       FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
        sessionId = net->active_session_id;
        error = WSAGetLastError();
        helper_net_reset_session(net);
        return helper_net_submit_connect_fail(net,
                                              sessionId,
                                              helper_net_map_socket_error((long)error));
    }

    result = connect(net->socket_handle,
                     (const struct sockaddr *)address,
                     sizeof(*address));
    if (result == 0) {
        if ((net->connect_flags & VMODEM_CONNECT_FLAG_RAW) != 0UL) {
            net->state = HELPER_NET_STATE_CONNECTED;
            net->deadline_tick = 0;
            net->peer_deadline_tick = 0;
            if (!helper_net_submit_connect_ok(net)) {
                return 0;
            }
            if (net->active_session_id == 0UL ||
                net->state == HELPER_NET_STATE_IDLE) {
                return 1;
            }
            return helper_net_flush_pending_send(net);
        }
        return helper_net_begin_delayed_connect(net);
    }

    error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
        net->state = HELPER_NET_STATE_CONNECTING;
        helper_net_tracef("VMODEM helper: connecting session=%lu target=%s:%u\r\n",
                          net->active_session_id,
                          net->target.host,
                          (unsigned int)net->target.port);
        return 1;
    }

    sessionId = net->active_session_id;
    helper_net_reset_session(net);
    return helper_net_submit_connect_fail(net,
                                          sessionId,
                                          helper_net_map_socket_error((long)error));
}

static int helper_net_begin_connect(HELPER_NET_RUNTIME *net,
                                    const VMODEM_PROTOCOL_MESSAGE *message)
{
    HELPER_NET_TARGET target;
    struct sockaddr_in address;
    unsigned long ipAddress;
    int parseOk;
    char detail[HELPER_PHONEBOOK_TARGET_LEN + 32];

    if (net == NULL || message == NULL) {
        return HELPER_NET_PROTOCOL_FATAL;
    }

    if (net->state != HELPER_NET_STATE_IDLE ||
        net->active_session_id != 0 ||
        net->pending_session_id != 0) {
        helper_net_tracef("VMODEM helper: rejecting connect while busy session=%lu\r\n",
                          message->session_id);
        if (!helper_net_submit_connect_fail(net,
                                            message->session_id,
                                            VMODEM_CONNECT_FAIL_LOCAL)) {
            return HELPER_NET_PROTOCOL_FATAL;
        }
        return HELPER_NET_PROTOCOL_OK;
    }

    parseOk = helper_net_parse_target(message->payload,
                                      message->payload_length,
                                      &target);
    if (!parseOk) {
        helper_net_tracef("VMODEM helper: invalid connect target session=%lu\r\n",
                          message->session_id);
        if (!helper_net_submit_connect_fail(net,
                                            message->session_id,
                                            VMODEM_CONNECT_FAIL_LOCAL)) {
            return HELPER_NET_PROTOCOL_FATAL;
        }
        return HELPER_NET_PROTOCOL_OK;
    }

    net->active_session_id = message->session_id;
    net->connect_flags = message->status;
    net->target = target;
    net->deadline_tick = GetTickCount() + HELPER_NET_CONNECT_TIMEOUT_MS;
    net->peer_deadline_tick = 0;
    helper_net_clear_peer_control(net);
    net->peer_preanswer_length = 0U;
    net->pending_send_length = 0;
    helper_telnet_init(&net->telnet);

    helper_net_tracef("VMODEM helper: connect request session=%lu target=%s:%u\r\n",
                      net->active_session_id,
                      net->target.host,
                      (unsigned int)net->target.port);
    wsprintfA(detail,
              "target=%s:%u raw=%lu",
              net->target.host,
              (unsigned int)net->target.port,
              (net->connect_flags & VMODEM_CONNECT_FLAG_RAW) ? 1UL : 0UL);
    helper_net_log_event(net,
                         "CONNECT_REQUEST",
                         net->active_session_id,
                         detail);

    if (helper_net_is_ipv4_literal(net->target.host, &ipAddress)) {
        ZeroMemory(&address, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(net->target.port);
        address.sin_addr.s_addr = ipAddress;
        return helper_net_begin_socket_connect(net, &address) ?
            HELPER_NET_PROTOCOL_OK :
            HELPER_NET_PROTOCOL_FATAL;
    }

    net->dns_request = WSAAsyncGetHostByName(net->hwnd,
                                             HELPER_NET_WM_DNS,
                                             net->target.host,
                                             net->dns_buffer,
                                             sizeof(net->dns_buffer));
    if (net->dns_request == NULL) {
        unsigned long sessionId;
        int error;

        sessionId = net->active_session_id;
        error = WSAGetLastError();
        helper_net_reset_session(net);
        if (!helper_net_submit_connect_fail(net,
                                            sessionId,
                                            helper_net_map_socket_error((long)error))) {
            return HELPER_NET_PROTOCOL_FATAL;
        }
        return HELPER_NET_PROTOCOL_OK;
    }

    net->state = HELPER_NET_STATE_RESOLVING;
    helper_net_tracef("VMODEM helper: resolving %s for session=%lu\r\n",
                      net->target.host,
                      net->active_session_id);
    helper_net_log_event(net,
                         "DNS_START",
                         net->active_session_id,
                         net->target.host);
    return HELPER_NET_PROTOCOL_OK;
}

static void helper_net_handle_dns_message(HELPER_NET_RUNTIME *net,
                                          WPARAM wParam,
                                          LPARAM lParam)
{
    struct hostent *host;
    struct sockaddr_in address;
    unsigned long sessionId;
    int error;

    if (net == NULL ||
        net->state != HELPER_NET_STATE_RESOLVING ||
        net->dns_request == NULL ||
        (HANDLE)wParam != net->dns_request) {
        return;
    }

    net->dns_request = NULL;
    error = WSAGETASYNCERROR(lParam);
    if (error != 0) {
        helper_net_tracef("VMODEM helper: dns failure session=%lu target=%s error=%d\r\n",
                          net->active_session_id,
                          net->target.host,
                          error);
        helper_net_log_event(net,
                             "DNS_FAIL",
                             net->active_session_id,
                             net->target.host);
        sessionId = net->active_session_id;
        helper_net_reset_session(net);
        helper_net_submit_connect_fail(net, sessionId, VMODEM_CONNECT_FAIL_DNS);
        return;
    }

    host = (struct hostent *)net->dns_buffer;
    if (host == NULL ||
        host->h_addrtype != AF_INET ||
        host->h_length < (int)sizeof(address.sin_addr) ||
        host->h_addr_list == NULL ||
        host->h_addr_list[0] == NULL) {
        helper_net_tracef("VMODEM helper: dns result unusable session=%lu target=%s\r\n",
                          net->active_session_id,
                          net->target.host);
        helper_net_log_event(net,
                             "DNS_FAIL",
                             net->active_session_id,
                             net->target.host);
        sessionId = net->active_session_id;
        helper_net_reset_session(net);
        helper_net_submit_connect_fail(net, sessionId, VMODEM_CONNECT_FAIL_DNS);
        return;
    }

    helper_net_tracef("VMODEM helper: dns success session=%lu target=%s\r\n",
                      net->active_session_id,
                      net->target.host);
    helper_net_log_event(net,
                         "DNS_OK",
                         net->active_session_id,
                         net->target.host);

    ZeroMemory(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(net->target.port);
    CopyMemory(&address.sin_addr, host->h_addr_list[0], sizeof(address.sin_addr));
    helper_net_begin_socket_connect(net, &address);
}

static void helper_net_handle_listen_message(HELPER_NET_RUNTIME *net,
                                             WPARAM wParam,
                                             LPARAM lParam)
{
    int eventCode;
    int error;
    SOCKET acceptedSocket;
    unsigned long sessionId;

    if (net == NULL ||
        net->listen_socket == INVALID_SOCKET ||
        (SOCKET)wParam != net->listen_socket) {
        return;
    }

    eventCode = WSAGETSELECTEVENT(lParam);
    error = WSAGETSELECTERROR(lParam);

    switch (eventCode) {
    case FD_ACCEPT:
        if (error != 0) {
            helper_net_tracef("VMODEM helper: listener accept error=%d\r\n",
                              error);
            return;
        }

        for (;;) {
            acceptedSocket = accept(net->listen_socket, NULL, NULL);
            if (acceptedSocket == INVALID_SOCKET) {
                error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) {
                    return;
                }

                helper_net_tracef("VMODEM helper: listener accept failed error=%d\r\n",
                                  error);
                return;
            }

            if (net->state != HELPER_NET_STATE_IDLE ||
                !helper_net_start_pending_inbound(net->active_session_id,
                                                  &net->pending_session_id,
                                                  &net->next_inbound_session_id,
                                                  &sessionId)) {
                helper_net_tracef("VMODEM helper: rejecting extra inbound connection while busy\r\n");
                helper_net_log_event(net,
                                     "INBOUND_REJECTED_BUSY",
                                     0UL,
                                     NULL);
                closesocket(acceptedSocket);
                continue;
            }

            net->pending_socket = acceptedSocket;
            net->pending_started_tick = GetTickCount();
            net->pending_deadline_tick =
                net->pending_started_tick + HELPER_NET_NEGOTIATION_WINDOW_MS;
            net->pending_mode = HELPER_NET_PENDING_MODE_PROBE;
            net->pending_preanswer_length = 0U;
            helper_net_clear_peer_control(net);
            helper_net_log_event(net,
                                 "INBOUND_PENDING",
                                 sessionId,
                                 NULL);
            if (WSAAsyncSelect(net->pending_socket,
                               net->hwnd,
                               HELPER_NET_WM_PENDING,
                               FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
                helper_net_tracef("VMODEM helper: pending async-select failed session=%lu error=%d\r\n",
                                  sessionId,
                                  WSAGetLastError());
                helper_net_reset_pending_inbound(net);
                return;
            }
            return;
        }

    case FD_CLOSE:
        helper_net_tracef("VMODEM helper: listener closed unexpectedly error=%d\r\n",
                          error);
        helper_net_close_listen_socket(net);
        if (net->listener_config.enabled) {
            helper_net_start_listener(net);
        }
        return;
    }
}

static void helper_net_handle_pending_message(HELPER_NET_RUNTIME *net,
                                              WPARAM wParam,
                                              LPARAM lParam)
{
    int eventCode;
    int error;
    unsigned char buffer[VMODEM_IPC_MAX_PAYLOAD];
    int received;
    unsigned long sessionId;

    if (net == NULL ||
        net->pending_socket == INVALID_SOCKET ||
        (SOCKET)wParam != net->pending_socket) {
        return;
    }

    sessionId = net->pending_session_id;
    if (sessionId == 0) {
        helper_net_reset_pending_inbound(net);
        return;
    }

    eventCode = WSAGETSELECTEVENT(lParam);
    error = WSAGETSELECTERROR(lParam);

    switch (eventCode) {
    case FD_READ:
        if (error != 0) {
            helper_net_tracef("VMODEM helper: pending read error session=%lu error=%d\r\n",
                              sessionId,
                              error);
            helper_net_close_pending_as_remote_closed(net, sessionId);
            return;
        }

        for (;;) {
            received = recv(net->pending_socket,
                            (char *)buffer,
                            sizeof(buffer),
                            0);
            if (received > 0) {
                helper_net_log_bytes(net,
                                     "SOCKET_RX",
                                     sessionId,
                                     buffer,
                                     (unsigned short)received);
                switch (net->pending_mode) {
                case HELPER_NET_PENDING_MODE_PROBE:
                    if (!helper_net_handle_pending_probe_bytes(
                            net,
                            buffer,
                            (unsigned short)received)) {
                        helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
                    }
                    break;

                case HELPER_NET_PENDING_MODE_RAW:
                    if (!helper_net_append_bytes(net->pending_preanswer,
                                                 &net->pending_preanswer_length,
                                                 HELPER_NET_PREANSWER_BUFFER_CAP,
                                                 buffer,
                                                 (unsigned short)received)) {
                        helper_net_tracef("VMODEM helper: pending raw buffer full session=%lu\r\n",
                                          sessionId);
                        helper_net_close_pending_as_remote_closed(net, sessionId);
                    }
                    break;

                case HELPER_NET_PENDING_MODE_DELAYED:
                    if (!helper_net_handle_pending_delayed_bytes(
                            net,
                            buffer,
                            (unsigned short)received)) {
                        helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
                    }
                    break;

                default:
                    break;
                }

                if (net->fatal ||
                    net->pending_socket == INVALID_SOCKET ||
                    net->pending_session_id != sessionId) {
                    return;
                }
                continue;
            }

            if (received == 0) {
                helper_net_tracef("VMODEM helper: pending remote close session=%lu\r\n",
                                  sessionId);
                helper_net_log_event(net,
                                     "PENDING_REMOTE_CLOSE",
                                     sessionId,
                                     NULL);
                helper_net_close_pending_as_remote_closed(net, sessionId);
                return;
            }

            error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                return;
            }

            helper_net_tracef("VMODEM helper: pending recv failed session=%lu error=%d\r\n",
                              sessionId,
                              error);
            helper_net_close_pending_as_remote_closed(net, sessionId);
            return;
        }

    case FD_WRITE:
        if (error != 0) {
            helper_net_tracef("VMODEM helper: pending write error session=%lu error=%d\r\n",
                              sessionId,
                              error);
            helper_net_close_pending_as_remote_closed(net, sessionId);
            return;
        }

        if (!helper_net_flush_peer_control(net)) {
            helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
        }
        return;

    case FD_CLOSE:
        helper_net_tracef("VMODEM helper: pending remote close session=%lu error=%d\r\n",
                          sessionId,
                          error);
        helper_net_log_event(net,
                             "PENDING_REMOTE_CLOSE",
                             sessionId,
                             NULL);
        helper_net_close_pending_as_remote_closed(net, sessionId);
        return;
    }
}

static void helper_net_handle_socket_message(HELPER_NET_RUNTIME *net,
                                             WPARAM wParam,
                                             LPARAM lParam)
{
    int eventCode;
    int error;
    unsigned char buffer[VMODEM_IPC_MAX_PAYLOAD];
    int received;
    unsigned long sessionId;

    if (net == NULL ||
        net->socket_handle == INVALID_SOCKET ||
        (SOCKET)wParam != net->socket_handle) {
        return;
    }

    eventCode = WSAGETSELECTEVENT(lParam);
    error = WSAGETSELECTERROR(lParam);

    switch (eventCode) {
    case FD_CONNECT:
        if (net->state != HELPER_NET_STATE_CONNECTING) {
            return;
        }

        if (error != 0) {
            sessionId = net->active_session_id;
            helper_net_reset_session(net);
            helper_net_submit_connect_fail(net,
                                           sessionId,
                                           helper_net_map_socket_error((long)error));
            return;
        }

        if ((net->connect_flags & VMODEM_CONNECT_FLAG_RAW) != 0UL) {
            net->state = HELPER_NET_STATE_CONNECTED;
            net->deadline_tick = 0;
            net->peer_deadline_tick = 0;
            if (!helper_net_submit_connect_ok(net)) {
                return;
            }
            if (net->active_session_id == 0UL ||
                net->state == HELPER_NET_STATE_IDLE) {
                return;
            }
            helper_net_flush_pending_send(net);
            return;
        }

        if (!helper_net_begin_delayed_connect(net)) {
            helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
        }
        return;

    case FD_READ:
        if (net->state != HELPER_NET_STATE_CONNECTED &&
            net->state != HELPER_NET_STATE_WAIT_HELLO_ACK &&
            net->state != HELPER_NET_STATE_WAIT_ANSWERED) {
            return;
        }

        if (error != 0) {
            sessionId = net->active_session_id;
            helper_net_close_active_as_remote_closed(net, sessionId);
            return;
        }

        for (;;) {
            received = recv(net->socket_handle,
                            (char *)buffer,
                            sizeof(buffer),
                            0);
            if (received > 0) {
                helper_net_log_bytes(net,
                                     "SOCKET_RX",
                                     net->active_session_id,
                                     buffer,
                                     (unsigned short)received);
                if (net->state == HELPER_NET_STATE_CONNECTED) {
                    if (!helper_net_process_connected_inbound(
                            net,
                            buffer,
                            (unsigned short)received)) {
                        return;
                    }
                } else if (net->state == HELPER_NET_STATE_WAIT_HELLO_ACK) {
                    if (!helper_net_handle_wait_hello_ack_bytes(
                            net,
                            buffer,
                            (unsigned short)received)) {
                        helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
                        return;
                    }
                } else if (net->state == HELPER_NET_STATE_WAIT_ANSWERED) {
                    if (!helper_net_handle_wait_answered_bytes(
                            net,
                            buffer,
                            (unsigned short)received)) {
                        helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
                        return;
                    }
                }

                if (net->fatal ||
                    net->socket_handle == INVALID_SOCKET) {
                    return;
                }
                continue;
            }

            if (received == 0) {
                sessionId = net->active_session_id;
                helper_net_close_active_as_remote_closed(net, sessionId);
                return;
            }

            error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                return;
            }

            sessionId = net->active_session_id;
            helper_net_close_active_as_remote_closed(net, sessionId);
            return;
        }

    case FD_WRITE:
        if (net->state != HELPER_NET_STATE_CONNECTED &&
            net->state != HELPER_NET_STATE_WAIT_HELLO_ACK &&
            net->state != HELPER_NET_STATE_WAIT_ANSWERED) {
            return;
        }

        if (error != 0) {
            sessionId = net->active_session_id;
            helper_net_close_active_as_remote_closed(net, sessionId);
            return;
        }

        if (!helper_net_flush_peer_control(net)) {
            helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
            return;
        }
        if (net->state == HELPER_NET_STATE_CONNECTED) {
            helper_net_flush_pending_send(net);
        }
        return;

    case FD_CLOSE:
        sessionId = net->active_session_id;

        if (net->state == HELPER_NET_STATE_CONNECTING) {
            helper_net_reset_session(net);
            helper_net_submit_connect_fail(net,
                                           sessionId,
                                           (error != 0) ?
                                               helper_net_map_socket_error((long)error) :
                                               VMODEM_CONNECT_FAIL_NETWORK);
            return;
        }

        helper_net_reset_session(net);
        if (sessionId != 0) {
            helper_net_submit_remote_closed(net, sessionId);
        }
        return;
    }
}

static LRESULT CALLBACK helper_net_window_proc(HWND hwnd,
                                               UINT message,
                                               WPARAM wParam,
                                               LPARAM lParam)
{
    HELPER_NET_RUNTIME *net;

    (void)hwnd;

    net = g_helper_net_runtime;
    if (net == NULL) {
        return DefWindowProcA(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case HELPER_NET_WM_DNS:
        helper_net_handle_dns_message(net, wParam, lParam);
        return 0;

    case HELPER_NET_WM_LISTEN:
        helper_net_handle_listen_message(net, wParam, lParam);
        return 0;

    case HELPER_NET_WM_PENDING:
        helper_net_handle_pending_message(net, wParam, lParam);
        return 0;

    case HELPER_NET_WM_SOCKET:
        helper_net_handle_socket_message(net, wParam, lParam);
        return 0;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

int helper_net_runtime_init(HELPER_NET_RUNTIME *net,
                            HINSTANCE hInstance,
                            HANDLE hDevice,
                            unsigned long helper_generation,
                            const HELPER_NET_LISTENER_CONFIG *listener_config)
{
    WNDCLASSA wc;
    WSADATA wsaData;

    if (net == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    ZeroMemory(net, sizeof(*net));
    net->socket_handle = INVALID_SOCKET;
    net->listen_socket = INVALID_SOCKET;
    net->pending_socket = INVALID_SOCKET;
    net->hDevice = hDevice;
    net->helper_generation = helper_generation;
    net->next_inbound_session_id = HELPER_NET_INBOUND_SESSION_BASE;
    helper_net_listener_config_init(&net->listener_config);
    if (listener_config != NULL) {
        net->listener_config = *listener_config;
    }
    helper_telnet_init(&net->telnet);

    if (WSAStartup(MAKEWORD(1, 1), &wsaData) != 0) {
        SetLastError((DWORD)WSAGetLastError());
        return 0;
    }
    net->winsock_started = 1;

    if (!g_helper_net_class_registered) {
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = helper_net_window_proc;
        wc.hInstance = hInstance;
        wc.lpszClassName = HELPER_NET_WINDOW_CLASS;
        if (!RegisterClassA(&wc)) {
            helper_net_runtime_shutdown(net);
            SetLastError(ERROR_GEN_FAILURE);
            return 0;
        }
        g_helper_net_class_registered = 1;
    }

    g_helper_net_runtime = net;
    net->hwnd = CreateWindowA(HELPER_NET_WINDOW_CLASS,
                              HELPER_NET_WINDOW_CLASS,
                              WS_POPUP,
                              0,
                              0,
                              0,
                              0,
                              NULL,
                              NULL,
                              hInstance,
                              NULL);
    if (net->hwnd == NULL) {
        g_helper_net_runtime = NULL;
        helper_net_runtime_shutdown(net);
        return 0;
    }

    if (net->listener_config.enabled) {
        helper_net_start_listener(net);
    }

    return 1;
}

void helper_net_runtime_shutdown(HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return;
    }

    helper_net_reset_session(net);
    helper_net_reset_pending_inbound(net);
    helper_net_close_listen_socket(net);
    if (net->hwnd != NULL) {
        DestroyWindow(net->hwnd);
        net->hwnd = NULL;
    }
    if (net->winsock_started) {
        WSACleanup();
        net->winsock_started = 0;
    }
    if (g_helper_net_runtime == net) {
        g_helper_net_runtime = NULL;
    }
}

void helper_net_runtime_set_protocol(HELPER_NET_RUNTIME *net,
                                     HANDLE hDevice,
                                     unsigned long helper_generation)
{
    if (net == NULL) {
        return;
    }

    net->hDevice = hDevice;
    net->helper_generation = helper_generation;
}

void helper_net_runtime_set_session_log(HELPER_NET_RUNTIME *net,
                                        HELPER_SESSION_LOG *session_log)
{
    if (net == NULL) {
        return;
    }

    net->session_log = session_log;
}

int helper_net_runtime_apply_listener_config(
    HELPER_NET_RUNTIME *net,
    const HELPER_NET_LISTENER_CONFIG *listener_config)
{
    HELPER_NET_LISTENER_CONFIG previous;
    DWORD error;

    if (net == NULL || listener_config == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!helper_net_listener_config_validate(listener_config)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    previous = net->listener_config;

    if (net->listen_socket != INVALID_SOCKET) {
        helper_net_close_listen_socket(net);
    }

    net->listener_config = *listener_config;
    if (!net->listener_config.enabled) {
        return 1;
    }

    if (helper_net_start_listener(net)) {
        return 1;
    }

    error = GetLastError();
    net->listener_config = previous;
    if (previous.enabled) {
        helper_net_start_listener(net);
    }
    SetLastError(error);
    return 0;
}

int helper_net_runtime_submit_connect_fail(HELPER_NET_RUNTIME *net,
                                           unsigned long session_id,
                                           unsigned long reason)
{
    if (net == NULL || session_id == 0UL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    return helper_net_submit_connect_fail(net, session_id, reason);
}

unsigned int helper_net_runtime_pump_messages(HELPER_NET_RUNTIME *net)
{
    MSG msg;
    unsigned int count;

    (void)net;

    count = 0;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        ++count;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return count;
}

void helper_net_runtime_check_timeout(HELPER_NET_RUNTIME *net)
{
    DWORD now;
    unsigned long sessionId;

    if (net == NULL || net->fatal) {
        return;
    }

    now = GetTickCount();

    if (net->pending_session_id != 0UL &&
        net->pending_mode == HELPER_NET_PENDING_MODE_PROBE &&
        net->pending_deadline_tick != 0UL &&
        (now - net->pending_deadline_tick) < 0x80000000UL) {
        helper_net_tracef("VMODEM helper: inbound negotiation timeout session=%lu -> raw ring\r\n",
                          net->pending_session_id);
        if (!helper_net_begin_pending_ring(net,
                                           HELPER_NET_PENDING_MODE_RAW,
                                           "timeout")) {
            helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
        }
    }

    if (net->state == HELPER_NET_STATE_WAIT_HELLO_ACK &&
        net->peer_deadline_tick != 0UL &&
        (now - net->peer_deadline_tick) < 0x80000000UL) {
        if (!helper_net_fallback_to_raw(net, "timeout")) {
            helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
        }
        return;
    }

    if (net->deadline_tick == 0 ||
        (net->state != HELPER_NET_STATE_RESOLVING &&
         net->state != HELPER_NET_STATE_CONNECTING) ||
        (now - net->deadline_tick) >= 0x80000000UL) {
        return;
    }

    sessionId = net->active_session_id;
    helper_net_tracef("VMODEM helper: connect timeout session=%lu target=%s:%u\r\n",
                      sessionId,
                      net->target.host,
                      (unsigned int)net->target.port);
    helper_net_log_event(net,
                         "CONNECT_TIMEOUT",
                         sessionId,
                         NULL);
    helper_net_reset_session(net);
    helper_net_submit_connect_fail(net, sessionId, VMODEM_CONNECT_FAIL_TIMEOUT);
}

int helper_net_runtime_handle_protocol(HELPER_NET_RUNTIME *net,
                                       const VMODEM_PROTOCOL_MESSAGE *message)
{
    unsigned char encoded[HELPER_TELNET_MAX_ENCODED_PAYLOAD];
    unsigned short encodedLength;
    unsigned short consumed;
    unsigned short remaining;
    DWORD pendingElapsed;
    SOCKET promotedSocket;

    if (net == NULL || message == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return HELPER_NET_PROTOCOL_FATAL;
    }

    if (net->fatal) {
        SetLastError(net->fatal_error);
        return HELPER_NET_PROTOCOL_FATAL;
    }

    switch (message->message_type) {
    case VMODEM_MSG_CONNECT_REQ:
        return helper_net_begin_connect(net, message);

    case VMODEM_MSG_DATA_TO_NET:
        if (net->state != HELPER_NET_STATE_CONNECTED ||
            message->session_id == 0 ||
            message->session_id != net->active_session_id) {
            return HELPER_NET_PROTOCOL_OK;
        }

        if (message->payload_length == 0) {
            return HELPER_NET_PROTOCOL_OK;
        }

        helper_net_log_bytes(net,
                             "SERIAL_TX",
                             message->session_id,
                             message->payload,
                             (unsigned short)message->payload_length);

        if (!helper_net_flush_pending_send(net)) {
            return helper_net_runtime_has_fatal_error(net) ?
                HELPER_NET_PROTOCOL_FATAL :
                HELPER_NET_PROTOCOL_OK;
        }

        encodedLength = helper_telnet_encode_outbound(&net->telnet,
                                                      message->payload,
                                                      (unsigned short)message->payload_length,
                                                      encoded,
                                                      sizeof(encoded));
        if (encodedLength == 0) {
            SetLastError(ERROR_GEN_FAILURE);
            helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
            return HELPER_NET_PROTOCOL_FATAL;
        }

        if (helper_telnet_control_length(&net->telnet) != 0 ||
            net->pending_send_length != 0) {
            if (!helper_net_append_pending(net, encoded, encodedLength)) {
                helper_net_tracef("VMODEM helper: pending-send full session=%lu dropping/defering newest chunk\r\n",
                                  message->session_id);
                return HELPER_NET_PROTOCOL_DEFER;
            }
            return HELPER_NET_PROTOCOL_OK;
        }

        consumed = 0;
        if (!helper_net_send_buffer(net, encoded, encodedLength, &consumed)) {
            return helper_net_runtime_has_fatal_error(net) ?
                HELPER_NET_PROTOCOL_FATAL :
                HELPER_NET_PROTOCOL_OK;
        }

        if (consumed == encodedLength) {
            return HELPER_NET_PROTOCOL_OK;
        }

        remaining = (unsigned short)(encodedLength - consumed);
        if (!helper_net_append_pending(net,
                                       encoded + consumed,
                                           remaining)) {
            helper_net_tracef("VMODEM helper: pending-send full after partial send session=%lu\r\n",
                              message->session_id);
            return HELPER_NET_PROTOCOL_DEFER;
        }
        return HELPER_NET_PROTOCOL_OK;

    case VMODEM_MSG_HANGUP_REQ:
        if (message->session_id != 0 &&
            message->session_id == net->active_session_id) {
            helper_net_tracef("VMODEM helper: local hangup/cancel session=%lu\r\n",
                              message->session_id);
            helper_net_log_event(net,
                                 "LOCAL_HANGUP",
                                 message->session_id,
                                 NULL);
            helper_net_reset_session(net);
        } else if (message->session_id != 0 &&
                   message->session_id == net->pending_session_id) {
            helper_net_tracef("VMODEM helper: local reject pending session=%lu\r\n",
                              message->session_id);
            helper_net_log_event(net,
                                 "LOCAL_REJECT_PENDING",
                                 message->session_id,
                                 NULL);
            helper_net_reset_pending_inbound(net);
        }
        return HELPER_NET_PROTOCOL_OK;

    case VMODEM_MSG_ANSWER_REQ:
        if (message->session_id == 0 ||
            message->session_id != net->pending_session_id ||
            net->pending_socket == INVALID_SOCKET ||
            net->active_session_id != 0 ||
            net->state != HELPER_NET_STATE_IDLE) {
            return HELPER_NET_PROTOCOL_OK;
        }

        promotedSocket = net->pending_socket;
        pendingElapsed = GetTickCount() - net->pending_started_tick;
        if (net->pending_mode == HELPER_NET_PENDING_MODE_DELAYED) {
            helper_net_clear_peer_control(net);
            if (!helper_net_queue_peer_frame(net,
                                             HELPER_VMODEM_TYPE_ANSWERED,
                                             HELPER_VMODEM_FLAG_DELAYED_CONNECT)) {
                helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
                return HELPER_NET_PROTOCOL_FATAL;
            }
        }
        if (!helper_net_promote_pending_inbound(&net->active_session_id,
                                                &net->pending_session_id,
                                                message->session_id)) {
            return HELPER_NET_PROTOCOL_OK;
        }

        net->pending_socket = INVALID_SOCKET;
        net->pending_started_tick = 0;
        net->pending_deadline_tick = 0;
        if (!helper_net_prepare_active_socket(net, promotedSocket)) {
            helper_net_tracef("VMODEM helper: answer promotion failed session=%lu error=%d\r\n",
                              message->session_id,
                              WSAGetLastError());
            closesocket(promotedSocket);
            helper_net_reset_session(net);
            if (!helper_net_submit_connect_fail(net,
                                                message->session_id,
                                                VMODEM_CONNECT_FAIL_LOCAL)) {
                return HELPER_NET_PROTOCOL_FATAL;
            }
            return HELPER_NET_PROTOCOL_OK;
        }

        if (net->pending_mode == HELPER_NET_PENDING_MODE_DELAYED &&
            !helper_net_flush_peer_control(net)) {
            helper_net_mark_fatal(net, ERROR_GEN_FAILURE);
            return HELPER_NET_PROTOCOL_FATAL;
        }

        helper_net_tracef("VMODEM helper: answered pending session=%lu after %lu ms\r\n",
                          message->session_id,
                          (unsigned long)pendingElapsed);
        helper_net_log_event(net,
                             "ANSWER_PENDING",
                             message->session_id,
                             NULL);
        net->pending_mode = HELPER_NET_PENDING_MODE_NONE;
        if (!helper_net_submit_connect_ok(net)) {
            return HELPER_NET_PROTOCOL_FATAL;
        }
        if (net->active_session_id == 0UL ||
            net->state == HELPER_NET_STATE_IDLE) {
            return HELPER_NET_PROTOCOL_OK;
        }
        net->peer_preanswer_length = net->pending_preanswer_length;
        if (net->peer_preanswer_length != 0U) {
            CopyMemory(net->peer_preanswer,
                       net->pending_preanswer,
                       net->peer_preanswer_length);
        }
        net->pending_preanswer_length = 0U;
        if (!helper_net_deliver_preanswer_buffer(net,
                                                 net->peer_preanswer,
                                                 &net->peer_preanswer_length)) {
            return HELPER_NET_PROTOCOL_FATAL;
        }
        return HELPER_NET_PROTOCOL_OK;
    }

    return HELPER_NET_PROTOCOL_OK;
}

int helper_net_runtime_has_fatal_error(const HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return 0;
    }

    return net->fatal;
}

DWORD helper_net_runtime_get_fatal_error(const HELPER_NET_RUNTIME *net)
{
    if (net == NULL) {
        return ERROR_GEN_FAILURE;
    }

    return (net->fatal_error != 0) ? net->fatal_error : ERROR_GEN_FAILURE;
}
