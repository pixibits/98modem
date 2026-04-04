#ifndef VMODEM_HELPER_VMODEM_PEER_H
#define VMODEM_HELPER_VMODEM_PEER_H

#define HELPER_VMODEM_FRAME_LEN 8U

#define HELPER_VMODEM_TYPE_HELLO     1U
#define HELPER_VMODEM_TYPE_HELLO_ACK 2U
#define HELPER_VMODEM_TYPE_ANSWERED  3U

#define HELPER_VMODEM_FLAG_DELAYED_CONNECT 0x01U

typedef struct HELPER_VMODEM_FRAME {
    unsigned char type;
    unsigned char flags;
} HELPER_VMODEM_FRAME;

void helper_vmodem_build_frame(unsigned char type,
                               unsigned char flags,
                               unsigned char *buffer,
                               unsigned short capacity);
int helper_vmodem_frame_prefix_valid(const unsigned char *buffer,
                                     unsigned short length);
int helper_vmodem_frame_parse(const unsigned char *buffer,
                              unsigned short length,
                              HELPER_VMODEM_FRAME *frame_out);

#endif /* VMODEM_HELPER_VMODEM_PEER_H */
