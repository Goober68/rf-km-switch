// =====================================================================
//  km_usb_host_bringup.ino
//  RF KM Switch — Step 1: RP2040 USB Host bring-up (NO RADIO YET)
//
//  Goal: prove the Adafruit Feather RP2040 *with USB Type A Host* (PID 5723)
//  can enumerate a keyboard + mouse (through a USB hub) and print their HID
//  reports over the USB-C serial port. This is the input front-end of the
//  console; once this is solid we add the XIAO/ESB transmit path.
//
//  ---------------------------------------------------------------------
//  ARDUINO IDE SETTINGS (all three matter — the build will silently
//  misbehave if any are wrong):
//    1. Boards Manager: install "Raspberry Pi Pico/RP2040" by Earle
//       Philhower (arduino-pico).
//    2. Board:  Tools -> Board -> "Adafruit Feather RP2040 USB Host"
//       (this defines PIN_USB_HOST_DP / PIN_USB_HOST_DM / PIN_5V_EN).
//    3. USB Stack:  Tools -> "Adafruit TinyUSB"   (NOT "Pico SDK").
//    4. CPU Speed:  Tools -> CPU Speed -> "120 MHz"
//       (Pico-PIO-USB needs a 120 MHz clock for correct USB timing).
//
//  LIBRARIES (Library Manager):
//    - "Adafruit TinyUSB Library"
//    - "Pico PIO USB"  (by sekigon-gonnoc)
//
//  WIRING:
//    Onboard USB-A host port  ->  USB 2.0 hub  ->  keyboard + mouse.
//    (For a first smoke test you can skip the hub and plug ONE device
//     straight into the USB-A port.)
//    Power the board over USB-C; that also feeds the 5V boost for the
//    downstream devices. GPIO 18 (PIN_5V_EN) enables that boost — set HIGH.
//
//  EXPECTED OUTPUT (Serial Monitor @ 115200):
//    - "HID mounted" line per device with VID/PID and KEYBOARD/MOUSE.
//    - Key presses print as characters (newly-pressed keys only).
//    - Mouse motion prints buttons + dx/dy/wheel.
//
//  NOTES:
//    - We force BOOT protocol on keyboards/mice so reports arrive in the
//      fixed hid_keyboard_report_t / hid_mouse_report_t layout and we don't
//      have to parse report descriptors yet. Report-protocol parsing (extra
//      mouse buttons, high report-rate gaming devices) is a later refinement.
//    - The TinyUSB host stack runs on core1 (setup1/loop1); core0 is free
//      for serial / future UI work.
// =====================================================================

#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"

// Fallbacks in case the board macros aren't defined (they should be, with
// the correct board selected).
#ifndef PIN_USB_HOST_DP
  #define PIN_USB_HOST_DP 16
#endif
#ifndef PIN_5V_EN
  #define PIN_5V_EN 18
#endif
#ifndef PIN_5V_EN_STATE
  #define PIN_5V_EN_STATE 1   // drive HIGH to enable the 5V boost
#endif

Adafruit_USBH_Host USBHost;

// HID keycode -> ASCII lookup (provided by TinyUSB's hid.h).
// Index = keycode, [.][0] = unshifted, [.][1] = shifted.
static uint8_t const keycode2ascii[128][2] = { HID_KEYCODE_TO_ASCII };

// Last keyboard report, so we only print *newly* pressed keys (not held).
static hid_keyboard_report_t prev_kbd = { 0, 0, { 0 } };

// ---------------------------------------------------------------------
// core0: serial + (future) UI
// ---------------------------------------------------------------------
void setup() {
  // Enable the 5V boost to the USB-A host port BEFORE anything tries to
  // enumerate — otherwise downstream devices get no power.
  pinMode(PIN_5V_EN, OUTPUT);
  digitalWrite(PIN_5V_EN, PIN_5V_EN_STATE);

  Serial.begin(115200);
  // Give the host port a moment to come up; don't block forever waiting
  // for the serial monitor (the board runs headless on the desk later).
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 2000)) { delay(10); }

  Serial.println();
  Serial.println("=== KM USB Host bring-up ===");
  Serial.println("Waiting for USB devices on the host port...");
}

void loop() {
  // Nothing on core0 yet. Host stack is serviced on core1.
}

