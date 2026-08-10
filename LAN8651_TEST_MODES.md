# Transmitter test modes on the 10BASE-T1S bus (LAN8651)

> **What this is.** The LAN8651 MAC-PHY on `eth0` implements the **transmitter test
> modes of IEEE 802.3-2022 §147.5.2** in hardware. They make the PHY emit a defined,
> continuous pattern with no user traffic involved, which is what you need to measure
> the transmitter and the bus with an oscilloscope or a spectrum analyser.
>
> This document explains what the modes are, what you measure with each of them and how
> to set that measurement up, and how the same thing is done both with the **generic
> `lan_read`/`lan_write`** register commands and with the **`testmode` convenience
> command** added on top of them.

---

## Contents

- [1. Why test modes exist](#1-why-test-modes-exist)
- [2. The four modes at a glance](#2-the-four-modes-at-a-glance)
- [3. The register behind them](#3-the-register-behind-them)
- [4. Doing it with the generic register commands](#4-doing-it-with-the-generic-register-commands)
- [5. Doing it with `testmode` — the convenience layer](#5-doing-it-with-testmode--the-convenience-layer)
- [6. `lan_rmw`: the same idea for multi-bit registers](#6-lan_rmw-the-same-idea-for-multi-bit-registers)
- [7. Measuring on the bus](#7-measuring-on-the-bus)
  - [7.1 Where and how to probe](#71-where-and-how-to-probe)
  - [7.2 What to disconnect first](#72-what-to-disconnect-first)
  - [7.3 Mode 1 — output voltage and timing jitter](#73-mode-1--output-voltage-and-timing-jitter)
  - [7.4 Mode 2 — output droop](#74-mode-2--output-droop)
  - [7.5 Mode 3 — power spectral density mask](#75-mode-3--power-spectral-density-mask)
  - [7.6 Mode 4 — transmitter high impedance](#76-mode-4--transmitter-high-impedance)
- [8. Verifying the modes without an instrument](#8-verifying-the-modes-without-an-instrument)
- [9. Operational notes and safety](#9-operational-notes-and-safety)
- [10. Where the numbers come from](#10-where-the-numbers-come-from)

---

## 1. Why test modes exist

A 10BASE-T1S transmitter has to be characterised for amplitude, timing jitter, droop and
spectral content. Doing that on ordinary traffic is awkward for three reasons:

- **Traffic is not deterministic.** Frame content, inter-frame gaps and PLCA transmit
  opportunities all vary, so the waveform on screen changes from acquisition to
  acquisition. Jitter and droop measurements need a repeating, known pattern.
- **Traffic needs a partner.** Something has to generate frames, and on a shared
  multidrop bus other nodes contribute their own transmissions.
- **Some measurements need the opposite** — a transmitter that is guaranteed *silent* and
  high-impedance, so the rest of the bus can be measured without it.

The test modes solve all three: the PHY emits a defined pattern (or nothing at all)
continuously, independent of the MAC, the link state and any other node. In this firmware
they are reachable from the console with no code changes, because they are nothing more
than a register write.

The bridge is a convenient place to do this from. It sits directly on the T1S segment
through the LAN8651, and its command interface runs over the **EDBG UART**, not over T1S —
so you can deliberately silence or disturb the bus and still keep full control of the
board. There is no way to lock yourself out.

---

## 2. The four modes at a glance

| Mode | Name in IEEE 802.3-2022 §147.5.2 | What it qualifies | Instrument |
|---|---|---|---|
| 0 | Normal operation | — | — |
| 1 | Transmitter output voltage and timing jitter | amplitude and edge placement | oscilloscope (differential) |
| 2 | Transmitter output droop | amplitude sag over a defined interval | oscilloscope (differential) |
| 3 | Transmitter power spectral density (PSD) mask | spectral content / emissions | spectrum analyser |
| 4 | Transmitter high impedance | this node's *absence* from the bus | oscilloscope, TDR, ohmmeter |

Modes 1–3 are **transmit** tests: the PHY drives the bus with a defined pattern. Mode 4 is
the inverse: the transmitter is placed in a high-impedance state so that everything else on
the segment can be observed, or so that the node's off-state contribution can be measured.

---

## 3. The register behind them

The mode lives in **`T1STSTCTL`**, and the mode field is the top three bits:

| Item | Value |
|---|---|
| Register | `T1STSTCTL` |
| Address as used by this firmware | **`0x000308FB`** |
| Mode field | bits **15:13** |
| Reset value | `0x0000` (normal operation) |
| Value to write | `mode << 13` |

| Mode | Written value |
|---|---|
| 0 | `0x0000` |
| 1 | `0x2000` |
| 2 | `0x4000` |
| 3 | `0x6000` |
| 4 | `0x8000` |

**About that address.** The register access in this firmware is 32 bits wide and encodes
the bank in the upper half: **upper 16 bits = MMS (Memory Map Selector), lower 16 bits =
register offset**. `0x000308FB` therefore means *MMS 3, offset `0x08FB`* — MMS 3 is the PHY
PMA/PMD bank. Direct access to MMS 3 works; no indirect Clause 22/45 addressing is needed.
A related register in the same bank is **`T1SPMACTL` (`0x000308F9`)**, which holds PMA
reset, transmit disable, low-power and PMA loopback bits, and **`T1SPMASTS`
(`0x000308FA`)**, which is read-only.

---

## 4. Doing it with the generic register commands

Nothing beyond the two generic register commands is required — this is how the modes were
originally brought up and verified on this hardware:

```text
lan_read  <addr_hex>
lan_write <addr_hex> <value_hex>
```

A complete measurement cycle looks like this:

```text
# 1. Record the starting state
lan_read  0x000308FB          # expect 0x00000000 (normal operation)
lan_read  0x000308F9          # expect 0x00000000 (T1SPMACTL at reset)

# 2. Select the mode
lan_write 0x000308FB 0x2000   # mode 1

# 3. ALWAYS read it back
lan_read  0x000308FB          # must return 0x00002000

# 4. ... take the measurement ...

# 5. Restore normal operation, with a check
lan_write 0x000308FB 0x0000
lan_read  0x000308FB          # must return 0x00000000
```

Step 3 is not optional, and this is the single most important habit in this whole document:

> **`LAN865X Write OK` means "the TC6 transaction completed", not "the register holds the
> value".** Only the readback shows that the value stayed. A PHY that reads back the mode
> you wrote has accepted it; if the readback differs, the mode was not taken — and that is
> far easier to spot in the register than at the oscilloscope, where it looks like a
> measurement problem.

Four properties of the register path are worth knowing when scripting it:

1. **It is asynchronous.** The command handler only sets a state; the app task runs the
   TC6 transaction and the answer arrives from a callback. Wait for the reply line before
   sending the next command.
2. **Only one operation at a time.** A second command issued too early is *rejected* with
   `ERROR: Previous LAN operation still in progress`, not queued.
3. **Served only in the idle application state**, with a 200 ms timeout.
4. **Register access shares the SPI/TC6 service path with the data path.** Do not poll
   registers during a throughput measurement — on comparable firmware this cost about 5 %
   UDP packet loss when reading every 200 ms in parallel with iperf.

---

## 5. Doing it with `testmode` — the convenience layer

Everything above still works and is still the ground truth. But typing hexadecimal values
and remembering to read the register back is easy to get wrong in exactly the ways that
cost time later, so a dedicated command was added. **It does the same register write** —
it just refuses to let you skip the parts that matter.

```text
testmode              # show the current mode, decoded
testmode 1            # enter mode 1, verified by readback
testmode 1 30         # ... and return to normal operation after 30 s
testmode 0            # back to normal operation
```

What it adds over `lan_write`:

| Added | Why it matters |
|---|---|
| **Automatic readback** of `T1STSTCTL` after the write | the verification step from §4 can no longer be forgotten |
| **`[VERIFY] PASS` / `FAIL`**, compared masked against `0xE000` | reserved bits read as 0, so a full-word comparison would produce false failures |
| **Decoded mode name** on every read of the register, including a bare `lan_read 0x000308FB` | no mental `>> 13` while looking at a waveform |
| **Optional auto-revert** after 1–600 s | a forgotten test mode presents later as "the link will not come up", with nothing in the ordinary log pointing at a test register |
| **Mode numbers instead of shifted hex** | `testmode 2` rather than `lan_write 0x000308FB 0x4000` |

A full transcript, with a 5-second timeout so both directions are visible:

```text
> testmode 1 5
[TESTMODE] requesting 1 - test mode 1 (output voltage / timing jitter) (T1STSTCTL=0x00002000)
[TESTMODE] the T1S link is down while this mode is active
[TESTMODE] auto-revert armed in 5 s
LAN865X Write OK: Addr=0x000308FB Value=0x00002000
LAN865X Read OK: Addr=0x000308FB Value=0x00002000
[VERIFY] PASS addr=0x000308FB masked=0x00002000 (mask 0x0000E000)
[TESTMODE] now 1 - test mode 1 (output voltage / timing jitter)
[TESTMODE] auto-revert: restoring normal operation
LAN865X Write OK: Addr=0x000308FB Value=0x00000000
LAN865X Read OK: Addr=0x000308FB Value=0x00000000
[VERIFY] PASS addr=0x000308FB masked=0x00000000 (mask 0x0000E000)
[TESTMODE] now 0 - normal operation
```

The two commands are interchangeable and can be mixed freely — `testmode` is a wrapper
around the same single-slot register state machine, so the constraints in §4 (asynchronous,
one operation at a time, idle state only, 200 ms timeout) apply to it unchanged.

---

## 6. `lan_rmw`: the same idea for multi-bit registers

`T1STSTCTL` is comfortable because the mode field is the only thing in it. **`T1SPMACTL`
(`0x000308F9`) is not** — it packs several independent control bits into one word:

| Bit | Mask | Name | Function |
|---|---|---|---|
| 15 | `0x8000` | `RST` | PMA reset, self-clearing — do **not** combine with other bits |
| 14 | `0x4000` | `TXD` | transmit disable |
| 11 | `0x0800` | `LPE` | low-power enable |
| 10 | `0x0400` | `MDE` | multidrop enable (no effect per the data sheet) |
| 0 | `0x0001` | `LBE` | PMA loopback enable |

Changing one of those with `lan_write` means reading the register, combining the bits on
the host and writing the whole word back — three steps, with a race in the middle and an
easy way to clear a bit you meant to leave alone. `lan_rmw` does it in one operation and
verifies the result:

```text
lan_rmw <addr_hex> <mask_hex> <value_hex>      # new = (old & ~mask) | value

lan_rmw 0x000308F9 0x00000001 0x00000001       # set LBE, leave everything else alone
lan_rmw 0x000308F9 0x00000001 0x00000000       # clear LBE
lan_rmw 0x000308F9 0x00004000 0x00004000       # set TXD (transmit disable)
```

Two things to know about it:

- **`value` is not masked by the driver.** The semantics are literally
  `new = (old & ~mask) | value`, so bits set in `value` outside of `mask` are written
  anyway. The command warns when it sees that, but still performs the access.
- **Self-clearing bits legitimately report `FAIL`.** `RST` (bit 15) reads back as 0 after
  the write because the hardware clears it. That is correct behaviour, not an error — do
  not verify such bits through this path.

**PMA loopback (`LBE`)** deserves a note since it is the other diagnostic in this register:
it loops the transmit path back internally (MAC → PCS → 4B/5B → PMA Manchester → back), so
it separates "the PHY is broken" from "the cabling is broken". It requires PLCA to be off or
this node to be the coordinator (node id 0), no external communication during the loopback,
and a reset afterwards for normal operation to resume cleanly. Its effect has **not** been
functionally verified on this hardware yet.

---

## 7. Measuring on the bus

### 7.1 Where and how to probe

The measurement point is the **MDI** — the two-wire differential bus itself, at the
LAN8651's media side, past the transformer/common-mode choke on the Two-Wire ETH Click.

- **Use a differential probe** across the pair. If you only have single-ended probes, use
  two channels with matched cables and compute A−B in the scope; the common-mode content on
  a two-wire bus is large enough that a single-ended measurement referenced to ground is
  not representative.
- **Mind the termination.** 10BASE-T1S multidrop segments are terminated at both physical
  ends. Amplitude readings depend directly on the load presented at the probe point, so the
  bus you measure must be terminated the way you intend to characterise it. For a
  *compliance* measurement, the standard's defined test fixture and load replace the live
  bus entirely.
- **Bandwidth.** 10BASE-T1S runs at 10 Mbit/s using differential Manchester encoding, so
  the line rate is 20 MBd. A scope and probe with a few hundred MHz of bandwidth captures
  the edges comfortably; anything at or below ~100 MHz starts rounding them and will bias
  jitter and rise-time readings.
- **Trigger** on the repeating pattern the mode emits, and use averaging for amplitude and
  droop while keeping a single-shot or infinite-persistence view for jitter.

### 7.2 What to disconnect first

This is where most confusing results come from: **10BASE-T1S is a shared multidrop bus.**
Anything else that is attached and transmitting appears in your acquisition and is not
distinguishable from the device under test by looking at the waveform.

- For a **transmitter** measurement (modes 1–3), the device under test should be the only
  transmitter on the segment. Physically remove the other nodes, or keep them from
  transmitting.
- Conveniently, the test modes do part of that work themselves. Entering any of modes 1–4
  takes the T1S link down; and **if this board is the PLCA coordinator (node id 0), its
  beacons stop, so no follower gets a transmit opportunity and the rest of the segment goes
  silent by itself.** That is worth setting up deliberately — check the node id with
  `plca_node` and the register with `lan_read 0x0004CA02` (low byte = node id, high byte =
  node count).
- For a measurement of the **rest** of the bus, use mode 4 or `TXD` instead — see §7.6.

### 7.3 Mode 1 — output voltage and timing jitter

**What you read off the screen:**

- **Differential output amplitude**, peak-to-peak, into the intended load. This is the
  headline transmitter parameter: too low and receivers at the far end of the segment lose
  margin, too high and the emissions budget suffers.
- **Timing jitter** of the edges relative to the ideal symbol grid — as a histogram of edge
  positions, or as an eye diagram with the mask of interest applied.
- Secondary but useful while you are there: **rise and fall time** and any **overshoot or
  ringing**, which points at termination or transformer problems rather than at the PHY.

**Setup:** differential probe at the MDI, terminated bus, averaging on for the amplitude
figure, then averaging off and infinite persistence for the jitter view. Trigger on a stable
edge of the repeating pattern.

### 7.4 Mode 2 — output droop

**Droop** is the sag of the transmitted level during a sustained symbol, caused by the AC
coupling in the path — the transformer and series capacitors cannot hold a DC level, so the
amplitude decays. Too much droop eats the receiver's decision margin on long runs of the
same symbol.

**What you read off the screen:** the amplitude at the start of the sustained interval
versus the amplitude at the end of it, expressed as a percentage of the initial value over
the interval the standard defines. Use the scope's cursors on the two points rather than an
automatic measurement, so you know exactly which two samples the number came from.

**Setup:** as in §7.3, with averaging on to suppress noise — droop is a slow, deterministic
effect, so averaging does not distort it. This is the measurement most sensitive to the
magnetics on the board, so note which hardware revision you measured.

### 7.5 Mode 3 — power spectral density mask

Mode 3 drives the transmitter with a pattern whose **spectral** content is what matters. You
compare the measured power spectral density against the mask in the standard: the emitted
energy has to stay under the envelope across the frequency range, which is what keeps a
10BASE-T1S segment from interfering with its surroundings.

**What you read off the screen:** the PSD trace against the mask, and specifically the
frequencies at which the trace comes closest to it.

**Setup notes**, because this measurement is the easiest one to get wrong:

- A spectrum analyser input is single-ended and 50 Ω; the bus is differential and 100 Ω. Use
  a **balun or transformer-coupled fixture**, and account for its insertion loss in the
  result. Feeding one leg of the pair straight into the analyser measures something, but not
  the quantity in the specification.
- Set the **resolution bandwidth** the standard calls for, not the analyser's default —
  measured PSD scales with RBW, so a wrong setting shifts the whole trace.
- Use **trace averaging** and let it settle before reading values.
- Watch the **input level** and use attenuation as needed; overloading the front end
  produces spurious content that looks like a mask violation.

### 7.6 Mode 4 — transmitter high impedance

Mode 4 does not transmit. It puts the transmitter into a high-impedance state, which makes
it the tool for questions about everything *other than* this node:

- **Measure the bus without this node's contribution.** Signals from the remaining nodes,
  reflections, termination quality, and the effect of stub lengths — all observable with
  this board still physically attached but electrically out of the way.
- **Check the node's off-state behaviour.** Does the transmitter present a genuinely high
  impedance when it is not driving, or does it load the bus? On a multidrop segment every
  node's off-state impedance is part of everyone else's signal integrity budget. Measure the
  impedance at the MDI, or use a TDR to see the discontinuity the node introduces.
- **Cable and termination work.** With a defined silent node, a TDR trace or a simple
  end-to-end resistance check across the pair becomes interpretable.

The `TXD` bit in `T1SPMACTL` (`lan_rmw 0x000308F9 0x00004000 0x00004000`) gives a similar
silence and is the alternative if you want the PHY otherwise in normal operation.

---

## 8. Verifying the modes without an instrument

Before reaching for a scope it is worth confirming that the PHY actually entered the mode.
Two levels of evidence are available from the console alone, and a third from ordinary
network traffic:

1. **Register readback** — `testmode` reports `[VERIFY] PASS`/`FAIL` on `T1STSTCTL`. Proves
   the address encoding is right and the register kept the value.
2. **Traffic stops** — with the board as PLCA coordinator, entering any of modes 1–4
   silences the segment, so periodic frames from a T1S node stop arriving on `eth1`.
3. **Traffic resumes** — and they come back after `testmode 0`.

Level 1 alone only proves the register latched a value. Levels 2 and 3 are what show the
PHY changed state, which is otherwise exactly the thing you would reach for an oscilloscope
to confirm.

**[`test_lan8651.py`](test_lan8651.py)** automates all three and exits non-zero on any
failure:

```text
python test_lan8651.py --port COM8
python test_lan8651.py --port COM8 --modes 1,2 --window 6
python test_lan8651.py --list-interfaces
```

The traffic oracle is whatever the T1S node emits by itself — by default a SOME/IP-SD OFFER
multicast at 1 Hz — counted with `tshark` on the `eth1` adapter. Nothing is generated on the
host, so counting it does not perturb the bus the way polling registers would. The script
requires this board to be the PLCA coordinator and refuses to run otherwise, because with an
external coordinator the other node could keep transmitting and "traffic stopped" would no
longer say anything about *this* board's transmitter. It always restores normal operation in
a `finally` block and reports whether that succeeded.

Result on this hardware — SAM E54 Curiosity Ultra + MIKROE-5543, T1S node at `192.168.0.54`,
2026-08-10. Baseline before the run: `T1STSTCTL = 0x00000000`, `PLCA_CTRL1 = 0x00000800`
(node id 0, node count 8), 4 frames in a 4 s window.

| Mode | `T1STSTCTL` | Readback | Frames during mode | Frames after revert |
|---|---|---|---|---|
| 1 | `0x00002000` | PASS | 0 | 4 / 4 s |
| 2 | `0x00004000` | PASS | 0 | 4 / 4 s |
| 3 | `0x00006000` | PASS | 0 | 4 / 4 s |
| 4 | `0x00008000` | PASS | 0 | 5 / 4 s |

All four modes pass all three levels — 19 checks, exit code 0. Note what this establishes
and what it does not: the PHY demonstrably changes its transmit state in every mode and
returns cleanly, but nothing here says anything about the waveform itself. That is what §7
is for.

Two implementation details found while building this, in case the numbers ever look wrong:
the readback is compared **masked** against `0xE000`, because the remaining bits of
`T1STSTCTL` read as 0 and a full-word comparison would fail spuriously; and the mode is
decoded on *every* read of the register, so a bare `lan_read 0x000308FB` reports it too.

---

## 9. Operational notes and safety

- **The T1S link is down in modes 1–4.** For a bridge that means the bridged connection is
  dead while a mode is active. Anything reached *through* `eth0` is unreachable.
- **You cannot lock yourself out.** The console is the EDBG virtual COM port over UART, not
  T1S, so the way back to `testmode 0` is always available.
- **Mode 3 exists to produce emissions.** Run it only in a controlled, isolated setup and
  observe local EMC rules. A continuously transmitting node is not something to leave
  running on a shared installation.
- **Do not run a test mode during traffic measurements**, and do not poll registers during
  an iperf run — see §4, point 4.
- **A forgotten `T1STSTCTL != 0` is hard to diagnose later.** The link simply will not come
  up and nothing in the ordinary log mentions a test register. Use the timeout argument
  (`testmode 1 30`), or make `testmode` with no argument the first thing you check when a
  T1S link refuses to establish.
- **`mirror` and `noip_send` are pointless while a mode is active** — there is no link to
  carry the frames. Restore normal operation first, then generate traffic.

---

## 10. Where the numbers come from

This document deliberately does **not** reproduce limit values, pattern definitions or mask
envelopes. Those are normative and belong to their sources:

- **IEEE 802.3-2022, Clause 147** — the 10BASE-T1S PHY: §147.5.2 for the test modes
  themselves, and the transmitter specification tables for the amplitude, jitter, droop and
  PSD mask limits, plus the test fixtures the measurements are defined against.
- **LAN8650/LAN8651 data sheet** — the register maps (`T1STSTCTL`, `T1SPMACTL`,
  `T1SPMASTS`), reset values and any device-specific deviations or additions.

What is documented here instead is everything that is specific to *this* firmware and *this*
board: the address encoding, the command paths, the state-machine constraints, the practical
measurement setup, and the verification results obtained on this hardware.

Related documents in this repository:

- **[`README.md`](README.md)** — hardware bill of materials, architecture, the full CLI
  reference, `env` configuration, port mirroring and iperf.
- **[`test_lan8651.py`](test_lan8651.py)** — the verification harness described in §8.
- **`CLAUDE.md`** *(German)* — working notes for this repository, including the register-path
  pitfalls and corrected misconceptions worth knowing before touching that code.
