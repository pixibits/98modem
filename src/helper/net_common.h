#ifndef VMODEM_HELPER_NET_COMMON_H
#define VMODEM_HELPER_NET_COMMON_H

#include "ipc_shared.h"

#define HELPER_NET_CONNECT_TIMEOUT_MS 5000UL
#define HELPER_NET_PENDING_SEND_CAP   2048U
#define HELPER_NET_HOST_BUFFER_LEN    (VMODEM_IPC_MAX_PAYLOAD + 1U)
#define HELPER_NET_DEFAULT_LISTEN_PORT 2323U
#define HELPER_NET_INBOUND_SESSION_BASE 0x40000000UL
#define HELPER_APP_CONFIG_TEXT_CAP    32768U
#define HELPER_PHONEBOOK_MAX_ENTRIES  128U
#define HELPER_PHONEBOOK_NAME_LEN     64U
#define HELPER_PHONEBOOK_NUMBER_LEN   64U
#define HELPER_PHONEBOOK_NORMALIZED_LEN 64U
#define HELPER_PHONEBOOK_TARGET_LEN   (VMODEM_IPC_MAX_PAYLOAD + 1U)

#define HELPER_DIAL_RESOLUTION_INVALID   0
#define HELPER_DIAL_RESOLUTION_DIRECT    1
#define HELPER_DIAL_RESOLUTION_PHONEBOOK 2
#define HELPER_DIAL_RESOLUTION_PROMPT    3

typedef struct HELPER_NET_TARGET {
    char           host[HELPER_NET_HOST_BUFFER_LEN];
    unsigned short port;
} HELPER_NET_TARGET;

typedef struct HELPER_NET_LISTENER_CONFIG {
    int            enabled;
    char           bind_host[HELPER_NET_HOST_BUFFER_LEN];
    unsigned short port;
} HELPER_NET_LISTENER_CONFIG;

typedef struct HELPER_LOG_CONFIG {
    int enabled;
} HELPER_LOG_CONFIG;

typedef struct HELPER_PHONEBOOK_ENTRY {
    char name[HELPER_PHONEBOOK_NAME_LEN];
    char number[HELPER_PHONEBOOK_NUMBER_LEN];
    char normalized_number[HELPER_PHONEBOOK_NORMALIZED_LEN];
    char target[HELPER_PHONEBOOK_TARGET_LEN];
    int  raw_mode;
} HELPER_PHONEBOOK_ENTRY;

typedef struct HELPER_APP_CONFIG {
    HELPER_NET_LISTENER_CONFIG listener;
    HELPER_LOG_CONFIG          logging;
    unsigned int               phonebook_count;
    HELPER_PHONEBOOK_ENTRY     phonebook[HELPER_PHONEBOOK_MAX_ENTRIES];
} HELPER_APP_CONFIG;

int helper_net_parse_target(const unsigned char *payload,
                            unsigned long payload_length,
                            HELPER_NET_TARGET *target);
unsigned long helper_net_map_socket_error(long error_code);
const char *helper_net_fail_reason_name(unsigned long reason);
void helper_app_config_init(HELPER_APP_CONFIG *config);
int helper_app_config_validate(HELPER_APP_CONFIG *config);
int helper_app_config_parse_text(const char *text,
                                 unsigned long text_length,
                                 HELPER_APP_CONFIG *config);
int helper_app_config_load_file(const char *path,
                                HELPER_APP_CONFIG *config);
int helper_app_config_save_file(const char *path,
                                const HELPER_APP_CONFIG *config);
void helper_log_config_init(HELPER_LOG_CONFIG *config);
void helper_phonebook_entry_init(HELPER_PHONEBOOK_ENTRY *entry);
int helper_log_config_validate(const HELPER_LOG_CONFIG *config);
void helper_net_listener_config_init(HELPER_NET_LISTENER_CONFIG *config);
int helper_net_listener_config_validate(
    const HELPER_NET_LISTENER_CONFIG *config);
int helper_net_listener_config_parse_text(const char *text,
                                          unsigned long text_length,
                                          HELPER_NET_LISTENER_CONFIG *config);
int helper_net_listener_config_load_file(const char *path,
                                         HELPER_NET_LISTENER_CONFIG *config);
int helper_phonebook_normalize_number(const char *text,
                                      char *normalized,
                                      unsigned int capacity);
int helper_phonebook_entry_validate(HELPER_PHONEBOOK_ENTRY *entry);
int helper_phonebook_find_by_number(const HELPER_APP_CONFIG *config,
                                    const char *normalized_number,
                                    unsigned int *index_out);
int helper_phonebook_payload_is_phone_like(
    const unsigned char *payload,
    unsigned long payload_length,
    char *display_number,
    unsigned int display_capacity,
    char *normalized_number,
    unsigned int normalized_capacity);
int helper_phonebook_resolve_dial(const HELPER_APP_CONFIG *config,
                                  const unsigned char *payload,
                                  unsigned long payload_length,
                                  char *target_text,
                                  unsigned int target_capacity,
                                  char *normalized_number,
                                  unsigned int normalized_capacity,
                                  unsigned int *index_out);
int helper_net_can_accept_inbound(unsigned long active_session_id,
                                  unsigned long pending_session_id);
unsigned long helper_net_allocate_inbound_session(
    unsigned long *next_session_id);
int helper_net_start_pending_inbound(unsigned long active_session_id,
                                     unsigned long *pending_session_id,
                                     unsigned long *next_session_id,
                                     unsigned long *session_id_out);
int helper_net_promote_pending_inbound(unsigned long *active_session_id,
                                       unsigned long *pending_session_id,
                                       unsigned long request_session_id);
int helper_net_clear_pending_inbound(unsigned long *pending_session_id,
                                     unsigned long request_session_id);

#endif /* VMODEM_HELPER_NET_COMMON_H */
