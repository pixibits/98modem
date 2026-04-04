#include <stdio.h>
#include <string.h>

#include "net_common.h"

#ifndef WSAHOST_NOT_FOUND
#define WSAHOST_NOT_FOUND 11001L
#endif

#ifndef WSATRY_AGAIN
#define WSATRY_AGAIN 11002L
#endif

#ifndef WSANO_RECOVERY
#define WSANO_RECOVERY 11003L
#endif

#ifndef WSANO_DATA
#define WSANO_DATA 11004L
#endif

#ifndef WSAEINTR
#define WSAEINTR 10004L
#endif

#ifndef WSAEBADF
#define WSAEBADF 10009L
#endif

#ifndef WSAEACCES
#define WSAEACCES 10013L
#endif

#ifndef WSAEFAULT
#define WSAEFAULT 10014L
#endif

#ifndef WSAEINVAL
#define WSAEINVAL 10022L
#endif

#ifndef WSAEMFILE
#define WSAEMFILE 10024L
#endif

#ifndef WSAEWOULDBLOCK
#define WSAEWOULDBLOCK 10035L
#endif

#ifndef WSAEINPROGRESS
#define WSAEINPROGRESS 10036L
#endif

#ifndef WSAENOTSOCK
#define WSAENOTSOCK 10038L
#endif

#ifndef WSAEDESTADDRREQ
#define WSAEDESTADDRREQ 10039L
#endif

#ifndef WSAEMSGSIZE
#define WSAEMSGSIZE 10040L
#endif

#ifndef WSAEPROTOTYPE
#define WSAEPROTOTYPE 10041L
#endif

#ifndef WSAENOPROTOOPT
#define WSAENOPROTOOPT 10042L
#endif

#ifndef WSAEPROTONOSUPPORT
#define WSAEPROTONOSUPPORT 10043L
#endif

#ifndef WSAESOCKTNOSUPPORT
#define WSAESOCKTNOSUPPORT 10044L
#endif

#ifndef WSAEAFNOSUPPORT
#define WSAEAFNOSUPPORT 10047L
#endif

#ifndef WSAEADDRINUSE
#define WSAEADDRINUSE 10048L
#endif

#ifndef WSAEADDRNOTAVAIL
#define WSAEADDRNOTAVAIL 10049L
#endif

#ifndef WSAENETDOWN
#define WSAENETDOWN 10050L
#endif

#ifndef WSAENETUNREACH
#define WSAENETUNREACH 10051L
#endif

#ifndef WSAENETRESET
#define WSAENETRESET 10052L
#endif

#ifndef WSAECONNABORTED
#define WSAECONNABORTED 10053L
#endif

#ifndef WSAECONNRESET
#define WSAECONNRESET 10054L
#endif

#ifndef WSAENOBUFS
#define WSAENOBUFS 10055L
#endif

#ifndef WSAEISCONN
#define WSAEISCONN 10056L
#endif

#ifndef WSAENOTCONN
#define WSAENOTCONN 10057L
#endif

#ifndef WSAETIMEDOUT
#define WSAETIMEDOUT 10060L
#endif

#ifndef WSAECONNREFUSED
#define WSAECONNREFUSED 10061L
#endif

#ifndef WSAEHOSTDOWN
#define WSAEHOSTDOWN 10064L
#endif

#ifndef WSAEHOSTUNREACH
#define WSAEHOSTUNREACH 10065L
#endif

static unsigned char helper_net_ascii_upper(unsigned char ch);
static void helper_net_trim_bounds(const char *text,
                                   unsigned long *start,
                                   unsigned long *end);
static int helper_net_text_equals_ci(const char *text,
                                     unsigned long start,
                                     unsigned long end,
                                     const char *expected);
static int helper_net_parse_bool(const char *text,
                                 unsigned long start,
                                 unsigned long end,
                                 int *value);
static int helper_net_parse_decimal(const char *text,
                                    unsigned long start,
                                    unsigned long end,
                                    unsigned long *value_out);
static int helper_net_copy_value(const char *text,
                                 unsigned long start,
                                 unsigned long end,
                                 char *dest,
                                 unsigned int capacity);
static int helper_net_copy_payload_text(const unsigned char *payload,
                                        unsigned long payload_length,
                                        char *dest,
                                        unsigned int capacity);
static int helper_net_validate_printable_ascii(const char *text);
static int helper_net_apply_listener_setting(
    HELPER_NET_LISTENER_CONFIG *config,
    const char *text,
    unsigned long key_start,
    unsigned long key_end,
    unsigned long value_start,
    unsigned long value_end);
