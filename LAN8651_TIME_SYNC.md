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
> lays out the **plan for running the PTP master on this bridge** with the follower in a
> separate project, based on an existing, measured implementation of both roles.

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
- [11. Master on the bridge, follower in its own project](#11-master-on-the-bridge-follower-in-its-own-project)
  - [11.1 Prior art: both roles already exist and are measured](#111-prior-art-both-roles-already-exist-and-are-measured)
  - [11.2 What this bridge already brings](#112-what-this-bridge-already-brings)
  - [11.3 The split](#113-the-split)
  - [11.4 Three things that are different on a bridge](#114-three-things-that-are-different-on-a-bridge)
  - [11.5 Staging: phase 1 needs no driver patch at all](#115-staging-phase-1-needs-no-driver-patch-at-all)
  - [11.6 Two caveats about the reference implementation](#116-two-caveats-about-the-reference-implementation)
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

`TXMCTL` bits that matter: `TXPMDET` = `0x0080` (read-only, pattern detected),
`MACTXTSE` = `0x0004`, `TXME` = `0x0002` (match enable). A grandmaster arms the detector per
`Sync` by writing `TXMCTL = 0x0002`; the one-time setup is `TXMLOC = 30`,
`TXMPATH = 0x88`, `TXMPATL = 0xF700`, `TXMMSKH = TXMMSKL = 0` — that is EtherType `0x88F7`
(PTP) at byte offset 30, matched without masking.

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

**Transmit.** The 2-bit `TSC` field in the **data header** selects which capture register
pair takes the egress timestamp — A, B or C. Completion is signalled by
`TTSCAA` / `TTSCAB` / `TTSCAC` in `OA_STATUS0`, after which `TTSCxH` / `TTSCxL` can be read
with ordinary control transactions. This can be interrupt-driven instead of polled.

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

Two simpler options remain if the delay mechanism is not wanted at all: treat the propagation
delay as a **measured constant** (roughly 5 ns per metre of twisted pair) or ignore it when
the run is short. Sync-only operation keeps the broadcast addressing legal.

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

`FTSE` (bit 7) turns frame timestamping on, `FTSS` (bit 6) selects 64-bit stamps. There are
two ways to set them, and **the first one is better** — it was found only after the second had
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

## 11. Master on the bridge, follower in its own project

The plan is to run the **PTP grandmaster on this bridge** — it sends `Sync` and `Follow_Up` on
the T1S segment — and to build the **follower as a separate project** on a second LAN865x board.
That split is the right one, and it is also the split the reference implementation below already
uses at runtime.

### 11.1 Prior art: both roles already exist and are measured

`github.com/zabooh/net_10base_t1s` (branch `master`) contains **both** roles, in one image,
switched at runtime by `ptp_mode master|follower`. It is not a sketch — it is instrumented,
documented and regression-tested:

| Source | Size | Role |
|---|---|---|
| `ptp_gm_task.c` / `.h` | 50 KB / 8 KB | grandmaster: Sync + Follow_Up, TX matcher arming, `Delay_Resp` |
| `ptp_fol_task.c` / `.h` | 60 KB / 11.7 KB | follower: message parsing, `Delay_Req`, the five-state servo |
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

**Recommendation: fork the follower from that code, do not re-derive the servo from AN1847.**
The app note gives the model — the states, why the clock must be *rated* and not *set*, why
peer delay does not work on multidrop. It does not give a converging loop. The parts that cost
the most time are exactly the ones the app note leaves open: the drift IIR and its outlier
rejection, the anchor-tick capture that makes the software clock usable between `Sync` messages,
and the two empirical offsets. Those are already measured, and re-measuring them means building
a second time-analysis rig.

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
straight to the LAN865x through `DRV_LAN865X_SendRawEthFrame()`. That matters for section 11.4.

### 11.3 The split

| | Bridge (this repo) | Follower (new repo) |
|---|---|---|
| Role | grandmaster: `Sync`, `Follow_Up`, later `Delay_Resp` | follower: parse, `Delay_Req`, servo, steer the wall clock |
| New source | `ptp_gm.c` / `.h` | forked `ptp_fol_task.c`, `ptp_clock.c`, `ptp_ts_ipc.h` |
| Register writes | `TXM*` matcher, `MAC_TI`/`MAC_TISUBN`, optional `PPSCTL` + `PADCTRL` | `MAC_TA` (offset), `MAC_TI`/`MAC_TISUBN` (rate) |
| Driver patch needed | none for phase 1 (see 11.5) | yes — RX timestamp must reach the application |
| Time source | see 11.4 | derived, by definition |

**The test tooling cannot live in the follower repo alone.** The ~40 Python tests in
`net_10base_t1s` under `tools/ptp-analysis/` and `tools/test-harness/` drive **both** boards over
**both** COM ports simultaneously — a master-side command, a follower-side reading, correlated.
The natural home for the ported subset is `tools/ptp/` **in this repo**, because this repo is the
one that also owns `cli.py`, `test_lan8651.py` and `test_mirror.py`.

### 11.4 Three things that are different on a bridge

**1. The MAC bridge floods both directions, and that cuts both ways.** Forwarding here is done by
the Harmony MAC bridge, not in application code — [app.c:481-482](firmware/src/app.c#L481-L482)
says so explicitly and returns `false` so frames go to normal stack/bridge processing. Two
consequences: broadcast `Sync`, `Follow_Up` and `Delay_Req` on `eth0` are **flooded to `eth1`**,
where they are visible to anything on the 100BASE-T side; and a foreign master's `Sync` arriving
on `eth1` is **imported into the T1S segment**, where a follower will happily lock to it. Decide
early which of the two is wanted. If neither: filter EtherType `0x88F7` in the bridge, and note
that a *forwarded* PTP frame carries no residence-time correction, so it is wrong by whatever the
bridge took to move it. Our own `Sync` sidesteps this entirely because it never enters the bridge.

**2. SPI bandwidth is shared with the datapath.** `CLAUDE.md` section 4 records ~5 % UDP loss when
`lan_read` polled every 200 ms during iperf — five transactions per second was enough to hurt. A
grandmaster cycle costs about six transactions (arm the matcher, send `Sync`, poll `OA_STATUS0`,
read `TTSCAH`, read `TTSCAL`, write-1-clear, send `Follow_Up`). At the reference's 8 Hz that is
~48/s; **start at 1 Hz** and take the completion through the existing `_OnStatus0` callback
instead of an explicit `OA_STATUS0` read. Then measure with `stats` and iperf, not by assumption.

**3. A grandmaster needs a time to be master *of*.** The reference runs free — its wall clock is
whatever the local 25 MHz oscillator says, and the follower tracks that. That is enough to prove
the loop and to measure it. It is not enough for a bridge whose whole point is having a
100BASE-T side: the honest version takes time from `eth1` (SNTP is already in the Harmony
configuration) and disciplines the wall clock against it, which turns the bridge into a
*boundary* clock and is a separate piece of work. Do the free-running version first.

### 11.5 Staging: phase 1 needs no driver patch at all

| Phase | Content | Driver patch |
|---|---|---|
| 1 | Grandmaster only: `Sync` with `tsc = 1`, `Follow_Up` from `TTSCAH/AL`, 1 Hz, no `Delay_Resp` | **none** |
| 2 | Follower in its own project: RX timestamps, `Delay_Req`/`Delay_Resp`, the servo | yes, on the follower |
| 3 | Test tooling in `tools/ptp/`, two-board regression | — |
| 4 | 1PPS on DIOA4 for a scope-visible comparison, then optionally SNTP discipline | — |

Phase 1 is entirely reachable from application code: `lan_rmw 0x00000004 0xC0 0xC0` for
`FTSE`/`FTSS` (section 10.1), the `TXM*` matcher setup from section 2
(`TXMLOC = 30`, `TXMPATH = 0x88`, `TXMPATL = 0xF700`, masks zero), arming with
`TXMCTL = 0x0002` per `Sync`, and `DRV_LAN865X_SendRawEthFrame(..., tsc = 1, ...)`. The RX
timestamp — the one place a driver change is unavoidable
([drv_lan865x_api.c:1348](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L1348),
`(void)rxTimestamp;`) — is only needed for `t4`, and `t4` is only needed for `Delay_Resp`. So the
generated-code hazard of `CLAUDE.md` section 6 lands on the follower project, not on the bridge.

### 11.6 Two caveats about the reference implementation

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
for a 64-byte frame at 10 Mbit/s plus PHY FIFO and notify latency) — deterministic per frame size,
and it cancels in `offset = t2 − t1 − delay`, which is exactly why the loop tolerates it.

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
- **Claims about this firmware** in sections 9, 11.2 and 11.5 were checked in the tree, not
  recalled: [app.c:481-482](firmware/src/app.c#L481-L482) for the bridging decision,
  [drv_lan865x.h:807](firmware/src/config/default/driver/lan865x/drv_lan865x.h#L807) for the raw
  TX entry point, [noip_test.c:131](firmware/src/noip_test.c#L131) for a live caller of it, and
  `drv_lan865x_api.c` line 1348 for the discarded RX timestamp.
- Address encoding, the protected-access convention and the SPI-load caveat: `CLAUDE.md`
  sections 3, 4 and 6.
