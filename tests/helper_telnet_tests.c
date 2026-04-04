#include <stdio.h>
#include <string.h>

#include "telnet_proto.h"

static int g_failures = 0;

static void expect_int(const char *label, long actual, long expected)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected %ld got %ld\n", label, expected, actual);
        ++g_failures;
    }
}

static void print_bytes(FILE *stream,
                        const unsigned char *bytes,
                        unsigned short length)
{
    unsigned short i;

    fputc('[', stream);
    for (i = 0; i < length; ++i) {
        if (i != 0) {
            fputc(' ', stream);
        }
        fprintf(stream, "%02X", (unsigned int)bytes[i]);
    }
    fputc(']', stream);
}

static void expect_bytes(const char *label,
                         const unsigned char *actual,
                         unsigned short actual_length,
                         const unsigned char *expected,
                         unsigned short expected_length)
{
    if (actual_length != expected_length ||
        (actual_length != 0 &&
         memcmp(actual, expected, actual_length) != 0)) {
        fprintf(stderr, "FAIL: %s\nexpected: ", label);
        print_bytes(stderr, expected, expected_length);
        fprintf(stderr, "\nactual:   ");
        print_bytes(stderr, actual, actual_length);
        fputc('\n', stderr);
        ++g_failures;
    }
}

static void expect_control_queue(const char *label,
                                 const HELPER_TELNET_SESSION *session,
                                 const unsigned char *expected,
                                 unsigned short expected_length)
{
    const unsigned char *control_data;
    unsigned short control_length;

    control_data = helper_telnet_control_data(session);
    control_length = helper_telnet_control_length(session);
    if (control_data == 0 && control_length == 0) {
        expect_bytes(label, (const unsigned char *)"", 0, expected, expected_length);
        return;
    }

    expect_bytes(label, control_data, control_length, expected, expected_length);
}

static void clear_control_queue(HELPER_TELNET_SESSION *session)
{
    helper_telnet_consume_control(session, helper_telnet_control_length(session));
}

static void test_accept_sga_and_echo(void)
{
    HELPER_TELNET_SESSION session;
    unsigned char output[HELPER_TELNET_MAX_FILTERED_PAYLOAD];
    static const unsigned char will_sga[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_WILL, HELPER_TELNET_OPT_SGA
    };
    static const unsigned char do_sga[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_DO, HELPER_TELNET_OPT_SGA
    };
    static const unsigned char will_echo[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_WILL, HELPER_TELNET_OPT_ECHO
    };
    static const unsigned char do_sga_reply[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_DO, HELPER_TELNET_OPT_SGA
    };
    static const unsigned char will_sga_reply[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_WILL, HELPER_TELNET_OPT_SGA
    };
    static const unsigned char do_echo_reply[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_DO, HELPER_TELNET_OPT_ECHO
    };
    unsigned short produced;

    helper_telnet_init(&session);

    produced = helper_telnet_filter_inbound(&session,
                                            will_sga,
                                            sizeof(will_sga),
                                            output,
                                            sizeof(output));
    expect_int("WILL SGA produced payload", produced, 0);
    expect_int("WILL SGA activates telnet mode",
               helper_telnet_is_telnet_mode(&session),
               1);
    expect_control_queue("WILL SGA -> DO SGA",
                         &session,
                         do_sga_reply,
                         sizeof(do_sga_reply));
    clear_control_queue(&session);

    helper_telnet_init(&session);
    produced = helper_telnet_filter_inbound(&session,
                                            do_sga,
                                            sizeof(do_sga),
                                            output,
                                            sizeof(output));
    expect_int("DO SGA produced payload", produced, 0);
    expect_control_queue("DO SGA -> WILL SGA",
                         &session,
                         will_sga_reply,
                         sizeof(will_sga_reply));
    clear_control_queue(&session);

    helper_telnet_init(&session);
    produced = helper_telnet_filter_inbound(&session,
                                            will_echo,
                                            sizeof(will_echo),
                                            output,
                                            sizeof(output));
    expect_int("WILL ECHO produced payload", produced, 0);
    expect_control_queue("WILL ECHO -> DO ECHO",
                         &session,
                         do_echo_reply,
                         sizeof(do_echo_reply));
}

