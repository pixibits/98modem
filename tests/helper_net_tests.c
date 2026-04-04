#include <stdio.h>
#include <string.h>

#include "ipc_shared.h"
#include "net_common.h"

static int g_failures = 0;

static void expect_int(const char *label, long actual, long expected)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected %ld got %ld\n", label, expected, actual);
        ++g_failures;
    }
}

static void expect_string(const char *label, const char *actual, const char *expected)
{
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL: %s\nexpected: [%s]\nactual:   [%s]\n",
                label,
                expected,
                actual);
        ++g_failures;
    }
}

static void set_phonebook_entry(HELPER_PHONEBOOK_ENTRY *entry,
                                const char *name,
                                const char *number,
                                const char *target,
                                int raw_mode)
{
    helper_phonebook_entry_init(entry);
    strcpy(entry->name, name);
    strcpy(entry->number, number);
    strcpy(entry->target, target);
    entry->raw_mode = raw_mode;
}

static void test_parse_target_success(void)
{
    HELPER_NET_TARGET target;

    expect_int("parse dns target",
               helper_net_parse_target(
                   (const unsigned char *)"example.com:23",
                   14UL,
                   &target),
               1);
    expect_string("parsed dns host", target.host, "example.com");
    expect_int("parsed dns port", (long)target.port, 23L);

    expect_int("parse ipv4 target",
               helper_net_parse_target(
                   (const unsigned char *)"192.168.1.10:2323",
                   17UL,
                   &target),
               1);
    expect_string("parsed ipv4 host", target.host, "192.168.1.10");
    expect_int("parsed ipv4 port", (long)target.port, 2323L);
}

static void test_parse_target_failures(void)
{
    HELPER_NET_TARGET target;

    expect_int("reject missing colon",
               helper_net_parse_target(
                   (const unsigned char *)"example.com",
                   11UL,
                   &target),
               0);
    expect_int("reject empty host",
               helper_net_parse_target(
                   (const unsigned char *)":23",
                   3UL,
                   &target),
               0);
    expect_int("reject empty port",
               helper_net_parse_target(
                   (const unsigned char *)"example.com:",
                   12UL,
                   &target),
               0);
    expect_int("reject bad port",
               helper_net_parse_target(
                   (const unsigned char *)"example.com:abc",
                   15UL,
                   &target),
               0);
    expect_int("reject second colon",
               helper_net_parse_target(
                   (const unsigned char *)"a:b:c",
                   5UL,
                   &target),
               0);
    expect_int("reject zero port",
               helper_net_parse_target(
                   (const unsigned char *)"example.com:0",
                   13UL,
                   &target),
               0);
}

static void test_socket_error_mapping(void)
{
    expect_int("timeout mapping",
               (long)helper_net_map_socket_error(10060L),
               (long)VMODEM_CONNECT_FAIL_TIMEOUT);
    expect_int("dns mapping",
               (long)helper_net_map_socket_error(11001L),
               (long)VMODEM_CONNECT_FAIL_DNS);
    expect_int("refused mapping",
               (long)helper_net_map_socket_error(10061L),
               (long)VMODEM_CONNECT_FAIL_REFUSED);
    expect_int("network mapping",
               (long)helper_net_map_socket_error(10065L),
               (long)VMODEM_CONNECT_FAIL_NETWORK);
    expect_int("local mapping",
               (long)helper_net_map_socket_error(10047L),
               (long)VMODEM_CONNECT_FAIL_LOCAL);
}

static void test_reason_names(void)
{
    expect_string("timeout reason name",
                  helper_net_fail_reason_name(VMODEM_CONNECT_FAIL_TIMEOUT),
                  "TIMEOUT");
    expect_string("dns reason name",
                  helper_net_fail_reason_name(VMODEM_CONNECT_FAIL_DNS),
                  "DNS");
    expect_string("refused reason name",
                  helper_net_fail_reason_name(VMODEM_CONNECT_FAIL_REFUSED),
                  "REFUSED");
    expect_string("network reason name",
                  helper_net_fail_reason_name(VMODEM_CONNECT_FAIL_NETWORK),
                  "NETWORK");
    expect_string("local reason name",
                  helper_net_fail_reason_name(VMODEM_CONNECT_FAIL_LOCAL),
                  "LOCAL");
}

