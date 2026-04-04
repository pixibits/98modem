#include <string.h>

#include "session_log.h"

static void helper_session_log_write_prefix(FILE *file);
static void helper_session_log_write_chunks(FILE *file,
                                            const unsigned char *bytes,
                                            unsigned short length);

static void helper_session_log_write_prefix(FILE *file)
{
    SYSTEMTIME time_now;

    if (file == NULL) {
        return;
    }

    GetLocalTime(&time_now);
    fprintf(file,
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            (unsigned int)time_now.wYear,
            (unsigned int)time_now.wMonth,
            (unsigned int)time_now.wDay,
            (unsigned int)time_now.wHour,
            (unsigned int)time_now.wMinute,
            (unsigned int)time_now.wSecond,
            (unsigned int)time_now.wMilliseconds);
}

static void helper_session_log_write_chunks(FILE *file,
                                            const unsigned char *bytes,
                                            unsigned short length)
{
    unsigned short offset;
    unsigned short i;
    unsigned short chunk;
    unsigned char ch;
    char hex_text[16U * 3U + 1U];
    char ascii_text[16U + 1U];
    char *hex_ptr;

    if (file == NULL || bytes == NULL || length == 0U) {
        return;
    }

    offset = 0;
    while (offset < length) {
        chunk = (unsigned short)(length - offset);
        if (chunk > 16U) {
            chunk = 16U;
        }

        hex_ptr = hex_text;
        for (i = 0; i < chunk; ++i) {
            wsprintfA(hex_ptr, "%02X ", (unsigned int)bytes[offset + i]);
            hex_ptr += 3;
            ch = bytes[offset + i];
            ascii_text[i] =
                (ch >= 0x20U && ch <= 0x7EU) ? (char)ch : '.';
        }
        for (; i < 16U; ++i) {
            lstrcpyA(hex_ptr, "   ");
            hex_ptr += 3;
            ascii_text[i] = ' ';
        }
        *hex_ptr = '\0';
        ascii_text[16] = '\0';

        fprintf(file,
                "    %04X: %s|%s|\r\n",
                (unsigned int)offset,
                hex_text,
                ascii_text);
        offset = (unsigned short)(offset + chunk);
    }
}

void helper_session_log_init(HELPER_SESSION_LOG *log)
{
    if (log == NULL) {
        return;
    }

    memset(log, 0, sizeof(*log));
}

void helper_session_log_shutdown(HELPER_SESSION_LOG *log)
{
    if (log == NULL) {
        return;
    }

    if (log->file != NULL) {
        helper_session_log_event(log, "LOG_STOP", 0UL, NULL);
        fclose(log->file);
        log->file = NULL;
    }

    log->enabled = 0;
}

int helper_session_log_apply(HELPER_SESSION_LOG *log,
                             const char *path,
                             int enabled)
{
    FILE *file;

    if (log == NULL || path == NULL || path[0] == '\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!enabled) {
        helper_session_log_shutdown(log);
        lstrcpynA(log->path, path, sizeof(log->path));
        return 1;
    }

    if (log->enabled &&
        log->file != NULL &&
        lstrcmpA(log->path, path) == 0) {
        return 1;
    }

    helper_session_log_shutdown(log);

    file = fopen(path, "ab");
    if (file == NULL) {
        SetLastError(ERROR_OPEN_FAILED);
        return 0;
    }

    log->file = file;
    log->enabled = 1;
    lstrcpynA(log->path, path, sizeof(log->path));
    helper_session_log_event(log, "LOG_START", 0UL, path);
    return 1;
}

int helper_session_log_is_enabled(const HELPER_SESSION_LOG *log)
{
    if (log == NULL) {
        return 0;
    }

    return (log->enabled && log->file != NULL) ? 1 : 0;
}

const char *helper_session_log_path(const HELPER_SESSION_LOG *log)
{
    if (log == NULL) {
        return "";
    }

    return log->path;
}

const char *helper_session_log_message_name(unsigned long message_type)
{
    switch (message_type) {
    case VMODEM_MSG_CONNECT_REQ:
        return "CONNECT_REQ";
    case VMODEM_MSG_CONNECT_OK:
        return "CONNECT_OK";
    case VMODEM_MSG_CONNECT_FAIL:
        return "CONNECT_FAIL";
    case VMODEM_MSG_DATA_TO_NET:
        return "DATA_TO_NET";
    case VMODEM_MSG_DATA_TO_SERIAL:
        return "DATA_TO_SERIAL";
    case VMODEM_MSG_ANSWER_REQ:
        return "ANSWER_REQ";
    case VMODEM_MSG_HANGUP_REQ:
        return "HANGUP_REQ";
    case VMODEM_MSG_INBOUND_RING:
        return "INBOUND_RING";
    case VMODEM_MSG_REMOTE_CLOSED:
        return "REMOTE_CLOSED";
    default:
        return "UNKNOWN";
    }
}

void helper_session_log_text(HELPER_SESSION_LOG *log,
                             const char *event_name,
                             const char *text)
{
    if (!helper_session_log_is_enabled(log) || event_name == NULL) {
        return;
    }

    helper_session_log_write_prefix(log->file);
    if (text != NULL && text[0] != '\0') {
        fprintf(log->file, "%s %s\r\n", event_name, text);
    } else {
        fprintf(log->file, "%s\r\n", event_name);
    }
    fflush(log->file);
}

void helper_session_log_event(HELPER_SESSION_LOG *log,
                              const char *event_name,
                              unsigned long session_id,
                              const char *detail)
{
    if (!helper_session_log_is_enabled(log) || event_name == NULL) {
        return;
    }

    helper_session_log_write_prefix(log->file);
    if (detail != NULL && detail[0] != '\0') {
        fprintf(log->file,
                "%s session=%lu %s\r\n",
                event_name,
                session_id,
                detail);
    } else {
        fprintf(log->file, "%s session=%lu\r\n", event_name, session_id);
    }
    fflush(log->file);
}

void helper_session_log_bytes(HELPER_SESSION_LOG *log,
                              const char *event_name,
                              unsigned long session_id,
                              const unsigned char *bytes,
                              unsigned short length)
{
    if (!helper_session_log_is_enabled(log) || event_name == NULL) {
        return;
    }

    helper_session_log_write_prefix(log->file);
    fprintf(log->file,
            "%s session=%lu len=%u\r\n",
            event_name,
            session_id,
            (unsigned int)length);
    helper_session_log_write_chunks(log->file, bytes, length);
    fflush(log->file);
}

void helper_session_log_protocol(HELPER_SESSION_LOG *log,
                                 const char *direction,
                                 const VMODEM_PROTOCOL_MESSAGE *message)
{
    char label[48];
    char detail[96];
    const char *message_name;

    if (!helper_session_log_is_enabled(log) ||
        direction == NULL ||
        message == NULL) {
        return;
    }

    message_name = helper_session_log_message_name(message->message_type);
    wsprintfA(label, "%s_%s", direction, message_name);
    wsprintfA(detail,
              "status=%lu payload=%lu",
              message->status,
              message->payload_length);

    if (message->payload_length != 0U) {
        helper_session_log_bytes(log,
                                 label,
                                 message->session_id,
                                 message->payload,
                                 (unsigned short)message->payload_length);
        helper_session_log_event(log, label, message->session_id, detail);
        return;
    }

    helper_session_log_event(log, label, message->session_id, detail);
}