static void test_refuse_unsupported_and_do_echo(void)
{
    HELPER_TELNET_SESSION session;
    unsigned char output[HELPER_TELNET_MAX_FILTERED_PAYLOAD];
    static const unsigned char do_echo[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_DO, HELPER_TELNET_OPT_ECHO
    };
    static const unsigned char will_other[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_WILL, 24U
    };
    static const unsigned char do_other[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_DO, 31U
    };
    static const unsigned char wont_echo_reply[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_WONT, HELPER_TELNET_OPT_ECHO
    };
    static const unsigned char dont_other_reply[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_DONT, 24U
    };
    static const unsigned char wont_other_reply[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_WONT, 31U
    };

    helper_telnet_init(&session);
    expect_int("DO ECHO payload stripped",
               helper_telnet_filter_inbound(&session,
                                            do_echo,
                                            sizeof(do_echo),
                                            output,
                                            sizeof(output)),
               0);
    expect_control_queue("DO ECHO -> WONT ECHO",
                         &session,
                         wont_echo_reply,
                         sizeof(wont_echo_reply));
    clear_control_queue(&session);

    expect_int("WILL unsupported payload stripped",
               helper_telnet_filter_inbound(&session,
                                            will_other,
                                            sizeof(will_other),
                                            output,
                                            sizeof(output)),
               0);
    expect_control_queue("WILL other -> DONT other",
                         &session,
                         dont_other_reply,
                         sizeof(dont_other_reply));
    clear_control_queue(&session);

    expect_int("DO unsupported payload stripped",
               helper_telnet_filter_inbound(&session,
                                            do_other,
                                            sizeof(do_other),
                                            output,
                                            sizeof(output)),
               0);
    expect_control_queue("DO other -> WONT other",
                         &session,
                         wont_other_reply,
                         sizeof(wont_other_reply));
}

static void test_repeat_offers_do_not_repeat_replies(void)
{
    HELPER_TELNET_SESSION session;
    unsigned char output[HELPER_TELNET_MAX_FILTERED_PAYLOAD];
    static const unsigned char will_sga[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_WILL, HELPER_TELNET_OPT_SGA
    };
    static const unsigned char do_echo[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_DO, HELPER_TELNET_OPT_ECHO
    };
    static const unsigned char empty_bytes[] = { 0 };

    helper_telnet_init(&session);

    helper_telnet_filter_inbound(&session,
                                 will_sga,
                                 sizeof(will_sga),
                                 output,
                                 sizeof(output));
    clear_control_queue(&session);

    helper_telnet_filter_inbound(&session,
                                 will_sga,
                                 sizeof(will_sga),
                                 output,
                                 sizeof(output));
    expect_control_queue("repeat WILL SGA produces no reply",
                         &session,
                         empty_bytes,
                         0);

    helper_telnet_filter_inbound(&session,
                                 do_echo,
                                 sizeof(do_echo),
                                 output,
                                 sizeof(output));
    clear_control_queue(&session);
    helper_telnet_filter_inbound(&session,
                                 do_echo,
                                 sizeof(do_echo),
                                 output,
                                 sizeof(output));
    expect_control_queue("repeat DO ECHO produces no reply",
                         &session,
                         empty_bytes,
                         0);
}

static void test_subnegotiation_and_chunk_boundaries(void)
{
    HELPER_TELNET_SESSION session;
    unsigned char output[HELPER_TELNET_MAX_FILTERED_PAYLOAD];
    static const unsigned char chunk1[] = {
        'H', 'i', HELPER_TELNET_IAC, HELPER_TELNET_SB, 24U, 1U, 2U
    };
    static const unsigned char chunk2[] = {
        3U, HELPER_TELNET_IAC, HELPER_TELNET_SE, '!'
    };
    unsigned short produced;

    helper_telnet_init(&session);

    produced = helper_telnet_filter_inbound(&session,
                                            chunk1,
                                            sizeof(chunk1),
                                            output,
                                            sizeof(output));
    expect_bytes("chunk1 strips subneg start",
                 output,
                 produced,
                 (const unsigned char *)"Hi",
                 2);
    expect_int("telnet mode after SB",
               helper_telnet_is_telnet_mode(&session),
               1);
    expect_int("ignored subneg event recorded",
               session.events[1].type,
               HELPER_TELNET_EVENT_SUBNEG_IGNORED);

    produced = helper_telnet_filter_inbound(&session,
                                            chunk2,
                                            sizeof(chunk2),
                                            output,
                                            sizeof(output));
    expect_bytes("chunk2 resumes payload after IAC SE",
                 output,
                 produced,
                 (const unsigned char *)"!",
                 1);
}