static void test_listener_config_defaults_and_parse(void)
{
    HELPER_NET_LISTENER_CONFIG config;
    static const char ini_text[] =
        "; listener defaults overridden for test\r\n"
        "[listener]\r\n"
        "enabled=1\r\n"
        "bind_host=0.0.0.0\r\n"
        "port=2023\r\n";

    helper_net_listener_config_init(&config);
    expect_int("listener default enabled", (long)config.enabled, 0L);
    expect_string("listener default bind host", config.bind_host, "127.0.0.1");
    expect_int("listener default port",
               (long)config.port,
               (long)HELPER_NET_DEFAULT_LISTEN_PORT);

    expect_int("parse listener ini",
               helper_net_listener_config_parse_text(ini_text,
                                                    sizeof(ini_text) - 1UL,
                                                    &config),
               1);
    expect_int("parsed listener enabled", (long)config.enabled, 1L);
    expect_string("parsed listener bind host", config.bind_host, "0.0.0.0");
    expect_int("parsed listener port", (long)config.port, 2023L);
    expect_int("listener config validates",
               helper_net_listener_config_validate(&config),
               1);
}

static void test_logging_config_defaults_and_validation(void)
{
    HELPER_LOG_CONFIG config;

    helper_log_config_init(&config);
    expect_int("logging default enabled", (long)config.enabled, 0L);
    expect_int("logging default validates",
               helper_log_config_validate(&config),
               1);

    config.enabled = 1;
    expect_int("logging enabled validates",
               helper_log_config_validate(&config),
               1);

    config.enabled = 2;
    expect_int("logging invalid value rejected",
               helper_log_config_validate(&config),
               0);
}

static void test_listener_config_failures(void)
{
    HELPER_NET_LISTENER_CONFIG config;
    static const char bad_enabled[] =
        "[listener]\n"
        "enabled=maybe\n";
    static const char bad_host[] =
        "[listener]\n"
        "bind_host=\n";
    static const char bad_port[] =
        "[listener]\n"
        "port=0\n";

    expect_int("reject bad enabled",
               helper_net_listener_config_parse_text(bad_enabled,
                                                    sizeof(bad_enabled) - 1UL,
                                                    &config),
               0);
    expect_int("reject empty host",
               helper_net_listener_config_parse_text(bad_host,
                                                    sizeof(bad_host) - 1UL,
                                                    &config),
               0);
    expect_int("reject zero port",
               helper_net_listener_config_parse_text(bad_port,
                                                    sizeof(bad_port) - 1UL,
                                                    &config),
               0);
}

static void test_app_config_parse_and_validate(void)
{
    HELPER_APP_CONFIG config;
    static const char ini_text[] =
        "[listener]\r\n"
        "enabled=1\r\n"
        "bind_host=0.0.0.0\r\n"
        "port=4040\r\n"
        "\r\n"
        "[logging]\r\n"
        "enabled=1\r\n"
        "\r\n"
        "[phonebook]\r\n"
        "count=2\r\n"
        "\r\n"
        "[phonebook.0]\r\n"
        "name=Local BBS\r\n"
        "number=555-1212\r\n"
        "target=example.com:23\r\n"
        "raw_mode=0\r\n"
        "\r\n"
        "[phonebook.1]\r\n"
        "name=Echo Test\r\n"
        "number=+1 (800) 555-0000\r\n"
        "target=192.168.1.10:2323\r\n"
        "raw_mode=1\r\n";

    expect_int("parse app config",
               helper_app_config_parse_text(ini_text,
                                            sizeof(ini_text) - 1UL,
                                            &config),
               1);
    expect_int("app listener enabled", (long)config.listener.enabled, 1L);
    expect_string("app listener bind host", config.listener.bind_host, "0.0.0.0");
    expect_int("app listener port", (long)config.listener.port, 4040L);
    expect_int("app logging enabled", (long)config.logging.enabled, 1L);
    expect_int("app phonebook count", (long)config.phonebook_count, 2L);
    expect_string("entry0 name", config.phonebook[0].name, "Local BBS");
    expect_string("entry0 number", config.phonebook[0].number, "555-1212");
    expect_string("entry0 normalized", config.phonebook[0].normalized_number, "5551212");
    expect_string("entry0 target", config.phonebook[0].target, "example.com:23");
    expect_int("entry0 raw", (long)config.phonebook[0].raw_mode, 0L);
    expect_string("entry1 normalized",
                  config.phonebook[1].normalized_number,
                  "18005550000");
    expect_int("entry1 raw", (long)config.phonebook[1].raw_mode, 1L);
    expect_int("validated parsed config", helper_app_config_validate(&config), 1);
}