static int helper_net_apply_logging_setting(
    HELPER_LOG_CONFIG *config,
    const char *text,
    unsigned long key_start,
    unsigned long key_end,
    unsigned long value_start,
    unsigned long value_end);
static int helper_net_section_phonebook_index(const char *text,
                                              unsigned long start,
                                              unsigned long end,
                                              unsigned int *index_out);
static int helper_net_apply_phonebook_meta(
    unsigned int *phonebook_count,
    const char *text,
    unsigned long key_start,
    unsigned long key_end,
    unsigned long value_start,
    unsigned long value_end);
static int helper_net_apply_phonebook_entry_setting(
    HELPER_PHONEBOOK_ENTRY *entry,
    const char *text,
    unsigned long key_start,
    unsigned long key_end,
    unsigned long value_start,
    unsigned long value_end);

static unsigned char helper_net_ascii_upper(unsigned char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (unsigned char)(ch - ('a' - 'A'));
    }

    return ch;
}

static void helper_net_trim_bounds(const char *text,
                                   unsigned long *start,
                                   unsigned long *end)
{
    if (text == 0 || start == 0 || end == 0) {
        return;
    }

    while (*start < *end &&
           (text[*start] == ' ' || text[*start] == '\t')) {
        ++(*start);
    }

    while (*end > *start &&
           (text[*end - 1U] == ' ' || text[*end - 1U] == '\t')) {
        --(*end);
    }
}

static int helper_net_text_equals_ci(const char *text,
                                     unsigned long start,
                                     unsigned long end,
                                     const char *expected)
{
    unsigned long i;

    if (text == 0 || expected == 0) {
        return 0;
    }

    i = 0;
    while ((start + i) < end && expected[i] != '\0') {
        if (helper_net_ascii_upper((unsigned char)text[start + i]) !=
            helper_net_ascii_upper((unsigned char)expected[i])) {
            return 0;
        }
        ++i;
    }

    if ((start + i) != end || expected[i] != '\0') {
        return 0;
    }

    return 1;
}

static int helper_net_parse_bool(const char *text,
                                 unsigned long start,
                                 unsigned long end,
                                 int *value)
{
    if (value == 0) {
        return 0;
    }

    if (end == start + 1U) {
        if (text[start] == '0') {
            *value = 0;
            return 1;
        }
        if (text[start] == '1') {
            *value = 1;
            return 1;
        }
    }

    if (helper_net_text_equals_ci(text, start, end, "TRUE") ||
        helper_net_text_equals_ci(text, start, end, "YES") ||
        helper_net_text_equals_ci(text, start, end, "ON")) {
        *value = 1;
        return 1;
    }

    if (helper_net_text_equals_ci(text, start, end, "FALSE") ||
        helper_net_text_equals_ci(text, start, end, "NO") ||
        helper_net_text_equals_ci(text, start, end, "OFF")) {
        *value = 0;
        return 1;
    }

    return 0;
}

static int helper_net_parse_decimal(const char *text,
                                    unsigned long start,
                                    unsigned long end,
                                    unsigned long *value_out)
{
    unsigned long value;
    unsigned long i;
    unsigned char ch;

    if (text == 0 || value_out == 0 || end <= start) {
        return 0;
    }

    value = 0;
    for (i = start; i < end; ++i) {
        ch = (unsigned char)text[i];
        if (ch < '0' || ch > '9') {
            return 0;
        }
        value = (value * 10UL) + (unsigned long)(ch - '0');
        if (value > 65535UL + HELPER_PHONEBOOK_MAX_ENTRIES) {
            return 0;
        }
    }

    *value_out = value;
    return 1;
}

static int helper_net_copy_value(const char *text,
                                 unsigned long start,
                                 unsigned long end,
                                 char *dest,
                                 unsigned int capacity)
{
    unsigned long i;
    unsigned char ch;

    if (text == 0 || dest == 0 || capacity == 0U) {
        return 0;
    }

    if (end <= start || (end - start) >= (unsigned long)capacity) {
        return 0;
    }

    for (i = start; i < end; ++i) {
        ch = (unsigned char)text[i];
        if (ch < 0x20U || ch > 0x7EU) {
            return 0;
        }
        dest[i - start] = (char)ch;
    }

    dest[end - start] = '\0';
    return 1;
}

