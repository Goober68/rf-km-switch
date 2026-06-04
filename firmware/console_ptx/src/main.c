/*
 * RF KM Switch — console PTX (Step 4b: typed payload).
 *
 * Runs on the Seeed XIAO nRF52840 Plus inside the desk console. Sends a
 * synthetic stream of KB / mouse / keep-alive packets in the
 * docs/rf_protocol.md format. No UART input yet; Step 4d wires the
 * RP2040 link in.
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
#include <string.h>
#include <dk_buttons_and_leds.h>

#include "km_protocol.h"

LOG_MODULE_REGISTER(console_ptx, CONFIG_CONSOLE_PTX_LOG_LEVEL);

static bool ready = true;
static struct esb_payload rx_payload;
static struct esb_payload tx_payload;

void event_handler(struct esb_evt const *event)
{
	ready = true;

	switch (event->evt_id) {
	case ESB_EVENT_TX_SUCCESS:
		LOG_DBG("TX SUCCESS %u attempts", event->tx_attempts);
		break;
	case ESB_EVENT_TX_FAILED:
		LOG_WRN("TX FAILED");
		break;
	case ESB_EVENT_RX_RECEIVED:
		while (esb_read_rx_payload(&rx_payload) == 0) {
			/* Receiver→console traffic not used yet. */
		}
		break;
#if IS_ENABLED(CONFIG_ESB_MPSL_TIMESLOT)
	case ESB_EVENT_TIMESLOT_FAILED:
		LOG_ERR("Timeslot error");
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

	struct esb_config config = ESB_DEFAULT_CONFIG;
	config.protocol         = ESB_PROTOCOL_ESB_DPL;
	config.retransmit_delay = 600;
	config.bitrate          = ESB_BITRATE_2MBPS;
	config.event_handler    = event_handler;
	config.mode             = ESB_MODE_PTX;
	config.selective_auto_ack = true;

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

/*
 * Build a synthetic typed payload for tick `seq`. Alternates KB / mouse,
 * with a keep-alive every 16th packet. Stand-in for real UART-driven
 * input until Step 4d.
 */
static void build_fake_payload(uint8_t seq, struct esb_payload *p)
{
	p->pipe  = 0;
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

int main(void)
{
	int err;
	uint8_t seq = 0;

	LOG_INF("RF KM Switch console PTX (typed payload)");

#if defined(CONFIG_SOC_SERIES_NRF54H)
	const struct device *dtm_uart = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));
	if (dtm_uart != NULL) {
		int ret = pm_device_runtime_get(dtm_uart);
		if (ret < 0) {
			printk("Failed to get DTM UART runtime PM: %d\n", ret);
		}
	}
#endif

	err = dk_leds_init();
	if (err) {
		LOG_ERR("LEDs init failed, err %d", err);
		return 0;
	}

	err = esb_initialize();
	if (err) {
		LOG_ERR("ESB init failed, err %d", err);
		return 0;
	}

	LOG_INF("Initialization complete, transmitting");

	while (1) {
		if (ready) {
			ready = false;
			esb_flush_tx();
			leds_update(seq);
			build_fake_payload(seq, &tx_payload);

			err = esb_write_payload(&tx_payload);
			if (err) {
				LOG_ERR("Payload write failed, err %d", err);
			}
			seq++;
		}
		k_sleep(K_MSEC(100));
	}
}
