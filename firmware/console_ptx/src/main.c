/*
 * RF KM Switch — console PTX (Step 4d: UART command parser).
 *
 * Runs on the Seeed XIAO nRF52840 Plus inside the desk console. Listens
 * on uart0 (P1.11 TX / P1.12 RX, 1 Mbaud) for the framed protocol from
 * docs/rf_protocol.md and transmits the corresponding ESB packet to the
 * currently-selected pipe.
 *
 * Until the RP2040 ↔ XIAO wires are run, a tick-driven fake-input
 * generator keeps the demo alive: each tick where the UART queue is
 * empty, the main loop builds a synthetic KB / mouse / keep-alive
 * packet aimed at `active_pipe`. Once real UART traffic shows up, real
 * commands take priority.
 *
 * Concurrency: the UART RX ISR feeds a single parser state machine and
 * drops complete TX events into k_msgq tx_q. SET_PIPE updates the
 * active_pipe byte directly (single-byte write is atomic on Cortex-M).
 * Main loop drains tx_q under the existing ESB "ready" pacing.
 *
 * Upstream provenance: nrf/samples/esb/esb_ptx (NCS v3.3.0).
 * Original copyright: (c) 2018 Nordic Semiconductor ASA, LicenseRef-Nordic-5-Clause.
 */
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <esb.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <dk_buttons_and_leds.h>

#include "km_protocol.h"

LOG_MODULE_REGISTER(console_ptx, CONFIG_CONSOLE_PTX_LOG_LEVEL);

/* --------------------------- shared state --------------------------- */

/* Which ESB pipe outbound packets target. Updated by UART SET_PIPE. */
static volatile uint8_t active_pipe = 1;

/* TX queue from UART parser → main loop. */
struct km_tx_event {
	uint8_t type;     /* KM_TYPE_KB or KM_TYPE_MOUSE */
	uint8_t body_len; /* 8 for KB, 4 for mouse */
	uint8_t body[KM_KB_BODY_LEN];
};

K_MSGQ_DEFINE(tx_q, sizeof(struct km_tx_event), 16, 4);

/* --------------------------- ESB --------------------------- */

static bool ready = true;
static struct esb_payload rx_payload;
static struct esb_payload tx_payload;

void event_handler(struct esb_evt const *event)
{
	ready = true;

	switch (event->evt_id) {
	case ESB_EVENT_TX_SUCCESS:
		break;
	case ESB_EVENT_TX_FAILED:
		break;
	case ESB_EVENT_RX_RECEIVED:
		while (esb_read_rx_payload(&rx_payload) == 0) {
			/* Receiver→console traffic not used yet. */
		}
		break;
#if IS_ENABLED(CONFIG_ESB_MPSL_TIMESLOT)
	case ESB_EVENT_TIMESLOT_FAILED:
		break;
#endif
	}
}

static int esb_initialize(void)
{
	int err;
	uint8_t base_addr_0[4] = {0xE7, 0xE7, 0xE7, 0xE7};
	uint8_t base_addr_1[4] = {0xC2, 0xC2, 0xC2, 0xC2};
	uint8_t addr_prefix[8] = {0xE7, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8};

	struct esb_config config   = ESB_DEFAULT_CONFIG;
	config.protocol            = ESB_PROTOCOL_ESB_DPL;
	config.retransmit_delay    = 600;
	config.bitrate             = ESB_BITRATE_2MBPS;
	config.event_handler       = event_handler;
	config.mode                = ESB_MODE_PTX;
	config.selective_auto_ack  = true;

	err = esb_init(&config);
	if (err) return err;

	err = esb_set_base_address_0(base_addr_0);
	if (err) return err;

	err = esb_set_base_address_1(base_addr_1);
	if (err) return err;

	return esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));
}

static void leds_update(uint8_t value)
{
	uint32_t leds_mask =
		(!(value % 8 > 0 && value % 8 <= 4) ? DK_LED1_MSK : 0) |
		(!(value % 8 > 1 && value % 8 <= 5) ? DK_LED2_MSK : 0) |
		(!(value % 8 > 2 && value % 8 <= 6) ? DK_LED3_MSK : 0) |
		(!(value % 8 > 3) ? DK_LED4_MSK : 0);

	dk_set_leds(leds_mask);
}

/* --------------------------- UART parser --------------------------- */

/* Dallas/Maxim (1-Wire) CRC-8: poly 0x31 reflected = 0x8C, init 0x00. */
static uint8_t crc8_maxim_step(uint8_t crc, uint8_t b)
{
	crc ^= b;
	for (int i = 0; i < 8; i++) {
		crc = (crc & 1) ? ((crc >> 1) ^ 0x8C) : (crc >> 1);
	}
	return crc;
}

