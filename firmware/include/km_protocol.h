/*
 * RF KM Switch — over-the-air payload format.
 *
 * Shared by firmware/console_ptx (XIAO) and firmware/receiver_prx (PCA10059).
 * See docs/rf_protocol.md for the protocol spec.
 */

#ifndef KM_PROTOCOL_H
#define KM_PROTOCOL_H

#include <stdint.h>

/* Soft cap on application payload (well under ESB DPL's 252 B limit). */
#define KM_PAYLOAD_MAX 32

/* Header bytes: type, seq. */
#define KM_HDR_LEN 2

/* Type discriminator (byte 0 of payload). */
enum km_type {
	KM_TYPE_KB        = 0x01,
	KM_TYPE_MOUSE     = 0x02,
	KM_TYPE_KEEPALIVE = 0x10,
};

/* Boot-keyboard report body (bytes 2..9, total 8 B). */
struct km_kb_body {
	uint8_t modifier;
	uint8_t reserved;
	uint8_t keycode[6];
} __attribute__((packed));
#define KM_KB_BODY_LEN  sizeof(struct km_kb_body)
#define KM_KB_TOTAL_LEN (KM_HDR_LEN + KM_KB_BODY_LEN)

/* Mouse report body (bytes 2..5, total 4 B). No pan — see docs/rf_protocol.md. */
struct km_mouse_body {
	uint8_t buttons;
	int8_t  dx;
	int8_t  dy;
	int8_t  wheel;
} __attribute__((packed));
#define KM_MOUSE_BODY_LEN  sizeof(struct km_mouse_body)
#define KM_MOUSE_TOTAL_LEN (KM_HDR_LEN + KM_MOUSE_BODY_LEN)

/* Keep-alive has no body. */
#define KM_KEEPALIVE_TOTAL_LEN KM_HDR_LEN

#endif /* KM_PROTOCOL_H */
