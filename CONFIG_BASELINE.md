# Configuration baseline — what to rebuild, and why each value is what it is

**What this is.** The follower project has left MCC. Its MCC model does not describe it (see
[§0.2](#02-why-the-follower-left-mcc)), so from here on the configuration lives in two places: the
mechanical part in [derive_follower.py](derive_follower.py), and *this* document — the values that
were paid for with measurements, and the reasons behind them.

**What it is for.** Setting the whole thing up again from a blank MCC project: same device, or a
different one. Every section therefore has two columns — the concrete value for this board, and the
rule that survives a change of controller. The concrete value is worthless on a new part; the rule is
what you actually need.

**What it is not.** It does not repeat `derive_follower.py`. That script already says, executably and
with anchors that refuse to half-apply, *what the follower is not* — one interface, no MAC bridge, no
mirror, no grandmaster. This document says why the remaining numbers are those numbers.

> **Scope.** The follower is the reference, because it is the project that left MCC. **The bridge
> keeps MCC, deliberately** — its model does describe it, and the option of getting "Generate Code"
> working there again is being held open on purpose ([§5.3](#53-the-bridge-keeps-mcc-and-what-that-costs)).
> Each section below therefore carries an *applies to* line. Sections [1](#1-clocking) and
> [5](#5-patches-in-generated-code) matter for both boards; §1 is **not yet applied to the bridge**,
> and how it gets there is the one decision that can quietly close the door on the bridge's MCC.

**Language:** English, like [README.md](README.md) and [LAN8651_TIME_SYNC.md](LAN8651_TIME_SYNC.md).
Only [CLAUDE.md](CLAUDE.md) is German.

---

## Contents

- [0. Ground truth and how it was established](#0-ground-truth-and-how-it-was-established)
- [1. Clocking](#1-clocking)
- [2. Network stack](#2-network-stack)
- [3. LAN865x driver and the T1S bus](#3-lan865x-driver-and-the-t1s-bus)
- [4. Timebase](#4-timebase)
- [5. Patches in generated code](#5-patches-in-generated-code)
- [6. System services](#6-system-services)
- [7. Order for a fresh setup](#7-order-for-a-fresh-setup)
- [8. How to check each item on hardware](#8-how-to-check-each-item-on-hardware)
- [9. Log](#9-log)

---

## 0. Ground truth and how it was established

*Applies to: **both boards**.*

### 0.1 Measuring the drift from MCC

`mcc-config.mc4` carries a `generatedFileHashHistoryMap`, and the hash there is **plain unsalted
SHA-256 of the file contents** — reproducible with `sha256sum`. Comparing map against disk therefore
finds every diverged generated file mechanically, with no guessing. Measured 2026-08-12 on 617 mapped
files:

| | unchanged | real divergences |
|---|---|---|
| bridge | 599 | 5 |
| follower | 597 | 7 |

Twelve further mismatches per project are **false positives**: stock TCP/IP headers (`arp.h`,
`ipv4.h`, `http.h`, `tcpip_heap.h`, …), byte-identical between both projects, fully CRLF and clean in
git. Their hash in the map is simply stale.

The real ones are `app.c`/`app.h` (user-owned by design, MCC only ever wrote the skeleton),
`initialization.c`, `drv_lan865x_api.c`, `drv_lan865x.h`, and on the follower additionally
`configuration.h` and `plib_clock.c`. All of them are catalogued in
[§5](#5-patches-in-generated-code).

> The comparison is worth re-running before believing any statement in this file. Method above; a
> ready-made script exists as `_mcc_drift.py` (read-only, prints the two lists).

### 0.2 Why the follower left MCC

The hash maps of the two projects are **byte-identical**, and the follower's module list still
contains `drvGmac`, `drvMiim`, `drvExtPhyLan8740`, `sercom0` and `tcpipNetConfig_1`. The follower's
model is the bridge's model.

Two consequences, and the second is a trap:

- Nothing was lost by abandoning MCC here — there was never a model that described this project.
- **Pressing "Generate Code" in the follower project does not restore an older follower. It turns the
  project into a bridge**: two interfaces, GMAC, MAC bridge. The failure then looks like a corrupted
  project rather than a regenerated one.

### 0.3 The rule this document exists for

> A value that cost a measurement belongs where the next person looks, not in a comment inside a file
> that a new project does not have.

Every register value below is either measured or derived from a measured one. Where a first attempt
failed, the failure is recorded — a dead end is worth as much as the answer, and costs less to read.

---

## 1. Clocking

*Applies to: **both boards**. Values below are read from the follower, which is the only one patched
so far.*

**Status: applied to the follower only. The bridge still runs open-loop.** Same file, same defect
([firmware/src/config/default/peripheral/clock/plib_clock.c](firmware/src/config/default/peripheral/clock/plib_clock.c)
has an empty `OSCCTRL_Initialize()` and `REFCLK(0)`).

> **How the bridge gets this fix is a decision, not a formality.** Copying the follower's hand patch
> across is the fast way and the wrong one: it would add a **fourth** unrepresentable patch to a
> project whose MCC is meant to stay usable ([§5.3](#53-the-bridge-keeps-mcc-and-what-that-costs)),
> and the next "Generate Code" would silently revert it — symptom: the rate wanders again while
> nothing else changes. On the bridge this belongs in the **Clock Configurator** (XOSC0 on, External
> Clock, 50 MHz, DPLL0 reference = XOSC0), which is a GUI action, not a model patch, because the
> model carries no clock component to patch ([§1.3](#13-the-two-defensive-details-and-why-they-are-not-decoration)).
> What the GUI cannot give you is the bounded wait and the DFLL fallback — so on the bridge that part
> stays a hand patch, and it is a small one: two guard loops around code MCC generates anyway.

### 1.1 Why this section exists at all

The MCU time base hung on the **open-loop DFLL48M**: no XOSC, no closed loop, and both
`OSCCTRL_Initialize()` and `DFLL_Initialize()` generated empty. Measured against the master's
wallclock: **+601 ppm**, then **+783 ppm** twenty minutes later — roughly 180 ppm of wander, tens of
ppm *per minute*. `SYS_TIME` was unusable as a frequency reference, and the detrended residual grew
with the window length (0.9 ms over 60 s, 4.0 ms over 180 s), which is the signature of a wandering
rate rather than of scheduling jitter.

| | before (DFLL) | after (XOSC0) |
|---|---|---|
| rate vs master | +783 ppm | **−62.7 ppm** |
| residual median / max | 1903 / 3997 µs | **14.9 / 62.7 µs** |
| min-filter winner spread | 3780 µs | **9.1 µs** |
| rate uncertainty | ±22.2 ppm | **±0.053 ppm** |

The 9.1 µs is what made [PTP_TIMEBASE_PLAN.md](PTP_TIMEBASE_PLAN.md) feasible; the plan had staked
itself on "single-digit µs".

### 1.2 The values

| Item | This board (ATSAME54P20A, Curiosity Ultra) | Portable rule |
|---|---|---|
| Oscillator | **XOSC0**, `ENABLE`, `XTALEN=0`, `ONDEMAND=0`, `STARTUP=0` | An **external clock** is not a crystal. Wrong `XTALEN` means no ready flag; a large `STARTUP` makes a present clock look absent — a driven clock needs no run-up. |
| Input frequency | **50 MHz** on XIN0 — the `DSC1001CI2-050.0000`, i.e. the RMII reference clock | **Measure it, do not read it off the schematic.** The obvious guess here was 12 MHz and it was wrong. |
| How it was measured | GCLK generator 3 from XOSC0 → **TC2** (32-bit, GCLK channel 26, independent of TC0/`SYS_TIME`): 1 100 457 527 counts in 21.945 s = **50.147 MHz** | Count the candidate against a timer that does *not* hang off the clock under test. Anything else is circular. |
| DPLL0 reference | `REFCLK = XOSC0`, `DIV = 9` → 50 MHz / (2·(9+1)) = **2.5 MHz** | The DPLL reference input has a **legal range** (32 kHz … 3.2 MHz here). Pick `DIV` for the range first, then the ratio. |
| DPLL0 ratio | `LDR = 47` → 2.5 MHz · 48 = **120 MHz exactly**, `LDRFRAC = 0` | Prefer an integer ratio. A fractional one adds jitter for nothing when the input divides cleanly. |
| GCLK0 | `DIV(1)`, `SRC(7)` = DPLL0 → **120 MHz**, `MCLK_CPUDIV = 1` | CPU clock unchanged by the swap — the point is the *reference*, not the frequency. |
| GCLK1 | `DIV(2)`, `SRC(7)` → **60 MHz** — feeds TC0/`SYS_TIME` | The timebase generator is the one that matters; see [§4](#4-timebase). |
| GCLK2 | `DIV(48)`, `SRC(6)` = DFLL48M → **1 MHz** | Left in place as the fallback reference. |
| Fallback path | `REFCLK(0)` = GCLK channel 1 from generator 2 (1 MHz), `LDR = 119` → 120 MHz | Keep the original generated path reachable. |

### 1.3 The two defensive details, and why they are not decoration

**Bounded waits.** Both the `XOSCRDY0` wait and the DPLL lock wait are `for` loops with a
1 000 000 guard, and failure falls back to the DFLL path. The first attempt at this patch used the
12 MHz assumption (`DIV = 5`), the DPLL never locked, and the **unbounded** generated wait left the
board dead — no console, no clue. A wrong clock must cost at most a fallback, never the board.

**This part is not model-representable.** MCC's template emits an unbounded wait; a Clock Configurator
entry gives you the register values and not the fallback. Anchoring §1 in the model therefore recovers
the numbers but loses the safety net — worth knowing before treating "put it in MCC" as a complete fix.

**And the model has no clock component at all.** Neither `core.yml` nor a separate yml carries one;
the Clock Configurator was never on anything but the default. A model patch per the MCC runbook would
have had to invent symbols, which is why this is a hand patch rather than an oversight.

---

## 2. Network stack

*Applies to: the table is **follower-specific** — the bridge has two interfaces, a MAC bridge, GMAC
and the LAN8740 PHY, all of which its model still describes correctly. [§2.1](#21-promiscuous--true-is-load-bearing-not-a-convenience)
and [§2.2](#22-addressing) apply to **both**.*

| Item | Value | Portable rule |
|---|---|---|
| Interfaces | `TCPIP_STACK_NETWORK_INTERAFCE_COUNT 1` (MCC's own typo — grep for it verbatim) | — |
| MAC bridge | `TCPIP_STACK_USE_MAC_BRIDGE` commented out; `tcpip_mac_bridge.c` then compiles to nothing, its `TCPIP_MAC_BRIDGE_*` settings stay but are inert | — |
| IDX0 | name `LAN865x`, host `MCHP_LAN865x`, MAC `00:04:25:01:02:03`, IP **192.168.0.200**, static | The compile default is a **collision waiting to happen** — the bridge is also `.200`. See §2.2. |
| IDX1 | the whole `…_IDX1` group is still in `configuration.h`, and the stack does not bring it up | Dead defines that look live. `stats` and `showenv` naming one interface is the real answer. |
| Heap | internal, `TCPIP_STACK_DRAM_SIZE 65536`, `ALLOC_UNCACHED`, one supported heap | |
| mDNS / SD | `TCPIP_STACK_USE_ZEROCONF_MDNS_SD` **not** enabled in either project | Enabling it means an MCC run, which endangers §5. |

### 2.1 `promiscuous = true` is load-bearing, not a convenience

`DRV_LAN865X_RxFilterHashTableEntrySet()` is a **stub** returning `TCPIP_MAC_RES_OP_ERR`
([drv_lan865x_api.c:603](firmware/src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c#L603)).
The driver cannot enter a single multicast group into the receive filter. That mDNS (`224.0.0.251`)
and SOME/IP-SD (`224.244.224.245`) arrive at all is due solely to
`DRV_LAN865X_PROMISCUOUS_IDX0 = true`, and **there is no fallback path**.

> Portable rule: before designing anything on multicast, check whether the MAC filter API of that
> driver is actually implemented. Setting the flag to `false` to save CPU kills every multicast
> discovery **silently**. Put critical traffic on **broadcast**, which the MAC filter always accepts,
> and leave multicast to convenience paths.

### 2.2 Addressing

A freshly flashed board comes up as `192.168.0.200` and collides with the bridge. The MAC is derived
from the SAME54 serial number at `0x008061FC` (`env_derive_mac()`,
[follower/firmware/src/env.c:66](follower/firmware/src/env.c#L66)) and is therefore unique in
practice — measured on the two boards on hand, `0xCACED9` and `0x9D4C63`, so n = 2 and not a
statement about a production lot. Planned scheme and the reasoning against deriving the IP from the
serial: [PTP_TIMEBASE_PLAN.md §2](PTP_TIMEBASE_PLAN.md#2-adressierung-und-identität).

> Portable rule: derive what can be derived from silicon (MAC), provision only what must be
> provisioned (node id), and make the **default invalid** rather than valid — a valid default is the
> fault, not the number.

---

## 3. LAN865x driver and the T1S bus

*Applies to: **both boards** — same silicon, same driver, same pins. Only the PLCA **values** differ
per board, by design: the bridge is coordinator (`plca_id 0`), each follower has its own id.*

| Item | Value | Portable rule |
|---|---|---|
| SPI | driver instance 0, **15 MHz** | |
| Chip select | `SYS_PORT_PIN_PC15` | Board wiring — the MikroBUS slot, not a choice. |
| Interrupt | `SYS_PORT_PIN_PC14` | |
| RX buffer | `DRV_LAN865X_MAX_RX_BUFFER_IDX0 1536` | |
| Cut-through | TX `true`, RX `false` | |
| PLCA | enabled, **node id 7**, node count 8, burst count 0, burst timer 128 | Node id 7 is the compile default on *every* board — see below. |

### 3.1 The PLCA node id is the one value that must be provisioned per board

Measured (T4b, [test_results.md](test_results.md)): a duplicate node id costs **~17 % of transmitted
frames** while **reception stays perfect** and the servo happily reports `FINE`. Nobody notices that
as an addressing problem. And raising `ENV_VERSION` invalidates the stored record, so `plca_id` snaps
back to the compile default 7 — at which point the bridge stops being coordinator and the endpoint
goes quiet, which reads as broken hardware.

Further measured properties, all from `test_results.md`:

- `PLCA_STATUS` bit 15 means "beacons present", **not** "I have a valid slot" — with id 20 in an
  8-slot cycle it still read `0x8000` while the node could not transmit at all (K2A).
- `NODE_CNT` on a follower is **inert**; the coordinator alone sets the cycle length (K2).
- PLCA register writes survive a link loss and a PMA reset (T5) — no firmware state needs to hold them.
- Frames a node could not send are **held, not dropped**, and flush out together once PLCA stops
  gating (K3: 5 sent, 10 arrived).

### 3.2 Two hard limits that shape application code

**Raw transmit queue: `TC6_TX_ETH_QSIZE = 4`** (`tc6-conf.h:131`). One frame goes out synchronously
inside `TC6_SendRawEthernetPacket()`, four fill the queue, the sixth is refused — so **five per pass
through the main loop, never more**, regardless of the requested gap. Draining needs `SYS_Tasks()`,
and a busy-wait inside a command handler services nothing.

> Portable rule: any raw sender emits one or two frames per task pass and then returns to the main
> loop. A loop that sends until it fails is measuring the queue, not the bus.

**Timestamping needs `FTSE` and `FTSS` together** (`lan_rmw 0x00000004 0xC0 0xC0`). Datasheet §5.2.5.1:
with frame timestamping disabled, no egress timestamp is captured — so `FTSE` is not an RX-only bit,
and setting `tsc = 1` alone leaves the capture register untouched while the search goes to the pattern
matcher. `FTSE` without `FTSS` costs the RX path four bytes of payload.

**The TX pattern matcher is already configured by the driver.** Init writes
`TXMMSKH/L = 0xFF/0xFFFF`, `TXMLOC = 0`, `TXMCTL = 0x0002` (`TXME`) — mask bits mean *ignore*, so
all-ones is "every frame, stamped at SFD". `TXME` is R/W and not self-clearing; re-arming per frame is
pure SPI load. Only frames sent with `TSC ≠ 0` are stamped, so stack traffic is unaffected.

---

## 4. Timebase

*Applies to: **both boards** — TC0 and GCLK1 are the same generated configuration in each. The tick
*quality* is not the same until §1 reaches the bridge.*

| Item | Value | Portable rule |
|---|---|---|
| Instance | `SYS_TIME` on **TC0**, `COUNT16`, `PRESCALER_DIV1`, `WAVEGEN_MPWM`, `CC[0] = 59999`, MC1 interrupt | |
| Clock | **GCLK1 = 60 MHz** → **16.667 ns/tick**, hardware wrap every **1.092 ms** | The counter width the datasheet gives is not the width you get; check what the PLIB configured. |
| Config | `HW_COUNTER_WIDTH 16`, `HW_COUNTER_PERIOD 0xFFFF`, `CPU_CLOCK_FREQUENCY 120000000`, `COMPARE_UPDATE_EXECUTION_CYCLES 232`, `MAX_TIMERS 5` | `MAX_TIMERS` is a budget — the trigger of plan phase C consumes one. |
| Free for later | only `plib_tc0.c` exists → **TC1…TC7 and all TCC are free**; `evsys` is in the model with **zero** channels | Phase E needs a 32-bit TC, which on this family is an **instance pair**; pick one with **both channels** free so phase F can capture on the same instance. |
| **Open — counter width** | `COUNT16` is the MCC default and was inherited, not chosen | **For a fresh setup pick `COUNT32` for the `SYS_TIME` instance** (wrap 71.6 s instead of 1.092 ms) and accept that it consumes an instance *pair*. The 16-bit wrap is a correctness hazard, see the second bullet below. |

Two things that look like bugs and are not:

- **`SYS_TIME_FrequencyGet()` returns 60000000 — the nominal value.** The true tick length differs by
  the crystal error, and determining it *is* the job of the affine fit. Never treat it as calibrated.
- **The 16-bit hardware wrap shows up as a spike at exactly 1.092 ms** in a latency histogram: a
  blocked compare interrupt costs one whole period. Sharp spike = missed overflow; smeared = jitter.
  ~~A min-filter is immune to it, because the error is always positive.~~ **Wrong — corrected
  2026-08-12.** A *delayed sample* has a positive error and a min-filter does reject it. A **missed
  overflow is different in kind**: the 64-bit extension fails to increment, so
  `SYS_TIME_Counter64Get()` returns a value 65536 ticks **too small**, the residual goes **negative**,
  and a filter that keeps the minimum residual therefore *prefers* the corrupted sample. Measured with
  the console under load: winner spread 970 210 ns on one board and 966 195 ns on the other, last
  residual −193 555 ns, and `tb_capture.py` counting 36 and 47 samples within one 1.092 ms period.
  The same corruption reaches `trig_arm_ticks()`, whose `target_L <= now` test then registers a
  one-shot that never lands in its window — a periodic trigger that stops while still reporting
  `armed: yes`. **So the wrap is a correctness hazard for the whole timebase, not a cosmetic spike:
  either keep main-loop and interrupt latency below 1.092 ms, or widen the counter.** Detail in
  [test_results.md §E2.3](test_results.md), operating rule in
  [GPIO_SYNC_TESTS.md §2.5](GPIO_SYNC_TESTS.md).

**Do not reach for `DWT->CYCCNT`.** Finer (8.3 ns) but 32 bits wrap every 35.8 s at 120 MHz, it hangs
off the debug block (`TRCENA` — the probe sets it and thereby hides a missing init), and it freezes in
sleep modes. `SYS_TIME_Counter64Get()` has none of those properties, and resolution was never the
limit.

---

## 5. Patches in generated code

*Applies to: **both boards**, per row.*

These are the five places where generated code had to be edited. Each row says whether a new project
can avoid it — because that is the only question that matters when setting up again.

| # | Where | What and why | Avoidable on a new setup? |
|---|---|---|---|
| 1 | `plib_clock.c` | XOSC0/DPLL0, [§1](#1-clocking) | **Yes** — Clock Configurator: XOSC0 on, External Clock, 50 MHz, DPLL0 reference = XOSC0. Loses the bounded wait and the fallback (§1.3). |
| 2 | `drv_lan865x_api.c` ~1352 (follower) | `ptp_follower_rx_hook()`: the RX timestamp is valid **only inside** that callback and only there unambiguously paired with its frame. Without it the follower receives `Sync` frames with no timestamps. | **No.** There is no symbol for "call my hook". Inherent to any PTP work on this driver. |
| 3 | `drv_lan865x_api.c` ~683 (bridge) | `mirror_eth0_tx_hook()` in the TX path | **No.** Same reason. Note the symbol name is not free: renaming `port_mirror.c`'s function breaks the link. |
| 4 | `drv_lan865x.h` | API extension: `DRV_LAN865X_SendRawEthFrame()` incl. the `tsc` argument and its TX-done callback, plus `DRV_LAN865X_ReadModifyWriteRegister()` | **No.** An extension to the framework driver's public interface. |
| 5 | `initialization.c:762` | `ENV_Init()` + `env_mac_str()` immediately before `TCPIP_STACK_Init()`, so the MAC comes from the emulated EEPROM | **Yes, by design change** — set the MAC through the stack API after init instead of patching the generated host table. |

### 5.1 The conclusion for a new project

Three of five are the **same file**: the LAN865x driver. On a new setup — a fresh project, a different
controller — take it out of the generated tree from the start: move `drv_lan865x*` into the project's
own source directory and drop the component from the model. Then those patches are ordinary code
instead of drift, and the worst pitfall of this project disappears structurally: an MCC run can no
longer remove the hooks silently.

The remaining two are avoidable — one through the Clock Configurator, one through a design change.
Which means a fresh setup can, in principle, keep a clean model.

> This is advice for a **new** project. It is deliberately *not* being applied to the bridge, for the
> reason in §5.3 — pulling the driver out of the model there would be a one-way door.

### 5.2 Whatever stays patched needs a test that notices its absence

An MCC run removes a hook without a word, and the resulting picture looks like a half-working feature
rather than a missing patch: the mirror still shows frames *from* the bus but not the bridge's own.
[test_mirror.py](test_mirror.py) exists for exactly that (mirror off = 0 frames, on > 0, both
directions), and it is the reason the rule "run it after every Generate Code" is in
[CLAUDE.md](CLAUDE.md) section 6.

> Portable rule: a patch in generated code is only as durable as the test that fails when it is gone.
> Write that test in the same session as the patch, not later.

### 5.3 The bridge keeps MCC, and what that costs

**Decision 2026-08-12: the bridge stays an MCC project.** Unlike the follower, its model actually
describes it, so "Generate Code" there is a repair option rather than a hazard — and that option is
worth holding open. It is not free, though, and the price is paid in advance:

| To keep the option open | Concretely |
|---|---|
| Do not let the patch count grow | Three rows of §5 apply to the bridge (3, 4, 5). Every further hand patch in generated code makes a regeneration more expensive to recover from. Before adding one, ask whether the model or an API can carry it instead. |
| Take the clock through the GUI | See the box in [§1](#1-clocking). Copying the follower's hand patch would make it four. |
| Leave the driver in the model | §5.1's advice — pull `drv_lan865x*` out of the generated tree — is right for a new project and a **one-way door** here: dropping the component changes the model, and the bridge's model is the thing being preserved. |
| Keep the detector working | [test_mirror.py](test_mirror.py) after every MCC run, per §5.2. It is what turns "MCC removed the hook" from a mystery into a failing test. |
| Know what a clean run looks like | A single Generate Code rewrites tens of thousands of lines across the `components/*.yml` and manifests with **no** change in meaning. Judge it by diffing `mcc-config.mc4` alone, not by line count — and use `git stash push`, not `git checkout --`, so a real hand change in the worktree cannot be thrown away with the noise. |

**What would actually make MCC usable again on the bridge:** a small re-apply script for the three
patches, in the spirit of [derive_follower.py](derive_follower.py) — anchored on exact text, refusing
to run rather than half-applying. Then "regenerate, re-patch, test" is three commands instead of an
archaeology session, and the door stays open by construction rather than by discipline. Not built yet.

---

## 6. System services

*Applies to: **both boards**, except the two module lists at the end, which are the follower's and are
marked as such.*

| Item | Value | Portable rule |
|---|---|---|
| CLI groups | **`MAX_CMD_GROUP = 8`** in the generated `system/command/sys_command.h:146` — **exhausted**, and there is no MCC symbol to raise it | A new command module must graft its subcommands onto an existing group. `ptp_trigger.c` does this through `PTP_TRIG_CliTry()` under `tbase`. Check the ceiling **before** designing a CLI. |
| Print buffer | `SYS_CMD_PRINT_BUFFER_SIZE 2048` | |
| Console | SERCOM1, 115200 8N1, via the EDBG COM port | Host side: `cli.py`. It drains until silence, so it **hangs** on a console that keeps talking by itself — use `serial_capture.py` for those. |
| Persistent config | `lib_emulated_eeprom` + `nvmctrl`; follower record magic `LANF`, `ENV_VERSION 1` | A separate magic per firmware role, so a bridge record cannot be read as a follower one. And: **raising `ENV_VERSION` discards every stored value** — IPs, MACs, PLCA id. Check `showenv` against expectation afterwards, before hunting for a hardware fault. |
| Modules in the model | `HarmonyCore`, `cmsis`, `core`, `dfp`, `drvExtMacLan865x_0`, `drv_spi_0`, `evsys`, `lib_crypto`, `lib_emulated_eeprom`, `lib_wolfcrypt`, `nvmctrl`, `sercom1`, `sys_command`, `sys_console`, `sys_console_0`, `sys_debug`, `sys_time`, `tc0`, `tcpipNetConfig_0`, `tcpipNetConfig_1` | On the follower, `tcpipNetConfig_1` and the GMAC group are bridge leftovers ([§0.2](#02-why-the-follower-left-mcc)) — do **not** carry them into a fresh setup. |
| Stack groups | `tcpipIperf`, `tcpipTelnet`, `tcpipCmd`, `tcpipStack`, `drvExtMacLan865x`, `drv_spi`, `tcpipNetConfig`, `tcpipArp`, `tcpipIPv4`, `tcpipIcmp`, `net_Pres`, `tcpipTcp`, `tcpipUdp` | Plus, on the bridge only: `drvExtPhyLan8740`, `drvGmac`, `drvMiim`, `sercom0`. |

---

## 7. Order for a fresh setup

*Applies to: a **new project** — different controller, or this one from scratch. Neither existing
project is meant to be rebuilt this way.*

Clickable in MCC first, hand work afterwards — and each step verified before the next, because a
wrong clock makes every later measurement lie.

1. **Device, clock, console.** Clock Configurator per [§1](#1-clocking) — including a measurement of
   the actual input frequency, not the schematic's claim. Then `sys_console`/`sys_command` and a
   banner over UART. **Verify before continuing:** [§8](#8-how-to-check-each-item-on-hardware) row 1.
2. **`SYS_TIME` on a timer whose generator you know** ([§4](#4-timebase)). Note the counter width and
   the wrap period; both show up later in histograms.
3. **SPI and the LAN865x driver**, pins per [§3](#3-lan865x-driver-and-the-t1s-bus). **Move the driver
   out of the generated tree now**, not after the first patch ([§5.1](#51-the-conclusion-for-a-new-project)).
4. **TCP/IP stack**, one interface, static address, `promiscuous = true`
   ([§2.1](#21-promiscuous--true-is-load-bearing-not-a-convenience)).
5. **Persistent config** (emulated EEPROM) with its own magic, and the MAC derived from the serial
   number. Provision the PLCA node id per board; make the default **invalid**
   ([§3.1](#31-the-plca-node-id-is-the-one-value-that-must-be-provisioned-per-board)).
6. **Generate Code once more and check the diff is trivial** — before any hand patch exists. This is
   the only moment at which "the model is complete" can still be established cheaply.
7. Only then the application: diagnostics, raw-frame test, PTP.

On the MCC side, two mechanical traps are worth reading up front:
`nbproject/configurations.xml` is the tracked source of truth for source files while
`nbproject/Makefile-*.mk` is generated (helper: `add_source_to_mk.py`), and the VS Code MPLAB
extension builds from `.vscode/*.mplab.json`, not from `configurations.xml`, once that cache exists.
Details in the cross-project MCC notes referenced from [CLAUDE.md](CLAUDE.md).

---

## 8. How to check each item on hardware

*Applies to: **both boards**. Rows 5 and 6 need the bridge (a mirror needs a second interface); rows
1–4 run on either.*

Every row is a check that fails loudly rather than degrading quietly.

| # | Item | Check | Expected |
|---|---|---|---|
| 1 | Clock reference | `dump 0x40001038` | `0x00090040` = DPLL0 on XOSC0. Anything else means the patch is gone and the rate wanders again. |
| 2 | Clock quality | `python tb_capture.py` + offline fit | rate uncertainty ≪ 1 ppm; residual **must not** grow with window length |
| 3 | Timebase model | `tbase` | `LOCKED` within ~10 s, slope stable, `usable: yes` |
| 4 | PLCA role | `showenv`, then `stats` | `plca_id` as provisioned; `eth0 RX: ok` counting up. `ok=0` usually means no coordinator, not dead hardware. |
| 5 | Generated-code patches | `python test_mirror.py` | mirror off = 0 frames, on > 0, both directions |
| 6 | Raw TX path | `python test_rawtx_mirror.py` | frames with own source MAC, ascending sequence |
| 7 | PTP master | `python test_ptp.py` | field-level dissection **plus** the device's own counters (`ts timeouts == 0`, one `follow_up` per `sync`) |
| 8 | Model drift | [§0.1](#01-measuring-the-drift-from-mcc) | the divergence list matches [§5](#5-patches-in-generated-code) — nothing new |

> Why row 7 checks counters too: a capture window of a few seconds does **not** find a fault that sets
> in after N cycles. `test_ptp.py` once passed while the bridge had been sending `Sync` without
> `Follow_Up` for minutes. For faults with a latency, the device's own counter is the instrument, not
> the capture.

---

## 9. Log

Newest first. Format: `YYYY-MM-DD — finding → consequence`, one or two sentences, a snippet where it
helps. Values that belong in a section above go **into that section**; this log is for things learned
after the fact and for corrections.

**2026-08-12 — `SYS_TIME` on a 16-bit counter is a correctness hazard, and the min-filter does not
save it → for a fresh setup configure the `SYS_TIME` timer as `COUNT32`.** A missed overflow makes
`SYS_TIME_Counter64Get()` return a value 65536 ticks **too small**, so the residual goes *negative* and
a min-filter *prefers* the corrupted sample — the opposite of what [§4](#4-timebase) claimed, now
corrected there. One root cause produced three unrelated-looking failures in a single afternoon: a
1.09 ms mode in the raw PTP pairs, a 1092.1 µs grid residual on one board, and a periodic trigger that
stalled while still reporting `armed: yes`. Values: winner spread 970 210 / 966 195 ns under console
load versus 13 421 ns at rest; 36 and 47 samples inside one wrap period. **Consequence for this
project:** latency must stay under 1.092 ms, which means querying `tbase` or enabling `ptpf tb on`
*during* a measurement invalidates it (48 ms of line time per query) — the rule is in
[GPIO_SYNC_TESTS.md §2.5](GPIO_SYNC_TESTS.md), and the trigger now has a main-loop watchdog so the
stall is a counted, recovered event instead of a silent death. Widening the counter is not done; it is
an MCC change on the bridge and a `plib_tc0.c` change on the follower.

**2026-08-12 — The two user LEDs are usable and were free: LED1 = PC21 (pin 75), LED2 = PA16
(pin 66), both active low.** Source is this board's own BSP (`Harmony3/bsp`,
`boards/sam_e54_cult/config/bsp.py`) — the board user guide names no pin at all, and the Xplained Pro
variant differs, so neither is a substitute. Both pins read "Available" in `pin_configurations.csv`, so
nothing in either project contends for them; the follower now drives LED1 from `tbase led`. Worth a
line here because a fresh MCC setup would otherwise have to rediscover which pins the board wires to
LEDs. Initialise with `OUTSET` before `DIRSET`, or the LED blinks during boot.

**2026-08-12 — Document created.** Trigger was the decision to stop configuring the follower with
MCC: the distance had grown too large and the benefit no longer covered the effort, while a new
project on a different controller would need a fresh MCC setup regardless. The measurement that
settled it is in [§0](#0-ground-truth-and-how-it-was-established) — the follower's model was the
bridge's all along, so there was nothing to give up. Values collected from the then-current tree
(bridge `ceab701` plus the uncommitted phase-C work).

**2026-08-12 — The bridge keeps MCC; the two projects are treated differently on purpose.** The
follower's model never described it, so there was nothing to preserve; the bridge's does, so
"Generate Code" there stays a repair option and is being held open. Immediate consequence, and the
reason this is a log entry rather than a footnote: **the bridge must get the §1 clock fix through the
Clock Configurator, not as a copy of the follower's hand patch** — the copy would be the fourth
unrepresentable patch in a project whose model is the thing being kept alive, and the next MCC run
would revert it silently. Conditions and the open item (a re-apply script for the three remaining
patches) in [§5.3](#53-the-bridge-keeps-mcc-and-what-that-costs).

*Findings about git, build or tooling do not belong here — they go to
[CLAUDE.md](CLAUDE.md) section 2/6. This log is for configuration.*
