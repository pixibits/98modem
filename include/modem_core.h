/*
 * modem_core.h - Shared modem state machine used by the Win98 VxD and
 *                host-side tests.
 *
 * This header intentionally avoids Win32/VxD headers so the same core can be
 * built in both environments.
 */
#ifndef VMODEM_MODEM_CORE_H
#define VMODEM_MODEM_CORE_H

#define VM_MODEM_CMD_BUFFER_LEN      128U
#define VM_MODEM_OUTPUT_BUFFER_LEN  8192U
#define VM_MODEM_ACTION_QUEUE_LEN     16U
#define VM_MODEM_ACTION_PAYLOAD_LEN  256U
#define VM_MODEM_ESCAPE_GUARD_MS     500UL
#define VM_MODEM_RING_ON_MS         2000UL
#define VM_MODEM_RING_OFF_MS        4000UL
#define VM_MODEM_RING_TIMEOUT_MS   30000UL

#define VM_MODEM_STATUS_CTS  0x0010UL
#define VM_MODEM_STATUS_DSR  0x0020UL
#define VM_MODEM_STATUS_RING 0x0040UL
#define VM_MODEM_STATUS_DCD  0x0080UL

typedef enum VM_MODEM_STATE {
    VM_MODEM_STATE_IDLE_CMD = 0,
    VM_MODEM_STATE_DIALING = 1,
    VM_MODEM_STATE_RINGING = 2,
    VM_MODEM_STATE_CONNECTED_DATA = 3,
    VM_MODEM_STATE_CONNECTED_CMD = 4,
    VM_MODEM_STATE_CLOSING = 5,
    VM_MODEM_STATE_HELPER_UNAVAILABLE = 6
} VM_MODEM_STATE;

typedef enum VM_MODEM_ACTION_TYPE {
    VM_MODEM_ACTION_NONE = 0,
    VM_MODEM_ACTION_CONNECT_REQ = 1,
    VM_MODEM_ACTION_DATA_TO_NET = 2,
    VM_MODEM_ACTION_ANSWER_REQ = 3,
    VM_MODEM_ACTION_HANGUP_REQ = 4
} VM_MODEM_ACTION_TYPE;

typedef struct VM_MODEM_ACTION {
    VM_MODEM_ACTION_TYPE type;
    unsigned long        session_id;
    unsigned long        status;
    unsigned short       payload_length;
    unsigned char        payload[VM_MODEM_ACTION_PAYLOAD_LEN];
} VM_MODEM_ACTION;

typedef struct VM_MODEM_CORE {
    VM_MODEM_STATE state;
    unsigned long  modem_status;
    int            echo_enabled;
    int            port_open;
    int            helper_available;
    int            host_dtr_asserted;
    int            host_rts_asserted;
    int            raw_mode_enabled;
    unsigned char  quiet_mode;
    int            numeric_responses;
    unsigned long  next_session_id;
    unsigned long  current_session_id;
    unsigned long  s0_auto_answer_rings;
    unsigned long  s1_ring_count;
    unsigned char  command_buffer[VM_MODEM_CMD_BUFFER_LEN];
    unsigned short command_length;
    int            command_overflowed;
    unsigned char  output_buffer[VM_MODEM_OUTPUT_BUFFER_LEN];
    unsigned short output_get;
    unsigned short output_put;
    unsigned short output_count;
    VM_MODEM_ACTION action_queue[VM_MODEM_ACTION_QUEUE_LEN];
    unsigned short  action_get;
    unsigned short  action_put;
    unsigned short  action_count;
    int            have_last_tx_time;
    unsigned long  last_tx_time_ms;
    unsigned long  last_clock_ms;
    unsigned short escape_plus_count;
    int            escape_post_guard_pending;
    unsigned long  escape_post_guard_deadline_ms;
    int            ring_signal_asserted;
    unsigned long  ring_next_time_ms;
    unsigned long  ring_signal_deadline_ms;
    unsigned long  ring_timeout_deadline_ms;
} VM_MODEM_CORE;

void vm_modem_init(VM_MODEM_CORE *modem);
void vm_modem_reset(VM_MODEM_CORE *modem);
void vm_modem_port_open(VM_MODEM_CORE *modem, unsigned long now_ms);
void vm_modem_port_close(VM_MODEM_CORE *modem);
void vm_modem_set_helper_available(VM_MODEM_CORE *modem, int available);
void vm_modem_set_host_lines(VM_MODEM_CORE *modem,
                             int dtr_asserted,
                             int rts_asserted,
                             unsigned long now_ms);
void vm_modem_poll(VM_MODEM_CORE *modem, unsigned long now_ms);
void vm_modem_ingest_tx(VM_MODEM_CORE *modem,
                        const unsigned char *bytes,
                        unsigned short count,
                        unsigned long now_ms);
int vm_modem_on_connect_ok(VM_MODEM_CORE *modem,
                           unsigned long session_id,
                           unsigned long now_ms);
int vm_modem_on_connect_fail(VM_MODEM_CORE *modem,
                             unsigned long session_id,
                             unsigned long reason);
unsigned short vm_modem_on_serial_from_helper(VM_MODEM_CORE *modem,
                                              unsigned long session_id,
                                              const unsigned char *bytes,
                                              unsigned short count);
int vm_modem_on_inbound_ring(VM_MODEM_CORE *modem,
                             unsigned long session_id);
int vm_modem_on_remote_closed(VM_MODEM_CORE *modem,
                              unsigned long session_id);
int vm_modem_peek_action(const VM_MODEM_CORE *modem,
                         VM_MODEM_ACTION *action);
void vm_modem_pop_action(VM_MODEM_CORE *modem);
unsigned short vm_modem_drain_output(VM_MODEM_CORE *modem,
                                     unsigned char *buffer,
                                     unsigned short capacity);
void vm_modem_clear_output(VM_MODEM_CORE *modem);
unsigned short vm_modem_output_count(const VM_MODEM_CORE *modem);
VM_MODEM_STATE vm_modem_get_state(const VM_MODEM_CORE *modem);
unsigned long vm_modem_get_status(const VM_MODEM_CORE *modem);
int vm_modem_get_echo_enabled(const VM_MODEM_CORE *modem);
unsigned long vm_modem_get_active_session_id(const VM_MODEM_CORE *modem);
int vm_modem_get_helper_available(const VM_MODEM_CORE *modem);
int vm_modem_get_raw_mode_enabled(const VM_MODEM_CORE *modem);
unsigned long vm_modem_get_s0_auto_answer_rings(const VM_MODEM_CORE *modem);
unsigned long vm_modem_get_s1_ring_count(const VM_MODEM_CORE *modem);
int vm_modem_get_next_timer_deadline(const VM_MODEM_CORE *modem,
                                     unsigned long *deadline_ms);

#endif /* VMODEM_MODEM_CORE_H */