static void test_partial_sequences_and_raw_fallback(void)
{
    HELPER_TELNET_SESSION session;
    unsigned char output[HELPER_TELNET_MAX_FILTERED_PAYLOAD];
    static const unsigned char part1[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_WILL
    };
    static const unsigned char part2[] = {
        HELPER_TELNET_OPT_SGA, 'A'
    };
    static const unsigned char raw1[] = {
        'A', HELPER_TELNET_IAC
    };
    static const unsigned char raw2[] = {
        'B', 'C'
    };
    static const unsigned char raw_out2[] = {
        HELPER_TELNET_IAC, 'B', 'C'
    };
    static const unsigned char do_sga_reply[] = {
        HELPER_TELNET_IAC, HELPER_TELNET_DO, HELPER_TELNET_OPT_SGA
    };
    unsigned short produced;

    helper_telnet_init(&session);
    produced = helper_telnet_filter_inbound(&session,
                                            part1,
                                            sizeof(part1),
                                            output,
                                            sizeof(output));
    expect_int("split negotiation first half produces no payload",
               produced,
               0);
    expect_int("split negotiation has no immediate control",
               helper_telnet_control_length(&session),
               0);

    produced = helper_telnet_filter_inbound(&session,
                                            part2,
                                            sizeof(part2),
                                            output,
                                            sizeof(output));
    expect_bytes("split negotiation second half yields payload",
                 output,
                 produced,
                 (const unsigned char *)"A",
                 1);
    expect_control_queue("split WILL SGA reply",
                         &session,
                         do_sga_reply,
                         sizeof(do_sga_reply));

    helper_telnet_init(&session);
    produced = helper_telnet_filter_inbound(&session,
                                            raw1,
                                            sizeof(raw1),
                                            output,
                                            sizeof(output));
    expect_bytes("raw text before invalid IAC",
                 output,
                 produced,
                 (const unsigned char *)"A",
                 1);
    expect_int("raw session remains non-telnet",
               helper_telnet_is_telnet_mode(&session),
               0);

    produced = helper_telnet_filter_inbound(&session,
                                            raw2,
                                            sizeof(raw2),
                                            output,
                                            sizeof(output));
    expect_bytes("invalid raw IAC is preserved",
                 output,
                 produced,
                 raw_out2,
                 sizeof(raw_out2));
    expect_int("raw session still non-telnet after invalid IAC",
               helper_telnet_is_telnet_mode(&session),
               0);
}

static void test_outbound_ff_escaping_and_payload_cleanliness(void)
{
    HELPER_TELNET_SESSION session;
    unsigned char output[HELPER_TELNET_MAX_FILTERED_PAYLOAD];
    unsigned char encoded[HELPER_TELNET_MAX_ENCODED_PAYLOAD];
    static const unsigned char raw_bytes[] = { 'A', HELPER_TELNET_IAC, 'B' };
    static const unsigned char activated_bytes[] = {
        'X',
        HELPER_TELNET_IAC, HELPER_TELNET_WILL, HELPER_TELNET_OPT_SGA,
        'Y',
        HELPER_TELNET_IAC, HELPER_TELNET_IAC,
        'Z'
    };
    static const unsigned char clean_payload[] = {
        'X', 'Y', HELPER_TELNET_IAC, 'Z'
    };
    static const unsigned char escaped_bytes[] = {
        'A', HELPER_TELNET_IAC, HELPER_TELNET_IAC, 'B'
    };
    unsigned short produced;

    helper_telnet_init(&session);
    produced = helper_telnet_encode_outbound(&session,
                                             raw_bytes,
                                             sizeof(raw_bytes),
                                             encoded,
                                             sizeof(encoded));
    expect_bytes("raw outbound bytes unchanged",
                 encoded,
                 produced,
                 raw_bytes,
                 sizeof(raw_bytes));

    helper_telnet_filter_inbound(&session,
                                 activated_bytes,
                                 sizeof(activated_bytes),
                                 output,
                                 sizeof(output));
    produced = helper_telnet_filter_inbound(&session,
                                            (const unsigned char *)"",
                                            0,
                                            output,
                                            sizeof(output));
    expect_int("empty follow-up produces no payload", produced, 0);

    helper_telnet_init(&session);
    produced = helper_telnet_filter_inbound(&session,
                                            activated_bytes,
                                            sizeof(activated_bytes),
                                            output,
                                            sizeof(output));
    expect_bytes("negotiation bytes stripped from payload",
                 output,
                 produced,
                 clean_payload,
                 sizeof(clean_payload));
    expect_int("telnet mode activated for escaped outbound",
               helper_telnet_is_telnet_mode(&session),
               1);

    produced = helper_telnet_encode_outbound(&session,
                                             raw_bytes,
                                             sizeof(raw_bytes),
                                             encoded,
                                             sizeof(encoded));
    expect_bytes("telnet outbound escapes IAC",
                 encoded,
                 produced,
                 escaped_bytes,
                 sizeof(escaped_bytes));
}

int main(void)
{
    test_accept_sga_and_echo();
    test_refuse_unsupported_and_do_echo();
    test_repeat_offers_do_not_repeat_replies();
    test_subnegotiation_and_chunk_boundaries();
    test_partial_sequences_and_raw_fallback();
    test_outbound_ff_escaping_and_payload_cleanliness();

    if (g_failures != 0) {
        fprintf(stderr, "%d helper telnet tests failed\n", g_failures);
        return 1;
    }

    printf("helper_telnet_tests: all checks passed\n");
    return 0;
}