enum parser_state {
	PS_SYNC0,
	PS_SYNC1,
	PS_LEN,
	PS_CMD,
	PS_PAYLOAD,
	PS_CRC,
};

static struct {
	enum parser_state state;
	uint8_t  len;
	uint8_t  cmd;
	uint8_t  payload[KM_UART_MAX_PAYLOAD];
	uint8_t  payload_idx;
	uint8_t  crc;
} ps;

static void parser_reset(void)
{
	ps.state = PS_SYNC0;
	ps.payload_idx = 0;
	ps.crc = 0;
}

static void parser_dispatch(void)
{
	switch (ps.cmd) {
	case KM_UART_CMD_TX_KB:
		if (ps.payload_idx == KM_KB_BODY_LEN) {
			struct km_tx_event ev = { .type = KM_TYPE_KB, .body_len = KM_KB_BODY_LEN };
			memcpy(ev.body, ps.payload, KM_KB_BODY_LEN);
			(void)k_msgq_put(&tx_q, &ev, K_NO_WAIT);
		}
		break;
	case KM_UART_CMD_TX_MOUSE:
		if (ps.payload_idx == KM_MOUSE_BODY_LEN) {
			struct km_tx_event ev = { .type = KM_TYPE_MOUSE, .body_len = KM_MOUSE_BODY_LEN };
			memcpy(ev.body, ps.payload, KM_MOUSE_BODY_LEN);
			(void)k_msgq_put(&tx_q, &ev, K_NO_WAIT);
		}
		break;
	case KM_UART_CMD_SET_PIPE:
		if (ps.payload_idx == 1 && ps.payload[0] <= 7) {
			active_pipe = ps.payload[0];
		}
		break;
	case KM_UART_CMD_GET_STATUS:
		/* Back-channel not implemented yet (Step 4d follow-up). */
		break;
	default:
		break;
	}
}

static void parser_feed(uint8_t b)
{
	switch (ps.state) {
	case PS_SYNC0:
		if (b == KM_UART_SYNC_0) ps.state = PS_SYNC1;
		break;
	case PS_SYNC1:
		if (b == KM_UART_SYNC_1) {
			ps.state = PS_LEN;
			ps.crc = 0;
		} else if (b == KM_UART_SYNC_0) {
			/* stay in SYNC1: 0xAA could be a fresh start of a sync pair */
		} else {
			parser_reset();
		}
		break;
	case PS_LEN:
		ps.len = b;
		ps.crc = crc8_maxim_step(ps.crc, b);
		if (ps.len == 0 || ps.len > 1 + KM_UART_MAX_PAYLOAD) {
			/* len=0 invalid; len > max overflows */
			parser_reset();
		} else {
			ps.state = PS_CMD;
		}
		break;
	case PS_CMD:
		ps.cmd = b;
		ps.crc = crc8_maxim_step(ps.crc, b);
		ps.payload_idx = 0;
		if (ps.len == 1) {
			ps.state = PS_CRC; /* no payload */
		} else {
			ps.state = PS_PAYLOAD;
		}
		break;
	case PS_PAYLOAD:
		ps.payload[ps.payload_idx++] = b;
		ps.crc = crc8_maxim_step(ps.crc, b);
		if (ps.payload_idx == ps.len - 1) {
			ps.state = PS_CRC;
		}
		break;
	case PS_CRC:
		if (b == ps.crc) {
			parser_dispatch();
		}
		parser_reset();
		break;
	}
}

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!uart_irq_rx_ready(dev)) {
			continue;
		}
		uint8_t buf[16];
		int n = uart_fifo_read(dev, buf, sizeof(buf));
		for (int i = 0; i < n; i++) {
			parser_feed(buf[i]);
		}
	}
}

static int uart_init(void)
{
	const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

	if (!device_is_ready(uart)) {
		return -EIO;
	}

	parser_reset();

	int err = uart_irq_callback_set(uart, uart_isr);
	if (err) return err;

	uart_irq_rx_enable(uart);
	return 0;
}

/* --------------------------- payload builders --------------------------- */

/*
 * UART-driven TX event: type + body → ESB payload. The pipe is read from
 * `active_pipe` at submit time so the most recent SET_PIPE wins.
 */
static void build_event_payload(const struct km_tx_event *ev, uint8_t seq,
				struct esb_payload *p)
{
	p->pipe   = active_pipe;
	p->noack  = false;
	p->data[0] = ev->type;
	p->data[1] = seq;
	memcpy(&p->data[2], ev->body, ev->body_len);
	p->length  = KM_HDR_LEN + ev->body_len;
}

/*
 * Tick-driven fake payload — fills in when the UART queue is empty so the
 * standalone demo still works. Round-robin testing (4c) is gone: fake data
 * goes to `active_pipe` like everything else now.
 */
