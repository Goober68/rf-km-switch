# RF KM Switch — Project State

**One-line:** A desk console that takes one wired keyboard + mouse (USB-A) and relays them to
multiple host computers over 2.4 GHz RF (no video). OLED + rotary encoder for machine select.

**Status:** Parts ordered, nothing built yet. This doc is the source of truth — reconstructed
from the BOM after the original design chat was lost. Anything marked *(my call)* is open to
correction.

> **Audio control is deferred — out of scope for now.** Long-term intent is a single box that
> also handles audio routing, and the second encoder is reserved for that. Not specced here.
> One constraint to remember for later: keep any audio path **analog** (the Astro is analog),
> because a USB-host-switched audio dongle would require wired USB to every machine and defeat
> the reason KM is going RF.

---

## Integrated / production build (post-POC)

Plan once the dev-board POC is proven:

- **Single console PCB.** RP2040 + radio + 5V boost + LiPo charger + OLED + 2 encoders on one
  board. The XIAO is folded in — no separate radio board.
- **Radio = pre-certified nRF52840 module** (Raytac MDBT50Q / Fanstel BT840 class), *not* the
  bare chip. Eliminates the 2.4 GHz matching network, antenna, and RF keepout layout; avoids
  reflowing the bare nRF52840 aQFN73; carries modular FCC/IC certification.
- **Assembly:** PCB fab makes bare boards; user populates + reflows (paste + oven). Footprints
  chosen for reflow-friendliness accordingly.
- **Division of labor:** Claude delivers schematic + sourced BOM + SKiDL-generated netlist +
  layout floorplan + design rules. **Routing + Gerbers are done collaboratively in KiCad** —
  Claude does not produce a finished routed layout/Gerbers blind (esp. anything RF).
- **Receivers** stay as PCA10059 dongles for now; an integrated module+USB-A receiver board is
  a later option.

---

## Scope / phasing

- **Phase 1: 3 machines.** Prove the full RF chain end-to-end.
- **Spare:** the 4th receiver dongle is brick-insurance, not a 4th host.
- **Phase 2: all 7 machines** once proven. Addressing designed for ≥7 from the start, so
  scaling is config, not redesign.
- The 7 machines are the same set from the audio-routing project.

---

## Architecture

### Console (transmitter side)

The real keyboard and mouse plug **directly into the console via USB-A**. KM goes to the hosts
over RF, so there is no wired link to any machine.

| Role | Part | Notes |
|------|------|-------|
| Main MCU + USB host | Adafruit Feather RP2040 w/ USB (5723) | Hosts the wired KB + mouse. Native USB = device/programming; **host** is via Pico-PIO-USB on spare GPIO. |
| KB/mouse input | **2× USB-A** | Two wired devices → one PIO-USB host port behind a small **USB 2.0 hub IC** (TinyUSB host supports hubs). Console supplies their VBUS. |
| Radio co-processor | Seeed XIAO nRF52840 Plus (100003489) | 2.4 GHz ESB transmitter (PTX). RP2040 hands it HID reports over UART/SPI. |
| Display | Adafruit FeatherWing OLED 128×32 (2900) | Active-machine status + menu. 3 onboard buttons. |
| Encoder 1 | Bourns PEC11R-4215F-S0024 | **Machine select** — rotate to pick host, press to commit. |
| Encoder 2 | Bourns PEC11R-4215F-S0024 | **Reserved** for future audio control. Unused in Phase 1; firmware leaves a hook. |
| Power | Adafruit 2 Ah LiPo (2011) + USB-C | USB-C primary; LiPo for UPS / portability. Charges via the Feather. |

### Receivers (one per host)

| Role | Part | Qty |
|------|------|-----|
| HID injector | Nordic nRF52840-Dongle / PCA10059 | 3 deployed + 1 spare |

Each dongle runs ESB receive (PRX), enumerates to its host as a **composite USB HID
keyboard + mouse**, and injects reports addressed to it.

### Bench / development

| Role | Part | Notes |
|------|------|-------|
| SWD debug + flash | SEGGER J-Link EDU Mini (8.08.91) | nRF Connect SDK / Zephyr dev + RTT logging on the nRF parts. |

**Flashing access caveat:** PCA10059 dongles flash over USB DFU (nRF Connect Programmer)
without the J-Link, but SWD wants soldered access — dongle SWD is **test pads**, XIAO SWD is
**castellated/bottom pads**. RP2040 is UF2 (BOOTSEL); no J-Link needed.

---

## RF link design (to build)

**Protocol: Nordic Enhanced ShockBurst (ESB)** — proprietary 2.4 GHz, not BLE, not WiFi.
Sub-ms latency, hardware addressable pipes, hardware ack/auto-retransmit. BLE rejected
(pairing/latency); WiFi rejected (latency/overhead).

- **Addressing:** each receiver = unique pipe address; console TX only to the active machine.
  Designed for ≥7 addresses.
- **Switching:** console retargets the active address (idle hosts get zero traffic).
- **Payload (to define):** keyboard report (mod + reserved + up to 6 keycodes ≈ 8 B) and mouse
  report (buttons + dX + dY + wheel ≈ 4–5 B) packed into ESB payloads, acked, HW retransmit.
- **RP2040 ↔ XIAO link:** *(my call: start UART)*. Carries HID reports + active-pipe control.

---

## Open items / decisions

1. ~~KB/mouse attach~~ **Resolved:** wired, 2× USB-A → PIO-USB host + USB hub IC.
2. **USB hub IC** — still to select/order (KM dependency for the two USB-A ports).
3. **Mouse positioning** — *(my call: relative deltas + hard select.)* Edge-flow switching is a
   later option, not Phase 1.
4. **Power budget** — measure KB + mouse + radio + OLED vs. USB-C / 2 Ah.

---

## Next steps (build order)

1. **RP2040 USB host bring-up** — enumerate KB + mouse through the hub, print HID over serial.
2. **ESB loopback** — XIAO PTX ↔ one dongle PRX, test payload + acks.
3. **Define ESB HID payload format.**
4. **Stitch KM chain** — RP2040 host → UART → XIAO TX → dongle RX → HID inject into one host.
5. **UI** — OLED + encoder-1 machine select; bind 3 receiver addresses.
6. **Prove on 3 machines**, then extend addressing to 7 (Phase 2).

---

## BOM (as ordered — DigiKey)

| Part | DK # | Mfr # | Qty | Unit | Ext |
|------|------|-------|-----|------|-----|
| Adafruit Feather RP2040 w/ USB | 1528-5723-ND | 5723 | 1 | $17.50 | $17.50 |
| Seeed XIAO nRF52840 Plus | 1597-100003489-ND | 100003489 | 1 | $9.90 | $9.90 |
| Nordic nRF52840-Dongle (PCA10059) | 1490-1073-ND | NRF52840-DONGLE | 4 | $11.02 | $44.08 |
| FeatherWing OLED 128×32 | 1528-1547-ND | 2900 | 1 | $14.95 | $14.95 |
| LiPo 3.7 V 2 Ah | 1528-1857-ND | 2011 | 1 | $12.50 | $12.50 |
| SEGGER J-Link EDU Mini | 899-8.08.91-ND | 8.08.91 | 1 | $69.60 | $69.60 |
| Bourns rotary encoder (w/ switch) | PEC11R-4215F-S0024-ND | PEC11R-4215F-S0024 | 2 | $2.27 | $4.54 |

**Parts subtotal:** $173.07 — **still to order:** USB 2.0 hub IC for the two USB-A ports.