static void test_app_config_failures(void)
{
    HELPER_APP_CONFIG config;
    static const char duplicate_numbers[] =
        "[listener]\n"
        "enabled=1\n"
        "bind_host=127.0.0.1\n"
        "port=2323\n"
        "\n"
        "[logging]\n"
        "enabled=0\n"
        "\n"
        "[phonebook]\n"
        "count=2\n"
        "\n"
        "[phonebook.0]\n"
        "name=One\n"
        "number=555-1212\n"
        "target=example.com:23\n"
        "raw_mode=1\n"
        "\n"
        "[phonebook.1]\n"
        "name=Two\n"
        "number=(555)1212\n"
        "target=example.net:2323\n"
        "raw_mode=0\n";
    static const char invalid_target[] =
        "[listener]\n"
        "enabled=0\n"
        "bind_host=127.0.0.1\n"
        "port=2323\n"
        "\n"
        "[logging]\n"
        "enabled=1\n"
        "\n"
        "[phonebook]\n"
        "count=1\n"
        "\n"
        "[phonebook.0]\n"
        "name=Bad\n"
        "number=12345\n"
        "target=missingport\n"
        "raw_mode=1\n";
    static const char invalid_logging[] =
        "[listener]\n"
        "enabled=0\n"
        "bind_host=127.0.0.1\n"
        "port=2323\n"
        "\n"
        "[logging]\n"
        "enabled=maybe\n";

    expect_int("reject duplicate normalized numbers",
               helper_app_config_parse_text(duplicate_numbers,
                                            sizeof(duplicate_numbers) - 1UL,
                                            &config),
               0);
    expect_int("reject invalid phonebook target",
               helper_app_config_parse_text(invalid_target,
                                            sizeof(invalid_target) - 1UL,
                                            &config),
               0);
    expect_int("reject invalid logging setting",
               helper_app_config_parse_text(invalid_logging,
                                            sizeof(invalid_logging) - 1UL,
                                            &config),
               0);
}

static void test_phonebook_raw_mode_default_migration(void)
{
    HELPER_APP_CONFIG config;
    static const char ini_text[] =
        "[listener]\n"
        "enabled=0\n"
        "bind_host=127.0.0.1\n"
        "port=2323\n"
        "\n"
        "[logging]\n"
        "enabled=0\n"
        "\n"
        "[phonebook]\n"
        "count=1\n"
        "\n"
        "[phonebook.0]\n"
        "name=Legacy\n"
        "number=5551212\n"
        "target=legacy.example:23\n";

    expect_int("parse legacy phonebook entry without raw_mode",
               helper_app_config_parse_text(ini_text,
                                            sizeof(ini_text) - 1UL,
                                            &config),
               1);
    expect_int("legacy raw_mode defaults on",
               (long)config.phonebook[0].raw_mode,
               1L);
}