static void build_fake_payload(uint8_t seq, struct esb_payload *p)
{
	p->pipe  = active_pipe;
	p->noack = false;

	if ((seq & 0x0F) == 0x0F) {
		p->data[0] = KM_TYPE_KEEPALIVE;
		p->data[1] = seq;
		p->length  = KM_KEEPALIVE_TOTAL_LEN;
		return;
	}

	if (seq & 1) {
		struct km_mouse_body body = {
			.buttons = 0,
			.dx      = 2,
			.dy      = 1,
			.wheel   = 0,
		};
		p->data[0] = KM_TYPE_MOUSE;
		p->data[1] = seq;
		memcpy(&p->data[2], &body, sizeof(body));
		p->length  = KM_MOUSE_TOTAL_LEN;
	} else {
		struct km_kb_body body = {
			.modifier = 0,
			.reserved = 0,
			.keycode  = { (uint8_t)(0x04 + (seq % 26)), 0, 0, 0, 0, 0 },
		};
		p->data[0] = KM_TYPE_KB;
		p->data[1] = seq;
		memcpy(&p->data[2], &body, sizeof(body));
		p->length  = KM_KB_TOTAL_LEN;
	}
}

/* --------------------------- self-test --------------------------- */

/*
 * Feed the parser two hand-crafted frames and check the resulting state.
 * Catches parser bugs without needing the Feather wired up. Returns 0 on
 * pass; the main loop diverts to a fast-blink loop on fail.
 */
static int parser_selftest(void)
{
	/* Frame 1: TX_KB carrying boot KB report with kc='a' (0x04). */
	uint8_t f1[] = {
		KM_UART_SYNC_0, KM_UART_SYNC_1,
		1 + KM_KB_BODY_LEN,            /* len = cmd + body */
		KM_UART_CMD_TX_KB,
		0, 0, 0x04, 0, 0, 0, 0, 0,     /* mod, res, kc[0..5] */
		0,                              /* CRC placeholder */
	};
	uint8_t c = 0;
	for (size_t i = 2; i < sizeof(f1) - 1; i++) c = crc8_maxim_step(c, f1[i]);
	f1[sizeof(f1) - 1] = c;

	parser_reset();
	for (size_t i = 0; i < sizeof(f1); i++) parser_feed(f1[i]);

	struct km_tx_event ev;
	if (k_msgq_get(&tx_q, &ev, K_NO_WAIT) != 0) return -1;
	if (ev.type != KM_TYPE_KB) return -2;
	if (ev.body_len != KM_KB_BODY_LEN) return -3;
	if (ev.body[2] != 0x04) return -4;

	/* Frame 2: SET_PIPE to 3. */
	uint8_t f2[] = {
		KM_UART_SYNC_0, KM_UART_SYNC_1,
		2,                              /* len = cmd + 1 payload byte */
		KM_UART_CMD_SET_PIPE,
		3,
		0,                              /* CRC placeholder */
	};
	c = 0;
	for (size_t i = 2; i < sizeof(f2) - 1; i++) c = crc8_maxim_step(c, f2[i]);
	f2[sizeof(f2) - 1] = c;

	uint8_t saved = active_pipe;
	active_pipe = 0;

	parser_reset();
	for (size_t i = 0; i < sizeof(f2); i++) parser_feed(f2[i]);

	if (active_pipe != 3) return -5;

	active_pipe = saved;
	k_msgq_purge(&tx_q);
	return 0;
}

/* --------------------------- main --------------------------- */

int main(void)
{
	int err;
	uint8_t seq = 0;

	LOG_INF("RF KM Switch console PTX (UART command parser)");

	err = dk_leds_init();
	if (err) return 0;

	if (parser_selftest() != 0) {
		while (1) {
			dk_set_leds(DK_LED1_MSK);
			k_msleep(100);
			dk_set_leds(0);
			k_msleep(100);
		}
	}

	err = esb_initialize();
	if (err) return 0;

	err = uart_init();
	if (err) return 0;

	LOG_INF("Init complete: pipe=%u, listening on uart0 @ 1Mbaud", active_pipe);

	while (1) {
		if (ready) {
			ready = false;
			esb_flush_tx();
			leds_update(seq);

			struct km_tx_event ev;
			if (k_msgq_get(&tx_q, &ev, K_NO_WAIT) == 0) {
				build_event_payload(&ev, seq, &tx_payload);
			} else {
				build_fake_payload(seq, &tx_payload);
			}

			err = esb_write_payload(&tx_payload);
			if (err) {
				LOG_ERR("Payload write failed, err %d", err);
			}
			seq++;
		}
		k_sleep(K_MSEC(100));
	}
}
