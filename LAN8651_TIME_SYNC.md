# Time synchronization on the 10BASE-T1S bus (LAN8651 wall clock)

> **What this is.** The LAN8651 MAC-PHY on `eth0` contains a **94-bit wall clock** (a
> gPTP/1588 Time Stamp Unit) together with hardware timestamping of transmitted and
> received frames. Together with `Sync`/`Follow_Up` messages and a **software servo** on
> the host, that clock can be locked to a grandmaster elsewhere on the segment.
>
> This document collects what the silicon offers, which registers reach it, what the
> **measured state of this board** is, the four constraints that matter on a 10BASE-T1S
> multidrop segment, and — honestly — **what this firmware already has and what is still
> missing** for a working follower.
>
> Sections 3 and 4 required no firmware change: everything there is reachable today with the
> existing `lan_read` / `lan_write` / `lan_rmw` commands. Section 11 goes one step further and
> lays out the **plan for running the PTP master on this bridge**, broadcasting `Sync` +
> `Follow_Up` one-way on the T1S segment so that any number of followers — built as a separate
> project — can steer their wall clocks from it, with no delay-request traffic at all. It is based
> on an existing, measured implementation of both roles.

---

## Contents

- [1. What the silicon provides](#1-what-the-silicon-provides)
- [2. Register map](#2-register-map)
- [3. Reading the clock from the console](#3-reading-the-clock-from-the-console)
  - [3.1 A worked example, measured on this board](#31-a-worked-example-measured-on-this-board)
  - [3.2 Decoding `OA_CONFIG0`](#32-decoding-oa_config0)
- [4. Timestamping frames](#4-timestamping-frames)
- [5. Why it is two-step Sync + Follow_Up](#5-why-it-is-two-step-sync--follow_up)
- [6. Four constraints specific to a multidrop segment](#6-four-constraints-specific-to-a-multidrop-segment)
- [7. The software servo](#7-the-software-servo)
- [8. Judging the result without a PTP analyser](#8-judging-the-result-without-a-ptp-analyser)
- [9. What this firmware has today](#9-what-this-firmware-has-today)
- [10. Enabling frame timestamps: two ways in](#10-enabling-frame-timestamps-two-ways-in)
  - [10.1 From application code, at runtime — preferred](#101-from-application-code-at-runtime--preferred)
  - [10.2 In the driver init table — works, but pays a price](#102-in-the-driver-init-table--works-but-pays-a-price)
  - [10.3 Two traps that apply either way](#103-two-traps-that-apply-either-way)
- [11. Master on the bridge, followers listening one-way](#11-master-on-the-bridge-followers-listening-one-way)
  - [11.1 Prior art: both roles already exist and are measured](#111-prior-art-both-roles-already-exist-and-are-measured)
  - [11.2 What this bridge already brings](#112-what-this-bridge-already-brings)
  - [11.3 The split](#113-the-split)
  - [11.4 One-way only: what that costs, and what it does not](#114-one-way-only-what-that-costs-and-what-it-does-not)
  - [11.5 Three things that are different on a bridge](#115-three-things-that-are-different-on-a-bridge)
  - [11.6 Staging: the bridge side never needs a driver patch](#116-staging-the-bridge-side-never-needs-a-driver-patch)
  - [11.7 Two caveats about the reference implementation](#117-two-caveats-about-the-reference-implementation)
- [12. Where the numbers come from](#12-where-the-numbers-come-from)

---

## 1. What the silicon provides

The Time Stamp Unit (TSU) holds a **94-bit counter**:

| Field | Bits | Width | Register-accessible |
|---|---|---|---|
| Seconds | [93:46] | 48 | yes (`MAC_TSH` + `MAC_TSL`) |
| Nanoseconds | [45:16] | 30 | yes (`MAC_TN`), rolls over at 1e9 |
| Sub-nanoseconds | [15:0] | 16 | **no** |

It is clocked from the 25.0 MHz oscillator, so the nominal step is **40 ns per tick** —
which is why `MAC_TI` reads `0x28` on an untouched device.

There are **three different ways to steer that clock**, and telling them apart is the whole
point of the servo design:

| Way | Register(s) | When to use it |
|---|---|---|
| Set it absolutely | `MAC_TSH`, `MAC_TSL`, `MAC_TN` | **once**, at startup |
| One-shot offset correction | `MAC_TA` (30-bit magnitude in ns, bit 31 = sign) | coarse steps while pulling in |
| Rate (frequency) correction | `MAC_TI` (ns) + `MAC_TISUBN` (sub-ns) | steady-state operation |

Beyond the clock itself the device offers **event capture** (up to 4 sources, timestamps in
`EC_Timestamp0..15`, burst-readable through the SPI auto-increment), **4 event generators**
on DIOA0-3, and a **1PPS output** on DIOA4 with a pulse width between 640 ns and 20.48 us,
routed through `PADCTRL`. Section 8 explains why that last one matters.

---

## 2. Register map

Addresses follow the usual encoding of this project — **upper 16 bits = MMS, lower 16 bits =
register offset** — so every one of them is reachable with `lan_read` / `lan_write` right
now. See `CLAUDE.md` section 3 for the encoding and its caveats.

### MMS 1 — MAC, the clock itself

| Address | Name | Contents |
|---|---|---|
| `0x0001006F` | `MAC_TISUBN` | Timer increment, sub-ns. Bits 31:24 = `LSBTIR[7:0]`, bits 15:0 = `MSBTIR[15:0]` |
| `0x00010070` | `MAC_TSH` | Seconds, bits 15:0 = `TCS[47:32]` |
| `0x00010074` | `MAC_TSL` | Seconds, bits 31:0 = `TCS[31:0]` |
| `0x00010075` | `MAC_TN` | Nanoseconds, bits 29:0 = `TNS[29:0]` |
| `0x00010076` | `MAC_TA` | Timer adjust. Bit 31 = `ADJ` (sign), bits 29:0 = ns to add/subtract |
| `0x00010077` | `MAC_TI` | Timer increment in ns per clock cycle. **Nominal `0x28`** |

### MMS 0 — OPEN Alliance standard registers

| Address | Name | Contents |
|---|---|---|
| `0x00000004` | `OA_CONFIG0` | `FTSE` = bit 7 (timestamping on/off), `FTSS` = bit 6 (0 = 32-bit, 1 = 64-bit) |
| `0x00000008` | `OA_STATUS0` | `TTSCAA` / `TTSCAB` / `TTSCAC` = bits 8 / 9 / 10 (capture available A/B/C) |
| `0x00000010` `0x00000011` | `TTSCAH` `TTSCAL` | Transmit timestamp capture A, high / low |
| `0x00000012` `0x00000013` | `TTSCBH` `TTSCBL` | Transmit timestamp capture B |
| `0x00000014` `0x00000015` | `TTSCCH` `TTSCCL` | Transmit timestamp capture C |

`OA_STATUS1` additionally carries `TTSCOFA/B/C` (overflow — a new capture arrived before the
previous one was read; the stored value is **not** overwritten) and `TTSCMA/B/C` (missed).

### MMS 4 — PHY vendor specific, the packet pattern matchers

There are **two independent matchers**, one per direction. Both are needed: the receive one
to stamp arriving `Sync` frames, the transmit one to stamp departing ones.

| Address | Name |
|---|---|
| `0x00040050` | `RXMCTL` — receive match control |
| `0x00040051` `0x00040052` | `RXMPATH` / `RXMPATL` — pattern, high / low |
| `0x00040053` `0x00040054` | `RXMMSKH` / `RXMMSKL` — mask, high / low |
| `0x00040055` | `RXMLOC` — match location |
| `0x00040059` | `RXMDLY` — matched packet delay |
| `0x00040040` | `TXMCTL` — transmit match control |
| `0x00040041` `0x00040042` | `TXMPATH` / `TXMPATL` — pattern, high / low |
| `0x00040043` `0x00040044` | `TXMMSKH` / `TXMMSKL` — mask, high / low |
| `0x00040045` | `TXMLOC` — match location |

`TXMCTL` bits that matter: `TXPMDET` = `0x0080` (read-**clear**, pattern detected),
`MACTXTSE` = `0x0004`, `TXME` = `0x0002` (match enable). The two enables are mutually exclusive by
datasheet note; `MACTXTSE` stamps in the MAC and asserts a logical collision, which is the wrong
side of the PLCA buffer, so `TXME` is the one to use.

**This needs no configuring on this firmware, and there is nothing to arm per `Sync`** — verified
2026-08-11. The driver's init table (`drv_lan865x_api.c`, lines ~1683-1690) already writes
`TXMMSKH = 0xFF`, `TXMMSKL = 0xFFFF`, `TXMLOC = 0` and `TXMCTL = 0x0002`, plus the receive
equivalents. Per datasheet 4.5.2.2 that is the documented default of every Microchip driver: **mask
bits mean "ignore"**, so all-ones matches any pattern and every frame is matched at the SFD. A frame
is then stamped exactly when its data header carries a non-zero `TSC`, which is why ordinary stack
traffic is unaffected. `TXME` is R/W and does not self-clear, so per-`Sync` re-arming would be
pointless work on the SPI bus.

If a PTP-only matcher is ever wanted, the datasheet's worked example is `TXMLOC = 30` (the location
is the first **nibble after** the pattern, and the pattern sits in nibbles 24..29),
`TXMPATH = 0x88`, `TXMPATL = 0xF710`, masks 0. Note the last nibble: the 24-bit pattern covers the
EtherType **and** the `transportSpecific|messageType` byte, `0x10` being 802.1AS's
`transportSpecific = 1` with `messageType = 0` (Sync). Masking the low nibble
(`TXMMSKL = 0x000F`) is what the datasheet suggests for accepting several message types. A pattern
of `0x88F700` is the equivalent for `transportSpecific = 0`, which is what
[ptp_gm.c](firmware/src/ptp_gm.c) sends.

Why the matchers are not optional is in section 6.

### MMS 10 (0x0A) — misc, the outputs

| Address | Name | Contents |
|---|---|---|
| `0x000A0088` | `PADCTRL` | pad routing. For 1PPS on DIOA4: set bit 8, clear bit 9 — as an RMW that is mask `0x300`, value `0x100` |
| `0x000A0239` | `PPSCTL` | 1PPS control. The reference grandmaster writes `0x7D` |
| `0x000A0220` | `PACTRL` | event generator / capture control |
| `0x000A0221` .. `0x000A0226` | `EG0STNS`, `EG0STSECL`, `EG0STSECH`, `EG0PW`, `EG0IT`, `EG0CTL` | event generator 0: start ns, start seconds low / high, pulse width, interval, control |

`PPSCTL` and `PADCTRL` together are what section 8 relies on — without the `PADCTRL` bit the
pulse is generated internally and never reaches the pin.

---

## 3. Reading the clock from the console

The single most useful one-liner — if this does not read `0x00000028`, stop and find out why
before doing anything else:

```
lan_read 0x00010077
```

The full survey, safe (read-only) and paste-friendly:

```
lan_read 0x00010077
lan_read 0x0001006F
lan_read 0x00010075
lan_read 0x00010075
lan_read 0x00010074
lan_read 0x00000004
```

Reading `MAC_TN` **twice** is the actual liveness test: if the value does not change, the
counter is not running.

> **Console note.** Copying a command out of a rendered document into a terminal can insert
> a non-breaking space (U+00A0) where a plain blank belongs. The echoed line then looks
> perfectly correct and the parser still answers `*** Command Processor: unknown command.
> ***`. That is why the blocks in this document contain **one bare command per line, no
> trailing comments**. The reliable path is the repo's own tool, which takes the string from
> the command line instead of the clipboard:
>
> ```
> python cli.py --port COM8 --read 1 "lan_read 0x00010077"
> ```
>
> If a paste still drops characters, set a transmit delay in the terminal (Tera Term:
> Setup, Serial port, 1 ms/char and 10 ms/line) — at 115200 with no flow control a pasted
> block can outrun the console.

### 3.1 A worked example, measured on this board

Measured 2026-08-11 over COM8, board untouched since power-up:

| Register | Value | Reading |
|---|---|---|
| `MAC_TI` | `0x00000028` | 40 ns per tick, nominal |
| `MAC_TISUBN` | `0x00000000` | no sub-ns correction — clock runs free |
| `MAC_TN` (1st) | `0x38292BC8` | 942 222 280 ns |
| `MAC_TN` (2nd) | `0x389C5788` | 949 770 120 ns |
| `MAC_TSL` | `0x000023FD` | 9213 s uptime = 2 h 33 min 33 s |
| `OA_CONFIG0` | `0x00009226` | see 3.2 |

**Watch out for the rollover when you check the delta.** Subtracting the two `MAC_TN`
values naively gives 7 547 840 ns = 7.5 ms, which does not match the roughly one second
between the two commands. The nanoseconds field wrapped once in between:

```
1 000 000 000 + 949 770 120 - 942 222 280 = 1 007 547 840 ns
```

That is 1.0075 s — exactly right for `--read 1` plus the command round-trip, and at the
same time proof that the field wraps at `0x3B9ACA00` (1e9) rather than at 2^30.

`MAC_TSL` is the second, independent sanity check: 9213 s agrees with how long the board had
been up, so the increment rate is not merely *configured* correctly, it *is* correct.

### 3.2 Decoding `OA_CONFIG0`

`0x00009226` decomposes as:

| Bits | Name | Value | Meaning |
|---|---|---|---|
| 15 | `SYNC` | 1 | configuration valid |
| 14 | `TXFCSVE` | 0 | |
| 13:12 | `RFA` | 01 | zero-align receive frame |
| 11:10 | `TXCTHRESH` | 00 | |
| 9 | `TXCTE` | 1 | TX cut-through enabled |
| 8 | `RXCTE` | 0 | RX store-and-forward |
| 7 | `FTSE` | **0** | **frame timestamping off** |
| 6 | `FTSS` | 0 | |
| 5 | `PROTE` | 1 | control transactions protected |
| 4 | `SEQE` | 0 | |
| 2:0 | `BPS` | 110 | 64-byte block payload |

`PROTE = 1` independently confirms what `CLAUDE.md` section 3.6 states about protected
access. The value is not magic: it is composed in
`firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c` around line 2049
as base `0x9026` plus `0x200` because TX cut-through is configured.

---

## 4. Timestamping frames

**Receive.** `FTSE` in `OA_CONFIG0` enables it and `FTSS` picks the size (0 = 32-bit,
1 = 64-bit). The timestamp is then **prepended to the first data block** of the frame, and
the chunk footer announces it through `RTSA` (added) and `RTSP` (parity).

**Transmit.** `FTSE` gates this direction too — that is easy to misread as receive-only, and the
datasheet is explicit in 5.2.5.1: "When the TSC header field is zero **or frame timestamping is
disabled**, no frame egress timestamp will be captured." With `FTSE` clear the capture registers
simply stay as they were, which looks like a dead pattern matcher rather than a missing enable.

With it set, the 2-bit `TSC` field in the **data header** selects which capture register pair takes
the egress timestamp — A, B or C — and it is only valid in the header that carries the start of the
frame (`SV = 1`). Completion is signalled by `TTSCAA` / `TTSCAB` / `TTSCAC` in `OA_STATUS0`, after
which `TTSCxH` / `TTSCxL` can be read with ordinary control transactions.

Two practical notes from building the grandmaster on this firmware:

- **The status bit is not usable as a handshake here.** The driver reads `OA_STATUS0` on every
  extended-status event and writes it straight back, which clears `TTSCAA` (`_OnStatus0()` in
  `drv_lan865x_api.c` — `static`, generated code). Anything in the application racing it loses
  sometimes. Comparing the capture against the previous one is both simpler and robust: a capture
  that has not happened yet still reads the old value. See PTP_IMPLEMENTATION_PLAN.md 1.4.
- **The register pair is one 64-bit value in the format of figure 5-4:** `TTSCxH` holds
  seconds\[31:0\], `TTSCxL` holds nanoseconds in bits 29:0 (bits 31:30 read zero). Measured on this
  board the seconds field tracks uptime, which is the cheapest sanity check there is.

Three pairs exist so that several frames can be in flight without one overwriting another's
timestamp — which is exactly the situation a two-step `Sync` creates.

---

## 5. Why it is two-step Sync + Follow_Up

In two-step operation the `Sync` message carries a timestamp taken when the message was
*assembled*, and the **true egress timestamp is sent afterwards in `Follow_Up`**. The
`twoStepFlag` in the `Sync` `flagField` tells the receiver to expect it.

This is not a fallback, it is the shape of the design: the timestamp only exists *after* the
frame has left, so with a single message there is nowhere to put it. The sequence on the
sender is therefore:

1. Send `Sync` with `TSC` selecting capture A.
2. Wait for `TTSCAA` in `OA_STATUS0`.
3. Read `TTSCAH` / `TTSCAL`.
4. Send `Follow_Up` carrying that value.

---

## 6. Four constraints specific to a multidrop segment

**1. The *peer*-delay mechanism does not work here — the *end-to-end* one does.** This
distinction matters and is easy to blur. `Pdelay_Req` / `Pdelay_Resp` are defined as broadcast
messages; on a mixing segment with several nodes there is no unambiguous way to tell which
response belongs to which request, so peer path-delay measurement does not apply and AN1847
explicitly puts it out of scope. That is **not** a statement about delay measurement in
general: the classic **end-to-end** mechanism does work, because `Delay_Resp` is sent
**unicast to the requester's MAC address** and carries the requester's sequence ID, so
attribution is unambiguous even with several followers on the wire. The reference
implementation in section 11 does exactly that and measures a mean path delay of 3788 ns
over roughly 50 cm of cable.

**This project takes neither road: it does no delay measurement at all.** The bridge broadcasts
`Sync` + `Follow_Up` on the T1S segment, every follower on the bus listens, and the path delay is
handled as a **compile-time constant** — see section 11. That is a deliberate simplification, and
section 11.4 spells out what it costs and what it does not.

**Addressing footnote.** The messages here go out as **Ethernet broadcast**, not to the PTP
multicast addresses `01:1B:19:00:00:00` / `01:80:C2:00:00:0E`. The LAN865x receive filter on
this hardware is not set up for those groups, so multicast frames are dropped in the MAC
filter — silently, which makes it a nasty first bug to chase.

**2. PLCA forces timestamping inside the PHY.** The PLCA elastic buffer inserts a bounded
but **variable** delay. A timestamp taken where the SFD leaves the MAC therefore carries
that jitter. The device instead uses the **packet pattern matcher** to stamp at the **end of
the SFD**, in the PHY, for both directions — configured with `RXMMSKH` / `RXMMSKL` set to
`0xFFFFFF` and `RXMLOC` set to 0.

**3. Setting the clock per `Sync` does not converge.** The SPI write itself jitters more
than the error being corrected; AN1847 shows residual errors in the hundreds of
milliseconds for that approach. Only **rate correction** through `MAC_TI` / `MAC_TISUBN`
settles, and it settles to roughly **93 to 256 ns**.

**4. A transparent L2 bridge adds its own variable residence time.** This applies to *this*
project specifically: forwarding `Sync` frames from `eth0` to `eth1` delays them by an amount
that is not constant. Synchronizing *across* the bridge needs either transparent-clock
behaviour (maintaining the correction field) or making the bridge itself an endpoint of the
time domain. As a plain follower on `eth0` the question does not arise.

---

## 7. The software servo

A software PLL with four states:

| State | Action |
|---|---|
| Initialization | write the clock once (`MAC_TSH` / `MAC_TSL` / `MAC_TN`) |
| Unlocked | large one-shot steps via `MAC_TA` |
| Locked-Coarse | small `MAC_TA` steps or increment changes |
| Locked-Fine | increment only (`MAC_TI` / `MAC_TISUBN`) |

At 8 or 16 `Sync` messages per second it locks in under about 20 messages.

**A working servo has five states, not four.** The implementation discussed in section 11
splits "unlocked" into a frequency-matching step and a hard-set step, and that ordering is the
part that is easy to get wrong:

| State | Entry condition | Action |
|---|---|---|
| `UNINIT` | start | write `MAC_TI` + `MAC_TISUBN` from the calibrated values |
| `MATCHFREQ` | after 16 `Sync` | hard-set `MAC_TSL` + `MAC_TN` |
| `HARDSYNC` | abs(offset) < 100 ms | `MAC_TA`, capped at 16 ms; fall back to `UNINIT` if abs(offset) > 1.07 s |
| `COARSE` | abs(offset) < 300 ns | FIR3-filtered `MAC_TA` |
| `FINE` | abs(offset) <= 150 ns | FIR3-filtered `MAC_TA`, and 1PPS is switched on |

The rate estimate on top of it is an IIR filter with `N = 128` by default, roughly an 11 s
half-life, adjustable from 8 to 4096. It rejects instantaneous rate estimates beyond
+/-5000 ppm, which occur about once every 10 to 30 s from nIRQ delivery jitter.

**Reference implementations.** `github.com/MicrochipTech/LAN865x-TimeSync` contains a
grandmaster and a follower, validated on a SAM D21 Curiosity Nano with a Two-Wire ETH Click,
using 64-bit timestamps, 8 `Sync`/s and about 50 cm of UTP. Closer to home and more relevant
here: **`github.com/zabooh/net_10base_t1s`** (branch `master`) — same silicon as this bridge,
both roles, with measurements. Section 11 is about porting it.

---

## 8. Judging the result without a PTP analyser

Use the **1PPS output on DIOA4** (pulse width 640 ns to 20.48 us, routed via `PADCTRL`).
Put the master's pulse and the follower's pulse on two oscilloscope channels and look at the
edge-to-edge distance. That is the only honest end-to-end measure available on the bench;
everything the servo reports about itself is the servo's own opinion.

**Load caveat.** Register access shares the TC6/SPI service path with the datapath — about
5 % UDP loss was measured on this firmware family with `lan_read` polling every 200 ms
during an iperf run (`CLAUDE.md` section 4). A servo issuing several control transactions per
`Sync` is on that same bus. At 8 to 16 `Sync`/s this is negligible; under full T1S load it
needs to be measured rather than assumed.

---

## 9. What this firmware has today

**The transmit side is already there.** `drv_lan865x.h` (around line 795) exposes a raw
Ethernet send with a caller-selected TSC flag, and the comment there says as much:
`0x01 = Capture A (use for Sync)`. The status decoding in `drv_lan865x_api.c` already knows
`TTSCAA/AB/AC` plus the overflow and missed bits.

**The receive path extracts the timestamp and then drops it one level up.** This is worth
stating precisely, because the two things are easy to confuse:

| Location | State |
|---|---|
| `tc6.c`, `on_rx_slice()` | **works** — when `RTSA` is set it assembles 64 bits from the first eight bytes, skips them in the data stream and stores them in `g->ts` |
| `tc6.c`, `on_rx_done()` | passes the pointer up via `TC6_CB_OnRxEthernetPacket` |
| `drv_lan865x_api.c`, `TC6_CB_OnRxEthernetPacket()` | **discards it** — `(void)rxTimestamp;` |
| `drv_lan865x_api.c`, normal TX path (around line 715) | sends with `0 /* no timestamp */` |

The remaining `TODO`s in `tc6.c` concern only the `RTSP` parity check and the 32-bit
timestamp variant.

So what is missing for a follower is: turn on `FTSE`/`FTSS`, configure the pattern matcher,
carry `rxTimestamp` up to the application, and add the servo. `noip_test.c` is the natural
starting point for the message handling, because it already bypasses the TCP/IP stack and
builds raw frames with its own EtherType.

---

## 10. Enabling frame timestamps: two ways in

`FTSE` (bit 7) turns frame timestamping on, `FTSS` (bit 6) selects 64-bit stamps. Both directions
need `FTSE`, transmit included (section 4), so this is not optional for a grandmaster either. There
are two ways to set them, and **the first one is better** — it was found only after the second had
already been written up here.

### 10.1 From application code, at runtime — preferred

A read-modify-write on `OA_CONFIG0` does it, with no change to generated code at all:

```
lan_rmw 0x00000004 0xC0 0xC0
```

Programmatically the same thing is `LAN865X_DIAG_Rmw()` from
[lan865x_diag.h](firmware/src/lan865x_diag.h), which is what the reference grandmaster does
during its own init. The register then reads `0x92E6` instead of `0x9226`. This survives MCC
regeneration, needs no rebuild of the driver, and can be undone at runtime — for a feature
that is only wanted while PTP is running, that is the right place for it.

### 10.2 In the driver init table — works, but pays a price

The other place is where `OA_CONFIG0` is composed during driver init
(`drv_lan865x_api.c`, around line 2049):

```c
regVal = 0x9026u | 0x80u | 0x40u;
```

Same result, but **it is generated code**, so it belongs to the category described in
`CLAUDE.md` section 6: an MCC *Generate Code* run removes the change without saying so, and the
symptom afterwards is that every timestamp suddenly reads zero. Use 10.1 unless timestamps have
to be on before the application runs.

### 10.3 Two traps that apply either way

**`FTSS = 1` is mandatory, not a preference.** `on_rx_slice()` skips a **fixed 8 bytes**
when `RTSA` is set. With `FTSS = 0` (32-bit timestamps) it would eat four bytes of payload
as well — the result is silently corrupted frames, not an error. Never set `FTSE` without
`FTSS`. That is why the RMW above uses mask **and** value `0xC0`, not `0x80`.

**One more quirk, for the day it matters.** `on_rx_done()` does
`pTS = (0u != g->ts) ? &g->ts : NULL`, so a timestamp of *exactly* zero is reported as "no
timestamp". Practically unreachable — seconds and nanoseconds would both have to be zero —
but it is the explanation if a single packet ever turns up without one.

---

## 11. Master on the bridge, followers listening one-way

The plan is to run the **PTP grandmaster on this bridge** and to build the **follower as a
separate project** on other LAN865x boards. The traffic is **one-way and broadcast**: the bridge
sends `Sync` + `Follow_Up` on the T1S segment, every follower on the bus receives the same pair
and steers its own wall clock from it. **There is no `Delay_Req` / `Delay_Resp` exchange at all** —
the difficulty the delay mechanism creates on a multidrop segment (section 6, constraint 1) is
avoided by not using it, and the path delay becomes a compile-time constant instead. Section 11.4
says what that costs and, more importantly, what it does not.

### 11.1 Prior art: both roles already exist and are measured

`github.com/zabooh/net_10base_t1s` (branch `master`) contains **both** roles, in one image,
switched at runtime by `ptp_mode master|follower`. It is not a sketch — it is instrumented,
documented and regression-tested:

| Source | Size | Role |
|---|---|---|
| `ptp_gm_task.c` / `.h` | 50 KB / 8 KB | grandmaster: Sync + Follow_Up, TX matcher arming — plus `Delay_Resp`, which is not needed here |
| `ptp_fol_task.c` / `.h` | 60 KB / 11.7 KB | follower: message parsing, the five-state servo — plus `Delay_Req`, likewise not needed |
| `ptp_clock.c` | 16 KB | software PTP clock: anchor interpolation on TC0, drift IIR |
| `ptp_cli.c` | 12.7 KB | `ptp_mode`, servo tuning, trace control |
| `ptp_rx.c`, `ptp_log.c`, `ptp_offset_trace.c` | 2 / 5.6 / 3.7 KB | RX hook, logging, offset traces |
| `ptp_ts_ipc.h` | 1.7 KB | the driver→application handover for RX timestamps |

All of it lives under `apps/tcpip_iperf_lan865x/firmware/src/` — the same Harmony
`tcpip_iperf_lan865x` base this bridge grew out of, so the driver, the TC6 layer and SYS_TIME
are the ones already in this tree.

**Measured there** (documentation section 11, build of 15 Apr 2026, verification run
16 Apr 2026): mean offset **+40 … +100 ns**, standard deviation **< 40 ns**, peak ~250 ns,
convergence into the `FINE` state in **2.7 s**, mean path delay **3788 ns**, 45/45 `Delay_Resp`
answered, 45/45 `t3` captures in hardware, zero wrong-sequence rejects. The crystal error
between the two boards was ~+5.4 ppm before `MATCHFREQ` pulled it in.

That run used the full two-way exchange, so the ±40 ns figure is the **best case** and does not
carry over unchanged — see 11.4. What *does* carry over is everything about the loop: the drift
filter, the convergence time, the anchor mechanism, and the 3788 ns path-delay number itself,
which is where the constant in 11.4 comes from.

**Recommendation: fork the follower from that code, do not re-derive the servo from AN1847.**
The app note gives the model — the states, why the clock must be *rated* and not *set*, why
peer delay does not work on multidrop. It does not give a converging loop. The parts that cost
the most time are exactly the ones the app note leaves open: the drift IIR and its outlier
rejection, the anchor-tick capture that makes the software clock usable between `Sync` messages,
and the two empirical offsets. Those are already measured, and re-measuring them means building
a second time-analysis rig. Dropping the delay exchange makes the fork **smaller**, not larger:
`Delay_Req` assembly, the response wait, the sequence-ID bookkeeping and the `t3` TX capture all
fall away, and what is left is the message parser plus the servo.

### 11.2 What this bridge already brings

Nothing in the grandmaster's first phase needs new plumbing:

| Need | Already present |
|---|---|
| Send a raw frame with a TX capture request | `DRV_LAN865X_SendRawEthFrame(idx, buf, len, tsc, cb, ctx)` — [drv_lan865x.h:807](firmware/src/config/default/driver/lan865x/drv_lan865x.h#L807), with `0x01 = Capture A (use for Sync)` in the comment above it |
| Proof that the raw path works | [noip_test.c:131](firmware/src/noip_test.c#L131) sends its `0x88B5` frames through exactly that call, with `tsc = 0x00` |
| Read back the egress timestamp | `TTSCAA/AB/AC`, `TTSCM*`, `TTSCOF*` are already decoded in `drv_lan865x_api.c` (around lines 2239 and 2340) |
| Periodic tick | `SYS_TIME_CallbackRegisterMS(cb, ctx, 1, SYS_TIME_PERIODIC)`, backed by TC0_CH0 |
| Monotonic local time | `SYS_TIME_Counter64Get()` |
| Register access for init and for `FTSE`/`FTSS` | `LAN865X_DIAG_Read/Write/Rmw()` from [lan865x_diag.h](firmware/src/lan865x_diag.h), plus the `lan_read`/`lan_write`/`lan_rmw` console commands for trying it by hand first |

The grandmaster's own `Sync` does **not** go through the TCP/IP stack or the bridge — it goes
straight to the LAN865x through `DRV_LAN865X_SendRawEthFrame()`. That matters for section 11.5.

### 11.3 The split

| | Bridge (this repo) | Follower (new repo) |
|---|---|---|
| Role | grandmaster: `Sync` + `Follow_Up`, broadcast, nothing else | follower: parse, servo, steer the wall clock. **Transmits nothing** |
| New source | `ptp_gm.c` / `.h` | forked `ptp_fol_task.c`, `ptp_clock.c`, `ptp_ts_ipc.h` |
| Register writes | `TXM*` matcher, `MAC_TI`/`MAC_TISUBN`, optional `PPSCTL` + `PADCTRL` | `MAC_TA` (offset), `MAC_TI`/`MAC_TISUBN` (rate) |
| Timestamps needed | TX only — `t1` from `TTSCAH`/`TTSCAL` | RX only — `t2` from the RX matcher |
| Driver patch needed | **none, ever** (see 11.6) | yes — RX timestamp must reach the application |
| Time source | see 11.5 | derived, by definition |
| Count | one | as many as the bus allows |

**The test tooling cannot live in the follower repo alone.** The ~40 Python tests in
`net_10base_t1s` under `tools/ptp-analysis/` and `tools/test-harness/` drive **both** boards over
**both** COM ports simultaneously — a master-side command, a follower-side reading, correlated.
The natural home for the ported subset is `tools/ptp/` **in this repo**, because this repo is the
one that also owns `cli.py`, `test_lan8651.py` and `test_mirror.py`.

### 11.4 One-way only: what that costs, and what it does not

**What falls away.** `Delay_Req` and `Delay_Resp`, and with them `t3`, `t4`, the sequence-ID
bookkeeping, the unicast reply path, the follower's TX timestamping — and the whole ambiguity
that made the *peer*-delay mechanism unusable here in the first place. The bridge never needs an
RX timestamp, so the one unavoidable driver patch
([drv_lan865x_api.c:1348](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L1348))
stays entirely on the follower side. Bus load stops depending on the number of nodes: the full
exchange costs `2 + 2N` frames per interval for `N` followers, one-way costs **2**, and followers
never compete for a PLCA transmit opportunity on account of PTP.

**What it costs: one constant, and only in absolute phase.** Without a measured delay, every
follower is late by the master→follower path delay. On the reference hardware that is **≈3788 ns**,
and the number is worth reading carefully: 50 cm of twisted pair contributes about **2.5 ns** of
it. The rest is PHY and MAC latency — a property of the LAN865x pair, not of the topology. So it
is a genuine constant, handled as one compile-time value in the same class as
`PTP_GM_STATIC_OFFSET`. Cable length enters at roughly **5 ns per metre**, so even a 25 m run adds
only 125 ns.

It is deliberately **assumed, not measured**: the reference number goes into a `#define` and stays
there. No delay request is implemented at any stage — not even temporarily as a bring-up
measurement, because that would drag exactly the machinery back in that one-way sync exists to
avoid. If the absolute value ever needs checking, the code-free route is to compare the 1PPS
outputs of master and follower on a scope, which the `FINE` state already provides on DIOA4
(section 2, `PPSCTL` / `PADCTRL`), and adjust the constant.

**What it does *not* cost — the two things that usually matter more.**

*Frequency lock is untouched.* The rate arm of the servo works from the **interval between
successive `Sync` messages**, not from the delay. `MATCHFREQ`, the drift IIR and the ±5000 ppm
outlier rejection behave identically, so the followers do not walk away over time. A constant
delay error cannot become a drift error.

*Follower-to-follower alignment is nearly unaffected.* A multidrop segment is a shared medium: the
same broadcast frame reaches all nodes at essentially the same instant, so the delay error is
**common to all followers and cancels between them**. What remains between two nodes is only the
*difference* of their path delays — cable-length difference at 5 ns/m plus part-to-part PHY
variation. For the usual reason to want time on such a bus (nodes agreeing with each other:
synchronized sampling, coordinated actuation) one-way sync is close to as good as the full
exchange, at a fraction of the code.

**Where it does bite** is absolute alignment to an outside reference — UTC, or the clock on the
100BASE-T side. That is the boundary-clock question, and it is the third point of 11.5.

### 11.5 Three things that are different on a bridge

**1. The MAC bridge floods what it *receives* — which cuts one way only.** Forwarding here is done
by the Harmony MAC bridge, not in application code — [app.c:481-482](firmware/src/app.c#L481-L482)
says so explicitly and returns `false` so frames go to normal stack/bridge processing. So a foreign
master's `Sync` arriving on `eth1` is **imported into the T1S segment**, where a follower will
happily lock to it. If that is not wanted, filter EtherType `0x88F7` in the bridge, and note that a
*forwarded* PTP frame carries no residence-time correction, so it is wrong by whatever the bridge
took to move it.

**The reverse does not hold for our own frames — and this is easy to get backwards.** A
self-generated `Sync` is never *received* on a port, so the bridge has nothing to flood; and it
leaves through
[`DRV_LAN865X_SendRawEthFrame()`](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L2416),
which `tsc = 1` makes mandatory, bypassing
[`DRV_LAN865X_PacketTx()`](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L664)
where the port mirror's TX hook sits. It is therefore invisible on `eth1` **even with `mirror 1`** —
exactly like `noip_send` frames. The driver still describes `PacketTx` as "the single eth0 egress
point"; that stopped being true when `SendRawEthFrame` was added.

Getting a host-side capture of our own `Sync` therefore needs an explicit clone call from the sender,
and the port mirror now exposes one:
[`MIRROR_RawTx(frame, len)`](firmware/src/port_mirror.h). Every raw sender calls it right after a
successful send — [`noip_test.c`](firmware/src/noip_test.c) already does, and it is what makes those
frames appear on `eth1` at all. It is gated by the same `mirror [0|1]` switch as the two stack-borne
paths, deliberately not by a second per-sender flag, and applies no MAC filter because the caller
built the frame. What the clone proves is frame content, completeness and cadence; the **wire
timing** on the two-wire segment it does not prove — that needs an instrument or a promiscuous
second T1S node.

**2. SPI bandwidth is shared with the datapath.** `CLAUDE.md` section 4 records ~5 % UDP loss when
`lan_read` polled every 200 ms during iperf — five transactions per second was enough to hurt. A
grandmaster cycle costs about six transactions (arm the matcher, send `Sync`, poll `OA_STATUS0`,
read `TTSCAH`, read `TTSCAL`, write-1-clear, send `Follow_Up`). At the reference's 8 Hz that is
~48/s; **default to 1 Hz** and take the completion through the existing `_OnStatus0` callback
instead of an explicit `OA_STATUS0` read. Then measure with `stats` and iperf, not by assumption —
and if the interval is settable at runtime, measure at the shortest interval to be released, not at
the default.

**3. A grandmaster needs a time to be master *of*.** The reference runs free — its wall clock is
whatever the local 25 MHz oscillator says, and the follower tracks that. That is enough to prove
the loop and to measure it. It is not enough for a bridge whose whole point is having a
100BASE-T side: the honest version takes time from `eth1` (SNTP is already in the Harmony
configuration) and disciplines the wall clock against it, which turns the bridge into a
*boundary* clock and is a separate piece of work. Do the free-running version first.

### 11.6 Staging: the bridge side never needs a driver patch

| Phase | Content | Driver patch |
|---|---|---|
| 1 | Grandmaster only: `Sync` with `tsc = 1`, `Follow_Up` from `TTSCAH/AL`, broadcast, variable interval, **off until switched on** | **none** |
| 2 | Follower in its own project: RX timestamps, the servo, the delay constant | yes, on the follower |
| 3 | Test tooling in `tools/ptp/`, two-board regression | — |
| 4 | 1PPS on DIOA4 for a scope-visible comparison, then optionally SNTP discipline | — |

Phase 1 is entirely reachable from application code: the `TXM*` matcher setup from section 2
(`TXMLOC = 30`, `TXMPATH = 0x88`, `TXMPATL = 0xF700`, masks zero), arming with
`TXMCTL = 0x0002` per `Sync`, and `DRV_LAN865X_SendRawEthFrame(..., tsc = 1, ...)`. Note that
`FTSE`/`FTSS` (section 10) are **not** part of it: those enable *RX* timestamps, and the
grandmaster in a one-way design never takes one. The single place a driver change is unavoidable
([drv_lan865x_api.c:1348](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L1348),
`(void)rxTimestamp;`) is needed for the follower's `t2`. So the generated-code hazard of
`CLAUDE.md` section 6 lands entirely on the follower project, and this repo stays clean of it.

### 11.7 Two caveats about the reference implementation

**Its own documentation contradicts itself about generated code.** Section 1.2 states that no
auto-generated Harmony files were modified; section 4.2 then documents four concrete driver
changes — an EIC EXTINT14 ISR for the nIRQ anchor tick, its re-arm in `DRV_LAN865X_Tasks()`, an
EtherType `0x88F7` filter in `TC6_CB_OnRxEthernetPacket` feeding `g_ptp_raw_rx`, and latching the
capture tick in `_OnStatus0`. Believe section 4.2. These are the same hazard class as the
port-mirror hook in `CLAUDE.md` section 6, and they need the same treatment: a test that fails
loudly after an MCC *Generate Code* run.

**Its two empirical offsets are specific to that hardware and that build.** The Follow_Up
`preciseOriginTimestamp` carries `PTP_GM_STATIC_OFFSET = 7650` ns for fixed TX-path asymmetry,
and the local software-clock anchor carries `PTP_GM_ANCHOR_OFFSET_NS = 800000` ns (compile-time
overridable). Port them as starting values, then re-measure. The anchor offset in particular
exists because the nIRQ tick arrives roughly 80 µs after the SFD on the wire (60 µs of wire time
for a 64-byte frame at 10 Mbit/s plus PHY FIFO and notify latency) — deterministic per frame size.
In a one-way design it is not a separate problem at all: a fixed anchor error is
indistinguishable from a fixed path delay, so it is absorbed by the single constant of 11.4 and
disappears when that constant is calibrated. `offset = t2 − t1 − D_const` is the whole equation.

---

## 12. Where the numbers come from

- **`LAN8650-1-Data-Sheet-60001734.pdf`** (DS60001734F): wall clock layout and the
  40 ns/tick nominal increment, section 4; the SPI timestamping mechanics, sections around
  pages 59 to 61; `OA_CONFIG0` bit definitions, section 11.1.5; `TTSCxH`/`TTSCxL`, sections
  11.1.11 to 11.1.16; the TSU timer registers, sections 11.2.19 to 11.2.24; the receive
  match registers, sections 11.5.27 to 11.5.33.
- **`LAN8650-1-Time-Synch-AN-60001847.pdf`** (AN1847, DS60001847C): the follower model, the
  servo states, the PDelay-on-multidrop restriction, the pattern-matcher rationale, and the
  measurement that shows why setting the clock per `Sync` fails (Figure 2-3).
- Both live in
  `C:\Users\M91221\OneDrive - Microchip Technology Inc\Documents\W\WNET\LAN865x\`.
- **Register values in section 3.1** were read from this board on 2026-08-11 over COM8 with
  `cli.py`; they are not copied from the documentation.
- **Everything in section 11** about the reference implementation comes from
  `github.com/zabooh/net_10base_t1s`, branch `master`: the sources under
  `apps/tcpip_iperf_lan865x/firmware/src/ptp_*.c`, and
  `documentation/ptp/implementation.md` (14 sections — section 1.2 and 4.2 for the generated-code
  question, section 11 for the measurements), plus `documentation/ptp/drift_filter.md` and
  `documentation/timing/software_ptp_clock_design.md`. Read on 2026-08-11; nothing in that repo
  was modified.
- **Claims about this firmware** in sections 9, 11.2 and 11.6 were checked in the tree, not
  recalled: [app.c:481-482](firmware/src/app.c#L481-L482) for the bridging decision,
  [drv_lan865x.h:807](firmware/src/config/default/driver/lan865x/drv_lan865x.h#L807) for the raw
  TX entry point, [noip_test.c:131](firmware/src/noip_test.c#L131) for a live caller of it, and
  `drv_lan865x_api.c` line 1348 for the discarded RX timestamp.
- Address encoding, the protected-access convention and the SPI-load caveat: `CLAUDE.md`
  sections 3, 4 and 6.
