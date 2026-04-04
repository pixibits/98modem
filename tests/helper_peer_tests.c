#include <stdio.h>
#include <string.h>

#include "vmodem_peer.h"

static int g_failures = 0;

static void expect_int(const char *label, long actual, long expected)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: expected %ld got %ld\n", label, expected, actual);
        ++g_failures;
    }
}

static void test_build_and_parse_frames(void)
{
    unsigned char frame[HELPER_VMODEM_FRAME_LEN];
    HELPER_VMODEM_FRAME parsed;

    memset(frame, 0, sizeof(frame));
    helper_vmodem_build_frame(HELPER_VMODEM_TYPE_HELLO,
                              HELPER_VMODEM_FLAG_DELAYED_CONNECT,
                              frame,
                              sizeof(frame));

    expect_int("frame prefix valid",
               helper_vmodem_frame_prefix_valid(frame, sizeof(frame)),
               1);
    expect_int("parse hello frame",
               helper_vmodem_frame_parse(frame, sizeof(frame), &parsed),
               1);
    expect_int("parsed hello type",
               (long)parsed.type,
               (long)HELPER_VMODEM_TYPE_HELLO);
    expect_int("parsed hello flags",
               (long)parsed.flags,
               (long)HELPER_VMODEM_FLAG_DELAYED_CONNECT);

    helper_vmodem_build_frame(HELPER_VMODEM_TYPE_HELLO_ACK,
                              HELPER_VMODEM_FLAG_DELAYED_CONNECT,
                              frame,
                              sizeof(frame));
    expect_int("parse hello ack frame",
               helper_vmodem_frame_parse(frame, sizeof(frame), &parsed),
               1);
    expect_int("parsed hello ack type",
               (long)parsed.type,
               (long)HELPER_VMODEM_TYPE_HELLO_ACK);
}

static void test_partial_prefix_and_invalid_frames(void)
{
    unsigned char frame[HELPER_VMODEM_FRAME_LEN];
    HELPER_VMODEM_FRAME parsed;

    helper_vmodem_build_frame(HELPER_VMODEM_TYPE_ANSWERED, 0U, frame, sizeof(frame));

    expect_int("partial prefix valid at 1 byte",
               helper_vmodem_frame_prefix_valid(frame, 1U),
               1);
    expect_int("partial prefix valid at 5 bytes",
               helper_vmodem_frame_prefix_valid(frame, 5U),
               1);

    frame[0] = 'X';
    expect_int("bad magic rejected",
               helper_vmodem_frame_prefix_valid(frame, 1U),
               0);

    helper_vmodem_build_frame(HELPER_VMODEM_TYPE_ANSWERED, 0U, frame, sizeof(frame));
    frame[4] = 2U;
    expect_int("bad version rejected",
               helper_vmodem_frame_prefix_valid(frame, 5U),
               0);

    helper_vmodem_build_frame(HELPER_VMODEM_TYPE_ANSWERED, 0U, frame, sizeof(frame));
    frame[5] = 99U;
    expect_int("bad type rejected",
               helper_vmodem_frame_prefix_valid(frame, 6U),
               0);

    helper_vmodem_build_frame(HELPER_VMODEM_TYPE_ANSWERED, 0U, frame, sizeof(frame));
    frame[7] = 1U;
    expect_int("bad reserved rejected",
               helper_vmodem_frame_parse(frame, sizeof(frame), &parsed),
               0);
}

int main(void)
{
    test_build_and_parse_frames();
    test_partial_prefix_and_invalid_frames();

    if (g_failures != 0) {
        fprintf(stderr, "%d helper peer tests failed\n", g_failures);
        return 1;
    }

    printf("helper_peer_tests: all checks passed\n");
    return 0;
}
