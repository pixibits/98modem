#ifndef VMODEM_HELPER_SESSION_LOG_H
#define VMODEM_HELPER_SESSION_LOG_H

#include <stdio.h>
#include <windows.h>

#include "ipc_shared.h"

#define HELPER_SESSION_LOG_FILE_NAME "vmodem-session.log"

typedef struct HELPER_SESSION_LOG {
    int  enabled;
    char path[MAX_PATH];
    FILE *file;
} HELPER_SESSION_LOG;

void helper_session_log_init(HELPER_SESSION_LOG *log);
void helper_session_log_shutdown(HELPER_SESSION_LOG *log);
int helper_session_log_apply(HELPER_SESSION_LOG *log,
                             const char *path,
                             int enabled);
int helper_session_log_is_enabled(const HELPER_SESSION_LOG *log);
const char *helper_session_log_path(const HELPER_SESSION_LOG *log);
const char *helper_session_log_message_name(unsigned long message_type);
void helper_session_log_text(HELPER_SESSION_LOG *log,
                             const char *event_name,
                             const char *text);
void helper_session_log_event(HELPER_SESSION_LOG *log,
                              const char *event_name,
                              unsigned long session_id,
                              const char *detail);
void helper_session_log_bytes(HELPER_SESSION_LOG *log,
                              const char *event_name,
                              unsigned long session_id,
                              const unsigned char *bytes,
                              unsigned short length);
void helper_session_log_protocol(HELPER_SESSION_LOG *log,
                                 const char *direction,
                                 const VMODEM_PROTOCOL_MESSAGE *message);

#endif /* VMODEM_HELPER_SESSION_LOG_H */
