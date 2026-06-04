# RF KM Switch — Wire Protocol (Step 3 design)

Two links carry the keystrokes/mouse motion from the wired KB+mouse to the
selected host:

1. **RP2040 console MCU → XIAO nRF52840 radio MCU** — wired UART on the PCB.
2. **XIAO (PTX) → PCA10059 (PRX) at selected machine** — 2.4 GHz ESB radio.

Both are little-endian, fixed-format, point-to-point.

---

## ESB radio frame (XIAO → PCA10059)

**Protocol:** Nordic Enhanced ShockBurst with Dynamic Payload Length (`ESB_PROTOCOL_ESB_DPL`),
2 Mbps. Hardware ack + auto-retransmit (the `selective_auto_ack` field is left at default
*true* per the upstream sample — PTX asks for an ack on every packet by setting
`payload.noack = 0`).

**Addressing.** nRF52 ESB exposes 8 pipes (0..7) but **two** base addresses:

- `base_addr_0` — used by pipe 0 only.
- `base_addr_1` — shared by pipes 1..7. Pipe address = `base_addr_1[3:0] || prefix[n]`.

We use this split deliberately. **Pipe 0 (its own base) is the OOB/control channel; pipes
1..7 (all on `base_addr_1`) carry per-machine HID traffic.** That way the seven machine
pipes share one address group, and OOB is naturally isolated.

| Pipe | Slot           | Base          | Notes                          |
|------|----------------|---------------|--------------------------------|
| 0    | OOB / control  | `base_addr_0` | future broadcast / discovery   |
| 1    | Machine 1      | `base_addr_1` | Phase 1                        |
| 2    | Machine 2      | `base_addr_1` | Phase 1                        |
| 3    | Machine 3      | `base_addr_1` | Phase 1                        |
| 4–7  | Machines 4–7   | `base_addr_1` | Phase 2                        |

Both base addresses and the prefix table are **the same on every console+receiver in
this desk's deployment** — they are the link's "network ID." Two RF-KM consoles in the
same RF range would need different base addresses (deferred — single-console scope
today).

Each receiver dongle is built per-pipe (Kconfig `CONFIG_KM_RX_PIPE=<n>`, valid values
1..7) and labelled physically. Re-pipe means re-flash; no runtime pipe negotiation.

### Payload layout (1–10 bytes, DPL)

```
byte  0      type        0x01 = KB report
                         0x02 = mouse report
                         0x10 = keep-alive (no body, used to test the link)
byte  1      seq         8-bit rolling counter, increments per TX call
byte  2..N   report      type-specific body (see below)
```

Empirical PHY-layer max for ESB DPL on nRF52 is 252 bytes; we cap the application
payload at **32 bytes** to leave slack for any future framing.

#### KB report body (type=0x01, 8 bytes)

Boot-keyboard layout, copied verbatim from `hid_keyboard_report_t`:

```
byte 2     modifier
byte 3     reserved (0)
byte 4..9  keycodes[6]
```

#### Mouse report body (type=0x02, 4 bytes)

Boot-mouse layout plus wheel:

```
byte 2     buttons       bit0=L, bit1=R, bit2=M
byte 3     dx            int8_t
byte 4     dy            int8_t
byte 5     wheel         int8_t
```

Pan/horizontal-wheel is omitted — boot mice don't have it, and the upstream Adafruit
TinyUSB host doesn't expose it without report-descriptor parsing (deferred). Hub-side
mouse parsing can re-add a byte later without breaking older PRX firmware (extra bytes
past byte 5 are ignored by the PRX).

#### Keep-alive (type=0x10, 0 bytes)

Sent every ~500 ms when no real input is happening. Lets the PRX side notice link
loss / scan for the right pipe / blink a status LED, and lets the PTX side notice
the receiver going dark (every TX_FAILED on a keep-alive = link down).

### Sequence number semantics

Pure debug aid. ESB hardware handles dedup at the link layer (matched ack), so the
PRX does **not** need `seq` for correctness. PRX logs `seq` so dropped packets
(uncommon — would imply the ESB stack lied) are visible in dev. PTX is free to
let `seq` wrap mid-burst.

### Reliability

- **TX_SUCCESS** — done; advance state.
- **TX_FAILED** — drop the payload (do not retry above ESB's own retransmit budget).
  KB modifier/keycode state is recovered on the next real report; mouse deltas are
  inherently lossy. A long stretch of TX_FAILED is reported up to the console UI as
  "link down."

---

## RP2040 ↔ XIAO UART frame

Half-duplex command/response over UART, 1 Mbaud, 8N1, no flow control. Both ends are
on the same PCB so cable noise isn't a concern; framing exists to recover from a
mid-byte reset.

```
byte 0     sync        0xAA
byte 1     sync        0x55
byte 2     len         number of bytes from `cmd` through `payload` inclusive
byte 3     cmd         see table
byte 4..N  payload     cmd-specific
byte N+1   crc8        Dallas/Maxim CRC-8 over bytes 2..N
```

### RP2040 → XIAO commands

| cmd  | name            | payload                              |
|------|-----------------|--------------------------------------|
| 0x01 | TX_KB           | 8-byte boot keyboard report          |
| 0x02 | TX_MOUSE        | 4-byte mouse report (btn,dx,dy,whl)  |
| 0x10 | SET_PIPE        | 1 byte: pipe index 0..7              |
| 0x20 | GET_STATUS      | (none)                               |

TX_KB / TX_MOUSE map 1:1 onto an ESB transmit on the currently-selected pipe.
The RP2040 frames whatever the wired HID host gave it and hands it off; the XIAO
does not interpret report contents.

### XIAO → RP2040 events

| cmd  | name            | payload                              |
|------|-----------------|--------------------------------------|
| 0x80 | LINK_STATUS     | 1 byte: 0 = down, 1 = up             |
| 0x81 | TX_RESULT       | 2 bytes: seq, result (0=ok, 1=fail)  |
| 0x82 | RX_PACKET       | reserved, future receiver-side data  |
| 0x8F | LOG             | UTF-8 string, for dev-time logs      |

LINK_STATUS is edge-triggered (sent on transition) plus once per second as a
heartbeat. The console UI uses it to drive a "no receiver" indicator.

---

## What's intentionally *not* in this version

- **Encryption / pairing.** Nordic ESB has no built-in encryption. Key material would
  need to live in flash on both ends; left out for Phase 1 (KM traffic is leaky-ish
  but the desk is a trusted environment). Revisit before any deployment that crosses
  a hostile RF environment.
- **Receiver-to-console traffic.** No KB/mouse LED state mirroring, no rumble, no
  consumer-control reports (volume keys etc.). All deferred.
- **Multi-master.** One console only. Two consoles on the same base address would
  collide; mitigation is operator policy, not protocol.
- **Runtime pipe negotiation.** Each receiver is hard-piped at flash time.