static int helper_net_copy_payload_text(const unsigned char *payload,
                                        unsigned long payload_length,
                                        char *dest,
                                        unsigned int capacity)
{
    unsigned long i;
    unsigned char ch;

    if (payload == 0 || dest == 0 || capacity == 0U) {
        return 0;
    }

    if (payload_length == 0 ||
        payload_length >= (unsigned long)capacity ||
        payload_length > VMODEM_IPC_MAX_PAYLOAD) {
        return 0;
    }

    for (i = 0; i < payload_length; ++i) {
        ch = payload[i];
        if (ch < 0x20U || ch > 0x7EU) {
            return 0;
        }
        dest[i] = (char)ch;
    }

    dest[payload_length] = '\0';
    return 1;
}

static int helper_net_validate_printable_ascii(const char *text)
{
    unsigned long i;
    unsigned char ch;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }

    for (i = 0; text[i] != '\0'; ++i) {
        ch = (unsigned char)text[i];
        if (ch < 0x20U || ch > 0x7EU) {
            return 0;
        }
    }

    return 1;
}

static int helper_net_apply_listener_setting(
    HELPER_NET_LISTENER_CONFIG *config,
    const char *text,
    unsigned long key_start,
    unsigned long key_end,
    unsigned long value_start,
    unsigned long value_end)
{
    unsigned long port_value;
    int enabled;

    if (config == 0 || text == 0) {
        return 0;
    }

    if (helper_net_text_equals_ci(text, key_start, key_end, "enabled")) {
        if (!helper_net_parse_bool(text, value_start, value_end, &enabled)) {
            return 0;
        }
        config->enabled = enabled;
        return 1;
    }

    if (helper_net_text_equals_ci(text, key_start, key_end, "bind_host")) {
        return helper_net_copy_value(text,
                                     value_start,
                                     value_end,
                                     config->bind_host,
                                     sizeof(config->bind_host));
    }

    if (helper_net_text_equals_ci(text, key_start, key_end, "port")) {
        if (!helper_net_parse_decimal(text,
                                      value_start,
                                      value_end,
                                      &port_value) ||
            port_value == 0UL ||
            port_value > 65535UL) {
            return 0;
        }

        config->port = (unsigned short)port_value;
        return 1;
    }

    return 1;
}

static int helper_net_apply_logging_setting(
    HELPER_LOG_CONFIG *config,
    const char *text,
    unsigned long key_start,
    unsigned long key_end,
    unsigned long value_start,
    unsigned long value_end)
{
    int enabled;

    if (config == 0 || text == 0) {
        return 0;
    }

    if (helper_net_text_equals_ci(text, key_start, key_end, "enabled")) {
        if (!helper_net_parse_bool(text, value_start, value_end, &enabled)) {
            return 0;
        }
        config->enabled = enabled;
        return 1;
    }

    return 1;
}

static int helper_net_section_phonebook_index(const char *text,
                                              unsigned long start,
                                              unsigned long end,
                                              unsigned int *index_out)
{
    unsigned long value;
    unsigned long index_start;

    if (text == 0 || index_out == 0) {
        return 0;
    }

    if ((end - start) <= sizeof("phonebook.") - 1U) {
        return 0;
    }

    if (!helper_net_text_equals_ci(text,
                                   start,
                                   start + (sizeof("phonebook.") - 1U),
                                   "phonebook.")) {
        return 0;
    }

    index_start = start + (sizeof("phonebook.") - 1U);
    if (!helper_net_parse_decimal(text, index_start, end, &value) ||
        value >= HELPER_PHONEBOOK_MAX_ENTRIES) {
        return 0;
    }

    *index_out = (unsigned int)value;
    return 1;
}

static int helper_net_apply_phonebook_meta(
    unsigned int *phonebook_count,
    const char *text,
    unsigned long key_start,
    unsigned long key_end,
    unsigned long value_start,
    unsigned long value_end)
{
    unsigned long value;

    if (phonebook_count == 0 || text == 0) {
        return 0;
    }

    if (!helper_net_text_equals_ci(text, key_start, key_end, "count")) {
        return 1;
    }

    if (!helper_net_parse_decimal(text, value_start, value_end, &value) ||
        value > HELPER_PHONEBOOK_MAX_ENTRIES) {
        return 0;
    }

    *phonebook_count = (unsigned int)value;
    return 1;
}

