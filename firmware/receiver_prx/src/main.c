/*
 * RF KM Switch — receiver PRX (Step 4e: USB HID composite, retry).
 *
 * Runs on a Nordic PCA10059 nRF52840-Dongle plugged into each target
 * host. Listens on its assigned ESB pipe (CONFIG_KM_RX_PIPE), decodes
 * the typed payload from docs/rf_protocol.md, and injects the report
 * to the host as USB HID — boot keyboard for KB packets, 3-button
 * mouse-with-wheel for MOUSE packets.
 *
 * Architecture: ESB events arrive in interrupt context, but
 * hid_device_submit_report needs thread context. event_handler pushes
 * decoded reports onto a k_msgq; the main thread drains the queue and
 * does the USB submit.
 *
 * Observability: with the Zephyr console disabled (so it doesn't
 * compete for the USB device controller via the board's CDC ACM
 * auto-init), LOG_* calls go nowhere. The four onboard LEDs are
 * staged instead:
 *   LED1 green0  on  =  main reached
 *   LED2 red     on  =  ESB initialised
 *   LED3 green1  on  =  USBD enabled
 *   LED4 blue    on  =  RX armed (=> main loop running)
 * If a stage failed, the corresponding LED stays off.
 *
 * Upstream provenance: nrf/samples/esb/esb_prx and
 * zephyr/samples/subsys/usb/hid-{keyboard,mouse} (NCS v3.3.0).
 */
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <esb.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <string.h>
#include <dk_buttons_and_leds.h>

#include "km_protocol.h"
#include "sample_usbd.h"

LOG_MODULE_REGISTER(receiver_prx, CONFIG_RECEIVER_PRX_LOG_LEVEL);

/* --------------------------- HID setup --------------------------- */

static const uint8_t kb_report_desc[]    = HID_KEYBOARD_REPORT_DESC();
static const uint8_t mouse_report_desc[] = HID_MOUSE_REPORT_DESC(3);

UDC_STATIC_BUF_DEFINE(kb_report,    KM_KB_BODY_LEN);
UDC_STATIC_BUF_DEFINE(mouse_report, KM_MOUSE_BODY_LEN);

static const struct device *kb_dev;
static const struct device *mouse_dev;
static volatile bool kb_ready;
static volatile bool mouse_ready;

static void kb_iface_ready(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);
	kb_ready = ready;
}

static void mouse_iface_ready(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);
	mouse_ready = ready;
}

static int hid_get_report(const struct device *dev,
			  const uint8_t type, const uint8_t id,
			  const uint16_t len, uint8_t *const buf)
{
	ARG_UNUSED(dev); ARG_UNUSED(type); ARG_UNUSED(id);
	ARG_UNUSED(len); ARG_UNUSED(buf);
	return 0;
}

/*
 * Boot-capable HID devices (protocol-code = "keyboard" / "mouse" in DT) carry
 * a non-zero bInterfaceSubClass, and hid_dev_register refuses to register
 * them without a set_protocol callback. We don't act on the protocol switch
 * — we always send report-protocol-shaped data, hosts that put us in boot
 * protocol read the leading bytes which are byte-compatible.
 */
static void hid_set_protocol(const struct device *dev, const uint8_t proto)
{
	ARG_UNUSED(dev); ARG_UNUSED(proto);
}

static struct hid_device_ops kb_ops = {
	.iface_ready  = kb_iface_ready,
	.get_report   = hid_get_report,
	.set_protocol = hid_set_protocol,
};

static struct hid_device_ops mouse_ops = {
	.iface_ready  = mouse_iface_ready,
	.get_report   = hid_get_report,
	.set_protocol = hid_set_protocol,
};

/* --------------------------- ISR→thread queue --------------------------- */

struct km_event {
	uint8_t type;
	uint8_t seq;
	uint8_t body[KM_KB_BODY_LEN]; /* large enough for any report body */
};

K_MSGQ_DEFINE(km_q, sizeof(struct km_event), 16, 4);

/* --------------------------- ESB --------------------------- */

static struct esb_payload rx_payload;

static void handle_payload(const struct esb_payload *p)
{
	if (p->length < KM_HDR_LEN) {
		return;
	}

	struct km_event ev = { .type = p->data[0], .seq = p->data[1] };

	switch (ev.type) {
	case KM_TYPE_KB:
		if (p->length < KM_KB_TOTAL_LEN) return;
		memcpy(ev.body, &p->data[2], KM_KB_BODY_LEN);
		break;
	case KM_TYPE_MOUSE:
		if (p->length < KM_MOUSE_TOTAL_LEN) return;
		memcpy(ev.body, &p->data[2], KM_MOUSE_BODY_LEN);
		break;
	case KM_TYPE_KEEPALIVE:
		return;
	default:
		return;
	}

	(void)k_msgq_put(&km_q, &ev, K_NO_WAIT);
}