static void test_app_config_load_save_round_trip(void)
{
    HELPER_APP_CONFIG saved;
    HELPER_APP_CONFIG loaded;
    FILE *file;
    static const char ini_path[] = "tests-helper-vmodem.ini";

    helper_app_config_init(&saved);
    saved.listener.enabled = 1;
    strcpy(saved.listener.bind_host, "10.0.0.5");
    saved.listener.port = 7878U;
    saved.logging.enabled = 1;
    saved.phonebook_count = 2U;
    set_phonebook_entry(&saved.phonebook[0],
                        "Bulletin Board",
                        "555-1212",
                        "bbs.example.com:23",
                        0);
    set_phonebook_entry(&saved.phonebook[1],
                        "Echo",
                        "+1 (800) 555-0000",
                        "127.0.0.1:2323",
                        1);

    expect_int("save app config",
               helper_app_config_save_file(ini_path, &saved),
               1);

    file = fopen(ini_path, "rb");
    if (file == NULL) {
        fprintf(stderr, "FAIL: reopen saved app config\n");
        ++g_failures;
        return;
    }
    fclose(file);

    expect_int("load saved app config",
               helper_app_config_load_file(ini_path, &loaded),
               1);
    expect_int("round-trip listener enabled", (long)loaded.listener.enabled, 1L);
    expect_string("round-trip listener host", loaded.listener.bind_host, "10.0.0.5");
    expect_int("round-trip listener port", (long)loaded.listener.port, 7878L);
    expect_int("round-trip logging enabled", (long)loaded.logging.enabled, 1L);
    expect_int("round-trip phonebook count", (long)loaded.phonebook_count, 2L);
    expect_string("round-trip entry0 normalized",
                  loaded.phonebook[0].normalized_number,
                  "5551212");
    expect_string("round-trip entry1 normalized",
                  loaded.phonebook[1].normalized_number,
                  "18005550000");
    expect_string("round-trip entry1 target",
                  loaded.phonebook[1].target,
                  "127.0.0.1:2323");
    expect_int("round-trip entry0 raw", (long)loaded.phonebook[0].raw_mode, 0L);
    expect_int("round-trip entry1 raw", (long)loaded.phonebook[1].raw_mode, 1L);

    remove(ini_path);
}

static void test_phonebook_normalization_and_resolution(void)
{
    HELPER_APP_CONFIG config;
    char normalized[HELPER_PHONEBOOK_NORMALIZED_LEN];
    char display_number[HELPER_PHONEBOOK_NUMBER_LEN];
    char target[HELPER_PHONEBOOK_TARGET_LEN];
    unsigned int match_index;

    expect_int("normalize phone number",
               helper_phonebook_normalize_number("+1 (800) 555-0000",
                                                 normalized,
                                                 sizeof(normalized)),
               1);
    expect_string("normalized phone number", normalized, "18005550000");

    expect_int("phone-like payload",
               helper_phonebook_payload_is_phone_like(
                   (const unsigned char *)"555-1212",
                   8UL,
                   display_number,
                   sizeof(display_number),
                   normalized,
                   sizeof(normalized)),
               1);
    expect_string("phone-like display number", display_number, "555-1212");
    expect_string("phone-like normalized", normalized, "5551212");

    expect_int("reject non phone-like payload",
               helper_phonebook_payload_is_phone_like(
                   (const unsigned char *)"example.com",
                   11UL,
                   display_number,
                   sizeof(display_number),
                   normalized,
                   sizeof(normalized)),
               0);

    helper_app_config_init(&config);
    config.phonebook_count = 1U;
    set_phonebook_entry(&config.phonebook[0],
                        "Local BBS",
                        "555-1212",
                        "example.com:23",
                        1);
    expect_int("validate config for resolve", helper_app_config_validate(&config), 1);

    expect_int("resolve direct host:port",
               helper_phonebook_resolve_dial(&config,
                                             (const unsigned char *)"example.net:2323",
                                             16UL,
                                             target,
                                             sizeof(target),
                                             normalized,
                                             sizeof(normalized),
                                             &match_index),
               HELPER_DIAL_RESOLUTION_DIRECT);
    expect_string("resolved direct target", target, "example.net:2323");

    expect_int("resolve phonebook match",
               helper_phonebook_resolve_dial(&config,
                                             (const unsigned char *)"555.1212",
                                             8UL,
                                             target,
                                             sizeof(target),
                                             normalized,
                                             sizeof(normalized),
                                             &match_index),
               HELPER_DIAL_RESOLUTION_PHONEBOOK);
    expect_string("resolved phonebook target", target, "example.com:23");
    expect_int("resolved phonebook index", (long)match_index, 0L);

    target[0] = '\0';
    expect_int("resolve phonebook match without index pointer",
               helper_phonebook_resolve_dial(&config,
                                             (const unsigned char *)"5551212",
                                             7UL,
                                             target,
                                             sizeof(target),
                                             normalized,
                                             sizeof(normalized),
                                             NULL),
               HELPER_DIAL_RESOLUTION_PHONEBOOK);
    expect_string("resolved phonebook target without index pointer",
                  target,
                  "example.com:23");

    expect_int("resolve unknown phone prompts",
               helper_phonebook_resolve_dial(&config,
                                             (const unsigned char *)"+1 (800) 555-0000",
                                             17UL,
                                             target,
                                             sizeof(target),
                                             normalized,
                                             sizeof(normalized),
                                             &match_index),
               HELPER_DIAL_RESOLUTION_PROMPT);
    expect_string("prompt normalized number", normalized, "18005550000");

    expect_int("reject invalid non phone dial",
               helper_phonebook_resolve_dial(&config,
                                             (const unsigned char *)"bbs.example.com",
                                             15UL,
                                             target,
                                             sizeof(target),
                                             normalized,
                                             sizeof(normalized),
                                             &match_index),
               HELPER_DIAL_RESOLUTION_INVALID);
}

