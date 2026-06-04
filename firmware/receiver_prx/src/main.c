/*
 * RF KM Switch — receiver PRX (Step 4b: typed payload decode).
 *
 * Runs on a Nordic PCA10059 nRF52840-Dongle plugged into each target
 * host. Receives ESB payloads in the docs/rf_protocol.md format, logs
 * the decoded contents, and cycles LEDs on receive. Step 4c hard-pipes
 * the receiver to one machine via CONFIG_KM_RX_PIPE; Step 4e injects
 * HID reports into the host over USB.
 *
 * Upstream provenance: nrf/samples/esb/esb_prx (NCS v3.3.0).
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

LOG_MODULE_REGISTER(receiver_prx, CONFIG_RECEIVER_PRX_LOG_LEVEL);

static struct esb_payload rx_payload;

static void leds_update(uint8_t value)
{
	uint32_t leds_mask =
		(!(value % 8 > 0 && value % 8 <= 4) ? DK_LED1_MSK : 0) |
		(!(value % 8 > 1 && value % 8 <= 5) ? DK_LED2_MSK : 0) |
		(!(value % 8 > 2 && value % 8 <= 6) ? DK_LED3_MSK : 0) |
		(!(value % 8 > 3) ? DK_LED4_MSK : 0);

	dk_set_leds(leds_mask);
}

static void handle_payload(const struct esb_payload *p)
{
	if (p->length < KM_HDR_LEN) {
		LOG_WRN("RX too short, len=%u", p->length);
		return;
	}
	uint8_t type = p->data[0];
	uint8_t seq  = p->data[1];
	leds_update(seq);

	switch (type) {
	case KM_TYPE_KB: {
		if (p->length < KM_KB_TOTAL_LEN) {
			LOG_WRN("KB short, len=%u seq=%u", p->length, seq);
			return;
		}
		const struct km_kb_body *kb = (const struct km_kb_body *)&p->data[2];
		LOG_INF("KB seq=%u mod=0x%02x kc=%02x %02x %02x %02x %02x %02x",
			seq, kb->modifier,
			kb->keycode[0], kb->keycode[1], kb->keycode[2],
			kb->keycode[3], kb->keycode[4], kb->keycode[5]);
		break;
	}
	case KM_TYPE_MOUSE: {
		if (p->length < KM_MOUSE_TOTAL_LEN) {
			LOG_WRN("MOUSE short, len=%u seq=%u", p->length, seq);
			return;
		}
		const struct km_mouse_body *m = (const struct km_mouse_body *)&p->data[2];
		LOG_INF("MOUSE seq=%u btn=0x%02x dx=%d dy=%d wheel=%d",
			seq, m->buttons, m->dx, m->dy, m->wheel);
		break;
	}
	case KM_TYPE_KEEPALIVE:
		LOG_INF("KEEPALIVE seq=%u", seq);
		break;
	default:
		LOG_WRN("Unknown type=0x%02x seq=%u len=%u", type, seq, p->length);
		break;
	}
}

void event_handler(struct esb_evt const *event)
{
	int err;

	switch (event->evt_id) {
	case ESB_EVENT_TX_SUCCESS:
	case ESB_EVENT_TX_FAILED:
		/* Ack-with-payload (PRX→PTX) not used yet. */
		break;
	case ESB_EVENT_RX_RECEIVED:
		while ((err = esb_read_rx_payload(&rx_payload)) == 0) {
			handle_payload(&rx_payload);
		}
		if (err && err != -ENODATA) {
			LOG_ERR("Error reading rx packet, err=%d", err);
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
	config.bitrate          = ESB_BITRATE_2MBPS;
	config.mode             = ESB_MODE_PRX;
	config.event_handler    = event_handler;
	config.selective_auto_ack = true;

	err = esb_init(&config);
	if (err) return err;

	err = esb_set_base_address_0(base_addr_0);
	if (err) return err;

	err = esb_set_base_address_1(base_addr_1);
	if (err) return err;

	return esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));
}

int main(void)
{
	int err;

	LOG_INF("RF KM Switch receiver PRX (typed payload decode)");

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

	LOG_INF("Initialization complete, listening");

	err = esb_start_rx();
	if (err) {
		LOG_ERR("RX setup failed, err %d", err);
		return 0;
	}

	while (true) {
		k_sleep(K_FOREVER);
	}
}