static void event_handler(struct esb_evt const *event)
{
	int err;

	switch (event->evt_id) {
	case ESB_EVENT_TX_SUCCESS:
	case ESB_EVENT_TX_FAILED:
		break;
	case ESB_EVENT_RX_RECEIVED:
		while ((err = esb_read_rx_payload(&rx_payload)) == 0) {
			handle_payload(&rx_payload);
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
	config.bitrate             = ESB_BITRATE_2MBPS;
	config.mode                = ESB_MODE_PRX;
	config.event_handler       = event_handler;
	config.selective_auto_ack  = true;

	err = esb_init(&config);
	if (err) return err;

	err = esb_set_base_address_0(base_addr_0);
	if (err) return err;

	err = esb_set_base_address_1(base_addr_1);
	if (err) return err;

	err = esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));
	if (err) return err;

	return esb_enable_pipes(BIT(CONFIG_KM_RX_PIPE));
}

/* --------------------------- USB HID bring-up --------------------------- */

/*
 * Sets `*fail_step` to 1..5 indicating which call failed:
 *   1 = device_is_ready (HID DT nodes)
 *   2 = hid_device_register kb
 *   3 = hid_device_register mouse
 *   4 = sample_usbd_init_device (any of the USBD setup steps)
 *   5 = usbd_enable
 * Returns 0 on full success.
 */
static int usb_hid_init(int *fail_step)
{
	struct usbd_context *ctx;
	int err;

	*fail_step = 1;
	kb_dev    = DEVICE_DT_GET(DT_NODELABEL(hid_kb));
	mouse_dev = DEVICE_DT_GET(DT_NODELABEL(hid_mouse));

	if (!device_is_ready(kb_dev) || !device_is_ready(mouse_dev)) {
		return -EIO;
	}

	*fail_step = 2;
	err = hid_device_register(kb_dev, kb_report_desc, sizeof(kb_report_desc),
				  &kb_ops);
	if (err) return err;

	*fail_step = 3;
	err = hid_device_register(mouse_dev, mouse_report_desc,
				  sizeof(mouse_report_desc), &mouse_ops);
	if (err) return err;

	*fail_step = 4;
	ctx = sample_usbd_init_device(NULL);
	if (!ctx) return -ENODEV;

	*fail_step = 5;
	err = usbd_enable(ctx);
	if (err) return err;

	*fail_step = 0;
	return 0;
}

static void blink_forever(uint8_t led, int count)
{
	while (true) {
		for (int i = 0; i < count; i++) {
			dk_set_led_on(led);
			k_msleep(150);
			dk_set_led_off(led);
			k_msleep(150);
		}
		k_msleep(1500);
	}
}

/* --------------------------- main --------------------------- */

int main(void)
{
	int err;

	err = dk_leds_init();
	if (err) {
		/* Can't even light an LED to complain. Hang. */
		while (1) k_msleep(1000);
	}
	dk_set_led_on(DK_LED1);  /* stage 1: main reached + LEDs OK */

	err = esb_initialize();
	if (err) return 0;
	dk_set_led_on(DK_LED2);  /* stage 2: ESB initialised */

	int hid_step;
	err = usb_hid_init(&hid_step);
	if (err) {
		/* hid_step 1..5 says which call failed; blink LED3 that many times. */
		blink_forever(DK_LED3, hid_step);
	}
	dk_set_led_on(DK_LED3);  /* stage 3: USBD enabled */

	err = esb_start_rx();
	if (err) return 0;
	dk_set_led_on(DK_LED4);  /* stage 4: RX armed */

	/*
	 * If the RF link goes silent (PTX out of range, console powered down,
	 * crashed wired KB), the host would otherwise see the last submitted
	 * KB report as "this key is held" and start auto-repeating. Watchdog:
	 * if no packet arrives within LINK_IDLE_MS, submit zeroed reports to
	 * release any held key / clear any phantom mouse state.
	 */
	const k_timeout_t LINK_IDLE = K_MSEC(250);
	bool released = true;
	struct km_event ev;
	while (true) {
		int ret = k_msgq_get(&km_q, &ev, LINK_IDLE);

		if (ret == -EAGAIN) {
			if (!released) {
				if (kb_ready) {
					memset(kb_report, 0, KM_KB_BODY_LEN);
					(void)hid_device_submit_report(kb_dev,
						KM_KB_BODY_LEN, kb_report);
				}
				if (mouse_ready) {
					memset(mouse_report, 0, KM_MOUSE_BODY_LEN);
					(void)hid_device_submit_report(mouse_dev,
						KM_MOUSE_BODY_LEN, mouse_report);
				}
				released = true;
			}
			continue;
		}

		released = false;

		switch (ev.type) {
		case KM_TYPE_KB:
			if (!kb_ready) break;
			memcpy(kb_report, ev.body, KM_KB_BODY_LEN);
			(void)hid_device_submit_report(kb_dev, KM_KB_BODY_LEN,
						       kb_report);
			break;

		case KM_TYPE_MOUSE:
			if (!mouse_ready) break;
			memcpy(mouse_report, ev.body, KM_MOUSE_BODY_LEN);
			(void)hid_device_submit_report(mouse_dev, KM_MOUSE_BODY_LEN,
						       mouse_report);
			break;
		}
	}
}