// ---------------------------------------------------------------------
// core1: dedicated to the TinyUSB host stack (tight PIO-USB timing)
// ---------------------------------------------------------------------
void setup1() {
  // Wait until core0 has configured clocks / pins.
  while (!Serial && (millis() < 2000)) { delay(10); }

  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = PIN_USB_HOST_DP;     // D- is assumed DP+1 (GPIO 17)
  USBHost.configure_pio_usb(1, &pio_cfg);
  USBHost.begin(1);                     // root hub port 1 = the PIO USB port
}

void loop1() {
  USBHost.task();
}

// ---------------------------------------------------------------------
// HID callbacks (called from the host stack on core1)
// ---------------------------------------------------------------------

// Device (interface) mounted.
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const* desc_report, uint16_t desc_len) {
  (void) desc_report; (void) desc_len;

  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  uint8_t const proto = tuh_hid_interface_protocol(dev_addr, instance);
  const char* pname = (proto == HID_ITF_PROTOCOL_KEYBOARD) ? "KEYBOARD"
                    : (proto == HID_ITF_PROTOCOL_MOUSE)    ? "MOUSE"
                    : "GENERIC";

  Serial.printf("HID mounted: addr=%u inst=%u  VID:PID=%04x:%04x  [%s]\r\n",
                dev_addr, instance, vid, pid, pname);

  // Force boot protocol for fixed report layout (bring-up convenience).
  if (proto == HID_ITF_PROTOCOL_KEYBOARD || proto == HID_ITF_PROTOCOL_MOUSE) {
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
  }

  // Arm the first report request; the host won't deliver reports otherwise.
  if (!tuh_hid_receive_report(dev_addr, instance)) {
    Serial.printf("  !! receive_report failed (addr=%u inst=%u)\r\n",
                  dev_addr, instance);
  }
}

// Device (interface) unplugged.
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  Serial.printf("HID unmounted: addr=%u inst=%u\r\n", dev_addr, instance);
}

// A report arrived.
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const* report, uint16_t len) {
  uint8_t const proto = tuh_hid_interface_protocol(dev_addr, instance);

  switch (proto) {
    case HID_ITF_PROTOCOL_KEYBOARD:
      if (len >= sizeof(hid_keyboard_report_t))
        process_kbd_report((hid_keyboard_report_t const*) report);
      break;
    case HID_ITF_PROTOCOL_MOUSE:
      // Boot-protocol mice send 3 bytes (buttons, x, y). hid_mouse_report_t
      // is 5 bytes (adds wheel + pan), so gating on sizeof drops every boot
      // report. Zero-fill the missing tail instead.
      if (len >= 3) {
        hid_mouse_report_t r = { 0 };
        memcpy(&r, report, (len < sizeof(r)) ? len : sizeof(r));
        process_mouse_report(&r);
      }
      break;
    default:
      break;
  }

  // Re-arm for the next report.
  if (!tuh_hid_receive_report(dev_addr, instance)) {
    Serial.printf("  !! re-arm receive_report failed (addr=%u inst=%u)\r\n",
                  dev_addr, instance);
  }
}

// ---------------------------------------------------------------------
// Report decoders
// ---------------------------------------------------------------------

static bool key_in_report(hid_keyboard_report_t const* r, uint8_t kc) {
  for (uint8_t i = 0; i < 6; i++) if (r->keycode[i] == kc) return true;
  return false;
}

void process_kbd_report(hid_keyboard_report_t const* report) {
  bool shift = report->modifier &
               (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);

  for (uint8_t i = 0; i < 6; i++) {
    uint8_t kc = report->keycode[i];
    if (kc == 0) continue;
    if (key_in_report(&prev_kbd, kc)) continue;   // already down = skip

    uint8_t ch = (kc < 128) ? keycode2ascii[kc][shift ? 1 : 0] : 0;
    if (ch) {
      Serial.printf("KEY '%c'  (kc=0x%02x mod=0x%02x)\r\n",
                    ch, kc, report->modifier);
    } else {
      Serial.printf("KEY [non-print] kc=0x%02x mod=0x%02x\r\n",
                    kc, report->modifier);
    }
  }
  prev_kbd = *report;   // remember for next-press detection
}

void process_mouse_report(hid_mouse_report_t const* report) {
  char btn[4] = "---";
  if (report->buttons & MOUSE_BUTTON_LEFT)   btn[0] = 'L';
  if (report->buttons & MOUSE_BUTTON_MIDDLE) btn[1] = 'M';
  if (report->buttons & MOUSE_BUTTON_RIGHT)  btn[2] = 'R';

  Serial.printf("MOUSE %s  dx=%4d dy=%4d wheel=%3d\r\n",
                btn, report->x, report->y, report->wheel);
}
