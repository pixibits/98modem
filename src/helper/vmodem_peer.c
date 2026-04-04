#include "vmodem_peer.h"

void helper_vmodem_build_frame(unsigned char type,
                               unsigned char flags,
                               unsigned char *buffer,
                               unsigned short capacity)
{
    if (buffer == 0 || capacity < HELPER_VMODEM_FRAME_LEN) {
        return;
    }

    buffer[0] = 'V';
    buffer[1] = 'M';
    buffer[2] = 'D';
    buffer[3] = 'M';
    buffer[4] = 1U;
    buffer[5] = type;
    buffer[6] = flags;
    buffer[7] = 0U;
}

int helper_vmodem_frame_prefix_valid(const unsigned char *buffer,
                                     unsigned short length)
{
    if (length > HELPER_VMODEM_FRAME_LEN) {
        return 0;
    }

    if (buffer == 0) {
        return (length == 0U) ? 1 : 0;
    }

    if (length >= 1U && buffer[0] != 'V') {
        return 0;
    }
    if (length >= 2U && buffer[1] != 'M') {
        return 0;
    }
    if (length >= 3U && buffer[2] != 'D') {
        return 0;
    }
    if (length >= 4U && buffer[3] != 'M') {
        return 0;
    }
    if (length >= 5U && buffer[4] != 1U) {
        return 0;
    }
    if (length >= 6U &&
        buffer[5] != HELPER_VMODEM_TYPE_HELLO &&
        buffer[5] != HELPER_VMODEM_TYPE_HELLO_ACK &&
        buffer[5] != HELPER_VMODEM_TYPE_ANSWERED) {
        return 0;
    }
    if (length >= 8U && buffer[7] != 0U) {
        return 0;
    }

    return 1;
}

int helper_vmodem_frame_parse(const unsigned char *buffer,
                              unsigned short length,
                              HELPER_VMODEM_FRAME *frame_out)
{
    if (frame_out != 0) {
        frame_out->type = 0U;
        frame_out->flags = 0U;
    }

    if (buffer == 0 ||
        frame_out == 0 ||
        length != HELPER_VMODEM_FRAME_LEN ||
        !helper_vmodem_frame_prefix_valid(buffer, length)) {
        return 0;
    }

    frame_out->type = buffer[5];
    frame_out->flags = buffer[6];
    return 1;
}