static void test_inbound_session_transitions(void)
{
    unsigned long active_session_id;
    unsigned long pending_session_id;
    unsigned long next_session_id;
    unsigned long allocated_session_id;

    active_session_id = 0UL;
    pending_session_id = 0UL;
    next_session_id = HELPER_NET_INBOUND_SESSION_BASE;
    allocated_session_id = 0UL;

    expect_int("idle can accept inbound",
               helper_net_can_accept_inbound(active_session_id,
                                             pending_session_id),
               1);
    expect_int("start pending inbound",
               helper_net_start_pending_inbound(active_session_id,
                                               &pending_session_id,
                                               &next_session_id,
                                               &allocated_session_id),
               1);
    expect_int("allocated inbound session id",
               (long)allocated_session_id,
               (long)HELPER_NET_INBOUND_SESSION_BASE);
    expect_int("pending inbound stored",
               (long)pending_session_id,
               (long)HELPER_NET_INBOUND_SESSION_BASE);
    expect_int("next inbound session advanced",
               (long)next_session_id,
               (long)(HELPER_NET_INBOUND_SESSION_BASE + 1UL));
    expect_int("reject extra inbound while pending",
               helper_net_start_pending_inbound(active_session_id,
                                               &pending_session_id,
                                               &next_session_id,
                                               &allocated_session_id),
               0);

    expect_int("promote wrong pending session rejected",
               helper_net_promote_pending_inbound(&active_session_id,
                                                 &pending_session_id,
                                                 HELPER_NET_INBOUND_SESSION_BASE + 9UL),
               0);
    expect_int("promote pending inbound to active",
               helper_net_promote_pending_inbound(&active_session_id,
                                                 &pending_session_id,
                                                 HELPER_NET_INBOUND_SESSION_BASE),
               1);
    expect_int("active inbound session after answer",
               (long)active_session_id,
               (long)HELPER_NET_INBOUND_SESSION_BASE);
    expect_int("pending cleared after answer",
               (long)pending_session_id,
               0L);
    expect_int("busy active session rejects new inbound",
               helper_net_can_accept_inbound(active_session_id,
                                             pending_session_id),
               0);

    active_session_id = 0UL;
    pending_session_id = HELPER_NET_INBOUND_SESSION_BASE + 1UL;
    expect_int("clear wrong pending session rejected",
               helper_net_clear_pending_inbound(&pending_session_id,
                                               HELPER_NET_INBOUND_SESSION_BASE),
               0);
    expect_int("clear pending session on hangup or remote close",
               helper_net_clear_pending_inbound(&pending_session_id,
                                               HELPER_NET_INBOUND_SESSION_BASE + 1UL),
               1);
    expect_int("pending cleared after close",
               (long)pending_session_id,
               0L);
}

int main(void)
{
    test_parse_target_success();
    test_parse_target_failures();
    test_socket_error_mapping();
    test_reason_names();
    test_listener_config_defaults_and_parse();
    test_logging_config_defaults_and_validation();
    test_listener_config_failures();
    test_app_config_parse_and_validate();
    test_app_config_failures();
    test_phonebook_raw_mode_default_migration();
    test_app_config_load_save_round_trip();
    test_phonebook_normalization_and_resolution();
    test_inbound_session_transitions();

    if (g_failures != 0) {
        fprintf(stderr, "%d helper net tests failed\n", g_failures);
        return 1;
    }

    printf("helper_net_tests: all checks passed\n");
    return 0;
}