static int helper_net_apply_phonebook_entry_setting(
    HELPER_PHONEBOOK_ENTRY *entry,
    const char *text,
    unsigned long key_start,
    unsigned long key_end,
    unsigned long value_start,
    unsigned long value_end)
{
    if (entry == 0 || text == 0) {
        return 0;
    }

    if (helper_net_text_equals_ci(text, key_start, key_end, "name")) {
        return helper_net_copy_value(text,
                                     value_start,
                                     value_end,
                                     entry->name,
                                     sizeof(entry->name));
    }

    if (helper_net_text_equals_ci(text, key_start, key_end, "number")) {
        return helper_net_copy_value(text,
                                     value_start,
                                     value_end,
                                     entry->number,
                                     sizeof(entry->number));
    }

    if (helper_net_text_equals_ci(text, key_start, key_end, "target")) {
        return helper_net_copy_value(text,
                                     value_start,
                                     value_end,
                                     entry->target,
                                     sizeof(entry->target));
    }

    if (helper_net_text_equals_ci(text, key_start, key_end, "raw_mode")) {
        return helper_net_parse_bool(text,
                                     value_start,
                                     value_end,
                                     &entry->raw_mode);
    }

    return 1;
}

int helper_net_parse_target(const unsigned char *payload,
                            unsigned long payload_length,
                            HELPER_NET_TARGET *target)
{
    unsigned long i;
    unsigned long colon_index;
    unsigned long port_value;
    unsigned char ch;

    if (payload == 0 ||
        target == 0 ||
        payload_length == 0 ||
        payload_length > VMODEM_IPC_MAX_PAYLOAD) {
        return 0;
    }

    colon_index = payload_length;
    for (i = 0; i < payload_length; ++i) {
        ch = payload[i];
        if (ch < 0x21U || ch > 0x7EU) {
            return 0;
        }
        if (ch == ':') {
            if (colon_index != payload_length) {
                return 0;
            }
            colon_index = i;
        }
    }

    if (colon_index == 0 || colon_index >= payload_length - 1U) {
        return 0;
    }

    for (i = 0; i < colon_index; ++i) {
        target->host[i] = (char)payload[i];
    }
    target->host[colon_index] = '\0';

    port_value = 0;
    for (i = colon_index + 1U; i < payload_length; ++i) {
        ch = payload[i];
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

    target->port = (unsigned short)port_value;
    return 1;
}

unsigned long helper_net_map_socket_error(long error_code)
{
    switch (error_code) {
    case WSAETIMEDOUT:
        return VMODEM_CONNECT_FAIL_TIMEOUT;

    case WSAHOST_NOT_FOUND:
    case WSATRY_AGAIN:
    case WSANO_RECOVERY:
    case WSANO_DATA:
        return VMODEM_CONNECT_FAIL_DNS;

    case WSAECONNREFUSED:
        return VMODEM_CONNECT_FAIL_REFUSED;

    case WSAENETDOWN:
    case WSAENETUNREACH:
    case WSAENETRESET:
    case WSAECONNABORTED:
    case WSAECONNRESET:
    case WSAEHOSTDOWN:
    case WSAEHOSTUNREACH:
        return VMODEM_CONNECT_FAIL_NETWORK;

    case WSAEINTR:
    case WSAEBADF:
    case WSAEACCES:
    case WSAEFAULT:
    case WSAEINVAL:
    case WSAEMFILE:
    case WSAENOTSOCK:
    case WSAEDESTADDRREQ:
    case WSAEMSGSIZE:
    case WSAEPROTOTYPE:
    case WSAENOPROTOOPT:
    case WSAEPROTONOSUPPORT:
    case WSAESOCKTNOSUPPORT:
    case WSAEAFNOSUPPORT:
    case WSAEADDRINUSE:
    case WSAEADDRNOTAVAIL:
    case WSAENOBUFS:
    case WSAEISCONN:
    case WSAENOTCONN:
        return VMODEM_CONNECT_FAIL_LOCAL;

    default:
        return VMODEM_CONNECT_FAIL_NETWORK;
    }
}

const char *helper_net_fail_reason_name(unsigned long reason)
{
    switch (reason) {
    case VMODEM_CONNECT_FAIL_TIMEOUT:
        return "TIMEOUT";
    case VMODEM_CONNECT_FAIL_DNS:
        return "DNS";
    case VMODEM_CONNECT_FAIL_REFUSED:
        return "REFUSED";
    case VMODEM_CONNECT_FAIL_NETWORK:
        return "NETWORK";
    case VMODEM_CONNECT_FAIL_LOCAL:
        return "LOCAL";
    default:
        return "UNKNOWN";
    }
}

void helper_net_listener_config_init(HELPER_NET_LISTENER_CONFIG *config)
{
    if (config == 0) {
        return;
    }

    config->enabled = 0;
    memcpy(config->bind_host,
           "127.0.0.1",
           sizeof("127.0.0.1"));
    config->port = HELPER_NET_DEFAULT_LISTEN_PORT;
}

void helper_log_config_init(HELPER_LOG_CONFIG *config)
{
    if (config == 0) {
        return;
    }

    config->enabled = 0;
}

void helper_phonebook_entry_init(HELPER_PHONEBOOK_ENTRY *entry)
{
    if (entry == 0) {
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->raw_mode = 1;
}

void helper_app_config_init(HELPER_APP_CONFIG *config)
{
    unsigned int i;

    if (config == 0) {
        return;
    }

    memset(config, 0, sizeof(*config));
    helper_net_listener_config_init(&config->listener);
    helper_log_config_init(&config->logging);
    for (i = 0; i < HELPER_PHONEBOOK_MAX_ENTRIES; ++i) {
        helper_phonebook_entry_init(&config->phonebook[i]);
    }
}

int helper_net_listener_config_validate(
    const HELPER_NET_LISTENER_CONFIG *config)
{
    if (config == 0 || config->bind_host[0] == '\0' || config->port == 0U) {
        return 0;
    }

    return helper_net_validate_printable_ascii(config->bind_host);
}

int helper_log_config_validate(const HELPER_LOG_CONFIG *config)
{
    if (config == 0) {
        return 0;
    }

    if (config->enabled != 0 && config->enabled != 1) {
        return 0;
    }

    return 1;
}

int helper_phonebook_normalize_number(const char *text,
                                      char *normalized,
                                      unsigned int capacity)
{
    unsigned int out_length;
    unsigned long i;
    unsigned char ch;

    if (text == 0 || normalized == 0 || capacity < 2U) {
        return 0;
    }

    out_length = 0;
    for (i = 0; text[i] != '\0'; ++i) {
        ch = (unsigned char)text[i];
        if (ch >= '0' && ch <= '9') {
            if ((out_length + 1U) >= capacity) {
                return 0;
            }
            normalized[out_length] = (char)ch;
            ++out_length;
            continue;
        }

        if (ch == ' ' ||
            ch == '-' ||
            ch == '.' ||
            ch == '(' ||
            ch == ')' ||
            ch == '+') {
            continue;
        }

        return 0;
    }

    if (out_length == 0U) {
        return 0;
    }

    normalized[out_length] = '\0';
    return 1;
}

int helper_phonebook_entry_validate(HELPER_PHONEBOOK_ENTRY *entry)
{
    HELPER_NET_TARGET target;

    if (entry == 0 ||
        (entry->raw_mode != 0 && entry->raw_mode != 1) ||
        !helper_net_validate_printable_ascii(entry->name) ||
        !helper_net_validate_printable_ascii(entry->number) ||
        !helper_net_validate_printable_ascii(entry->target) ||
        !helper_phonebook_normalize_number(entry->number,
                                           entry->normalized_number,
                                           sizeof(entry->normalized_number))) {
        return 0;
    }

    if (!helper_net_parse_target((const unsigned char *)entry->target,
                                 (unsigned long)strlen(entry->target),
                                 &target)) {
        return 0;
    }

    return 1;
}

int helper_app_config_validate(HELPER_APP_CONFIG *config)
{
    unsigned int i;
    unsigned int j;

    if (config == 0 ||
        config->phonebook_count > HELPER_PHONEBOOK_MAX_ENTRIES ||
        !helper_net_listener_config_validate(&config->listener) ||
        !helper_log_config_validate(&config->logging)) {
        return 0;
    }

    for (i = 0; i < config->phonebook_count; ++i) {
        if (!helper_phonebook_entry_validate(&config->phonebook[i])) {
            return 0;
        }

        for (j = 0; j < i; ++j) {
            if (strcmp(config->phonebook[i].normalized_number,
                       config->phonebook[j].normalized_number) == 0) {
                return 0;
            }
        }
    }

    return 1;
}

int helper_app_config_parse_text(const char *text,
                                 unsigned long text_length,
                                 HELPER_APP_CONFIG *config)
{
    unsigned long offset;
    unsigned long line_start;
    unsigned long line_end;
    unsigned long key_start;
    unsigned long key_end;
    unsigned long value_start;
    unsigned long value_end;
    unsigned long equals_index;
    unsigned int declared_phonebook_count;
    unsigned int max_phonebook_index;
    unsigned int current_entry_index;
    int section_kind;

    if (text == 0 || config == 0) {
        return 0;
    }

    helper_app_config_init(config);
    declared_phonebook_count = 0U;
    max_phonebook_index = 0U;
    current_entry_index = 0U;
    section_kind = 0;
    offset = 0;

    while (offset < text_length) {
        line_start = offset;
        while (offset < text_length &&
               text[offset] != '\r' &&
               text[offset] != '\n') {
            ++offset;
        }
        line_end = offset;

        if (offset < text_length && text[offset] == '\r') {
            ++offset;
        }
        if (offset < text_length && text[offset] == '\n') {
            ++offset;
        }

        helper_net_trim_bounds(text, &line_start, &line_end);
        if (line_start >= line_end) {
            continue;
        }

        if (text[line_start] == ';' || text[line_start] == '#') {
            continue;
        }

        if (text[line_start] == '[' && text[line_end - 1U] == ']') {
            ++line_start;
            --line_end;
            helper_net_trim_bounds(text, &line_start, &line_end);

            if (helper_net_text_equals_ci(text,
                                          line_start,
                                          line_end,
                                          "listener")) {
                section_kind = 1;
                continue;
            }

            if (helper_net_text_equals_ci(text,
                                          line_start,
                                          line_end,
                                          "phonebook")) {
                section_kind = 2;
                continue;
            }

            if (helper_net_text_equals_ci(text,
                                          line_start,
                                          line_end,
                                          "logging")) {
                section_kind = 4;
                continue;
            }

            if (helper_net_section_phonebook_index(text,
                                                   line_start,
                                                   line_end,
                                                   &current_entry_index)) {
                section_kind = 3;
                if ((current_entry_index + 1U) > max_phonebook_index) {
                    max_phonebook_index = current_entry_index + 1U;
                }
                continue;
            }

            section_kind = 0;
            continue;
        }

        equals_index = line_start;
        while (equals_index < line_end && text[equals_index] != '=') {
            ++equals_index;
        }
        if (equals_index >= line_end) {
            continue;
        }

        key_start = line_start;
        key_end = equals_index;
        value_start = equals_index + 1U;
        value_end = line_end;
        helper_net_trim_bounds(text, &key_start, &key_end);
        helper_net_trim_bounds(text, &value_start, &value_end);
        if (key_start >= key_end) {
            continue;
        }

        switch (section_kind) {
        case 1:
            if (!helper_net_apply_listener_setting(&config->listener,
                                                   text,
                                                   key_start,
                                                   key_end,
                                                   value_start,
                                                   value_end)) {
                return 0;
            }
            break;

        case 2:
            if (!helper_net_apply_phonebook_meta(&declared_phonebook_count,
                                                 text,
                                                 key_start,
                                                 key_end,
                                                 value_start,
                                                 value_end)) {
                return 0;
            }
            break;

        case 3:
            if (!helper_net_apply_phonebook_entry_setting(
                    &config->phonebook[current_entry_index],
                    text,
                    key_start,
                    key_end,
                    value_start,
                    value_end)) {
                return 0;
            }
            break;

        case 4:
            if (!helper_net_apply_logging_setting(&config->logging,
                                                  text,
                                                  key_start,
                                                  key_end,
                                                  value_start,
                                                  value_end)) {
                return 0;
            }
            break;
        }
    }

    config->phonebook_count = declared_phonebook_count;
    if (max_phonebook_index > config->phonebook_count) {
        config->phonebook_count = max_phonebook_index;
    }

    return helper_app_config_validate(config);
}

int helper_app_config_load_file(const char *path,
                                HELPER_APP_CONFIG *config)
{
    FILE *file;
    char buffer[HELPER_APP_CONFIG_TEXT_CAP];
    size_t read_count;
    unsigned long total;

    if (path == 0 || config == 0) {
        return 0;
    }

    helper_app_config_init(config);
    file = fopen(path, "rb");
    if (file == 0) {
        return 0;
    }

    total = 0;
    while ((read_count = fread(buffer + total,
                               1U,
                               sizeof(buffer) - 1U - total,
                               file)) != 0U) {
        total = (unsigned long)(total + (unsigned long)read_count);
        if (total >= (unsigned long)(sizeof(buffer) - 1U)) {
            fclose(file);
            helper_app_config_init(config);
            return 0;
        }
    }

    fclose(file);
    buffer[total] = '\0';
    return helper_app_config_parse_text(buffer, total, config);
}

int helper_app_config_save_file(const char *path,
                                const HELPER_APP_CONFIG *config)
{
    FILE *file;
    HELPER_APP_CONFIG validated;
    unsigned int i;

    if (path == 0 || config == 0) {
        return 0;
    }

    validated = *config;
    if (!helper_app_config_validate(&validated)) {
        return 0;
    }

    file = fopen(path, "wb");
    if (file == 0) {
        return 0;
    }

    if (fprintf(file,
                "; 98Modem helper configuration\r\n"
                "; listener settings, logging, and phonebook entries\r\n"
                "\r\n"
                "[listener]\r\n"
                "enabled=%d\r\n"
                "bind_host=%s\r\n"
                "port=%u\r\n"
                "\r\n"
                "[logging]\r\n"
                "enabled=%d\r\n"
                "\r\n"
                "[phonebook]\r\n"
                "count=%u\r\n"
                "\r\n",
                validated.listener.enabled ? 1 : 0,
                validated.listener.bind_host,
                (unsigned int)validated.listener.port,
                validated.logging.enabled ? 1 : 0,
                validated.phonebook_count) < 0) {
        fclose(file);
        return 0;
    }

    for (i = 0; i < validated.phonebook_count; ++i) {
        if (fprintf(file,
                    "[phonebook.%u]\r\n"
                    "name=%s\r\n"
                    "number=%s\r\n"
                    "target=%s\r\n"
                    "raw_mode=%d\r\n"
                    "\r\n",
                    i,
                    validated.phonebook[i].name,
                    validated.phonebook[i].number,
                    validated.phonebook[i].target,
                    validated.phonebook[i].raw_mode ? 1 : 0) < 0) {
            fclose(file);
            return 0;
        }
    }

    fclose(file);
    return 1;
}

int helper_net_listener_config_parse_text(const char *text,
                                          unsigned long text_length,
                                          HELPER_NET_LISTENER_CONFIG *config)
{
    HELPER_APP_CONFIG app_config;

    if (config == 0) {
        return 0;
    }

    if (!helper_app_config_parse_text(text, text_length, &app_config)) {
        helper_net_listener_config_init(config);
        return 0;
    }

    *config = app_config.listener;
    return 1;
}

int helper_net_listener_config_load_file(const char *path,
                                         HELPER_NET_LISTENER_CONFIG *config)
{
    HELPER_APP_CONFIG app_config;

    if (config == 0) {
        return 0;
    }

    if (!helper_app_config_load_file(path, &app_config)) {
        helper_net_listener_config_init(config);
        return 0;
    }

    *config = app_config.listener;
    return 1;
}

int helper_phonebook_find_by_number(const HELPER_APP_CONFIG *config,
                                    const char *normalized_number,
                                    unsigned int *index_out)
{
    unsigned int i;

    if (index_out != 0) {
        *index_out = HELPER_PHONEBOOK_MAX_ENTRIES;
    }

    if (config == 0 || normalized_number == 0 || normalized_number[0] == '\0') {
        return 0;
    }

    for (i = 0; i < config->phonebook_count; ++i) {
        if (strcmp(config->phonebook[i].normalized_number,
                   normalized_number) == 0) {
            if (index_out != 0) {
                *index_out = i;
            }
            return 1;
        }
    }

    return 0;
}

int helper_phonebook_payload_is_phone_like(
    const unsigned char *payload,
    unsigned long payload_length,
    char *display_number,
    unsigned int display_capacity,
    char *normalized_number,
    unsigned int normalized_capacity)
{
    unsigned long i;
    unsigned int digit_count;
    unsigned int normalized_length;
    unsigned char ch;

    if (payload == 0 || payload_length == 0 || payload_length > VMODEM_IPC_MAX_PAYLOAD) {
        return 0;
    }

    if (display_number != 0) {
        if (!helper_net_copy_payload_text(payload,
                                          payload_length,
                                          display_number,
                                          display_capacity)) {
            return 0;
        }
    }

    if (normalized_number != 0) {
        if (normalized_capacity < 2U) {
            return 0;
        }
        normalized_number[0] = '\0';
    }

    digit_count = 0U;
    normalized_length = 0U;

    for (i = 0; i < payload_length; ++i) {
        ch = payload[i];
        if (ch == ':') {
            return 0;
        }

        if (ch >= '0' && ch <= '9') {
            ++digit_count;
            if (normalized_number != 0) {
                if ((normalized_length + 1U) >= normalized_capacity) {
                    return 0;
                }
                normalized_number[normalized_length] = (char)ch;
                ++normalized_length;
            }
            continue;
        }

        if (ch == ' ' ||
            ch == '-' ||
            ch == '.' ||
            ch == '(' ||
            ch == ')' ||
            ch == '+') {
            continue;
        }

        return 0;
    }

    if (digit_count == 0U) {
        return 0;
    }

    if (normalized_number != 0) {
        normalized_number[normalized_length] = '\0';
    }

    return 1;
}

int helper_phonebook_resolve_dial(const HELPER_APP_CONFIG *config,
                                  const unsigned char *payload,
                                  unsigned long payload_length,
                                  char *target_text,
                                  unsigned int target_capacity,
                                  char *normalized_number,
                                  unsigned int normalized_capacity,
                                  unsigned int *index_out)
{
    HELPER_NET_TARGET target;
    char raw_target[HELPER_PHONEBOOK_TARGET_LEN];
    unsigned int match_index;

    if (target_text != 0 && target_capacity != 0U) {
        target_text[0] = '\0';
    }
    if (normalized_number != 0 && normalized_capacity != 0U) {
        normalized_number[0] = '\0';
    }
    if (index_out != 0) {
        *index_out = HELPER_PHONEBOOK_MAX_ENTRIES;
    }
    match_index = HELPER_PHONEBOOK_MAX_ENTRIES;

    if (payload == 0 || payload_length == 0 || payload_length > VMODEM_IPC_MAX_PAYLOAD) {
        return HELPER_DIAL_RESOLUTION_INVALID;
    }

    if (helper_net_parse_target(payload, payload_length, &target)) {
        if (target_text == 0 ||
            !helper_net_copy_payload_text(payload,
                                          payload_length,
                                          raw_target,
                                          sizeof(raw_target)) ||
            (target_capacity != 0U &&
             !helper_net_copy_value(raw_target,
                                    0,
                                    (unsigned long)strlen(raw_target),
                                    target_text,
                                    target_capacity))) {
            return HELPER_DIAL_RESOLUTION_INVALID;
        }
        return HELPER_DIAL_RESOLUTION_DIRECT;
    }

    if (!helper_phonebook_payload_is_phone_like(payload,
                                                payload_length,
                                                0,
                                                0U,
                                                normalized_number,
                                                normalized_capacity)) {
        return HELPER_DIAL_RESOLUTION_INVALID;
    }

    if (config != 0 &&
        normalized_number != 0 &&
        helper_phonebook_find_by_number(config, normalized_number, &match_index)) {
        if (index_out != 0) {
            *index_out = match_index;
        }
        if (target_text == 0 ||
            !helper_net_copy_value(config->phonebook[match_index].target,
                                   0,
                                   (unsigned long)strlen(
                                       config->phonebook[match_index].target),
                                   target_text,
                                   target_capacity)) {
            return HELPER_DIAL_RESOLUTION_INVALID;
        }
        return HELPER_DIAL_RESOLUTION_PHONEBOOK;
    }

    return HELPER_DIAL_RESOLUTION_PROMPT;
}

int helper_net_can_accept_inbound(unsigned long active_session_id,
                                  unsigned long pending_session_id)
{
    return (active_session_id == 0UL && pending_session_id == 0UL) ? 1 : 0;
}

unsigned long helper_net_allocate_inbound_session(
    unsigned long *next_session_id)
{
    unsigned long session_id;

    if (next_session_id == 0) {
        return 0;
    }

    if (*next_session_id < HELPER_NET_INBOUND_SESSION_BASE) {
        *next_session_id = HELPER_NET_INBOUND_SESSION_BASE;
    }

    session_id = *next_session_id;
    ++(*next_session_id);
    if (*next_session_id < HELPER_NET_INBOUND_SESSION_BASE ||
        *next_session_id == 0UL) {
        *next_session_id = HELPER_NET_INBOUND_SESSION_BASE;
    }

    return session_id;
}

int helper_net_start_pending_inbound(unsigned long active_session_id,
                                     unsigned long *pending_session_id,
                                     unsigned long *next_session_id,
                                     unsigned long *session_id_out)
{
    unsigned long session_id;

    if (pending_session_id == 0 ||
        next_session_id == 0 ||
        session_id_out == 0 ||
        !helper_net_can_accept_inbound(active_session_id, *pending_session_id)) {
        return 0;
    }

    session_id = helper_net_allocate_inbound_session(next_session_id);
    if (session_id == 0UL) {
        return 0;
    }

    *pending_session_id = session_id;
    *session_id_out = session_id;
    return 1;
}

int helper_net_promote_pending_inbound(unsigned long *active_session_id,
                                       unsigned long *pending_session_id,
                                       unsigned long request_session_id)
{
    if (active_session_id == 0 ||
        pending_session_id == 0 ||
        request_session_id == 0UL ||
        *active_session_id != 0UL ||
        *pending_session_id != request_session_id) {
        return 0;
    }

    *active_session_id = request_session_id;
    *pending_session_id = 0UL;
    return 1;
}

int helper_net_clear_pending_inbound(unsigned long *pending_session_id,
                                     unsigned long request_session_id)
{
    if (pending_session_id == 0 ||
        request_session_id == 0UL ||
        *pending_session_id != request_session_id) {
        return 0;
    }

    *pending_session_id = 0UL;
    return 1;
}
